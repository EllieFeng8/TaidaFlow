#include "infrastructure/proxy_mirror/wasmmirrorproxy.h"

#include "infrastructure/proxy_mirror/proxymirror.h"
#include "infrastructure/proxy_mirror/proxyrequestrelay.h"

#include <QAbstractSocket>
#include <QDebug>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <QWebSocket>

#if !defined(Q_OS_WASM)
#  include <QWebSocketServer>
#endif

#include <algorithm>
#include <utility>
#include <vector>

namespace {

QJsonObject decorateEnvelope(QJsonObject envelope,
                             const QString &mirrorName)
{
    envelope.insert(QStringLiteral("protocol"),
                    WasmMirrorProxy::WireProtocolVersion);
    envelope.insert(QStringLiteral("mirrorName"), mirrorName);
    return envelope;
}

QJsonObject normalizeEnvelope(QJsonObject envelope)
{
    envelope.insert(QStringLiteral("protocol"),
                    ProxyMirrorHost::ProtocolVersion);
    envelope.remove(QStringLiteral("mirrorName"));
    envelope.remove(QStringLiteral("connectionSessionId"));
    return envelope;
}

QString normalizedPath(QString path)
{
    path = path.trimmed();
    if (path.isEmpty()) {
        return QStringLiteral("/");
    }
    if (!path.startsWith(QLatin1Char('/'))) {
        path.prepend(QLatin1Char('/'));
    }
    return path;
}

constexpr int ReconnectDelays[] = {250, 500, 1000, 2000, 4000, 5000};

} // namespace

bool WasmMirrorConfig::addProxy(const QString &mirrorName,
                                QObject &uiProxy,
                                WasmMirrorProxyOptions options)
{
    const QString normalizedName = mirrorName.trimmed();
    if (normalizedName.isEmpty()) {
        m_validationError = QObject::tr("Proxy Mirror name must not be blank.");
        return false;
    }
    const auto duplicate = std::find_if(
        m_registrations.cbegin(), m_registrations.cend(),
        [&normalizedName](const Registration &registration) {
            return registration.mirrorName == normalizedName;
        });
    if (duplicate != m_registrations.cend()) {
        m_validationError = QObject::tr("Duplicate Proxy Mirror name: %1")
                                .arg(normalizedName);
        return false;
    }
    const auto duplicateObject = std::find_if(
        m_registrations.cbegin(), m_registrations.cend(),
        [&uiProxy](const Registration &registration) {
            return registration.proxy == &uiProxy;
        });
    if (duplicateObject != m_registrations.cend()) {
        m_validationError = QObject::tr(
            "The same Proxy object cannot be registered more than once.");
        return false;
    }

    m_registrations.append(
        {normalizedName, QPointer<QObject>(&uiProxy), std::move(options)});
    m_validationError.clear();
    return true;
}

QStringList WasmMirrorConfig::mirrorNames() const
{
    QStringList names;
    names.reserve(m_registrations.size());
    for (const Registration &registration : m_registrations) {
        names.append(registration.mirrorName);
    }
    names.sort();
    return names;
}

QString WasmMirrorConfig::validationError() const
{
    return m_validationError;
}

class WasmMirrorProxy::Private
{
public:
    struct Entry
    {
        QString mirrorName;
        QPointer<QObject> proxy;
        WasmMirrorProxyOptions options;
#if defined(Q_OS_WASM)
        std::unique_ptr<ProxyMirrorClient> client;
        bool snapshotReady = false;
        bool transportReady = false;
#else
        std::unique_ptr<ProxyMirrorHost> host;
#endif
    };

#if !defined(Q_OS_WASM)
    struct ConnectionState
    {
        QString requestSessionId;
        QString connectionSessionId;
        QSet<QString> acceptedMirrors;
    };
#endif

    Private(WasmMirrorProxy *owner, const WasmMirrorConfig &config)
        : q(owner)
        , hostName(config.host.trimmed())
        , configuredPort(config.port)
        , path(normalizedPath(config.path))
        , allowedOrigins(config.allowedOrigins)
#if !defined(Q_OS_WASM)
        , server(QStringLiteral("WasmMirrorProxy"),
                 QWebSocketServer::NonSecureMode)
#endif
    {
#if defined(Q_OS_WASM)
        reconnectTimer.setSingleShot(true);
        handshakeTimer.setSingleShot(true);
        QObject::connect(&reconnectTimer, &QTimer::timeout,
                         q, [this] { openSocket(); });
        QObject::connect(&handshakeTimer, &QTimer::timeout,
                         q, [this] {
            failAndReconnect(
                {}, QObject::tr("Proxy Mirror handshake timed out."));
        });
#endif
    }

