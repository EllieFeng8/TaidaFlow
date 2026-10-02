// w2-087: TaidaFlowProxy::deviceStatus written by the Core (DeviceStatusPublisher, owned by Manager).
//
// Real ModbusClient + Ms300FaultReader + TaidaFlowProxy, no device and no plant address:
//  * four ADAM devices against Modbus TCP servers of this test (QModbusTcpServer, one per device,
//    loopback, ports picked by the OS); ADAM-6217 B points at a loopback address nobody listens on
//    (configured 127.0.0.210, refused);
//  * MS300 with a serial port name that does not exist on this PC (open fails).
// The same executable runs twice under CTest: tst_device_status (the configured hosts) and
// tst_device_status_simulator_profile (TAIDAFLOW_DEVICE_PROFILE=simulator: ModbusClient replaces every
// host by 127.0.0.201..205 and keeps the test's port, so the servers of this test listen there; the
// shared Adam60xxSimulator's port 502 is never used). The address in deviceStatus must be the
// replaced one then.
//
//  * lifecycle: empty map at start; a device gets its key only after its first connection attempt
//    (checked at every write); after all first attempts the complete map (4 ADAM online, ADAM-6217 B
//    and MS300 offline, names = model, addresses = the effective ones, sinceMs = time of the change);
//    the repeated failed reconnects / port opens write nothing; a lost connection (server closed)
//    -> online false, written once although the reconnect attempts keep failing; restored -> true;
//    stop() -> {} and nothing after it (the devices' disconnects of the shutdown are not reported).
//  * ms300ReadRule: the MS300 rule on the publisher's inputs (port open -> online with the first
//    answer; 3 failed reads in a row -> offline; answer again -> online; port closed -> offline).
//    The serial read path itself cannot be run here (no serial port on this PC).
//  * managerWiring: the real Manager (as Core uses it): empty map after construction, keys only after
//    the first attempts after start(), the complete map, stop() (called by Core::shutdown) -> {}.
//  * sourceWiring: Core::shutdown stops the Manager before deleting it, Manager::start / stop start /
//    stop the publisher before the devices, the publisher is in the desktop-only Core sources.
#include "DeviceStatusPublisher.h"
#include "Modbus_Client.h"
#include "Ms300FaultReader.h"
#include "TaidaFlowProxy.h"
#include "manager.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QModbusTcpServer>
#include <QRegularExpression>
#include <QSerialPortInfo>
#include <QSet>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

namespace {
const QString kNoSuchPort = QStringLiteral("COM239");

bool simulatorProfile()
{
    return qEnvironmentVariable("TAIDAFLOW_DEVICE_PROFILE") == QLatin1String("simulator");
}

QString simulatorHost(ModbusClient::Device device)
{
    switch (device) {
    case ModbusClient::Device::Adam6256_201: return QStringLiteral("127.0.0.201");
    case ModbusClient::Device::Adam6217_202: return QStringLiteral("127.0.0.202");
    case ModbusClient::Device::Adam6217_203: return QStringLiteral("127.0.0.203");
    case ModbusClient::Device::Adam6224_204: return QStringLiteral("127.0.0.204");
    case ModbusClient::Device::Adam6022_205: return QStringLiteral("127.0.0.205");
    case ModbusClient::Device::Unassigned: break;
    }
    return QString();
}

quint16 freePort(const QString &host)
{
    QTcpServer probe;
    if (!probe.listen(QHostAddress(host), 0))
        return 0;
    return probe.serverPort();
}

struct TestDevice {
    ModbusClient::Device device;
    QString key;
    QString model;
    QString configuredHost;
    quint16 port = 0;
    bool served = true;                       // false: nobody listens (ADAM-6217 B)
    QString effectiveHost() const { return simulatorProfile() ? simulatorHost(device) : configuredHost; }
};

QModbusDataUnitMap registerMap()
{
    QModbusDataUnitMap map;
    map.insert(QModbusDataUnit::Coils, {QModbusDataUnit::Coils, 0, 64});
    map.insert(QModbusDataUnit::DiscreteInputs, {QModbusDataUnit::DiscreteInputs, 0, 64});
    map.insert(QModbusDataUnit::InputRegisters, {QModbusDataUnit::InputRegisters, 0, 64});
    map.insert(QModbusDataUnit::HoldingRegisters, {QModbusDataUnit::HoldingRegisters, 0, 64});
    return map;
}

// Records every write of deviceStatus and checks at each write that only devices whose first
// connection attempt already has a result have a key.
struct WriteLog {
    QList<QVariantMap> maps;
    QSet<QString> attempted;                  // keys with at least one connection result
    QStringList violations;
};
} // namespace

