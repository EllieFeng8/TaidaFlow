#pragma once
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <sstream>
#include <iomanip>
#include <QVariantList>
#include <QVariantMap>
#include <QStringList>
#include <string>
#include <QDir>
#include <QGuiApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTextStream>
#include <QCoreApplication>
#include <QDebug>
#include <QDateTime>
#include <QFileDialog>
#include <QTimer>
#include <cmath>
class TaidaFlowProxy : public QObject
{
    Q_OBJECT

    // Sensor 設定（SV）：設定頁按「套用」後寫入，透過 Mirror 同步。
    // 外層 key 與分組：
    //   pt04 / pt05：設備接口出口壓力；tt01 / tt02：設備接口出口溫度。
    //   pt06 / pt07：設備接口入口壓力；tt03 / tt04：設備接口入口溫度。
    //   pt01、pt02、pt03、flowMeter（流量計）、filter（過濾器壓差）各自一組。
    // 每個 key 對應一個 QVariantMap，各 sensor 獨立設定：
    //   offset (double)：加在原始 PV 上的校正值，可為負數。
    //   lower / upper (double)：下限 / 上限。
    //   lowerEnabled / upperEnabled (bool)：是否啟用對應限制；UI 留空表示停用。
    // 儲存單位固定為：PT 與 filter 使用 kPa、TT 使用 °C、flowMeter 使用 L/min。
    // filter 比較含各自 offset 的 PT-02 − PT-03；UI 不提供 filter 獨立 offset。
    // 例如 filter 下限 10、上限 20 kPa：
    //   {offset: 0, lower: 10, upper: 20, lowerEnabled: true, upperEnabled: true}
    // 漏水感測器不在此 Map 內，仍由 leakDetectedPv 提供狀態。
    // 修改時須複製目前整份 Map、更新指定項目，再寫回；此 Proxy 本身不負責持久化。
    Q_PROPERTY(QVariantMap sensorSettingsSv READ sensorSettingsSv WRITE setSensorSettingsSv NOTIFY sensorSettingsSvChanged)

    // 壓力顯示單位（SV）：下拉選單立即寫入，只接受 "kPa" / "psi" / "bar"，預設 kPa。
    // UI 換算顯示數值與設定欄位；原始壓力 PV、offset、上下限仍以 kPa 儲存。
    Q_PROPERTY(QString pressureUnitSv READ pressureUnitSv WRITE setPressureUnitSv NOTIFY pressureUnitSvChanged)

    // Writable set values (SV): edited by TextField / switch controls.
    Q_PROPERTY(double m1ValueSv READ m1ValueSv WRITE setM1ValueSv NOTIFY m1ValueSvChanged)
    Q_PROPERTY(double m2ValueSv READ m2ValueSv WRITE setM2ValueSv NOTIFY m2ValueSvChanged)
    Q_PROPERTY(double m3ValueSv READ m3ValueSv WRITE setM3ValueSv NOTIFY m3ValueSvChanged)
    Q_PROPERTY(double m4ValueSv READ m4ValueSv WRITE setM4ValueSv NOTIFY m4ValueSvChanged)
    Q_PROPERTY(double m1ValuePv READ m1ValuePv WRITE setM1ValuePv NOTIFY m1ValuePvChanged)
    Q_PROPERTY(double m2ValuePv READ m2ValuePv WRITE setM2ValuePv NOTIFY m2ValuePvChanged)
    Q_PROPERTY(double m3ValuePv READ m3ValuePv WRITE setM3ValuePv NOTIFY m3ValuePvChanged)
    Q_PROPERTY(double m4ValuePv READ m4ValuePv WRITE setM4ValuePv NOTIFY m4ValuePvChanged)
    Q_PROPERTY(double pump2HzSv READ pump2HzSv WRITE setPump2HzSv NOTIFY pump2HzSvChanged)
    Q_PROPERTY(double pump2HzPv READ pump2HzPv WRITE setPump2HzPv NOTIFY pump2HzPvChanged)
    Q_PROPERTY(bool motorRunningSv READ motorRunningSv WRITE setMotorRunningSv NOTIFY motorRunningSvChanged)
    Q_PROPERTY(bool motorRunningPv READ motorRunningPv WRITE setMotorRunningPv NOTIFY motorRunningPvChanged)
    Q_PROPERTY(bool wayValveOpenSv READ wayValveOpenSv WRITE setWayValveOpenSv NOTIFY wayValveOpenSvChanged)
    Q_PROPERTY(bool wayValveOpenPv READ wayValveOpenPv WRITE setWayValveOpenPv NOTIFY wayValveOpenPvChanged)
    // Inverter commands. Reset is momentary in the UI; emergency stop retains
    // its state until the operator releases it.
    Q_PROPERTY(bool inverterResetSv READ inverterResetSv WRITE setInverterResetSv NOTIFY inverterResetSvChanged)
    Q_PROPERTY(bool emergencyStopSv READ emergencyStopSv WRITE setEmergencyStopSv NOTIFY emergencyStopSvChanged)

