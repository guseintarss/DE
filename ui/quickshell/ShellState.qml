pragma Singleton
import QtQuick
import Quickshell

// Общее состояние оболочки DE (доступ из всех компонентов).
Singleton {
    id: root
    property bool launcherOpen: false

    function toggleLauncher() {
        console.log("TEMP toggle ->", !root.launcherOpen); // TEMP
        root.launcherOpen = !root.launcherOpen;
    }

    function closeLauncher() {
        console.log("TEMP closeLauncher called @", Date.now()); // TEMP
        root.launcherOpen = false;
    }
}
