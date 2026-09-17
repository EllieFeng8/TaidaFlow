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
    void setWayValveOpenSv(bool open);
    void setInverterResetSv(bool active);
    void setEmergencyStopSv(bool active);
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
    bool writeCommand(ModbusMapping::CommandPoint point, double value);
    void tripDi0Interlock();

    TaidaFlowProxy *m_proxy = nullptr;
    SqlManager *m_sql = nullptr;
    ModbusClient m_modbus;
    QTimer m_pollTimer;
    QList<ModbusMapping::ReadBinding> m_readBindings;
    QList<ModbusMapping::WriteBinding> m_writeBindings;
    QVector<quint16> m_serverInputRegisters;
    quint8 m_completedAiGroups = 0;
    bool m_startVfdAfterFrequencyWrite = false;
    // The machine starts in the conservative state.  A true DI0 sample is
    // required before either DO0 (00017) or DO3 (00020) can be energized.
    bool m_di0OutputPermit = false;
    bool m_di0StateKnown = false;
};
