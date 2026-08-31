#pragma once

#include "ModbusMapping.h"

#include <QObject>
#include <QTimer>
#include <QVector>

class TaidaFlowProxy;
class SqlManager;

class Manager final : public QObject
{
    Q_OBJECT

public:
    explicit Manager(TaidaFlowProxy *proxy,
                     SqlManager *sql,
                     QObject *parent = nullptr);
    ~Manager() override;

    void start();
    void stop();

public slots:
    void setM1Sv(double value);
    void setM2Sv(double value);
    void setM3Sv(double value);
    void setM4Sv(double value);
    void setPump2HzSv(double value);
    void setMotorRunningSv(bool running);
    void writeServerData(QModbusDataUnit::RegisterType table,
                         int offset,
                         const QList<quint16> &values);

signals:
    void serverCoilUpdated(quint16 offset, bool value);
    void serverInputRegisterUpdated(quint16 offset, quint16 value);
    void serverHoldingRegisterUpdated(quint16 offset, quint16 value);

private:
    void pollConfiguredPoints();
    void mirrorClientData(ModbusClient::Device device,
                          QModbusDataUnit::RegisterType registerType,
                          int startAddress,
                          const QList<quint16> &values);
    void saveServerInputData();
    void mirrorHmiCommandToServer(const ModbusMapping::WriteBinding &binding,
                                  quint16 rawValue);
    void updateProcessPoint(ModbusMapping::ProcessPoint point, double value);
    void writeCommand(ModbusMapping::CommandPoint point, double value);

    TaidaFlowProxy *m_proxy = nullptr;
    SqlManager *m_sql = nullptr;
    ModbusClient m_modbus;
    QTimer m_pollTimer;
    QList<ModbusMapping::ReadBinding> m_readBindings;
    QList<ModbusMapping::WriteBinding> m_writeBindings;
    QVector<quint16> m_serverInputRegisters;
    quint8 m_completedAiGroups = 0;
};
