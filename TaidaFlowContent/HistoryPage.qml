import QtQuick
import QtQuick.Controls
import TaidaFlowBackend 1.0

// =========================================================
// 歷史紀錄頁面
// =========================================================
Item {
    id: historyPage

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
    property string exportMessage: ""

    // Range convention shared with the Core (Core/TaidaFlowProxy.h): epoch ms,
    // both ends inclusive; 0 .. 8640000000000000 (largest JS Date value, finite
    // so the mirror accepts it) = unbounded, i.e. "顯示全部" (all months).
    readonly property double unboundedFromMs: 0
    readonly property double unboundedToMs: 8640000000000000

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
    readonly property int currentPage: Td.historyCurrentPage
    readonly property int totalPages: Td.historyTotalPages

    readonly property var columnTitles: Td.historyTitle
    // The Core already pages the requested range (spec §2): this is the current
    // page (10 rows) exactly as pushed, no local filtering.
    property var historySourceModel: []
    readonly property int tableWidth: 64 + 210 + 160 + Math.max(0, columnTitles.length - 2) * 146

    function columnWidth(index) { return index === 0 ? 210 : index === 1 ? 160 : 146 }
    function cellText(value) {
        return value === undefined || value === null ? "—"
             : typeof value === "number" ? value.toFixed(2) : String(value)
    }

    function twoDigits(value) {
        return value < 10 ? "0" + value : String(value)
    }

    function formatDate(date) {
        return date.getFullYear() + "/"
                + twoDigits(date.getMonth() + 1) + "/"
                + twoDigits(date.getDate())
    }

    function parseDate(value) {
        var match = value.match(/^(\d{4})[\/-](\d{2})[\/-](\d{2})$/)
        if (!match)
            return null

        var date = new Date(Number(match[1]), Number(match[2]) - 1,
                            Number(match[3]), 0, 0, 0, 0)
        if (date.getFullYear() !== Number(match[1])
                || date.getMonth() !== Number(match[2]) - 1
                || date.getDate() !== Number(match[3]))
            return null

        return date
    }

    function reloadHistoryData() {
        var rows = []
        var records = Td.historyRecords
        for (var i = 0; i < records.length; ++i)
            rows.push({ timestampMs: Number(records[i].timestampMs), values: records[i].values })
        rows.sort(function(a, b) { return b.timestampMs - a.timestampMs })
        historySourceModel = rows
        historyList.positionViewAtBeginning()
    }

    // Show the range the Core is currently paging (historyRangeFromMs/ToMs) in
    // the date fields. Unbounded ("顯示全部") leaves the fields empty.
    function isUnboundedRange(fromMs, toMs) {
        return fromMs <= unboundedFromMs && toMs >= unboundedToMs
    }

    // Reads Td directly (not the derived bindings) because it runs from
    // Connections handlers, where derived properties may not be updated yet.
    function syncRangeFields() {
        if (isUnboundedRange(Td.historyRangeFromMs, Td.historyRangeToMs)) {
            startDateField.text = ""
            endDateField.text = ""
            return
        }
        startDateField.text = formatDate(new Date(Td.historyRangeFromMs))
        endDateField.text = formatDate(new Date(Td.historyRangeToMs))
    }

    function rangeText() {
        if (isUnboundedRange(Td.historyRangeFromMs, Td.historyRangeToMs))
            return "全部"
        return formatDate(new Date(Td.historyRangeFromMs)) + " – "
                + formatDate(new Date(Td.historyRangeToMs))
    }

    // "篩選": ask the Core for the range (spec §2). Local calendar days:
    // start day 00:00:00.000 .. end day 23:59:59.999 (next day's midnight - 1 ms).
    function applyFilter() {
        if (!Td.transportReady)
            return

        var startDate = parseDate(startDateField.text)
        var endDate = parseDate(endDateField.text)

        if (!startDate || !endDate) {
            filterMessage = "請輸入正確日期（YYYY/MM/DD）"
            return
        }
        if (startDate.getTime() > endDate.getTime()) {
            filterMessage = "起始日期不可晚於結束日期"
            return
        }

        filterMessage = ""
        var endExclusive = new Date(endDate.getFullYear(), endDate.getMonth(), endDate.getDate() + 1)
        Td.historyRangeRequested(startDate.getTime(), endExclusive.getTime() - 1)
    }

    function goToPage(page) {
        if (page < 1 || page > totalPages || page === currentPage)
            return

        Td.historyCurrentPage = page
    }

    // "顯示全部": unbounded range, all months (spec §2).
    function showAllRecords() {
        if (!Td.transportReady)
            return

        filterMessage = ""
        Td.historyRangeRequested(unboundedFromMs, unboundedToMs)
    }

    function formatCount(value) {
        var text = String(Math.max(0, Math.round(Number(value) || 0)))
        return text.replace(/\B(?=(\d{3})+(?!\d))/g, ",")
    }

    // "下載 CSV": the Core exports the raw data of the current query range
    // (spec §3.1); progress comes back through historyExportStatus.
    function downloadCsv() {
        if (!Td.transportReady || exportActive)
            return

        exportRequestedHere = true
        dismissedExportKey = ""
        Td.historyExportRequested(Td.clientSessionId, Td.historyRangeFromMs, Td.historyRangeToMs)
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
        reloadHistoryData()
        syncRangeFields()
    }

    // TopNav.qml keeps this page instantiated and switches pages by binding
    // `visible` to root.currentPage, so the page becoming visible is the
    // moment the history page is shown. Ask the authoritative side to load.
    onVisibleChanged: {
        if (visible)
            Td.historyRefreshRequested()
    }

    Connections {
        target: Td
        function onHistoryRecordsChanged() {
            reloadHistoryData()
        }
        function onHistoryRangeFromMsChanged() {
            syncRangeFields()
        }
        function onHistoryRangeToMsChanged() {
            syncRangeFields()
        }
        function onHistoryExportStatusChanged() {
            handleExportStatus()
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

        Row {
            width: parent.width
            height: 54
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

                Text {
                    width: parent.width
                    text: "感測器與設備紀錄 · " + columnTitles.length + " 個欄位 · 左右捲動查看完整資料"
                    color: mutedTextColor
                    font.pixelSize: 14
                    elide: Text.ElideRight
                }
            }

            // Export job of this client (historyExportStatus[clientSessionId],
            // spec §3.2): status line, progress bar and cancel / close actions.
            Rectangle {
                id: exportPanel
                width: Math.min(480, Math.max(300, parent.width * 0.32))
                height: 54
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
                    elide: Text.ElideMiddle
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
                        enabled: Td.transportReady
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
                            text: "✕"
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
                enabled: Td.transportReady && !historyPage.exportActive
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

        Rectangle {
            width: parent.width
            height: 132
            radius: 10
            color: "#111D32"
            border.color: dividerColor
            border.width: 1

            Text {
                anchors.top: parent.top
                anchors.topMargin: 8
                anchors.left: parent.left
                anchors.leftMargin: 24
                anchors.right: parent.right
                anchors.rightMargin: 24
                text: "日期格式：YYYY/MM/DD（例如：2026/09/24），結束日期包含當天全部資料；篩選可跨月，「顯示全部」為不限區間"
                color: historyPage.mutedTextColor
                font.pixelSize: 12
                elide: Text.ElideRight
            }

            Row {
                anchors.left: parent.left
                anchors.leftMargin: 24
                anchors.verticalCenter: parent.verticalCenter
                spacing: 14

                Column {
                    spacing: 8

                    Text {
                        text: "起始日期"
                        color: mutedTextColor
                        font.pixelSize: 13
                    }

                    TextField {
                        id: startDateField
                        width: Math.max(170, Math.min(260, (historyPage.width - 450) / 2))
                        height: 46
                        color: "white"
                        font.pixelSize: 16
                        font.family: "Consolas"
                        selectByMouse: true
                        placeholderText: "YYYY/MM/DD"
                        placeholderTextColor: "#5F7890"

                        background: Rectangle {
                            radius: 6
                            color: "#0B1527"
                            border.color: startDateField.activeFocus ? root.mainBlue : dividerColor
                            border.width: 1
                        }

                        onAccepted: applyFilter()
                    }
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.verticalCenterOffset: 14
                    text: "—"
                    color: mutedTextColor
                    font.pixelSize: 20
                }

                Column {
                    spacing: 8

                    Text {
                        text: "結束日期"
                        color: mutedTextColor
                        font.pixelSize: 13
                    }

                    TextField {
                        id: endDateField
                        width: Math.max(170, Math.min(260, (historyPage.width - 450) / 2))
                        height: 46
                        color: "white"
                        font.pixelSize: 16
                        font.family: "Consolas"
                        selectByMouse: true
                        placeholderText: "YYYY/MM/DD"
                        placeholderTextColor: "#5F7890"

                        background: Rectangle {
                            radius: 6
                            color: "#0B1527"
                            border.color: endDateField.activeFocus ? root.mainBlue : dividerColor
                            border.width: 1
                        }

                        onAccepted: applyFilter()
                    }
                }

                Button {
                    id: filterButton
                    width: 110
                    height: 46
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.verticalCenterOffset: 14
                    // The range query is a request to the Core: disabled while offline.
                    enabled: Td.transportReady
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

                    onClicked: applyFilter()
                }

                Button {
                    id: allButton
                    width: 110
                    height: 46
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.verticalCenterOffset: 14
                    enabled: Td.transportReady
                    hoverEnabled: true

                    background: Rectangle {
                        radius: 6
                        color: !allButton.enabled ? "#16243A"
                             : allButton.hovered ? "#223D5A" : "transparent"
                        border.color: allButton.enabled ? dividerColor : "transparent"
                        border.width: 1
                    }

                    contentItem: Text {
                        text: "顯示全部"
                        color: allButton.enabled ? root.textColor : "#536A80"
                        font.pixelSize: 15
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    onClicked: showAllRecords()
                }
            }

            Text {
                anchors.right: parent.right
                anchors.rightMargin: 24
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 8
                text: filterMessage.length > 0
                      ? filterMessage
                      : "區間：" + historyPage.rangeText() + " · 本頁 " + historySourceModel.length + " 筆"
                color: filterMessage.length > 0 ? dangerColor : mutedTextColor
                font.pixelSize: 14
            }
        }

        Rectangle {
            width: parent.width
            height: parent.height - 230
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
                                        text: historyPage.cellText(recordRow.modelData.values[index])
                                        color: text === "ON" ? historyPage.dangerColor
                                             : text === "OFF" ? historyPage.successColor
                                             : index < 2 ? root.textColor : "#A7D9F5"
                                        font.pixelSize: 14
                                        font.family: index === 1 ? "Microsoft JhengHei" : "Consolas"
                                        verticalAlignment: Text.AlignVCenter
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
                text: "此日期區間沒有歷史資料"
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
                        // historyCurrentPage is a mirrored property: paging is a remote
                        // write, so it is disabled while the WASM transport is offline.
                        enabled: currentPage > 1 && Td.transportReady
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

                        onClicked: goToPage(currentPage - 1)
                    }

                    Text {
                        width: 110
                        anchors.verticalCenter: parent.verticalCenter
                        text: "第 " + currentPage + " / " + totalPages + " 頁"
                        color: "#BFD8EC"
                        font.pixelSize: 14
                        horizontalAlignment: Text.AlignHCenter
                    }

                    Button {
                        id: nextPageButton
                        width: 96
                        height: 34
                        enabled: currentPage < totalPages && Td.transportReady
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

                        onClicked: goToPage(currentPage + 1)
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
