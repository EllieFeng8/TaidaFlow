pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "DateTimeUtil.js" as DateTimeUtil

// =========================================================
// Date + time input of the History page range (spec §2, revised 2026-09-25):
// type "YYYY/MM/DD HH:mm" (or the date only) and press Enter, or pick it with
// the calendar button on the right: month calendar + hour/minute lists, then
// 「確定」. Picking never runs the filter; the page still does that on Enter /
// 「篩選」. The typed text stays the single source of the value (`text`).
// =========================================================
Item {
    id: control

    // The field text; HistoryPage reads it in applyFilter() and writes it in
    // syncRangeFields().
    property alias text: input.text
    // End-of-range field: a date-only value and the picker default use 23:59
    // instead of 00:00.
    property bool isEnd: false
    // Visual scale of the page. Web (App.qml): the 1920x1080 design (TopNav)
    // is scaled by webScale, but a popup is drawn in the window overlay, which
    // that scale does not reach, so the popup applies it itself. Desktop: 1.
    property real popupScale: 1

    // Dark theme of the page (TopNav.qml / HistoryPage.qml colors).
    property color accentColor: "#0087DC"
    property color accentHoverColor: "#19B8FF"
    property color borderColor: "#284766"
    property color textColor: "#E8F4FF"
    property color mutedTextColor: "#8FAFC8"
    property color dimTextColor: "#536A80"
    property color fieldColor: "#0B1527"
    property color panelColor: "#111D32"
    property color hoverColor: "#223D5A"

    // Picker state: the selection shown in the popup, only written to `text`
    // by 「確定」.
    property int viewYear: 2000
    property int viewMonth: 0
    property int selectedYear: 2000
    property int selectedMonth: 0
    property int selectedDay: 1
    property int selectedHour: 0
    property int selectedMinute: 0
    // Today when the picker was opened (the "today" mark of the calendar).
    property int todayYear: 2000
    property int todayMonth: 0
    property int todayDay: 1

    readonly property alias pickerOpen: picker.visible
    // The picker popup (read-only access, e.g. for tests of its geometry).
    readonly property alias pickerPopup: picker
    readonly property string selectedText: DateTimeUtil.formatParts(selectedYear, selectedMonth, selectedDay,
                                                                    selectedHour, selectedMinute)
    // Week starts on Sunday (日 一 二 三 四 五 六). DayOfWeekRow and MonthGrid use
    // the same locale, so the header and the day columns always line up.
    readonly property var calendarLocale: Qt.locale("zh_TW")
    readonly property var weekdayLabels: ["", "一", "二", "三", "四", "五", "六", "日"]   // Qt.DayOfWeek 1..7

    readonly property int cellWidth: 40
    readonly property int cellHeight: 32
    readonly property int timeListWidth: 64
    readonly property int pickerButtonWidth: 76

    signal accepted()

    implicitWidth: 280
    implicitHeight: 46

    function openPicker() {
        var now = new Date()
        todayYear = now.getFullYear()
        todayMonth = now.getMonth()
        todayDay = now.getDate()

        // Start from the field's current value; empty or invalid -> today with
        // the field's default time (start 00:00, end 23:59).
        var parts = DateTimeUtil.parseDateTime(input.text, isEnd)
        if (!parts)
            parts = DateTimeUtil.dayParts(now, isEnd)
        selectedYear = parts.year
        selectedMonth = parts.month
        selectedDay = parts.day
        selectedHour = parts.hour
        selectedMinute = parts.minute
        viewYear = parts.year
        viewMonth = parts.month
        picker.open()
    }

    function showMonth(offset) {
        var first = new Date(viewYear, viewMonth + offset, 1)
        viewYear = first.getFullYear()
        viewMonth = first.getMonth()
    }

    // A day of another month (dimmed cells) also switches the view to it.
    function selectDay(year, month, day) {
        selectedYear = year
        selectedMonth = month
        selectedDay = day
        if (year !== viewYear || month !== viewMonth) {
            viewYear = year
            viewMonth = month
        }
    }

    function confirmPicker() {
        input.text = selectedText
        picker.close()
    }

    TextField {
        id: input
        objectName: "dateTimeInput"
        anchors.fill: parent
        rightPadding: calendarButton.width + calendarButton.anchors.rightMargin + 8
        color: "white"
        font.pixelSize: 16
        font.family: "Consolas"
        selectByMouse: true
        placeholderText: "YYYY/MM/DD HH:mm"
        placeholderTextColor: "#5F7890"

        background: Rectangle {
            radius: 6
            color: control.fieldColor
            border.color: input.activeFocus || picker.visible ? control.accentColor : control.borderColor
            border.width: 1
        }

        onAccepted: control.accepted()
    }

    Button {
        id: calendarButton
        anchors.right: parent.right
        anchors.rightMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        width: 38
        height: 38
        hoverEnabled: true
        // Keep the keyboard focus (and the cursor) in the text field.
        focusPolicy: Qt.NoFocus

        background: Rectangle {
            radius: 5
            color: calendarButton.hovered || picker.visible ? control.hoverColor : "transparent"
        }

        // Calendar glyph drawn with rectangles (no icon font / image needed on the web).
        contentItem: Item {
            Rectangle {
                id: glyphBody
                anchors.centerIn: parent
                anchors.verticalCenterOffset: 1
                width: 18
                height: 16
                radius: 2
                color: "transparent"
                border.color: picker.visible ? control.accentHoverColor : control.mutedTextColor
                border.width: 2

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 5
                    color: glyphBody.border.color
                }

                Grid {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 3
                    columns: 3
                    spacing: 2

                    Repeater {
                        model: 6
                        delegate: Rectangle {
                            width: 2
                            height: 2
                            color: glyphBody.border.color
                        }
                    }
                }
            }

            Repeater {
                model: 2
                delegate: Rectangle {
                    required property int index
                    x: glyphBody.x + 4 + index * 8
                    y: glyphBody.y - 3
                    width: 2
                    height: 5
                    radius: 1
                    color: glyphBody.border.color
                }
            }
        }

        onClicked: {
            if (picker.visible)
                picker.close()
            else
                control.openPicker()
        }
    }

    // Opened from the calendar button: Popup.CloseOnPressOutsideParent with the
    // button as parent lets the button toggle the popup (a press on it does not
    // close it first), while a press anywhere else - the text field included -
    // closes it without changing the value, like Esc and 「取消」.
    Popup {
        id: picker
        parent: calendarButton
        // Always an item in this window's overlay (never a separate window), so
        // the web scaling below applies on every platform.
        popupType: Popup.Item
        // Just below the field, left-aligned with it. x/y are in the parent's
        // (calendarButton) coordinates; the popup positioner maps this point
        // into the overlay with mapToItem, which applies every ancestor
        // transform - the web scale of TopNav and the scroll offset of the web
        // viewport included - so it is the field's visual bottom-left corner at
        // any scale.
        x: -calendarButton.x
        y: control.height - calendarButton.y + 4
        // Web scaling (App.qml): the overlay is not scaled, so the popup uses the
        // page's scale itself (popupScale = TopNav root's scale); TopLeft keeps
        // the scaled popup's top-left corner on the mapped point above, so it
        // stays attached to the field and grows/shrinks like the page.
        // Desktop: the scale is 1, so the origin makes no difference there (no
        // platform check needed; a dropdown's natural origin is TopLeft anyway).
        scale: control.popupScale
        transformOrigin: Popup.TopLeft
        padding: 14
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

        background: Rectangle {
            radius: 8
            color: control.panelColor
            border.color: control.borderColor
            border.width: 1
        }

        Column {
            spacing: 12

            Row {
                spacing: 14

                // ---- Calendar -------------------------------------------------
                Column {
                    id: calendarColumn
                    spacing: 6

                    Item {
                        width: control.cellWidth * 7
                        height: 34

                        Button {
                            id: previousMonthButton
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            width: 34
                            height: 34
                            hoverEnabled: true
                            focusPolicy: Qt.NoFocus
                            background: Rectangle {
                                radius: 6
                                color: previousMonthButton.hovered ? control.hoverColor : "transparent"
                            }
                            contentItem: Text {
                                text: "‹"
                                color: control.textColor
                                font.pixelSize: 22
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: control.showMonth(-1)
                        }

                        Text {
                            anchors.centerIn: parent
                            text: control.viewYear + " 年 " + (control.viewMonth + 1) + " 月"
                            color: control.textColor
                            font.pixelSize: 16
                            font.bold: true
                        }

                        Button {
                            id: nextMonthButton
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            width: 34
                            height: 34
                            hoverEnabled: true
                            focusPolicy: Qt.NoFocus
                            background: Rectangle {
                                radius: 6
                                color: nextMonthButton.hovered ? control.hoverColor : "transparent"
                            }
                            contentItem: Text {
                                text: "›"
                                color: control.textColor
                                font.pixelSize: 22
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: control.showMonth(1)
                        }
                    }

                    DayOfWeekRow {
                        locale: control.calendarLocale
                        padding: 0
                        spacing: 0
                        delegate: Text {
                            required property int day
                            width: control.cellWidth
                            height: 26
                            text: control.weekdayLabels[day]
                            color: day === Qt.Sunday || day === Qt.Saturday ? control.mutedTextColor
                                                                              : control.textColor
                            font.pixelSize: 13
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }

                    MonthGrid {
                        id: monthGrid
                        month: control.viewMonth
                        year: control.viewYear
                        locale: control.calendarLocale
                        padding: 0
                        spacing: 0

                        // Clicks are handled per cell with the integer year/month/day
                        // roles (not onClicked(date)), so no Date/time-zone conversion
                        // can shift the picked day.
                        delegate: Rectangle {
                            id: dayCell
                            required property var model
                            readonly property bool inViewMonth: dayCell.model.month === monthGrid.month
                            readonly property bool isSelected: dayCell.model.year === control.selectedYear
                                                               && dayCell.model.month === control.selectedMonth
                                                               && dayCell.model.day === control.selectedDay
                            readonly property bool isToday: dayCell.model.year === control.todayYear
                                                            && dayCell.model.month === control.todayMonth
                                                            && dayCell.model.day === control.todayDay
                            width: control.cellWidth
                            height: control.cellHeight
                            radius: 6
                            color: dayCell.isSelected ? control.accentColor
                                 : dayMouse.containsMouse ? control.hoverColor : "transparent"
                            // Today: outlined; the selected day: filled (both when they coincide).
                            border.color: dayCell.isToday ? control.accentHoverColor : "transparent"
                            border.width: dayCell.isToday ? 2 : 0

                            Text {
                                anchors.centerIn: parent
                                text: dayCell.model.day
                                color: dayCell.isSelected ? "white"
                                     : dayCell.inViewMonth ? control.textColor : control.dimTextColor
                                font.pixelSize: 14
                                font.bold: dayCell.isSelected || dayCell.isToday
                            }

                            MouseArea {
                                id: dayMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: control.selectDay(dayCell.model.year, dayCell.model.month,
                                                             dayCell.model.day)
                            }
                        }
                    }
                }

                Rectangle {
                    width: 1
                    height: calendarColumn.height
                    color: control.borderColor
                }

                // ---- Time -----------------------------------------------------
                Column {
                    spacing: 6

                    Item {
                        width: timeLists.implicitWidth
                        height: 34

                        Text {
                            anchors.centerIn: parent
                            text: "時間"
                            color: control.textColor
                            font.pixelSize: 16
                            font.bold: true
                        }
                    }

                    Row {
                        spacing: timeLists.spacing

                        Repeater {
                            model: ["時", "分"]
                            delegate: Text {
                                required property string modelData
                                width: control.timeListWidth
                                height: 26
                                text: modelData
                                color: control.textColor
                                font.pixelSize: 13
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }

                    Row {
                        id: timeLists
                        spacing: 8

                        // Hour (00-23) and minute (00-59) lists: click a value to select it.
                        Repeater {
                            model: [24, 60]
                            delegate: ListView {
                                id: timeList
                                required property int index
                                required property int modelData
                                readonly property bool isHour: timeList.index === 0
                                readonly property int selectedValue: timeList.isHour ? control.selectedHour
                                                                                     : control.selectedMinute
                                width: control.timeListWidth
                                height: control.cellHeight * 6
                                model: timeList.modelData
                                clip: true
                                boundsBehavior: Flickable.StopAtBounds

                                ScrollBar.vertical: ScrollBar {
                                    policy: ScrollBar.AlwaysOn
                                    width: 6
                                    contentItem: Rectangle {
                                        implicitWidth: 4
                                        radius: 2
                                        color: control.dimTextColor
                                    }
                                    background: Item {}
                                }

                                delegate: Rectangle {
                                    id: timeCell
                                    required property int index
                                    readonly property bool isSelected: timeCell.index === timeList.selectedValue
                                    width: timeList.width - 10
                                    height: control.cellHeight
                                    radius: 6
                                    color: timeCell.isSelected ? control.accentColor
                                         : timeMouse.containsMouse ? control.hoverColor : "transparent"

                                    Text {
                                        anchors.centerIn: parent
                                        text: DateTimeUtil.pad2(timeCell.index)
                                        color: timeCell.isSelected ? "white" : control.textColor
                                        font.pixelSize: 15
                                        font.family: "Consolas"
                                        font.bold: timeCell.isSelected
                                    }

                                    MouseArea {
                                        id: timeMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        onClicked: {
                                            if (timeList.isHour)
                                                control.selectedHour = timeCell.index
                                            else
                                                control.selectedMinute = timeCell.index
                                        }
                                    }
                                }

                                // Show the selection when the popup opens (the lists
                                // are laid out only while the popup is shown).
                                Connections {
                                    target: picker
                                    function onOpened() {
                                        timeList.forceLayout()
                                        timeList.positionViewAtIndex(timeList.selectedValue, ListView.Center)
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: 1
                color: control.borderColor
            }

            Item {
                width: parent.width
                height: 34

                // The value 「確定」 will write into the field.
                Text {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: control.selectedText
                    color: control.accentHoverColor
                    font.pixelSize: 16
                    font.family: "Consolas"
                }

                Row {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8

                    Button {
                        id: cancelPickerButton
                        width: control.pickerButtonWidth
                        height: 34
                        hoverEnabled: true
                        focusPolicy: Qt.NoFocus

                        background: Rectangle {
                            radius: 6
                            color: cancelPickerButton.hovered ? control.hoverColor : "transparent"
                            border.color: control.borderColor
                            border.width: 1
                        }

                        contentItem: Text {
                            text: "取消"
                            color: control.textColor
                            font.pixelSize: 15
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        onClicked: picker.close()
                    }

                    Button {
                        id: confirmPickerButton
                        width: control.pickerButtonWidth
                        height: 34
                        hoverEnabled: true
                        focusPolicy: Qt.NoFocus

                        background: Rectangle {
                            radius: 6
                            color: confirmPickerButton.hovered ? control.accentHoverColor : control.accentColor
                        }

                        contentItem: Text {
                            text: "確定"
                            color: "white"
                            font.pixelSize: 15
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        onClicked: control.confirmPicker()
                    }
                }
            }
        }
    }
}