    // Read-only process values (PV): QML can observe but cannot write.
    Q_PROPERTY(double tt01ValuePv READ tt01ValuePv WRITE setTt01ValuePv NOTIFY tt01ValuePvChanged)
    Q_PROPERTY(double tt02ValuePv READ tt02ValuePv WRITE setTt02ValuePv NOTIFY tt02ValuePvChanged)
    Q_PROPERTY(double tt03ValuePv READ tt03ValuePv WRITE setTt03ValuePv NOTIFY tt03ValuePvChanged)
    Q_PROPERTY(double tt04ValuePv READ tt04ValuePv WRITE setTt04ValuePv NOTIFY tt04ValuePvChanged)
    Q_PROPERTY(double pt01ValuePv READ pt01ValuePv WRITE setPt01ValuePv NOTIFY pt01ValuePvChanged)
    Q_PROPERTY(double pt02ValuePv READ pt02ValuePv WRITE setPt02ValuePv NOTIFY pt02ValuePvChanged)
    Q_PROPERTY(double pt03ValuePv READ pt03ValuePv WRITE setPt03ValuePv NOTIFY pt03ValuePvChanged)
    Q_PROPERTY(double pt04ValuePv READ pt04ValuePv WRITE setPt04ValuePv NOTIFY pt04ValuePvChanged)
    Q_PROPERTY(double pt05ValuePv READ pt05ValuePv WRITE setPt05ValuePv NOTIFY pt05ValuePvChanged)
    Q_PROPERTY(double pt06ValuePv READ pt06ValuePv WRITE setPt06ValuePv NOTIFY pt06ValuePvChanged)
    Q_PROPERTY(double pt07ValuePv READ pt07ValuePv WRITE setPt07ValuePv NOTIFY pt07ValuePvChanged)
    Q_PROPERTY(double flowMeterValuePv READ flowMeterValuePv WRITE setFlowMeterValuePv NOTIFY flowMeterValuePvChanged)
    // Leakage sensor state: written by the core backend from ADAM-6224 DI1 (1 = leak detected).
    Q_PROPERTY(bool leakDetectedPv READ leakDetectedPv WRITE setLeakDetectedPv NOTIFY leakDetectedPvChanged)

    // List data is owned by the authoritative side. QML only reads a local copy.
    // historyTitle (column titles) stays shared by all clients (spec §2.1).
    Q_PROPERTY(QVariantList historyTitle READ historyTitle WRITE setHistoryTitle NOTIFY historyTitleChanged)
    Q_PROPERTY(QVariantList alarmRecords READ alarmRecords WRITE setAlarmRecords NOTIFY alarmRecordsChanged)

    // Per-client history views (docs/taidaflow_history_export_spec.md §2.1, revised 2026-09-27;
    // replaces the former shared records / page / range properties and their two requests).
    // Mirrored (Core -> all clients), written only by the Core. Key = the client's
    // clientSessionId ("desktop", "web-xxxx"); value = map
    //   { fromMs, toMs,   // this client's range, local-epoch ms, both ends inclusive
    //     page,           // current page, 1-based
    //     totalPages,     // >= 1
    //     totalRows,      // rows in the whole range
    //     records,        // this page (10 rows), each { timestampMs, values }
    //                     //   (values: one cell per historyTitle column, in that order)
    //     revision }      // number; changes whenever this entry's content changes
    // Each client's QML reads only historyViews[clientSessionId] and redraws only when that
    // entry's revision changes, so one client filtering / paging never changes another
    // client's screen. A missing key = not loaded yet: the Core creates the entry on the
    // client's first historyViewRequested, removes web entries idle for 30 min (never
    // "desktop") and keeps at most 32; a removed client's next request rebuilds it.
    Q_PROPERTY(QVariantMap historyViews READ historyViews WRITE setHistoryViews NOTIFY historyViewsChanged)
    // Mirrored (Core -> all clients): export jobs keyed by clientSessionId; each value is a map
    // {state, progress, queuePosition, rowsWritten, totalRows, fileName, url, downloadPort,
    // savedPath, message} (spec §3.2; url is the path "/exports/<file>" and downloadPort the
    // Core's download port, spec §3.5 revised). Written only by the Core; each client's QML
    // reads only its own key.
    Q_PROPERTY(QVariantMap historyExportStatus READ historyExportStatus WRITE setHistoryExportStatus NOTIFY historyExportStatusChanged)

