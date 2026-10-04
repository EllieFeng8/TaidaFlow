// w2-089 (Mango 2026-10-04): the MS300 fault / warning alarm records are named "循環泵變頻器"
// (what the inverter drives) instead of the model "MS300" - only the new rows; rows stored earlier
// keep "MS300" and are never rewritten.
//
// Real Manager (as Core creates it) with the real SqlManager on a temporary data folder, the real
// AlarmRecordFormat conversion (what Core::loadAlarmRecords -> TaidaFlowProxy::alarmRecords uses)
// and the real AlarmViewService (TaidaFlowProxy::alarmViews, what AlarmPage.qml reads). Manager is
// not started: no device connection, no network port.
//
// Boundary (no serial port on this PC, so the MS300 can never answer here): the fault status is
// put in through the Manager's own Ms300FaultReader child by emitting its faultStatusChanged
// signal - the signal Ms300FaultReader::publishFaultStatus emits after a 0x2100 read - so the code
// under test (Manager's connected handler -> saveAlarm -> alarm_history row -> alarmRecords /
// alarmViews) is the production code. The serial read itself is covered by tst_ms300_offline.
//
//  * newAlarmsNamed: fault code != 0 -> one row, sensor 循環泵變頻器, status 異常 (嚴重 / 未處理);
//    warning only -> 警告; "正常" (0 / 0) -> no row; no new row named MS300; alarmRecords and the
//    desktop alarmViews rows show sensorName 循環泵變頻器.
//  * oldRowsKept: a row stored earlier with sensor "MS300" is byte-identical after the run, still
//    shown as MS300 / 未處理 and counted in activeCount next to the new rows.
//  * noLookupByName: no Core source holds the string literal "MS300" (only "[MS300]" log tags and
//    log texts), 循環泵變頻器 only in manager.cpp (the alarm) and DeviceStatusPublisher.cpp (the
//    banner); the only lookups of stored rows by sensor name (findUnresolvedAlarms) are the DI
//    restart hand-over (Manager, DI names) and the limit alarms (LimitAlarms, sensor screen names),
//    so the rename cannot change how old or new MS300 rows are found / resolved (they never are).
#include "AlarmRecordFormat.h"
#include "AlarmViews.h"
#include "Ms300FaultReader.h"
#include "SqlManager.h"
#include "TaidaFlowProxy.h"
#include "manager.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

namespace {
const QString kNewName = QStringLiteral("循環泵變頻器");
const QString kOldName = QStringLiteral("MS300");

struct Row {
    qint64 id = -1;
    qint64 occurrence = 0;
    QString reason;                       // as stored
    QJsonObject json() const { return QJsonDocument::fromJson(reason.toUtf8()).object(); }
    QString sensor() const { return json().value(QStringLiteral("sensor")).toString(); }
};
} // namespace

class TestMs300AlarmName : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void newAlarmsNamed();
    void oldRowsKept();
    void noLookupByName();

private:
    QList<Row> allRows() const;
    void loadAlarmRecords();                         // Core::loadAlarmRecords' conversion
    QVariantMap recordOf(const QVariantList &list, qint64 id) const;
    QVariantMap desktopView() const { return m_proxy->alarmViews().value(QStringLiteral("desktop")).toMap(); }

    QTemporaryDir m_dir;
    QString m_oldCwd;
    SqlManager *m_sql = nullptr;
    std::unique_ptr<TaidaFlowProxy> m_proxy;
    std::unique_ptr<AlarmViewService> m_views;
    std::unique_ptr<Manager> m_manager;
    Ms300FaultReader *m_reader = nullptr;
    Row m_oldRow;                                    // seeded "MS300" row of an earlier run
    qint64 m_faultId = -1;
    qint64 m_warningId = -1;
};

