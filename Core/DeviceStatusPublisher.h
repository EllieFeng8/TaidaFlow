#pragma once
// w2-087: the Core's writer of TaidaFlowProxy::deviceStatus (contract: the "Device status (w1-087)"
// block of TaidaFlowProxy.h; the UI shows the orange banner "設備離線：<name>（<address>）、…").
//
// Keys and devices (TaidaFlowProxy::deviceStatusKeys(), banner order):
//   adam6256  = ModbusClient::Device::Adam6256_201   name "ADAM-6256"
//   adam6217a = ModbusClient::Device::Adam6217_202   name "ADAM-6217"
//   adam6217b = ModbusClient::Device::Adam6217_203   name "ADAM-6217"
//   adam6224  = ModbusClient::Device::Adam6224_204   name "ADAM-6224"
//   adam6022  = ModbusClient::Device::Adam6022_205   name "ADAM-6022"
//   ms300     = Ms300FaultReader (serial port)        name "MS300"
// name is the model only (not ModbusClient::displayName(), which already holds the address and
// A/B). address is the address really used: the ADAM host of ModbusClient::deviceConfigs() (the
// config.json host, or the simulator address when TAIDAFLOW_DEVICE_PROFILE=simulator replaced it),
// the MS300 serial port name (config.json devices.ms300.serialPort, e.g. COM2).
//
// When a device counts as online:
//  * ADAM: its Modbus TCP connection (ModbusClient::deviceConnectionChanged) - the outage of w2-072:
//    from the failed connection / the lost connection until connected again. Connected -> online
//    true at once; not connected -> false. The repeated failed reconnect attempts of one outage
//    (one every 3 s) change nothing (same state -> no new write).
//  * MS300: the serial port is open AND the inverter answers the fault-status read (register
//    0x2100, polled every second by Ms300FaultReader). The port cannot be opened / was closed ->
//    false at once; the port is open -> true with the first successful read; open but
//    kMs300FailedReadsOffline (3) fault-status reads in a row failed (no answer, ~2 s each) -> false
//    (an existing COM port with a switched-off or unplugged inverter); the next successful read ->
//    true again.
//
// Writes (always on the Proxy's thread, always the whole map):
//  * start(): the empty map ("unknown", no banner). A device gets its key only when its first
//    connection attempt has a result (online or offline); a device whose first attempt is still
//    running has no key (missing key = unknown, never reported offline).
//  * every change of a device's online state -> the whole map again, that device's sinceMs = the
//    moment of the change (epoch ms). An unchanged state writes nothing (no periodic rewrite).
//  * stop(): the empty map again, and nothing after it (Manager::stop() calls it BEFORE it closes
//    the devices, so the disconnects of the shutdown are not reported; Core::shutdown ->
//    Manager::stop()).
// Desktop only (Core/CMakeLists.txt); owned by Manager (manager.cpp).
#include "Modbus_Client.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>

#include <functional>

class Ms300FaultReader;
class TaidaFlowProxy;

class DeviceStatusPublisher final : public QObject
{
    Q_OBJECT
public:
    // Wall clock in epoch ms for sinceMs (default QDateTime::currentMSecsSinceEpoch; tests inject one).
    using Clock = std::function<qint64()>;
    static constexpr int kMs300FailedReadsOffline = 3;

    explicit DeviceStatusPublisher(TaidaFlowProxy *proxy, QObject *parent = nullptr);
    DeviceStatusPublisher(TaidaFlowProxy *proxy, Clock clock, QObject *parent = nullptr);
    ~DeviceStatusPublisher() override;

    // The five ADAM sessions of the client (their effective hosts), followed through
    // deviceConnectionChanged. Call before start().
    void attachModbus(ModbusClient *client);
    // The MS300 reader (its serial port name), followed through portOpenChanged / faultStatusRead /
    // faultStatusReadFailed. Call before start().
    void attachMs300(Ms300FaultReader *reader);

    // Every device unknown again, writes the empty map, reports from now on. Call before the
    // devices are started (Manager::start()).
    void start();
    // Writes the empty map and reports nothing after it. Idempotent.
    void stop();
    bool isActive() const { return m_active; }

    static QString keyOf(ModbusClient::Device device);     // "adam6256", ... ("" = unassigned)
    static QString modelOf(ModbusClient::Device device);   // "ADAM-6256", ...

    // The inputs (connected to the device signals by attach*()).
    void setModbusConnected(ModbusClient::Device device, bool connected);
    void setMs300PortOpen(bool open);
    void ms300ReadSucceeded();
    void ms300ReadFailed();

    // The map of the current state (what the last write sent) and the number of writes since
    // construction (tests / log).
    QVariantMap currentMap() const;
    quint64 writeCount() const { return m_writes; }

private:
    enum class State { Unknown, Online, Offline };
    struct Entry {
        QString name;
        QString address;
        State state = State::Unknown;
        qint64 sinceMs = 0;
    };

    void setOnline(const QString &key, bool online, const QString &reason);
    void write(const QVariantMap &map);

    QPointer<TaidaFlowProxy> m_proxy;
    Clock m_clock;
    QHash<QString, Entry> m_entries;
    bool m_active = false;
    bool m_ms300PortOpen = false;
    int m_ms300FailedReads = 0;
    quint64 m_writes = 0;
};
