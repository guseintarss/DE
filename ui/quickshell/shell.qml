import QtQuick
import Quickshell

// Оболочка DE на QuickShell (стиль macOS Sequoia):
//   BarBackdrop    — подложка попапов бара (меню/центр управления);
//   TopBar         — верхний менюбар;
//   PowerMenu      — меню питания под кнопкой Apple (ПКМ);
//   BarMenu        — выпадающие меню File/Edit/View/Help;
//   ControlCenter  — центр управления (громкость/яркость/медиа/календарь);
//   Dock           — нижний док с открытыми окнами (foreign-toplevel);
//   Launcher       — встроенный лаунчер приложений.
//
// Порядок задаёт Z-стек слоя Top: подложка ниже бара и попапов.
// Запуск: qs -p ui/quickshell  (внутри сессии DE, [shell] builtin = false)
ShellRoot {
    BarBackdrop {}
    TopBar {}
    PowerMenu {}
    BarMenu {}
    ControlCenter {}
    Dock {}
    Launcher {}
}
