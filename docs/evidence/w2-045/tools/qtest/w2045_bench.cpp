// w2-045 D3 measurement program (not a test): the real SqlManager on the 12-month bench
// (build/w2-045-bench/data, made by ../make_bench_db.py), with the main-thread load of the
// application: a 1 ms tick timer (event-loop stall = tick gap) and a BLOCKING
// SqlManager::saveSensorData(now, 40 values) every --save-ms (Manager does this every second).
//
//   w2045_bench <scenario> [--cold] [--save-ms 1000] [--idle-s 12]
//
// Just before each save the main thread also makes a blocking no-op call into the SqlManager
// thread: its duration is the pure queue wait (how long a History step held the thread); the
// save's own duration (SQLite commit + fsync) is in the save time, not in the wait.
//
// scenarios
//   idle      no History request, only the load (baseline for the save wait)
//   long      12-month range (2025-10-01 .. end of today), page 1: first (uncached) load,
//             then the same request again (cached)
//   unbounded 0 .. 8640000000000000 ms range (older web clients), page 1
//   deep      12-month range: jump to the middle page (as if historyCurrentPage were set),
//             then 20 x next page
//   last      12-month range: page 1, then the last page set directly
// --cold      before SqlManager opens any file, every month file is opened once with
//             FILE_FLAG_NO_BUFFERING and closed, which makes Windows drop the file's cached
//             pages (checked: random 4 KiB reads 3 us -> 60 us); the process is new, so the
//             count cache is empty too ("restart + cold file cache").
//
// After the measured part the load is stopped and every page of the deep/last scenarios is
// compared with an independent OFFSET reference (per month COUNT + ORDER BY timestamp DESC,
// rowid DESC LIMIT/OFFSET, the pre-w2-045 algorithm, on the snapshot the result reports).
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <functional>

#include "SqlManager.h"

#define NOMINMAX
#include <windows.h>

namespace {
const QString kData = QStringLiteral(W2045_BENCH_DATA_DIR);
const QString kWork = QStringLiteral(W2045_BENCH_WORK_DIR);

void out(const QString &line)
{
    std::fputs(qPrintable(line + QLatin1Char('\n')), stdout);
    std::fflush(stdout);
}

QStringList monthFiles()
{
    return QDir(kData).entryList(QStringList{QStringLiteral("sensor_*.sqlite")}, QDir::Files, QDir::Name);
}

int purgeFileCache()
{
    int n = 0;
    for (const QString &name : monthFiles()) {
        const std::wstring path = QDir::toNativeSeparators(kData + QLatin1Char('/') + name).toStdWString();
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            ++n;
        }
    }
    return n;
}

struct Stat
{
    double max = 0, p50 = 0, p90 = 0;
    int n = 0, over10 = 0, over20 = 0, over40 = 0;
};
Stat stat(QList<double> v)
{
    Stat s;
    s.n = v.size();
    if (v.isEmpty())
        return s;
    std::sort(v.begin(), v.end());
    s.max = v.last();
    s.p50 = v.at(v.size() / 2);
    s.p90 = v.at(std::min<qsizetype>(v.size() - 1, qsizetype(v.size() * 0.9)));
    for (double x : v) {
        s.over10 += x > 10.0;
        s.over20 += x > 20.0;
        s.over40 += x > 40.0;
    }
    return s;
}

// W2045_OLD: the same program built against the pre-w2-045 SqlManager (HEAD 8e87b70, copied to
// build/w2-045-old/Core by the PM-rerunnable command in the report), for the before/after
// comparison.  There the whole request is one uninterrupted lambda = one step.
#ifdef W2045_OLD
const char *const kImpl = "old";
QList<double> stepsOf(const SensorHistoryPageResult &r) { return {r.countMs + r.pageMs}; }
int deltasOf(const SensorHistoryPageResult &) { return 0; }
QString methodOf(const SensorHistoryPageResult &) { return QStringLiteral("old-offset"); }
QHash<QString, qint64> snapshotOf(const SensorHistoryPageResult &) { return {}; }
#else
const char *const kImpl = "new";
QList<double> stepsOf(const SensorHistoryPageResult &r) { return QList<double>(r.stepMs.cbegin(), r.stepMs.cend()); }
int deltasOf(const SensorHistoryPageResult &r) { return r.countCacheDeltas; }
QString methodOf(const SensorHistoryPageResult &r) { return r.pageMethod; }
QHash<QString, qint64> snapshotOf(const SensorHistoryPageResult &r) { return r.snapshotRowids; }
#endif

