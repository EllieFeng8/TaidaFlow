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
    readonly property int currentPage: Td.historyCurrentPage
    readonly property int totalPages: Td.historyTotalPages

    readonly property var columnTitles: Td.historyTitle
    property var historySourceModel: []
    property var filteredHistoryModel: []
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
    }

    function applyFilter() {
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
        // Include the entire end date, using the next local calendar day's midnight.
        var endExclusive = new Date(endDate.getFullYear(), endDate.getMonth(), endDate.getDate() + 1)
        filteredHistoryModel = historySourceModel.filter(function(row) {
            return row.timestampMs >= startDate.getTime()
                    && row.timestampMs < endExclusive.getTime()
        })
        historyList.positionViewAtBeginning()
    }

    function goToPage(page) {
        if (page < 1 || page > totalPages || page === currentPage)
            return

        Td.historyCurrentPage = page
    }

    function showAllRecords() {
        if (historySourceModel.length === 0)
            return

        startDateField.text = formatDate(new Date(historySourceModel[historySourceModel.length - 1].timestampMs))
        endDateField.text = formatDate(new Date(historySourceModel[0].timestampMs))
        applyFilter()
    }

    function csvCell(value) {
        return "\"" + String(value).replace(/\"/g, "\"\"") + "\""
    }

    function downloadCsv() {
        if (filteredHistoryModel.length === 0) {
            exportMessage = "目前沒有可下載的資料"
            exportMessageTimer.restart()
            return
        }

        var headings = [csvCell("序號")]
        for (var c = 0; c < columnTitles.length; ++c)
            headings.push(csvCell(columnTitles[c]))
        var lines = [headings.join(",")]
        for (var i = 0; i < filteredHistoryModel.length; ++i) {
            var cells = [csvCell(i + 1)]
            for (var j = 0; j < columnTitles.length; ++j)
                cells.push(csvCell(cellText(filteredHistoryModel[i].values[j])))
            lines.push(cells.join(","))
        }

        var savedPath = Td.saveHistoryCsv(lines.join("\r\n"))
        if (savedPath.indexOf("ERROR:") === 0)
            exportMessage = "下載失敗：" + savedPath.substring(6)
        else if (savedPath.indexOf("DOWNLOAD:") === 0)
            // WebAssembly: the browser download was started asynchronously; the
            // file name is only a hint, the browser decides where the file goes.
            exportMessage = "已開始下載 " + filteredHistoryModel.length + " 筆資料（"
                    + savedPath.substring(9) + "）"
        else if (savedPath.length > 0)
            exportMessage = "已下載 " + filteredHistoryModel.length + " 筆資料"
        else
            exportMessage = ""

        if (exportMessage.length > 0)
            exportMessageTimer.restart()
    }

    Component.onCompleted: {
        reloadHistoryData()
        var now = new Date()
        var oneDayAgo = new Date(now.getFullYear(), now.getMonth(), now.getDate() - 1)
        startDateField.text = formatDate(oneDayAgo)
        endDateField.text = formatDate(now)
        applyFilter()
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
            applyFilter()
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

            Column {
                width: parent.width - downloadButton.width
                spacing: 4

                Text {
                    text: "歷史資料"
                    color: root.textColor
                    font.pixelSize: 28
                    font.bold: true
                }

                Text {
                    text: "感測器與設備紀錄 · " + columnTitles.length + " 個欄位 · 左右捲動查看完整資料"
                    color: mutedTextColor
                    font.pixelSize: 14
                }
            }

            Button {
                id: downloadButton
                width: 166
                height: 48
                hoverEnabled: true

                background: Rectangle {
                    radius: 7
                    color: downloadButton.hovered ? "#16A34A" : successColor
                    border.color: downloadButton.hovered ? "#86EFAC" : "transparent"
                    border.width: 1
                }

                contentItem: Text {
                    text: "↓  下載 CSV"
                    color: "white"
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
                text: "日期格式：YYYY/MM/DD（例如：2026/09/24），結束日期包含當天全部資料"
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
                    hoverEnabled: true

                    background: Rectangle {
                        radius: 6
                        color: filterButton.hovered ? root.lightBlue : root.mainBlue
                    }

                    contentItem: Text {
                        text: "篩選"
                        color: "white"
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
                    hoverEnabled: true

                    background: Rectangle {
                        radius: 6
                        color: allButton.hovered ? "#223D5A" : "transparent"
                        border.color: dividerColor
                        border.width: 1
                    }

                    contentItem: Text {
                        text: "顯示全部"
                        color: root.textColor
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
                      : "共 " + filteredHistoryModel.length + " 筆"
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
                    model: historyPage.filteredHistoryModel
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
                visible: filteredHistoryModel.length === 0 && filterMessage.length === 0
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
