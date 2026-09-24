// w2-041 QTest: History range paging across month files + raw CSV export.
// Real code under test: Core/SqlManager.cpp, Core/HistoryExport.cpp, Core/TaidaFlowProxy.h.
// Data: build/w2-041-bench/data (make_bench_db.py): 30 days over sensor_202608/202609
// (2,592,003 rows) + 100 July rows.
#include <QtTest>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTcpSocket>
#include <QTimer>

#include "HistoryExport.h"
#include "SqlManager.h"
#include "TaidaFlowProxy.h"

#define NOMINMAX
#include <windows.h>
#include <psapi.h>

#include <functional>

namespace {
const QString kData = QStringLiteral(W2041_BENCH_DATA_DIR);
const QString kWork = QStringLiteral(W2041_WORK_DIR);

qint64 localSecs(int y, int m, int d, int hh = 0, int mm = 0, int ss = 0)
{
    return QDateTime(QDate(y, m, d), QTime(hh, mm, ss)).toSecsSinceEpoch();
}
const qint64 k30From = localSecs(2026, 8, 17);
const qint64 k30To = localSecs(2026, 9, 16) - 1;
constexpr qint64 k30Rows = 2592003;          // 1,296,000 + 1,296,000 + 3 duplicates
constexpr double kUnboundedToMs = 8640000000000000.0;

struct Mem { double privMB; double wsMB; };
Mem memNow()
{
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc));
    return {pmc.PrivateUsage / 1048576.0, pmc.WorkingSetSize / 1048576.0};
}

QString fx(double v)
{
    QByteArray b;
    HistoryExport::appendFixed2(b, v);
    return QString::fromLatin1(b);
}

// Main-thread load while an export runs: 1 ms tick (event-loop stall), a blocking
// SqlManager::saveSensorData every second from the main thread (exactly what Manager does)
// and a History range page every 2 s; memory sampled every 100 ms.
struct LoadMonitor
{
    QTimer tick, save, page, mem;
    QElapsedTimer tickClock, since;
    double maxGapMs = 0, maxSaveMs = 0, sumSaveMs = 0, maxPageMs = 0;
    int saves = 0, pages = 0, pagesAnswered = 0;
    double maxPrivMB = 0, maxWsMB = 0;
    quint64 pageId = 0;
    QElapsedTimer pageClock;
    QMetaObject::Connection pageConn;

    void start(SqlManager *sql)
    {
        tick.setTimerType(Qt::PreciseTimer);
        tick.setInterval(1);
        QObject::connect(&tick, &QTimer::timeout, [this]() {
            const double gap = tickClock.nsecsElapsed() / 1.0e6;
            maxGapMs = std::max(maxGapMs, gap);
            tickClock.restart();
        });
        save.setInterval(1000);
        QObject::connect(&save, &QTimer::timeout, [this, sql]() {
            QElapsedTimer t;
            t.start();
            QVector<double> readings(40, 1234.0);
            sql->saveSensorData(QDateTime::currentDateTime(), readings);
            const double ms = t.nsecsElapsed() / 1.0e6;
            maxSaveMs = std::max(maxSaveMs, ms);
            sumSaveMs += ms;
            ++saves;
        });
        page.setInterval(2000);
        QObject::connect(&page, &QTimer::timeout, [this, sql]() {
            static quint64 s_pageSeq = 900000;   // ids keep rising across monitors (superseded rule)
            pageId = ++s_pageSeq;
            ++pages;
            pageClock.start();
            sql->requestSensorHistoryRangePage(pageId, k30From, k30To, 1, 10);
        });
        pageConn = QObject::connect(sql, &SqlManager::sensorHistoryPageReady, &page,
                                    [this](const SensorHistoryPageResult &r) {
            if (r.requestId == pageId) {
                ++pagesAnswered;
                maxPageMs = std::max(maxPageMs, pageClock.nsecsElapsed() / 1.0e6);
            }
        }, Qt::QueuedConnection);
        mem.setInterval(100);
        QObject::connect(&mem, &QTimer::timeout, [this]() {
            const Mem m = memNow();
            maxPrivMB = std::max(maxPrivMB, m.privMB);
            maxWsMB = std::max(maxWsMB, m.wsMB);
        });
        tickClock.start();
        since.start();
        tick.start();
        save.start();
        page.start();
        mem.start();
    }
    void stop()
    {
        tick.stop();
        save.stop();
        page.stop();
        mem.stop();
        QObject::disconnect(pageConn);
    }
    QString summary() const
    {
        return QStringLiteral("main-thread max tick gap %1 ms; %2 blocking saves max %3 ms avg %4 ms; "
                              "%5/%6 History range pages answered, max round trip %7 ms; "
                              "max private %8 MB, max working set %9 MB")
                .arg(maxGapMs, 0, 'f', 2).arg(saves).arg(maxSaveMs, 0, 'f', 2)
                .arg(saves ? sumSaveMs / saves : 0.0, 0, 'f', 2).arg(pagesAnswered).arg(pages)
                .arg(maxPageMs, 0, 'f', 2).arg(maxPrivMB, 0, 'f', 1).arg(maxWsMB, 0, 'f', 1);
    }
};