    // ---- Per-client alarm views (w1-078, 2026-09-30; same pattern as historyViews) ----
    // Mirrored (Core -> all clients), written only by the Core. Key = the client's
    // clientSessionId ("desktop", "web-xxxx"); value = map
    //   { fromMs, toMs,   // this client's range, local-epoch ms, both ends inclusive
    //                     //   (from = start minute :00.000, to = end minute :59.999)
    //     page,           // current page, 1-based, within 1..totalPages
    //     pageSize,       // 9 (rows per page of the alarm page)
    //     totalCount,     // alarms in the whole range
    //     totalPages,     // >= 1 (1 when totalCount is 0)
    //     activeCount,    // alarms in the whole range whose alarmStatus is "未處理"
    //     rows,           // this page (<= pageSize rows), each map with the alarmRecords fields
    //                     //   timestampMs, alarmTime ("yyyy/MM/dd HH:mm"), equipment, sensorName,
    //                     //   alarmMessage, severity ("嚴重"/"警告"), alarmStatus ("未處理"/"已解除")
    //                     //   plus serialNumber = 1-based position in the whole range
    //                     //   ((page - 1) * pageSize + index + 1)
    //     state,          // "ready", or "error" when the range could not be read
    //     message,        // error text when state is "error" (the page shows it), else ""
    //     revision }      // number; changes whenever this entry's content changes
    // Order of the range (and so of the pages): newest first = timestampMs descending, equal
    // times by alarm id descending - the order alarmRecords has today (Core::loadAlarmRecords).
    // The Core (re)writes a client's entry on its alarmViewRequested, when a new alarm inside
    // the entry's range is saved and when an alarm inside it is resolved (rows / activeCount),
    // keeping the client's range and page (page clamped to the new totalPages). Cleanup as
    // historyViews: web entries idle for 30 min are removed (never "desktop"), at most 32 kept
    // (least recently used removed); a removed client's next request rebuilds its entry.
    // AlarmPage.qml reads only alarmViews[clientSessionId] and redraws only when that entry's
    // revision changes. Without its entry (Core without alarm views, main alone, or removed)
    // the page filters alarmRecords itself; alarmRecords itself is unchanged.
    Q_PROPERTY(QVariantMap alarmViews READ alarmViews WRITE setAlarmViews NOTIFY alarmViewsChanged)
    // Local only (STORED false, never mirrored): this client's id, "desktop" on the desktop,
    // "web-xxxx" per browser tab (set by main.cpp before the mirror is created, spec §1).
    // QML sends it with history view / export requests and uses it as the key into
    // historyViews and historyExportStatus.
    Q_PROPERTY(QString clientSessionId READ clientSessionId NOTIFY clientSessionIdChanged STORED false)
    // Local only (STORED false, never mirrored): host name of the page URL (location.hostname),
    // set by the WASM main.cpp before the mirror is created; "" on the desktop. HistoryPage.qml
    // builds the download link "http://" + pageHost + ":" + downloadPort + url (spec §3.5).
    Q_PROPERTY(QString pageHost READ pageHost NOTIFY pageHostChanged STORED false)

    // Local transport overlay (wasm-mirror pack 1.0.1, package-integration §6.2/§9).
    // STORED false keeps both properties out of the Mirror contract, so the WASM
    // offline state is never written back into the authoritative desktop Proxy.
    // Desktop keeps the default `true`; only the WASM composition root (main.cpp)
    // switches it to false before QML loads and installs transportStateHandler.
    Q_PROPERTY(bool transportReady READ transportReady NOTIFY transportReadyChanged STORED false)
    Q_PROPERTY(QString transportMessage READ transportMessage NOTIFY transportMessageChanged STORED false)

public:
    // 回傳目前設定的副本；呼叫端更新後需透過 setter 寫回才會通知 UI / Mirror。
    QVariantMap sensorSettingsSv() const { return m_sensorSettingsSv; }
    QString pressureUnitSv() const { return m_pressureUnitSv; }

    // 忽略不支援或未改變的單位；不修改 sensorSettingsSv 或任何 PV 數值。
    void setPressureUnitSv(const QString &unit)
    {
        if (unit != QStringLiteral("kPa") && unit != QStringLiteral("psi") && unit != QStringLiteral("bar"))
            return;
        if (m_pressureUnitSv == unit)
            return;
        m_pressureUnitSv = unit;
        emit pressureUnitSvChanged();
    }

