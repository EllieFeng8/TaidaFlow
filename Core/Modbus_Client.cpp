#include "Modbus_Client.h"

#include <QDebug>
#include <QHash>
#include <QModbusDevice>
#include <QModbusReply>
#include <QModbusTcpClient>
#include <QTimer>
#include <QVariant>

namespace {
// Test-only device address switch.  TAIDAFLOW_DEVICE_PROFILE=simulator points the five
// ADAM sessions at Adam60xxSimulator (127.0.0.201..205, same port and unit id), whatever
// host config.json gives (w2-062).  Unset (or empty) keeps the configured addresses; any
// other value is logged as a warning and also keeps the configured addresses.
bool useSimulatorProfile()
{
    static const bool simulator = [] {
        const QString profile = qEnvironmentVariable("TAIDAFLOW_DEVICE_PROFILE");
        if (profile.isEmpty())
            return false;
        if (profile == QLatin1String("simulator"))
            return true;
        qWarning().noquote()
                << QStringLiteral("[Modbus] Unknown TAIDAFLOW_DEVICE_PROFILE=\"%1\" "
                                  "(expected \"simulator\"); using default device addresses.")
                           .arg(profile);
        return false;
    }();
    return simulator;
}

// Adam60xxSimulator address of a device: 127.0.0.201..205 (the last byte of its plant
// address); empty for an unassigned device.
QString simulatorHost(ModbusClient::Device device)
{
    switch (device) {
    case ModbusClient::Device::Adam6256_201: return QStringLiteral("127.0.0.201");
    case ModbusClient::Device::Adam6217_202: return QStringLiteral("127.0.0.202");
    case ModbusClient::Device::Adam6217_203: return QStringLiteral("127.0.0.203");
    case ModbusClient::Device::Adam6224_204: return QStringLiteral("127.0.0.204");
    case ModbusClient::Device::Adam6022_205: return QStringLiteral("127.0.0.205");
    case ModbusClient::Device::Unassigned: break;
    }
    return QString();
}

// Effective host of each device of the last constructed client (displayName() in logs).
QHash<int, QString> &effectiveHosts()
{
    static QHash<int, QString> hosts;
    return hosts;
}

QString readRequestKey(ModbusClient::Device device,
                       QModbusDataUnit::RegisterType registerType,
                       int startAddress,
                       quint16 valueCount)
{
    return QStringLiteral("%1:%2:%3:%4")
            .arg(static_cast<int>(device))
            .arg(static_cast<int>(registerType))
            .arg(startAddress)
            .arg(valueCount);
}
}

struct ModbusClient::DeviceSession
{
    DeviceConfig config;
    QModbusTcpClient *client = nullptr;
    QTimer *reconnectTimer = nullptr;
    // w2-072 (review D-003): the current outage (from the first failure until connected).
    bool inOutage = false;
    bool notConnectedReported = false;     // read() reported "not connected" in this outage
    int reconnectAttempts = 0;             // connection attempts in this outage
    QElapsedTimer outageClock;
    RepeatedWarningLimiter connectionWarnings;
};

void ModbusClient::setWarningIntervalMs(qint64 intervalMs)
{
    for (const auto &session : m_sessions)
        session->connectionWarnings.setIntervalMs(intervalMs);
}

// w2-072: a connection failure of an outage (connectDevice() refused, errorOccurred while not
// connected). Starts the outage; the warning (deviceError) is rate limited per device.
void ModbusClient::reportConnectionProblem(DeviceSession *session, const QString &message)
{
    if (!session->inOutage) {
        session->inOutage = true;
        session->outageClock.start();
    }
    qint64 heldBack = 0;
    if (session->connectionWarnings.allow(&heldBack)) {
        emit deviceError(session->config.device,
                         QStringLiteral("%1 (offline for %2 s, %3 reconnect attempt(s) so far; one every %4 s)%5")
                                 .arg(message.isEmpty() ? QStringLiteral("Connection failed.") : message)
                                 .arg(session->outageClock.elapsed() / 1000)
                                 .arg(session->reconnectAttempts)
                                 .arg(kReconnectDelayMs / 1000)
                                 .arg(session->connectionWarnings.suffix(heldBack)));
    }
}

