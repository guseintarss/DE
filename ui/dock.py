#!/usr/bin/env python3
"""
PySide6 Dock UI с интеграцией QML для macOS-подобных анимаций.
Запускается как отдельный процесс и связывается с C++ ядром через D-Bus.
"""

import sys
import os
from PySide6.QtWidgets import QApplication
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtCore import QUrl, QObject, Signal, Slot, Property
from PySide6.QtDBus import QDBusConnection, QDBusInterface, QDBusMessage

class DockController(QObject):
    """Контроллер дока, связывающий QML UI с C++ ядром через D-Bus"""
    
    # Сигналы для обновления UI
    appListChanged = Signal()
    
    def __init__(self, parent=None):
        super().__init__(parent)
        self._apps = ["🚀", "📁", "⚙️", "🌐", "🎵"]
        
        # Подключение к D-Bus сервису C++ ядра
        self.dbus_interface = None
        self.connect_to_core()
    
    @Property(list, notify=appListChanged)
    def apps(self):
        """Список приложений для отображения в доке"""
        return self._apps
    
    @Slot(int)
    def launch_app(self, index):
        """Запуск приложения по индексу"""
        if 0 <= index < len(self._apps):
            app_name = self._apps[index]
            print(f"Launching app: {app_name}")
            
            # Вызов метода C++ ядра через D-Bus
            if self.dbus_interface:
                reply = self.dbus_interface.call("LaunchApp", app_name)
                if reply.errorName():
                    print(f"D-Bus error: {reply.errorMessage()}")
                else:
                    print(f"App {app_name} launched successfully")
    
    def connect_to_core(self):
        """Подключение к D-Bus сервису C++ ядра"""
        connection = QDBusConnection.sessionBus()
        
        if not connection.isConnected():
            print("Warning: Cannot connect to D-Bus session bus")
            return
        
        # Интерфейс к C++ ядру (должен быть зарегистрирован в C++ коде)
        self.dbus_interface = QDBusInterface(
            "com.myde.WindowManager",  # Сервис C++ ядра
            "/com/myde/WindowManager",  # Путь объекта
            "com.myde.WindowManager",  # Интерфейс
            connection
        )
        
        if self.dbus_interface.isValid():
            print("Connected to DE core via D-Bus")
        else:
            print("Warning: DE core D-Bus interface not available (running in standalone mode)")


def main():
    app = QApplication(sys.argv)
    app.setApplicationName("DE Dock")
    
    # Создаем контроллер
    controller = DockController()
    
    # Настраиваем QML движок
    engine = QQmlApplicationEngine()
    
    # Экспортируем контроллер в QML контекст
    engine.rootContext().setContextProperty("dockController", controller)
    
    # Загружаем QML файл
    qml_file = os.path.join(os.path.dirname(__file__), "dock.qml")
    engine.load(QUrl.fromLocalFile(qml_file))
    
    if not engine.rootObjects():
        print("Error: Failed to load QML")
        sys.exit(-1)
    
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
