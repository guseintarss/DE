# DE — оконный менеджер на основе Wayfire

Рабочий стол (композитор + оболочка), собранный вокруг
[Wayfire](https://wayfire.org) — плагинного композитора на базе wlroots.
Это полностью заменило прежний самописный композитор на чистом C
(wlroots), который сохранён в `legacy/` как справочный материал.

## Архитектура

```
Wayfire (вендорен в third_party/wayfire, собран из исходников)
 ├── плагины: animation, grid, simple-tile, vswitch, scale, ipc,
 │            foreign-toplevel, layer-shell-протоколы и т.д.
 ├── de-wallpaper   — обои (слой background через zwlr-layer-shell-v1)
 ├── qs (QuickShell)— менюбар, док, лаунчер (ui/quickshell)
 └── swaylock/alacritty — по желанию
```

Ключевые принципы:

- **Всё внутри проекта.** Wayfire и его библиотеки (`wf-config`,
  `wf-utils`, `wf-json`, `wf-touch`) вендорены в `third_party/` и
  собираются здесь же. В систему ничего не устанавливается и не меняется —
  используются только уже присутствующие библиотеки (wayland, cairo, glm,
  yyjson, системный wlroots-0.20 и т.п.).
- **Sh де = wayfire.** Оболочка (QuickShell) подключается к композитору
  стандартными протоколами `zwlr-layer-shell-v1` и
  `wlr-foreign-toplevel-management-v1`, которые предоставляет wayfire.
- **Конфиг.** `config/wayfire.ini` — единая точка настройки (раскладка,
  рабочие столы, горячие клавиши, анимации, автозапуск). Часть клавиш
  перенесена из прежнего `config.toml`.

## Сборка

Требуется: gcc/clang (C++17), meson, ninja, wayland-protocols,
wayland-client/server, wlroots-0.20 (dev), cairo, glm, yyejson, libevdev,
libxml-2.0, libdrm, libinput, xkbcommon, egl/gles.

```sh
meson setup build --prefix="$PWD/build/de"
meson install -C build    # собирает и «ставит» весь стек в build/de/
```

- `meson compile -C build` — только обои (`de-wallpaper`);
- `meson compile -C build wayfire` — только ядро композитора;
- `meson install -C build` — полный стек: wayfire, плагины, metadata,
  оболочку запуска, конфиг, .desktop-файлы сессии и икон-тему BigSur
  (вендореная, собирается в `build/de/share/icons`).

## Запуск

```sh
./session/run-de.sh                  # старт DE (wayfire + оболочка + обои)
./session/run-de.sh --help           # аргументы wayfire пробрасываются
```

Скрипт генерирует рантайм-конфиг (`build/run/wayfire.ini`), подставив корень
репозитория, и стартует композитор. Логи: `build/run/logs/wayfire.log`.

Для входа через менеджер сессий установите `build/de/share/wayland-sessions`
в путь поиска (или скопируйте `session/de.desktop`) — Exec указывает на
`session/run-de.sh`.

## Горячие клавиши (основные)

| Комбинация | Действие |
|---|---|
| `Super+Enter` | терминал (alacritty) |
| `Super+Shift+Q`, `Alt+F4` | закрыть окно |
| `Super+Shift+E` | выход из сессии |
| `Super+L` | блокировщик (swaylock) |
| `Alt+Tab` / `Super+Tab` | переключение окон |
| `Super+стрелки` | укладка/перемещение окна |
| `Ctrl+Super+стрелки` | смена рабочего стола |
| `Ctrl+Super+Shift+стрелки` | переместить окно между столами |
| `Super+мышь` | перетаскивание, `Super+ПКМ` — resize |

Анимации, цвета, размеры — в `config/wayfire.ini` (применяются на лету,
wayfire перечитывает файл при изменении).

## Иконки (Big Sur)

В проекте вендорена macOS-икон-тема [BigSur](https://github.com/yeyushengfan258/BigSur-icon-theme).
На `meson install` она собирается в `build/de/share/icons` (темы `BigSur`,
`BigSur-dark` и hicolor-фолбек), `session/run-de.sh` подключает её через
`XDG_DATA_DIRS` и ставит `QS_ICON_THEME=BigSur`. Док, лаунчер и системные
иконки резолвятся через стандартный freedesktop-поиск (`QIcon::fromTheme`),
поэтому даже приложения без иконки в теме (например, zen-browser — для него
добавлен алиас) получают macOS-пиктограммы.

## Структура

```
src/de-wallpaper.c   — клиент обоев (слой background, wlr-layer-shell)
src/de-workspace.c   — мост рабочих столов (FIFO ws-cmd → IPC wayfire)
config/wayfire.ini   — конфигурация wayfire для DE
scripts/             — вспомогательные скрипты (install-icons.sh)
session/run-de.sh    — запуск сессии
session/de.desktop   — запись для менеджера сессий
third_party/         — вендорен: wayfire, wf-config, wf-utils, wf-json, wf-touch,
                       BigSur-icon-theme
subprojects/         — симлинки на third_party/* (для meson)
legacy/              — прежний композитор на wlroots (C), конфиг config.toml
ui/quickshell        — оболочка QuickShell (менюбар, док, лаунчер)
```

### Вендоренные версии

| Проект | Ревизия |
|---|---|
| wayfire | `v0.11.0` |
| wf-config | `v0.11.0` |
| wf-utils | `329c3ff` |
| wf-json | `70039e1` |
| wf-touch | `d7ae5e7` |
| BigSur-icon-theme | `master` (срез 2026-08-29) |

Обновление: заместить дерево в `third_party/<name>` на новую ревизию
(симлинки в `subprojects/` остаются) и пересобрать.