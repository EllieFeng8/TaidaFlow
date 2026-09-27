#include "core.h"

#include "AppHttpServer/AppHttpServer.h"
#include "HistoryExport.h"
#include "Modbus_Server.h"
#include "SqlManager.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
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
}

Core& Core::instance()
{
    static Core inst; // 建立唯一的靜態實例
    return inst;      // 回傳該實例的引用
}

Core::~Core()
{
    // w2-039: stop History results first.  A load may still be running on the
    // SqlManager thread; after this disconnect its result is not delivered to
    // Core, and a result already queued for Core is removed with Core by
    // ~QObject.  The SqlManager side only captures its own 'this' and values.
    if (m_sqlManager)
        disconnect(m_sqlManager, nullptr, this, nullptr);
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

void Core::init()
{
    if (m_proxy)
        return;

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
    // w2-039 (Mango): the History page is loaded only when it is shown
    // (historyRefreshRequested, emitted by HistoryPage.qml, relayed from WASM)
    // and when the page number changes, plus once at start-up.  It is no
    // longer reloaded on every saved sample (Manager::serverInputDataSaved is
    // still emitted, nothing here listens to it).  Loads run asynchronously on
    // the SqlManager thread; results come back queued.
    connect(m_sqlManager, &SqlManager::sensorHistoryPageReady,
            this, &Core::applyHistoryPage, Qt::QueuedConnection);
    connect(m_proxy, &TaidaFlowProxy::historyRefreshRequested, this,
            [this]() { loadHistoryRecords("page shown"); });
    connect(m_proxy, &TaidaFlowProxy::historyCurrentPageChanged, this, [this](int) {
        const bool rangeChange = m_historyRangeChangePending;
        m_historyRangeChangePending = false;
        loadHistoryRecords(rangeChange ? "range changed" : "page changed");
    });
    // w2-041 (spec §2): History range, relayed from WASM like the page number.
    connect(m_proxy, &TaidaFlowProxy::historyRangeRequested, this, &Core::onHistoryRangeRequested);
    // w2-041 (spec §3): raw CSV export queue/engine (export folder <working
    // directory>/exports); it mounts GET /exports/<file> on the AppHttpServer
    // singleton, which also serves the web page (w2-049, startHttpServer).
    // w2-050: the port of the download links comes from TAIDAFLOW_DOWNLOAD_PORT (default 8124;
    // 8123 when nginx serves the export folder).
    HistoryExportManager::Options exportOptions;
    exportOptions.downloadPort = HistoryExport::downloadPortFromEnvironment();
    m_historyExport = new HistoryExportManager(m_proxy, m_sqlManager, exportOptions, this);
    startHttpServer();
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
    loadHistoryRecords("start-up");
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

void Core::onHistoryRangeRequested(double fromMs, double toMs)
{
    // w2-041 (spec §2): epoch ms, both ends inclusive; 0 .. 8640000000000000
    // is "all months".  The range and the page are shared by every client.
    if (!m_proxy)
        return;
    qint64 fromSec = 0;
    qint64 toSec = 0;
    if (!HistoryExport::rangeMsToSecs(fromMs, toMs, &fromSec, &toSec)) {
        qWarning().noquote() << QStringLiteral("[History] range request ignored: invalid range %1 .. %2 ms")
                                        .arg(fromMs, 0, 'f', 0).arg(toMs, 0, 'f', 0);
        return;
    }
    qInfo().noquote() << QStringLiteral("[History] range requested: %1 .. %2 ms (%3 .. %4 s)")
                                 .arg(fromMs, 0, 'f', 0).arg(toMs, 0, 'f', 0).arg(fromSec).arg(toSec);
    m_proxy->setHistoryRangeFromMs(fromMs);
    m_proxy->setHistoryRangeToMs(toMs);
    if (m_proxy->historyCurrentPage() != 1) {
        m_historyRangeChangePending = true;
        m_proxy->setHistoryCurrentPage(1);        // loads through historyCurrentPageChanged
        return;
    }
    loadHistoryRecords("range changed");
}

namespace {
constexpr int kHistoryPageSize = 10;
}

void Core::loadHistoryRecords(const char *reason)
{
    if (!m_proxy)
        return;

    if (!m_sqlManager) {
        qWarning() << "[SQL] Sensor history skipped: SqlManager is unavailable.";
        m_proxy->setHistoryRecords(QVariantList{});
        return;
    }

    // w2-041 (spec §2): the range is the shared historyRangeFromMs/ToMs (the
    // Proxy starts with the current month, as before; 0 .. 8640000000000000 =
    // all months), converted to whole seconds, and may span several months.
    qint64 from = 0;
    qint64 to = 0;
    if (!HistoryExport::rangeMsToSecs(m_proxy->historyRangeFromMs(), m_proxy->historyRangeToMs(),
                                      &from, &to)) {
        const QDate today = QDate::currentDate();
        const QDate monthStart(today.year(), today.month(), 1);
        from = QDateTime(monthStart, QTime(0, 0)).toSecsSinceEpoch();
        to = QDateTime(monthStart.addMonths(1), QTime(0, 0)).addSecs(-1).toSecsSinceEpoch();
        qWarning().noquote() << "[History] invalid historyRangeFromMs/ToMs; using the current month.";
    }
    const int page = m_proxy->historyCurrentPage();

    // w2-039: asynchronous.  The per-month COUNTs plus one newest-first page
    // run on the SqlManager thread; applyHistoryPage() receives the result
    // queued.  Only the result of the newest request is applied.
    const quint64 requestId = ++m_historyRequestId;
    m_historyRequestClock.start();
    m_sqlManager->requestSensorHistoryRangePage(requestId, from, to, page, kHistoryPageSize);
    qInfo().noquote()
            << QStringLiteral("[History] request #%1 (%2) page %3, range %4 .. %5 s, posted to SqlManager thread in %6 us.")
                       .arg(requestId)
                       .arg(QString::fromLatin1(reason))
                       .arg(page)
                       .arg(from)
                       .arg(to)
                       .arg(m_historyRequestClock.nsecsElapsed() / 1000);
}

void Core::applyHistoryPage(const SensorHistoryPageResult &result)
{
    if (!m_proxy)
        return;

    if (result.requestId != m_historyRequestId) {
        qInfo().noquote()
                << QStringLiteral("[History] result #%1 (page %2) dropped: stale, newest request is #%3%4.")
                           .arg(result.requestId)
                           .arg(result.page)
                           .arg(m_historyRequestId)
                           .arg(result.superseded ? QStringLiteral(" (not executed by SqlManager)")
                                                  : QString());
        return;
    }
    const double roundTripMs = m_historyRequestClock.nsecsElapsed() / 1.0e6;

    if (!result.countOk) {
        qWarning().noquote() << "[SQL] Failed to count sensor-history rows:" << result.errorMessage;
        m_proxy->setHistoryTotalPages(1);
        m_proxy->setHistoryRecords(QVariantList{});
        return;
    }

    const qint64 totalRows = result.totalRows;
    const int totalPages = totalRows > 0
            ? static_cast<int>((totalRows + kHistoryPageSize - 1) / kHistoryPageSize)
            : 1;
    m_proxy->setHistoryTotalPages(totalPages);

    if (result.page > totalPages) {
        // Emits historyCurrentPageChanged, which requests the last page.
        qInfo().noquote()
                << QStringLiteral("[History] #%1 page %2 is past the last page %3; moving to page %3.")
                           .arg(result.requestId)
                           .arg(result.page)
                           .arg(totalPages);
        m_proxy->setHistoryCurrentPage(totalPages);
        return;
    }
    if (!result.ok) {
        qWarning().noquote() << "[SQL] Failed to load sensor-history page:" << result.errorMessage;
        m_proxy->setHistoryRecords(QVariantList{});
        return;
    }

    // SqlManager returns the page newest first (ORDER BY timestamp DESC), the
    // order the History page shows; no chronological re-paging is needed.
    constexpr double adcFullScale = 65535.0;
    QVariantList records;
    records.reserve(result.samples.size());
    for (const QJsonValue &sampleValue : result.samples) {
        const QJsonObject sample = sampleValue.toObject();
        const qint64 timestamp = static_cast<qint64>(
                sample.value(QStringLiteral("ts")).toDouble());
        if (timestamp <= 0)
            continue;

        const auto valueAt = [&sample](int sensorIndex, double scale = 1.0) -> QVariant {
            const QJsonValue sensorValue = sample.value(
                    QStringLiteral("s%1").arg(sensorIndex));
            if (sensorValue.isNull() || sensorValue.isUndefined())
                return {};
            bool isNumber = false;
            const double rawValue = sensorValue.toVariant().toDouble(&isNumber);
            return isNumber ? QVariant(rawValue * scale) : QVariant();
        };

        const QDateTime sampleTime = QDateTime::fromSecsSinceEpoch(timestamp);
        QVariantList values{sampleTime.toString(QStringLiteral("yyyy/MM/dd HH:mm:ss"))};
        for (int sensorIndex = 1; sensorIndex <= 4; ++sensorIndex)
            values.append(valueAt(sensorIndex, 100.0 / adcFullScale));
        for (int sensorIndex = 5; sensorIndex <= 11; ++sensorIndex)
            values.append(valueAt(sensorIndex, 1000.0 / adcFullScale));
        values.append(valueAt(12));
        for (int sensorIndex = 13; sensorIndex <= 16; ++sensorIndex)
            values.append(valueAt(sensorIndex, 100.0 / adcFullScale));

        records.append(QVariantMap{
            {QStringLiteral("timestampMs"), timestamp * 1000},
            {QStringLiteral("values"), values}
        });
    }

    qInfo().noquote()
            << QStringLiteral("[SQL] Loaded sensor-history page %1/%2: %3 record(s).")
                       .arg(result.page)
                       .arg(totalPages)
                       .arg(records.size());
    qInfo().noquote()
            << QStringLiteral("[History] #%1 applied: page %2/%3, %4 record(s), total %5 row(s) in %9 month file(s) "
                              "(%10 count(s) from cache); SqlManager thread count %6 ms + page %7 ms; "
                              "request-to-apply %8 ms.")
                       .arg(result.requestId)
                       .arg(result.page)
                       .arg(totalPages)
                       .arg(records.size())
                       .arg(totalRows)
                       .arg(result.countMs, 0, 'f', 2)
                       .arg(result.pageMs, 0, 'f', 2)
                       .arg(roundTripMs, 0, 'f', 2)
                       .arg(result.months)
                       .arg(result.countCacheHits);
    // w2-045: the request ran as queued steps on the SqlManager thread.
    qInfo().noquote()
            << QStringLiteral("[History] #%1 steps: %2 (longest %3 ms), %4 count(s) updated by new rows, page read %5.")
                       .arg(result.requestId)
                       .arg(result.steps)
                       .arg(result.maxStepMs, 0, 'f', 2)
                       .arg(result.countCacheDeltas)
                       .arg(result.pageMethod);
    m_proxy->setHistoryRecords(records);
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