class TestDeviceStatus : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void lifecycle();
    void ms300ReadRule();
    void managerWiring();
    void sourceWiring();

private:
    QList<ModbusClient::DeviceConfig> deviceConfigs() const;
    Ms300FaultReader::Settings ms300Settings() const;
    void watch(TaidaFlowProxy *proxy, ModbusClient *client, Ms300FaultReader *reader, WriteLog *log);
    void checkCompleteMap(const QVariantMap &map, qint64 notBefore, qint64 notAfter) const;
    const TestDevice &deviceOf(const QString &key) const;

    QTemporaryDir m_dir;
    QString m_oldCwd;
    QList<TestDevice> m_devices;
    QHash<QString, std::shared_ptr<QModbusTcpServer>> m_servers;
};

void TestDeviceStatus::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_oldCwd = QDir::currentPath();
    QVERIFY(QDir::setCurrent(m_dir.path()));    // TaidaFlowSettings.ini of Manager goes here
    qInfo().noquote() << QStringLiteral("[w2-087 test] TAIDAFLOW_DEVICE_PROFILE=%1")
                                 .arg(simulatorProfile() ? QStringLiteral("simulator") : QStringLiteral("(unset)"));
    for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts())
        QVERIFY2(info.portName().compare(kNoSuchPort, Qt::CaseInsensitive) != 0, "the test port name exists here");

    m_devices = {
        {ModbusClient::Device::Adam6256_201, QStringLiteral("adam6256"), QStringLiteral("ADAM-6256"), QStringLiteral("127.0.0.1")},
        {ModbusClient::Device::Adam6217_202, QStringLiteral("adam6217a"), QStringLiteral("ADAM-6217"), QStringLiteral("127.0.0.1")},
        {ModbusClient::Device::Adam6217_203, QStringLiteral("adam6217b"), QStringLiteral("ADAM-6217"), QStringLiteral("127.0.0.210"), 0, false},
        {ModbusClient::Device::Adam6224_204, QStringLiteral("adam6224"), QStringLiteral("ADAM-6224"), QStringLiteral("127.0.0.1")},
        {ModbusClient::Device::Adam6022_205, QStringLiteral("adam6022"), QStringLiteral("ADAM-6022"), QStringLiteral("127.0.0.1")},
    };
    for (TestDevice &d : m_devices) {
        d.port = freePort(d.effectiveHost());
        QVERIFY2(d.port != 0 && d.port != 502, qPrintable(d.key));
        if (!d.served)
            continue;
        auto server = std::make_shared<QModbusTcpServer>();
        server->setMap(registerMap());
        server->setServerAddress(1);
        server->setConnectionParameter(QModbusDevice::NetworkAddressParameter, d.effectiveHost());
        server->setConnectionParameter(QModbusDevice::NetworkPortParameter, d.port);
        QVERIFY2(server->connectDevice(), qPrintable(server->errorString()));
        m_servers.insert(d.key, server);
    }
    for (const TestDevice &d : m_devices) {
        qInfo().noquote() << QStringLiteral("[w2-087 test] %1 configured %2:%3 -> effective %4:%5 %6")
                                     .arg(d.key, d.configuredHost).arg(d.port).arg(d.effectiveHost()).arg(d.port)
                                     .arg(d.served ? QStringLiteral("(server of the test)") : QStringLiteral("(nobody listens)"));
    }
}

