#!/usr/bin/env sh
# =============================================================================
# Сборка вендоренной икон-темы BigSur в префикс проекта.
#
# Запускается meson'ом как install-скрипт (meson.add_install_script) на этапе
# `meson install -C build`:
#   - собирает темы BigSur/BigSur-dark из third_party/BigSur-icon-theme
#     (штатный install.sh вендора) в $PREFIX/share/icons;
#   - заводит hicolor-фолбек: Qt/GTK ищут иконки ТЕКУЩЕЙ икон-темы, а по
#     умолчанию это hicolor, поэтому hicolor перенаправляется на BigSur
#     (Inherits -> BigSur, без цикла: у BigSur hicolor убран из Inherits);
#   - обновляет кэш иконок (gtk-update-icon-cache).
#
# Дополнительно: BIGSUR_THEME=<default|black|...> — варианты вендора.
# Прямой запуск вне meson:  scripts/install-icons.sh /abs/path/to/prefix
# =============================================================================

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if [ -n "${MESON_SOURCE_ROOT:-}" ]; then
    SRC="$MESON_SOURCE_ROOT/third_party/BigSur-icon-theme"
else
    SRC="$SCRIPT_DIR/third_party/BigSur-icon-theme"
fi

if [ "$#" -ge 1 ]; then
    PREFIX="$1"
elif [ -n "${MESON_INSTALL_DESTDIR_PREFIX:-}" ]; then
    PREFIX="$MESON_INSTALL_DESTDIR_PREFIX"
else
    PREFIX="${MESON_INSTALL_PREFIX:?prefix not given (run via meson install or pass prefix as argument)}"
fi

ICONDIR="$PREFIX/share/icons"

[ -d "$SRC" ] || { echo "install-icons: vendored theme missing: $SRC" >&2; exit 1; }
mkdir -p "$ICONDIR"

echo "install-icons: assembling BigSur theme into $ICONDIR"
if [ -n "${BIGSUR_THEME:-}" ]; then
    bash "$SRC/install.sh" -d "$ICONDIR" -t "$BIGSUR_THEME"
else
    bash "$SRC/install.sh" -d "$ICONDIR"
fi

# У вендора BigSur наследует hicolor; наш hicolor будет наследовать BigSur —
# убираем цикл, фиксируя наследника на системную breeze. Имя темы не трогаем.
sed -i 's/^Inherits=.*/Inherits=breeze/' "$ICONDIR/BigSur/index.theme"
if [ -f "$ICONDIR/BigSur-dark/index.theme" ]; then
    sed -i 's/^Inherits=.*/Inherits=breeze/' "$ICONDIR/BigSur-dark/index.theme"
fi

# hicolor-фолбек: shell (док/лаунчер) резолвит иконки через QIcon::fromTheme,
# которая по умолчанию ищет в теме hicolor. Направляем hicolor на BigSur.
mkdir -p "$ICONDIR/hicolor"
cat > "$ICONDIR/hicolor/index.theme" <<'EOF'
[Icon Theme]
Name=hicolor
Comment=BigSur icon theme (vendored) exposed as the hicolor fallback
Inherits=BigSur
EOF

# Алиасы приложений дока, которых нет в теме вендора: zen-browser запинен в
# Dock.qml, но иконки у него в теме нет — отдаём общий браузерный значок.
(cd "$ICONDIR/BigSur/apps/scalable" \
    && ln -sfn web-browser.svg net.zen_browser.zen.svg \
    && ln -sfn web-browser.svg zen-browser.svg \
    && ln -sfn web-browser.svg zen.svg)

if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t -f "$ICONDIR/hicolor"
fi

echo "install-icons: done ($ICONDIR/BigSur, BigSur-dark, hicolor -> BigSur)"