QList<ModbusClient::DeviceConfig> ModbusClient::defaultDeviceConfigs()
{
    return {
        {Device::Adam6256_201, QStringLiteral("ADAM-6256"), QStringLiteral("192.168.1.201")},
        {Device::Adam6217_202, QStringLiteral("ADAM-6217 A"), QStringLiteral("192.168.1.202")},
        {Device::Adam6217_203, QStringLiteral("ADAM-6217 B"), QStringLiteral("192.168.1.203")},
        {Device::Adam6224_204, QStringLiteral("ADAM-6224"), QStringLiteral("192.168.1.204")},
        {Device::Adam6022_205, QStringLiteral("ADAM-6022"), QStringLiteral("192.168.1.205")},
    };
}

ModbusClient::ModbusClient(QObject *parent)
    : ModbusClient(defaultDeviceConfigs(), parent)
{
}

ModbusClient::ModbusClient(const QList<DeviceConfig> &requestedConfigs, QObject *parent)
    : QObject(parent)
{
    QList<DeviceConfig> configs = requestedConfigs;
    const bool simulator = useSimulatorProfile();
    qInfo().noquote() << QStringLiteral("[Modbus] device profile=%1")
                                 .arg(simulator ? QStringLiteral("simulator")
                                                : QStringLiteral("default"));
    effectiveHosts().clear();
    for (DeviceConfig &config : configs) {
        QString note;
        const QString simHost = simulatorHost(config.device);
        if (simulator && !simHost.isEmpty() && simHost != config.host) {
            note = QStringLiteral(" (configured host %1 replaced by TAIDAFLOW_DEVICE_PROFILE=simulator)")
                           .arg(config.host);
            config.host = simHost;
        }
        effectiveHosts().insert(static_cast<int>(config.device), config.host);
        qInfo().noquote() << QStringLiteral("[Modbus] device %1 -> %2:%3 unit=%4 timeout=%5ms retries=%6%7")
                                     .arg(config.name, config.host)
                                     .arg(config.port)
                                     .arg(config.unitId)
                                     .arg(config.timeoutMs)
                                     .arg(config.retryCount)
                                     .arg(note);
    }

    for (const DeviceConfig &config : configs) {
        auto session = std::make_unique<DeviceSession>();
        session->config = config;
        session->client = new QModbusTcpClient(this);
        session->client->setTimeout(config.timeoutMs);
        session->client->setNumberOfRetries(config.retryCount);

        session->reconnectTimer = new QTimer(this);
        session->reconnectTimer->setSingleShot(true);
        session->reconnectTimer->setInterval(kReconnectDelayMs);

        DeviceSession *rawSession = session.get();
        connect(session->reconnectTimer, &QTimer::timeout, this, [this, rawSession] {
            connectDevice(rawSession);
        });

        connect(session->client, &QModbusDevice::stateChanged, this,
                [this, rawSession](QModbusDevice::State state) {
            if (state == QModbusDevice::ConnectedState) {
                rawSession->reconnectTimer->stop();
                // w2-072: the end of an outage is logged once (Manager logs this signal as info).
                QString detail;
                if (rawSession->inOutage) {
                    detail = QStringLiteral("(connected again after %1 s offline, %2 reconnect attempt(s), "
                                            "%3 connection warning(s) held back)")
                                     .arg(rawSession->outageClock.elapsed() / 1000.0, 0, 'f', 1)
                                     .arg(rawSession->reconnectAttempts)
                                     .arg(rawSession->connectionWarnings.totalHeldBack());
                }
                rawSession->inOutage = false;
                rawSession->notConnectedReported = false;
                rawSession->reconnectAttempts = 0;
                rawSession->outageClock.invalidate();
                rawSession->connectionWarnings.reset();
                emit deviceConnectionChanged(rawSession->config.device, true, detail);
                return;
            }

            if (state == QModbusDevice::UnconnectedState) {
                const QString detail = rawSession->client->errorString();
                if (!rawSession->inOutage) {
                    rawSession->inOutage = true;
                    rawSession->outageClock.start();
                }
                emit deviceConnectionChanged(rawSession->config.device, false, detail);
                if (m_autoReconnect)
                    scheduleReconnect(rawSession);
            }
        });

        connect(session->client, &QModbusDevice::errorOccurred, this,
                [this, rawSession](QModbusDevice::Error error) {
            const QString message = rawSession->client->errorString();
            if (message.isEmpty())
                return;
            // w2-072: connection errors (and any error while not connected) belong to the
            // outage and are rate limited; other errors of a connected device as before.
            if (error == QModbusDevice::ConnectionError
                    || rawSession->client->state() != QModbusDevice::ConnectedState) {
                reportConnectionProblem(rawSession, message);
                return;
            }
            emit deviceError(rawSession->config.device, message);
        });

        m_sessions.push_back(std::move(session));
    }
}

