import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Qt5Compat.GraphicalEffects

import Quickshell
import Quickshell.Wayland
import Quickshell.Widgets

PanelWindow {
    id: root

    anchors {
        left: true
        right: true
        top: true
        bottom: true
    }
    
    visible: root.shown || root.closing
    color: "transparent"
    focusable: true

    WlrLayershell.layer: WlrLayer.Overlay
    exclusionMode: ExclusionMode.Ignore
    WlrLayershell.keyboardFocus: root.closing ? WlrKeyboardFocus.None : WlrKeyboardFocus.Exclusive

    // === АНИМАЦИЯ ПОЯВЛЕНИЯ/ИСЧЕЗНОВЕНИЯ ===
    property bool closing: false
    property bool shown: false
    property real targetScale: root.shown ? 1.0 : 0.92
    property real targetOpacity: root.shown ? 1.0 : 0.0

    // Храним ссылку на приложение, для которого открыто меню
    property var currentContextMenuEntry: null

    Timer {
        id: focusTimer
        interval: 100
        onTriggered: search.forceActiveFocus()
    }

    Timer {
        id: closeTimer
        interval: 270
        onTriggered: root.closing = false
    }

    property bool cascadeActive: false
    Timer {
        id: cascadeTimer
        interval: 700
        onTriggered: root.cascadeActive = false
    }

    Connections {
        target: ShellState
        function onLauncherOpenChanged() {
            const open = ShellState.launcherOpen;
            if (open) {
                root.shown = true;
                root.closing = false;
                closeTimer.stop();
                
                if (search.text !== "") {
                    search.text = "";
                } else {
                    Qt.callLater(root.refresh);
                }
                
                root.cascadeActive = true;
                cascadeTimer.restart();
                focusTimer.restart();
            } else {
                root.closing = true;
                closeTimer.restart();
                root.shown = false;
            }
        }
    }

    // === КОНТЕКСТНОЕ МЕНЮ ===
    Menu {
        id: contextMenu
        // Стилизация меню под glassmorphism
        background: Rectangle {
            implicitWidth: 200
            radius: 12
            color: "#E61C1C1E" // Чуть менее прозрачный для читаемости
            border.color: "#33FFFFFF"
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
        }

        MenuItem {
            text: "📌 Добавить на рабочий стол"
            font.family: "SF Pro Display, Segoe UI, sans-serif"
            font.pixelSize: 14
            // Стилизация элемента меню
            background: Rectangle {
                color: parent.highlighted ? "#33FFFFFF" : "transparent"
                radius: 8
                anchors.fill: parent
                anchors.margins: 4
            }
            contentItem: Text {
                text: parent.text
                font: parent.font
                color: "#FFFFFF"
                leftPadding: 12
                rightPadding: 12
                verticalAlignment: Text.AlignVCenter
            }
            onTriggered: {
                if (root.currentContextMenuEntry) {
                    root.addToDesktop(root.currentContextMenuEntry);
                }
            }
        }

        MenuSeparator {
            contentItem: Rectangle {
                implicitWidth: 180
                implicitHeight: 1
                color: "#33FFFFFF"
            }
        }

        MenuItem {
            text: "🚀 Добавить в док"
            font.family: "SF Pro Display, Segoe UI, sans-serif"
            font.pixelSize: 14
            background: Rectangle {
                color: parent.highlighted ? "#33FFFFFF" : "transparent"
                radius: 8
                anchors.fill: parent
                anchors.margins: 4
            }
            contentItem: Text {
                text: parent.text
                font: parent.font
                color: "#FFFFFF"
                leftPadding: 12
                rightPadding: 12
                verticalAlignment: Text.AlignVCenter
            }
            onTriggered: {
                if (root.currentContextMenuEntry) {
                    root.addToDock(root.currentContextMenuEntry);
                }
            }
        }
    }

    Item {
        id: contentContainer
        width: 600
        height: 480
        anchors.centerIn: parent
        
        Behavior on scale {
            NumberAnimation { duration: 250; easing.type: Easing.OutQuint }
        }
        Behavior on opacity {
            NumberAnimation { duration: 200; easing.type: Easing.OutQuint }
        }

        scale: targetScale
        opacity: targetOpacity

        Rectangle {
            anchors.fill: parent
            radius: 18
            color: "#CC1C1C1E" 
            border.color: "#33FFFFFF"
            border.width: 1

            layer.enabled: true
            layer.effect: DropShadow {
                transparentBorder: true
                horizontalOffset: 0
                verticalOffset: 6
                radius: 12
                samples: 16
                color: "#66000000"
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                // === ПОЛЕ ПОИСКА ===
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    IconImage {
                        source: Quickshell.iconPath("system-search")
                        width: 20
                        height: 20
                        opacity: search.activeFocus ? 1.0 : 0.55
                        Behavior on opacity { NumberAnimation { duration: 150 } }
                    }

                    TextField {
                        id: search
                        Layout.fillWidth: true
                        font.family: "SF Pro Display, SF Pro Text, Segoe UI, Cantarell, sans-serif"
                        font.pixelSize: 18
                        font.weight: Font.Normal
                        placeholderText: "Поиск приложений"
                        color: "#FFFFFF"
                        selectionColor: "#0A84FF"
                        selectedTextColor: "#FFFFFF"
                        background: Item {} 
                        palette.placeholderText: "#8E8E93"
                        palette.text: "#FFFFFF"

                        onTextChanged: Qt.callLater(root.refresh)
                        
                        Keys.onDownPressed: {
                            if (appGrid.count > 0) {
                                appGrid.currentIndex = 0;
                                appGrid.forceActiveFocus();
                            }
                        }
                        Keys.onEscapePressed: ShellState.closeLauncher()
                        Keys.onReturnPressed: root.launch(0)
                        Keys.onEnterPressed: root.launch(0)
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: "#33FFFFFF"
                }

                // === СЕТКА ПРИЛОЖЕНИЙ (GRID) ===
                GridView {
                    id: appGrid
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    
                    cellWidth: 110
                    cellHeight: 110
                    flow: GridView.FlowLeftToRight
                    
                    currentIndex: 0
                    keyNavigationEnabled: true
                    keyNavigationWraps: false
                    boundsBehavior: Flickable.StopAtBounds
                    flickDeceleration: 1500

                    Keys.onUpPressed: {
                        if (appGrid.currentIndex < 4) {
                            search.forceActiveFocus();
                        } else {
                            appGrid.decrementCurrentIndex();
                        }
                    }

                    delegate: Item {
                        id: gridItem
                        required property var modelData
                        required property int index
                        width: appGrid.cellWidth
                        height: appGrid.cellHeight

                        property real appearP: root.cascadeActive ? 0 : 1
                        opacity: appearP
                        transform: Translate {
                            y: (1 - gridItem.appearP) * 24
                        }
                        
                        Component.onCompleted: {
                            if (root.cascadeActive && index < 40) {
                                cascadeAnim.restart();
                            } else {
                                gridItem.appearP = 1;
                            }
                        }
                        
                        SequentialAnimation {
                            id: cascadeAnim
                            PauseAnimation { duration: gridItem.index * 15 }
                            NumberAnimation {
                                target: gridItem
                                property: "appearP"
                                to: 1
                                duration: 220
                                easing.type: Easing.OutQuad
                            }
                        }

                        Rectangle {
                            id: rowBg
                            anchors.fill: parent
                            anchors.margins: 6
                            radius: 12
                            color: (gridItem.GridView.isCurrentItem || gridArea.containsMouse) ? "#33FFFFFF" : "transparent"
                            Behavior on color { ColorAnimation { duration: 150; easing.type: Easing.OutQuad } }
                        }

                        IconImage {
                            id: appIco
                            width: 48
                            height: 48
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.top: parent.top
                            anchors.topMargin: 10
                            asynchronous: true
                            source: {
                                let n = gridItem.modelData.icon || "";
                                if (n === "") return "";
                                try {
                                    return Quickshell.iconPath(n, "image-missing");
                                } catch (err) {
                                    return n;
                                }
                            }
                            scale: gridArea.containsMouse ? 1.1 : 1.0
                            Behavior on scale { SpringAnimation { spring: 3; damping: 0.5; duration: 300 } }
                        }

                        Text {
                            id: appName
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.top: appIco.bottom
                            anchors.topMargin: 6
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 8
                            width: parent.width - 16
                            
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignTop
                            
                            font.family: "SF Pro Display, Segoe UI, sans-serif"
                            font.pixelSize: 12
                            font.weight: Font.Medium
                            color: "#FFFFFF"
                            elide: Text.ElideRight
                            maximumLineCount: 2
                            wrapMode: Text.WordWrap
                            text: gridItem.modelData.name || ""
                            
                            opacity: gridItem.GridView.isCurrentItem ? 1.0 : 0.85
                            Behavior on opacity { NumberAnimation { duration: 150 } }
                        }

                        // === ОБНОВЛЕННАЯ MOUSEAREA ===
                        MouseArea {
                            id: gridArea
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.LeftButton | Qt.RightButton // Разрешаем правый клик
                            cursorShape: Qt.PointingHandCursor
                            
                            onClicked: (mouse) => {
                                if (mouse.button === Qt.LeftButton) {
                                    appGrid.currentIndex = index;
                                    root.launch(index);
                                } else if (mouse.button === Qt.RightButton) {
                                    appGrid.currentIndex = index;
                                    // Сохраняем текущий элемент и показываем меню
                                    root.currentContextMenuEntry = gridItem.modelData;
                                    
                                    // Позиционируем меню рядом с курсором, но с учетом границ экрана
                                    let globalPos = gridArea.mapToItem(null, mouse.x, mouse.y);
                                    contextMenu.popup(root, globalPos.x, globalPos.y);
                                }
                            }
                            onEntered: appGrid.currentIndex = index
                        }
                    }
                    
                    ScrollBar.vertical: ScrollBar {
                        policy: ScrollBar.AsNeeded
                        contentItem: Rectangle {
                            implicitWidth: 6
                            radius: 3
                            color: parent.pressed ? "#66FFFFFF" : "#33FFFFFF"
                            Behavior on color { ColorAnimation { duration: 200 } }
                        }
                    }
                }
            }
        }
    }

    property var cachedEntries: DesktopEntries.applications
        ? DesktopEntries.applications.values : []
    onCachedEntriesChanged: Qt.callLater(root.refresh)

    function refresh() {
        const q = search.text.toLowerCase();
        const out = [];
        const arr = root.cachedEntries;

        for (let i = 0; i < arr.length; i++) {
            const entry = arr[i];
            if (!entry || entry.noDisplay)
                continue;
            if (q !== "" && (entry.name || "").toLowerCase().indexOf(q) === -1)
                continue;
            out.push(entry);
        }
        
        out.sort((a, b) => {
            const nameA = (a.name || "").toLowerCase();
            const nameB = (b.name || "").toLowerCase();
            if (nameA === q) return -1;
            if (nameB === q) return 1;
            return nameA.localeCompare(nameB);
        });
        
        appGrid.model = out;
    }

    function launch(i) {
        if (i < 0 || i >= appGrid.count)
            return;
        const entry = appGrid.itemAtIndex(i)?.modelData;
        if (!entry)
            return;
        try {
            entry.execute();
        } catch (e) {
            console.warn("launch failed:", e);
        }
        ShellState.closeLauncher();
    }

    // === ФУНКЦИИ ДЛЯ КОНТЕКСТНОГО МЕНЮ ===
    
    function addToDesktop(entry) {
        console.log("Добавление на рабочий стол:", entry.name);
        
        // ПРИМЕЧАНИЕ: QML не имеет прямого доступа к файловой системе для копирования.
        // Вариант 1: Если Quickshell поддерживает выполнение команд, используйте его.
        // Вариант 2: Вызвать внешний bash-скрипт через Qt.openUrlExternally (хак).
        // Пример логики, которую нужно реализовать:
        // 1. Получить путь к .desktop файлу: entry.filePath или entry.path
        // 2. Скопировать его в ~/Desktop/
        // 3. Сделать исполняемым: chmod +x ~/Desktop/filename.desktop
        
        /* Пример (требует адаптации под ваш бэкенд):
        const desktopPath = entry.filePath; // или entry.path
        const scriptUrl = `file:///home/${getUser()}/.local/bin/add-to-desktop.sh "${desktopPath}"`;
        Qt.openUrlExternally(scriptUrl);
        */
       
       console.warn("Функция addToDesktop требует реализации бэкенд-логики (копирование .desktop файла)");
    }

    function addToDock(entry) {
        console.log("Добавление в док:", entry.name);
        
        // ПРИМЕЧАНИЕ: Реализация зависит от того, как написан ваш док.
        // Вариант А: Если док читает JSON-файл конфигурации, добавьте путь entry.filePath в этот массив.
        // Вариант Б: Если док на Quickshell, возможно, у вас есть глобальный объект Settings или JsonConfig, 
        //             в который можно сделать push: DockConfig.pinnedApps.push(entry.filePath)
        
        /* Пример для Quickshell JsonConfig (если используется):
        if (DockSettings && DockSettings.pinned) {
            if (!DockSettings.pinned.includes(entry.filePath)) {
                DockSettings.pinned.push(entry.filePath);
                // Сохранить конфиг
            }
        }
        */
        
        console.warn("Функция addToDock требует реализации логики обновления конфига вашего дока");
    }

    MouseArea {
        anchors.fill: parent
        z: -1
        onClicked: ShellState.closeLauncher()
    }
}