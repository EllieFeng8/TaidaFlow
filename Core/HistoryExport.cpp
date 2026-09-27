#include "HistoryExport.h"

#include "AppHttpServer/AppHttpServer.h"
#include "SqlManager.h"
#include "TaidaFlowProxy.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <charconv>
#include <cmath>

namespace HistoryExport {

namespace {
// History page conversion (Core::applyHistoryPage): value = raw * scale.
constexpr double kAdcFullScale = 65535.0;
constexpr int kExportedSensors = 16;                    // s1..s16
double scaleForSensor(int sensorIndex)                  // 1-based
{
    if (sensorIndex <= 4)
        return 100.0 / kAdcFullScale;                   // TT-01..04 (°C)
    if (sensorIndex <= 11)
        return 1000.0 / kAdcFullScale;                  // PT-01..07 (bar)
    if (sensorIndex == 12)
        return 1.0;                                     // FM-01 (L/min)
    return 100.0 / kAdcFullScale;                       // M1..M4 (%)
}

const QRegularExpression &sessionIdPattern()
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,40}$"));
    return re;
}

const QRegularExpression &exportFilePattern()
{
    // <sessionId>_<yyyyMMdd_HHmmss>.csv, nothing else (no dots, no separators).
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,40}_\\d{8}_\\d{6}\\.csv$"));
    return re;
}

const QByteArray kMissingValue = QByteArrayLiteral("\xE2\x80\x94");   // "—" (cellText of null)
} // namespace

QVariantList historyColumnTitles()
{
    return QVariantList{
        QStringLiteral("時間"),
        QStringLiteral("TT-01 (°C)"), QStringLiteral("TT-02 (°C)"),
        QStringLiteral("TT-03 (°C)"), QStringLiteral("TT-04 (°C)"),
        QStringLiteral("PT-01 (bar)"), QStringLiteral("PT-02 (bar)"),
        QStringLiteral("PT-03 (bar)"), QStringLiteral("PT-04 (bar)"),
        QStringLiteral("PT-05 (bar)"), QStringLiteral("PT-06 (bar)"),
        QStringLiteral("PT-07 (bar)"), QStringLiteral("FM-01 (L/min)"),
        QStringLiteral("M1 (%)"), QStringLiteral("M2 (%)"),
        QStringLiteral("M3 (%)"), QStringLiteral("M4 (%)")
    };
}

bool rangeMsToSecs(double fromMs, double toMs, qint64 *fromSec, qint64 *toSec)
{
    if (!std::isfinite(fromMs) || !std::isfinite(toMs) || fromMs > toMs)
        return false;
    // 8.64e15 ms (the unbounded end) is 8.64e12 s, far inside qint64.
    constexpr double kLimitMs = 8.64e15;
    const double from = std::clamp(fromMs, -kLimitMs, kLimitMs);
    const double to = std::clamp(toMs, -kLimitMs, kLimitMs);
    *fromSec = std::max<qint64>(1, static_cast<qint64>(std::ceil(from / 1000.0)));
    *toSec = static_cast<qint64>(std::floor(to / 1000.0));
    return true;
}

QByteArray csvHeader()
{
    QByteArray out = QByteArrayLiteral("\xEF\xBB\xBF");
    out += "\"";
    out += QStringLiteral("序號").toUtf8();
    out += "\"";
    for (const QVariant &title : historyColumnTitles()) {
        out += ",\"";
        out += title.toString().replace(QLatin1Char('"'), QStringLiteral("\"\"")).toUtf8();
        out += "\"";
    }
    out += "\r\n";
    return out;
}

void appendFixed2(QByteArray &out, double v)
{
    // JS toFixed(2) picks the nearest 2-decimal value of the EXACT double and,
    // on an exact tie, the larger magnitude.  std::to_chars(fixed, 2) is also
    // exact-value rounding, so the two only differ on exact ties.  A double is
    // exactly halfway between two cents only if it is an odd multiple of 1/8
    // (x.125/.375/.625/.875); those are nudged away from zero first.
    if (!std::isfinite(v)) {
        out += std::isnan(v) ? "NaN" : (v > 0 ? "Infinity" : "-Infinity");
        return;
    }
    if (v == 0.0)
        v = 0.0;                                    // (-0).toFixed(2) === "0.00"
    const double eighths = v * 8.0;                 // exact (power of two)
    if (std::fabs(eighths) < 9.0e15 && eighths == std::floor(eighths)
        && std::fmod(std::fabs(eighths), 2.0) == 1.0)
        v += v > 0 ? 0.001 : -0.001;
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), v,
                                      std::chars_format::fixed, 2);
    out.append(buffer, int(result.ptr - buffer));
}

