import QtQuick
import QtQuick.Controls

Rectangle {
    id:interfaceBox
    property string label: "測試設備接口"

    width: 120
    height: 86
    radius: 12

    color: root.panelColor
    border.color: root.mainBlue
    border.width: 3

    Text {
        anchors.centerIn: parent
        text: label
        color: root.textColor
        font.pixelSize: 15
        font.bold: true
    }
}
