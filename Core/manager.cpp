#include "manager.h"

#include "ModbusServerBridgeMapping.h"
#include "SqlManager.h"
#include "TaidaFlowProxy.h"

#include <QDateTime>
#include <QDebug>
#include <QStringList>

#include <cmath>
#include <limits>

namespace {
constexpr int kPollIntervalMs = 1000;

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
    , m_readBindings(ModbusMapping::defaultReadBindings())
    , m_writeBindings(ModbusMapping::defaultWriteBindings())
    , m_serverInputRegisters(ModbusServerBridgeMapping::ServerInputRegisterCount, 0)
{
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
}

Manager::~Manager()
{
    stop();
}

void Manager::start()
{
    m_modbus.connectAll();
    m_pollTimer.start();
}

void Manager::stop()
{
    m_pollTimer.stop();
    m_modbus.disconnectAll();
}

void Manager::setM1Sv(double value)
{
    writeCommand(ModbusMapping::CommandPoint::M1, value);
}

void Manager::setM2Sv(double value)
{
    writeCommand(ModbusMapping::CommandPoint::M2, value);
}

void Manager::setM3Sv(double value)
{
    writeCommand(ModbusMapping::CommandPoint::M3, value);
}

void Manager::setM4Sv(double value)
{
    writeCommand(ModbusMapping::CommandPoint::M4, value);
}

void Manager::setPump2HzSv(double value)
{
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
}

void Manager::mirrorClientData(ModbusClient::Device device,
                               QModbusDataUnit::RegisterType registerType,
                               int startAddress,
                               const QList<quint16> &values)
{
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
            qInfo().noquote()
                    << QStringLiteral("[Modbus][Read] device=%1 point=DO%2 offset=%3 raw=%4 value=%5")
                               .arg(ModbusClient::displayName(device))
                               .arg(doOffset)
                               .arg(startAddress + static_cast<int>(index))
                               .arg(values.at(index))
                               .arg(state ? 1 : 0);
        }
    }
}

void Manager::setWayValveOpenSv(bool open)
{
    writeCommand(ModbusMapping::CommandPoint::WayValveOpen, open ? 1.0 : 0.0);
}

void Manager::setInverterResetSv(bool active)
{
    writeCommand(ModbusMapping::CommandPoint::InverterReset, active ? 1.0 : 0.0);
}

void Manager::setEmergencyStopSv(bool active)
{
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
            }
        }

        qInfo().noquote()
                << QStringLiteral("[ModbusServer->Client] ADAM-6256 DO%1..DO%2 values=%3")
                           .arg(offset)
                           .arg(offset + values.size() - 1)
                           .arg(modbusValuesText(permittedValues));
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
