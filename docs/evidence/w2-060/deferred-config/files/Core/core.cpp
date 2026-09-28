#include "core.h"

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
#include <QSettings>
#include <QStringList>
#include <QTemporaryFile>
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
}

Core& Core::instance()
{
    static Core inst; // 建立唯一的靜態實例
    return inst;      // 回傳該實例的引用
}

Core::~Core()
{
    // w2-039/w2-052: stop History results first.  A load may still be running
    // on the SqlManager thread; after this its result is not delivered, and a
    // result already queued is removed with the receiver by ~QObject.  The
    // SqlManager side only captures its own 'this' and values.
    if (m_sqlManager)
        disconnect(m_sqlManager, nullptr, this, nullptr);
    // w2-060: normally already stopped on aboutToQuit (no-op then).
    stopRestServer();
    if (m_historyViews) {
        delete m_historyViews;
        m_historyViews = nullptr;
    }
    // w2-041: normally already stopped on QCoreApplication::aboutToQuit; this
    // covers the other exit paths (export thread + download service).
    if (m_historyExport) {
        delete m_historyExport;
        m_historyExport = nullptr;
    }
    // w2-049: normally already stopped on aboutToQuit (no-op then).
    AppHttpServer::instance().stop();
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

namespace {
// w2-060 (Mango 2026-09-28): the data folder of a field installation comes from
// <exe folder>\config.json (key "dataDir"; the only site configuration file - the other keys
// useNginx / nginxPort / restPort are read by the start scripts). Order:
//   1. environment variable TAIDAFLOW_DATA_DIR (set by start-taidaflow.ps1 to the folder it uses,
//      so a -DataDir given to the script for one run wins; relative = to the current directory),
//   2. "dataDir" of <exe folder>\config.json (relative = to the exe folder),
//   3. nothing configured -> the working directory is not changed (development: run-desktop.ps1
//      starts build\desktop\TaidaFlowApp.exe in build\runtime-cwd; build\desktop has no config.json).
// The folder is created when missing and must be writable; then it becomes the working
// directory, before SqlManager / HistoryExport / RESTManager take their paths from it
// (settings.sqlite, data\, exports\, TaidaFlowSettings.ini, device_info.ini stay where they
// were: in the data folder). Any failure is only logged and the working directory stays as it
// was (never a crash, never an exit).
constexpr auto kDataDirEnv = "TAIDAFLOW_DATA_DIR";
constexpr auto kConfigFileName = "config.json";

void applyDataDirectory()
{
    const QString before = QDir::toNativeSeparators(QDir::currentPath());
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QString configPath = QDir::toNativeSeparators(QDir(exeDir).filePath(QLatin1String(kConfigFileName)));

    // config.json is always read (when present) so that its content is in the log.
    QString configDataDir;
    QFile file(configPath);
    if (!file.exists()) {
        qInfo().noquote() << QStringLiteral("[Config] %1 not found").arg(configPath);
    } else if (!file.open(QIODevice::ReadOnly)) {
        qWarning().noquote() << QStringLiteral("[Config] %1 cannot be read (%2) - ignored")
                                        .arg(configPath, file.errorString());
    } else {
        QByteArray bytes = file.readAll();
        if (bytes.startsWith("\xEF\xBB\xBF"))   // UTF-8 BOM (e.g. saved by an editor) is allowed
            bytes.remove(0, 3);
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            qWarning().noquote() << QStringLiteral("[Config] %1 is not a valid JSON object (%2 at offset %3; "
                                                   "a Windows path needs \\\\ or / in JSON, e.g. \"C:\\\\TaidaFlowData\") "
                                                   "- ignored")
                                            .arg(configPath,
                                                 error.error != QJsonParseError::NoError ? error.errorString()
                                                                                         : QStringLiteral("not an object"))
                                            .arg(error.offset);
        } else {
            const QJsonObject obj = doc.object();
            for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
                const QString key = it.key();
                if (key == QLatin1String("dataDir")) {
                    if (it.value().isString() && !it.value().toString().trimmed().isEmpty())
                        configDataDir = it.value().toString().trimmed();
                    else
                        qWarning().noquote() << QStringLiteral("[Config] %1: dataDir must be a non-empty string - ignored")
                                                        .arg(configPath);
                } else if (key == QLatin1String("useNginx") || key == QLatin1String("nginxPort")
                           || key == QLatin1String("restPort")) {
                    qInfo().noquote() << QStringLiteral("[Config] %1: %2=%3 (read by the start scripts, not by the app)")
                                                 .arg(configPath, key, it.value().toVariant().toString());
                } else if (!key.startsWith(QLatin1Char('_'))) {   // "_comment" etc.: silently ignored
                    qWarning().noquote() << QStringLiteral("[Config] %1: unsupported key \"%2\" - ignored")
                                                    .arg(configPath, key);
                }
            }
        }
    }

    QString source;
    QString requested;
    QString baseDir;
    const QString env = qEnvironmentVariable(kDataDirEnv).trimmed();
    if (!env.isEmpty()) {
        source = QStringLiteral("environment variable %1").arg(QLatin1String(kDataDirEnv));
        requested = env;
        baseDir = QDir::currentPath();
        if (!configDataDir.isEmpty())
            qInfo().noquote() << QStringLiteral("[Config] %1 overrides dataDir \"%2\" of config.json")
                                         .arg(QLatin1String(kDataDirEnv), configDataDir);
    } else if (!configDataDir.isEmpty()) {
        source = configPath;
        requested = configDataDir;
        baseDir = exeDir;
    } else {
        qInfo().noquote() << QStringLiteral("[Config] no data folder configured (%1 / config.json dataDir) - data "
                                            "folder = current working directory %2")
                                     .arg(QLatin1String(kDataDirEnv), before);
        return;
    }

    const QString dir = QDir::toNativeSeparators(QDir::cleanPath(QDir(baseDir).absoluteFilePath(requested)));
    if (!QDir().mkpath(dir)) {
        qWarning().noquote() << QStringLiteral("[Config] data folder \"%1\" (from %2) cannot be created - data folder "
                                               "stays the current working directory %3")
                                        .arg(dir, source, before);
        return;
    }
    {
        QTemporaryFile probe(QDir(dir).filePath(QStringLiteral(".write-test-XXXXXX")));
        if (!probe.open()) {
            qWarning().noquote() << QStringLiteral("[Config] data folder \"%1\" (from %2) is not writable (%3) - data "
                                                   "folder stays the current working directory %4")
                                            .arg(dir, source, probe.errorString(), before);
            return;
        }
    }   // the probe file is removed here
    if (!QDir::setCurrent(dir)) {
        qWarning().noquote() << QStringLiteral("[Config] cannot change the working directory to \"%1\" (from %2) - "
                                               "data folder stays %3")
                                        .arg(dir, source, before);
        return;
    }
    qInfo().noquote() << QStringLiteral("[Config] data folder %1 (from %2; working directory was %3)")
                                 .arg(QDir::toNativeSeparators(QDir::currentPath()), source, before);
}
} // namespace

