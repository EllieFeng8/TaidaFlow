import QtQuick
import QtQuick.Controls
import TaidaFlowBackend 1.0
import "components" as Components
import "components/DateTimeUtil.js" as DateTimeUtil
import "components/HistoryViewUtil.js" as HistoryViewUtil
import "components/AlarmViewUtil.js" as AlarmViewUtil

// =========================================================
// 異常警告頁面
// =========================================================
Item {
    id: alarmPage

    // Below the offline banner (TopNav.qml): the banner pushes this page down
    // while shown; hidden it has height 0, i.e. this equals topNavBar.bottom.
    anchors.top: offlineBanner.bottom
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
    property int pageSize: AlarmViewUtil.pageSize
    property string filterMessage: ""
    property int totalPages: 1
    // Alarms in the shown range (all pages).
    property int totalCount: 0
    // Scale of TopNav (root), for the date/time picker popups (same as HistoryPage.qml).
    readonly property real designScale: parent ? parent.scale : 1
    readonly property int dateFieldWidth: 280

    // ---- Range and data source (w1-078, 2026-09-30) ------------------------------
    // Input like the history page (components/DateTimeField.qml + DateTimeUtil.js):
    // from = the start minute's :00.000, to = the end minute's :59.999, both inclusive.
    // Default = the last 24 hours (AlarmViewUtil.defaultRange), as before.
    //
    // Data: our own entry of Td.alarmViews (keyed by clientSessionId, written by the Core,
    // format in Core/TaidaFlowProxy.h) when there is one; until then (Core without alarm
    // views, main alone) or after the Core removed it, the page filters Td.alarmRecords
    // itself (AlarmViewUtil.localView, the former filter / 9 rows per page / 未處理 count).
    // Every filter, "顯示前一周", page change, page show and transport return sends
    // Td.alarmViewRequested(clientSessionId, fromMs, toMs, page), so a Core with alarm views
    // answers with our entry and the page switches to it; nothing waits for it.
    //
    // true while our own alarmViews entry is shown; false = alarmRecords fallback.
    property bool viewLoaded: false
    // Revision of the entry shown; null = none (fallback).
    property var appliedRevision: null
    // Error of our entry (state "error"), shown in the status text.
    property string viewErrorMessage: ""
    // Range of the rows shown.
    property double shownFromMs: 0
    property double shownToMs: 0
    // Our current range / page as last requested (or last shown).
    property double requestFromMs: 0
    property double requestToMs: 0
    property int requestPage: 1
    // true until 篩選 / 顯示前一周: the range is the last 24 hours and moves on with the
    // clock - recomputed when the page is shown and when alarmRecords changes (a new or
    // resolved alarm; page 1 then, like the former reload). A chosen range stays as it is.
    property bool rangeIsDefault: true

    ListModel { id: pagedAlarmModel }

    // Rows / counts of one view (our entry, or the fallback) into the table and pager.
    function showView(view) {
        shownFromMs = view.fromMs
        shownToMs = view.toMs
        currentPage = view.page
        totalPages = view.totalPages
        totalCount = view.totalCount
        activeAlarmCount = view.activeCount
        viewErrorMessage = view.state === "error" ? view.message : ""
        pagedAlarmModel.clear()
        for (var i = 0; i < view.rows.length; ++i)
            pagedAlarmModel.append(view.rows[i])
        alarmList.positionViewAtBeginning()
    }

    // Fallback: filter Td.alarmRecords in QML (no own entry).
    function showLocalView(fromMs, toMs, page) {
        showView(AlarmViewUtil.localView(Td.alarmRecords, fromMs, toMs, page, pageSize))
        // The fallback clamps the page (e.g. fewer rows after a reload).
        requestPage = currentPage
    }

    // Show our own entry of Td.alarmViews when its revision changed (same rules as
    // HistoryPage.applyOwnView). "lost" (the Core removed our entry) goes back to the
    // alarmRecords fallback with the same range and page, so the table never goes blank.
    function applyOwnView() {
        var change = AlarmViewUtil.viewChange(Td.alarmViews, Td.clientSessionId, appliedRevision)
        if (change.kind === "lost") {
            appliedRevision = null
            viewLoaded = false
            showLocalView(requestFromMs, requestToMs, requestPage)
            return
        }
        if (change.kind !== "apply")
            return

        var view = change.view
        var rangeChanged = !viewLoaded || view.fromMs !== shownFromMs || view.toMs !== shownToMs
        appliedRevision = view.revision
        viewLoaded = true
        requestFromMs = view.fromMs
        requestToMs = view.toMs
        requestPage = view.page
        showView(view)
        // Fields follow only a new range (not paging), so text being typed is kept.
        if (rangeChanged)
            setRangeFields(view.fromMs, view.toMs)
    }

    function setRangeFields(fromMs, toMs) {
        startTimeField.text = DateTimeUtil.formatDateTime(new Date(fromMs))
        endTimeField.text = DateTimeUtil.formatDateTime(new Date(toMs))
    }

    function rangeText() {
        return DateTimeUtil.formatDateTime(new Date(shownFromMs)) + " – "
                + DateTimeUtil.formatDateTime(new Date(shownToMs))
    }

    // The only alarm range request: our own session id, range and page. Without an own
    // entry the fallback shows the same range / page at once. Sent only while the page is
    // shown and the transport is ready (offline: remembered, sent when it comes back).
    function requestView(fromMs, toMs, page) {
        requestFromMs = fromMs
        requestToMs = toMs
        requestPage = page
        if (!viewLoaded)
            showLocalView(fromMs, toMs, page)
        if (!Td.transportReady || !visible)
            return
        Td.alarmViewRequested(Td.clientSessionId, fromMs, toMs, page)
    }

    // The last 24 hours up to now, in the fields and as our range.
    function useDefaultRange(page) {
        var range = AlarmViewUtil.defaultRange(new Date())
        setRangeFields(range.fromMs, range.toMs)
        requestView(range.fromMs, range.toMs, page)
    }

    // Page shown (or transport back while shown): ask for our current range and page again;
    // the default range first moves on to the last 24 hours up to now.
    function requestOwnView() {
        if (rangeIsDefault) {
            useDefaultRange(requestPage)
            return
        }
        requestView(requestFromMs, requestToMs, requestPage)
    }

    // alarmRecords changed (new / resolved alarm, or the Core reloaded it).
    function handleAlarmRecordsChanged() {
        if (rangeIsDefault) {
            // As the former page: back to the last 24 hours up to now, page 1.
            useDefaultRange(1)
            return
        }
        if (!viewLoaded)
            showLocalView(requestFromMs, requestToMs, requestPage)
    }

    // "篩選": same input rules and messages as HistoryPage.applyFilter (fields
    // YYYY/MM/DD HH:mm or the date only -> start 00:00 / end 23:59; page 1).
    function applyFilter() {
        if (!Td.transportReady)
            return

        var start = DateTimeUtil.parseDateTime(startTimeField.text, false)
        var end = DateTimeUtil.parseDateTime(endTimeField.text, true)

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
        rangeIsDefault = false
        requestView(fromMs, toMs, 1)
        // Normalize the fields (a date-only entry shows its 00:00 / 23:59).
        setRangeFields(fromMs, toMs)
    }

    // "顯示前一周" (was "顯示全部"): the last 7 local calendar days including today,
    // (today - 6 days) 00:00:00.000 .. today 23:59:59.999, page 1 - the history page's
    // range (HistoryViewUtil.lastWeekRange).
    function showLastWeek() {
        if (!Td.transportReady)
            return

        filterMessage = ""
        rangeIsDefault = false
        var range = HistoryViewUtil.lastWeekRange(new Date())
        requestView(range.fromMs, range.toMs, 1)
        setRangeFields(range.fromMs, range.toMs)
    }

    // Previous / next page of the shown range (a request like the history page's paging).
    function goToPage(page) {
        if (!Td.transportReady || page < 1 || page > totalPages || page === currentPage)
            return

        requestView(shownFromMs, shownToMs, page)
    }

    Component.onCompleted: {
        useDefaultRange(1)
        applyOwnView()
    }

    // TopNav.qml keeps this page instantiated and switches pages with `visible`.
    onVisibleChanged: {
        if (visible)
            requestOwnView()
    }

    Connections {
        target: Td
        function onAlarmRecordsChanged() {
            alarmPage.handleAlarmRecordsChanged()
        }
        function onAlarmViewsChanged() {
            alarmPage.applyOwnView()
        }
        // WASM: the mirror (re)connected while the page is shown - a request made
        // while offline was not sent. The desktop's transportReady never changes.
        function onTransportReadyChanged() {
            if (Td.transportReady && alarmPage.visible)
                alarmPage.requestOwnView()
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
            id: headerRow
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

        // Range filter like the history page: the format hint, then the date/time fields
        // and buttons with the current range (or the input error) to their right.
        // Height follows the content, so wrapped text is never cut off.
        Rectangle {
            id: filterPanel
            width: parent.width
            height: filterContent.height + 26
            radius: 10
            color: "#111D32"
            border.color: alarmPage.dividerColor
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

                Text {
                    width: parent.width
                    text: "日期時間格式：YYYY/MM/DD HH:mm（例如：2026/09/24 08:30），可直接輸入或按欄位右側的日曆圖示選擇；"
                          + "只輸入日期時，起始為 00:00、結束為 23:59；結束時間包含該分鐘；"
                          + "預設顯示最近 24 小時；篩選可跨月，「顯示前一周」為今天往前 7 天（含今天，00:00 至 23:59）"
                    color: alarmPage.mutedTextColor
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
                                color: alarmPage.mutedTextColor
                                font.pixelSize: 13
                            }

                            // Type YYYY/MM/DD HH:mm (or the date only -> 00:00) or pick it with
                            // the calendar button; Enter runs the filter, picking does not.
                            Components.DateTimeField {
                                id: startTimeField
                                width: alarmPage.dateFieldWidth
                                height: 46
                                isEnd: false
                                popupScale: alarmPage.designScale
                                borderColor: alarmPage.dividerColor
                                mutedTextColor: alarmPage.mutedTextColor
                                onAccepted: alarmPage.applyFilter()
                            }
                        }

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.verticalCenterOffset: 14
                            text: "—"
                            color: alarmPage.mutedTextColor
                            font.pixelSize: 20
                        }

                        Column {
                            spacing: 8

                            Text {
                                text: "結束日期時間"
                                color: alarmPage.mutedTextColor
                                font.pixelSize: 13
                            }

                            // Date only -> 23:59 (the whole end day).
                            Components.DateTimeField {
                                id: endTimeField
                                width: alarmPage.dateFieldWidth
                                height: 46
                                isEnd: true
                                popupScale: alarmPage.designScale
                                borderColor: alarmPage.dividerColor
                                mutedTextColor: alarmPage.mutedTextColor
                                onAccepted: alarmPage.applyFilter()
                            }
                        }

                        Button {
                            id: filterButton
                            width: 110
                            height: 46
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.verticalCenterOffset: 14
                            // The range query is a request to the Core: disabled while offline
                            // (as on the history page).
                            enabled: Td.transportReady
                            hoverEnabled: true

                            background: Rectangle {
                                radius: 6
                                color: !filterButton.enabled ? "#16243A"
                                     : filterButton.hovered ? "#FF7881" : alarmPage.dangerColor
                            }

                            contentItem: Text {
                                text: "篩選"
                                color: filterButton.enabled ? "white" : "#536A80"
                                font.pixelSize: 16
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }

                            onClicked: alarmPage.applyFilter()
                        }

                        Button {
                            id: lastWeekButton
                            width: 110
                            height: 46
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.verticalCenterOffset: 14
                            enabled: Td.transportReady
                            hoverEnabled: true

                            background: Rectangle {
                                radius: 6
                                color: !lastWeekButton.enabled ? "#16243A"
                                     : lastWeekButton.hovered ? "#223D5A" : "transparent"
                                border.color: lastWeekButton.enabled ? alarmPage.dividerColor : "transparent"
                                border.width: 1
                            }

                            contentItem: Text {
                                text: "顯示前一周"
                                color: lastWeekButton.enabled ? root.textColor : "#536A80"
                                font.pixelSize: 15
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }

                            onClicked: alarmPage.showLastWeek()
                        }
                    }

                    // Current range and count (or the input / read error), level with the fields.
                    Text {
                        id: statusText
                        anchors.left: filterRow.right
                        anchors.leftMargin: 24
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 46
                        text: alarmPage.filterMessage.length > 0 ? alarmPage.filterMessage
                            : alarmPage.viewErrorMessage.length > 0 ? "讀取警報失敗：" + alarmPage.viewErrorMessage
                            : "區間：" + alarmPage.rangeText() + " · 共 " + alarmPage.totalCount + " 筆"
                        color: alarmPage.filterMessage.length > 0 || alarmPage.viewErrorMessage.length > 0 ? alarmPage.dangerColor : alarmPage.mutedTextColor
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

            Text {
                anchors.centerIn: alarmList
                visible: pagedAlarmModel.count === 0 && alarmPage.viewErrorMessage.length === 0
                text: "此日期區間沒有警報紀錄"
                color: alarmPage.mutedTextColor
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
                        // Paging is a request (alarmViewRequested): disabled while offline,
                        // as on the history page.
                        enabled: alarmPage.currentPage > 1 && Td.transportReady
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
                        enabled: alarmPage.currentPage < alarmPage.totalPages && Td.transportReady
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
