#include "ServerHeartbeat.h"

#include "TaidaFlowProxy.h"

#include <QDateTime>
#include <QThread>
#include <QtDebug>

ServerHeartbeat::ServerHeartbeat(TaidaFlowProxy *proxy, QObject *parent)
    : ServerHeartbeat(proxy, TaidaFlowProxy::kServerHeartbeatIntervalMs,
                      []() { return QDateTime::currentMSecsSinceEpoch(); }, parent)
{
}

ServerHeartbeat::ServerHeartbeat(TaidaFlowProxy *proxy, int intervalMs, Clock clock, QObject *parent)
    : QObject(parent)
    , m_proxy(proxy)
    , m_clock(clock ? std::move(clock) : Clock([]() { return QDateTime::currentMSecsSinceEpoch(); }))
{
    // PreciseTimer: one tick per second without the 5 % slack of a coarse timer; the page's
    // thresholds (5 s / 15 s) are multiples of this interval.
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(intervalMs > 0 ? intervalMs : TaidaFlowProxy::kServerHeartbeatIntervalMs);
    connect(&m_timer, &QTimer::timeout, this, &ServerHeartbeat::beat);
}

void ServerHeartbeat::start()
{
    if (!m_proxy || m_timer.isActive())
        return;
    if (m_proxy->thread() != QThread::currentThread()) {
        // The Proxy (and the Mirror that sends its changes) live on the main thread; a heartbeat
        // written from another thread would race with them.
        qWarning().noquote() << "[Heartbeat] start() not on the Proxy's thread - server heartbeat NOT started";
        return;
    }
    beat();                     // first value at once, not only after the first interval
    m_timer.start();
    qInfo().noquote() << QStringLiteral("[Heartbeat] server heartbeat started: serverHeartbeatMs = epoch ms "
                                        "every %1 ms (main thread)").arg(m_timer.interval());
}

void ServerHeartbeat::stop()
{
    if (!m_timer.isActive())
        return;
    m_timer.stop();
    qInfo().noquote() << QStringLiteral("[Heartbeat] server heartbeat stopped after %1 write(s)").arg(m_beats);
}

void ServerHeartbeat::beat()
{
    if (!m_proxy)
        return;
    m_proxy->setServerHeartbeatMs(static_cast<double>(m_clock()));
    ++m_beats;
}
