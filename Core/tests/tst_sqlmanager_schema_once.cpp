// w2-071 D3/D5 (review D-002): SqlManager runs the data schema (data_schema.sql or the built-in
// DDL) only the first time a month connection is used, and logs "Schema file not found" at most
// once per run.
//
// How "once per connection" is measured without a test hook: the test puts a data_schema.sql
// into the current folder (SqlManager reads it from there) whose statements, besides the normal
// tables, append one row to a table schema_runs. After many saves / REST range queries /
// History page requests / alarm calls on the same month, schema_runs of that month file must
// hold exactly one row (it held one row per call before w2-071). Then the file is removed:
// a new month file is still created with all tables (built-in DDL, behaviour unchanged), and
// the whole run logs "Schema file not found" once (settings_schema.sql is never there either).
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "SqlManager.h"

namespace {
QMutex g_logMutex;
QStringList g_log;
QtMessageHandler g_previousHandler = nullptr;

void captureMessages(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    {
        QMutexLocker locker(&g_logMutex);
        g_log << message;
    }
    if (g_previousHandler)
        g_previousHandler(type, context, message);
}

int logCount(const QString &needle)
{
    QMutexLocker locker(&g_logMutex);
    int n = 0;
    for (const QString &line : std::as_const(g_log))
        n += line.contains(needle) ? 1 : 0;
    return n;
}

QString schemaWithCounter()
{
    QStringList sensorColumns{QStringLiteral("timestamp INTEGER NOT NULL")};
    for (int i = 1; i <= 40; ++i)
        sensorColumns << QStringLiteral("s%1 REAL").arg(i);
    QStringList holdingColumns{QStringLiteral("timestamp INTEGER NOT NULL")};
    for (int i = 1; i <= 100; ++i)
        holdingColumns << QStringLiteral("h%1 INTEGER").arg(i);
    return QStringList{
        QStringLiteral("CREATE TABLE IF NOT EXISTS sensor_data (%1)").arg(sensorColumns.join(QStringLiteral(", "))),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_sensor_data_ts ON sensor_data(timestamp)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS holding_register (%1)").arg(holdingColumns.join(QStringLiteral(", "))),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_holding_register_ts ON holding_register(timestamp)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS alarm_history (id INTEGER PRIMARY KEY, occurrence_time INTEGER, reason VARCHAR(255))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_alarm_history_time ON alarm_history(occurrence_time)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS schema_runs (n INTEGER)"),
        QStringLiteral("INSERT INTO schema_runs (n) VALUES (1)"),
    }.join(QStringLiteral(";\n")) + QStringLiteral(";\n");
}

// Reads with an own connection (not SqlManager's).
QVariant scalar(const QString &file, const QString &sql)
{
    QVariant value;
    const QString name = QStringLiteral("probe_%1").arg(qHash(file + sql));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(file);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (db.open()) {
            QSqlQuery q(db);
            if (q.exec(sql) && q.next())
                value = q.value(0);
            else
                value = QStringLiteral("error: %1").arg(q.lastError().text());
        }
    }
    QSqlDatabase::removeDatabase(name);
    return value;
}
} // namespace

class TestSqlManagerSchemaOnce : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void oncePerConnection();
    void secondMonthOwnConnection();
    void missingSchemaFileLoggedOnce();

private:
    void exerciseMonth(const QDate &month, int saves);
    QString monthFile(const QDate &month) const
    {
        return m_dir.filePath(QStringLiteral("data/sensor_%1.sqlite").arg(month.toString(QStringLiteral("yyyyMM"))));
    }

    QTemporaryDir m_dir;
    QString m_oldCwd;
};

void TestSqlManagerSchemaOnce::initTestCase()
{
    QVERIFY(m_dir.isValid());
    g_previousHandler = qInstallMessageHandler(captureMessages);
    m_oldCwd = QDir::currentPath();
    QVERIFY(QDir::setCurrent(m_dir.path()));
    QFile schema(m_dir.filePath(QStringLiteral("data_schema.sql")));
    QVERIFY(schema.open(QIODevice::WriteOnly | QIODevice::Text));
    schema.write(schemaWithCounter().toUtf8());
    schema.close();

    SqlManager *sql = SqlManager::instance();   // schema paths = the current folder
    sql->setDataDirectory(m_dir.filePath(QStringLiteral("data")));
    sql->setSettingsFile(m_dir.filePath(QStringLiteral("settings.sqlite")));
    QVERIFY(sql->initialize());
}

void TestSqlManagerSchemaOnce::cleanupTestCase()
{
    SqlManager::instance()->shutdown();
    QDir::setCurrent(m_oldCwd);
    qInstallMessageHandler(g_previousHandler);
}

