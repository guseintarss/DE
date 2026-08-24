import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Quickshell
import Quickshell.Io
import Quickshell.Wayland
import Quickshell.Widgets

// Нижний док macOS Sequoia: кнопка лаунчера + закреплённые лаунчеры
// (терминал, браузер, файлы) + иконки открытых окон
// (zwlr-foreign-toplevel). Клик по окну — фокус/поднять, средняя кнопка —
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

    // Закреплённые лаунчеры: всегда в доке слева от открытых окон.
    // fallback — имя из темы иконок, если .desktop не нашёлся.
    readonly property var pinnedApps: [
        {
            appId: "Alacritty",
            name: "Терминал",
            cmd: ["alacritty"],
            fallback: "utilities-terminal"
        },
        {
            appId: "zen",
            name: "Zen Browser",
            cmd: ["zen-browser"],
            fallback: "zen-browser"
        },
        {
            appId: "org.gnome.Nautilus",
            name: "Файлы",
            cmd: ["nautilus"],
            fallback: "org.gnome.Nautilus"
        }
    ]

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
            entry = null;
        }
        if (!entry) {
            const ids = [appId, appId.toLowerCase()];
            for (let i = 0; i < ids.length && !entry; i++) {
                try {
                    entry = DesktopEntries.byId(ids[i]);
                } catch (e2) {
                    entry = null;
                }
            }
        }
        entryCache[appId] = entry;
        return entry;
    }

    // Иконка для окна/лаунчера: .desktop -> fallback -> само appId.
    // IconImage не резолвит имена темы сам — через Quickshell.iconPath.
    function iconFor(appId, fallback) {
        const e = findEntry(appId);
        let name = e && e.icon && e.icon !== "" ? e.icon : "";
        if (name === "" && fallback && fallback !== "")
            name = fallback;
        if (name === "")
            name = appId || "";
        if (name === "")
            return "";
        try {
            return Quickshell.iconPath(name, "image-missing");
        } catch (err) {
            return name;
        }
    }

    // Есть ли среди открытых окон приложение с таким app_id.
    function isRunning(appId) {
        const ts = ToplevelManager.toplevels;
        for (let i = 0; i < ts.values.length; i++) {
            if (ts.values[i].appId === appId)
                return true;
        }
        return false;
    }

    function launch(cmd) {
        try {
            Quickshell.execDetached(cmd);
            return;
        } catch (e) {}
        spawner.command = cmd;
        spawner.running = true;
    }

    Process {
        id: spawner
    }

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 6
        width: Math.max(pillRow.implicitWidth + 24, 120)
        height: 46
        radius: 14
        color: '#3fffffff'
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

                Behavior on scale {
                    SpringAnimation {
                        duration: 400
                        spring: 3       // Жёсткость пружины
                        damping: 0.4    // Затухание (меньше = больше bounce)
                    }
                }
        
                Behavior on y {
                    SpringAnimation {
                        duration: 400
                        spring: 2
                        damping: 0.5
                    }
                }

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
                    anchors.fill: parent
                    hoverEnabled: true
                    onEntered: {
                        icon.scale = 1.4
                        icon.y = -15
                    }
                    onExited: {
                        icon.scale = 1.0
                        icon.y = 0
                    }
                }
            }

            Rectangle {
                Layout.preferredHeight: 26
                Layout.preferredWidth: 1
                color: "#33000000"
            }

            // --- Закреплённые лаунчеры ---
            Repeater {
                model: root.pinnedApps

                delegate: Item {
                    id: pinIcon

                    required property var modelData

                    Layout.fillHeight: true
                    Layout.preferredWidth: 42

                    Rectangle {
                        anchors.fill: parent
                        radius: 8
                        color: pinArea.containsMouse ? "#1F000000" : "transparent"
                    }

        

                    IconImage {
                        anchors.centerIn: parent
                        width: 34
                        height: 34
                        source: root.iconFor(pinIcon.modelData.appId,
                                             pinIcon.modelData.fallback)
                        asynchronous: true
                    }
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.bottom
                        width: 4
                        height: 4
                        radius: 2
                        color: "#E6000000"
                        visible: root.isRunning(pinIcon.modelData.appId)
                    }
                    ToolTip.visible: pinArea.containsMouse
                    ToolTip.delay: 400
                    ToolTip.text: pinIcon.modelData.name
                    MouseArea {
                        id: pinArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.launch(pinIcon.modelData.cmd)
                    }
                }
            }

            Rectangle {
                Layout.preferredHeight: 26
                Layout.preferredWidth: 1
                color: root.pinnedApps.length > 0 ? "#33000000" : "transparent"
            }

            Repeater {
                model: ToplevelManager.toplevels

                delegate: Item {
                    id: appIcon

                    required property var modelData

                    property bool focused: modelData === ToplevelManager.activeToplevel
                    // Окно закреплённого приложения — его иконка уже в доке.
                    property bool pinnedDup: {
                        const pins = root.pinnedApps;
                        for (let i = 0; i < pins.length; i++) {
                            if (pins[i].appId === modelData.appId)
                                return true;
                        }
                        return false;
                    }

                    visible: !pinnedDup
                    Layout.fillHeight: true
                    Layout.preferredWidth: visible ? 42 : 0

                    Rectangle {
                        anchors.fill: parent
                        radius: 8
                        color: appArea.containsMouse ? "#1F000000" : "transparent"
                    }
                    IconImage {
                        anchors.centerIn: parent
                        width: 34
                        height: 34
                        source: root.iconFor(appIcon.modelData.appId, appIcon.modelData.appId)
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
