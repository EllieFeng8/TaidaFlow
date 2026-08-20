import QtQuick
import QtQuick.Controls
import "components" as Components
import Core 1.0
Item {
    id: mainPage

    anchors.top: topNavBar.bottom
    anchors.left: parent.left
    anchors.right: parent.right
    anchors.bottom: parent.bottom
    anchors.topMargin: -70

    scale: 1.5
    visible: root.currentPage === 0
    transformOrigin: Item.TopLeft

    // Proxy updates the simulated/read-only PV values. This timer only
    // repaints the canvas so pipe colors follow the latest Proxy values.
    Timer {
        id: systemTestTimer
        interval: 300
        repeat: true
        running: true
        onTriggered: pipes.requestPaint()
    }

    // =========================================================
    // 管線
    // =========================================================
    Canvas {
        id: pipes
        anchors.fill: parent

        property real dashOffset: 0
        property int dashLength: 14
        property int gapLength: 8
        property color flowColor: "#19B8FF"
        // FM > 0 才流動
        property bool flowRunning: flowMeter.value > 0
        // 灰色停止狀態
        property color stoppedColor: "#5F6875"

        Timer {
            interval: 60
            running: pipes.flowRunning
            repeat: true
            onTriggered: {
                // 如果方向反了，就改成 += 2
                pipes.dashOffset += 2
                pipes.requestPaint()
            }
        }
        // FM 從 >0 變成 0 時，也立即重新畫成灰色
        onFlowRunningChanged: {
            requestPaint()
        }

        function temperatureColor(temp) {

            var minTemp = 20
            var maxTemp = 40

            // 限制範圍
            temp = Math.max(minTemp, Math.min(maxTemp, temp))

            var ratio = (temp - minTemp) / (maxTemp - minTemp)

            // ==============================
            // 科技感顏色階段
            // ==============================
            var colors = [
                { r: 0,   g: 135, b: 220 },  // #0087DC 主藍
                { r: 0,   g: 210, b: 255 },  // #00D2FF 電光青
                { r: 0,   g: 255, b: 220 },  // #00FFDC 青綠光
                { r: 120, g: 80,  b: 255 },  // #7850FF 電光紫
                { r: 255, g: 60,  b: 180 },  // #FF3CB4 桃紅
                { r: 255, g: 70,  b: 80 }    // #FF4650 警示紅
            ]

            var segmentCount = colors.length - 1

            var scaled = ratio * segmentCount

            var index = Math.floor(scaled)

            if (index >= segmentCount)
                index = segmentCount - 1

            var localRatio = scaled - index

            var c1 = colors[index]
            var c2 = colors[index + 1]

            var r = Math.round(
                        c1.r +
                        (c2.r - c1.r) * localRatio
                    )

            var g = Math.round(
                        c1.g +
                        (c2.g - c1.g) * localRatio
                    )

            var b = Math.round(
                        c1.b +
                        (c2.b - c1.b) * localRatio
                    )

            return Qt.rgba(
                        r / 255,
                        g / 255,
                        b / 255,
                        1
                    )
        }

        function line(ctx, x1, y1, x2, y2) {
            ctx.beginPath()
            ctx.moveTo(x1, y1)
            ctx.lineTo(x2, y2)
            ctx.stroke()
        }

        function polyline(ctx, points) {
            ctx.beginPath()
            ctx.moveTo(points[0][0], points[0][1])

            for (var i = 1; i < points.length; ++i)
                ctx.lineTo(points[i][0], points[i][1])

            ctx.stroke()
        }

        function arrowLeft(ctx, x, y) {
            ctx.beginPath()
            ctx.moveTo(x, y)
            ctx.lineTo(x + 22, y - 13)
            ctx.lineTo(x + 22, y + 13)
            ctx.closePath()
            ctx.fill()
        }

        function arrowRight(ctx, x, y) {
            ctx.beginPath()
            ctx.moveTo(x, y)
            ctx.lineTo(x - 22, y - 13)
            ctx.lineTo(x - 22, y + 13)
            ctx.closePath()
            ctx.fill()
        }

        function drawDashedSegment(ctx, x1, y1, x2, y2, offset) {
            var dx = x2 - x1
            var dy = y2 - y1
            var len = Math.sqrt(dx * dx + dy * dy)

            if (len <= 0)
                return

            var ux = dx / len
            var uy = dy / len
            var period = pipes.dashLength + pipes.gapLength

            var start = (-offset) % period
            if (start < 0)
                start += period

            for (var d = -start; d < len; d += period) {
                var s = Math.max(d, 0)
                var e = Math.min(d + pipes.dashLength, len)

                if (e > 0 && s < len) {
                    ctx.beginPath()
                    ctx.moveTo(x1 + ux * s, y1 + uy * s)
                    ctx.lineTo(x1 + ux * e, y1 + uy * e)
                    ctx.stroke()
                }
            }
        }

        function drawDashedPolyline(ctx, points, offset) {
            var accumulated = 0

            for (var i = 0; i < points.length - 1; ++i) {
                var x1 = points[i][0]
                var y1 = points[i][1]
                var x2 = points[i + 1][0]
                var y2 = points[i + 1][1]

                drawDashedSegment(ctx, x1, y1, x2, y2, offset + accumulated)

                var dx = x2 - x1
                var dy = y2 - y1
                accumulated += Math.sqrt(dx * dx + dy * dy)
            }
        }

        onPaint: {
            var ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)

            // =====================================================
            // 基本線條樣式
            // =====================================================
            ctx.strokeStyle = root.mainBlue
            ctx.fillStyle = root.mainBlue
            ctx.lineWidth = 4
            ctx.lineCap = "round"
            ctx.lineJoin = "round"

            // =====================================================
            // 左半邊：維持原本實線 + 箭頭
            // =====================================================

            // 左側主流程
            polyline(ctx, [
                [68, 409],
                [201, 409]
            ])
            arrowRight(ctx, 200, 409)

            // 泵浦 -> M3 -> 主槽
            polyline(ctx, [
                [274, 385],
                [284, 385],
                [284, 409],
                [480, 409]
            ])
            arrowRight(ctx, 485, 409)

            // M2 上迴路
            polyline(ctx, [
                [316, 409],
                [316, 302],
                [168, 302]
            ])
            arrowLeft(ctx, 147, 302)

            // M4 下迴路
            polyline(ctx, [
                [123, 410],
                [123, 588],
                [488, 588]
            ])
            arrowLeft(ctx, 326, 588)

            // =====================================================
            // 右半邊：改成虛線逆時針流動動畫
            // =====================================================
            ctx.strokeStyle = pipes.flowRunning
                    ? pipes.flowColor
                    : pipes.stoppedColor
            ctx.fillStyle = pipes.flowColor
            ctx.lineWidth = 3
            // 下方：主槽底部 -> 泵浦2
            drawDashedPolyline(ctx, [
                [542, 586],
                [592, 586]
            ], pipes.dashOffset + 220)

            // 下方：泵浦2 -> filter 前
            drawDashedPolyline(ctx, [
                [666, 560],
                [676, 560],
                [676, 586],
                [818, 586]
            ], pipes.dashOffset + 280)

            // 下方：filter -> FM
            drawDashedPolyline(ctx, [
                [858, 586],
                [1074, 586]
            ], pipes.dashOffset + 380)

            // 右側往上：filter 後 -> Air Separator
            drawDashedPolyline(ctx, [
                [989, 586],
                [989, 390],
                [894, 390]
            ], pipes.dashOffset + 520)

            // Air Separator -> valve（往左）
            drawDashedPolyline(ctx, [
                [804, 390],
                [708, 390]
            ], pipes.dashOffset + 680)

            // valve down（往下）
            drawDashedPolyline(ctx, [
                [708, 407],
                [708, 586]
            ], pipes.dashOffset + 760)

            // // 上方：加熱器接口 -> 測試設備（往左）TT03
            var ttBottomMax = Math.max(
                        Number(tt03.value),
                        Number(tt04.value)
                    )

            ctx.strokeStyle = pipes.flowRunning
                    ? pipes.temperatureColor(ttBottomMax)
                    : pipes.stoppedColor
            drawDashedPolyline(ctx, [
                [1105, 520],
                [1105, 181],
                [900, 181]
            ], pipes.dashOffset)


            // 上方：測試設備 -> 主槽頂部（往左再往下）TT01
            var ttTopMax = Math.max(
                        Number(tt01.value),
                        Number(tt02.value)
                    )

            ctx.strokeStyle = pipes.flowRunning
                    ? pipes.temperatureColor(ttTopMax)
                    : pipes.stoppedColor

            drawDashedPolyline(ctx, [
                [782, 181],
                [515, 181],
                [515, 338]
            ], pipes.dashOffset + 120)

            // =====================================================
            // leakage sensor
            // =====================================================
            ctx.strokeStyle = "#6864FF"
            ctx.lineWidth = 6
            line(ctx, 483, 680, 729, 680)
        }
    }
    // =========================================================
    // 左側馬達
    // =========================================================

    Components.MotorIcon {
        x: 30
        y: 374
        motorName: "M1"
        value: String(Td.m1ValueSv)
        onValueEdited: function(newValue) { Td.m1ValueSv = newValue }
    }

    Components.MotorIcon {
        x: 195
        y: 259
        motorName: "M2"
        value: String(Td.m2ValueSv)
        onValueEdited: function(newValue) { Td.m2ValueSv = newValue }
    }

    Components.MotorIcon {
        x: 350
        y: 369
        motorName: "M3"
        value: String(Td.m3ValueSv)
        onValueEdited: function(newValue) { Td.m3ValueSv = newValue }
    }

    Components.MotorIcon {
        x: 85
        y: 474
        motorName: "M4"
        value: String(Td.m4ValueSv)
        onValueEdited: function(newValue) { Td.m4ValueSv = newValue }
    }

    // =========================================================
    // 左側泵浦
    // =========================================================
    Item {
        x: 197
        y: 369
        width: 76
        height: 74


        Image {
            id: motor1
            x: -48
            y: -50
            scale: motorMouse.containsMouse ? 0.43 : 0.4
            source: "assets/motor2.png"
            Behavior on scale {
                NumberAnimation {
                    duration: 120
                }
            }
            MouseArea {
                id: motorMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor

                onClicked: {
                    motorDialog.open()
                }
            }
        }

        Item {
            width: 40
            height: 40
            x: 17
            y: 72

            // 外層呼吸光圈
            Rectangle {
                id: breathingRing

                width: 30
                height: 30
                radius: 15
                anchors.centerIn: parent

                color: "transparent"

                border.width: 7
                border.color: Td.motorRunningSv
                              ? "#2866FF00"
                              : "#18666666"

                opacity: Td.motorRunningSv ? 0.8 : 0.3

                // 呼吸縮放
                SequentialAnimation on scale {
                    running: Td.motorRunningSv
                    loops: Animation.Infinite

                    NumberAnimation {
                        from: 1.0
                        to: 1.55
                        duration: 900
                        easing.type: Easing.InOutSine
                    }

                    NumberAnimation {
                        from: 1.55
                        to: 1.0
                        duration: 900
                        easing.type: Easing.InOutSine
                    }
                }

                // 呼吸透明度
                SequentialAnimation on opacity {
                    running: Td.motorRunningSv
                    loops: Animation.Infinite

                    NumberAnimation {
                        from: 0.85
                        to: 0.45
                        duration: 900
                        easing.type: Easing.InOutSine
                    }

                    NumberAnimation {
                        from: 0.45
                        to: 0.85
                        duration: 900
                        easing.type: Easing.InOutSine
                    }
                }
            }

            // 原本外圈
            Rectangle {
                width: 30
                height: 30
                radius: 15
                anchors.centerIn: parent

                color: Td.motorRunningSv
                       ? "#3366FF00"
                       : "#33666666"

                // 中心燈
                Rectangle {
                    width: 14
                    height: 14
                    radius: 7

                    anchors.centerIn: parent

                    color: Td.motorRunningSv
                           ? "#66FF00"
                           : "#666666"
                }
            }
        }
    }

    // =========================================================
    // 中央槽體
    // =========================================================
    Rectangle {
        x: 495
        y: 386
        Image {
            id: center
            scale: 1.5
            source: "assets/center.png"
        }
    }

    // =========================================================
    // PT-01
    // =========================================================
    Components.ValueTag {
        x: 578
        y: 436
        title: "PT-01"
        value: Td.pt01ValuePv.toFixed(1)
        unit: "Pa"
    }

    // 小型管件
    Image {
        id: comp
        x: 545
        y: 455
        scale: 1.4
        source: "assets/Group 121.png"
    }

    // =========================================================
    // 上方溫度、壓力
    // =========================================================
    Row {
        x: 506
        y: 118
        spacing: 7

        Components.ValueTag {
            id: tt01
            title: "TT-01"
            value: Td.tt01ValuePv.toFixed(1)
            unit: "°C"
        }

        Components.ValueTag {
            id: tt02
            title: "TT-02"
            value: Td.tt02ValuePv.toFixed(1)
            unit: "°C"
        }

        Components.ValueTag {
            title: "PT-04"
            value: Td.pt04ValuePv.toFixed(1)
            unit: "Pa"
        }

        Components.ValueTag {
            title: "PT-05"
            value: Td.pt05ValuePv.toFixed(1)
            unit: "Pa"
        }
    }

    Components.InterfaceBox {
        x: 781
        y: 136
        label: "測試設備接口"
    }

    Row {
        x: 914
        y: 190
        spacing: 14

        Components.ValueTag {
            id: tt03
            title: "TT-03"
            value: Td.tt03ValuePv.toFixed(1)
            unit: "°C"
        }

        Components.ValueTag {
            id: tt04
            title: "TT-04"
            value: Td.tt04ValuePv.toFixed(1)
            unit: "°C"
        }
    }
    Row {
        x: 914
        y: 118
        spacing: 14
    Components.ValueTag {

        title: "PT-07"
        value: Td.pt07ValuePv.toFixed(1)
        unit: "Pa"
    }

    Components.ValueTag {

        title: "PT-06"
        value: Td.pt06ValuePv.toFixed(1)
        unit: "Pa"
    }
    }

    // =========================================================
    // Air Separator
    // =========================================================
    Rectangle {
        x: 803
        y: 362
        width: 92
        height: 48
        radius: 12
        color: "#7B8393"
        border.color: root.mainBlue
        border.width: 1

        Text {
            anchors.centerIn: parent
            text: "Air\nSaperator"
            horizontalAlignment: Text.AlignHCenter
            color: "white"
            font.pixelSize: 15
            font.family: "Consolas"
        }
    }

    // =========================================================
    // 2-way Valve
    // =========================================================
    Item {
        x: 660
        y: 359
        width: 58
        height: 60

        Text {
            x: -53
            y: 2
            width: 50
            text: "2-way\nValve"
            rotation: -90
            color: "white"
            font.pixelSize: 14
            horizontalAlignment: Text.AlignHCenter
        }

        Image {
            id: wayValve
            scale:1.2
            source: "assets/Group 164.png"
        }


    }

    // =========================================================
    // 下方泵浦
    // =========================================================
    Item {
        id: motor2Item

        x: 595
        y: 547
        width: 73
        height: 74

        readonly property string value: String(Td.pump2HzSv)

        function commitValue() {
            var newValue = Number(motor2EditField.text)
            if (isNaN(newValue))
                return

            Td.pump2HzSv = newValue
            Qt.inputMethod.hide()
            motor2ValueDialog.close()
        }

        Image {
            id: motor2
            x: -55
            y: -50
            scale: 0.4
            source: "assets/motor2.png"
        }
        Rectangle {
            id: motorValueBox

            width: 55
            height: 32
            x: 9
            y: 79

            color: motorMouseArea.containsMouse
                   ? "#339EDCFF"
                   : "#7C5CFC"

            border.color: motorMouseArea.containsMouse
                          ? "#BFEAFF"
                          : "transparent"

            border.width: motorMouseArea.containsMouse ? 1 : 0
            radius: 2

            scale: motorMouseArea.containsMouse ? 1.05 : 1.0

            Behavior on color {
                ColorAnimation {
                    duration: 120
                }
            }

            Behavior on scale {
                NumberAnimation {
                    duration: 120
                }
            }

            Text {
                anchors.top: parent.top
                anchors.topMargin: 1
                anchors.horizontalCenter: parent.horizontalCenter

                text: motor2Item.value

                color: motorMouseArea.containsMouse
                       ? "#FFFFFF"
                       : "white"

                font.pixelSize: 15
                font.family: "Consolas"
            }

            Text {
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 1
                anchors.horizontalCenter: parent.horizontalCenter

                text: "PV HZ"

                color: motorMouseArea.containsMouse
                       ? "#D9F5FF"
                       : "white"

                font.pixelSize: 14
                font.family: "Consolas"
            }

            MouseArea {
                id: motorMouseArea
                anchors.fill: parent

                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor

                onClicked: {
                    motor2EditField.text = motor2Item.value
                    motor2ValueDialog.open()
                }
            }
        }

        Dialog {
            id: motor2ValueDialog

            width: Overlay.overlay.width * 0.3
            height: Overlay.overlay.height * 0.3

            anchors.centerIn: Overlay.overlay

            modal: true
            standardButtons: Dialog.NoButton

            background: Rectangle {
                color: "#152238"
                radius: 12

                border.color: root.mainBlue
                border.width: 2
            }

            contentItem: Column {
                anchors.fill: parent
                anchors.margins: 30
                spacing: 20

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter

                    text: "修改馬達頻率"

                    color: "white"
                    font.pixelSize: 24
                    font.bold: true
                }

                TextField {
                    id: motor2EditField

                    width: 220
                    height: 55
                    anchors.horizontalCenter: parent.horizontalCenter

                    color: "white"

                    font.pixelSize: 26
                    font.family: "Consolas"

                    horizontalAlignment: TextInput.AlignHCenter
                    verticalAlignment: TextInput.AlignVCenter

                    inputMethodHints: Qt.ImhFormattedNumbersOnly

                    validator: DoubleValidator {
                        bottom: 0
                        top: 9999
                        decimals: 2
                    }

                    background: Rectangle {
                        color: "#0F192D"
                        radius: 6

                        border.color: motor2EditField.activeFocus
                                      ? root.mainBlue
                                      : root.mainBlue

                        border.width: 2
                    }

                    onPressed: {
                        forceActiveFocus()
                        Qt.inputMethod.show()
                    }
                    Keys.onReturnPressed: {
                        motor2Item.commitValue()
                    }
                    Keys.onEnterPressed: {
                        motor2Item.commitValue()
                    }
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter

                    text: "單位：Hz"

                    color: "white"
                    font.pixelSize: 17
                }

                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 30

                    Button {
                        width: 140
                        height: 50

                        background: Rectangle {
                            color: root.mainBlue
                            radius: 6
                        }

                        contentItem: Text {
                            text: "確認"

                            color: "white"
                            font.pixelSize: 18
                            font.bold: true

                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        onClicked: {
                            motor2Item.commitValue()
                        }
                    }

                    Button {
                        width: 140
                        height: 50

                        background: Rectangle {
                            color: "#4B5563"
                            radius: 6
                        }

                        contentItem: Text {
                            text: "取消"

                            color: "white"
                            font.pixelSize: 18
                            font.bold: true

                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        onClicked: {
                            Qt.inputMethod.hide()
                            motor2ValueDialog.close()
                        }
                    }
                }
            }

            onOpened: {
                motor2EditField.text = motor2Item.value
                motor2EditField.forceActiveFocus()
                motor2EditField.selectAll()

                Qt.inputMethod.show()
            }

            onClosed: {
                Qt.inputMethod.hide()
            }
        }
    }
    // =========================================================
    // PT-02 / PT-03
    // =========================================================
    Components.ValueTag {
        x: 732
        y: 523
        title: "PT-02"
        value: Td.pt02ValuePv.toFixed(1)
        unit: "Pa"
    }

    Components.ValueTag {
        x: 897
        y: 523
        title: "PT-03"
        value: Td.pt03ValuePv.toFixed(1)
        unit: "Pa"
    }

    // =========================================================
    // Filter
    // =========================================================
    Rectangle {
        x: 818
        y: 475
        width: 40
        height: 168
        radius: 20

        color: "#858B94"
        border.color: "#8FE8E6"
        border.width: 3

        Text {
            anchors.centerIn: parent
            text: "filter"
            rotation: 90
            color: "white"
            font.pixelSize: 15
            font.family: "Consolas"
        }
    }

    // =========================================================
    // Flow Meter
    // =========================================================
    Item {
        x: 1003
        y: 559
        width: 60
        height: 97
        id: flowMeter
        readonly property real value: Td.flowMeterValuePv


        Rectangle {
            width: 55
            height: 55
            radius: 27
            anchors.horizontalCenter: parent.horizontalCenter
            color: "#8B8D8F"
            border.color: root.mainBlue
            border.width: 2

            Text {
                anchors.centerIn: parent
                text: "FM"
                color: "white"
                font.pixelSize: 17
                font.family: "Consolas"
            }
        }

        Rectangle {
            width: 55
            height: 32
            anchors.horizontalCenter: parent.horizontalCenter
            y: 64
            color: "#21BCE8"

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                text: flowMeter.value
                color: "white"
                font.pixelSize: 14
                font.family: "Consolas"
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                text: "L/MIN"
                color: "white"
                font.pixelSize: 14
                font.family: "Consolas"
            }
        }
    }

    // =========================================================
    // 加熱器接口
    // =========================================================
    Components.InterfaceBox {
        x: 1073
        y: 518
        label: "加熱器接口"
    }

    // =========================================================
    // Leakage Sensor
    // =========================================================
    Text {
        x: 416
        y: 658
        text: "Leakage\nSensor"
        color: root.textColor
        font.pixelSize: 15
        font.family: "Consolas"
        horizontalAlignment: Text.AlignHCenter
    }
    Dialog {
        id: motorDialog

        width: Overlay.overlay.width * 0.3
        height: Overlay.overlay.height * 0.3

        anchors.centerIn: Overlay.overlay

        modal: true
        // title: "泵浦控制"
        // standardButtons: Dialog.NoButton

        background: Rectangle {
            color: "#152238"
            radius: 12

            border.color: "#0087DC"
            border.width: 2
        }

        contentItem: Column {
            anchors.fill: parent
            anchors.margins: 90

            spacing: 30

            Text {
                anchors.horizontalCenter: parent.horizontalCenter

                text: Td.motorRunningSv
                      ? "是否關閉泵浦？"
                      : "是否開啟泵浦？"

                color: "white"
                font.pixelSize: 24
                font.bold: true
            }

            Item {
                width: 1
                height: 20
            }


            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 30

                Button {
                    id: confirmButton
                    width: 140
                    height: 50
                    hoverEnabled: true

                    background: Rectangle {
                        radius: 6
                        color: confirmButton.hovered
                           ? "#19B8FF"
                           : "#0087DC"
                        border.color: confirmButton.hovered
                                      ? "#8EDCFF"
                                      : "transparent"

                        border.width: confirmButton.hovered ? 1 : 0

                        scale: confirmButton.hovered ? 1.05 : 1.0

                        Behavior on color {
                            ColorAnimation { duration: 120 }
                        }

                        Behavior on scale {
                            NumberAnimation { duration: 120 }
                        }
                    }

                    contentItem: Text {
                        text: Td.motorRunningSv ? "關閉" : "開啟"

                        color: "white"
                        font.pixelSize: confirmButton.hovered ? 19 : 18
                        font.bold: true

                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter

                        Behavior on font.pixelSize {
                            NumberAnimation { duration: 120 }
                        }
                    }

                    onClicked: {
                        Td.motorRunningSv = !Td.motorRunningSv

                        motorDialog.close()
                    }
                }

                Button {
                    id: cancelButton
                    width: 140
                    height: 50
                    hoverEnabled: true

                    background: Rectangle {
                        radius: 6
                        color: cancelButton.hovered
                               ? "#657184"
                               : "#4B5563"

                        border.color: cancelButton.hovered
                                      ? "#AFC5D8"
                                      : "transparent"

                        border.width: cancelButton.hovered ? 1 : 0

                        scale: cancelButton.hovered ? 1.05 : 1.0

                        Behavior on color {
                            ColorAnimation { duration: 120 }
                        }

                        Behavior on scale {
                            NumberAnimation { duration: 120 }
                        }
                    }

                    contentItem: Text {
                        text: "取消"

                        color: "white"
                        font.pixelSize: cancelButton.hovered ? 19 : 18
                        font.bold: true

                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter

                        Behavior on font.pixelSize {
                            NumberAnimation { duration: 120 }
                        }
                    }

                    onClicked: {
                        motorDialog.close()
                    }
                }
            }
        }
    }
}
