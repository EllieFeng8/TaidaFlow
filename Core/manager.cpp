#include "manager.h"

#include "ModbusServerBridgeMapping.h"
#include "SqlManager.h"
#include "TaidaFlowProxy.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopedValueRollback>
#include <QSettings>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr int kPollIntervalMs = 1000;
constexpr double kDefaultAiHighAlarmPercent = 90.0;
// A failed pump-frequency read-back is logged on the first failed poll and
// then every kPump2HzFailureLogEvery polls, so an offline ADAM-6022 does not
// flood the log.
constexpr int kPump2HzFailureLogEvery = 30;

// SVs taken from a device are rounded to 0.01 for display (QML shows the SV
// with String()).  For the 12-bit AOs this is loss-free: the rounding error is
// at most 0.005 Hz = 0.34 raw steps / 0.005 % = 0.20 raw steps (< 0.5), so
// re-encoding the SV with writeCommand() gives back the same raw value.
double roundSv(double value)
{
    return std::round(value * 100.0) / 100.0;
}

QString highInputAlarmSensorName(quint16 serverOffset)
{
    switch (serverOffset) {
    case 0: return QStringLiteral("TT-01");
    case 1: return QStringLiteral("TT-02");
    case 2: return QStringLiteral("TT-03");
    case 3: return QStringLiteral("TT-04");
    case 4: return QStringLiteral("PT-01");
    case 5: return QStringLiteral("PT-02");
    case 6: return QStringLiteral("PT-03");
    case 7: return QStringLiteral("PT-04");
    case 8: return QStringLiteral("PT-05");
    case 9: return QStringLiteral("PT-06");
    case 10: return QStringLiteral("PT-07");
    default: return {};
    }
}

QString digitalInputAlarmMessage(quint16 diOffset)
{
    switch (diOffset) {
    case 0: return QStringLiteral("相位異常");
    case 1: return QStringLiteral("漏液檢出");
    case 2: return QStringLiteral("補水泵 OL");
    default: return {};
    }
}

QString modbusValuesText(const QList<quint16> &values)
{
    QStringList parts;
    parts.reserve(values.size());
    for (quint16 value : values)
        parts.append(QString::number(value));
    return parts.join(QLatin1Char(','));
}

QString processPointName(ModbusMapping::ProcessPoint point)
{
    switch (point) {
    case ModbusMapping::ProcessPoint::Tt01: return QStringLiteral("TT-01");
    case ModbusMapping::ProcessPoint::Tt02: return QStringLiteral("TT-02");
    case ModbusMapping::ProcessPoint::Tt03: return QStringLiteral("TT-03");
    case ModbusMapping::ProcessPoint::Tt04: return QStringLiteral("TT-04");
    case ModbusMapping::ProcessPoint::Pt01: return QStringLiteral("PT-01");
    case ModbusMapping::ProcessPoint::Pt02: return QStringLiteral("PT-02");
    case ModbusMapping::ProcessPoint::Pt03: return QStringLiteral("PT-03");
    case ModbusMapping::ProcessPoint::Pt04: return QStringLiteral("PT-04");
    case ModbusMapping::ProcessPoint::Pt05: return QStringLiteral("PT-05");
    case ModbusMapping::ProcessPoint::Pt06: return QStringLiteral("PT-06");
    case ModbusMapping::ProcessPoint::Pt07: return QStringLiteral("PT-07");
    case ModbusMapping::ProcessPoint::FlowMeter: return QStringLiteral("FlowMeter");
    case ModbusMapping::ProcessPoint::Mv1Position: return QStringLiteral("MV1 Position");
    case ModbusMapping::ProcessPoint::Mv2Position: return QStringLiteral("MV2 Position");
    case ModbusMapping::ProcessPoint::Mv3Position: return QStringLiteral("MV3 Position");
    case ModbusMapping::ProcessPoint::Mv4Position: return QStringLiteral("MV4 Position");
    }

    return QStringLiteral("Unknown");
}

QString commandPointName(ModbusMapping::CommandPoint point)
{
    switch (point) {
    case ModbusMapping::CommandPoint::M1: return QStringLiteral("MV1");
    case ModbusMapping::CommandPoint::M2: return QStringLiteral("MV2");
    case ModbusMapping::CommandPoint::M3: return QStringLiteral("MV3");
    case ModbusMapping::CommandPoint::M4: return QStringLiteral("MV4");
    case ModbusMapping::CommandPoint::Pump2Hz: return QStringLiteral("Pump2Hz");
    case ModbusMapping::CommandPoint::MotorRunning: return QStringLiteral("MakeupPumpStart");
    case ModbusMapping::CommandPoint::WayValveOpen: return QStringLiteral("CirculationBypassValveOpen");
    case ModbusMapping::CommandPoint::VfdRun: return QStringLiteral("VfdRun");
    case ModbusMapping::CommandPoint::InverterReset: return QStringLiteral("InverterReset");
    case ModbusMapping::CommandPoint::EmergencyStop: return QStringLiteral("EmergencyStop");
    }

    return QStringLiteral("Unknown");
}
}