QList<Row> TestMs300AlarmName::allRows() const
{
    const QDateTime now = QDateTime::currentDateTime();
    QJsonArray history;
    QString error;
    if (!m_sql->getAlarmHistory(now.addDays(-40).toSecsSinceEpoch(), now.addDays(1).toSecsSinceEpoch(), &history, &error))
        qWarning() << "getAlarmHistory failed" << error;
    QList<Row> list;
    for (const QJsonValue &v : std::as_const(history)) {
        const QJsonObject o = v.toObject();
        Row r;
        r.id = static_cast<qint64>(o.value(QStringLiteral("id")).toDouble());
        r.occurrence = static_cast<qint64>(o.value(QStringLiteral("occurrence_time")).toDouble());
        r.reason = o.value(QStringLiteral("reason")).toString();
        list << r;
    }
    return list;
}

void TestMs300AlarmName::loadAlarmRecords()
{
    // Core::loadAlarmRecords (core.cpp), the slot Manager::alarmSaved is connected to.
    const QDateTime now = QDateTime::currentDateTime();
    QJsonArray history;
    QString error;
    QVERIFY2(m_sql->getAlarmHistory(now.addDays(-3).toSecsSinceEpoch(), now.toSecsSinceEpoch(), &history, &error),
             qPrintable(error));
    m_proxy->setAlarmRecords(AlarmRecordFormat::recordsFromHistory(history, nullptr));
}

QVariantMap TestMs300AlarmName::recordOf(const QVariantList &list, qint64 id) const
{
    for (const QVariant &v : list) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("id")).toLongLong() == id)
            return m;
    }
    return {};
}

void TestMs300AlarmName::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_oldCwd = QDir::currentPath();
    QVERIFY(QDir::setCurrent(m_dir.path()));         // TaidaFlowSettings.ini of Manager goes here
    m_sql = SqlManager::instance();
    m_sql->setDataDirectory(m_dir.filePath(QStringLiteral("data")));
    m_sql->setSettingsFile(m_dir.filePath(QStringLiteral("settings.sqlite")));
    QVERIFY(m_sql->initialize());

    // A row of an earlier version (before w2-089): what Manager stored for an MS300 fault then.
    const QJsonObject oldReason{{QStringLiteral("sensor"), kOldName},
                                {QStringLiteral("alarmMessage"), QStringLiteral("Fault 21 (0x15): oL - 變頻器過載")},
                                {QStringLiteral("status"), QStringLiteral("異常")}};
    m_oldRow.reason = QString::fromUtf8(QJsonDocument(oldReason).toJson(QJsonDocument::Compact));
    const QDateTime oldTime = QDateTime::currentDateTime().addSecs(-120);
    QString error;
    QVERIFY2(m_sql->insertAlarm(oldTime, m_oldRow.reason, &error, &m_oldRow.id), qPrintable(error));
    m_oldRow.occurrence = oldTime.toSecsSinceEpoch();

    m_proxy = std::make_unique<TaidaFlowProxy>();
    m_views = std::make_unique<AlarmViewService>(m_proxy.get(), m_sql, AlarmViewService::Options{});
    Manager::DeviceSettings devices;
    for (ModbusClient::DeviceConfig &config : devices.modbusDevices) {
        config.host = QStringLiteral("127.0.0.210");      // never the plant addresses (not connected anyway)
        config.port = 1;
    }
    devices.ms300.serialPort = QStringLiteral("COM239");   // never opened: Manager is not started
    m_manager = std::make_unique<Manager>(m_proxy.get(), m_sql, devices);
    connect(m_manager.get(), &Manager::alarmSaved, this, &TestMs300AlarmName::loadAlarmRecords);
    const QList<Ms300FaultReader *> readers = m_manager->findChildren<Ms300FaultReader *>();
    QCOMPARE(readers.size(), 1);
    m_reader = readers.constFirst();
    QCOMPARE(m_reader->settings().serialPort, QStringLiteral("COM239"));

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    emit m_proxy->alarmViewRequested(QStringLiteral("desktop"), double(nowMs - 3600 * 1000), double(nowMs + 3600 * 1000), 1);
    QTRY_COMPARE_WITH_TIMEOUT(desktopView().value(QStringLiteral("state")).toString(), QStringLiteral("ready"), 10000);
    QCOMPARE(desktopView().value(QStringLiteral("totalCount")).toLongLong(), 1);   // the old row only
}

