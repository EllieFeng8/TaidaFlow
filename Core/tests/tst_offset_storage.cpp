// w2-086 D1/D1b/D2/D4: settings-page offsets applied when a sample is stored (sensor_data) and to the
// Modbus server input registers (SensorOffsetStorage, Core/SensorOffsetStorage.h).
// Real TaidaFlowProxy, real SqlManager on a temporary data folder; managerIntegration runs the real
// Manager against two Modbus TCP servers of this test on 127.0.0.1 (ports picked by the OS), the
// real ModbusServer (connected like Core::init), the real HistoryViewService, HistoryExportManager
// and RESTManager. No device, no fixed port.
//
//  * columnTable: key <-> sensor_data column / Modbus server register <-> scale, derived from
//    ModbusMapping::defaultReadBindings() (single source), = the History page / CSV conversion;
//  * applyRules (data driven): positive / negative / zero / sub-count offsets, rounding, clamp at 0
//    and 65535, pressure / temperature / flow scales; offsetOf(): missing entry / field, non-finite;
//  * clampWarningRateLimited: one warning per column per interval, the next one reports how many
//    were held back; other columns have their own limiter;
//  * settingsChangeNextSample: the sample after a settings change already uses the new offset;
//    offset back to 0 -> raw; PVs stay raw; MV columns and registers 16..19 never change;
//  * uiEquivalence: stored * scale (the History conversion) == SensorUnits.adjusted() of the UI (its
//    own JavaScript, read from TaidaFlowContent) within half a count, for every column and a grid
//    of raw values and offsets (clamped cases excluded and counted);
//  * managerIntegration: Modbus reply -> Manager -> sensor_data row with the offsets; the same
//    values through the History page (historyViews), the CSV export and REST /api/sensor/last; the
//    Modbus server input registers (read with a Modbus TCP client) carry the same corrected counts;
//    DI coils unchanged; Proxy PVs raw; the 90 % alarm still judged on the raw count; a settings
//    change is used by the next saved row and the next server update;
//  * sourceWiring: manager.cpp / manager.h / Core/CMakeLists.txt (desktop only).
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJSEngine>
#include <QJsonDocument>
#include <QJsonObject>
#include <QModbusTcpClient>
#include <QModbusTcpServer>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>

#include <cmath>
#include <limits>
#include <memory>

#include "HistoryExport.h"
#include "HistoryViews.h"
#include "ModbusMapping.h"
#include "ModbusServerBridgeMapping.h"
#include "Modbus_Server.h"
#include "RESTManager.h"
#include "SensorOffsetStorage.h"
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

QStringList logLines(const QString &needle)
{
    QMutexLocker locker(&g_logMutex);
    QStringList out;
    for (const QString &line : std::as_const(g_log)) {
        if (line.contains(needle))
            out << line;
    }
    return out;
}

QString readSource(const QString &relative)
{
    QFile f(QStringLiteral(CORE_SOURCE_DIR "/") + relative);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll()).replace(QStringLiteral("\r\n"), QStringLiteral("\n"));   // autocrlf checkouts
}

quint16 freePort()
{
    QTcpServer probe;
    probe.listen(QHostAddress::LocalHost, 0);
    return probe.serverPort();
}

// Three ports the OS picks (all probes listen at the same time, so they differ).
void freePorts(quint16 *a, quint16 *b, quint16 *c)
{
    QTcpServer pa, pb, pc;
    pa.listen(QHostAddress::LocalHost, 0);
    pb.listen(QHostAddress::LocalHost, 0);
    pc.listen(QHostAddress::LocalHost, 0);
    *a = pa.serverPort();
    *b = pb.serverPort();
    *c = pc.serverPort();
}

constexpr double kT = 100.0 / 65535.0;    // degC per count
constexpr double kP = 1000.0 / 65535.0;   // kPa per count
constexpr double kF = 1.0;                // L/min per count

QVariantMap withOffsets(QVariantMap settings, const QHash<QString, double> &offsets)
{
    for (auto it = offsets.cbegin(); it != offsets.cend(); ++it) {
        QVariantMap entry = settings.value(it.key()).toMap();
        entry.insert(QStringLiteral("offset"), it.value());
        settings.insert(it.key(), entry);
    }
    return settings;
}

// The UI's own SensorUnits.js (TaidaFlowContent/components), evaluated in a QJSEngine.
struct UiUnits
{
    QJSEngine engine;
    QJSValue adjusted;
    bool load(QString *error)
    {
        QFile f(QStringLiteral(CORE_SOURCE_DIR "/../TaidaFlowContent/components/SensorUnits.js"));
        if (!f.open(QIODevice::ReadOnly)) {
            *error = QStringLiteral("SensorUnits.js not found");
            return false;
        }
        QString units = QString::fromUtf8(f.readAll());
        units.remove(QStringLiteral(".pragma library"));
        const QJSValue result = engine.evaluate(units);
        if (result.isError()) {
            *error = result.toString();
            return false;
        }
        adjusted = engine.globalObject().property(QStringLiteral("adjusted"));
        return adjusted.isCallable();
    }
    double call(double raw, const QString &key, const QVariantMap &settings)
    {
        return adjusted.call({raw, key, engine.toScriptValue(settings)}).toNumber();
    }
};
} // namespace

