pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import TaidaFlowBackend 1.0
import "SensorUnits.js" as Units

// 設定頁的一張卡片（一個設定組）。檔名沿用 SensorSettingRow（Core/tests/tst_sensor_settings 以此路徑載入）。
// members 為同位置的一組感測器（例如 PT-04 / PT-05）：組內共用一組上下限（套用時每個成員寫入
// 相同的 lower / upper / 啟用旗標），Offset 則每個成員各自一格、各自寫入。單一感測器時 members
// 只有 sensorId 自己。
// 版面（由上而下）：標題列（組名 + 單位小標籤）｜每個成員一行「<成員> Offset [輸入框]」｜
// 下限｜上限｜（不一致提示 / 錯誤訊息）｜右下「套用」「還原」。
// 所有輸入框左緣對齊、同寬；標籤欄固定寬。卡片高度由 SettingsPage 的網格決定（同一橫列等高），
// 多出來的高度留在欄位與按鈕之間，按鈕固定在卡片底部。
Rectangle {
    id: row
    required property string sensorId
    required property string label
    property var members: [{id: sensorId, label: label}]
    // 卡片標題；單一感測器時預設就是自己的名稱（只顯示一次）。
    property string title: label
    // 取代 Offset 行的小字說明（Filter 用）。
    property string note: ""
    property bool hasOffset: true
    // w1-083: SettingsPage passes its linkAlive (TopNav root.linkAlive); the edit controls use
    // it instead of Td.transportReady. Default true when the card is loaded alone.
    property bool linkAlive: true
    property string editUnit: "kPa"
    property string errorText: ""
    property bool dirty: false
    // 組內成員目前的上下限不一致（舊資料或其他端寫入）；畫面顯示第一個成員的值。
    property bool limitsMismatch: false
    readonly property var memberIds: members.map(member => member.id)
    readonly property real conversion: Units.factor(sensorId, editUnit)

    // 卡片內尺寸：標籤欄固定寬，輸入框依卡片寬度伸縮、上限 maxFieldWidth。
    property int cardPadding: 18
    property int labelColumnWidth: 116
    property int labelSpacing: 12
    property int maxFieldWidth: 480
    property int lineSpacing: 10
    property int buttonWidth: 84
    property int headerHeight: 28
    readonly property int innerWidth: Math.max(0, width - 2 * cardPadding)
    readonly property int fieldWidth: Math.max(80, Math.min(maxFieldWidth, innerWidth - labelColumnWidth - labelSpacing))

    implicitWidth: 460
    implicitHeight: content.implicitHeight + 2 * cardPadding
    color: "#111D32"
    border.color: "#284766"
    border.width: 1
    radius: 8

    // 依 members 順序回傳各成員的 Offset 欄位。
    function offsetFields() {
        var fields = [offsetField]
        for (var i = 0; i < extraOffsets.count; ++i) {
            var cell = extraOffsets.itemAt(i) as MemberOffset
            if (cell) fields.push(cell.field)
        }
        return fields
    }
    function offsetFieldFor(id) {
        var index = memberIds.indexOf(id)
        return index < 0 ? null : offsetFields()[index] || null
    }
    // 只比較實際生效的部分：啟用旗標相同，且啟用中的上下限數值相同。
    function sameLimits(a, b) {
        return a.lowerEnabled === b.lowerEnabled && a.upperEnabled === b.upperEnabled
            && (!a.lowerEnabled || a.lower === b.lower)
            && (!a.upperEnabled || a.upper === b.upper)
    }
    function memberColor(id) { return id.indexOf("tt") === 0 ? "#FF9F43" : "#4CD7FF" }
    function reload() {
        var settings = Td.sensorSettingsSv
        var entries = []
        for (var id of memberIds) {
            if (!settings[id]) return
            entries.push(settings[id])
        }
        var fields = offsetFields()
        if (fields.length !== entries.length) return
        editUnit = Td.pressureUnitSv
        // 沒有 Offset 的卡片（Filter）欄位隱藏、內容留空。
        for (var i = 0; i < fields.length; ++i)
            fields[i].text = hasOffset ? Units.editNumber(entries[i].offset * conversion) : ""
        var first = entries[0]
        lowerField.text = first.lowerEnabled ? Units.editNumber(first.lower * conversion) : ""
        upperField.text = first.upperEnabled ? Units.editNumber(first.upper * conversion) : ""
        limitsMismatch = entries.some(entry => !row.sameLimits(first, entry))
        errorText = ""
        dirty = false
    }
    function changeUnit() {
        var previousFactor = conversion
        editUnit = Td.pressureUnitSv
        for (var field of offsetFields().concat([lowerField, upperField])) {
            var value = Units.parseNumber(field.text)
            if (isFinite(value)) field.text = Units.editNumber(value / previousFactor * conversion)
        }
    }
    function save() {
        var fields = offsetFields()
        if (fields.length !== memberIds.length) return
        var offsets = fields.map(field => row.hasOffset ? Units.parseNumber(field.text) / row.conversion : 0)
        var lowerEnabled = lowerField.text.trim() !== ""
        var upperEnabled = upperField.text.trim() !== ""
        var lower = lowerEnabled ? Units.parseNumber(lowerField.text) / conversion : 0
        var upper = upperEnabled ? Units.parseNumber(upperField.text) / conversion : 0
        if (offsets.some(offset => !isFinite(offset)) || !isFinite(lower) || !isFinite(upper)) {
            errorText = "請輸入有效數字；上下限可留空。"
            return
        }
        if (lowerEnabled && upperEnabled && lower > upper) {
            errorText = "下限不可大於上限。"
            return
        }
        var settings = JSON.parse(JSON.stringify(Td.sensorSettingsSv))
        for (var i = 0; i < memberIds.length; ++i)
            settings[memberIds[i]] = {offset: offsets[i], lower: lower, upper: upper,
                                      lowerEnabled: lowerEnabled, upperEnabled: upperEnabled}
        Td.sensorSettingsSv = settings
        dirty = false
        limitsMismatch = false
        errorText = ""
    }
    Component.onCompleted: reload()
    Connections {
        target: Td
        function onPressureUnitSvChanged() { row.changeUnit() }
        function onSensorSettingsSvChanged() { if (!row.dirty) row.reload() }
        function onTransportReadyChanged() { if (!Td.transportReady) row.reload() }
    }

    // 欄位行的左側標籤欄文字。
    component FieldCaption: Label {
        color: "#AFC5D8"
        font.pixelSize: 15
    }
    // 組內第二個以後的成員：一行「<成員> Offset [輸入框]」（第一個成員寫在下方 offsetLine）。
    component MemberOffset: RowLayout {
        id: memberCell
        required property var modelData
        readonly property alias field: memberField
        spacing: row.labelSpacing
        Layout.fillWidth: false
        RowLayout {
            spacing: 6
            Layout.fillWidth: false
            Layout.preferredWidth: row.labelColumnWidth
            Label {
                objectName: "memberLabel-" + memberCell.modelData.id
                text: memberCell.modelData.label
                color: row.memberColor(memberCell.modelData.id)
                font.pixelSize: 15
            }
            FieldCaption { text: "Offset" }
            Item { Layout.fillWidth: true }
        }
        TextField {
            id: memberField
            objectName: "offsetField-" + memberCell.modelData.id
            Layout.preferredWidth: row.fieldWidth
            enabled: row.linkAlive && row.hasOffset
            placeholderText: "Offset"
            selectByMouse: true
            onTextEdited: row.dirty = true
        }
    }

    ColumnLayout {
        id: content
        objectName: "cardContent"
        anchors.fill: parent
        anchors.margins: row.cardPadding
        spacing: 0

        // 標題列：組名（單一感測器就是感測器名稱，只出現一次）+ 右側單位小標籤。
        RowLayout {
            objectName: "cardHeader"
            spacing: 12
            Layout.fillWidth: true
            // 固定高度：中英文標題的字型行高不同，固定後同一橫列卡片的分隔線與欄位 y 一致。
            Layout.preferredHeight: row.headerHeight
            Layout.fillHeight: false
            Label {
                objectName: "titleLabel"
                text: row.title
                color: "#E8F4FF"
                font.pixelSize: 18
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
            Rectangle {
                objectName: "unitChip"
                implicitWidth: unitLabel.implicitWidth + 16
                implicitHeight: unitLabel.implicitHeight + 4
                radius: height / 2
                color: "#182C45"
                border.color: "#3A5F85"
                border.width: 1
                Label {
                    id: unitLabel
                    objectName: "unitLabel"
                    anchors.centerIn: parent
                    text: Units.unit(row.sensorId, row.editUnit)
                    color: "#AFC5D8"
                    font.pixelSize: 13
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: 8
            Layout.bottomMargin: 10
            implicitHeight: 1
            color: "#284766"
        }

        // 欄位：標籤欄固定寬、輸入框左緣對齊且同寬。
        ColumnLayout {
            objectName: "fieldsColumn"
            spacing: row.lineSpacing
            Layout.fillWidth: true
            Layout.fillHeight: false
            RowLayout {
                objectName: "offsetLine"
                visible: row.hasOffset
                spacing: row.labelSpacing
                Layout.fillWidth: false
                RowLayout {
                    spacing: 6
                    Layout.fillWidth: false
                    Layout.preferredWidth: row.labelColumnWidth
                    Label {
                        objectName: "memberLabel-" + row.sensorId
                        // 單一感測器：卡片標題就是名稱，這裡不重複。
                        text: row.members.length > 1 ? row.members[0].label : ""
                        visible: text !== ""
                        color: row.memberColor(row.sensorId)
                        font.pixelSize: 15
                    }
                    FieldCaption { text: "Offset" }
                    Item { Layout.fillWidth: true }
                }
                TextField {
                    id: offsetField
                    objectName: "offsetField"
                    Layout.preferredWidth: row.fieldWidth
                    enabled: row.linkAlive && row.hasOffset
                    placeholderText: "Offset"
                    selectByMouse: true
                    onTextEdited: row.dirty = true
                }
            }
            Repeater {
                id: extraOffsets
                model: row.members.slice(1)
                delegate: MemberOffset {}
                onItemAdded: if (!row.dirty) row.reload()
            }
            // Filter：沒有 Offset 行，改以小字說明計算方式。
            Label {
                objectName: "noteLabel"
                text: row.note
                visible: text !== ""
                color: "#8FA7BF"
                font.pixelSize: 13
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(implicitHeight, offsetField.implicitHeight)
                verticalAlignment: Text.AlignVCenter
            }
            RowLayout {
                objectName: "lowerLine"
                spacing: row.labelSpacing
                Layout.fillWidth: false
                FieldCaption { text: "下限"; Layout.preferredWidth: row.labelColumnWidth }
                TextField {
                    id: lowerField
                    objectName: "lowerField"
                    Layout.preferredWidth: row.fieldWidth
                    enabled: row.linkAlive
                    placeholderText: "未設定下限"
                    selectByMouse: true
                    onTextEdited: row.dirty = true
                }
            }
            RowLayout {
                objectName: "upperLine"
                spacing: row.labelSpacing
                Layout.fillWidth: false
                FieldCaption { text: "上限"; Layout.preferredWidth: row.labelColumnWidth }
                TextField {
                    id: upperField
                    objectName: "upperField"
                    Layout.preferredWidth: row.fieldWidth
                    enabled: row.linkAlive
                    placeholderText: "未設定上限"
                    selectByMouse: true
                    onTextEdited: row.dirty = true
                }
            }
        }

        // 同一橫列等高時多出的高度放在這裡，按鈕固定在卡片底部。
        Item { Layout.fillHeight: true; Layout.minimumHeight: 16 }

        // 不一致提示與錯誤訊息：在按鈕上方，只有出現時才佔高度（該橫列的卡片一起增高）。
        Label {
            objectName: "limitsMismatchHint"
            text: "組內上下限不一致，套用後將統一"
            visible: row.limitsMismatch
            color: "#FFB347"
            font.pixelSize: 14
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            Layout.bottomMargin: 10
        }
        Label {
            objectName: "errorLabel"
            text: row.errorText
            visible: text !== ""
            color: "#FF8A80"
            font.pixelSize: 14
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            Layout.bottomMargin: 10
        }

        RowLayout {
            objectName: "buttonsCell"
            spacing: 8
            Layout.fillWidth: true
            Layout.fillHeight: false
            Item { Layout.fillWidth: true }
            Button {
                objectName: "applyButton"
                text: "套用"
                Layout.preferredWidth: row.buttonWidth
                enabled: row.linkAlive && (row.dirty || row.limitsMismatch)
                onClicked: row.save()
            }
            Button {
                objectName: "revertButton"
                text: "還原"
                Layout.preferredWidth: row.buttonWidth
                enabled: row.dirty
                onClicked: row.reload()
            }
        }
    }
}