void appendLocalTime(QByteArray &out, qint64 epochSec, TimeFormatCache &cache)
{
    // Same text as the History page ("yyyy/MM/dd HH:mm:ss", local time), but
    // QDateTime is only asked once per minute.
    const qint64 minute = epochSec >= 0 ? epochSec / 60 : -((-epochSec + 59) / 60);
    if (minute != cache.minute) {
        const QDateTime start = QDateTime::fromSecsSinceEpoch(minute * 60);
        cache.minute = minute;
        cache.aligned = start.time().second() == 0;
        cache.prefix = start.toString(QStringLiteral("yyyy/MM/dd HH:mm:")).toLatin1();
    }
    if (!cache.aligned) {
        out += QDateTime::fromSecsSinceEpoch(epochSec)
                       .toString(QStringLiteral("yyyy/MM/dd HH:mm:ss")).toLatin1();
        return;
    }
    const int second = int(epochSec - minute * 60);
    out += cache.prefix;
    out += char('0' + second / 10);
    out += char('0' + second % 10);
}

void appendCsvRow(QByteArray &out, qint64 sequence, qint64 epochSec, const double *raw,
                  const bool *valid, TimeFormatCache &cache)
{
    char buffer[32];
    const auto seq = std::to_chars(buffer, buffer + sizeof(buffer), sequence);
    out += '"';
    out.append(buffer, int(seq.ptr - buffer));
    out += "\",\"";
    appendLocalTime(out, epochSec, cache);
    out += '"';
    for (int i = 0; i < kExportedSensors; ++i) {
        out += ",\"";
        if (valid[i])
            appendFixed2(out, raw[i] * scaleForSensor(i + 1));
        else
            out += kMissingValue;
        out += '"';
    }
    out += "\r\n";
}

quint16 downloadPortFromEnvironment()
{
    const QString value = qEnvironmentVariable(kDownloadPortEnv).trimmed();
    if (value.isEmpty()) {
        qInfo().noquote() << QStringLiteral("[Export] %1 is not set - download links use port %2 (AppHttpServer)")
                                     .arg(QLatin1String(kDownloadPortEnv)).arg(kDefaultDownloadPort);
        return kDefaultDownloadPort;
    }
    bool ok = false;
    const uint port = value.toUInt(&ok);
    if (!ok || port == 0 || port > 65535) {
        qWarning().noquote() << QStringLiteral("[Export] %1=\"%2\" is not a port (1..65535) - ignored, download "
                                               "links use port %3 (AppHttpServer)")
                                        .arg(QLatin1String(kDownloadPortEnv), value).arg(kDefaultDownloadPort);
        return kDefaultDownloadPort;
    }
    qInfo().noquote() << QStringLiteral("[Export] %1=%2 - download links use port %2%3")
                                 .arg(QLatin1String(kDownloadPortEnv)).arg(port)
                                 .arg(port == kDefaultDownloadPort
                                              ? QStringLiteral(" (AppHttpServer)")
                                              : QStringLiteral(" (served by another program, e.g. nginx; /exports on "
                                                               "AppHttpServer port %1 stays available as fallback)")
                                                        .arg(kDefaultDownloadPort));
    return quint16(port);
}

bool isValidSessionId(const QString &sessionId)
{
    return sessionIdPattern().match(sessionId).hasMatch();
}

bool isValidExportFileName(const QString &fileName)
{
    return exportFilePattern().match(fileName).hasMatch();
}

QString exportFileName(const QString &sessionId, const QDateTime &time)
{
    return QStringLiteral("%1_%2.csv").arg(sessionId, time.toString(QStringLiteral("yyyyMMdd_HHmmss")));
}

CleanupResult cleanupExportDir(const QString &dir, const QString &keepFileName, int maxFiles,
                               qint64 maxBytes)
{
    CleanupResult result;
    QFileInfoList files = QDir(dir).entryInfoList(QStringList{QStringLiteral("*.csv")},
                                                  QDir::Files | QDir::NoDotAndDotDot);
    std::sort(files.begin(), files.end(), [](const QFileInfo &a, const QFileInfo &b) {
        const QDateTime ta = a.lastModified();
        const QDateTime tb = b.lastModified();
        return ta != tb ? ta < tb : a.fileName() < b.fileName();
    });
    qint64 total = 0;
    for (const QFileInfo &file : files)
        total += file.size();
    int count = int(files.size());

    for (const QFileInfo &file : files) {
        if (count <= maxFiles && total <= maxBytes)
            break;
        if (file.fileName() == keepFileName)
            continue;                                   // never the file just produced
        if (QFile::remove(file.absoluteFilePath())) {
            result.removed << file.fileName();
            --count;
            total -= file.size();
        } else {
            result.failed << file.fileName();
        }
    }
    result.filesLeft = count;
    result.bytesLeft = total;
    const QFileInfo kept(QDir(dir).filePath(keepFileName));
    result.keptOversize = kept.exists() && kept.size() > maxBytes;
    return result;
}

} // namespace HistoryExport

