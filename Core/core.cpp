#include "core.h"

// w2-062: config.json reader and /runtime.json format of the App target (App/appconfig.h,
// App/runtimeinfo.h, compiled into TaidaFlowApp; Core/CMakeLists.txt adds App/ to the include
// path). Only this file uses them: the backend classes get plain values from here, so their
// stand-alone test projects need neither.
#include "appconfig.h"
#include "runtimeinfo.h"

#include "AppHttpServer/AppHttpServer.h"
#include "HistoryExport.h"
#include "HistoryViews.h"
#include "Modbus_Server.h"
#include "RESTManager.h"
#include "SqlManager.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSettings>
#include <QStringList>
#include <QTime>
#include <QVariantMap>

namespace {
constexpr auto kHmiInputSettingsFile = "TaidaFlowSettings.ini";
constexpr auto kHmiInputSettingsGroup = "HmiInput";

// Alarm vocabulary: Core status (stored in alarm.reason JSON 'status') ->
// UI contract fields of Td.alarmRecords (AlarmPage.qml counts and colours
// alarmStatus === "未處理"; severity === "嚴重" is red, anything else amber).
//
//  Core status   | produced by                                  | alarmStatus | severity
//  --------------+----------------------------------------------+-------------+---------
//  異常          | DI0 相位異常 / DI1 漏液檢出 / DI2 補水泵 OL    | 未處理      | 嚴重
//                | (Manager::checkDigitalInputAlarm), MS300     |             |
//                | fault code != 0 (Ms300FaultReader)           |             |
//  警告          | MS300 warning code only (fault code == 0)    | 未處理      | 警告
//  數值異常      | AI >= high limit (Manager::checkHighInputAlarm) | 未處理   | 警告
//  正常          | 設備啟動 (Core::init) - informational record  | 已解除      | 警告
//  未處理/已解除 | already UI vocabulary (pass-through)         | same        | 警告
//  (missing)     | legacy plain-text reason / JSON w/o status   | 未處理      | 警告
//  anything else | unknown                                      | 未處理      | 警告
//
// 設備啟動 is not a fault: 已解除 keeps it out of the page's "未處理" count and
// shows it green; the UI has no info level, so it takes the lower level 警告.
// Unknown or missing statuses default to 未處理 so that nothing that might
// need attention is hidden.  A valid 'severity' stored in the JSON (嚴重/警告)
// overrides the table (no producer writes one today).  The database is not
// migrated: the conversion runs on every read, so existing rows are covered.
//
// w2-037: a DI alarm row (DI0/DI1/DI2) is updated in place when the input
// returns to normal: Manager keeps 'status' (異常) and adds "resolved": true,
// "resolvedAt" (epoch s) and "resolvedDetail" (e.g. 漏液檢出 解除（DI1=0）).
// A row with resolved == true reads as alarmStatus 已解除; its severity still
// comes from 'status', so a resolved 異常 row stays 嚴重.
struct AlarmUiFields {
    QString alarmStatus;
    QString severity;
    bool known = true;
};

AlarmUiFields alarmUiFieldsForStatus(const QString &coreStatus, bool hasStatus, bool isResolved)
{
    const QString unhandled = QStringLiteral("未處理");
    const QString resolved = QStringLiteral("已解除");
    const QString critical = QStringLiteral("嚴重");
    const QString warning = QStringLiteral("警告");

    AlarmUiFields fields{unhandled, warning, false};
    if (!hasStatus)
        fields = {unhandled, warning, true};
    else if (coreStatus == QStringLiteral("異常"))
        fields = {unhandled, critical, true};
    else if (coreStatus == QStringLiteral("警告") || coreStatus == QStringLiteral("數值異常"))
        fields = {unhandled, warning, true};
    else if (coreStatus == QStringLiteral("正常"))
        fields = {resolved, warning, true};
    else if (coreStatus == unhandled || coreStatus == resolved)
        fields = {coreStatus, warning, true};
    if (isResolved)
        fields.alarmStatus = resolved;
    return fields;
}

// ---- w2-062: backend values from config.json (docs/taidaflow_config_spec.md §2) -----------
// AppConfig::instance() was loaded by main() before Core::init() (App/main.cpp). Every value
// the backends use is logged once here with its source (file / default / environment); the
// full config.json listing is printed by main ("[Config] ..."). Timeouts, retries and poll
// intervals stay fixed in the backend classes (Mango: not in config.json).
QString configSource(const AppConfig &config, const QString &key)
{
    return AppConfig::sourceName(config.source(key));
}

// "<k1>: <source>, <k2>: <source>" for the keys of one group.
QString configSources(const AppConfig &config, const QString &group, const QStringList &names)
{
    QStringList parts;
    for (const QString &name : names)
        parts.append(QStringLiteral("%1 %2").arg(name, configSource(config, group + QLatin1Char('.') + name)));
    return parts.join(QStringLiteral(", "));
}

Manager::DeviceSettings deviceSettingsFromConfig(const AppConfig &config)
{
    struct Map { AppConfig::Device configDevice; ModbusClient::Device clientDevice; };
    static const Map map[] = {
        {AppConfig::Device::Adam6256, ModbusClient::Device::Adam6256_201},
        {AppConfig::Device::Adam6217A, ModbusClient::Device::Adam6217_202},
        {AppConfig::Device::Adam6217B, ModbusClient::Device::Adam6217_203},
        {AppConfig::Device::Adam6224, ModbusClient::Device::Adam6224_204},
        {AppConfig::Device::Adam6022, ModbusClient::Device::Adam6022_205},
    };
    Manager::DeviceSettings settings;   // names, timeouts and retries from the defaults
    for (ModbusClient::DeviceConfig &device : settings.modbusDevices) {
        for (const Map &m : map) {
            if (m.clientDevice != device.device)
                continue;
            const AppConfig::TcpDevice configured = config.device(m.configDevice);
            device.host = configured.host;
            device.port = configured.port;
            device.unitId = configured.unitId;
            const QString group = QStringLiteral("devices.") + AppConfig::deviceKey(m.configDevice);
            qInfo().noquote() << QStringLiteral("[Config] Core %1 (%2) -> %3:%4 unit %5 (%6)")
                                         .arg(device.name, group, device.host)
                                         .arg(device.port).arg(device.unitId)
                                         .arg(configSources(config, group, {QStringLiteral("host"), QStringLiteral("port"),
                                                                            QStringLiteral("unitId")}));
        }
    }
    const AppConfig::SerialDevice ms300 = config.ms300();
    settings.ms300.serialPort = ms300.serialPort;
    settings.ms300.baudRate = ms300.baudRate;
    settings.ms300.dataBits = ms300.dataBits;
    settings.ms300.parity = ms300.parity;
    settings.ms300.stopBits = ms300.stopBits;
    settings.ms300.unitId = ms300.unitId;
    qInfo().noquote() << QStringLiteral("[Config] Core MS300 (devices.ms300) -> %1 %2 baud, data bits %3, parity %4, "
                                        "stop bits %5, unit %6 (%7)")
                                 .arg(ms300.serialPort).arg(ms300.baudRate).arg(ms300.dataBits)
                                 .arg(ms300.parity).arg(ms300.stopBits).arg(ms300.unitId)
                                 .arg(configSources(config, QStringLiteral("devices.ms300"),
                                                    {QStringLiteral("serialPort"), QStringLiteral("baudRate"),
                                                     QStringLiteral("dataBits"), QStringLiteral("parity"),
                                                     QStringLiteral("stopBits"), QStringLiteral("unitId")}));
    return settings;
}

// Writes <web folder>/runtime.json (spec §3, revised 2026-09-28: nginx and AppHttpServer send it
// as a static file). Only when the content differs, so an unchanged file keeps its date.
// A folder that cannot be written only logs a warning: the page then falls back to port 8125.
void writeRuntimeJson(const QString &webDir, quint16 mirrorPublicPort, const QString &portSource)
{
    const QString path = QDir(webDir).filePath(QStringLiteral("runtime.json"));
    const QByteArray body = TaidaFlowRuntime::buildRuntimeJson(mirrorPublicPort);
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly) && existing.readAll() == body) {
        qInfo().noquote() << QStringLiteral("[Web] runtime.json unchanged: %1 = %2 (%3)")
                                     .arg(QDir::toNativeSeparators(path), QString::fromUtf8(body), portSource);
        return;
    }
    existing.close();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(body) != body.size() || !file.commit()) {
        qWarning().noquote() << QStringLiteral("[Web] runtime.json NOT written to %1 (%2) - web pages fall back to "
                                               "Mirror port %3; the application keeps running")
                                        .arg(QDir::toNativeSeparators(path), file.errorString())
                                        .arg(TaidaFlowRuntime::kDefaultMirrorPublicPort);
        return;
    }
    // A pre-compressed copy would be older than the new file (AppHttpServer ignores it) but
    // nginx gzip_static does not compare dates: never leave one behind.
    const QString gz = path + QStringLiteral(".gz");
    if (QFileInfo::exists(gz) && !QFile::remove(gz))
        qWarning().noquote() << QStringLiteral("[Web] cannot remove stale %1").arg(QDir::toNativeSeparators(gz));
    qInfo().noquote() << QStringLiteral("[Web] runtime.json written: %1 = %2 (%3)")
                                 .arg(QDir::toNativeSeparators(path), QString::fromUtf8(body), portSource);
}
}

