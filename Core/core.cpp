#include "core.h"

#include "Modbus_Server.h"
#include "SqlManager.h"

#include <QDateTime>
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
    connect(m_manager, &Manager::serverInputDataSaved,
            this, &Core::loadHistoryRecords);
    connect(m_proxy, &TaidaFlowProxy::historyCurrentPageChanged, this,
            [this](int) { loadHistoryRecords(); });
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
    loadHistoryRecords();
    loadAlarmRecords();
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

void Core::loadHistoryRecords()
{
    if (!m_proxy)
        return;

    m_proxy->setHistoryTitle(QVariantList{
        QStringLiteral("時間"),
        QStringLiteral("TT-01 (°C)"), QStringLiteral("TT-02 (°C)"),
        QStringLiteral("TT-03 (°C)"), QStringLiteral("TT-04 (°C)"),
        QStringLiteral("PT-01 (bar)"), QStringLiteral("PT-02 (bar)"),
        QStringLiteral("PT-03 (bar)"), QStringLiteral("PT-04 (bar)"),
        QStringLiteral("PT-05 (bar)"), QStringLiteral("PT-06 (bar)"),
        QStringLiteral("PT-07 (bar)"), QStringLiteral("FM-01 (L/min)"),
        QStringLiteral("M1 (%)"), QStringLiteral("M2 (%)"),
        QStringLiteral("M3 (%)"), QStringLiteral("M4 (%)")
    });

    QVariantList records;
    if (!m_sqlManager) {
        qWarning() << "[SQL] Sensor history skipped: SqlManager is unavailable.";
        m_proxy->setHistoryRecords(records);
        return;
    }

    constexpr int kHistoryPageSize = 10;
    const QDate today = QDate::currentDate();
    const QDate monthStart(today.year(), today.month(), 1);
    const qint64 from = QDateTime(monthStart, QTime(0, 0)).toSecsSinceEpoch();
    const qint64 to = QDateTime(monthStart.addMonths(1), QTime(0, 0))
                           .addSecs(-1)
                           .toSecsSinceEpoch();

    qint64 totalRows = 0;
    QString errorMessage;
    if (!m_sqlManager->countSensorRange(from, to, &totalRows, &errorMessage)) {
        qWarning().noquote() << "[SQL] Failed to count sensor-history rows:" << errorMessage;
        m_proxy->setHistoryTotalPages(1);
        m_proxy->setHistoryRecords(records);
        return;
    }

    const int totalPages = totalRows > 0
            ? static_cast<int>((totalRows + kHistoryPageSize - 1) / kHistoryPageSize)
            : 1;
    m_proxy->setHistoryTotalPages(totalPages);

    const int currentPage = m_proxy->historyCurrentPage();
    if (currentPage > totalPages) {
        m_proxy->setHistoryCurrentPage(totalPages);
        return;
    }
    if (totalRows == 0) {
        m_proxy->setHistoryRecords(records);
        return;
    }

    // SqlManager reads chronological pages.  This calculates the matching
    // chronological interval for a newest-first UI page.
    const qint64 endRow = totalRows
            - static_cast<qint64>(currentPage - 1) * kHistoryPageSize;
    const qint64 startRow = qMax<qint64>(0, endRow - kHistoryPageSize);
    const int firstSqlPage = static_cast<int>(startRow / kHistoryPageSize) + 1;
    const int lastSqlPage = static_cast<int>((endRow - 1) / kHistoryPageSize) + 1;

    QJsonArray samples;
    for (int sqlPage = firstSqlPage; sqlPage <= lastSqlPage; ++sqlPage) {
        QJsonArray pageSamples;
        if (!m_sqlManager->queryRangeJsonPaged(from, to, sqlPage,
                                                kHistoryPageSize, &pageSamples,
                                                &errorMessage)) {
            qWarning().noquote() << "[SQL] Failed to load sensor-history page:"
                                 << errorMessage;
            m_proxy->setHistoryRecords(QVariantList{});
            return;
        }

        const qint64 pageStartRow = static_cast<qint64>(sqlPage - 1) * kHistoryPageSize;
        const qint64 copyStart = qMax(startRow, pageStartRow);
        const qint64 copyEnd = qMin(endRow, pageStartRow
                                    + static_cast<qint64>(pageSamples.size()));
        for (qint64 row = copyStart; row < copyEnd; ++row)
            samples.append(pageSamples.at(static_cast<qsizetype>(row - pageStartRow)));
    }

    constexpr double adcFullScale = 65535.0;
    records.reserve(samples.size());
    for (const QJsonValue &sampleValue : samples) {
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
                       .arg(currentPage)
                       .arg(totalPages)
                       .arg(records.size());
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
