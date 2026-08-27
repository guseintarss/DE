pragma Singleton
import QtQuick
import Quickshell

// Общее состояние оболочки DE (доступ из всех компонентов).
Singleton {
    id: root
    property bool launcherOpen: false

    function toggleLauncher() {
        root.launcherOpen = !root.launcherOpen;
    }

    function closeLauncher() {
        root.launcherOpen = false;
    }

    // === Попапы верхней панели ===
    // "" — закрыто, "menu" — выпадающее меню (barMenuName), "cc" —
    // центр управления (громкость/яркость/батарея/календарь).
    property string barPopup: ""
    property string barMenuName: ""

    function toggleBarMenu(name) {
        if (root.barPopup === "menu" && root.barMenuName === name) {
            root.barPopup = "";
        } else {
            root.barPopup = "menu";
            root.barMenuName = name;
        }
    }

    function toggleCC() {
        root.barPopup = root.barPopup === "cc" ? "" : "cc";
    }

    function closeBarPopup() {
        root.barPopup = "";
    }
}