    bool addRegistration(const QString &mirrorName,
                         QObject &proxy,
                         const WasmMirrorProxyOptions &options,
                         QString *error)
    {
        Entry entry;
        entry.mirrorName = mirrorName;
        entry.proxy = &proxy;
        entry.options = options;
#if defined(Q_OS_WASM)
        entry.client = std::make_unique<ProxyMirrorClient>(proxy, q);
        if (!entry.client->isValid()) {
            if (error) {
                *error = QObject::tr("Proxy %1 has an invalid mirror contract: %2")
                             .arg(mirrorName, entry.client->validationError());
            }
            return false;
        }
#else
        entry.host = std::make_unique<ProxyMirrorHost>(proxy, q);
        if (!entry.host->isValid()) {
            if (error) {
                *error = QObject::tr("Proxy %1 has an invalid mirror contract: %2")
                             .arg(mirrorName, entry.host->validationError());
            }
            return false;
        }
#endif

#if !defined(Q_OS_WASM)
        ProxyMirrorHost *host = entry.host.get();
        QObject::connect(host, &ProxyMirrorHost::patchReady, q,
                         [this, mirrorName](const QJsonObject &patch) {
            broadcast(mirrorName, decorateEnvelope(patch, mirrorName));
        });
#endif
        entries.push_back(std::move(entry));

        QObject::connect(&proxy, &QObject::destroyed, q,
                         [this, mirrorName] {
            const Entry *entry = findEntry(mirrorName);
            if (!entry) {
                return;
            }
            const QString message = QObject::tr(
                "Registered Proxy %1 was destroyed before the Mirror runtime.")
                                        .arg(mirrorName);
#if defined(Q_OS_WASM)
            contractRejected = entry->options.required;
            failAndReconnect(mirrorName, message);
#else
            const QList<QWebSocket *> sockets = connections.keys();
            for (QWebSocket *clientSocket : sockets) {
                rejectConnection(clientSocket,
                                 QStringLiteral("proxyDestroyed"),
                                 message,
                                 mirrorName);
            }
#endif
        });

#if defined(Q_OS_WASM)
        Entry &storedEntry = entries.back();
        ProxyMirrorClient *client = storedEntry.client.get();
        QObject::connect(client, &ProxyMirrorClient::stateApplied, q,
                         [this, mirrorName] {
            Entry *entry = findEntry(mirrorName);
            if (!entry) {
                return;
            }
            entry->snapshotReady = true;
            setProxyTransportState(*entry, true, {});
            updateReady();
            reconnectAttempt = 0;
        });
        QObject::connect(client, &ProxyMirrorClient::propertyWriteReady, q,
                         [this, mirrorName](QJsonObject envelope) {
            sendProxyEnvelope(mirrorName, std::move(envelope));
        });
        QObject::connect(client, &ProxyMirrorClient::resyncRequired, q,
                         [this, mirrorName] {
            failAndReconnect(
                mirrorName,
                QObject::tr("Proxy state synchronization was interrupted."));
        });
        QObject::connect(client, &ProxyMirrorClient::envelopeRejected, q,
                         [this, mirrorName](const QString &message) {
            emit q->transportError(mirrorName, message);
        });

        QString relayError;
        if (!connectProxyMirrorEvents(
                proxy, *q,
                [this, mirrorName](const QString &signalSignature,
                                   const QVariantList &arguments) {
                    sendSignal(mirrorName, signalSignature, arguments);
                },
                &relayError)) {
            entries.pop_back();
            if (error) {
                *error = QObject::tr("Proxy %1 is not registered for signal relay: %2")
                             .arg(mirrorName, relayError);
            }
            return false;
        }
#endif
        return true;
    }

    Entry *findEntry(const QString &mirrorName)
    {
        const auto found = std::find_if(
            entries.begin(), entries.end(),
            [&mirrorName](const Entry &entry) {
                return entry.mirrorName == mirrorName;
            });
        return found == entries.end() ? nullptr : &*found;
    }

    const Entry *findEntry(const QString &mirrorName) const
    {
        const auto found = std::find_if(
            entries.cbegin(), entries.cend(),
            [&mirrorName](const Entry &entry) {
                return entry.mirrorName == mirrorName;
            });
        return found == entries.cend() ? nullptr : &*found;
    }

