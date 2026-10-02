// w2-086 test tool (dev-only): prints the sensor_data rows of <dataDir>/sensor_<yyyyMM>.sqlite in insert
// order (read-only connection, nothing is created or changed), one line per row:
//   rowid=<n> ts=<epoch s> time=<yyyy-MM-dd HH:mm:ss> s=<s1>,<s2>,...,<s16>
// Usage: sensor_dump <data folder> [--month yyyyMM]
// Exit 0 = file read (also with 0 rows); 1 = file missing / not readable.
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>

#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 2) {
        std::fprintf(stderr, "usage: sensor_dump <data folder> [--month yyyyMM]\n");
        return 1;
    }
    QString month = QDate::currentDate().toString(QStringLiteral("yyyyMM"));
    for (int i = 2; i + 1 < args.size(); i += 2) {
        if (args.at(i) == QLatin1String("--month"))
            month = args.at(i + 1);
    }
    const QString file = QDir(args.at(1)).filePath(QStringLiteral("sensor_%1.sqlite").arg(month));
    if (!QFileInfo::exists(file)) {
        std::printf("file not found: %s\n", qPrintable(QDir::toNativeSeparators(file)));
        return 1;
    }
    int rows = 0;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("dump"));
        db.setDatabaseName(file);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000"));
        if (!db.open()) {
            std::printf("open failed: %s\n", qPrintable(db.lastError().text()));
            return 1;
        }
        QStringList cols;
        for (int i = 1; i <= 16; ++i)
            cols << QStringLiteral("s%1").arg(i);
        QSqlQuery q(db);
        if (!q.exec(QStringLiteral("SELECT rowid, timestamp, %1 FROM sensor_data ORDER BY rowid").arg(cols.join(QLatin1Char(','))))) {
            std::printf("query failed: %s\n", qPrintable(q.lastError().text()));
            return 1;
        }
        std::printf("db=%s (read-only) sensor_data rows in insert order:\n", qPrintable(QDir::toNativeSeparators(file)));
        while (q.next()) {
            ++rows;
            QStringList values;
            for (int i = 0; i < 16; ++i)
                values << (q.value(2 + i).isNull() ? QStringLiteral("null") : QString::number(q.value(2 + i).toDouble(), 'g', 12));
            const qint64 ts = q.value(1).toLongLong();
            std::printf("rowid=%lld ts=%lld time=%s s=%s\n", q.value(0).toLongLong(), ts,
                        qPrintable(QDateTime::fromSecsSinceEpoch(ts).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))),
                        qPrintable(values.join(QLatin1Char(','))));
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("dump"));
    std::printf("%d row(s)\n", rows);
    return 0;
}