Core& Core::instance()
{
    static Core inst; // 建立唯一的靜態實例
    return inst;      // 回傳該實例的引用
}

Core::~Core()
{
    // w2-067: Core is the function-local static of Core::instance(), so this destructor runs in
    // the C runtime's exit handlers - after main() returned, after QApplication was destroyed and
    // after every function-local static constructed later than Core (AppHttpServer::instance(),
    // the host table of ModbusClient::displayName, ...) was already destroyed.  The backend was
    // therefore released by shutdown() while the application still existed (aboutToQuit, or the
    // post routine of ~QApplication); calling into it from here was the cause of the
    // 0xC0000005 on close (w2-067 report: ~Core -> Manager::stop -> socket disconnected ->
    // ModbusClient::displayName() on the destroyed host table).
    // Not reachable from App/main.cpp (the application always outlives init() and runs the post
    // routine); logged if it ever happens - the children are then deleted by ~QObject as before.
    if (!m_shutDown && m_manager)
        qWarning().noquote() << "[Core] destroyed without shutdown(): backend released without an application";
    // Normally only the Proxy is left: QML and the Mirror (both destroyed in main() before the
    // application) used it until the end.  It owns no socket, running timer or thread.
    if (m_proxy) {
        delete m_proxy;
        m_proxy = nullptr;
    }
}