using namespace HistoryExport;

// ---------------------------------------------------------------------------
// Worker (export thread)
// ---------------------------------------------------------------------------

void HistoryExportWorker::run(const RunSpec &spec, std::shared_ptr<std::atomic_bool> cancel)
{
    QElapsedTimer clock;
    clock.start();
    qint64 rowsWritten = 0;
    qint64 totalRows = 0;

    // The month files are listed by SqlManager (it owns the data directory);
    // this is a short blocking call into the SqlManager thread (directory
    // listing only).  Rows are then read here with our own read-only SQLite
    // connections, so the SqlManager thread (per-second saves, History page)
    // is never occupied by the export.
    const QStringList files = m_sql ? m_sql->sensorDataFilesInRange(spec.fromSec, spec.toSec)
                                    : QStringList();

    struct Source
    {
        QString connection;
        QString path;
        qint64 count = 0;
    };
    QList<Source> sources;
    // Every QSqlQuery/QSqlDatabase handle lives inside the step lambdas below,
    // so they are gone before the connections are removed here.
    const auto closeAll = [&sources]() {
        for (const Source &source : std::as_const(sources)) {
            {
                QSqlDatabase db = QSqlDatabase::database(source.connection, false);
                if (db.isValid())
                    db.close();
            }
            QSqlDatabase::removeDatabase(source.connection);
        }
        sources.clear();
    };
    const auto fail = [&](const QString &message, const QString &detail) {
        closeAll();
        emit finished(spec.id, Failed, rowsWritten, 0, message, detail);
    };

    // Step 1: open each month file read-only and count the rows in range.
    QString error;
    const auto openAndCount = [&](int index) -> bool {
        Source source;
        source.connection = QStringLiteral("tf_export_%1_%2").arg(spec.id).arg(index);
        source.path = files.at(index);
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), source.connection);
        db.setDatabaseName(source.path);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000"));
        sources.append(source);
        if (!db.open()) {
            error = QStringLiteral("open %1: %2").arg(source.path, db.lastError().text());
            return false;
        }
        QSqlQuery count(db);
        count.prepare(QStringLiteral("SELECT COUNT(1) FROM sensor_data WHERE timestamp >= :from AND timestamp <= :to"));
        count.bindValue(QStringLiteral(":from"), spec.fromSec);
        count.bindValue(QStringLiteral(":to"), spec.toSec);
        if (!count.exec() || !count.next()) {
            error = QStringLiteral("count %1: %2").arg(source.path, count.lastError().text());
            return false;
        }
        sources.last().count = count.value(0).toLongLong();
        totalRows += sources.last().count;
        return true;
    };
    for (int i = 0; i < files.size(); ++i) {
        if (!openAndCount(i)) {
            fail(QStringLiteral("讀取資料失敗"), error);
            return;
        }
        if (cancel->load()) {
            closeAll();
            emit finished(spec.id, Cancelled, 0, 0, QString(), QStringLiteral("cancelled while counting"));
            return;
        }
    }
    emit counted(spec.id, totalRows);
    const double countMs = clock.nsecsElapsed() / 1.0e6;

    if (spec.webFile && !QDir().mkpath(spec.exportDir)) {
        fail(QStringLiteral("寫入失敗"), QStringLiteral("cannot create %1").arg(spec.exportDir));
        return;
    }
    QSaveFile out(spec.targetPath);       // temp file + rename on commit; discarded otherwise
    if (!out.open(QIODevice::WriteOnly)) {
        fail(QStringLiteral("寫入失敗"), QStringLiteral("open %1: %2").arg(spec.targetPath, out.errorString()));
        return;
    }

    QByteArray buffer;
    constexpr qsizetype kFlushBytes = 1 << 20;
    buffer.reserve(kFlushBytes + 64 * 1024);
    buffer += csvHeader();
    qint64 bytes = 0;
    const auto flush = [&]() -> bool {
        if (buffer.isEmpty())
            return true;
        if (out.write(buffer) != buffer.size())
            return false;
        bytes += buffer.size();
        buffer.clear();
        return true;
    };

    QString columns = QStringLiteral("rowid, timestamp");
    for (int i = 1; i <= kExportedSensors; ++i)
        columns += QStringLiteral(", s%1").arg(i);
    // Keyset pagination, newest first: each chunk is its own short statement
    // (finished before the next one), so the read lock on the month file is
    // held for one chunk only and the SqlManager writer is never starved.
    const QString sql = QStringLiteral(
            "SELECT %1 FROM sensor_data WHERE timestamp >= :from AND timestamp <= :upper "
            "AND (timestamp < :upper2 OR rowid < :lastRowid) "
            "ORDER BY timestamp DESC, rowid DESC LIMIT :limit").arg(columns);

    TimeFormatCache timeCache;
    double raw[kExportedSensors];
    bool valid[kExportedSensors];
    QElapsedTimer progressClock;
    progressClock.start();
    qint64 chunks = 0;
    double maxChunkMs = 0.0;

    // Step 2: stream one month file (newest first) into the CSV buffer.
    enum class Read { Ok, Cancelled, ReadFailed, WriteFailed };
    const auto readSource = [&](const Source &source) -> Read {
        QSqlDatabase db = QSqlDatabase::database(source.connection, false);
        QSqlQuery query(db);
        query.setForwardOnly(true);
        if (!query.prepare(sql)) {
            error = QStringLiteral("prepare %1: %2").arg(source.path, query.lastError().text());
            return Read::ReadFailed;
        }
        qint64 upper = spec.toSec;
        qint64 lastRowid = std::numeric_limits<qint64>::max();
        for (;;) {
            if (cancel->load())
                return Read::Cancelled;
            QElapsedTimer chunkClock;
            chunkClock.start();
            query.bindValue(QStringLiteral(":from"), spec.fromSec);
            query.bindValue(QStringLiteral(":upper"), upper);
            query.bindValue(QStringLiteral(":upper2"), upper);
            query.bindValue(QStringLiteral(":lastRowid"), lastRowid);
            query.bindValue(QStringLiteral(":limit"), spec.chunkRows);
            if (!query.exec()) {
                error = QStringLiteral("chunk %1: %2").arg(source.path, query.lastError().text());
                return Read::ReadFailed;
            }
            int got = 0;
            while (query.next()) {
                lastRowid = query.value(0).toLongLong();
                upper = query.value(1).toLongLong();
                for (int i = 0; i < kExportedSensors; ++i) {
                    const QVariant value = query.value(i + 2);
                    valid[i] = !value.isNull();
                    raw[i] = valid[i] ? value.toDouble() : 0.0;
                }
                appendCsvRow(buffer, ++rowsWritten, upper, raw, valid, timeCache);
                ++got;
            }
            query.finish();                    // releases the SHARED lock now
            ++chunks;
            maxChunkMs = std::max(maxChunkMs, chunkClock.nsecsElapsed() / 1.0e6);
            if (buffer.size() >= kFlushBytes && !flush()) {
                error = QStringLiteral("write: %1").arg(out.errorString());
                return Read::WriteFailed;
            }
            if (progressClock.elapsed() >= 100) {
                progressClock.restart();
                emit progress(spec.id, rowsWritten, std::max(totalRows, rowsWritten));
            }
            if (got < spec.chunkRows)
                return Read::Ok;
        }
    };

    for (const Source &source : std::as_const(sources)) {
        if (source.count == 0)
            continue;
        const Read result = readSource(source);
        if (result == Read::Ok)
            continue;
        out.cancelWriting();                   // the unfinished temp file is removed
        if (result == Read::Cancelled) {
            closeAll();
            emit finished(spec.id, Cancelled, rowsWritten, bytes, QString(),
                          QStringLiteral("cancelled after %1 row(s); partial file discarded").arg(rowsWritten));
        } else {
            fail(result == Read::WriteFailed ? QStringLiteral("寫入失敗") : QStringLiteral("讀取資料失敗"), error);
        }
        return;
    }
    closeAll();

    if (!flush() || !out.commit()) {
        fail(QStringLiteral("寫入失敗"), QStringLiteral("commit %1: %2").arg(spec.targetPath, out.errorString()));
        return;
    }

    QString detail = QStringLiteral("%1 row(s), %2 byte(s), %3 month file(s), count %4 ms, total %5 ms, "
                                    "%6 chunk(s) of <= %7 rows, longest chunk read %8 ms")
                             .arg(rowsWritten).arg(bytes).arg(files.size())
                             .arg(countMs, 0, 'f', 1).arg(clock.nsecsElapsed() / 1.0e6, 0, 'f', 1)
                             .arg(chunks).arg(spec.chunkRows).arg(maxChunkMs, 0, 'f', 2);
    if (spec.webFile) {
        const CleanupResult cleanup = cleanupExportDir(spec.exportDir, QFileInfo(spec.targetPath).fileName(),
                                                       spec.maxFiles, spec.maxBytes);
        detail += QStringLiteral("; cleanup: removed %1 [%2], failed %3 [%4], left %5 file(s) / %6 byte(s)")
                          .arg(QString::number(cleanup.removed.size()), cleanup.removed.join(QStringLiteral(", ")),
                               QString::number(cleanup.failed.size()), cleanup.failed.join(QStringLiteral(", ")),
                               QString::number(cleanup.filesLeft), QString::number(cleanup.bytesLeft));
        if (cleanup.keptOversize)
            detail += QStringLiteral("; the new file alone exceeds %1 bytes and is kept").arg(spec.maxBytes);
    }
    emit finished(spec.id, Done, rowsWritten, bytes, QString(), detail);
}

