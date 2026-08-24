import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Qt5Compat.GraphicalEffects
import Quickshell
import Quickshell.Wayland
import Quickshell.Widgets

PanelWindow {
    id: root

    // Позиционирование как у Spotlight (по центру сверху):
    // при единственном якоре top композитор сам центрирует окно по горизонтали
    anchors {
        top: true
    }
    margins {
        top: 120 // Отступ от верхнего края экрана
    }
    
    implicitWidth: 600  // Сделаем пошире, как в macOS
    implicitHeight: 480
    // Окно не размапливаем сразу при закрытии: держим его, пока играет
    // обратная анимация, иначе поверхность исчезнет в тот же кадр.
    // ВАЖНО: управляем внутренним флагом shown, а не читаем visible —
    // биндинг visible пересчитывается раньше обработчика сигнала, и
    // окно успевает скрыться до запуска анимации закрытия.
    visible: root.shown || root.closing
    color: "transparent"
    focusable: true

    // Оверлей поверх баров: не резервируем эксклюзивную зону, иначе
    // при открытии лаунчер выталкивает TopBar вниз и схлопывает док.
    WlrLayershell.layer: WlrLayer.Overlay
    exclusionMode: ExclusionMode.Ignore

    // Во время закрытия фокус отдаём сразу, не дожидаясь конца анимации.
    WlrLayershell.keyboardFocus: root.closing
        ? WlrKeyboardFocus.None : WlrKeyboardFocus.Exclusive

    // === АНИМАЦИЯ ПОЯВЛЕНИЯ/ИСЧЕЗНОВЕНИЯ В СТИЛЕ MACOS ===
    // Анимируем внутренний контейнер, а не само окно, чтобы избежать
    // артефактов Wayland. Цели привязаны к shown: при закрытии Behavior
    // сам проиграет значения в обратную сторону, окно скроет таймер.
    property bool closing: false
    property bool shown: false
    property real targetScale: root.shown ? 1.0 : 0.92
    property real targetOpacity: root.shown ? 1.0 : 0.0

    Timer {
        id: focusTimer
        interval: 100
        onTriggered: search.forceActiveFocus()
    }

    // Длительность закрытия: чуть длиннее самой долгой анимации (250мс)
    Timer {
        id: closeTimer
        interval: 270
        onTriggered: root.closing = false
    }

    // Следим за переключателем: при открытии поднимаем shown и возвращаем
    // фокус, при закрытии запускаем обратную анимацию и только потом
    // прячем окно.
    Connections {
        target: ShellState
        function onLauncherOpenChanged() {
            const open = ShellState.launcherOpen;
            if (open) {
                // Порядок важен: сначала поднимаем shown, потом снимаем
                // closing — иначе биндинг visible увидит false в промежутке
                // и поверхность мигнёт unmap/map.
                root.shown = true;
                root.closing = false;
                closeTimer.stop();
                // Очистка поиска запускает фильтрацию через onTextChanged;
                // явный refresh нужен только если текст уже пуст (события
                // не будет). Qt.callLater схлопывает повторы в один кадр.
                if (search.text !== "") {
                    search.text = "";
                } else {
                    Qt.callLater(root.refresh);
                }
                // Небольшая задержка для плавности фокуса после анимации
                focusTimer.restart();
            } else {
                // Зеркально: сначала ставим флаг «держать окно замапленным»,
                // и лишь затем опускаем shown, запуская обратную анимацию.
                root.closing = true;
                closeTimer.restart();
                root.shown = false;
            }
        }
    }

    // Главный контейнер с анимацией
    Item {
        id: contentContainer
        anchors.fill: parent
        
        // Магия анимации: Behavior автоматически интерполирует значения
        Behavior on scale {
            NumberAnimation { 
                duration: 250 
                easing.type: Easing.OutQuint // Фирменная плавная кривая Apple
            }
        }
        Behavior on opacity {
            NumberAnimation { 
                duration: 200 
                easing.type: Easing.OutQuint 
            }
        }

        scale: targetScale
        opacity: targetOpacity

        // === СТЕКЛЯННАЯ ПАНЕЛЬ (Glassmorphism) ===
        Rectangle {
            anchors.fill: parent
            radius: 18 // Большие скругления как в macOS
            // Глубокий темный фон с прозрачностью (для светлой темы можно заменить на #CCFFFFFF)
            color: "#CC1C1C1E" 
            // Тонкая светлая граница создает эффект "объема" стекла
            border.color: "#33FFFFFF"
            border.width: 1

            // Легкая тень (симуляция через градиент, если DropShadow недоступен)
            layer.enabled: true
            layer.effect: DropShadow {
                transparentBorder: true
                horizontalOffset: 0
                verticalOffset: 6
                radius: 12
                samples: 16
                color: "#66000000"
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                // === ПОЛЕ ПОИСКА В СТИЛЕ SPOTLIGHT ===
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    // Иконка лупы
                    IconImage {
                        source: Quickshell.iconPath("system-search") // Или "edit-find"
                        width: 20
                        height: 20
                        // macOS-синий при фокусе имитируем яркостью: у IconImage нет свойства color
                        opacity: search.activeFocus ? 1.0 : 0.55
                        Behavior on opacity { NumberAnimation { duration: 150 } }
                    }

                    TextField {
                        id: search
                        Layout.fillWidth: true
                        font.family: "SF Pro Display, SF Pro Text, Segoe UI, Cantarell, sans-serif"
                        font.pixelSize: 18
                        font.weight: Font.Normal
                        placeholderText: "Поиск приложений"
                        color: "#FFFFFF"
                        selectionColor: "#0A84FF"
                        selectedTextColor: "#FFFFFF"
                        
                        // Убираем стандартный фон Qt
                        background: Item {} 

                        onTextChanged: Qt.callLater(root.refresh)
                        
                        // Стилизация плейсхолдера (через палитру)
                        palette.placeholderText: "#8E8E93"
                        palette.text: "#FFFFFF"

                        Keys.onEscapePressed: ShellState.closeLauncher()
                        Keys.onReturnPressed: launch(0)
                        Keys.onEnterPressed: launch(0)
                        Keys.onDownPressed: appList.incrementCurrentIndex()
                        Keys.onUpPressed: appList.decrementCurrentIndex()
                    }
                }

                // Разделительная линия (очень тонкая)
                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: "#33FFFFFF"
                }

                // === СПИСОК ПРИЛОЖЕНИЙ ===
                ListView {
                    id: appList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 4
                    currentIndex: 0
                    keyNavigationEnabled: true
                    boundsBehavior: Flickable.StopAtBounds

                    // Плавная прокрутка
                    flickDeceleration: 1500

                    delegate: Item {
                        id: row
                        required property var modelData
                        required property int index
                        width: appList.width
                        height: 48

                        // Фон элемента с анимацией
                        Rectangle {
                            id: rowBg
                            anchors.fill: parent
                            anchors.margins: 4
                            radius: 10
                            // Цвет меняется при наведении или фокусе клавиатуры
                            color: (row.ListView.isCurrentItem || rowArea.containsMouse) ? "#33FFFFFF" : "transparent"
                            
                            Behavior on color {
                                ColorAnimation { duration: 150; easing.type: Easing.OutQuad }
                            }
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            spacing: 14

                            // Иконка приложения с легкой анимацией масштаба при наведении
                            IconImage {
                                id: appIco
                                Layout.preferredWidth: 32
                                Layout.preferredHeight: 32
                                asynchronous: true
                                
                                source: {
                                    let n = row.modelData.icon || "";
                                    if (n === "") return "";
                                    try {
                                        return Quickshell.iconPath(n, "image-missing");
                                    } catch (err) {
                                        return n;
                                    }
                                }
                                
                                // Пружинное увеличение иконки при наведении на строку
                                scale: rowArea.containsMouse ? 1.1 : 1.0
                                Behavior on scale { 
                                    SpringAnimation { spring: 3; damping: 0.5; duration: 300 } 
                                }
                            }

                            // Название приложения
                            Text {
                                Layout.fillWidth: true
                                font.family: "SF Pro Display, Segoe UI, sans-serif"
                                font.pixelSize: 15
                                font.weight: Font.Medium
                                color: "#FFFFFF"
                                elide: Text.ElideRight
                                text: row.modelData.name || ""
                                
                                // Если элемент выбран, делаем текст ярче
                                opacity: row.ListView.isCurrentItem ? 1.0 : 0.85
                                Behavior on opacity { NumberAnimation { duration: 150 } }
                            }
                        }

                        MouseArea {
                            id: rowArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                appList.currentIndex = index;
                                root.launch(index);
                            }
                            onEntered: appList.currentIndex = index
                        }
                    }
                    
                    // Индикатор прокрутки (скроллбар) в стиле macOS
                    ScrollBar.vertical: ScrollBar {
                        policy: ScrollBar.AsNeeded
                        contentItem: Rectangle {
                            implicitWidth: 6
                            radius: 3
                            color: parent.pressed ? "#66FFFFFF" : "#33FFFFFF"
                            Behavior on color { ColorAnimation { duration: 200 } }
                        }
                    }
                }
            }
        }
    }

    // Кэш всех .desktop-записей. Биндинг (а не одноразовый захват!)
    // обязателен: скан .desktop у quickshell асинхронный, при
    // Component.onCompleted список ещё пуст. Биндинг подтянет записи,
    // когда скан завершится.
    property var cachedEntries: DesktopEntries.applications.values
    onCachedEntriesChanged: Qt.callLater(root.refresh)

    function refresh() {
        const q = search.text.toLowerCase();
        const out = [];
        const arr = root.cachedEntries;

        for (let i = 0; i < arr.length; i++) {
            const entry = arr[i];
            if (!entry || entry.noDisplay)
                continue;
            if (q !== "" && (entry.name || "").toLowerCase().indexOf(q) === -1)
                continue;
            out.push(entry);
        }
        
        // Сортировка: сначала точное совпадение, потом по алфавиту
        out.sort((a, b) => {
            const nameA = (a.name || "").toLowerCase();
            const nameB = (b.name || "").toLowerCase();
            if (nameA === q) return -1;
            if (nameB === q) return 1;
            return nameA.localeCompare(nameB);
        });
        
        appList.model = out;
    }

    function launch(i) {
        if (i < 0 || i >= appList.count)
            return;
        const entry = appList.itemAtIndex(i)?.modelData;
        if (!entry)
            return;
        try {
            entry.execute();
        } catch (e) {
            console.warn("launch failed:", e);
        }
        ShellState.closeLauncher();
    }

    // Клик мимо панели — закрыть (с анимацией)
    MouseArea {
        anchors.fill: parent
        z: -1
        onClicked: ShellState.closeLauncher()
    }
}