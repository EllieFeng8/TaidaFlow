import QtQuick
import QtQuick.Controls
import Core 1.0

// =========================================================
// 歷史紀錄頁面
// =========================================================
Item {
    id: historyPage

    anchors.top: topNavBar.bottom
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
    property int currentPage: 1
    property int pageSize: 9
    readonly property int totalPages: Math.max(1, Math.ceil(filteredHistoryModel.count / pageSize))

    ListModel { id: historySourceModel }
    ListModel { id: filteredHistoryModel }
    ListModel { id: pagedHistoryModel }

    function twoDigits(value) {
        return value < 10 ? "0" + value : String(value)
    }

    function formatDateTime(date) {
        return date.getFullYear() + "/"
                + twoDigits(date.getMonth() + 1) + "/"
                + twoDigits(date.getDate()) + " "
                + twoDigits(date.getHours()) + ":"
                + twoDigits(date.getMinutes())
    }

    function parseDateTime(value) {
        var match = value.match(/^(\d{4})[\/-](\d{2})[\/-](\d{2})\s+(\d{2}):(\d{2})$/)
        if (!match)
            return null

        var date = new Date(Number(match[1]), Number(match[2]) - 1,
                            Number(match[3]), Number(match[4]), Number(match[5]), 0, 0)
        if (date.getFullYear() !== Number(match[1])
                || date.getMonth() !== Number(match[2]) - 1
                || date.getDate() !== Number(match[3])
                || date.getHours() !== Number(match[4])
                || date.getMinutes() !== Number(match[5]))
            return null

        return date
    }

    function seedHistoryData() {
        historySourceModel.clear()

        var devices = ["循環泵浦 A", "循環泵浦 B", "主水槽", "過濾器", "測試設備", "加熱器"]
        var sensors = ["TT-01", "PT-02", "LS-01", "FM-01", "TT-03", "PT-06"]
        var now = new Date()

        for (var i = 0; i < 36; ++i) {
            var recordDate = new Date(now.getTime() - i * 2 * 60 * 60 * 1000)
            var leakOn = i === 7 || i === 19
            historySourceModel.append({
                "timestampMs": recordDate.getTime(),
                "recordTime": formatDateTime(recordDate),
                "equipment": devices[i % devices.length],
                "sensorName": sensors[i % sensors.length],
                "switchState": (i % 5 === 0 || leakOn) ? "OFF" : "ON",
                "leakState": leakOn ? "ON" : "OFF"
            })
        }
    }

    function applyFilter() {
        var startDate = parseDateTime(startTimeField.text)
        var endDate = parseDateTime(endTimeField.text)

        if (!startDate || !endDate) {
            filterMessage = "請輸入正確時間（YYYY/MM/DD HH:MM）"
            return
        }
        if (startDate.getTime() > endDate.getTime()) {
            filterMessage = "起始時間不可晚於結束時間"
            return
        }

        filterMessage = ""
        filteredHistoryModel.clear()
        for (var i = 0; i < historySourceModel.count; ++i) {
            var row = historySourceModel.get(i)
            if (row.timestampMs >= startDate.getTime()
                    && row.timestampMs <= endDate.getTime()) {
                filteredHistoryModel.append({
                    "timestampMs": row.timestampMs,
                    "recordTime": row.recordTime,
                    "equipment": row.equipment,
                    "sensorName": row.sensorName,
                    "switchState": row.switchState,
                    "leakState": row.leakState
                })
            }
        }
        currentPage = 1
        refreshPagedModel()
    }

    function refreshPagedModel() {
        pagedHistoryModel.clear()

        if (filteredHistoryModel.count === 0) {
            currentPage = 1
            return
        }

        currentPage = Math.max(1, Math.min(currentPage, totalPages))
        var startIndex = (currentPage - 1) * pageSize
        var endIndex = Math.min(startIndex + pageSize, filteredHistoryModel.count)

        for (var i = startIndex; i < endIndex; ++i) {
            var row = filteredHistoryModel.get(i)
            pagedHistoryModel.append({
                "serialNumber": i + 1,
                "timestampMs": row.timestampMs,
                "recordTime": row.recordTime,
                "equipment": row.equipment,
                "sensorName": row.sensorName,
                "switchState": row.switchState,
                "leakState": row.leakState
            })
        }

        historyList.positionViewAtBeginning()
    }

    function goToPage(page) {
        if (page < 1 || page > totalPages || page === currentPage)
            return

        currentPage = page
        refreshPagedModel()
    }

    function showAllRecords() {
        if (historySourceModel.count === 0)
            return

        startTimeField.text = historySourceModel.get(historySourceModel.count - 1).recordTime
        endTimeField.text = historySourceModel.get(0).recordTime
        applyFilter()
    }

    function csvCell(value) {
        return "\"" + String(value).replace(/\"/g, "\"\"") + "\""
    }

    function downloadCsv() {
        if (filteredHistoryModel.count === 0) {
            exportMessage = "目前沒有可下載的資料"
            exportMessageTimer.restart()
            return
        }

        var lines = ["序號,時間,設備,SENSOR,開關,漏水"]
        for (var i = 0; i < filteredHistoryModel.count; ++i) {
            var row = filteredHistoryModel.get(i)
            lines.push(csvCell(i + 1) + ","
                       + csvCell(row.recordTime) + ","
                       + csvCell(row.equipment) + ","
                       + csvCell(row.sensorName) + ","
                       + csvCell(row.switchState) + ","
                       + csvCell(row.leakState))
        }

        var savedPath = Td.saveHistoryCsv(lines.join("\r\n"))
        if (savedPath.indexOf("ERROR:") === 0)
            exportMessage = "下載失敗：" + savedPath.substring(6)
        else if (savedPath.length > 0)
            exportMessage = "已下載 " + filteredHistoryModel.count + " 筆資料"
        else
            exportMessage = ""

        if (exportMessage.length > 0)
            exportMessageTimer.restart()
    }

    Component.onCompleted: {
        seedHistoryData()
        var now = new Date()
        var oneDayAgo = new Date(now.getTime() - 24 * 60 * 60 * 1000)
        startTimeField.text = formatDateTime(oneDayAgo)
        endTimeField.text = formatDateTime(now)
        applyFilter()
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
                    text: "查看設備運行、感測器與漏水狀態"
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
            height: 124
            radius: 10
            color: "#111D32"
            border.color: dividerColor
            border.width: 1

            Row {
                anchors.left: parent.left
                anchors.leftMargin: 24
                anchors.verticalCenter: parent.verticalCenter
                spacing: 14

                Column {
                    spacing: 8

                    Text {
                        text: "起始時間"
                        color: mutedTextColor
                        font.pixelSize: 13
                    }

                    TextField {
                        id: startTimeField
                        width: 260
                        height: 46
                        color: "white"
                        font.pixelSize: 16
                        font.family: "Consolas"
                        selectByMouse: true
                        placeholderText: "YYYY/MM/DD HH:MM"
                        placeholderTextColor: "#5F7890"

                        background: Rectangle {
                            radius: 6
                            color: "#0B1527"
                            border.color: startTimeField.activeFocus ? root.mainBlue : dividerColor
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
                        text: "結束時間"
                        color: mutedTextColor
                        font.pixelSize: 13
                    }

                    TextField {
                        id: endTimeField
                        width: 260
                        height: 46
                        color: "white"
                        font.pixelSize: 16
                        font.family: "Consolas"
                        selectByMouse: true
                        placeholderText: "YYYY/MM/DD HH:MM"
                        placeholderTextColor: "#5F7890"

                        background: Rectangle {
                            radius: 6
                            color: "#0B1527"
                            border.color: endTimeField.activeFocus ? root.mainBlue : dividerColor
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
                anchors.verticalCenter: parent.verticalCenter
                text: filterMessage.length > 0
                      ? filterMessage
                      : "共 " + filteredHistoryModel.count + " 筆"
                color: filterMessage.length > 0 ? dangerColor : mutedTextColor
                font.pixelSize: 14
            }
        }

        Rectangle {
            width: parent.width
            height: parent.height - 222
            radius: 10
            color: "#111D32"
            border.color: dividerColor
            border.width: 1
            clip: true

            Rectangle {
                id: tableHeader
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: 56
                color: "#172941"

                Row {
                    anchors.fill: parent
                    anchors.leftMargin: 24
                    anchors.rightMargin: 24

                    Repeater {
                        model: [
                            { "title": "序號", "ratio": 0.07 },
                            { "title": "時間", "ratio": 0.21 },
                            { "title": "設備", "ratio": 0.20 },
                            { "title": "SENSOR", "ratio": 0.19 },
                            { "title": "開關", "ratio": 0.15 },
                            { "title": "漏水 (ON/OFF)", "ratio": 0.18 }
                        ]

                        Item {
                            width: (tableHeader.width - 48) * modelData.ratio
                            height: tableHeader.height

                            Text {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData.title
                                color: "#BFD8EC"
                                font.pixelSize: 14
                                font.bold: true
                            }
                        }
                    }
                }
            }

            ListView {
                id: historyList
                anchors.top: tableHeader.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: paginationBar.top
                model: pagedHistoryModel
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AsNeeded
                }

                delegate: Rectangle {
                    width: historyList.width
                    height: 62
                    color: index % 2 === 0 ? "#101C30" : "#132139"

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 1
                        color: "#203B57"
                    }

                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: 24
                        anchors.rightMargin: 24

                        Item {
                            width: (historyList.width - 48) * 0.07
                            height: parent.height
                            Text {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                text: model.serialNumber
                                color: historyPage.mutedTextColor
                                font.pixelSize: 15
                                font.family: "Consolas"
                            }
                        }

                        Item {
                            width: (historyList.width - 48) * 0.21
                            height: parent.height
                            Text {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                text: model.recordTime
                                color: root.textColor
                                font.pixelSize: 15
                                font.family: "Consolas"
                            }
                        }

                        Item {
                            width: (historyList.width - 48) * 0.20
                            height: parent.height
                            Text {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                text: model.equipment
                                color: root.textColor
                                font.pixelSize: 15
                            }
                        }

                        Item {
                            width: (historyList.width - 48) * 0.19
                            height: parent.height
                            Text {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                text: model.sensorName
                                color: root.lightBlue
                                font.pixelSize: 15
                                font.bold: true
                                font.family: "Consolas"
                            }
                        }

                        Item {
                            width: (historyList.width - 48) * 0.15
                            height: parent.height

                            Rectangle {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                width: 68
                                height: 30
                                radius: 15
                                color: model.switchState === "ON" ? "#183E32" : "#3B2D37"
                                border.color: model.switchState === "ON" ? successColor : dangerColor
                                border.width: 1

                                Text {
                                    anchors.centerIn: parent
                                    text: model.switchState
                                    color: model.switchState === "ON" ? "#6EE7A0" : "#FF8790"
                                    font.pixelSize: 13
                                    font.bold: true
                                    font.family: "Consolas"
                                }
                            }
                        }

                        Item {
                            width: (historyList.width - 48) * 0.18
                            height: parent.height

                            Rectangle {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                width: 68
                                height: 30
                                radius: 15
                                color: model.leakState === "ON" ? "#462B35" : "#173A32"
                                border.color: model.leakState === "ON" ? dangerColor : successColor
                                border.width: 1

                                Text {
                                    anchors.centerIn: parent
                                    text: model.leakState
                                    color: model.leakState === "ON" ? "#FF8790" : "#6EE7A0"
                                    font.pixelSize: 13
                                    font.bold: true
                                    font.family: "Consolas"
                                }
                            }
                        }
                    }
                }

                Text {
                    anchors.centerIn: parent
                    visible: filteredHistoryModel.count === 0 && filterMessage.length === 0
                    text: "此時間區間沒有歷史資料"
                    color: mutedTextColor
                    font.pixelSize: 18
                }
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
                        enabled: currentPage > 1
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
                        enabled: currentPage < totalPages
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
