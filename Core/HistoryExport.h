#pragma once
// w2-041 (desktop only, never compiled into WebAssembly): raw History export
// (docs/taidaflow_history_export_spec.md §3).
//
//  * HistoryExportManager (main thread): FIFO queue keyed by clientSessionId,
//    one export at a time, cancel queued/running, throttled
//    TaidaFlowProxy::historyExportStatus updates, desktop "save as" dialog.
//  * HistoryExportWorker (its own QThread): streams the rows newest first
//    straight from the month files (own read-only SQLite connections, keyset
//    chunks) into a QSaveFile, so memory does not grow with the file.
//  * Download service (w2-049): the manager mounts the export folder on the
//    process-wide AppHttpServer singleton (Core/AppHttpServer, own thread) as
//    GET /exports/<file name> (file name rule isValidExportFileName, streamed from
//    disk, never read into memory as a whole).  The listener (0.0.0.0:8124) is
//    started and stopped by Core, not here.
#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QVariantList>
#include <QVariantMap>
#include <atomic>
#include <limits>
#include <memory>

class SqlManager;
class TaidaFlowProxy;

namespace HistoryExport {

constexpr quint16 kDefaultDownloadPort = 8124;
// w2-050: port of the download links sent to the page (historyExportStatus downloadPort).
// TAIDAFLOW_DOWNLOAD_PORT=<1..65535> (e.g. 8123 when nginx serves the export folder, see
// scripts/nginx-start.ps1); unset or invalid -> kDefaultDownloadPort (the app's own
// AppHttpServer, whose /exports mount stays active as a fallback).  The value is logged.
constexpr auto kDownloadPortEnv = "TAIDAFLOW_DOWNLOAD_PORT";
quint16 downloadPortFromEnvironment();
constexpr int kMaxExportFiles = 20;
constexpr qint64 kMaxExportBytes = 2LL * 1024 * 1024 * 1024;   // 2 GB
constexpr int kDefaultChunkRows = 2000;

// History page column titles (Td.historyTitle), shared by the page and the CSV.
QVariantList historyColumnTitles();

// UI range contract (w2-040): epoch milliseconds, both ends inclusive,
// 0 .. 8640000000000000 = unbounded.  Converted to the whole seconds of
// sensor_data.timestamp: ceil(from/1000) .. floor(to/1000); rows with
// timestamp <= 0 are never shown (Core skips them), so from is at least 1.
// Returns false for non-finite values or from > to.
bool rangeMsToSecs(double fromMs, double toMs, qint64 *fromSec, qint64 *toSec);

// CSV pieces (same cells as the History page / its former QML CSV export:
// every cell quoted, 序號 + the 17 titles, numbers like JS toFixed(2), missing
// value "—", CRLF line ends, UTF-8 with BOM).
QByteArray csvHeader();                          // BOM + header line + CRLF
void appendFixed2(QByteArray &out, double v);    // == JS Number(v).toFixed(2)

struct TimeFormatCache
{
    qint64 minute = std::numeric_limits<qint64>::min();
    bool aligned = false;
    QByteArray prefix;                           // "yyyy/MM/dd HH:mm:"
};
void appendLocalTime(QByteArray &out, qint64 epochSec, TimeFormatCache &cache);

// One History row: raw s1..s16 values (valid[i] false = SQL NULL).
void appendCsvRow(QByteArray &out, qint64 sequence, qint64 epochSec, const double *raw,
                  const bool *valid, TimeFormatCache &cache);

bool isValidSessionId(const QString &sessionId);       // [A-Za-z0-9_-]{1,40}
bool isValidExportFileName(const QString &fileName);   // what the download server accepts
QString exportFileName(const QString &sessionId, const QDateTime &time);

// Cleanup of the web export folder (spec §3.5): while there are more than
// maxFiles *.csv files or they total more than maxBytes, delete the oldest
// (modification time) - never keepFileName.  If keepFileName alone is larger
// than maxBytes it is kept and keptOversize is set.
struct CleanupResult
{
    QStringList removed;
    QStringList failed;          // could not be deleted (e.g. being downloaded)
    int filesLeft = 0;
    qint64 bytesLeft = 0;
    bool keptOversize = false;
};
CleanupResult cleanupExportDir(const QString &dir, const QString &keepFileName,
                               int maxFiles = kMaxExportFiles, qint64 maxBytes = kMaxExportBytes);

struct RunSpec
{
    quint64 id = 0;
    QString sessionId;
    qint64 fromSec = 0;
    qint64 toSec = 0;
    QString targetPath;          // final file (written through QSaveFile)
    bool webFile = false;        // true: in exportDir, cleanup afterwards
    QString exportDir;
    int maxFiles = kMaxExportFiles;
    qint64 maxBytes = kMaxExportBytes;
    int chunkRows = kDefaultChunkRows;
};

enum Outcome { Done = 0, Cancelled = 1, Failed = 2 };

} // namespace HistoryExport

