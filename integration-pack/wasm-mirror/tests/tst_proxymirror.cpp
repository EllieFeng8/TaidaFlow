#include "infrastructure/proxy_mirror/proxymirror.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QVariantMap>

#include <memory>
#include <utility>

class MirrorTestProxy final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int count READ count WRITE setCount NOTIFY countChanged)
    Q_PROPERTY(QString label READ label WRITE setLabel NOTIFY labelChanged)
    Q_PROPERTY(QString platformName READ platformName CONSTANT)
    Q_PROPERTY(QString localOverlay READ localOverlay
               NOTIFY localOverlayChanged STORED false)

public:
    explicit MirrorTestProxy(QString platformName, QObject *parent = nullptr)
        : QObject(parent)
        , m_platformName(std::move(platformName))
    {
    }

    int count() const { return m_count; }
    QString label() const { return m_label; }
    QString platformName() const { return m_platformName; }
    QString localOverlay() const { return m_localOverlay; }

    void setCount(int value)
    {
        if (m_count == value) {
            return;
        }
        m_count = value;
        emit countChanged(m_count);
    }

    void setLabel(const QString &value)
    {
        if (m_label == value) {
            return;
        }
        m_label = value;
        emit labelChanged();
    }

    void setLocalOverlay(const QString &value)
    {
        if (m_localOverlay == value) {
            return;
        }
        m_localOverlay = value;
        emit localOverlayChanged();
    }

    void setAuthoritativeState(int count, const QString &label)
    {
        const bool countChangedValue = m_count != count;
        const bool labelChangedValue = m_label != label;
        m_count = count;
        m_label = label;
        if (countChangedValue) {
            emit countChanged(m_count);
        }
        if (labelChangedValue) {
            emit labelChanged();
        }
    }

signals:
    // Qt 慣用的單一 value 參數 NOTIFY；ProxyMirror 必須像零參數
    // NOTIFY 一樣同步 property，並在遠端重新發出目前值。
    void countChanged(int value);
    void labelChanged();
    void localOverlayChanged();
    void startTimer(int seconds, const QString &name);

private:
    int m_count = 0;
    QString m_label;
    const QString m_platformName;
    QString m_localOverlay;
};

namespace {

QJsonObject waitForPatch(QSignalSpy &patchSpy)
{
    if (patchSpy.isEmpty()) {
        patchSpy.wait(1000);
    }
    if (patchSpy.isEmpty()) {
        return {};
    }
    return patchSpy.takeFirst().constFirst().toJsonObject();
}

} // namespace

class ProxyMirrorTest final : public QObject
{
    Q_OBJECT

private slots:
    void snapshotRoundTrip();
    void patchRoundTrip();
    void requestSignalRoundTrip();
    void propertyWriteRoundTripDoesNotEcho();
    void duplicateSequenceIsNotReplayed();
    void contractMismatchIsRejected();
    void revisionGapRequiresSnapshot();
    void commandFromPreviousHostStateSessionIsRejected();
    void requestSequenceGapIsRejectedWithoutConsumingSequence();
    void malformedStateIsRejectedAtomically();
    void requiredSnapshotCannotRollBackTheSameSession();
    void mirrorSurvivesTargetDestruction();
    void storedFalsePropertyIsTargetLocal();
    void localPropertyWritesCanBeSuspendedWithoutBlockingNotify();
};