void Core::shutdown(const char *reason)
{
    if (m_shutDown)
        return;
    m_shutDown = true;
    QElapsedTimer elapsed;
    elapsed.start();
    qInfo().noquote() << QStringLiteral("[Core] shutdown (%1): stopping the backend").arg(QLatin1String(reason));

    // w2-039/w2-052: History results first.  A load may still be running on the SqlManager thread;
    // after this its result is not delivered (a result already queued is removed with the receiver).
    if (m_sqlManager)
        disconnect(m_sqlManager, nullptr, this, nullptr);

    // 1. Data acquisition: poll timer, MS300 serial client, the five Modbus TCP clients (sockets
    //    closed now, while the event dispatcher and ModbusClient's host table still exist), then the
    //    Modbus server (no external writes any more).  Deleting Manager also removes its connections
    //    to the Proxy (SV changes from QML during the engine teardown reach nothing) and to the server.
    if (m_manager)
        m_manager->stop();
    if (m_modbusServer)
        m_modbusServer->stop();
    delete m_manager;
    m_manager = nullptr;
    delete m_modbusServer;
    m_modbusServer = nullptr;

    // 2. Services that read the database: REST API, History views, CSV export (its thread is joined)
    //    and the HTTP service (page + /exports; its own thread is joined by stop()).
    stopRestServer();
    delete m_historyViews;
    m_historyViews = nullptr;
    delete m_historyExport;
    m_historyExport = nullptr;
    AppHttpServer::instance().stop();

    // 3. Last: the SqlManager worker thread.  Every write above was synchronous (blocking queued
    //    call), so the last sample is already committed; the connections are closed on the worker
    //    thread and the thread is joined.
    if (m_sqlManager)
        m_sqlManager->shutdown();

    qInfo().noquote() << QStringLiteral("[Core] shutdown complete in %1 ms").arg(elapsed.elapsed());
}

