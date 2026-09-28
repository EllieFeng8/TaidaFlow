// w2-052 QTest: one History view per client (docs/taidaflow_history_export_spec.md §2.1).
// Real code under test: Core/HistoryViews.cpp (HistoryViewService, what Core::init creates),
// Core/SqlManager.cpp (per-session requestSensorHistoryRangePage / releaseHistorySession),
// Core/HistoryExport.cpp (session id and range rules shared with the export) and the real
// Core/TaidaFlowProxy.h contract (historyViewRequested -> historyViews).  Requests are sent
// the way the app receives them: by emitting TaidaFlowProxy::historyViewRequested.
// Data: a fresh copy of build/w2-041-bench/data (30 days over sensor_202608/202609,
// 2,592,003 rows, + 100 July rows) in <build>/work/data.  No writes during the tests.
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

#include <cmath>
#include <limits>

#include "HistoryExport.h"
#include "HistoryViews.h"
#include "SqlManager.h"
#include "TaidaFlowProxy.h"

namespace {
const QString kSource = QStringLiteral(W2052_SOURCE_DATA_DIR);
const QString kWork = QStringLiteral(W2052_WORK_DIR);
const QString kData = kWork + QStringLiteral("/data");

double localMs(int y, int m, int d, int hh = 0, int mm = 0, int ss = 0)
{
    return double(QDateTime(QDate(y, m, d), QTime(hh, mm, ss)).toMSecsSinceEpoch());
}
// The ranges the UI sends: first ms .. last ms, both inclusive.
const double k30From = localMs(2026, 8, 17);
const double k30To = localMs(2026, 9, 16) - 1;
const double kWeekFrom = localMs(2026, 9, 9);                  // "顯示前一周" as seen on 2026-09-15
const double kWeekTo = localMs(2026, 9, 16) - 1;
const double kAllFrom = 0;                                     // unbounded
const double kAllTo = 8640000000000000.0;
const double kJulyFrom = localMs(2026, 7, 1);
const double kJulyTo = localMs(2026, 8, 1) - 1;
const QString kA = QStringLiteral("web-3f9a");
const QString kB = QStringLiteral("web-b71c");
const QString kD = QStringLiteral("desktop");

struct Req
{
    QString sid;
    double from;
    double to;
    int page;
};

QString describe(const QVariantMap &e)
{
    return QStringLiteral("from %1 to %2 page %3/%4 rows %5 records %6 rev %7")
            .arg(e.value(QStringLiteral("fromMs")).toDouble(), 0, 'f', 0)
            .arg(e.value(QStringLiteral("toMs")).toDouble(), 0, 'f', 0)
            .arg(e.value(QStringLiteral("page")).toInt())
            .arg(e.value(QStringLiteral("totalPages")).toInt())
            .arg(e.value(QStringLiteral("totalRows")).toLongLong())
            .arg(e.value(QStringLiteral("records")).toList().size())
            .arg(e.value(QStringLiteral("revision")).toLongLong());
}

QVariantMap withoutRevision(QVariantMap e)
{
    e.remove(QStringLiteral("revision"));
    return e;
}
} // namespace

// Records every historyViews write (the whole map each time).
class ViewsRecorder : public QObject
{
    Q_OBJECT
public:
    explicit ViewsRecorder(TaidaFlowProxy *proxy)
    {
        connect(proxy, &TaidaFlowProxy::historyViewsChanged, this,
                [this](const QVariantMap &views) { writes.append(views); });
    }
    QList<QVariantMap> writes;
};

class TestW2052Views : public QObject
{
    Q_OBJECT

private:
    SqlManager *m_sql = nullptr;
    int m_supersededResults = 0;         // SqlManager results dropped as stale, per test
    int m_totalResults = 0;
    QHash<QString, int> m_resultsBySession;

    static QVariantMap entry(TaidaFlowProxy &proxy, const QString &sid)
    {
        return proxy.historyViews().value(sid).toMap();
    }
    static bool matches(const QVariantMap &e, double from, double to, int page)
    {
        return !e.isEmpty() && e.value(QStringLiteral("fromMs")).toDouble() == from
                && e.value(QStringLiteral("toMs")).toDouble() == to
                && e.value(QStringLiteral("page")).toInt() == page;
    }

