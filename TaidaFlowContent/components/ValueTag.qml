import QtQuick

Item {
    id: valueTag

    property string title: "PT-01"
    property string value: "2.5"
    property string unit: "kPa"
    property color tagColor: title.indexOf("TT-") === 0 ? "#FF9F43" : root.borderColor

    width: 62
    height: 54

    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.color: valueTag.tagColor
        border.width: 1
        radius: 2
    }

    Text {
        anchors.top: parent.top
        anchors.topMargin: 5
        anchors.horizontalCenter: parent.horizontalCenter
        text: parent.title
        color: valueTag.tagColor
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

            width: Math.max(24, valueTag.width - unitField.implicitWidth - 6)
            height: 25

            text: parent.parent.value
            color: valueTag.tagColor
            font.pixelSize: 18
            font.family: "Consolas"
            fontSizeMode: Text.Fit
            minimumPixelSize: 10

            horizontalAlignment: TextInput.AlignHCenter
            verticalAlignment: TextInput.AlignVCenter
        }
        Text {
            id: unitField
            anchors.verticalCenter: parent.verticalCenter

            text: parent.parent.unit
            color: valueTag.tagColor
            font.pixelSize: 8
            font.family: "Consolas"
        }
    }

}