class TestOffsetStorage : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void columnTable();
    void applyRules_data();
    void applyRules();
    void offsetOfRules();
    void clampWarningRateLimited();
    void settingsChangeNextSample();
    void uiEquivalence();
    void managerIntegration();
    void sourceWiring();

private:
    QTemporaryDir m_dir;
    QString m_oldCwd;
    SqlManager *m_sql = nullptr;
};

void TestOffsetStorage::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_oldCwd = QDir::currentPath();
    QVERIFY(QDir::setCurrent(m_dir.path()));   // TaidaFlowSettings.ini of Manager / SqlManager schema lookups
    g_previousHandler = qInstallMessageHandler(captureMessages);
    m_sql = SqlManager::instance();
    m_sql->setDataDirectory(m_dir.filePath(QStringLiteral("data")));
    m_sql->setSettingsFile(m_dir.filePath(QStringLiteral("settings.sqlite")));
    QVERIFY(m_sql->initialize());
}

void TestOffsetStorage::cleanupTestCase()
{
    SqlManager::instance()->shutdown();
    qInstallMessageHandler(g_previousHandler);
    QDir::setCurrent(m_oldCwd);
}

void TestOffsetStorage::columnTable()
{
    const QList<SensorOffsetStorage::Column> columns =
            SensorOffsetStorage::columnsOf(ModbusMapping::defaultReadBindings());
    struct Expected { const char *key; int column; double scale; const char *unit; };
    const Expected expected[] = {
        {"tt01", 1, kT, "°C"}, {"tt02", 2, kT, "°C"}, {"tt03", 3, kT, "°C"}, {"tt04", 4, kT, "°C"},
        {"pt01", 5, kP, "kPa"}, {"pt02", 6, kP, "kPa"}, {"pt03", 7, kP, "kPa"}, {"pt04", 8, kP, "kPa"},
        {"pt05", 9, kP, "kPa"}, {"pt06", 10, kP, "kPa"}, {"pt07", 11, kP, "kPa"},
        {"flowMeter", 12, kF, "L/min"},
    };
    QCOMPARE(columns.size(), qsizetype(std::size(expected)));
    QStringList table;
    for (qsizetype i = 0; i < columns.size(); ++i) {
        const SensorOffsetStorage::Column &c = columns.at(i);
        QCOMPARE(c.key, QString::fromLatin1(expected[i].key));
        QCOMPARE(c.column, expected[i].column);
        QCOMPARE(c.serverRegister, expected[i].column - 1);
        QCOMPARE(c.scale, expected[i].scale);
        QCOMPARE(c.unit, QString::fromUtf8(expected[i].unit));
        table << QStringLiteral("%1=s%2 x%3").arg(c.key).arg(c.column).arg(c.scale, 0, 'g', 12);
    }
    qInfo().noquote() << "[w2-086 test] column table:" << table.join(QStringLiteral(", "));

    // The History page / CSV conversion of the same columns (HistoryExport::appendCsvRow converts
    // with the same per-column scale as HistoryViews): raw 65535 -> scale x 65535.
    double raw[16];
    bool valid[16];
    for (int i = 0; i < 16; ++i) {
        raw[i] = 65535.0;
        valid[i] = true;
    }
    QByteArray row;
    HistoryExport::TimeFormatCache cache;
    HistoryExport::appendCsvRow(row, 1, 1700000000, raw, valid, cache);
    const QList<QByteArray> cells = row.trimmed().split(',');
    QCOMPARE(cells.size(), 2 + 16);
    for (const SensorOffsetStorage::Column &c : columns) {
        QByteArray cell = cells.at(1 + c.column);
        cell.replace('"', "");
        QByteArray expectedCell;
        HistoryExport::appendFixed2(expectedCell, 65535.0 * c.scale);
        QCOMPARE(cell, expectedCell);
    }
    // MV positions and FM are the only other columns: no offset key.
    QCOMPARE(SensorOffsetStorage::sensorKey(ModbusMapping::ProcessPoint::Mv1Position), QString());
    QCOMPARE(SensorOffsetStorage::sensorKey(ModbusMapping::ProcessPoint::Mv4Position), QString());
}

void TestOffsetStorage::applyRules_data()
{
    QTest::addColumn<int>("raw");
    QTest::addColumn<double>("offset");
    QTest::addColumn<double>("scale");
    QTest::addColumn<int>("expected");
    QTest::addColumn<int>("clamp");     // 0 none, -1 low, 1 high

    QTest::newRow("pressure +12.5 kPa") << 32768 << 12.5 << kP << 33587 << 0;      // +819.1875
    QTest::newRow("pressure -7.3 kPa") << 40000 << -7.3 << kP << 39522 << 0;       // -478.4055
    QTest::newRow("pressure 0") << 40000 << 0.0 << kP << 40000 << 0;
    QTest::newRow("pressure < half count") << 1000 << 0.0076 << kP << 1000 << 0;  // +0.498
    QTest::newRow("pressure > half count") << 1000 << 0.0077 << kP << 1001 << 0;  // +0.505
    QTest::newRow("pressure clamp high") << 65000 << 20.0 << kP << 65535 << 1;     // 66310.7
    QTest::newRow("pressure exactly 65535") << 65535 - 819 << 12.5 << kP << 65535 << 0;   // 65535.19
    QTest::newRow("pressure clamp low") << 100 << -10.0 << kP << 0 << -1;          // -555.35
    QTest::newRow("temperature +2.5 degC") << 30000 << 2.5 << kT << 31638 << 0;    // +1638.375
    QTest::newRow("temperature -1.25 degC") << 30000 << -1.25 << kT << 29181 << 0; // -819.1875
    QTest::newRow("temperature to exactly 0") << 3277 << -5.0 << kT << 0 << 0;     // 0.25 -> 0
    QTest::newRow("temperature clamp low") << 100 << -5.0 << kT << 0 << -1;
    QTest::newRow("temperature clamp high") << 60000 << 50.0 << kT << 65535 << 1;
    QTest::newRow("flow +3.4 L/min") << 150 << 3.4 << kF << 153 << 0;
    QTest::newRow("flow +3.6 L/min") << 150 << 3.6 << kF << 154 << 0;
    QTest::newRow("flow -0.5 L/min (half away from zero)") << 150 << -0.5 << kF << 150 << 0;   // 149.5 -> 150
    QTest::newRow("flow +0.5 L/min (half away from zero)") << 150 << 0.5 << kF << 151 << 0;    // 150.5 -> 151
    QTest::newRow("flow clamp low") << 3 << -10.0 << kF << 0 << -1;
}

