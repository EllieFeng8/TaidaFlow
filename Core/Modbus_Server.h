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
    static constexpr quint16 RegisterCount = 20;

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

    std::unique_ptr<BridgeModbusTcpServer> m_server;
    bool m_applyingLocalValue = false;
};
