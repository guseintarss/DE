project('DE', 'c',
  version: '0.1',
  default_options: ['warning_level=3', 'c_std=gnu11'])

add_project_arguments('-DWLR_USE_UNSTABLE', language: 'c')

deps = [
  dependency('wayland-server'),
  dependency('wayland-protocols'),
  dependency('wlroots-0.20', version: '>=0.16.0'),
  dependency('xkbcommon'),
  dependency('pixman-1'),
  dependency('libdrm'),
  dependency('cairo'),
  dependency('fontconfig'),
]

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
  'src/dock.c',
  'src/bar.c',
  'src/animation.c',
  'third_party/tomlc99/toml.c',
  wallpaper_data,
  include_directories: include_directories('include', 'third_party/tomlc99'),
  dependencies: deps,
  install: true)