import QtQuick
import QtQuick.Layouts
import Quickshell
import Quickshell.Io
import Quickshell.Wayland
import Quickshell.Services.SystemTray
import Quickshell.Services.UPower
import Quickshell.Widgets

// Верхний «менюбар» macOS Sequoia: лаунчер, File/Edit/View/Help,
// в центре — имя активного приложения, справа — статус-область.
PanelWindow {
    id: root

    // Кнопка менюбара (иконка или текст) с подсветкой при наведении.
    component AbstractButton: Item {
        id: btn

        property string glyph: ""
        property string label: ""
        signal clicked()

        Layout.fillHeight: true
        Layout.preferredWidth: Math.max(labelText.implicitWidth, glyphText.implicitWidth) + 24

        Rectangle {
            anchors.fill: parent
            anchors.margins: 2
            radius: 5
            color: btnArea.containsMouse ? "#26000000" : "transparent"
        }
        Text {
            id: glyphText
            visible: btn.glyph !== ""
            anchors.centerIn: parent
            font.family: root.iconFont
            font.pixelSize: 15
            color: "#E6000000"
            text: btn.glyph
        }
        Text {
            id: labelText
            visible: btn.label !== ""
            anchors.centerIn: parent
            font.family: root.uiFont
            font.pixelSize: 13
            color: "#E6000000"
            text: btn.label
        }
        MouseArea {
            id: btnArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: btn.clicked()
        }
    }

    anchors {
        top: true
        left: true
        right: true
    }
    implicitHeight: 30
    color: "transparent"

    readonly property string iconFont: "MesloLGLDZ Nerd Font"
    readonly property string uiFont: "SF Pro Display"

    Rectangle {
        anchors.fill: parent
        color: "#B3FFFFFF"
        border.color: "#1F000000"
        border.width: 1
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // --- Левая секция ---
        RowLayout {
            Layout.fillHeight: true
            Layout.alignment: Qt.AlignLeft
            spacing: 0

            AbstractButton {
                glyph: ""
                onClicked: ShellState.toggleLauncher()
            }
            Repeater {
                model: ["File", "Edit", "View", "Help"]
                delegate: AbstractButton {
                    label: modelData
                }
            }
            Item {
                Layout.fillWidth: true
            }
        }

        // --- Центр: активное приложение ---
        Text {
            Layout.alignment: Qt.AlignHCenter
            font.family: root.uiFont
            font.pixelSize: 13
            font.bold: true
            color: "#E6000000"
            text: ToplevelManager.activeToplevel
                  ? (ToplevelManager.activeToplevel.title || "") : ""
            elide: Text.ElideMiddle
            Layout.maximumWidth: 420
        }

        // --- Правая секция ---
        RowLayout {
            Layout.fillHeight: true
            Layout.alignment: Qt.AlignRight
            spacing: 6

            Text {
                visible: volumeProc.text !== ""
                font.family: root.iconFont
                font.pixelSize: 13
                color: "#E6000000"
                text: volumeProc.muted ? "" : ""
                rightPadding: 0
            }
            Text {
                visible: volumeProc.text !== ""
                font.family: root.uiFont
                font.pixelSize: 12
                color: "#E6000000"
                text: volumeProc.label
            }

            Text {
                id: bat
                // Иконка батареи: зарядка/разряд (UPower state).
                property string iconName_: {
                    const d = UPower.displayDevice;
                    if (!d || d.percentage <= 0)
                        return "";
                    return d.state === UPowerDeviceState.Charging ? "" : "";
                }
                visible: iconName_ !== ""
                font.family: root.iconFont
                font.pixelSize: 13
                color: "#E6000000"
            }
            Text {
                // Процент заряда.
                property bool hasBat: bat.iconName_ !== ""
                visible: hasBat
                font.family: root.uiFont
                font.pixelSize: 12
                color: "#E6000000"
                text: hasBat ? Math.round(UPower.displayDevice.percentage) + "%" : ""
            }

            Repeater {
                model: SystemTray.items
                delegate: Item {
                    required property var modelData
                    Layout.preferredWidth: 20
                    Layout.preferredHeight: 20
                    IconImage {
                        anchors.fill: parent
                        anchors.margins: 2
                        source: modelData.icon
                        asynchronous: true
                    }
                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: (mouse) => {
                            if (mouse.button === Qt.LeftButton)
                                modelData.activate();
                            else
                                modelData.secondaryActivate();
                        }
                    }
                }
            }

            Text {
                font.family: root.uiFont
                font.pixelSize: 12
                color: "#E6000000"
                text: Qt.formatDateTime(clock.date, "ddd d MMM  HH:mm")
                rightPadding: 12

                SystemClock {
                    id: clock
                    precision: SystemClock.Minutes
                }
            }
        }
    }

    // Компонент кнопки менюбара объявлен в начале файла.

    Process {
        id: volumeProc
        property string text: ""
        property bool muted: false
        property string label: ""

        command: ["sh", "-c",
                  "wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null"]
        stdout: StdioCollector {
            onStreamFinished: {
                const s = this.text.trim();          // "Volume: 0.65" | "... [MUTED]"
                if (s === "") {
                    volumeProc.text = "";
                    return;
                }
                volumeProc.muted = /MUTED/.test(s);
                const m = s.match(/([\d.]+)\s*$/);
                const v = m ? Math.round(parseFloat(m[1]) * 100) : 0;
                volumeProc.label = volumeProc.muted ? "" : v + "%";
                volumeProc.text = s;
            }
        }
    }

    Timer {
        interval: 3000
        repeat: true
        running: true
        triggeredOnStart: true
        onTriggered: volumeProc.running = true
    }
}
