// w2-053: DI alarm rows left 未處理 by an earlier run, handled on each DI's first
// read after a restart (taken over when the DI is still in alarm, resolved when it
// reads normal; only the newest one is taken over, the others are resolved).
//
// Real code under test: Core/manager.cpp (Manager), Core/SqlManager.cpp
// (findUnresolvedAlarms, insertAlarm, updateAlarmReason).  A "run" of the app is
// one Manager instance on the same SqlManager and the same data folder; a restart
// is deleting it and creating a new one (Manager keeps no state on disk other
// than the alarm rows).  DI samples go through the real signal path
// ModbusClient::registersRead -> Manager::mirrorClientData ->
// Manager::checkDigitalInputAlarm.  DI0=1 and DI2=0 unless a case needs
// otherwise, so no interlock write is attempted.  Rows are checked with a separate
// read connection of this test on the month files.
#include "Modbus_Client.h"
#include "SqlManager.h"
#include "TaidaFlowProxy.h"
#include "manager.h"

#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

namespace {

QStringList g_log;
QtMessageHandler g_previousHandler = nullptr;

void captureMessages(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    g_log.append(message);
    if (g_previousHandler)
        g_previousHandler(type, context, message);
}

QString monthKey(const QDate &date)
{
    return date.toString(QStringLiteral("yyyyMM"));
}

// Minute 'minute' of the first day of the month of 'date' (in the past for this
// month unless the test runs in the first minutes of a month).
QDateTime inMonth(const QDate &date, int minute)
{
    return QDateTime(QDate(date.year(), date.month(), 1), QTime(0, 0)).addSecs(60LL * minute);
}

QString reasonJson(const QString &sensor, const QString &message, const QString &status,
                   bool resolved = false)
{
    QJsonObject alarm{
        {QStringLiteral("sensor"), sensor},
        {QStringLiteral("alarmMessage"), message},
        {QStringLiteral("status"), status},
    };
    if (resolved) {
        alarm.insert(QStringLiteral("resolved"), true);
        alarm.insert(QStringLiteral("resolvedAt"), 1790000000);
        alarm.insert(QStringLiteral("resolvedDetail"), message + QStringLiteral(" 解除（earlier）"));
    }
    return QString::fromUtf8(QJsonDocument(alarm).toJson(QJsonDocument::Compact));
}

int countLog(const QString &needle)
{
    int n = 0;
    for (const QString &line : std::as_const(g_log)) {
        if (line.contains(needle))
            ++n;
    }
    return n;
}

} // namespace

class TestW2053DiRestart : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanupTestCase();

    void noPreviousRow();
    void previousRowStillAbnormal();
    void previousRowNowNormal();
    void multipleOldRowsStillAbnormal();
    void multipleOldRowsNowNormal();
    void checkRunsOncePerStart();
    void resolveRetriedAfterWriteFailure();
    void lookupRetriedAfterReadFailure();

private:
    std::unique_ptr<Manager> startRun();
    void feed(Manager &manager, int di0, int di1, int di2);
    qint64 seed(const QDateTime &occurrence, const QString &reason);
    QString dataFile(const QDate &month) const;
    int rowCount(const QDate &month);
    QString reasonText(const QDate &month, qint64 id);
    QJsonObject reason(const QDate &month, qint64 id);
    qint64 newestId(const QDate &month, const QString &sensor);

    QTemporaryDir *m_tmp = nullptr;
    QString m_dataDir;
    SqlManager *m_sql = nullptr;
    TaidaFlowProxy *m_proxy = nullptr;
    QDate m_cur;
    QDate m_prev;
    QDate m_old;
    int m_connectionSerial = 0;
};

