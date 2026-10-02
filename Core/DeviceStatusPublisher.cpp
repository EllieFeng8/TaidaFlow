#include "DeviceStatusPublisher.h"

#include "Ms300FaultReader.h"
#include "TaidaFlowProxy.h"

#include <QDateTime>
#include <QDebug>
#include <QMetaObject>
#include <QStringList>
#include <QThread>

namespace {
const QString kMs300Key = QStringLiteral("ms300");
}

DeviceStatusPublisher::DeviceStatusPublisher(TaidaFlowProxy *proxy, QObject *parent)
    : DeviceStatusPublisher(proxy, [] { return QDateTime::currentMSecsSinceEpoch(); }, parent)
{
}

DeviceStatusPublisher::DeviceStatusPublisher(TaidaFlowProxy *proxy, Clock clock, QObject *parent)
    : QObject(parent)
    , m_proxy(proxy)
    , m_clock(std::move(clock))
{
}

DeviceStatusPublisher::~DeviceStatusPublisher()
{
    stop();
}

QString DeviceStatusPublisher::keyOf(ModbusClient::Device device)
{
    switch (device) {
    case ModbusClient::Device::Adam6256_201: return QStringLiteral("adam6256");
    case ModbusClient::Device::Adam6217_202: return QStringLiteral("adam6217a");
    case ModbusClient::Device::Adam6217_203: return QStringLiteral("adam6217b");
    case ModbusClient::Device::Adam6224_204: return QStringLiteral("adam6224");
    case ModbusClient::Device::Adam6022_205: return QStringLiteral("adam6022");
    case ModbusClient::Device::Unassigned: break;
    }
    return QString();
}

QString DeviceStatusPublisher::modelOf(ModbusClient::Device device)
{
    switch (device) {
    case ModbusClient::Device::Adam6256_201: return QStringLiteral("ADAM-6256");
    case ModbusClient::Device::Adam6217_202:
    case ModbusClient::Device::Adam6217_203: return QStringLiteral("ADAM-6217");
    case ModbusClient::Device::Adam6224_204: return QStringLiteral("ADAM-6224");
    case ModbusClient::Device::Adam6022_205: return QStringLiteral("ADAM-6022");
    case ModbusClient::Device::Unassigned: break;
    }
    return QString();
}

void DeviceStatusPublisher::attachModbus(ModbusClient *client)
{
    if (!client)
        return;
    // deviceConfigs() holds the hosts really used (after the simulator profile replacement).
    const QList<ModbusClient::DeviceConfig> configs = client->deviceConfigs();
    for (const ModbusClient::DeviceConfig &config : configs) {
        const QString key = keyOf(config.device);
        if (key.isEmpty())
            continue;
        Entry entry;
        entry.name = modelOf(config.device);
        entry.address = config.host;
        m_entries.insert(key, entry);
    }
    connect(client, &ModbusClient::deviceConnectionChanged, this,
            [this](ModbusClient::Device device, bool connected, const QString &) {
        setModbusConnected(device, connected);
    });
}

void DeviceStatusPublisher::attachMs300(Ms300FaultReader *reader)
{
    if (!reader)
        return;
    Entry entry;
    entry.name = QStringLiteral("MS300");
    entry.address = reader->settings().serialPort;
    m_entries.insert(kMs300Key, entry);
    connect(reader, &Ms300FaultReader::portOpenChanged, this, &DeviceStatusPublisher::setMs300PortOpen);
    connect(reader, &Ms300FaultReader::faultStatusRead, this, [this] { ms300ReadSucceeded(); });
    connect(reader, &Ms300FaultReader::faultStatusReadFailed, this, &DeviceStatusPublisher::ms300ReadFailed);
}

void DeviceStatusPublisher::start()
{
    for (Entry &entry : m_entries) {
        entry.state = State::Unknown;
        entry.sinceMs = 0;
    }
    m_ms300PortOpen = false;
    m_ms300FailedReads = 0;
    m_active = true;
    QStringList devices;
    for (const QString &key : TaidaFlowProxy::deviceStatusKeys()) {
        if (m_entries.contains(key))
            devices.append(QStringLiteral("%1 %2 %3").arg(key, m_entries.value(key).name, m_entries.value(key).address));
    }
    qInfo().noquote() << QStringLiteral("[DeviceStatus] started (empty map until each device's first connection "
                                        "attempt has a result): %1")
                                 .arg(devices.join(QStringLiteral(", ")));
    write(QVariantMap());
}