    bool start(QString *error)
    {
        if (entries.empty()) {
            if (error) {
                *error = QObject::tr("At least one Proxy must be registered.");
            }
            return false;
        }
        if (hostName.isEmpty()) {
            if (error) {
                *error = QObject::tr("Proxy Mirror host must not be blank.");
            }
            return false;
        }

#if defined(Q_OS_WASM)
        if (configuredPort == 0) {
            if (error) {
                *error = QObject::tr(
                    "WebAssembly requires a fixed Proxy Mirror port or discovery.");
            }
            return false;
        }
        actualPort = configuredPort;
        started = true;
        for (Entry &entry : entries) {
            setProxyTransportState(
                entry, false,
                QObject::tr("Connecting to the Qt desktop Core..."));
        }
        openSocket();
        return true;
#else
        QHostAddress bindAddress;
        if (hostName.compare(QStringLiteral("localhost"),
                             Qt::CaseInsensitive) == 0) {
            bindAddress = QHostAddress::LocalHost;
        } else if (!bindAddress.setAddress(hostName)) {
            if (error) {
                *error = QObject::tr("Proxy Mirror host is not a numeric address: %1")
                             .arg(hostName);
            }
            return false;
        }
        if (!bindAddress.isLoopback()) {
            if (error) {
                *error = QObject::tr(
                    "Proxy Mirror currently permits loopback addresses only.");
            }
            return false;
        }
        if (!server.listen(bindAddress, configuredPort)) {
            if (error) {
                *error = QObject::tr("Could not listen for Proxy Mirror clients on %1:%2: %3")
                             .arg(hostName)
                             .arg(configuredPort)
                             .arg(server.errorString());
            }
            return false;
        }
        actualPort = server.serverPort();
        QObject::connect(&server, &QWebSocketServer::newConnection,
                         q, [this] { acceptConnections(); });
        ready = true;
        return true;
#endif
    }

    void stop()
    {
#if defined(Q_OS_WASM)
        started = false;
        reconnectTimer.stop();
        handshakeTimer.stop();
        requestSessionId.clear();
        connectionSessionId.clear();
        acceptedMirrors.clear();
        setReady(false);
        for (Entry &entry : entries) {
            entry.snapshotReady = false;
            entry.client->requireSnapshot();
            entry.client->setLocalPropertyWritesEnabled(false);
            setProxyTransportState(
                entry, false,
                QObject::tr("Proxy Mirror transport stopped."));
        }
        if (socket) {
            QObject::disconnect(socket.get(), nullptr, q, nullptr);
            socket->close();
            socket.reset();
        }
#else
        ready = false;
        const QList<QWebSocket *> sockets = connections.keys();
        for (QWebSocket *socket : sockets) {
            if (!socket) {
                continue;
            }
            QObject::disconnect(socket, nullptr, q, nullptr);
            socket->close(QWebSocketProtocol::CloseCodeGoingAway,
                          QStringLiteral("Proxy Mirror stopped"));
            delete socket;
        }
        connections.clear();
        server.close();
#endif
    }

#if defined(Q_OS_WASM)
    void ensureSocket()
    {
        if (socket) {
            return;
        }
        socket = std::make_unique<QWebSocket>();
        QObject::connect(socket.get(), &QWebSocket::connected, q,
                         [this] { beginHandshake(); });
        QObject::connect(socket.get(), &QWebSocket::textMessageReceived,
                         q, [this](const QString &message) {
            handleClientMessage(message);
        });
        QObject::connect(socket.get(), &QWebSocket::disconnected, q,
                         [this] {
            connectionSessionId.clear();
            requestSessionId.clear();
            acceptedMirrors.clear();
            setReady(false);
            for (Entry &entry : entries) {
                entry.snapshotReady = false;
                entry.client->requireSnapshot();
                entry.client->setLocalPropertyWritesEnabled(false);
                setProxyTransportState(
                    entry, false,
                    contractRejected
                        ? QObject::tr("Proxy Mirror contract was rejected.")
                        : QObject::tr("Qt desktop Core connection was lost."));
            }
            if (started && !contractRejected) {
                scheduleReconnect();
            }
        });
        QObject::connect(socket.get(), &QWebSocket::stateChanged, q,
                         [this](QAbstractSocket::SocketState state) {
            if (state == QAbstractSocket::UnconnectedState
                && started && !contractRejected) {
                scheduleReconnect();
            }
        });
        QObject::connect(socket.get(), &QWebSocket::errorOccurred, q,
                         [this](QAbstractSocket::SocketError) {
            const QString message = socket ? socket->errorString()
                                           : QObject::tr("Proxy Mirror socket error.");
            failAndReconnect({}, message);
        });
    }

    void openSocket()
    {
        if (!started || contractRejected) {
            return;
        }
        ensureSocket();
        if (socket->state() == QAbstractSocket::UnconnectedState) {
            socket->open(q->webSocketUrl());
        }
    }

