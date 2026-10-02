// w2-085 test tool: prints alarm_history rows of <dataDir>/sensor_<yyyyMM>.sqlite (read-only
// connection, nothing is created or changed), one line per row:
//   id=<id> time=<yyyy-MM-dd HH:mm:ss> sensor=<..> status=<..> limit=<..> resolved=<0|1> message=<..> [detail=<..>]
// Usage: alarm_dump <data folder> [--month yyyyMM] [--min-id N] [--sensor NAME]
// Exit 0 = file read (also with 0 rows); 1 = file missing / not readable.
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 2) {
        std::fprintf(stderr, "usage: alarm_dump <data folder> [--month yyyyMM] [--min-id N] [--sensor NAME]\n");
        return 1;
    }
    QString month = QDate::currentDate().toString(QStringLiteral("yyyyMM"));
    qint64 minId = 0;
    QString sensorFilter;
    for (int i = 2; i + 1 < args.size(); i += 2) {
        if (args.at(i) == QLatin1String("--month")) month = args.at(i + 1);
        else if (args.at(i) == QLatin1String("--min-id")) minId = args.at(i + 1).toLongLong();
        else if (args.at(i) == QLatin1String("--sensor")) sensorFilter = args.at(i + 1);
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
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT id, occurrence_time, reason FROM alarm_history WHERE id > :min ORDER BY id"));
        q.bindValue(QStringLiteral(":min"), minId);
        if (!q.exec()) {
            std::printf("query failed: %s\n", qPrintable(q.lastError().text()));
            return 1;
        }
        std::printf("db=%s (read-only) rows with id > %lld%s:\n", qPrintable(QDir::toNativeSeparators(file)), minId,
                    sensorFilter.isEmpty() ? "" : qPrintable(QStringLiteral(" sensor=") + sensorFilter));
        while (q.next()) {
            const QJsonObject o = QJsonDocument::fromJson(q.value(2).toString().toUtf8()).object();
            const QString sensor = o.value(QStringLiteral("sensor")).toString();
            if (!sensorFilter.isEmpty() && sensor != sensorFilter)
                continue;
            ++rows;
            QString line = QStringLiteral("id=%1 time=%2 sensor=%3 status=%4 limit=%5 resolved=%6 message=%7")
                    .arg(q.value(0).toLongLong())
                    .arg(QDateTime::fromSecsSinceEpoch(q.value(1).toLongLong()).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                         sensor, o.value(QStringLiteral("status")).toString(),
                         o.value(QStringLiteral("limit")).toString(QStringLiteral("-")),
                         o.value(QStringLiteral("resolved")).toBool() ? QStringLiteral("1") : QStringLiteral("0"),
                         o.value(QStringLiteral("alarmMessage")).toString());
            if (o.contains(QStringLiteral("resolvedDetail")))
                line += QStringLiteral(" detail=%1 resolvedAt=%2")
                        .arg(o.value(QStringLiteral("resolvedDetail")).toString(),
                             QDateTime::fromSecsSinceEpoch(o.value(QStringLiteral("resolvedAt")).toInteger())
                                     .toString(QStringLiteral("HH:mm:ss")));
            const QByteArray utf8 = (line + QLatin1Char('\n')).toUtf8();
            std::fwrite(utf8.constData(), 1, size_t(utf8.size()), stdout);
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("dump"));
    std::printf("%d row(s)\n", rows);
    return 0;
}