class HistoryExportWorker : public QObject
{
    Q_OBJECT
public:
    explicit HistoryExportWorker(SqlManager *sql) : m_sql(sql) {}
    // Runs on the worker thread.
    void run(const HistoryExport::RunSpec &spec, std::shared_ptr<std::atomic_bool> cancel);

signals:
    void counted(quint64 id, qint64 totalRows);
    void progress(quint64 id, qint64 rowsWritten, qint64 totalRows);
    void finished(quint64 id, int outcome, qint64 rowsWritten, qint64 bytes, QString message,
                  QString logDetail);

private:
    SqlManager *m_sql = nullptr;
};

class HistoryExportManager : public QObject
{
    Q_OBJECT
public:
    struct Options
    {
        QString exportDir;                                   // web exports (spec §3.5)
        quint16 downloadPort = HistoryExport::kDefaultDownloadPort;   // sent to the page with each file
        // Mount exportDir as /exports on AppHttpServer::instance() (unmounted on shutdown).
        bool mountDownloads = true;
        int maxFiles = HistoryExport::kMaxExportFiles;
        qint64 maxBytes = HistoryExport::kMaxExportBytes;
        int chunkRows = HistoryExport::kDefaultChunkRows;
    };

    HistoryExportManager(TaidaFlowProxy *proxy, SqlManager *sql, const Options &options,
                         QObject *parent = nullptr);
    ~HistoryExportManager() override;

    // Connected to TaidaFlowProxy::historyExportRequested / ...CancelRequested.
    void requestExport(const QString &sessionId, double fromMs, double toMs);
    void cancelExport(const QString &sessionId);
    // Desktop flow after the "save as" dialog returned a path (spec §3.6).
    void enqueueDesktopExport(const QString &targetPath, double fromMs, double toMs);
    void shutdown();

    QString exportDir() const { return m_options.exportDir; }
    // /exports is mounted and AppHttpServer::instance() is listening.
    bool downloadServerListening() const;
    int statusPublishCount() const { return m_publishCount; }

signals:
    void jobFinished(const QString &sessionId, const QString &state);

private:
    struct Job
    {
        quint64 id = 0;
        QString sessionId;
        double fromMs = 0;
        double toMs = 0;
        qint64 fromSec = 0;
        qint64 toSec = 0;
        bool desktop = false;
        QString targetPath;
        QString fileName;
        qint64 rowsWritten = 0;
        qint64 totalRows = 0;
        int progress = 0;
        QString message;
        std::shared_ptr<std::atomic_bool> cancel;
        QElapsedTimer runClock;
    };

    void openDesktopDialog(double fromMs, double toMs);
    void enqueue(Job job);
    void startNext();
    bool sessionBusy(const QString &sessionId) const;
    void setEntry(const QString &sessionId, const QVariantMap &entry);
    QVariantMap entryFor(const Job &job, const QString &state, int queuePosition) const;
    void publishQueue();
    void publishRunning(bool force);
    void rejectRequest(const QString &sessionId, const QString &message, const QString &logReason);
    void onCounted(quint64 id, qint64 totalRows);
    void onProgress(quint64 id, qint64 rowsWritten, qint64 totalRows);
    void onFinished(quint64 id, int outcome, qint64 rowsWritten, qint64 bytes,
                    const QString &message, const QString &logDetail);

    TaidaFlowProxy *m_proxy = nullptr;
    SqlManager *m_sql = nullptr;
    Options m_options;
    QThread m_workerThread;
    HistoryExportWorker *m_worker = nullptr;
    bool m_downloadsMounted = false;
    QList<Job> m_queue;
    std::unique_ptr<Job> m_running;
    quint64 m_nextJobId = 1;
    bool m_desktopDialogOpen = false;
    bool m_shutDown = false;
    QVariantMap m_status;
    QStringList m_finishedOrder;       // finished session ids, oldest first (pruning)
    QElapsedTimer m_lastPublishClock;
    int m_lastPublishedProgress = 0;
    int m_publishCount = 0;
};