    // Single-session reference: one request at a time on a session of its own, waiting for
    // each result.
    QVariantMap single(TaidaFlowProxy &proxy, const QString &sid, double from, double to, int page, int expectPage = -1)
    {
        const qint64 before = entry(proxy, sid).value(QStringLiteral("revision")).toLongLong();
        emit proxy.historyViewRequested(sid, from, to, page);
        const int want = expectPage > 0 ? expectPage : page;
        if (!QTest::qWaitFor([&]() {
                const QVariantMap e = entry(proxy, sid);
                return matches(e, from, to, want) && e.value(QStringLiteral("revision")).toLongLong() != before;
            }, 300000))
            return {};
        return entry(proxy, sid);
    }

    // Column cells of one record checked against the row in the month file.
    void checkRecordFormat(const QVariantMap &e)
    {
        const QVariantList records = e.value(QStringLiteral("records")).toList();
        const int columns = HistoryExport::historyColumnTitles().size();
        qint64 previous = std::numeric_limits<qint64>::max();
        for (const QVariant &v : records) {
            const QVariantMap r = v.toMap();
            QCOMPARE(r.keys(), (QStringList{QStringLiteral("timestampMs"), QStringLiteral("values")}));
            const qint64 tsMs = r.value(QStringLiteral("timestampMs")).toLongLong();
            QVERIFY(tsMs > 0 && tsMs % 1000 == 0);
            QVERIFY2(tsMs <= previous, "records are not newest first");
            previous = tsMs;
            const QVariantList values = r.value(QStringLiteral("values")).toList();
            QCOMPARE(values.size(), columns);
            QCOMPARE(values.at(0).toString(),
                     QDateTime::fromMSecsSinceEpoch(tsMs).toString(QStringLiteral("yyyy/MM/dd HH:mm:ss")));
        }
    }

private slots:
    void initTestCase()
    {
        QVERIFY2(QFileInfo::exists(kSource + QStringLiteral("/sensor_202608.sqlite")),
                 "run docs/evidence/w2-041/tools/make_bench_db.py first");
        QDir(kWork).removeRecursively();
        QVERIFY(QDir().mkpath(kData));
        for (const QString &f : {QStringLiteral("sensor_202607.sqlite"), QStringLiteral("sensor_202608.sqlite"),
                                 QStringLiteral("sensor_202609.sqlite")})
            QVERIFY(QFile::copy(kSource + QLatin1Char('/') + f, kData + QLatin1Char('/') + f));
        m_sql = SqlManager::instance();
        m_sql->setDataDirectory(kData);
        m_sql->setSettingsFile(kWork + QStringLiteral("/settings.sqlite"));
        QVERIFY(m_sql->initialize());
        connect(m_sql, &SqlManager::sensorHistoryPageReady, this, [this](const SensorHistoryPageResult &r) {
            if (r.superseded)
                ++m_supersededResults;
            ++m_resultsBySession[r.sessionKey];
            ++m_totalResults;
        }, Qt::QueuedConnection);
    }

    void init()
    {
        m_supersededResults = 0;
        m_resultsBySession.clear();
        m_totalResults = 0;
    }

