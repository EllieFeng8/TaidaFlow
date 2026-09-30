pragma ComponentBehavior: Bound
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

    // 卡片網格：每個設定組一張卡片，依可用寬度換欄（≥ 1500 → 3 欄、≥ 1000 → 2 欄、其餘 1 欄）；
    // 卡片填滿可用寬度、欄寬平均、欄距一致，同一橫列的卡片等高（GridLayout 列高取該列最高者）。
    readonly property int pageMargin: 28
    readonly property int cardSpacing: 20
    readonly property real gridWidth: Math.max(0, settingsScroll.availableWidth - 2 * pageMargin)
    readonly property int cardColumns: gridWidth >= 1500 ? 3 : gridWidth >= 1000 ? 2 : 1
    readonly property real cardWidth: (gridWidth - (cardColumns - 1) * cardSpacing) / cardColumns

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

    // 內容超出高度時整頁捲動；捲軸落在右側頁邊內，不壓到卡片。
    ScrollView {
        id: settingsScroll
        objectName: "settingsScroll"
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        contentHeight: pageColumn.implicitHeight + 2 * settingsPage.pageMargin

        ColumnLayout {
            id: pageColumn
            objectName: "pageColumn"
            x: settingsPage.pageMargin
            y: settingsPage.pageMargin
            width: settingsPage.gridWidth
            spacing: 0
            // 標題列與說明：左右邊界與卡片網格相同。
            RowLayout {
                objectName: "pageHeader"
                Layout.fillWidth: true
                spacing: 12
                Label { text: "Sensor 設定"; color: "#E8F4FF"; font.pixelSize: 28; font.bold: true }
                Item { Layout.fillWidth: true }
                Label { text: "壓力單位"; color: "#E8F4FF"; font.pixelSize: 18 }
                ComboBox {
                    objectName: "pressureUnitBox"
                    model: ["kPa", "psi", "bar"]
                    currentIndex: model.indexOf(Td.pressureUnitSv)
                    enabled: Td.transportReady
                    onActivated: Td.pressureUnitSv = currentText
                    Layout.preferredWidth: 150
                }
            }
            Label {
                objectName: "pageDescription"
                text: "量測值加上 Offset 後顯示；上下限留空表示未啟用；同位置的感測器共用一組上下限，Offset 各自設定。每組修改後請按「套用」。"
                color: "#AFC5D8"
                font.pixelSize: 16
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                Layout.topMargin: 8
                Layout.bottomMargin: 18
            }
            GridLayout {
                id: cardGrid
                objectName: "cardGrid"
                columns: settingsPage.cardColumns
                columnSpacing: settingsPage.cardSpacing
                rowSpacing: settingsPage.cardSpacing
                Layout.fillWidth: true
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
                        {name: "Filter 壓差", note: "PT-02 − PT-03，含各自 Offset", sensors: [{id: "filter", label: "Filter ΔP"}]}
                    ]
                    // 每個設定組一張卡片：同位置的感測器共用一組上下限，Offset 每個成員一行。
                    Components.SensorSettingRow {
                        id: sensorCard
                        required property var modelData
                        objectName: "settings-" + sensorId
                        sensorId: modelData.sensors[0].id
                        label: modelData.sensors[0].label
                        members: modelData.sensors
                        title: modelData.name
                        note: modelData.note || ""
                        hasOffset: sensorId !== "filter"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.preferredWidth: settingsPage.cardWidth
                        Layout.minimumWidth: settingsPage.cardWidth
                        Layout.maximumWidth: settingsPage.cardWidth
                        onDirtyChanged: settingsPage.trackChanges(sensorCard)
                    }
                }
            }
        }
    }
}
