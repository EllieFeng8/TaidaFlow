#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QModbusDataUnit>
#include <QObject>
#include <QSet>
#include <QString>

#include <memory>
#include <vector>

// w2-072 (review D-003): rate limit of the repeated warnings of one source (a device that is
// offline, a set value that keeps being refused, ...). The first warning is written at once;
// after that at most one per interval, and that one reports how many were held back since the
// previous written one. reset() starts over (e.g. the device is connected again).
// Used by ModbusClient, Ms300FaultReader and Manager; main thread only (no locking).
class RepeatedWarningLimiter
{
public:
    static constexpr qint64 kDefaultIntervalMs = 60000;

    explicit RepeatedWarningLimiter(qint64 intervalMs = kDefaultIntervalMs) : m_intervalMs(intervalMs) {}
    void setIntervalMs(qint64 intervalMs) { m_intervalMs = intervalMs; }

    // One more warning. True: write it now (*suppressedBefore = warnings held back since the
    // previous written one); false: hold it back.
    bool allow(qint64 *suppressedBefore = nullptr)
    {
        ++m_total;
        if (m_clock.isValid() && m_clock.elapsed() < m_intervalMs) {
            ++m_suppressed;
            return false;
        }
        if (suppressedBefore)
            *suppressedBefore = m_suppressed;
        m_suppressed = 0;
        m_clock.start();
        return true;
    }
    qint64 total() const { return m_total; }              // warnings since the last reset()
    qint64 heldBack() const { return m_suppressed; }      // held back and not reported yet
    qint64 totalHeldBack() const { return m_totalHeldBack + m_suppressed; }
    void reset()
    {
        m_clock.invalidate();
        m_suppressed = 0;
        m_total = 0;
        m_totalHeldBack = 0;
    }
    // Text appended to a written warning: "" or " (N similar warning(s) held back since the
    // previous one)"; also adds N to totalHeldBack().
    QString suffix(qint64 suppressedBefore)
    {
        m_totalHeldBack += suppressedBefore;
        return suppressedBefore > 0
                ? QStringLiteral(" (%1 similar warning(s) held back since the previous one; at most one "
                                 "per %2 s)").arg(suppressedBefore).arg(m_intervalMs / 1000.0, 0, 'f', 0)
                : QString();
    }

private:
    QElapsedTimer m_clock;
    qint64 m_intervalMs;
    qint64 m_suppressed = 0;
    qint64 m_total = 0;
    qint64 m_totalHeldBack = 0;
};

class ModbusClient final : public QObject
{
    Q_OBJECT

public:
    enum class Device {
        Unassigned,
        Adam6256_201,
        Adam6217_202,
        Adam6217_203,
        Adam6224_204,
        Adam6022_205
    };
    Q_ENUM(Device)

    // host / port / unitId come from config.json (w2-062: Core builds the list from
    // AppConfig, see Core::init); timeoutMs / retryCount stay fixed in the code (Mango: not
    // in config.json).
    struct DeviceConfig {
        Device device = Device::Unassigned;
        QString name;
        QString host;
        quint16 port = 502;
        int unitId = 1;
        int timeoutMs = 1000;
        int retryCount = 2;
    };

    // The five ADAM modules with the built-in plant addresses 192.168.1.201..205:502,
    // unit 1 (= the config.json defaults). Used by the default constructor (stand-alone
    // tests) and as the template Core fills with the configured values.
    static QList<DeviceConfig> defaultDeviceConfigs();

    // Default addresses (defaultDeviceConfigs()).
    explicit ModbusClient(QObject *parent = nullptr);
    // w2-062: the given devices (normally from config.json). TAIDAFLOW_DEVICE_PROFILE=simulator
    // (test only) still replaces every host with the simulator address 127.0.0.201..205 of
    // that device (port and unit id are kept); the replacement is logged.
    explicit ModbusClient(const QList<DeviceConfig> &configs, QObject *parent = nullptr);
    ~ModbusClient() override;

    QList<DeviceConfig> deviceConfigs() const;

    // Connections are safe to establish before the I/O address table is known.
    // Reads and writes are initiated explicitly by Manager only after a binding
    // is configured.
    void connectAll();
    void disconnectAll();

    void read(Device device,
              QModbusDataUnit::RegisterType registerType,
              int startAddress,
              quint16 valueCount);
    // Returns true only when Qt accepted the request for transmission.
    bool write(Device device,
               QModbusDataUnit::RegisterType registerType,
               int startAddress,
               const QList<quint16> &values);

    static QString displayName(Device device);

    // w2-072 (review D-003): while a device is not connected
    //  * read() sends nothing and reports "not connected" (deviceError) once per outage;
    //    write() still reports every refused write (operator action);
    //  * reconnects only through the reconnect timer: one attempt kReconnectDelayMs (3 s)
    //    after the previous one failed (read()/write() never connect directly);
    //  * connection failures (connectDevice() refused, errorOccurred) are reported through
    //    deviceError: the first at once, then at most one per warning interval (60 s) with the
    //    number held back; deviceConnectionChanged(true, ...) ends the outage (its detail then
    //    says how long it lasted, the attempts and the warnings held back).
    // deviceConnectionChanged is emitted on every state change as before.
    static constexpr int kReconnectDelayMs = 3000;
    // Tests only: a shorter warning interval than RepeatedWarningLimiter::kDefaultIntervalMs.
    void setWarningIntervalMs(qint64 intervalMs);

signals:
    void deviceConnectionChanged(Device device, bool connected, const QString &detail);
    void deviceError(Device device, const QString &message);
    void registersRead(Device device,
                       QModbusDataUnit::RegisterType registerType,
                       int startAddress,
                       const QList<quint16> &values);
    void writeSucceeded(Device device,
                        QModbusDataUnit::RegisterType registerType,
                        int startAddress,
                        quint16 valueCount);

private:
    struct DeviceSession;

    DeviceSession *sessionFor(Device device);
    void connectDevice(DeviceSession *session);
    void scheduleReconnect(DeviceSession *session);
    bool ensureConnected(DeviceSession *session);
    void reportConnectionProblem(DeviceSession *session, const QString &message);   // w2-072

    std::vector<std::unique_ptr<DeviceSession>> m_sessions;
    QSet<QString> m_pendingReadRequests;
    bool m_autoReconnect = false;
};