void TestOffsetStorage::applyRules()
{
    QFETCH(int, raw);
    QFETCH(double, offset);
    QFETCH(double, scale);
    QFETCH(int, expected);
    QFETCH(int, clamp);
    const SensorOffsetStorage::Result r = SensorOffsetStorage::apply(quint16(raw), offset, scale);
    QCOMPARE(int(r.value), expected);
    const SensorOffsetStorage::Clamp expectedClamp = clamp < 0 ? SensorOffsetStorage::Clamp::Low
            : clamp > 0 ? SensorOffsetStorage::Clamp::High : SensorOffsetStorage::Clamp::None;
    QCOMPARE(r.clamp, expectedClamp);
    if (clamp == 0) {
        // Not clamped: stored x scale is the corrected engineering value within half a count.
        QVERIFY2(std::abs(r.value * scale - (raw * scale + offset)) <= 0.5 * scale + 1e-9,
                 qPrintable(QStringLiteral("%1 vs %2").arg(r.value * scale).arg(raw * scale + offset)));
    }
}

void TestOffsetStorage::offsetOfRules()
{
    QVariantMap settings;
    QCOMPARE(SensorOffsetStorage::offsetOf(settings, QStringLiteral("pt01")), 0.0);          // no entry
    settings.insert(QStringLiteral("pt01"), QVariantMap{{"lower", 1.0}});
    QCOMPARE(SensorOffsetStorage::offsetOf(settings, QStringLiteral("pt01")), 0.0);          // no offset field
    settings.insert(QStringLiteral("pt01"), QVariantMap{{"offset", std::numeric_limits<double>::quiet_NaN()}});
    QCOMPARE(SensorOffsetStorage::offsetOf(settings, QStringLiteral("pt01")), 0.0);          // non-finite
    settings.insert(QStringLiteral("pt01"), QVariantMap{{"offset", std::numeric_limits<double>::infinity()}});
    QCOMPARE(SensorOffsetStorage::offsetOf(settings, QStringLiteral("pt01")), 0.0);
    settings.insert(QStringLiteral("pt01"), QVariantMap{{"offset", -3.5}});
    QCOMPARE(SensorOffsetStorage::offsetOf(settings, QStringLiteral("pt01")), -3.5);
    // apply() with a non-finite offset / scale: raw unchanged.
    QCOMPARE(int(SensorOffsetStorage::apply(1234, std::numeric_limits<double>::quiet_NaN(), kP).value), 1234);
    QCOMPARE(int(SensorOffsetStorage::apply(1234, 5.0, 0.0).value), 1234);

    // The Proxy's default map (every offset 0 = nothing configured) changes nothing.
    TaidaFlowProxy proxy;
    SensorOffsetStorage storage(&proxy, ModbusMapping::defaultReadBindings());
    QVector<quint16> regs(ModbusServerBridgeMapping::ServerInputRegisterCount);
    for (int i = 0; i < regs.size(); ++i)
        regs[i] = quint16(1000 * (i + 1) + 7);
    const QVector<double> sample = storage.correctedSample(regs);
    QCOMPARE(sample.size(), regs.size());
    for (int i = 0; i < regs.size(); ++i) {
        QCOMPARE(sample.at(i), double(regs.at(i)));
        QCOMPARE(storage.correctedServerRegister(i, regs.at(i)), regs.at(i));
    }
    // Without a Proxy: raw.
    SensorOffsetStorage noProxy(nullptr, ModbusMapping::defaultReadBindings());
    QCOMPARE(noProxy.correctedSample(regs).at(4), double(regs.at(4)));
    QCOMPARE(noProxy.correctedServerRegister(4, 500), quint16(500));
}

