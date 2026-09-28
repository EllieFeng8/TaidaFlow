#pragma once
#include <QObject>
#include <QDate>
#include <QDateTime>
#include <QSqlDatabase>
#include <QVector>
#include <QStringList>
#include <QVariantList>
#include <QJsonArray>
#include <QJsonObject>
#include <memory>
#include <QThread>
#include <type_traits>
#include <QString>
#include <QtGlobal>
#include <QHash>
#include <QList>
#include <QSet>
#include <QMutex>
#include <atomic>

// w2-039/w2-041: result of one History-page load (COUNT + one newest-first page),
// produced on the SqlManager thread and delivered queued to the requester.
struct SensorHistoryPageResult
{
    quint64 requestId = 0;
    int page = 0;              // 1-based, page 1 = newest rows
    int pageSize = 0;
    qint64 totalRows = 0;      // rows of sensor_data in [from, to]
    QJsonArray samples;        // newest first; same object layout as queryRangeJsonPaged
    bool ok = false;
    bool countOk = false;
    bool superseded = false;   // skipped: a newer request was already queued
    QString errorMessage;
    double countMs = 0.0;      // measured on the SqlManager thread
    double pageMs = 0.0;
    // w2-041 (requestSensorHistoryRangePage only): month files in the range and
    // how many of their COUNTs came from the count cache.
    int months = 0;
    int countCacheHits = 0;
    // w2-045 (requestSensorHistoryRangePage only, diagnostics): months whose
    // cached COUNT was brought up to date by counting only the rows added since
    // (by rowid); the queued steps the request ran in on the SqlManager thread,
    // each step's duration, the longest one and the time from the request to
    // the result; how the page rows were read ("offset-desc", "offset-asc",
    // "keyset", "seek-desc", "seek-asc", or "none" for an empty page); and the
    // MAX(rowid) of each month file that the counts and the page were taken at.
    int countCacheDeltas = 0;
    int steps = 0;
    QVector<double> stepMs;
    double maxStepMs = 0.0;
    double totalMs = 0.0;
    QString pageMethod;
    QHash<QString, qint64> snapshotRowids;
    // w2-052: the session key of requestSensorHistoryRangePage(sessionKey, ...); empty for
    // the requests without a session (the w2-041 overload).
    QString sessionKey;
};
Q_DECLARE_METATYPE(SensorHistoryPageResult)

class SqlManager : public QObject
{
    Q_OBJECT

public:
    static SqlManager* instance();

    ~SqlManager();

    // w2-067: orderly stop at application exit (Core::shutdown, while QCoreApplication still
    // exists; call it after every user of the SqlManager has been stopped). The worker thread's
    // event loop ends after the jobs already queued, then - still on the worker thread, which
    // opened them - every SQLite connection (data_<yyyyMM>, settings) is closed and removed, and
    // the thread is joined. Afterwards every call that needs the worker thread is refused
    // (warning + empty / false result) instead of blocking on a thread that no longer runs.
    // Idempotent; returns at once when called again.
    void shutdown();
    bool isShutDown() const { return m_shutDown.load(); }

    void setDataDirectory(const QString& path);
    void setSettingsFile(const QString& filePath);

    bool initialize();

    bool saveSensorData(const QDateTime& timestamp, const QVector<double>& readings, const QVector<quint16>& holdings = {});
    QVector<QVariantList> fetchSensorData(const QDate& date, int limit = 10);
    QVector<QVariantList> fetchHoldingRegisters(const QDate& date, int limit = 10);
    bool setSensorName(int index, const QString& name);
    QString sensorName(int index);
    QStringList sensorNames();
    QString dataFilePathForMonth(const QDate& date) const;

