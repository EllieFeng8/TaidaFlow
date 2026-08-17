import QtQuick

Item {
    id: valueTag

    property string title: "PT-01"
    property string value: "2.5"
    property string unit: "Pa"

    width: 62
    height: 54

    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.color: root.borderColor
        border.width: 1
        radius: 2
    }

    Text {
        anchors.top: parent.top
        anchors.topMargin: 5
        anchors.horizontalCenter: parent.horizontalCenter
        text: parent.title
        color: root.textColor
        font.pixelSize: 14
        font.family: "Consolas"
    }

    Rectangle {
        width: parent.width - 2
        height: 1
        anchors.horizontalCenter: parent.horizontalCenter
        y: 27
        color: "#36536A"
    }
    Row {
           anchors.bottom: parent.bottom
           anchors.bottomMargin: 2
           anchors.horizontalCenter: parent.horizontalCenter

           spacing: 2
        Text {
            id: valueField

            // anchors.bottom: parent.bottom
            // anchors.bottomMargin: 2
            // anchors.horizontalCenter: parent.horizontalCenter

            width: 42
            height: 25

            text: parent.parent.value
            color: root.mainBlue
            font.pixelSize: 18
            font.family: "Consolas"

            horizontalAlignment: TextInput.AlignHCenter
            verticalAlignment: TextInput.AlignVCenter
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter

            text: parent.parent.unit
            color: root.textColor
            font.pixelSize: 8
            font.family: "Consolas"
        }
    }

}