void ProxyMirrorTest::snapshotRoundTrip()
{
    MirrorTestProxy nativeProxy(QStringLiteral("native"));
    nativeProxy.setAuthoritativeState(17, QStringLiteral("ready"));
    ProxyMirrorHost host(nativeProxy);

    MirrorTestProxy webProxy(QStringLiteral("webassembly"));
    ProxyMirrorClient client(webProxy);

    QVERIFY2(host.isValid(), qPrintable(host.validationError()));
    QVERIFY2(client.isValid(), qPrintable(client.validationError()));
    QCOMPARE(host.contractHash(), client.contractHash());

    QSignalSpy countChangedSpy(&webProxy, &MirrorTestProxy::countChanged);

    const QJsonObject snapshot = host.makeSnapshot();
    QCOMPARE(snapshot.value(QStringLiteral("type")).toString(),
             QStringLiteral("proxy.snapshot"));
    QVERIFY(!snapshot.value(QStringLiteral("properties"))
                 .toObject()
                 .contains(QStringLiteral("platformName")));

    const ProxyMirrorApplyResult result = client.applyStateEnvelope(snapshot);
    QVERIFY(result.code == ProxyMirrorApplyCode::Applied);
    QCOMPARE(webProxy.count(), 17);
    QCOMPARE(countChangedSpy.count(), 1);
    QCOMPARE(countChangedSpy.constFirst().constFirst().toInt(), 17);
    QCOMPARE(webProxy.label(), QStringLiteral("ready"));
    QCOMPARE(webProxy.platformName(), QStringLiteral("webassembly"));
    QCOMPARE(client.sessionId(), host.stateSessionId());
    QCOMPARE(client.revision(), host.revision());
    QVERIFY(client.hasState());
}

void ProxyMirrorTest::patchRoundTrip()
{
    MirrorTestProxy nativeProxy(QStringLiteral("native"));
    ProxyMirrorHost host(nativeProxy);
    MirrorTestProxy webProxy(QStringLiteral("webassembly"));
    ProxyMirrorClient client(webProxy);
    QVERIFY(client.applyStateEnvelope(host.makeSnapshot()).accepted());

    QSignalSpy patchSpy(&host, &ProxyMirrorHost::patchReady);
    QSignalSpy countChangedSpy(&webProxy, &MirrorTestProxy::countChanged);
    nativeProxy.setAuthoritativeState(8, QStringLiteral("running"));
    const QJsonObject patch = waitForPatch(patchSpy);
    QVERIFY2(!patch.isEmpty(), "Host did not publish a property patch.");
    QCOMPARE(patch.value(QStringLiteral("type")).toString(),
             QStringLiteral("proxy.patch"));

    const ProxyMirrorApplyResult result = client.applyStateEnvelope(patch);
    QVERIFY(result.code == ProxyMirrorApplyCode::Applied);
    QCOMPARE(webProxy.count(), 8);
    QCOMPARE(countChangedSpy.count(), 1);
    QCOMPARE(countChangedSpy.constFirst().constFirst().toInt(), 8);
    QCOMPARE(webProxy.label(), QStringLiteral("running"));
    QCOMPARE(client.revision(), host.revision());
}

void ProxyMirrorTest::requestSignalRoundTrip()
{
    MirrorTestProxy nativeProxy(QStringLiteral("native"));
    ProxyMirrorHost host(nativeProxy);
    MirrorTestProxy webProxy(QStringLiteral("webassembly"));
    ProxyMirrorClient client(webProxy);
    QVERIFY(client.applyStateEnvelope(host.makeSnapshot()).accepted());

    QSignalSpy requestSpy(&nativeProxy, &MirrorTestProxy::startTimer);
    QString error;
    const QJsonObject envelope = client.makeSignalEnvelope(
        QStringLiteral("startTimer(int,QString)"),
        {12, QStringLiteral("countdown")}, &error);
    QVERIFY2(!envelope.isEmpty(), qPrintable(error));
    QCOMPARE(envelope.value(QStringLiteral("type")).toString(),
             QStringLiteral("proxy.signal"));

    const ProxyMirrorApplyResult result = host.applySignalEnvelope(envelope);
    QVERIFY2(result.code == ProxyMirrorApplyCode::Applied,
             qPrintable(result.error));
    QCOMPARE(requestSpy.count(), 1);
    QCOMPARE(requestSpy.constFirst().at(0).toInt(), 12);
    QCOMPARE(requestSpy.constFirst().at(1).toString(),
             QStringLiteral("countdown"));

}

