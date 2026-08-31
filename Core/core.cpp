#include "core.h"

#include "Modbus_Server.h"
#include "SqlManager.h"

#include <QSettings>

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
    m_modbusServer = new ModbusServer(this);

    connect(m_proxy, &TaidaFlowProxy::m1ValueSvChanged, m_manager, &Manager::setM1Sv);
    connect(m_proxy, &TaidaFlowProxy::m2ValueSvChanged, m_manager, &Manager::setM2Sv);
    connect(m_proxy, &TaidaFlowProxy::m3ValueSvChanged, m_manager, &Manager::setM3Sv);
    connect(m_proxy, &TaidaFlowProxy::m4ValueSvChanged, m_manager, &Manager::setM4Sv);
    connect(m_proxy, &TaidaFlowProxy::pump2HzSvChanged, m_manager, &Manager::setPump2HzSv);
    connect(m_proxy, &TaidaFlowProxy::motorRunningSvChanged,
            m_manager, &Manager::setMotorRunningSv);

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
    settings.endGroup();
    m_loadingHmiInputSettings = false;

    if (settings.status() != QSettings::NoError)
        qWarning() << "Failed to load HMI input settings:" << settings.fileName();
}
