#pragma once
// w2-084: the Core's server heartbeat (TaidaFlowProxy::serverHeartbeatMs, contract of w1-083).
//
// Every TaidaFlowProxy::kServerHeartbeatIntervalMs (1 s) a QTimer of the MAIN thread (the thread of
// the Proxy and of the Mirror) writes the current epoch ms into serverHeartbeatMs; the Mirror
// sends the change to every web page (one small proxy.patch per second). The web page's
// LinkWatchdog (TaidaFlowContent/components/LinkWatchdog.qml) only looks at WHEN the value last
// changed: no change for more than 5 s -> "連線中斷", 15 s -> the page reloads itself.
//
// Deliberately on the main event loop, not on a worker thread: when the Core's main thread is
// stuck, the Mirror stops sending too, and the page then shows the real state. The heartbeat
// does not go through SqlManager (no database call, no queued call to another thread).
//
// Core (core.cpp) creates it in init() and stops it FIRST in shutdown(), so nothing is written
// while the backend is torn down. Desktop only (Core/CMakeLists.txt): the WebAssembly page
// never writes the heartbeat, it only reads the mirrored value.
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <functional>

class TaidaFlowProxy;

class ServerHeartbeat : public QObject
{
    Q_OBJECT
public:
    // Wall clock in epoch ms (default QDateTime::currentMSecsSinceEpoch). The tests inject their
    // own clock and may use a shorter interval; the Core uses the defaults.
    using Clock = std::function<qint64()>;

    explicit ServerHeartbeat(TaidaFlowProxy *proxy, QObject *parent = nullptr);
    ServerHeartbeat(TaidaFlowProxy *proxy, int intervalMs, Clock clock, QObject *parent = nullptr);

    // Writes the first heartbeat at once, then every intervalMs(). Must be called on the thread of
    // the Proxy (the main thread). A second call while running changes nothing.
    void start();
    // Stops the timer; no write after this returns. Idempotent. The last value stays in the Proxy
    // (the web page then sees "no change" -> its own detection, or the WebSocket close first).
    void stop();

    bool isActive() const { return m_timer.isActive(); }
    int intervalMs() const { return m_timer.interval(); }
    // Number of writes since construction (tests / log).
    quint64 beatCount() const { return m_beats; }

private:
    void beat();

    QPointer<TaidaFlowProxy> m_proxy;
    Clock m_clock;
    QTimer m_timer;
    quint64 m_beats = 0;
};
