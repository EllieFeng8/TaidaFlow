// w2-072 D1/D2/D5 (review D-003): ModbusClient while a device is offline. Real QModbusTcpClient
// against 127.0.0.1 only: a port where nothing listens (connection refused) and a QTcpServer of
// this test that is opened and closed to connect / drop the device. No device, no fixed port.
//
// How the connection attempts are counted: ModbusClient logs every attempt of an outage as
// "[Modbus] <device> reconnect attempt #N" (info) and emits deviceConnectionChanged(false) when
// an attempt ends; the test records both with their times (message handler + signal).
//
//  (a) 60 read() calls (the per-second poll, shortened to 150 ms steps) while the port refuses:
//      deviceError "not connected" at most once, connection warnings at most once (60 s rate
//      limit), attempts only through the 3 s reconnect timer (>= 2.9 s from the end of one
//      attempt to the start of the next; the former read() connected on every call);
//  (b) connected -> dropped -> reads: "not connected" once; connected again (detail says how
//      long the outage lasted) -> dropped again -> reported once more;
//  plus RepeatedWarningLimiter itself (first at once, then one per interval with the count).
#include <QtTest>
#include <QMutex>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>

#include "Modbus_Client.h"

namespace {
QMutex g_logMutex;
QList<QPair<qint64, QString>> g_log;   // (ms since start, message)
QElapsedTimer g_clock;
QtMessageHandler g_previousHandler = nullptr;

void captureMessages(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    {
        QMutexLocker locker(&g_logMutex);
        g_log.append({g_clock.elapsed(), message});
    }
    if (g_previousHandler)
        g_previousHandler(type, context, message);
}

QList<qint64> logTimes(const QString &needle, qint64 since)
{
    QMutexLocker locker(&g_logMutex);
    QList<qint64> times;
    for (const auto &entry : std::as_const(g_log)) {
        if (entry.first >= since && entry.second.contains(needle))
            times << entry.first;
    }
    return times;
}

quint16 closedPort()
{
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0))
        return 0;
    const quint16 port = probe.serverPort();
    probe.close();
    return port;
}

ModbusClient::DeviceConfig localDevice(quint16 port)
{
    ModbusClient::DeviceConfig config;
    config.device = ModbusClient::Device::Adam6217_203;
    config.name = QStringLiteral("ADAM-6217 B (test)");
    config.host = QStringLiteral("127.0.0.1");
    config.port = port;
    config.unitId = 1;
    config.timeoutMs = 1000;
    config.retryCount = 0;
    return config;
}

struct Recorder
{
    QList<QPair<qint64, QString>> errors;
    QList<QPair<qint64, bool>> connection;   // (time, connected)
    QStringList connectedDetails;

    void attach(ModbusClient &client)
    {
        QObject::connect(&client, &ModbusClient::deviceError, &client,
                         [this](ModbusClient::Device, const QString &message) { errors.append({g_clock.elapsed(), message}); });
        QObject::connect(&client, &ModbusClient::deviceConnectionChanged, &client,
                         [this](ModbusClient::Device, bool connected, const QString &detail) {
                             connection.append({g_clock.elapsed(), connected});
                             if (connected)
                                 connectedDetails << detail;
                         });
    }
    int errorCount(const QString &needle) const
    {
        int n = 0;
        for (const auto &e : errors)
            n += e.second.contains(needle) ? 1 : 0;
        return n;
    }
    int connectedCount() const
    {
        int n = 0;
        for (const auto &c : connection)
            n += c.second ? 1 : 0;
        return n;
    }
};
} // namespace

class TestModbusClientOffline : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void limiterRules();
    void readsWhileRefused();
    void dropReportedOncePerOutage();
};

void TestModbusClientOffline::initTestCase()
{
    qunsetenv("TAIDAFLOW_DEVICE_PROFILE");   // configured addresses only (127.0.0.1)
    g_clock.start();
    g_previousHandler = qInstallMessageHandler(captureMessages);
}

void TestModbusClientOffline::cleanupTestCase()
{
    qInstallMessageHandler(g_previousHandler);
}

void TestModbusClientOffline::limiterRules()
{
    RepeatedWarningLimiter limiter(300);
    qint64 held = -1;
    QVERIFY(limiter.allow(&held));
    QCOMPARE(held, 0LL);
    QCOMPARE(limiter.suffix(held), QString());
    for (int i = 0; i < 9; ++i)
        QVERIFY(!limiter.allow());
    QCOMPARE(limiter.heldBack(), 9LL);
    QTest::qWait(350);
    QVERIFY(limiter.allow(&held));
    QCOMPARE(held, 9LL);
    QVERIFY(limiter.suffix(held).contains(QLatin1String("9 similar warning(s) held back")));
    QCOMPARE(limiter.total(), 11LL);
    QCOMPARE(limiter.totalHeldBack(), 9LL);
    limiter.reset();
    QVERIFY(limiter.allow(&held));   // after reset: at once again
    QCOMPARE(held, 0LL);
}