void TestOffsetStorage::clampWarningRateLimited()
{
    TaidaFlowProxy proxy;
    proxy.setSensorSettingsSv(withOffsets(proxy.sensorSettingsSv(), {{"pt01", 900.0}, {"tt03", -50.0}}));
    SensorOffsetStorage::Options options;
    options.clampWarningIntervalMs = 400;
    SensorOffsetStorage storage(&proxy, ModbusMapping::defaultReadBindings(), options);
    QVector<quint16> regs(ModbusServerBridgeMapping::ServerInputRegisterCount, 10000);
    const int before = logCount(QStringLiteral("[SensorOffset] pt01 (s5)"));
    const int beforeTt = logCount(QStringLiteral("[SensorOffset] tt03 (s3)"));
    for (int i = 0; i < 10; ++i) {
        const QVector<double> sample = storage.correctedSample(regs);
        QCOMPARE(sample.at(4), 65535.0);   // pt01 clamped high
        QCOMPARE(sample.at(2), 0.0);       // tt03 clamped low
        QCOMPARE(storage.correctedServerRegister(4, 10000), quint16(65535));
    }
    // 20 clamps of pt01 (10 samples + 10 server values) -> one warning; tt03 its own limiter.
    QCOMPARE(logCount(QStringLiteral("[SensorOffset] pt01 (s5)")) - before, 1);
    QCOMPARE(logCount(QStringLiteral("[SensorOffset] tt03 (s3)")) - beforeTt, 1);
    QCOMPARE(storage.clampWarningsWritten(), 2);
    const QStringList first = logLines(QStringLiteral("[SensorOffset] pt01 (s5)"));
    QVERIFY(first.last().contains(QStringLiteral("raw=10000 + offset 900.000 kPa")));
    QVERIFY(first.last().contains(QStringLiteral("clamped to 65535")));
    QTest::qWait(450);
    storage.correctedSample(regs);
    QCOMPARE(logCount(QStringLiteral("[SensorOffset] pt01 (s5)")) - before, 2);
    const QStringList second = logLines(QStringLiteral("[SensorOffset] pt01 (s5)"));
    QVERIFY2(second.last().contains(QStringLiteral("(19 similar warning(s) held back since the previous one")),
             qPrintable(second.last()));
    qInfo().noquote() << "[w2-086 test] clamp warning after the interval:" << second.last();
}

void TestOffsetStorage::settingsChangeNextSample()
{
    TaidaFlowProxy proxy;
    SensorOffsetStorage storage(&proxy, ModbusMapping::defaultReadBindings());
    QVector<quint16> regs(ModbusServerBridgeMapping::ServerInputRegisterCount, 0);
    for (int i = 0; i < regs.size(); ++i)
        regs[i] = quint16(20000 + i);
    proxy.setPt04ValuePv(regs.at(7) * kP);
    const double pvBefore = proxy.pt04ValuePv();

    proxy.setSensorSettingsSv(withOffsets(proxy.sensorSettingsSv(), {{"pt04", 10.0}}));
    QCOMPARE(storage.correctedSample(regs).at(7), std::round(regs.at(7) + 10.0 / kP));
    proxy.setSensorSettingsSv(withOffsets(proxy.sensorSettingsSv(), {{"pt04", -4.0}}));
    QCOMPARE(storage.correctedSample(regs).at(7), std::round(regs.at(7) - 4.0 / kP));       // next sample
    QCOMPARE(storage.correctedServerRegister(7, regs.at(7)), quint16(std::round(regs.at(7) - 4.0 / kP)));
    proxy.setSensorSettingsSv(withOffsets(proxy.sensorSettingsSv(), {{"pt04", 0.0}}));
    QCOMPARE(storage.correctedSample(regs).at(7), double(regs.at(7)));                       // back to raw
    QCOMPARE(proxy.pt04ValuePv(), pvBefore);                                                   // PV untouched

    // MV positions (s13..s16) and registers 16..19 have no offset key: never changed.
    proxy.setSensorSettingsSv(withOffsets(proxy.sensorSettingsSv(),
            {{"pt01", 5.0}, {"pt02", 5.0}, {"pt03", 5.0}, {"pt04", 5.0}, {"pt05", 5.0}, {"pt06", 5.0},
             {"pt07", 5.0}, {"tt01", 5.0}, {"tt02", 5.0}, {"tt03", 5.0}, {"tt04", 5.0}, {"flowMeter", 5.0}}));
    const QVector<double> all = storage.correctedSample(regs);
    for (int i = 0; i < 12; ++i)
        QVERIFY2(all.at(i) != double(regs.at(i)), qPrintable(QStringLiteral("s%1").arg(i + 1)));
    for (int i = 12; i < regs.size(); ++i) {
        QCOMPARE(all.at(i), double(regs.at(i)));
        QCOMPARE(storage.correctedServerRegister(i, regs.at(i)), regs.at(i));
    }
    QVERIFY(logCount(QStringLiteral("pt01 5.000 kPa = 327.6")) >= 1);
}

