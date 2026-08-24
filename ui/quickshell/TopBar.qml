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

    // === СТЕКЛЯННЫЙ ФОН МЕНЮБАРА ===
    Rectangle {
        anchors.fill: parent
        // Темное стекло (для светлой темы замените на "#B3FFFFFF" и border на "#33000000")
        color: "#991C1C1E" 
        border.color: "#33FFFFFF"
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
                        label: modelData
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

                // --- Громкость ---
                RowLayout {
                    spacing: 6
                    visible: volumeProc.label !== "" || volumeProc.muted
                    
                    Text {
                        font.family: root.iconFont
                        font.pixelSize: 14
                        color: "#FFFFFF"
                        // Иконка динамика: перечеркнутый если muted, обычный если нет
                        text: volumeProc.muted ? "" : "" 
                    }
                    Text {
                        font.family: root.uiFont
                        font.pixelSize: 13
                        font.weight: Font.Medium
                        color: "#FFFFFF"
                        text: volumeProc.label
                    }
                }

                // --- Батарея ---
                RowLayout {
                    spacing: 6
                    visible: UPower.displayDevice && UPower.displayDevice.percentage > 0
                    
                    Text {
                        font.family: root.iconFont
                        font.pixelSize: 14
                        color: "#FFFFFF"
                        // Динамическая иконка батареи в зависимости от заряда и состояния
                        text: {
                            const d = UPower.displayDevice;
                            const pct = d.percentage;
                            if (d.state === UPowerDeviceState.Charging) return ""; // Молния
                            if (pct > 80) return "";
                            if (pct > 60) return "";
                            if (pct > 40) return "";
                            if (pct > 20) return "";
                            return "";
                        }
                    }
                    Text {
                        font.family: root.uiFont
                        font.pixelSize: 13
                        font.weight: Font.Medium
                        color: "#FFFFFF"
                        text: Math.round(UPower.displayDevice.percentage) + "%"
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
        signal clicked()

        Layout.fillHeight: true
        Layout.preferredWidth: Math.max(labelText.implicitWidth, glyphText.implicitWidth) + 16

        // Фон с анимацией
        Rectangle {
            anchors.fill: parent
            anchors.margins: 4
            radius: 6
            color: btnArea.containsMouse ? "#33FFFFFF" : "transparent"
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
}