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
    // Высота с запасом сверху: там рисуется лейбл приложения.
    implicitWidth: 900
    implicitHeight: 140
    // Без этого окно рисуется дефолтным белым фоном Qt.
    color: "transparent"

    // Плавающая капсула поверх окон; место снизу резервирует
    // ExclusionMode.Auto.
    WlrLayershell.layer: WlrLayer.Top
    // Резерв под капсулу (56px) + зазор (8px). Режим Ignore с явной
    // зоной: Auto резервирует ВСЮ высоту окна (140px — с запасом под
    // лейблы), из-за чего максимизированные окна висели над капсулой.
    exclusionMode: ExclusionMode.Ignore
    WlrLayershell.exclusiveZone: 64

    // Ввод принимает капсула дока + открытое меню (оно рисуется над
    // капсулой; без включения в маску клики по пунктам проваливаются
    // сквозь окно). Скрытое меню не даёт области ввода.
    mask: Region {
        Region { item: dockBg }
        Region { item: contextMenu }
    }

    readonly property string iconFont: "SF Pro Display, Segoe UI, sans-serif"

    // === ОБЩИЙ ЛЕЙБЛ ПРИЛОЖЕНИЯ (над доком, стиль macOS) ===
    property Item hoveredOwner: null
    property string hoveredName: ""
    property real hoveredX: 0 // центр иконки в координатах окна

    function hoverLabel(owner, name, centerX) {
        hoveredOwner = owner;
        hoveredName = name;
        hoveredX = centerX;
    }

    function unhoverLabel(owner) {
        if (hoveredOwner === owner) {
            hoveredOwner = null;
            hoveredName = "";
        }
    }

    // === КОНТЕКСТНОЕ МЕНЮ (правый клик по иконке) ===
    property var menuTarget: null // {name, appId, cmd, pinned, running, toplevel}
    property real menuX: 0

    function openMenu(target, centerX) {
        menuTarget = target;
        menuX = centerX;
    }

    function closeMenu() {
        menuTarget = null;
    }

    // Закрыть все окна приложения (по appId)
    function closeAllByAppId(appId) {
        const ts = ToplevelManager.toplevels.values;
        for (let i = 0; i < ts.length; i++) {
            if (ts[i].appId === appId) {
                ts[i].close();
            }
        }
    }

    // Плашка меню над иконкой; пункты зависят от пина/запущенности.
    Rectangle {
        id: contextMenu
        visible: root.menuTarget !== null
        opacity: visible ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 120 } }
        z: 60 // над подложкой закрытия

        readonly property var items: {
            const t = root.menuTarget;
            if (t === null) return [];
            const list = [];
            if (t.pinned) list.push("Запустить");
            if (t.running) list.push("Закрыть");
            return list;
        }

        readonly property real pillTop: parent.height - 64
        width: Math.max(menuCol.implicitWidth + 16, 140)
        height: menuCol.implicitHeight + 12
        x: Math.max(8, Math.min(parent.width - width - 8, root.menuX - width / 2))
        y: pillTop - height - 6
        radius: 10
        color: "#E61C1C1E"
        border.color: "#33FFFFFF"
        border.width: 1

        Column {
            id: menuCol
            anchors.centerIn: parent
            spacing: 2

            Repeater {
                model: contextMenu.items
                delegate: Item {
                    required property string modelData
                    required property int index
                    // Фиксированная ширина: ширина меню считается из
                    // implicitWidth колонки — зависеть от неё нельзя
                    // (цикл polish, меню пустое и раздутое).
                    width: 128
                    height: 24
                    x: 6

                    Rectangle {
                        anchors.fill: parent
                        radius: 6
                        color: itemArea.containsMouse ? "#33FFFFFF" : "transparent"
                        Behavior on color { ColorAnimation { duration: 120 } }
                    }
                    Text {
                        anchors.centerIn: parent
                        text: parent.modelData
                        color: "#FFFFFF"
                        font.family: root.iconFont
                        font.pixelSize: 12
                    }
                    MouseArea {
                        id: itemArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            // ВАЖНО: действие — ДО closeMenu. closeMenu
                            // обнуляет menuTarget, Repeater уничтожает
                            // делегат, и контекст обработчика умирает —
                            // любой доступ после закрытия бросает
                            // ReferenceError (действие молча пропадает).
                            const t = root.menuTarget;
                            const action = modelData;
                            if (t === null) return;
                            if (action === "Запустить" && t.cmd) {
                                root.launch(t.cmd);
                            } else if (action === "Закрыть") {
                                if (t.toplevel) {
                                    t.toplevel.close();
                                } else if (t.appId) {
                                    root.closeAllByAppId(t.appId);
                                }
                            }
                            root.closeMenu();
                        }
                    }
                }
            }
        }
    }

    // Невидимая подложка поверх дока на время меню: клик мимо пунктов
    // закрывает меню, не задевая иконки.
    MouseArea {
        anchors.fill: parent
        visible: root.menuTarget !== null
        enabled: visible
        z: 50
        onClicked: root.closeMenu()
    }

    // Плашка с именем приложения над капсулой
    Rectangle {
        id: appLabel
        visible: opacity > 0.01
        opacity: root.hoveredName === "" ? 0 : 1
        Behavior on opacity { NumberAnimation { duration: 120; easing.type: Easing.OutQuad } }

        readonly property real pillTop: parent.height - 64 // 56 капсула + 8 отступ
        y: pillTop - height - 6
        x: Math.max(8, Math.min(parent.width - width - 8, root.hoveredX - width / 2))

        width: Math.min(labelText.implicitWidth + 20, 280)
        height: 26
        radius: 8
        color: "#E61C1C1E"
        border.color: "#33FFFFFF"
        border.width: 1

        Text {
            id: labelText
            anchors.centerIn: parent
            width: Math.min(implicitWidth, parent.width - 12)
            elide: Text.ElideMiddle
            text: root.hoveredName
            color: "#FFFFFF"
            font.family: root.iconFont
            font.pixelSize: 12
            font.weight: Font.Medium
        }
    }

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
        signal rightClicked(real centerX)

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

        // Область клика
        MouseArea {
            id: dockItemArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
            // Имя показывается в общем лейбле НАД доком; координаты
            // считаем В МОМЕНТ события: mapToItem в биндинге оценивается
            // один раз при создании (геометрия ещё нулевая) — лейблы
            // «слипались» слева.
            onHoveredChanged: {
                if (containsMouse) {
                    const p = dockItemTemplate.mapToItem(
                        root.contentItem, dockItemTemplate.width / 2, 0);
                    root.hoverLabel(dockItemTemplate, dockItemTemplate.appName,
                                    p.x);
                } else {
                    root.unhoverLabel(dockItemTemplate);
                }
            }
            onClicked: (mouse) => {
                if (mouse.button === Qt.MiddleButton) {
                    dockItemTemplate.middleClicked();
                } else if (mouse.button === Qt.RightButton) {
                    dockItemTemplate.rightClicked(
                        dockItemTemplate.mapToItem(
                            root.contentItem, dockItemTemplate.width / 2, 0).x);
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
                    onRightClicked: (cx) => root.openMenu(
                        { name: appName, appId: modelData.appId,
                          cmd: modelData.cmd, pinned: true,
                          running: root.isRunning(modelData.appId),
                          toplevel: null },
                        cx)
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
                    onRightClicked: (cx) => root.openMenu(
                        { name: appName, appId: modelData.appId,
                          cmd: null, pinned: false, running: true,
                          toplevel: modelData },
                        cx)
                }
            }
        }
    }
}