void Core::init()
{
    if (m_proxy)
        return;

    m_proxy = new TaidaFlowProxy(this);
    // w2-067: release the backend while the application object still exists (see Core::shutdown).
    // aboutToQuit = normal close (connected first, so it runs before the per-service handlers
    // below, which then find nothing left to stop).  The post routine covers a main() that returns
    // before app.exec() (QML or Mirror failed): QApplication's destructor calls post routines
    // before it tears anything down; after a normal close it finds shutdown() already done.
    if (QCoreApplication::instance()) {
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this,
                [this]() { shutdown("aboutToQuit"); });
        qAddPostRoutine([]() { Core::instance().shutdown("QApplication destructor, main() ended before app.exec()"); });
    }
    // w2-036: SVs are no longer restored from TaidaFlowSettings.ini.  They
    // start at the proxy defaults and Manager replaces them with the actual
    // device state after each device's first successful read (no Modbus
    // write).  Old [HmiInput] keys are left in the file untouched, only logged.
    reportIgnoredHmiInputSettings();
    m_sqlManager = SqlManager::instance();
    if (!m_sqlManager->initialize())
        qWarning() << "SqlManager initialization failed; Server Input Registers will not be saved.";

    // w2-062: every backend value from config.json (AppConfig, loaded by main before init()).
    const AppConfig &config = AppConfig::instance();
    m_manager = new Manager(m_proxy, m_sqlManager, deviceSettingsFromConfig(config), this);
    connect(m_manager, &Manager::alarmSaved, this, &Core::loadAlarmRecords);
    // w2-052 (spec §2.1, Mango 2026-09-27): every client has its own History
    // range and page.  historyViewRequested(sessionId, fromMs, toMs, page) -
    // emitted by HistoryPage.qml, relayed from WASM - is the only History
    // trigger; HistoryViewService pages the client's range asynchronously on the
    // SqlManager thread (w2-039/w2-045 steps, stale rule and keyset anchor per
    // sessionId) and writes historyViews[sessionId].  Nothing is loaded at
    // start-up and nothing on saved samples: a client's page asks when shown.
    m_historyViews = new HistoryViewService(m_proxy, m_sqlManager, HistoryViewService::Options{}, this);
    // w2-041 (spec §3): raw CSV export queue/engine (export folder <working
    // directory>/exports); it mounts GET /exports/<file> on the AppHttpServer
    // singleton, which also serves the web page (w2-049, startHttpServer).
    // w2-062 (spec §2 nginx): the port of the download links = nginx.enabled ? nginx.port :
    // http.port from config.json; TAIDAFLOW_DOWNLOAD_PORT stays a temporary override (the start
    // script sets it to http.port when nginx is enabled but could not be started).
    HistoryExportManager::Options exportOptions;
    exportOptions.downloadPort = config.downloadPort();
    qInfo().noquote() << QStringLiteral("[Config] Core download links -> port %1 (%2; nginx.enabled %3 %4, "
                                        "nginx.port %5 %6, http.port %7 %8)")
                                 .arg(exportOptions.downloadPort)
                                 .arg(AppConfig::sourceName(config.downloadPortSource()))
                                 .arg(config.nginx().enabled ? QStringLiteral("true") : QStringLiteral("false"),
                                      configSource(config, QStringLiteral("nginx.enabled")))
                                 .arg(config.nginx().port).arg(configSource(config, QStringLiteral("nginx.port")))
                                 .arg(config.http().port).arg(configSource(config, QStringLiteral("http.port")));
    m_historyExport = new HistoryExportManager(m_proxy, m_sqlManager, exportOptions, this);
    startHttpServer();
    startRestServer();
    if (!m_manager->saveAlarm(QStringLiteral("100"),
                              QStringLiteral("設備啟動"),
                              QStringLiteral("正常"))) {
        // Existing alarm rows must still display if the startup insert fails.
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

    // w2-036: the SV -> [HmiInput] INI write-back was removed together with
    // the restore: a value that is never read back has no purpose, and the
    // device itself is now the source of the SVs at startup.

    connect(m_modbusServer, &ModbusServer::writeRequested,
            m_manager, &Manager::writeServerData);
    connect(m_manager, &Manager::serverCoilUpdated,
            m_modbusServer, &ModbusServer::setCoil);
    connect(m_manager, &Manager::serverInputRegisterUpdated,
            m_modbusServer, &ModbusServer::setInputRegister);
    connect(m_manager, &Manager::serverHoldingRegisterUpdated,
            m_modbusServer, &ModbusServer::setHoldingRegister);

    m_manager->start();
    const AppConfig::ModbusServerSettings serverSettings = config.modbusServer();
    qInfo().noquote() << QStringLiteral("[Config] Core Modbus server (modbusServer) -> %1:%2 unit %3 (%4)")
                                 .arg(serverSettings.bind).arg(serverSettings.port).arg(serverSettings.unitId)
                                 .arg(configSources(config, QStringLiteral("modbusServer"),
                                                    {QStringLiteral("bind"), QStringLiteral("port"),
                                                     QStringLiteral("unitId")}));
    m_modbusServer->start(QHostAddress(serverSettings.bind), serverSettings.port, serverSettings.unitId);
    setHistoryTitleOnce();
    loadAlarmRecords();
}