void TestW2053DiRestart::initTestCase()
{
    qputenv("TAIDAFLOW_DEVICE_PROFILE", "simulator");   // only 127.0.0.x hosts, never used here
    QVERIFY(QDir().mkpath(QStringLiteral(W2053_WORK_DIR)));
    m_tmp = new QTemporaryDir(QStringLiteral(W2053_WORK_DIR) + QStringLiteral("/run-XXXXXX"));
    QVERIFY(m_tmp->isValid());
    // SqlManager's defaults and Manager's TaidaFlowSettings.ini use the current folder.
    QVERIFY(QDir::setCurrent(m_tmp->path()));
    m_dataDir = m_tmp->path() + QStringLiteral("/data");

    m_sql = SqlManager::instance();
    m_sql->setDataDirectory(m_dataDir);
    m_sql->setSettingsFile(m_tmp->path() + QStringLiteral("/settings.sqlite"));
    QVERIFY(m_sql->initialize());
    m_proxy = new TaidaFlowProxy;

    m_cur = QDate::currentDate();
    m_prev = m_cur.addMonths(-1);
    m_old = m_cur.addMonths(-2);
    // Month files used by the cases: this month, last month, the month before.
    for (const QDate &month : {m_cur, m_prev, m_old}) {
        QString err;
        QVERIFY2(m_sql->insertAlarm(inMonth(month, 0),
                                    reasonJson(QStringLiteral("setup"), QStringLiteral("create file"),
                                               QStringLiteral("正常")),
                                    &err),
                 qPrintable(err));
    }
    g_previousHandler = qInstallMessageHandler(captureMessages);
    qInfo().noquote() << "w2-053 data folder:" << m_dataDir << "months" << monthKey(m_cur)
                      << monthKey(m_prev) << monthKey(m_old);
}

void TestW2053DiRestart::init()
{
    // Every case starts with empty alarm_history tables (files stay, SqlManager
    // keeps its connections open).
    for (const QDate &month : {m_cur, m_prev, m_old}) {
        const QString name = QStringLiteral("tst_clear_%1").arg(++m_connectionSerial);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            db.setDatabaseName(dataFile(month));
            QVERIFY2(db.open(), qPrintable(db.lastError().text()));
            QSqlQuery query(db);
            QVERIFY2(query.exec(QStringLiteral("DELETE FROM alarm_history")),
                     qPrintable(query.lastError().text()));
            db.close();
        }
        QSqlDatabase::removeDatabase(name);
    }
    g_log.clear();
}

void TestW2053DiRestart::cleanupTestCase()
{
    qInstallMessageHandler(g_previousHandler);
    delete m_proxy;
    m_proxy = nullptr;
    QDir::setCurrent(QStringLiteral(W2053_WORK_DIR));
    // SqlManager is a process singleton that keeps its connections; the
    // temporary folder is removed by QTemporaryDir where Windows allows it.
    delete m_tmp;
}

std::unique_ptr<Manager> TestW2053DiRestart::startRun()
{
    // Same construction as Core does (core.cpp); Manager::start() is not
    // called, so no Modbus/serial connection is opened.
    return std::make_unique<Manager>(m_proxy, m_sql);
}

void TestW2053DiRestart::feed(Manager &manager, int di0, int di1, int di2)
{
    auto *client = manager.findChild<ModbusClient *>();
    QVERIFY(client);
    QElapsedTimer timer;
    timer.start();
    emit client->registersRead(ModbusClient::Device::Adam6224_204,
                               QModbusDataUnit::DiscreteInputs, 0,
                               QList<quint16>{quint16(di0), quint16(di1), quint16(di2)});
    qInfo().noquote() << QStringLiteral("[w2-053 test] fed DI0=%1 DI1=%2 DI2=%3, handled in %4 ms")
                                 .arg(di0).arg(di1).arg(di2)
                                 .arg(timer.nsecsElapsed() / 1.0e6, 0, 'f', 2);
}

qint64 TestW2053DiRestart::seed(const QDateTime &occurrence, const QString &reason)
{
    QString err;
    qint64 id = -1;
    if (!m_sql->insertAlarm(occurrence, reason, &err, &id))
        qWarning() << "seed failed" << err;
    return id;
}

QString TestW2053DiRestart::dataFile(const QDate &month) const
{
    return m_dataDir + QStringLiteral("/sensor_%1.sqlite").arg(monthKey(month));
}