Manager::Manager(TaidaFlowProxy *proxy, SqlManager *sql, QObject *parent)
    : QObject(parent)
    , m_proxy(proxy)
    , m_sql(sql)
    , m_modbus(this)
    , m_ms300FaultReader(this)
    , m_readBindings(ModbusMapping::defaultReadBindings())
    , m_writeBindings(ModbusMapping::defaultWriteBindings())
    , m_serverInputRegisters(ModbusServerBridgeMapping::ServerInputRegisterCount, 0)
{
    loadAiHighAlarmPercentSetting();
    m_pollTimer.setInterval(kPollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &Manager::pollConfiguredPoints);

    connect(&m_modbus, &ModbusClient::registersRead, this,
            [this](ModbusClient::Device device,
                   QModbusDataUnit::RegisterType registerType,
                   int startAddress,
                   const QList<quint16> &values) {
        mirrorClientData(device, registerType, startAddress, values);

        for (const ModbusMapping::ReadBinding &binding : m_readBindings) {
            if (!binding.isConfigured()
                    || binding.device != device
                    || binding.registerType != registerType
                    || binding.startAddress != startAddress
                    || binding.valueOffset >= values.size()) {
                continue;
            }

            const quint16 raw = values.at(binding.valueOffset);
            const double decoded = binding.valueFormat == ModbusMapping::ValueFormat::Signed16
                    ? static_cast<double>(static_cast<qint16>(raw))
                    : static_cast<double>(raw);
            const double value = decoded * binding.scale + binding.offset;
            qInfo().noquote()
                    << QStringLiteral("[Modbus][Read] device=%1 point=%2 offset=%3 raw=%4 value=%5")
                               .arg(ModbusClient::displayName(device),
                                    processPointName(binding.point))
                               .arg(startAddress)
                               .arg(raw)
                               .arg(value, 0, 'f', 3);
            updateProcessPoint(binding.point, value);
        }
    });

    connect(&m_modbus, &ModbusClient::writeSucceeded, this,
            [this](ModbusClient::Device device,
               QModbusDataUnit::RegisterType registerType,
               int startAddress,
               quint16 valueCount) {
        qInfo().noquote()
                << QStringLiteral("[Modbus][Write completed] device=%1 type=%2 offset=%3 count=%4")
                           .arg(ModbusClient::displayName(device))
                           .arg(static_cast<int>(registerType))
                           .arg(startAddress)
                           .arg(valueCount);

        if (m_startVfdAfterFrequencyWrite
                && device == ModbusClient::Device::Adam6022_205
                && registerType == QModbusDataUnit::HoldingRegisters
                && startAddress == ModbusServerBridgeMapping::Adam6022Ao0HoldingRegister
                && valueCount == 1) {
            m_startVfdAfterFrequencyWrite = false;
            writeCommand(ModbusMapping::CommandPoint::VfdRun, 1.0);
        }
    });

    connect(&m_modbus, &ModbusClient::deviceConnectionChanged, this,
            [](ModbusClient::Device device, bool connected, const QString &detail) {
        qInfo().noquote() << ModbusClient::displayName(device)
                          << (connected ? QStringLiteral("connected")
                                        : QStringLiteral("disconnected"))
                          << detail;
    });
    connect(&m_modbus, &ModbusClient::deviceError, this,
            [](ModbusClient::Device device, const QString &message) {
        qWarning().noquote() << ModbusClient::displayName(device) << message;
    });

    connect(&m_ms300FaultReader, &Ms300FaultReader::faultStatusChanged, this,
            [this](quint8 faultCode, quint8 warningCode, const QString &message) {
        if (faultCode == 0 && warningCode == 0)
            return;

        const QString status = faultCode != 0
                ? QStringLiteral("異常")
                : QStringLiteral("警告");
        saveAlarm(QStringLiteral("MS300"), message, status);
    });
}

Manager::~Manager()
{
    stop();
}

void Manager::start()
{
    m_modbus.connectAll();
    m_ms300FaultReader.start();
    m_pollTimer.start();
}

void Manager::stop()
{
    m_pollTimer.stop();
    m_ms300FaultReader.stop();
    m_modbus.disconnectAll();
}

bool Manager::saveAlarm(const QString &sensor,
                        const QString &message,
                        const QString &status)
{
    if (!m_sql) {
        qWarning() << "[SQL] Insert alarm skipped: SqlManager is unavailable.";
        return false;
    }

    // 'status' is stored in Core's source vocabulary (異常 / 警告 / 數值異常 /
    // 正常).  It is translated to the UI contract (alarmStatus 未處理/已解除,
    // severity 嚴重/警告) when the rows are read back: see
    // alarmUiFieldsForStatus() in core.cpp, which also covers existing rows.
    const QJsonObject alarm{
        {QStringLiteral("sensor"), sensor},
        {QStringLiteral("alarmMessage"), message},
        {QStringLiteral("status"), status},
    };
    const QString reason = QString::fromUtf8(
            QJsonDocument(alarm).toJson(QJsonDocument::Compact));

    QString errorMessage;
    if (!m_sql->insertAlarm(reason, &errorMessage)) {
        qWarning().noquote() << "[SQL] Insert alarm failed:" << errorMessage;
        return false;
    }

    qInfo().noquote()
            << QStringLiteral("[SQL] Alarm inserted: sensor=%1 message=%2 status=%3")
                       .arg(sensor, message, status);
    emit alarmSaved();
    return true;
}

bool Manager::isSvWriteSuppressed(const char *svName)
{
    if (!m_syncingSvFromDevice)
        return false;

    ++m_suppressedSvWrites;
    qInfo().noquote()
            << QStringLiteral("[SV sync] %1 changed by the startup device sync; Modbus write suppressed.")
                       .arg(QLatin1String(svName));
    return true;
}

void Manager::setM1Sv(double value)
{
    if (isSvWriteSuppressed("m1ValueSv"))
        return;
    writeCommand(ModbusMapping::CommandPoint::M1, value);
}

void Manager::setM2Sv(double value)
{
    if (isSvWriteSuppressed("m2ValueSv"))
        return;
    writeCommand(ModbusMapping::CommandPoint::M2, value);
}

void Manager::setM3Sv(double value)
{
    if (isSvWriteSuppressed("m3ValueSv"))
        return;
    writeCommand(ModbusMapping::CommandPoint::M3, value);
}

void Manager::setM4Sv(double value)
{
    if (isSvWriteSuppressed("m4ValueSv"))
        return;
    writeCommand(ModbusMapping::CommandPoint::M4, value);
}

void Manager::setPump2HzSv(double value)
{
    if (isSvWriteSuppressed("pump2HzSv"))
        return;

    if (value <= 0.0) {
        m_startVfdAfterFrequencyWrite = false;
        writeCommand(ModbusMapping::CommandPoint::VfdRun, 0.0);
        writeCommand(ModbusMapping::CommandPoint::Pump2Hz, value);
        return;
    }

    // The 0..10 V frequency output must be accepted by ADAM-6022 before DO0
    // (manual coil 00017) enables the VFD.
    m_startVfdAfterFrequencyWrite = true;
    if (!writeCommand(ModbusMapping::CommandPoint::Pump2Hz, value))
        m_startVfdAfterFrequencyWrite = false;
}