void TestMs300AlarmName::cleanupTestCase()
{
    m_manager.reset();
    m_views.reset();
    m_proxy.reset();
    SqlManager::instance()->shutdown();
    QDir::setCurrent(m_oldCwd);
}

void TestMs300AlarmName::newAlarmsNamed()
{
    const int before = allRows().size();
    QCOMPARE(before, 1);

    // Fault code 21 (what publishFaultStatus emits for raw 0x0015).
    emit m_reader->faultStatusChanged(21, 0, QStringLiteral("Fault 21 (0x15): oL - 變頻器過載"));
    QList<Row> rows = allRows();
    QCOMPARE(rows.size(), before + 1);
    const Row fault = rows.constLast();
    m_faultId = fault.id;
    QCOMPARE(fault.sensor(), kNewName);
    QCOMPARE(fault.json().value(QStringLiteral("status")).toString(), QStringLiteral("異常"));
    QCOMPARE(fault.json().value(QStringLiteral("alarmMessage")).toString(), QStringLiteral("Fault 21 (0x15): oL - 變頻器過載"));

    // Warning only.
    emit m_reader->faultStatusChanged(0, 5, QStringLiteral("Warning 5 (0x05): test warning"));
    rows = allRows();
    QCOMPARE(rows.size(), before + 2);
    const Row warning = rows.constLast();
    m_warningId = warning.id;
    QCOMPARE(warning.sensor(), kNewName);
    QCOMPARE(warning.json().value(QStringLiteral("status")).toString(), QStringLiteral("警告"));

    // Back to normal: no row (unchanged rule).
    emit m_reader->faultStatusChanged(0, 0, QStringLiteral("正常"));
    rows = allRows();
    QCOMPARE(rows.size(), before + 2);
    int oldNamed = 0;
    for (const Row &r : std::as_const(rows))
        oldNamed += r.sensor() == kOldName ? 1 : 0;
    QCOMPARE(oldNamed, 1);                           // only the seeded row of the earlier run

    // alarmRecords (Core's conversion): sensorName 循環泵變頻器, equipment 系統, 嚴重 / 警告, 未處理.
    const QVariantList records = m_proxy->alarmRecords();
    const QVariantMap recFault = recordOf(records, m_faultId);
    const QVariantMap recWarning = recordOf(records, m_warningId);
    QCOMPARE(recFault.value(QStringLiteral("sensorName")).toString(), kNewName);
    QCOMPARE(recFault.value(QStringLiteral("equipment")).toString(), QStringLiteral("系統"));
    QCOMPARE(recFault.value(QStringLiteral("severity")).toString(), QStringLiteral("嚴重"));
    QCOMPARE(recFault.value(QStringLiteral("alarmStatus")).toString(), QStringLiteral("未處理"));
    QCOMPARE(recWarning.value(QStringLiteral("sensorName")).toString(), kNewName);
    QCOMPARE(recWarning.value(QStringLiteral("severity")).toString(), QStringLiteral("警告"));
    QCOMPARE(recWarning.value(QStringLiteral("alarmStatus")).toString(), QStringLiteral("未處理"));

    // alarmViews (desktop entry, updated by SqlManager::alarmHistoryChanged): the same names.
    QTRY_COMPARE_WITH_TIMEOUT(desktopView().value(QStringLiteral("totalCount")).toLongLong(), 3, 10000);
    const QVariantList viewRows = desktopView().value(QStringLiteral("rows")).toList();
    QCOMPARE(recordOf(viewRows, m_faultId).value(QStringLiteral("sensorName")).toString(), kNewName);
    QCOMPARE(recordOf(viewRows, m_warningId).value(QStringLiteral("sensorName")).toString(), kNewName);
    qInfo().noquote() << QStringLiteral("[w2-089 test] new rows: id %1 %2 / id %3 %4")
                                 .arg(m_faultId).arg(fault.reason).arg(m_warningId).arg(warning.reason);
}