// ---------------------------------------------------------------------------
// Manager (main thread)
// ---------------------------------------------------------------------------

namespace {
QString stateName(int outcome)
{
    return outcome == Done ? QStringLiteral("done")
         : outcome == Cancelled ? QStringLiteral("cancelled")
                                : QStringLiteral("error");
}
constexpr int kMaxFinishedEntries = 30;
} // namespace

HistoryExportManager::HistoryExportManager(TaidaFlowProxy *proxy, SqlManager *sql,
                                           const Options &options, QObject *parent)
    : QObject(parent), m_proxy(proxy), m_sql(sql), m_options(options)
{
    if (m_options.exportDir.isEmpty())
        m_options.exportDir = QDir::currentPath() + QStringLiteral("/exports");
    m_options.exportDir = QDir(m_options.exportDir).absolutePath();
    QDir().mkpath(m_options.exportDir);

    // A QSaveFile temp file (<name>.csv.XXXXXX) can only be left behind by a
    // crash; nothing can finish it, so remove it at start-up.
    static const QRegularExpression leftover(QStringLiteral("\\.csv\\.[^.]+$"));
    const QStringList names = QDir(m_options.exportDir).entryList(QDir::Files);
    for (const QString &name : names) {
        if (leftover.match(name).hasMatch() && QFile::remove(QDir(m_options.exportDir).filePath(name)))
            qInfo().noquote() << "[Export] removed unfinished temp file" << name;
    }

    m_worker = new HistoryExportWorker(sql);
    m_worker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &HistoryExportWorker::counted, this, &HistoryExportManager::onCounted);
    connect(m_worker, &HistoryExportWorker::progress, this, &HistoryExportManager::onProgress);
    connect(m_worker, &HistoryExportWorker::finished, this, &HistoryExportManager::onFinished);
    m_workerThread.setObjectName(QStringLiteral("HistoryExportThread"));
    m_workerThread.start(QThread::LowPriority);

    if (m_proxy) {
        connect(m_proxy, &TaidaFlowProxy::historyExportRequested, this,
                &HistoryExportManager::requestExport);
        connect(m_proxy, &TaidaFlowProxy::historyExportCancelRequested, this,
                &HistoryExportManager::cancelExport);
    }
    if (QCoreApplication::instance()) {
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this,
                &HistoryExportManager::shutdown);
    }

    qInfo().noquote() << QStringLiteral("[Export] web export folder %1 (max %2 files / %3 bytes), download links -> port %4")
                                 .arg(QDir::toNativeSeparators(m_options.exportDir))
                                 .arg(m_options.maxFiles).arg(m_options.maxBytes).arg(m_options.downloadPort);
    if (m_options.mountDownloads) {
        // Same rules as the former ExportDownloadServer: only <sessionId>_<yyyyMMdd_HHmmss>.csv
        // directly inside the export folder, attachment, ACAO *, no-store, streamed.
        m_downloadsMounted = AppHttpServer::instance().mountDownloads(
                QStringLiteral("/exports"), m_options.exportDir,
                [](const QString &name) { return isValidExportFileName(name); });
        qInfo().noquote() << QStringLiteral("[Export] download mount GET /exports/<file> -> %1: %2")
                                     .arg(QDir::toNativeSeparators(m_options.exportDir),
                                          m_downloadsMounted ? QStringLiteral("mounted on AppHttpServer")
                                                             : QStringLiteral("FAILED"));
    }
}