void ProxyMirrorTest::propertyWriteRoundTripDoesNotEcho()
{
    MirrorTestProxy nativeProxy(QStringLiteral("native"));
    ProxyMirrorHost host(nativeProxy);
    MirrorTestProxy webProxy(QStringLiteral("webassembly"));
    ProxyMirrorClient client(webProxy);
    QVERIFY(client.applyStateEnvelope(host.makeSnapshot()).accepted());

    QSignalSpy propertyWriteSpy(&client,
                                &ProxyMirrorClient::propertyWriteReady);
    QSignalSpy nativeCountSpy(&nativeProxy,
                              &MirrorTestProxy::countChanged);
    QSignalSpy patchSpy(&host, &ProxyMirrorHost::patchReady);

    webProxy.setCount(23);
    QCOMPARE(propertyWriteSpy.count(), 1);
    const QJsonObject propertyEnvelope =
        propertyWriteSpy.constFirst().constFirst().toJsonObject();
    QCOMPARE(propertyEnvelope.value(QStringLiteral("type")).toString(),
             QStringLiteral("proxy.properties"));
    QCOMPARE(propertyEnvelope.value(QStringLiteral("properties"))
                 .toObject().value(QStringLiteral("count")).toInt(), 23);

    const ProxyMirrorApplyResult applied =
        host.applyPropertyEnvelope(propertyEnvelope);
    QVERIFY2(applied.code == ProxyMirrorApplyCode::Applied,
             qPrintable(applied.error));
    QCOMPARE(nativeProxy.count(), 23);
    QCOMPARE(nativeCountSpy.count(), 1);
    QCOMPARE(nativeCountSpy.constFirst().constFirst().toInt(), 23);

    const QJsonObject patch = waitForPatch(patchSpy);
    QVERIFY(!patch.isEmpty());
    QVERIFY(client.applyStateEnvelope(patch).accepted());
    QCOMPARE(webProxy.count(), 23);
    // Applying the authoritative echo must not create a second outbound
    // property write, even when a setter's NOTIFY carries the value.
    QCOMPARE(propertyWriteSpy.count(), 1);

    webProxy.setLabel(QStringLiteral("edited in web"));
    QCOMPARE(propertyWriteSpy.count(), 2);
    const QJsonObject zeroArgumentNotifyEnvelope =
        propertyWriteSpy.constLast().constFirst().toJsonObject();
    QVERIFY(host.applyPropertyEnvelope(zeroArgumentNotifyEnvelope).accepted());
    QCOMPARE(nativeProxy.label(), QStringLiteral("edited in web"));
    const QJsonObject labelPatch = waitForPatch(patchSpy);
    QVERIFY(!labelPatch.isEmpty());
    QVERIFY(client.applyStateEnvelope(labelPatch).accepted());
    QCOMPARE(propertyWriteSpy.count(), 2);
}

void ProxyMirrorTest::duplicateSequenceIsNotReplayed()
{
    MirrorTestProxy nativeProxy(QStringLiteral("native"));
    ProxyMirrorHost host(nativeProxy);
    MirrorTestProxy webProxy(QStringLiteral("webassembly"));
    ProxyMirrorClient client(webProxy);
    QVERIFY(client.applyStateEnvelope(host.makeSnapshot()).accepted());
    QSignalSpy requestSpy(&nativeProxy, &MirrorTestProxy::startTimer);

    const QJsonObject envelope = client.makeSignalEnvelope(
        QStringLiteral("startTimer(int,QString)"),
        {3, QStringLiteral("once")});
    QVERIFY(!envelope.isEmpty());
    QVERIFY(host.applySignalEnvelope(envelope).code
            == ProxyMirrorApplyCode::Applied);
    const ProxyMirrorApplyResult duplicate = host.applySignalEnvelope(envelope);
    QVERIFY(duplicate.code == ProxyMirrorApplyCode::Duplicate);
    QCOMPARE(requestSpy.count(), 1);
}

void ProxyMirrorTest::contractMismatchIsRejected()
{
    MirrorTestProxy nativeProxy(QStringLiteral("native"));
    nativeProxy.setAuthoritativeState(99, QStringLiteral("authoritative"));
    ProxyMirrorHost host(nativeProxy);
    MirrorTestProxy webProxy(QStringLiteral("webassembly"));
    webProxy.setAuthoritativeState(4, QStringLiteral("unchanged"));
    ProxyMirrorClient client(webProxy);

    QJsonObject snapshot = host.makeSnapshot();
    snapshot.insert(QStringLiteral("contractHash"),
                    QStringLiteral("incompatible-contract"));
    const ProxyMirrorApplyResult result = client.applyStateEnvelope(snapshot);
    QVERIFY(result.code == ProxyMirrorApplyCode::Rejected);
    QCOMPARE(webProxy.count(), 4);
    QCOMPARE(webProxy.label(), QStringLiteral("unchanged"));
    QVERIFY(!client.hasState());
}