void TestDeviceStatus::cleanupTestCase()
{
    for (const auto &server : std::as_const(m_servers))
        server->disconnectDevice();
    m_servers.clear();
    QDir::setCurrent(m_oldCwd);
}

QList<ModbusClient::DeviceConfig> TestDeviceStatus::deviceConfigs() const
{
    QList<ModbusClient::DeviceConfig> configs = ModbusClient::defaultDeviceConfigs();
    for (ModbusClient::DeviceConfig &config : configs) {
        for (const TestDevice &d : m_devices) {
            if (d.device == config.device) {
                config.host = d.configuredHost;      // never the plant addresses
                config.port = d.port;
            }
        }
    }
    return configs;
}

Ms300FaultReader::Settings TestDeviceStatus::ms300Settings() const
{
    Ms300FaultReader::Settings settings;
    settings.serialPort = kNoSuchPort;
    return settings;
}

const TestDevice &TestDeviceStatus::deviceOf(const QString &key) const
{
    for (const TestDevice &d : m_devices) {
        if (d.key == key)
            return d;
    }
    qFatal("unknown key");
    return m_devices.constFirst();
}

void TestDeviceStatus::watch(TaidaFlowProxy *proxy, ModbusClient *client, Ms300FaultReader *reader, WriteLog *log)
{
    // Connected before the publisher's own connections: a result is marked before the publisher
    // writes because of it.
    if (client) {
        connect(client, &ModbusClient::deviceConnectionChanged, this,
                [log](ModbusClient::Device device, bool, const QString &) {
            log->attempted.insert(DeviceStatusPublisher::keyOf(device));
        });
    }
    if (reader)
        connect(reader, &Ms300FaultReader::portOpenChanged, this, [log](bool) { log->attempted.insert(QStringLiteral("ms300")); });
    connect(proxy, &TaidaFlowProxy::deviceStatusChanged, this, [proxy, log] {
        const QVariantMap map = proxy->deviceStatus();
        for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
            if (!log->attempted.contains(it.key()))
                log->violations.append(QStringLiteral("write #%1 has %2 before its first connection result")
                                               .arg(log->maps.size() + 1).arg(it.key()));
        }
        if (!log->maps.isEmpty() && log->maps.constLast() == map)
            log->violations.append(QStringLiteral("write #%1 repeats the previous map").arg(log->maps.size() + 1));
        log->maps.append(map);
    });
}

void TestDeviceStatus::checkCompleteMap(const QVariantMap &map, qint64 notBefore, qint64 notAfter) const
{
    QCOMPARE(map.keys().size(), 6);
    for (const QString &key : TaidaFlowProxy::deviceStatusKeys()) {
        QVERIFY2(map.contains(key), qPrintable(key));
        const QVariantMap entry = map.value(key).toMap();
        QCOMPARE(entry.keys().size(), 4);
        QCOMPARE(entry.value(QStringLiteral("online")).typeId(), QMetaType::Bool);
        const double since = entry.value(QStringLiteral("sinceMs")).toDouble();
        QVERIFY2(since >= double(notBefore) && since <= double(notAfter),
                 qPrintable(QStringLiteral("%1 sinceMs %2 not in %3..%4").arg(key).arg(qint64(since)).arg(notBefore).arg(notAfter)));
        if (key == QLatin1String("ms300")) {
            QCOMPARE(entry.value(QStringLiteral("name")).toString(), QStringLiteral("MS300"));
            QCOMPARE(entry.value(QStringLiteral("address")).toString(), kNoSuchPort);
            QCOMPARE(entry.value(QStringLiteral("online")).toBool(), false);
            continue;
        }
        const TestDevice &d = deviceOf(key);
        QCOMPARE(entry.value(QStringLiteral("name")).toString(), d.model);
        QCOMPARE(entry.value(QStringLiteral("address")).toString(), d.effectiveHost());
        QCOMPARE(entry.value(QStringLiteral("online")).toBool(), d.served);
    }
}

