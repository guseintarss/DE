project('DE', 'c', 'cpp',
  version: '0.2',
  license: 'MIT',
  meson_version: '>=0.64.0',
  default_options: [
    'c_std=gnu11',
    'cpp_std=c++17',
    'warning_level=2',
    'werror=false',
  ])

# =============================================================================
# Ядро DE: Wayfire (вендорен в third_party/, собирается из исходников).
#
# wayfire и его библиотеки (wf-config, wf-utils, wf-json, wf-touch) живут в
# third_party/ и подключаются как subprojects (см. subprojects/*). Системные
# пакеты не используются/не меняются: библиотеки уровня wayland, cairo, glm,
# yyjson и т.п. берутся уже установленными, а сам wayfire компилируется здесь.
#
# Полный стек ставится командой `meson install -C build` (бинарь, плагины,
# metadata XML). Для запуска из исходников смотри session/run-de.sh.
# =============================================================================

wayfire_sp = subproject('wayfire',
  default_options: [
    'use_system_wlroots=enabled',
    'use_system_wfconfig=disabled',
    'xwayland=enabled',
    'enable_gles32=true',
    'tests=disabled',
    'build_locales=disabled',
  ])

# Собрать ядро композитора без установки: `meson compile -C build wayfire`
# (полный стек, включая плагины и metadata, ставится `meson install -C build`).

# =============================================================================
# Обои: маленький Wayland-клиент (слой background через wlr-layer-shell).
# Заменяет wallpaper.c старого композитора: статические обои как отдельный
# процесс поверх wayfire, без внешних инструментов (swaybg и т.п.).
# =============================================================================

wayland_scanner = find_program('wayland-scanner')

layer_shell_proto = custom_target('wlr-layer-shell-client-protocol',
  input: 'third_party/protocols/wlr-layer-shell-unstable-v1.xml',
  output: 'wlr-layer-shell-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

layer_shell_code = custom_target('wlr-layer-shell-client-code',
  input: 'third_party/protocols/wlr-layer-shell-unstable-v1.xml',
  output: 'wlr-layer-shell-unstable-v1-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

wayland_protocols_dir = dependency('wayland-protocols').get_variable('pkgdatadir')

# layer-shell ссылается на xdg-popup; нужен и его код (иначе undefined refs).
xdg_shell_client = custom_target('xdg-shell-client-code',
  input: wayland_protocols_dir + '/stable/xdg-shell/xdg-shell.xml',
  output: 'xdg-shell-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

xxd = find_program('xxd')
wallpaper_data = custom_target('wallpaper_data',
  input: 'assets/wallpaper.png',
  output: 'wallpaper_data.c',
  command: [xxd, '-i', '-n', 'wallpaper_png', '@INPUT@', '@OUTPUT@'])

de_wallpaper = executable('de-wallpaper',
  'src/de-wallpaper.c',
  layer_shell_proto,
  layer_shell_code,
  xdg_shell_client,
  wallpaper_data,
  dependencies: [
    dependency('wayland-client'),
    dependency('cairo'),
  ],
  include_directories: include_directories('include'),
  install: true,
  install_dir: get_option('bindir'),
)

# =============================================================================
# Конфиг и сессия.
# =============================================================================

install_data('config/wayfire.ini',
  install_dir: get_option('datadir') / 'de')

install_data('session/run-de.sh',
  install_dir: get_option('bindir'),
  install_mode: 'rwxr-xr-x')

de_session = configure_file(
  input: 'session/de.desktop',
  output: 'de.desktop',
  configuration: {'DE_ROOT': meson.global_source_root()},
)
install_data(de_session,
  install_dir: get_option('datadir') / 'wayland-sessions')