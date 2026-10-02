import QtQuick
import QtQuick.Controls
import TaidaFlowBackend 1.0
import "components" as Components

Rectangle {
    id: root
    width: 1920
    height: 1080
    visible: true
    color: "#0F192D"

    // ===== Link state (w1-083) ===============================================
    // webPage: WebPageControl from App/main.cpp, passed in by App.qml (null when TopNav is
    // loaded alone, e.g. Core/tests/Preview.qml: no heartbeat detection then).
    property var webPage: null
    // THE flag for every control that writes to the Core: transportReady (the pack's
    // offline state) AND, on the web page, no heartbeat silence (LinkWatchdog). Desktop:
    // equals Td.transportReady (always true). Each page gets it as its own `linkAlive`
    // property (see the page instances below) and uses it where it used Td.transportReady
    // for `enabled` (Main.qml: controlsEnabled).
    readonly property bool linkAlive: linkWatchdog.linkAlive

    Components.LinkWatchdog {
        id: linkWatchdog
        objectName: "linkWatchdog"
        backend: Td
        pageControl: root.webPage
    }
    // ===== end of link state ==================================================
    property color mainBlue: "#0087DC"
    property color lightBlue: "#19B8FF"
    property color textColor: "#E8F4FF"
    property color borderColor: "#4CD7FF"
    property color panelColor: "#111D32"

    property int currentPage: 0
    property int pendingPage: -1

    function navigateTo(page) {
        if (page === currentPage) return
        if (currentPage === 3 && sensorSettingsPage.hasUnsavedChanges) {
            pendingPage = page
            unsavedSettingsDialog.open()
            return
        }
        currentPage = page
    }
    // 0 = 主程式
    // 1 = 異常警告
    // 2 = 歷史紀錄
    property string currentDate: ""
    property string currentTime: ""
    Timer {
        interval: 1000
        running: true
        repeat: true
        triggeredOnStart: true

        onTriggered: {
            var now = new Date()

            root.currentDate = Qt.formatDate(now, "yyyy/MM/dd")
            root.currentTime = Qt.formatTime(now, "HH:mm:ss")
        }
    }
    // =========================================================
    // 上方導覽列
    // =========================================================
    Rectangle {
        id: topNavBar

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right

        height: 72
        z: 100

        color: "#111D32"

        // 底部分隔線
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom

            height: 1
            color: "#284766"
        }

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 24
            anchors.verticalCenter: parent.verticalCenter
            spacing: 20

            Image {
                anchors.verticalCenter: parent.verticalCenter
                width: 300
                height: 42
                source: "assets/logo_8TFs7.png"
                fillMode: Image.PreserveAspectFit
                smooth: true
            }

            Column {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                Text {
                    text: "Tel : +886-3487-1786"
                    color: root.textColor
                    font.pixelSize: 15
                }
            }
        }

        Row {
            anchors.centerIn: parent
            spacing: 15

            // =========================
            // 主程式
            // =========================
            Rectangle {
                width: 180
                height: 48
                radius: 6

                color: root.currentPage === 0
                       ? "#223D5A"
                       : mainProgramMouse.containsMouse
                         ? "#182C45"
                         : "transparent"

                border.color: root.currentPage === 0
                              ? root.mainBlue
                              : "transparent"

                border.width: 1

                Text {
                    anchors.centerIn: parent

                    text: "Main"
                    font.weight: Font.Bold
                    color: root.currentPage === 0
                           ? "#FFFFFF"
                           : "#AFC5D8"

                    font.pixelSize: 18
                    font.bold: root.currentPage === 0
                }

                // 選中下方藍線
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom

                    width: root.currentPage === 0 ? 100 : 0
                    height: 3

                    radius: 2
                    color: root.mainBlue

                    Behavior on width {
                        NumberAnimation { duration: 150 }
                    }
                }

                MouseArea {
                    id: mainProgramMouse

                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor

                    onClicked: {
                        root.navigateTo(0)
                    }
                }
            }

            // =========================
            // 異常警告
            // =========================
            Rectangle {
                width: 180
                height: 48
                radius: 6

                color: root.currentPage === 1
                       ? "#3A2932"
                       : alarmMouse.containsMouse
                         ? "#2A2632"
                         : "transparent"

                border.color: root.currentPage === 1
                              ? "#FF5964"
                              : "transparent"

                border.width: 1

                Text {
                    anchors.centerIn: parent

                    text: "Alarm"
                    font.weight: Font.Bold
                    color: root.currentPage === 1
                           ? "#FF7881"
                           : "#AFC5D8"

                    font.pixelSize: 18
                    font.bold: root.currentPage === 1
                }

                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom

                    width: root.currentPage === 1 ? 100 : 0
                    height: 3

                    radius: 2
                    color: "#FF5964"

                    Behavior on width {
                        NumberAnimation { duration: 150 }
                    }
                }

                MouseArea {
                    id: alarmMouse

                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor

                    onClicked: {
                        root.navigateTo(1)
                    }
                }
            }

            // =========================
            // 歷史紀錄
            // =========================
            Rectangle {
                width: 180
                height: 48
                radius: 6

                color: root.currentPage === 2
                       ? "#225A3D"
                       : historyMouse.containsMouse
                         ? "#182C45"
                         : "transparent"

                border.color: root.currentPage === 2
                              ? "#22C55E"
                              : "transparent"

                border.width: 1

                Text {
                    anchors.centerIn: parent

                    text: "History"
                    font.weight: Font.Bold
                    color: root.currentPage === 2
                           ? "#FFFFFF"
                           : "#AFC5D8"

                    font.pixelSize: 18
                    font.bold: root.currentPage === 2
                }

                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom

                    width: root.currentPage === 2 ? 100 : 0
                    height: 3

                    radius: 2
                    color: "#22C55E"

                    Behavior on width {
                        NumberAnimation { duration: 150 }
                    }
                }

                MouseArea {
                    id: historyMouse

                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor

                    onClicked: {
                        root.navigateTo(2)
                    }
                }
            }

        }
        Button {
            id: settingsNavButton
            anchors.right: parent.right
            anchors.rightMargin: 240
            anchors.verticalCenter: parent.verticalCenter
            width: 120
            height: 48
            text: "設定"
            highlighted: root.currentPage === 3
            background: Rectangle {
                radius: 6
                color: root.currentPage === 3 ? "#203F5D" : settingsNavButton.hovered ? "#182C45" : "transparent"
                border.width: 1
                border.color: root.currentPage === 3 ? root.mainBlue : "#284766"
            }
            contentItem: Text {
                text: settingsNavButton.text
                color: root.currentPage === 3 ? "white" : "#AFC5D8"
                font.pixelSize: 18
                font.bold: root.currentPage === 3
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: root.navigateTo(3)
        }
        // =========================
       // 右側時間
       // =========================
       Row {
           anchors.right: parent.right
           anchors.rightMargin: 35
           anchors.verticalCenter: parent.verticalCenter

           spacing: 12

           Image {
               id: time
               y:3
               source: "assets/schedule.png"
           }

           Column {
               anchors.verticalCenter: parent.verticalCenter

               Text {
                   text: root.currentDate

                   color: "#8FAFC8"
                   font.pixelSize: 12
                   font.family: "Consolas"
                }

               Text {
                   text: root.currentTime

                   color: "white"
                   font.pixelSize: 19
                   font.bold: true
                   font.family: "Consolas"
                }
            }
        }

    }
    Main{
        visible: root.currentPage === 0
        linkAlive: root.linkAlive
    }
    AlarmPage{
        visible: root.currentPage === 1
        linkAlive: root.linkAlive
    }
    HistoryPage{
        visible: root.currentPage === 2
        linkAlive: root.linkAlive
    }
    SettingsPage {
        id: sensorSettingsPage
        visible: root.currentPage === 3
        linkAlive: root.linkAlive
    }

    Dialog {
        id: unsavedSettingsDialog
        objectName: "unsavedSettingsDialog"
        parent: Overlay.overlay
        modal: true
        closePolicy: Popup.CloseOnEscape
        width: 540
        scale: root.scale
        transformOrigin: Item.TopLeft
        x: (parent.width - width * scale) / 2
        y: (parent.height - height * scale) / 2
        padding: 24
        background: Rectangle {
            color: "#111D32"
            border.color: "#FFD166"
            border.width: 1
            radius: 8
        }
        header: Label {
            text: "尚未保存"
            color: "#FFD166"
            font.pixelSize: 24
            font.bold: true
            padding: 24
            bottomPadding: 0
        }
        contentItem: Label {
            text: "設定已修改，但尚未按「套用」。\n要繼續編輯，還是放棄未保存的修改並離開？"
            color: "#E8F4FF"
            font.pixelSize: 18
            wrapMode: Text.WordWrap
        }
        footer: DialogButtonBox {
            padding: 16
            spacing: 12
            background: Rectangle { color: "transparent" }
            Button {
                text: "繼續編輯"
                implicitHeight: 44
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
                contentItem: Text { text: parent.text; font.pixelSize: 18; color: "#E8F4FF"; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                background: Rectangle { color: "#203F5D"; radius: 4 }
            }
            Button {
                text: "放棄修改並離開"
                implicitHeight: 44
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
                contentItem: Text { text: parent.text; font.pixelSize: 18; color: "#FFD166"; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                background: Rectangle { color: "#3B3022"; radius: 4 }
            }
        }
        onAccepted: {
            sensorSettingsPage.discardChanges()
            root.currentPage = root.pendingPage
        }
        onClosed: root.pendingPage = -1
    }

    // =========================================================
    // 離線提示 (transport overlay, wasm-mirror pack §9)
    // Td.transportReady is a local STORED false property: always true on the
    // desktop (banner never shown), false on WebAssembly while the desktop Core
    // is unreachable / synchronizing. All write controls are disabled meanwhile
    // (Main.qml controlsEnabled, HistoryPage paging); the page keeps the last
    // synchronized state.
    // w1-083: the banner also shows (with its own text) while the web page gets no
    // server heartbeat (LinkWatchdog: transportReady true, no change of
    // Td.serverHeartbeatMs for > 5 s); visible = !root.linkAlive, the same flag that
    // disables the controls. It disappears with the next heartbeat.
    // Layout: the banner never covers page content. It takes space only while it
    // is shown: AlarmPage / HistoryPage anchor their top to offlineBanner.bottom,
    // so the banner pushes them down (titles, filter bar and "下載 CSV" stay fully
    // visible); Main's first content row starts below the banner anyway (see the
    // note in Main.qml). Hidden -> height 0 -> the pages sit exactly at
    // topNavBar.bottom, i.e. the desktop layout is unchanged.
    // =========================================================
    Rectangle {
        id: offlineBanner
        objectName: "offlineBanner"

        anchors.top: topNavBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: visible ? 64 : 0
        z: 200
        visible: !root.linkAlive
        // true: heartbeat silence (half-open connection) rather than the pack's offline state.
        readonly property bool heartbeatLost: linkWatchdog.heartbeatStale

        // Opaque: the banner now sits in its own row (nothing is drawn behind it).
        color: "#B3261E"
        border.color: "#FF8A80"
        border.width: 2

        Row {
            anchors.centerIn: parent
            spacing: 18

            Rectangle {
                width: 84
                height: 36
                radius: 6
                anchors.verticalCenter: parent.verticalCenter
                color: "#FFFFFF"

                Text {
                    anchors.centerIn: parent
                    text: offlineBanner.heartbeatLost ? "中斷" : "離線"
                    color: "#B3261E"
                    font.pixelSize: 22
                    font.bold: true
                }
            }

            Column {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2

                Text {
                    objectName: "offlineBannerTitle"
                    text: offlineBanner.heartbeatLost
                          ? "連線中斷，正在恢復…所有操作已停用（畫面保留最後收到的狀態）"
                          : "未連線到桌面端，所有操作已停用（畫面保留最後同步的狀態）"
                    color: "white"
                    font.pixelSize: 22
                    font.bold: true
                }

                Text {
                    objectName: "offlineBannerDetail"
                    text: offlineBanner.heartbeatLost
                          ? "已 " + Math.floor(linkWatchdog.silentMs / 1000) + " 秒未收到桌面端心跳；"
                            + (linkWatchdog.reloadInMs > 0
                               ? "約 " + Math.ceil(linkWatchdog.reloadInMs / 1000) + " 秒後自動重新整理頁面"
                               : "正在重新整理頁面…")
                          : Td.transportMessage
                    color: "#FFE0DC"
                    font.pixelSize: 15
                }
            }
        }
    }

}