void Manager::setMotorRunningSv(bool running)
{
    if (isSvWriteSuppressed("motorRunningSv"))
        return;

    if (!writeCommand(ModbusMapping::CommandPoint::MotorRunning, running ? 1.0 : 0.0)
            && running && m_proxy && m_proxy->motorRunningSv()) {
        // Keep HMI state consistent with the safety-rejected physical command.
        m_proxy->setMotorRunningSv(false);
    }
}

void Manager::pollConfiguredPoints()
{
    for (const ModbusMapping::ReadBinding &binding : m_readBindings) {
        if (!binding.isConfigured())
            continue;

        m_modbus.read(binding.device,
                      binding.registerType,
                      binding.startAddress,
                      binding.valueCount);
    }

    m_modbus.read(ModbusClient::Device::Adam6224_204,
                  QModbusDataUnit::DiscreteInputs,
                  ModbusServerBridgeMapping::Adam6224DiStart,
                  ModbusServerBridgeMapping::ServerDiCount);
    m_modbus.read(ModbusClient::Device::Adam6256_201,
                  QModbusDataUnit::Coils,
                  ModbusServerBridgeMapping::Adam6256DoStart,
                  9);

    // Pump frequency feedback: read back ADAM-6022 AO0 (HR10 / 40011) every
    // poll.  A read that has not succeeded by the next poll counts as failed;
    // pump2HzPv is then left unchanged (last good value).
    if (m_pump2HzReadOutstanding) {
        ++m_pump2HzReadFailures;
        if (m_pump2HzReadFailures == 1
                || m_pump2HzReadFailures % kPump2HzFailureLogEvery == 0) {
            qWarning().noquote()
                    << QStringLiteral("[Pump2Hz feedback] ADAM-6022 HR%1 read had no successful reply "
                                      "(%2 consecutive poll(s)); pump2HzPv kept at %3 Hz.")
                               .arg(ModbusServerBridgeMapping::Adam6022Ao0HoldingRegister)
                               .arg(m_pump2HzReadFailures)
                               .arg(m_proxy ? m_proxy->pump2HzPv() : 0.0, 0, 'f', 3);
        }
    }
    m_pump2HzReadOutstanding = true;
    m_modbus.read(ModbusClient::Device::Adam6022_205,
                  QModbusDataUnit::HoldingRegisters,
                  ModbusServerBridgeMapping::Adam6022Ao0HoldingRegister,
                  1);

    // ADAM-6224 AO0..AO3 (HR0..HR3 = MV1..MV4) are read only until the
    // startup SV sync has used them once.
    if (!m_svSyncedAdam6224) {
        m_modbus.read(ModbusClient::Device::Adam6224_204,
                      QModbusDataUnit::HoldingRegisters,
                      ModbusServerBridgeMapping::ServerAoStart,
                      ModbusServerBridgeMapping::ServerAoCount);
    }
}

double Manager::decodeCommandRaw(ModbusMapping::CommandPoint point, quint16 raw) const
{
    // Inverse of writeCommand(): value = raw * scale + offset, using the same
    // binding (ModbusMapping.h is the single source of truth for scaling).
    for (const ModbusMapping::WriteBinding &binding : m_writeBindings) {
        if (binding.point == point)
            return static_cast<double>(raw) * binding.scale + binding.offset;
    }
    return static_cast<double>(raw);
}

void Manager::handlePump2HzFeedback(const QList<quint16> &values)
{
    if (values.isEmpty())
        return;

    m_pump2HzReadOutstanding = false;
    if (m_pump2HzReadFailures > 0) {
        qInfo().noquote()
                << QStringLiteral("[Pump2Hz feedback] ADAM-6022 HR%1 read recovered after %2 failed poll(s).")
                           .arg(ModbusServerBridgeMapping::Adam6022Ao0HoldingRegister)
                           .arg(m_pump2HzReadFailures);
        m_pump2HzReadFailures = 0;
    }

    const quint16 raw = values.constFirst();
    // raw 0..4095 = 0..10 V = 0..60 Hz (raw x 60 / 4095).
    const double hz = decodeCommandRaw(ModbusMapping::CommandPoint::Pump2Hz, raw);
    qInfo().noquote()
            << QStringLiteral("[Modbus][Read] device=%1 point=Pump2Hz(AO0 read-back) offset=%2 raw=%3 value=%4 -> pump2HzPv")
                       .arg(ModbusClient::displayName(ModbusClient::Device::Adam6022_205))
                       .arg(ModbusServerBridgeMapping::Adam6022Ao0HoldingRegister)
                       .arg(raw)
                       .arg(hz, 0, 'f', 3);
    if (m_proxy)
        m_proxy->setPump2HzPv(hz);

    if (m_svSyncedAdam6022 || !m_proxy)
        return;

    m_svSyncedAdam6022 = true;
    const double svBefore = m_proxy->pump2HzSv();
    const double sv = roundSv(hz);
    {
        const QScopedValueRollback<bool> guard(m_syncingSvFromDevice, true);
        m_proxy->setPump2HzSv(sv);
    }
    qInfo().noquote()
            << QStringLiteral("[SV sync] pump2HzSv <- %1 Hz from ADAM-6022 HR%2 raw=%3 (was %4)")
                       .arg(sv, 0, 'f', 2)
                       .arg(ModbusServerBridgeMapping::Adam6022Ao0HoldingRegister)
                       .arg(raw)
                       .arg(svBefore, 0, 'f', 2);
    logStartupSyncProgress();
}

