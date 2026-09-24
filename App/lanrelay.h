// LanRelay: plain TCP relay that lets other computers on the LAN reach the Proxy Mirror.
//
// TEMPORARY WORKAROUND - remove once wasm-mirror pack 1.0.2 is in use.
// The pack (1.0.0 on main, 1.0.1 on core, read-only for this project) refuses to bind the
// desktop Mirror server to a non-loopback address ("Proxy Mirror currently permits loopback
// addresses only.", wasmmirrorproxy.cpp start()). Until pack 1.0.2 ships an official switch
// for that, the desktop binds the Mirror to 127.0.0.1:18125 (internal port) and this relay
// listens on 0.0.0.0:8125 (the public Mirror port the web pages connect to) and copies every
// connection byte-for-byte to 127.0.0.1:18125 in both directions. When 1.0.2 arrives: bind
// the Mirror to 0.0.0.0:8125 with the pack's switch, delete this header and its use in
// App/main.cpp.
//
// Intranet system: by Mango's decision there is no access control here (any host that can
// reach port 8125 is relayed), same as the Mirror's empty allowedOrigins.
//
// Desktop only (main.cpp includes it under !Q_OS_WASM). Header-only and deliberately
// without Q_OBJECT, so no moc run and no CMakeLists change is needed: all signal handling
// uses functor connections with a context object.
#pragma once

#include <QAbstractSocket>
#include <QDebug>
#include <QHostAddress>
#include <QPointer>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>

#include <memory>

class LanRelay
{
public:
    LanRelay(const QHostAddress &listenAddress, quint16 listenPort,
             const QHostAddress &targetAddress, quint16 targetPort)
        : m_listenAddress(listenAddress)
        , m_listenPort(listenPort)
        , m_targetAddress(targetAddress)
        , m_targetPort(targetPort)
    {
    }

    LanRelay(const LanRelay &) = delete;
    LanRelay &operator=(const LanRelay &) = delete;

    // Starts listening. On failure returns false and fills *error; the caller only logs it
    // (the relay is optional: the local machine still reaches the Mirror directly).
    bool start(QString *error = nullptr)
    {
        if (m_server.isListening())
            return true;
        if (!m_server.listen(m_listenAddress, m_listenPort)) {
            if (error) {
                *error = QStringLiteral("LAN relay could not listen on %1:%2: %3")
                             .arg(m_listenAddress.toString())
                             .arg(m_listenPort)
                             .arg(m_server.errorString());
            }
            return false;
        }
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server,
                         [this] { acceptPendingConnections(); });
        return true;
    }

    bool isListening() const { return m_server.isListening(); }
    QString description() const
    {
        return QStringLiteral("%1:%2 -> %3:%4")
            .arg(m_listenAddress.toString())
            .arg(m_server.isListening() ? m_server.serverPort() : m_listenPort)
            .arg(m_targetAddress.toString())
            .arg(m_targetPort);
    }

private:
    void acceptPendingConnections()
    {
        while (m_server.hasPendingConnections()) {
            QTcpSocket *client = m_server.nextPendingConnection(); // child of m_server
            if (client)
                relay(client);
        }
    }

    // One client <-> upstream pair. Both sockets are children of m_server, so whatever is
    // still open when the relay is destroyed is deleted with it. Each socket deletes itself
    // (deleteLater) once it is closed; the peer is reached only through a QPointer.
    void relay(QTcpSocket *client)
    {
        auto *upstream = new QTcpSocket(&m_server);
        const QString peer = QStringLiteral("%1:%2")
                                 .arg(client->peerAddress().toString())
                                 .arg(client->peerPort());
        QPointer<QTcpSocket> c(client);
        QPointer<QTcpSocket> u(upstream);
        // Set once the upstream connection is up: distinguishes "could not connect"
        // (errorOccurred without disconnected) from a normal close.
        auto upstreamConnected = std::make_shared<bool>(false);

        client->setSocketOption(QAbstractSocket::LowDelayOption, 1);

        // Upstream up: flush what the client already sent (it stayed buffered in the client
        // socket), or close right away if the client left while we were connecting.
        QObject::connect(upstream, &QTcpSocket::connected, upstream,
                         [c, u, upstreamConnected] {
            *upstreamConnected = true;
            u->setSocketOption(QAbstractSocket::LowDelayOption, 1);
            if (!c || c->state() != QAbstractSocket::ConnectedState) {
                u->disconnectFromHost();
                return;
            }
            if (c->bytesAvailable() > 0)
                u->write(c->readAll());
        });

        // client -> upstream. Before the upstream is connected the data is left in the
        // client's read buffer and flushed by the connected() handler above.
        QObject::connect(client, &QTcpSocket::readyRead, client, [c, u] {
            if (u && u->state() == QAbstractSocket::ConnectedState)
                u->write(c->readAll());
        });

        // upstream -> client.
        QObject::connect(upstream, &QTcpSocket::readyRead, upstream, [c, u] {
            if (c && c->state() == QAbstractSocket::ConnectedState)
                c->write(u->readAll());
            else
                u->skip(u->bytesAvailable()); // client already gone: drop
        });

        // Either side closes -> forward what is left, then close the other side.
        // disconnectFromHost() waits until pending writes are sent before closing.
        QObject::connect(client, &QTcpSocket::disconnected, client, [c, u, peer] {
            qInfo().noquote() << "LAN relay: closed" << peer;
            if (u) {
                if (u->state() == QAbstractSocket::ConnectedState) {
                    if (c->bytesAvailable() > 0)
                        u->write(c->readAll());
                    u->disconnectFromHost();
                } else {
                    // Still connecting (or never connected): give up on the upstream.
                    u->abort();
                    u->deleteLater();
                }
            }
            c->deleteLater();
        });
        QObject::connect(upstream, &QTcpSocket::disconnected, upstream, [c, u] {
            if (c && c->state() == QAbstractSocket::ConnectedState) {
                if (u->bytesAvailable() > 0)
                    c->write(u->readAll());
                c->disconnectFromHost();
            }
            u->deleteLater();
        });

        // Upstream could not be reached (e.g. the Mirror is not listening): log, drop the
        // client. Errors on an established link end in disconnected() above instead.
        QObject::connect(upstream, &QTcpSocket::errorOccurred, upstream,
                         [c, u, upstreamConnected, peer](QAbstractSocket::SocketError) {
            if (*upstreamConnected)
                return;
            qWarning().noquote() << "LAN relay: upstream connection for" << peer
                                 << "failed:" << u->errorString();
            if (c)
                c->abort();
            u->deleteLater();
        });

        qInfo().noquote() << "LAN relay: accepted" << peer << "->"
                          << m_targetAddress.toString() + QLatin1Char(':')
                                 + QString::number(m_targetPort);
        upstream->connectToHost(m_targetAddress, m_targetPort);
    }

    QTcpServer m_server;
    QHostAddress m_listenAddress;
    quint16 m_listenPort;
    QHostAddress m_targetAddress;
    quint16 m_targetPort;
};