    void scheduleReconnect()
    {
        if (!started || contractRejected || reconnectTimer.isActive()) {
            return;
        }
        const int index = std::min(
            reconnectAttempt,
            static_cast<int>(std::size(ReconnectDelays)) - 1);
        ++reconnectAttempt;
        reconnectTimer.start(ReconnectDelays[index]);
    }

    bool sendJson(const QJsonObject &message)
    {
        return socket
               && socket->state() == QAbstractSocket::ConnectedState
               && !message.isEmpty()
               && socket->sendTextMessage(QString::fromUtf8(
                      QJsonDocument(message).toJson(QJsonDocument::Compact))) > 0;
    }

    void beginHandshake()
    {
        requestSessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        handshakeTimer.start(5000);
        connectionSessionId.clear();
        acceptedMirrors.clear();
        setReady(false);
        QJsonArray mirrors;
        for (Entry &entry : entries) {
            entry.client->beginRequestSession();
            entry.client->requireSnapshot();
            entry.client->setLocalPropertyWritesEnabled(false);
            entry.snapshotReady = false;
            setProxyTransportState(
                entry, false,
                QObject::tr("Synchronizing with the Qt desktop Core..."));
            mirrors.append(QJsonObject{
                {QStringLiteral("mirrorName"), entry.mirrorName},
                {QStringLiteral("contractHash"), entry.client->contractHash()},
                {QStringLiteral("required"), entry.options.required}
            });
        }
        if (!sendJson(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("proxy.hello")},
                {QStringLiteral("protocol"),
                 WasmMirrorProxy::WireProtocolVersion},
                {QStringLiteral("requestSessionId"), requestSessionId},
                {QStringLiteral("mirrors"), mirrors}
            })) {
            failAndReconnect({},
                             QObject::tr("Could not start Proxy Mirror synchronization."));
        }
    }

    void handleWelcome(const QJsonObject &message)
    {
        if (message.value(QStringLiteral("protocol")).toInt(-1)
                != WasmMirrorProxy::WireProtocolVersion
            || message.value(QStringLiteral("requestSessionId")).toString()
                != requestSessionId
            || !message.value(QStringLiteral("mirrors")).isArray()) {
            failAndReconnect({}, QObject::tr("Invalid Proxy Mirror welcome message."));
            return;
        }
        const QString newConnectionSession = message.value(
            QStringLiteral("connectionSessionId")).toString();
        if (newConnectionSession.isEmpty()) {
            failAndReconnect({}, QObject::tr("Proxy Mirror connection session is missing."));
            return;
        }

        QSet<QString> accepted;
        for (const QJsonValue &value : message.value(
                 QStringLiteral("mirrors")).toArray()) {
            const QJsonObject object = value.toObject();
            const QString name = object.value(
                QStringLiteral("mirrorName")).toString();
            const Entry *entry = findEntry(name);
            if (!entry
                || object.value(QStringLiteral("contractHash")).toString()
                    != entry->client->contractHash()
                || accepted.contains(name)) {
                failAndReconnect(name,
                                 QObject::tr("Proxy Mirror welcome contract is invalid."));
                return;
            }
            accepted.insert(name);
        }
        for (Entry &entry : entries) {
            if (entry.options.required && !accepted.contains(entry.mirrorName)) {
                failAndReconnect(entry.mirrorName,
                                 QObject::tr("Required Proxy was not accepted."));
                return;
            }
        }
        acceptedMirrors = accepted;
        connectionSessionId = newConnectionSession;
        // 若所有 Proxy 都是 optional 且 server 沒有接受任何一個，沒有
        // snapshot 事件可觸發 updateReady；Welcome 本身就足以完成握手。
        updateReady();
    }

    void handleState(const QJsonObject &message)
    {
        const QString mirrorName = message.value(
            QStringLiteral("mirrorName")).toString();
        Entry *entry = findEntry(mirrorName);
        if (connectionSessionId.isEmpty() || !entry
            || !acceptedMirrors.contains(mirrorName)) {
            failAndReconnect(mirrorName,
                             QObject::tr("Proxy Mirror state arrived before a valid welcome."));
            return;
        }
        const ProxyMirrorApplyResult result = entry->client->applyStateEnvelope(
            normalizeEnvelope(message));
        if (!result.accepted()) {
            failAndReconnect(mirrorName, result.error);
        }
    }

    void handleClientMessage(const QString &text)
    {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(
            text.toUtf8(), &parseError);
        if (!document.isObject()) {
            failAndReconnect({}, QObject::tr("Invalid Proxy Mirror JSON: %1")
                                    .arg(parseError.errorString()));
            return;
        }
        const QJsonObject message = document.object();
        const QString type = message.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("proxy.welcome")) {
            handleWelcome(message);
        } else if (type == QStringLiteral("proxy.snapshot")
                   || type == QStringLiteral("proxy.patch")) {
            handleState(message);
        } else if (type == QStringLiteral("proxy.error")) {
            const QString mirrorName = message.value(
                QStringLiteral("mirrorName")).toString();
            const QString code = message.value(QStringLiteral("code")).toString();
            const QString error = message.value(
                QStringLiteral("message")).toString();
            if (code == QStringLiteral("contractMismatch")
                || code == QStringLiteral("protocolMismatch")) {
                contractRejected = true;
            }
            failAndReconnect(mirrorName, error);
        }
    }

    void sendProxyEnvelope(const QString &mirrorName, QJsonObject envelope)
    {
        if (!ready || connectionSessionId.isEmpty()
            || !acceptedMirrors.contains(mirrorName)) {
            const QString message = QObject::tr(
                "The Qt desktop Core is offline; the Proxy update was not sent.");
            if (Entry *entry = findEntry(mirrorName)) {
                setProxyTransportState(*entry, false, message);
            }
            emit q->transportError(mirrorName, message);
            return;
        }
        envelope = decorateEnvelope(std::move(envelope), mirrorName);
        envelope.insert(QStringLiteral("requestSessionId"), requestSessionId);
        envelope.insert(QStringLiteral("connectionSessionId"),
                        connectionSessionId);
        if (!sendJson(envelope)) {
            failAndReconnect(mirrorName,
                             QObject::tr("Proxy update could not be sent."));
        }
    }

    void sendSignal(const QString &mirrorName,
                    const QString &signature,
                    const QVariantList &arguments)
    {
        Entry *entry = findEntry(mirrorName);
        if (!entry) {
            return;
        }
        if (!ready || connectionSessionId.isEmpty()
            || !acceptedMirrors.contains(mirrorName)) {
            const QString message = QObject::tr(
                "The Qt desktop Core is offline; the Proxy event was not sent.");
            emit q->transportError(mirrorName, message);
            return;
        }
        QString error;
        QJsonObject envelope = entry->client->makeSignalEnvelope(
            signature, arguments, &error);
        if (envelope.isEmpty()) {
            emit q->transportError(mirrorName, error);
            return;
        }
        sendProxyEnvelope(mirrorName, std::move(envelope));
    }

    void setProxyTransportState(Entry &entry,
                                bool state,
                                const QString &message)
    {
        const bool changed = entry.transportReady != state;
        entry.transportReady = state;
        if (entry.options.transportStateHandler) {
            entry.options.transportStateHandler(state, message);
        }
        if (changed) {
            emit q->proxyReadyChanged(entry.mirrorName, state);
        }
    }

    void setReady(bool state)
    {
        if (ready == state) {
            return;
        }
        ready = state;
        emit q->readyChanged(state);
    }

    void updateReady()
    {
        const bool allRequiredReady = std::all_of(
            entries.cbegin(), entries.cend(),
            [this](const Entry &entry) {
                return !entry.options.required
                       || (acceptedMirrors.contains(entry.mirrorName)
                           && entry.snapshotReady);
            });
        const bool nextReady = !connectionSessionId.isEmpty()
                               && allRequiredReady;
        setReady(nextReady);
        if (nextReady) {
            handshakeTimer.stop();
        }
        for (Entry &entry : entries) {
            entry.client->setLocalPropertyWritesEnabled(
                nextReady && acceptedMirrors.contains(entry.mirrorName)
                && entry.snapshotReady);
        }
    }

    void failAndReconnect(const QString &mirrorName,
                          const QString &message)
    {
        qWarning().noquote()
            << "Proxy Mirror client resynchronizing:"
            << "mirror=" << mirrorName << message;
        setReady(false);
        handshakeTimer.stop();
        for (Entry &entry : entries) {
            entry.client->setLocalPropertyWritesEnabled(false);
        }
        if (Entry *entry = findEntry(mirrorName)) {
            setProxyTransportState(*entry, false, message);
        } else {
            for (Entry &registered : entries) {
                setProxyTransportState(registered, false, message);
            }
        }
        emit q->transportError(mirrorName, message);
        if (socket) {
            socket->close();
        }
    }
