// w2-072 D4/D5c (review I-003): Manager refuses NaN / +Inf / -Inf set values before anything
// else. Real Manager + ModbusClient + TaidaFlowProxy (wired like Core: proxy *SvChanged ->
// Manager slots) against a small Modbus TCP server of this test on 127.0.0.1 (all five ADAM
// sessions point to it; it answers write requests only, reads are left unanswered). MS300 uses a
// serial port name that does not exist. No device, no fixed port.
//
// What is observed on the wire: every Modbus write request frame the server receives
// (function 05/06/0F/10, address). Checks:
//  * NaN, +Inf, -Inf on m1..m4ValueSv and pump2HzSv (through the proxy and directly): no write
//    frame at all, no "VfdRun" start attempt, the proxy SV is put back (finite) at once, and
//    100 more NaN on the same point give no further warning line (rate limited per point);
//  * 30.0 on m1ValueSv and pump2HzSv: one holding-register write each (HR0 raw 1229, HR10 raw
//    2048), and after the pump frequency write succeeded the VFD start is attempted (refused by
//    the DI0 interlock, because no DI was read - logged "VfdRun was not energized");
//  * NaN after that: the SV is put back to the last accepted 30.0, no write.
#include <QtTest>
#include <QMutex>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <cmath>
#include <limits>

#include "TaidaFlowProxy.h"
#include "manager.h"

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

struct WriteFrame
{
    int function = 0;
    int address = 0;
    int value = 0;   // first value
};

// Minimal Modbus TCP server: answers write requests (05, 06, 0F, 10), records them; ignores reads.
class FakeModbusServer : public QObject
{
public:
    bool start() { connect(&m_server, &QTcpServer::newConnection, this, [this] { accept(); });
                   return m_server.listen(QHostAddress::LocalHost, 0); }
    quint16 port() const { return m_server.serverPort(); }
    int connections() const { return m_connections; }
    QList<WriteFrame> writes;

private:
    void accept()
    {
        while (QTcpSocket *socket = m_server.nextPendingConnection()) {
            ++m_connections;
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onData(socket); });
        }
    }
    void onData(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();
        while (buffer.size() >= 7) {
            const int length = (quint8(buffer[4]) << 8) | quint8(buffer[5]);
            if (buffer.size() < 6 + length)
                return;
            const QByteArray frame = buffer.left(6 + length);
            buffer.remove(0, 6 + length);
            const int function = quint8(frame[7]);
            if (function != 0x05 && function != 0x06 && function != 0x0F && function != 0x10)
                continue;   // reads: no answer
            WriteFrame w;
            w.function = function;
            w.address = (quint8(frame[8]) << 8) | quint8(frame[9]);
            if (function == 0x05 || function == 0x06)
                w.value = (quint8(frame[10]) << 8) | quint8(frame[11]);
            else if (function == 0x10 && frame.size() >= 15)
                w.value = (quint8(frame[13]) << 8) | quint8(frame[14]);
            else if (function == 0x0F && frame.size() >= 14)
                w.value = quint8(frame[13]) & 1;
            writes << w;
            // Response: MBAP (same transaction/unit) + function + address + value/quantity.
            QByteArray reply = frame.left(4);
            reply += char(0);
            reply += char(6);
            reply += frame[6];
            reply += frame.mid(7, 5);
            socket->write(reply);
        }
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    int m_connections = 0;
};

int holdingWrites(const QList<WriteFrame> &writes)
{
    int n = 0;
    for (const WriteFrame &w : writes)
        n += (w.function == 0x06 || w.function == 0x10) ? 1 : 0;
    return n;
}
} // namespace

class TestManagerSvGuard : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void nonFiniteRefusedValidWritten();

private:
    QTemporaryDir m_dir;
    QString m_oldCwd;
};

void TestManagerSvGuard::initTestCase()
{
    QVERIFY(m_dir.isValid());
    qunsetenv("TAIDAFLOW_DEVICE_PROFILE");
    m_oldCwd = QDir::currentPath();
    QVERIFY(QDir::setCurrent(m_dir.path()));   // TaidaFlowSettings.ini of Manager goes here
    g_previousHandler = qInstallMessageHandler(captureMessages);
}

void TestManagerSvGuard::cleanupTestCase()
{
    qInstallMessageHandler(g_previousHandler);
    QDir::setCurrent(m_oldCwd);
}

