import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Quickshell
import Quickshell.Io
import Quickshell.Wayland
import Quickshell.Widgets

PanelWindow {
    id: root

    anchors {
        bottom: true
    }
    // Якорь только снизу: композитор сам центрирует окно по горизонтали.
    // Ширина с запасом — видимая «капсула» дока центрируется внутри сама.
    implicitWidth: 900
    implicitHeight: 70

    // Плавающая капсула поверх окон; место снизу резервирует
    // ExclusionMode.Auto.
    WlrLayershell.layer: WlrLayer.Top
    color: "transparent"
    exclusionMode: ExclusionMode.Auto

    readonly property string iconFont: "SF Pro Display, Segoe UI, sans-serif"

    readonly property var pinnedApps: [
        { appId: "Alacritty", name: "Т_TERMINAL", cmd: ["alacritty"], fallback: "utilities-terminal" },
        { appId: "zen", name: "Zen Browser", cmd: ["zen-browser"], fallback: "zen-browser" },
        { appId: "org.gnome.Nautilus", name: "Файлы", cmd: ["nautilus"], fallback: "org.gnome.Nautilus" }
    ]

    property var entryCache: ({})

    function findEntry(appId) {
        if (!appId) return null;
        if (entryCache[appId] !== undefined) return entryCache[appId];
        let entry = null;
        try { entry = DesktopEntries.heuristicLookup(appId); } catch (e) {}
        if (!entry) {
            const ids = [appId, appId.toLowerCase()];
            for (let i = 0; i < ids.length && !entry; i++) {
                try { entry = DesktopEntries.byId(ids[i]); } catch (e2) {}
            }
        }
        entryCache[appId] = entry;
        return entry;
    }

    function iconFor(appId, fallback) {
        const e = findEntry(appId);
        let name = (e && e.icon) ? e.icon : "";
        if (name === "" && fallback) name = fallback;
        if (name === "") name = appId || "";
        if (name === "") return "";
        try { return Quickshell.iconPath(name, "image-missing"); } 
        catch (err) { return name; }
    }

    function isRunning(appId) {
        const ts = ToplevelManager.toplevels;
        for (let i = 0; i < ts.values.length; i++) {
            if (ts.values[i].appId === appId) return true;
        }
        return false;
    }

    // Новая функция для определения активного окна
    function isActive(appId) {
        const active = ToplevelManager.activeToplevel;
        return active && active.appId === appId;
    }

    function launch(cmd) {
        try { Quickshell.execDetached(cmd); return; } catch (e) {}
        spawner.command = cmd;
        spawner.running = true;
    }

    Process { id: spawner }

    // === ЭЛЕМЕНТ ДОКА ===
    // Inline-компонент: каждый экземпляр в Row ниже получает свою копию
    // этого дерева. Фиксированный размер предотвращает «дергание» дока.
    component DockItem: Item {
        id: dockItemTemplate
        property string appName: ""
        property string appIcon: ""
        property bool isRunning: false
        property bool isActive: false
        signal clicked()
        signal middleClicked()

        width: 56
        height: 56

        // Фон при наведении
        Rectangle {
            anchors.centerIn: parent
            width: 48
            height: 48
            radius: 12
            color: dockItemArea.containsMouse ? "#33FFFFFF" : "transparent"
            Behavior on color { ColorAnimation { duration: 150; easing.type: Easing.OutQuad } }
        }

        // Иконка с МАГНИФИКАЦИЕЙ (macOS style)
        IconImage {
            id: iconImg
            anchors.centerIn: parent
            width: 40
            height: 40
            source: dockItemTemplate.appIcon
            asynchronous: true

            // Пружинная анимация
            scale: dockItemArea.containsMouse ? 1.4 : 1.0
            y: dockItemArea.containsMouse ? -8 : 0

            Behavior on scale {
                SpringAnimation { spring: 3; damping: 0.5; duration: 300 }
            }
            Behavior on y {
                SpringAnimation { spring: 3; damping: 0.5; duration: 300 }
            }
        }

        // Индикатор (точка/капсула)
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 4
            height: 4
            radius: 2
            color: dockItemTemplate.isActive ? "#FFFFFF" : "#88FFFFFF"
            visible: dockItemTemplate.isRunning

            // Плавное расширение в капсулу для активного окна
            width: dockItemTemplate.isActive ? 16 : 4
            Behavior on width {
                NumberAnimation { duration: 200; easing.type: Easing.OutQuart }
            }
        }

        // Всплывающая подсказка
        ToolTip {
            visible: dockItemArea.containsMouse && dockItemTemplate.appName !== ""
            delay: 500
            text: dockItemTemplate.appName
            // Стилизация тултипа (работает в большинстве тем)
            background: Rectangle {
                color: "#E61C1C1E"
                border.color: "#33FFFFFF"
                radius: 6
            }
            contentItem: Text {
                text: parent.text
                color: "#FFFFFF"
                font.family: root.iconFont
                font.pixelSize: 12
                font.weight: Font.Medium
            }
        }

        // Область клика
        MouseArea {
            id: dockItemArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton
            onClicked: (mouse) => {
                if (mouse.button === Qt.MiddleButton) {
                    dockItemTemplate.middleClicked();
                } else {
                    dockItemTemplate.clicked();
                }
            }
        }
    }

    // === СТЕКЛЯННЫЙ ФОН ДОКА (Безопасный, без DropShadow) ===
    Rectangle {
        id: dockBg
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 8
        
        // Динамическая ширина с плавным изменением
        width: Math.max(pillRow.implicitWidth + 24, 120)
        height: 56
        radius: 18
        
        // macOS Glassmorphism (темная тема). Для светлой замените на #B3FFFFFF и border на #33000000
        color: "#4D1C1C1E" 
        border.color: "#33FFFFFF"
        border.width: 1

        // Используем Row вместо RowLayout для стабильности размеров иконок
        Row {
            id: pillRow
            anchors.centerIn: parent
            spacing: 6

            // 1. Кнопка Лаунчера
            DockItem {
                appName: "Лаунчер"
                appIcon: root.iconFor("", "image-missing")
                onClicked: ShellState.launcherOpen = !ShellState.launcherOpen
            }

            // Разделитель
            Rectangle {
                width: 1
                height: 32
                color: "#33FFFFFF"
                anchors.verticalCenter: parent.verticalCenter
            }

            // 2. Закрепленные приложения
            Repeater {
                model: root.pinnedApps
                delegate: DockItem {
                    appName: modelData.name
                    appIcon: root.iconFor(modelData.appId, modelData.fallback)
                    isRunning: root.isRunning(modelData.appId)
                    isActive: root.isActive(modelData.appId)
                    onClicked: root.launch(modelData.cmd)
                }
            }

            // Разделитель (показывается только если есть открытые окна)
            Rectangle {
                width: 1
                height: 32
                color: "#33FFFFFF"
                anchors.verticalCenter: parent.verticalCenter
                visible: ToplevelManager.toplevels.values.length > 0
            }

            // 3. Открытые окна
            Repeater {
                model: ToplevelManager.toplevels
                delegate: DockItem {
                    appName: modelData.title || modelData.appId || "Окно"
                    appIcon: root.iconFor(modelData.appId, modelData.appId)
                    isRunning: true
                    isActive: modelData === ToplevelManager.activeToplevel
                    onClicked: {
                        if (modelData.minimized || !modelData.activated) {
                            modelData.activate();
                        } else {
                            modelData.activate();
                        }
                    }
                    onMiddleClicked: modelData.close()
                }
            }
        }
    }
}