void TestMs300AlarmName::oldRowsKept()
{
    QVERIFY(m_faultId > 0);
    // The earlier row is never rewritten.
    bool found = false;
    for (const Row &r : allRows()) {
        if (r.id == m_oldRow.id && r.occurrence == m_oldRow.occurrence) {
            found = true;
            QCOMPARE(r.reason, m_oldRow.reason);
        }
    }
    QVERIFY(found);
    // Still shown as it was stored (MS300, 未處理, 嚴重) next to the new rows.
    const QVariantMap rec = recordOf(m_proxy->alarmRecords(), m_oldRow.id);
    QCOMPARE(rec.value(QStringLiteral("sensorName")).toString(), kOldName);
    QCOMPARE(rec.value(QStringLiteral("alarmStatus")).toString(), QStringLiteral("未處理"));
    QCOMPARE(rec.value(QStringLiteral("severity")).toString(), QStringLiteral("嚴重"));
    const QVariantMap view = desktopView();
    QCOMPARE(recordOf(view.value(QStringLiteral("rows")).toList(), m_oldRow.id).value(QStringLiteral("sensorName")).toString(), kOldName);
    QCOMPARE(view.value(QStringLiteral("activeCount")).toLongLong(), 3);   // old + fault + warning, all 未處理
}

void TestMs300AlarmName::noLookupByName()
{
    QStringList quotedOld;      // "MS300" as a whole string literal
    QStringList newNameFiles;
    QStringList lookups;        // findUnresolvedAlarms( calls outside SqlManager
    QDirIterator it(QStringLiteral(CORE_SOURCE_DIR), {QStringLiteral("*.cpp"), QStringLiteral("*.h")}, QDir::Files);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(f.readAll());
        const QString name = QFileInfo(path).fileName();
        if (text.contains(QStringLiteral("\"MS300\"")))
            quotedOld << name;
        if (text.contains(kNewName))
            newNameFiles << name;
        if (!name.startsWith(QStringLiteral("SqlManager"))) {
            static const QRegularExpression call(QStringLiteral("findUnresolvedAlarms\\(([^,]+),"));
            for (auto m = call.globalMatch(text); m.hasNext();)
                lookups << name + QLatin1Char(':') + m.next().captured(1).trimmed();
        }
    }
    newNameFiles.sort();
    lookups.sort();
    qInfo().noquote() << "[w2-089 test] \"MS300\" literal in:" << quotedOld << "| 循環泵變頻器 in:" << newNameFiles
                      << "| findUnresolvedAlarms lookups:" << lookups;
    QVERIFY2(quotedOld.isEmpty(), qPrintable(quotedOld.join(QLatin1Char(','))));
    QCOMPARE(newNameFiles, (QStringList{QStringLiteral("DeviceStatusPublisher.cpp"), QStringLiteral("DeviceStatusPublisher.h"),
                                        QStringLiteral("manager.cpp")}));
    QCOMPARE(lookups, (QStringList{QStringLiteral("LimitAlarms.cpp:check.name"), QStringLiteral("manager.cpp:sensor")}));

    // manager.cpp's lookup is the DI restart hand-over: its 'sensor' is a DI name.
    QFile mf(QStringLiteral(CORE_SOURCE_DIR "/manager.cpp"));
    QVERIFY(mf.open(QIODevice::ReadOnly));
    const QString manager = QString::fromUtf8(mf.readAll());
    const qsizetype call = manager.indexOf(QStringLiteral("m_sql->findUnresolvedAlarms("));
    const qsizetype fn = manager.lastIndexOf(QRegularExpression(QStringLiteral("\\n\\w[^\\n]*Manager::\\w+\\(")), call);
    QVERIFY(call > 0 && fn > 0);
    const QString signature = manager.mid(fn + 1, manager.indexOf(QLatin1Char('\n'), fn + 1) - fn - 1);
    qInfo().noquote() << "[w2-089 test] manager.cpp lookup in:" << signature;
    QVERIFY2(signature.contains(QStringLiteral("diOffset")) || signature.contains(QStringLiteral("DigitalInput")),
             qPrintable(signature));
    QVERIFY(manager.contains(QStringLiteral("saveAlarm(QStringLiteral(\"循環泵變頻器\"), message, status);")));
}

QTEST_MAIN(TestMs300AlarmName)
#include "tst_ms300_alarm_name.moc"
