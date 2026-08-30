import QtQuick
import QtQuick.Layouts
import Qt5Compat.GraphicalEffects
import Quickshell
import Quickshell.Wayland

// Меню питания под кнопкой Apple (ПКМ по яблоку в TopBar).
// Это отдельное окно слоя Top, иначе выпадающий блок обрезался бы
// границами 36px-поверхности менюбара.
PanelWindow {
    id: root

    anchors {
        top: true
        left: true
    }
    margins {
        top: 40   // высота бара 36 + зазор 4
        left: 12  // под кнопкой Apple (левый отступ бара)
    }
    implicitWidth: 210
    implicitHeight: menuCol.implicitHeight + 16
    visible: ShellState.appleMenuOpen
    color: "transparent"

    WlrLayershell.layer: WlrLayer.Overlay
    exclusionMode: ExclusionMode.Ignore

    readonly property string uiFont: "SF Pro Display, Segoe UI, Cantarell, sans-serif"

    // Закрыть по Esc; клик мимо закрывает BarBackdrop.
    focusable: true

    // Keys нельзя вешать на PanelWindow (это не Item) — ставим на внутренний
    // фокусируемый элемент.
    Item {
        anchors.fill: parent
        focus: true
        Keys.onEscapePressed: ShellState.closePowerMenu()
    }

    Rectangle {
        anchors.fill: parent
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
            id: menuCol
            anchors.centerIn: parent
            width: parent.width - 12
            spacing: 4

            Repeater {
                model: [
                    { text: "Sleep", action: "sleep" },
                    { text: "separator" },
                    { text: "Restart", action: "restart" },
                    { text: "Shutdown", action: "shutdown" },
                    { text: "separator" },
                    { text: "Log Out", action: "logout" }
                ]
                delegate: Item {
                    id: entry
                    required property var modelData
                    width: menuCol.width
                    height: modelData === "separator" ? 1 : 32

                    Rectangle {
                        anchors.fill: parent
                        color: "#33FFFFFF"
                        visible: entry.modelData === "separator"
                    }

                    // Ховер в стиле macOS: акцентная подложка + сдвиг текста.
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 2
                        radius: 6
                        color: itemArea.containsMouse ? "#FF0A84FF" : "transparent"
                        Behavior on color { ColorAnimation { duration: 120; easing.type: Easing.OutQuad } }
                        visible: entry.modelData !== "separator"
                    }

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: itemArea.containsMouse ? 20 : 12
                        Behavior on anchors.leftMargin { NumberAnimation { duration: 100; easing.type: Easing.OutQuad } }
                        text: (entry.modelData === "separator" ? "" : entry.modelData.text)
                        color: "#FFFFFF"
                        opacity: itemArea.containsMouse ? 1.0 : 0.85
                        Behavior on opacity { NumberAnimation { duration: 120 } }
                        font.family: root.uiFont
                        font.pixelSize: 13
                        font.weight: Font.Medium
                    }

                    MouseArea {
                        id: itemArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        visible: entry.modelData !== "separator"
                        onClicked: {
                            root.executePowerAction(entry.modelData.action);
                            ShellState.closePowerMenu();
                        }
                    }
                }
            }
        }
    }

    // === ВЫПОЛНЕНИЕ POWER КОМАНД ===
    function executePowerAction(action) {
        console.log("Power action:", action);

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
                // Выход через loginctl (терминирует пользовательскую сессию).
                args = ["loginctl", "terminate-session", Quickshell.env("XDG_SESSION_ID") || ""];
                break;
        }

        if (args.length > 0) {
            if (args[args.length - 1] === "") {
                // Нет XDG_SESSION_ID — завершаем собственную сессию по имени пользователя.
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