void TestDeviceStatus::lifecycle()
{
    // Recorders first: they outlive the devices (whose destructors still emit on a failed check).
    WriteLog log;
    int failedAttempts6217b = 0;
    int failedAttempts6224 = 0;
    int ms300Closed = 0;
    TaidaFlowProxy proxy;
    ModbusClient client(deviceConfigs());
    Ms300FaultReader reader(ms300Settings());
    watch(&proxy, &client, &reader, &log);
    connect(&client, &ModbusClient::deviceConnectionChanged, this,
            [&](ModbusClient::Device device, bool connected, const QString &) {
        if (!connected && device == ModbusClient::Device::Adam6217_203)
            ++failedAttempts6217b;
        if (!connected && device == ModbusClient::Device::Adam6224_204)
            ++failedAttempts6224;
    });
    connect(&reader, &Ms300FaultReader::portOpenChanged, this, [&](bool open) { ms300Closed += open ? 0 : 1; });

    DeviceStatusPublisher publisher(&proxy);
    publisher.attachModbus(&client);
    publisher.attachMs300(&reader);
    QVERIFY(proxy.deviceStatus().isEmpty());

    // 1. Start: empty map; the ADAM connections are asynchronous -> no ADAM key right after.
    const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
    publisher.start();
    QVERIFY(proxy.deviceStatus().isEmpty());
    client.connectAll();
    QVERIFY2(proxy.deviceStatus().isEmpty(), "no key before a first result");
    // MS300: the first open attempt fails synchronously (port does not exist) -> its key at once.
    reader.start();
    QCOMPARE(proxy.deviceStatus().keys(), QStringList{QStringLiteral("ms300")});
    QCOMPARE(proxy.deviceStatus().value("ms300").toMap().value("online").toBool(), false);

    QTRY_COMPARE_WITH_TIMEOUT(proxy.deviceStatus().size(), 6, 20000);
    const qint64 t1 = QDateTime::currentMSecsSinceEpoch();
    checkCompleteMap(proxy.deviceStatus(), t0, t1);
    QVERIFY2(log.violations.isEmpty(), qPrintable(log.violations.join(QLatin1Char('\n'))));
    QCOMPARE(log.maps.size(), 6);                  // one write per device's first result
    QCOMPARE(publisher.writeCount(), quint64(7));  // + the empty map of start() (unchanged -> no signal)
    for (int i = 0; i < log.maps.size(); ++i)
        QCOMPARE(log.maps.at(i).size(), i + 1);   // one more key per write
    qInfo().noquote() << QStringLiteral("[w2-087 test] complete map after %1 ms: %2")
                                 .arg(t1 - t0).arg(QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(proxy.deviceStatus()))
                                                                     .toJson(QJsonDocument::Compact)));

    // 2. Same state: the failed reconnects (3 s after the previous one ended) and port opens write nothing.
    const QVariantMap complete = proxy.deviceStatus();
    const int attemptsBefore = failedAttempts6217b;
    const int closedBefore = ms300Closed;
    QTest::qWait(15000);
    qInfo().noquote() << QStringLiteral("[w2-087 test] 15 s unchanged: ADAM-6217 B failed attempts %1 -> %2, MS300 open failures %3 -> %4, writes %5")
                                 .arg(attemptsBefore).arg(failedAttempts6217b).arg(closedBefore).arg(ms300Closed).arg(log.maps.size());
    QVERIFY(failedAttempts6217b >= attemptsBefore + 2);
    QVERIFY(ms300Closed >= closedBefore + 3);   // one open attempt every ~4 s (1 s poll, >= 3 s apart)
    QCOMPARE(log.maps.size(), 6);
    QCOMPARE(proxy.deviceStatus(), complete);

    // 3. Outage of ADAM-6224: its server closes every connection and stops listening.
    const std::shared_ptr<QModbusTcpServer> server6224 = m_servers.value(QStringLiteral("adam6224"));
    QVERIFY(server6224);
    const qint64 tLost = QDateTime::currentMSecsSinceEpoch();
    server6224->disconnectDevice();
    QTRY_VERIFY_WITH_TIMEOUT(!proxy.deviceStatus().value("adam6224").toMap().value("online").toBool(), 10000);
    QCOMPARE(log.maps.size(), 7);
    const QVariantMap lost = proxy.deviceStatus();
    const double sinceLost = lost.value("adam6224").toMap().value("sinceMs").toDouble();
    QVERIFY(sinceLost >= double(tLost) && sinceLost <= double(QDateTime::currentMSecsSinceEpoch()));
    for (const QString &key : TaidaFlowProxy::deviceStatusKeys()) {
        if (key != QLatin1String("adam6224"))
            QCOMPARE(lost.value(key), complete.value(key));
    }
    const int lostAttempts = failedAttempts6224;
    QTest::qWait(15000);   // reconnect attempts keep failing: no new write
    qInfo().noquote() << QStringLiteral("[w2-087 test] ADAM-6224 outage: %1 failed connection result(s) in 15 s, writes %2")
                                 .arg(failedAttempts6224 - lostAttempts).arg(log.maps.size());
    QVERIFY(failedAttempts6224 >= lostAttempts + 2);
    QCOMPARE(log.maps.size(), 7);
    QCOMPARE(proxy.deviceStatus(), lost);

    // 4. Restored: listening again -> the next reconnect attempt -> online true, one write.
    const qint64 tBack = QDateTime::currentMSecsSinceEpoch();
    QVERIFY2(server6224->connectDevice(), qPrintable(server6224->errorString()));
    QTRY_VERIFY_WITH_TIMEOUT(proxy.deviceStatus().value("adam6224").toMap().value("online").toBool(), 15000);
    QCOMPARE(log.maps.size(), 8);
    const double sinceBack = proxy.deviceStatus().value("adam6224").toMap().value("sinceMs").toDouble();
    QVERIFY(sinceBack >= double(tBack) && sinceBack <= double(QDateTime::currentMSecsSinceEpoch()));
    for (const QString &key : TaidaFlowProxy::deviceStatusKeys()) {
        if (key != QLatin1String("adam6224"))
            QCOMPARE(proxy.deviceStatus().value(key), complete.value(key));
    }
    QVERIFY2(log.violations.isEmpty(), qPrintable(log.violations.join(QLatin1Char('\n'))));

    // 5. Shutdown order of Manager::stop(): the publisher first -> {}, then the devices close
    //    (their disconnects are not reported).
    publisher.stop();
    QVERIFY(proxy.deviceStatus().isEmpty());
    QCOMPARE(log.maps.size(), 9);
    reader.stop();
    client.disconnectAll();
    QTest::qWait(1000);
    QVERIFY(proxy.deviceStatus().isEmpty());
    QCOMPARE(log.maps.size(), 9);
    publisher.stop();                              // idempotent
    QCOMPARE(log.maps.size(), 9);
}

