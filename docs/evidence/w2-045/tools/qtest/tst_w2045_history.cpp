// w2-045 QTest: History range request in queued steps + keyset anchor.
// Real code under test: Core/SqlManager.cpp (requestSensorHistoryRangePage).
// Data: a fresh copy of build/w2-041-bench/data (30 days over sensor_202608/202609,
// 2,592,003 rows incl. 3 same-second duplicates, + 100 July rows) in <build>/work/data.
//
// References, both on the test's own connections and restricted per month to the rowid
// snapshot the result reports (rows saved after the snapshot are not part of the page):
//  * reference(): "the current OFFSET version" = the pre-w2-045 algorithm (per month COUNT,
//    then ORDER BY timestamp DESC, rowid DESC LIMIT n OFFSET <local>); every page is checked.
//  * referenceUnion(): ATTACH every month file and run ONE query, UNION ALL of all months,
//    ORDER BY month DESC, timestamp DESC, rowid DESC LIMIT n OFFSET (page-1)*n (sorts the
//    whole range, several seconds): month-boundary and duplicate-second pages.
#include <QtTest>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>

#include <algorithm>

#include "SqlManager.h"

namespace {
const QString kSource = QStringLiteral(W2045_SOURCE_DATA_DIR);
const QString kWork = QStringLiteral(W2045_WORK_DIR);
const QString kData = kWork + QStringLiteral("/data");

qint64 localSecs(int y, int m, int d, int hh = 0, int mm = 0, int ss = 0)
{
    return QDateTime(QDate(y, m, d), QTime(hh, mm, ss)).toSecsSinceEpoch();
}
const qint64 k30From = localSecs(2026, 8, 17);
const qint64 k30To = localSecs(2026, 9, 16) - 1;
constexpr qint64 k30Rows = 2592003;
constexpr qint64 kSepRows30 = 1296003;             // September rows inside the 30-day range
constexpr qint64 kAllFrom = 1;                     // HistoryExport::rangeMsToSecs(0, 8640000000000000)
constexpr qint64 kAllTo = 8640000000000;

struct Ref
{
    qint64 total = 0;
    QJsonArray samples;
};

QList<qint64> tsList(const QJsonArray &a)
{
    QList<qint64> out;
    for (const QJsonValue &v : a)
        out << qint64(v.toObject().value(QStringLiteral("ts")).toDouble());
    return out;
}
} // namespace

class TestW2045History : public QObject
{
    Q_OBJECT

private:
    SqlManager *m_sql = nullptr;
    quint64 m_nextId = 1;
    int m_results = 0;
    double m_maxStepMs = 0;
    int m_totalSteps = 0;
    QList<double> m_keysetPageMs;          // hot, no concurrent writes
    QMap<QString, int> m_methods;
    QTimer m_writer;                       // blocking saves from this thread while requests run
    int m_writes = 0;
    int m_offsetChecks = 0;
    int m_unionChecks = 0;

    SensorHistoryPageResult waitFor(quint64 id, const std::function<void()> &post)
    {
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
        QTimer::singleShot(300000, &loop, &QEventLoop::quit);
        post();
        if (!got)
            loop.exec();
        disconnect(c);
        return out;
    }

    SensorHistoryPageResult rangePage(qint64 from, qint64 to, int page)
    {
        const quint64 id = m_nextId++;
        SensorHistoryPageResult r = waitFor(id, [&]() { m_sql->requestSensorHistoryRangePage(id, from, to, page, 10); });
        note(r);
        return r;
    }

    void note(const SensorHistoryPageResult &r)
    {
        if (r.superseded)
            return;
        ++m_results;
        m_totalSteps += r.steps;
        m_maxStepMs = std::max(m_maxStepMs, r.maxStepMs);
        m_methods[r.pageMethod] += 1;
    }

    static QStringList monthFiles()
    {
        return QDir(kData).entryList(QStringList{QStringLiteral("sensor_*.sqlite")}, QDir::Files, QDir::Name);
    }

