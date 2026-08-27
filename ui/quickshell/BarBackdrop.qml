import QtQuick
import Quickshell
import Quickshell.Wayland

// Невидимая подложка на время попапов верхней панели (меню/центр
// управления): клик мимо закрывает. Регистрируется ПЕРВОЙ в shell.qml —
// ниже TopBar и самих попапов, но выше обычных окон (слой Top).
PanelWindow {
    anchors {
        left: true
        right: true
        top: true
        bottom: true
    }
    visible: ShellState.barPopup !== ""
    color: "transparent"

    WlrLayershell.layer: WlrLayer.Top
    exclusionMode: ExclusionMode.Ignore

    MouseArea {
        anchors.fill: parent
        onClicked: ShellState.closeBarPopup()
    }
}