// Every SqlManager path that calls ensureDataSchema on one month.
void TestSqlManagerSchemaOnce::exerciseMonth(const QDate &month, int saves)
{
    SqlManager *sql = SqlManager::instance();
    const qint64 start = QDateTime(month, QTime(1, 0)).toSecsSinceEpoch();
    const qint64 end = QDateTime(month.addMonths(1), QTime(0, 0)).toSecsSinceEpoch() - 1;
    for (int i = 0; i < saves; ++i) {
        QVector<double> readings(40, double(i));
        QVector<quint16> holdings(100, quint16(i));
        QVERIFY(sql->saveSensorData(QDateTime::fromSecsSinceEpoch(start + i), readings, holdings));
    }
    for (int i = 0; i < 5; ++i) {
        QJsonArray rows;
        QVERIFY(sql->queryRangeJsonPaged(start, end, 1, 10, &rows));
        QCOMPARE(rows.size(), qsizetype(qMin(10, saves)));
        QVERIFY(sql->queryHoldingRangeJsonPaged(start, end, 1, 10, &rows));
        qint64 total = 0;
        QVERIFY(sql->countSensorRange(start, end, &total));
        QCOMPARE(total, qint64(saves));
        QVERIFY(sql->countHoldingRange(start, end, &total));
        QCOMPARE(total, qint64(saves));
    }
    QVERIFY(!sql->fetchSensorData(month, 1).isEmpty());
    QVERIFY(!sql->fetchHoldingRegisters(month, 1).isEmpty());
    QVERIFY(sql->insertAlarm(QDateTime::fromSecsSinceEpoch(start + 5), QStringLiteral("{\"sensor\":\"t\"}")));
    QJsonArray alarms;
    QVERIFY(sql->getAlarmHistory(start, end, &alarms));
    QCOMPARE(alarms.size(), qsizetype(1));

    // History page (w2-045 stepped request) on the same month.
    // (emitted on the SqlManager thread; received queued on this thread)
    static quint64 requestId = 0;
    const quint64 id = ++requestId;
    bool received = false;
    SensorHistoryPageResult result;
    QObject receiver;
    connect(sql, &SqlManager::sensorHistoryPageReady, &receiver,
            [&](const SensorHistoryPageResult &r) {
                if (r.sessionKey == QLatin1String("schema-test") && r.requestId == id) {
                    result = r;
                    received = true;
                }
            },
            Qt::QueuedConnection);
    sql->requestSensorHistoryRangePage(QStringLiteral("schema-test"), id, start, end, 1, 20);
    QTRY_VERIFY_WITH_TIMEOUT(received, 10000);
    QVERIFY2(result.ok, qPrintable(result.errorMessage));
    QCOMPARE(result.totalRows, qint64(saves));
}

void TestSqlManagerSchemaOnce::oncePerConnection()
{
    const QDate month(2026, 5, 1);
    exerciseMonth(month, 30);
    const QVariant runs = scalar(monthFile(month), QStringLiteral("SELECT COUNT(*) FROM schema_runs"));
    qInfo("schema_runs rows in %s after 30 saves + 20 range calls + fetch + alarm + History page: %s",
          qPrintable(QFileInfo(monthFile(month)).fileName()), qPrintable(runs.toString()));
    QCOMPARE(runs.toLongLong(), 1LL);
}

void TestSqlManagerSchemaOnce::secondMonthOwnConnection()
{
    const QDate month(2026, 6, 1);
    exerciseMonth(month, 12);
    QCOMPARE(scalar(monthFile(month), QStringLiteral("SELECT COUNT(*) FROM schema_runs")).toLongLong(), 1LL);
    // Further calls on the first month (connection already checked) add no schema run.
    qint64 total = 0;
    const QDate may(2026, 5, 1);
    QVERIFY(SqlManager::instance()->countSensorRange(QDateTime(may, QTime(0, 0)).toSecsSinceEpoch(),
                                                     QDateTime(may.addMonths(1), QTime(0, 0)).toSecsSinceEpoch() - 1, &total));
    QCOMPARE(total, 30LL);
    QCOMPARE(scalar(monthFile(QDate(2026, 5, 1)), QStringLiteral("SELECT COUNT(*) FROM schema_runs")).toLongLong(), 1LL);
}

void TestSqlManagerSchemaOnce::missingSchemaFileLoggedOnce()
{
    QVERIFY(QFile::remove(m_dir.filePath(QStringLiteral("data_schema.sql"))));
    const QDate month(2026, 4, 1);
    SqlManager *sql = SqlManager::instance();
    for (int i = 0; i < 50; ++i) {
        QVector<double> readings(40, 1.5);
        QVERIFY(sql->saveSensorData(QDateTime(month, QTime(2, 0)).addSecs(i), readings));
        QVERIFY(sql->readFrequency() > 0);   // settings path: settings_schema.sql is missing too
    }
    // New month file created by the built-in DDL: every table, no schema_runs.
    const QString file = monthFile(month);
    QVERIFY(QFileInfo::exists(file));
    QCOMPARE(scalar(file, QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name IN "
                                         "('sensor_data', 'holding_register', 'alarm_history')")).toLongLong(), 3LL);
    QCOMPARE(scalar(file, QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE name = 'schema_runs'")).toLongLong(), 0LL);
    QCOMPARE(scalar(file, QStringLiteral("SELECT COUNT(*) FROM sensor_data")).toLongLong(), 50LL);

    const int warnings = logCount(QStringLiteral("Schema file not found"));
    qInfo("\"Schema file not found\" lines in the whole run: %d", warnings);
    QCOMPARE(warnings, 1);
}

QTEST_GUILESS_MAIN(TestSqlManagerSchemaOnce)
#include "tst_sqlmanager_schema_once.moc"
