// w2-071 D2 evidence tool: the REST range functions of SqlManager before and after w2-071 must
// return the same rows for normal ranges. Built twice from this one file (build-and-compare.bat):
//   range_dump_before  <- Core/SqlManager.cpp of git HEAD (997042d, month loop), copied by
//                         "git show" into build\w2-071-compare\before-src (read-only git)
//   range_dump_after   <- Core/SqlManager.cpp of the work tree (w2-071, month file listing)
//
//   range_dump --seed <dataDir>          writes the synthetic 4-month data set (after build only)
//   range_dump --dump <dataDir> <out>    runs the query list below, writes one line per query
//
// Every line: query, ok flag, error text, total count, and the items JSON (compact). The two
// outputs are compared byte for byte (SHA256) by build-and-compare.bat.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTextStream>

#include "SqlManager.h"

namespace {
qint64 at(int y, int m, int d, int hh = 0, int mm = 0, int ss = 0)
{
    return QDateTime(QDate(y, m, d), QTime(hh, mm, ss)).toSecsSinceEpoch();
}

int seed(SqlManager *sql)
{
    // June .. September 2026, rows at irregular steps, the first and last second of each month,
    // duplicate timestamps, NULL values (fewer readings) and holding registers on every 3rd row.
    int rows = 0;
    for (int month = 6; month <= 9; ++month) {
        const qint64 first = at(2026, month, 1);
        const qint64 last = at(2026, month + 1 > 12 ? 1 : month + 1, 1) - 1;
        QList<qint64> stamps{first, first, last};
        for (qint64 t = first + 17; t < last && stamps.size() < 700; t += 3607 + (stamps.size() % 11) * 13)
            stamps << t;
        for (int i = 0; i < stamps.size(); ++i) {
            QVector<double> readings;
            const int n = (i % 7 == 0) ? 12 : 40;   // NULL for s13..s40 on every 7th row
            for (int s = 0; s < n; ++s)
                readings << (month * 1000 + i) + s * 0.25;
            QVector<quint16> holdings;
            if (i % 3 == 0) {
                for (int h = 0; h < 100; ++h)
                    holdings << quint16((month * 7 + i + h) % 65536);
            }
            if (!sql->saveSensorData(QDateTime::fromSecsSinceEpoch(stamps.at(i)), readings, holdings))
                return -1;
            ++rows;
        }
    }
    return rows;
}

struct Query
{
    qint64 from;
    qint64 to;
    int page;
    int pageSize;
};

QList<Query> queries()
{
    const QList<QPair<qint64, qint64>> ranges{
        {at(2026, 6, 1), at(2026, 9, 30, 23, 59, 59)},        // the whole seed
        {at(2026, 7, 10), at(2026, 7, 20)},                   // inside one month
        {at(2026, 7, 25), at(2026, 8, 5)},                    // across a month boundary
        {at(2026, 7, 31, 23, 59, 59), at(2026, 8, 1)},        // last second .. first second
        {at(2026, 8, 1), at(2026, 8, 1)},                     // a single second (2 rows)
        {0, 2147483647},                                      // 1970 .. 2038 (every file)
        {at(2026, 9, 15), 2147483647},
        {1, at(2026, 6, 30, 12)},
        {at(2025, 1, 1), at(2025, 12, 31)},                   // no file in range
    };
    const QList<QPair<int, int>> pages{{1, 1}, {2, 1}, {1, 7}, {5, 7}, {1, 200}, {2, 200}, {3, 200},
                                       {1, 1000}, {2, 1000}, {3, 1000}, {9, 1000}, {400, 7}};
    QList<Query> list;
    for (const auto &r : ranges) {
        for (const auto &p : pages)
            list << Query{r.first, r.second, p.first, p.second};
    }
    return list;
}

int dump(SqlManager *sql, const QString &outPath)
{
    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return 2;
    int lines = 0;
    for (const Query &q : queries()) {
        for (const bool holding : {false, true}) {
            QJsonArray items;
            QString pageError;
            const bool pageOk = holding
                    ? sql->queryHoldingRangeJsonPaged(q.from, q.to, q.page, q.pageSize, &items, &pageError)
                    : sql->queryRangeJsonPaged(q.from, q.to, q.page, q.pageSize, &items, &pageError);
            qint64 total = -1;
            QString countError;
            const bool countOk = holding ? sql->countHoldingRange(q.from, q.to, &total, &countError)
                                         : sql->countSensorRange(q.from, q.to, &total, &countError);
            const QByteArray line = QStringLiteral("%1 from=%2 to=%3 page=%4 pageSize=%5 pageOk=%6 countOk=%7 total=%8 "
                                                   "items=%9 err=[%10|%11] ")
                                            .arg(holding ? QStringLiteral("holding") : QStringLiteral("sensor"))
                                            .arg(q.from).arg(q.to).arg(q.page).arg(q.pageSize)
                                            .arg(pageOk).arg(countOk).arg(total).arg(items.size())
                                            .arg(pageError, countError)
                                            .toUtf8()
                    + QJsonDocument(items).toJson(QJsonDocument::Compact) + '\n';
            out.write(line);
            ++lines;
        }
    }
    out.close();
    QTextStream(stdout) << "wrote " << lines << " query results to " << QDir::toNativeSeparators(outPath) << Qt::endl;
    return 0;
}
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 3 || (args.at(1) == QLatin1String("--dump") && args.size() < 4)) {
        QTextStream(stderr) << "usage: range_dump --seed <dataDir> | --dump <dataDir> <outFile>" << Qt::endl;
        return 1;
    }
    SqlManager *sql = SqlManager::instance();
    sql->setDataDirectory(QDir(args.at(2)).absolutePath());
    sql->setSettingsFile(QDir(args.at(2)).absoluteFilePath(QStringLiteral("settings.sqlite")));
    int rc = 1;
    if (args.at(1) == QLatin1String("--seed")) {
        const int rows = seed(sql);
        QTextStream(stdout) << "seeded " << rows << " rows into " << args.at(2) << Qt::endl;
        rc = rows > 0 ? 0 : 3;
    } else if (args.at(1) == QLatin1String("--dump")) {
        rc = dump(sql, args.at(3));
    }
    sql->shutdown();   // w2-067 orderly stop (in both versions)
    return rc;
}
