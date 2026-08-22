# Инструкция по запуску PySide6 UI для DE

## Архитектура
- **C++ ядро** (компилятор/оконный менеджер) - управляет окнами, Wayland, вводом
- **PySide6 UI** (отдельный процесс) - отрисовка панелей, дока, настроек
- **Связь через D-Bus** - высокоуровневые команды и события

## Требования
```bash
# Arch Linux
sudo pacman -S python-pyside6 qt6-quick qt6-dbus

# Или через pip
pip install PySide6
```

## Запуск Dock UI (тестовый режим без C++ ядра)
```bash
cd /workspace/ui
python dock.py
```

Окно дока откроется с анимированными иконками. При клике будет логироваться попытка запуска приложения.

## Интеграция с C++ ядром

### 1. Реализация D-Bus сервиса в C++
Добавьте в ваш C++ код (например, в `src/server.c` или отдельный файл):

```c
#include <gio/gio.h>

static void handle_launch_app(GDBusMethodInvocation *invocation, const gchar *app_name) {
    g_print("Launching app: %s\n", app_name);
    
    // Здесь код запуска приложения
    // ...
    
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", TRUE));
}

// Регистрация сервиса при инициализации сервера
void register_dbus_service(MyWMServer *server) {
    GDBusNodeInfo *introspection_data = ...; // загрузить из XML
    
    g_dbus_proxy_new_for_bus(...);
}
```

### 2. Сборка C++ ядра с D-Bus поддержкой
В `meson.build` добавьте:
```python
dependency('glib-2.0')
dependency('gio-2.0')
```

### 3. Запуск полного DE
```bash
# Терминал 1: C++ ядро
./build/de_app

# Терминал 2: PySide6 Dock
python /workspace/ui/dock.py

# Терминал 3: PySide6 Panel (если есть)
python /workspace/ui/panel.py
```

## Настройка дизайна

### В QML (dock.qml)
- Измените `color: "#CC000000"` для настройки прозрачности панели
- Настройте `radius: 16` для скругления углов
- Отрегулируйте параметры `SpringAnimation` для изменения физики анимации

### В Python (dock.py)
- Добавьте свои методы в `DockController`
- Настройте список приложений в `self._apps`
- Реализуйте обработку сигналов от C++ ядра

## Layer Shell для Wayland
Для позиционирования окна как системной панели поверх всех окон:

1. Создайте C++ обертку над `wlr-layer-shell`
2. Скомпилируйте как `.so` библиотеку
3. Подключите через `ctypes` в Python:

```python
import ctypes

layer_shell_lib = ctypes.CDLL("./liblayer_shell.so")
layer_shell_lib.create_layer_surface(window_id, layer="top", anchor="bottom")
```

## Следующие шаги
1. ✅ Создан QML файл с пружинными анимациями
2. ✅ Создан Python контроллер с D-Bus интеграцией
3. ✅ Создан XML интерфейс для D-Bus
4. ⬜ Реализовать D-Bus сервис в C++ ядре
5. ⬜ Добавить Layer Shell обертку для Wayland
6. ⬜ Создать панель (panel.py) и другие UI компоненты