void TestOffsetStorage::uiEquivalence()
{
    UiUnits ui;
    QString error;
    QVERIFY2(ui.load(&error), qPrintable(error));
    TaidaFlowProxy proxy;
    const QList<SensorOffsetStorage::Column> columns =
            SensorOffsetStorage::columnsOf(ModbusMapping::defaultReadBindings());
    const QList<int> raws{0, 1, 100, 3277, 12345, 32768, 40000, 58982, 65000, 65534, 65535};
    const QList<double> offsets{-50.0, -12.5, -1.25, -0.3, -0.0076, 0.0, 0.0076, 0.3, 2.5, 12.5, 50.0};
    int compared = 0;
    int clamped = 0;
    double worstCounts = 0.0;
    for (const SensorOffsetStorage::Column &c : columns) {
        for (double offset : offsets) {
            const QVariantMap settings = withOffsets(proxy.sensorSettingsSv(), {{c.key, offset}});
            for (int raw : raws) {
                const SensorOffsetStorage::Result r = SensorOffsetStorage::apply(quint16(raw), offset, c.scale);
                if (r.clamp != SensorOffsetStorage::Clamp::None) {
                    ++clamped;
                    QVERIFY(r.value == 0 || r.value == 65535);
                    continue;
                }
                const double history = r.value * c.scale;            // HistoryViews / CSV / REST x scale
                const double screen = ui.call(raw * c.scale, c.key, settings);   // PV raw*scale + offset
                const double diffCounts = std::abs(history - screen) / c.scale;
                worstCounts = std::max(worstCounts, diffCounts);
                if (diffCounts > 0.5 + 1e-6)
                    QFAIL(qPrintable(QStringLiteral("%1 raw=%2 offset=%3: history %4 vs screen %5 (%6 counts)")
                                             .arg(c.key).arg(raw).arg(offset).arg(history, 0, 'f', 6)
                                             .arg(screen, 0, 'f', 6).arg(diffCounts, 0, 'f', 4)));
                ++compared;
            }
        }
    }
    qInfo().noquote() << QStringLiteral("[w2-086 test] History value vs SensorUnits.adjusted(): %1 cases within "
                                        "half a count (worst %2 counts), %3 clamped cases excluded")
                                 .arg(compared).arg(worstCounts, 0, 'f', 4).arg(clamped);
    QVERIFY(compared > 1000);
    QVERIFY(clamped > 0);
}

