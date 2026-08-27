import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Wayland

// Выпадающее меню File/Edit/View/Help под верхней панелью.
// Контент зависит от ShellState.barMenuName; пункты View переключают
// рабочие столы через FIFO композитора.
PanelWindow {
    id: root

    anchors {
        top: true
        left: true
    }
    margins {
        top: 40   // высота бара 36 + зазор 4
        left: 8
    }
    implicitWidth: 250
    implicitHeight: menuCol.implicitHeight + 16
    visible: ShellState.barPopup === "menu"
    color: "transparent"

    WlrLayershell.layer: WlrLayer.Top
    exclusionMode: ExclusionMode.Ignore

    // Число столов — из файла состояния композитора
    property int wsCount: 1
    FileView {
        id: wsStateFile
        path: Quickshell.env("XDG_RUNTIME_DIR") + "/de/workspaces"
        watchChanges: true
        onFileChanged: wsStateFile.reload()
        onLoaded: {
            const parts = wsStateFile.text().trim().split(" ");
            if (parts.length === 2 && !isNaN(parseInt(parts[1]))) {
                root.wsCount = Math.max(1, parseInt(parts[1]));
            }
        }
    }

    // Пункты меню: {label, enabled, cmd} | {sep: true}
    readonly property var menuItems: {
        let items = [];
        const active = ToplevelManager.activeToplevel;
        if (ShellState.barMenuName === "File") {
            items = [
                { label: "Новое окно терминала", enabled: true,
                  cmd: ["alacritty"] },
                { sep: true },
                { label: "Закрыть окно", enabled: active !== null,
                  close: true },
            ];
        } else if (ShellState.barMenuName === "Edit") {
            items = [
                { label: "Копировать", enabled: false },
                { label: "Вставить", enabled: false },
                { label: "Выделить всё", enabled: false },
            ];
        } else if (ShellState.barMenuName === "View") {
            items = [
                { label: "Следующий стол", enabled: true, ws: "next" },
                { label: "Предыдущий стол", enabled: true, ws: "prev" },
                { sep: true },
            ];
            for (let i = 1; i <= root.wsCount; i++) {
                items.push({ label: "Рабочий стол " + i, enabled: true,
                             ws: String(i) });
            }
        } else if (ShellState.barMenuName === "Help") {
            items = [
                { label: "Терминал", enabled: true,
                  cmd: ["alacritty"] },
                { sep: true },
                { label: "О системе", enabled: false },
            ];
        }
        return items;
    }

    function runItem(item) {
        if (item.cmd) {
            cmdProc.command = item.cmd;
            cmdProc.running = true;
        } else if (item.ws !== undefined) {
            wsSend.command = ["sh", "-c",
                "printf '%s\\n' " + item.ws +
                " > \"$XDG_RUNTIME_DIR/de/ws-cmd\""];
            wsSend.running = true;
        } else if (item.close && ToplevelManager.activeToplevel) {
            ToplevelManager.activeToplevel.close();
        }
        ShellState.closeBarPopup();
    }

    Process { id: cmdProc }
    Process { id: wsSend }

    Rectangle {
        anchors.fill: parent
        radius: 12
        color: "#E61C1C1E"
        border.color: "#33FFFFFF"
        border.width: 1

        Column {
            id: menuCol
            anchors.centerIn: parent
            width: parent.width - 12
            spacing: 2

            Repeater {
                // Модель — число пунктов; данные берём по индексу:
                // required modelData на Loader в этой сборке qs не
                // инициализируется (меню рендерилось пустым/белым).
                model: root.menuItems.length
                delegate: Item {
                    id: menuEntry
                    required property int index
                    readonly property var itemData: root.menuItems[index]
                    width: menuCol.width
                    height: itemData && itemData.sep ? 1 : 30

                    // Разделитель
                    Rectangle {
                        anchors.fill: parent
                        color: "#33FFFFFF"
                        visible: menuEntry.itemData !== undefined &&
                                 menuEntry.itemData.sep === true
                    }

                    // Пункт меню
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 1
                        radius: 8
                        color: itemArea.containsMouse &&
                               menuEntry.itemData && menuEntry.itemData.enabled
                               ? "#33FFFFFF" : "transparent"
                        Behavior on color { ColorAnimation { duration: 120 } }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        text: menuEntry.itemData ? menuEntry.itemData.label : ""
                        color: "#FFFFFF"
                        opacity: menuEntry.itemData && menuEntry.itemData.enabled
                                 ? 0.95 : 0.4
                        font.family: "SF Pro Display, Segoe UI, Cantarell, sans-serif"
                        font.pixelSize: 13
                    }
                    MouseArea {
                        id: itemArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: menuEntry.itemData && menuEntry.itemData.enabled
                                     ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: {
                            if (menuEntry.itemData && menuEntry.itemData.enabled) {
                                root.runItem(menuEntry.itemData);
                            }
                        }
                    }
                }
            }
        }
    }
}