void Manager::handleAdam6224AnalogOutputs(const QList<quint16> &values)
{
    if (m_svSyncedAdam6224 || !m_proxy
            || values.size() < ModbusServerBridgeMapping::ServerAoCount) {
        return;
    }

    m_svSyncedAdam6224 = true;
    using Setter = void (TaidaFlowProxy::*)(double);
    using Getter = double (TaidaFlowProxy::*)() const;
    struct AoSv {
        ModbusMapping::CommandPoint point;
        const char *name;
        Getter get;
        Setter set;
    };
    const AoSv svs[] = {
        {ModbusMapping::CommandPoint::M1, "m1ValueSv", &TaidaFlowProxy::m1ValueSv, &TaidaFlowProxy::setM1ValueSv},
        {ModbusMapping::CommandPoint::M2, "m2ValueSv", &TaidaFlowProxy::m2ValueSv, &TaidaFlowProxy::setM2ValueSv},
        {ModbusMapping::CommandPoint::M3, "m3ValueSv", &TaidaFlowProxy::m3ValueSv, &TaidaFlowProxy::setM3ValueSv},
        {ModbusMapping::CommandPoint::M4, "m4ValueSv", &TaidaFlowProxy::m4ValueSv, &TaidaFlowProxy::setM4ValueSv},
    };

    for (int index = 0; index < ModbusServerBridgeMapping::ServerAoCount; ++index) {
        const AoSv &entry = svs[index];
        const quint16 raw = values.at(index);
        // raw 0..4095 = 0..10 V = 0..100 % valve opening (raw x 100 / 4095).
        const double sv = roundSv(decodeCommandRaw(entry.point, raw));
        const double svBefore = (m_proxy->*entry.get)();
        {
            const QScopedValueRollback<bool> guard(m_syncingSvFromDevice, true);
            (m_proxy->*entry.set)(sv);
        }
        qInfo().noquote()
                << QStringLiteral("[SV sync] %1 <- %2 % from ADAM-6224 HR%3 raw=%4 (was %5)")
                           .arg(QLatin1String(entry.name))
                           .arg(sv, 0, 'f', 2)
                           .arg(ModbusServerBridgeMapping::ServerAoStart + index)
                           .arg(raw)
                           .arg(svBefore, 0, 'f', 2);
    }
    logStartupSyncProgress();
}

void Manager::syncCoilSvsFromAdam6256(int startAddress, const QList<quint16> &values)
{
    if (m_svSyncedAdam6256 || !m_proxy)
        return;

    const auto coilAt = [&](int doOffset, bool *state) {
        const qsizetype index = ModbusServerBridgeMapping::Adam6256DoStart + doOffset - startAddress;
        if (index < 0 || index >= values.size())
            return false;
        *state = values.at(index) != 0;
        return true;
    };

    bool do2 = false;
    bool do3 = false;
    bool do4 = false;
    if (!coilAt(2, &do2) || !coilAt(3, &do3) || !coilAt(4, &do4))
        return;

    m_svSyncedAdam6256 = true;

    // DO3 (00020) = makeup pump command: 1 = running.
    // A safety trip that has already happened (DI0 = 0 known, or DI2 OL) wrote
    // DO3 = 0; this read may predate that write, so keep the SV off then.
    bool motorRunning = do3;
    if (motorRunning && ((m_di0StateKnown && !m_di0OutputPermit) || m_makeupPumpOverload)) {
        qWarning().noquote()
                << QStringLiteral("[SV sync] DO3 read 1 but a safety interlock (DI0=0 or DI2 OL) is active; motorRunningSv kept false.");
        motorRunning = false;
    }
    // DO4 (00021) = circulation bypass / two-way valve: 1 = open.
    const bool wayValveOpen = do4;
    // DO2 (00019) = emergency stop, active-low (Manager::setEmergencyStopSv
    // writes DO2 = 0 for UI ON).  So emergencyStopSv = (DO2 == 0).
    const bool emergencyStop = !do2;

    const bool motorBefore = m_proxy->motorRunningSv();
    const bool valveBefore = m_proxy->wayValveOpenSv();
    const bool estopBefore = m_proxy->emergencyStopSv();
    {
        const QScopedValueRollback<bool> guard(m_syncingSvFromDevice, true);
        m_proxy->setMotorRunningSv(motorRunning);
        m_proxy->setWayValveOpenSv(wayValveOpen);
        m_proxy->setEmergencyStopSv(emergencyStop);
    }
    qInfo().noquote()
            << QStringLiteral("[SV sync] motorRunningSv <- %1 from ADAM-6256 DO3 (coil 19) = %2 (was %3)")
                       .arg(motorRunning ? QStringLiteral("true") : QStringLiteral("false"))
                       .arg(do3 ? 1 : 0)
                       .arg(motorBefore ? QStringLiteral("true") : QStringLiteral("false"));
    qInfo().noquote()
            << QStringLiteral("[SV sync] wayValveOpenSv <- %1 from ADAM-6256 DO4 (coil 20) = %2 (was %3)")
                       .arg(wayValveOpen ? QStringLiteral("true") : QStringLiteral("false"))
                       .arg(do4 ? 1 : 0)
                       .arg(valveBefore ? QStringLiteral("true") : QStringLiteral("false"));
    qInfo().noquote()
            << QStringLiteral("[SV sync] emergencyStopSv <- %1 from ADAM-6256 DO2 (coil 18) = %2, active-low (was %3)")
                       .arg(emergencyStop ? QStringLiteral("true") : QStringLiteral("false"))
                       .arg(do2 ? 1 : 0)
                       .arg(estopBefore ? QStringLiteral("true") : QStringLiteral("false"));
    logStartupSyncProgress();
}

void Manager::logStartupSyncProgress()
{
    if (m_startupSyncReported
            || !m_svSyncedAdam6224 || !m_svSyncedAdam6022 || !m_svSyncedAdam6256) {
        return;
    }

    m_startupSyncReported = true;
    qInfo().noquote()
            << QStringLiteral("[SV sync] Startup device sync complete (ADAM-6224 HR0..3, ADAM-6022 HR10, "
                              "ADAM-6256 DO2..DO4): SV-triggered writes suppressed=%1, "
                              "Modbus write requests since start=%2.")
                       .arg(m_suppressedSvWrites)
                       .arg(m_modbusWriteRequests);
}

void Manager::countModbusWriteRequest()
{
    ++m_modbusWriteRequests;
}

