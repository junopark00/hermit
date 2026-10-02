import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.Material

import HermitTheme 1.0
import Session 1.0

// Offered by StreamSegue when a stream is cut off by a network problem. Counts down, then starts
// a new session for the same host and app, as if the app had been clicked again. If the host is
// offline when the countdown ends, that attempt is skipped; after the last attempt the normal
// error dialog is shown.
NavigableDialog {
    id: dialog

    readonly property int maxAttempts: 3
    readonly property int delaySeconds: 5

    property Session nextSession: null
    property string appName: ""
    // The attempt that runs when the countdown ends, 1..maxAttempts
    property int attempt: 1
    // Why the last stream (or attempt) ended, shown as detail and in the final error
    property string reason: ""
    property int secondsLeft: delaySeconds
    property bool hostOnline: true

    function start(session, name, attemptNumber, reasonText) {
        nextSession = session
        appName = name
        attempt = attemptNumber
        reason = reasonText || ""
        restartCountdown()
        open()
    }

    function restartCountdown() {
        secondsLeft = delaySeconds
        hostOnline = nextSession !== null && nextSession.isHostOnline()
        countdown.restart()
    }

    function connectNow() {
        countdown.stop()
        var session = nextSession
        nextSession = null
        close()

        var component = Qt.createComponent("StreamSegue.qml")
        var segue = component.createObject(stackView, {
                                               "appName": appName,
                                               "session": session,
                                               "isResume": true,
                                               "reconnectAttempt": attempt
                                           })
        stackView.push(segue)
    }

    function giveUp() {
        countdown.stop()
        nextSession = null
        close()

        var text = reason
        if (!hostOnline) {
            text = qsTr("The host is offline.") + (text ? "\n\n" + text : "")
        }
        if (text) {
            streamSegueErrorDialog.text = text
            streamSegueErrorDialog.quitAfter = false
            streamSegueErrorDialog.open()
        }
    }

    function cancel() {
        countdown.stop()
        nextSession = null
        gc()
    }

    Timer {
        id: countdown
        interval: 1000
        repeat: true
        onTriggered: {
            dialog.hostOnline = dialog.nextSession !== null && dialog.nextSession.isHostOnline()
            dialog.secondsLeft--
            if (dialog.secondsLeft > 0) {
                return
            }

            if (dialog.hostOnline) {
                dialog.connectNow()
            }
            else if (dialog.attempt < dialog.maxAttempts) {
                // Skip this attempt and keep waiting for the host
                dialog.attempt++
                dialog.restartCountdown()
            }
            else {
                dialog.giveUp()
            }
        }
    }

    title: qsTr("Connection lost")
    width: Math.min(parent ? parent.width - 48 : 480, 480)
    padding: 24
    topPadding: 8
    // Clicking outside must not silently cancel the reconnection; Escape (gamepad B) still does.
    closePolicy: Popup.CloseOnEscape

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

    ColumnLayout {
        width: dialog.availableWidth
        spacing: 8

        Label {
            Layout.fillWidth: true
            text: dialog.hostOnline ? qsTr("The connection was lost. Reconnecting in %1 s.").arg(dialog.secondsLeft)
                                    : qsTr("The host is offline. Checking again in %1 s.").arg(dialog.secondsLeft)
            font.features: { "tnum": 1 }
            font.pointSize: 11
            color: HermitTheme.textPrimary
            wrapMode: Text.Wrap
        }

        Label {
            Layout.fillWidth: true
            text: (dialog.appName ? dialog.appName + " · " : "") + qsTr("Attempt %1 of %2").arg(dialog.attempt).arg(dialog.maxAttempts)
            font.features: { "tnum": 1 }
            font.pointSize: 10
            color: HermitTheme.textSecondary
            elide: Text.ElideRight
        }

        Label {
            Layout.fillWidth: true
            Layout.topMargin: 4
            visible: text !== ""
            text: dialog.reason.replace(/\n\n/g, "\n")
            font.pointSize: 9
            color: HermitTheme.textHelper
            wrapMode: Text.Wrap
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

            Item { Layout.fillWidth: true }
            FooterButton {
                text: qsTr("Cancel")
                flat: true
                onClicked: dialog.reject()
            }
            FooterButton {
                id: connectButton
                text: qsTr("Connect now")
                Material.background: HermitTheme.accentFill
                Material.foreground: "white"
                Material.roundedScale: Material.NotRounded
                onClicked: dialog.connectNow()
            }
        }
    }

    // Escape, gamepad B and the Cancel button
    onRejected: cancel()

    onOpened: connectButton.forceActiveFocus(Qt.TabFocus)
}
