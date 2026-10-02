// w2-085 D1/D2/D4: settings-page upper / lower limit alarms (LimitAlarmMonitor, Core/LimitAlarms.h).
// Real TaidaFlowProxy, real SqlManager on a temporary data folder, real AlarmViewService; the
// monitor's clock is injected (ms), except in realClockTimerResolves and managerIntegration.
// No device; managerIntegration runs the real Manager against two Modbus TCP servers of this test
// on 127.0.0.1 (ports picked by the OS) and a serial port name that does not exist.
//
//  * upperAndLowerInsertAtOnce: above upper / below lower -> one row each at once, status 警告,
//    sensor = screen name, message text; alarmRecords (the Core::loadAlarmRecords conversion) and
//    alarmViews show them 未處理 / 警告; repeated updates add no second row;
//  * equalIsNormal / disabledNotJudged: value == limit (also via offset) and disabled limits: no row;
//  * resolveAfterTwoSeconds: back in range, 1.9 s later over the limit again -> same row, nothing
//    resolved, no new row; in range 1999 ms -> still open; 2000 ms -> resolved (resolved, resolvedAt,
//    resolvedDetail); alarmRecords / alarmViews show 已解除 and activeCount 0;
//  * realClockTimerResolves: the 2 s countdown runs on the monitor's own timer (no PV update needed);
//  * groupSensorsSeparately: PT-04 / PT-05 share limits but are recorded and resolved separately;
//  * filterUsesCorrectedDifference / offsetApplied: filter = corrected PT-02 - corrected PT-03 with
//    the filter limits; offsets count, PVs stay raw; filter needs both PVs;
//  * settingsChangeReevaluates: a stricter limit raises at once without a PV update, disabling a
//    limit resolves 2 s later, lower and upper are independent rows;
//  * restartNoDuplicate: a new monitor (= restart) takes over the newest unresolved row of each kind
//    (this and last month's file), resolves older duplicates at once, writes no new row while still
//    violated, resolves a recovered one 2 s later; other sensors' rows and 數值異常 / 異常 rows untouched;
//  * restartLookupFailure: a locked month file -> no new row, other sensors deferred, retried; after
//    the last attempt (shared by all sensors) the normal logic runs for the remaining sensors at once
//    (DI pattern of w2-053, without one busy wait per sensor);
//  * uiRuleEquivalence: TaidaFlowContent/components/SensorUnits.js + the limitState expression of
//    Main.qml (read from the sources, run in QJSEngine) give the same state as limitState() for a
//    grid of values, offsets, flags and limits (all 13 sensors, filter with PT-02 / PT-03);
//  * managerIntegration: Modbus reply -> Manager::updateProcessPoint -> alarm row, Manager::alarmSaved
//    emitted; back in range -> resolved; the 90 % high alarm (數值異常) still written as before;
//  * sourceWiring: manager.cpp / core.cpp / Core/CMakeLists.txt wiring.
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QModbusTcpServer>
#include <QMutex>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTemporaryDir>

#include <cmath>
#include <memory>

#include "AlarmRecordFormat.h"
#include "AlarmViews.h"
#include "LimitAlarms.h"
#include "ModbusMapping.h"
#include "SqlManager.h"
#include "TaidaFlowProxy.h"
#include "manager.h"

#ifndef CORE_SOURCE_DIR
#error CORE_SOURCE_DIR must point to taidaflow/Core
#endif

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

struct DbRow
{
    qint64 id = -1;
    QDateTime occurrence;
    QJsonObject reason;
    QString sensor() const { return reason.value(QStringLiteral("sensor")).toString(); }
    QString message() const { return reason.value(QStringLiteral("alarmMessage")).toString(); }
    QString status() const { return reason.value(QStringLiteral("status")).toString(); }
    bool resolved() const { return reason.value(QStringLiteral("resolved")).toBool(false); }
};

QString json(const QJsonObject &o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }

QString readSource(const QString &relative)
{
    QFile f(QStringLiteral(CORE_SOURCE_DIR "/") + relative);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll()).replace(QStringLiteral("\r\n"), QStringLiteral("\n"));   // autocrlf checkouts
}

// Two ports the OS picks (both probes listen at the same time, so they differ).
void freePorts(quint16 *a, quint16 *b)
{
    QTcpServer probeA;
    QTcpServer probeB;
    probeA.listen(QHostAddress::LocalHost, 0);
    probeB.listen(QHostAddress::LocalHost, 0);
    *a = probeA.serverPort();
    *b = probeB.serverPort();
}

using Limit = LimitAlarmMonitor::Limit;
} // namespace

class TestLimitAlarms : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void upperAndLowerInsertAtOnce();
    void equalIsNormal();
    void disabledNotJudged();
    void resolveAfterTwoSeconds();
    void realClockTimerResolves();
    void groupSensorsSeparately();
    void filterUsesCorrectedDifference();
    void offsetApplied();
    void settingsChangeReevaluates();
    void restartNoDuplicate();
    void restartLookupFailure();
    void uiRuleEquivalence();
    void managerIntegration();
    void sourceWiring();

private:
    std::unique_ptr<LimitAlarmMonitor> makeMonitor(LimitAlarmMonitor::Options options = {});
    void setLimits(const QString &key, bool lowerEnabled, double lower, bool upperEnabled, double upper,
                   double offset = 0.0);
    void setOffset(const QString &key, double offset);
    void setPv(const QString &key, double raw, bool flush = true);
    QList<DbRow> rows() const;                      // rows written during this test function, oldest first
    QList<DbRow> allRows() const;                   // every alarm row of the last 40 days, oldest first
    QList<DbRow> rowsOf(const QString &sensor) const;
    QList<DbRow> openRows() const;                  // not resolved
    DbRow rowById(qint64 id, const QDate &month) const;
    void resolveAllOpen();                          // test isolation: nothing open for the next test
    void loadAlarmRecords();                        // Core::loadAlarmRecords' conversion
    QVariantMap recordOf(qint64 id) const;          // alarmRecords entry
    void requestDesktopView();
    QVariantMap desktopView() const { return m_proxy->alarmViews().value(QStringLiteral("desktop")).toMap(); }
    qint64 seed(const QDateTime &time, const QJsonObject &reason);

    QTemporaryDir m_dir;
    SqlManager *m_sql = nullptr;
    std::unique_ptr<TaidaFlowProxy> m_proxy;
    std::unique_ptr<AlarmViewService> m_views;
    std::unique_ptr<LimitAlarmMonitor> m_monitor;
    qint64 m_now = 0;
    int m_lockSerial = 0;
    QSet<QString> m_baseline;                       // rows that existed before the test function
};

// ---- helpers ----------------------------------------------------------------------------------

std::unique_ptr<LimitAlarmMonitor> TestLimitAlarms::makeMonitor(LimitAlarmMonitor::Options options)
{
    if (!options.clock)
        options.clock = [this]() { return m_now; };
    auto monitor = std::make_unique<LimitAlarmMonitor>(m_proxy.get(), m_sql, options);
    connect(monitor.get(), &LimitAlarmMonitor::alarmSaved, this, &TestLimitAlarms::loadAlarmRecords);
    return monitor;
}

