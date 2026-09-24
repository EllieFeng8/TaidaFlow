import QtQuick
import QtQuick.Controls
import TaidaFlowBackend 1.0

Rectangle {
    id: root
    width: 1920
    height: 1080
    visible: true
    color: "#0F192D"
    property color mainBlue: "#0087DC"
    property color lightBlue: "#19B8FF"
    property color textColor: "#E8F4FF"
    property color borderColor: "#4CD7FF"
    property color panelColor: "#111D32"

    property int currentPage: 0
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
                    font.family: "Consolas"
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
                        root.currentPage = 0
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
                    font.family: "Consolas"
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
                        root.currentPage = 1
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
                    font.family: "Consolas"
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
                        root.currentPage = 2
                    }
                }
            }

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
    }
    AlarmPage{
        visible: root.currentPage === 1
    }
    HistoryPage{
        visible: root.currentPage === 2
    }

    // =========================================================
    // 離線提示 (transport overlay, wasm-mirror pack §9)
    // Td.transportReady is a local STORED false property: always true on the
    // desktop (banner never shown), false on WebAssembly while the desktop Core
    // is unreachable / synchronizing. All write controls are disabled meanwhile
    // (Main.qml controlsEnabled, HistoryPage paging); the page keeps the last
    // synchronized state.
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
        visible: !Td.transportReady

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
                    text: "離線"
                    color: "#B3261E"
                    font.pixelSize: 22
                    font.bold: true
                }
            }

            Column {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2

                Text {
                    text: "未連線到桌面端，所有操作已停用（畫面保留最後同步的狀態）"
                    color: "white"
                    font.pixelSize: 22
                    font.bold: true
                }

                Text {
                    text: Td.transportMessage
                    color: "#FFE0DC"
                    font.pixelSize: 15
                }
            }
        }
    }

}
