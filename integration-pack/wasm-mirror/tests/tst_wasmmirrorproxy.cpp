#include "infrastructure/proxy_mirror/wasmmirrorproxy.h"
#include "infrastructure/proxy_mirror/proxymirror.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTest>
#include <QUuid>
#include <QWebSocket>

class RuntimeTestProxy final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int value READ value WRITE setValue NOTIFY valueChanged)

public:
    explicit RuntimeTestProxy(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    int value() const { return m_value; }

    void setValue(int value)
    {
        if (m_value == value) {
            return;
        }
        m_value = value;
        emit valueChanged(value);
    }

signals:
    void valueChanged(int value);
    void actionRequested(int value);

private:
    int m_value = 0;
};

class WasmMirrorProxyTest final : public QObject
{
    Q_OBJECT

private slots:
    void configAcceptsUniqueNamesAndRejectsBlankOrDuplicateNames();
    void createUsesOneEndpointForAllRegisteredProxies();
    void newConnectionReceivesSnapshotBeforeAnyPendingPatch();
    void namedPropertySignalAndPatchNeverTouchPeer();
    void requiredContractMismatchRejectsHandshakeAtomically();
    void destroyingRuntimeClosesClientAndReleasesPort();
};

namespace {

QJsonObject parseMessage(const QList<QVariant> &arguments)
{
    return QJsonDocument::fromJson(
        arguments.constFirst().toString().toUtf8()).object();
}

QJsonObject engineEnvelope(QJsonObject wireEnvelope)
{
    wireEnvelope.insert(QStringLiteral("protocol"),
                        ProxyMirrorHost::ProtocolVersion);
    wireEnvelope.remove(QStringLiteral("mirrorName"));
    wireEnvelope.remove(QStringLiteral("connectionSessionId"));
    return wireEnvelope;
}

QJsonObject wireEnvelope(QJsonObject engineMessage,
                         const QString &mirrorName,
                         const QString &requestSessionId,
                         const QString &connectionSessionId)
{
    engineMessage.insert(QStringLiteral("protocol"),
                         WasmMirrorProxy::WireProtocolVersion);
    engineMessage.insert(QStringLiteral("mirrorName"), mirrorName);
    engineMessage.insert(QStringLiteral("requestSessionId"), requestSessionId);
    engineMessage.insert(QStringLiteral("connectionSessionId"),
                         connectionSessionId);
    return engineMessage;
}

void sendObject(QWebSocket &socket, const QJsonObject &object)
{
    QVERIFY(socket.sendTextMessage(QString::fromUtf8(
        QJsonDocument(object).toJson(QJsonDocument::Compact))) > 0);
}

} // namespace

void WasmMirrorProxyTest::configAcceptsUniqueNamesAndRejectsBlankOrDuplicateNames()
{
    RuntimeTestProxy machine;
    RuntimeTestProxy alarm;
    RuntimeTestProxy duplicate;
    RuntimeTestProxy blank;

    WasmMirrorConfig config;
    QVERIFY(config.addProxy(QStringLiteral("Machine"), machine));
    QVERIFY(config.addProxy(QStringLiteral("Alarm"), alarm));
    QVERIFY(!config.addProxy(QStringLiteral("  "), blank));
    QVERIFY(!config.addProxy(QStringLiteral("Machine"), duplicate));
    QCOMPARE(config.mirrorNames(),
             QStringList({QStringLiteral("Alarm"),
                          QStringLiteral("Machine")}));
}