void TestLimitAlarms::setLimits(const QString &key, bool lowerEnabled, double lower, bool upperEnabled,
                                double upper, double offset)
{
    QVariantMap settings = m_proxy->sensorSettingsSv();       // copy, replace one entry, write back
    settings.insert(key, QVariantMap{{QStringLiteral("offset"), offset},
                                     {QStringLiteral("lower"), lower},
                                     {QStringLiteral("upper"), upper},
                                     {QStringLiteral("lowerEnabled"), lowerEnabled},
                                     {QStringLiteral("upperEnabled"), upperEnabled}});
    m_proxy->setSensorSettingsSv(settings);
    const QVariantMap entry = m_proxy->sensorSettingsSv().value(key).toMap();
    QCOMPARE(entry.value("upper").toDouble(), upper);
    QCOMPARE(entry.value("lowerEnabled").toBool(), lowerEnabled);
}

void TestLimitAlarms::setOffset(const QString &key, double offset)
{
    QVariantMap settings = m_proxy->sensorSettingsSv();
    QVariantMap entry = settings.value(key).toMap();
    entry.insert(QStringLiteral("offset"), offset);
    settings.insert(key, entry);
    m_proxy->setSensorSettingsSv(settings);
}

void TestLimitAlarms::setPv(const QString &key, double raw, bool flush)
{
    TaidaFlowProxy &p = *m_proxy;
    if (key == "pt01") p.setPt01ValuePv(raw);
    else if (key == "pt02") p.setPt02ValuePv(raw);
    else if (key == "pt03") p.setPt03ValuePv(raw);
    else if (key == "pt04") p.setPt04ValuePv(raw);
    else if (key == "pt05") p.setPt05ValuePv(raw);
    else if (key == "pt06") p.setPt06ValuePv(raw);
    else if (key == "pt07") p.setPt07ValuePv(raw);
    else if (key == "tt01") p.setTt01ValuePv(raw);
    else if (key == "tt02") p.setTt02ValuePv(raw);
    else if (key == "tt03") p.setTt03ValuePv(raw);
    else if (key == "tt04") p.setTt04ValuePv(raw);
    else if (key == "flowMeter") p.setFlowMeterValuePv(raw);
    else qFatal("unknown key");
    m_monitor->valueUpdated(key);          // what Manager::updateProcessPoint does after the setter
    if (flush)
        m_monitor->flushPendingUpdates();
}

QString rowKey(const DbRow &r) { return r.occurrence.toString(QStringLiteral("yyyyMM")) + QLatin1Char(':') + QString::number(r.id); }

QList<DbRow> TestLimitAlarms::rows() const
{
    QList<DbRow> list;
    for (const DbRow &r : allRows())
        if (!m_baseline.contains(rowKey(r)))
            list << r;
    return list;
}

QList<DbRow> TestLimitAlarms::allRows() const
{
    const QDateTime now = QDateTime::currentDateTime();
    QJsonArray history;
    QString error;
    if (!m_sql->getAlarmHistory(now.addDays(-40).toSecsSinceEpoch(), now.addDays(1).toSecsSinceEpoch(), &history, &error))
        qWarning() << "getAlarmHistory failed" << error;
    QList<DbRow> list;
    for (const QJsonValue &v : std::as_const(history)) {
        const QJsonObject o = v.toObject();
        DbRow r;
        r.id = static_cast<qint64>(o.value("id").toDouble());
        r.occurrence = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(o.value("occurrence_time").toDouble()));
        r.reason = QJsonDocument::fromJson(o.value("reason").toString().toUtf8()).object();
        list << r;
    }
    std::stable_sort(list.begin(), list.end(), [](const DbRow &a, const DbRow &b) {
        if (a.occurrence != b.occurrence)
            return a.occurrence < b.occurrence;
        return a.id < b.id;
    });
    return list;
}

QList<DbRow> TestLimitAlarms::rowsOf(const QString &sensor) const
{
    QList<DbRow> list;
    for (const DbRow &r : rows())
        if (r.sensor() == sensor)
            list << r;
    return list;
}

QList<DbRow> TestLimitAlarms::openRows() const
{
    QList<DbRow> list;
    for (const DbRow &r : allRows())
        if (!r.resolved())
            list << r;
    return list;
}

DbRow TestLimitAlarms::rowById(qint64 id, const QDate &month) const
{
    for (const DbRow &r : allRows())
        if (r.id == id && r.occurrence.date().year() == month.year() && r.occurrence.date().month() == month.month())
            return r;
    return DbRow{};
}

void TestLimitAlarms::resolveAllOpen()
{
    for (DbRow r : openRows()) {
        r.reason.insert(QStringLiteral("resolved"), true);
        r.reason.insert(QStringLiteral("resolvedDetail"), QStringLiteral("test cleanup"));
        QString error;
        if (!m_sql->updateAlarmReason(r.occurrence, r.id, json(r.reason), &error))
            qWarning() << "cleanup update failed" << r.id << error;
    }
}

void TestLimitAlarms::loadAlarmRecords()
{
    // Core::loadAlarmRecords (core.cpp), the slot Manager::alarmSaved is connected to.
    const QDateTime now = QDateTime::currentDateTime();
    QJsonArray history;
    QString error;
    if (!m_sql->getAlarmHistory(now.addDays(-3).toSecsSinceEpoch(), now.toSecsSinceEpoch(), &history, &error)) {
        // Core logs this and shows no rows; here only while restartLookupFailure locks a month file.
        qWarning().noquote() << "[w2-085 test] alarm history not loaded:" << error;
        return;
    }
    m_proxy->setAlarmRecords(AlarmRecordFormat::recordsFromHistory(history, nullptr));
}

QVariantMap TestLimitAlarms::recordOf(qint64 id) const
{
    const QDate today = QDate::currentDate();
    for (const QVariant &v : m_proxy->alarmRecords()) {
        const QVariantMap m = v.toMap();
        const QDate d = QDateTime::fromMSecsSinceEpoch(m.value("timestampMs").toLongLong()).date();
        if (m.value("id").toLongLong() == id && d.month() == today.month())
            return m;
    }
    return {};
}

void TestLimitAlarms::requestDesktopView()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 before = desktopView().value("revision").toLongLong();
    emit m_proxy->alarmViewRequested(QStringLiteral("desktop"), double(nowMs - 3600 * 1000), double(nowMs + 3600 * 1000), 1);
    QTRY_VERIFY_WITH_TIMEOUT(desktopView().value("revision").toLongLong() > before
                             || desktopView().value("state").toString() == "ready", 10000);
}

qint64 TestLimitAlarms::seed(const QDateTime &time, const QJsonObject &reason)
{
    qint64 id = -1;
    QString error;
    if (!m_sql->insertAlarm(time, json(reason), &error, &id))
        qFatal("seed failed: %s", qPrintable(error));
    return id;
}

