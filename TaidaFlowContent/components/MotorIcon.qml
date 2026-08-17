import QtQuick
import QtQuick.Controls
import Core 1.0
Item {
    id: motorIcon

    property string motorName: "M1"
    property string value: "20"
    property string unit: "PV %"

    width: 78
    height: 110

    Rectangle {
        width: 30
        height: 22
        anchors.horizontalCenter: parent.horizontalCenter
        color: "#071729"
        border.color: root.mainBlue
        border.width: 1

        Text {
            anchors.centerIn: parent
            text: motorName
            color: root.mainBlue
            font.pixelSize: 14
        }
    }

    Rectangle {
        width: 38
        height: 38
        radius: 19
        anchors.horizontalCenter: parent.horizontalCenter
        y: 28
        color: "#E7F3FF"
    }

    Rectangle {
        width: 2
        height: 48
        anchors.horizontalCenter: parent.horizontalCenter
        y: 25
        color: "#9EDCFF"
    }

    Rectangle {
        width: 56
        height: 32
        y: 74
        anchors.horizontalCenter: parent.horizontalCenter
        color: root.mainBlue

        // 數值顯示
        TextField {
            id: valueField1

            width: parent.width
            height: 17

            anchors.top: parent.top

            text: motorIcon.value
            readOnly: true

            color: valueMouseArea.containsMouse ? "#FFFF00" : "white"
            font.pixelSize: valueMouseArea.containsMouse ? 16 : 15
            font.family: "Consolas"

            horizontalAlignment: TextInput.AlignHCenter
            verticalAlignment: TextInput.AlignVCenter

            padding: 0

            background: Rectangle {
                color: valueMouseArea.containsMouse
                   ? "#40FFFFFF"
                   : "transparent"
                border.color: valueMouseArea.containsMouse
                   ? "#FFFFFF"
                   : "transparent"
                border.width: valueMouseArea.containsMouse ? 1 : 0
                radius: 3

                Behavior on color {
                   ColorAnimation { duration: 120 }
                }
            }


            MouseArea {
                id: valueMouseArea
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                hoverEnabled: true
                onClicked: {
                    valueDialog.open()
                }
            }
        }

        Text {
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 1
            anchors.horizontalCenter: parent.horizontalCenter
            text: unit
            color: "white"
            font.pixelSize: 14
            font.family: "Consolas"
        }
    }
    // =====================================================
    // 修改數值 Dialog
    // =====================================================
    Dialog {
        id: valueDialog

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

            spacing: 25

            Text {
                anchors.horizontalCenter: parent.horizontalCenter

                text: "修改 " + motorName + " 數值"

                color: "white"
                font.pixelSize: 24
                font.bold: true
            }

            TextField {
                id: editField

                width: 200
                height: 55
                anchors.horizontalCenter: parent.horizontalCenter

                color: "white"

                font.pixelSize: 24
                font.family: "Consolas"

                horizontalAlignment: TextInput.AlignHCenter
                verticalAlignment: TextInput.AlignVCenter

                // 數字鍵盤
                inputMethodHints: Qt.ImhFormattedNumbersOnly

                validator: DoubleValidator {
                    bottom: 0
                    top: 9999
                    decimals: 2
                }

                background: Rectangle {
                    color: "#0F192D"
                    radius: 6

                    border.color: root.mainBlue
                    border.width: 2
                }

                // Dialog 開啟後直接取得輸入焦點
                Component.onCompleted: {
                    forceActiveFocus()
                    Qt.inputMethod.show()
                }
                Keys.onReturnPressed: {
                    motorIcon.value = editField.text
                    Qt.inputMethod.hide()
                    valueDialog.close()
                }
                Keys.onEnterPressed: {
                    motorIcon.value = editField.text
                    Qt.inputMethod.hide()
                    valueDialog.close()
                }
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter

                text: "單位：" + unit

                color: "#AFC5D8"
                font.pixelSize: 16
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
                        if(motorName == "M1"){
                            Td.m1ValueSv = editField.text
                        }
                        value = editField.text

                        // 如果要傳給 C++
                        // proxy.setMotorValue(motorName, Number(editField.text))

                        valueDialog.close()
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
                        valueDialog.close()
                    }
                }
            }
        }

        onOpened: {
            editField.text = motorIcon.value
            editField.forceActiveFocus()
            editField.selectAll()
            // 如果已啟用 Qt Virtual Keyboard
            Qt.inputMethod.show()
        }
        onClosed: {
            Qt.inputMethod.hide()
        }
    }

}
