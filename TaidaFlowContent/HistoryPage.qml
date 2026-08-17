import QtQuick
import QtQuick.Controls

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

    Text {
        anchors.centerIn: parent

        text: "歷史紀錄"

        color: root.textColor
        font.pixelSize: 36
        font.bold: true
    }
}
