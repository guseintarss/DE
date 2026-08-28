# Прежний самописный композитор (не используется)

Это старый композитор DE на wlroots (чистый C): `compositor/` — исходники,
`config.toml` — его конфиг, `third_party/tomlc99` — парсер TOML.

Он заменён на Wayfire (см. корневой README и `config/wayfire.ini`) и
сохранён только как справочник по прежнему поведению: как создавались
поверхности шелла (bar/dock/wallpaper), как обрабатывались layer-shell и
foreign-toplevel, как раскладывались окна. Собирать/запускать его не нужно —
то, что он умел, теперь делают плагины wayfire (`decoration`, `grid`,
`simple-tile`, `foreign-toplevel`, layer-shell) и внешние клиенты
(`de-wallpaper`, QuickShell).