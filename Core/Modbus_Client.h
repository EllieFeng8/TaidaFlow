#pragma once

#include <QList>
#include <QModbusDataUnit>
#include <QObject>
#include <QSet>
#include <QString>

#include <memory>
#include <vector>

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

    struct DeviceConfig {
        Device device = Device::Unassigned;
        QString name;
        QString host;
        quint16 port = 502;
        int unitId = 1;
        int timeoutMs = 1000;
        int retryCount = 2;
    };

    explicit ModbusClient(QObject *parent = nullptr);
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
    void write(Device device,
               QModbusDataUnit::RegisterType registerType,
               int startAddress,
               const QList<quint16> &values);

    static QString displayName(Device device);

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

    std::vector<std::unique_ptr<DeviceSession>> m_sessions;
    QSet<QString> m_pendingReadRequests;
    bool m_autoReconnect = false;
};