    // D1/D2/D4: three clients (two web tabs + desktop) request different ranges and pages
    // at the same time, round after round.  No request of one client makes another's stale
    // (SqlManager reports no superseded result), every entry shows its own range / page,
    // and every entry equals the entry a single client gets for the same range and page.
    void interleavedSessionsMatchSingleSession()
    {
        TaidaFlowProxy proxy;
        ViewsRecorder rec(&proxy);
        HistoryViewService views(&proxy, m_sql, HistoryViewService::Options{});
        const int cross = int(1296003 / 10) + 1;                  // 30d page over 2026-09-01 00:00
        const int last30 = int((2592003 + 9) / 10);
        const QList<QList<Req>> rounds{
            {{kA, k30From, k30To, 1}, {kB, kWeekFrom, kWeekTo, 1}, {kD, kAllFrom, kAllTo, 1}},
            {{kA, k30From, k30To, 2}, {kB, kWeekFrom, kWeekTo, 2}, {kD, kAllFrom, kAllTo, 2}},
            {{kA, k30From, k30To, cross}, {kB, kWeekFrom, kWeekTo, 50}, {kD, kAllFrom, kAllTo, 3}},
            {{kA, k30From, k30To, last30}, {kB, kWeekFrom, kWeekTo, 49}, {kD, kAllFrom, kAllTo, 1}},
            // uncached ranges (full recounts in many steps) for both web tabs at once
            {{kA, k30From + 5000, k30To - 5000, 1}, {kB, kWeekFrom + 3000, kWeekTo - 86400000, 2},
             {kD, kJulyFrom, kJulyTo, 4}},
            {{kA, k30From + 5000, k30To - 5000, 2}, {kB, kWeekFrom + 3000, kWeekTo - 86400000, 1},
             {kD, kJulyFrom, kJulyTo, 5}},
        };
        QList<QPair<Req, QVariantMap>> got;
        for (int round = 0; round < rounds.size(); ++round) {
            for (const Req &r : rounds.at(round))
                emit proxy.historyViewRequested(r.sid, r.from, r.to, r.page);   // back to back, no waiting
            QVERIFY(QTest::qWaitFor([&]() {
                for (const Req &r : rounds.at(round))
                    if (!matches(entry(proxy, r.sid), r.from, r.to, r.page))
                        return false;
                return true;
            }, 300000));
            for (const Req &r : rounds.at(round)) {
                const QVariantMap e = entry(proxy, r.sid);
                qInfo().noquote() << QStringLiteral("round %1 %2: %3").arg(round + 1).arg(r.sid, describe(e));
                checkRecordFormat(e);
                got.append({r, e});
            }
        }
        QCOMPARE(m_supersededResults, 0);
        QCOMPARE(proxy.historyViews().keys(), (QStringList{kD, kA, kB}));
        // each write is a whole map; revisions only grow
        qint64 lastRevision = 0;
        for (const QVariantMap &w : std::as_const(rec.writes)) {
            qint64 maxRev = 0;
            for (const QVariant &v : w)
                maxRev = std::max(maxRev, v.toMap().value(QStringLiteral("revision")).toLongLong());
            QVERIFY(maxRev > lastRevision);
            lastRevision = maxRev;
        }
        qInfo().noquote() << "writes:" << rec.writes.size() << "for" << got.size() << "requests";

        // Reference: the same range/page as a single client, one after another.
        TaidaFlowProxy refProxy;
        HistoryViewService refViews(&refProxy, m_sql, HistoryViewService::Options{});
        for (const auto &[r, e] : std::as_const(got)) {
            const QVariantMap ref = single(refProxy, QStringLiteral("web-ref"), r.from, r.to, r.page);
            QVERIFY2(!ref.isEmpty(), "reference request did not complete");
            if (withoutRevision(ref) != withoutRevision(e)) {
                qWarning().noquote() << "interleaved" << r.sid << describe(e) << "\nsingle     " << describe(ref);
                QFAIL("entry differs from the single-session entry");
            }
        }
        qInfo().noquote() << got.size() << "interleaved entries equal the single-session entries record by record";
    }

    // D2/D4: the same client asks again before its previous request finished: only the
    // newest request is applied (one write for that client); the other client's request in
    // between is not affected.
    void sameSessionOnlyNewestApplied()
    {
        TaidaFlowProxy proxy;
        ViewsRecorder rec(&proxy);
        HistoryViewService views(&proxy, m_sql, HistoryViewService::Options{});
        const double from = k30From + 7000;                      // uncached: the first request takes many steps
        emit proxy.historyViewRequested(kA, from, k30To, 5);
        emit proxy.historyViewRequested(kB, kWeekFrom, kWeekTo, 7);
        emit proxy.historyViewRequested(kA, from, k30To, 6);
        emit proxy.historyViewRequested(kA, from, k30To, 7);
        QVERIFY(QTest::qWaitFor([&]() {
            return matches(entry(proxy, kA), from, k30To, 7) && matches(entry(proxy, kB), kWeekFrom, kWeekTo, 7);
        }, 300000));
        QTest::qWait(300);                                       // let any late result arrive
        QSet<qint64> revisionsA;
        int writesWithA = 0;
        for (const QVariantMap &w : std::as_const(rec.writes)) {
            if (w.contains(kA)) {
                revisionsA.insert(w.value(kA).toMap().value(QStringLiteral("revision")).toLongLong());
                ++writesWithA;
            }
        }
        qInfo().noquote() << QStringLiteral("writes %1, A revisions %2, A page %3, SqlManager results for A %4 "
                                            "(superseded %5), for B %6")
                                     .arg(rec.writes.size()).arg(revisionsA.size())
                                     .arg(entry(proxy, kA).value(QStringLiteral("page")).toInt())
                                     .arg(m_resultsBySession.value(kA)).arg(m_supersededResults)
                                     .arg(m_resultsBySession.value(kB));
        QCOMPARE(revisionsA.size(), 1);                          // pages 5 and 6 never shown
        QCOMPARE(entry(proxy, kA).value(QStringLiteral("page")).toInt(), 7);
        QCOMPARE(m_resultsBySession.value(kA), 3);               // all three answered (dropped or applied)
        QVERIFY(m_supersededResults >= 1);                       // at least page 5 was dropped in SqlManager
        QCOMPARE(m_resultsBySession.value(kB), 1);
        const QVariantMap ref = single(proxy, QStringLiteral("web-ref2"), from, k30To, 7);
        QCOMPARE(withoutRevision(ref).value(QStringLiteral("records")),
                 withoutRevision(entry(proxy, kA)).value(QStringLiteral("records")));
    }