#endif

#if !defined(Q_OS_WASM)
    bool isAllowedOrigin(const QString &origin) const
    {
        return origin.isEmpty() || allowedOrigins.isEmpty()
               || allowedOrigins.contains(origin);
    }

    void acceptConnections()
    {
        while (server.hasPendingConnections()) {
            QWebSocket *socket = server.nextPendingConnection();
            if (!socket) {
                continue;
            }
            if (!isAllowedOrigin(socket->origin())
                || socket->requestUrl().path() != path) {
                socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                              QStringLiteral("Origin or path is not allowed"));
                socket->deleteLater();
                continue;
            }
            connections.insert(socket, {});
            QObject::connect(socket, &QWebSocket::textMessageReceived,
                             q, [this, socket](const QString &message) {
                handleMessage(socket, message);
            });
            QObject::connect(socket, &QWebSocket::disconnected,
                             q, [this, socket] {
                const ConnectionState state = connections.take(socket);
                if (!state.requestSessionId.isEmpty()) {
                    for (const QString &mirrorName : state.acceptedMirrors) {
                        if (Entry *entry = findEntry(mirrorName)) {
                            entry->host->releaseRequestSession(
                                state.requestSessionId);
                        }
                    }
                }
                socket->deleteLater();
            });
        }
    }

    bool sendJson(QWebSocket *socket, const QJsonObject &object) const
    {
        return socket
               && socket->state() == QAbstractSocket::ConnectedState
               && socket->sendTextMessage(QString::fromUtf8(
                      QJsonDocument(object).toJson(QJsonDocument::Compact))) > 0;
    }

    void sendError(QWebSocket *socket,
                   const QString &code,
                   const QString &message,
                   const QString &mirrorName = {})
    {
        QJsonObject error{
            {QStringLiteral("type"), QStringLiteral("proxy.error")},
            {QStringLiteral("protocol"),
             WasmMirrorProxy::WireProtocolVersion},
            {QStringLiteral("code"), code},
            {QStringLiteral("message"), message}
        };
        if (!mirrorName.isEmpty()) {
            error.insert(QStringLiteral("mirrorName"), mirrorName);
        }
        sendJson(socket, error);
    }

    void rejectConnection(QWebSocket *socket,
                          const QString &code,
                          const QString &message,
                          const QString &mirrorName = {})
    {
        qWarning().noquote()
            << "Proxy Mirror rejected client:"
            << code << message
            << "mirror=" << mirrorName
            << "origin=" << (socket ? socket->origin() : QString())
            << "path=" << (socket ? socket->requestUrl().path() : QString());
        sendError(socket, code, message, mirrorName);
        socket->close(QWebSocketProtocol::CloseCodePolicyViolated, message);
    }

    void handleHello(QWebSocket *socket, const QJsonObject &message)
    {
        ConnectionState &state = connections[socket];
        if (!state.connectionSessionId.isEmpty()) {
            rejectConnection(socket, QStringLiteral("duplicateHandshake"),
                             QObject::tr("Proxy Mirror handshake already completed."));
            return;
        }
        if (message.value(QStringLiteral("protocol")).toInt(-1)
                != WasmMirrorProxy::WireProtocolVersion
            || !message.value(QStringLiteral("mirrors")).isArray()) {
            rejectConnection(socket, QStringLiteral("protocolMismatch"),
                             QObject::tr("Proxy Mirror wire protocol mismatch."));
            return;
        }
        const QString requestSessionId = message.value(
            QStringLiteral("requestSessionId")).toString();
        const QUuid parsedRequestSession(requestSessionId);
        bool requestSessionInUse = false;
        for (auto connection = connections.constBegin();
             connection != connections.constEnd(); ++connection) {
            if (connection.key() != socket
                && connection.value().requestSessionId == requestSessionId) {
                requestSessionInUse = true;
                break;
            }
        }
        if (parsedRequestSession.isNull()
            || parsedRequestSession.toString(QUuid::WithoutBraces)
                   .compare(requestSessionId, Qt::CaseInsensitive) != 0
            || requestSessionInUse) {
            rejectConnection(socket, QStringLiteral("invalidRequestSession"),
                             QObject::tr("Proxy Mirror request session is invalid or already active."));
            return;
        }

        struct ClientContract {
            QString hash;
            bool required = true;
        };
        QHash<QString, ClientContract> clientContracts;
        for (const QJsonValue &value : message.value(
                 QStringLiteral("mirrors")).toArray()) {
            if (!value.isObject()) {
                rejectConnection(socket, QStringLiteral("invalidMirrors"),
                                 QObject::tr("Proxy Mirror list is invalid."));
                return;
            }
            const QJsonObject object = value.toObject();
            const QString name = object.value(
                QStringLiteral("mirrorName")).toString().trimmed();
            const QString hash = object.value(
                QStringLiteral("contractHash")).toString();
            if (name.isEmpty() || hash.isEmpty()
                || clientContracts.contains(name)) {
                rejectConnection(socket, QStringLiteral("invalidMirrors"),
                                 QObject::tr("Proxy Mirror names and contracts must be unique."),
                                 name);
                return;
            }
            clientContracts.insert(
                name,
                {hash, object.value(QStringLiteral("required")).toBool(true)});
        }

        QSet<QString> accepted;
        for (const Entry &entry : entries) {
            if (!entry.proxy) {
                if (entry.options.required) {
                    rejectConnection(socket, QStringLiteral("proxyDestroyed"),
                                     QObject::tr("Required Proxy %1 was destroyed.")
                                         .arg(entry.mirrorName),
                                     entry.mirrorName);
                    return;
                }
                continue;
            }
            const auto client = clientContracts.constFind(entry.mirrorName);
            if (client == clientContracts.constEnd()) {
                if (entry.options.required) {
                    rejectConnection(socket, QStringLiteral("contractMismatch"),
                                     QObject::tr("Required Proxy %1 is missing.")
                                         .arg(entry.mirrorName),
                                     entry.mirrorName);
                    return;
                }
                continue;
            }
            if (client->hash != entry.host->contractHash()) {
                if (entry.options.required || client->required) {
                    rejectConnection(socket, QStringLiteral("contractMismatch"),
                                     QObject::tr("Proxy %1 contract does not match.")
                                         .arg(entry.mirrorName),
                                     entry.mirrorName);
                    return;
                }
                continue;
            }
            accepted.insert(entry.mirrorName);
        }
        for (auto client = clientContracts.constBegin();
             client != clientContracts.constEnd(); ++client) {
            if (!findEntry(client.key()) && client->required) {
                rejectConnection(socket, QStringLiteral("contractMismatch"),
                                 QObject::tr("Required client Proxy %1 is unavailable.")
                                     .arg(client.key()),
                                 client.key());
                return;
            }
        }

        QJsonArray mirrors;
        QStringList acceptedNames(accepted.cbegin(), accepted.cend());
        acceptedNames.sort();
        QList<QPair<QString, QJsonObject>> snapshots;
        snapshots.reserve(acceptedNames.size());
        for (const QString &name : std::as_const(acceptedNames)) {
            Entry *entry = findEntry(name);
            QJsonObject snapshot = entry->host->makeSnapshot();
            if (snapshot.isEmpty()) {
                rejectConnection(socket, QStringLiteral("proxyUnavailable"),
                                 QObject::tr("Proxy %1 cannot produce a snapshot.")
                                     .arg(name),
                                 name);
                return;
            }
            mirrors.append(QJsonObject{
                {QStringLiteral("mirrorName"), name},
                {QStringLiteral("contractHash"), entry->host->contractHash()}
            });
            snapshots.append({name, std::move(snapshot)});
        }

        // makeSnapshot() 可能同步 flush pending patch。先建立全部 snapshot，
        // 再把 socket 標成 accepted，可保證新 client 第一份 state 一定是
        //完整 snapshot，不會先收到沒有 base revision 的 patch。
        state.requestSessionId = requestSessionId;
        state.connectionSessionId = QUuid::createUuid().toString(
            QUuid::WithoutBraces);
        state.acceptedMirrors = accepted;

        sendJson(socket, QJsonObject{
            {QStringLiteral("type"), QStringLiteral("proxy.welcome")},
            {QStringLiteral("protocol"),
             WasmMirrorProxy::WireProtocolVersion},
            {QStringLiteral("requestSessionId"), requestSessionId},
            {QStringLiteral("connectionSessionId"), state.connectionSessionId},
            {QStringLiteral("mirrors"), mirrors}
        });
        for (const auto &snapshot : std::as_const(snapshots)) {
            sendJson(socket, decorateEnvelope(snapshot.second, snapshot.first));
        }
    }

    void handleProxyEnvelope(QWebSocket *socket, const QJsonObject &message)
    {
        const ConnectionState state = connections.value(socket);
        const QString mirrorName = message.value(
            QStringLiteral("mirrorName")).toString();
        Entry *entry = findEntry(mirrorName);
        if (state.connectionSessionId.isEmpty()) {
            rejectConnection(socket, QStringLiteral("handshakeRequired"),
                             QObject::tr("Proxy Mirror handshake is required."));
            return;
        }
        if (!entry || !state.acceptedMirrors.contains(mirrorName)
            || message.value(QStringLiteral("connectionSessionId")).toString()
                   != state.connectionSessionId
            || message.value(QStringLiteral("requestSessionId")).toString()
                   != state.requestSessionId) {
            rejectConnection(socket, QStringLiteral("invalidSession"),
                             QObject::tr("Proxy Mirror route or session is invalid."),
                             mirrorName);
            return;
        }

        const QString type = message.value(QStringLiteral("type")).toString();
        if (message.value(QStringLiteral("protocol")).toInt(-1)
            != WasmMirrorProxy::WireProtocolVersion) {
            rejectConnection(socket, QStringLiteral("protocolMismatch"),
                             QObject::tr("Proxy Mirror wire protocol mismatch."),
                             mirrorName);
            return;
        }
        const QJsonObject normalized = normalizeEnvelope(message);
        ProxyMirrorApplyResult result;
        if (type == QStringLiteral("proxy.signal")) {
            result = entry->host->applySignalEnvelope(normalized);
        } else if (type == QStringLiteral("proxy.properties")) {
            result = entry->host->applyPropertyEnvelope(normalized);
        } else {
            sendError(socket, QStringLiteral("unsupportedMessage"),
                      QObject::tr("Unsupported Proxy Mirror message."),
                      mirrorName);
            return;
        }
        if (!result.accepted()) {
            sendError(socket, QStringLiteral("invalidEnvelope"),
                      result.error, mirrorName);
        }
    }

    void handleMessage(QWebSocket *socket, const QString &text)
    {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(
            text.toUtf8(), &parseError);
        if (!document.isObject()) {
            rejectConnection(socket, QStringLiteral("invalidJson"),
                             QObject::tr("Invalid Proxy Mirror JSON: %1")
                                 .arg(parseError.errorString()));
            return;
        }
        const QJsonObject message = document.object();
        const QString type = message.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("proxy.hello")) {
            handleHello(socket, message);
        } else {
            handleProxyEnvelope(socket, message);
        }
    }

    void broadcast(const QString &mirrorName, const QJsonObject &message)
    {
        for (auto connection = connections.constBegin();
             connection != connections.constEnd(); ++connection) {
            if (connection.value().acceptedMirrors.contains(mirrorName)) {
                sendJson(connection.key(), message);
            }
        }
    }