namespace {
constexpr auto kWebDirEnv = "TAIDAFLOW_WEB_DIR";
const QString kWebPage = QStringLiteral("TaidaFlowApp.html");

// w2-049: folder of the WebAssembly build served at http://<host>:8124/.
// Order: TAIDAFLOW_WEB_DIR -> <exe folder>/web -> <exe folder>/../wasm-release (development
// default: build\desktop -> build\wasm-release; w2-062: relative, so no build-machine path is
// compiled into the exe).  A candidate is used when it holds TaidaFlowApp.html.  Every candidate
// and the result are logged.
QString resolveWebDir(QString *source)
{
    struct Candidate { QString source; QString dir; };
    QList<Candidate> candidates;
    const QString env = qEnvironmentVariable(kWebDirEnv).trimmed();
    if (env.isEmpty())
        qInfo().noquote() << QStringLiteral("[Web] %1 is not set").arg(QLatin1String(kWebDirEnv));
    else
        candidates.append({QString::fromLatin1(kWebDirEnv), env});
    candidates.append({QStringLiteral("<exe folder>/web"),
                       QCoreApplication::applicationDirPath() + QStringLiteral("/web")});
    candidates.append({QStringLiteral("development default <exe folder>/../wasm-release"),
                       QCoreApplication::applicationDirPath() + QStringLiteral("/../wasm-release")});
    for (const Candidate &c : std::as_const(candidates)) {
        const QString dir = QDir::cleanPath(QDir(c.dir).absolutePath());
        const bool hasDir = QFileInfo(dir).isDir();
        const bool hasPage = hasDir && QFileInfo(dir + QLatin1Char('/') + kWebPage).isFile();
        qInfo().noquote() << QStringLiteral("[Web] candidate %1: %2 -> %3")
                                     .arg(c.source, QDir::toNativeSeparators(dir),
                                          hasPage ? QStringLiteral("has %1, used").arg(kWebPage)
                                                  : hasDir ? QStringLiteral("no %1, skipped").arg(kWebPage)
                                                           : QStringLiteral("does not exist, skipped"));
        if (hasPage) {
            *source = c.source;
            return dir;
        }
    }
    return QString();
}
} // namespace