void TestModbusClientOffline::readsWhileRefused()
{
    const quint16 port = closedPort();
    QVERIFY(port != 0);
    ModbusClient client(QList<ModbusClient::DeviceConfig>{localDevice(port)});
    Recorder rec;
    rec.attach(client);
    const qint64 start = g_clock.elapsed();

    client.connectAll();   // first connection at start: at once (unchanged)
    for (int i = 0; i < 60; ++i) {
        client.read(ModbusClient::Device::Adam6217_203, QModbusDataUnit::InputRegisters, 0, 8);
        QTest::qWait(150);
    }
    const qint64 end = g_clock.elapsed();

    const int notConnected = rec.errorCount(QStringLiteral("not connected"));
    const int connectionWarnings = int(rec.errors.size()) - notConnected;
    const QList<qint64> attempts = logTimes(QStringLiteral("reconnect attempt #"), start);
    QList<qint64> attemptEnds;
    for (const auto &c : rec.connection) {
        if (!c.second)
            attemptEnds << c.first;
    }
    qInfo("60 reads in %lld ms: deviceError total %lld (not connected %d, connection %d); "
          "reconnect attempts %lld at %s ms; attempt ends at %s ms",
          end - start, qint64(rec.errors.size()), notConnected, connectionWarnings, qint64(attempts.size()),
          qPrintable([&] { QStringList s; for (qint64 t : attempts) s << QString::number(t - start); return s.join(','); }()),
          qPrintable([&] { QStringList s; for (qint64 t : attemptEnds) s << QString::number(t - start); return s.join(','); }()));
    for (const auto &e : rec.errors)
        qInfo("  deviceError at %lld ms: %s", e.first - start, qPrintable(e.second));

    QCOMPARE(notConnected, 1);
    QVERIFY2(connectionWarnings <= 1, "connection warnings are limited to one per 60 s");
    QVERIFY(!attemptEnds.isEmpty());
    // Every reconnect attempt starts >= 3 s (timer, 100 ms tolerance) after the previous attempt ended.
    for (qint64 attempt : attempts) {
        qint64 previousEnd = -1;
        for (qint64 e : attemptEnds) {
            if (e <= attempt)
                previousEnd = e;
        }
        QVERIFY(previousEnd >= 0);
        QVERIFY2(attempt - previousEnd >= ModbusClient::kReconnectDelayMs - 100,
                 qPrintable(QStringLiteral("attempt at %1 ms, previous attempt ended at %2 ms")
                                    .arg(attempt - start).arg(previousEnd - start)));
    }
    // 60 reads in ~9 s: at most 1 + 9 / 3 attempts (the former read() tried on every call).
    QVERIFY2(attemptEnds.size() <= 1 + (end - start) / ModbusClient::kReconnectDelayMs,
             qPrintable(QStringLiteral("%1 attempts").arg(attemptEnds.size())));
    client.disconnectAll();
}

void TestModbusClientOffline::dropReportedOncePerOutage()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    const quint16 port = server.serverPort();
    QList<QTcpSocket *> accepted;
    connect(&server, &QTcpServer::newConnection, this, [&] {
        while (QTcpSocket *s = server.nextPendingConnection())
            accepted << s;
    });
    const auto drop = [&] {
        server.close();
        for (QTcpSocket *s : std::as_const(accepted)) {
            s->abort();
            s->deleteLater();
        }
        accepted.clear();
    };

    ModbusClient client(QList<ModbusClient::DeviceConfig>{localDevice(port)});
    Recorder rec;
    rec.attach(client);
    const qint64 start = g_clock.elapsed();
    client.connectAll();
    QTRY_COMPARE_WITH_TIMEOUT(rec.connectedCount(), 1, 5000);
    QCOMPARE(rec.connectedDetails.value(0), QString());   // first connection: no outage detail
    QCOMPARE(rec.errors.size(), qsizetype(0));

    for (int outage = 1; outage <= 2; ++outage) {
        drop();
        QTRY_VERIFY_WITH_TIMEOUT(!rec.connection.isEmpty() && !rec.connection.last().second, 5000);
        for (int i = 0; i < 20; ++i) {
            client.read(ModbusClient::Device::Adam6217_203, QModbusDataUnit::InputRegisters, 0, 8);
            QTest::qWait(50);
        }
        QCOMPARE(rec.errorCount(QStringLiteral("not connected")), outage);
        // Device back: the reconnect timer connects it again (within one 3 s delay + attempt).
        QVERIFY(server.listen(QHostAddress::LocalHost, port));
        QTRY_COMPARE_WITH_TIMEOUT(rec.connectedCount(), outage + 1, 10000);
        QVERIFY2(rec.connectedDetails.last().contains(QLatin1String("connected again after")),
                 qPrintable(rec.connectedDetails.last()));
        qInfo("outage %d: connected again, detail \"%s\"", outage, qPrintable(rec.connectedDetails.last()));
    }
    for (const auto &e : rec.errors)
        qInfo("  deviceError at %lld ms: %s", e.first - start, qPrintable(e.second));
    client.disconnectAll();
    drop();
}

QTEST_GUILESS_MAIN(TestModbusClientOffline)
#include "tst_modbusclient_offline.moc"
