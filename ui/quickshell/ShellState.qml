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
}
