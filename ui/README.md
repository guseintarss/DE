# Оболочка DE: внешний UI поверх композитора

Композитор поддерживает два режима оболочки (`[shell]` в `config.toml`):

- **`builtin = true`** — менюбар и док рисует сам композитор
  (`src/bar.c`, `src/dock.c`).
- **`builtin = false`** (текущий) — панель/док рисует внешний клиент
  через `zwlr-layer-shell-v1`. Список окон публикуется через
  `wlr-foreign-toplevel-management-v1`.

## Протоколы, добавленные в композитор

| Протокол | Зачем |
|---|---|
| `zwlr-layer-shell-v1` | панели по краям экрана, эксклюзивные зоны (окна не залезают под бар), клавиатурный фокус для лаунчеров |
| `wlr-foreign-toplevel-management-v1` | список окон, заголовки/app_id/состояния, управление (activate/close/min/max) |
| `zxdg-output-manager` (wlroots) | имена/геометрия выходов; обязателен для waybar |

Эксклюзивные зоны слоёв учитываются при максимизации/тайлинге окон
(`shell_usable_box()` в `src/layer_shell.c`).

## Основная оболочка: QuickShell (`ui/quickshell/`)

Установлена в системе (`qs`, пакет noctalia-qs). Состав:

- `TopBar.qml` — верхний менюбар macOS Sequoia: кнопка-лаунчер,
  File/Edit/View/Help, в центре имя активного окна, справа громкость
  (wpctl), батарея (UPower), трей (StatusNotifier), часы.
- `Dock.qml` — нижний док: лаунчер + иконки всех открытых окон
  (`ToplevelManager`); клик — фокус, средняя кнопка — закрыть, точка =
  сфокусированное окно. Иконки резолвятся из .desktop
  (`DesktopEntries.heuristicLookup`).
- `Launcher.qml` — встроенный лаунчер приложений (поиск по .desktop,
  Enter/клик — запуск; внешних wofi/rofi в системе нет).
- `ShellState.qml` + `qmldir` — общее состояние (открыт ли лаунчер).

Запуск внутри сессии DE:

```bash
qs -p ui/quickshell        # или абсолютный путь
```

Автозапуск: в `config.toml` указано `start = "qs -p ui/quickshell"` —
композитор сам запускает оболочку при старте (после создания сокета,
путь относителен каталогу запуска DE). Чтобы отключить — очистите `start`.

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

Требует те же два протокола, поддержка уже есть в композиторе.

## Отладка

- Лог композитора: `[DEBUG] new layer surface: layer=N output=...`.
- Логи шелла: `/run/user/$UID/quickshell/by-id/<id>/log.qslog`.
- Если внешний клиент не видит баров — проверить, что глобалы
  `zwlr_layer_shell_v1` и `zwlr_foreign_toplevel_manager_v1`
  присутствуют в реестре сессии.
- Вернуть встроенную оболочку: `[shell] builtin = true` и перезапуск DE.
