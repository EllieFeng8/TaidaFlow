#include "core.h"

#include "Modbus_Server.h"
#include "SqlManager.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSettings>
#include <QTime>
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

    // Ensure every restorable HMI setting has an explicit value in the INI
    // file, even if the operator has never changed its default value.
    saveHmiInputSettings();

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