struct Interval
{
    qint64 startNs;
    qint64 endNs;
    double ms() const { return (endNs - startNs) / 1.0e6; }
};
} // namespace

class Bench : public QObject
{
public:
    Bench(const QString &scenario, bool cold, int saveMs, int idleS)
        : m_scenario(scenario), m_cold(cold), m_saveMs(saveMs), m_idleS(idleS) {}

    void start()
    {
        m_clock.start();
        m_sql = SqlManager::instance();
        m_sql->setDataDirectory(kData);
        m_sql->setSettingsFile(kWork + QStringLiteral("/settings.sqlite"));
        m_sql->initialize();
        QObject::connect(m_sql, &SqlManager::sensorHistoryPageReady, this,
                         [this](const SensorHistoryPageResult &r) { onResult(r); }, Qt::QueuedConnection);

        m_tick.setTimerType(Qt::PreciseTimer);
        m_tick.setInterval(1);
        QObject::connect(&m_tick, &QTimer::timeout, this, [this]() {
            const qint64 now = m_clock.nsecsElapsed();
            if (m_lastTick >= 0 && now - m_lastTick > 2000000)
                m_gaps.append({m_lastTick, now});
            m_lastTick = now;
        });
        m_save.setTimerType(Qt::PreciseTimer);
        m_save.setInterval(m_saveMs);
        QObject::connect(&m_save, &QTimer::timeout, this, [this]() {
            const qint64 w0 = m_clock.nsecsElapsed();
            QMetaObject::invokeMethod(m_sql, []() {}, Qt::BlockingQueuedConnection);   // queue wait only
            const qint64 t0 = m_clock.nsecsElapsed();
            m_waits.append({w0, t0});
            m_sql->saveSensorData(QDateTime::currentDateTime(), m_readings);   // blocking, like Manager
            m_saves.append({t0, m_clock.nsecsElapsed()});
        });
        for (int i = 0; i < 40; ++i)
            m_readings.append(1000.0 + i);

        m_lastTick = -1;
        m_tick.start();
        m_save.start();
        // 2 s of load first (and the idle baseline for the whole run).
        QTimer::singleShot(2000, this, [this]() { runScenario(); });
    }

private:
    QString m_scenario;
    bool m_cold = false;
    int m_saveMs = 1000;
    int m_idleS = 12;
    SqlManager *m_sql = nullptr;
    QElapsedTimer m_clock;
    QTimer m_tick, m_save;
    qint64 m_lastTick = -1;
    QList<Interval> m_gaps, m_saves, m_waits;
    QVector<double> m_readings;
    quint64 m_nextId = 1;
    quint64 m_pendingId = 0;
    qint64 m_postNs = 0;
    std::function<void(const SensorHistoryPageResult &, Interval)> m_pendingCb;
    qint64 m_measureStart = 0, m_measureEnd = 0;
    QList<double> m_allSteps;
    double m_maxRequestMs = 0;
    struct Check { qint64 from, to; int page; SensorHistoryPageResult r; QString name; };
    QList<Check> m_checks;

    qint64 rangeFrom() const { return QDateTime(QDate(2025, 10, 1), QTime(0, 0)).toSecsSinceEpoch(); }
    qint64 rangeTo() const { return QDateTime(QDate::currentDate().addDays(1), QTime(0, 0)).toSecsSinceEpoch() - 1; }

    void request(qint64 from, qint64 to, int page, std::function<void(const SensorHistoryPageResult &, Interval)> cb)
    {
        m_pendingId = m_nextId++;
        m_pendingCb = std::move(cb);
        m_postNs = m_clock.nsecsElapsed();
        m_sql->requestSensorHistoryRangePage(m_pendingId, from, to, page, 10);
    }