void Manager::updateLeakDetected(bool leak)
{
    if (!m_proxy)
        return;

    if (!m_di1StateKnown || m_proxy->leakDetectedPv() != leak) {
        qInfo().noquote()
                << QStringLiteral("[Leak] ADAM-6224 DI1=%1 -> leakDetectedPv=%2")
                           .arg(leak ? 1 : 0)
                           .arg(leak ? QStringLiteral("true") : QStringLiteral("false"));
    }
    m_di1StateKnown = true;
    m_proxy->setLeakDetectedPv(leak);
}

void Manager::updateMakeupPumpOverload(bool overload)
{
    const bool wasKnown = m_di2StateKnown;
    const bool wasActive = m_makeupPumpOverload;
    m_di2StateKnown = true;
    m_makeupPumpOverload = overload;

    if (overload && (!wasKnown || !wasActive)) {
        tripMakeupPumpOverload();
    } else if (!overload && wasKnown && wasActive) {
        qInfo().noquote()
                << QStringLiteral("[Safety Interlock] ADAM-6224 DI2 (makeup pump OL) cleared; "
                                  "DO3 stays off until the operator starts the makeup pump again.");
    }
}

void Manager::tripMakeupPumpOverload()
{
    qWarning().noquote()
            << QStringLiteral("[Safety Interlock] ADAM-6224 DI2 (makeup pump OL) is 1; "
                              "forcing ADAM-6256 DO3 (00020) off and motorRunningSv=false.");

    // Same pattern as tripDi0Interlock(): clearing the SV drives
    // Manager::setMotorRunningSv(false), which writes DO3 = 0.  If the SV is
    // already off, write DO3 = 0 explicitly so a stale output is still made safe.
    if (m_proxy && m_proxy->motorRunningSv())
        m_proxy->setMotorRunningSv(false);
    else
        writeCommand(ModbusMapping::CommandPoint::MotorRunning, 0.0);
}

void Manager::mirrorClientData(ModbusClient::Device device,
                               QModbusDataUnit::RegisterType registerType,
                               int startAddress,
                               const QList<quint16> &values)
{
    if (device == ModbusClient::Device::Adam6022_205
            && registerType == QModbusDataUnit::HoldingRegisters
            && startAddress == ModbusServerBridgeMapping::Adam6022Ao0HoldingRegister) {
        handlePump2HzFeedback(values);
        return;
    }

    if (device == ModbusClient::Device::Adam6224_204
            && registerType == QModbusDataUnit::HoldingRegisters
            && startAddress == ModbusServerBridgeMapping::ServerAoStart) {
        handleAdam6224AnalogOutputs(values);
        return;
    }

    if (registerType == QModbusDataUnit::HoldingRegisters
            && (device == ModbusClient::Device::Adam6217_202
                || device == ModbusClient::Device::Adam6217_203)) {
        const int serverStart = device == ModbusClient::Device::Adam6217_202
                ? ModbusServerBridgeMapping::ServerAdam6217AInputStart
                : ModbusServerBridgeMapping::ServerAdam6217BInputStart;
        for (qsizetype index = 0; index < values.size(); ++index) {
            const int aiOffset = startAddress + static_cast<int>(index);
            if (aiOffset < 0 || aiOffset >= ModbusServerBridgeMapping::Adam6217AiCount)
                continue;

            const quint16 serverOffset = static_cast<quint16>(serverStart + aiOffset);
            m_serverInputRegisters[serverOffset] = values.at(index);
            emit serverInputRegisterUpdated(serverOffset, values.at(index));
            checkHighInputAlarm(serverOffset, values.at(index));
            qInfo().noquote()
                    << QStringLiteral("[ModbusServer][Mirror] inputRegister=%1 device=%2 AI=%3 raw=%4")
                               .arg(serverOffset)
                               .arg(ModbusClient::displayName(device))
                               .arg(aiOffset)
                               .arg(values.at(index));
        }

        const quint8 groupBit = device == ModbusClient::Device::Adam6217_202
                ? 0x01
                : 0x02;
        m_completedAiGroups |= groupBit;
        if (m_completedAiGroups == 0x03) {
            saveServerInputData();
            m_completedAiGroups = 0;
        }
        return;
    }

    if (device == ModbusClient::Device::Adam6224_204
            && registerType == QModbusDataUnit::DiscreteInputs) {
        for (qsizetype index = 0; index < values.size(); ++index) {
            const int diOffset = startAddress + static_cast<int>(index);
            if (diOffset < ModbusServerBridgeMapping::Adam6224DiStart
                    || diOffset >= ModbusServerBridgeMapping::Adam6224DiStart
                    + ModbusServerBridgeMapping::ServerDiCount)
                continue;

            const quint16 serverOffset = static_cast<quint16>(
                    ModbusServerBridgeMapping::ServerDiStart + diOffset
                    - ModbusServerBridgeMapping::Adam6224DiStart);
            const bool state = values.at(index) != 0;
            emit serverCoilUpdated(serverOffset, state);
            checkDigitalInputAlarm(static_cast<quint16>(diOffset), state);
            qInfo().noquote()
                    << QStringLiteral("[ModbusServer][Mirror] coil=%1 device=%2 DI=%3 value=%4")
                               .arg(serverOffset)
                               .arg(ModbusClient::displayName(device))
                               .arg(diOffset)
                               .arg(state ? 1 : 0);

            if (diOffset == 0) {
                const bool mustTrip = !state && (!m_di0StateKnown || m_di0OutputPermit);
                m_di0OutputPermit = state;
                m_di0StateKnown = true;
                if (mustTrip)
                    tripDi0Interlock();
            } else if (diOffset == 1) {
                // DI1 = leak sensor (1 = leak).  The '漏液檢出' alarm above is kept.
                updateLeakDetected(state);
            } else if (diOffset == 2) {
                // DI2 = makeup pump overload (1 = OL tripped).
                updateMakeupPumpOverload(state);
            }
        }
        return;
    }

    if (device == ModbusClient::Device::Adam6256_201
            && registerType == QModbusDataUnit::Coils) {
        for (qsizetype index = 0; index < values.size(); ++index) {
            const int doOffset = startAddress + static_cast<int>(index)
                    - ModbusServerBridgeMapping::Adam6256DoStart;
            if (doOffset < 0 || doOffset > 8)
                continue;

            const bool state = values.at(index) != 0;
            if (doOffset < ModbusServerBridgeMapping::ServerDoCount)
                emit serverCoilUpdated(static_cast<quint16>(doOffset), state);

            // PV is the physical feedback read from ADAM-6256, not the HMI
            // command (SV).  DO3 is the makeup-pump command (00020); DO4 is
            // the circulation bypass / two-way valve command (00021).
            if (m_proxy && doOffset == 3)
                m_proxy->setMotorRunningPv(state);
            else if (m_proxy && doOffset == 4)
                m_proxy->setWayValveOpenPv(state);

            qInfo().noquote()
                    << QStringLiteral("[Modbus][Read] device=%1 point=DO%2 offset=%3 raw=%4 value=%5")
                               .arg(ModbusClient::displayName(device))
                               .arg(doOffset)
                               .arg(startAddress + static_cast<int>(index))
                               .arg(values.at(index))
                               .arg(state ? 1 : 0);
        }
        syncCoilSvsFromAdam6256(startAddress, values);
    }
}

