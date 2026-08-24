import QtQuick
import Quickshell
import Quickshell.Wayland

// Невидимая подложка на весь экран ПОД лаунчером: любой клик мимо
// панели закрывает его. Обязана инстанцироваться РАНЬШЕ Launcher
// в shell.qml — сцена-ноды слоя Overlay складываются по порядку
// регистрации, и лаунчер останется сверху.
PanelWindow {
    anchors {
        left: true
        right: true
        top: true
        bottom: true
    }
    visible: ShellState.launcherOpen
    color: "transparent"

    WlrLayershell.layer: WlrLayer.Overlay
    exclusionMode: ExclusionMode.Ignore

    // Гасим все события указателя: они не должны просачиваться
    // к окнам под лаунчером.
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onClicked: ShellState.closeLauncher()
        onPositionChanged: (mouse) => mouse.accepted = true
        onWheel: (wheel) => wheel.accepted = true
    }
}