ModbusClient::~ModbusClient()
{
    disconnectAll();
}

QList<ModbusClient::DeviceConfig> ModbusClient::deviceConfigs() const
{
    QList<DeviceConfig> configs;
    configs.reserve(static_cast<qsizetype>(m_sessions.size()));
    for (const auto &session : m_sessions)
        configs.append(session->config);
    return configs;
}

void ModbusClient::connectAll()
{
    m_autoReconnect = true;
    for (const auto &session : m_sessions)
        connectDevice(session.get());
}

void ModbusClient::disconnectAll()
{
    m_autoReconnect = false;
    m_pendingReadRequests.clear();
    for (const auto &session : m_sessions) {
        session->reconnectTimer->stop();
        if (session->client->state() != QModbusDevice::UnconnectedState)
            session->client->disconnectDevice();
    }
}

void ModbusClient::read(Device device,
                        QModbusDataUnit::RegisterType registerType,
                        int startAddress,
                        quint16 valueCount)
{
    DeviceSession *session = sessionFor(device);
    if (!session)
        return;
    if (startAddress < 0 || valueCount == 0 || registerType == QModbusDataUnit::Invalid) {
        emit deviceError(device, QStringLiteral("Invalid Modbus read request."));
        return;
    }
    // w2-072: the same read still waiting for its reply is skipped first (before the connection
    // check), as before.
    const QString requestKey = readRequestKey(device, registerType, startAddress, valueCount);
    if (m_pendingReadRequests.contains(requestKey))
        return;
    if (session->client->state() != QModbusDevice::ConnectedState) {
        // w2-072 (review D-003): nothing is sent while the device is not connected; this is
        // reported once per outage (the poll calls read() for every binding every second), and
        // the reconnect is left to the reconnect timer. While a connection attempt is still in
        // progress (Connecting) the read is skipped silently; a failed attempt reports itself.
        if (session->client->state() == QModbusDevice::UnconnectedState && !session->notConnectedReported) {
            session->notConnectedReported = true;
            if (!session->inOutage) {
                session->inOutage = true;
                session->outageClock.start();
            }
            emit deviceError(device, QStringLiteral("Device is not connected; reads are skipped until it is "
                                                    "connected again (reconnect attempt every %1 s).")
                                             .arg(kReconnectDelayMs / 1000));
        }
        if (session->client->state() == QModbusDevice::UnconnectedState)
            scheduleReconnect(session);
        return;
    }

    const QModbusDataUnit request(registerType, startAddress, valueCount);
    QModbusReply *reply = session->client->sendReadRequest(request, session->config.unitId);
    if (!reply) {
        emit deviceError(device, session->client->errorString());
        return;
    }

    m_pendingReadRequests.insert(requestKey);

    const auto handleReply = [this, reply, device, registerType, startAddress, requestKey] {
        m_pendingReadRequests.remove(requestKey);
        if (reply->error() == QModbusDevice::NoError) {
            emit registersRead(device, registerType, startAddress, reply->result().values());
        } else {
            emit deviceError(device, reply->errorString());
        }
        reply->deleteLater();
    };

    if (reply->isFinished())
        handleReply();
    else
        connect(reply, &QModbusReply::finished, this, handleReply);
}