void ProxyMirrorTest::revisionGapRequiresSnapshot()
{
    MirrorTestProxy nativeProxy(QStringLiteral("native"));
    ProxyMirrorHost host(nativeProxy);
    MirrorTestProxy webProxy(QStringLiteral("webassembly"));
    ProxyMirrorClient client(webProxy);
    QVERIFY(client.applyStateEnvelope(host.makeSnapshot()).accepted());

    QSignalSpy patchSpy(&host, &ProxyMirrorHost::patchReady);
    QSignalSpy resyncSpy(&client, &ProxyMirrorClient::resyncRequired);
    nativeProxy.setAuthoritativeState(5, QStringLiteral("new"));
    QJsonObject gap = waitForPatch(patchSpy);
    QVERIFY(!gap.isEmpty());
    gap.insert(QStringLiteral("baseRevision"), QStringLiteral("999"));
    gap.insert(QStringLiteral("revision"), QStringLiteral("1000"));

    const ProxyMirrorApplyResult gapResult = client.applyStateEnvelope(gap);
    QVERIFY(gapResult.code == ProxyMirrorApplyCode::Rejected);
    QCOMPARE(resyncSpy.count(), 1);
    QCOMPARE(webProxy.count(), 0);

    const ProxyMirrorApplyResult recovered = client.applyStateEnvelope(
        host.makeSnapshot());
    QVERIFY(recovered.code == ProxyMirrorApplyCode::Applied);
    QCOMPARE(webProxy.count(), 5);
    QCOMPARE(webProxy.label(), QStringLiteral("new"));
}

void ProxyMirrorTest::commandFromPreviousHostStateSessionIsRejected()
{
    MirrorTestProxy nativeProxy(QStringLiteral("native"));
    ProxyMirrorHost previousHost(nativeProxy);
    MirrorTestProxy webProxy(QStringLiteral("webassembly"));
    ProxyMirrorClient client(webProxy);
    QVERIFY(client.applyStateEnvelope(previousHost.makeSnapshot()).accepted());

    const QJsonObject previousSessionCommand = client.makeSignalEnvelope(
        QStringLiteral("startTimer(int,QString)"),
        {6, QStringLiteral("old-host")});
    QVERIFY(!previousSessionCommand.isEmpty());

    ProxyMirrorHost currentHost(nativeProxy);
    QVERIFY(currentHost.stateSessionId() != previousHost.stateSessionId());
    client.beginRequestSession();
    client.requireSnapshot();
    QVERIFY(client.applyStateEnvelope(currentHost.makeSnapshot()).accepted());
    QSignalSpy requestSpy(&nativeProxy, &MirrorTestProxy::startTimer);

    const ProxyMirrorApplyResult result = currentHost.applySignalEnvelope(
        previousSessionCommand);
    QVERIFY(result.code == ProxyMirrorApplyCode::Rejected);
    QCOMPARE(requestSpy.count(), 0);

    const QJsonObject currentSessionCommand = client.makeSignalEnvelope(
        QStringLiteral("startTimer(int,QString)"),
        {7, QStringLiteral("current-host")});
    QVERIFY(currentHost.applySignalEnvelope(currentSessionCommand).code
            == ProxyMirrorApplyCode::Applied);
    QCOMPARE(requestSpy.count(), 1);
}

