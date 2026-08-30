#!/usr/bin/env sh
# =============================================================================
# Запуск сессии DE поверх Wayfire.
#
# DE_ROOT — корень репозитория. Всё собирается и живёт внутри проекта:
#   build/de        — prefix «установки» (мезоновский --prefix=build/de),
#   build/run       — рантайм (сгенерированный wayfire.ini, логи).
# В систему ничего не ставится.
#
# Обязательные шаги перед первым запуском:
#   meson setup build --prefix="$PWD/build/de"
#   meson install -C build
#
# Использование:
#   ./session/run-de.sh [аргументы wayfire...]
# Окружение: DE_ROOT, DE_WALLPAPER, DE_WALLPAPER_MODE (см. src/de-wallpaper.c).
# =============================================================================

set -eu

DE_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

PREFIX="${DE_PREFIX:-$DE_ROOT/build/de}"
BINDIR="$PREFIX/bin"
LIBWAYFIRE="$PREFIX/lib/wayfire"
RUNDIR="$DE_ROOT/build/run"
CFG="$RUNDIR/wayfire.ini"
WAYFIRE_BIN="$BINDIR/wayfire"

if [ ! -x "$WAYFIRE_BIN" ]; then
    echo "de: wayfire не собран. Запустите:" >&2
    echo "  meson setup build --prefix=\"\$PWD/build/de\"" >&2
    echo "  meson install -C build" >&2
    exit 1
fi

# Рантайм-каталог (asm-файл конфига генерируется на лету).
mkdir -p "$RUNDIR"

# Подставить корень репозитория в команды автозапуска конфига.
sed 's#@DE_ROOT@#'"$DE_ROOT"'#g' "$DE_ROOT/config/wayfire.ini" > "$CFG"

export XDG_CURRENT_DESKTOP=DE:Wayland
export WAYFIRE_CONFIG_FILE="$CFG"
export WAYFIRE_PLUGIN_PATH="$LIBWAYFIRE"
export WAYFIRE_PLUGIN_XML_PATH="$PREFIX/share/wayfire/metadata"
export XDG_DATA_DIRS="$PREFIX/share:${XDG_DATA_DIRS:-}"

# Икон-тема BigSur (вендореная, собирается в $PREFIX/share/icons):
# QuickShell читает QS_ICON_THEME -> QIcon::setThemeName(), так что док и
# лаунчер резолвят macOS-иконки через стандартный freedesktop-поиск.
export QS_ICON_THEME=BigSur

# vendor-библиотеки (wf-config/wf-utils) приоритетнее системных.
export LD_LIBRARY_PATH="$PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# de-wallpaper ищется в PATH командой autostart.
export PATH="$BINDIR:$PATH"

# Логи композитора и оболочки.
mkdir -p "$RUNDIR/logs"
WF_LOG="$RUNDIR/logs/wayfire.log"

# Данные сессии (для сокетов wayfire и т.п.).
# Сокет композитора поднимает сам wayfire; здесь просто гарантируем каталог.
mkdir -p "${XDG_RUNTIME_DIR:-$RUNDIR}"

echo "de: старт wayfire (конфиг: $CFG, лог: $WF_LOG)"
exec "$WAYFIRE_BIN" "$@" >"$WF_LOG" 2>&1