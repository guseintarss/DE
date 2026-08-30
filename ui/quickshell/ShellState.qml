pragma Singleton
import QtQuick
import Quickshell

// Общее состояние оболочки DE (доступ из всех компонентов).
Singleton {
    id: root
    property bool launcherOpen: false

    // Меню питания (ПКМ по кнопке Apple). Отдельное окно PowerMenu.
    property bool appleMenuOpen: false

    function toggleLauncher() {
        root.launcherOpen = !root.launcherOpen;
        if (root.launcherOpen) {
            root.barPopup = "";
            root.appleMenuOpen = false;
        }
    }

    function closeLauncher() {
        root.launcherOpen = false;
    }

    function openPowerMenu() {
        root.barPopup = "";
        root.launcherOpen = false;
        root.appleMenuOpen = true;
    }

    function closePowerMenu() {
        root.appleMenuOpen = false;
    }

    // === Попапы верхней панели ===
    // "" — закрыто, "menu" — выпадающее меню (barMenuName), "cc" —
    // центр управления (громкость/яркость/батарея/календарь).
    property string barPopup: ""
    property string barMenuName: ""

    function toggleBarMenu(name) {
        root.appleMenuOpen = false;
        if (root.barPopup === "menu" && root.barMenuName === name) {
            root.barPopup = "";
        } else {
            root.barPopup = "menu";
            root.barMenuName = name;
        }
    }

    function toggleCC() {
        root.appleMenuOpen = false;
        root.barPopup = root.barPopup === "cc" ? "" : "cc";
    }

    function closeBarPopup() {
        root.barPopup = "";
    }
}