HistoryExportManager::~HistoryExportManager()
{
    shutdown();
}

bool HistoryExportManager::downloadServerListening() const
{
    return m_downloadsMounted && AppHttpServer::instance().isListening();
}

void HistoryExportManager::shutdown()
{
    if (m_shutDown)
        return;
    m_shutDown = true;
    if (m_running)
        m_running->cancel->store(true);
    m_queue.clear();
    m_workerThread.quit();
    m_workerThread.wait();                // a running export stops at its next chunk
    if (m_downloadsMounted) {
        AppHttpServer::instance().unmount(QStringLiteral("/exports"));
        m_downloadsMounted = false;
    }
    qInfo().noquote() << "[Export] stopped (export thread; /exports unmounted).";
}

bool HistoryExportManager::sessionBusy(const QString &sessionId) const
{
    if (m_running && m_running->sessionId == sessionId)
        return true;
    for (const Job &job : m_queue) {
        if (job.sessionId == sessionId)
            return true;
    }
    return sessionId == QStringLiteral("desktop") && m_desktopDialogOpen;
}

void HistoryExportManager::rejectRequest(const QString &sessionId, const QString &message,
                                         const QString &logReason)
{
    qWarning().noquote() << QStringLiteral("[Export] request from \"%1\" rejected: %2").arg(sessionId, logReason);
    QVariantMap entry = m_status.value(sessionId).toMap();
    if (entry.isEmpty()) {
        entry = QVariantMap{
            {QStringLiteral("state"), QStringLiteral("error")},
            {QStringLiteral("progress"), 0}, {QStringLiteral("queuePosition"), 0},
            {QStringLiteral("rowsWritten"), 0.0}, {QStringLiteral("totalRows"), 0.0},
            {QStringLiteral("fileName"), QString()}, {QStringLiteral("url"), QString()},
            {QStringLiteral("downloadPort"), 0}, {QStringLiteral("savedPath"), QString()},
        };
    }
    entry.insert(QStringLiteral("message"), message);
    setEntry(sessionId, entry);
}