    // 整份驗證通過後才一次更新：sensor key 與欄位必須完整且不可多出未知項目。
    // 數值必須可轉成有限 double，啟用旗標必須為 bool；上下限皆啟用時須 lower <= upper。
    // 任一項無效就保留原設定；內容沒有改變時不發出通知。
    void setSensorSettingsSv(const QVariantMap &settings)
    {
        if (settings.keys() != m_sensorSettingsSv.keys())
            return;
        QVariantMap normalized;
        for (auto it = settings.cbegin(); it != settings.cend(); ++it) {
            const auto entry = it.value().toMap();
            if (entry.keys() != m_sensorSettingsSv.value(it.key()).toMap().keys())
                return;
            QVariantMap clean;
            for (const auto &key : {QStringLiteral("offset"), QStringLiteral("lower"), QStringLiteral("upper")}) {
                bool ok = false;
                const double value = entry.value(key).toDouble(&ok);
                if (!ok || !std::isfinite(value))
                    return;
                clean.insert(key, value);
            }
            for (const auto &key : {QStringLiteral("lowerEnabled"), QStringLiteral("upperEnabled")}) {
                if (entry.value(key).metaType().id() != QMetaType::Bool)
                    return;
                clean.insert(key, entry.value(key));
            }
            if (clean.value("lowerEnabled").toBool() && clean.value("upperEnabled").toBool()
                && clean.value("lower").toDouble() > clean.value("upper").toDouble())
                return;
            normalized.insert(it.key(), clean);
        }
        if (m_sensorSettingsSv == normalized)
            return;
        m_sensorSettingsSv = normalized;
        emit sensorSettingsSvChanged();
    }

    explicit TaidaFlowProxy(QObject *parent = nullptr)
        : QObject(parent)
    {
        // m_processTimer.setInterval(300);
        // connect(&m_processTimer, &QTimer::timeout,
        //         this, &TaidaFlowProxy::updateSimulatedProcess);
        // m_processTimer.start();
        initializeListData();
    }

    double m1ValueSv() const { return m_m1ValueSv; }
    double m2ValueSv() const { return m_m2ValueSv; }
    double m3ValueSv() const { return m_m3ValueSv; }
    double m4ValueSv() const { return m_m4ValueSv; }
    double m1ValuePv() const { return m_m1ValuePv; }
    double m2ValuePv() const { return m_m2ValuePv; }
    double m3ValuePv() const { return m_m3ValuePv; }
    double m4ValuePv() const { return m_m4ValuePv; }
    double pump2HzSv() const { return m_pump2HzSv; }
    double pump2HzPv() const { return m_pump2HzPv; }
    bool motorRunningSv() const { return m_motorRunningSv; }
    bool motorRunningPv() const { return m_motorRunningPv; }
    bool wayValveOpenSv() const { return m_wayValveOpenSv; }
    bool wayValveOpenPv() const { return m_wayValveOpenPv; }
    bool inverterResetSv() const { return m_inverterResetSv; }
    bool emergencyStopSv() const { return m_emergencyStopSv; }

    double tt01ValuePv() const { return m_tt01ValuePv; }
    double tt02ValuePv() const { return m_tt02ValuePv; }
    double tt03ValuePv() const { return m_tt03ValuePv; }
    double tt04ValuePv() const { return m_tt04ValuePv; }
    double pt01ValuePv() const { return m_pt01ValuePv; }
    double pt02ValuePv() const { return m_pt02ValuePv; }
    double pt03ValuePv() const { return m_pt03ValuePv; }
    double pt04ValuePv() const { return m_pt04ValuePv; }
    double pt05ValuePv() const { return m_pt05ValuePv; }
    double pt06ValuePv() const { return m_pt06ValuePv; }
    double pt07ValuePv() const { return m_pt07ValuePv; }
    double flowMeterValuePv() const { return m_flowMeterValuePv; }
    bool leakDetectedPv() const { return m_leakDetectedPv; }
    QVariantList historyTitle() const { return m_historyTitle; }
    QVariantList alarmRecords() const { return m_alarmRecords; }
    QVariantMap historyViews() const { return m_historyViews; }
    QVariantMap historyExportStatus() const { return m_historyExportStatus; }
    QString clientSessionId() const { return m_clientSessionId; }
    QString pageHost() const { return m_pageHost; }
    bool transportReady() const { return m_transportReady; }
    QString transportMessage() const { return m_transportMessage; }

    // Called only by the WASM composition root / transportStateHandler (main.cpp).
    void setTransportState(bool ready, const QString &message)
    {
        // Update both members before notifying so observers see a consistent pair.
        const bool readyChanged = m_transportReady != ready;
        const bool messageChanged = m_transportMessage != message;
        m_transportReady = ready;
        m_transportMessage = message;
        if (readyChanged)
            emit transportReadyChanged();
        if (messageChanged)
            emit transportMessageChanged();
    }