void ProxyMirrorTest::requestSequenceGapIsRejectedWithoutConsumingSequence()
{
    MirrorTestProxy nativeProxy(QStringLiteral("native"));
    ProxyMirrorHost host(nativeProxy);
    MirrorTestProxy webProxy(QStringLiteral("webassembly"));
    ProxyMirrorClient client(webProxy);
    QVERIFY(client.applyStateEnvelope(host.makeSnapshot()).accepted());
    QSignalSpy requestSpy(&nativeProxy, &MirrorTestProxy::startTimer);

    const QJsonObject first = client.makeSignalEnvelope(
        QStringLiteral("startTimer(int,QString)"),
        {1, QStringLiteral("first")});
    const QJsonObject second = client.makeSignalEnvelope(
        QStringLiteral("startTimer(int,QString)"),
        {2, QStringLiteral("second")});
    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());

    const ProxyMirrorApplyResult gap = host.applySignalEnvelope(second);
    QVERIFY(gap.code == ProxyMirrorApplyCode::Rejected);
    QCOMPARE(requestSpy.count(), 0);

    QVERIFY(host.applySignalEnvelope(first).code
            == ProxyMirrorApplyCode::Applied);
    QVERIFY(host.applySignalEnvelope(second).code
            == ProxyMirrorApplyCode::Applied);
    QCOMPARE(requestSpy.count(), 2);
    QCOMPARE(requestSpy.at(0).at(1).toString(), QStringLiteral("first"));
    QCOMPARE(requestSpy.at(1).at(1).toString(), QStringLiteral("second"));

    const QJsonObject dropped = client.makeSignalEnvelope(
        QStringLiteral("startTimer(int,QString)"),
        {99, QStringLiteral("dropped-frame")});
    QVERIFY(!dropped.isEmpty());

    // A transport replacement starts a fresh request session.  A frame lost
    // on the old socket therefore cannot leave the replacement connection in
    // a permanent sequence gap.
    const QString previousRequestSession = dropped.value(
        QStringLiteral("requestSessionId")).toString();
    host.releaseRequestSession(previousRequestSession);
    client.beginRequestSession();
    client.requireSnapshot();
    QVERIFY(client.applyStateEnvelope(host.makeSnapshot()).accepted());
    const QJsonObject afterReconnect = client.makeSignalEnvelope(
        QStringLiteral("startTimer(int,QString)"),
        {3, QStringLiteral("after-reconnect")});
    QCOMPARE(afterReconnect.value(QStringLiteral("sequence")).toString(),
             QStringLiteral("1"));
    QVERIFY(host.applySignalEnvelope(afterReconnect).code
            == ProxyMirrorApplyCode::Applied);
    QCOMPARE(requestSpy.count(), 3);
}

void ProxyMirrorTest::malformedStateIsRejectedAtomically()
{
    MirrorTestProxy nativeProxy(QStringLiteral("native"));
    nativeProxy.setAuthoritativeState(10, QStringLiteral("ready"));
    ProxyMirrorHost host(nativeProxy);
    MirrorTestProxy webProxy(QStringLiteral("webassembly"));
    webProxy.setAuthoritativeState(1, QStringLiteral("unchanged"));
    ProxyMirrorClient client(webProxy);
    QSignalSpy resyncSpy(&client, &ProxyMirrorClient::resyncRequired);

    const QJsonObject validSnapshot = host.makeSnapshot();
    QJsonObject malformedSnapshot = validSnapshot;
    QJsonObject snapshotProperties = malformedSnapshot
                                         .value(QStringLiteral("properties"))
                                         .toObject();
    snapshotProperties.insert(QStringLiteral("count"), 99);
    snapshotProperties.insert(QStringLiteral("label"), QJsonArray{1, 2});
    malformedSnapshot.insert(QStringLiteral("properties"), snapshotProperties);

    const ProxyMirrorApplyResult snapshotResult = client.applyStateEnvelope(
        malformedSnapshot);
    QVERIFY(snapshotResult.code == ProxyMirrorApplyCode::Rejected);
    QCOMPARE(resyncSpy.count(), 1);
    QCOMPARE(webProxy.count(), 1);
    QCOMPARE(webProxy.label(), QStringLiteral("unchanged"));
    QVERIFY(!client.hasState());

    QVERIFY(client.applyStateEnvelope(validSnapshot).accepted());
    QCOMPARE(webProxy.count(), 10);
    QCOMPARE(webProxy.label(), QStringLiteral("ready"));

    QSignalSpy patchSpy(&host, &ProxyMirrorHost::patchReady);
    nativeProxy.setAuthoritativeState(20, QStringLiteral("next"));
    QJsonObject malformedPatch = waitForPatch(patchSpy);
    QVERIFY(!malformedPatch.isEmpty());
    QJsonObject patchProperties = malformedPatch
                                      .value(QStringLiteral("properties"))
                                      .toObject();
    patchProperties.insert(QStringLiteral("count"), 21);
    patchProperties.insert(QStringLiteral("label"), QJsonObject{
        {QStringLiteral("not"), QStringLiteral("a string")}
    });
    malformedPatch.insert(QStringLiteral("properties"), patchProperties);

    const ProxyMirrorApplyResult patchResult = client.applyStateEnvelope(
        malformedPatch);
    QVERIFY(patchResult.code == ProxyMirrorApplyCode::Rejected);
    QCOMPARE(resyncSpy.count(), 2);
    QCOMPARE(webProxy.count(), 10);
    QCOMPARE(webProxy.label(), QStringLiteral("ready"));
    QCOMPARE(client.revision(), 1ULL);
}

