#include "core.h"

#include "Modbus_Server.h"
#include "SqlManager.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSettings>
#include <QVariantMap>

namespace {
constexpr auto kHmiInputSettingsFile = "TaidaFlowSettings.ini";
constexpr auto kHmiInputSettingsGroup = "HmiInput";
}

Core& Core::instance()
{
    static Core inst; // 建立唯一的靜態實例
    return inst;      // 回傳該實例的引用
}

Core::~Core()
{
    saveHmiInputSettings();

    if (m_modbusServer) {
        m_modbusServer->stop();
        delete m_modbusServer;
        m_modbusServer = nullptr;
    }
    // 釋放 Manager 物件 (這會進一步觸發 Manager 的解構式停止執行緒)
    if (m_manager) {
        m_manager->stop();
        delete m_manager;
        m_manager = nullptr;
    }
    // 釋放 TdProxy 物件
    if (m_proxy) {
        delete m_proxy;
        m_proxy = nullptr;
    }
}

void Core::init()
{
    if (m_proxy)
        return;

    m_proxy = new TaidaFlowProxy(this);
    // Load before Manager and ModbusServer signal connections exist. The
    // restored SVs therefore update only the HMI and never drive an ADAM.
    loadHmiInputSettings();
    m_sqlManager = SqlManager::instance();
    if (!m_sqlManager->initialize())
        qWarning() << "SqlManager initialization failed; Server Input Registers will not be saved.";

    m_manager = new Manager(m_proxy, m_sqlManager, this);
    connect(m_manager, &Manager::alarmSaved, this, &Core::loadAlarmRecords);
    if (!m_manager->saveAlarm(QStringLiteral("100"),
                              QStringLiteral("設備啟動"),
                              QStringLiteral("正常"))) {
        // Existing alarm rows must still display if the startup insert fails.
        loadAlarmRecords();
    }
    m_modbusServer = new ModbusServer(this);

    connect(m_proxy, &TaidaFlowProxy::m1ValueSvChanged, m_manager, &Manager::setM1Sv);
    connect(m_proxy, &TaidaFlowProxy::m2ValueSvChanged, m_manager, &Manager::setM2Sv);
    connect(m_proxy, &TaidaFlowProxy::m3ValueSvChanged, m_manager, &Manager::setM3Sv);
    connect(m_proxy, &TaidaFlowProxy::m4ValueSvChanged, m_manager, &Manager::setM4Sv);
    connect(m_proxy, &TaidaFlowProxy::pump2HzSvChanged, m_manager, &Manager::setPump2HzSv);
    connect(m_proxy, &TaidaFlowProxy::motorRunningSvChanged,
            m_manager, &Manager::setMotorRunningSv);
    connect(m_proxy, &TaidaFlowProxy::wayValveOpenSvChanged,
            m_manager, &Manager::setWayValveOpenSv);
    connect(m_proxy, &TaidaFlowProxy::inverterResetSvChanged,
            m_manager, &Manager::setInverterResetSv);
    connect(m_proxy, &TaidaFlowProxy::emergencyStopSvChanged,
            m_manager, &Manager::setEmergencyStopSv);

    connect(m_proxy, &TaidaFlowProxy::m1ValueSvChanged, this,
            [this](double) { saveHmiInputSettings(); });
    connect(m_proxy, &TaidaFlowProxy::m2ValueSvChanged, this,
            [this](double) { saveHmiInputSettings(); });
    connect(m_proxy, &TaidaFlowProxy::m3ValueSvChanged, this,
            [this](double) { saveHmiInputSettings(); });
    connect(m_proxy, &TaidaFlowProxy::m4ValueSvChanged, this,
            [this](double) { saveHmiInputSettings(); });
    connect(m_proxy, &TaidaFlowProxy::pump2HzSvChanged, this,
            [this](double) { saveHmiInputSettings(); });
    connect(m_proxy, &TaidaFlowProxy::motorRunningSvChanged, this,
            [this](bool) { saveHmiInputSettings(); });
    connect(m_proxy, &TaidaFlowProxy::wayValveOpenSvChanged, this,
            [this](bool) { saveHmiInputSettings(); });

    connect(m_modbusServer, &ModbusServer::writeRequested,
            m_manager, &Manager::writeServerData);
    connect(m_manager, &Manager::serverCoilUpdated,
            m_modbusServer, &ModbusServer::setCoil);
    connect(m_manager, &Manager::serverInputRegisterUpdated,
            m_modbusServer, &ModbusServer::setInputRegister);
    connect(m_manager, &Manager::serverHoldingRegisterUpdated,
            m_modbusServer, &ModbusServer::setHoldingRegister);

    m_manager->start();
    m_modbusServer->start();
}