void TestOffsetStorage::managerIntegration()
{
    UiUnits ui;
    QString error;
    QVERIFY2(ui.load(&error), qPrintable(error));

    // Modbus TCP servers of this test: A = ADAM-6217 .202 (TT-01..04, PT-01..04 in HR0..7),
    // B = everything else (.203 PT-05..07 / flow / MV; .201 coils; .204 DIs / AOs; .205 HR10).
    QModbusTcpServer serverA;
    QModbusTcpServer serverB;
    QModbusDataUnitMap map;
    map.insert(QModbusDataUnit::Coils, {QModbusDataUnit::Coils, 0, 64});
    map.insert(QModbusDataUnit::DiscreteInputs, {QModbusDataUnit::DiscreteInputs, 0, 16});
    map.insert(QModbusDataUnit::HoldingRegisters, {QModbusDataUnit::HoldingRegisters, 0, 32});
    map.insert(QModbusDataUnit::InputRegisters, {QModbusDataUnit::InputRegisters, 0, 16});
    quint16 portA = 0, portB = 0, portServer = 0;
    freePorts(&portA, &portB, &portServer);
    QVERIFY(portA && portB && portServer && portA != portB && portB != portServer && portA != portServer);
    for (auto [server, port] : {std::pair<QModbusTcpServer *, quint16>{&serverA, portA}, {&serverB, portB}}) {
        server->setMap(map);
        server->setServerAddress(1);
        server->setConnectionParameter(QModbusDevice::NetworkAddressParameter, QStringLiteral("127.0.0.1"));
        server->setConnectionParameter(QModbusDevice::NetworkPortParameter, port);
        QVERIFY2(server->connectDevice(), qPrintable(server->errorString()));
    }
    serverB.setData(QModbusDataUnit::DiscreteInputs, 0, 1);    // DI0 = 1 (healthy), DI1 = DI2 = 0
    // Raw counts: A HR0..7 = TT-01..04, PT-01..04; B HR0..7 = PT-05..07, FM, MV1..4.
    const quint16 rawA[8] = {30000, 30000, 100, 20000, 32768, 40000, 65000, 58000};
    const quint16 rawB[8] = {1000, 50000, 12345, 150, 1000, 2000, 3000, 4000};
    for (int i = 0; i < 8; ++i) {
        serverA.setData(QModbusDataUnit::HoldingRegisters, quint16(i), rawA[i]);
        serverB.setData(QModbusDataUnit::HoldingRegisters, quint16(i), rawB[i]);
    }
    quint16 raw[16];
    for (int i = 0; i < 8; ++i) {
        raw[i] = rawA[i];
        raw[8 + i] = rawB[i];
    }

    TaidaFlowProxy proxy;
    // tt01 +2.5, tt02 -1.25, tt03 -5 (clamped low), tt04 0; pt01 +12.5, pt02 -7.3, pt03 +20 (clamped
    // high, raw 65000 is also over the 90 % alarm), pt04 +30 (raw 58000 < 90 % = 58982, corrected 59966
    // > 58982: the 90 % alarm stays raw -> no alarm), pt05 0, pt06 0, pt07 -0.5, flowMeter +3.4.
    const QHash<QString, double> offsets{{"tt01", 2.5}, {"tt02", -1.25}, {"tt03", -5.0}, {"pt01", 12.5},
                                         {"pt02", -7.3}, {"pt03", 20.0}, {"pt04", 30.0}, {"pt07", -0.5},
                                         {"flowMeter", 3.4}};
    proxy.setSensorSettingsSv(withOffsets(proxy.sensorSettingsSv(), offsets));
    const QVariantMap settings = proxy.sensorSettingsSv();
    const QList<SensorOffsetStorage::Column> columns =
            SensorOffsetStorage::columnsOf(ModbusMapping::defaultReadBindings());
    quint16 expected[16];
    for (int i = 0; i < 16; ++i)
        expected[i] = raw[i];
    for (const SensorOffsetStorage::Column &c : columns)
        expected[c.serverRegister] = SensorOffsetStorage::apply(raw[c.serverRegister], offsets.value(c.key), c.scale).value;
    QCOMPARE(int(expected[0]), 31638);
    QCOMPARE(int(expected[2]), 0);
    QCOMPARE(int(expected[4]), 33587);
    QCOMPARE(int(expected[6]), 65535);
    QCOMPARE(int(expected[7]), 59966);
    QCOMPARE(int(expected[11]), 153);

    Manager::DeviceSettings devices;
    for (ModbusClient::DeviceConfig &config : devices.modbusDevices) {
        config.host = QStringLiteral("127.0.0.1");               // never the plant addresses
        config.port = config.device == ModbusClient::Device::Adam6217_202 ? portA : portB;
    }
    devices.ms300.serialPort = QStringLiteral("TAIDAFLOW_TEST_NO_SUCH_PORT");

    // The real external Modbus server, connected like Core::init (core.cpp).
    ModbusServer modbusServer;
    QVERIFY(modbusServer.start(QHostAddress(QHostAddress::LocalHost), portServer, 1));
    Manager manager(&proxy, m_sql, devices);
    connect(&manager, &Manager::serverCoilUpdated, &modbusServer, &ModbusServer::setCoil);
    connect(&manager, &Manager::serverInputRegisterUpdated, &modbusServer, &ModbusServer::setInputRegister);
    connect(&manager, &Manager::serverHoldingRegisterUpdated, &modbusServer, &ModbusServer::setHoldingRegister);
    QSignalSpy savedSpy(&manager, &Manager::serverInputDataSaved);
    const qint64 startSec = QDateTime::currentSecsSinceEpoch();
    manager.start();
    QTRY_VERIFY_WITH_TIMEOUT(savedSpy.count() >= 2, 10000);

    // ---- Modbus server input registers (external HMI view), read with a Modbus TCP client ----
    auto readServer = [&](QModbusDataUnit::RegisterType type, int start, int count, QVector<quint16> *out) {
        QModbusTcpClient client;
        client.setConnectionParameter(QModbusDevice::NetworkAddressParameter, QStringLiteral("127.0.0.1"));
        client.setConnectionParameter(QModbusDevice::NetworkPortParameter, portServer);
        client.setTimeout(3000);
        client.setNumberOfRetries(0);
        if (!client.connectDevice())
            return false;
        QElapsedTimer t;
        t.start();
        while (client.state() != QModbusDevice::ConnectedState && t.elapsed() < 5000)
            QTest::qWait(20);
        if (client.state() != QModbusDevice::ConnectedState)
            return false;
        QModbusReply *reply = client.sendReadRequest(QModbusDataUnit(type, start, quint16(count)), 1);
        if (!reply)
            return false;
        while (!reply->isFinished() && t.elapsed() < 10000)
            QTest::qWait(20);
        const bool ok = reply->isFinished() && reply->error() == QModbusDevice::NoError;
        if (ok)
            *out = reply->result().values();
        reply->deleteLater();
        client.disconnectDevice();
        return ok;
    };
    QVector<quint16> ir;
    QVERIFY(readServer(QModbusDataUnit::InputRegisters, 0, 16, &ir));
    QCOMPARE(ir.size(), 16);
    QStringList irText;
    for (int i = 0; i < 16; ++i) {
        irText << QStringLiteral("IR%1=%2(raw %3)").arg(i).arg(ir.at(i)).arg(raw[i]);
        QVERIFY2(ir.at(i) == expected[i], qPrintable(QStringLiteral("IR%1: %2 != %3").arg(i).arg(ir.at(i)).arg(expected[i])));
    }
    qInfo().noquote() << "[w2-086 test] Modbus server input registers:" << irText.join(' ');
    QVector<quint16> coils;
    QVERIFY(readServer(QModbusDataUnit::Coils, ModbusServerBridgeMapping::ServerDiStart, 3, &coils));
    QVERIFY(coils.size() >= 3);
    QCOMPARE(coils.mid(0, 3), (QVector<quint16>{1, 0, 0}));   // DI0..DI2 mirrored unchanged

    // ---- Proxy PVs stay raw; the 90 % alarm is judged on the raw count ----
    QVERIFY(std::abs(proxy.pt01ValuePv() - raw[4] * kP) < 1e-9);
    QVERIFY(std::abs(proxy.tt01ValuePv() - raw[0] * kT) < 1e-9);
    QVERIFY(std::abs(proxy.flowMeterValuePv() - raw[11] * kF) < 1e-9);
    QTRY_VERIFY_WITH_TIMEOUT(logCount(QStringLiteral("[SQL] Alarm inserted: sensor=PT-03 message=輸入值達到設定高限")) >= 1, 5000);
    QCOMPARE(logCount(QStringLiteral("[SQL] Alarm inserted: sensor=PT-04 message=輸入值達到設定高限")), 0);

    // ---- settings change: the next saved row and the next server update use the new offset ----
    proxy.setSensorSettingsSv(withOffsets(proxy.sensorSettingsSv(), {{"pt01", -12.5}}));
    const int savedAtChange = savedSpy.count();
    QTRY_VERIFY_WITH_TIMEOUT(savedSpy.count() >= savedAtChange + 1, 5000);
    const quint16 pt01New = SensorOffsetStorage::apply(raw[4], -12.5, kP).value;
    QCOMPARE(int(pt01New), 31949);    // 32768 - 819.1875
    {
        const QVector<QVariantList> rows = m_sql->fetchSensorData(QDate::currentDate(), 1);
        QVERIFY(!rows.isEmpty());
        QCOMPARE(rows.first().at(5).toDouble(), double(pt01New));   // index 0 = timestamp, 5 = s5
    }
    QVector<quint16> irAfter;
    QVERIFY(readServer(QModbusDataUnit::InputRegisters, 4, 1, &irAfter));
    QCOMPARE(irAfter.value(0), pt01New);
    // back to the first offsets for the rest of the checks
    proxy.setSensorSettingsSv(settings);
    const int savedAtRestore = savedSpy.count();
    QTRY_VERIFY_WITH_TIMEOUT(savedSpy.count() >= savedAtRestore + 2, 6000);
    manager.stop();
    QTest::qWait(300);

    // ---- sensor_data row (newest) ----
    const QVector<QVariantList> rows = m_sql->fetchSensorData(QDate::currentDate(), 1);
    QVERIFY(!rows.isEmpty());
    const QVariantList dbRow = rows.first();
    const qint64 ts = dbRow.at(0).toLongLong();
    QVERIFY(ts >= startSec);
    for (int i = 0; i < 16; ++i)
        QCOMPARE(dbRow.at(1 + i).toDouble(), double(expected[i]));
    qInfo().noquote() << "[w2-086 test] newest sensor_data row ts" << ts << "s1..s16 ="
                      << [&]() { QStringList l; for (int i = 0; i < 16; ++i) l << dbRow.at(1 + i).toString(); return l.join(','); }();

    // ---- History page (HistoryViewService -> historyViews["desktop"]) for exactly that second ----
    HistoryViewService views(&proxy, m_sql, HistoryViewService::Options{});
    views.handleRequest(QStringLiteral("desktop"), double(ts) * 1000.0, double(ts) * 1000.0 + 999.0, 1);
    QTRY_VERIFY_WITH_TIMEOUT(!proxy.historyViews().value(QStringLiteral("desktop")).toMap()
                                     .value(QStringLiteral("records")).toList().isEmpty(), 10000);
    const QVariantList records = proxy.historyViews().value(QStringLiteral("desktop")).toMap()
            .value(QStringLiteral("records")).toList();
    const QVariantList history = records.first().toMap().value(QStringLiteral("values")).toList();
    QCOMPARE(history.size(), 1 + 16);
    for (const SensorOffsetStorage::Column &c : columns) {
        const int i = c.serverRegister;
        const double h = history.at(1 + i).toDouble();
        QVERIFY2(std::abs(h - expected[i] * c.scale) < 1e-9, qPrintable(c.key));
        const bool clamped = SensorOffsetStorage::apply(raw[i], offsets.value(c.key), c.scale).clamp
                != SensorOffsetStorage::Clamp::None;
        // The main screen shows SensorUnits.adjusted(PV): PV = raw x scale (what Manager wrote).
        const double pv = proxy.property((c.key + QStringLiteral("ValuePv")).toLatin1().constData()).toDouble();
        const double screen = ui.call(pv, c.key, settings);
        if (!clamped) {
            QVERIFY2(std::abs(h - screen) <= 0.5 * c.scale + 1e-9,
                     qPrintable(QStringLiteral("%1: history %2 vs screen %3").arg(c.key).arg(h, 0, 'f', 6).arg(screen, 0, 'f', 6)));
        }
        qInfo().noquote() << QStringLiteral("[w2-086 test] %1 s%2 raw=%3 stored=%4 history=%5 screen=%6 %7%8")
                                     .arg(c.key).arg(c.column).arg(raw[i]).arg(expected[i])
                                     .arg(h, 0, 'f', 4).arg(screen, 0, 'f', 4).arg(c.unit)
                                     .arg(clamped ? QStringLiteral(" (clamped)") : QString());
    }
    for (int i = 12; i < 16; ++i)     // MV1..MV4 positions: raw
        QVERIFY(std::abs(history.at(1 + i).toDouble() - raw[i] * (100.0 / 65535.0)) < 1e-9);

    // ---- CSV export of the same second (desktop flow, the real export worker) ----
    HistoryExportManager::Options exportOptions;
    exportOptions.exportDir = m_dir.filePath(QStringLiteral("exports"));
    exportOptions.mountDownloads = false;
    HistoryExportManager exporter(&proxy, m_sql, exportOptions);
    QSignalSpy exportDone(&exporter, &HistoryExportManager::jobFinished);
    const QString csvPath = m_dir.filePath(QStringLiteral("w2-086.csv"));
    exporter.enqueueDesktopExport(csvPath, double(ts) * 1000.0, double(ts) * 1000.0 + 999.0);
    QTRY_VERIFY_WITH_TIMEOUT(exportDone.count() >= 1, 20000);
    QCOMPARE(exportDone.first().at(1).toString(), QStringLiteral("done"));
    QFile csv(csvPath);
    QVERIFY(csv.open(QIODevice::ReadOnly));
    const QList<QByteArray> lines = csv.readAll().trimmed().split('\n');
    QCOMPARE(lines.size(), 2);                                // header + one row
    QList<QByteArray> cells = lines.at(1).trimmed().split(',');
    QCOMPARE(cells.size(), 2 + 16);
    for (int i = 0; i < 16; ++i) {
        QByteArray cell = cells.at(2 + i);
        cell.replace('"', "");
        QByteArray fromHistory;
        HistoryExport::appendFixed2(fromHistory, history.at(1 + i).toDouble());
        QVERIFY2(cell == fromHistory, qPrintable(QStringLiteral("CSV s%1 %2 vs history %3").arg(i + 1)
                                                         .arg(QString::fromLatin1(cell), QString::fromLatin1(fromHistory))));
    }
    exporter.shutdown();

    // ---- REST /api/sensor/last (the database row as stored: counts) ----
    RESTManager rest(m_sql);
    QVERIFY(rest.start(0, QHostAddress(QHostAddress::LocalHost)));
    QNetworkAccessManager nam;
    QNetworkReply *reply = nam.get(QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:%1/api/sensor/last")
                                                                .arg(rest.serverPort()))));
    QSignalSpy finished(reply, &QNetworkReply::finished);
    QVERIFY(reply->isFinished() || finished.wait(10000));
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
    const QJsonObject last = QJsonDocument::fromJson(reply->readAll()).object();
    reply->deleteLater();
    QCOMPARE(last.value(QStringLiteral("ts")).toInteger(), ts);
    for (int i = 0; i < 16; ++i) {
        const double counts = last.value(QStringLiteral("s%1").arg(i + 1)).toDouble();
        QCOMPARE(counts, double(expected[i]));
        const double scale = i < 4 ? kT : i < 11 ? kP : i == 11 ? kF : 100.0 / 65535.0;
        QVERIFY(std::abs(counts * scale - history.at(1 + i).toDouble()) < 1e-9);   // REST x scale == History
    }
    qInfo().noquote() << "[w2-086 test] REST /api/sensor/last =" << QJsonDocument(last).toJson(QJsonDocument::Compact);
    modbusServer.stop();
}