    void setM1ValueSv(double value) { setWritableValue(m_m1ValueSv, value, &TaidaFlowProxy::m1ValueSvChanged); }
    void setM2ValueSv(double value) { setWritableValue(m_m2ValueSv, value, &TaidaFlowProxy::m2ValueSvChanged); }
    void setM3ValueSv(double value) { setWritableValue(m_m3ValueSv, value, &TaidaFlowProxy::m3ValueSvChanged); }
    void setM4ValueSv(double value) { setWritableValue(m_m4ValueSv, value, &TaidaFlowProxy::m4ValueSvChanged); }
    void setM1ValuePv(double value) { setWritableValue(m_m1ValuePv, value, &TaidaFlowProxy::m1ValuePvChanged); }
    void setM2ValuePv(double value) { setWritableValue(m_m2ValuePv, value, &TaidaFlowProxy::m2ValuePvChanged); }
    void setM3ValuePv(double value) { setWritableValue(m_m3ValuePv, value, &TaidaFlowProxy::m3ValuePvChanged); }
    void setM4ValuePv(double value) { setWritableValue(m_m4ValuePv, value, &TaidaFlowProxy::m4ValuePvChanged); }
    void setPump2HzSv(double value) { setWritableValue(m_pump2HzSv, value, &TaidaFlowProxy::pump2HzSvChanged); }
    void setPump2HzPv(double value) { setWritableValue(m_pump2HzPv, value, &TaidaFlowProxy::pump2HzPvChanged); }
    void setInverterResetSv(bool value) { setBooleanValue(m_inverterResetSv, value, &TaidaFlowProxy::inverterResetSvChanged); }
    void setWayValveOpenSv(bool value) { setBooleanValue(m_wayValveOpenSv, value, &TaidaFlowProxy::wayValveOpenSvChanged); }
    void setWayValveOpenPv(bool value) { setBooleanValue(m_wayValveOpenPv, value, &TaidaFlowProxy::wayValveOpenPvChanged); }
    void setEmergencyStopSv(bool value) { setBooleanValue(m_emergencyStopSv, value, &TaidaFlowProxy::emergencyStopSvChanged); }
    void setTt01ValuePv(double value) { setProcessValue(m_tt01ValuePv, value, &TaidaFlowProxy::tt01ValuePvChanged); }
    void setTt02ValuePv(double value) { setProcessValue(m_tt02ValuePv, value, &TaidaFlowProxy::tt02ValuePvChanged); }
    void setTt03ValuePv(double value) { setProcessValue(m_tt03ValuePv, value, &TaidaFlowProxy::tt03ValuePvChanged); }
    void setTt04ValuePv(double value) { setProcessValue(m_tt04ValuePv, value, &TaidaFlowProxy::tt04ValuePvChanged); }
    void setPt01ValuePv(double value) { setProcessValue(m_pt01ValuePv, value, &TaidaFlowProxy::pt01ValuePvChanged); }
    void setPt02ValuePv(double value) { setProcessValue(m_pt02ValuePv, value, &TaidaFlowProxy::pt02ValuePvChanged); }
    void setPt03ValuePv(double value) { setProcessValue(m_pt03ValuePv, value, &TaidaFlowProxy::pt03ValuePvChanged); }
    void setPt04ValuePv(double value) { setProcessValue(m_pt04ValuePv, value, &TaidaFlowProxy::pt04ValuePvChanged); }
    void setPt05ValuePv(double value) { setProcessValue(m_pt05ValuePv, value, &TaidaFlowProxy::pt05ValuePvChanged); }
    void setPt06ValuePv(double value) { setProcessValue(m_pt06ValuePv, value, &TaidaFlowProxy::pt06ValuePvChanged); }
    void setPt07ValuePv(double value) { setProcessValue(m_pt07ValuePv, value, &TaidaFlowProxy::pt07ValuePvChanged); }
    void setFlowMeterValuePv(double value) { setProcessValue(m_flowMeterValuePv, value, &TaidaFlowProxy::flowMeterValuePvChanged); }
    void setLeakDetectedPv(bool value) { setBooleanValue(m_leakDetectedPv, value, &TaidaFlowProxy::leakDetectedPvChanged); }

    void setHistoryTitle(const QVariantList &title)
    {
        if (m_historyTitle == title)
            return;
        m_historyTitle = title;
        emit historyTitleChanged(title);
    }
    void setAlarmRecords(const QVariantList &records)
    {
        m_alarmRecords = records;
        emit alarmRecordsChanged(records);
    }
    // Written only by the Core, always the whole map (spec §2.1); an unchanged map is not re-sent.
    void setHistoryViews(const QVariantMap &views)
    {
        if (m_historyViews == views)
            return;
        m_historyViews = views;
        emit historyViewsChanged(views);
    }
    void setHistoryExportStatus(const QVariantMap &status)
    {
        if (m_historyExportStatus == status)
            return;
        m_historyExportStatus = status;
        emit historyExportStatusChanged(status);
    }