    void onResult(const SensorHistoryPageResult &r)
    {
        if (r.requestId != m_pendingId || !m_pendingCb)
            return;
        const Interval w{m_postNs, m_clock.nsecsElapsed()};
        auto cb = std::move(m_pendingCb);
        m_pendingCb = nullptr;
        cb(r, w);
    }

    // Longest blocking save / tick gap that overlaps [a, b].
    double maxOverlap(const QList<Interval> &list, qint64 a, qint64 b, int *count = nullptr) const
    {
        double m = 0;
        int n = 0;
        for (const Interval &i : list) {
            if (i.startNs < b && i.endNs > a) {
                m = std::max(m, i.ms());
                ++n;
            }
        }
        if (count)
            *count = n;
        return m;
    }

    void report(const QString &name, const SensorHistoryPageResult &r, const Interval &w)
    {
        const QList<double> steps = stepsOf(r);
        const Stat s = stat(steps);
        m_allSteps.append(steps);
        int saves = 0;
        const double saveMax = maxOverlap(m_saves, w.startNs, w.endNs, &saves);
        const double gapMax = maxOverlap(m_gaps, w.startNs, w.endNs);
        m_maxRequestMs = std::max(m_maxRequestMs, w.ms());
        out(QStringLiteral("REQ %1 %2 impl=%23 page=%3 ok=%4 total_rows=%5 months=%6 hits=%7 deltas=%8 method=%9 "
                           "request_ms=%10 steps=%11 step_max_ms=%12 step_p50_ms=%13 step_p90_ms=%14 steps_over_10ms=%15 "
                           "steps_over_20ms=%16 steps_over_40ms=%17 count_work_ms=%18 page_work_ms=%19 "
                           "saves_during=%20 save_max_ms_during=%21 tick_gap_max_ms_during=%22 wait_max_ms_during=%24")
                    .arg(m_scenario, name).arg(r.page).arg(r.ok && r.countOk).arg(r.totalRows).arg(r.months)
                    .arg(r.countCacheHits).arg(deltasOf(r)).arg(methodOf(r))
                    .arg(w.ms(), 0, 'f', 2).arg(s.n).arg(s.max, 0, 'f', 2).arg(s.p50, 0, 'f', 2)
                    .arg(s.p90, 0, 'f', 2).arg(s.over10).arg(s.over20).arg(s.over40)
                    .arg(r.countMs, 0, 'f', 2).arg(r.pageMs, 0, 'f', 2)
                    .arg(saves).arg(saveMax, 0, 'f', 2).arg(gapMax, 0, 'f', 2).arg(QLatin1String(kImpl))
                    .arg(maxOverlap(m_waits, w.startNs, w.endNs), 0, 'f', 2));
    }

    void runScenario()
    {
        m_measureStart = m_clock.nsecsElapsed();
        const qint64 from = rangeFrom();
        const qint64 to = rangeTo();
        if (m_scenario == QStringLiteral("idle")) {
            QTimer::singleShot(m_idleS * 1000, this, [this]() { finish(); });
        } else if (m_scenario == QStringLiteral("long")) {
            request(from, to, 1, [=](const SensorHistoryPageResult &r, Interval w) {
                report(QStringLiteral("first-load"), r, w);
                m_checks.append({from, to, 1, r, QStringLiteral("first-load")});
                QTimer::singleShot(300, this, [=]() {
                    request(from, to, 1, [=](const SensorHistoryPageResult &r2, Interval w2) {
                        report(QStringLiteral("again-cached"), r2, w2);
                        m_checks.append({from, to, 1, r2, QStringLiteral("again-cached")});
                        QTimer::singleShot(1500, this, [this]() { finish(); });
                    });
                });
            });
        } else if (m_scenario == QStringLiteral("unbounded")) {
            request(1, 8640000000000, 1, [=](const SensorHistoryPageResult &r, Interval w) {
                report(QStringLiteral("first-load"), r, w);
                m_checks.append({1, 8640000000000, 1, r, QStringLiteral("unbounded")});
                QTimer::singleShot(1500, this, [this]() { finish(); });
            });
        } else if (m_scenario == QStringLiteral("deep")) {
            // The page holding 2026-03-16 12:00 (the middle of the March file, about 1.34 M
            // rows from either end of it: the worst case for an OFFSET).  One row per second,
            // so the page number is the number of seconds from there to the newest row / 10.
            const qint64 midMarch = QDateTime(QDate(2026, 3, 16), QTime(12, 0)).toSecsSinceEpoch();
            const qint64 newest = QDateTime(QDate(2026, 9, 25), QTime(0, 0)).toSecsSinceEpoch() - 1;
            const int mid = int((newest - midMarch) / 10) + 1;
            request(from, to, mid, [=](const SensorHistoryPageResult &r, Interval w) {
                report(QStringLiteral("jump-middle"), r, w);
                m_checks.append({from, to, mid, r, QStringLiteral("jump-middle")});
                nextPages(from, to, mid + 1, 20);
            });
        } else if (m_scenario == QStringLiteral("last")) {
            request(from, to, 1, [=](const SensorHistoryPageResult &r, Interval w) {
                report(QStringLiteral("page-1"), r, w);
                const int last = int((r.totalRows + 9) / 10);
                QTimer::singleShot(300, this, [=]() {
                    request(from, to, last, [=](const SensorHistoryPageResult &r2, Interval w2) {
                        report(QStringLiteral("last-page"), r2, w2);
                        m_checks.append({from, to, last, r2, QStringLiteral("last-page")});
                        QTimer::singleShot(1500, this, [this]() { finish(); });
                    });
                });
            });
        } else {
            out(QStringLiteral("unknown scenario %1").arg(m_scenario));
            QCoreApplication::exit(2);
        }
    }