void TestOffsetStorage::sourceWiring()
{
    const QString manager = readSource(QStringLiteral("manager.cpp"));
    const QString managerH = readSource(QStringLiteral("manager.h"));
    const QString cmake = readSource(QStringLiteral("CMakeLists.txt"));
    QVERIFY(!manager.isEmpty() && !managerH.isEmpty() && !cmake.isEmpty());
    QVERIFY(manager.contains(QStringLiteral("m_offsetStorage = new SensorOffsetStorage(proxy, m_readBindings, this);")));
    QVERIFY(managerH.contains(QStringLiteral("SensorOffsetStorage *m_offsetStorage = nullptr;")));
    // Mirror path: raw kept for the 90 % alarm and the sample, the server gets the corrected value.
    const int mirror = manager.indexOf(QStringLiteral("void Manager::mirrorClientData("));
    const int keep = manager.indexOf(QStringLiteral("m_serverInputRegisters[serverOffset] = values.at(index);"), mirror);
    const int corr = manager.indexOf(QStringLiteral("m_offsetStorage->correctedServerRegister(serverOffset, values.at(index));"), mirror);
    const int emitIr = manager.indexOf(QStringLiteral("emit serverInputRegisterUpdated(serverOffset, serverValue);"), mirror);
    const int alarm = manager.indexOf(QStringLiteral("checkHighInputAlarm(serverOffset, values.at(index));"), mirror);
    QVERIFY(mirror > 0 && keep > mirror && corr > keep && emitIr > corr && alarm > emitIr);
    QCOMPARE(manager.count(QStringLiteral("emit serverInputRegisterUpdated(")), 1);
    // Save path: the readings come from the offset storage.
    const int save = manager.indexOf(QStringLiteral("void Manager::saveServerInputData()"));
    const int sample = manager.indexOf(QStringLiteral("m_offsetStorage->correctedSample(m_serverInputRegisters);"), save);
    const int write = manager.indexOf(QStringLiteral("m_sql->saveSensorData(QDateTime::currentDateTime(), readings);"), save);
    QVERIFY(save > 0 && sample > save && write > sample);
    QCOMPARE(manager.count(QStringLiteral("saveSensorData(")), 1);
    // updateProcessPoint (Proxy PVs) unchanged: no offset there.
    const int upd = manager.indexOf(QStringLiteral("void Manager::updateProcessPoint("));
    const int updEnd = manager.indexOf(QStringLiteral("void Manager::writeServerData("), upd);
    QVERIFY(upd > 0 && updEnd > upd);
    QVERIFY(!manager.mid(upd, updEnd - upd).contains(QStringLiteral("offset")));
    // Desktop-only source.
    const int desktop = cmake.indexOf(QStringLiteral("if(TAIDAFLOW_IS_WINDOWS_DESKTOP)"));
    const int endif = cmake.indexOf(QStringLiteral("endif()"), desktop);
    const int src = cmake.indexOf(QStringLiteral("SensorOffsetStorage.cpp"));
    QVERIFY(desktop > 0 && src > desktop && src < endif);
    QCOMPARE(cmake.count(QStringLiteral("SensorOffsetStorage.cpp")), 1);
}

QTEST_MAIN(TestOffsetStorage)
#include "tst_offset_storage.moc"