void HistoryExportManager::requestExport(const QString &sessionId, double fromMs, double toMs)
{
    if (m_shutDown)
        return;
    if (!isValidSessionId(sessionId)) {
        rejectRequest(sessionId, QStringLiteral("匯出失敗:連線端錯誤"),
                      QStringLiteral("invalid session id"));
        return;
    }
    qint64 fromSec = 0;
    qint64 toSec = 0;
    if (!rangeMsToSecs(fromMs, toMs, &fromSec, &toSec)) {
        rejectRequest(sessionId, QStringLiteral("匯出失敗:日期區間錯誤"),
                      QStringLiteral("invalid range %1 .. %2").arg(fromMs, 0, 'f', 0).arg(toMs, 0, 'f', 0));
        return;
    }
    // Same session already queued/running: the new request is refused (the
    // running/queued job and its progress stay as they are; only the message
    // of that entry says why).  See the w2-041 report.
    if (sessionBusy(sessionId)) {
        if (m_running && m_running->sessionId == sessionId)
            m_running->message = QStringLiteral("已有匯出進行中,請取消後再試");
        rejectRequest(sessionId, QStringLiteral("已有匯出進行中,請取消後再試"),
                      QStringLiteral("this session already has an export queued or running"));
        return;
    }

    if (sessionId == QStringLiteral("desktop")) {
        // Desktop (spec §3.6): ask for the target first.  Deferred so that the
        // native dialog's nested loop does not run inside the QML signal
        // emission; the queue, other exports and the mirror keep running while
        // the dialog is open.
        m_desktopDialogOpen = true;
        QTimer::singleShot(0, this, [this, fromMs, toMs]() { openDesktopDialog(fromMs, toMs); });
        return;
    }

    Job job;
    job.id = m_nextJobId++;
    job.sessionId = sessionId;
    job.fromMs = fromMs;
    job.toMs = toMs;
    job.fromSec = fromSec;
    job.toSec = toSec;
    job.fileName = exportFileName(sessionId, QDateTime::currentDateTime());
    job.targetPath = QDir(m_options.exportDir).filePath(job.fileName);
    enqueue(std::move(job));
}

void HistoryExportManager::openDesktopDialog(double fromMs, double toMs)
{
    const QString suggested = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
            .filePath(exportFileName(QStringLiteral("desktop"), QDateTime::currentDateTime()));
    qInfo().noquote() << "[Export] desktop: opening the save dialog, suggested" << QDir::toNativeSeparators(suggested);
    const QString path = QFileDialog::getSaveFileName(nullptr, QStringLiteral("下載歷史資料"), suggested,
                                                      QStringLiteral("CSV 檔案 (*.csv)"));
    m_desktopDialogOpen = false;
    if (m_shutDown)
        return;
    if (path.isEmpty()) {
        qInfo().noquote() << "[Export] desktop: save dialog cancelled -> state cancelled";
        QVariantMap entry{
            {QStringLiteral("state"), QStringLiteral("cancelled")},
            {QStringLiteral("progress"), 0}, {QStringLiteral("queuePosition"), 0},
            {QStringLiteral("rowsWritten"), 0.0}, {QStringLiteral("totalRows"), 0.0},
            {QStringLiteral("fileName"), QString()}, {QStringLiteral("url"), QString()},
            {QStringLiteral("downloadPort"), 0}, {QStringLiteral("savedPath"), QString()},
            {QStringLiteral("message"), QStringLiteral("已取消儲存")},
        };
        setEntry(QStringLiteral("desktop"), entry);
        emit jobFinished(QStringLiteral("desktop"), QStringLiteral("cancelled"));
        return;
    }
    enqueueDesktopExport(path, fromMs, toMs);
}