void WasmMirrorProxyTest::createUsesOneEndpointForAllRegisteredProxies()
{
    RuntimeTestProxy machine;
    RuntimeTestProxy alarm;
    machine.setValue(11);
    alarm.setValue(22);

    WasmMirrorConfig config;
    config.port = 0;
    QVERIFY(config.addProxy(QStringLiteral("Machine"), machine));
    QVERIFY(config.addProxy(QStringLiteral("Alarm"), alarm));

    QString error;
    std::unique_ptr<WasmMirrorProxy> runtime =
        WasmMirrorProxy::create(config, &error);
    QVERIFY2(runtime, qPrintable(error));
    QVERIFY(runtime->actualPort() > 0);
    QCOMPARE(runtime->webSocketUrl().port(), runtime->actualPort());

    ProxyMirrorHost machineContract(machine);
    ProxyMirrorHost alarmContract(alarm);
    QVERIFY(machineContract.isValid());
    QVERIFY(alarmContract.isValid());

    QWebSocket socket;
    QSignalSpy connectedSpy(&socket, &QWebSocket::connected);
    QSignalSpy messageSpy(&socket, &QWebSocket::textMessageReceived);
    socket.open(runtime->webSocketUrl());
    QVERIFY(connectedSpy.wait(2000));

    const QString requestSessionId =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QJsonObject hello{
        {QStringLiteral("type"), QStringLiteral("proxy.hello")},
        {QStringLiteral("protocol"), WasmMirrorProxy::WireProtocolVersion},
        {QStringLiteral("requestSessionId"), requestSessionId},
        {QStringLiteral("mirrors"), QJsonArray{
             QJsonObject{{QStringLiteral("mirrorName"),
                          QStringLiteral("Machine")},
                         {QStringLiteral("contractHash"),
                          machineContract.contractHash()},
                         {QStringLiteral("required"), true}},
             QJsonObject{{QStringLiteral("mirrorName"),
                          QStringLiteral("Alarm")},
                         {QStringLiteral("contractHash"),
                          alarmContract.contractHash()},
                         {QStringLiteral("required"), true}}
         }}
    };
    socket.sendTextMessage(QString::fromUtf8(
        QJsonDocument(hello).toJson(QJsonDocument::Compact)));

    QTRY_VERIFY_WITH_TIMEOUT(messageSpy.count() >= 3, 2000);
    QList<QJsonObject> messages;
    while (!messageSpy.isEmpty()) {
        messages.append(QJsonDocument::fromJson(
            messageSpy.takeFirst().constFirst().toString().toUtf8()).object());
    }
    QCOMPARE(messages.constFirst().value(QStringLiteral("type")).toString(),
             QStringLiteral("proxy.welcome"));
    QCOMPARE(messages.constFirst().value(
                 QStringLiteral("requestSessionId")).toString(),
             requestSessionId);

    QSet<QString> snapshotNames;
    QHash<QString, int> snapshotValues;
    for (const QJsonObject &message : std::as_const(messages)) {
        if (message.value(QStringLiteral("type")).toString()
            != QStringLiteral("proxy.snapshot")) {
            continue;
        }
        const QString name = message.value(
            QStringLiteral("mirrorName")).toString();
        snapshotNames.insert(name);
        snapshotValues.insert(
            name,
            message.value(QStringLiteral("properties"))
                .toObject().value(QStringLiteral("value")).toInt());
    }
    QCOMPARE(snapshotNames,
             QSet<QString>({QStringLiteral("Alarm"),
                            QStringLiteral("Machine")}));
    QCOMPARE(snapshotValues.value(QStringLiteral("Machine")), 11);
    QCOMPARE(snapshotValues.value(QStringLiteral("Alarm")), 22);
}