    // Per-client alarm views (w1-078): written only by the Core, always the whole map; an
    // unchanged map is not re-sent (same as setHistoryViews).
    QVariantMap alarmViews() const { return m_alarmViews; }
    void setAlarmViews(const QVariantMap &views)
    {
        if (m_alarmViews == views)
            return;
        m_alarmViews = views;
        emit alarmViewsChanged(views);
    }
    // Called only by the composition root (main.cpp) before the mirror is created.
    void setClientSessionId(const QString &sessionId)
    {
        if (m_clientSessionId == sessionId)
            return;
        m_clientSessionId = sessionId;
        emit clientSessionIdChanged();
    }
    // Called only by the WASM composition root (main.cpp) before the mirror is created.
    void setPageHost(const QString &host)
    {
        if (m_pageHost == host)
            return;
        m_pageHost = host;
        emit pageHostChanged();
    }
    void setMotorRunningSv(bool value)
    {
        m_motorRunningSv = value;
        emit motorRunningSvChanged(value);
    }
    void setMotorRunningPv(bool value)
    {
        m_motorRunningPv = value;
        emit motorRunningPvChanged(value);
    }
signals:
    // 設定 Map 實際更新後通知；接收端可重新讀取 sensorSettingsSv()。
    void sensorSettingsSvChanged();
    // 顯示單位實際更新後通知，讓 UI 重新換算數值與單位標籤。
    void pressureUnitSvChanged();
    void m1ValueSvChanged(double value);
    void m2ValueSvChanged(double value);
    void m3ValueSvChanged(double value);
    void m4ValueSvChanged(double value);
    void m1ValuePvChanged(double value);
    void m2ValuePvChanged(double value);
    void m3ValuePvChanged(double value);
    void m4ValuePvChanged(double value);
    void pump2HzSvChanged(double value);
    void pump2HzPvChanged(double value);
    void motorRunningSvChanged(bool value);
    void motorRunningPvChanged(bool value);
    void wayValveOpenSvChanged(bool value);
    void wayValveOpenPvChanged(bool value);
    void inverterResetSvChanged(bool value);
    void emergencyStopSvChanged(bool value);

    void tt01ValuePvChanged(double value);
    void tt02ValuePvChanged(double value);
    void tt03ValuePvChanged(double value);
    void tt04ValuePvChanged(double value);
    void pt01ValuePvChanged(double value);
    void pt02ValuePvChanged(double value);
    void pt03ValuePvChanged(double value);
    void pt04ValuePvChanged(double value);
    void pt05ValuePvChanged(double value);
    void pt06ValuePvChanged(double value);
    void pt07ValuePvChanged(double value);
    void flowMeterValuePvChanged(double value);
    void leakDetectedPvChanged(bool value);
    void historyTitleChanged(const QVariantList &title);
    void alarmRecordsChanged(const QVariantList &records);
    void historyViewsChanged(const QVariantMap &views);
    void historyExportStatusChanged(const QVariantMap &status);
    void clientSessionIdChanged();
    void pageHostChanged();
    void transportReadyChanged();
    void transportMessageChanged();

    // Request (not a property NOTIFY; UI -> Core, WASM relayed to the Desktop by the mirror):
    // the only history view request (spec §2.1). HistoryPage.qml sends its own sessionId
    // (= clientSessionId) with its own range and page: when the page is shown (current range
    // and page; the first time the current month, page 1), on "篩選" / "顯示前一周" (page 1),
    // on previous / next page (page -/+ 1), and when the WASM transport comes back while the
    // page is shown. fromMs/toMs are local-epoch ms, both ends inclusive, and are the
    // client's range: the Core pages exactly this range (it keeps no range of its own that
    // the request would depend on) and writes the result to historyViews[sessionId]. The
    // Core still accepts 0 .. 8640000000000000 as "unbounded", although the UI no longer
    // sends it. One sessionId's requests never make another sessionId's request stale.
    void historyViewRequested(QString sessionId, double fromMs, double toMs, int page);
    // Request (UI -> Core, WASM relayed): "下載 CSV" asks the Core to export the raw data of the
    // current range for this client (sessionId = clientSessionId); progress comes back through
    // historyExportStatus[sessionId] (spec §3.1).
    void historyExportRequested(QString sessionId, double fromMs, double toMs);
    // Request (UI -> Core, WASM relayed): cancel this client's queued or running export (spec §3.1).
    void historyExportCancelRequested(QString sessionId);