void TestLimitAlarms::initTestCase()
{
    QVERIFY(m_dir.isValid());
    QVERIFY(QDir::setCurrent(m_dir.path()));   // TaidaFlowSettings.ini of Manager goes here
    g_previousHandler = qInstallMessageHandler(captureMessages);
    m_sql = SqlManager::instance();
    m_sql->setDataDirectory(m_dir.filePath(QStringLiteral("data")));
    m_sql->setSettingsFile(m_dir.filePath(QStringLiteral("settings.sqlite")));
    QVERIFY(m_sql->initialize());
}

void TestLimitAlarms::cleanupTestCase()
{
    SqlManager::instance()->shutdown();
    qInstallMessageHandler(g_previousHandler);
}

void TestLimitAlarms::init()
{
    m_now = 100000;
    m_baseline.clear();
    for (const DbRow &r : allRows())
        m_baseline.insert(rowKey(r));
    m_proxy = std::make_unique<TaidaFlowProxy>();
    m_views = std::make_unique<AlarmViewService>(m_proxy.get(), m_sql, AlarmViewService::Options{});
    m_monitor = makeMonitor();
}

void TestLimitAlarms::cleanup()
{
    m_monitor.reset();
    m_views.reset();
    m_proxy.reset();
    resolveAllOpen();
    QCoreApplication::processEvents();
}

// ---- tests ------------------------------------------------------------------------------------

void TestLimitAlarms::upperAndLowerInsertAtOnce()
{
    requestDesktopView();
    const qint64 activeBefore = desktopView().value("activeCount").toLongLong();
    setLimits("pt04", true, 50, true, 100);
    setLimits("tt01", true, 20, true, 80);
    const int before = rows().size();

    setPv("pt04", 150.0);
    setPv("tt01", 12.5);

    const QList<DbRow> pt04 = rowsOf("PT-04");
    const QList<DbRow> tt01 = rowsOf("TT-01");
    QCOMPARE(rows().size(), before + 2);
    QCOMPARE(pt04.size(), 1);
    QCOMPARE(tt01.size(), 1);
    QCOMPARE(pt04.first().message(), QStringLiteral("超過上限：150.00 kPa（上限 100.00 kPa）"));
    QCOMPARE(pt04.first().status(), QStringLiteral("警告"));
    QCOMPARE(pt04.first().reason.value("limit").toString(), QStringLiteral("upper"));
    QCOMPARE(pt04.first().reason.value("sensorKey").toString(), QStringLiteral("pt04"));
    QCOMPARE(tt01.first().message(), QStringLiteral("低於下限：12.50 °C（下限 20.00 °C）"));
    QCOMPARE(tt01.first().reason.value("limit").toString(), QStringLiteral("lower"));
    QVERIFY(!pt04.first().resolved());

    // Same violation again (every poll): no second row.
    for (int i = 0; i < 5; ++i) {
        m_now += 1000;
        setPv("pt04", 150.0 + i);
        setPv("tt01", 12.0 - i);
    }
    QCOMPARE(rows().size(), before + 2);

    // alarmRecords (Core's conversion): 未處理 / 警告, sensorName = screen name, equipment 系統.
    const QVariantMap rec = recordOf(pt04.first().id);
    QCOMPARE(rec.value("sensorName").toString(), QStringLiteral("PT-04"));
    QCOMPARE(rec.value("equipment").toString(), QStringLiteral("系統"));
    QCOMPARE(rec.value("alarmMessage").toString(), QStringLiteral("超過上限：150.00 kPa（上限 100.00 kPa）"));
    QCOMPARE(rec.value("severity").toString(), QStringLiteral("警告"));
    QCOMPARE(rec.value("alarmStatus").toString(), QStringLiteral("未處理"));
    QCOMPARE(recordOf(tt01.first().id).value("alarmStatus").toString(), QStringLiteral("未處理"));
    QCOMPARE(recordOf(tt01.first().id).value("severity").toString(), QStringLiteral("警告"));
    // alarmViews: updated by itself (SqlManager::alarmHistoryChanged), 2 more 未處理.
    QTRY_COMPARE_WITH_TIMEOUT(desktopView().value("activeCount").toLongLong(), activeBefore + 2, 10000);
    const QVariantList viewRows = desktopView().value("rows").toList();
    QVERIFY(!viewRows.isEmpty());
    QCOMPARE(viewRows.first().toMap().value("severity").toString(), QStringLiteral("警告"));
    QCOMPARE(m_monitor->activeAlarms().size(), 2);
}

void TestLimitAlarms::equalIsNormal()
{
    setLimits("pt01", true, 10, true, 100);
    setLimits("flowMeter", true, 5, true, 60);
    const int before = rows().size();
    setPv("pt01", 100.0);            // == upper
    setPv("flowMeter", 5.0);         // == lower
    QCOMPARE(rows().size(), before);
    setPv("pt01", 10.0);             // == lower
    setPv("flowMeter", 60.0);        // == upper
    QCOMPARE(rows().size(), before);
    setLimits("pt01", true, 10, true, 100, 5.0);
    setPv("pt01", 95.0);             // 95 + 5 == upper
    QCOMPARE(rows().size(), before);
    QCOMPARE(LimitAlarmMonitor::limitState(100.0, m_proxy->sensorSettingsSv().value("pt01").toMap()), 0);
    // Just over: raised.
    setPv("pt01", 95.000001);
    QCOMPARE(rows().size(), before + 1);
    QCOMPARE(rowsOf("PT-01").size(), 1);
}

void TestLimitAlarms::disabledNotJudged()
{
    setLimits("pt02", false, 50, false, 10);       // values would violate both if enabled
    setLimits("tt04", false, 30, true, 90);         // only upper enabled
    const int before = rows().size();
    setPv("pt02", 30.0);
    setPv("tt04", 5.0);                             // below the disabled lower
    QCOMPARE(rows().size(), before);
    QCOMPARE(m_monitor->activeAlarms().size(), 0);
    setPv("tt04", 95.0);                            // above the enabled upper
    QCOMPARE(rows().size(), before + 1);
    QCOMPARE(rowsOf("TT-04").first().message(), QStringLiteral("超過上限：95.00 °C（上限 90.00 °C）"));
}

