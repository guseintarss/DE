import QtQuick 6.0
import QtQuick.Window 6.0
import QtQuick.Controls 6.0
import QtQuick.Layouts 6.0

Window {
    id: root
    width: 400
    height: 80
    visible: true
    color: "transparent" // Прозрачный фон для композитора
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint

    // Эффект матового стекла (эмуляция через полупрозрачность)
    Rectangle {
        anchors.fill: parent
        radius: 16
        color: "#CC000000" // Полупрозрачный черный
        border.color: "#33FFFFFF"
        border.width: 1
        
        // Шум для эффекта стекла (опционально)
        // Здесь можно добавить ShaderEffect для настоящего blur, если композитор не поддерживает

        RowLayout {
            anchors.centerIn: parent
            spacing: 12

            Repeater {
                model: ["🚀", "📁", "⚙️", "🌐", "🎵"] // Эмодзи вместо иконок для теста

                delegate: Item {
                    id: iconItem
                    width: 60
                    height: 60

                    property real scaleFactor: 1.0

                    Text {
                        anchors.centerIn: parent
                        font.pixelSize: 40 * iconItem.scaleFactor
                        text: modelData
                        Behavior on font.pixelSize {
                            // Пружинная анимация
                            SpringAnimation {
                                spring: 3.0
                                damping: 0.6
                            }
                        }
                        
                        // Тень
                        layer.enabled: true
                        layer.effect: DropShadow {
                            transparentBorder: true
                            horizontalOffset: 0
                            verticalOffset: 4
                            radius: 8
                            samples: 16
                            color: "#80000000"
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        
                        onEntered: {
                            iconItem.scaleFactor = 1.5
                            // Увеличиваем соседей (эффект рыбьего глаза - упрощенно)
                            if (index > 0) parent.children[index-1].scaleFactor = 1.2
                            if (index < parent.children.length - 1) parent.children[index+1].scaleFactor = 1.2
                        }
                        
                        onExited: {
                            iconItem.scaleFactor = 1.0
                            if (index > 0) parent.children[index-1].scaleFactor = 1.0
                            if (index < parent.children.length - 1) parent.children[index+1].scaleFactor = 1.0
                        }
                        
                        onClicked: {
                            console.log("App launched:", modelData)
                            // Здесь вызов D-Bus метода для запуска приложения
                        }
                    }
                }
            }
        }
    }
}
