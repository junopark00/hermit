import QtQuick 2.0
import QtQuick.Controls 2.2
import QtQuick.Window 2.2

import SdlGamepadKeyNavigation 1.0
import Session 1.0
import SystemProperties 1.0
import StreamingPreferences 1.0

Item {
    property Session session
    property string appName
    property string stageText : isResume ? qsTr("Resuming %1...").arg(appName) :
                                           qsTr("Starting %1...").arg(appName)
    property bool isResume : false
    property bool quitAfter : false
    // Automatic reconnection: which attempt this session is (0 for a normal launch),
    // and when the stream started (0 if it never did)
    property int reconnectAttempt : 0
    property double streamStartedMs : 0

    function stageStarting(stage)
    {
        // Update the spinner text
        stageText = qsTr("Starting %1...").arg(stage)
    }

    function stageFailed(stage, errorCode, failingPorts)
    {
        // Display the error dialog after Session::exec() returns
        streamSegueErrorDialog.text = qsTr("Starting %1 failed: Error %2").arg(stage).arg(errorCode)

        if (failingPorts) {
            streamSegueErrorDialog.text += "\n\n" + qsTr("Check your firewall and port forwarding rules for port(s): %1").arg(failingPorts)
        }
    }

    function connectionStarted()
    {
        streamStartedMs = Date.now()

        // Hide the UI contents so the user doesn't
        // see them briefly when we pop off the StackView
        stageSpinner.visible = false
        stageLabel.visible = false
        hintText.visible = false

        // Hide the window now that streaming has begun
        window.visible = false
    }

    function displayLaunchError(text)
    {
        // Display the error dialog after Session::exec() returns
        streamSegueErrorDialog.text = text
        console.error(text)
    }

    function quitStarting()
    {
        // Avoid the push transition animation
        var component = Qt.createComponent("QuitSegue.qml")
        stackView.replace(stackView.currentItem, component.createObject(stackView, {"appName": appName}), StackView.Immediate)

        // Show the Qt window again to show quit segue
        window.visible = true
    }

    function sessionFinished()
    {
        // Re-enable GUI gamepad usage now
        SdlGamepadKeyNavigation.enable()

        // Hermit: the stream panel ended the stream to apply new settings. Start the next
        // session at once (it resumes the running host app); no summary or dialog in between.
        if (session && session.restartRequested() && !streamSegueErrorDialog.text) {
            var nextSegue = Qt.createComponent("StreamSegue.qml").createObject(stackView, {
                                                                                 "appName": appName,
                                                                                 "session": session.createReconnectSession(),
                                                                                 "isResume": true,
                                                                                 "quitAfter": quitAfter
                                                                             })
            window.visible = true
            stackView.replace(stackView.currentItem, nextSegue, StackView.Immediate)
            return
        }

        // Offer to reconnect if an established stream was cut off by a network problem, or if
        // a reconnection attempt failed before streaming. A stream that lasted over a minute
        // starts a new series of attempts.
        var reconnectSession = null
        var nextAttempt = 0
        if (!quitAfter && StreamingPreferences.autoReconnect && session) {
            var lost = session.connectionLost()
            if (lost || (reconnectAttempt > 0 && streamStartedMs === 0 && streamSegueErrorDialog.text &&
                         session.launchRetryable())) {
                var used = (lost && Date.now() - streamStartedMs > 60000) ? 0 : reconnectAttempt
                if (used < reconnectDialog.maxAttempts) {
                    reconnectSession = session.createReconnectSession()
                    nextAttempt = used + 1
                }
            }
        }

        // Read the session summary before this page (and its session) goes away
        var summary = null
        var previous = null
        if (!quitAfter && !reconnectSession && StreamingPreferences.showSessionSummary && session) {
            summary = session.summary()
            previous = session.previousSessionsAverage()
        }

        // Pop the StreamSegue off the stack if this is a GUI-based app launch
        if (!quitAfter) {
            stackView.pop()
        }

        if (quitAfter && !streamSegueErrorDialog.text) {
            // If this was a CLI launch without errors, exit now
            Qt.quit()
        }
        else {
            // Show the Qt window again after streaming
            window.visible = true

            // Display any launch errors. We do this after
            // the Qt UI is visible again to prevent losing
            // focus on the dialog which would impact gamepad
            // users.
            if (reconnectSession) {
                // The error becomes the dialog's detail; it is shown normally if we give up
                var reason = streamSegueErrorDialog.text
                streamSegueErrorDialog.text = ""
                reconnectDialog.start(reconnectSession, appName, nextAttempt, reason)
            }
            else if (streamSegueErrorDialog.text) {
                streamSegueErrorDialog.quitAfter = quitAfter
                streamSegueErrorDialog.open()
            }
            else if (summary && summary.avgFps !== undefined) {
                sessionSummaryDialog.show(summary, previous)
            }
        }
    }

    function sessionReadyForDeletion()
    {
        // Garbage collect the Session object since it's pretty heavyweight
        // and keeps other libraries (like SDL_TTF) around until it is deleted.
        session = null
        gc()
    }

    StackView.onDeactivating: {
        // Show the toolbar again when popped off the stack
        toolBar.visible = true

        // Re-enable GUI gamepad usage now
        SdlGamepadKeyNavigation.enable()
    }

    StackView.onActivated: {
        // Hide the toolbar before we start loading
        toolBar.visible = false

        // Hook up our signals
        session.stageStarting.connect(stageStarting)
        session.stageFailed.connect(stageFailed)
        session.connectionStarted.connect(connectionStarted)
        session.displayLaunchError.connect(displayLaunchError)
        session.quitStarting.connect(quitStarting)
        session.sessionFinished.connect(sessionFinished)
        session.readyForDeletion.connect(sessionReadyForDeletion)

        // Command-line launches keep their option overrides out of the saved settings
        session.setSavePreferences(!quitAfter)

        // Ensure the SystemProperties async thread is finished,
        // since it may currently be using the SDL video subsystem
        SystemProperties.waitForAsyncLoad()

        // Kick off the stream
        streamLoader.active = true
    }

    Timer {
        id: startSessionTimer
        onTriggered: {
            // Garbage collect QML stuff before we start streaming,
            // since we'll probably be streaming for a while and we
            // won't be able to GC during the stream.
            gc()

            // Run the streaming session to completion
            session.start()
        }
    }

    Loader {
        id: streamLoader
        active: false
        asynchronous: true

        onLoaded: {
            // Set the hint text. We do this here rather than
            // in the hintText control itself to synchronize
            // with Session.exec() which requires no concurrent
            // gamepad usage.
            // Hermit: the shortcuts worth knowing before the stream takes the screen
            hintText.text = qsTr("Tip:") + " " + qsTr("Ctrl+Alt+Shift+P stream settings · Ctrl+Alt+Shift+H shortcut list · Ctrl+Alt+Shift+Q disconnect")
            if (SdlGamepadKeyNavigation.getConnectedGamepads() > 0) {
                hintText.text += "\n" + qsTr("Gamepad: Start+Select+L1+R1 to disconnect")
            }

            // Stop GUI gamepad usage now
            SdlGamepadKeyNavigation.disable()

            // Initialize the session and probe for host/client capabilities
            if (!session.initialize(window)) {
                sessionFinished();
                sessionReadyForDeletion();
                return;
            }

            // This spinner is shown only after session.initialize() has completed
            // to prevent active animations from running during decoder probing,
            // which causes re-entrant event loop livelocks with libdecor-gtk.
            stageSpinner.visible = true

            // Don't wait unless we have toasts to display
            startSessionTimer.interval = 0

            // Display the toasts together in a vertical centered arrangement
            var yOffset = 0
            for (var i = 0; i < session.launchWarnings.length; i++) {
                var text = session.launchWarnings[i]
                console.warn(text)

                // Show the tooltip for 3 seconds
                var toast = Qt.createQmlObject('import QtQuick.Controls 2.2; ToolTip {}', parent, '')
                toast.timeout = 3000
                toast.text = text
                toast.y += yOffset
                toast.visible = true

                // Offset the next toast below the previous one
                yOffset = toast.y + toast.padding + toast.height

                // Allow an extra 500 ms for the tooltip's fade-out animation to finish
                startSessionTimer.interval = toast.timeout + 500;
            }

            // Start the timer to wait for toasts (or start the session immediately)
            startSessionTimer.start()
        }

        sourceComponent: Item {}
    }

    Row {
        anchors.centerIn: parent
        spacing: 5

        BusyIndicator {
            id: stageSpinner
            running: visible
            visible: false
        }

        Label {
            id: stageLabel
            height: stageSpinner.height
            text: stageText
            font.pointSize: 20
            verticalAlignment: Text.AlignVCenter

            wrapMode: Text.Wrap
        }
    }

    Label {
        id: hintText
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 50
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(implicitWidth, parent.width - 48)
        font.pointSize: 14
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter

        wrapMode: Text.Wrap
    }
}