void TestLimitAlarms::resolveAfterTwoSeconds()
{
    requestDesktopView();
    setLimits("pt06", true, 10, true, 200);
    setPv("pt06", 250.0);
    const QList<DbRow> raised = rowsOf("PT-06");
    QCOMPARE(raised.size(), 1);
    const qint64 id = raised.first().id;
    QTRY_VERIFY_WITH_TIMEOUT(desktopView().value("activeCount").toLongLong() >= 1, 10000);
    const qint64 activeWhileOpen = desktopView().value("activeCount").toLongLong();

    // Back in range at t0; 1.9 s later over the limit again: same row, not resolved, no new row.
    const qint64 t0 = m_now;
    setPv("pt06", 150.0);
    QVERIFY(m_monitor->timerActive());
    QVERIFY(m_monitor->timerRemainingMs() <= 2000);
    m_now = t0 + 1900;
    m_monitor->processDue();
    QVERIFY(!rowById(id, QDate::currentDate()).resolved());
    setPv("pt06", 201.0);
    QCOMPARE(rowsOf("PT-06").size(), 1);
    QVERIFY(!rowById(id, QDate::currentDate()).resolved());
    QCOMPARE(logCount(QStringLiteral("again (201.00 kPa, 上限 200.00 kPa) 1900 ms after it was back in range: id=%1").arg(id)), 1);

    // Back in range again at t1: resolved only after a full 2000 ms from t1.
    m_now = t0 + 2500;
    m_monitor->processDue();                        // still violated: nothing
    QVERIFY(!rowById(id, QDate::currentDate()).resolved());
    const qint64 t1 = m_now;
    setPv("pt06", 199.99);
    m_now = t1 + 1999;
    m_monitor->processDue();
    setPv("pt06", 180.0);                           // PV updates in range do not shorten the time
    QVERIFY(!rowById(id, QDate::currentDate()).resolved());
    m_now = t1 + 2000;
    m_monitor->processDue();
    const DbRow resolved = rowById(id, QDate::currentDate());
    QVERIFY(resolved.resolved());
    QCOMPARE(resolved.message(), QStringLiteral("超過上限：250.00 kPa（上限 200.00 kPa）"));
    QCOMPARE(resolved.status(), QStringLiteral("警告"));
    QCOMPARE(resolved.reason.value("resolvedDetail").toString(),
             QStringLiteral("超過上限 解除（PT-06=180.00 kPa，上限 200.00 kPa，回到範圍內 2 秒）"));
    QVERIFY(resolved.reason.value("resolvedAt").toInteger() > 0);
    QCOMPARE(rowsOf("PT-06").size(), 1);
    QVERIFY(!m_monitor->timerActive());
    QCOMPARE(m_monitor->activeAlarms().size(), 0);

    // alarmRecords: 已解除, severity still 警告; alarmViews: one 未處理 less.
    QCOMPARE(recordOf(id).value("alarmStatus").toString(), QStringLiteral("已解除"));
    QCOMPARE(recordOf(id).value("severity").toString(), QStringLiteral("警告"));
    QTRY_COMPARE_WITH_TIMEOUT(desktopView().value("activeCount").toLongLong(), activeWhileOpen - 1, 10000);

    // Over the limit again later: a new row (the old one is resolved).
    setPv("pt06", 300.0);
    QCOMPARE(rowsOf("PT-06").size(), 2);
}

void TestLimitAlarms::realClockTimerResolves()
{
    // A real monotonic clock (makeMonitor injects the test clock only when none is given).
    QElapsedTimer real;
    real.start();
    LimitAlarmMonitor::Options o;
    o.clock = [&real]() { return real.elapsed(); };
    m_monitor = makeMonitor(o);

    setLimits("pt07", false, 0, true, 100);
    setPv("pt07", 120.0);
    const qint64 id = rowsOf("PT-07").first().id;
    QElapsedTimer sinceBack;
    setPv("pt07", 90.0);                         // back in range once; no further PV update
    sinceBack.start();
    QVERIFY(m_monitor->timerActive());
    QTRY_VERIFY_WITH_TIMEOUT(rowById(id, QDate::currentDate()).resolved(), 5000);
    const qint64 elapsed = sinceBack.elapsed();
    qInfo().noquote() << QStringLiteral("[w2-085 test] resolved %1 ms after the value went back in range (no PV update)").arg(elapsed);
    QVERIFY2(elapsed >= 1990 && elapsed < 2700, qPrintable(QString::number(elapsed)));
}

void TestLimitAlarms::groupSensorsSeparately()
{
    // The settings page writes the same limits to both keys of a group.
    setLimits("pt04", true, 20, true, 100, 1.0);
    setLimits("pt05", true, 20, true, 100, -1.0);
    setPv("pt04", 120.0);
    setPv("pt05", 60.0);
    QCOMPARE(rowsOf("PT-04").size(), 1);
    QCOMPARE(rowsOf("PT-05").size(), 0);
    setPv("pt05", 130.0);
    QCOMPARE(rowsOf("PT-05").size(), 1);
    QCOMPARE(rowsOf("PT-05").first().message(), QStringLiteral("超過上限：129.00 kPa（上限 100.00 kPa）"));
    const qint64 id4 = rowsOf("PT-04").first().id;
    const qint64 id5 = rowsOf("PT-05").first().id;
    QVERIFY(id4 != id5);

    setPv("pt04", 50.0);                          // PT-04 back, PT-05 still over
    m_now += 2000;
    m_monitor->processDue();
    QVERIFY(rowById(id4, QDate::currentDate()).resolved());
    QVERIFY(!rowById(id5, QDate::currentDate()).resolved());
    // PT-05 drops below the lower limit: its upper alarm counts down, its lower alarm is a new row.
    setPv("pt05", 10.0);
    QCOMPARE(rowsOf("PT-05").size(), 2);
    QCOMPARE(rowsOf("PT-05").last().message(), QStringLiteral("低於下限：9.00 kPa（下限 20.00 kPa）"));
    m_now += 2000;
    m_monitor->processDue();
    QVERIFY(rowById(id5, QDate::currentDate()).resolved());
    QVERIFY(!rowsOf("PT-05").last().resolved());
    QCOMPARE(m_monitor->activeAlarms().size(), 1);
    QCOMPARE(m_monitor->activeAlarms().first().key, QStringLiteral("pt05"));
    QCOMPARE(m_monitor->activeAlarms().first().limit, Limit::Lower);
}

void TestLimitAlarms::filterUsesCorrectedDifference()
{
    setLimits("pt02", false, 0, false, 0, 10.0);    // PT-02 offset +10
    setLimits("pt03", false, 0, false, 0, -5.0);    // PT-03 offset -5
    setLimits("filter", true, 20, true, 60);

    setPv("pt02", 300.0);                           // only PT-02 known: filter not judged yet
    QCOMPARE(rowsOf("Filter 壓差").size(), 0);
    setPv("pt03", 250.0);                           // raw difference 50 (normal), corrected 310 - 245 = 65
    QCOMPARE(rowsOf("Filter 壓差").size(), 1);
    QCOMPARE(rowsOf("Filter 壓差").first().message(), QStringLiteral("超過上限：65.00 kPa（上限 60.00 kPa）"));
    QCOMPARE(rowsOf("Filter 壓差").first().reason.value("sensorKey").toString(), QStringLiteral("filter"));
    QCOMPARE(rowsOf("PT-02").size(), 0);            // PT-02 / PT-03 have no limits enabled
    QCOMPARE(m_proxy->pt02ValuePv(), 300.0);        // PVs stay raw
    QCOMPARE(m_proxy->pt03ValuePv(), 250.0);

    // Both PVs of one Modbus reply are evaluated together (queued): a step that is normal before and
    // after never raises, although "new PT-02 with old PT-03" (410 - 255 = 155) would be over 60.
    const qint64 filterId = rowsOf("Filter 壓差").first().id;
    setPv("pt03", 260.0);                           // 310 - 255 = 55: in range
    m_now += 2000;
    m_monitor->processDue();
    QVERIFY(rowById(filterId, QDate::currentDate()).resolved());
    setPv("pt02", 400.0, false);
    setPv("pt03", 360.0, false);                    // same reply: 410 - 355 = 55
    QCoreApplication::processEvents();              // the queued evaluation (as in the app)
    m_monitor->flushPendingUpdates();               // nothing left
    QCOMPARE(rowsOf("Filter 壓差").size(), 1);

    // Below the lower limit (difference 410 - 395 = 15 < 20).
    setPv("pt03", 400.0);
    QCOMPARE(rowsOf("Filter 壓差").size(), 2);
    QCOMPARE(rowsOf("Filter 壓差").last().message(), QStringLiteral("低於下限：15.00 kPa（下限 20.00 kPa）"));
}