int TestW2053DiRestart::rowCount(const QDate &month)
{
    int count = -1;
    const QString name = QStringLiteral("tst_read_%1").arg(++m_connectionSerial);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(dataFile(month));
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (db.open()) {
            QSqlQuery query(db);
            if (query.exec(QStringLiteral("SELECT COUNT(*) FROM alarm_history")) && query.next())
                count = query.value(0).toInt();
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(name);
    return count;
}

QString TestW2053DiRestart::reasonText(const QDate &month, qint64 id)
{
    QString text;
    const QString name = QStringLiteral("tst_read_%1").arg(++m_connectionSerial);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(dataFile(month));
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (db.open()) {
            QSqlQuery query(db);
            query.prepare(QStringLiteral("SELECT reason FROM alarm_history WHERE id = :id"));
            query.bindValue(QStringLiteral(":id"), id);
            if (query.exec() && query.next())
                text = query.value(0).toString();
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(name);
    return text;
}

QJsonObject TestW2053DiRestart::reason(const QDate &month, qint64 id)
{
    return QJsonDocument::fromJson(reasonText(month, id).toUtf8()).object();
}

qint64 TestW2053DiRestart::newestId(const QDate &month, const QString &sensor)
{
    qint64 id = -1;
    const QString name = QStringLiteral("tst_read_%1").arg(++m_connectionSerial);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(dataFile(month));
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (db.open()) {
            QSqlQuery query(db);
            if (query.exec(QStringLiteral("SELECT id, reason FROM alarm_history ORDER BY id DESC"))) {
                while (query.next()) {
                    const QJsonObject obj = QJsonDocument::fromJson(query.value(1).toString().toUtf8()).object();
                    if (obj.value(QStringLiteral("sensor")).toString() == sensor) {
                        id = query.value(0).toLongLong();
                        break;
                    }
                }
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(name);
    return id;
}

// Situation "無列": no unresolved row of an earlier run.  Behaviour as before:
// an abnormal DI inserts exactly one row, a normal read resolves it.
void TestW2053DiRestart::noPreviousRow()
{
    auto run = startRun();
    feed(*run, 1, 1, 0);
    QCOMPARE(rowCount(m_cur), 1);
    const qint64 id = newestId(m_cur, QStringLiteral("DI1"));
    QVERIFY(id > 0);
    QCOMPARE(reason(m_cur, id).value("alarmMessage").toString(), QStringLiteral("漏液檢出（DI1=1）"));
    QVERIFY(!reason(m_cur, id).contains("resolved"));
    QCOMPARE(countLog(QStringLiteral("[Alarm][Restart] DI1 first read after start: DI1=1 (alarm active); "
                                     "unresolved 異常 rows from earlier runs in %1+%2: 0")
                              .arg(monthKey(m_cur), monthKey(m_prev))), 1);

    feed(*run, 1, 1, 0);
    feed(*run, 1, 1, 0);
    QCOMPARE(rowCount(m_cur), 1);

    feed(*run, 1, 0, 0);
    QCOMPARE(rowCount(m_cur), 1);
    const QJsonObject after = reason(m_cur, id);
    QCOMPARE(after.value("resolved").toBool(), true);
    QCOMPARE(after.value("resolvedDetail").toString(), QStringLiteral("漏液檢出 解除（DI1=0）"));
    QCOMPARE(rowCount(m_prev), 0);
    QCOMPARE(rowCount(m_old), 0);
}

// Situation "異常" (DoD D3 scenario A in a test): the DI is still in alarm after
// the restart -> the row is taken over, no duplicate, later resolved in place.
void TestW2053DiRestart::previousRowStillAbnormal()
{
    qint64 id = -1;
    {
        auto first = startRun();
        feed(*first, 1, 1, 0);
        id = newestId(m_cur, QStringLiteral("DI1"));
    }   // app closed while DI1 is still 1
    QVERIFY(id > 0);
    QCOMPARE(rowCount(m_cur), 1);
    const QString before = reasonText(m_cur, id);
    g_log.clear();

    auto second = startRun();
    feed(*second, 1, 1, 0);
    feed(*second, 1, 1, 0);
    QCOMPARE(rowCount(m_cur), 1);                       // no duplicate row
    QCOMPARE(reasonText(m_cur, id), before);            // still 未處理, unchanged
    QCOMPARE(countLog(QStringLiteral("[Alarm][Restart] DI1 still in alarm: took over id=%1").arg(id)), 1);
    QCOMPARE(countLog(QStringLiteral("[SQL] Alarm inserted")), 0);

    feed(*second, 1, 0, 0);
    QCOMPARE(rowCount(m_cur), 1);
    const QJsonObject after = reason(m_cur, id);
    QCOMPARE(after.value("resolved").toBool(), true);
    QCOMPARE(after.value("status").toString(), QStringLiteral("異常"));
    QCOMPARE(after.value("alarmMessage").toString(), QStringLiteral("漏液檢出（DI1=1）"));
    QCOMPARE(after.value("resolvedDetail").toString(), QStringLiteral("漏液檢出 解除（DI1=0）"));
}

// Situation "正常" (scenario B in a test): the DI reads normal after the restart
// -> the row is resolved with 重啟後讀值, no new row.  Also DI0 (normal = 1) and a
// row in last month's file.
void TestW2053DiRestart::previousRowNowNormal()
{
    qint64 di1 = -1;
    {
        auto first = startRun();
        feed(*first, 1, 1, 0);
        di1 = newestId(m_cur, QStringLiteral("DI1"));
    }
    QVERIFY(di1 > 0);
    const qint64 di0 = seed(inMonth(m_cur, 5), reasonJson(QStringLiteral("DI0"), QStringLiteral("相位異常（DI0=0）"),
                                                          QStringLiteral("異常")));
    const qint64 di2 = seed(inMonth(m_prev, 30), reasonJson(QStringLiteral("DI2"), QStringLiteral("補水泵 OL（DI2=1）"),
                                                            QStringLiteral("異常")));
    QVERIFY(di0 > 0 && di2 > 0);
    const int curRows = rowCount(m_cur);
    const int prevRows = rowCount(m_prev);
    g_log.clear();

    auto second = startRun();
    feed(*second, 1, 0, 0);
    QCOMPARE(rowCount(m_cur), curRows);
    QCOMPARE(rowCount(m_prev), prevRows);
    QCOMPARE(reason(m_cur, di1).value("resolved").toBool(), true);
    QCOMPARE(reason(m_cur, di1).value("resolvedDetail").toString(), QStringLiteral("漏液檢出 解除（重啟後讀值 DI1=0）"));
    QCOMPARE(reason(m_cur, di0).value("resolved").toBool(), true);
    QCOMPARE(reason(m_cur, di0).value("resolvedDetail").toString(), QStringLiteral("相位異常 解除（重啟後讀值 DI0=1）"));
    QCOMPARE(reason(m_prev, di2).value("resolved").toBool(), true);
    QCOMPARE(reason(m_prev, di2).value("resolvedDetail").toString(), QStringLiteral("補水泵 OL 解除（重啟後讀值 DI2=0）"));
    QCOMPARE(reason(m_prev, di2).value("status").toString(), QStringLiteral("異常"));
    QCOMPARE(countLog(QStringLiteral("[SQL] Alarm resolved")), 3);
    QCOMPARE(countLog(QStringLiteral("[SQL] Alarm inserted")), 0);

    // Further normal reads change nothing.
    const QString settled = reasonText(m_cur, di1);
    feed(*second, 1, 0, 0);
    QCOMPARE(reasonText(m_cur, di1), settled);
    QCOMPARE(countLog(QStringLiteral("[SQL] Alarm resolved")), 3);
}

namespace {
struct OldRows {
    qint64 a = -1, b = -1, c = -1;          // DI1 unresolved: this month (a older, b newer), last month
    qint64 d = -1;                          // DI1 unresolved two months ago (outside the lookup)
    qint64 e = -1;                          // DI1 already resolved (this month, newest)
    qint64 f = -1;                          // DI1 status 正常 (old new-row format), this month
    qint64 g = -1;                          // MS300 異常, this month
    qint64 h = -1;                          // "DI12" 異常 (text contains DI1), this month
};
}

// Situation "多筆舊列": several unresolved rows of the same DI (e.g. w2-037 run A/B
// left overs) plus rows that must not be touched.
#define W2053_SEED_OLD_ROWS(rows)                                                                        \
    rows.a = seed(inMonth(m_cur, 10), reasonJson("DI1", QStringLiteral("漏液檢出（DI1=1）"), QStringLiteral("異常")));  \
    rows.b = seed(inMonth(m_cur, 20), reasonJson("DI1", QStringLiteral("漏液檢出（DI1=1）"), QStringLiteral("異常")));  \
    rows.c = seed(inMonth(m_prev, 40), reasonJson("DI1", QStringLiteral("漏液檢出（DI1=1）"), QStringLiteral("異常"))); \
    rows.d = seed(inMonth(m_old, 50), reasonJson("DI1", QStringLiteral("漏液檢出（DI1=1）"), QStringLiteral("異常")));  \
    rows.e = seed(inMonth(m_cur, 30), reasonJson("DI1", QStringLiteral("漏液檢出（DI1=1）"), QStringLiteral("異常"), true)); \
    rows.f = seed(inMonth(m_cur, 25), reasonJson("DI1", QStringLiteral("漏液檢出 解除（DI1=0）"), QStringLiteral("正常"))); \
    rows.g = seed(inMonth(m_cur, 26), reasonJson("MS300", QStringLiteral("MS300 fault"), QStringLiteral("異常")));    \
    rows.h = seed(inMonth(m_cur, 27), reasonJson("DI12", QStringLiteral("other"), QStringLiteral("異常")));

void TestW2053DiRestart::multipleOldRowsStillAbnormal()
{
    OldRows rows;
    W2053_SEED_OLD_ROWS(rows)
    QVERIFY(rows.a > 0 && rows.b > 0 && rows.c > 0 && rows.d > 0 && rows.e > 0 && rows.f > 0 && rows.g > 0 && rows.h > 0);
    const QString bBefore = reasonText(m_cur, rows.b);
    const QString dBefore = reasonText(m_old, rows.d);
    const QString eBefore = reasonText(m_cur, rows.e);
    const QString fBefore = reasonText(m_cur, rows.f);
    const QString gBefore = reasonText(m_cur, rows.g);
    const QString hBefore = reasonText(m_cur, rows.h);
    const int cur = rowCount(m_cur), prev = rowCount(m_prev), old = rowCount(m_old);

    auto run = startRun();
    feed(*run, 1, 1, 0);
    QCOMPARE(rowCount(m_cur), cur);
    QCOMPARE(rowCount(m_prev), prev);
    QCOMPARE(rowCount(m_old), old);
    QCOMPARE(reasonText(m_cur, rows.b), bBefore);                   // newest: taken over, still 未處理
    const QString dup = QStringLiteral("漏液檢出 解除（重啟後讀值 DI1=1，由 id=%1 接手）").arg(rows.b);
    QCOMPARE(reason(m_cur, rows.a).value("resolved").toBool(), true);
    QCOMPARE(reason(m_cur, rows.a).value("resolvedDetail").toString(), dup);
    QCOMPARE(reason(m_prev, rows.c).value("resolved").toBool(), true);
    QCOMPARE(reason(m_prev, rows.c).value("resolvedDetail").toString(), dup);
    QCOMPARE(reasonText(m_old, rows.d), dBefore);                   // outside this month + last month
    QCOMPARE(reasonText(m_cur, rows.e), eBefore);
    QCOMPARE(reasonText(m_cur, rows.f), fBefore);
    QCOMPARE(reasonText(m_cur, rows.g), gBefore);
    QCOMPARE(reasonText(m_cur, rows.h), hBefore);
    QCOMPARE(countLog(QStringLiteral("took over id=%1").arg(rows.b)), 1);
    QCOMPARE(countLog(QStringLiteral("[SQL] Alarm resolved")), 2);

    feed(*run, 1, 0, 0);
    QCOMPARE(rowCount(m_cur), cur);
    QCOMPARE(reason(m_cur, rows.b).value("resolved").toBool(), true);
    QCOMPARE(reason(m_cur, rows.b).value("resolvedDetail").toString(), QStringLiteral("漏液檢出 解除（DI1=0）"));
    QCOMPARE(reasonText(m_old, rows.d), dBefore);
}

void TestW2053DiRestart::multipleOldRowsNowNormal()
{
    OldRows rows;
    W2053_SEED_OLD_ROWS(rows)
    QVERIFY(rows.a > 0 && rows.b > 0 && rows.c > 0 && rows.d > 0 && rows.h > 0);
    const QString dBefore = reasonText(m_old, rows.d);
    const QString eBefore = reasonText(m_cur, rows.e);
    const QString fBefore = reasonText(m_cur, rows.f);
    const QString gBefore = reasonText(m_cur, rows.g);
    const QString hBefore = reasonText(m_cur, rows.h);
    const int cur = rowCount(m_cur), prev = rowCount(m_prev), old = rowCount(m_old);

    auto run = startRun();
    feed(*run, 1, 0, 0);
    QCOMPARE(rowCount(m_cur), cur);
    QCOMPARE(rowCount(m_prev), prev);
    QCOMPARE(rowCount(m_old), old);
    const QString detail = QStringLiteral("漏液檢出 解除（重啟後讀值 DI1=0）");
    QCOMPARE(reason(m_cur, rows.a).value("resolvedDetail").toString(), detail);
    QCOMPARE(reason(m_cur, rows.b).value("resolvedDetail").toString(), detail);
    QCOMPARE(reason(m_prev, rows.c).value("resolvedDetail").toString(), detail);
    QCOMPARE(reason(m_cur, rows.b).value("resolved").toBool(), true);
    QCOMPARE(reasonText(m_old, rows.d), dBefore);
    QCOMPARE(reasonText(m_cur, rows.e), eBefore);
    QCOMPARE(reasonText(m_cur, rows.f), fBefore);
    QCOMPARE(reasonText(m_cur, rows.g), gBefore);
    QCOMPARE(reasonText(m_cur, rows.h), hBefore);
    QCOMPARE(countLog(QStringLiteral("[SQL] Alarm resolved")), 3);
    QCOMPARE(countLog(QStringLiteral("took over")), 0);
}

// D2: the hand-over check runs once per DI per start.
void TestW2053DiRestart::checkRunsOncePerStart()
{
    auto run = startRun();
    feed(*run, 1, 0, 0);
    // A row that appears later is not an earlier run's row for this start.
    const qint64 late = seed(inMonth(m_cur, 60), reasonJson("DI1", QStringLiteral("漏液檢出（DI1=1）"), QStringLiteral("異常")));
    const QString before = reasonText(m_cur, late);
    for (int i = 0; i < 5; ++i)
        feed(*run, 1, 0, 0);
    QCOMPARE(reasonText(m_cur, late), before);
    QCOMPARE(countLog(QStringLiteral("[Alarm][Restart] DI0 first read after start")), 1);
    QCOMPARE(countLog(QStringLiteral("[Alarm][Restart] DI1 first read after start")), 1);
    QCOMPARE(countLog(QStringLiteral("[Alarm][Restart] DI2 first read after start")), 1);

    // A new start checks again.
    run.reset();
    g_log.clear();
    auto again = startRun();
    feed(*again, 1, 0, 0);
    QCOMPARE(reason(m_cur, late).value("resolvedDetail").toString(), QStringLiteral("漏液檢出 解除（重啟後讀值 DI1=0）"));
}

// D2: a failed resolve update (database write lock) is retried on the next poll.
void TestW2053DiRestart::resolveRetriedAfterWriteFailure()
{
    const qint64 id = seed(inMonth(m_cur, 15), reasonJson("DI1", QStringLiteral("漏液檢出（DI1=1）"), QStringLiteral("異常")));
    QVERIFY(id > 0);
    const QString before = reasonText(m_cur, id);
    const int cur = rowCount(m_cur);

    const QString name = QStringLiteral("tst_lock_%1").arg(++m_connectionSerial);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(dataFile(m_cur));
        db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=0"));
        QVERIFY(db.open());
        QSqlQuery lock(db);
        QVERIFY2(lock.exec(QStringLiteral("BEGIN IMMEDIATE")), qPrintable(lock.lastError().text()));   // readers ok, writers blocked

        auto run = startRun();
        feed(*run, 1, 0, 0);                                  // lookup ok, UPDATE fails (busy)
        QCOMPARE(countLog(QStringLiteral("[Alarm][Restart] DI1 resolving id=%1").arg(id)), 1);
        QCOMPARE(countLog(QStringLiteral("[SQL] Resolve alarm failed: id=%1").arg(id)), 1);

        QVERIFY(lock.exec(QStringLiteral("ROLLBACK")));
        lock.finish();
        QCOMPARE(reasonText(m_cur, id), before);              // not changed yet

        feed(*run, 1, 0, 0);                                  // retried on the next poll
        QCOMPARE(reason(m_cur, id).value("resolved").toBool(), true);
        QCOMPARE(reason(m_cur, id).value("resolvedDetail").toString(), QStringLiteral("漏液檢出 解除（重啟後讀值 DI1=0）"));
        QCOMPARE(rowCount(m_cur), cur);
        QCOMPARE(countLog(QStringLiteral("[Alarm][Restart] DI1 first read after start")), 1);
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
}

// D2: a failed lookup (last month's file locked) writes no new row and is retried
// on the next poll; the other DIs of that poll are deferred (one busy wait per
// poll); then the row is taken over.
void TestW2053DiRestart::lookupRetriedAfterReadFailure()
{
    const qint64 id = seed(inMonth(m_cur, 15), reasonJson("DI1", QStringLiteral("漏液檢出（DI1=1）"), QStringLiteral("異常")));
    QVERIFY(id > 0);
    const QString before = reasonText(m_cur, id);
    const int cur = rowCount(m_cur);

    auto run = startRun();
    const QString name = QStringLiteral("tst_lock_%1").arg(++m_connectionSerial);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(dataFile(m_prev));
        db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=0"));
        QVERIFY(db.open());
        QSqlQuery lock(db);
        QVERIFY2(lock.exec(QStringLiteral("BEGIN EXCLUSIVE")), qPrintable(lock.lastError().text()));   // readers blocked too

        QElapsedTimer blocked;
        blocked.start();
        feed(*run, 1, 1, 0);                                  // lookup fails on last month's file
        // DI0 (first in the sample) waits for SQLite's busy timeout and fails;
        // DI1 and DI2 of the same poll are deferred instead of waiting again.
        QCOMPARE(countLog(QStringLiteral("[Alarm][Restart] DI0 lookup of unresolved rows from earlier runs failed (attempt 1/5")), 1);
        QCOMPARE(countLog(QStringLiteral("[Alarm][Restart] DI1 lookup deferred to the next poll")), 1);
        QCOMPARE(countLog(QStringLiteral("[Alarm][Restart] DI2 lookup deferred to the next poll")), 1);
        QCOMPARE(countLog(QStringLiteral("[Alarm][Restart] DI1 lookup of unresolved rows")), 0);
        qInfo().noquote() << QStringLiteral("[w2-053 test] poll with the locked file took %1 ms").arg(blocked.elapsed());
        QCOMPARE(rowCount(m_cur), cur);                       // this month's file is writable, yet no new row
        QCOMPARE(countLog(QStringLiteral("[SQL] Alarm inserted")), 0);

        QVERIFY(lock.exec(QStringLiteral("ROLLBACK")));
        lock.finish();
        db.close();
    }
    QSqlDatabase::removeDatabase(name);

    QTest::qWait(1000);                                       // next poll (1 s interval in the app)
    feed(*run, 1, 1, 0);                                      // retried: taken over
    QCOMPARE(countLog(QStringLiteral("took over id=%1").arg(id)), 1);
    QCOMPARE(rowCount(m_cur), cur);
    QCOMPARE(reasonText(m_cur, id), before);
    QCOMPARE(countLog(QStringLiteral("[SQL] Alarm inserted")), 0);

    feed(*run, 1, 0, 0);
    QCOMPARE(reason(m_cur, id).value("resolvedDetail").toString(), QStringLiteral("漏液檢出 解除（DI1=0）"));
    QCOMPARE(rowCount(m_cur), cur);
}

QTEST_MAIN(TestW2053DiRestart)
#include "tst_w2053_di_restart.moc"