    // D1/D4: a client's request changes only its own entry's revision; the same request
    // again (same content) writes nothing and keeps the revision.
    void revisionOnlyChangesForOwnEntry()
    {
        TaidaFlowProxy proxy;
        ViewsRecorder rec(&proxy);
        HistoryViewService views(&proxy, m_sql, HistoryViewService::Options{});
        QVERIFY(!single(proxy, kA, k30From, k30To, 3).isEmpty());
        QVERIFY(!single(proxy, kB, kWeekFrom, kWeekTo, 4).isEmpty());
        QVERIFY(!single(proxy, kD, kAllFrom, kAllTo, 1).isEmpty());
        const QVariantMap before = proxy.historyViews();
        const int writesBefore = rec.writes.size();

        QVERIFY(!single(proxy, kA, k30From, k30To, 4).isEmpty());
        QCOMPARE(rec.writes.size(), writesBefore + 1);
        const QVariantMap after = proxy.historyViews();
        QCOMPARE(after.value(kB), before.value(kB));             // content and revision unchanged
        QCOMPARE(after.value(kD), before.value(kD));
        const qint64 revA = after.value(kA).toMap().value(QStringLiteral("revision")).toLongLong();
        QVERIFY(revA > before.value(kA).toMap().value(QStringLiteral("revision")).toLongLong());
        QVERIFY(revA > before.value(kB).toMap().value(QStringLiteral("revision")).toLongLong());
        QVERIFY(revA > before.value(kD).toMap().value(QStringLiteral("revision")).toLongLong());

        // B asks for what it already shows: SqlManager answers, nothing is written.
        const int resultsB = m_resultsBySession.value(kB);
        emit proxy.historyViewRequested(kB, kWeekFrom, kWeekTo, 4);
        QVERIFY(QTest::qWaitFor([&]() { return m_resultsBySession.value(kB) > resultsB; }, 60000));
        QTest::qWait(100);
        QCOMPARE(rec.writes.size(), writesBefore + 1);
        QCOMPARE(proxy.historyViews(), after);
        qInfo().noquote() << "revisions: A" << revA << "B"
                          << after.value(kB).toMap().value(QStringLiteral("revision")).toLongLong() << "D"
                          << after.value(kD).toMap().value(QStringLiteral("revision")).toLongLong()
                          << "; repeated B request wrote nothing";
    }