void Manager::checkHighInputAlarm(quint16 serverOffset, quint16 rawValue)
{
    const QString sensor = highInputAlarmSensorName(serverOffset);
    if (sensor.isEmpty())
        return;

    const quint16 threshold = static_cast<quint16>(std::ceil(
            std::numeric_limits<quint16>::max() * m_aiHighAlarmPercent / 100.0));
    if (rawValue < threshold) {
        m_activeHighInputAlarms.remove(serverOffset);
        return;
    }

    if (m_activeHighInputAlarms.contains(serverOffset))
        return;

    const QString message = QStringLiteral("輸入值達到設定高限 %1%（raw=%2，threshold=%3）")
            .arg(m_aiHighAlarmPercent, 0, 'f', 1)
            .arg(rawValue)
            .arg(threshold);
    if (saveAlarm(sensor, message, QStringLiteral("數值異常")))
        m_activeHighInputAlarms.insert(serverOffset);
}

void Manager::loadAiHighAlarmPercentSetting()
{
    QSettings settings(QStringLiteral("TaidaFlowSettings.ini"), QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("Alarm"));

    bool isValidNumber = false;
    const double storedPercent = settings.value(
            QStringLiteral("aiHighAlarmPercent"), kDefaultAiHighAlarmPercent)
            .toDouble(&isValidNumber);
    const double requestedPercent = isValidNumber && std::isfinite(storedPercent)
            ? storedPercent
            : kDefaultAiHighAlarmPercent;
    m_aiHighAlarmPercent = std::clamp(requestedPercent, 1.0, 100.0);

    settings.setValue(QStringLiteral("aiHighAlarmPercent"), m_aiHighAlarmPercent);
    settings.endGroup();
    settings.sync();

    if (settings.status() != QSettings::NoError) {
        qWarning() << "[Alarm] Failed to save AI high-alarm setting:" << settings.fileName();
    }

    const quint16 threshold = static_cast<quint16>(std::ceil(
            std::numeric_limits<quint16>::max() * m_aiHighAlarmPercent / 100.0));
    qInfo().noquote()
            << QStringLiteral("[Alarm] AI high-alarm setting loaded: %1% (raw >= %2).")
                       .arg(m_aiHighAlarmPercent, 0, 'f', 1)
                       .arg(threshold);
}

void Manager::checkDigitalInputAlarm(quint16 diOffset, bool state)
{
    const QString alarmMessage = digitalInputAlarmMessage(diOffset);
    if (alarmMessage.isEmpty())
        return;

    // DI0 is healthy at 1 and faults at 0. DI1 and DI2 fault at 1.
    const bool alarmActive = diOffset == 0 ? !state : state;
    if (!alarmActive) {
        m_activeDigitalInputAlarms.remove(diOffset);
        return;
    }

    if (m_activeDigitalInputAlarms.contains(diOffset))
        return;

    const QString sensor = QStringLiteral("DI%1").arg(diOffset);
    const QString message = QStringLiteral("%1（%2=%3）")
            .arg(alarmMessage, sensor)
            .arg(state ? 1 : 0);
    if (saveAlarm(sensor, message, QStringLiteral("異常")))
        m_activeDigitalInputAlarms.insert(diOffset);
}

void Manager::setWayValveOpenSv(bool open)
{
    if (isSvWriteSuppressed("wayValveOpenSv"))
        return;
    writeCommand(ModbusMapping::CommandPoint::WayValveOpen, open ? 1.0 : 0.0);
}

void Manager::setInverterResetSv(bool active)
{
    writeCommand(ModbusMapping::CommandPoint::InverterReset, active ? 1.0 : 0.0);
}

void Manager::setEmergencyStopSv(bool active)
{
    if (isSvWriteSuppressed("emergencyStopSv"))
        return;

    // The emergency-stop circuit is active-low: UI ON asserts the stop by
    // writing DO2 = 0; UI OFF releases it by writing DO2 = 1.
    writeCommand(ModbusMapping::CommandPoint::EmergencyStop, active ? 0.0 : 1.0);

    if (!active)
        return;

    qWarning().noquote()
            << QStringLiteral("[Emergency Stop] Setting frequency to 0 Hz and forcing ADAM-6256 DO0 (00017) and DO3 (00020) off.");
    m_startVfdAfterFrequencyWrite = false;

    // Setters update the visible HMI value and synchronously issue the
    // corresponding physical stop command.  If the HMI already shows an off
    // value, explicitly write the device command so a stale field output is
    // still made safe.
    if (m_proxy && m_proxy->pump2HzSv() != 0.0) {
        m_proxy->setPump2HzSv(0.0);
    } else {
        writeCommand(ModbusMapping::CommandPoint::VfdRun, 0.0);
        writeCommand(ModbusMapping::CommandPoint::Pump2Hz, 0.0);
    }

    if (m_proxy && m_proxy->motorRunningSv())
        m_proxy->setMotorRunningSv(false);
    else
        writeCommand(ModbusMapping::CommandPoint::MotorRunning, 0.0);
}