void DeviceStatusPublisher::stop()
{
    if (!m_active)
        return;
    m_active = false;
    qInfo().noquote() << QStringLiteral("[DeviceStatus] stopped: deviceStatus = {} (unknown)");
    write(QVariantMap());
}

void DeviceStatusPublisher::setModbusConnected(ModbusClient::Device device, bool connected)
{
    const QString key = keyOf(device);
    if (!key.isEmpty())
        setOnline(key, connected, connected ? QStringLiteral("connected") : QStringLiteral("not connected"));
}

void DeviceStatusPublisher::setMs300PortOpen(bool open)
{
    if (!m_active)
        return;
    m_ms300PortOpen = open;
    m_ms300FailedReads = 0;
    // Open: online only with the first answer of the inverter (ms300ReadSucceeded).
    if (!open)
        setOnline(kMs300Key, false, QStringLiteral("serial port not open"));
}

void DeviceStatusPublisher::ms300ReadSucceeded()
{
    if (!m_active || !m_ms300PortOpen)
        return;
    m_ms300FailedReads = 0;
    setOnline(kMs300Key, true, QStringLiteral("fault-status read answered"));
}

void DeviceStatusPublisher::ms300ReadFailed()
{
    if (!m_active || !m_ms300PortOpen)
        return;
    ++m_ms300FailedReads;
    if (m_ms300FailedReads >= kMs300FailedReadsOffline)
        setOnline(kMs300Key, false, QStringLiteral("%1 fault-status reads in a row failed").arg(m_ms300FailedReads));
}

void DeviceStatusPublisher::setOnline(const QString &key, bool online, const QString &reason)
{
    if (!m_active)
        return;
    auto it = m_entries.find(key);
    if (it == m_entries.end())
        return;
    const State state = online ? State::Online : State::Offline;
    if (it->state == state)
        return;                                         // same state: nothing is rewritten
    const bool first = it->state == State::Unknown;
    it->state = state;
    it->sinceMs = m_clock();
    qInfo().noquote() << QStringLiteral("[DeviceStatus] %1 %2 (%3): %4 (%5%6)")
                                 .arg(it->name, it->address, key,
                                      online ? QStringLiteral("online") : QStringLiteral("OFFLINE"), reason,
                                      first ? QStringLiteral(", first result") : QString());
    write(currentMap());
}

QVariantMap DeviceStatusPublisher::currentMap() const
{
    QVariantMap map;
    if (!m_active)
        return map;
    for (const QString &key : TaidaFlowProxy::deviceStatusKeys()) {
        const auto it = m_entries.constFind(key);
        if (it == m_entries.constEnd() || it->state == State::Unknown)
            continue;                                   // no result yet: no key (= unknown)
        map.insert(key, TaidaFlowProxy::deviceStatusEntry(it->name, it->address, it->state == State::Online,
                                                          static_cast<double>(it->sinceMs)));
    }
    return map;
}

void DeviceStatusPublisher::write(const QVariantMap &map)
{
    TaidaFlowProxy *proxy = m_proxy.data();
    if (!proxy)
        return;
    ++m_writes;
    QStringList parts;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it)
        parts.append(QStringLiteral("%1=%2").arg(it.key(), it.value().toMap().value(QStringLiteral("online")).toBool()
                                                                   ? QStringLiteral("online") : QStringLiteral("OFFLINE")));
    qInfo().noquote() << QStringLiteral("[DeviceStatus] write #%1: {%2}").arg(m_writes).arg(parts.join(QStringLiteral(", ")));
    // The contract: only on the Proxy's thread. Manager (and with it this object) lives there; a
    // call from another thread is moved to the Proxy's thread instead of writing from here.
    if (QThread::currentThread() == proxy->thread()) {
        proxy->setDeviceStatus(map);
    } else {
        qWarning().noquote() << "[DeviceStatus] write from another thread - queued to the Proxy's thread";
        QMetaObject::invokeMethod(proxy, [proxy, map] { proxy->setDeviceStatus(map); }, Qt::QueuedConnection);
    }
}