    void nextPages(qint64 from, qint64 to, int page, int left)
    {
        if (left == 0) {
            QTimer::singleShot(1500, this, [this]() { finish(); });
            return;
        }
        QTimer::singleShot(50, this, [=]() {
            request(from, to, page, [=](const SensorHistoryPageResult &r, Interval w) {
                report(QStringLiteral("next-%1").arg(21 - left), r, w);
                m_checks.append({from, to, page, r, QStringLiteral("next-%1").arg(21 - left)});
                nextPages(from, to, page + 1, left - 1);
            });
        });
    }

    // Pre-w2-045 algorithm on the reported snapshot: per month COUNT, then
    // ORDER BY timestamp DESC, rowid DESC LIMIT 10 OFFSET <local> (+ continuation).
    QJsonArray reference(qint64 from, qint64 to, int page, const QHash<QString, qint64> &snap)
    {
        QJsonArray rows;
        qint64 skip = qint64(page - 1) * 10;
        int take = 10;
        QStringList names = monthFiles();
        std::reverse(names.begin(), names.end());          // newest first
        for (const QString &name : names) {
            if (take <= 0)
                break;
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
            const QString cond = QStringLiteral("timestamp >= %1 AND timestamp <= %2 AND rowid <= %3")
                                         .arg(lo).arg(hi).arg(snap.value(key, std::numeric_limits<qint64>::max()));
            qint64 count = 0;
            const QString cacheKey = key + QLatin1Char('@') + cond;
            if (m_refCounts.contains(cacheKey)) {
                count = m_refCounts.value(cacheKey);
            } else {
                QSqlQuery q(db);
                q.exec(QStringLiteral("SELECT COUNT(1) FROM sensor_data WHERE ") + cond);
                q.next();
                count = q.value(0).toLongLong();
                m_refCounts.insert(cacheKey, count);
            }
            if (skip >= count) {
                skip -= count;
                continue;
            }
            QStringList cols{QStringLiteral("timestamp")};
            for (int i = 1; i <= 40; ++i)
                cols << QStringLiteral("s%1").arg(i);
            QSqlQuery q(db);
            q.exec(QStringLiteral("SELECT %1 FROM sensor_data WHERE %2 ORDER BY timestamp DESC, rowid DESC LIMIT %3 OFFSET %4")
                           .arg(cols.join(QStringLiteral(", ")), cond).arg(take).arg(skip));
            while (q.next()) {
                QJsonObject obj;
                obj.insert(QStringLiteral("ts"), q.value(0).toLongLong());
                for (int i = 0; i < 40; ++i)
                    obj.insert(QStringLiteral("s%1").arg(i + 1), QJsonValue::fromVariant(q.value(i + 1)));
                rows.append(obj);
                --take;
            }
            skip = 0;
        }
        return rows;
    }
    QHash<QString, qint64> m_refCounts;

