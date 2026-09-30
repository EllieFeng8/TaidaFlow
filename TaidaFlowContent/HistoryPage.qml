import QtQuick
import QtQuick.Controls
import TaidaFlowBackend 1.0
import "components" as Components
import "components/DateTimeUtil.js" as DateTimeUtil
import "components/HistoryViewUtil.js" as HistoryViewUtil
import "components/SensorUnits.js" as SensorUnits

// =========================================================
// 歷史紀錄頁面
// =========================================================
Item {
    id: historyPage

    // w1-083: controls are enabled only while the link to the Core is alive; TopNav.qml binds
    // it to root.linkAlive (Td.transportReady and, on the web page, no heartbeat silence).
    // Default true only when this page is loaded without TopNav.
    property bool linkAlive: true

    // Below the offline banner (TopNav.qml): the banner pushes this page down
    // while shown; hidden it has height 0, i.e. this equals topNavBar.bottom.
    anchors.top: offlineBanner.bottom
    anchors.left: parent.left
    anchors.right: parent.right
    anchors.bottom: parent.bottom

    visible: root.currentPage === 2

    property color mutedTextColor: "#8FAFC8"
    property color dividerColor: "#284766"
    property color successColor: "#22C55E"
    property color dangerColor: "#FF5964"
    property string filterMessage: ""
    // Scale of TopNav (root): App.qml scales it by webScale on the web, 1 on the
    // desktop. The date/time pickers are popups in the (unscaled) window overlay
    // and apply this scale themselves (DateTimeField.popupScale). `parent` is
    // root: this page anchors to its sibling offlineBanner, which is only
    // possible as a direct child of TopNav's root.
    readonly property real designScale: parent ? parent.scale : 1
    // Room for 16 characters "YYYY/MM/DD HH:mm" (Consolas 16 px) plus the
    // calendar button inside the field.
    readonly property int dateFieldWidth: 280
    property string exportMessage: ""

    // Range convention shared with the Core (Core/TaidaFlowProxy.h): epoch ms,
    // both ends inclusive; 0 .. 8640000000000000 (largest JS Date value, finite
    // so the mirror accepts it) = unbounded (all months). This page no longer
    // sends it (the former show-all button is now "顯示前一周", spec §2 revised
    // 2026-09-25) and each client now has its own range (spec §2.1), but the Core
    // still accepts it, so the page still shows it as "全部" if it ever comes back.
    readonly property double unboundedFromMs: 0
    readonly property double unboundedToMs: 8640000000000000

    // ---- This client's history view (spec §2.1, revised 2026-09-27) -------------
    // The Core writes Td.historyViews (keyed by clientSessionId); this page reads
    // only its own entry and copies it into the view* properties below when that
    // entry's revision changes (applyOwnView). Entries of other clients changing
    // re-send the whole map but leave everything here untouched, so another
    // client's filter / paging never changes this screen.
    // true once our own entry has been shown; false = not loaded yet (the table
    // shows "載入中" / the pager shows "—").
    property bool viewLoaded: false
    // Revision of the entry currently shown; null = none (not loaded, or the Core
    // removed our entry - then any rebuilt entry is shown, whatever its revision).
    property var appliedRevision: null
    property double viewFromMs: 0
    property double viewToMs: 0
    property int viewPage: 1
    property int viewTotalPages: 1
    property double viewTotalRows: 0
    // Our current range / page as last requested (or last shown): sent again when
    // the page is shown. rangeChosen = false until the first time the page is
    // shown, which picks the current month (HistoryViewUtil.monthRange).
    property bool rangeChosen: false
    property double requestFromMs: 0
    property double requestToMs: 0
    property int requestPage: 1
    // Range shown in the status text and used by the export: our shown view once
    // loaded, else the range we asked for.
    readonly property double shownFromMs: viewLoaded ? viewFromMs : requestFromMs
    readonly property double shownToMs: viewLoaded ? viewToMs : requestToMs

    // This client's export job (spec §3.2): the entry of historyExportStatus
    // keyed by our own clientSessionId, or null when there is none.
    readonly property var myExport: {
        var all = Td.historyExportStatus
        var entry = all ? all[Td.clientSessionId] : undefined
        return entry ? entry : null
    }
    readonly property string exportState: myExport && myExport.state ? String(myExport.state) : ""
    readonly property bool exportActive: exportState === "queued" || exportState === "running"
    readonly property real exportProgressFraction: exportState === "done" ? 1
        : exportState === "running" ? Math.max(0, Math.min(100, Number(myExport.progress) || 0)) / 100
        : 0
    // Web download link of a finished export (spec §3.5); desktop entries use savedPath.
    readonly property string exportUrl: exportState === "done" ? exportDownloadUrl(myExport) : ""
    // Set when this page sends an export request; only an export we asked for
    // may open a browser download (never a stale entry from the snapshot).
    property bool exportRequestedHere: false
    property string lastOpenedExportUrl: ""
    // Terminal states (done/cancelled/error) stay in the Core's map; the close
    // button hides the one currently shown until the entry changes.
    property string dismissedExportKey: ""
    readonly property string exportKey: myExport
                                        ? exportState + "|" + String(myExport.fileName || "")
                                          + "|" + String(myExport.message || "")
                                        : ""
    readonly property bool exportPanelShown: myExport !== null
                                             && (exportActive || exportKey !== dismissedExportKey)
    readonly property var columnTitles: {
        var result = []
        for (var title of Td.historyTitle)
            result.push(/^PT-\d+\s*\((kPa|psi|bar|Pa)\)$/.test(title)
                        ? title.replace(/\([^)]*\)/, "(" + Td.pressureUnitSv + ")") : title)
        return result
    }
    // The Core already pages our range (spec §2.1): this is the records of our
    // own entry (10 rows), newest first, no local filtering.
    property var historySourceModel: []
    // Column widths: at least the design widths (time 210, column 1 160, values
    // 146 - wide enough for "yyyy/MM/dd HH:mm:ss" and the longest value text
    // "65535.00"), and never narrower than the column title + 24 px, measured
    // with the font actually used (desktop font / web embedded font), so no
    // title overflows into the next column whatever the Core sends.
    readonly property var columnWidths: {
        var widths = []
        for (var i = 0; i < columnTitles.length; ++i) {
            var base = i === 0 ? 210 : i === 1 ? 160 : 146
            widths.push(Math.max(base, Math.ceil(headerFontMetrics.advanceWidth(String(columnTitles[i]))) + 24))
        }
        return widths
    }
    readonly property int tableWidth: {
        var total = 64
        for (var i = 0; i < columnWidths.length; ++i)
            total += columnWidths[i]
        return total
    }

    function columnWidth(index) {
        return index < columnWidths.length ? columnWidths[index] : 146
    }
    function cellText(value) {
        return value === undefined || value === null ? "—"
             : typeof value === "number" ? value.toFixed(2) : String(value)
    }
    function historyCellText(value, index) {
        var match = /^PT-\d+\s*\((kPa|psi|bar|Pa)\)$/.exec(String(Td.historyTitle[index]))
        if (match && typeof value === "number") {
            var sourceFactor = match[1] === "Pa" ? 1000 : SensorUnits.pressureFactor(match[1])
            return (value / sourceFactor * SensorUnits.pressureFactor(Td.pressureUnitSv)).toFixed(2)
        }
        return cellText(value)
    }

    // Same font as the table header titles below (14 px bold, default family).
    FontMetrics {
        id: headerFontMetrics
        font.pixelSize: 14
        font.bold: true
    }

    // Show our own entry of Td.historyViews when its revision changed (spec
    // §2.1). Reads Td directly because it runs from a Connections handler.
    // "none" (only other clients' entries changed, or still not loaded) leaves
    // the table, pager, fields and scroll position exactly as they are. "lost"
    // (the Core removed our entry: 30 idle minutes / 32-entry limit / Core
    // restart) keeps the screen and only forgets the revision; our next request
    // (paging, filter, showing the page, transport back) rebuilds the entry.
    function applyOwnView() {
        var change = HistoryViewUtil.viewChange(Td.historyViews, Td.clientSessionId, appliedRevision)
        if (change.kind === "lost") {
            appliedRevision = null
            return
        }
        if (change.kind !== "apply")
            return

        var view = change.view
        var rangeChanged = !viewLoaded || view.fromMs !== viewFromMs || view.toMs !== viewToMs
        appliedRevision = view.revision
        viewFromMs = view.fromMs
        viewToMs = view.toMs
        viewPage = view.page
        viewTotalPages = view.totalPages
        viewTotalRows = view.totalRows
        viewLoaded = true
        // What is shown is now our current range / page.
        rangeChosen = true
        requestFromMs = view.fromMs
        requestToMs = view.toMs
        requestPage = view.page
        historySourceModel = view.rows
        historyList.positionViewAtBeginning()
        // Fields follow only a new range (not paging), so text being typed is
        // not overwritten by a page change.
        if (rangeChanged)
            setRangeFields(view.fromMs, view.toMs)
    }

    // Our range in the date/time fields as YYYY/MM/DD HH:mm, i.e. the minute of
    // each end (e.g. "顯示前一周" -> 2026/09/19 00:00 .. 2026/09/25 23:59; the
    // end 23:59:59.999 shows as 23:59). Unbounded leaves the fields empty and
    // rangeText() shows "全部" (never requested by this page any more).
    function isUnboundedRange(fromMs, toMs) {
        return fromMs <= unboundedFromMs && toMs >= unboundedToMs
    }

    function setRangeFields(fromMs, toMs) {
        if (isUnboundedRange(fromMs, toMs)) {
            startDateField.text = ""
            endDateField.text = ""
            return
        }
        startDateField.text = DateTimeUtil.formatDateTime(new Date(fromMs))
        endDateField.text = DateTimeUtil.formatDateTime(new Date(toMs))
    }

    function rangeText() {
        if (!rangeChosen)
            return "—"
        if (isUnboundedRange(shownFromMs, shownToMs))
            return "全部"
        return DateTimeUtil.formatDateTime(new Date(shownFromMs)) + " – "
                + DateTimeUtil.formatDateTime(new Date(shownToMs))
    }

    // The only history request (spec §2.1): our own session id, range and page.
    // Remembered as our current range / page even while offline (then nothing
    // is sent; the request is repeated when the transport comes back).
    function requestView(fromMs, toMs, page) {
        rangeChosen = true
        requestFromMs = fromMs
        requestToMs = toMs
        requestPage = page
        if (!Td.transportReady)
            return
        Td.historyViewRequested(Td.clientSessionId, fromMs, toMs, page)
    }

    // Page shown (or transport back while shown): ask for our current range and
    // page again. The first time, our range is the current month, page 1.
    function requestOwnView() {
        if (!rangeChosen) {
            var month = HistoryViewUtil.monthRange(new Date())
            requestView(month.fromMs, month.toMs, 1)
            setRangeFields(month.fromMs, month.toMs)
            return
        }
        requestView(requestFromMs, requestToMs, requestPage)
    }

    // "篩選": ask the Core for our range, page 1 (spec §2 revised 2026-09-25,
    // §2.1). Fields are YYYY/MM/DD HH:mm in local time, or the date only (start
    // 00:00, end 23:59). From = the start minute's :00.000, to = the end
    // minute's :59.999, both inclusive, built with new Date(y, m, d, h, mi, s, ms)
    // (components/DateTimeUtil.js).
    function applyFilter() {
        if (!Td.transportReady)
            return

        var start = DateTimeUtil.parseDateTime(startDateField.text, false)
        var end = DateTimeUtil.parseDateTime(endDateField.text, true)

        if (!start || !end) {
            filterMessage = "請輸入正確日期時間（YYYY/MM/DD HH:mm）"
            return
        }
        var fromMs = DateTimeUtil.startMs(start)
        var toMs = DateTimeUtil.endMs(end)
        if (fromMs > toMs) {
            filterMessage = "起始時間不可晚於結束時間"
            return
        }

        filterMessage = ""
        requestView(fromMs, toMs, 1)
        // Normalize the fields (a date-only entry shows its 00:00 / 23:59). Our
        // entry's new range (applyOwnView) shows the same text.
        setRangeFields(fromMs, toMs)
    }

    // Previous / next page: the shown page -/+ 1 within the shown range (the
    // range and page of our own entry, spec §2.1).
    function goToPage(page) {
        if (!Td.transportReady || !viewLoaded || page < 1 || page > viewTotalPages || page === viewPage)
            return

        requestView(viewFromMs, viewToMs, page)
    }

    // "顯示前一周" (spec §2, revised 2026-09-25): the last 7 local calendar days
    // including today, i.e. (today - 6 days) 00:00:00.000 .. today 23:59:59.999,
    // page 1 (HistoryViewUtil.lastWeekRange: new Date(y, m, d +/- n) rolls over
    // month/year ends and stays on local midnight across DST changes).
    function showLastWeek() {
        if (!Td.transportReady)
            return

        filterMessage = ""
        var range = HistoryViewUtil.lastWeekRange(new Date())
        requestView(range.fromMs, range.toMs, 1)
        // Fields: (today - 6) 00:00 .. today 23:59 right away (our entry's new
        // range shows the same text when it arrives).
        setRangeFields(range.fromMs, range.toMs)
    }

    function formatCount(value) {
        var text = String(Math.max(0, Math.round(Number(value) || 0)))
        return text.replace(/\B(?=(\d{3})+(?!\d))/g, ",")
    }

    // "下載 CSV": the Core exports the raw data of our own current range (spec
    // §3.1, §2.1: the range shown in the status text); progress comes back
    // through historyExportStatus.
    function downloadCsv() {
        if (!Td.transportReady || exportActive || !rangeChosen)
            return

        exportRequestedHere = true
        dismissedExportKey = ""
        Td.historyExportRequested(Td.clientSessionId, shownFromMs, shownToMs)
        exportMessage = "已送出匯出要求（區間：" + rangeText() + "）"
        exportMessageTimer.restart()
    }

    function cancelExport() {
        if (!Td.transportReady || !exportActive)
            return

        Td.historyExportCancelRequested(Td.clientSessionId)
    }

    // Full download link of an export entry (spec §3.5, revised 2026-09-24). The Core does
    // not know which host name the page used, so it sends url = "/exports/<file>" plus
    // downloadPort; the page completes it with its own host (Td.pageHost, location.hostname
    // set by the WASM main.cpp): "http://" + pageHost + ":" + downloadPort + url.
    // Default port 8124 when downloadPort is missing. An absolute "http..." url is used as is.
    function exportDownloadUrl(entry) {
        var url = entry && entry.url ? String(entry.url) : ""
        if (url.length === 0)
            return ""
        if (url.charAt(0) === "/") {
            var port = Number(entry.downloadPort) > 0 ? Number(entry.downloadPort) : 8124
            return "http://" + Td.pageHost + ":" + port + url
        }
        return url
    }

    function exportStatusText() {
        if (!myExport)
            return ""
        switch (exportState) {
        case "queued":
            // queuePosition: 1 = next to run (0 only while running, spec §3.2).
            return "排隊中 · 第 " + Number(myExport.queuePosition || 0) + " 位"
        case "running":
            return "匯出中 " + Math.round(Number(myExport.progress || 0)) + "% · "
                    + formatCount(myExport.rowsWritten) + " / "
                    + formatCount(myExport.totalRows) + " 筆"
        case "done":
            if (myExport.savedPath)
                return "匯出完成 · " + formatCount(myExport.rowsWritten) + " 筆 · 已儲存：" + myExport.savedPath
            return "匯出完成 · " + formatCount(myExport.rowsWritten) + " 筆 · " + String(myExport.fileName || "")
        case "cancelled":
            return "匯出已取消"
        case "error":
            return "匯出失敗：" + String(myExport.message || "未知錯誤")
        default:
            return String(myExport.message || exportState)
        }
    }

    // Web: an export we requested finished with a download link -> hand it to
    // the browser (spec §3.5). Desktop entries carry savedPath instead of url.
    // Reads Td directly: runs from a Connections handler, where the derived
    // myExport/exportState bindings may not be re-evaluated yet.
    function handleExportStatus() {
        var all = Td.historyExportStatus
        var entry = all ? all[Td.clientSessionId] : undefined
        if (!entry || !exportRequestedHere)
            return
        var state = String(entry.state || "")
        if (state === "done") {
            var url = exportDownloadUrl(entry)
            exportRequestedHere = false
            if (url.length > 0 && url !== lastOpenedExportUrl) {
                lastOpenedExportUrl = url
                Qt.openUrlExternally(url)
                exportMessage = "已開始下載 " + String(entry.fileName || "")
                exportMessageTimer.restart()
            }
        } else if (state === "cancelled" || state === "error") {
            exportRequestedHere = false
        }
    }

    Component.onCompleted: {
        applyOwnView()
        if (visible)
            requestOwnView()
    }

    // TopNav.qml keeps this page instantiated and switches pages by binding
    // `visible` to root.currentPage, so the page becoming visible is the
    // moment the history page is shown. Ask the Core for our own view.
    onVisibleChanged: {
        if (visible)
            requestOwnView()
    }

    Connections {
        target: Td
        function onHistoryViewsChanged() {
            historyPage.applyOwnView()
        }
        function onHistoryExportStatusChanged() {
            handleExportStatus()
        }
        // WASM: the mirror (re)connected while the page is shown - a request
        // made while offline was not sent, and after a desktop restart the Core
        // has no entry for us. The desktop's transportReady never changes.
        function onTransportReadyChanged() {
            if (Td.transportReady && historyPage.visible)
                historyPage.requestOwnView()
        }
    }

    Timer {
        id: exportMessageTimer
        interval: 3200
        onTriggered: exportMessage = ""
    }

    Column {
        anchors.fill: parent
        anchors.leftMargin: 48
        anchors.rightMargin: 48
        anchors.topMargin: 34
        anchors.bottomMargin: 36
        spacing: 22

        // Height follows the content (at least 54): the export panel grows when
        // its status text wraps (e.g. a long saved path), nothing is cut off.
        Row {
            id: headerRow
            width: parent.width
            spacing: 16

            Column {
                // Positioners skip invisible children, so the export panel only
                // takes space (and a spacing gap) while it is shown.
                width: parent.width - downloadButton.width - parent.spacing
                       - (exportPanel.visible ? exportPanel.width + parent.spacing : 0)
                spacing: 4

                Text {
                    text: "歷史資料"
                    color: root.textColor
                    font.pixelSize: 28
                    font.bold: true
                }

                // Needs <= 354 px (measured); the column is 726 px wide even with the export
                // panel shown (1824 - 166 - 900 - 2 x 16). Wraps instead of eliding.
                Text {
                    width: parent.width
                    text: "感測器與設備紀錄 · " + historyPage.columnTitles.length + " 個欄位 · 左右捲動查看完整資料"
                    color: historyPage.mutedTextColor
                    font.pixelSize: 14
                    wrapMode: Text.Wrap
                }
            }

            // Export job of this client (historyExportStatus[clientSessionId],
            // spec §3.2): status line, progress bar and cancel / close actions.
            // 900 px at the 1920 design width (was 480): the status text has >= 744 px
            // and wraps (panel grows) instead of eliding a long file name / path.
            Rectangle {
                id: exportPanel
                width: Math.min(900, parent.width * 0.5)
                height: Math.max(54, exportStatusLabel.implicitHeight + 32)
                visible: historyPage.exportPanelShown
                radius: 7
                color: "#111D32"
                border.color: historyPage.exportState === "error" ? dangerColor : dividerColor
                border.width: 1

                Text {
                    id: exportStatusLabel
                    anchors.left: parent.left
                    anchors.leftMargin: 14
                    anchors.right: exportActions.left
                    anchors.rightMargin: 10
                    anchors.top: parent.top
                    anchors.topMargin: 9
                    text: historyPage.exportStatusText()
                    color: historyPage.exportState === "error" ? dangerColor
                         : historyPage.exportState === "done" ? successColor
                         : root.textColor
                    font.pixelSize: 13
                    // Full text always: wraps (word boundary, or anywhere inside a
                    // long path) and the panel height follows implicitHeight.
                    wrapMode: Text.Wrap
                }

                Rectangle {
                    id: exportProgressTrack
                    anchors.left: parent.left
                    anchors.leftMargin: 14
                    anchors.right: exportActions.left
                    anchors.rightMargin: 10
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 11
                    height: 6
                    radius: 3
                    color: "#0B1527"

                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        radius: 3
                        width: parent.width * historyPage.exportProgressFraction
                        color: historyPage.exportState === "done" ? successColor : root.mainBlue
                    }
                }

                Row {
                    id: exportActions
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6

                    // Web fallback: browsers may block a download opened outside a
                    // click, so a finished web export also offers the link as a button.
                    Button {
                        id: openExportButton
                        width: 84
                        height: 32
                        visible: historyPage.exportUrl.length > 0
                        hoverEnabled: true

                        background: Rectangle {
                            radius: 5
                            color: openExportButton.hovered ? "#16A34A" : successColor
                        }

                        contentItem: Text {
                            text: "下載檔案"
                            color: "white"
                            font.pixelSize: 13
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        onClicked: Qt.openUrlExternally(historyPage.exportUrl)
                    }

                    Button {
                        id: cancelExportButton
                        width: 64
                        height: 32
                        visible: historyPage.exportActive
                        // Cancel is a request relayed to the Core, so it needs the transport.
                        enabled: historyPage.linkAlive
                        hoverEnabled: true

                        background: Rectangle {
                            radius: 5
                            color: !cancelExportButton.enabled ? "#16243A"
                                 : cancelExportButton.hovered ? "#223D5A" : "transparent"
                            border.color: cancelExportButton.enabled ? dangerColor : "transparent"
                            border.width: 1
                        }

                        contentItem: Text {
                            text: "取消"
                            color: cancelExportButton.enabled ? dangerColor : "#536A80"
                            font.pixelSize: 13
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        onClicked: historyPage.cancelExport()
                    }

                    Button {
                        id: closeExportButton
                        width: 32
                        height: 32
                        visible: !historyPage.exportActive
                        hoverEnabled: true

                        background: Rectangle {
                            radius: 5
                            color: closeExportButton.hovered ? "#223D5A" : "transparent"
                        }

                        contentItem: Text {
                            text: "×"
                            color: mutedTextColor
                            font.pixelSize: 14
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        onClicked: historyPage.dismissedExportKey = historyPage.exportKey
                    }
                }
            }

            Button {
                id: downloadButton
                width: 166
                height: 48
                // Export is a request to the Core: needs the transport, and only one
                // export per client at a time (spec §3.3).
                enabled: historyPage.linkAlive && !historyPage.exportActive
                hoverEnabled: true

                background: Rectangle {
                    radius: 7
                    color: !downloadButton.enabled ? "#16243A"
                         : downloadButton.hovered ? "#16A34A" : successColor
                    border.color: downloadButton.enabled && downloadButton.hovered ? "#86EFAC" : "transparent"
                    border.width: 1
                }

                contentItem: Text {
                    text: "↓  下載 CSV"
                    color: downloadButton.enabled ? "white" : "#536A80"
                    font.pixelSize: 16
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                onClicked: downloadCsv()
            }
        }

        // Range filter, two rows: the format hint, then the date/time fields and
        // buttons with the current range (or the input error) to their right.
        // Height follows the content, so wrapped text is never cut off.
        Rectangle {
            id: filterPanel
            width: parent.width
            height: filterContent.height + 26
            radius: 10
            color: "#111D32"
            border.color: historyPage.dividerColor
            border.width: 1

            Column {
                id: filterContent
                anchors.left: parent.left
                anchors.leftMargin: 24
                anchors.right: parent.right
                anchors.rightMargin: 24
                anchors.top: parent.top
                anchors.topMargin: 12
                spacing: 10

                // One line at the 1920 design width (1776 px available); wraps
                // instead of eliding if it ever gets longer.
                Text {
                    width: parent.width
                    text: "日期時間格式：YYYY/MM/DD HH:mm（例如：2026/09/24 08:30），可直接輸入或按欄位右側的日曆圖示選擇；"
                          + "只輸入日期時，起始為 00:00、結束為 23:59；結束時間包含該分鐘；"
                          + "篩選可跨月，「顯示前一周」為今天往前 7 天（含今天，00:00 至 23:59）"
                    color: historyPage.mutedTextColor
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }

                Item {
                    width: parent.width
                    height: filterRow.height

                    Row {
                        id: filterRow
                        spacing: 14

                        Column {
                            spacing: 8

                            Text {
                                text: "起始日期時間"
                                color: historyPage.mutedTextColor
                                font.pixelSize: 13
                            }

                            // Type YYYY/MM/DD HH:mm (or the date only -> 00:00) or pick it
                            // with the calendar button; Enter runs the filter, picking does not.
                            // Like the former TextField it stays enabled while offline
                            // (applyFilter() itself needs the transport).
                            Components.DateTimeField {
                                id: startDateField
                                width: historyPage.dateFieldWidth
                                height: 46
                                isEnd: false
                                popupScale: historyPage.designScale
                                borderColor: historyPage.dividerColor
                                mutedTextColor: historyPage.mutedTextColor
                                onAccepted: historyPage.applyFilter()
                            }
                        }

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.verticalCenterOffset: 14
                            text: "—"
                            color: historyPage.mutedTextColor
                            font.pixelSize: 20
                        }

                        Column {
                            spacing: 8

                            Text {
                                text: "結束日期時間"
                                color: historyPage.mutedTextColor
                                font.pixelSize: 13
                            }

                            // Date only -> 23:59 (the whole end day, as before).
                            Components.DateTimeField {
                                id: endDateField
                                width: historyPage.dateFieldWidth
                                height: 46
                                isEnd: true
                                popupScale: historyPage.designScale
                                borderColor: historyPage.dividerColor
                                mutedTextColor: historyPage.mutedTextColor
                                onAccepted: historyPage.applyFilter()
                            }
                        }

                        Button {
                            id: filterButton
                            width: 110
                            height: 46
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.verticalCenterOffset: 14
                            // The range query is a request to the Core: disabled while offline.
                            enabled: historyPage.linkAlive
                            hoverEnabled: true

                            background: Rectangle {
                                radius: 6
                                color: !filterButton.enabled ? "#16243A"
                                     : filterButton.hovered ? root.lightBlue : root.mainBlue
                            }

                            contentItem: Text {
                                text: "篩選"
                                color: filterButton.enabled ? "white" : "#536A80"
                                font.pixelSize: 16
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }

                            onClicked: historyPage.applyFilter()
                        }

                        Button {
                            id: lastWeekButton
                            width: 110
                            height: 46
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.verticalCenterOffset: 14
                            enabled: historyPage.linkAlive
                            hoverEnabled: true

                            background: Rectangle {
                                radius: 6
                                color: !lastWeekButton.enabled ? "#16243A"
                                     : lastWeekButton.hovered ? "#223D5A" : "transparent"
                                border.color: lastWeekButton.enabled ? historyPage.dividerColor : "transparent"
                                border.width: 1
                            }

                            contentItem: Text {
                                text: "顯示前一周"
                                color: lastWeekButton.enabled ? root.textColor : "#536A80"
                                font.pixelSize: 15
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }

                            onClicked: historyPage.showLastWeek()
                        }
                    }

                    // Current range (or the input error), level with the fields, in the
                    // ~890 px right of the buttons; wraps rather than overlapping them.
                    Text {
                        anchors.left: filterRow.right
                        anchors.leftMargin: 24
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 46
                        text: historyPage.filterMessage.length > 0
                              ? historyPage.filterMessage
                              : "區間：" + historyPage.rangeText()
                                + (historyPage.viewLoaded
                                   ? " · 共 " + historyPage.formatCount(historyPage.viewTotalRows) + " 筆 · 本頁 "
                                     + historyPage.historySourceModel.length + " 筆"
                                   : " · 載入中")
                        color: historyPage.filterMessage.length > 0 ? historyPage.dangerColor : historyPage.mutedTextColor
                        font.pixelSize: 14
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                        wrapMode: Text.Wrap
                    }
                }
            }
        }

        Rectangle {
            width: parent.width
            height: parent.height - headerRow.height - filterPanel.height - 2 * parent.spacing
            radius: 10
            color: "#111D32"
            border.color: dividerColor
            border.width: 1
            clip: true

            Flickable {
                id: horizontalTable
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: paginationBar.top
                anchors.bottomMargin: 18
                contentWidth: Math.max(width, historyPage.tableWidth)
                contentHeight: height
                flickableDirection: Flickable.HorizontalFlick
                boundsBehavior: Flickable.StopAtBounds
                clip: true

                ScrollBar.horizontal: ScrollBar {
                    id: xbar
                    parent: horizontalTable.parent
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: paginationBar.top
                    height: 18
                    policy: ScrollBar.AlwaysOn
                }

                Rectangle {
                    id: tableHeader
                    width: horizontalTable.contentWidth
                    height: 56
                    color: "#172941"
                    Row {
                        Text {
                            width: 64; height: 56
                            text: "序號"
                            color: historyPage.mutedTextColor
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        Repeater {
                            model: historyPage.columnTitles
                            delegate: Text {
                                required property int index
                                required property var modelData
                                width: historyPage.columnWidth(index); height: 56
                                text: modelData
                                color: "#BFD8EC"
                                font.pixelSize: 14
                                font.bold: true
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                }

                ListView {
                    id: historyList
                    y: tableHeader.height
                    width: horizontalTable.contentWidth
                    height: horizontalTable.height - tableHeader.height
                    model: historyPage.historySourceModel
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {
                        x: horizontalTable.contentX + horizontalTable.width - width
                        policy: ScrollBar.AsNeeded
                    }
                    delegate: Rectangle {
                        id: recordRow
                        required property int index
                        required property var modelData
                        width: historyList.width
                        height: 54
                        color: index % 2 === 0 ? "#101C30" : "#132139"
                        Row {
                            Text {
                                width: 64; height: 54
                                text: recordRow.index + 1
                                color: historyPage.mutedTextColor
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            Repeater {
                                model: historyPage.columnTitles
                                delegate: Item {
                                    required property int index
                                    width: historyPage.columnWidth(index); height: 54
                                    Text {
                                        anchors.fill: parent
                                        anchors.rightMargin: 12
                                        text: historyPage.historyCellText(recordRow.modelData.values[index], index)
                                        color: text === "ON" ? historyPage.dangerColor
                                             : text === "OFF" ? historyPage.successColor
                                             : index < 2 ? root.textColor : "#A7D9F5"
                                        font.pixelSize: 14
                                        font.family: index === 1 ? Application.font.family : "Consolas"
                                        verticalAlignment: Text.AlignVCenter
                                        // Safety net only: every cell text fits its column
                                        // (time "yyyy/MM/dd HH:mm:ss" in >= 198 px, values at
                                        // most "65535.00" in >= 134 px, see columnWidths).
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                        }
                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width; height: 1
                            color: "#203B57"
                        }
                    }
                }
            }

            Text {
                anchors.centerIn: horizontalTable
                visible: historySourceModel.length === 0 && filterMessage.length === 0
                // Our entry not in historyViews yet (spec §2.1): not loaded, not "no data".
                text: historyPage.viewLoaded ? "此日期區間沒有歷史資料" : "載入中"
                color: mutedTextColor
                font.pixelSize: 18
            }

            Rectangle {
                id: paginationBar
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 54
                color: "#101C30"

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 1
                    color: dividerColor
                }

                Row {
                    anchors.centerIn: parent
                    spacing: 16

                    Button {
                        id: previousPageButton
                        width: 96
                        height: 34
                        // Paging is a request to the Core (historyViewRequested, spec
                        // §2.1), so it is disabled while the WASM transport is offline.
                        enabled: historyPage.viewLoaded && historyPage.viewPage > 1 && historyPage.linkAlive
                        hoverEnabled: true

                        background: Rectangle {
                            radius: 5
                            color: !previousPageButton.enabled
                                   ? "#16243A"
                                   : previousPageButton.hovered ? "#284766" : "#1B304A"
                            border.color: previousPageButton.enabled ? dividerColor : "transparent"
                            border.width: 1
                        }

                        contentItem: Text {
                            text: "‹  上一頁"
                            color: previousPageButton.enabled ? root.textColor : "#536A80"
                            font.pixelSize: 14
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        onClicked: historyPage.goToPage(historyPage.viewPage - 1)
                    }

                    // At least the former 110 px, wider when the page numbers need it
                    // (e.g. "第 123456 / 123456 頁"), so the text is never cut off.
                    Text {
                        width: Math.max(110, implicitWidth)
                        anchors.verticalCenter: parent.verticalCenter
                        text: historyPage.viewLoaded
                              ? "第 " + historyPage.viewPage + " / " + historyPage.viewTotalPages + " 頁"
                              : "第 — / — 頁"
                        color: "#BFD8EC"
                        font.pixelSize: 14
                        horizontalAlignment: Text.AlignHCenter
                    }

                    Button {
                        id: nextPageButton
                        width: 96
                        height: 34
                        enabled: historyPage.viewLoaded && historyPage.viewPage < historyPage.viewTotalPages && historyPage.linkAlive
                        hoverEnabled: true

                        background: Rectangle {
                            radius: 5
                            color: !nextPageButton.enabled
                                   ? "#16243A"
                                   : nextPageButton.hovered ? "#284766" : "#1B304A"
                            border.color: nextPageButton.enabled ? dividerColor : "transparent"
                            border.width: 1
                        }

                        contentItem: Text {
                            text: "下一頁  ›"
                            color: nextPageButton.enabled ? root.textColor : "#536A80"
                            font.pixelSize: 14
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        onClicked: historyPage.goToPage(historyPage.viewPage + 1)
                    }
                }
            }
        }
    }

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 28
        z: 20
        visible: exportMessage.length > 0
        width: exportMessageText.implicitWidth + 44
        height: 42
        radius: 21
        color: "#203B57"
        border.color: dividerColor
        border.width: 1

        Text {
            id: exportMessageText
            anchors.centerIn: parent
            text: exportMessage
            color: "white"
            font.pixelSize: 14
        }
    }
}
