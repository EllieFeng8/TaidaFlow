// w2-067: SqlManager::shutdown() - the last step of Core::shutdown() at application exit.
// The test functions run in order on the one SqlManager singleton:
//   1. writesBeforeShutdown       - samples + an alarm written through the normal (blocking) API
//   2. shutdownClosesAndJoins     - shutdown(): worker thread finished, no SQLite connection left
//                                   (QSqlDatabase::connectionNames() is process-wide), log line
//   3. callsAfterShutdownRefused  - later calls return false / empty at once (no blocking call to a
//                                   thread that no longer runs), with the warning
//   4. dataFileIntact             - the monthly file opens (new connection), integrity_check ok,
//                                   every row written in 1. is there, the last one included
//   5. secondShutdownIsNoOp       - idempotent
#include "SqlManager.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

namespace {
QStringList g_messages;
QtMessageHandler g_previousHandler = nullptr;
void captureMessages(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    g_messages.append(message);
    if (g_previousHandler)
        g_previousHandler(type, context, message);
}

bool hasDataConnection(const QStringList &names)
{
    return std::any_of(names.cbegin(), names.cend(),
                       [](const QString &n) { return n.startsWith(QStringLiteral("data_")); });
}
} // namespace

class TestW2067SqlManagerShutdown : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void writesBeforeShutdown();
    void shutdownClosesAndJoins();
    void callsAfterShutdownRefused();
    void dataFileIntact();
    void secondShutdownIsNoOp();
    void cleanupTestCase();

private:
    QTemporaryDir *m_tmp = nullptr;
    QString m_dataDir;
    SqlManager *m_sql = nullptr;
    QList<qint64> m_written;   // epoch seconds of the rows written in writesBeforeShutdown
};

void TestW2067SqlManagerShutdown::initTestCase()
{
    QVERIFY(QDir().mkpath(QStringLiteral(W2067_WORK_DIR)));
    m_tmp = new QTemporaryDir(QStringLiteral(W2067_WORK_DIR) + QStringLiteral("/run-XXXXXX"));
    QVERIFY(m_tmp->isValid());
    QVERIFY(QDir::setCurrent(m_tmp->path()));
    m_dataDir = m_tmp->path() + QStringLiteral("/data");
    m_sql = SqlManager::instance();
    m_sql->setDataDirectory(m_dataDir);
    m_sql->setSettingsFile(m_tmp->path() + QStringLiteral("/settings.sqlite"));
    QVERIFY(m_sql->initialize());
    QVERIFY(!m_sql->isShutDown());
    QVERIFY(m_sql->thread() != QThread::currentThread());
    QVERIFY(m_sql->thread()->isRunning());
    g_previousHandler = qInstallMessageHandler(captureMessages);
}

void TestW2067SqlManagerShutdown::writesBeforeShutdown()
{
    // One row per second, like Manager's 1 s poll cycle; the last one just before shutdown().
    const qint64 base = QDateTime::currentSecsSinceEpoch() - 10;
    for (int i = 0; i < 10; ++i) {
        const qint64 ts = base + i;
        QVector<double> readings;
        for (int r = 0; r < 20; ++r)
            readings.append(1000 + i * 10 + r);
        QVERIFY(m_sql->saveSensorData(QDateTime::fromSecsSinceEpoch(ts), readings));
        m_written.append(ts);
    }
    QString err;
    QVERIFY2(m_sql->insertAlarm(QDateTime::fromSecsSinceEpoch(base + 9),
                                QStringLiteral("{\"sensor\":\"w2-067\"}"), &err),
             qPrintable(err));
    const QStringList names = QSqlDatabase::connectionNames();
    QVERIFY2(names.contains(QStringLiteral("settings")), qPrintable(names.join(QLatin1Char(','))));
    QVERIFY2(hasDataConnection(names), qPrintable(names.join(QLatin1Char(','))));
}