void ProxyMirrorTest::requiredSnapshotCannotRollBackTheSameSession()
{
    MirrorTestProxy nativeProxy(QStringLiteral("native"));
    nativeProxy.setAuthoritativeState(1, QStringLiteral("old"));
    ProxyMirrorHost host(nativeProxy);
    MirrorTestProxy webProxy(QStringLiteral("webassembly"));
    ProxyMirrorClient client(webProxy);
    const QJsonObject oldSnapshot = host.makeSnapshot();
    QVERIFY(client.applyStateEnvelope(oldSnapshot).accepted());

    QSignalSpy patchSpy(&host, &ProxyMirrorHost::patchReady);
    nativeProxy.setAuthoritativeState(2, QStringLiteral("new"));
    const QJsonObject patch = waitForPatch(patchSpy);
    QVERIFY(!patch.isEmpty());
    QVERIFY(client.applyStateEnvelope(patch).accepted());
    QCOMPARE(webProxy.count(), 2);
    QCOMPARE(client.revision(), 2ULL);

    QSignalSpy resyncSpy(&client, &ProxyMirrorClient::resyncRequired);
    MirrorTestProxy replacementProxy(QStringLiteral("replacement"));
    replacementProxy.setAuthoritativeState(99, QStringLiteral("replacement"));
    ProxyMirrorHost replacementHost(replacementProxy);
    const QJsonObject replacementSnapshot = replacementHost.makeSnapshot();

    const ProxyMirrorApplyResult unexpectedSession =
        client.applyStateEnvelope(replacementSnapshot);
    QVERIFY(unexpectedSession.code == ProxyMirrorApplyCode::Rejected);
    QCOMPARE(resyncSpy.count(), 1);
    QCOMPARE(webProxy.count(), 2);

    const ProxyMirrorApplyResult result = client.applyStateEnvelope(oldSnapshot);
    QVERIFY(result.code == ProxyMirrorApplyCode::Rejected);
    QCOMPARE(resyncSpy.count(), 2);
    QCOMPARE(webProxy.count(), 2);
    QCOMPARE(webProxy.label(), QStringLiteral("new"));
    QCOMPARE(client.revision(), 2ULL);

    QVERIFY(client.applyStateEnvelope(replacementSnapshot).accepted());
    QCOMPARE(webProxy.count(), 99);
    QCOMPARE(webProxy.label(), QStringLiteral("replacement"));
    QCOMPARE(client.revision(), 1ULL);
}

