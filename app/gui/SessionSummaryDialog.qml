import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.Material

import HermitTheme 1.0
import StreamingPreferences 1.0
import SessionHistory 1.0

// Shown after a stream ends: a compact report of the session, as a metrics table compared
// with the previous sessions, plus notes when something needs attention.
NavigableDialog {
    id: dialog

    property var summary: ({})
    property var previous: ({})

    function show(summaryMap, previousMap) {
        summary = summaryMap
        previous = previousMap || {}
        open()
    }

    function fixed(value, digits) {
        return (value === undefined || value === null || isNaN(value)) ? "–" : Number(value).toFixed(digits)
    }

    // Status of a value where lower is better: 0 good, 1 attention, 2 poor, -1 not rated.
    function level(value, good, bad) {
        if (value === undefined || value === null || isNaN(value)) return -1
        return value <= good ? 0 : (value <= bad ? 1 : 2)
    }

    function levelText(l) {
        return l === 0 ? qsTr("Good") : l === 1 ? qsTr("Check") : l === 2 ? qsTr("Poor") : ""
    }

    function levelColor(l) {
        return l === 0 ? HermitTheme.success : l === 1 ? HermitTheme.warning : l === 2 ? HermitTheme.danger : "transparent"
    }

    function previousValue(key, digits, unit) {
        if (!previous.sessions || previous[key] === undefined) return "–"
        return fixed(previous[key], digits) + unit
    }

    function notes() {
        var list = []
        if (summary.framePacing && summary.queueMs > 3) {
            list.push(qsTr("Frame pacing added %1 ms of queue delay. If the picture stays smooth, turn it off in Settings or in the stream settings (Ctrl+Alt+Shift+P) for faster response.").arg(fixed(summary.queueMs, 1)))
        }
        if (summary.networkDropPct > 1) {
            list.push(qsTr("Many frames were lost on the network. Try a lower bitrate or a wired connection."))
        }
        return list
    }

    // The bitrate changed during the stream (live, by the user or automatic bitrate): its
    // time-weighted average, with the ceiling or the last value
    function bitrateText() {
        if (!summary.bitrateMbps) return "–"
        if (summary.autoBitrate) {
            return qsTr("Automatic · average %1 Mbps (up to %2)").arg(fixed(summary.bitrateMbps, 0)).arg(fixed(summary.bitrateChosenMbps, 0))
        }
        if (summary.bitrateChanged) {
            return qsTr("Average %1 Mbps (last %2)").arg(fixed(summary.bitrateMbps, 0)).arg(fixed(summary.bitrateLastMbps, 0))
        }
        return fixed(summary.bitrateMbps, 0) + " Mbps"
    }

    function durationText(minutes) {
        var total = Math.round(minutes)
        if (total < 60) return qsTr("%1 min").arg(Math.max(1, total))
        return qsTr("%1 h %2 min").arg(Math.floor(total / 60)).arg(total % 60)
    }

    readonly property var rows: [
        // Not rated: the host sends frames only when the screen changes, so a quiet screen lowers
        // the average without anything being wrong.
        { name: qsTr("Average FPS"), value: fixed(summary.avgFps, 1) + (summary.targetFps ? " / " + summary.targetFps : ""),
          prev: previousValue("avgFps", 1, ""), level: -1 },
        { name: qsTr("Network loss"), value: fixed(summary.networkDropPct, 2) + "%",
          prev: previousValue("networkDropPct", 2, "%"), level: level(summary.networkDropPct, 0.1, 1) },
        { name: qsTr("Jitter drops"), value: fixed(summary.jitterDropPct, 2) + "%",
          prev: previousValue("jitterDropPct", 2, "%"), level: level(summary.jitterDropPct, 0.1, 1) },
        { name: qsTr("Host latency, average"), value: fixed(summary.hostLatencyAvgMs, 1) + " ms",
          prev: previousValue("hostLatencyAvgMs", 1, " ms"), level: level(summary.hostLatencyAvgMs, 8, 16) },
        { name: qsTr("Host latency, maximum"), value: fixed(summary.hostLatencyMaxMs, 1) + " ms", prev: "–", level: -1 },
        { name: qsTr("Round trip (RTT)"), value: summary.rttMs ? summary.rttMs + " ms  ±" + (summary.rttVarianceMs || 0) : "–",
          prev: previousValue("rttMs", 0, " ms"), level: level(summary.rttMs, 30, 80) },
        { name: qsTr("Queue delay"), value: fixed(summary.queueMs, 1) + " ms",
          prev: previousValue("queueMs", 1, " ms"), level: level(summary.queueMs, 2, 8) },
        { name: qsTr("Decode / render"), value: fixed(summary.decodeMs, 1) + " / " + fixed(summary.renderMs, 1) + " ms", prev: "–", level: -1 }
    ]

    title: qsTr("Session summary")
    width: Math.min(parent ? parent.width - 48 : 680, 680)
    // The content scrolls when the window is shorter than the dialog (the default window is 600 px).
    height: Math.min(implicitHeight, parent ? parent.height - 48 : implicitHeight)
    padding: 24
    topPadding: 8

    background: Rectangle {
        color: HermitTheme.layer1
        border.color: HermitTheme.borderSubtle
        radius: 0
    }

    header: Label {
        text: dialog.title
        padding: 24
        bottomPadding: 4
        font.family: HermitTheme.fontFamily
        font.pointSize: 15
        font.weight: Font.DemiBold
        color: HermitTheme.textPrimary
    }

    ScrollView {
        id: scroller
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true

    ColumnLayout {
        width: scroller.availableWidth
        spacing: 20

        // Session facts as label/value pairs
        GridLayout {
            Layout.fillWidth: true
            columns: dialog.width > 560 ? 4 : 2
            columnSpacing: 24
            rowSpacing: 12

            Repeater {
                model: [
                    { label: qsTr("Host"), value: summary.host || "–" },
                    { label: qsTr("App"), value: summary.app || "–" },
                    { label: qsTr("Duration"), value: summary.durationMin !== undefined ? durationText(summary.durationMin) : "–" },
                    { label: qsTr("Ended"), value: summary.endedAt || "–" },
                    { label: qsTr("Video"), span: 2, value: [summary.resolution, summary.codec].filter(function(x) { return x }).join(" · ") || "–" },
                    { label: qsTr("Bitrate"), value: bitrateText() },
                    { label: qsTr("Display"), value: [summary.windowed ? qsTr("Window") : qsTr("Full screen"),
                                                      summary.vsync ? qsTr("V-Sync") : "",
                                                      summary.framePacing ? qsTr("Frame pacing") : ""].filter(function(x) { return x }).join(" · ") }
                ]
                delegate: ColumnLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: modelData.span || 1
                    Layout.columnSpan: dialog.width > 560 ? (modelData.span || 1) : 1
                    spacing: 2
                    Label {
                        text: modelData.label
                        font.family: HermitTheme.labelFamily
                        font.pointSize: 9
                        font.letterSpacing: 0.3
                        color: HermitTheme.textHelper
                    }
                    Label {
                        Layout.fillWidth: true
                        text: modelData.value
                        font.features: { "tnum": 1 }
                        font.pointSize: 10
                        color: HermitTheme.textPrimary
                        wrapMode: Text.Wrap
                    }
                }
            }
        }

        // Metrics table
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 32
                color: HermitTheme.layer2

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 12
                    Repeater {
                        model: [ { t: qsTr("Metric"), w: 3, right: false }, { t: qsTr("This session"), w: 2, right: true },
                                 { t: qsTr("Previous 10 avg."), w: 2, right: true }, { t: qsTr("Status"), w: 1.4, right: false } ]
                        delegate: Label {
                            Layout.fillWidth: true
                            Layout.preferredWidth: modelData.w * 100
                            text: modelData.t
                            horizontalAlignment: modelData.right ? Text.AlignRight : Text.AlignLeft
                            font.family: HermitTheme.labelFamily
                            font.pointSize: 9
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0.3
                            color: HermitTheme.textSecondary
                            elide: Text.ElideRight
                        }
                    }
                }
            }

            Repeater {
                model: dialog.rows
                delegate: Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 34
                    color: "transparent"

                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: HermitTheme.borderSubtle
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 12

                        Label {
                            Layout.fillWidth: true
                            Layout.preferredWidth: 300
                            text: modelData.name
                            color: HermitTheme.textPrimary
                            font.pointSize: 10
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.preferredWidth: 200
                            text: modelData.value
                            horizontalAlignment: Text.AlignRight
                            font.features: { "tnum": 1 }
                            font.pointSize: 10
                            color: HermitTheme.textPrimary
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.preferredWidth: 200
                            text: modelData.prev
                            horizontalAlignment: Text.AlignRight
                            font.features: { "tnum": 1 }
                            font.pointSize: 10
                            color: HermitTheme.textHelper
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.preferredWidth: 140
                            spacing: 8
                            Rectangle {
                                visible: modelData.level >= 0
                                width: 8
                                height: 8
                                color: dialog.levelColor(modelData.level)
                            }
                            Label {
                                Layout.fillWidth: true
                                text: dialog.levelText(modelData.level)
                                font.pointSize: 10
                                color: HermitTheme.textSecondary
                            }
                        }
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.topMargin: -12
            text: qsTr("The host sends frames only when the screen changes, so the average FPS is lower on quiet screens. Judge smoothness by network loss and jitter drops.")
            wrapMode: Text.Wrap
            font.pointSize: 9
            color: HermitTheme.textHelper
        }

        // Notes, as an inline notification
        Rectangle {
            Layout.fillWidth: true
            visible: dialog.visible && notes().length > 0
            Layout.preferredHeight: noteRow.implicitHeight + 24
            color: HermitTheme.layer2

            Rectangle {
                width: 3
                height: parent.height
                color: HermitTheme.info
            }

            RowLayout {
                id: noteRow
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                anchors.topMargin: 12
                anchors.bottomMargin: 12
                spacing: 12

                Image {
                    Layout.alignment: Qt.AlignTop
                    source: "qrc:/res/hermit/info.svg"
                    sourceSize.width: 16
                    sourceSize.height: 16
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Repeater {
                        model: dialog.visible ? notes() : []
                        delegate: Label {
                            Layout.fillWidth: true
                            text: modelData
                            wrapMode: Text.Wrap
                            font.pointSize: 10
                            color: HermitTheme.textPrimary
                        }
                    }
                }
            }
        }
    }
    }

    component FooterButton: Button {
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        Keys.onRightPressed: nextItemInFocusChain(true).forceActiveFocus(Qt.TabFocus)
        Keys.onLeftPressed: nextItemInFocusChain(false).forceActiveFocus(Qt.TabFocus)
    }

    footer: Rectangle {
        implicitHeight: 64
        color: HermitTheme.layer1

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 24
            spacing: 8

            FooterButton {
                text: qsTr("Open history")
                flat: true
                visible: summary.historySaved === true
                onClicked: Qt.openUrlExternally(SessionHistory.folderUrl)
            }
            FooterButton {
                text: qsTr("Don't show again")
                flat: true
                onClicked: {
                    StreamingPreferences.showSessionSummary = false
                    StreamingPreferences.save()
                    dialog.close()
                }
            }
            Item { Layout.fillWidth: true }
            FooterButton {
                id: closeButton
                text: qsTr("Close")
                Material.background: HermitTheme.accentFill
                Material.foreground: "white"
                Material.roundedScale: Material.NotRounded
                onClicked: dialog.close()
            }
        }
    }

    onOpened: closeButton.forceActiveFocus(Qt.TabFocus)
}