bool ModbusClient::write(Device device,
                         QModbusDataUnit::RegisterType registerType,
                         int startAddress,
                         const QList<quint16> &values)
{
    DeviceSession *session = sessionFor(device);
    if (!session)
        return false;
    if ((registerType != QModbusDataUnit::Coils
         && registerType != QModbusDataUnit::HoldingRegisters)
        || startAddress < 0 || values.isEmpty()) {
        emit deviceError(device, QStringLiteral("Invalid Modbus write request."));
        return false;
    }
    if (!ensureConnected(session))
        return false;

    const QModbusDataUnit request(registerType, startAddress, values);
    QModbusReply *reply = session->client->sendWriteRequest(request, session->config.unitId);
    if (!reply) {
        emit deviceError(device, session->client->errorString());
        return false;
    }

    const quint16 valueCount = static_cast<quint16>(values.size());
    const auto handleReply = [this, reply, device, registerType, startAddress, valueCount] {
        if (reply->error() == QModbusDevice::NoError) {
            emit writeSucceeded(device, registerType, startAddress, valueCount);
        } else {
            emit deviceError(device, reply->errorString());
        }
        reply->deleteLater();
    };

    if (reply->isFinished())
        handleReply();
    else
        connect(reply, &QModbusReply::finished, this, handleReply);

    return true;
}

QString ModbusClient::displayName(Device device)
{
    // The address shown is the effective one of the running client (config.json, or the
    // simulator address); before any client exists the built-in default.
    QString host = effectiveHosts().value(static_cast<int>(device));
    if (host.isEmpty()) {
        for (const DeviceConfig &config : defaultDeviceConfigs()) {
            if (config.device == device)
                host = config.host;
        }
    }
    switch (device) {
    case Device::Adam6256_201:
        return QStringLiteral("ADAM-6256 (%1)").arg(host);
    case Device::Adam6217_202:
        return QStringLiteral("ADAM-6217 A (%1)").arg(host);
    case Device::Adam6217_203:
        return QStringLiteral("ADAM-6217 B (%1)").arg(host);
    case Device::Adam6224_204:
        return QStringLiteral("ADAM-6224 (%1)").arg(host);
    case Device::Adam6022_205:
        return QStringLiteral("ADAM-6022 (%1)").arg(host);
    case Device::Unassigned:
        return QStringLiteral("Unassigned device");
    }
    return QStringLiteral("Unknown device");
}

ModbusClient::DeviceSession *ModbusClient::sessionFor(Device device)
{
    for (const auto &session : m_sessions) {
        if (session->config.device == device)
            return session.get();
    }

    emit deviceError(device, QStringLiteral("No Modbus session is configured for this device."));
    return nullptr;
}

void ModbusClient::connectDevice(DeviceSession *session)
{
    if (!session || session->client->state() != QModbusDevice::UnconnectedState)
        return;

    // w2-072: attempts during an outage are counted and logged (info, full log only), so the
    // log shows the 3 s rhythm.
    if (session->inOutage) {
        ++session->reconnectAttempts;
        qInfo().noquote() << QStringLiteral("[Modbus] %1 reconnect attempt #%2 (offline for %3 s)")
                                     .arg(displayName(session->config.device))
                                     .arg(session->reconnectAttempts)
                                     .arg(session->outageClock.elapsed() / 1000.0, 0, 'f', 1);
    }
    session->client->setConnectionParameter(QModbusDevice::NetworkAddressParameter,
                                             session->config.host);
    session->client->setConnectionParameter(QModbusDevice::NetworkPortParameter,
                                             session->config.port);
    if (!session->client->connectDevice()) {
        reportConnectionProblem(session, session->client->errorString());
        scheduleReconnect(session);
    }
}

void ModbusClient::scheduleReconnect(DeviceSession *session)
{
    if (m_autoReconnect && session && !session->reconnectTimer->isActive())
        session->reconnectTimer->start();
}

bool ModbusClient::ensureConnected(DeviceSession *session)
{
    if (session && session->client->state() == QModbusDevice::ConnectedState)
        return true;

    if (session) {
        // Used by write(): every refused write is reported (operator action, rare).
        // w2-072 (review D-003): no direct connectDevice() any more (it bypassed the 3 s
        // reconnect delay); the reconnect timer is started if it is not running.
        emit deviceError(session->config.device,
                         QStringLiteral("Device is not connected; request was not sent."));
        if (session->client->state() == QModbusDevice::UnconnectedState)
            scheduleReconnect(session);
    }
    return false;
}