void Core::startHttpServer()
{
    AppHttpServer &http = AppHttpServer::instance();
    // w2-062: http.bind / http.port from config.json (default 0.0.0.0:8124).
    const AppConfig &config = AppConfig::instance();
    const AppConfig::Listener listener = config.http();
    const QHostAddress bindAddress(listener.bind);
    qInfo().noquote() << QStringLiteral("[Config] Core HTTP service (http) -> %1:%2 (%3)")
                                 .arg(listener.bind).arg(listener.port)
                                 .arg(configSources(config, QStringLiteral("http"),
                                                    {QStringLiteral("bind"), QStringLiteral("port")}));
    QString source;
    const QString webDir = resolveWebDir(&source);
    if (webDir.isEmpty()) {
        qWarning().noquote() << QStringLiteral("[Web] no web page folder found (set %1, or deploy with "
                                               "scripts\\deploy-web.ps1 to <exe folder>\\web) - the web page "
                                               "is not served (runtime.json not written); /exports downloads "
                                               "work as before")
                                        .arg(QLatin1String(kWebDirEnv));
    } else {
        // Replaces the former development web server (COOP/COEP/CORP on, as before),
        // plus ETag/304 revalidation (Cache-Control: no-cache) and pre-compressed .gz files.
        AppHttpServer::StaticOptions options;
        options.indexFile = kWebPage;                  // "/" -> 302 /TaidaFlowApp.html
        // Web file types only: the development default is the build folder itself, which
        // also holds CMakeCache.txt, build.ninja, sources of generated code, ...
        options.fileSuffixes = QStringList{
            QStringLiteral("html"), QStringLiteral("js"), QStringLiteral("mjs"), QStringLiteral("wasm"),
            QStringLiteral("css"), QStringLiteral("json"), QStringLiteral("map"), QStringLiteral("svg"),
            QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("ico"), QStringLiteral("ttf"),
            QStringLiteral("otf"), QStringLiteral("woff"), QStringLiteral("woff2")};
        // w2-062 (spec §3): runtime.json (Mirror port for the page) is a static file of this
        // folder, rewritten at every start - never cached by the browser.
        options.fileCacheControl.insert(QStringLiteral("runtime.json"), QByteArrayLiteral("no-store"));
        // Mango A2 (2026-09-28, the plant firewall opens only port 80): with nginx.enabled the page
        // reaches the Mirror through nginx (location /mirror -> 127.0.0.1:mirror.internalPort), so
        // the port it gets is nginx.port; without nginx it is the LanRelay port mirror.publicPort.
        const bool viaNginx = config.nginx().enabled;
        const quint16 pagePort = viaNginx ? config.nginx().port : config.mirror().publicPort;
        const QString pagePortSource = viaNginx
                ? QStringLiteral("nginx.enabled true -> nginx.port %1, /mirror proxied by nginx")
                          .arg(configSource(config, QStringLiteral("nginx.port")))
                : QStringLiteral("nginx.enabled false -> mirror.publicPort %1 (LanRelay)")
                          .arg(configSource(config, QStringLiteral("mirror.publicPort")));
        writeRuntimeJson(webDir, pagePort, pagePortSource);
        if (http.mountStatic(QStringLiteral("/"), webDir, options)) {
            qInfo().noquote() << QStringLiteral("[Web] web page folder (%1): %2 -> http://<host>:%3/%4")
                                         .arg(source, QDir::toNativeSeparators(webDir))
                                         .arg(listener.port).arg(kWebPage);
        }
    }
    // One listener for the page and the CSV downloads.  A bind failure is only logged.
    if (http.start(listener.port, bindAddress)) {
        qInfo().noquote() << QStringLiteral("[Web] HTTP service listening on %1:%2 (web page %3, /exports downloads)")
                                     .arg(listener.bind).arg(http.port())
                                     .arg(webDir.isEmpty() ? QStringLiteral("NOT served") : QStringLiteral("served"));
    } else {
        qWarning().noquote() << QStringLiteral("[Web] HTTP service NOT started on %1:%2: %3 (web page and "
                                               "CSV downloads unavailable; the application keeps running)")
                                        .arg(listener.bind).arg(listener.port).arg(http.lastError());
    }
    if (QCoreApplication::instance()) {
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this,
                []() { AppHttpServer::instance().stop(); });
    }
}

namespace {
// w2-060 (Mango 2026-09-28): the REST API of RESTManager (original backend code) is enabled.
// It listens on the loopback address only; the LAN reaches it through nginx on port 80
// (deploy/nginx/taidaflow.conf: location /api/ -> proxy_pass http://127.0.0.1:<port>).
// w2-062: address and port come from config.json rest.bind / rest.port (default 127.0.0.1:18080;
// 18080 is not used by any other TaidaFlow service: 80 nginx, 502 Modbus server, 8124
// AppHttpServer, 8125 LAN relay, 18125 internal mirror). The former environment variable
// TAIDAFLOW_REST_PORT is no longer read. No access control (intranet, Mango's decision).

// The routes RESTManager::setupRoutes() registers, logged at start-up.  scripts/check-rest-routes.ps1
// compares this table with the m_httpServer.route(...) calls in RESTManager.cpp (exit 1 when they
// differ), so the log and the documentation cannot silently drift from the code.
struct RestRoute { const char *methods; const char *path; const char *purpose; };
constexpr RestRoute kRestRoutes[] = {
    {"GET",             "/",                               "status {\"status\": \"ok\"} (via nginx: /api/)"},
    {"GET,PUT,OPTIONS", "/api/settings/sensors",           "sensor key/name map (settings.sqlite sensor_config)"},
    {"GET,PUT,OPTIONS", "/api/settings/frequency",         "read_frequency (settings.sqlite app_settings)"},
    {"GET,PUT,OPTIONS", "/api/modbus/mode",                "mode network|standalone (in memory only)"},
    {"GET,OPTIONS",     "/api/sensor/range",               "sensor rows, from/to epoch seconds"},
    {"GET,OPTIONS",     "/api/holding/range",              "holding register rows, from/to epoch seconds"},
    {"GET,OPTIONS",     "/api/device/sn",                  "device serial number (device_info.ini)"},
    {"GET,OPTIONS",     "/api/sensor/last",                "newest sensor row of the current month"},
    {"GET,OPTIONS",     "/api/holding/last",               "newest holding register row of the current month"},
    {"GET,OPTIONS",     "/api/sensor/rangeDateTime",       "sensor rows, from/to ISO date-time"},
    {"GET,OPTIONS",     "/api/sensor/rangeDateTimePage",   "sensor rows paged (page, pageSize <= 1000)"},
    {"GET,OPTIONS",     "/api/holding/rangeDateTime",      "holding register rows, from/to ISO date-time"},
    {"GET,OPTIONS",     "/api/holding/rangeDateTimePage",  "holding register rows paged (page, pageSize <= 1000)"},
};

} // namespace

