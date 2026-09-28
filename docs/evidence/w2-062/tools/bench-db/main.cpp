// w2-062: C++ / Qt SQL rewrite of the w2-041 bench data generator (docs/evidence/w2-041/tools/
// make_bench_db.py, which is Python and stays untouched in its evidence folder). The project uses no
// Python any more (Mango, w2-062), so the QTest harnesses of w2-041 (w2-049 version), w2-045, w2-052
// get their data from this program instead.
//
// Same data set as make_bench_db.py: build/w2-041-bench/data
//   sensor_202607.sqlite  2026-07-10 12:00:00 .. +99 s                   100 rows + 2 (ts 0 / -5)
//   sensor_202608.sqlite  2026-08-17 00:00:00 .. 2026-08-31 23:59:59   1,296,000 rows
//   sensor_202609.sqlite  2026-09-01 00:00:00 .. 2026-09-15 23:59:59   1,296,000 rows + 3 duplicates
// schema sensor_data(timestamp INTEGER NOT NULL, s1..s40 REAL) + idx_sensor_data_ts,
// holding_register(timestamp, h1..h100) + index, alarm_history + index; rollback journal (DELETE);
// one row per second, s1..s20 = uniform(0, 65535); every 997th row s3 = NULL, every 1009th s14 = NULL,
// every 101st row s12 = an exact binary tie (12.125, 0.375, 7.625, 3.875, 100.125, 0.125).
// The random numbers use the same algorithm as Python's random module: MT19937 seeded with
// init_by_array({41}) (random.seed(41)) and random() = genrand_res53, uniform(a, b) = a + (b - a) * random(),
// consumed in the same order.
//
// Usage: make_bench_db <data folder> [--rebuild]   (prints the file list and row counts)
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QTime>
#include <QVariant>

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

namespace {

// MT19937 exactly as CPython's _randommodule.c (init_by_array, genrand_uint32, random()).
class PyMt
{
public:
    explicit PyMt(std::uint32_t seed)
    {
        const std::uint32_t key[1] = {seed};
        initByArray(key, 1);
    }
    double random()   // genrand_res53
    {
        const std::uint32_t a = next() >> 5, b = next() >> 6;
        return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
    }
    double uniform(double a, double b) { return a + (b - a) * random(); }

private:
    static constexpr int N = 624, M = 397;
    std::array<std::uint32_t, N> mt{};
    int mti = N + 1;

    void initGenrand(std::uint32_t s)
    {
        mt[0] = s;
        for (mti = 1; mti < N; mti++)
            mt[mti] = 1812433253U * (mt[mti - 1] ^ (mt[mti - 1] >> 30)) + std::uint32_t(mti);
    }
    void initByArray(const std::uint32_t *key, std::size_t keyLength)
    {
        initGenrand(19650218U);
        std::size_t i = 1, j = 0;
        std::size_t k = (N > keyLength ? N : keyLength);
        for (; k; k--) {
            mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1664525U)) + key[j] + std::uint32_t(j);
            i++; j++;
            if (i >= N) { mt[0] = mt[N - 1]; i = 1; }
            if (j >= keyLength) j = 0;
        }
        for (k = N - 1; k; k--) {
            mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1566083941U)) - std::uint32_t(i);
            i++;
            if (i >= N) { mt[0] = mt[N - 1]; i = 1; }
        }
        mt[0] = 0x80000000U;
    }
    std::uint32_t next()
    {
        static const std::uint32_t mag01[2] = {0x0U, 0x9908b0dfU};
        std::uint32_t y;
        if (mti >= N) {
            int kk;
            for (kk = 0; kk < N - M; kk++) {
                y = (mt[kk] & 0x80000000U) | (mt[kk + 1] & 0x7fffffffU);
                mt[kk] = mt[kk + M] ^ (y >> 1) ^ mag01[y & 0x1U];
            }
            for (; kk < N - 1; kk++) {
                y = (mt[kk] & 0x80000000U) | (mt[kk + 1] & 0x7fffffffU);
                mt[kk] = mt[kk + (M - N)] ^ (y >> 1) ^ mag01[y & 0x1U];
            }
            y = (mt[N - 1] & 0x80000000U) | (mt[0] & 0x7fffffffU);
            mt[N - 1] = mt[M - 1] ^ (y >> 1) ^ mag01[y & 0x1U];
            mti = 0;
        }
        y = mt[mti++];
        y ^= (y >> 11);
        y ^= (y << 7) & 0x9d2c5680U;
        y ^= (y << 15) & 0xefc60000U;
        y ^= (y >> 18);
        return y;
    }
};