void HistoryExportManager::enqueueDesktopExport(const QString &targetPath, double fromMs, double toMs)
{
    if (m_shutDown)
        return;
    qint64 fromSec = 0;
    qint64 toSec = 0;
    if (!rangeMsToSecs(fromMs, toMs, &fromSec, &toSec)) {
        rejectRequest(QStringLiteral("desktop"), QStringLiteral("匯出失敗:日期區間錯誤"),
                      QStringLiteral("invalid range"));
        return;
    }
    if (m_running && m_running->sessionId == QStringLiteral("desktop")) {
        rejectRequest(QStringLiteral("desktop"), QStringLiteral("已有匯出進行中,請取消後再試"),
                      QStringLiteral("desktop export already running"));
        return;
    }
    for (const Job &queued : std::as_const(m_queue)) {
        if (queued.sessionId == QStringLiteral("desktop")) {
            rejectRequest(QStringLiteral("desktop"), QStringLiteral("已有匯出進行中,請取消後再試"),
                          QStringLiteral("desktop export already queued"));
            return;
        }
    }
    Job job;
    job.id = m_nextJobId++;
    job.sessionId = QStringLiteral("desktop");
    job.desktop = true;
    job.fromMs = fromMs;
    job.toMs = toMs;
    job.fromSec = fromSec;
    job.toSec = toSec;
    job.targetPath = QFileInfo(targetPath).absoluteFilePath();
    job.fileName = QFileInfo(targetPath).fileName();
    enqueue(std::move(job));
}

void HistoryExportManager::enqueue(Job job)
{
    job.cancel = std::make_shared<std::atomic_bool>(false);
    qInfo().noquote() << QStringLiteral("[Export] #%1 queued: session %2, %3, range %4 .. %5 s, file %6")
                                 .arg(job.id).arg(job.sessionId,
                                                  job.desktop ? QStringLiteral("desktop save-as")
                                                              : QStringLiteral("web (export folder)"))
                                 .arg(job.fromSec).arg(job.toSec)
                                 .arg(QDir::toNativeSeparators(job.targetPath));
    m_finishedOrder.removeAll(job.sessionId);
    m_queue.append(std::move(job));
    publishQueue();
    startNext();
}

void HistoryExportManager::startNext()
{
    if (m_running || m_queue.isEmpty() || m_shutDown)
        return;
    m_running = std::make_unique<Job>(m_queue.takeFirst());
    m_running->message = QStringLiteral("讀取資料中");
    m_running->runClock.start();
    qInfo().noquote() << QStringLiteral("[Export] #%1 running: session %2 (%3 still queued)")
                                 .arg(m_running->id).arg(m_running->sessionId).arg(m_queue.size());
    publishQueue();
    publishRunning(true);

    RunSpec spec;
    spec.id = m_running->id;
    spec.sessionId = m_running->sessionId;
    spec.fromSec = m_running->fromSec;
    spec.toSec = m_running->toSec;
    spec.targetPath = m_running->targetPath;
    spec.webFile = !m_running->desktop;
    spec.exportDir = m_options.exportDir;
    spec.maxFiles = m_options.maxFiles;
    spec.maxBytes = m_options.maxBytes;
    spec.chunkRows = m_options.chunkRows;
    HistoryExportWorker *worker = m_worker;
    auto cancel = m_running->cancel;
    QMetaObject::invokeMethod(worker, [worker, spec, cancel]() { worker->run(spec, cancel); },
                              Qt::QueuedConnection);
}

void HistoryExportManager::cancelExport(const QString &sessionId)
{
    if (m_shutDown)
        return;
    for (qsizetype i = 0; i < m_queue.size(); ++i) {
        if (m_queue.at(i).sessionId != sessionId)
            continue;
        Job job = m_queue.takeAt(i);
        qInfo().noquote() << QStringLiteral("[Export] #%1 cancelled while queued (position %2): session %3")
                                     .arg(job.id).arg(i + 1).arg(sessionId);
        job.message = QStringLiteral("已取消");
        setEntry(sessionId, entryFor(job, QStringLiteral("cancelled"), 0));
        m_finishedOrder.append(sessionId);
        publishQueue();
        emit jobFinished(sessionId, QStringLiteral("cancelled"));
        return;
    }
    if (m_running && m_running->sessionId == sessionId) {
        qInfo().noquote() << QStringLiteral("[Export] #%1 cancel requested while running: session %2")
                                     .arg(m_running->id).arg(sessionId);
        m_running->cancel->store(true);
        m_running->message = QStringLiteral("取消中");
        publishRunning(true);
        return;                    // becomes "cancelled" when the worker stops (file discarded)
    }
    qInfo().noquote() << QStringLiteral("[Export] cancel from \"%1\" ignored: nothing queued or running").arg(sessionId);
}