void TestDeviceStatus::ms300ReadRule()
{
    WriteLog log;                                 // outlives the publisher (its destructor writes)
    TaidaFlowProxy proxy;
    Ms300FaultReader reader(ms300Settings());     // registers the MS300 entry; not started
    qint64 now = 1000;
    DeviceStatusPublisher publisher(&proxy, [&now] { return now; });
    publisher.attachMs300(&reader);
    log.attempted = QSet<QString>{QStringLiteral("ms300")};   // inputs driven here: only the repeat check
    watch(&proxy, nullptr, nullptr, &log);
    publisher.start();
    const auto ms300 = [&proxy] { return proxy.deviceStatus().value(QStringLiteral("ms300")).toMap(); };

    publisher.ms300ReadSucceeded();               // port not open: ignored
    publisher.setMs300PortOpen(true);             // open: still unknown until the inverter answers
    QVERIFY(proxy.deviceStatus().isEmpty());
    now = 2000;
    publisher.ms300ReadSucceeded();
    QCOMPARE(ms300().value("online").toBool(), true);
    QCOMPARE(ms300().value("sinceMs").toDouble(), 2000.0);
    QCOMPARE(ms300().value("address").toString(), kNoSuchPort);
    QCOMPARE(log.maps.size(), 1);
    publisher.ms300ReadSucceeded();               // same state
    now = 3000;
    publisher.ms300ReadFailed();
    publisher.ms300ReadFailed();
    QCOMPARE(ms300().value("online").toBool(), true);    // 2 failures: still online
    publisher.ms300ReadSucceeded();               // resets the count
    publisher.ms300ReadFailed();
    publisher.ms300ReadFailed();
    QCOMPARE(log.maps.size(), 1);
    now = 4000;
    publisher.ms300ReadFailed();                  // 3rd in a row -> offline
    QCOMPARE(ms300().value("online").toBool(), false);
    QCOMPARE(ms300().value("sinceMs").toDouble(), 4000.0);
    QCOMPARE(log.maps.size(), 2);
    now = 5000;
    publisher.ms300ReadFailed();
    publisher.ms300ReadFailed();
    QCOMPARE(log.maps.size(), 2);                 // still offline: no write
    now = 6000;
    publisher.ms300ReadSucceeded();
    QCOMPARE(ms300().value("online").toBool(), true);
    QCOMPARE(ms300().value("sinceMs").toDouble(), 6000.0);
    now = 7000;
    publisher.setMs300PortOpen(false);            // port lost -> offline at once
    QCOMPARE(ms300().value("online").toBool(), false);
    QCOMPARE(ms300().value("sinceMs").toDouble(), 7000.0);
    publisher.ms300ReadSucceeded();               // port not open: ignored
    publisher.setMs300PortOpen(false);
    QCOMPARE(log.maps.size(), 4);
    QVERIFY2(log.violations.isEmpty(), qPrintable(log.violations.join(QLatin1Char('\n'))));
    publisher.stop();
    QVERIFY(proxy.deviceStatus().isEmpty());
    publisher.ms300ReadSucceeded();               // stopped: nothing
    QVERIFY(proxy.deviceStatus().isEmpty());
}

