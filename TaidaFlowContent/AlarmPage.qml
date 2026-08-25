import QtQuick
import QtQuick.Controls
import Core 1.0

// =========================================================
// 異常警告頁面
// =========================================================
Item {
    id: alarmPage

    anchors.top: topNavBar.bottom
    anchors.left: parent.left
    anchors.right: parent.right
    anchors.bottom: parent.bottom

    visible: root.currentPage === 1

    property color mutedTextColor: "#8FAFC8"
    property color dividerColor: "#284766"
    property color dangerColor: "#FF5964"
    property color warningColor: "#F59E0B"
    property color successColor: "#22C55E"
    property int activeAlarmCount: 0
    property int currentPage: 1
    property int pageSize: 9
    property string filterMessage: ""
    readonly property int totalPages: Math.max(1, Math.ceil(filteredAlarmModel.count / pageSize))

    ListModel { id: alarmListModel }
    ListModel { id: filteredAlarmModel }
    ListModel { id: pagedAlarmModel }

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

    function reloadAlarmData() {
        alarmListModel.clear()
        activeAlarmCount = 0
        var records = Td.alarmRecords
        for (var i = 0; i < records.length; ++i) {
            var row = records[i]
            alarmListModel.append({
                "serialNumber": i + 1,
                "timestampMs": Number(row.timestampMs),
                "alarmTime": String(row.alarmTime),
                "equipment": String(row.equipment),
                "sensorName": String(row.sensorName),
                "alarmMessage": String(row.alarmMessage),
                "severity": String(row.severity),
                "alarmStatus": String(row.alarmStatus)
            })
        }

        var now = new Date()
        var oneDayAgo = new Date(now.getTime() - 24 * 60 * 60 * 1000)
        startTimeField.text = formatDateTime(oneDayAgo)
        endTimeField.text = formatDateTime(now)
        applyFilter()
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
        activeAlarmCount = 0
        filteredAlarmModel.clear()

        for (var i = 0; i < alarmListModel.count; ++i) {
            var row = alarmListModel.get(i)
            if (row.timestampMs >= startDate.getTime()
                    && row.timestampMs <= endDate.getTime()) {
                if (row.alarmStatus === "未處理")
                    activeAlarmCount += 1

                filteredAlarmModel.append({
                    "serialNumber": filteredAlarmModel.count + 1,
                    "timestampMs": row.timestampMs,
                    "alarmTime": row.alarmTime,
                    "equipment": row.equipment,
                    "sensorName": row.sensorName,
                    "alarmMessage": row.alarmMessage,
                    "severity": row.severity,
                    "alarmStatus": row.alarmStatus
                })
            }
        }

        currentPage = 1
        refreshPagedModel()
    }

    function showAllRecords() {
        if (alarmListModel.count === 0)
            return

        startTimeField.text = alarmListModel.get(alarmListModel.count - 1).alarmTime
        endTimeField.text = alarmListModel.get(0).alarmTime
        applyFilter()
    }

    function refreshPagedModel() {
        pagedAlarmModel.clear()

        if (filteredAlarmModel.count === 0) {
            currentPage = 1
            return
        }

        currentPage = Math.max(1, Math.min(currentPage, totalPages))
        var startIndex = (currentPage - 1) * pageSize
        var endIndex = Math.min(startIndex + pageSize, filteredAlarmModel.count)

        for (var i = startIndex; i < endIndex; ++i) {
            var row = filteredAlarmModel.get(i)
            pagedAlarmModel.append({
                "serialNumber": row.serialNumber,
                "timestampMs": row.timestampMs,
                "alarmTime": row.alarmTime,
                "equipment": row.equipment,
                "sensorName": row.sensorName,
                "alarmMessage": row.alarmMessage,
                "severity": row.severity,
                "alarmStatus": row.alarmStatus
            })
        }

        alarmList.positionViewAtBeginning()
    }

    function goToPage(page) {
        if (page < 1 || page > totalPages || page === currentPage)
            return

        currentPage = page
        refreshPagedModel()
    }

    Component.onCompleted: reloadAlarmData()

    Connections {
        target: Td
        function onAlarmRecordsChanged() {
            reloadAlarmData()
        }
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
                width: parent.width - activeBadge.width
                spacing: 4

                Text {
                    text: "異常警告"
                    color: root.textColor
                    font.pixelSize: 28
                    font.bold: true
                }

                Text {
                    text: "設備異常、感測器警報與處理狀態"
                    color: mutedTextColor
                    font.pixelSize: 14
                }
            }

            Rectangle {
                id: activeBadge
                width: 166
                height: 48
                radius: 7
                color: "#3A2932"
                border.color: dangerColor
                border.width: 1

                Row {
                    anchors.centerIn: parent
                    spacing: 10

                    Rectangle {
                        width: 10
                        height: 10
                        radius: 5
                        anchors.verticalCenter: parent.verticalCenter
                        color: dangerColor
                    }

                    Text {
                        text: "未處理 " + activeAlarmCount + " 筆"
                        color: "#FF8790"
                        font.pixelSize: 15
                        font.bold: true
                    }
                }
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
                        color: filterButton.hovered ? "#FF7881" : dangerColor
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
                        color: allButton.hovered ? "#3A2932" : "transparent"
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
                      : "共 " + filteredAlarmModel.count + " 筆"
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
                id: alarmTableHeader
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
                            { "title": "時間", "ratio": 0.19 },
                            { "title": "設備", "ratio": 0.17 },
                            { "title": "SENSOR", "ratio": 0.14 },
                            { "title": "警報內容", "ratio": 0.28 },
                            { "title": "狀態", "ratio": 0.15 }
                        ]

                        Item {
                            width: (alarmTableHeader.width - 48) * modelData.ratio
                            height: alarmTableHeader.height

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
                id: alarmList
                anchors.top: alarmTableHeader.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: paginationBar.top
                model: pagedAlarmModel
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AsNeeded
                }

                delegate: Rectangle {
                    width: alarmList.width
                    height: 62
                    color: model.alarmStatus === "未處理"
                           ? (index % 2 === 0 ? "#231C2A" : "#281E2C")
                           : (index % 2 === 0 ? "#101C30" : "#132139")

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 1
                        color: "#203B57"
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 3
                        visible: model.alarmStatus === "未處理"
                        color: dangerColor
                    }

                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: 24
                        anchors.rightMargin: 24

                        Item {
                            width: (alarmList.width - 48) * 0.07
                            height: parent.height
                            Text {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                text: model.serialNumber
                                color: mutedTextColor
                                font.pixelSize: 15
                                font.family: "Consolas"
                            }
                        }

                        Item {
                            width: (alarmList.width - 48) * 0.19
                            height: parent.height
                            Text {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                text: model.alarmTime
                                color: root.textColor
                                font.pixelSize: 15
                                font.family: "Consolas"
                            }
                        }

                        Item {
                            width: (alarmList.width - 48) * 0.17
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
                            width: (alarmList.width - 48) * 0.14
                            height: parent.height
                            Text {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                text: model.sensorName
                                color: model.alarmStatus === "未處理" ? "#FF8790" : root.lightBlue
                                font.pixelSize: 15
                                font.bold: true
                                font.family: "Consolas"
                            }
                        }

                        Item {
                            width: (alarmList.width - 48) * 0.28
                            height: parent.height

                            Row {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 10

                                Rectangle {
                                    width: 48
                                    height: 26
                                    radius: 13
                                    color: model.severity === "嚴重" ? "#462B35" : "#46391F"
                                    border.color: model.severity === "嚴重" ? dangerColor : warningColor
                                    border.width: 1

                                    Text {
                                        anchors.centerIn: parent
                                        text: model.severity
                                        color: model.severity === "嚴重" ? "#FF8790" : "#FBC85C"
                                        font.pixelSize: 12
                                        font.bold: true
                                    }
                                }

                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: model.alarmMessage
                                    color: root.textColor
                                    font.pixelSize: 14
                                }
                            }
                        }

                        Item {
                            width: (alarmList.width - 48) * 0.15
                            height: parent.height

                            Rectangle {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                width: 76
                                height: 30
                                radius: 15
                                color: model.alarmStatus === "未處理" ? "#462B35" : "#173A32"
                                border.color: model.alarmStatus === "未處理" ? dangerColor : successColor
                                border.width: 1

                                Text {
                                    anchors.centerIn: parent
                                    text: model.alarmStatus
                                    color: model.alarmStatus === "未處理" ? "#FF8790" : "#6EE7A0"
                                    font.pixelSize: 13
                                    font.bold: true
                                }
                            }
                        }
                    }
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
                                   : previousPageButton.hovered ? "#3A2932" : "#1B304A"
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
                                   : nextPageButton.hovered ? "#3A2932" : "#1B304A"
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
}