QVariantMap HistoryExportManager::entryFor(const Job &job, const QString &state, int queuePosition) const
{
    const bool done = state == QStringLiteral("done");
    return QVariantMap{
        {QStringLiteral("state"), state},
        {QStringLiteral("progress"), job.progress},
        {QStringLiteral("queuePosition"), queuePosition},
        {QStringLiteral("rowsWritten"), double(job.rowsWritten)},
        {QStringLiteral("totalRows"), double(job.totalRows)},
        {QStringLiteral("fileName"), job.fileName},
        {QStringLiteral("url"), done && !job.desktop ? QStringLiteral("/exports/") + job.fileName : QString()},
        {QStringLiteral("downloadPort"), job.desktop ? 0 : int(m_options.downloadPort)},
        {QStringLiteral("savedPath"), done && job.desktop ? QDir::toNativeSeparators(job.targetPath) : QString()},
        {QStringLiteral("message"), job.message},
    };
}

void HistoryExportManager::setEntry(const QString &sessionId, const QVariantMap &entry)
{
    m_status.insert(sessionId, entry);
    // Keep the mirrored map small: drop the oldest finished entries.
    while (m_finishedOrder.size() > kMaxFinishedEntries) {
        const QString oldest = m_finishedOrder.takeFirst();
        if (oldest != sessionId && !sessionBusy(oldest))
            m_status.remove(oldest);
    }
    ++m_publishCount;
    if (m_proxy)
        m_proxy->setHistoryExportStatus(m_status);
}

void HistoryExportManager::publishQueue()
{
    for (qsizetype i = 0; i < m_queue.size(); ++i) {
        Job &job = m_queue[i];
        job.message.clear();                     // the page shows the queue position itself
        const QVariantMap entry = entryFor(job, QStringLiteral("queued"), int(i + 1));
        if (m_status.value(job.sessionId).toMap() != entry)
            setEntry(job.sessionId, entry);
    }
}

void HistoryExportManager::publishRunning(bool force)
{
    if (!m_running)
        return;
    Job &job = *m_running;
    const int progress = job.totalRows > 0
            ? int(std::min<qint64>(100, job.rowsWritten * 100 / job.totalRows))
            : 0;
    // Throttle (spec §3.2: 1% or 500 ms, whichever gives fewer updates): a progress-only update
    // is published when progress rose by at least 1 point AND at least 500 ms
    // passed since the previous one, i.e. at most one update per 1% and at
    // most two per second, whichever is fewer.  State changes are immediate.
    if (!force && (progress - m_lastPublishedProgress < 1 || m_lastPublishClock.elapsed() < 500))
        return;
    job.progress = progress;
    m_lastPublishedProgress = progress;
    m_lastPublishClock.start();
    setEntry(job.sessionId, entryFor(job, QStringLiteral("running"), 0));
}

void HistoryExportManager::onCounted(quint64 id, qint64 totalRows)
{
    if (!m_running || m_running->id != id)
        return;
    m_running->totalRows = totalRows;
    if (!m_running->cancel->load())
        m_running->message = QStringLiteral("匯出中");
    qInfo().noquote() << QStringLiteral("[Export] #%1 %2 row(s) to write").arg(id).arg(totalRows);
    publishRunning(true);
}

void HistoryExportManager::onProgress(quint64 id, qint64 rowsWritten, qint64 totalRows)
{
    if (!m_running || m_running->id != id)
        return;
    m_running->rowsWritten = rowsWritten;
    m_running->totalRows = totalRows;
    publishRunning(false);
}

void HistoryExportManager::onFinished(quint64 id, int outcome, qint64 rowsWritten, qint64 bytes,
                                      const QString &message, const QString &logDetail)
{
    if (!m_running || m_running->id != id)
        return;
    std::unique_ptr<Job> job = std::move(m_running);
    const double elapsedMs = job->runClock.nsecsElapsed() / 1.0e6;
    job->rowsWritten = rowsWritten;
    if (outcome == Done) {
        job->totalRows = rowsWritten;
        job->progress = 100;
        job->message = QStringLiteral("匯出完成,共 %1 筆").arg(rowsWritten);
    } else if (outcome == Cancelled) {
        job->message = QStringLiteral("已取消");
    } else {
        job->message = message.isEmpty() ? QStringLiteral("匯出失敗") : message;
    }
    const QString state = stateName(outcome);
    // Single-pass arg(): logDetail may contain paths/file names with '%'.
    const QString line = QStringLiteral("[Export] #%1 %2: session %3, %4 ms; %5; %6 byte(s) -> %7")
                                 .arg(QString::number(id), state, job->sessionId,
                                      QString::number(elapsedMs, 'f', 1), logDetail, QString::number(bytes),
                                      outcome == Done ? QDir::toNativeSeparators(job->targetPath)
                                                      : QStringLiteral("(no file)"));
    if (outcome == Failed)
        qWarning().noquote() << line;
    else
        qInfo().noquote() << line;
    if (outcome == Done && !job->desktop && !downloadServerListening())
        qWarning().noquote() << "[Export] the download service is not running; the file is only on disk.";

    setEntry(job->sessionId, entryFor(*job, state, 0));
    m_finishedOrder.append(job->sessionId);
    emit jobFinished(job->sessionId, state);
    startNext();
}
