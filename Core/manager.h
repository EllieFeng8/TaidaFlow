#pragma once

#include "ModbusMapping.h"
#include "Ms300FaultReader.h"

#include <QObject>
#include <QSet>
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
    bool saveAlarm(const QString &sensor,
                   const QString &message,
                   const QString &status);

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
    void alarmSaved();
    void serverInputDataSaved();
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
    void loadAiHighAlarmPercentSetting();
    void checkHighInputAlarm(quint16 serverOffset, quint16 rawValue);
    void checkDigitalInputAlarm(quint16 diOffset, bool state);
    void updateProcessPoint(ModbusMapping::ProcessPoint point, double value);
    bool writeCommand(ModbusMapping::CommandPoint point, double value);
    void tripDi0Interlock();
    // w2-036: ADAM-6022 AO0 (HR10) read-back -> pump2HzPv, and startup SV sync.
    void handlePump2HzFeedback(const QList<quint16> &values);
    void handleAdam6224AnalogOutputs(const QList<quint16> &values);
    void syncCoilSvsFromAdam6256(int startAddress, const QList<quint16> &values);
    bool isSvWriteSuppressed(const char *svName);
    double decodeCommandRaw(ModbusMapping::CommandPoint point, quint16 raw) const;
    void logStartupSyncProgress();
    // w2-036: DI1 leak -> leakDetectedPv; DI2 makeup-pump OL interlock.
    void updateLeakDetected(bool leak);
    void updateMakeupPumpOverload(bool overload);
    void tripMakeupPumpOverload();
    void countModbusWriteRequest();

    TaidaFlowProxy *m_proxy = nullptr;
    SqlManager *m_sql = nullptr;
    ModbusClient m_modbus;
    Ms300FaultReader m_ms300FaultReader;
    QTimer m_pollTimer;
    QList<ModbusMapping::ReadBinding> m_readBindings;
    QList<ModbusMapping::WriteBinding> m_writeBindings;
    QVector<quint16> m_serverInputRegisters;
    double m_aiHighAlarmPercent = 90.0;
    quint8 m_completedAiGroups = 0;
    bool m_startVfdAfterFrequencyWrite = false;
    QSet<quint16> m_activeHighInputAlarms;
    QSet<quint16> m_activeDigitalInputAlarms;
    // The machine starts in the conservative state.  A true DI0 sample is
    // required before either DO0 (00017) or DO3 (00020) can be energized.
    bool m_di0OutputPermit = false;
    bool m_di0StateKnown = false;

    // DI2 = makeup pump overload (OL).  While it is 1, DO3 (00020) is held off
    // and every request to energize it is rejected.  Clearing DI2 does NOT
    // restart the pump; the operator has to start it again.
    bool m_makeupPumpOverload = false;
    bool m_di2StateKnown = false;
    bool m_di1StateKnown = false;

    // Pump frequency feedback (ADAM-6022 AO0 read-back, HR10 / 40011).
    bool m_pump2HzReadOutstanding = false;
    int m_pump2HzReadFailures = 0;

    // Startup SV synchronisation: each device's first successful read sets the
    // matching SVs once.  While m_syncingSvFromDevice is true the SV slots
    // below do not issue any Modbus write (the proxy's *SvChanged signals are
    // directly connected to them).
    bool m_syncingSvFromDevice = false;
    bool m_svSyncedAdam6224 = false;
    bool m_svSyncedAdam6022 = false;
    bool m_svSyncedAdam6256 = false;
    bool m_startupSyncReported = false;
    int m_suppressedSvWrites = 0;
    // Every Modbus write request handed to ModbusClient (HMI, bridge, safety).
    int m_modbusWriteRequests = 0;
};