    // Second, fully independent reference (slow: sorts every row of the range): one query over
    // all ATTACHed month files.  Used for the month-boundary and duplicate-second pages.
    Ref referenceUnion(qint64 from, qint64 to, qint64 offset, int limit, const QHash<QString, qint64> &snap)
    {
        Ref ref;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("ref"));
            db.setDatabaseName(QStringLiteral(":memory:"));
            if (!db.open())
                return ref;
            QSqlQuery q(db);
            QStringList parts;
            QStringList cols{QStringLiteral("timestamp")};
            for (int i = 1; i <= 40; ++i)
                cols << QStringLiteral("s%1").arg(i);
            for (const QString &name : monthFiles()) {
                const QString key = name.mid(7, 6);
                const QString alias = QStringLiteral("m") + key;
                if (!q.exec(QStringLiteral("ATTACH DATABASE '%1/%2' AS %3").arg(kData, name, alias)))
                    qWarning() << q.lastError().text();
                QString part = QStringLiteral("SELECT %1 AS mk, rowid AS rid, %2 FROM %3.sensor_data "
                                              "WHERE timestamp >= %4 AND timestamp <= %5")
                                       .arg(key, cols.join(QStringLiteral(", ")), alias).arg(from).arg(to);
                if (snap.contains(key))
                    part += QStringLiteral(" AND rowid <= %1").arg(snap.value(key));
                parts << part;
            }
            const QString u = parts.join(QStringLiteral(" UNION ALL "));
            if (!q.exec(QStringLiteral("SELECT COUNT(1) FROM (%1)").arg(u)) || !q.next())
                qWarning() << q.lastError().text();
            ref.total = q.value(0).toLongLong();
            q.finish();
            if (!q.exec(QStringLiteral("SELECT %1 FROM (%2) ORDER BY mk DESC, timestamp DESC, rid DESC LIMIT %3 OFFSET %4")
                                .arg(cols.join(QStringLiteral(", ")), u).arg(limit).arg(offset)))
                qWarning() << q.lastError().text();
            while (q.next()) {
                QJsonObject obj;
                obj.insert(QStringLiteral("ts"), q.value(0).toLongLong());
                for (int i = 0; i < 40; ++i)
                    obj.insert(QStringLiteral("s%1").arg(i + 1), QJsonValue::fromVariant(q.value(i + 1)));
                ref.samples.append(obj);
            }
            q.finish();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("ref"));
        return ref;
    }

    // Main reference = the pre-w2-045 OFFSET algorithm (w2-041 requestSensorHistoryRangePage):
    // months newest first, per month COUNT(1) over the range, then in the month holding the
    // offset ORDER BY timestamp DESC, rowid DESC LIMIT n OFFSET <local>, continued in the next
    // older month; each month limited to the rowid snapshot the result reports.  Own
    // connections; COUNTs cached per (month, range, snapshot) because they do not change.
    Ref reference(qint64 from, qint64 to, qint64 offset, int limit, const QHash<QString, qint64> &snap)
    {
        Ref ref;
        QStringList names = monthFiles();
        std::reverse(names.begin(), names.end());
        QStringList cols{QStringLiteral("timestamp")};
        for (int i = 1; i <= 40; ++i)
            cols << QStringLiteral("s%1").arg(i);
        qint64 skip = offset;
        int take = limit;
        for (const QString &name : names) {
            const QString key = name.mid(7, 6);
            const QDate monthStart(key.left(4).toInt(), key.mid(4).toInt(), 1);
            const qint64 lo = std::max(from, QDateTime(monthStart, QTime(0, 0)).toSecsSinceEpoch());
            const qint64 hi = std::min(to, QDateTime(monthStart.addMonths(1), QTime(0, 0)).toSecsSinceEpoch() - 1);
            if (lo > hi)
                continue;
            const QString conn = QStringLiteral("ref_") + key;
            QSqlDatabase db = QSqlDatabase::contains(conn) ? QSqlDatabase::database(conn)
                                                           : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(kData + QLatin1Char('/') + name);
            if (!db.isOpen() && !db.open())
                return {};
            QString cond = QStringLiteral("timestamp >= %1 AND timestamp <= %2").arg(lo).arg(hi);
            if (snap.contains(key))
                cond += QStringLiteral(" AND rowid <= %1").arg(snap.value(key));
            const QString cacheKey = key + QLatin1Char('|') + cond;
            qint64 count = 0;
            if (snap.contains(key) && m_refCounts.contains(cacheKey)) {
                count = m_refCounts.value(cacheKey);
            } else {
                QSqlQuery q(db);
                if (!q.exec(QStringLiteral("SELECT COUNT(1) FROM sensor_data WHERE ") + cond) || !q.next())
                    qWarning() << q.lastError().text();
                count = q.value(0).toLongLong();
                if (snap.contains(key))
                    m_refCounts.insert(cacheKey, count);
            }
            ref.total += count;
            if (take <= 0)
                continue;
            if (skip >= count) {
                skip -= count;
                continue;
            }
            QSqlQuery q(db);
            if (!q.exec(QStringLiteral("SELECT %1 FROM sensor_data WHERE %2 ORDER BY timestamp DESC, rowid DESC LIMIT %3 OFFSET %4")
                                .arg(cols.join(QStringLiteral(", ")), cond).arg(take).arg(skip)))
                qWarning() << q.lastError().text();
            while (q.next()) {
                QJsonObject obj;
                obj.insert(QStringLiteral("ts"), q.value(0).toLongLong());
                for (int i = 0; i < 40; ++i)
                    obj.insert(QStringLiteral("s%1").arg(i + 1), QJsonValue::fromVariant(q.value(i + 1)));
                ref.samples.append(obj);
                --take;
            }
            skip = 0;
        }
        return ref;
    }
    QHash<QString, qint64> m_refCounts;

    void closeReferenceConnections()
    {
        for (const QString &name : QSqlDatabase::connectionNames())
            if (name.startsWith(QStringLiteral("ref_")))
                QSqlDatabase::removeDatabase(name);
    }

    // Compares one result with the reference on the snapshot it reports.
    void checkPage(const SensorHistoryPageResult &r, qint64 from, qint64 to, int page, const char *what,
                   bool alsoUnion = false)
    {
        QVERIFY2(r.ok && r.countOk, qPrintable(QStringLiteral("%1 page %2: %3").arg(QLatin1String(what)).arg(page).arg(r.errorMessage)));
        QCOMPARE(r.page, page);
        if (alsoUnion) {
            const Ref u = referenceUnion(from, to, qint64(page - 1) * 10, 10, r.snapshotRowids);
            QCOMPARE(r.totalRows, u.total);
            QVERIFY2(r.samples == u.samples, qPrintable(QStringLiteral("%1 page %2 differs from the UNION ALL reference")
                                                             .arg(QLatin1String(what)).arg(page)));
            ++m_unionChecks;
        }
        ++m_offsetChecks;
        const Ref ref = reference(from, to, qint64(page - 1) * 10, 10, r.snapshotRowids);
        QCOMPARE(r.totalRows, ref.total);
        if (r.samples != ref.samples) {
            qWarning().noquote() << "got" << tsList(r.samples) << "\nref" << tsList(ref.samples);
            QFAIL(qPrintable(QStringLiteral("%1 page %2 (%3) differs from the OFFSET reference")
                                     .arg(QLatin1String(what)).arg(page).arg(r.pageMethod)));
        }
        QVERIFY2(r.maxStepMs < 40.0, qPrintable(QStringLiteral("step %1 ms").arg(r.maxStepMs)));
    }

    void startWriter(int intervalMs, const std::function<QDateTime()> &when)
    {
        m_writer.stop();
        disconnect(&m_writer, nullptr, this, nullptr);
        m_writer.setInterval(intervalMs);
        connect(&m_writer, &QTimer::timeout, this, [this, when]() {
            if (m_sql->saveSensorData(when(), QVector<double>(40, double(m_writes % 997))))
                ++m_writes;
        });
        m_writer.start();
    }

    qint64 maxRowid(const QString &key)
    {
        qint64 v = -1;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("probe"));
            db.setDatabaseName(kData + QStringLiteral("/sensor_%1.sqlite").arg(key));
            if (db.open()) {
                QSqlQuery q(db);
                if (q.exec(QStringLiteral("SELECT MAX(rowid) FROM sensor_data")) && q.next())
                    v = q.value(0).toLongLong();
            }
        }
        QSqlDatabase::removeDatabase(QStringLiteral("probe"));
        return v;
    }

    // Keyset: must use the anchor.  Adjacent: next to the previous page (anchor, or OFFSET
    // at a month file's edge when the page starts in another month), never the seek.
    enum Expect { Any, Adjacent, Keyset };
    struct Step { int page; Expect expect; bool unionCheck = false; };
    void runSequence(qint64 from, qint64 to, const QList<Step> &steps, const char *what, bool keysetTimed)
    {
        for (const Step &s : steps) {
            const SensorHistoryPageResult r = rangePage(from, to, s.page);
            checkPage(r, from, to, s.page, what, s.unionCheck);
            if (QTest::currentTestFailed())
                return;
            qInfo().noquote() << QStringLiteral("%1 page %2: %3 rows %4..%5 method=%6 count %7 ms page %8 ms steps %9 max step %10 ms")
                                         .arg(QLatin1String(what)).arg(s.page).arg(r.samples.size())
                                         .arg(tsList(r.samples).value(0)).arg(tsList(r.samples).value(r.samples.size() - 1))
                                         .arg(r.pageMethod).arg(r.countMs, 0, 'f', 2).arg(r.pageMs, 0, 'f', 2)
                                         .arg(r.steps).arg(r.maxStepMs, 0, 'f', 2);
            if (s.expect == Keyset)
                QCOMPARE(r.pageMethod, QStringLiteral("keyset"));
            if (s.expect == Adjacent)
                QVERIFY2(!r.pageMethod.startsWith(QStringLiteral("seek")), qPrintable(r.pageMethod));
            if (s.expect != Any && keysetTimed)
                m_keysetPageMs << r.pageMs;
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
        qInfo().noquote() << "data (fresh copy of the w2-041 bench):" << kData << monthFiles();
    }

    // D1: adjacent pages by keyset, other pages by OFFSET from the nearer end or by the
    // time-slice seek; every page equal to the OFFSET reference.  30-day range, no writes.
    void pagesMatchOffset30Days()
    {
        const int cross = int(kSepRows30 / 10) + 1;        // 3 September rows + 7 August rows
        const int last = int((k30Rows + 9) / 10);          // 3 rows
        QList<Step> steps;
        steps << Step{1, Any, true};                       // 2026-09-15 23:59:59 twice
        for (int p = 2; p <= 12; ++p)
            steps << Step{p, Keyset};
        for (int p = 11; p >= 9; --p)
            steps << Step{p, Keyset};
        steps << Step{cross - 6, Any};                     // jump
        for (int p = cross - 5; p <= cross + 5; ++p)       // next, over 2026-09-01 00:00 (duplicate at 00:00:05)
            steps << Step{p, Keyset, p >= cross - 1 && p <= cross + 1};
        for (int p = cross + 4; p >= cross - 5; --p)       // previous, back over the boundary; the page that
            steps << Step{p, p == cross ? Adjacent : Keyset};   // starts in September is read from its oldest end
        steps << Step{cross, Adjacent} << Step{cross, Keyset};   // same page again (refresh)
        steps << Step{last, Any};                          // jump to the last page (OFFSET from the oldest end)
        for (int p = last - 1; p >= last - 3; --p)
            steps << Step{p, Keyset};
        steps << Step{last - 2, Keyset} << Step{last - 1, Keyset} << Step{last, Keyset};
        steps << Step{194400, Any};                        // middle of August (seek)
        for (int p = 194401; p <= 194420; ++p)
            steps << Step{p, Keyset};                      // 20 x next, deep
        steps << Step{64800, Any};                         // middle of September
        for (int p = 64801; p <= 64803; ++p)
            steps << Step{p, Keyset};
        for (int p = 64802; p >= 64800; --p)
            steps << Step{p, Keyset};
        // around the 2026-09-08 12:00:00 duplicate (position of 12:00:00 from the newest end)
        const int dupPage = int((k30To - localSecs(2026, 9, 8, 12)) / 10) + 1;
        steps << Step{dupPage - 2, Any};
        for (int p = dupPage - 1; p <= dupPage + 2; ++p)
            steps << Step{p, Keyset, true};
        runSequence(k30From, k30To, steps, "30d", true);
    }

    // D1: the same while rows are saved continuously (blocking saves from this thread every
    // 2 ms, so they land between the request's steps), over the unbounded range that holds
    // the saved rows (current month) and the August/September/July bench rows.
    void pagesMatchOffsetWhileWriting()
    {
        const QString nowKey = QDate::currentDate().toString(QStringLiteral("yyyyMM"));
        startWriter(2, []() { return QDateTime::currentDateTime(); });
        int snapshotsBehind = 0;
        QList<Step> steps;
        steps << Step{1, Any};
        for (int p = 2; p <= 4; ++p)
            steps << Step{p, Keyset};
        for (int p = 3; p >= 1; --p)
            steps << Step{p, Keyset};
        steps << Step{150000, Any};
        for (int p = 150001; p <= 150008; ++p)
            steps << Step{p, Keyset};
        for (int p = 150007; p >= 150003; --p)
            steps << Step{p, Keyset};
        for (const Step &s : steps) {
            const SensorHistoryPageResult r = rangePage(kAllFrom, kAllTo, s.page);
            m_writer.stop();
            const qint64 now = maxRowid(nowKey);
            if (r.snapshotRowids.value(nowKey, now) < now)
                ++snapshotsBehind;                         // rows were saved during the request
            checkPage(r, kAllFrom, kAllTo, s.page, "all+writes");
            if (QTest::currentTestFailed())
                return;
            if (s.expect == Keyset)
                QCOMPARE(r.pageMethod, QStringLiteral("keyset"));
            qInfo().noquote() << QStringLiteral("all+writes page %1: total %2, first ts %3, method %4, deltas %5, steps %6, "
                                                "max step %9 ms, page %10 ms, snapshot %7 vs now %8")
                                         .arg(s.page).arg(r.totalRows).arg(tsList(r.samples).value(0))
                                         .arg(r.pageMethod).arg(r.countCacheDeltas).arg(r.steps)
                                         .arg(r.snapshotRowids.value(nowKey)).arg(now)
                                         .arg(r.maxStepMs, 0, 'f', 2).arg(r.pageMs, 0, 'f', 2);
            m_writer.start();
        }
        m_writer.stop();
        qInfo().noquote() << "saves during the sequence:" << m_writes << "; results whose snapshot was behind the file:"
                          << snapshotsBehind;
        QVERIFY(snapshotsBehind > 0);
    }

    // D1: rows saved into the anchor's own month between two pages, newer and older than
    // the anchor (REST-style samples with old timestamps), and a new month file.
    void anchorAfterInsertsAndNewMonth()
    {
        const int base = 200000;                           // August, deep
        SensorHistoryPageResult r = rangePage(kAllFrom, kAllTo, base);
        checkPage(r, kAllFrom, kAllTo, base, "inserts");
        const qint64 anchorTs = tsList(r.samples).last();
        // 3 rows newer than the anchor, 2 older, all in August
        for (qint64 t : {anchorTs + 1, anchorTs + 3600, anchorTs, anchorTs - 1, anchorTs - 7200})
            QVERIFY(m_sql->saveSensorData(QDateTime::fromSecsSinceEpoch(t), QVector<double>(40, 5.0)));
        for (int p = base + 1; p <= base + 3; ++p) {
            r = rangePage(kAllFrom, kAllTo, p);
            checkPage(r, kAllFrom, kAllTo, p, "inserts");
            QCOMPARE(r.pageMethod, QStringLiteral("keyset"));
            qInfo().noquote() << "after inserts into the anchor month, page" << p << "method" << r.pageMethod
                              << "first ts" << tsList(r.samples).value(0) << "max step ms" << r.maxStepMs;
        }
        // A new, newer month file appears (month rollover): 4 rows on 2026-10-01.
        const qint64 oct = localSecs(2026, 10, 1, 0, 0, 5);
        for (int i = 0; i < 4; ++i)
            QVERIFY(m_sql->saveSensorData(QDateTime::fromSecsSinceEpoch(oct + i), QVector<double>(40, 6.0)));
        QVERIFY(QFileInfo::exists(kData + QStringLiteral("/sensor_202610.sqlite")));
        for (int p = base + 2; p >= base; --p) {
            r = rangePage(kAllFrom, kAllTo, p);
            checkPage(r, kAllFrom, kAllTo, p, "new month");
            QCOMPARE(r.pageMethod, QStringLiteral("keyset"));
        }
        qInfo().noquote() << "after the new month file: months" << r.months << "total" << r.totalRows;
        r = rangePage(kAllFrom, kAllTo, 1);
        checkPage(r, kAllFrom, kAllTo, 1, "new month page 1");
        QCOMPARE(qint64(r.samples.at(0).toObject().value(QStringLiteral("ts")).toDouble()), oct + 3);
    }

    // Count cache: unchanged months are hits, the month with new rows is updated by
    // counting only the new rows (rowid above the cached snapshot).
    void countCacheDelta()
    {
        SensorHistoryPageResult a = rangePage(kAllFrom, kAllTo, 1);
        SensorHistoryPageResult b = rangePage(kAllFrom, kAllTo, 1);
        QCOMPARE(b.countCacheHits, b.months);
        QCOMPARE(b.totalRows, a.totalRows);
        QVERIFY(m_sql->saveSensorData(QDateTime::currentDateTime(), QVector<double>(40, 7.0)));
        QVERIFY(m_sql->saveSensorData(QDateTime::currentDateTime(), QVector<double>(40, 8.0)));
        SensorHistoryPageResult c = rangePage(kAllFrom, kAllTo, 1);
        checkPage(c, kAllFrom, kAllTo, 1, "delta");
        qInfo().noquote() << QStringLiteral("cache: all hits %1/%2 (%3 ms); after 2 saves hits %4, deltas %5, total %6 -> %7 (%8 ms)")
                                     .arg(b.countCacheHits).arg(b.months).arg(b.countMs, 0, 'f', 2)
                                     .arg(c.countCacheHits).arg(c.countCacheDeltas).arg(b.totalRows).arg(c.totalRows)
                                     .arg(c.countMs, 0, 'f', 2);
        QCOMPARE(c.countCacheDeltas, 1);
        QCOMPARE(c.countCacheHits, c.months - 1);
        QCOMPARE(c.totalRows, b.totalRows + 2);
    }

    // D2: an uncached range is counted in time slices over many steps; far pages are found
    // by the slice seek; no step reaches 40 ms.
    void uncachedCountAndSeekInSteps()
    {
        const qint64 from = k30From + 7;
        const qint64 to = k30To - 3;
        SensorHistoryPageResult r = rangePage(from, to, 1);
        checkPage(r, from, to, 1, "uncached");
        qInfo().noquote() << QStringLiteral("uncached 2-month count: %1 steps, count %2 ms, max step %3 ms, total %4 ms; steps(ms): %5")
                                     .arg(r.steps).arg(r.countMs, 0, 'f', 2).arg(r.maxStepMs, 0, 'f', 2)
                                     .arg(r.totalMs, 0, 'f', 2)
                                     .arg([&]() { QStringList l; for (double v : r.stepMs) l << QString::number(v, 'f', 1); return l.join(QLatin1Char(' ')); }());
        QVERIFY(r.steps > 5);
        const qint64 total = r.totalRows;
        for (int p : {int(total / 40), int(total / 20 * 7 / 10), int(total / 10 * 3 / 4), int(total / 10) - 20000}) {
            r = rangePage(from, to, p);
            checkPage(r, from, to, p, "seek");
            qInfo().noquote() << QStringLiteral("page %1: method %2, steps %3, page %4 ms, max step %5 ms")
                                         .arg(p).arg(r.pageMethod).arg(r.steps).arg(r.pageMs, 0, 'f', 2)
                                         .arg(r.maxStepMs, 0, 'f', 2);
            QVERIFY(r.pageMethod.startsWith(QStringLiteral("seek")));
        }
    }

    // D2: a request that becomes stale between its steps drops the remaining steps.
    void supersededBetweenSteps()
    {
        bool droppedMidway = false;
        for (int attempt = 0; attempt < 6 && !droppedMidway; ++attempt) {
            const qint64 from = k30From + 100 + attempt;     // new range: full recount, many steps
            const quint64 idA = m_nextId++;
            const quint64 idB = m_nextId++;
            SensorHistoryPageResult a;
            bool gotA = false;
            auto c = connect(m_sql, &SqlManager::sensorHistoryPageReady, this, [&](const SensorHistoryPageResult &r) {
                if (r.requestId == idA) { a = r; gotA = true; }
            }, Qt::QueuedConnection);
            const SensorHistoryPageResult b = waitFor(idB, [&]() {
                m_sql->requestSensorHistoryRangePage(idA, from, k30To, 1, 10);
                QTest::qWait(8 + 4 * attempt);             // A is running its steps now
                m_sql->requestSensorHistoryRangePage(idB, from, k30To, 2, 10);
            });
            QTRY_VERIFY(gotA);
            disconnect(c);
            note(b);
            checkPage(b, from, k30To, 2, "after superseded");
            qInfo().noquote() << QStringLiteral("attempt %1: A superseded=%2 after %3 step(s) (%4 ms); B ok, %5 steps")
                                         .arg(attempt).arg(a.superseded).arg(a.steps).arg(a.totalMs, 0, 'f', 2).arg(b.steps);
            QVERIFY(a.superseded);
            droppedMidway = a.steps > 0;
        }
        QVERIFY2(droppedMidway, "request A was never dropped between its steps");
    }

    void cleanupTestCase()
    {
        m_writer.stop();
        std::sort(m_keysetPageMs.begin(), m_keysetPageMs.end());
        const double median = m_keysetPageMs.isEmpty() ? 0 : m_keysetPageMs.at(m_keysetPageMs.size() / 2);
        const double maxKey = m_keysetPageMs.isEmpty() ? 0 : m_keysetPageMs.last();
        QStringList methods;
        for (auto it = m_methods.cbegin(); it != m_methods.cend(); ++it)
            methods << QStringLiteral("%1=%2").arg(it.key()).arg(it.value());
        closeReferenceConnections();
        qInfo().noquote() << QStringLiteral("SUMMARY: %1 results, %2 steps, longest step %3 ms; keyset pages (30d, hot, no writes): "
                                            "%4, page time median %5 ms, max %6 ms; methods %7; pages compared with the "
                                            "OFFSET reference %8, also with the UNION ALL reference %9")
                                     .arg(m_results).arg(m_totalSteps).arg(m_maxStepMs, 0, 'f', 2)
                                     .arg(m_keysetPageMs.size()).arg(median, 0, 'f', 3).arg(maxKey, 0, 'f', 3)
                                     .arg(methods.join(QStringLiteral(", "))).arg(m_offsetChecks).arg(m_unionChecks);
        QVERIFY(m_maxStepMs < 40.0);
        QVERIFY(!m_keysetPageMs.isEmpty());
        QVERIFY2(maxKey <= 2.0, qPrintable(QStringLiteral("keyset page %1 ms > 2 ms").arg(maxKey)));
    }
};

QTEST_GUILESS_MAIN(TestW2045History)
#include "tst_w2045_history.moc"