void Core::saveHmiInputSettings()
{
    if (!m_proxy || m_loadingHmiInputSettings)
        return;

    QSettings settings(QString::fromLatin1(kHmiInputSettingsFile), QSettings::IniFormat);
    settings.beginGroup(QString::fromLatin1(kHmiInputSettingsGroup));
    settings.setValue(QStringLiteral("m1ValueSv"), m_proxy->m1ValueSv());
    settings.setValue(QStringLiteral("m2ValueSv"), m_proxy->m2ValueSv());
    settings.setValue(QStringLiteral("m3ValueSv"), m_proxy->m3ValueSv());
    settings.setValue(QStringLiteral("m4ValueSv"), m_proxy->m4ValueSv());
    settings.setValue(QStringLiteral("pump2HzSv"), m_proxy->pump2HzSv());
    settings.setValue(QStringLiteral("motorRunningSv"), m_proxy->motorRunningSv());
    settings.setValue(QStringLiteral("wayValveOpenSv"), m_proxy->wayValveOpenSv());
    settings.endGroup();
    settings.sync();

    if (settings.status() != QSettings::NoError)
        qWarning() << "Failed to save HMI input settings:" << settings.fileName();
}

void Core::loadHmiInputSettings()
{
    if (!m_proxy)
        return;

    QSettings settings(QString::fromLatin1(kHmiInputSettingsFile), QSettings::IniFormat);
    m_loadingHmiInputSettings = true;
    settings.beginGroup(QString::fromLatin1(kHmiInputSettingsGroup));
    m_proxy->setM1ValueSv(settings.value(QStringLiteral("m1ValueSv"), 0.0).toDouble());
    m_proxy->setM2ValueSv(settings.value(QStringLiteral("m2ValueSv"), 0.0).toDouble());
    m_proxy->setM3ValueSv(settings.value(QStringLiteral("m3ValueSv"), 0.0).toDouble());
    m_proxy->setM4ValueSv(settings.value(QStringLiteral("m4ValueSv"), 0.0).toDouble());
    m_proxy->setPump2HzSv(settings.value(QStringLiteral("pump2HzSv"), 0.0).toDouble());
    m_proxy->setMotorRunningSv(settings.value(QStringLiteral("motorRunningSv"), false).toBool());
    m_proxy->setWayValveOpenSv(settings.value(QStringLiteral("wayValveOpenSv"), false).toBool());
    settings.endGroup();
    m_loadingHmiInputSettings = false;

    if (settings.status() != QSettings::NoError)
        qWarning() << "Failed to load HMI input settings:" << settings.fileName();
}

void Core::loadAlarmRecords()
{
    if (!m_proxy)
        return;

    QVariantList records;
    if (!m_sqlManager) {
        qWarning() << "[SQL] Alarm history skipped: SqlManager is unavailable.";
        m_proxy->setAlarmRecords(records);
        return;
    }

    const QDateTime now = QDateTime::currentDateTime();
    QJsonArray history;
    QString errorMessage;
    if (!m_sqlManager->getAlarmHistory(now.addDays(-3).toSecsSinceEpoch(),
                                        now.toSecsSinceEpoch(),
                                        &history,
                                        &errorMessage)) {
        qWarning().noquote() << "[SQL] Failed to load alarm history:" << errorMessage;
        m_proxy->setAlarmRecords(records);
        return;
    }

    records.reserve(history.size());
    // SqlManager returns oldest first. AlarmPage expects newest first when it
    // constructs its default date range and its "show all" range.
    for (qsizetype index = history.size(); index > 0; --index) {
        const QJsonObject alarm = history.at(index - 1).toObject();
        const qint64 occurrence = static_cast<qint64>(
                alarm.value(QStringLiteral("occurrence_time")).toDouble());
        const QString storedReason = alarm.value(QStringLiteral("reason")).toString();
        QJsonParseError parseError;
        const QJsonDocument reasonDocument = QJsonDocument::fromJson(
                storedReason.toUtf8(), &parseError);
        const QJsonObject reasonObject = parseError.error == QJsonParseError::NoError
                && reasonDocument.isObject()
                ? reasonDocument.object()
                : QJsonObject();

        // reason remains a QString in SqlManager.  New records carry JSON;
        // legacy plain text is kept as the warning message with defaults.
        const QString sensor = reasonObject.value(QStringLiteral("sensor"))
                .toString(QStringLiteral("—"));
        const QString alarmMessage = reasonObject.contains(QStringLiteral("alarmMessage"))
                ? reasonObject.value(QStringLiteral("alarmMessage")).toString()
                : reasonObject.value(QStringLiteral("message")).toString(storedReason);
        const QString status = reasonObject.value(QStringLiteral("status"))
                .toString(QStringLiteral("未處理"));
        records.append(QVariantMap{
            {QStringLiteral("id"), static_cast<qint64>(
                    alarm.value(QStringLiteral("id")).toDouble())},
            {QStringLiteral("timestampMs"), occurrence * 1000},
            {QStringLiteral("alarmTime"), QDateTime::fromSecsSinceEpoch(occurrence)
                     .toString(QStringLiteral("yyyy/MM/dd HH:mm"))},
            {QStringLiteral("equipment"), QStringLiteral("系統")},
            {QStringLiteral("sensorName"), sensor},
            {QStringLiteral("alarmMessage"), alarmMessage},
            {QStringLiteral("severity"), QStringLiteral("警告")},
            {QStringLiteral("alarmStatus"), status},
        });
    }

    qInfo().noquote()
            << QStringLiteral("[SQL] Loaded %1 alarm-history records from the last 3 days into the UI.")
                       .arg(records.size());
    m_proxy->setAlarmRecords(records);
}
