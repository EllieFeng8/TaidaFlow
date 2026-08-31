#include "Modbus_Server.h"

#include "ModbusServerBridgeMapping.h"

#include <QDebug>
#include <QModbusDevice>
#include <QModbusTcpServer>

class BridgeModbusTcpServer final : public QModbusTcpServer
{
public:
    explicit BridgeModbusTcpServer(QObject *parent = nullptr)
        : QModbusTcpServer(parent)
    {
    }

protected:
    bool writeData(const QModbusDataUnit &unit) override
    {
        const int startAddress = unit.startAddress();
        const bool isDigitalOutput = unit.registerType() == QModbusDataUnit::Coils
                && ModbusServerBridgeMapping::isContainedRange(
                        startAddress, unit.valueCount(),
                        ModbusServerBridgeMapping::ServerDoStart,
                        ModbusServerBridgeMapping::ServerDoCount);
        const bool isAnalogOutput = unit.registerType() == QModbusDataUnit::HoldingRegisters
                && ModbusServerBridgeMapping::isContainedRange(
                        startAddress, unit.valueCount(),
                        ModbusServerBridgeMapping::ServerAoStart,
                        ModbusServerBridgeMapping::ServerAoCount);

        if (!isDigitalOutput && !isAnalogOutput) {
            setError(QStringLiteral("This Modbus Server address is read-only or unmapped."),
                     QModbusDevice::WriteError);
            return false;
        }

        if (isAnalogOutput) {
            for (quint16 value : unit.values()) {
                if (value > ModbusServerBridgeMapping::Adam6224AoMaximumRawValue) {
                    setError(QStringLiteral("AO value must be in the raw range 0..4095."),
                             QModbusDevice::WriteError);
                    return false;
                }
            }
        }

        if (!isDigitalOutput)
            return QModbusTcpServer::writeData(unit);

        QModbusDataUnit normalizedUnit = unit;
        QList<quint16> normalizedValues = normalizedUnit.values();
        for (quint16 &value : normalizedValues)
            value = value == 0 ? 0 : 1;
        normalizedUnit.setValues(normalizedValues);
        return QModbusTcpServer::writeData(normalizedUnit);
    }
};

ModbusServer::ModbusServer(QObject *parent)
    : QObject(parent)
    , m_server(std::make_unique<BridgeModbusTcpServer>())
{
    QModbusDataUnitMap registerMap;
    registerMap.insert(QModbusDataUnit::Coils,
                       {QModbusDataUnit::Coils, 0, RegisterCount});
    registerMap.insert(QModbusDataUnit::InputRegisters,
                       {QModbusDataUnit::InputRegisters, 0, RegisterCount});
    registerMap.insert(QModbusDataUnit::HoldingRegisters,
                       {QModbusDataUnit::HoldingRegisters, 0, RegisterCount});

    if (!m_server->setMap(registerMap)) {
        const QString message = m_server->errorString();
        qWarning().noquote() << "[ModbusServer] map setup failed:" << message;
        emit serverError(message);
    }

    connect(m_server.get(), &QModbusServer::dataWritten, this,
            [this](QModbusDataUnit::RegisterType table, int offset, int count) {
        if (m_applyingLocalValue) {
            qInfo().noquote()
                    << QStringLiteral("[ModbusServer][Local update] type=%1 offset=%2 count=%3")
                               .arg(static_cast<int>(table))
                               .arg(offset)
                               .arg(count);
            emit dataChanged(table, offset, count);
            return;
        }

        qInfo().noquote()
                << QStringLiteral("[ModbusServer][Write] type=%1 offset=%2 count=%3")
                           .arg(static_cast<int>(table))
                           .arg(offset)
                           .arg(count);
        emit dataChanged(table, offset, count);

        QList<quint16> values;
        values.reserve(count);
        for (int index = 0; index < count; ++index) {
            quint16 value = 0;
            if (!m_server->data(table, static_cast<quint16>(offset + index), &value)) {
                const QString message = QStringLiteral("Could not read externally written Server data.");
                qWarning().noquote() << "[ModbusServer]" << message;
                emit serverError(message);
                return;
            }
            values.append(value);
        }
        emit writeRequested(table, offset, values);
    });

    connect(m_server.get(), &QModbusDevice::errorOccurred, this,
            [this](QModbusDevice::Error) {
        const QString message = m_server->errorString();
        if (!message.isEmpty()) {
            qWarning().noquote() << "[ModbusServer]" << message;
            emit serverError(message);
        }
    });
}

ModbusServer::~ModbusServer()
{
    stop();
}

bool ModbusServer::start(const QHostAddress &address, quint16 port, int unitId)
{
    if (isRunning())
        return true;

    m_server->setConnectionParameter(QModbusDevice::NetworkAddressParameter,
                                     address.toString());
    m_server->setConnectionParameter(QModbusDevice::NetworkPortParameter, port);
    m_server->setServerAddress(unitId);

    if (!m_server->connectDevice()) {
        const QString message = m_server->errorString();
        qWarning().noquote() << "[ModbusServer] start failed:" << message;
        emit serverError(message);
        return false;
    }

    qInfo().noquote()
            << QStringLiteral("[ModbusServer] listening on %1:%2 unit=%3")
                       .arg(address.toString())
                       .arg(port)
                       .arg(unitId);
    emit serverStarted(address.toString(), port, unitId);
    return true;
}

void ModbusServer::stop()
{
    if (!isRunning())
        return;

    m_server->disconnectDevice();
    qInfo() << "[ModbusServer] stopped";
    emit serverStopped();
}

bool ModbusServer::isRunning() const
{
    return m_server->state() == QModbusDevice::ConnectedState;
}

bool ModbusServer::setCoil(quint16 offset, bool value)
{
    return setValue(QModbusDataUnit::Coils, offset, value ? 1 : 0);
}

bool ModbusServer::setInputRegister(quint16 offset, quint16 value)
{
    return setValue(QModbusDataUnit::InputRegisters, offset, value);
}

bool ModbusServer::setHoldingRegister(quint16 offset, quint16 value)
{
    return setValue(QModbusDataUnit::HoldingRegisters, offset, value);
}

bool ModbusServer::value(QModbusDataUnit::RegisterType table,
                         quint16 offset,
                         quint16 *result) const
{
    return isSupportedTable(table) && offset < RegisterCount
            && m_server->data(table, offset, result);
}

bool ModbusServer::setValue(QModbusDataUnit::RegisterType table,
                            quint16 offset,
                            quint16 value)
{
    if (!isSupportedTable(table) || offset >= RegisterCount) {
        const QString message = QStringLiteral("Invalid register table or offset: %1")
                                        .arg(offset);
        qWarning().noquote() << "[ModbusServer]" << message;
        emit serverError(message);
        return false;
    }

    m_applyingLocalValue = true;
    const bool didSetValue = m_server->setData(table, offset, value);
    m_applyingLocalValue = false;
    if (!didSetValue) {
        const QString message = m_server->errorString();
        qWarning().noquote() << "[ModbusServer] set data failed:" << message;
        emit serverError(message);
        return false;
    }

    return true;
}

bool ModbusServer::isSupportedTable(QModbusDataUnit::RegisterType table)
{
    return table == QModbusDataUnit::Coils
            || table == QModbusDataUnit::InputRegisters
            || table == QModbusDataUnit::HoldingRegisters;
}
