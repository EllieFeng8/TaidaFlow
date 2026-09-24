// w2-039 QTest: History query of taidaflow/Core/SqlManager on a 30-day sensor_data file.
//
// Uses the real SqlManager (Core/SqlManager.cpp compiled in) against the DB made by
// docs/evidence/w2-039/tools/history_query_bench.py: 2,592,000 rows, 1 row per second
// from 2026-09-01 00:00 local time (ts = monthStart + i).  Nothing is written to that DB
// (ensureDataSchema only runs CREATE ... IF NOT EXISTS).
//
// Measured / checked:
//  - newest-first page contents (page 1, 1000, 10000, last, past-the-end)
//  - old load (Core before w2-039: countSensorRange + queryRangeJsonPaged ASC with the
//    chronological re-paging) vs new load (1 COUNT + DESC page), same Release build
//  - main-thread event-loop stall: old load called on the main thread vs new async
//    request (1 ms PreciseTimer tick gaps)
//  - stale requests: only the newest of 5 rapid requests is executed/applied
//  - receiver destroyed while a request runs: no callback afterwards
//  - design alternative: slicing the existing fetchSensorData(date, page*10)
#include <QtTest>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <algorithm>

#include "SqlManager.h"

namespace {
constexpr int kPage = 10;
constexpr qint64 kRows = 2'592'000;
qint64 monthStart() { return QDateTime(QDate(2026, 9, 1), QTime(0, 0)).toSecsSinceEpoch(); }
qint64 monthEnd() { return QDateTime(QDate(2026, 10, 1), QTime(0, 0)).toSecsSinceEpoch() - 1; }
qint64 newestTsOfPage(int page) { return monthStart() + kRows - 1 - qint64(page - 1) * kPage; }
quint64 g_nextId = 1000;

// Max gap between 1 ms ticks of a main-thread timer = longest main-thread stall.
class TickProbe : public QObject
{
public:
    TickProbe()
    {
        m_timer.setTimerType(Qt::PreciseTimer);
        m_timer.setInterval(1);
        connect(&m_timer, &QTimer::timeout, this, [this] {
            const qint64 now = m_clock.nsecsElapsed();
            if (m_last >= 0)
                m_maxGapNs = std::max(m_maxGapNs, now - m_last);
            m_last = now;
            ++m_ticks;
        });
    }
    void start() { m_last = -1; m_maxGapNs = 0; m_ticks = 0; m_clock.start(); m_timer.start(); }
    void stop() { m_timer.stop(); }
    double maxGapMs() const { return m_maxGapNs / 1.0e6; }
    int ticks() const { return m_ticks; }

private:
    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_last = -1;
    qint64 m_maxGapNs = 0;
    int m_ticks = 0;
};

// Core::loadHistoryRecords before w2-039 (page 1 = newest): count, then the ASC SQL
// page(s) covering [total - page*10, total - (page-1)*10), each with its own COUNT.
bool oldLoad(SqlManager *sql, int page, QJsonArray *newestFirst)
{
    qint64 total = 0;
    if (!sql->countSensorRange(monthStart(), monthEnd(), &total))
        return false;
    const qint64 endRow = total - qint64(page - 1) * kPage;
    const qint64 startRow = qMax<qint64>(0, endRow - kPage);
    const int firstSqlPage = int(startRow / kPage) + 1;
    const int lastSqlPage = int((endRow - 1) / kPage) + 1;
    QJsonArray samples;
    for (int sqlPage = firstSqlPage; sqlPage <= lastSqlPage; ++sqlPage) {
        QJsonArray pageSamples;
        if (!sql->queryRangeJsonPaged(monthStart(), monthEnd(), sqlPage, kPage, &pageSamples))
            return false;
        const qint64 pageStartRow = qint64(sqlPage - 1) * kPage;
        const qint64 copyStart = qMax(startRow, pageStartRow);
        const qint64 copyEnd = qMin(endRow, pageStartRow + pageSamples.size());
        for (qint64 row = copyStart; row < copyEnd; ++row)
            samples.append(pageSamples.at(row - pageStartRow));
    }
    *newestFirst = QJsonArray();
    for (qsizetype i = samples.size(); i > 0; --i)
        newestFirst->append(samples.at(i - 1));
    return true;
}

// Receives results on the main thread (queued), like Core does.
class Collector : public QObject
{
public:
    explicit Collector(SqlManager *sql)
    {
        connect(sql, &SqlManager::sensorHistoryPageReady, this,
                [this](const SensorHistoryPageResult &r) { results.append(r); }, Qt::QueuedConnection);
    }
    int count() const { return int(results.size()); }
    QList<SensorHistoryPageResult> results;
};

qint64 tsAt(const QJsonArray &a, int i) { return qint64(a.at(i).toObject().value("ts").toDouble()); }
}