void TestLimitAlarms::offsetApplied()
{
    setLimits("pt01", false, 0, true, 100, 10.0);
    setPv("pt01", 95.0);                            // corrected 105
    QCOMPARE(rowsOf("PT-01").size(), 1);
    QCOMPARE(rowsOf("PT-01").first().message(), QStringLiteral("超過上限：105.00 kPa（上限 100.00 kPa）"));
    QCOMPARE(m_proxy->pt01ValuePv(), 95.0);         // raw
    const qint64 id = rowsOf("PT-01").first().id;
    setOffset("pt01", 0.0);                         // settings change only: 95 now in range
    QVERIFY(m_monitor->timerActive());
    m_now += 2000;
    m_monitor->processDue();
    QVERIFY(rowById(id, QDate::currentDate()).resolved());
    // Negative offset below a lower limit.
    setLimits("tt02", true, 30, false, 0, -10.0);
    setPv("tt02", 35.0);                            // corrected 25
    QCOMPARE(rowsOf("TT-02").size(), 1);
    QCOMPARE(rowsOf("TT-02").first().message(), QStringLiteral("低於下限：25.00 °C（下限 30.00 °C）"));
}

void TestLimitAlarms::settingsChangeReevaluates()
{
    setLimits("tt03", false, 0, true, 80);
    setPv("tt03", 50.0);
    QCOMPARE(rowsOf("TT-03").size(), 0);
    // Stricter upper limit: raised at once without a PV update.
    setLimits("tt03", false, 0, true, 40);
    QCOMPARE(rowsOf("TT-03").size(), 1);
    QCOMPARE(rowsOf("TT-03").first().message(), QStringLiteral("超過上限：50.00 °C（上限 40.00 °C）"));
    const qint64 upperId = rowsOf("TT-03").first().id;
    QVERIFY(logCount(QStringLiteral("[LimitAlarm] sensor settings changed: re-evaluating")) >= 1);
    // Disable the upper limit and enable a lower one above the value: lower raised at once, upper
    // resolved 2 s later (a disabled limit counts as in range).
    setLimits("tt03", true, 60, false, 40);
    QCOMPARE(rowsOf("TT-03").size(), 2);
    QCOMPARE(rowsOf("TT-03").last().message(), QStringLiteral("低於下限：50.00 °C（下限 60.00 °C）"));
    QVERIFY(!rowById(upperId, QDate::currentDate()).resolved());
    m_now += 1999;
    m_monitor->processDue();
    QVERIFY(!rowById(upperId, QDate::currentDate()).resolved());
    m_now += 1;
    m_monitor->processDue();
    const DbRow up = rowById(upperId, QDate::currentDate());
    QVERIFY(up.resolved());
    QCOMPARE(up.reason.value("resolvedDetail").toString(), QStringLiteral("超過上限 解除（上限已停用，TT-03=50.00 °C）"));
    QVERIFY(!rowsOf("TT-03").last().resolved());
    // Disabling everything: the lower alarm is resolved 2 s later.
    setLimits("tt03", false, 60, false, 40);
    m_now += 2000;
    m_monitor->processDue();
    QVERIFY(rowsOf("TT-03").last().resolved());
    QCOMPARE(rowsOf("TT-03").size(), 2);
    // A sensor that never had a PV in this run is not judged by a settings change.
    setLimits("tt04", true, 10, true, 20);          // tt04 PV is 0 but was never written
    QCOMPARE(rowsOf("TT-04").size(), 0);
}

void TestLimitAlarms::restartNoDuplicate()
{
    const int firstValueLogs = logCount(QStringLiteral("[LimitAlarm][Restart] PT-04 first value after start"));
    // Run 1: PT-04 over the upper limit, TT-01 below the lower limit, then the program stops.
    setLimits("pt04", true, 50, true, 100);
    setLimits("tt01", true, 20, true, 80);
    setPv("pt04", 150.0);
    setPv("tt01", 10.0);
    const qint64 pt04Id = rowsOf("PT-04").last().id;
    const qint64 tt01Id = rowsOf("TT-01").last().id;
    // Older duplicates left open by earlier runs (one in last month's file), rows of other kinds.
    const QDateTime now = QDateTime::currentDateTime();
    const QJsonObject dup{{"sensor", "PT-04"}, {"alarmMessage", "超過上限：140.00 kPa（上限 100.00 kPa）"},
                          {"status", "警告"}, {"limit", "upper"}, {"sensorKey", "pt04"}};
    const qint64 dupThisMonth = seed(now.addSecs(-120), dup);
    const QDateTime lastMonth = now.addMonths(-1);
    const qint64 dupLastMonth = seed(lastMonth, dup);
    const qint64 highAlarm = seed(now.addSecs(-60), QJsonObject{{"sensor", "PT-04"},
            {"alarmMessage", "輸入值達到設定高限 90.0%（raw=65535，threshold=58982）"}, {"status", "數值異常"}});
    const qint64 msWarning = seed(now.addSecs(-60), QJsonObject{{"sensor", "MS300"}, {"alarmMessage", "warn"}, {"status", "警告"}});
    // PT-02 had an upper alarm in an earlier run and is in range now.
    const qint64 pt02Old = seed(now.addSecs(-30), QJsonObject{{"sensor", "PT-02"},
            {"alarmMessage", "超過上限：70.00 kPa（上限 60.00 kPa）"}, {"status", "警告"}, {"limit", "upper"}, {"sensorKey", "pt02"}});
    // A row without "limit" (older format) is classified by its message.
    const qint64 pt03Text = seed(now.addSecs(-30), QJsonObject{{"sensor", "PT-03"},
            {"alarmMessage", "低於下限：1.00 kPa（下限 5.00 kPa）"}, {"status", "警告"}});
    m_monitor.reset();
    const int rowsBefore = rows().size();

    // Run 2 with the same settings (they are restored from the INI by Core before Manager starts).
    m_monitor = makeMonitor();
    setLimits("pt02", false, 0, true, 60);
    setLimits("pt03", true, 5, false, 0);
    setPv("pt04", 151.0);                           // still over
    setPv("tt01", 25.0);                            // recovered
    setPv("pt02", 30.0);                            // recovered
    setPv("pt03", 0.5);                             // still under
    QCOMPARE(rows().size(), rowsBefore);            // no new row at all
    QCOMPARE(logCount(QStringLiteral("[LimitAlarm][Restart] PT-04 超過上限: took over id=%1").arg(pt04Id)), 1);
    QCOMPARE(logCount(QStringLiteral("[LimitAlarm][Restart] PT-03 低於下限: took over id=%1").arg(pt03Text)), 1);
    // Older PT-04 duplicates resolved at once (both files), the taken-over row stays open.
    QVERIFY(rowById(dupThisMonth, now.date()).resolved());
    QCOMPARE(rowById(dupThisMonth, now.date()).reason.value("resolvedDetail").toString(),
             QStringLiteral("超過上限 解除（重啟後由 id=%1 接手）").arg(pt04Id));
    QVERIFY(rowById(dupLastMonth, lastMonth.date()).resolved());
    QVERIFY(!rowById(pt04Id, now.date()).resolved());
    QVERIFY(!rowById(highAlarm, now.date()).resolved());     // 90 % alarm row untouched
    QVERIFY(!rowById(msWarning, now.date()).resolved());     // MS300 警告 untouched
    // Recovered sensors are resolved 2 s later, not at once.
    QVERIFY(!rowById(tt01Id, now.date()).resolved());
    QVERIFY(!rowById(pt02Old, now.date()).resolved());
    m_now += 2000;
    m_monitor->processDue();
    QVERIFY(rowById(tt01Id, now.date()).resolved());
    QVERIFY(rowById(pt02Old, now.date()).resolved());
    QVERIFY(!rowById(pt04Id, now.date()).resolved());
    QVERIFY(!rowById(pt03Text, now.date()).resolved());
    QCOMPARE(rows().size(), rowsBefore);
    // The taken-over row behaves like one of this run.
    setPv("pt04", 80.0);
    m_now += 2000;
    m_monitor->processDue();
    QVERIFY(rowById(pt04Id, now.date()).resolved());
    QCOMPARE(rows().size(), rowsBefore);
    QCOMPARE(logCount(QStringLiteral("[LimitAlarm][Restart] PT-04 first value after start")), firstValueLogs + 2);  // run 1 and 2
}

