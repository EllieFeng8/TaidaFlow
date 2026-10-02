import QtQuick
import "SensorUnits.js" as SensorUnits

Item {
    id: valueTag

    property string title: "PT-01"
    property string value: "2.5"
    property string unit: "kPa"
    property color tagColor: title.indexOf("TT-") === 0 ? "#FF9F43" : root.borderColor

    // w1-088: limit state of the shown value (SensorUnits.limitState: 1 above the
    // enabled upper limit, -1 below the enabled lower limit, 0 normal). A sensor tag
    // sets sensorId / raw / settings and the state follows the corrected raw value;
    // other tags (ΔP) bind limitState directly. Out of limit, the tag is filled with
    // the Filter's red / orange (border too) and its text turns white like the Filter
    // label: a filled box is visible at a glance, keeps the layout as it is, and is not
    // confused with the orange text of the TT tags.
    property string sensorId: ""
    property real raw: 0
    property var settings: ({})
    property int limitState: sensorId === "" ? 0 : SensorUnits.sensorLimitState(raw, sensorId, settings)
    readonly property color frameColor: SensorUnits.limitFillColor(limitState, "transparent")
    readonly property color frameBorderColor: SensorUnits.limitBorderColor(limitState, tagColor)
    readonly property color textColor: limitState === 0 ? tagColor : "white"

    width: 62
    height: 54

    Rectangle {
        objectName: "valueTagFrame"
        anchors.fill: parent
        color: valueTag.frameColor
        border.color: valueTag.frameBorderColor
        border.width: 1
        radius: 2
    }

    Text {
        objectName: "valueTagTitle"
        anchors.top: parent.top
        anchors.topMargin: 5
        anchors.horizontalCenter: parent.horizontalCenter
        text: parent.title
        color: valueTag.textColor
        font.pixelSize: 14
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
            objectName: "valueTagValue"

            // anchors.bottom: parent.bottom
            // anchors.bottomMargin: 2
            // anchors.horizontalCenter: parent.horizontalCenter

            width: Math.max(24, valueTag.width - unitField.implicitWidth - 6)
            height: 25

            text: parent.parent.value
            color: valueTag.textColor
            font.pixelSize: 18
            font.family: "Consolas"
            fontSizeMode: Text.Fit
            minimumPixelSize: 10

            horizontalAlignment: TextInput.AlignHCenter
            verticalAlignment: TextInput.AlignVCenter
        }
        Text {
            id: unitField
            objectName: "valueTagUnit"
            anchors.verticalCenter: parent.verticalCenter

            text: parent.parent.unit
            color: valueTag.textColor
            font.pixelSize: 8
        }
    }

}
