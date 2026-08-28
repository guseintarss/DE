import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import Qt5Compat.GraphicalEffects
import Quickshell
import Quickshell.Io
import Quickshell.Wayland
import Quickshell.Services.SystemTray
import Quickshell.Services.UPower
import Quickshell.Widgets

PanelWindow {
    id: root

    anchors {
        top: true
        left: true
        right: true
    }
    implicitHeight: 36
    color: "transparent"
    exclusionMode: ExclusionMode.Auto

    readonly property string uiFont: "SF Pro Display, Segoe UI, Cantarell, sans-serif"
    readonly property string iconFont: "MesloLGLDZ Nerd Font, Material Design Icons, sans-serif"

    property int wsCurrent: 1
    property int wsCount: 1
    
    // Состояние меню Apple
    property bool appleMenuOpen: false

    Rectangle {
        anchors.fill: parent
        color: '#00d6d6d8' 
        border.color: '#00e9e9e9'
        border.width: 1
        
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 0

            RowLayout {
                Layout.fillHeight: true
                Layout.alignment: Qt.AlignVCenter
                spacing: 4

                // === КНОПКА APPLE С POWER MENU ===
                MenuButton {
                    id: appleButton
                    glyph: "\uF8FF" // логотип Apple (Material Design Icons)
                    active: root.appleMenuOpen
                    onClicked: root.appleMenuOpen = !root.appleMenuOpen
                }

                Rectangle {
                    Layout.fillHeight: true
                    Layout.preferredWidth: 1
                    Layout.margins: 8
                    color: "#33FFFFFF"
                }

                Repeater {
                    model: ["File", "Edit", "View", "Help"]
                    delegate: MenuButton {
                        label: modelData
                        active: ShellState.barPopup === "menu" &&
                                ShellState.barMenuName === modelData
                        onClicked: ShellState.toggleBarMenu(modelData)
                    }
                }
                
                Item { Layout.fillWidth: true }
            }

            Text {
                Layout.alignment: Qt.AlignVCenter | Qt.AlignHCenter
                font.family: root.uiFont
                font.pixelSize: 14
                font.weight: Font.Medium
                color: "#FFFFFF"
                text: {
                    const active = ToplevelManager.activeToplevel;
                    return active && active.title ? active.title : "Рабочий стол";
                }
                elide: Text.ElideMiddle
                Layout.maximumWidth: 400
                opacity: 0.9
                Behavior on opacity { NumberAnimation { duration: 200 } }
            }

            Item { Layout.fillWidth: true }

            RowLayout {
                Layout.fillHeight: true
                Layout.alignment: Qt.AlignVCenter
                spacing: 16

                Row {
                    spacing: 7
                    Layout.alignment: Qt.AlignVCenter
                    Repeater {
                        model: root.wsCount
                        delegate: Item {
                            required property int index
                            property int wsNum: index + 1
                            width: 18
                            height: 20

                            Rectangle {
                                anchors.centerIn: parent
                                width: root.wsCurrent === parent.wsNum ? 16 : 7
                                height: 7
                                radius: 3.5
                                color: root.wsCurrent === parent.wsNum
                                       ? "#FFFFFF" : "#55FFFFFF"
                                Behavior on width {
                                    NumberAnimation { duration: 180; easing.type: Easing.OutQuint }
                                }
                                Behavior on color {
                                    ColorAnimation { duration: 180 }
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.wsSwitch(parent.wsNum)
                            }
                        }
                    }
                }

                Item {
                    Layout.fillHeight: true
                    implicitWidth: volInner.implicitWidth
                    implicitHeight: volInner.implicitHeight
                    visible: volumeProc.label !== "" || volumeProc.muted

                    RowLayout {
                        id: volInner
                        anchors.fill: parent
                        spacing: 6
                        Text {
                            font.family: root.iconFont
                            font.pixelSize: 14
                            color: "#FFFFFF"
                            text: volumeProc.muted ? "" : ""
                        }
                        Text {
                            font.family: root.uiFont
                            font.pixelSize: 13
                            font.weight: Font.Medium
                            color: "#FFFFFF"
                            text: volumeProc.label
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: ShellState.toggleCC()
                    }
                }

                Item {
                    Layout.fillHeight: true
                    implicitWidth: batInner.implicitWidth
                    implicitHeight: batInner.implicitHeight
                    visible: UPower.displayDevice && UPower.displayDevice.percentage > 0

                    RowLayout {
                        id: batInner
                        anchors.fill: parent
                        spacing: 6
                        Text {
                            font.family: root.iconFont
                            font.pixelSize: 14
                            color: "#FFFFFF"
                            text: {
                                const d = UPower.displayDevice;
                                if (d === null) return "";
                                const pct = d.percentage;
                                if (d.state === UPowerDeviceState.Charging) return "";
                                if (pct > 80) return "";
                                if (pct > 60) return "";
                                if (pct > 40) return "";
                                if (pct > 20) return "";
                                return "";
                            }
                        }
                        Text {
                            font.family: root.uiFont
                            font.pixelSize: 13
                            font.weight: Font.Medium
                            color: "#FFFFFF"
                            text: UPower.displayDevice
                                  ? Math.round(UPower.displayDevice.percentage) + "%" : ""
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: ShellState.toggleCC()
                    }
                }

                RowLayout {
                    spacing: 8
                    Repeater {
                        model: SystemTray.items
                        delegate: Item {
                            required property var modelData
                            Layout.preferredWidth: 20
                            Layout.preferredHeight: 20
                            
                            IconImage {
                                anchors.fill: parent
                                source: modelData.icon
                                asynchronous: true
                            }
                            
                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: (mouse) => {
                                    if (mouse.button === Qt.LeftButton) modelData.activate();
                                    else modelData.secondaryActivate();
                                }
                            }
                        }
                    }
                }

                Text {
                    font.family: root.uiFont
                    font.pixelSize: 13
                    font.weight: Font.Medium
                    color: "#FFFFFF"
                    text: Qt.formatDateTime(clock.date, "ddd, d MMM  HH:mm")

                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -6
                        cursorShape: Qt.PointingHandCursor
                        onClicked: ShellState.toggleCC()
                    }

                    SystemClock {
                        id: clock
                        precision: SystemClock.Minutes
                    }
                }
            }
        }

        // === POWER MENU (Выпадает под кнопкой Apple) ===
        MouseArea {
            id: powerMenuOverlay
            anchors.fill: parent
            visible: root.appleMenuOpen
            z: 999
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: root.appleMenuOpen = false
        }

        Rectangle {
            id: powerMenu
            visible: root.appleMenuOpen
            z: 1000
            x: 12 // Позиция под кнопкой Apple (с учетом левого отступа)
            y: root.height + 4 // 4px отступ вниз от топбара
            width: 200
            height: powerMenuColumn.height + 16
            radius: 12
            color: "#E61C1C1E"
            border.color: "#44FFFFFF"
            border.width: 1

            layer.enabled: true
            layer.effect: DropShadow {
                transparentBorder: true
                horizontalOffset: 0
                verticalOffset: 8
                radius: 16
                samples: 20
                color: "#88000000"
            }

            Column {
                id: powerMenuColumn
                anchors.fill: parent
                anchors.margins: 8
                spacing: 4

                Repeater {
                    model: [
                        { text: " Sleep", action: "sleep", icon: "" },
                        { text: "separator" },
                        { text: " Restart", action: "restart", icon: "" },
                        { text: " Shutdown", action: "shutdown", icon: "" },
                        { text: "separator" },
                        { text: " Log Out", action: "logout", icon: "" }
                    ]
                    delegate: Item {
                        width: parent.width
                        height: modelData === "separator" ? 1 : 36

                        Rectangle {
                            anchors.fill: parent
                            radius: 8
                            color: (modelData !== "separator" && powerMenuArea.containsMouse) ? "#33FFFFFF" : "transparent"
                            Behavior on color { ColorAnimation { duration: 150; easing.type: Easing.OutQuad } }
                            visible: modelData !== "separator"
                        }

                        Rectangle {
                            anchors.fill: parent
                            color: "#33FFFFFF"
                            visible: modelData === "separator"
                        }

                        Row {
                            anchors.centerIn: parent
                            spacing: 8
                            visible: modelData !== "separator"

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData.icon
                                font.family: root.iconFont
                                font.pixelSize: 14
                                color: "#FFFFFF"
                            }

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: modelData.text
                                color: "#FFFFFF"
                                font.family: root.uiFont
                                font.pixelSize: 13
                                font.weight: Font.Medium
                            }
                        }

                        MouseArea {
                            id: powerMenuArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            visible: modelData !== "separator"
                            onClicked: {
                                root.executePowerAction(modelData.action);
                                root.appleMenuOpen = false;
                            }
                        }
                    }
                }
            }
        }
    }

    component MenuButton: Item {
        id: btn
        property string glyph: ""
        property string label: ""
        property bool active: false
        signal clicked()

        Layout.fillHeight: true
        Layout.preferredWidth: Math.max(labelText.implicitWidth, glyphText.implicitWidth) + 16

        Rectangle {
            anchors.fill: parent
            anchors.margins: 4
            radius: 6
            color: btnArea.containsMouse || btn.active ? '#1bf4f3f3' : "transparent"
            Behavior on color { ColorAnimation { duration: 150; easing.type: Easing.OutQuad } }
        }

        Text {
            id: glyphText
            visible: btn.glyph !== ""
            anchors.centerIn: parent
            font.family: root.iconFont
            font.pixelSize: 16
            color: "#FFFFFF"
            text: btn.glyph
        }

        Text {
            id: labelText
            visible: btn.label !== ""
            anchors.centerIn: parent
            font.family: root.uiFont
            font.pixelSize: 13
            font.weight: btnArea.containsMouse ? Font.Bold : Font.Normal
            color: "#FFFFFF"
            text: btn.label
            Behavior on font.weight { NumberAnimation { duration: 150 } }
        }

        MouseArea {
            id: btnArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: btn.clicked()
        }
    }

    Process {
        id: volumeProc
        property string label: ""
        property bool muted: false

        command: ["sh", "-c", "wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null || echo 'Volume: 0.00'"]
        
        stdout: StdioCollector {
            onStreamFinished: {
                const s = this.text.trim();
                if (s === "") return;
                
                volumeProc.muted = /MUTED/.test(s);
                const m = s.match(/([\d.]+)\s*$/);
                const v = m ? Math.round(parseFloat(m[1]) * 100) : 0;
                
                volumeProc.label = volumeProc.muted ? "Muted" : v + "%";
            }
        }
    }

    Timer {
        interval: 2000
        repeat: true
        running: true
        triggeredOnStart: true
        onTriggered: volumeProc.running = true
    }

    FileView {
        id: wsStateFile
        path: Quickshell.env("XDG_RUNTIME_DIR") + "/de/workspaces"
        watchChanges: true
        onFileChanged: wsStateFile.reload()
        onLoaded: {
            const parts = wsStateFile.text().trim().split(" ");
            if (parts.length === 2) {
                const cur = parseInt(parts[0]);
                const cnt = parseInt(parts[1]);
                if (!isNaN(cur)) root.wsCurrent = cur;
                if (!isNaN(cnt) && cnt >= 1) root.wsCount = cnt;
            }
        }
    }

    Process {
        id: wsCmdProc
        command: ["sh", "-c",
                  "printf '%s\\n' 1 > \"$XDG_RUNTIME_DIR/de/ws-cmd\""]
    }

    function wsSwitch(n) {
        wsCmdProc.command = ["sh", "-c",
            "printf '%s\\n' " + n + " > \"$XDG_RUNTIME_DIR/de/ws-cmd\""];
        wsCmdProc.running = true;
    }

    // === ФУНКЦИЯ ВЫПОЛНЕНИЯ POWER КОМАНД ===
    function executePowerAction(action) {
        console.log("Power action:", action);

        // Бинарные служебные команды запускаем напрямую через execDetached:
        // systemctl управляет питанием, loginctl — выходом из сессии.
        let args = [];

        switch(action) {
            case "sleep":
                args = ["systemctl", "suspend"];
                break;
            case "restart":
                args = ["systemctl", "reboot"];
                break;
            case "shutdown":
                args = ["systemctl", "poweroff"];
                break;
            case "logout":
                // Выход из сессии через loginctl (терминирует пользовательскую
                // сессию по $XDG_SESSION_ID). Работает в большинстве DE/WM.
                args = ["loginctl", "terminate-session", Quickshell.env("XDG_SESSION_ID") || ""];
                break;
        }

        if (args.length > 0) {
            if (args[args.length - 1] === "") {
                // Нет XDG_SESSION_ID — используем kill на наш собственный PID:
                // завершение сессии эквивалентно выходу из shell.
                args = ["loginctl", "terminate-user", Quickshell.env("USER") || ""];
            }
            try {
                Quickshell.execDetached(args);
            } catch (e) {
                console.warn("Power action failed:", e);
            }
        }
    }
}