struct HttpReply
{
    int status = 0;
    QByteArray head;
    QByteArray body;
};

HttpReply rawHttp(const QByteArray &method, const QByteArray &target, quint16 port)
{
    HttpReply reply;
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    if (!socket.waitForConnected(3000))
        return reply;
    socket.write(method + ' ' + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    QByteArray data;
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 10000) {
        if (socket.waitForReadyRead(500))
            data += socket.readAll();
        if (socket.state() == QAbstractSocket::UnconnectedState) {
            data += socket.readAll();
            break;
        }
        const int sep = data.indexOf("\r\n\r\n");
        if (sep >= 0) {
            const QByteArray head = data.left(sep);
            const QRegularExpression lenRe(QStringLiteral("(?im)^content-length:\\s*(\\d+)"));
            const auto m = lenRe.match(QString::fromLatin1(head));
            if (m.hasMatch() && data.size() - sep - 4 >= m.captured(1).toLongLong())
                break;
        }
    }
    const int sep = data.indexOf("\r\n\r\n");
    reply.head = sep >= 0 ? data.left(sep) : data;
    reply.body = sep >= 0 ? data.mid(sep + 4) : QByteArray();
    const QList<QByteArray> first = reply.head.split('\n').value(0).split(' ');
    reply.status = first.size() > 1 ? first.at(1).toInt() : 0;
    return reply;
}

QByteArray header(const HttpReply &r, const QByteArray &name)
{
    for (const QByteArray &line : r.head.split('\n')) {
        const int colon = line.indexOf(':');
        if (colon > 0 && line.left(colon).trimmed().toLower() == name.toLower())
            return line.mid(colon + 1).trimmed();
    }
    return {};
}
} // namespace

class TestW2041Export : public QObject
{
    Q_OBJECT

private:
    SqlManager *m_sql = nullptr;
    quint64 m_nextRequestId = 1;

    SensorHistoryPageResult rangePage(qint64 from, qint64 to, int page)
    {
        const quint64 id = m_nextRequestId++;
        SensorHistoryPageResult out;
        bool got = false;
        QEventLoop loop;
        auto c = connect(m_sql, &SqlManager::sensorHistoryPageReady, &loop,
                         [&](const SensorHistoryPageResult &r) {
            if (r.requestId == id) {
                out = r;
                got = true;
                loop.quit();
            }
        }, Qt::QueuedConnection);
        QTimer::singleShot(120000, &loop, &QEventLoop::quit);
        m_sql->requestSensorHistoryRangePage(id, from, to, page, 10);
        if (!got)
            loop.exec();
        disconnect(c);
        return out;
    }

    // Independent reference: one connection, month files ATTACHed, UNION ALL.
    QList<qint64> referencePage(qint64 from, qint64 to, qint64 offset, qint64 *total)
    {
        QList<qint64> ts;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("ref"));
            db.setDatabaseName(kData + QStringLiteral("/sensor_202609.sqlite"));
            db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
            if (!db.open())
                return ts;
            QSqlQuery q(db);
            q.exec(QStringLiteral("ATTACH DATABASE '%1/sensor_202608.sqlite' AS aug").arg(kData));
            q.exec(QStringLiteral("ATTACH DATABASE '%1/sensor_202607.sqlite' AS jul").arg(kData));
            const QString u = QStringLiteral(
                "SELECT timestamp ts, rowid r, 3 m FROM main.sensor_data WHERE timestamp >= %1 AND timestamp <= %2 "
                "UNION ALL SELECT timestamp, rowid, 2 FROM aug.sensor_data WHERE timestamp >= %1 AND timestamp <= %2 "
                "UNION ALL SELECT timestamp, rowid, 1 FROM jul.sensor_data WHERE timestamp >= %1 AND timestamp <= %2")
                    .arg(from).arg(to);
            q.exec(QStringLiteral("SELECT COUNT(1) FROM (%1)").arg(u));
            q.next();
            *total = q.value(0).toLongLong();
            q.exec(QStringLiteral("SELECT ts FROM (%1) ORDER BY m DESC, ts DESC, r DESC LIMIT 10 OFFSET %2").arg(u).arg(offset));
            while (q.next())
                ts << q.value(0).toLongLong();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("ref"));
        return ts;
    }

    static QStringList tempFiles(const QString &dir)
    {
        return QDir(dir).entryList(QStringList{QStringLiteral("*.csv.*")}, QDir::Files);
    }

    static void makeFile(const QString &path, qint64 size, const QDateTime &mtime)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("\"x\"\r\n");
        QVERIFY(f.resize(size));
        QVERIFY(f.setFileTime(mtime, QFileDevice::FileModificationTime));
        f.close();
    }