void TestW2067SqlManagerShutdown::shutdownClosesAndJoins()
{
    QThread *worker = m_sql->thread();
    g_messages.clear();
    QElapsedTimer t;
    t.start();
    m_sql->shutdown();
    qInfo() << "shutdown() took" << t.elapsed() << "ms";
    QVERIFY(m_sql->isShutDown());
    QVERIFY(worker->isFinished());
    const QStringList names = QSqlDatabase::connectionNames();
    QVERIFY2(!names.contains(QStringLiteral("settings")), qPrintable(names.join(QLatin1Char(','))));
    QVERIFY2(!hasDataConnection(names), qPrintable(names.join(QLatin1Char(','))));
    const QString joined = g_messages.join(QLatin1Char('\n'));
    QVERIFY2(joined.contains(QStringLiteral("[SQL] SqlManager stopped: worker thread finished, 2 SQLite connection(s) closed")),
             qPrintable(joined));
    QVERIFY2(!joined.contains(QStringLiteral("still in use")) && !joined.contains(QStringLiteral("does not belong")),
             qPrintable(joined));
}

void TestW2067SqlManagerShutdown::callsAfterShutdownRefused()
{
    g_messages.clear();
    QElapsedTimer t;
    t.start();
    QVERIFY(!m_sql->saveSensorData(QDateTime::currentDateTime(), QVector<double>(20, 1.0)));
    QJsonArray history;
    QVERIFY(!m_sql->getAlarmHistory(0, QDateTime::currentSecsSinceEpoch(), &history));
    QVERIFY(history.isEmpty());
    QVERIFY2(t.elapsed() < 1000, qPrintable(QString::number(t.elapsed())));   // returned at once
    QCOMPARE(g_messages.count(QStringLiteral("[SQL] request after SqlManager shutdown ignored.")), 2);
    // Nothing was reopened by the refused calls.
    QVERIFY(!QSqlDatabase::connectionNames().contains(QStringLiteral("settings")));
}

void TestW2067SqlManagerShutdown::dataFileIntact()
{
    const QString month = QDateTime::fromSecsSinceEpoch(m_written.last()).toString(QStringLiteral("yyyyMM"));
    const QString file = QDir(m_dataDir).filePath(QStringLiteral("sensor_%1.sqlite").arg(month));
    QVERIFY2(QFile::exists(file), qPrintable(file));
    QVERIFY2(!QFile::exists(file + QStringLiteral("-journal")), "rollback journal left behind");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("tst_check"));
        db.setDatabaseName(file);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("PRAGMA integrity_check")) && q.next());
        QCOMPARE(q.value(0).toString(), QStringLiteral("ok"));
        QVERIFY(q.exec(QStringLiteral("SELECT COUNT(*), MAX(timestamp) FROM sensor_data")) && q.next());
        QCOMPARE(q.value(0).toLongLong(), qint64(m_written.size()));
        QCOMPARE(q.value(1).toLongLong(), m_written.last());   // the last sample before shutdown()
        QVERIFY(q.exec(QStringLiteral("SELECT s1, s20 FROM sensor_data WHERE timestamp = %1").arg(m_written.last()))
                && q.next());
        QCOMPARE(q.value(0).toDouble(), 1090.0);
        QCOMPARE(q.value(1).toDouble(), 1109.0);
        QVERIFY(q.exec(QStringLiteral("SELECT COUNT(*) FROM alarm_history")) && q.next());
        QCOMPARE(q.value(0).toLongLong(), qint64(1));
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("tst_check"));
}

void TestW2067SqlManagerShutdown::secondShutdownIsNoOp()
{
    g_messages.clear();
    m_sql->shutdown();
    QVERIFY(m_sql->isShutDown());
    QVERIFY2(g_messages.isEmpty(), qPrintable(g_messages.join(QLatin1Char('\n'))));
}

void TestW2067SqlManagerShutdown::cleanupTestCase()
{
    qInstallMessageHandler(g_previousHandler);
    QDir::setCurrent(QStringLiteral(W2067_WORK_DIR));
    delete m_tmp;
}

QTEST_GUILESS_MAIN(TestW2067SqlManagerShutdown)
#include "tst_w2067_sqlmanager_shutdown.moc"