void WasmMirrorProxyTest::newConnectionReceivesSnapshotBeforeAnyPendingPatch()
{
    RuntimeTestProxy machine;
    WasmMirrorConfig config;
    config.port = 0;
    QVERIFY(config.addProxy(QStringLiteral("Machine"), machine));

    QString error;
    const std::unique_ptr<WasmMirrorProxy> runtime =
        WasmMirrorProxy::create(config, &error);
    QVERIFY2(runtime, qPrintable(error));
    ProxyMirrorHost contract(machine);
    QVERIFY(contract.isValid());

    // Host 已排程 patch，但新 client 尚未取得基準 revision。握手時第一份
    // named state 必須是完整 snapshot，否則 client 會判定 revision gap。
    // 先讓第一個 client 完成握手，才能觀察 broadcast patch 是否誤送到
    // 正在握手的第二個 client。
    QWebSocket existingSocket;
    QSignalSpy existingConnectedSpy(&existingSocket, &QWebSocket::connected);
    QSignalSpy existingMessageSpy(&existingSocket,
                                  &QWebSocket::textMessageReceived);
    existingSocket.open(runtime->webSocketUrl());
    QVERIFY(existingConnectedSpy.wait(2000));
    sendObject(existingSocket, QJsonObject{
        {QStringLiteral("type"), QStringLiteral("proxy.hello")},
        {QStringLiteral("protocol"), WasmMirrorProxy::WireProtocolVersion},
        {QStringLiteral("requestSessionId"),
         QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("mirrors"), QJsonArray{
             QJsonObject{{QStringLiteral("mirrorName"),
                          QStringLiteral("Machine")},
                         {QStringLiteral("contractHash"),
                          contract.contractHash()},
                         {QStringLiteral("required"), true}}
         }}
    });
    QTRY_VERIFY_WITH_TIMEOUT(existingMessageSpy.count() >= 2, 2000);
    existingMessageSpy.clear();
    machine.setValue(7);

    QWebSocket socket;
    QSignalSpy connectedSpy(&socket, &QWebSocket::connected);
    QSignalSpy messageSpy(&socket, &QWebSocket::textMessageReceived);
    socket.open(runtime->webSocketUrl());
    QVERIFY(connectedSpy.wait(2000));

    sendObject(socket, QJsonObject{
        {QStringLiteral("type"), QStringLiteral("proxy.hello")},
        {QStringLiteral("protocol"), WasmMirrorProxy::WireProtocolVersion},
        {QStringLiteral("requestSessionId"),
         QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("mirrors"), QJsonArray{
             QJsonObject{{QStringLiteral("mirrorName"),
                          QStringLiteral("Machine")},
                         {QStringLiteral("contractHash"),
                          contract.contractHash()},
                         {QStringLiteral("required"), true}}
         }}
    });

    QTRY_VERIFY_WITH_TIMEOUT(messageSpy.count() >= 2, 2000);
    const QJsonObject welcome = parseMessage(messageSpy.takeFirst());
    const QJsonObject firstState = parseMessage(messageSpy.takeFirst());
    QCOMPARE(welcome.value(QStringLiteral("type")).toString(),
             QStringLiteral("proxy.welcome"));
    QCOMPARE(firstState.value(QStringLiteral("type")).toString(),
             QStringLiteral("proxy.snapshot"));
    QCOMPARE(firstState.value(QStringLiteral("mirrorName")).toString(),
             QStringLiteral("Machine"));
    QCOMPARE(firstState.value(QStringLiteral("properties")).toObject()
                 .value(QStringLiteral("value")).toInt(),
             7);
}

