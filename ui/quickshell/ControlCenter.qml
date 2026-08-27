import QtQuick
import QtQuick.Controls.Basic
import Quickshell
import Quickshell.Io
import Quickshell.Wayland
import Quickshell.Services.UPower
import Quickshell.Services.Mpris

// Центр управления: громкость, яркость, батарея, медиа-плеер (MPRIS)
// и календарь. Открывается кликом по громкости/батарее/часам в баре.
PanelWindow {
    id: root

    anchors {
        top: true
        right: true
    }
    margins {
        top: 40
        right: 8
    }
    implicitWidth: 330
    implicitHeight: ccCol.implicitHeight + 24
    visible: ShellState.barPopup === "cc"
    color: "transparent"

    WlrLayershell.layer: WlrLayer.Top
    exclusionMode: ExclusionMode.Ignore

    readonly property string uiFont: "SF Pro Display, Segoe UI, Cantarell, sans-serif"
    readonly property string iconFont: "MesloLGLDZ Nerd Font, Material Design Icons, sans-serif"

    // === Громкость ===
    property real volume: 0        // 0..1
    property bool muted: false
    // Значение слайдера: пока тянут — локальное, иначе из wpctl
    property real volSlider: 0
    property bool volDragging: false

    Process {
        id: volGet
        command: ["sh", "-c",
                  "wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null || echo 'Volume: 0.00'"]
        stdout: StdioCollector {
            onStreamFinished: {
                const t = this.text.trim();
                root.muted = /MUTED/.test(t);
                const v = parseFloat(t.split(":")[1]);
                root.volume = isNaN(v) ? 0 : Math.min(1.0, v);
                if (!root.volDragging) root.volSlider = root.volume;
            }
        }
    }
    Process {
        id: volSet
        property real pendingVol: 0
        command: ["sh", "-c",
                  "wpctl set-volume @DEFAULT_AUDIO_SINK@ " + pendingVol.toFixed(2)]
    }
    function applyVolume(v) {
        volSet.pendingVol = v;
        volSet.running = true;
    }

    Timer {
        interval: 2000
        running: true
        repeat: true
        triggeredOnStart: true
        onTriggered: volGet.running = true
    }
    // Сглаживание дёрганья слайдера: применяем не чаще 120 мс
    Timer {
        id: volThrottle
        interval: 120
        onTriggered: root.applyVolume(root.volSlider)
    }

    // === Яркость (brightnessctl; нет утилиты — секция скрыта) ===
    property bool hasBrightness: false
    property real brightness: 1.0 // 0..1
    property bool briDragging: false
    Process {
        id: briGet
        command: ["sh", "-c",
                  "echo $(( $(brightnessctl get 2>/dev/null || echo 0) * 100 / $(brightnessctl max 2>/dev/null || echo 1) ))"]
        stdout: StdioCollector {
            onStreamFinished: {
                const v = parseInt(this.text.trim());
                if (!isNaN(v) && v > 0) {
                    root.hasBrightness = true;
                    if (!root.briDragging) root.brightness = v / 100.0;
                }
            }
        }
    }
    Process {
        id: briSet
        property real pendingBri: 1.0
        command: ["sh", "-c",
                  "brightnessctl set " + Math.max(1, Math.round(pendingBri * 100)) + "% >/dev/null 2>&1"]
    }
    Timer {
        id: briThrottle
        interval: 120
        onTriggered: briSet.running = true
    }

    Timer {
        interval: 5000
        running: true
        repeat: true
        triggeredOnStart: true
        onTriggered: briGet.running = true
    }

    // === Календарь ===
    property date viewDate: new Date()
    readonly property int calYear: viewDate.getFullYear()
    readonly property int calMonth: viewDate.getMonth() // 0-based
    readonly property int daysInMonth: new Date(calYear, calMonth + 1, 0).getDate()
    readonly property int firstOffset: (new Date(calYear, calMonth, 1).getDay() + 6) % 7 // Пн-первый
    function isToday(day) {
        const now = new Date();
        return now.getFullYear() === calYear && now.getMonth() === calMonth &&
               now.getDate() === day;
    }

    Rectangle {
        anchors.fill: parent
        radius: 14
        color: "#E61C1C1E"
        border.color: "#33FFFFFF"
        border.width: 1

        Column {
            id: ccCol
            anchors.centerIn: parent
            width: parent.width - 24
            spacing: 14

            // --- Заголовок ---
            Text {
                text: "Центр управления"
                color: "#FFFFFF"
                opacity: 0.55
                font.family: root.uiFont
                font.pixelSize: 12
                font.weight: Font.Medium
            }

            // --- Громкость ---
            Rectangle {
                width: parent.width
                height: volRow.implicitHeight + 20
                radius: 10
                color: "#22FFFFFF"
                Column {
                    id: volRow
                    anchors.centerIn: parent
                    width: parent.width - 20
                    spacing: 6
                    Row {
                        spacing: 8
                        Text {
                            text: root.muted ? "\uF026" : "\uF028"
                            font.family: root.iconFont
                            font.pixelSize: 14
                            color: "#FFFFFF"
                        }
                        Text {
                            text: root.muted ? "Muted" : Math.round(root.volume * 100) + "%"
                            font.family: root.uiFont
                            font.pixelSize: 13
                            font.weight: Font.Medium
                            color: "#FFFFFF"
                        }
                    }
                    Slider {
                        id: volSlider
                        width: parent.width
                        from: 0
                        to: 1
                        value: root.volSlider
                        enabled: !root.muted
                        onPressedChanged: if (!pressed) root.volDragging = false
                        onMoved: {
                            root.volDragging = true;
                            root.volSlider = value;
                            volThrottle.restart();
                        }
                        background: Rectangle {
                            x: volSlider.leftPadding
                            y: volSlider.topPadding + volSlider.availableHeight / 2 - height / 2
                            width: volSlider.availableWidth
                            height: 6
                            radius: 3
                            color: "#44FFFFFF"
                            Rectangle {
                                width: volSlider.visualPosition * parent.width
                                height: parent.height
                                radius: 3
                                color: root.muted ? "#66FFFFFF" : "#0A84FF"
                            }
                        }
                        handle: Rectangle {
                            x: volSlider.leftPadding + volSlider.visualPosition *
                               (volSlider.availableWidth - width)
                            y: volSlider.topPadding + volSlider.availableHeight / 2 - height / 2
                            width: 16
                            height: 16
                            radius: 8
                            color: "#FFFFFF"
                        }
                    }
                }
            }

            // --- Яркость ---
            Rectangle {
                width: parent.width
                height: briRow.implicitHeight + 20
                radius: 10
                color: "#22FFFFFF"
                visible: root.hasBrightness
                Column {
                    id: briRow
                    anchors.centerIn: parent
                    width: parent.width - 20
                    spacing: 6
                    Row {
                        spacing: 8
                        Text {
                            text: "\uF185" // солнце
                            font.family: root.iconFont
                            font.pixelSize: 14
                            color: "#FFFFFF"
                        }
                        Text {
                            text: Math.round(root.brightness * 100) + "%"
                            font.family: root.uiFont
                            font.pixelSize: 13
                            font.weight: Font.Medium
                            color: "#FFFFFF"
                        }
                    }
                    Slider {
                        id: briSlider
                        width: parent.width
                        from: 0.01
                        to: 1
                        value: root.brightness
                        onPressedChanged: if (!pressed) root.briDragging = false
                        onMoved: {
                            root.briDragging = true;
                            root.brightness = value;
                            briSet.pendingBri = value;
                            briThrottle.restart();
                        }
                        background: Rectangle {
                            x: briSlider.leftPadding
                            y: briSlider.topPadding + briSlider.availableHeight / 2 - height / 2
                            width: briSlider.availableWidth
                            height: 6
                            radius: 3
                            color: "#44FFFFFF"
                            Rectangle {
                                width: briSlider.visualPosition * parent.width
                                height: parent.height
                                radius: 3
                                color: "#FFD60A"
                            }
                        }
                        handle: Rectangle {
                            x: briSlider.leftPadding + briSlider.visualPosition *
                               (briSlider.availableWidth - width)
                            y: briSlider.topPadding + briSlider.availableHeight / 2 - height / 2
                            width: 16
                            height: 16
                            radius: 8
                            color: "#FFFFFF"
                        }
                    }
                }
            }

            // --- Батарея ---
            Rectangle {
                width: parent.width
                height: 52
                radius: 10
                color: "#22FFFFFF"
                visible: UPower.displayDevice != null
                Row {
                    anchors.centerIn: parent
                    spacing: 10
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: {
                            const dev = UPower.displayDevice;
                            if (dev === null) return "";
                            if (dev.state === 1) return "\uF0E7";
                            if (dev.percentage > 80) return "\uF240";
                            if (dev.percentage > 60) return "\uF241";
                            if (dev.percentage > 40) return "\uF242";
                            if (dev.percentage > 20) return "\uF243";
                            return "\uF244";
                        }
                        font.family: root.iconFont
                        font.pixelSize: 20
                        color: "#FFFFFF"
                    }
                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        Text {
                            text: UPower.displayDevice
                                  ? Math.round(UPower.displayDevice.percentage) + "%" : ""
                            font.family: root.uiFont
                            font.pixelSize: 14
                            font.weight: Font.Medium
                            color: "#FFFFFF"
                        }
                        Text {
                            text: {
                                const dev = UPower.displayDevice;
                                if (dev === null) return "";
                                if (dev.state === 1) return "Заряжается";
                                if (dev.state === 4) return "Полностью заряжен";
                                return "От батареи";
                            }
                            font.family: root.uiFont
                            font.pixelSize: 11
                            color: "#FFFFFF"
                            opacity: 0.6
                        }
                    }
                }
            }

            // --- Медиа (MPRIS) ---
            Rectangle {
                width: parent.width
                height: mprisCol.implicitHeight + 20
                radius: 10
                color: "#22FFFFFF"
                visible: Mpris.players.values.length > 0

                readonly property var player: Mpris.players.values[0] ?? null

                Column {
                    id: mprisCol
                    anchors.centerIn: parent
                    width: parent.width - 20
                    spacing: 8

                    Text {
                        width: parent.width
                        elide: Text.ElideMiddle
                        text: {
                            const p = Mpris.players.values[0];
                            if (p === undefined) return "";
                            const artist = p.trackArtist ? p.trackArtist + " — " : "";
                            return artist + p.trackTitle;
                        }
                        font.family: root.uiFont
                        font.pixelSize: 13
                        font.weight: Font.Medium
                        color: "#FFFFFF"
                    }
                    Row {
                        spacing: 24
                        anchors.horizontalCenter: parent.horizontalCenter
                        Text {
                            text: "\uF04A" // prev
                            font.family: root.iconFont
                            font.pixelSize: 16
                            color: "#FFFFFF"
                            opacity: {
                                const p = Mpris.players.values[0];
                                return p !== undefined && p.canGoPrevious ? 0.9 : 0.3;
                            }
                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -8
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    const p = Mpris.players.values[0];
                                    if (p !== undefined && p.canGoPrevious) p.previous();
                                }
                            }
                        }
                        Text {
                            text: {
                                const p = Mpris.players.values[0];
                                return p !== undefined &&
                                       p.playbackState === Mpris.PlayingState
                                       ? "\uF04C" : "\uF04B";
                            }
                            font.family: root.iconFont
                            font.pixelSize: 20
                            color: "#FFFFFF"
                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -8
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    const p = Mpris.players.values[0];
                                    if (p !== undefined && p.canTogglePlaying)
                                        p.togglePlaying();
                                }
                            }
                        }
                        Text {
                            text: "\uF04E" // next
                            font.family: root.iconFont
                            font.pixelSize: 16
                            color: "#FFFFFF"
                            opacity: {
                                const p = Mpris.players.values[0];
                                return p !== undefined && p.canGoNext ? 0.9 : 0.3;
                            }
                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -8
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    const p = Mpris.players.values[0];
                                    if (p !== undefined && p.canGoNext) p.next();
                                }
                            }
                        }
                    }
                }
            }

            // --- Календарь ---
            Rectangle {
                width: parent.width
                height: calCol.implicitHeight + 20
                radius: 10
                color: "#22FFFFFF"

                Column {
                    id: calCol
                    anchors.centerIn: parent
                    width: parent.width - 16
                    spacing: 8

                    Row {
                        width: parent.width
                        Text {
                            width: parent.width - 60
                            anchors.verticalCenter: parent.verticalCenter
                            text: Qt.formatDateTime(root.viewDate, "MMMM yyyy")
                            font.family: root.uiFont
                            font.pixelSize: 13
                            font.weight: Font.Medium
                            color: "#FFFFFF"
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 30
                            horizontalAlignment: Text.AlignHCenter
                            text: "\uF053" // chevron-left
                            font.family: root.iconFont
                            font.pixelSize: 12
                            color: "#FFFFFF"
                            opacity: prevArea.containsMouse ? 1 : 0.6
                            MouseArea {
                                id: prevArea
                                anchors.fill: parent
                                anchors.margins: -6
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.viewDate =
                                    new Date(root.calYear, root.calMonth - 1, 1)
                            }
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 30
                            horizontalAlignment: Text.AlignHCenter
                            text: "\uF054" // chevron-right
                            font.family: root.iconFont
                            font.pixelSize: 12
                            color: "#FFFFFF"
                            opacity: nextArea.containsMouse ? 1 : 0.6
                            MouseArea {
                                id: nextArea
                                anchors.fill: parent
                                anchors.margins: -6
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.viewDate =
                                    new Date(root.calYear, root.calMonth + 1, 1)
                            }
                        }
                    }

                    // Дни недели
                    Grid {
                        columns: 7
                        width: parent.width
                        Repeater {
                            model: ["Пн", "Вт", "Ср", "Чт", "Пт", "Сб", "Вс"]
                            Text {
                                required property string modelData
                                width: parent.width / 7
                                horizontalAlignment: Text.AlignHCenter
                                text: modelData
                                font.family: root.uiFont
                                font.pixelSize: 10
                                color: "#FFFFFF"
                                opacity: 0.45
                            }
                        }
                    }

                    // Сетка дней
                    Grid {
                        columns: 7
                        width: parent.width
                        Repeater {
                            model: root.firstOffset + root.daysInMonth
                            Item {
                                required property int index
                                readonly property int day: index - root.firstOffset + 1
                                width: parent.width / 7
                                height: 30
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: 26
                                    height: 26
                                    radius: 13
                                    visible: parent.day >= 1 && root.isToday(parent.day)
                                    color: "#0A84FF"
                                }
                                Text {
                                    anchors.centerIn: parent
                                    visible: parent.day >= 1
                                    text: parent.day
                                    font.family: root.uiFont
                                    font.pixelSize: 11
                                    font.weight: root.isToday(parent.day)
                                                 ? Font.Bold : Font.Normal
                                    color: "#FFFFFF"
                                    opacity: root.isToday(parent.day) ? 1 : 0.8
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