private slots:
    void initTestCase()
    {
        QVERIFY2(QFileInfo::exists(kData + QStringLiteral("/sensor_202608.sqlite")),
                 "run make_bench_db.py first");
        QDir(kWork).removeRecursively();
        QVERIFY(QDir().mkpath(kWork));
        m_sql = SqlManager::instance();
        m_sql->setDataDirectory(kData);
        m_sql->setSettingsFile(kWork + QStringLiteral("/settings.sqlite"));
        QVERIFY(m_sql->initialize());
        qInfo().noquote() << "bench data" << kData << "work" << kWork
                          << QStringLiteral("30-day range %1 .. %2 s").arg(k30From).arg(k30To);
    }

    void formatting()
    {
        // JS Number.prototype.toFixed(2) results (History page cellText).
        QCOMPARE(fx(12.125), QStringLiteral("12.13"));
        QCOMPARE(fx(0.375), QStringLiteral("0.38"));
        QCOMPARE(fx(7.625), QStringLiteral("7.63"));
        QCOMPARE(fx(3.875), QStringLiteral("3.88"));
        QCOMPARE(fx(0.125), QStringLiteral("0.13"));
        QCOMPARE(fx(-0.125), QStringLiteral("-0.13"));
        QCOMPARE(fx(1.005), QStringLiteral("1.00"));
        QCOMPARE(fx(2.675), QStringLiteral("2.67"));
        QCOMPARE(fx(8.345), QStringLiteral("8.35"));
        QCOMPARE(fx(0.015), QStringLiteral("0.01"));
        QCOMPARE(fx(0.005), QStringLiteral("0.01"));
        QCOMPARE(fx(0.0), QStringLiteral("0.00"));
        QCOMPARE(fx(-0.0), QStringLiteral("0.00"));
        QCOMPARE(fx(-0.001), QStringLiteral("-0.00"));
        QCOMPARE(fx(2.5), QStringLiteral("2.50"));
        QCOMPARE(fx(65535.0 * (100.0 / 65535.0)), QStringLiteral("100.00"));

        HistoryExport::TimeFormatCache cache;
        for (qint64 t = k30From - 3; t < k30From + 200; ++t) {
            QByteArray b;
            HistoryExport::appendLocalTime(b, t, cache);
            QCOMPARE(QString::fromLatin1(b),
                     QDateTime::fromSecsSinceEpoch(t).toString(QStringLiteral("yyyy/MM/dd HH:mm:ss")));
        }

        QVERIFY(HistoryExport::isValidExportFileName(QStringLiteral("web-3f9a_20260924_101500.csv")));
        QVERIFY(HistoryExport::isValidExportFileName(QStringLiteral("desktop_20260924_101500.csv")));
        for (const char *bad : {"../web_20260924_101500.csv", "..\\web_20260924_101500.csv", "..",
                                "web_20260924_101500.csv.a1b2c3", "C:web_20260924_101500.csv",
                                "web_20260924_101500.CSV", "%2e%2e%2fweb_20260924_101500.csv",
                                "a/b_20260924_101500.csv", "", "web.csv"})
            QVERIFY2(!HistoryExport::isValidExportFileName(QString::fromLatin1(bad)), bad);
        QVERIFY(HistoryExport::isValidSessionId(QStringLiteral("web-3f9a")));
        QVERIFY(!HistoryExport::isValidSessionId(QStringLiteral("../x")));

        qint64 f = 0, t = 0;
        QVERIFY(HistoryExport::rangeMsToSecs(0, kUnboundedToMs, &f, &t));
        QCOMPARE(f, qint64(1));
        QCOMPARE(t, qint64(8640000000000));
        QVERIFY(HistoryExport::rangeMsToSecs(1000.5, 2999.9, &f, &t));
        QCOMPARE(f, qint64(2));
        QCOMPARE(t, qint64(2));
        QVERIFY(!HistoryExport::rangeMsToSecs(qQNaN(), 1, &f, &t));
        QVERIFY(!HistoryExport::rangeMsToSecs(5000, 1000, &f, &t));

        const QByteArray head = HistoryExport::csvHeader();
        QVERIFY(head.startsWith("\xEF\xBB\xBF\"" ));
        QCOMPARE(QString::fromUtf8(head.mid(3)),
                 QStringLiteral("\"序號\",\"時間\",\"TT-01 (°C)\",\"TT-02 (°C)\",\"TT-03 (°C)\",\"TT-04 (°C)\","
                                "\"PT-01 (bar)\",\"PT-02 (bar)\",\"PT-03 (bar)\",\"PT-04 (bar)\",\"PT-05 (bar)\","
                                "\"PT-06 (bar)\",\"PT-07 (bar)\",\"FM-01 (L/min)\",\"M1 (%)\",\"M2 (%)\","
                                "\"M3 (%)\",\"M4 (%)\"\r\n"));
    }

    void rangePagingAcrossMonths()
    {
        // 30-day range over two month files.
        qint64 refTotal = 0;
        const qint64 sepRows = 1296003;
        const qint64 lastPage = (k30Rows + 9) / 10;
        for (qint64 page : {qint64(1), qint64(2), sepRows / 10, sepRows / 10 + 1, sepRows / 10 + 2, lastPage - 1, lastPage}) {
            const SensorHistoryPageResult r = rangePage(k30From, k30To, int(page));
            QVERIFY2(r.ok && r.countOk, qPrintable(r.errorMessage));
            QCOMPARE(r.totalRows, k30Rows);
            QCOMPARE(r.months, 2);
            const QList<qint64> ref = referencePage(k30From, k30To, (page - 1) * 10, &refTotal);
            QCOMPARE(refTotal, k30Rows);
            QList<qint64> got;
            for (const QJsonValue &v : r.samples)
                got << qint64(v.toObject().value(QStringLiteral("ts")).toDouble());
            QCOMPARE(got, ref);
            qInfo().noquote() << QStringLiteral("30-day page %1: %2 row(s) %3 .. %4; count %5 ms (%6 cached) + page %7 ms")
                                         .arg(page).arg(got.size()).arg(got.value(0)).arg(got.value(got.size() - 1))
                                         .arg(r.countMs, 0, 'f', 2).arg(r.countCacheHits).arg(r.pageMs, 0, 'f', 2);
        }
        // The page that crosses 2026-09-01 00:00: 3 September + 7 August rows.
        const SensorHistoryPageResult cross = rangePage(k30From, k30To, int(sepRows / 10 + 1));
        QCOMPARE(cross.samples.size(), 10);
        QVERIFY(qint64(cross.samples.at(2).toObject().value(QStringLiteral("ts")).toDouble()) >= localSecs(2026, 9, 1));
        QVERIFY(qint64(cross.samples.at(3).toObject().value(QStringLiteral("ts")).toDouble()) < localSecs(2026, 9, 1));

        // "All months" (the UI's 0 .. 8640000000000000 ms -> 1 .. 8.64e12 s), 3 files.
        qint64 f = 0, t = 0;
        QVERIFY(HistoryExport::rangeMsToSecs(0, kUnboundedToMs, &f, &t));
        SensorHistoryPageResult all = rangePage(f, t, 1);
        QVERIFY(all.ok);
        QCOMPARE(all.months, 3);
        QList<qint64> ref = referencePage(f, t, 0, &refTotal);
        QCOMPARE(all.totalRows, refTotal);
        const qint64 allLast = (all.totalRows + 9) / 10;
        SensorHistoryPageResult last = rangePage(f, t, int(allLast));
        ref = referencePage(f, t, (allLast - 1) * 10, &refTotal);
        QList<qint64> got;
        for (const QJsonValue &v : last.samples)
            got << qint64(v.toObject().value(QStringLiteral("ts")).toDouble());
        QCOMPARE(got, ref);
        QVERIFY(got.last() > 0);                       // timestamps 0 / -5 are never paged
        qInfo().noquote() << QStringLiteral("all months: %1 row(s) in %2 files; last page %3 = %4 row(s) ending at %5 (July)")
                                     .arg(all.totalRows).arg(all.months).arg(allLast).arg(got.size()).arg(got.last());

        // Count cache: unchanged files are not recounted; a write invalidates only its month.
        SensorHistoryPageResult again = rangePage(f, t, 1);
        QCOMPARE(again.countCacheHits, 3);
        QCOMPARE(again.totalRows, all.totalRows);
        QVERIFY(m_sql->saveSensorData(QDateTime::currentDateTime(), QVector<double>(40, 7.0)));
        SensorHistoryPageResult afterWrite = rangePage(f, t, 1);
        const int writeMonthHits = QDate::currentDate().month() == 9 && QDate::currentDate().year() == 2026 ? 2 : 3;
        qInfo().noquote() << QStringLiteral("count cache: uncached %1 ms, all cached %2 ms (%3 hits), after a write %4 ms (%5 hits), total %6 -> %7")
                                     .arg(all.countMs, 0, 'f', 2).arg(again.countMs, 0, 'f', 2).arg(again.countCacheHits)
                                     .arg(afterWrite.countMs, 0, 'f', 2).arg(afterWrite.countCacheHits)
                                     .arg(again.totalRows).arg(afterWrite.totalRows);
        if (writeMonthHits == 2) {
            QCOMPARE(afterWrite.countCacheHits, 2);
            QCOMPARE(afterWrite.totalRows, again.totalRows + 1);
        }
    }

    void export30DaysCorrectMemoryMainThread()
    {
        const QString dir = kWork + QStringLiteral("/exports_30d");
        TaidaFlowProxy proxy;
        HistoryExportManager::Options options;
        options.exportDir = dir;
        options.startDownloadServer = false;
        HistoryExportManager manager(&proxy, m_sql, options);

        // Baseline: same load, no export.
        LoadMonitor idle;
        idle.start(m_sql);
        QTest::qWait(6000);
        idle.stop();
        qInfo().noquote() << "IDLE (no export, 6 s):" << idle.summary();

        struct Pub { qint64 ms; QString state; int progress; qint64 rows; };
        QList<Pub> pubs;
        QElapsedTimer clock;
        clock.start();
        const QString sid = QStringLiteral("web-t30d");
        connect(&proxy, &TaidaFlowProxy::historyExportStatusChanged, this, [&](const QVariantMap &all) {
            const QVariantMap e = all.value(sid).toMap();
            if (!e.isEmpty())
                pubs.append({clock.elapsed(), e.value(QStringLiteral("state")).toString(),
                             e.value(QStringLiteral("progress")).toInt(),
                             qint64(e.value(QStringLiteral("rowsWritten")).toDouble())});
        });
        QString finishedState;
        connect(&manager, &HistoryExportManager::jobFinished, this,
                [&](const QString &s, const QString &st) { if (s == sid) finishedState = st; });

        LoadMonitor busy;
        const Mem before = memNow();
        busy.start(m_sql);
        QElapsedTimer run;
        run.start();
        emit proxy.historyExportRequested(sid, double(k30From) * 1000.0, double(k30To) * 1000.0 + 999.0);
        QTRY_VERIFY_WITH_TIMEOUT(!finishedState.isEmpty(), 600000);
        const double runMs = run.nsecsElapsed() / 1.0e6;
        busy.stop();
        QCOMPARE(finishedState, QStringLiteral("done"));

        const QVariantMap entry = proxy.historyExportStatus().value(sid).toMap();
        const QString fileName = entry.value(QStringLiteral("fileName")).toString();
        QVERIFY(QRegularExpression(QStringLiteral("^web-t30d_\\d{8}_\\d{6}\\.csv$")).match(fileName).hasMatch());
        QCOMPARE(entry.value(QStringLiteral("url")).toString(), QStringLiteral("/exports/") + fileName);
        QCOMPARE(entry.value(QStringLiteral("downloadPort")).toInt(), 8124);
        QCOMPARE(entry.value(QStringLiteral("progress")).toInt(), 100);
        QCOMPARE(qint64(entry.value(QStringLiteral("rowsWritten")).toDouble()), k30Rows);
        QCOMPARE(qint64(entry.value(QStringLiteral("totalRows")).toDouble()), k30Rows);
        QCOMPARE(entry.value(QStringLiteral("queuePosition")).toInt(), 0);
        QCOMPARE(entry.value(QStringLiteral("savedPath")).toString(), QString());
        const QString path = dir + QStringLiteral("/") + fileName;
        QVERIFY(QFileInfo::exists(path));
        QVERIFY(tempFiles(dir).isEmpty());
        const qint64 bytes = QFileInfo(path).size();

        // Throttle: progress-only publications at least 500 ms and 1 point apart.
        int running = 0;
        qint64 minGap = std::numeric_limits<qint64>::max();
        int minStep = 100;
        for (int i = 1; i < pubs.size(); ++i) {
            if (pubs[i].state != QStringLiteral("running") || pubs[i - 1].state != QStringLiteral("running"))
                continue;
            if (pubs[i].progress == 0)
                continue;                              // forced (state/count) update
            ++running;
            minGap = std::min(minGap, pubs[i].ms - pubs[i - 1].ms);
            minStep = std::min(minStep, pubs[i].progress - pubs[i - 1].progress);
        }
        qInfo().noquote() << QStringLiteral("status publications for %1: %2 total, %3 progress-only; min interval %4 ms, min step %5 point(s)")
                                     .arg(sid).arg(pubs.size()).arg(running).arg(minGap).arg(minStep);
        QVERIFY(minGap >= 499);
        QVERIFY(minStep >= 1);

        qInfo().noquote() << QStringLiteral("EXPORT 30 days: %1 rows, %2 bytes (%3 MB) in %4 ms (%5 rows/s) -> %6")
                                     .arg(k30Rows).arg(bytes).arg(bytes / 1048576.0, 0, 'f', 1)
                                     .arg(runMs, 0, 'f', 0).arg(k30Rows / (runMs / 1000.0), 0, 'f', 0)
                                     .arg(QDir::toNativeSeparators(path));
        qInfo().noquote() << "BUSY (during export):" << busy.summary();
        qInfo().noquote() << QStringLiteral("memory before export: private %1 MB, working set %2 MB; "
                                            "max during: private +%3 MB, working set +%4 MB; file %5 MB")
                                     .arg(before.privMB, 0, 'f', 1).arg(before.wsMB, 0, 'f', 1)
                                     .arg(busy.maxPrivMB - before.privMB, 0, 'f', 1)
                                     .arg(busy.maxWsMB - before.wsMB, 0, 'f', 1)
                                     .arg(bytes / 1048576.0, 0, 'f', 1);
        QVERIFY(busy.pagesAnswered >= 1);
        // Memory must not follow the file size (hundreds of MB).
        QVERIFY(busy.maxPrivMB - before.privMB < 64.0);

        // Same load, 1-day export: the peak does not depend on the file size.
        const QString sid1 = QStringLiteral("web-t1d");
        finishedState.clear();
        connect(&manager, &HistoryExportManager::jobFinished, this,
                [&](const QString &s, const QString &st) { if (s == sid1) finishedState = st; });
        LoadMonitor smallLoad;
        const Mem before1 = memNow();
        smallLoad.start(m_sql);
        emit proxy.historyExportRequested(sid1, double(localSecs(2026, 9, 10)) * 1000.0,
                                          double(localSecs(2026, 9, 11)) * 1000.0 - 1.0);
        QTRY_VERIFY_WITH_TIMEOUT(!finishedState.isEmpty(), 120000);
        QTest::qWait(300);
        smallLoad.stop();
        const QString file1 = dir + QStringLiteral("/")
                + proxy.historyExportStatus().value(sid1).toMap().value(QStringLiteral("fileName")).toString();
        qInfo().noquote() << QStringLiteral("EXPORT 1 day: %1 rows, %2 MB; max private +%3 MB, working set +%4 MB (30 days: +%5 / +%6 MB)")
                                     .arg(qint64(proxy.historyExportStatus().value(sid1).toMap().value(QStringLiteral("rowsWritten")).toDouble()))
                                     .arg(QFileInfo(file1).size() / 1048576.0, 0, 'f', 1)
                                     .arg(smallLoad.maxPrivMB - before1.privMB, 0, 'f', 1)
                                     .arg(smallLoad.maxWsMB - before1.wsMB, 0, 'f', 1)
                                     .arg(busy.maxPrivMB - before.privMB, 0, 'f', 1)
                                     .arg(busy.maxWsMB - before.wsMB, 0, 'f', 1);
        qInfo().noquote() << "VERIFY_CSV" << QDir::toNativeSeparators(path) << k30From << k30To;
        qInfo().noquote() << "VERIFY_CSV_1DAY" << QDir::toNativeSeparators(file1) << localSecs(2026, 9, 10)
                          << localSecs(2026, 9, 11) - 1;
    }

    void queueOrderAndCancel()
    {
        const QString dir = kWork + QStringLiteral("/exports_queue");
        const QString desktopDir = kWork + QStringLiteral("/desktop_target");
        QDir().mkpath(desktopDir);
        TaidaFlowProxy proxy;
        HistoryExportManager::Options options;
        options.exportDir = dir;
        options.startDownloadServer = false;
        HistoryExportManager manager(&proxy, m_sql, options);
        QStringList order;
        connect(&manager, &HistoryExportManager::jobFinished, this,
                [&](const QString &s, const QString &st) { order << s + QLatin1Char('=') + st; });
        const auto entry = [&proxy](const QString &sid) { return proxy.historyExportStatus().value(sid).toMap(); };
        const auto state = [&](const QString &sid) { return entry(sid).value(QStringLiteral("state")).toString(); };
        const auto pos = [&](const QString &sid) { return entry(sid).value(QStringLiteral("queuePosition")).toInt(); };

        const double f30 = double(k30From) * 1000.0, t30 = double(k30To) * 1000.0 + 999.0;
        const double f1 = double(localSecs(2026, 9, 12)) * 1000.0, t1 = double(localSecs(2026, 9, 13)) * 1000.0 - 1.0;
        emit proxy.historyExportRequested(QStringLiteral("web-aaaa"), f30, t30);
        emit proxy.historyExportRequested(QStringLiteral("web-bbbb"), f30, t30);
        emit proxy.historyExportRequested(QStringLiteral("web-cccc"), f1, t1);
        manager.enqueueDesktopExport(desktopDir + QStringLiteral("/chosen by user.csv"), f1, t1);
        QCOMPARE(state(QStringLiteral("web-aaaa")), QStringLiteral("running"));
        QCOMPARE(pos(QStringLiteral("web-aaaa")), 0);
        QCOMPARE(state(QStringLiteral("web-bbbb")), QStringLiteral("queued"));
        QCOMPARE(pos(QStringLiteral("web-bbbb")), 1);
        QCOMPARE(pos(QStringLiteral("web-cccc")), 2);
        QCOMPARE(state(QStringLiteral("desktop")), QStringLiteral("queued"));
        QCOMPARE(pos(QStringLiteral("desktop")), 3);
        qInfo().noquote() << "queue after 4 requests: aaaa running(0), bbbb queued(1), cccc queued(2), desktop queued(3)";

        // Same session again while running: refused, the running job is untouched.
        emit proxy.historyExportRequested(QStringLiteral("web-aaaa"), f1, t1);
        QCOMPARE(state(QStringLiteral("web-aaaa")), QStringLiteral("running"));
        QCOMPARE(entry(QStringLiteral("web-aaaa")).value(QStringLiteral("message")).toString(),
                 QStringLiteral("已有匯出進行中,請取消後再試"));
        QCOMPARE(pos(QStringLiteral("web-bbbb")), 1);

        // Cancel a queued job: positions move up.
        emit proxy.historyExportCancelRequested(QStringLiteral("web-bbbb"));
        QCOMPARE(state(QStringLiteral("web-bbbb")), QStringLiteral("cancelled"));
        QCOMPARE(pos(QStringLiteral("web-cccc")), 1);
        QCOMPARE(pos(QStringLiteral("desktop")), 2);

        // Cancel the running job once it has written rows: the partial file is removed.
        QTRY_VERIFY_WITH_TIMEOUT(entry(QStringLiteral("web-aaaa")).value(QStringLiteral("rowsWritten")).toDouble() > 0, 120000);
        const QStringList partial = tempFiles(dir);
        qInfo().noquote() << "while web-aaaa runs, unfinished file(s) in the export folder:" << partial;
        QCOMPARE(partial.size(), 1);
        emit proxy.historyExportCancelRequested(QStringLiteral("web-aaaa"));
        QTRY_COMPARE_WITH_TIMEOUT(state(QStringLiteral("web-aaaa")), QStringLiteral("cancelled"), 30000);
        QTRY_COMPARE_WITH_TIMEOUT(state(QStringLiteral("desktop")), QStringLiteral("done"), 120000);
        QCOMPARE(state(QStringLiteral("web-cccc")), QStringLiteral("done"));
        QCOMPARE(order, (QStringList{QStringLiteral("web-bbbb=cancelled"), QStringLiteral("web-aaaa=cancelled"),
                                     QStringLiteral("web-cccc=done"), QStringLiteral("desktop=done")}));
        qInfo().noquote() << "finish order:" << order.join(QStringLiteral(", "));

        // Export folder: only web-cccc's file (no partial file of web-aaaa, nothing of bbbb).
        const QStringList left = QDir(dir).entryList(QDir::Files);
        qInfo().noquote() << "export folder after the run:" << left;
        QCOMPARE(left.size(), 1);
        QVERIFY(left.first().startsWith(QStringLiteral("web-cccc_")));
        const QVariantMap c = entry(QStringLiteral("web-cccc"));
        QCOMPARE(c.value(QStringLiteral("url")).toString(), QStringLiteral("/exports/") + left.first());
        // Desktop: written where the user chose, not in the export folder, savedPath set.
        const QVariantMap d = entry(QStringLiteral("desktop"));
        const QString chosen = desktopDir + QStringLiteral("/chosen by user.csv");
        QCOMPARE(d.value(QStringLiteral("savedPath")).toString(), QDir::toNativeSeparators(chosen));
        QCOMPARE(d.value(QStringLiteral("url")).toString(), QString());
        QCOMPARE(d.value(QStringLiteral("downloadPort")).toInt(), 0);
        QFile a(chosen), b(dir + QStringLiteral("/") + left.first());
        QVERIFY(a.open(QIODevice::ReadOnly) && b.open(QIODevice::ReadOnly));
        QCOMPARE(a.readAll(), b.readAll());            // same range -> same bytes
        QCOMPARE(qint64(d.value(QStringLiteral("rowsWritten")).toDouble()), qint64(86400));
    }

    void cleanupMoreThan20Files()
    {
        const QString dir = kWork + QStringLiteral("/exports_21");
        QDir().mkpath(dir);
        const QDateTime base = QDateTime::currentDateTime().addDays(-2);
        for (int i = 0; i < 20; ++i)
            makeFile(dir + QStringLiteral("/web-old%1_20260901_0000%2.csv").arg(i, 2, 10, QLatin1Char('0')).arg(i, 2, 10, QLatin1Char('0')),
                     1024, base.addSecs(60 * i));
        TaidaFlowProxy proxy;
        HistoryExportManager::Options options;
        options.exportDir = dir;
        options.startDownloadServer = false;
        HistoryExportManager manager(&proxy, m_sql, options);
        QString st;
        connect(&manager, &HistoryExportManager::jobFinished, this, [&](const QString &, const QString &s) { st = s; });
        emit proxy.historyExportRequested(QStringLiteral("web-new1"), double(localSecs(2026, 9, 15, 23)) * 1000.0,
                                          double(localSecs(2026, 9, 16)) * 1000.0 - 1.0);
        QTRY_COMPARE_WITH_TIMEOUT(st, QStringLiteral("done"), 60000);
        const QStringList files = QDir(dir).entryList(QStringList{QStringLiteral("*.csv")}, QDir::Files, QDir::Name);
        qInfo().noquote() << "21-file case, left:" << files.size() << "files; oldest two now:" << files.mid(0, 2);
        QCOMPARE(files.size(), 20);
        QVERIFY(!files.contains(QStringLiteral("web-old00_20260901_000000.csv")));   // the oldest
        QVERIFY(files.contains(QStringLiteral("web-old01_20260901_000001.csv")));
        QVERIFY(std::any_of(files.begin(), files.end(), [](const QString &f) { return f.startsWith(QStringLiteral("web-new1_")); }));
    }

    void cleanupOver2GB()
    {
        const QString dir = kWork + QStringLiteral("/exports_2gb");
        QDir().mkpath(dir);
        const QDateTime base = QDateTime::currentDateTime().addDays(-1);
        const qint64 mb800 = 800LL * 1024 * 1024;
        makeFile(dir + QStringLiteral("/web-big1_20260920_000001.csv"), mb800, base);
        makeFile(dir + QStringLiteral("/web-big2_20260920_000002.csv"), mb800, base.addSecs(60));
        makeFile(dir + QStringLiteral("/web-big3_20260920_000003.csv"), mb800, base.addSecs(120));
        TaidaFlowProxy proxy;
        HistoryExportManager::Options options;
        options.exportDir = dir;
        options.startDownloadServer = false;
        HistoryExportManager manager(&proxy, m_sql, options);
        QString st;
        connect(&manager, &HistoryExportManager::jobFinished, this, [&](const QString &, const QString &s) { st = s; });
        emit proxy.historyExportRequested(QStringLiteral("web-new2"), double(localSecs(2026, 9, 15, 23)) * 1000.0,
                                          double(localSecs(2026, 9, 16)) * 1000.0 - 1.0);
        QTRY_COMPARE_WITH_TIMEOUT(st, QStringLiteral("done"), 60000);
        const QStringList files = QDir(dir).entryList(QStringList{QStringLiteral("*.csv")}, QDir::Files, QDir::Name);
        qint64 total = 0;
        for (const QString &f : files)
            total += QFileInfo(dir + QStringLiteral("/") + f).size();
        qInfo().noquote() << ">2 GB case (3 x 800 MB + new): left" << files << total << "bytes";
        QCOMPARE(files.size(), 3);
        QVERIFY(!files.contains(QStringLiteral("web-big1_20260920_000001.csv")));
        QVERIFY(total <= HistoryExport::kMaxExportBytes);

        // A single new file larger than 2 GB is kept (older files still go).
        const QString dir2 = kWork + QStringLiteral("/exports_oversize");
        QDir().mkpath(dir2);
        makeFile(dir2 + QStringLiteral("/web-old_20260920_000001.csv"), 1024, base);
        makeFile(dir2 + QStringLiteral("/web-huge_20260920_000002.csv"), HistoryExport::kMaxExportBytes + 100 * 1024 * 1024,
                 base.addSecs(60));
        const HistoryExport::CleanupResult r = HistoryExport::cleanupExportDir(dir2, QStringLiteral("web-huge_20260920_000002.csv"));
        qInfo().noquote() << "oversize case: removed" << r.removed << "keptOversize" << r.keptOversize
                          << "left" << r.filesLeft << r.bytesLeft;
        QVERIFY(r.keptOversize);
        QCOMPARE(r.removed, QStringList{QStringLiteral("web-old_20260920_000001.csv")});
        QVERIFY(QFileInfo::exists(dir2 + QStringLiteral("/web-huge_20260920_000002.csv")));
        QDir(dir).removeRecursively();
        QDir(dir2).removeRecursively();
    }

    void downloadServiceAndTraversal()
    {
        const QString dir = kWork + QStringLiteral("/exports_http");
        QDir().mkpath(dir);
        // A file with a valid export name OUTSIDE the export folder: must never be served.
        QFile secret(kWork + QStringLiteral("/secret_20260924_000000.csv"));
        QVERIFY(secret.open(QIODevice::WriteOnly));
        secret.write("SECRET-OUTSIDE-EXPORT-FOLDER");
        secret.close();

        TaidaFlowProxy proxy;
        HistoryExportManager::Options options;
        options.exportDir = dir;
        HistoryExportManager manager(&proxy, m_sql, options);
        QVERIFY(manager.downloadServerListening());
        QString st;
        connect(&manager, &HistoryExportManager::jobFinished, this, [&](const QString &, const QString &s) { st = s; });
        emit proxy.historyExportRequested(QStringLiteral("web-http"), double(localSecs(2026, 9, 14)) * 1000.0,
                                          double(localSecs(2026, 9, 15)) * 1000.0 - 1.0);
        QTRY_COMPARE_WITH_TIMEOUT(st, QStringLiteral("done"), 60000);
        const QVariantMap e = proxy.historyExportStatus().value(QStringLiteral("web-http")).toMap();
        const QString url = e.value(QStringLiteral("url")).toString();
        const quint16 port = quint16(e.value(QStringLiteral("downloadPort")).toInt());
        QCOMPARE(port, quint16(8124));
        QFile file(dir + QStringLiteral("/") + e.value(QStringLiteral("fileName")).toString());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray expected = file.readAll();

        const HttpReply ok = rawHttp("GET", url.toLatin1(), port);
        qInfo().noquote() << "GET" << url << "->" << ok.status << "\n" << QString::fromLatin1(ok.head);
        QCOMPARE(ok.status, 200);
        QCOMPARE(header(ok, "Content-Disposition"),
                 QByteArray("attachment; filename=\"") + e.value(QStringLiteral("fileName")).toString().toLatin1() + '"');
        QCOMPARE(header(ok, "Access-Control-Allow-Origin"), QByteArray("*"));
        QCOMPARE(header(ok, "Content-Length").toLongLong(), qint64(expected.size()));
        QCOMPARE(ok.body, expected);

        const QList<QByteArray> attacks{
            "/exports/../secret_20260924_000000.csv",
            "/exports/..%2Fsecret_20260924_000000.csv",
            "/exports/%2e%2e%2fsecret_20260924_000000.csv",
            "/exports/%2E%2E/secret_20260924_000000.csv",
            "/exports/..%5Csecret_20260924_000000.csv",
            "/exports/..\\secret_20260924_000000.csv",
            "/exports/%2e%2e",
            "/exports/C:%5Cwindows%5Cwin.ini",
            "/exports//secret_20260924_000000.csv",
            "/secret_20260924_000000.csv",
            "/exports/",
            "/exports/web-http_20260924_000000.csv.abc123",
            "/exports/nothere_20260101_000000.csv",
        };
        for (const QByteArray &target : attacks) {
            const HttpReply r = rawHttp("GET", target, port);
            qInfo().noquote() << "GET" << target << "->" << r.status << r.body.left(40).trimmed();
            QVERIFY2(r.status >= 400 && r.status < 500, target.constData());
            QVERIFY(!r.body.contains("SECRET"));
        }
        const HttpReply post = rawHttp("POST", url.toLatin1(), port);
        qInfo().noquote() << "POST" << url << "->" << post.status;
        QCOMPARE(post.status, 404);

        // Port already taken: start() fails, logs, nothing crashes.
        ExportDownloadServer second(dir, 8124);
        QVERIFY(!second.start());
        QVERIFY(manager.downloadServerListening());
        QFile::remove(kWork + QStringLiteral("/secret_20260924_000000.csv"));
    }
};

QTEST_MAIN(TestW2041Export)
#include "tst_w2041_export.moc"