    // D3/D4: web entries idle for idleTimeoutMs are removed by the sweep timer, the desktop
    // entry never; the SqlManager state of a removed session is released; a removed client
    // is rebuilt by its next request with a new, higher revision.
    void idleWebEntriesRemoved()
    {
        TaidaFlowProxy proxy;
        ViewsRecorder rec(&proxy);
        HistoryViewService::Options options;
        options.idleTimeoutMs = 400;
        options.sweepIntervalMs = 50;
        HistoryViewService views(&proxy, m_sql, options);
        const QString idle = QStringLiteral("web-idle1");
        const QString busy = QStringLiteral("web-busy1");
        QVERIFY(!single(proxy, kD, kWeekFrom, kWeekTo, 1).isEmpty());
        QVERIFY(!single(proxy, idle, kWeekFrom, kWeekTo, 2).isEmpty());
        QVERIFY(!single(proxy, busy, kWeekFrom, kWeekTo, 3).isEmpty());
        const qint64 idleRevision = entry(proxy, idle).value(QStringLiteral("revision")).toLongLong();
        QVERIFY(m_sql->historySessionKeys().contains(idle));
        QElapsedTimer t;
        t.start();
        int page = 3;
        while (t.elapsed() < 900) {                              // busy keeps asking, idle and desktop do not
            emit proxy.historyViewRequested(busy, kWeekFrom, kWeekTo, page = (page == 3 ? 4 : 3));
            QTest::qWait(120);
        }
        QTRY_VERIFY_WITH_TIMEOUT(!proxy.historyViews().contains(idle), 2000);
        QVERIFY(proxy.historyViews().contains(kD));              // desktop is never removed
        QVERIFY(proxy.historyViews().contains(busy));
        QVERIFY(!views.sessionIds().contains(idle));
        QVERIFY(views.sessionIds().contains(kD));
        const QStringList sqlKeys = m_sql->historySessionKeys();
        QVERIFY(!sqlKeys.contains(idle));
        QVERIFY(sqlKeys.contains(busy));
        int removalWrites = 0;
        for (int i = 1; i < rec.writes.size(); ++i)
            removalWrites += rec.writes.at(i - 1).contains(idle) && !rec.writes.at(i).contains(idle);
        QCOMPARE(removalWrites, 1);
        qInfo().noquote() << "idle entry removed after" << t.elapsed() << "ms; SqlManager keys" << sqlKeys;

        // rebuilt by its next request
        const QVariantMap rebuilt = single(proxy, idle, kWeekFrom, kWeekTo, 2);
        QVERIFY(!rebuilt.isEmpty());
        QVERIFY(rebuilt.value(QStringLiteral("revision")).toLongLong() > idleRevision);
        QVERIFY(m_sql->historySessionKeys().contains(idle));
    }

    // D3: several entries idle at the same sweep are removed with ONE historyViews write.
    void idleRemovalWritesMapOnce()
    {
        TaidaFlowProxy proxy;
        ViewsRecorder rec(&proxy);
        HistoryViewService::Options options;
        options.idleTimeoutMs = 200;
        options.sweepIntervalMs = 3600 * 1000;                   // sweep by hand below
        HistoryViewService views(&proxy, m_sql, options);
        for (const QString &sid : {QStringLiteral("web-x1"), QStringLiteral("web-x2"), QStringLiteral("web-x3")})
            QVERIFY(!single(proxy, sid, kJulyFrom, kJulyTo, 1).isEmpty());
        QVERIFY(!single(proxy, kD, kJulyFrom, kJulyTo, 2).isEmpty());
        QTest::qWait(300);
        const int writes = rec.writes.size();
        views.sweepIdle();
        QCOMPARE(rec.writes.size(), writes + 1);
        QCOMPARE(proxy.historyViews().keys(), QStringList{kD});
        views.sweepIdle();                                       // nothing left to remove: no write
        QCOMPARE(rec.writes.size(), writes + 1);
        const QStringList sqlKeys = m_sql->historySessionKeys();
        for (const QString &sid : {QStringLiteral("web-x1"), QStringLiteral("web-x2"), QStringLiteral("web-x3")})
            QVERIFY(!sqlKeys.contains(sid));
    }

    // D3/D4: at most maxSessions entries (desktop included); a new client removes the least
    // recently used web client (never desktop), with one write; removed clients rebuild.
    void entryLimitRemovesLeastRecentlyUsed()
    {
        TaidaFlowProxy proxy;
        ViewsRecorder rec(&proxy);
        HistoryViewService::Options options;
        options.maxSessions = 4;
        HistoryViewService views(&proxy, m_sql, options);
        QVERIFY(!single(proxy, kD, kJulyFrom, kJulyTo, 1).isEmpty());          // oldest, never removed
        QVERIFY(!single(proxy, QStringLiteral("web-1"), kJulyFrom, kJulyTo, 2).isEmpty());
        QVERIFY(!single(proxy, QStringLiteral("web-2"), kJulyFrom, kJulyTo, 3).isEmpty());
        QVERIFY(!single(proxy, QStringLiteral("web-3"), kJulyFrom, kJulyTo, 4).isEmpty());
        QVERIFY(!single(proxy, QStringLiteral("web-1"), kJulyFrom, kJulyTo, 5).isEmpty());   // web-1 used again
        int writes = rec.writes.size();
        emit proxy.historyViewRequested(QStringLiteral("web-4"), kJulyFrom, kJulyTo, 6);
        QCOMPARE(rec.writes.size(), writes + 1);                 // the removal, written once, synchronously
        QCOMPARE(rec.writes.last().keys(), (QStringList{kD, QStringLiteral("web-1"), QStringLiteral("web-3")}));
        QVERIFY(QTest::qWaitFor([&]() { return matches(entry(proxy, QStringLiteral("web-4")), kJulyFrom, kJulyTo, 6); }, 60000));
        QCOMPARE(proxy.historyViews().keys(),
                 (QStringList{kD, QStringLiteral("web-1"), QStringLiteral("web-3"), QStringLiteral("web-4")}));
        QCOMPARE(views.sessionIds().size(), 4);
        QVERIFY(!m_sql->historySessionKeys().contains(QStringLiteral("web-2")));

        // web-2 comes back: rebuilt, and now web-3 is the least recently used web client
        writes = rec.writes.size();
        const QVariantMap back = single(proxy, QStringLiteral("web-2"), kJulyFrom, kJulyTo, 3);
        QVERIFY(!back.isEmpty());
        QCOMPARE(proxy.historyViews().keys(),
                 (QStringList{kD, QStringLiteral("web-1"), QStringLiteral("web-2"), QStringLiteral("web-4")}));
        QCOMPARE(rec.writes.size(), writes + 2);                 // removal of web-3, then web-2's entry
        for (const QVariantMap &w : std::as_const(rec.writes))
            QVERIFY(w.size() <= 4);
        // desktop as the only other entry never goes, even when it is the least recently used
        qInfo().noquote() << "entries after the limit tests:" << proxy.historyViews().keys();
    }