void Manager::saveServerInputData()
{
    if (!m_sql) {
        qWarning() << "[SQL] Server Input Register sample skipped: SqlManager is unavailable.";
        return;
    }

    QVector<double> readings;
    readings.reserve(m_serverInputRegisters.size());
    for (quint16 value : m_serverInputRegisters)
        readings.append(static_cast<double>(value));

    const bool saved = m_sql->saveSensorData(QDateTime::currentDateTime(), readings);
    if (saved) {
        qInfo().noquote()
                << QStringLiteral("[SQL] Saved Server Input Registers 0..%1 to sensor_data.")
                           .arg(m_serverInputRegisters.size() - 1);
        emit serverInputDataSaved();
    } else {
        qWarning() << "[SQL] Failed to save Server Input Register sample.";
    }
}

void Manager::updateProcessPoint(ModbusMapping::ProcessPoint point, double value)
{
    if (!m_proxy)
        return;

    switch (point) {
    case ModbusMapping::ProcessPoint::Tt01: m_proxy->setTt01ValuePv(value); break;
    case ModbusMapping::ProcessPoint::Tt02: m_proxy->setTt02ValuePv(value); break;
    case ModbusMapping::ProcessPoint::Tt03: m_proxy->setTt03ValuePv(value); break;
    case ModbusMapping::ProcessPoint::Tt04: m_proxy->setTt04ValuePv(value); break;
    case ModbusMapping::ProcessPoint::Pt01: m_proxy->setPt01ValuePv(value); break;
    case ModbusMapping::ProcessPoint::Pt02: m_proxy->setPt02ValuePv(value); break;
    case ModbusMapping::ProcessPoint::Pt03: m_proxy->setPt03ValuePv(value); break;
    case ModbusMapping::ProcessPoint::Pt04: m_proxy->setPt04ValuePv(value); break;
    case ModbusMapping::ProcessPoint::Pt05: m_proxy->setPt05ValuePv(value); break;
    case ModbusMapping::ProcessPoint::Pt06: m_proxy->setPt06ValuePv(value); break;
    case ModbusMapping::ProcessPoint::Pt07: m_proxy->setPt07ValuePv(value); break;
    case ModbusMapping::ProcessPoint::FlowMeter: m_proxy->setFlowMeterValuePv(value); break;
    case ModbusMapping::ProcessPoint::Mv1Position: m_proxy->setM1ValuePv(value); break;
    case ModbusMapping::ProcessPoint::Mv2Position: m_proxy->setM2ValuePv(value); break;
    case ModbusMapping::ProcessPoint::Mv3Position: m_proxy->setM3ValuePv(value); break;
    case ModbusMapping::ProcessPoint::Mv4Position: m_proxy->setM4ValuePv(value); break;
    }
}

void Manager::writeServerData(QModbusDataUnit::RegisterType table,
                              int offset,
                              const QList<quint16> &values)
{
    if (offset < 0 || values.isEmpty())
        return;

    if (table == QModbusDataUnit::Coils
            && ModbusServerBridgeMapping::isContainedRange(
                    offset, values.size(),
                    ModbusServerBridgeMapping::ServerDoStart,
                    ModbusServerBridgeMapping::ServerDoCount)) {
        QList<quint16> permittedValues = values;
        for (qsizetype index = 0; index < permittedValues.size(); ++index) {
            const int doOffset = offset + static_cast<int>(index);
            if ((doOffset == 0 || doOffset == 3)
                    && permittedValues.at(index) != 0
                    && !m_di0OutputPermit) {
                qWarning().noquote()
                        << QStringLiteral("[Safety Interlock] DI0 is false/unknown; rejecting ADAM-6256 DO%1 command.")
                                   .arg(doOffset);
                permittedValues[index] = 0;
                emit serverCoilUpdated(static_cast<quint16>(doOffset), false);
            } else if (doOffset == 3
                       && permittedValues.at(index) != 0
                       && m_makeupPumpOverload) {
                qWarning().noquote()
                        << QStringLiteral("[Safety Interlock] DI2 makeup pump OL is active; rejecting ADAM-6256 DO3 command.");
                permittedValues[index] = 0;
                emit serverCoilUpdated(static_cast<quint16>(doOffset), false);
            }
        }

        qInfo().noquote()
                << QStringLiteral("[ModbusServer->Client] ADAM-6256 DO%1..DO%2 values=%3")
                           .arg(offset)
                           .arg(offset + values.size() - 1)
                           .arg(modbusValuesText(permittedValues));
        countModbusWriteRequest();
        m_modbus.write(ModbusClient::Device::Adam6256_201,
                       QModbusDataUnit::Coils,
                       ModbusServerBridgeMapping::Adam6256DoStart + offset,
                       permittedValues);
        return;
    }

    if (table == QModbusDataUnit::HoldingRegisters
            && ModbusServerBridgeMapping::isContainedRange(
                    offset, values.size(),
                    ModbusServerBridgeMapping::ServerPumpSpeedHoldingRegister,
                    ModbusServerBridgeMapping::ServerPumpSpeedHoldingRegisterCount)) {
        const quint16 rawValue = values.constFirst();
        if (rawValue > ModbusServerBridgeMapping::Adam6224AoMaximumRawValue) {
            qWarning().noquote()
                    << QStringLiteral("[ModbusServer->Client] Pump AO value %1 is outside raw range 0..4095.")
                               .arg(rawValue);
            return;
        }

        qInfo().noquote()
                << QStringLiteral("[ModbusServer->Client] ADAM-6022 AO0 values=%1")
                           .arg(modbusValuesText(values));
        countModbusWriteRequest();
        m_modbus.write(ModbusClient::Device::Adam6022_205,
                       QModbusDataUnit::HoldingRegisters,
                       ModbusServerBridgeMapping::Adam6022Ao0HoldingRegister,
                       values);
        return;
    }

    if (table == QModbusDataUnit::HoldingRegisters
            && ModbusServerBridgeMapping::isContainedRange(
                    offset, values.size(),
                    ModbusServerBridgeMapping::ServerAoStart,
                    ModbusServerBridgeMapping::ServerAoCount)) {
        for (quint16 value : values) {
            if (value > ModbusServerBridgeMapping::Adam6224AoMaximumRawValue) {
                qWarning().noquote()
                        << QStringLiteral("[ModbusServer->Client] AO value %1 is outside raw range 0..4095.")
                                   .arg(value);
                return;
            }
        }

        qInfo().noquote()
                << QStringLiteral("[ModbusServer->Client] ADAM-6224 AO%1..AO%2 values=%3")
                           .arg(offset)
                           .arg(offset + values.size() - 1)
                           .arg(modbusValuesText(values));
        countModbusWriteRequest();
        m_modbus.write(ModbusClient::Device::Adam6224_204,
                       QModbusDataUnit::HoldingRegisters,
                       offset,
                       values);
        return;
    }

    qWarning().noquote()
            << QStringLiteral("[ModbusServer->Client] Rejected write type=%1 offset=%2 count=%3")
                       .arg(static_cast<int>(table))
                       .arg(offset)
                       .arg(values.size());
}