void Core::startRestServer()
{
    if (m_rest || !m_sqlManager)
        return;
    const AppConfig &config = AppConfig::instance();
    const AppConfig::Listener rest = config.rest();
    const quint16 port = rest.port;
    const QHostAddress address(rest.bind);
    qInfo().noquote() << QStringLiteral("[Config] Core REST API (rest) -> %1:%2 (%3)")
                                 .arg(rest.bind).arg(rest.port)
                                 .arg(configSources(config, QStringLiteral("rest"),
                                                    {QStringLiteral("bind"), QStringLiteral("port")}));
    if (!address.isLoopback()) {
        qWarning().noquote() << QStringLiteral("[REST] rest.bind %1 is not a loopback address: the REST API (no "
                                               "access control) is reachable directly on the network, not only "
                                               "through nginx /api/")
                                        .arg(rest.bind);
    }
    m_rest = new RESTManager(m_sqlManager, this);
    // RESTManager::start() reads (and creates, when missing) device_info.ini in the working
    // directory, registers the routes and listens.  A failure is only logged; the application
    // keeps running without the REST API.
    if (!m_rest->start(port, address)) {
        qWarning().noquote() << QStringLiteral("[REST] REST API NOT started on %1:%2 (port in use or not "
                                               "allowed?) - http://<host>/api/ answers 502 from nginx; the "
                                               "application keeps running")
                                        .arg(address.toString()).arg(port);
        delete m_rest;
        m_rest = nullptr;
        return;
    }
    qInfo().noquote() << QStringLiteral("[REST] REST API listening on %1:%2 (%3; LAN: "
                                        "http://<host>/api/... through nginx; config.json rest.port %4)")
                                 .arg(address.toString()).arg(port)
                                 .arg(address.isLoopback() ? QStringLiteral("loopback only")
                                                           : QStringLiteral("NOT loopback"),
                                      configSource(config, QStringLiteral("rest.port")));
    for (const RestRoute &r : kRestRoutes) {
        qInfo().noquote() << QStringLiteral("[REST] route %1 %2 - %3")
                                     .arg(QLatin1String(r.methods), -16)
                                     .arg(QLatin1String(r.path), QLatin1String(r.purpose));
    }
    if (QCoreApplication::instance()) {
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this,
                [this]() { stopRestServer(); });
    }
}

void Core::stopRestServer()
{
    if (!m_rest)
        return;
    // Deleting RESTManager closes its listening socket (QTcpServer) and the QHttpServer with
    // its open connections.
    delete m_rest;
    m_rest = nullptr;
    qInfo().noquote() << QStringLiteral("[REST] REST API stopped");
}