    // JSON helpers for REST layer
    QJsonArray getSensorMapJson() const;
    bool updateSensorMapJson(const QJsonArray& arr, QString* errMsg = nullptr);
    bool insertOneSampleJson(const QJsonObject& obj, QString* errMsg = nullptr);
    bool insertBatchSamplesJson(const QJsonArray& items, int* inserted = nullptr, QString* errMsg = nullptr);
    bool queryRangeJson(qint64 from, qint64 to, QJsonArray* out, QString* errMsg = nullptr);
    bool queryRangeJsonPaged(qint64 from, qint64 to, int page, int pageSize, QJsonArray* out, QString* errMsg = nullptr);
    bool queryHoldingRangeJson(qint64 from, qint64 to, QJsonArray* out, QString* errMsg = nullptr);
    bool queryHoldingRangeJsonPaged(qint64 from, qint64 to, int page, int pageSize, QJsonArray* out, QString* errMsg = nullptr);
    bool countSensorRange(qint64 from, qint64 to, qint64* total, QString* errMsg = nullptr);
    bool countHoldingRange(qint64 from, qint64 to, qint64* total, QString* errMsg = nullptr);
    int readFrequency(QString* errMsg = nullptr) const;
    bool setReadFrequency(int value, QString* errMsg = nullptr);

    // Alarm history helpers
    // insertedId (optional) receives the new row id.  Ids are per monthly data
    // file, so a row is identified by (occurrence month, id).
    bool insertAlarm(const QDateTime& occurrence, const QString& reason, QString* errMsg = nullptr,
                     qint64* insertedId = nullptr);
    bool insertAlarm(const QString& reason, QString* errMsg = nullptr);
    // Replaces the reason of row 'id' in the data file of 'occurrence's month.
    // Fails when no such row exists.
    bool updateAlarmReason(const QDateTime& occurrence, qint64 id, const QString& reason,
                           QString* errMsg = nullptr);
    bool getAlarmHistory(qint64 from, qint64 to, QJsonArray* out, QString* errMsg = nullptr);

    // w2-053: one alarm_history row found by findUnresolvedAlarms().
    struct UnresolvedAlarm
    {
        QString monthKey;          // yyyyMM of the data file that holds the row
        qint64 id = -1;            // row id inside that file
        qint64 occurrenceTime = 0; // alarm_history.occurrence_time (epoch seconds)
        QString reason;            // reason JSON as stored
    };
    // w2-053 (restart hand-over of DI alarms): the alarm_history rows whose
    // reason JSON has "sensor" == sensor, "status" == status and no
    // "resolved": true, in the data file of 'month' and in the file of the
    // month before it.  Read-only: a month file that does not exist (or has
    // no alarm_history table) is skipped, nothing is created or changed.
    // Newest first (occurrence_time DESC, then month DESC, then id DESC).
    // Returns false (out cleared) when a file cannot be opened or read.
    bool findUnresolvedAlarms(const QString& sensor, const QString& status, const QDate& month,
                              QList<UnresolvedAlarm>* out, QString* errMsg = nullptr);

    // w2-041 History range (spec §2): asynchronous History load; [from, to]
    // (epoch seconds, both inclusive) may span any number of months,
    // including the "unbounded" range.  Only month files that exist in the data
    // directory are visited (newest month first); totalRows is the sum of the
    // per-month COUNTs and the page is taken newest first across months
    // (ORDER BY timestamp DESC, rowid DESC inside each month).  Per-month COUNTs
    // are cached and reused while the month file is unchanged (same size and
    // same SQLite file change counter).  Returns immediately; the result comes
    // through sensorHistoryPageReady() (connect with a queued connection); a
    // request whose id is lower than the newest id requested through this
    // overload is dropped with superseded = true.
    //
    // w2-045: the work runs as a chain of short queued steps (about 6 ms of
    // work each), so the blocking calls of other threads (the main thread's
    // saveSensorData every second) run between the steps.  Before each step the
    // request is dropped (result with superseded = true) when a newer request
    // id exists.  Each month's COUNT and the page read take the month file's
    // MAX(rowid) at the start as a snapshot: rows saved between the steps are
    // not counted and not shown by this request (sensor_data rows are only
    // ever inserted, so rowid only grows).  Long COUNTs and long OFFSET skips
    // are split into chunks of index rows (keyset from the chunk's last row).
    // A request for the page next to (or equal to) the previous result's
    // page, with the same range, starts from that page's first/last row
    // (timestamp + rowid keyset) instead of an OFFSET; other pages use
    // OFFSET from the nearer end of the month file.
    // The rows returned are the same as the OFFSET query on the snapshot.
    void requestSensorHistoryRangePage(quint64 requestId, qint64 from, qint64 to,
                                       int page, int pageSize);

