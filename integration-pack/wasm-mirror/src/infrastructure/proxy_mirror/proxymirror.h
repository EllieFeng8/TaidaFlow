// 功能：定義通用 Proxy Mirror protocol 的 Host、Client 與狀態套用介面。
// 它從 Qt meta-object 自動發現可同步 Q_PROPERTY 與 typed event signal，負責
// contract、snapshot/patch、revision 與參數驗證；新增一般功能不應修改此檔。
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <memory>

enum class ProxyMirrorApplyCode
{
    Applied,
    Duplicate,
    Stale,
    Rejected
};

struct ProxyMirrorApplyResult
{
    ProxyMirrorApplyCode code = ProxyMirrorApplyCode::Rejected;
    QString error;
    quint64 revision = 0;
    quint64 sequence = 0;

    bool accepted() const
    {
        return code == ProxyMirrorApplyCode::Applied
               || code == ProxyMirrorApplyCode::Duplicate
               || code == ProxyMirrorApplyCode::Stale;
    }
};

// Native-side mirror for the authoritative Proxy object.
//
// The contract is discovered from the QObject meta-object:
//   * every STORED, non-CONSTANT property declared by the concrete Proxy class
//     is mirrored as state (STORED false is a target-local UI overlay);
//   * every mirrored property must be readable, writable and have either a
//     zero-argument NOTIFY or a one-argument NOTIFY matching its property type;
//   * every signal declared by that class that is not a Q_PROPERTY NOTIFY is
//     a remotely callable UI -> Core event; no naming prefix is required.
class ProxyMirrorHost final : public QObject
{
    Q_OBJECT

public:
    // Version 2 adds the server-issued connection session used by
    // proxy.welcome/proxy.signal.  It is intentionally separate from the
    // legacy React snapshot protocol, which remains version 1.
    static constexpr int ProtocolVersion = 2;

    explicit ProxyMirrorHost(QObject &authoritativeProxy,
                             QObject *parent = nullptr);
    ~ProxyMirrorHost() override;

    bool isValid() const;
    QString validationError() const;
    QString contractHash() const;
    QJsonObject contractDescriptor() const;
    QString stateSessionId() const;
    quint64 revision() const;

    // Flushes pending property notifications first, then returns a coherent
    // full state envelope suitable for a new or reconnecting client.
    QJsonObject makeSnapshot();

    // Re-emits a validated request signal on the authoritative Proxy. Existing
    // local Core signal/slot connections therefore remain the only handlers.
    ProxyMirrorApplyResult applySignalEnvelope(const QJsonObject &envelope);

    // Applies writable Q_PROPERTY values sent by a WebAssembly replica. The
    // normal WRITE setter and NOTIFY signal are used, so existing SystemCore
    // signal/slot connections observe the change without transport code.
    ProxyMirrorApplyResult applyPropertyEnvelope(const QJsonObject &envelope);

    // A WebSocket connection owns its request session.  Once that connection
    // is gone, the transport nonce prevents old envelopes from being reused,
    // so the sequence entry can be released instead of growing forever.
    void releaseRequestSession(const QString &requestSessionId);

signals:
    void patchReady(const QJsonObject &envelope);
    void requestApplied(const QString &sessionId,
                        quint64 sequence,
                        const QString &signalSignature);
    void envelopeRejected(const QString &error);

private slots:
    void handlePropertyNotification();

private:
    class Private;
    std::unique_ptr<Private> d;
};

// WebAssembly-side mirror for the UI-facing Proxy object. Snapshot/patch
// envelopes are applied atomically through each Q_PROPERTY WRITE setter.
// Feature code never supplies a state Map or a feature-specific adapter.
class ProxyMirrorClient final : public QObject
{
    Q_OBJECT

public:
    explicit ProxyMirrorClient(QObject &mirroredProxy,
                               QObject *parent = nullptr);
    ~ProxyMirrorClient() override;

    bool isValid() const;
    QString validationError() const;
    QString contractHash() const;
    QJsonObject contractDescriptor() const;
    QString sessionId() const;
    quint64 revision() const;
    bool hasState() const;

    // A WebSocket connection owns one request sequence.  Starting a new
    // connection prevents commands from an earlier connection being replayed.
    void beginRequestSession();
    QJsonObject makeHello() const;
    QJsonObject makeSignalEnvelope(const QString &signalSignature,
                                   const QVariantList &arguments,
                                   QString *errorMessage = nullptr);
    ProxyMirrorApplyResult applyStateEnvelope(const QJsonObject &envelope);

    // Runtime 在 handshake、重連或更新本機 transport overlay 時，可暫停
    // 產生遠端 property envelope；QObject setter/NOTIFY 本身仍正常運作。
    void setLocalPropertyWritesEnabled(bool enabled);
    bool localPropertyWritesEnabled() const;

    // Call after the network connection is replaced. The Proxy values are
    // retained, but the next accepted state message must be a full snapshot.
    void requireSnapshot();

signals:
    void stateApplied(quint64 revision, const QStringList &propertyNames);
    // Emitted only for a local WebAssembly-side property change. State being
    // applied from a snapshot/patch is suppressed to prevent an echo loop.
    void propertyWriteReady(const QJsonObject &envelope);
    void resyncRequired();
    void envelopeRejected(const QString &error);

private slots:
    void handleLocalPropertyNotification();

private:
    class Private;
    std::unique_ptr<Private> d;
};