void WasmMirrorProxyTest::namedPropertySignalAndPatchNeverTouchPeer()
{
    RuntimeTestProxy machine;
    RuntimeTestProxy alarm;
    machine.setValue(10);
    alarm.setValue(20);

    WasmMirrorConfig config;
    config.port = 0;
    QVERIFY(config.addProxy(QStringLiteral("Machine"), machine));
    QVERIFY(config.addProxy(QStringLiteral("Alarm"), alarm));
    QString error;
    const std::unique_ptr<WasmMirrorProxy> runtime =
        WasmMirrorProxy::create(config, &error);
    QVERIFY2(runtime, qPrintable(error));

    RuntimeTestProxy machineReplica;
    RuntimeTestProxy alarmReplica;
    ProxyMirrorClient machineClient(machineReplica);
    ProxyMirrorClient alarmClient(alarmReplica);
    QVERIFY(machineClient.isValid());
    QVERIFY(alarmClient.isValid());

    QWebSocket socket;
    QSignalSpy connectedSpy(&socket, &QWebSocket::connected);
    QSignalSpy messageSpy(&socket, &QWebSocket::textMessageReceived);
    socket.open(runtime->webSocketUrl());
    QVERIFY(connectedSpy.wait(2000));

    machineClient.beginRequestSession();
    alarmClient.beginRequestSession();
    const QString requestSessionId =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    sendObject(socket, QJsonObject{
        {QStringLiteral("type"), QStringLiteral("proxy.hello")},
        {QStringLiteral("protocol"), WasmMirrorProxy::WireProtocolVersion},
        {QStringLiteral("requestSessionId"), requestSessionId},
        {QStringLiteral("mirrors"), QJsonArray{
             QJsonObject{{QStringLiteral("mirrorName"),
                          QStringLiteral("Machine")},
                         {QStringLiteral("contractHash"),
                          machineClient.contractHash()},
                         {QStringLiteral("required"), true}},
             QJsonObject{{QStringLiteral("mirrorName"),
                          QStringLiteral("Alarm")},
                         {QStringLiteral("contractHash"),
                          alarmClient.contractHash()},
                         {QStringLiteral("required"), true}}
         }}
    });
    QTRY_VERIFY_WITH_TIMEOUT(messageSpy.count() >= 3, 2000);

    QString connectionSessionId;
    while (!messageSpy.isEmpty()) {
        const QJsonObject message = parseMessage(messageSpy.takeFirst());
        const QString type = message.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("proxy.welcome")) {
            connectionSessionId = message.value(
                QStringLiteral("connectionSessionId")).toString();
        } else if (type == QStringLiteral("proxy.snapshot")) {
            const QString name = message.value(
                QStringLiteral("mirrorName")).toString();
            const ProxyMirrorApplyResult result =
                name == QStringLiteral("Machine")
                    ? machineClient.applyStateEnvelope(engineEnvelope(message))
                    : alarmClient.applyStateEnvelope(engineEnvelope(message));
            QVERIFY2(result.accepted(), qPrintable(result.error));
        }
    }
    QVERIFY(!connectionSessionId.isEmpty());
    QCOMPARE(machineReplica.value(), 10);
    QCOMPARE(alarmReplica.value(), 20);

    QSignalSpy machinePropertySpy(
        &machineClient, &ProxyMirrorClient::propertyWriteReady);
    machineReplica.setValue(31);
    QCOMPARE(machinePropertySpy.count(), 1);
    sendObject(socket, wireEnvelope(
        machinePropertySpy.takeFirst().constFirst().toJsonObject(),
        QStringLiteral("Machine"), requestSessionId, connectionSessionId));
    QTRY_COMPARE_WITH_TIMEOUT(machine.value(), 31, 2000);
    QCOMPARE(alarm.value(), 20);

    QSignalSpy machineActionSpy(&machine,
                                &RuntimeTestProxy::actionRequested);
    QSignalSpy alarmActionSpy(&alarm,
                              &RuntimeTestProxy::actionRequested);
    QJsonObject signal = machineClient.makeSignalEnvelope(
        QStringLiteral("actionRequested(int)"), {77}, &error);
    QVERIFY2(!signal.isEmpty(), qPrintable(error));
    sendObject(socket, wireEnvelope(signal, QStringLiteral("Machine"),
                                    requestSessionId, connectionSessionId));
    QTRY_COMPARE_WITH_TIMEOUT(machineActionSpy.count(), 1, 2000);
    QCOMPARE(machineActionSpy.constFirst().constFirst().toInt(), 77);
    QCOMPARE(alarmActionSpy.count(), 0);

    // Machine 的 property write 可能先送出自己的 patch；先清空，再觀察 Alarm。
    QTest::qWait(20);
    messageSpy.clear();
    alarm.setValue(42);
    QTRY_VERIFY_WITH_TIMEOUT(!messageSpy.isEmpty(), 2000);
    bool alarmPatchSeen = false;
    while (!messageSpy.isEmpty()) {
        const QJsonObject message = parseMessage(messageSpy.takeFirst());
        if (message.value(QStringLiteral("type")).toString()
                == QStringLiteral("proxy.patch")
            && message.value(QStringLiteral("mirrorName")).toString()
                == QStringLiteral("Alarm")) {
            alarmPatchSeen = true;
            const ProxyMirrorApplyResult result =
                alarmClient.applyStateEnvelope(engineEnvelope(message));
            QVERIFY2(result.accepted(), qPrintable(result.error));
        }
    }
    QVERIFY(alarmPatchSeen);
    QCOMPARE(alarmReplica.value(), 42);
    QCOMPARE(machineReplica.value(), 31);
}