    // D3: the default options (what Core::init uses): 30 min idle, at most 32 entries.
    // desktop + 32 web clients -> 32 entries, the first web client removed, desktop kept.
    void defaultLimitIs32()
    {
        TaidaFlowProxy proxy;
        HistoryViewService views(&proxy, m_sql, HistoryViewService::Options{});
        QCOMPARE(views.options().maxSessions, 32);
        QCOMPARE(views.options().idleTimeoutMs, 30LL * 60 * 1000);
        QVERIFY(!single(proxy, kD, kJulyFrom, kJulyTo, 1).isEmpty());
        for (int i = 1; i <= 32; ++i) {
            const QString sid = QStringLiteral("web-n%1").arg(i, 2, 10, QLatin1Char('0'));
            QVERIFY(!single(proxy, sid, kJulyFrom, kJulyTo, 1 + i % 10).isEmpty());
            QVERIFY(proxy.historyViews().size() <= 32);
        }
        const QStringList keys = proxy.historyViews().keys();
        qInfo().noquote() << keys.size() << "entries; first:" << keys.mid(0, 3) << "last:" << keys.last();
        QCOMPARE(keys.size(), 32);
        QVERIFY(keys.contains(kD));
        QVERIFY(!keys.contains(QStringLiteral("web-n01")));
        QVERIFY(keys.contains(QStringLiteral("web-n02")));
        QVERIFY(keys.contains(QStringLiteral("web-n32")));
    }