qint64 ts(int y, int m, int d, int hh = 0, int mm = 0, int ss = 0)
{
    return QDateTime(QDate(y, m, d), QTime(hh, mm, ss), Qt::LocalTime).toSecsSinceEpoch();   // local time, like Core
}

struct Span { qint64 start; qint64 count; };
struct FileSpec { QString key; std::vector<Span> spans; };
using Row = std::array<std::optional<double>, 20>;

bool exec(QSqlQuery &q, const QString &sql)
{
    if (!q.exec(sql)) {
        std::fprintf(stderr, "SQL failed: %s: %s\n", qPrintable(sql), qPrintable(q.lastError().text()));
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 2) {
        std::fprintf(stderr, "usage: make_bench_db <data folder> [--rebuild]\n");
        return 2;
    }
    const QString data = QDir(args.at(1)).absolutePath();
    const bool rebuild = args.contains(QStringLiteral("--rebuild"));
    QDir().mkpath(data);

    const std::vector<FileSpec> files = {
        {QStringLiteral("202607"), {{ts(2026, 7, 10, 12), 100}}},
        {QStringLiteral("202608"), {{ts(2026, 8, 17), 15 * 86400}}},
        {QStringLiteral("202609"), {{ts(2026, 9, 1), 15 * 86400}}},
    };
    const double ties[] = {12.125, 0.375, 7.625, 3.875, 100.125, 0.125};

    bool missing = false;
    for (const FileSpec &f : files)
        missing = missing || !QFileInfo::exists(data + QStringLiteral("/sensor_%1.sqlite").arg(f.key));

    QString insertSql = QStringLiteral("INSERT INTO sensor_data (timestamp");
    for (int i = 1; i <= 20; ++i)
        insertSql += QStringLiteral(", s%1").arg(i);
    insertSql += QStringLiteral(") VALUES (?") + QStringLiteral(",?").repeated(20) + QLatin1Char(')');

    if (rebuild || missing) {
        QElapsedTimer timer;
        timer.start();
        PyMt rng(41);
        qint64 n = 0;
        for (const FileSpec &f : files) {
            const QString path = data + QStringLiteral("/sensor_%1.sqlite").arg(f.key);
            QFile::remove(path);
            const QString conn = QStringLiteral("bench_%1").arg(f.key);
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
                db.setDatabaseName(path);
                if (!db.open()) { std::fprintf(stderr, "cannot open %s\n", qPrintable(path)); return 1; }
                QSqlQuery q(db);
                QString cols, hcols;
                for (int i = 1; i <= 40; ++i) cols += QStringLiteral(", s%1 REAL").arg(i);
                for (int i = 1; i <= 100; ++i) hcols += QStringLiteral(", h%1 INTEGER").arg(i);
                if (!exec(q, QStringLiteral("PRAGMA journal_mode=DELETE"))
                    || !exec(q, QStringLiteral("CREATE TABLE sensor_data (timestamp INTEGER NOT NULL%1)").arg(cols))
                    || !exec(q, QStringLiteral("CREATE INDEX idx_sensor_data_ts ON sensor_data(timestamp)"))
                    || !exec(q, QStringLiteral("CREATE TABLE holding_register (timestamp INTEGER NOT NULL%1)").arg(hcols))
                    || !exec(q, QStringLiteral("CREATE INDEX idx_holding_register_ts ON holding_register(timestamp)"))
                    || !exec(q, QStringLiteral("CREATE TABLE alarm_history (id INTEGER PRIMARY KEY, occurrence_time INTEGER, reason VARCHAR(255))"))
                    || !exec(q, QStringLiteral("CREATE INDEX idx_alarm_history_time ON alarm_history(occurrence_time)")))
                    return 1;
                QSqlQuery ins(db);
                if (!ins.prepare(insertSql)) { std::fprintf(stderr, "prepare failed: %s\n", qPrintable(ins.lastError().text())); return 1; }
                std::vector<std::pair<qint64, Row>> batch;
                batch.reserve(50000);
                const auto flush = [&]() -> bool {
                    if (!db.transaction()) return false;
                    for (const auto &r : batch) {
                        ins.bindValue(0, r.first);
                        for (int i = 0; i < 20; ++i)
                            ins.bindValue(i + 1, r.second[i] ? QVariant(*r.second[i]) : QVariant(QMetaType::fromType<double>()));
                        if (!ins.exec()) { std::fprintf(stderr, "insert failed: %s\n", qPrintable(ins.lastError().text())); return false; }
                    }
                    batch.clear();
                    return db.commit();
                };
                for (const Span &s : f.spans) {
                    for (qint64 k = 0; k < s.count; ++k) {
                        Row vals;
                        for (int i = 0; i < 20; ++i) vals[i] = rng.uniform(0, 65535);
                        if (n % 997 == 0) vals[2].reset();
                        if (n % 1009 == 0) vals[13].reset();
                        if (n % 101 == 0) vals[11] = ties[(n / 101) % 6];
                        batch.push_back({s.start + k, vals});
                        ++n;
                        if (batch.size() == 50000 && !flush()) return 1;
                    }
                }
                if (f.key == QLatin1String("202609")) {
                    for (qint64 extra : {ts(2026, 9, 1, 0, 0, 5), ts(2026, 9, 8, 12), ts(2026, 9, 15, 23, 59, 59)}) {
                        Row vals;
                        for (int i = 0; i < 20; ++i) vals[i] = rng.uniform(0, 65535);
                        batch.push_back({extra, vals});
                    }
                }
                if (f.key == QLatin1String("202607")) {
                    Row ones, twos;
                    for (int i = 0; i < 20; ++i) { ones[i] = 1.0; twos[i] = 2.0; }
                    batch.push_back({0, ones});
                    batch.push_back({-5, twos});
                }
                if (!flush()) return 1;
                db.close();
            }
            QSqlDatabase::removeDatabase(conn);
        }
        std::printf("built in %.0f s\n", timer.elapsed() / 1000.0);
    }

    for (const FileSpec &f : files) {
        const QString path = data + QStringLiteral("/sensor_%1.sqlite").arg(f.key);
        const QString conn = QStringLiteral("check_%1").arg(f.key);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(path);
            db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
            if (!db.open()) { std::fprintf(stderr, "cannot open %s\n", qPrintable(path)); return 1; }
            QSqlQuery q(db);
            q.exec(QStringLiteral("SELECT COUNT(1), MIN(timestamp), MAX(timestamp) FROM sensor_data"));
            q.next();
            const qint64 c = q.value(0).toLongLong(), lo = q.value(1).toLongLong(), hi = q.value(2).toLongLong();
            q.exec(QStringLiteral("PRAGMA journal_mode"));
            q.next();
            std::printf("sensor_%s.sqlite: rows=%lld min=%lld max=%lld size=%lld journal=%s\n", qPrintable(f.key),
                        c, lo, hi, QFileInfo(path).size(), qPrintable(q.value(0).toString()));
            db.close();
        }
        QSqlDatabase::removeDatabase(conn);
    }
    std::printf("30-day range: %lld .. %lld s = %lld .. %lld ms\n", ts(2026, 8, 17), ts(2026, 9, 16) - 1,
                ts(2026, 8, 17) * 1000, ts(2026, 9, 16) * 1000 - 1);
    return 0;
}