// Holds an exclusive lock on a month file (readers are blocked too) until destroyed.
class ExclusiveLock
{
public:
    ExclusiveLock(const QString &file, const QString &name) : m_name(name)
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
        db.setDatabaseName(file);
        db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=0"));
        if (db.open()) {
            QSqlQuery q(db);
            m_locked = q.exec(QStringLiteral("BEGIN EXCLUSIVE"));
            if (!m_locked)
                m_error = q.lastError().text();
        } else {
            m_error = db.lastError().text();
        }
    }
    ~ExclusiveLock() { release(); }
    bool locked() const { return m_locked; }
    QString error() const { return m_error; }
    void release()
    {
        if (m_released)
            return;
        m_released = true;
        {
            QSqlDatabase db = QSqlDatabase::database(m_name, false);
            if (db.isOpen()) {
                QSqlQuery q(db);
                if (m_locked)
                    q.exec(QStringLiteral("ROLLBACK"));
                q.finish();
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(m_name);
    }

private:
    QString m_name;
    QString m_error;
    bool m_locked = false;
    bool m_released = false;
};

void TestLimitAlarms::restartLookupFailure()
{
    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime lastMonth = now.addMonths(-1);
    const qint64 old = seed(now.addSecs(-30), QJsonObject{{"sensor", "PT-06"},
            {"alarmMessage", "超過上限：250.00 kPa（上限 200.00 kPa）"}, {"status", "警告"}, {"limit", "upper"}, {"sensorKey", "pt06"}});
    seed(lastMonth, QJsonObject{{"sensor", "100"}, {"alarmMessage", "設備啟動"}, {"status", "正常"}});   // last month's file exists
    setLimits("pt06", false, 0, true, 200);
    setLimits("pt07", false, 0, true, 200);
    const int before = rows().size();
    const QString lockFile = m_sql->dataFilePathForMonth(lastMonth.date());

    // Last month's file locked: the lookup (this + last month) fails after SQLite's busy timeout.
    // Nothing is read from the database while it is locked; the checks follow after the release.
    const int insertedBefore = logCount(QStringLiteral("[SQL] Alarm inserted"));
    int failedLogs = 0;
    int deferredLogs = 0;
    int activeWhileLocked = -1;
    bool retryTimer = false;
    {
        ExclusiveLock lock(lockFile, QStringLiteral("w2085_lock_%1").arg(++m_lockSerial));
        QVERIFY2(lock.locked(), qPrintable(lock.error()));
        setPv("pt06", 250.0, false);
        setPv("pt07", 250.0, false);
        m_monitor->flushPendingUpdates();               // PT-06 lookup fails, PT-07 deferred
        failedLogs = logCount(QStringLiteral("[LimitAlarm][Restart] PT-06 lookup of unresolved rows from earlier runs failed (attempt 1/5"));
        deferredLogs = logCount(QStringLiteral("[LimitAlarm][Restart] PT-07 lookup deferred"));
        activeWhileLocked = m_monitor->activeAlarms().size();
        retryTimer = m_monitor->timerActive();
    }
    QCOMPARE(failedLogs, 1);
    QCOMPARE(deferredLogs, 1);
    QCOMPARE(activeWhileLocked, 0);
    QVERIFY(retryTimer);                                // retried
    QCOMPARE(logCount(QStringLiteral("[SQL] Alarm inserted")), insertedBefore);
    QCOMPARE(rows().size(), before);                    // no new row while the lookup was not done

    m_now += 600;
    setPv("pt06", 250.0, false);
    setPv("pt07", 250.0, false);
    m_monitor->flushPendingUpdates();
    QCOMPARE(logCount(QStringLiteral("[LimitAlarm][Restart] PT-06 超過上限: took over id=%1").arg(old)), 1);
    QCOMPARE(rowsOf("PT-06").size(), 1);                // taken over, no new row
    QCOMPARE(rowsOf("PT-07").size(), 1);                // normal logic: a new row
    QCOMPARE(rows().size(), before + 1);

    // The last attempt fails -> the normal logic runs (a new row while violated), like the DIs.
    m_monitor.reset();
    resolveAllOpen();
    const qint64 old2 = seed(now.addSecs(-20), QJsonObject{{"sensor", "PT-06"},
            {"alarmMessage", "超過上限：250.00 kPa（上限 200.00 kPa）"}, {"status", "警告"}, {"limit", "upper"}, {"sensorKey", "pt06"}});
    LimitAlarmMonitor::Options o;
    o.restartLookupMaxAttempts = 1;
    m_monitor = makeMonitor(o);
    // The alarmRecords reload (Core's 3-day read) would wait for the locked file itself; the time
    // measured below is the monitor's own.
    disconnect(m_monitor.get(), nullptr, this, nullptr);
    const int before2 = rows().size();
    int giveUpLogs = 0;
    int sharedLogs = 0;
    qint64 pt07Ms = -1;
    {
        ExclusiveLock lock(lockFile, QStringLiteral("w2085_lock_%1").arg(++m_lockSerial));
        QVERIFY2(lock.locked(), qPrintable(lock.error()));
        setPv("pt06", 250.0);                           // insert goes to this month's file (not locked)
        giveUpLogs = logCount(QStringLiteral("[LimitAlarm][Restart] PT-06 lookup failed 1 times"));
        // The attempts are shared: PT-07 does not wait for the locked file again.
        QElapsedTimer pt07;
        pt07.start();
        m_now += 600;
        setPv("pt07", 250.0);
        pt07Ms = pt07.elapsed();
        sharedLogs = logCount(QStringLiteral("[LimitAlarm][Restart] PT-07: restart lookups were given up after 1 failures"));
    }
    QCOMPARE(giveUpLogs, 1);
    QCOMPARE(sharedLogs, 1);
    QVERIFY2(pt07Ms < 1000, qPrintable(QString::number(pt07Ms)));
    QCOMPARE(rows().size(), before2 + 2);               // PT-06 and PT-07, normal logic
    QCOMPARE(rowsOf("PT-07").size(), 2);                // the first part's row (resolved) + the new one
    QVERIFY(!rowsOf("PT-07").last().resolved());
    QCOMPARE(rowsOf("PT-06").last().message(), QStringLiteral("超過上限：250.00 kPa（上限 200.00 kPa）"));
    QVERIFY(!rowsOf("PT-06").last().resolved());
    QVERIFY(!rowById(old2, now.date()).resolved());     // left as it is
}

void TestLimitAlarms::uiRuleEquivalence()
{
    // The UI's own code, read from the sources: SensorUnits.adjusted() and SensorUnits.limitState(),
    // the one rule that filterBody.limitState (and, since w1-088, every value tag) calls.
    QFile unitsFile(QStringLiteral(CORE_SOURCE_DIR "/../TaidaFlowContent/components/SensorUnits.js"));
    QVERIFY(unitsFile.open(QIODevice::ReadOnly));
    QString units = QString::fromUtf8(unitsFile.readAll());
    units.remove(QStringLiteral(".pragma library"));
    QFile mainFile(QStringLiteral(CORE_SOURCE_DIR "/../TaidaFlowContent/Main.qml"));
    QVERIFY(mainFile.open(QIODevice::ReadOnly));
    const QString main = QString::fromUtf8(mainFile.readAll());
    const QRegularExpression re(QStringLiteral("readonly property int limitState:\\s*SensorUnits\\.(limitState\\(deltaKpa, limits\\))"),
                                QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch m = re.match(main);
    QVERIFY2(m.hasMatch(), "limitState expression not found in Main.qml");
    const QString expr = m.captured(1).simplified();
    qInfo().noquote() << "[w2-085 test] Main.qml limitState:" << expr;
    QVERIFY(main.contains(QStringLiteral("SensorUnits.adjusted(Td.pt02ValuePv, \"pt02\", Td.sensorSettingsSv)")));
    QVERIFY(main.contains(QStringLiteral("- SensorUnits.adjusted(Td.pt03ValuePv, \"pt03\", Td.sensorSettingsSv)")));

    QJSEngine engine;
    QJSValue result = engine.evaluate(units);
    QVERIFY2(!result.isError(), qPrintable(result.toString()));
    result = engine.evaluate(QStringLiteral("function uiLimitState(limits, deltaKpa) { return %1; }").arg(expr));
    QVERIFY2(!result.isError(), qPrintable(result.toString()));
    QJSValue adjusted = engine.globalObject().property("adjusted");
    QJSValue uiLimitState = engine.globalObject().property("uiLimitState");
    QVERIFY(adjusted.isCallable() && uiLimitState.isCallable());

    TaidaFlowProxy proxy;
    const QList<double> raws{-5.0, 0.0, 9.9, 10.0, 10.1, 14.5, 15.0, 19.99, 20.0, 20.01, 25.0, 100.0};
    const QList<double> offsets{-2.5, 0.0, 0.1, 5.0};
    const QList<QPair<double, double>> limits{{10.0, 20.0}, {15.0, 15.0}, {-3.0, 12.5}};
    int compared = 0;
    int nonZero = 0;
    for (const QString &key : LimitAlarmMonitor::sensorKeys()) {
        for (const auto &lim : limits) {
            for (int flags = 0; flags < 4; ++flags) {
                for (double offset : offsets) {
                    QVariantMap settings = proxy.sensorSettingsSv();
                    const auto entry = [&](double off) {
                        return QVariantMap{{"offset", off}, {"lower", lim.first}, {"upper", lim.second},
                                           {"lowerEnabled", bool(flags & 1)}, {"upperEnabled", bool(flags & 2)}};
                    };
                    settings.insert(key, entry(key == "filter" ? 0.0 : offset));
                    if (key == "filter") {     // filter: PT-02 / PT-03 offsets count
                        settings.insert("pt02", entry(offset));
                        settings.insert("pt03", entry(-offset / 2));
                    }
                    proxy.setSensorSettingsSv(settings);
                    QCOMPARE(proxy.sensorSettingsSv(), settings);
                    for (double raw : raws) {
                        double uiValue = 0.0;
                        const QJSValue jsSettings = engine.toScriptValue(settings);
                        if (key == "filter") {
                            proxy.setPt02ValuePv(raw);
                            proxy.setPt03ValuePv(raw / 3.0);
                            uiValue = adjusted.call({raw, "pt02", jsSettings}).toNumber()
                                    - adjusted.call({raw / 3.0, "pt03", jsSettings}).toNumber();
                        } else {
                            QVERIFY(proxy.setProperty((key + QStringLiteral("ValuePv")).toLatin1().constData(), raw));
                            uiValue = adjusted.call({raw, key, jsSettings}).toNumber();
                        }
                        double cppValue = 0.0;
                        QVERIFY(LimitAlarmMonitor::correctedValue(proxy, key, &cppValue));
                        QCOMPARE(cppValue, uiValue);
                        const int ui = uiLimitState.call({jsSettings.property(key), uiValue}).toInt();
                        const int cpp = LimitAlarmMonitor::limitState(cppValue, settings.value(key).toMap());
                        if (ui != cpp)
                            QFAIL(qPrintable(QStringLiteral("%1 raw=%2 offset=%3 flags=%4 limits=%5..%6: ui=%7 cpp=%8")
                                                     .arg(key).arg(raw).arg(offset).arg(flags).arg(lim.first)
                                                     .arg(lim.second).arg(ui).arg(cpp)));
                        ++compared;
                        nonZero += cpp != 0 ? 1 : 0;
                    }
                }
            }
        }
    }
    qInfo().noquote() << QStringLiteral("[w2-085 test] UI rule vs limitState(): %1 cases equal (%2 violated, %3 normal)")
                                 .arg(compared).arg(nonZero).arg(compared - nonZero);
    QCOMPARE(compared, 13 * 3 * 4 * 4 * 12);
    QVERIFY(nonZero > 500);
}

void TestLimitAlarms::managerIntegration()
{
    m_monitor.reset();      // the Manager owns its own monitor
    // Modbus TCP servers of this test: A = ADAM-6217 .202 (TT-01..04, PT-01..04 in HR0..7),
    // B = everything else (.203 PT-05..07 / flow / MV; .201 coils; .204 DIs / AOs; .205 HR10).
    QModbusTcpServer serverA;
    QModbusTcpServer serverB;
    QModbusDataUnitMap map;
    map.insert(QModbusDataUnit::Coils, {QModbusDataUnit::Coils, 0, 64});
    map.insert(QModbusDataUnit::DiscreteInputs, {QModbusDataUnit::DiscreteInputs, 0, 16});
    map.insert(QModbusDataUnit::HoldingRegisters, {QModbusDataUnit::HoldingRegisters, 0, 32});
    map.insert(QModbusDataUnit::InputRegisters, {QModbusDataUnit::InputRegisters, 0, 16});
    quint16 portA = 0;
    quint16 portB = 0;
    freePorts(&portA, &portB);
    QVERIFY(portA != 0 && portB != 0 && portA != portB);
    for (auto [server, port] : {std::pair<QModbusTcpServer *, quint16>{&serverA, portA}, {&serverB, portB}}) {
        server->setMap(map);
        server->setServerAddress(1);
        server->setConnectionParameter(QModbusDevice::NetworkAddressParameter, QStringLiteral("127.0.0.1"));
        server->setConnectionParameter(QModbusDevice::NetworkPortParameter, port);
        QVERIFY2(server->connectDevice(), qPrintable(server->errorString()));
    }
    serverB.setData(QModbusDataUnit::DiscreteInputs, 0, 1);    // DI0 = 1 (healthy), DI1 = DI2 = 0
    const quint16 raw500 = 32768;                              // x 1000 / 65535 = 500.0076 kPa
    serverA.setData(QModbusDataUnit::HoldingRegisters, 7, raw500);   // PT-04
    serverA.setData(QModbusDataUnit::HoldingRegisters, 6, 65535);    // PT-03 at full scale: 90 % alarm

    Manager::DeviceSettings devices;
    for (ModbusClient::DeviceConfig &config : devices.modbusDevices) {
        config.host = QStringLiteral("127.0.0.1");               // never the plant addresses
        config.port = config.device == ModbusClient::Device::Adam6217_202 ? portA : portB;
    }
    devices.ms300.serialPort = QStringLiteral("TAIDAFLOW_TEST_NO_SUCH_PORT");
    setLimits("pt04", false, 0, true, 400);
    const int before = rows().size();

    Manager manager(m_proxy.get(), m_sql, devices);
    int saved = 0;
    connect(&manager, &Manager::alarmSaved, this, [&saved]() { ++saved; });
    manager.start();
    QTRY_VERIFY_WITH_TIMEOUT(rowsOf("PT-04").size() == 1, 8000);
    const DbRow row = rowsOf("PT-04").first();
    QCOMPARE(row.message(), QStringLiteral("超過上限：500.01 kPa（上限 400.00 kPa）"));
    QCOMPARE(row.status(), QStringLiteral("警告"));
    QVERIFY(saved >= 1);
    QVERIFY(std::abs(m_proxy->pt04ValuePv() - 500.0076) < 0.001);   // the PV itself stays raw
    // The 90 % high alarm (unchanged path) still records PT-03.
    QTRY_VERIFY_WITH_TIMEOUT(!rowsOf("PT-03").isEmpty(), 5000);
    QCOMPARE(rowsOf("PT-03").first().status(), QStringLiteral("數值異常"));
    QVERIFY(rowsOf("PT-03").first().message().startsWith(QStringLiteral("輸入值達到設定高限 90.0%")));

    // Back in range (250 kPa): resolved about 2 s later; further polls write nothing new.
    const int savedBefore = saved;
    serverA.setData(QModbusDataUnit::HoldingRegisters, 7, 16384);
    QElapsedTimer t;
    t.start();
    QTRY_VERIFY_WITH_TIMEOUT(rowById(row.id, QDate::currentDate()).resolved(), 8000);
    qInfo().noquote() << QStringLiteral("[w2-085 test] Manager path: resolved %1 ms after the register changed (poll 1 s + 2 s)").arg(t.elapsed());
    QVERIFY(t.elapsed() >= 2000);
    QVERIFY(saved > savedBefore);
    QTest::qWait(2500);
    QCOMPARE(rowsOf("PT-04").size(), 1);
    manager.stop();
    QCOMPARE(rows().size(), before + 2);            // PT-04 limit + PT-03 90 %
}

void TestLimitAlarms::sourceWiring()
{
    const QString manager = readSource(QStringLiteral("manager.cpp"));
    const QString managerH = readSource(QStringLiteral("manager.h"));
    const QString core = readSource(QStringLiteral("core.cpp"));
    const QString cmake = readSource(QStringLiteral("CMakeLists.txt"));
    QVERIFY(!manager.isEmpty() && !core.isEmpty() && !cmake.isEmpty());
    // Manager creates the monitor with its proxy / SqlManager and forwards alarmSaved.
    QVERIFY(manager.contains(QStringLiteral("m_limitAlarms = new LimitAlarmMonitor(proxy, sql, this);")));
    QVERIFY(manager.contains(QStringLiteral("connect(m_limitAlarms, &LimitAlarmMonitor::alarmSaved, this, &Manager::alarmSaved);")));
    QVERIFY(managerH.contains(QStringLiteral("LimitAlarmMonitor *m_limitAlarms = nullptr;")));
    // updateProcessPoint: the PV setter switch first, then the monitor.
    const int fn = manager.indexOf(QStringLiteral("void Manager::updateProcessPoint("));
    const int sw = manager.indexOf(QStringLiteral("case ModbusMapping::ProcessPoint::Mv4Position: m_proxy->setM4ValuePv(value); break;"), fn);
    const int call = manager.indexOf(QStringLiteral("m_limitAlarms->processPointUpdated(point);"), fn);
    const int next = manager.indexOf(QStringLiteral("void Manager::writeServerData("), fn);
    QVERIFY(fn > 0 && sw > fn && call > sw && call < next);
    QCOMPARE(manager.count(QStringLiteral("m_limitAlarms->processPointUpdated(")), 1);
    // The 90 % alarm is still called from the mirror path.
    QVERIFY(manager.contains(QStringLiteral("checkHighInputAlarm(serverOffset, values.at(index));")));
    // Core reloads alarmRecords on Manager::alarmSaved.
    QVERIFY(core.contains(QStringLiteral("connect(m_manager, &Manager::alarmSaved, this, &Core::loadAlarmRecords);")));
    // Desktop-only source.
    const int desktop = cmake.indexOf(QStringLiteral("if(TAIDAFLOW_IS_WINDOWS_DESKTOP)"));
    const int endif = cmake.indexOf(QStringLiteral("endif()"), desktop);
    const int src = cmake.indexOf(QStringLiteral("LimitAlarms.cpp"));
    QVERIFY(desktop > 0 && src > desktop && src < endif);
    QCOMPARE(cmake.count(QStringLiteral("LimitAlarms.cpp")), 1);
}

QTEST_MAIN(TestLimitAlarms)
#include "tst_limit_alarms.moc"