void TestDeviceStatus::managerWiring()
{
    Manager::DeviceSettings devices;
    devices.modbusDevices = deviceConfigs();
    devices.ms300 = ms300Settings();
    TaidaFlowProxy proxy;
    WriteLog log;
    // The Manager's devices are private: the first-result order is checked right after start()
    // below; the log checks repeated maps.
    for (const QString &key : TaidaFlowProxy::deviceStatusKeys())
        log.attempted.insert(key);
    watch(&proxy, nullptr, nullptr, &log);
    qint64 t0 = 0;
    {
        Manager manager(&proxy, nullptr, devices);
        QVERIFY(proxy.deviceStatus().isEmpty());
        t0 = QDateTime::currentMSecsSinceEpoch();
        manager.start();
        for (const QString &key : TaidaFlowProxy::deviceStatusKeys()) {
            if (key != QLatin1String("ms300"))
                QVERIFY2(!proxy.deviceStatus().contains(key), qPrintable(key + QStringLiteral(" before its first result")));
        }
        QTRY_COMPARE_WITH_TIMEOUT(proxy.deviceStatus().size(), 6, 20000);
        checkCompleteMap(proxy.deviceStatus(), t0, QDateTime::currentMSecsSinceEpoch());
        QCOMPARE(log.maps.size(), 6);
        QTest::qWait(4000);
        QCOMPARE(log.maps.size(), 6);
        manager.stop();                            // Core::shutdown -> Manager::stop()
        QVERIFY(proxy.deviceStatus().isEmpty());
        QCOMPARE(log.maps.size(), 7);
        QTest::qWait(1000);
    }                                              // ~Manager (Core::shutdown deletes it)
    QTest::qWait(500);
    QVERIFY(proxy.deviceStatus().isEmpty());
    QCOMPARE(log.maps.size(), 7);
    QVERIFY2(log.violations.isEmpty(), qPrintable(log.violations.join(QLatin1Char('\n'))));
}