class TstW2039History : public QObject
{
    Q_OBJECT

private:
    SqlManager *sql = nullptr;

    SensorHistoryPageResult requestAndWait(int page, double *maxGapMs = nullptr, double *postUs = nullptr)
    {
        Collector spy(sql);
        TickProbe probe;
        probe.start();
        QTest::qWait(20);                       // probe runs before the request
        const quint64 id = ++g_nextId;
        QElapsedTimer post;
        post.start();
        sql->requestSensorHistoryPage(id, monthStart(), monthEnd(), page, kPage);
        if (postUs)
            *postUs = post.nsecsElapsed() / 1000.0;
        [&] { QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 10000); }();
        QTest::qWait(20);                       // ... and after the result
        probe.stop();
        if (maxGapMs)
            *maxGapMs = probe.maxGapMs();
        for (const SensorHistoryPageResult &r : std::as_const(spy.results)) {
            if (r.requestId == id)
                return r;
        }
        return {};
    }

private slots:
    void initTestCase()
    {
        QDir().mkpath(W2039_WORK_DIR);
        QDir::setCurrent(W2039_WORK_DIR);
        const QString dbFile = QStringLiteral(W2039_BENCH_DATA_DIR "/sensor_202609.sqlite");
        if (!QFileInfo::exists(dbFile))
            QSKIP("30-day DB missing: run docs/evidence/w2-039/tools/history_query_bench.py first");
        sql = SqlManager::instance();
        sql->setDataDirectory(QStringLiteral(W2039_BENCH_DATA_DIR));
        sql->setSettingsFile(QStringLiteral(W2039_WORK_DIR "/settings.sqlite"));
        QVERIFY(sql->initialize());
        qint64 total = 0;
        QVERIFY(sql->countSensorRange(monthStart(), monthEnd(), &total));
        QCOMPARE(total, kRows);
        QJsonArray warm;                        // warm the page cache once
        QVERIFY(sql->querySensorRangeDescPaged(monthStart(), monthEnd(), 1, kPage, &warm));
    }

    void descPageContents_data()
    {
        QTest::addColumn<int>("page");
        QTest::newRow("page 1") << 1;
        QTest::newRow("page 2") << 2;
        QTest::newRow("page 1000") << 1000;
        QTest::newRow("page 10000") << 10000;
        QTest::newRow("last page 259200") << int(kRows / kPage);
    }
    void descPageContents()
    {
        QFETCH(int, page);
        QJsonArray rows;
        QString err;
        QVERIFY2(sql->querySensorRangeDescPaged(monthStart(), monthEnd(), page, kPage, &rows, &err),
                 qPrintable(err));
        QCOMPARE(rows.size(), kPage);
        for (int i = 0; i < kPage; ++i)
            QCOMPARE(tsAt(rows, i), newestTsOfPage(page) - i);          // newest first, no gaps
        QJsonArray old;
        QVERIFY(oldLoad(sql, page, &old));                                // same rows as before
        QCOMPARE(old.size(), rows.size());
        for (int i = 0; i < kPage; ++i)
            QCOMPARE(tsAt(old, i), tsAt(rows, i));
    }

    void descPageValidation()
    {
        QJsonArray rows;
        QString err;
        QVERIFY(!sql->querySensorRangeDescPaged(monthStart(), monthEnd() + 86400, 1, kPage, &rows, &err));
        QCOMPARE(err, QStringLiteral("from and to must be in the same month"));
        QVERIFY(!sql->querySensorRangeDescPaged(monthStart(), monthEnd(), 0, kPage, &rows, &err));
        QVERIFY(sql->querySensorRangeDescPaged(monthStart(), monthEnd(), int(kRows / kPage) + 1, kPage, &rows, &err));
        QCOMPARE(rows.size(), 0);
    }

    // Load times, same process and build: best of 5 after warm-up.
    void loadTimes()
    {
        const int pages[] = {1, 1000, 10000, int(kRows / kPage)};
        qInfo("%-44s %8s %10s", "load (Release, best of 5)", "page", "ms");
        for (int page : pages) {
            double bestOld = 1e9, bestNew = 1e9, bestCount = 1e9, bestPage = 1e9;
            for (int rep = 0; rep < 5; ++rep) {
                QElapsedTimer t;
                QJsonArray a;
                t.start();
                QVERIFY(oldLoad(sql, page, &a));
                bestOld = std::min(bestOld, t.nsecsElapsed() / 1.0e6);
                const SensorHistoryPageResult r = requestAndWait(page);
                QVERIFY(r.ok && r.countOk && !r.superseded);
                QCOMPARE(r.totalRows, kRows);
                QCOMPARE(tsAt(r.samples, 0), newestTsOfPage(page));
                bestNew = std::min(bestNew, r.countMs + r.pageMs);
                bestCount = std::min(bestCount, r.countMs);
                bestPage = std::min(bestPage, r.pageMs);
            }
            qInfo("%-44s %8d %10.1f", "OLD countSensorRange + ASC paged (2 COUNT)", page, bestOld);
            qInfo("%-44s %8d %10.1f  (count %.1f + page %.2f)", "NEW 1 COUNT + DESC page (SqlManager thread)",
                  page, bestNew, bestCount, bestPage);
        }
    }

    // Design alternative that needs no new SqlManager function: slice page p out of the
    // existing fetchSensorData(date, limit = p*10) (ORDER BY timestamp DESC LIMIT n, whole
    // month file, every row materialised as 41 QVariants).  Measured to choose the design;
    // the last page (limit = 2,592,000 rows) is not run because of its memory use.
    void alternativeFetchSensorDataSlice()
    {
        qInfo("%-44s %8s %10s %10s", "fetchSensorData slice (alternative)", "page", "rows read", "ms");
        for (int page : {1, 1000, 10000, 25920}) {
            double best = 1e9;
            QVector<QVariantList> rows;
            for (int rep = 0; rep < 3; ++rep) {
                QElapsedTimer t;
                t.start();
                rows = sql->fetchSensorData(QDate(2026, 9, 15), page * kPage);
                best = std::min(best, t.nsecsElapsed() / 1.0e6);
            }
            QCOMPARE(rows.size(), page * kPage);
            QCOMPARE(rows.at((page - 1) * kPage).at(0).toLongLong(), newestTsOfPage(page));
            qInfo("%-44s %8d %10d %10.1f", "fetchSensorData(limit = page*10) + slice", page,
                  page * kPage, best);
        }
    }

    // Main-thread stall while one load runs: old (blocking, as Core did every second)
    // vs new (async request).
    void mainThreadStall()
    {
        TickProbe probe;
        probe.start();                          // idle reference: timer jitter only
        QTest::qWait(300);
        probe.stop();
        const double idleGap = probe.maxGapMs();
        qInfo("main-thread max tick gap with no load (timer jitter reference): %.1f ms", idleGap);

        probe.start();
        QTest::qWait(20);
        QJsonArray a;
        QTimer::singleShot(0, this, [&] { QVERIFY(oldLoad(sql, 1, &a)); });
        QTest::qWait(400);
        probe.stop();
        const double oldGap = probe.maxGapMs();

        double newGap = 0, postUs = 0;
        const SensorHistoryPageResult r = requestAndWait(1, &newGap, &postUs);
        QVERIFY(r.ok);
        qInfo("main-thread max tick gap: OLD blocking load %.1f ms | NEW async load %.1f ms "
              "(request posted in %.0f us; SqlManager thread count %.1f ms + page %.2f ms)",
              oldGap, newGap, postUs, r.countMs, r.pageMs);
        QVERIFY2(newGap < 25.0, "main thread stalled during the async load");
        QVERIFY(oldGap > r.countMs);   // the old path stalled at least one COUNT

        // Residual (not changed by w2-039): any *blocking* SqlManager call made from the
        // main thread while a load runs (e.g. Manager's 1 s saveSensorData) waits in the
        // SqlManager queue until the load is done.  Measured with readFrequency().
        Collector spy(sql);
        sql->requestSensorHistoryPage(++g_nextId, monthStart(), monthEnd(), 1, kPage);
        QElapsedTimer t;
        t.start();
        QVERIFY(sql->readFrequency() > 0);
        const double waitedMs = t.nsecsElapsed() / 1.0e6;
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() == 1, 10000);
        const SensorHistoryPageResult r2 = spy.results.at(0);
        qInfo("residual: a blocking SqlManager call issued right after the request waited %.1f ms "
              "(load count %.1f + page %.2f ms)", waitedMs, r2.countMs, r2.pageMs);
    }

    // 5 rapid page changes: only the newest is executed and applied (Core rule: apply only
    // requestId == newest id).
    void staleRequestsDropped()
    {
        Collector spy(sql);
        const quint64 first = g_nextId + 1;
        for (int page = 1; page <= 5; ++page)
            sql->requestSensorHistoryPage(++g_nextId, monthStart(), monthEnd(), page * 100, kPage);
        const quint64 newest = g_nextId;
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() == 5, 10000);
        int superseded = 0, applied = 0, dropped = 0;
        for (const SensorHistoryPageResult &r : std::as_const(spy.results)) {
            QVERIFY(r.requestId >= first && r.requestId <= newest);
            if (r.superseded)
                ++superseded;
            if (r.requestId != newest) {
                ++dropped;
                continue;
            }
            ++applied;
            QVERIFY(r.ok && !r.superseded);
            QCOMPARE(r.page, 500);
            QCOMPARE(tsAt(r.samples, 0), newestTsOfPage(500));
        }
        qInfo("5 rapid requests: applied %d (newest, page 500), dropped %d, of which not executed "
              "by SqlManager (superseded) %d", applied, dropped, superseded);
        QCOMPARE(applied, 1);
        QCOMPARE(dropped, 4);
        QVERIFY(superseded >= 3);
    }

    // The receiver is destroyed while its request runs: no callback afterwards.
    void receiverDestroyedWhileLoading()
    {
        int delivered = 0;
        auto *receiver = new QObject;
        connect(sql, &SqlManager::sensorHistoryPageReady, receiver,
                [&delivered](const SensorHistoryPageResult &) { ++delivered; }, Qt::QueuedConnection);
        Collector spy(sql);
        sql->requestSensorHistoryPage(++g_nextId, monthStart(), monthEnd(), 1, kPage);
        delete receiver;                        // load is still running on the SqlManager thread
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() == 1, 10000);
        QTest::qWait(50);
        QCOMPARE(delivered, 0);
    }
};

QTEST_GUILESS_MAIN(TstW2039History)
#include "tst_w2039_history.moc"