    // w2-052 (spec §2.1, one History view per client): the same stepped range request,
    // but the "stale request" rule and the keyset anchor belong to sessionKey (the
    // client's clientSessionId) only.  A request is dropped (superseded = true) only
    // when a newer request id of the SAME sessionKey exists, or the session was
    // released; requests of other sessions (and of the overloads without a session,
    // which keep their own shared id sequence and anchor) never make it stale.  Each
    // session has its own anchor (previous result's first/last row), so clients paging
    // different ranges do not take each other's anchor.  requestId only has to grow
    // within one sessionKey.  The result carries sessionKey.  Steps, snapshots and the
    // count cache work as described above (the count cache is shared by all requests;
    // w2-052 keys it by month and range, so ranges of different clients do not evict
    // each other).
    void requestSensorHistoryRangePage(const QString& sessionKey, quint64 requestId, qint64 from,
                                       qint64 to, int page, int pageSize);

    // w2-052: forgets everything kept for sessionKey (its newest request id and its
    // anchor).  A request of that session still running is dropped at its next step
    // (superseded = true); a later request of the same key starts a new session.
    // Returns immediately (the anchor is removed on the SqlManager thread).
    void releaseHistorySession(const QString& sessionKey);

    // w2-052 (diagnostics / tests): the session keys SqlManager currently keeps state
    // for (newest request id or anchor), sorted.  Blocking like the other public
    // functions when called from another thread.
    QStringList historySessionKeys();

    // w2-041 export: absolute paths of the existing sensor_YYYYMM.sqlite files
    // whose month intersects [from, to] (epoch seconds), newest month first.
    // Only lists files (no query).  Blocking like the other public functions
    // when called from another thread.
    QStringList sensorDataFilesInRange(qint64 from, qint64 to);

signals:
    void sensorHistoryPageReady(const SensorHistoryPageResult& result);

private:
    explicit SqlManager(QObject* parent = nullptr);
    void startWorkerThread();

    template <typename F>
    auto runOnThread(F&& func) const -> decltype(func());

    QString m_dataDir;
    QString m_settingsPath;
    QStringList m_dataConnectionNames;
    QString m_settingsSchemaPath;
    QString m_dataSchemaPath;
    QThread* m_thread;
    bool m_threadStarted;
    std::atomic<quint64> m_latestHistoryRequestId{0};
    std::atomic<bool> m_shutDown{false};   // w2-067: set by shutdown()

    // w2-041 (SqlManager thread only)
    struct SensorMonthFile
    {
        QString key;          // yyyyMM
        qint64 monthFrom = 0; // first second of the month (local time)
        qint64 monthTo = 0;   // last second of the month
    };
    struct RangeCountCacheEntry
    {
        qint64 from = 0;
        qint64 to = 0;
        qint64 fileSize = -1;
        quint32 changeCounter = 0;
        qint64 count = 0;
        qint64 snapRowid = -1;   // w2-045: MAX(rowid) of sensor_data the count was taken at
        QString path;            // w2-045: file the entry belongs to
        quint64 lastUse = 0;     // w2-052: for dropping the least recently used entry
    };
    // w2-052: key = rangeCountCacheKey(yyyyMM, lo, hi) (was yyyyMM only, one range per
    // month), at most kRangeCountCacheMax entries (least recently used dropped).
    QHash<QString, RangeCountCacheEntry> m_rangeCountCache;
    quint64 m_rangeCountCacheUse = 0;
    static QString rangeCountCacheKey(const QString& monthKey, qint64 lo, qint64 hi);
    void rangeCountCacheStore(const QString& key, const RangeCountCacheEntry& entry);
    QList<SensorMonthFile> sensorMonthFilesInRange(qint64 from, qint64 to) const;

