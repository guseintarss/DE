import QtQuick
import QtQuick.Controls.Basic
import Quickshell
import Quickshell.Wayland
import Quickshell.Widgets

// Встроенный лаунчер приложений (в системе нет wofi/rofi):
// стеклянная панель под менюбаром, поиск по .desktop, Enter/клик — запуск.
PanelWindow {
    id: root

    anchors {
        top: true
        left: true
    }
    margins {
        top: 38
        left: 8
    }
    implicitWidth: 420
    implicitHeight: 480
    visible: ShellState.launcherOpen
    color: "transparent"
    focusable: true

    WlrLayershell.keyboardFocus: WlrKeyboardFocus.Exclusive

    onVisibleChanged: {
        if (visible) {
            search.text = "";
            refresh();
            search.forceActiveFocus();
        }
    }

    function refresh() {
        const q = search.text.toLowerCase();
        const out = [];
        let arr = null;
        try {
            arr = DesktopEntries.applications.values;
        } catch (e) {
            arr = DesktopEntries.applications;
        }
        for (let i = 0; i < arr.length; i++) {
            const entry = arr[i];
            if (!entry || entry.noDisplay)
                continue;
            if (q !== "" && (entry.name || "").toLowerCase().indexOf(q) === -1)
                continue;
            out.push(entry);
        }
        out.sort((a, b) => (a.name || "").localeCompare(b.name || ""));
        appList.model = out;
    }

    Rectangle {
        anchors.fill: parent
        radius: 12
        color: "#D9FFFFFF"
        border.color: "#33000000"
        border.width: 1

        Column {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 8

            TextField {
                id: search
                width: parent.width
                font.family: "SF Pro Display"
                font.pixelSize: 14
                placeholderText: "Поиск приложений"
                background: Rectangle {
                    radius: 8
                    color: "#33FFFFFF"
                    border.color: search.activeFocus ? "#66007AFF" : "#22000000"
                }
                onTextChanged: root.refresh()
                Keys.onEscapePressed: ShellState.closeLauncher()
                Keys.onReturnPressed: launch(0)
                Keys.onEnterPressed: launch(0)
                Keys.onDownPressed: appList.incrementCurrentIndex()
                Keys.onUpPressed: appList.decrementCurrentIndex()
            }

            ListView {
                id: appList
                width: parent.width
                height: parent.height - search.height - 8
                clip: true
                spacing: 2
                currentIndex: 0
                keyNavigationEnabled: true

                delegate: Item {
                    id: row
                    required property var modelData
                    width: appList.width
                    height: 40

                    Rectangle {
                        anchors.fill: parent
                        radius: 8
                        color: (row.ListView.isCurrentItem
                                || rowArea.containsMouse) ? "#26000000" : "transparent"
                    }
                    IconImage {
                        id: appIco
                        x: 6
                        anchors.verticalCenter: parent.verticalCenter
                        width: 28
                        height: 28
                        source: row.modelData.icon || ""
                        asynchronous: true
                    }
                    Text {
                        x: 44
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 52
                        font.family: "SF Pro Display"
                        font.pixelSize: 13
                        color: "#E6000000"
                        elide: Text.ElideRight
                        text: row.modelData.name || ""
                    }
                    MouseArea {
                        id: rowArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            appList.currentIndex = index;
                            root.launch(index);
                        }
                        onEntered: appList.currentIndex = index
                    }
                }
            }
        }
    }

    // Запуск приложения по индексу списка и закрытие лаунчера.
    function launch(i) {
        if (i < 0 || i >= appList.count)
            return;
        const entry = appList.itemAtIndex(i)?.modelData;
        if (!entry)
            return;
        try {
            entry.execute();
        } catch (e) {
            console.warn("launch failed:", e);
        }
        ShellState.closeLauncher();
    }

    // Клик мимо панели — закрыть.
    MouseArea {
        anchors.fill: parent
        z: -1
        onClicked: ShellState.closeLauncher()
    }
}
