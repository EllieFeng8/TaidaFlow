// w2-067 D3: opens a TaidaFlow monthly data file (sensor_yyyyMM.sqlite) READ-ONLY with Qt's SQLite
// driver and checks
//   1. PRAGMA integrity_check == "ok" (the file opens and is consistent after the app was closed);
//   2. for every close time given (epoch seconds of the WM_CLOSE of a run), the newest sensor_data
//      row at or before that time is at most --max-gap seconds older (default 3; the app saves one
//      row per 1 s poll cycle), i.e. the last seconds before the close were written.
// Usage: w2067_dbcheck <sensor_yyyyMM.sqlite> [--max-gap N] [closeEpochSec ...]
// Exit code: 0 = all checks passed, 1 = a check failed, 2 = usage / file cannot be opened.
#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QTextStream>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QStringList args = app.arguments().mid(1);
    if (args.isEmpty()) {
        out << "usage: w2067_dbcheck <sensor_yyyyMM.sqlite> [--max-gap N] [closeEpochSec ...]\n";
        return 2;
    }
    const QString path = args.takeFirst();
    qint64 maxGap = 3;
    QList<qint64> closes;
    for (int i = 0; i < args.size(); ++i) {
        if (args.at(i) == QLatin1String("--max-gap") && i + 1 < args.size()) {
            maxGap = args.at(++i).toLongLong();
            continue;
        }
        bool ok = false;
        const qint64 t = args.at(i).toLongLong(&ok);
        if (!ok) { out << "not an epoch second: " << args.at(i) << "\n"; return 2; }
        closes.append(t);
    }
    if (!QFileInfo(path).isFile()) { out << "file not found: " << path << "\n"; return 2; }
    int failed = 0;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("check"));
        db.setDatabaseName(path);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (!db.open()) { out << "cannot open " << path << ": " << db.lastError().text() << "\n"; return 2; }
        out << "file: " << QFileInfo(path).absoluteFilePath() << " (" << QFileInfo(path).size() << " bytes)\n";
        QSqlQuery q(db);
        q.exec(QStringLiteral("PRAGMA journal_mode"));
        out << "journal_mode: " << (q.next() ? q.value(0).toString() : QStringLiteral("?")) << "\n";
        QStringList integrity;
        if (q.exec(QStringLiteral("PRAGMA integrity_check"))) {
            while (q.next()) integrity << q.value(0).toString();
        } else {
            integrity << (QStringLiteral("query failed: ") + q.lastError().text());
        }
        const bool integrityOk = integrity == QStringList{QStringLiteral("ok")};
        out << "integrity_check: " << integrity.join(QStringLiteral(" | ")) << " -> " << (integrityOk ? "PASS" : "FAIL") << "\n";
        if (!integrityOk) ++failed;
        q.exec(QStringLiteral("SELECT COUNT(*), MIN(timestamp), MAX(timestamp) FROM sensor_data"));
        if (q.next()) {
            out << "sensor_data rows: " << q.value(0).toLongLong() << ", first "
                << QDateTime::fromSecsSinceEpoch(q.value(1).toLongLong()).toString(Qt::ISODate) << ", last "
                << QDateTime::fromSecsSinceEpoch(q.value(2).toLongLong()).toString(Qt::ISODate) << "\n";
        }
        for (const qint64 close : std::as_const(closes)) {
            QSqlQuery last(db);
            last.prepare(QStringLiteral("SELECT MAX(timestamp) FROM sensor_data WHERE timestamp <= :t"));
            last.bindValue(QStringLiteral(":t"), close);
            qint64 ts = -1;
            if (last.exec() && last.next() && !last.value(0).isNull()) ts = last.value(0).toLongLong();
            const qint64 gap = ts < 0 ? -1 : close - ts;
            const bool ok = ts >= 0 && gap <= maxGap;
            if (!ok) ++failed;
            out << "close " << QDateTime::fromSecsSinceEpoch(close).toString(Qt::ISODate) << ": newest row "
                << (ts < 0 ? QStringLiteral("none") : QDateTime::fromSecsSinceEpoch(ts).toString(Qt::ISODate))
                << ", " << gap << " s before the close (max " << maxGap << ") -> " << (ok ? "PASS" : "FAIL") << "\n";
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("check"));
    out << "=== " << failed << " check(s) failed\n";
    return failed ? 1 : 0;
}
