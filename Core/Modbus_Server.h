#pragma once

#include <QHostAddress>
#include <QList>
#include <QModbusDataUnit>
#include <QObject>
#include <QString>

#include <memory>

class BridgeModbusTcpServer;

class ModbusServer final : public QObject
{
    Q_OBJECT

public:
    // Coils and Input Registers retain 20 positions.  Settings are mirrored
    // to Holding Registers 11..40, so that table exposes 51 positions.
    static constexpr quint16 CoilCount = 20;
    static constexpr quint16 InputRegisterCount = 20;
    static constexpr quint16 HoldingRegisterCount = 51;

    explicit ModbusServer(QObject *parent = nullptr);
    ~ModbusServer() override;

    bool start(const QHostAddress &address = QHostAddress::AnyIPv4,
               quint16 port = 502,
               int unitId = 1);
    void stop();
    bool isRunning() const;

    bool setCoil(quint16 offset, bool value);
    bool setInputRegister(quint16 offset, quint16 value);
    bool setHoldingRegister(quint16 offset, quint16 value);
    bool value(QModbusDataUnit::RegisterType table,
               quint16 offset,
               quint16 *result) const;

signals:
    void serverStarted(const QString &address, quint16 port, int unitId);
    void serverStopped();
    void serverError(const QString &message);
    void dataChanged(QModbusDataUnit::RegisterType table, int offset, int count);
    void writeRequested(QModbusDataUnit::RegisterType table,
                        int offset,
                        const QList<quint16> &values);

private:
    bool setValue(QModbusDataUnit::RegisterType table, quint16 offset, quint16 value);
    static bool isSupportedTable(QModbusDataUnit::RegisterType table);
    static quint16 tableRegisterCount(QModbusDataUnit::RegisterType table);

    std::unique_ptr<BridgeModbusTcpServer> m_server;
    bool m_applyingLocalValue = false;
};
