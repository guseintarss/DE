# Оболочка DE: внешний UI поверх композитора

С момента миграции на Wayfire оболочка всегда внешняя: десктопные панели
рисуются клиентами по `zwlr-layer-shell-v1`, список окон публикует сам
wayfire через `wlr-foreign-toplevel-management-v1`. Встроенной оболочки
(как в прежнем композиторе в `legacy/`) больше нет.

- Автозапуск оболочки и обоев при старте сессии — в `config/wayfire.ini`,
  секция `[autostart]` (`shell = qs -p @DE_ROOT@/ui/quickshell`,
  `wallpaper = de-wallpaper`).
- Клавиатурное переключение окон/фокус даёт wayfire (`[switcher]`,
  `[fast-switcher]`), а не шелл.
-` к процессу wayfire применяется foreign-toplevel протоколом.

## Протоколы

| Протокол | Кто предоставляет | Зачем |
|---|---|---|
| `zwlr-layer-shell-v1` | wayfire | панели по краям экрана, эксклюзивные зоны (окна не залезают под бар), клавиатурный фокус для лаунчеров |
| `wlr-foreign-toplevel-management-v1` | wayfire | список окон, заголовки/app_id/состояния, управление (activate/close/min/max) |
| `zxdg-output-manager` (wlroots) | wayfire | имена/геометрия выходов; обязателен для waybar |

Эксклюзивные зоны слоёв wayfire учитывает при максимизации/тайлинге окон
из коробки (`[simple-tile]`, `[grid]`).

## Основная оболочка: QuickShell (`ui/quickshell/`)

Установлена в системе (`qs`, пакет noctalia-qs). Состав:

- `TopBar.qml` — верхний менюбар macOS Sequoia: кнопка-лаунчер,
  File/Edit/View/Help, в центре имя активного окна, справа громкость
  (wpctl), батарея (UPower), трей (StatusNotifier), часы.
- `Dock.qml` — нижний док: лаунчер + иконки всех открытых окон
  (`ToplevelManager`); клик — фокус, средняя кнопка — закрыть, точка =
  сфокусированное окно. Иконки резолвятся из .desktop
  (`DesktopEntries.heuristicLookup`) и рисуются из вендоренной икон-темы
  BigSur (`build/de/share/icons`, `QS_ICON_THEME=BigSur` в run-de.sh).
- `Launcher.qml` — встроенный лаунчер приложений (поиск по .desktop,
  Enter/клик — запуск; внешних wofi/rofi в системе нет).
- `ShellState.qml` + `qmldir` — общее состояние (открыт ли лаунчер).

### Рабочие столы

Точки рабочих столов в `TopBar.qml` и подменю «View» в `BarMenu.qml`
переключают столы композитора. Оболочка не трогает wayfire напрямую: она
пишет команду в FIFO `$XDG_RUNTIME_DIR/de/ws-cmd` (`1..N`, `next`, `prev`) и
читает текущее состояние из `$XDG_RUNTIME_DIR/de/workspaces`
(`"<текущий> <всего>"`). Потребитель — демон `de-workspace`
(`src/de-workspace.c`), который:

- создаёт `$XDG_RUNTIME_DIR/de/`, FIFO `ws-cmd` и файл состояния;
- переключает стол по IPC-методу wayfire `vswitch/set-workspace`;
- всегда держит `workspaces` актуальным (сетка 2×2 = 4 стола).

`de-workspace` автозапускается wayfire (секция `[autostart]` в
`config/wayfire.ini`) и наследует `WAYFIRE_SOCKET` от композитора.

Запуск внутри сессии DE:

```bash
qs -p ui/quickshell        # или абсолютный путь
```

Автозапуск: секция `[autostart]` в `config/wayfire.ini`:
`shell = qs -p @DE_ROOT@/ui/quickshell` — wayfire запускает шелл при
старте (путь подставляется `session/run-de.sh`). Чтобы отключить —
очистите значение.

Проверено живьём на headless-запуске DE: оба бара мапятся, окно
alacritty появляется в доке, клики/закрытие работают через
foreign-toplevel.

## Альтернатива: Waybar (стиль macOS Sequoia)

Ставится в систему отдельно:

```bash
sudo pacman -S waybar   # опционально: wofi pavucontrol brightnessctl network-manager-applet bluez-utils
```

```bash
waybar -c ui/waybar/config.jsonc -s ui/waybar/style.css
```

Стилизация адаптирована из <https://github.com/kamlendras/waybar-macos-sequoia>
(MIT): убраны Hyprland/Sway-модули, таскбар работает через foreign-toplevel.
Также проверено живьём (оба бара + иконки окон).

## AGSv2 (Astal) — план

Требует те же два протокола; оба предоставляет wayfire.

## Отладка

- Лог сессии: `build/run/logs/wayfire.log`.
- Логи шелла: `/run/user/$UID/quickshell/by-id/<id>/log.qslog`.
- Если внешний клиент не видит баров — проверить, что глобалы
  `zwlr_layer_shell_v1` и `zwlr_foreign_toplevel_manager_v1`
  присутствуют в реестре сессии (`weston-debug`/`wayland-info`).
