#pragma once

#include "ModbusMapping.h"
#include "Ms300FaultReader.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVector>

class TaidaFlowProxy;
class SqlManager;
class LimitAlarmMonitor;    // w2-085: settings-page limit alarms (Core/LimitAlarms.h)

class Manager final : public QObject
{
    Q_OBJECT

public:
    // w2-062: device addresses and serial parameters (Core fills them from config.json via
    // AppConfig; the backend itself does not read files).
    struct DeviceSettings {
        QList<ModbusClient::DeviceConfig> modbusDevices = ModbusClient::defaultDeviceConfigs();
        Ms300FaultReader::Settings ms300;
    };

    // Built-in default devices (stand-alone tests).
    explicit Manager(TaidaFlowProxy *proxy,
                     SqlManager *sql,
                     QObject *parent = nullptr);
    Manager(TaidaFlowProxy *proxy,
            SqlManager *sql,
            const DeviceSettings &devices,
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
    // One alarm_history row written by this run.  Row ids are per monthly
    // data file, so the insert time is kept to find the file again.
    struct AlarmRow {
        qint64 id = -1;
        QDateTime occurrence;
        QJsonObject reason;     // JSON stored in alarm_history.reason
    };

    bool insertAlarmRow(const QString &sensor,
                        const QString &message,
                        const QString &status,
                        AlarmRow *row);
    bool markAlarmRowResolved(const AlarmRow &row, const QString &detail);
    // w2-053: restart hand-over, run on each DI's first read after start.
    // Returns false only when the lookup failed and must be retried on the
    // next poll (the normal DI alarm logic is skipped until then).
    bool takeOverPreviousRunDigitalInputAlarms(quint16 diOffset, bool state, bool alarmActive);
    void resolvePendingRestartRows(quint16 diOffset);
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
    // w2-072 (review I-003): NaN / +-Inf set values are refused before anything else (no Modbus
    // write, no VFD start); the warning is rate limited per point and the proxy SV is put back
    // to the last accepted value (or the PV when none is known yet).
    bool rejectNonFiniteSv(ModbusMapping::CommandPoint point, const char *svName, double value);
    void warnNonFinite(ModbusMapping::CommandPoint point, const QString &text);
    void restoreSv(ModbusMapping::CommandPoint point, QString *restoredText);
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
    // w2-037: DI offset -> the row written when that DI alarm was raised in
    // this run.  When the DI returns to normal that same row is marked
    // resolved; the entry is removed only after the update succeeded.
    QHash<quint16, AlarmRow> m_activeDigitalInputAlarms;
    // w2-053: rows left 未處理 by an earlier run.  On each DI's first read
    // after start the newest unresolved 異常 row of that DI (this month and
    // the month before) is taken over into m_activeDigitalInputAlarms when
    // the DI is still in alarm; every other such row is resolved.  The check
    // runs once per DI per start (m_diRestartChecked).  Rows whose resolve
    // update failed stay in m_restartResolvePending and are retried on the
    // DI's next poll, like the w2-037 clear path.
    struct PendingResolve {
        AlarmRow row;
        QString detail;
    };
    QSet<quint16> m_diRestartChecked;
    QHash<quint16, int> m_diRestartLookupFailures;
    QElapsedTimer m_restartLookupFailure;   // started when a lookup failed
    QHash<quint16, QList<PendingResolve>> m_restartResolvePending;
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
    // w2-072 (review I-003): last set value accepted for M1..M4 / Pump2Hz (written to the
    // device, or taken from it by the startup sync), per CommandPoint; the rate limit of the
    // "not a finite number" warning per CommandPoint; true while restoreSv() writes the proxy.
    QHash<int, double> m_lastAcceptedSv;
    QHash<int, RepeatedWarningLimiter> m_nonFiniteWarnings;
    bool m_restoringSv = false;
    // w2-085: judged after every PV update (updateProcessPoint); its alarmSaved -> alarmSaved.
    LimitAlarmMonitor *m_limitAlarms = nullptr;
};
