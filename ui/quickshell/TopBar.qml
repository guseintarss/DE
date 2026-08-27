import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
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
    implicitHeight: 36 // Чуть выше для "воздуха", как в macOS
    color: "transparent"
    exclusionMode: ExclusionMode.Auto

    // Шрифты: SF Pro для текста, Nerd Font для иконок
    readonly property string uiFont: "SF Pro Display, Segoe UI, Cantarell, sans-serif"
    readonly property string iconFont: "MesloLGLDZ Nerd Font, Material Design Icons, sans-serif"

    // === РАБОЧИЕ СТОЛЫ (Spaces): состояние из композитора ===
    // Файл $XDG_RUNTIME_DIR/de/workspaces: "current count" (1-based).
    property int wsCurrent: 1
    property int wsCount: 1

    // === СТЕКЛЯННЫЙ ФОН МЕНЮБАРА ===
    Rectangle {
        anchors.fill: parent
        // Темное стекло (для светлой темы замените на "#B3FFFFFF" и border на "#33000000")
        color: '#0bd6d6d8' 
        border.color: '#00e9e9e9'
        border.width: 1
        
        // Основной макет
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 0

            // ==========================================
            // ЛЕВАЯ СЕКЦИЯ: Меню
            // ==========================================
            RowLayout {
                Layout.fillHeight: true
                Layout.alignment: Qt.AlignVCenter
                spacing: 4

                // Кнопка Apple / Лаунчер
                MenuButton {
                    glyph: "" // Unicode логотип Apple (работает в Nerd Font)
                    onClicked: ShellState.toggleLauncher()
                }

                // Разделитель (тонкая вертикальная черта)
                Rectangle {
                    Layout.fillHeight: true
                    Layout.preferredWidth: 1
                    Layout.margins: 8
                    color: "#33FFFFFF"
                }

                // Пункты меню
                Repeater {
                    model: ["File", "Edit", "View", "Help"]
                    delegate: MenuButton {
                        Component.onCompleted: console.log("TEMP btn:", modelData, "label:", label, "w:", width) // TEMP
                        label: modelData
                        active: ShellState.barPopup === "menu" &&
                                ShellState.barMenuName === modelData
                        onClicked: ShellState.toggleBarMenu(modelData)
                    }
                }
                
                // Растягиваем левую часть, чтобы центр был ровно посередине
                Item { Layout.fillWidth: true }
            }

            // ==========================================
            // ЦЕНТР: Активное приложение
            // ==========================================
            Text {
                Layout.alignment: Qt.AlignVCenter | Qt.AlignHCenter
                font.family: root.uiFont
                font.pixelSize: 14
                font.weight: Font.Medium // Полужирный, как в macOS
                color: "#FFFFFF"
                text: {
                    const active = ToplevelManager.activeToplevel;
                    return active && active.title ? active.title : "Рабочий стол";
                }
                elide: Text.ElideMiddle
                Layout.maximumWidth: 400
                
                // Плавное изменение прозрачности при смене окна
                opacity: 0.9
                Behavior on opacity { NumberAnimation { duration: 200 } }
            }

            // Растягиваем центр, чтобы правая часть прижалась вправо
            Item { Layout.fillWidth: true }

            // ==========================================
            // ПРАВАЯ СЕКЦИЯ: Статус
            // ==========================================
            RowLayout {
                Layout.fillHeight: true
                Layout.alignment: Qt.AlignVCenter
                spacing: 16

                // --- Рабочие столы (Spaces) ---
                // Точки-индикаторы: клик переключает стол через FIFO.
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

                // --- Громкость ---
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
                    // Клик открывает центр управления
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: ShellState.toggleCC()
                    }
                }

                // --- Батарея ---
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
                            // Динамическая иконка батареи: зарядка/уровни
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
                    // Клик открывает центр управления
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: ShellState.toggleCC()
                    }
                }

                // --- Системный трей ---
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

                // --- Часы ---
                Text {
                    font.family: root.uiFont
                    font.pixelSize: 13
                    font.weight: Font.Medium
                    color: "#FFFFFF"
                    text: Qt.formatDateTime(clock.date, "ddd, d MMM  HH:mm")

                    // Клик по часам — центр управления (календарь)
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
    }

    // ==========================================
    // КОМПОНЕНТЫ
    // ==========================================

    // Премиальная кнопка меню с плавным ховером
    component MenuButton: Item {
        id: btn
        property string glyph: ""
        property string label: ""
        // Меню открыто (кнопка «залипает» подсветкой)
        property bool active: false
        signal clicked()

        Layout.fillHeight: true
        Layout.preferredWidth: Math.max(labelText.implicitWidth, glyphText.implicitWidth) + 16

        // Фон с анимацией
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

    // ==========================================
    // ЛОГИКА ГРОМКОСТИ (Оптимизированная)
    // ==========================================
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
                
                // Показываем громкость только если она не 0 и не замьючена, 
                // или можно показывать всегда. Оставим всегда для стабильности UI.
                volumeProc.label = volumeProc.muted ? "Muted" : v + "%";
            }
        }
    }

    // Обновляем громкость раз в 2 секунды (чаще не нужно)
    Timer {
        interval: 2000
        repeat: true
        running: true
        triggeredOnStart: true
        onTriggered: volumeProc.running = true
    }

    // ==========================================
    // РАБОЧИЕ СТОЛЫ: IPC с композитором
    // ==========================================
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
        // Запись в FIFO композитора; читатель (DE) открыт всегда —
        // запись не блокируется.
        command: ["sh", "-c",
                  "printf '%s\\n' 1 > \"$XDG_RUNTIME_DIR/de/ws-cmd\""]
    }

    function wsSwitch(n) {
        wsCmdProc.command = ["sh", "-c",
            "printf '%s\\n' " + n + " > \"$XDG_RUNTIME_DIR/de/ws-cmd\""];
        wsCmdProc.running = true;
    }
}