    // Per-client alarm views (w1-078). NOTIFY of alarmViews.
    void alarmViewsChanged(const QVariantMap &views);
    // Request (not a property NOTIFY; UI -> Core, WASM relayed to the Desktop by the mirror):
    // the only alarm range request. AlarmPage.qml sends its own sessionId (= clientSessionId)
    // with its own range and page: when the page is shown and when the WASM transport comes back
    // while it is shown (current range and page), on "篩選" / "顯示前一周" (page 1), on previous /
    // next page (page -/+ 1), and - while the range is still the default "最近 24 小時" - with a
    // fresh last-24-hours range, page 1, whenever alarmRecords changes. fromMs/toMs are
    // local-epoch ms, both ends inclusive (start minute :00.000 .. end minute :59.999), may span
    // months, and are the client's range: the Core pages exactly this range (newest first,
    // pageSize 9) and writes the result to alarmViews[sessionId]; a page beyond the last one is
    // clamped. One sessionId's requests never make another sessionId's request stale.
    void alarmViewRequested(QString sessionId, double fromMs, double toMs, int page);

private:
    // 預設無校正、上下限皆停用；lower / upper 的 0 只是初始值，不代表已啟用限制。
    static QVariantMap defaultSensorSettings()
    {
        QVariantMap settings;
        for (const auto &id : {"pt01", "pt02", "pt03", "pt04", "pt05", "pt06", "pt07",
                              "tt01", "tt02", "tt03", "tt04", "flowMeter", "filter"}) {
            settings.insert(QString::fromLatin1(id), QVariantMap{
                {"offset", 0.0}, {"lower", 0.0}, {"upper", 0.0},
                {"lowerEnabled", false}, {"upperEnabled", false}});
        }
        return settings;
    }
    QVariantMap m_sensorSettingsSv = defaultSensorSettings();
    QString m_pressureUnitSv = QStringLiteral("kPa");

    using ValueSignal = void (TaidaFlowProxy::*)(double);

    void setWritableValue(double &target, double value, ValueSignal signal)
    {
        if (qFuzzyCompare(target + 1.0, value + 1.0)) {
            return;
        }
        target = value;
        emit (this->*signal)(value);
    }

    void setProcessValue(double &target, double value, ValueSignal signal)
    {
        if (qFuzzyCompare(target + 1.0, value + 1.0)) {
            return;
        }
        target = value;
        emit (this->*signal)(value);
    }

    using BooleanSignal = void (TaidaFlowProxy::*)(bool);

    void setBooleanValue(bool &target, bool value, BooleanSignal signal)
    {
        if (target == value) {
            return;
        }
        target = value;
        emit (this->*signal)(value);
    }

    void initializeListData()
    {
        // Column order is shared by the table and CSV export. Each row has 20 cells.
        const QStringList titles{
            QStringLiteral("時間"), QStringLiteral("設備"),
            QStringLiteral("TT-01 (°C)"), QStringLiteral("TT-02 (°C)"),
            QStringLiteral("TT-03 (°C)"), QStringLiteral("TT-04 (°C)"),
            QStringLiteral("PT-01 (kPa)"), QStringLiteral("PT-02 (kPa)"),
            QStringLiteral("PT-03 (kPa)"), QStringLiteral("PT-04 (kPa)"),
            QStringLiteral("PT-05 (kPa)"), QStringLiteral("PT-06 (kPa)"),
            QStringLiteral("PT-07 (kPa)"), QStringLiteral("FM-01 (L/min)"),
            QStringLiteral("M1 (%)"), QStringLiteral("M2 (%)"),
            QStringLiteral("M3 (%)"), QStringLiteral("M4 (%)"),
            QStringLiteral("泵浦頻率 (Hz)"), QStringLiteral("漏水 (ON/OFF)")};
        for (const auto &title : titles)
            m_historyTitle.append(title);
        static const QStringList alarmDevices{
            QStringLiteral("循環泵浦 A"), QStringLiteral("主水槽"),
            QStringLiteral("過濾器"), QStringLiteral("測試設備"),
            QStringLiteral("加熱器"), QStringLiteral("循環泵浦 B")};
        static const QStringList alarmSensors{
            QStringLiteral("M1"), QStringLiteral("LS-01"),
            QStringLiteral("PT-03"), QStringLiteral("TT-04"),
            QStringLiteral("TT-03"), QStringLiteral("FM-01")};
        static const QStringList alarmMessages{
            QStringLiteral("馬達回授訊號異常"), QStringLiteral("偵測到漏水訊號"),
            QStringLiteral("壓力超過安全範圍"), QStringLiteral("出口溫度過高"),
            QStringLiteral("加熱溫度偏高"), QStringLiteral("流量低於設定值")};

        // No history rows here: history views are per client and only exist after the
        // client's historyViewRequested reaches the Core (spec §2.1), so main alone (no
        // backend) shows an empty history table.
        const QDateTime now = QDateTime::currentDateTime();
        for (int i = 0; i < 14; ++i) {
            const QDateTime alarmTime = now.addSecs(-i * 37 * 60);
            const bool active = i == 0 || i == 3 || i == 8;
            m_alarmRecords.append(QVariantMap{
                {QStringLiteral("timestampMs"), alarmTime.toMSecsSinceEpoch()},
                {QStringLiteral("alarmTime"), alarmTime.toString(QStringLiteral("yyyy/MM/dd HH:mm"))},
                {QStringLiteral("equipment"), alarmDevices.at(i % alarmDevices.size())},
                {QStringLiteral("sensorName"), alarmSensors.at(i % alarmSensors.size())},
                {QStringLiteral("alarmMessage"), alarmMessages.at(i % alarmMessages.size())},
                {QStringLiteral("severity"), i % 4 == 0 ? QStringLiteral("嚴重") : QStringLiteral("警告")},
                {QStringLiteral("alarmStatus"), active ? QStringLiteral("未處理") : QStringLiteral("已解除")}});
        }
    }

