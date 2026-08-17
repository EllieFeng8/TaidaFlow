import QtQuick
import QtQuick.Controls
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

    Text {
        anchors.centerIn: parent

        text: "異常警告"

        color: "#FF5964"
        font.pixelSize: 36
        font.bold: true
    }
}