void ProxyMirrorTest::mirrorSurvivesTargetDestruction()
{
    auto authoritativeProxy = std::make_unique<MirrorTestProxy>(
        QStringLiteral("native"));
    auto host = std::make_unique<ProxyMirrorHost>(*authoritativeProxy);
    authoritativeProxy->setAuthoritativeState(3, QStringLiteral("pending"));
    authoritativeProxy.reset();
    QCoreApplication::processEvents();
    QVERIFY(!host->isValid());
    QVERIFY(host->makeSnapshot().isEmpty());
    host.reset();

    MirrorTestProxy sourceProxy(QStringLiteral("source"));
    ProxyMirrorHost sourceHost(sourceProxy);
    const QJsonObject sourceSnapshot = sourceHost.makeSnapshot();

    auto mirroredProxy = std::make_unique<MirrorTestProxy>(
        QStringLiteral("webassembly"));
    auto client = std::make_unique<ProxyMirrorClient>(
        *mirroredProxy);
    mirroredProxy.reset();
    QVERIFY(!client->isValid());
    const ProxyMirrorApplyResult result = client->applyStateEnvelope(
        sourceSnapshot);
    QVERIFY(result.code == ProxyMirrorApplyCode::Rejected);
    client.reset();
}

void ProxyMirrorTest::storedFalsePropertyIsTargetLocal()
{
    MirrorTestProxy nativeProxy(QStringLiteral("native"));
    nativeProxy.setAuthoritativeState(4, QStringLiteral("shared"));
    nativeProxy.setLocalOverlay(QStringLiteral("desktop overlay"));
    ProxyMirrorHost host(nativeProxy);

    MirrorTestProxy webProxy(QStringLiteral("webassembly"));
    webProxy.setLocalOverlay(QStringLiteral("browser overlay"));
    ProxyMirrorClient client(webProxy);
    QVERIFY2(host.isValid(), qPrintable(host.validationError()));
    QVERIFY2(client.isValid(), qPrintable(client.validationError()));

    QStringList contractProperties;
    const QJsonArray descriptors = host.contractDescriptor()
                                       .value(QStringLiteral("properties"))
                                       .toArray();
    for (const QJsonValue &descriptor : descriptors) {
        contractProperties.append(
            descriptor.toObject().value(QStringLiteral("name")).toString());
    }
    QCOMPARE(contractProperties, QStringList({QStringLiteral("count"),
                                               QStringLiteral("label")}));

    const QJsonObject snapshot = host.makeSnapshot();
    const QJsonObject properties = snapshot.value(QStringLiteral("properties"))
                                       .toObject();
    QVERIFY(!properties.contains(QStringLiteral("platformName")));
    QVERIFY(!properties.contains(QStringLiteral("localOverlay")));
    QVERIFY(client.applyStateEnvelope(snapshot).accepted());
    QCOMPARE(webProxy.localOverlay(), QStringLiteral("browser overlay"));

    QSignalSpy patchSpy(&host, &ProxyMirrorHost::patchReady);
    nativeProxy.setLocalOverlay(QStringLiteral("changed desktop overlay"));
    QTest::qWait(10);
    QCOMPARE(patchSpy.count(), 0);
}

void ProxyMirrorTest::localPropertyWritesCanBeSuspendedWithoutBlockingNotify()
{
    MirrorTestProxy nativeProxy(QStringLiteral("native"));
    ProxyMirrorHost host(nativeProxy);
    MirrorTestProxy webProxy(QStringLiteral("webassembly"));
    ProxyMirrorClient client(webProxy);
    QVERIFY(client.applyStateEnvelope(host.makeSnapshot()).accepted());

    QSignalSpy notifySpy(&webProxy, &MirrorTestProxy::countChanged);
    QSignalSpy propertyWriteSpy(&client,
                                &ProxyMirrorClient::propertyWriteReady);

    client.setLocalPropertyWritesEnabled(false);
    QVERIFY(!client.localPropertyWritesEnabled());
    webProxy.setCount(7);
    QCOMPARE(notifySpy.count(), 1);
    QCOMPARE(propertyWriteSpy.count(), 0);

    client.setLocalPropertyWritesEnabled(true);
    webProxy.setCount(8);
    QCOMPARE(notifySpy.count(), 2);
    QCOMPARE(propertyWriteSpy.count(), 1);
    QCOMPARE(propertyWriteSpy.constFirst().constFirst().toJsonObject()
                 .value(QStringLiteral("sequence")).toString(),
             QStringLiteral("1"));
}

QTEST_MAIN(ProxyMirrorTest)

#include "tst_proxymirror.moc"