void TestManagerSvGuard::nonFiniteRefusedValidWritten()
{
    FakeModbusServer server;
    QVERIFY(server.start());

    Manager::DeviceSettings devices;
    for (ModbusClient::DeviceConfig &config : devices.modbusDevices) {
        config.host = QStringLiteral("127.0.0.1");   // never the plant addresses
        config.port = server.port();
    }
    devices.ms300.serialPort = QStringLiteral("TAIDAFLOW_TEST_NO_SUCH_PORT");

    TaidaFlowProxy proxy;
    Manager manager(&proxy, nullptr, devices);
    // As Core::init wires them.
    connect(&proxy, &TaidaFlowProxy::m1ValueSvChanged, &manager, &Manager::setM1Sv);
    connect(&proxy, &TaidaFlowProxy::m2ValueSvChanged, &manager, &Manager::setM2Sv);
    connect(&proxy, &TaidaFlowProxy::m3ValueSvChanged, &manager, &Manager::setM3Sv);
    connect(&proxy, &TaidaFlowProxy::m4ValueSvChanged, &manager, &Manager::setM4Sv);
    connect(&proxy, &TaidaFlowProxy::pump2HzSvChanged, &manager, &Manager::setPump2HzSv);

    manager.start();
    QTRY_COMPARE_WITH_TIMEOUT(server.connections(), 5, 10000);
    QTest::qWait(300);
    QCOMPARE(holdingWrites(server.writes), 0);

    using Setter = void (TaidaFlowProxy::*)(double);
    using Getter = double (TaidaFlowProxy::*)() const;
    const QList<QPair<Setter, Getter>> svs{
        {&TaidaFlowProxy::setM1ValueSv, &TaidaFlowProxy::m1ValueSv},
        {&TaidaFlowProxy::setM2ValueSv, &TaidaFlowProxy::m2ValueSv},
        {&TaidaFlowProxy::setM3ValueSv, &TaidaFlowProxy::m3ValueSv},
        {&TaidaFlowProxy::setM4ValueSv, &TaidaFlowProxy::m4ValueSv},
        {&TaidaFlowProxy::setPump2HzSv, &TaidaFlowProxy::pump2HzSv},
    };
    const double bad[] = {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                          -std::numeric_limits<double>::infinity()};

    // 1. NaN / +Inf / -Inf through the proxy (the mirror path) and directly on the slots.
    const int guardBefore = logCount(QStringLiteral("[SV guard]"));
    for (const auto &sv : svs) {
        for (const double value : bad) {
            (proxy.*sv.first)(value);
            QVERIFY2(std::isfinite((proxy.*sv.second)()), "the proxy SV is put back at once");
        }
    }
    for (const double value : bad) {
        manager.setM1Sv(value);
        manager.setM2Sv(value);
        manager.setM3Sv(value);
        manager.setM4Sv(value);
        manager.setPump2HzSv(value);
    }
    const int guardAfterFirst = logCount(QStringLiteral("[SV guard]")) - guardBefore;
    for (int i = 0; i < 100; ++i)
        manager.setPump2HzSv(std::numeric_limits<double>::quiet_NaN());
    QTest::qWait(1500);   // any write / VFD start would reach the server meanwhile
    const int guardLines = logCount(QStringLiteral("[SV guard]")) - guardBefore;
    qInfo("non-finite phase: write frames %lld, [SV guard] warning lines %d (after the first round %d), "
          "VfdRun start attempts %d",
          qint64(server.writes.size()), guardLines, guardAfterFirst,
          logCount(QStringLiteral("VfdRun was not energized")));
    QCOMPARE(holdingWrites(server.writes), 0);
    QCOMPARE(logCount(QStringLiteral("VfdRun was not energized")), 0);
    QCOMPARE(logCount(QStringLiteral("[Modbus][Write request]")), 0);
    QCOMPARE(guardLines, 5);   // one per point (M1..M4, Pump2Hz), the rest held back
    QCOMPARE(guardAfterFirst, 5);

    // 2. Valid values are written as before.
    proxy.setM1ValueSv(30.0);
    proxy.setPump2HzSv(30.0);
    QTRY_COMPARE_WITH_TIMEOUT(holdingWrites(server.writes), 2, 5000);
    bool m1 = false;
    bool pump = false;
    for (const WriteFrame &w : std::as_const(server.writes)) {
        m1 = m1 || (w.address == 0 && w.value == 1229);      // 30 % of 4095
        pump = pump || (w.address == 10 && w.value == 2048); // 30 Hz of 60 Hz = 4095 / 2
    }
    QVERIFY(m1);
    QVERIFY(pump);
    // Frequency write accepted -> VFD start requested -> refused by the DI0 interlock (no DI read).
    QTRY_COMPARE_WITH_TIMEOUT(logCount(QStringLiteral("VfdRun was not energized")), 1, 5000);

    // 3. NaN after a valid write: put back to the last accepted value.
    proxy.setM1ValueSv(std::numeric_limits<double>::quiet_NaN());
    QCOMPARE(proxy.m1ValueSv(), 30.0);
    proxy.setPump2HzSv(-std::numeric_limits<double>::infinity());
    QCOMPARE(proxy.pump2HzSv(), 30.0);
    QTest::qWait(500);
    QCOMPARE(holdingWrites(server.writes), 2);
    QCOMPARE(logCount(QStringLiteral("VfdRun was not energized")), 1);

    manager.stop();
}

QTEST_MAIN(TestManagerSvGuard)
#include "tst_manager_sv_guard.moc"