    // D1/D4: unbounded range; invalid session ids / ranges / pages are ignored (no write, no
    // SqlManager request); a page past the end is moved to the last page; an empty range is
    // one empty page.
    void unboundedInvalidAndClamp()
    {
        TaidaFlowProxy proxy;
        ViewsRecorder rec(&proxy);
        HistoryViewService views(&proxy, m_sql, HistoryViewService::Options{});

        // unbounded: every row of every month file
        qint64 allRows = 0;
        qint64 newest = 0;
        for (const QString &name : QDir(kData).entryList(QStringList{QStringLiteral("sensor_*.sqlite")}, QDir::Files)) {
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("count"));
                db.setDatabaseName(kData + QLatin1Char('/') + name);
                QVERIFY(db.open());
                QSqlQuery q(db);
                QVERIFY(q.exec(QStringLiteral("SELECT COUNT(1), MAX(timestamp) FROM sensor_data WHERE timestamp >= 1")) && q.next());
                allRows += q.value(0).toLongLong();
                newest = std::max(newest, q.value(1).toLongLong());
            }
            QSqlDatabase::removeDatabase(QStringLiteral("count"));
        }
        const QVariantMap all = single(proxy, kD, kAllFrom, kAllTo, 1);
        QVERIFY(!all.isEmpty());
        qInfo().noquote() << "unbounded:" << describe(all) << "; rows in the files" << allRows;
        QCOMPARE(all.value(QStringLiteral("totalRows")).toLongLong(), allRows);
        QCOMPARE(all.value(QStringLiteral("totalPages")).toInt(), int((allRows + 9) / 10));
        const QVariantList records = all.value(QStringLiteral("records")).toList();
        QCOMPARE(records.size(), 10);
        QCOMPARE(records.first().toMap().value(QStringLiteral("timestampMs")).toLongLong(), newest * 1000);
        checkRecordFormat(all);
        {
            // cells = the converted raw values of that row (TT x100/65535, PT x1000/65535, FM raw, M x100/65535)
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("row"));
            db.setDatabaseName(kData + QStringLiteral("/sensor_202609.sqlite"));
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("SELECT s1, s5, s12, s16 FROM sensor_data WHERE timestamp = %1 "
                                          "ORDER BY rowid DESC LIMIT 1").arg(newest)) && q.next());
            const QVariantList cells = records.first().toMap().value(QStringLiteral("values")).toList();
            QCOMPARE(cells.at(1).toDouble(), q.value(0).toDouble() * (100.0 / 65535.0));
            QCOMPARE(cells.at(5).toDouble(), q.value(1).toDouble() * (1000.0 / 65535.0));
            QCOMPARE(cells.at(12).toDouble(), q.value(2).toDouble());
            QCOMPARE(cells.at(16).toDouble(), q.value(3).toDouble() * (100.0 / 65535.0));
        }
        QSqlDatabase::removeDatabase(QStringLiteral("row"));

        // invalid requests: nothing written, nothing asked from SqlManager
        const int writes = rec.writes.size();
        const int results = m_totalResults;
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        const QList<Req> invalid{
            {QString(), k30From, k30To, 1},
            {QStringLiteral("web 1"), k30From, k30To, 1},
            {QStringLiteral("web/1"), k30From, k30To, 1},
            {QStringLiteral("../desktop"), k30From, k30To, 1},
            {QStringLiteral("web-é"), k30From, k30To, 1},
            {QString(41, QLatin1Char('a')), k30From, k30To, 1},
            {QStringLiteral("web-ok"), nan, k30To, 1},
            {QStringLiteral("web-ok"), k30From, inf, 1},
            {QStringLiteral("web-ok"), k30To, k30From, 1},
            {QStringLiteral("web-ok"), k30From, k30To, 0},
            {QStringLiteral("web-ok"), k30From, k30To, -3},
        };
        for (const Req &r : invalid)
            emit proxy.historyViewRequested(r.sid, r.from, r.to, r.page);
        QTest::qWait(300);
        QCOMPARE(rec.writes.size(), writes);
        QCOMPARE(m_totalResults, results);
        QCOMPARE(views.sessionIds(), QStringList{kD});

        // a 40-character id is valid
        const QString longId(40, QLatin1Char('Z'));
        QVERIFY(!single(proxy, longId, kJulyFrom, kJulyTo, 1).isEmpty());

        // page past the end -> last page (July: 100 rows, 10 pages)
        const QVariantMap clamped = single(proxy, kA, kJulyFrom, kJulyTo, 99, 10);
        QVERIFY(!clamped.isEmpty());
        QCOMPARE(clamped.value(QStringLiteral("totalPages")).toInt(), 10);
        QCOMPARE(clamped.value(QStringLiteral("totalRows")).toLongLong(), 100);
        const QVariantMap page10 = single(proxy, kB, kJulyFrom, kJulyTo, 10);
        QCOMPARE(withoutRevision(clamped), withoutRevision(page10));
        qInfo().noquote() << "page 99 of July ->" << describe(clamped);

        // empty range, page 3 -> page 1 of 1, no rows
        const double emptyFrom = localMs(2001, 1, 1);
        const double emptyTo = localMs(2001, 2, 1) - 1;
        const QVariantMap empty = single(proxy, kA, emptyFrom, emptyTo, 3, 1);
        QVERIFY(!empty.isEmpty());
        QCOMPARE(empty.value(QStringLiteral("totalPages")).toInt(), 1);
        QCOMPARE(empty.value(QStringLiteral("totalRows")).toLongLong(), 0);
        QVERIFY(empty.value(QStringLiteral("records")).toList().isEmpty());
        qInfo().noquote() << "empty range page 3 ->" << describe(empty);
    }

    void cleanupTestCase()
    {
        qInfo().noquote() << "SqlManager session keys at the end:" << m_sql->historySessionKeys();
    }
};

QTEST_MAIN(TestW2052Views)
#include "tst_w2052_views.moc"