    // void updateSimulatedProcess()
    // {
    //     if (m_temperatureIncreasing) {
    //         m_testTemperature += 1.0;
    //         if (m_testTemperature >= 50.0) {
    //             m_testTemperature = 50.0;
    //             m_temperatureIncreasing = false;
    //         }
    //     } else {
    //         m_testTemperature -= 1.0;
    //         if (m_testTemperature <= 20.0) {
    //             m_testTemperature = 20.0;
    //             m_temperatureIncreasing = true;
    //         }
    //     }
    //
    //     // setProcessValue(m_tt01ValuePv, m_testTemperature, &TaidaFlowProxy::tt01ValuePvChanged);
    //     // setProcessValue(m_tt02ValuePv, m_testTemperature + 3.0, &TaidaFlowProxy::tt02ValuePvChanged);
    //     // setProcessValue(m_tt03ValuePv, m_testTemperature - 2.0, &TaidaFlowProxy::tt03ValuePvChanged);
    //     // setProcessValue(m_tt04ValuePv, m_testTemperature + 5.0, &TaidaFlowProxy::tt04ValuePvChanged);
    //     // setProcessValue(m_flowMeterValuePv,
    //     //                 m_testTemperature <= 23.0 ? 0.0 : 20.0,
    //     //                 &TaidaFlowProxy::flowMeterValuePvChanged);
    // }

    double m_m1ValueSv = 0.0;
    double m_m2ValueSv = 0.0;
    double m_m3ValueSv = 0.0;
    double m_m4ValueSv = 0.0;
    double m_m1ValuePv = 0.0;
    double m_m2ValuePv = 0.0;
    double m_m3ValuePv = 0.0;
    double m_m4ValuePv = 0.0;
    double m_pump2HzSv = 0.0;
    double m_pump2HzPv = 0.0;
    bool m_motorRunningSv = false;
    bool m_motorRunningPv = false;
    bool m_wayValveOpenSv = false;
    bool m_wayValveOpenPv = false;
    bool m_inverterResetSv = false;
    bool m_emergencyStopSv = false;

    double m_tt01ValuePv = 0.0;
    double m_tt02ValuePv = 0.0;
    double m_tt03ValuePv = 0.0;
    double m_tt04ValuePv = 0.0;
    double m_pt01ValuePv = 0.0;
    double m_pt02ValuePv = 9.0;
    double m_pt03ValuePv = 1.0;
    double m_pt04ValuePv = 0.0;
    double m_pt05ValuePv = 0.0;
    double m_pt06ValuePv = 0.0;
    double m_pt07ValuePv = 0.0;
    double m_flowMeterValuePv = 0.0;
    bool m_leakDetectedPv = false;
    QVariantList m_historyTitle;
    QVariantList m_alarmRecords;
    // Per-client history views keyed by clientSessionId (spec §2.1); only the Core writes it.
    QVariantMap m_historyViews;
    QVariantMap m_historyExportStatus;
    // Per-client alarm views keyed by clientSessionId (w1-078); only the Core writes it.
    QVariantMap m_alarmViews;
    // Desktop default (spec §1); only the WASM composition root replaces it with "web-xxxx".
    QString m_clientSessionId = QStringLiteral("desktop");
    // Only the WASM composition root sets it (location.hostname); desktop stays empty.
    QString m_pageHost;
    // Desktop has no remote transport to wait for, so the overlay default is
    // "ready"; see main.cpp for the WASM-only initial false state.
    bool m_transportReady = true;
    QString m_transportMessage;

    double m_testTemperature = 0.0;
    bool m_temperatureIncreasing = false;
    QTimer m_processTimer;

};