    void finish()
    {
        m_measureEnd = m_clock.nsecsElapsed();
        m_tick.stop();
        m_save.stop();
        int saves = 0;
        const double saveMax = maxOverlap(m_saves, m_measureStart, m_measureEnd, &saves);
        double saveSum = 0;
        for (const Interval &i : std::as_const(m_saves))
            if (i.startNs >= m_measureStart)
                saveSum += i.ms();
        const Stat s = stat(m_allSteps);
        out(QStringLiteral("SCENARIO %1 impl=%16 cold=%2 save_every_ms=%3 measured_s=%4 saves=%5 save_max_ms=%6 save_avg_ms=%7 "
                           "tick_gap_max_ms=%8 steps=%9 step_max_ms=%10 step_p50_ms=%11 step_p90_ms=%12 steps_over_20ms=%13 "
                           "steps_over_40ms=%14 request_max_ms=%15 wait_max_ms=%17")
                    .arg(m_scenario).arg(m_cold).arg(m_saveMs)
                    .arg((m_measureEnd - m_measureStart) / 1.0e9, 0, 'f', 1).arg(saves).arg(saveMax, 0, 'f', 2)
                    .arg(saves ? saveSum / saves : 0.0, 0, 'f', 2)
                    .arg(maxOverlap(m_gaps, m_measureStart, m_measureEnd), 0, 'f', 2)
                    .arg(s.n).arg(s.max, 0, 'f', 2).arg(s.p50, 0, 'f', 2).arg(s.p90, 0, 'f', 2)
                    .arg(s.over20).arg(s.over40).arg(m_maxRequestMs, 0, 'f', 2).arg(QLatin1String(kImpl))
                    .arg(maxOverlap(m_waits, m_measureStart, m_measureEnd), 0, 'f', 2));
        int bad = 0;
#ifdef W2045_OLD
        m_checks.clear();   // no snapshot in the old result (saves continue during the checks)
#endif
        for (const Check &c : std::as_const(m_checks)) {
            const QJsonArray ref = reference(c.from, c.to, c.page, snapshotOf(c.r));
            const bool same = ref == c.r.samples && !ref.isEmpty();
            bad += !same;
            out(QStringLiteral("CHECK %1 %2 page=%3 rows=%4 same_as_offset_reference=%5")
                        .arg(m_scenario, c.name).arg(c.page).arg(c.r.samples.size()).arg(same));
        }
        for (const QString &name : QSqlDatabase::connectionNames())
            if (name.startsWith(QStringLiteral("ref_")))
                QSqlDatabase::removeDatabase(name);
        out(QStringLiteral("CHECKS %1 total=%2 mismatches=%3").arg(m_scenario).arg(m_checks.size()).arg(bad));
        QCoreApplication::exit(bad ? 1 : 0);
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 2) {
        out(QStringLiteral("usage: w2045_bench <idle|long|unbounded|deep|last> [--cold] [--save-ms N] [--idle-s N]"));
        return 2;
    }
    const QString scenario = args.at(1);
    const bool cold = args.contains(QStringLiteral("--cold"));
    const int saveIdx = args.indexOf(QStringLiteral("--save-ms"));
    const int saveMs = saveIdx > 0 ? args.value(saveIdx + 1).toInt() : 1000;
    const int idleIdx = args.indexOf(QStringLiteral("--idle-s"));
    const int idleS = idleIdx > 0 ? args.value(idleIdx + 1).toInt() : 12;
    if (!QFileInfo::exists(kData + QStringLiteral("/sensor_202608.sqlite"))) {
        out(QStringLiteral("bench data missing: run docs/evidence/w2-045/tools/make_bench_db.py"));
        return 2;
    }
    QDir().mkpath(kWork);
    if (cold)
        out(QStringLiteral("PURGE file cache of %1 month files (FILE_FLAG_NO_BUFFERING open)").arg(purgeFileCache()));
    Bench bench(scenario, cold, saveMs, idleS);
    QTimer::singleShot(0, &bench, [&bench]() { bench.start(); });
    return app.exec();
}
