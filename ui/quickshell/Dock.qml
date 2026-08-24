import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Quickshell
import Quickshell.Wayland
import Quickshell.Widgets

// Нижний док macOS Sequoia: кнопка лаунчера + иконки всех открытых
// окон (zwlr-foreign-toplevel). Клик — фокус/поднять, средняя кнопка —
// закрыть. Точка под иконкой — сфокусированное окно.
PanelWindow {
    id: root

    anchors {
        bottom: true
        left: true
        right: true
    }
    implicitHeight: 58
    color: "transparent"
    exclusionMode: ExclusionMode.Auto

    readonly property string iconFont: "MesloLGLDZ Nerd Font"

    // Кэш: appId -> DesktopEntry (для иконок).
    property var entryCache: ({})

    function findEntry(appId) {
        if (!appId)
            return null;
        if (entryCache[appId] !== undefined)
            return entryCache[appId];
        let entry = null;
        try {
            entry = DesktopEntries.heuristicLookup(appId);
        } catch (e) {
            try {
                entry = DesktopEntries.byId(appId.toLowerCase());
            } catch (e2) {
                entry = null;
            }
        }
        entryCache[appId] = entry;
        return entry;
    }

    function iconFor(appId) {
        const e = findEntry(appId);
        return e && e.icon ? e.icon : "";
    }

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 6
        width: Math.max(pillRow.implicitWidth + 24, 120)
        height: 46
        radius: 14
        color: "#A6FFFFFF"
        border.color: "#26000000"
        border.width: 1

        RowLayout {
            id: pillRow
            anchors.fill: parent
            anchors.margins: 5
            spacing: 4

            Item {
                Layout.fillHeight: true
                Layout.preferredWidth: 36

                Rectangle {
                    anchors.fill: parent
                    radius: 8
                    color: launcherArea.containsMouse ? "#26000000" : "transparent"
                }
                Text {
                    anchors.centerIn: parent
                    font.family: root.iconFont
                    font.pixelSize: 22
                    color: "#1A1A1ACC"
                    text: ""
                }
                MouseArea {
                    id: launcherArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: ShellState.toggleLauncher()
                }
            }

            Rectangle {
                Layout.preferredHeight: 26
                Layout.preferredWidth: 1
                color: "#33000000"
            }

            Repeater {
                model: ToplevelManager.toplevels

                delegate: Item {
                    id: appIcon

                    required property var modelData

                    property bool focused: modelData === ToplevelManager.activeToplevel

                    Layout.fillHeight: true
                    Layout.preferredWidth: 42

                    Rectangle {
                        anchors.fill: parent
                        radius: 8
                        color: appArea.containsMouse ? "#1F000000" : "transparent"
                    }
                    IconImage {
                        anchors.centerIn: parent
                        width: 34
                        height: 34
                        source: root.iconFor(appIcon.modelData.appId)
                        asynchronous: true
                    }
                    // Индикатор активного окна.
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.bottom
                        width: 4
                        height: 4
                        radius: 2
                        color: "#E6000000"
                        visible: appIcon.focused
                    }
                    ToolTip.visible: appArea.containsMouse
                                     && appIcon.modelData.title !== ""
                    ToolTip.delay: 400
                    ToolTip.text: appIcon.modelData.title || ""

                    MouseArea {
                        id: appArea
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                        onClicked: (mouse) => {
                            if (mouse.button === Qt.MiddleButton) {
                                appIcon.modelData.close();
                                return;
                            }
                            if (appIcon.modelData.minimized
                                || !appIcon.modelData.activated)
                                appIcon.modelData.activate();
                            else
                                appIcon.modelData.activate(); // повторный клик — просто фокус
                        }
                    }
                }
            }
        }
    }
}