void WasmMirrorProxyTest::requiredContractMismatchRejectsHandshakeAtomically()
{
    RuntimeTestProxy machine;
    RuntimeTestProxy alarm;
    WasmMirrorConfig config;
    config.port = 0;
    QVERIFY(config.addProxy(QStringLiteral("Machine"), machine));
    QVERIFY(config.addProxy(QStringLiteral("Alarm"), alarm));
    QString error;
    const std::unique_ptr<WasmMirrorProxy> runtime =
        WasmMirrorProxy::create(config, &error);
    QVERIFY2(runtime, qPrintable(error));

    ProxyMirrorHost machineContract(machine);
    QWebSocket socket;
    QSignalSpy connectedSpy(&socket, &QWebSocket::connected);
    QSignalSpy messageSpy(&socket, &QWebSocket::textMessageReceived);
    QSignalSpy disconnectedSpy(&socket, &QWebSocket::disconnected);
    socket.open(runtime->webSocketUrl());
    QVERIFY(connectedSpy.wait(2000));

    sendObject(socket, QJsonObject{
        {QStringLiteral("type"), QStringLiteral("proxy.hello")},
        {QStringLiteral("protocol"), WasmMirrorProxy::WireProtocolVersion},
        {QStringLiteral("requestSessionId"),
         QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("mirrors"), QJsonArray{
             QJsonObject{{QStringLiteral("mirrorName"),
                          QStringLiteral("Machine")},
                         {QStringLiteral("contractHash"),
                          machineContract.contractHash()},
                         {QStringLiteral("required"), true}},
             QJsonObject{{QStringLiteral("mirrorName"),
                          QStringLiteral("Alarm")},
                         {QStringLiteral("contractHash"),
                          QStringLiteral("deliberately-wrong")},
                         {QStringLiteral("required"), true}}
         }}
    });

    QTRY_VERIFY_WITH_TIMEOUT(!messageSpy.isEmpty(), 2000);
    const QJsonObject rejection = parseMessage(messageSpy.takeFirst());
    QCOMPARE(rejection.value(QStringLiteral("type")).toString(),
             QStringLiteral("proxy.error"));
    QCOMPARE(rejection.value(QStringLiteral("code")).toString(),
             QStringLiteral("contractMismatch"));
    QCOMPARE(rejection.value(QStringLiteral("mirrorName")).toString(),
             QStringLiteral("Alarm"));
    if (disconnectedSpy.isEmpty()) {
        QVERIFY(disconnectedSpy.wait(2000));
    }
    QTest::qWait(20);
    QCOMPARE(messageSpy.count(), 0);
}

void WasmMirrorProxyTest::destroyingRuntimeClosesClientAndReleasesPort()
{
    RuntimeTestProxy machine;
    WasmMirrorConfig config;
    config.port = 0;
    QVERIFY(config.addProxy(QStringLiteral("Machine"), machine));
    QString error;
    std::unique_ptr<WasmMirrorProxy> runtime =
        WasmMirrorProxy::create(config, &error);
    QVERIFY2(runtime, qPrintable(error));
    const quint16 port = runtime->actualPort();

    QWebSocket socket;
    QSignalSpy connectedSpy(&socket, &QWebSocket::connected);
    QSignalSpy disconnectedSpy(&socket, &QWebSocket::disconnected);
    socket.open(runtime->webSocketUrl());
    QVERIFY(connectedSpy.wait(2000));

    runtime.reset();
    if (disconnectedSpy.isEmpty()) {
        QVERIFY(disconnectedSpy.wait(2000));
    }

    QTcpServer replacement;
    QVERIFY2(replacement.listen(QHostAddress::LocalHost, port),
             qPrintable(replacement.errorString()));
}

QTEST_GUILESS_MAIN(WasmMirrorProxyTest)

#include "tst_wasmmirrorproxy.moc"
