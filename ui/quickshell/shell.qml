import QtQuick
import Quickshell

// Оболочка DE на QuickShell (стиль macOS Sequoia):
//   TopBar    — верхний менюбар;
//   Dock      — нижний док с открытыми окнами (foreign-toplevel);
//   Launcher  — встроенный лаунчер приложений.
//
// Запуск: qs -p ui/quickshell  (внутри сессии DE, [shell] builtin = false)
ShellRoot {
    TopBar {}
    Dock {}
    Launcher {}
}
