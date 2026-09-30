import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import TaidaFlowBackend 1.0
import "SensorUnits.js" as Units

ColumnLayout {
    id: row
    required property string sensorId
    required property string label
    property bool hasOffset: true
    property string editUnit: "kPa"
    property string errorText: ""
    property bool dirty: false
    readonly property real conversion: Units.factor(sensorId, editUnit)
    spacing: 4

    function reload() {
        var entry = Td.sensorSettingsSv[sensorId]
        if (!entry) return
        editUnit = Td.pressureUnitSv
        offsetField.text = Units.editNumber(entry.offset * conversion)
        lowerField.text = entry.lowerEnabled ? Units.editNumber(entry.lower * conversion) : ""
        upperField.text = entry.upperEnabled ? Units.editNumber(entry.upper * conversion) : ""
        errorText = ""
        dirty = false
    }
    function changeUnit() {
        var previousFactor = conversion
        editUnit = Td.pressureUnitSv
        for (var field of [offsetField, lowerField, upperField]) {
            var value = Units.parseNumber(field.text)
            if (isFinite(value)) field.text = Units.editNumber(value / previousFactor * conversion)
        }
    }
    function save() {
        var offset = hasOffset ? Units.parseNumber(offsetField.text) / conversion : 0
        var lowerEnabled = lowerField.text.trim() !== ""
        var upperEnabled = upperField.text.trim() !== ""
        var lower = lowerEnabled ? Units.parseNumber(lowerField.text) / conversion : 0
        var upper = upperEnabled ? Units.parseNumber(upperField.text) / conversion : 0
        if (!isFinite(offset) || !isFinite(lower) || !isFinite(upper)) {
            errorText = "請輸入有效數字；上下限可留空。"
            return
        }
        if (lowerEnabled && upperEnabled && lower > upper) {
            errorText = "下限不可大於上限。"
            return
        }
        var settings = JSON.parse(JSON.stringify(Td.sensorSettingsSv))
        settings[sensorId] = {offset: offset, lower: lower, upper: upper,
                              lowerEnabled: lowerEnabled, upperEnabled: upperEnabled}
        Td.sensorSettingsSv = settings
        dirty = false
        errorText = ""
    }
    Component.onCompleted: reload()
    Connections {
        target: Td
        function onPressureUnitSvChanged() { row.changeUnit() }
        function onSensorSettingsSvChanged() { if (!row.dirty) row.reload() }
        function onTransportReadyChanged() { if (!Td.transportReady) row.reload() }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 16
        Label {
            text: row.label
            color: row.sensorId.indexOf("tt") === 0 ? "#FF9F43" : "#4CD7FF"
            font.pixelSize: 18
            Layout.preferredWidth: 155
        }
        Label { text: Units.unit(row.sensorId, row.editUnit); color: "#AFC5D8"; Layout.preferredWidth: 75 }
        TextField {
            id: offsetField
            objectName: "offsetField"
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            enabled: Td.transportReady && row.hasOffset
            placeholderText: row.hasOffset ? "Offset" : "—"
            selectByMouse: true
            onTextEdited: row.dirty = true
        }
        TextField {
            id: lowerField
            objectName: "lowerField"
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            enabled: Td.transportReady
            placeholderText: "未設定下限"
            selectByMouse: true
            onTextEdited: row.dirty = true
        }
        TextField {
            id: upperField
            objectName: "upperField"
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            enabled: Td.transportReady
            placeholderText: "未設定上限"
            selectByMouse: true
            onTextEdited: row.dirty = true
        }
        Button { text: "套用"; Layout.preferredWidth: 84; enabled: Td.transportReady && row.dirty; onClicked: row.save() }
        Button { text: "還原"; Layout.preferredWidth: 84; enabled: row.dirty; onClicked: row.reload() }
    }
    Label { text: row.errorText; visible: text !== ""; color: "#FF8A80"; Layout.fillWidth: true }
}