void Manager::mirrorHmiCommandToServer(const ModbusMapping::WriteBinding &binding,
                                       quint16 rawValue)
{
    if (binding.device == ModbusClient::Device::Adam6224_204
            && binding.registerType == QModbusDataUnit::HoldingRegisters
            && ModbusServerBridgeMapping::isContainedRange(
                    binding.startAddress, 1,
                    ModbusServerBridgeMapping::ServerAoStart,
                    ModbusServerBridgeMapping::ServerAoCount)) {
        emit serverHoldingRegisterUpdated(static_cast<quint16>(binding.startAddress), rawValue);
        qInfo().noquote()
                << QStringLiteral("[HMI->ModbusServer] holdingRegister=%1 raw=%2")
                           .arg(binding.startAddress)
                           .arg(rawValue);
        return;
    }

    if (binding.device == ModbusClient::Device::Adam6256_201
            && binding.registerType == QModbusDataUnit::Coils
            && ModbusServerBridgeMapping::isContainedRange(
                    binding.startAddress, 1,
                    ModbusServerBridgeMapping::Adam6256DoStart,
                    ModbusServerBridgeMapping::ServerDoCount)) {
        const quint16 serverOffset = static_cast<quint16>(binding.startAddress
                - ModbusServerBridgeMapping::Adam6256DoStart
                + ModbusServerBridgeMapping::ServerDoStart);
        emit serverCoilUpdated(serverOffset, rawValue != 0);
        qInfo().noquote()
                << QStringLiteral("[HMI->ModbusServer] coil=%1 value=%2")
                           .arg(serverOffset)
                           .arg(rawValue);
        return;
    }

    if (binding.device == ModbusClient::Device::Adam6022_205
            && binding.registerType == QModbusDataUnit::HoldingRegisters
            && binding.startAddress == ModbusServerBridgeMapping::Adam6022Ao0HoldingRegister) {
        emit serverHoldingRegisterUpdated(
                ModbusServerBridgeMapping::ServerPumpSpeedHoldingRegister, rawValue);
        qInfo().noquote()
                << QStringLiteral("[HMI->ModbusServer] holdingRegister=%1 raw=%2")
                           .arg(ModbusServerBridgeMapping::ServerPumpSpeedHoldingRegister)
                           .arg(rawValue);
    }
}

bool Manager::writeCommand(ModbusMapping::CommandPoint point, double value)
{
    if ((point == ModbusMapping::CommandPoint::VfdRun
            || point == ModbusMapping::CommandPoint::MotorRunning)
            && value != 0.0 && !m_di0OutputPermit) {
        qWarning().noquote()
                << QStringLiteral("[Safety Interlock] DI0 is false/unknown; %1 was not energized.")
                           .arg(commandPointName(point));
        return false;
    }

    if (point == ModbusMapping::CommandPoint::MotorRunning
            && value != 0.0 && m_makeupPumpOverload) {
        qWarning().noquote()
                << QStringLiteral("[Safety Interlock] DI2 makeup pump OL is active; %1 was not energized.")
                           .arg(commandPointName(point));
        return false;
    }

    for (const ModbusMapping::WriteBinding &binding : m_writeBindings) {
        if (binding.point != point)
            continue;

        if (!binding.isConfigured()) {
            qInfo() << "Modbus write skipped: address mapping is not configured.";
            return false;
        }

        if (value < binding.minimumValue || value > binding.maximumValue) {
            qWarning() << "Modbus write skipped: value is outside the configured range."
                       << value << binding.minimumValue << binding.maximumValue;
            return false;
        }

        const double rawValue = (value - binding.offset) / binding.scale;
        const qint64 roundedValue = std::llround(rawValue);
        if (roundedValue < 0 || roundedValue > std::numeric_limits<quint16>::max()) {
            qWarning() << "Modbus write skipped: encoded value is outside uint16 range.";
            return false;
        }

        qInfo().noquote()
                << QStringLiteral("[Modbus][Write request] device=%1 command=%2 offset=%3 input=%4 raw=%5")
                           .arg(ModbusClient::displayName(binding.device),
                                commandPointName(point))
                           .arg(binding.startAddress)
                           .arg(value, 0, 'f', 3)
                           .arg(roundedValue);
        const quint16 encodedValue = static_cast<quint16>(roundedValue);
        mirrorHmiCommandToServer(binding, encodedValue);
        countModbusWriteRequest();
        return m_modbus.write(binding.device,
                              binding.registerType,
                              binding.startAddress,
                              {encodedValue});
    }

    qWarning().noquote()
            << QStringLiteral("Modbus write skipped: no mapping exists for %1.")
                       .arg(commandPointName(point));
    return false;
}

void Manager::tripDi0Interlock()
{
    qWarning().noquote()
            << QStringLiteral("[Safety Interlock] ADAM-6224 DI0 is false; forcing ADAM-6256 DO0 (00017) and DO3 (00020) off.");

    m_startVfdAfterFrequencyWrite = false;
    writeCommand(ModbusMapping::CommandPoint::VfdRun, 0.0);

    if (m_proxy && m_proxy->motorRunningSv())
        m_proxy->setMotorRunningSv(false);
    else
        writeCommand(ModbusMapping::CommandPoint::MotorRunning, 0.0);
}