    // w2-045 (SqlManager thread only): stepped History range request.
    struct HistoryMonth;
    struct HistoryChunkWalk;
    struct HistoryRow;
    struct HistoryRangeJob;
    struct HistoryAnchorRow
    {
        QString key;          // yyyyMM of the row's month file
        qint64 ts = 0;
        qint64 rowid = 0;
        qint64 pos = -1;      // 0-based position, newest first, in the whole range
    };
    // First and last row of the previous result, with the month snapshots
    // (MAX(rowid), row count) the result was taken at.
    struct HistoryAnchor
    {
        bool valid = false;
        qint64 from = 0;
        qint64 to = 0;
        int page = 0;
        int pageSize = 0;
        QHash<QString, QPair<qint64, qint64>> months;   // yyyyMM -> (snapshot rowid, count)
        HistoryAnchorRow first;
        HistoryAnchorRow last;
    };
    HistoryAnchor m_historyAnchor;          // requests without a session (w2-041 overload)
    // w2-052: per session (SqlManager thread only): the anchor of each session's last result.
    QHash<QString, HistoryAnchor> m_sessionHistoryAnchors;
    // w2-052: per session newest request id, written by the requesting thread and read by
    // the steps on the SqlManager thread (guarded by m_historySessionMutex).
    mutable QMutex m_historySessionMutex;
    QHash<QString, quint64> m_sessionLatestRequestIds;
    bool historyJobStale(const HistoryRangeJob& job) const;
    const HistoryAnchor& historyAnchorFor(const HistoryRangeJob& job) const;
    void storeHistoryAnchor(const HistoryRangeJob& job, const HistoryAnchor& anchor);
    QSet<QString> m_historySchemaChecked;   // month keys whose schema was checked once
    enum class HistoryKeyset { None, OlderOrEqual, Older, Newer };

    void runHistoryRangeStep(const std::shared_ptr<HistoryRangeJob>& job);
    bool historyRangeUnit(HistoryRangeJob& job);
    bool historyCountUnit(HistoryRangeJob& job);
    bool historyLocate(HistoryRangeJob& job);
    bool historyFetchUnit(HistoryRangeJob& job);
    void historyFinish(HistoryRangeJob& job, bool ok, const QString& error);
    bool historyEvaluateAnchor(HistoryRangeJob& job, QString* errMsg);
    bool historyOpenMonth(HistoryMonth& month, QSqlDatabase* db, QString* errMsg);
    bool historyMaxRowid(QSqlDatabase& db, qint64* rowid, QString* errMsg);
    bool historySnapFilter(QSqlDatabase& db, const HistoryMonth& month, bool* filter, QString* errMsg);
    bool historyWalkUnit(QSqlDatabase& db, const HistoryMonth& month, bool snapFilter,
                         HistoryChunkWalk& walk, QString* errMsg);
    bool historyFetchRows(QSqlDatabase& db, const HistoryMonth& month, bool snapFilter,
                          qint64 lo, qint64 hi, bool ascending, HistoryKeyset keyset,
                          const HistoryAnchorRow& anchor, qint64 offset, qint64 limit,
                          QList<HistoryRow>* rows, QString* errMsg);

    QString monthKey(const QDate& date) const;
    QString dataFileForKey(const QString& key) const;
    QString dataConnectionName(const QString& key) const;
    bool ensureDirExists(const QString& dir) const;

    bool ensureSettingsDb();
    bool ensureDataSchema(QSqlDatabase& db) const;
    QSqlDatabase openDataDb(const QString& monthKey);
    bool executeSqlFile(const QString& path, QSqlDatabase& db) const;

    static SqlManager* s_instance;
};