void TestDeviceStatus::sourceWiring()
{
    const auto read = [](const QString &relative) {
        QFile f(QStringLiteral(CORE_SOURCE_DIR "/") + relative);
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    };
    const auto body = [](const QString &text, const QString &signature) {
        const qsizetype start = text.indexOf(signature);
        if (start < 0)
            return QString();
        const qsizetype end = text.indexOf(QStringLiteral("\n}"), start);
        return text.mid(start, end - start);
    };
    const QString core = read(QStringLiteral("core.cpp"));
    const QString shutdown = body(core, QStringLiteral("void Core::shutdown("));
    QVERIFY(!shutdown.isEmpty());
    QVERIFY(shutdown.indexOf(QStringLiteral("m_manager->stop()")) > 0);
    QVERIFY(shutdown.indexOf(QStringLiteral("m_manager->stop()")) < shutdown.indexOf(QStringLiteral("delete m_manager")));

    const QString manager = read(QStringLiteral("manager.cpp"));
    const QString start = body(manager, QStringLiteral("void Manager::start()"));
    const QString stop = body(manager, QStringLiteral("void Manager::stop()"));
    QVERIFY(start.indexOf(QStringLiteral("m_deviceStatus->start()")) > 0);
    QVERIFY(start.indexOf(QStringLiteral("m_deviceStatus->start()")) < start.indexOf(QStringLiteral("m_modbus.connectAll()")));
    QVERIFY(start.indexOf(QStringLiteral("m_deviceStatus->start()")) < start.indexOf(QStringLiteral("m_ms300FaultReader.start()")));
    QVERIFY(stop.indexOf(QStringLiteral("m_deviceStatus->stop()")) > 0);
    QVERIFY(stop.indexOf(QStringLiteral("m_deviceStatus->stop()")) < stop.indexOf(QStringLiteral("m_ms300FaultReader.stop()")));
    QVERIFY(stop.indexOf(QStringLiteral("m_deviceStatus->stop()")) < stop.indexOf(QStringLiteral("m_modbus.disconnectAll()")));
    QVERIFY(manager.contains(QStringLiteral("m_deviceStatus->attachModbus(&m_modbus);")));
    QVERIFY(manager.contains(QStringLiteral("m_deviceStatus->attachMs300(&m_ms300FaultReader);")));

    // The name is the model, never displayName() (address and A/B already in it).
    const QString publisher = read(QStringLiteral("DeviceStatusPublisher.cpp"));
    QVERIFY(!publisher.isEmpty());
    QVERIFY(!publisher.contains(QStringLiteral("displayName(")));
    QVERIFY(publisher.contains(QStringLiteral("proxy->setDeviceStatus(")));

    // Desktop only: in the TAIDAFLOW_IS_WINDOWS_DESKTOP block of Core/CMakeLists.txt.
    const QString cmake = read(QStringLiteral("CMakeLists.txt"));
    const qsizetype desktop = cmake.indexOf(QStringLiteral("if(TAIDAFLOW_IS_WINDOWS_DESKTOP)"));
    const qsizetype endif = cmake.indexOf(QStringLiteral("endif()"), desktop);
    const qsizetype source = cmake.indexOf(QStringLiteral("DeviceStatusPublisher.cpp"));
    QVERIFY(desktop > 0 && source > desktop && source < endif);
}

QTEST_MAIN(TestDeviceStatus)
#include "tst_device_status.moc"