void Core::init()
{
    if (m_proxy)
        return;

    // w2-060: must run before anything takes a path from the working directory (SqlManager,
    // HistoryExport, RESTManager, TaidaFlowSettings.ini).
    applyDataDirectory();

    m_proxy = new TaidaFlowProxy(this);
    // w2-036: SVs are no longer restored from TaidaFlowSettings.ini.  They
    // start at the proxy defaults and Manager replaces them with the actual
    // device state after each device's first successful read (no Modbus
    // write).  Old [HmiInput] keys are left in the file untouched, only logged.
    reportIgnoredHmiInputSettings();
    m_sqlManager = SqlManager::instance();
    if (!m_sqlManager->initialize())
        qWarning() << "SqlManager initialization failed; Server Input Registers will not be saved.";

    m_manager = new Manager(m_proxy, m_sqlManager, this);
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
    // w2-050: the port of the download links comes from TAIDAFLOW_DOWNLOAD_PORT (default 8124;
    // 8123 when nginx serves the export folder).
    HistoryExportManager::Options exportOptions;
    exportOptions.downloadPort = HistoryExport::downloadPortFromEnvironment();
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
    m_modbusServer->start();
    setHistoryTitleOnce();
    loadAlarmRecords();
}

namespace {
constexpr auto kWebDirEnv = "TAIDAFLOW_WEB_DIR";
const QString kWebPage = QStringLiteral("TaidaFlowApp.html");

// w2-049: folder of the WebAssembly build served at http://<host>:8124/.
// Order: TAIDAFLOW_WEB_DIR -> <exe folder>/web -> the repository's build/wasm-release
// (development default, compiled in; only if it exists).  A candidate is used when it
// holds TaidaFlowApp.html.  Every candidate and the result are logged.
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
#ifdef TAIDAFLOW_DEV_WEB_DIR
    candidates.append({QStringLiteral("development default"), QStringLiteral(TAIDAFLOW_DEV_WEB_DIR)});
#endif
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
    QString source;
    const QString webDir = resolveWebDir(&source);
    if (webDir.isEmpty()) {
        qWarning().noquote() << QStringLiteral("[Web] no web page folder found (set %1, or deploy with "
                                               "scripts\\deploy-web.ps1 to <exe folder>\\web) - the web page "
                                               "is not served; /exports downloads work as before")
                                        .arg(QLatin1String(kWebDirEnv));
    } else {
        // Replaces the former Python development server (COOP/COEP/CORP on, as before),
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
        if (http.mountStatic(QStringLiteral("/"), webDir, options)) {
            qInfo().noquote() << QStringLiteral("[Web] web page folder (%1): %2 -> http://<host>:%3/%4")
                                         .arg(source, QDir::toNativeSeparators(webDir))
                                         .arg(HistoryExport::kDefaultDownloadPort).arg(kWebPage);
        }
    }
    // One listener for the page and the CSV downloads.  A bind failure is only logged.
    if (http.start(HistoryExport::kDefaultDownloadPort, QHostAddress::AnyIPv4)) {
        qInfo().noquote() << QStringLiteral("[Web] HTTP service listening on 0.0.0.0:%1 (web page %2, /exports downloads)")
                                     .arg(http.port())
                                     .arg(webDir.isEmpty() ? QStringLiteral("NOT served") : QStringLiteral("served"));
    } else {
        qWarning().noquote() << QStringLiteral("[Web] HTTP service NOT started on 0.0.0.0:%1: %2 (web page and "
                                               "CSV downloads unavailable; the application keeps running)")
                                        .arg(HistoryExport::kDefaultDownloadPort).arg(http.lastError());
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
// The port is TAIDAFLOW_REST_PORT (1..65535), default 18080 (free on the development PC and
// not used by any other TaidaFlow service: 80 nginx, 502 Modbus server, 8124 AppHttpServer,
// 8125 LAN relay, 18125 internal mirror). No access control (intranet, Mango's decision).
constexpr auto kRestPortEnv = "TAIDAFLOW_REST_PORT";
constexpr quint16 kDefaultRestPort = 18080;

// The routes RESTManager::setupRoutes() registers, logged at start-up.  scripts/check_rest_routes.py
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

quint16 restPortFromEnvironment()
{
    const QString text = qEnvironmentVariable(kRestPortEnv).trimmed();
    if (text.isEmpty())
        return kDefaultRestPort;
    bool ok = false;
    const uint value = text.toUInt(&ok);
    if (!ok || value < 1 || value > 65535) {
        qWarning().noquote() << QStringLiteral("[REST] %1=\"%2\" is not a port (1..65535) - using %3")
                                        .arg(QLatin1String(kRestPortEnv), text).arg(kDefaultRestPort);
        return kDefaultRestPort;
    }
    return static_cast<quint16>(value);
}
} // namespace

void Core::startRestServer()
{
    if (m_rest || !m_sqlManager)
        return;
    const quint16 port = restPortFromEnvironment();
    const QHostAddress address(QHostAddress::LocalHost);
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
    qInfo().noquote() << QStringLiteral("[REST] REST API listening on %1:%2 (loopback only; LAN: "
                                        "http://<host>/api/... through nginx; %3=%4)")
                                 .arg(address.toString()).arg(port)
                                 .arg(QLatin1String(kRestPortEnv),
                                      qEnvironmentVariableIsSet(kRestPortEnv) ? qEnvironmentVariable(kRestPortEnv)
                                                                             : QStringLiteral("(not set, default)"));
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
