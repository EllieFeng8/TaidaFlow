import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Universal
import QtQuick.Layouts
import TaidaFlowBackend 1.0
import "components" as Components

Pane {
    id: settingsPage
    padding: 0
    Universal.theme: Universal.Dark
    Universal.accent: "#4CD7FF"
    background: Rectangle { color: "#0F192D" }
    anchors.top: offlineBanner.bottom
    anchors.left: parent.left
    anchors.right: parent.right
    anchors.bottom: parent.bottom

    property var pendingRows: ({})
    readonly property bool hasUnsavedChanges: Object.keys(pendingRows).length > 0

    function trackChanges(row) {
        var next = Object.assign({}, pendingRows)
        if (row.dirty) next[row.sensorId] = row
        else delete next[row.sensorId]
        pendingRows = next
    }

    function discardChanges() {
        var rows = pendingRows
        for (var key of Object.keys(rows)) rows[key].reload()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 32
        spacing: 16
        RowLayout {
            Layout.fillWidth: true
            Label { text: "Sensor 設定"; color: "#E8F4FF"; font.pixelSize: 28; font.bold: true }
            Item { Layout.fillWidth: true }
            Label { text: "壓力單位"; color: "#E8F4FF"; font.pixelSize: 18 }
            ComboBox {
                model: ["kPa", "psi", "bar"]
                currentIndex: model.indexOf(Td.pressureUnitSv)
                enabled: Td.transportReady
                onActivated: Td.pressureUnitSv = currentText
                Layout.preferredWidth: 150
            }
        }
        Label {
            text: "量測值加上 Offset 後顯示；上下限留空表示未啟用。每列修改後請按「套用」。"
            color: "#AFC5D8"
            font.pixelSize: 16
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 16
            Label { text: "Sensor"; color: "#AFC5D8"; Layout.preferredWidth: 155 }
            Label { text: "單位"; color: "#AFC5D8"; Layout.preferredWidth: 75 }
            Repeater {
                model: ["Offset", "下限", "上限"]
                Label { required property string modelData; text: modelData; color: "#AFC5D8"; Layout.fillWidth: true; Layout.preferredWidth: 1 }
            }
            Item { Layout.preferredWidth: 184 }
        }
        ScrollView {
            id: settingsScroll
            objectName: "settingsScroll"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth
            ColumnLayout {
                width: settingsScroll.availableWidth
                spacing: 18
                Repeater {
                    model: [
                        {name: "設備接口出口壓力", sensors: [{id: "pt04", label: "PT-04"}, {id: "pt05", label: "PT-05"}]},
                        {name: "設備接口出口溫度", sensors: [{id: "tt01", label: "TT-01"}, {id: "tt02", label: "TT-02"}]},
                        {name: "設備接口入口壓力", sensors: [{id: "pt06", label: "PT-06"}, {id: "pt07", label: "PT-07"}]},
                        {name: "設備接口入口溫度", sensors: [{id: "tt03", label: "TT-03"}, {id: "tt04", label: "TT-04"}]},
                        {name: "PT-01", sensors: [{id: "pt01", label: "PT-01"}]},
                        {name: "PT-02", sensors: [{id: "pt02", label: "PT-02"}]},
                        {name: "PT-03", sensors: [{id: "pt03", label: "PT-03"}]},
                        {name: "流量計", sensors: [{id: "flowMeter", label: "FM-01"}]},
                        {name: "Filter 壓差（PT-02 − PT-03，含各自 Offset）", sensors: [{id: "filter", label: "Filter ΔP"}]}
                    ]
                    ColumnLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 8
                        Label { text: modelData.name; color: "#E8F4FF"; font.pixelSize: 20; font.bold: true }
                        Repeater {
                            model: modelData.sensors
                            Components.SensorSettingRow {
                                objectName: "settings-" + sensorId
                                required property var modelData
                                Layout.fillWidth: true
                                sensorId: modelData.id
                                label: modelData.label
                                hasOffset: sensorId !== "filter"
                                onDirtyChanged: settingsPage.trackChanges(this)
                            }
                        }
                        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: "#284766" }
                    }
                }
            }
        }
    }
}