#endif

    WasmMirrorProxy *q;
    QString hostName;
    quint16 configuredPort = 0;
    QString path;
    QStringList allowedOrigins;
    std::vector<Entry> entries;
    quint16 actualPort = 0;
    bool ready = false;

#if !defined(Q_OS_WASM)
    QWebSocketServer server;
    QHash<QWebSocket *, ConnectionState> connections;
#else
    std::unique_ptr<QWebSocket> socket;
    QTimer reconnectTimer;
    QTimer handshakeTimer;
    QString requestSessionId;
    QString connectionSessionId;
    QSet<QString> acceptedMirrors;
    int reconnectAttempt = 0;
    bool started = false;
    bool contractRejected = false;
#endif
};

WasmMirrorProxy::WasmMirrorProxy(const WasmMirrorConfig &config,
                                 QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>(this, config))
{
}

WasmMirrorProxy::~WasmMirrorProxy()
{
    d->stop();
}

std::unique_ptr<WasmMirrorProxy> WasmMirrorProxy::create(
    const WasmMirrorConfig &config,
    QString *errorMessage)
{
    if (!config.validationError().isEmpty()) {
        if (errorMessage) {
            *errorMessage = config.validationError();
        }
        return {};
    }

    auto runtime = std::unique_ptr<WasmMirrorProxy>(
        new WasmMirrorProxy(config));
    QString error;
    for (const WasmMirrorConfig::Registration &registration
         : config.m_registrations) {
        if (!registration.proxy) {
            error = tr("Registered Proxy %1 was destroyed before initialization.")
                        .arg(registration.mirrorName);
            break;
        }
        if (!runtime->d->addRegistration(registration.mirrorName,
                                         *registration.proxy,
                                         registration.options,
                                         &error)) {
            break;
        }
    }
    if (error.isEmpty() && !runtime->d->start(&error)) {
        // start() supplies the actionable message.
    }
    if (!error.isEmpty()) {
        if (errorMessage) {
            *errorMessage = error;
        }
        return {};
    }
    if (errorMessage) {
        errorMessage->clear();
    }
    return runtime;
}

quint16 WasmMirrorProxy::actualPort() const
{
    return d->actualPort;
}

QUrl WasmMirrorProxy::webSocketUrl() const
{
    QUrl url;
    url.setScheme(QStringLiteral("ws"));
    url.setHost(d->hostName);
    url.setPort(d->actualPort > 0 ? d->actualPort : d->configuredPort);
    url.setPath(d->path);
    return url;
}

QStringList WasmMirrorProxy::mirrorNames() const
{
    QStringList names;
    names.reserve(static_cast<qsizetype>(d->entries.size()));
    for (const Private::Entry &entry : d->entries) {
        names.append(entry.mirrorName);
    }
    names.sort();
    return names;
}

bool WasmMirrorProxy::isReady() const
{
    return d->ready;
}
