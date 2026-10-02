import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import QtQuick.Controls.Material

import HermitTheme 1.0
import StreamingPreferences 1.0
import SystemProperties 1.0

// Hermit: settings panel over the stream (app/streaming/streampanel.h). Opened with
// Ctrl+Alt+Shift+P or the chevron handle at the edge of the stream window, on the handle's side. The context property "panel"
// is the StreamPanel object. Settings are the same as on the Settings page and are saved.
Window {
    id: root

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    width: 380
    height: 720
    color: HermitTheme.layer1
    title: qsTr("Stream settings")

    Material.theme: Material.Dark
    Material.accent: HermitTheme.accent
    Material.background: HermitTheme.layer1
    Material.foreground: HermitTheme.textPrimary
    Material.roundedScale: Material.ExtraSmallScale

    // Changes that are applied by reconnecting with the new settings
    // The bitrate counts only when the host cannot change it live (Shell can)
    readonly property bool revertNeeded:
        StreamingPreferences.width !== panel.startWidth ||
        StreamingPreferences.height !== panel.startHeight ||
        StreamingPreferences.fps !== panel.startFps ||
        (panel.liveBitrateState === 2 && panel.chosenBitrateFor(StreamingPreferences.bitrateKbps) !== panel.startBitrateKbps) ||
        StreamingPreferences.videoCodecConfig !== panel.startVideoCodecConfig ||
        StreamingPreferences.enableHdr !== panel.startEnableHdr
    // Also offered while a failed live change leaves the host's bitrate uncertain (Revert has
    // nothing to undo there: the retries bring the host to the chosen bitrate)
    readonly property bool reconnectNeeded: revertNeeded || panel.appliedUncertain

    // "Adjust the bitrate to the resolution" (Settings, on until the bitrate is picked by hand):
    // the bitrate for a new resolution or frame rate, 0 when it stays. Set only when reconnecting,
    // so the stream in progress keeps its bitrate and Revert has nothing more to undo.
    readonly property int followingBitrateKbps:
        StreamingPreferences.autoAdjustBitrate &&
        (StreamingPreferences.width !== panel.startWidth || StreamingPreferences.height !== panel.startHeight ||
         StreamingPreferences.fps !== panel.startFps) ?
            StreamingPreferences.getDefaultBitrate(StreamingPreferences.width, StreamingPreferences.height,
                                                   StreamingPreferences.fps, StreamingPreferences.enableYUV444) : 0

    // The typed resolution was not usable
    property bool resolutionWarning: false
    // "Quit app and disconnect" waits for a confirmation in the panel
    property bool confirmingQuit: false
    property bool metricsExpanded: false

    function revertStreamSettings() {
        StreamingPreferences.width = panel.startWidth
        StreamingPreferences.height = panel.startHeight
        StreamingPreferences.fps = panel.startFps
        // The bitrate is part of the reconnect only when the host cannot change it live. Back to
        // the user's last pick that took effect (never a value automatic bitrate chose), with the
        // "adjust the bitrate to the resolution" setting it had (moving the slider turns it off)
        if (panel.liveBitrateState === 2) {
            StreamingPreferences.bitrateKbps = panel.revertBitrateKbps
            StreamingPreferences.autoAdjustBitrate = panel.revertAutoAdjustBitrate
        }
        // A pick may have been saved before the host turned out unable to change it live
        panel.savePreferences()
        StreamingPreferences.videoCodecConfig = panel.startVideoCodecConfig
        StreamingPreferences.enableHdr = panel.startEnableHdr
        resolutionWarning = false
    }

    function applyAndReconnect() {
        if (followingBitrateKbps > 0) {
            StreamingPreferences.bitrateKbps = followingBitrateKbps
        }
        panel.reconnect()
    }

    // Keeps the item with keyboard focus inside the visible part of the scrolled settings
    function ensureVisible(item) {
        var p = item
        while (p && p !== content) {
            p = p.parent
        }
        if (!p || item === content) {
            return
        }
        var flick = scroller.contentItem
        var top = item.mapToItem(content, 0, 0).y - 8
        var bottom = top + item.height + 16
        if (top < flick.contentY) {
            flick.contentY = Math.max(0, top)
        }
        else if (bottom > flick.contentY + flick.height) {
            flick.contentY = Math.max(0, Math.min(bottom - flick.height, flick.contentHeight - flick.height))
        }
    }

    onActiveFocusItemChanged: ensureVisible(activeFocusItem)

    onVisibleChanged: {
        confirmingQuit = false
        if (visible) {
            // Keyboard users start at the first field (typing a bitrate changes nothing until
            // Enter or leaving the field)
            bitrateField.forceActiveFocus(Qt.TabFocusReason)
        }
        else {
            bitrateKeyTimer.stop()
        }
    }

    // Esc (or the same hotkey) closes the panel and returns to the stream
    Shortcut {
        sequences: ["Esc", "Ctrl+Alt+Shift+P"]
        onActivated: panel.closePanel()
    }

    // Alt+F4 must not close the window behind the panel's back (or quit the app)
    onClosing: (close) => {
        close.accepted = false
        panel.closePanel()
    }

    component SectionTitle: Label {
        Layout.fillWidth: true
        Layout.topMargin: 14
        font.family: HermitTheme.labelFamily
        font.pointSize: 10
        font.weight: Font.DemiBold
        color: HermitTheme.textSecondary
        wrapMode: Text.Wrap
    }

    component Helper: Label {
        Layout.fillWidth: true
        font.pointSize: 9
        wrapMode: Text.Wrap
        color: HermitTheme.textHelper
    }

    component Divider: Rectangle {
        Layout.fillWidth: true
        Layout.topMargin: 10
        implicitHeight: 1
        color: HermitTheme.borderSubtle
    }

    component FieldLabel: Label {
        Layout.preferredWidth: 92
        font.pointSize: 10
        color: HermitTheme.textSecondary
    }

    // Applies a bitrate changed with the keyboard once the arrow keys rest (the mouse applies it
    // on release); the stream panel treats it like a held slider meanwhile
    Timer {
        id: bitrateKeyTimer
        interval: 600
        onTriggered: {
            // A mouse drag that started meanwhile applies the value on release
            if (bitrateSlider.pressed) {
                return
            }
            panel.setBitrateEditing(false)
            panel.chooseBitrate(StreamingPreferences.bitrateKbps)
        }
    }

    // 1px border against the stream, on the inner edge
    Rectangle {
        x: panel.handleSide === 1 ? parent.width - width : 0
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 1
        color: HermitTheme.borderStrong
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: panel.handleSide === 1 ? 0 : 1
        anchors.rightMargin: panel.handleSide === 1 ? 1 : 0
        spacing: 0

        // Header
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.rightMargin: 8
            Layout.topMargin: 12
            Layout.bottomMargin: 8
            spacing: 8

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    text: qsTr("Stream settings")
                    font.pointSize: 14
                    font.weight: Font.DemiBold
                    color: HermitTheme.textPrimary
                }
                Label {
                    Layout.fillWidth: true
                    text: panel.hostName + " · " + panel.appName
                    elide: Text.ElideRight
                    font.pointSize: 9
                    color: HermitTheme.textHelper
                }
            }

            ToolButton {
                text: "✕"
                font.pointSize: 12
                onClicked: panel.closePanel()
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: qsTr("Back to the stream (Esc)")
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: HermitTheme.borderSubtle
        }

        ScrollView {
            id: scroller
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true

            ColumnLayout {
                id: content
                width: scroller.availableWidth - 40
                x: 20
                spacing: 6

                // ---- Bitrate: applied live where the host can (Shell) ---------------------
                SectionTitle { text: qsTr("BITRATE") }

                RowLayout {
                    Layout.fillWidth: true
                    Slider {
                        id: bitrateSlider
                        Layout.fillWidth: true
                        from: 500
                        to: StreamingPreferences.unlockBitrate ? 500000 : 150000
                        stepSize: 500
                        snapMode: Slider.SnapOnRelease
                        value: StreamingPreferences.bitrateKbps
                        onMoved: {
                            StreamingPreferences.bitrateKbps = value
                            StreamingPreferences.autoAdjustBitrate = false
                            if (!pressed) {
                                // Arrow keys: applied once they rest
                                panel.setBitrateEditing(true)
                                bitrateKeyTimer.restart()
                            }
                        }
                        // Apply once the slider is released
                        onPressedChanged: {
                            bitrateKeyTimer.stop()
                            panel.setBitrateEditing(pressed)
                            if (!pressed) {
                                panel.chooseBitrate(StreamingPreferences.bitrateKbps)
                            }
                        }
                    }
                    TextField {
                        id: bitrateField
                        Layout.preferredWidth: 58
                        horizontalAlignment: TextInput.AlignRight
                        font.pointSize: 10
                        selectByMouse: true
                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                        validator: DoubleValidator { bottom: 0.5; top: bitrateSlider.to / 1000; decimals: 1; notation: DoubleValidator.StandardNotation }
                        function formatted() {
                            return (Math.round(StreamingPreferences.bitrateKbps / 100) / 10).toString()
                        }
                        text: formatted()
                        function apply() {
                            var mbps = parseFloat(text.replace(",", "."))
                            var kbps = isNaN(mbps) ? -1 : Math.round(Math.min(Math.max(mbps, 0.5), bitrateSlider.to / 1000) * 10) * 100
                            // Typing replaced the binding; follow the setting again
                            text = Qt.binding(formatted)
                            if (kbps < 0 || kbps === StreamingPreferences.bitrateKbps) {
                                return  // nothing new typed (also when the field is only passed through)
                            }
                            StreamingPreferences.bitrateKbps = kbps
                            StreamingPreferences.autoAdjustBitrate = false
                            panel.chooseBitrate(kbps)
                        }
                        onEditingFinished: apply()
                    }
                    Label {
                        text: "Mbps"
                        font.pointSize: 9
                        color: HermitTheme.textSecondary
                    }
                }
                CheckBox {
                    text: qsTr("Adjust automatically to the network")
                    font.pointSize: 10
                    checked: StreamingPreferences.adaptiveBitrate
                    enabled: panel.liveBitrateState !== 2
                    onToggled: {
                        StreamingPreferences.adaptiveBitrate = checked
                        panel.applyLive()
                    }
                    ToolTip.visible: hovered || activeFocus
                    ToolTip.delay: 600
                    ToolTip.timeout: 15000
                    ToolTip.text: qsTr("Lowers the bitrate when frames are lost or the round trip rises, and raises it again up to the chosen bitrate when the network is calm. Needs a Shell host.")
                }
                Helper {
                    visible: StreamingPreferences.adaptiveBitrate && panel.autoBitrateKbps > 0
                    text: qsTr("Automatic: now %1 Mbps (up to %2 Mbps)").arg(Math.round(panel.autoBitrateKbps / 100) / 10).arg(Math.round(panel.chosenBitrateFor(StreamingPreferences.bitrateKbps) / 100) / 10)
                }
                Helper {
                    visible: text.length > 0
                    color: panel.liveBitrateState === 2 || panel.appliedUncertain ? HermitTheme.warning : HermitTheme.textHelper
                    text: panel.appliedUncertain ? qsTr("The last bitrate change could not be confirmed; trying again every 5 seconds. Apply and reconnect if it keeps failing.") :
                          panel.liveBitratePending ? qsTr("Applying the bitrate...") :
                          panel.liveBitrateState === 2 ? qsTr("This host cannot change the bitrate during a stream; it is applied by reconnecting.") : ""
                }
                Helper {
                    property int h264: StreamingPreferences.getRecommendedBitrate(StreamingPreferences.width, StreamingPreferences.height, StreamingPreferences.fps, StreamingPreferences.enableYUV444, false) / 1000
                    property int efficient: StreamingPreferences.getRecommendedBitrate(StreamingPreferences.width, StreamingPreferences.height, StreamingPreferences.fps, StreamingPreferences.enableYUV444, true) / 1000
                    text: StreamingPreferences.videoCodecConfig === StreamingPreferences.VCC_AUTO ?
                              qsTr("Recommended: %1 Mbps (HEVC/AV1: %2 Mbps)").arg(h264).arg(efficient) :
                              qsTr("Recommended: %1 Mbps").arg(StreamingPreferences.videoCodecConfig === StreamingPreferences.VCC_FORCE_H264 ? h264 : efficient)
                }

                Divider {}

                // ---- Resolution, frame rate and codec: applied by reconnecting -------------
                SectionTitle { text: qsTr("RESOLUTION, FRAME RATE AND CODEC (RECONNECT TO APPLY)") }
                Helper { text: qsTr("The picture pauses for 2 to 3 seconds and the host app keeps running.") }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    FieldLabel { text: qsTr("Resolution") }
                    ComboBox {
                        id: resolutionBox
                        Layout.fillWidth: true
                        editable: true
                        font.pointSize: 10
                        property var landscapePresets: ["1280x720", "1920x1080", "2560x1440", "3840x2160"]
                        property var portraitPresets: ["720x1280", "1080x1920"]
                        // Hermit: sizes with the aspect ratio of the display the stream is on, for
                        // 21:9, 16:10, 3:2 and other displays (none for a 16:9 display)
                        property var aspectPresets: {
                            var sizes = SystemProperties.getDisplayAspectResolutions(Math.round(Screen.width * Screen.devicePixelRatio),
                                                                                     Math.round(Screen.height * Screen.devicePixelRatio),
                                                                                     true)
                            var list = []
                            for (var i = 0; i < sizes.length; i++) {
                                var size = sizes[i].width + "x" + sizes[i].height
                                if (landscapePresets.indexOf(size) < 0 && portraitPresets.indexOf(size) < 0) {
                                    list.push(qsTr("%1x%2 (display aspect)").arg(sizes[i].width).arg(sizes[i].height))
                                }
                            }
                            return list
                        }
                        model: {
                            var current = StreamingPreferences.width + "x" + StreamingPreferences.height
                            var list = landscapePresets.concat(aspectPresets, portraitPresets)
                            if (indexOfSize(list, current) < 0) {
                                list.unshift(current)
                            }
                            return list
                        }
                        currentIndex: indexOfSize(model, StreamingPreferences.width + "x" + StreamingPreferences.height)
                        // A size, optionally followed by a label in parentheses (the aspect presets)
                        validator: RegularExpressionValidator { regularExpression: /^\s*\d{3,5}\s*[xX×]\s*\d{3,5}\s*(\([^()]*\))?\s*$/ }

                        // "WxH" of an entry or typed text, or "" if it has no size
                        function sizeOf(text) {
                            var match = /^\s*(\d{3,5})\s*[xX×]\s*(\d{3,5})\s*(\([^()]*\))?\s*$/.exec(text)
                            return match ? parseInt(match[1]) + "x" + parseInt(match[2]) : ""
                        }

                        function indexOfSize(list, size) {
                            for (var i = 0; i < list.length; i++) {
                                if (sizeOf(list[i]) === size) {
                                    return i
                                }
                            }
                            return -1
                        }

                        function applyText(text) {
                            var match = /^\s*(\d{3,5})\s*[xX×]\s*(\d{3,5})\s*(\([^()]*\))?\s*$/.exec(text)
                            // Even sizes only; odd sizes do not work well with the encoders
                            var w = match ? parseInt(match[1]) & ~1 : 0
                            var h = match ? parseInt(match[2]) & ~1 : 0
                            root.resolutionWarning = !(w >= 320 && h >= 240 && w <= 7680 && h <= 4320)
                            if (!root.resolutionWarning) {
                                StreamingPreferences.width = w
                                StreamingPreferences.height = h
                            }
                        }
                        onActivated: (index) => applyText(model[index])
                        onAccepted: applyText(editText)

                        // A typed size counts when the field is left too, not only on Enter
                        Connections {
                            target: resolutionBox.contentItem
                            ignoreUnknownSignals: true
                            function onEditingFinished() { resolutionBox.applyText(resolutionBox.editText) }
                        }
                    }
                }
                Helper {
                    color: root.resolutionWarning ? HermitTheme.warning : HermitTheme.textHelper
                    text: root.resolutionWarning ? qsTr("Sizes from 320x240 to 7680x4320 can be entered.") :
                                                   qsTr("Type any size as width x height, for example 720x1280 for a portrait screen.")
                }

                RowLayout {
                    Layout.fillWidth: true
                    FieldLabel { text: qsTr("Frame rate") }
                    ComboBox {
                        Layout.fillWidth: true
                        font.pointSize: 10
                        property var presets: [30, 60, 90, 120, 144]
                        model: {
                            var list = presets.slice()
                            if (list.indexOf(StreamingPreferences.fps) < 0) {
                                list.push(StreamingPreferences.fps)
                                list.sort(function(a, b) { return a - b })
                            }
                            return list.map(function(v) { return v + " FPS" })
                        }
                        currentIndex: model.indexOf(StreamingPreferences.fps + " FPS")
                        onActivated: (index) => { StreamingPreferences.fps = parseInt(model[index]) }
                    }
                }
                Helper {
                    visible: root.followingBitrateKbps > 0 && root.followingBitrateKbps !== StreamingPreferences.bitrateKbps
                    text: qsTr("Reconnecting also sets the bitrate to %1 Mbps for the new resolution and frame rate.").arg(Math.round(root.followingBitrateKbps / 100) / 10)
                }

                RowLayout {
                    Layout.fillWidth: true
                    FieldLabel { text: qsTr("Video codec") }
                    ComboBox {
                        Layout.fillWidth: true
                        font.pointSize: 10
                        textRole: "text"
                        model: [
                            { text: qsTr("Automatic"), value: StreamingPreferences.VCC_AUTO },
                            { text: "H.264", value: StreamingPreferences.VCC_FORCE_H264 },
                            { text: "HEVC (H.265)", value: StreamingPreferences.VCC_FORCE_HEVC },
                            { text: "AV1", value: StreamingPreferences.VCC_FORCE_AV1 }
                        ]
                        currentIndex: {
                            for (var i = 0; i < model.length; i++) {
                                if (model[i].value === StreamingPreferences.videoCodecConfig) return i
                            }
                            return 0
                        }
                        onActivated: (index) => { StreamingPreferences.videoCodecConfig = model[index].value }
                    }
                }

                CheckBox {
                    text: qsTr("HDR")
                    font.pointSize: 10
                    // As on the Settings page: only where this PC can show an HDR stream
                    enabled: SystemProperties.supportsHdr
                    checked: enabled && StreamingPreferences.enableHdr
                    onToggled: StreamingPreferences.enableHdr = checked
                    ToolTip.visible: !enabled && hovered
                    ToolTip.delay: 600
                    ToolTip.text: qsTr("HDR streaming is not supported on this PC.")
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    spacing: 8
                    Button {
                        text: qsTr("Apply and reconnect")
                        highlighted: true
                        enabled: root.reconnectNeeded
                        Material.roundedScale: Material.NotRounded
                        onClicked: root.applyAndReconnect()
                    }
                    Button {
                        text: qsTr("Revert")
                        flat: true
                        enabled: root.revertNeeded
                        onClicked: root.revertStreamSettings()
                    }
                }

                Divider {}

                // ---- Display: applied at once ------------------------------------------
                SectionTitle { text: qsTr("DISPLAY") }
                Helper { text: qsTr("Applied at once.") }

                CheckBox {
                    text: qsTr("Performance overlay")
                    font.pointSize: 10
                    checked: StreamingPreferences.showPerformanceOverlay
                    onToggled: {
                        StreamingPreferences.showPerformanceOverlay = checked
                        panel.applyLive()
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 28
                    spacing: 8
                    visible: StreamingPreferences.showPerformanceOverlay

                    Label {
                        text: qsTr("Text size")
                        font.pointSize: 9
                        color: HermitTheme.textSecondary
                    }
                    ComboBox {
                        Layout.fillWidth: true
                        font.pointSize: 9
                        textRole: "text"
                        model: [
                            { text: qsTr("Small"), size: 16 },
                            { text: qsTr("Medium"), size: 20 },
                            { text: qsTr("Large"), size: 26 }
                        ]
                        currentIndex: StreamingPreferences.overlayTextSize <= 16 ? 0 : (StreamingPreferences.overlayTextSize >= 26 ? 2 : 1)
                        onActivated: (index) => {
                            StreamingPreferences.overlayTextSize = model[index].size
                            panel.applyLive()
                        }
                    }
                }

                // The twelve metrics stay folded away until asked for
                Button {
                    Layout.leftMargin: 20
                    visible: StreamingPreferences.showPerformanceOverlay
                    text: (root.metricsExpanded ? "▾  " : "▸  ") + qsTr("Choose metrics")
                    flat: true
                    font.pointSize: 9
                    onClicked: root.metricsExpanded = !root.metricsExpanded
                }

                GridLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 28
                    columns: 2
                    columnSpacing: 4
                    rowSpacing: 0
                    visible: StreamingPreferences.showPerformanceOverlay && root.metricsExpanded

                    Repeater {
                        // Bits of StatsOverlay::Metric (app/streaming/video/statsoverlay.h)
                        model: [
                            { bit: 1 << 0, text: qsTr("Video") },
                            { bit: 1 << 2, text: qsTr("Bitrate") },
                            { bit: 1 << 3, text: qsTr("Network loss") },
                            { bit: 1 << 5, text: qsTr("Round trip") },
                            { bit: 1 << 6, text: qsTr("Host latency") },
                            { bit: 1 << 10, text: qsTr("Total latency") },
                            { bit: 1 << 1, text: qsTr("Frame rates") },
                            { bit: 1 << 4, text: qsTr("Jitter drops") },
                            { bit: 1 << 7, text: qsTr("Decode time") },
                            { bit: 1 << 8, text: qsTr("Queue delay") },
                            { bit: 1 << 9, text: qsTr("Render time") },
                            { bit: 1 << 11, text: qsTr("Decoder") }
                        ]
                        delegate: CheckBox {
                            Layout.fillWidth: true
                            text: modelData.text
                            font.pointSize: 9
                            checked: (StreamingPreferences.overlayMetrics & modelData.bit) !== 0
                            onToggled: {
                                StreamingPreferences.overlayMetrics = checked ?
                                            (StreamingPreferences.overlayMetrics | modelData.bit) :
                                            (StreamingPreferences.overlayMetrics & ~modelData.bit)
                                panel.applyLive()
                            }
                        }
                    }
                }

                CheckBox {
                    text: qsTr("V-Sync")
                    font.pointSize: 10
                    checked: StreamingPreferences.enableVsync
                    onToggled: {
                        StreamingPreferences.enableVsync = checked
                        panel.applyLive()
                    }
                }
                CheckBox {
                    text: qsTr("Frame pacing")
                    font.pointSize: 10
                    enabled: StreamingPreferences.enableVsync
                    // Off while V-Sync is off (it has no effect then), as on the Settings page
                    checked: StreamingPreferences.enableVsync && StreamingPreferences.framePacing
                    onToggled: {
                        StreamingPreferences.framePacing = checked
                        panel.applyLive()
                    }
                }
                Helper { text: qsTr("Changing V-Sync or frame pacing recreates the renderer: the picture blinks once.") }

                Divider {}

                // ---- Input and other ---------------------------------------------------
                SectionTitle { text: qsTr("INPUT AND SOUND") }

                CheckBox {
                    // The user's mute: kept when the window loses focus and across reconnects
                    text: qsTr("Mute on this PC")
                    font.pointSize: 10
                    checked: panel.muted
                    onToggled: panel.toggleMute()
                }
                CheckBox {
                    text: qsTr("Sync clipboard with the host")
                    font.pointSize: 10
                    checked: StreamingPreferences.clipboardSync
                    onToggled: {
                        StreamingPreferences.clipboardSync = checked
                        panel.applyLive()
                    }
                }

                Button {
                    Layout.fillWidth: true
                    text: qsTr("Toggle full screen")
                    flat: true
                    font.pointSize: 10
                    onClicked: panel.toggleFullScreen()
                }
                Button {
                    Layout.fillWidth: true
                    text: panel.absoluteMouse ? qsTr("Mouse: remote desktop") : qsTr("Mouse: game")
                    flat: true
                    font.pointSize: 10
                    onClicked: panel.toggleMouseMode()
                    ToolTip.visible: hovered || activeFocus
                    ToolTip.delay: 600
                    ToolTip.timeout: 15000
                    ToolTip.text: qsTr("Click to switch between game mouse (relative, captured) and remote desktop mouse (absolute, follows the pointer). Ctrl+Alt+Shift+M also switches.")
                }
                Button {
                    Layout.fillWidth: true
                    text: qsTr("Keyboard shortcuts")
                    flat: true
                    font.pointSize: 10
                    onClicked: panel.showHotkeys()
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    ToolTip.text: qsTr("Shows the shortcut list over the stream (also Ctrl+Alt+Shift+H)")
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    FieldLabel {
                        Layout.fillWidth: true
                        text: qsTr("Handle position")
                    }
                    ComboBox {
                        id: handleSideBox
                        Layout.preferredWidth: 140
                        font.pointSize: 10
                        model: [qsTr("Right edge"), qsTr("Left edge")]
                        currentIndex: panel.handleSide
                        onActivated: (index) => { panel.handleSide = index }
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        ToolTip.text: qsTr("Where the panel handle sits and the panel opens. Press and hold the handle, then drag, to move it up or down.")
                    }
                }

                Divider {}

                // ---- Session -----------------------------------------------------------
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    Layout.bottomMargin: 16
                    spacing: 8
                    visible: !root.confirmingQuit
                    Button {
                        Layout.fillWidth: true
                        text: qsTr("Disconnect")
                        font.pointSize: 10
                        Material.roundedScale: Material.NotRounded
                        onClicked: panel.disconnect()
                    }
                    Button {
                        Layout.fillWidth: true
                        text: qsTr("Quit app and disconnect")
                        font.pointSize: 10
                        Material.roundedScale: Material.NotRounded
                        Material.foreground: HermitTheme.danger
                        onClicked: {
                            root.confirmingQuit = true
                            cancelQuitButton.forceActiveFocus(Qt.TabFocusReason)
                        }
                    }
                }

                // Quitting the host app loses unsaved work: confirm here, in the panel
                Rectangle {
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    Layout.bottomMargin: 16
                    visible: root.confirmingQuit
                    implicitHeight: confirmColumn.implicitHeight + 24
                    color: HermitTheme.layer2
                    border.color: HermitTheme.danger
                    border.width: 1

                    ColumnLayout {
                        id: confirmColumn
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 8
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Quit %1? Unsaved progress is lost.").arg(panel.appName)
                            font.pointSize: 10
                            wrapMode: Text.Wrap
                            color: HermitTheme.textPrimary
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Button {
                                Layout.fillWidth: true
                                text: qsTr("Quit app")
                                font.pointSize: 10
                                Material.roundedScale: Material.NotRounded
                                Material.background: HermitTheme.danger
                                Material.foreground: HermitTheme.textPrimary
                                onClicked: panel.quitAppAndDisconnect()
                            }
                            Button {
                                id: cancelQuitButton
                                Layout.fillWidth: true
                                text: qsTr("Cancel")
                                flat: true
                                font.pointSize: 10
                                onClicked: root.confirmingQuit = false
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: HermitTheme.borderSubtle
        }
        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.topMargin: 8
            Layout.bottomMargin: 10
            text: qsTr("Ctrl+Alt+Shift+P opens and closes this panel.")
            font.pointSize: 9
            color: HermitTheme.textHelper
        }
    }
}