void Core::reportIgnoredHmiInputSettings()
{
    // Read-only: the file is not modified here.  Other groups (e.g. [Alarm]
    // aiHighAlarmPercent, handled by Manager) are unaffected.
    QSettings settings(QString::fromLatin1(kHmiInputSettingsFile), QSettings::IniFormat);
    settings.beginGroup(QString::fromLatin1(kHmiInputSettingsGroup));
    const QStringList keys = settings.childKeys();
    QStringList pairs;
    for (const QString &key : keys)
        pairs.append(QStringLiteral("%1=%2").arg(key, settings.value(key).toString()));
    settings.endGroup();

    if (pairs.isEmpty()) {
        qInfo().noquote()
                << QStringLiteral("[HmiInput] SVs are not restored from %1; they are taken from the "
                                  "first successful device read.")
                           .arg(settings.fileName());
    } else {
        qInfo().noquote()
                << QStringLiteral("[HmiInput] Ignoring stale [%1] values in %2 (%3); SVs are taken "
                                  "from the first successful device read instead.")
                           .arg(QString::fromLatin1(kHmiInputSettingsGroup),
                                settings.fileName(),
                                pairs.join(QStringLiteral(", ")));
    }
}

void Core::setHistoryTitleOnce()
{
    // w2-039: the column titles never change, so they are set once at start-up
    // instead of on every load.
    if (!m_proxy)
        return;

    // w2-041: the same 17 titles are the CSV export header (after 序號), so
    // they are defined once in HistoryExport.
    m_proxy->setHistoryTitle(HistoryExport::historyColumnTitles());
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
    // Only the newest rows are logged field by field, the rest are counted.
    constexpr qsizetype kLoggedAlarmRows = 8;
    int unhandledCount = 0;
    int resolvedCount = 0;
    int criticalCount = 0;
    int warningCount = 0;
    int unknownStatusCount = 0;
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
        const bool hasStatus = reasonObject.value(QStringLiteral("status")).isString();
        const QString coreStatus = reasonObject.value(QStringLiteral("status")).toString();
        const bool isResolved = reasonObject.value(QStringLiteral("resolved")).toBool(false);
        AlarmUiFields ui = alarmUiFieldsForStatus(coreStatus, hasStatus, isResolved);
        const QString storedSeverity = reasonObject.value(QStringLiteral("severity")).toString();
        if (storedSeverity == QStringLiteral("嚴重") || storedSeverity == QStringLiteral("警告"))
            ui.severity = storedSeverity;
        if (!ui.known)
            ++unknownStatusCount;
        if (ui.alarmStatus == QStringLiteral("未處理"))
            ++unhandledCount;
        else
            ++resolvedCount;
        if (ui.severity == QStringLiteral("嚴重"))
            ++criticalCount;
        else
            ++warningCount;

        const qint64 alarmId = static_cast<qint64>(alarm.value(QStringLiteral("id")).toDouble());
        if (records.size() < kLoggedAlarmRows) {
            qInfo().noquote()
                    << QStringLiteral("[Alarm][UI] id=%1 sensor=%2 message=%3 coreStatus=%4%5 -> alarmStatus=%6 severity=%7")
                               .arg(alarmId)
                               .arg(sensor, alarmMessage,
                                    hasStatus ? coreStatus : QStringLiteral("(none)"),
                                    isResolved ? QStringLiteral(" resolved=true") : QString(),
                                    ui.alarmStatus, ui.severity);
        }
        records.append(QVariantMap{
            {QStringLiteral("id"), static_cast<qint64>(
                    alarm.value(QStringLiteral("id")).toDouble())},
            {QStringLiteral("timestampMs"), occurrence * 1000},
            {QStringLiteral("alarmTime"), QDateTime::fromSecsSinceEpoch(occurrence)
                     .toString(QStringLiteral("yyyy/MM/dd HH:mm"))},
            {QStringLiteral("equipment"), QStringLiteral("系統")},
            {QStringLiteral("sensorName"), sensor},
            {QStringLiteral("alarmMessage"), alarmMessage},
            {QStringLiteral("severity"), ui.severity},
            {QStringLiteral("alarmStatus"), ui.alarmStatus},
        });
    }

    qInfo().noquote()
            << QStringLiteral("[SQL] Loaded %1 alarm-history records from the last 3 days into the UI "
                              "(alarmStatus 未處理=%2 已解除=%3; severity 嚴重=%4 警告=%5; unknown core status=%6).")
                       .arg(records.size())
                       .arg(unhandledCount)
                       .arg(resolvedCount)
                       .arg(criticalCount)
                       .arg(warningCount)
                       .arg(unknownStatusCount);
    m_proxy->setAlarmRecords(records);
}
