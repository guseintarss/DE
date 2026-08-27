project('DE', 'c',
  version: '0.1',
  default_options: ['warning_level=3', 'c_std=gnu11'])

add_project_arguments('-DWLR_USE_UNSTABLE', language: 'c')

wayland_scanner = find_program('wayland-scanner')
wayland_protocols_dir = dependency('wayland-protocols').get_variable('pkgdatadir')

xdg_shell_proto = custom_target('xdg-shell-proto',
  input: wayland_protocols_dir + '/stable/xdg-shell/xdg-shell.xml',
  output: 'xdg-shell-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@']
)

# wlr-layer-shell не входит в wayland-protocols — XML вендорится в
# third_party/protocols (источник: репозиторий wlroots).
layer_shell_proto = custom_target('layer-shell-proto',
  input: 'third_party/protocols/wlr-layer-shell-unstable-v1.xml',
  output: 'wlr-layer-shell-unstable-v1-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@']
)

inc = include_directories('include', 'third_party/tomlc99', '.')

deps = [
  dependency('wayland-server'),
  dependency('wayland-protocols'),
  dependency('wlroots-0.20', version: '>=0.16.0'),
  dependency('xkbcommon'),
  dependency('libinput'),
  dependency('pixman-1'),
  dependency('libdrm'),
  dependency('cairo'),
  dependency('fontconfig'),
]

rsvg = dependency('librsvg-2.0', required: false)
if rsvg.found()
  deps += rsvg
  add_project_arguments('-DHAVE_RSVG', language: 'c')
endif

xxd = find_program('xxd')
wallpaper_data = custom_target('wallpaper_data',
  input: 'assets/wallpaper.png',
  output: 'wallpaper_data.c',
  command: [xxd, '-i', '-n', 'wallpaper_png', '@INPUT@', '@OUTPUT@'])

executable('DE',
  'src/main.c',
  'src/server.c',
  'src/cursor.c',
  'src/keyboard.c',
  'src/xdg_shell.c',
  'src/wallpaper.c',
  'src/config.c',
  'src/design.c',
  'src/dock.c',
  'src/apps_menu.c',
  'src/bar.c',
  'src/layer_shell.c',
  'src/foreign_toplevel.c',
  'src/session_lock.c',
  'src/workspace.c',
  'src/animation.c',
  'src/effects.c',
  'src/icons.c',
  'third_party/tomlc99/toml.c',
  wallpaper_data,
  xdg_shell_proto,
  layer_shell_proto,
  include_directories: inc,
  dependencies: deps,
  install: true)
