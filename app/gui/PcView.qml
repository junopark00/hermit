import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import ComputerModel 1.0

import ComputerManager 1.0
import StreamingPreferences 1.0
import SystemProperties 1.0
import SdlGamepadKeyNavigation 1.0

CenteredGridView {
    property ComputerModel computerModel : createModel()

    id: pcGrid
    focus: true
    activeFocusOnTab: true
    topMargin: 20
    bottomMargin: 5
    cellWidth: 310; cellHeight: 330;
    objectName: qsTr("Computers")

    Component.onCompleted: {
        // Don't show any highlighted item until interacting with them.
        // We do this here instead of onActivated to avoid losing the user's
        // selection when backing out of a different page of the app.
        currentIndex = -1
    }

    // Note: Any initialization done here that is critical for streaming must
    // also be done in CliStartStreamSegue.qml, since this code does not run
    // for command-line initiated streams.
    StackView.onActivated: {
        // Setup signals on CM
        ComputerManager.computerAddCompleted.connect(addComplete)

        // Highlight the first item if a gamepad is connected
        if (currentIndex === -1 && SdlGamepadKeyNavigation.getConnectedGamepads() > 0) {
            currentIndex = 0
        }
    }

    StackView.onDeactivating: {
        ComputerManager.computerAddCompleted.disconnect(addComplete)
    }

    function pairingComplete(error)
    {
        // Close the PIN dialog
        pairDialog.close()

        // Display a failed dialog if we got an error
        if (error !== undefined) {
            errorDialog.text = error
            errorDialog.helpText = ""
            errorDialog.open()
        }
    }

    function addComplete(success)
    {
        if (!success) {
            errorDialog.text = qsTr("Unable to connect to the specified PC.")
            errorDialog.helpText = qsTr("Click the Help button for possible solutions.")
            errorDialog.open()
        }
    }

    function createModel()
    {
        var model = Qt.createQmlObject('import ComputerModel 1.0; ComputerModel {}', parent, '')
        model.initialize(ComputerManager)
        model.pairingCompleted.connect(pairingComplete)
        model.powerQueryCompleted.connect(shutdownPcDialog.queryComplete)
        model.powerActionCompleted.connect(shutdownPcDialog.actionComplete)
        return model
    }

    Row {
        anchors.centerIn: parent
        spacing: 5
        visible: pcGrid.count === 0

        BusyIndicator {
            id: searchSpinner
            visible: StreamingPreferences.enableMdns
            running: visible
        }

        Label {
            height: searchSpinner.height
            elide: Label.ElideRight
            text: StreamingPreferences.enableMdns ? qsTr("Searching for compatible hosts on your local network...")
                                                  : qsTr("Automatic PC discovery is disabled. Add your PC manually.")
            font.pointSize: 20
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.Wrap
        }
    }

    model: computerModel

    delegate: NavigableItemDelegate {
        width: 300; height: 320;
        grid: pcGrid

        property alias pcContextMenu : pcContextMenuLoader.item

        Image {
            id: pcIcon
            anchors.horizontalCenter: parent.horizontalCenter
            source: "qrc:/res/desktop_windows-48px.svg"
            sourceSize {
                width: 200
                height: 200
            }
        }

        Image {
            // TODO: Tooltip
            id: stateIcon
            anchors.horizontalCenter: pcIcon.horizontalCenter
            anchors.verticalCenter: pcIcon.verticalCenter
            anchors.verticalCenterOffset: !model.online ? -18 : -16
            visible: !model.statusUnknown && (!model.online || !model.paired)
            source: !model.online ? "qrc:/res/warning_FILL1_wght300_GRAD200_opsz24.svg" : "qrc:/res/baseline-lock-24px.svg"
            sourceSize {
                width: !model.online ? 75 : 70
                height: !model.online ? 75 : 70
            }
        }

        BusyIndicator {
            id: statusUnknownSpinner
            anchors.horizontalCenter: pcIcon.horizontalCenter
            anchors.verticalCenter: pcIcon.verticalCenter
            anchors.verticalCenterOffset: -15
            width: 75
            height: 75
            visible: model.statusUnknown
            running: visible
        }

        Label {
            id: pcNameText
            text: model.name

            width: parent.width
            anchors.top: pcIcon.bottom
            anchors.bottom: parent.bottom
            font.pointSize: 36
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            elide: Text.ElideRight
        }

        Loader {
            id: pcContextMenuLoader
            asynchronous: true
            sourceComponent: NavigableMenu {
                id: pcContextMenu
                initiator: pcContextMenuLoader.parent
                MenuItem {
                    text: qsTr("PC Status: %1").arg(model.online ? qsTr("Online") : qsTr("Offline"))
                    font.bold: true
                    enabled: false
                }
                NavigableMenuItem {
                    text: qsTr("View All Apps")
                    onTriggered: {
                        var component = Qt.createComponent("AppView.qml")
                        var appView = component.createObject(stackView, {"computerIndex": index, "objectName": model.name, "showHiddenGames": true})
                        stackView.push(appView)
                    }
                    visible: model.online && model.paired
                }
                NavigableMenuItem {
                    text: qsTr("Wake PC")
                    onTriggered: computerModel.wakeComputer(index)
                    visible: !model.online && model.wakeable
                }

                NavigableMenuItem {
                    text: qsTr("Rename PC")
                    onTriggered: {
                        renamePcDialog.pcIndex = index
                        renamePcDialog.originalName = model.name
                        renamePcDialog.open()
                    }
                }
                NavigableMenuItem {
                    text: qsTr("Delete PC")
                    onTriggered: {
                        deletePcDialog.pcIndex = index
                        deletePcDialog.pcName = model.name
                        deletePcDialog.open()
                    }
                }
                NavigableMenuItem {
                    text: qsTr("View Details")
                    onTriggered: {
                        showPcDetailsDialog.pcDetails = model.details
                        showPcDetailsDialog.open()
                    }
                }
                MenuSeparator {
                    visible: model.online && model.paired
                    height: visible ? implicitHeight : 0
                }
                NavigableMenuItem {
                    text: qsTr("Restart PC…")
                    onTriggered: shutdownPcDialog.start(index, model.name, true)
                    visible: model.online && model.paired
                }
                NavigableMenuItem {
                    text: qsTr("Shut down PC…")
                    onTriggered: shutdownPcDialog.start(index, model.name, false)
                    visible: model.online && model.paired
                }
            }
        }

        onClicked: {
            if (model.online) {
                if (model.paired) {
                    // go to game view
                    var component = Qt.createComponent("AppView.qml")
                    var appView = component.createObject(stackView, {"computerIndex": index, "objectName": model.name})
                    stackView.push(appView)
                }
                else {
                    var pin = computerModel.generatePinString()

                    // Kick off pairing in the background
                    computerModel.pairComputer(index, pin)

                    // Display the pairing dialog
                    pairDialog.pin = pin
                    pairDialog.pcUuid = computerModel.computerUuid(index)
                    pairDialog.open()
                }
            } else if (!model.online) {
                // Using open() here because it may be activated by keyboard
                pcContextMenu.open()
            }
        }

        onPressAndHold: {
            // popup() ensures the menu appears under the mouse cursor
            if (pcContextMenu.popup) {
                pcContextMenu.popup()
            }
            else {
                // Qt 5.9 doesn't have popup()
                pcContextMenu.open()
            }
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton;
            onClicked: {
                parent.pressAndHold()
            }
        }

        Keys.onMenuPressed: {
            // We must use open() here so the menu is positioned on
            // the ItemDelegate and not where the mouse cursor is
            pcContextMenu.open()
        }

        Keys.onDeletePressed: {
            deletePcDialog.pcIndex = index
            deletePcDialog.pcName = model.name
            deletePcDialog.open()
        }
    }

    ErrorMessageDialog {
        id: errorDialog
    }

    NavigableMessageDialog {
        id: pairDialog
        closePolicy: Popup.CloseOnEscape

        // don't allow edits to the rest of the window while open
        property string pin : "0000"
        // Hermit: the PC by uuid, for the pairing page button (the list can be re-sorted meanwhile)
        property string pcUuid : ""
        // Hermit: shown under the text when the pairing page could not be opened
        property string notice : ""
        text:qsTr("Please enter %1 on your host PC. This dialog will close when pairing is completed.").arg(pin)+"\n\n"+
             qsTr("Enter the PIN in the Shell web UI on the host PC (https://<host address>:47990).")+
             (SystemProperties.hasBrowser ? "\n\n"+
             qsTr("Open Shell pairing page fills in the PIN and this PC's name for you. The browser warns about the host's self-signed certificate and asks for the web UI password.") : "")+
             (notice !== "" ? "\n\n"+notice : "")
        standardButtons: Dialog.Cancel
        // Hermit: opens the host's pairing page in the browser; the dialog stays open until pairing completes
        actionText: SystemProperties.hasBrowser ? qsTr("Open Shell pairing page") : ""
        onActionClicked: {
            if (!computerModel.openPairingPage(pcUuid, pin)) {
                notice = qsTr("The pairing page could not be opened. Enter the PIN in the web UI yourself.")
            }
        }
        onClosed: {
            notice = ""
        }
        onRejected: {
            // FIXME: We should interrupt pairing here
        }
    }

    NavigableMessageDialog {
        id: deletePcDialog
        // don't allow edits to the rest of the window while open
        property int pcIndex : -1
        property string pcName : ""
        text: qsTr("Are you sure you want to remove '%1'?").arg(pcName)
        standardButtons: Dialog.Yes | Dialog.No

        onAccepted: {
            computerModel.deleteComputer(pcIndex)
        }
    }

    NavigableDialog {
        id: renamePcDialog
        property string label: qsTr("Enter the new name for this PC:")
        property string originalName
        property int pcIndex : -1;

        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened: {
            // Force keyboard focus on the textbox so keyboard navigation works
            editText.forceActiveFocus()
        }

        onClosed: {
            editText.clear()
        }

        onAccepted: {
            if (editText.text) {
                computerModel.renameComputer(pcIndex, editText.text)
            }
        }

        ColumnLayout {
            Label {
                text: renamePcDialog.label
                font.bold: true
            }

            TextField {
                id: editText
                placeholderText: renamePcDialog.originalName
                Layout.fillWidth: true
                focus: true

                Keys.onReturnPressed: {
                    renamePcDialog.accept()
                }

                Keys.onEnterPressed: {
                    renamePcDialog.accept()
                }
            }
        }
    }

    // Hermit: turning the host PC off or restarting it. Asks the host first who else is streaming,
    // so nobody is cut off by accident, then asks for confirmation.
    NavigableDialog {
        id: shutdownPcDialog
        // The PC by uuid: the list can be re-sorted while the dialog is open
        property string pcUuid: ""
        property string pcName: ""
        property bool restart: false
        // "checking", "confirm", "sending", "done" or "error"
        property string phase: "checking"
        property var clients: []
        property string message: ""

        title: restart ? qsTr("Restart %1").arg(pcName) : qsTr("Shut down %1").arg(pcName)
        closePolicy: Popup.CloseOnEscape

        function start(index, name, restartPc) {
            pcUuid = computerModel.computerUuid(index)
            pcName = name
            restart = restartPc
            phase = "checking"
            clients = []
            message = ""
            forceBox.checked = false
            open()
            computerModel.queryPower(pcUuid)
        }

        function showResult() {
            closeButton.forceActiveFocus(Qt.TabFocus)
        }

        function errorText(error) {
            if (error === "permission") {
                return qsTr("This device may not turn the PC off or restart it. In the Shell web UI, open Pairing and allow this device to launch apps.")
            }
            if (error === "gone") {
                return qsTr("This PC is no longer in the list.")
            }
            if (error === "unsupported") {
                return qsTr("This host can't be turned off or restarted remotely. Update Shell on the host PC.")
            }
            return qsTr("Couldn't reach the host: %1").arg(error.replace(/^network:/, ""))
        }

        function queryComplete(uuid, allowed, otherClients, error) {
            if (uuid !== pcUuid || phase !== "checking" || !visible) {
                return
            }
            if (error) {
                message = errorText(error)
                phase = "error"
                showResult()
            }
            else if (!allowed) {
                message = errorText("permission")
                phase = "error"
                showResult()
            }
            else {
                clients = otherClients
                phase = "confirm"
                confirmButton.forceActiveFocus(Qt.TabFocus)
            }
        }

        function actionComplete(uuid, error) {
            if (uuid !== pcUuid || phase !== "sending" || !visible) {
                return
            }
            if (error) {
                message = errorText(error)
                phase = "error"
            }
            else {
                message = restart ? qsTr("%1 restarts in a few seconds. It shows as online again once it has started.").arg(pcName)
                                  : qsTr("%1 turns off in a few seconds.").arg(pcName)
                phase = "done"
            }
            showResult()
        }

        ColumnLayout {
            spacing: 12

            RowLayout {
                spacing: 10
                visible: shutdownPcDialog.phase === "checking" || shutdownPcDialog.phase === "sending"
                BusyIndicator {
                    running: parent.visible
                }
                Label {
                    text: shutdownPcDialog.phase !== "sending" ? qsTr("Checking who is connected…")
                          : shutdownPcDialog.restart ? qsTr("Restarting the PC…") : qsTr("Turning the PC off…")
                }
            }

            // Other devices streaming from this PC right now
            Rectangle {
                visible: shutdownPcDialog.phase === "confirm" && shutdownPcDialog.clients.length > 0
                Layout.fillWidth: true
                Layout.maximumWidth: 420
                implicitHeight: warningColumn.implicitHeight + 24
                color: "transparent"
                border.color: "#f1c21b"
                border.width: 1

                Rectangle {
                    width: 3
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    color: "#f1c21b"
                }

                ColumnLayout {
                    id: warningColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 16
                    anchors.rightMargin: 12
                    spacing: 4

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("%1 other device(s) connected to this PC right now").arg(shutdownPcDialog.clients.length)
                        font.bold: true
                        wrapMode: Text.Wrap
                    }
                    Label {
                        Layout.fillWidth: true
                        text: shutdownPcDialog.clients.join(", ")
                        wrapMode: Text.Wrap
                    }
                    Label {
                        Layout.fillWidth: true
                        text: shutdownPcDialog.restart ? qsTr("Their streams end when the PC restarts.") : qsTr("Their streams end when the PC turns off.")
                        wrapMode: Text.Wrap
                    }
                }
            }

            Label {
                visible: shutdownPcDialog.phase === "confirm"
                Layout.maximumWidth: 420
                text: shutdownPcDialog.restart ? qsTr("Restart %1? Apps on it are asked to close first.").arg(shutdownPcDialog.pcName)
                                               : qsTr("Turn %1 off? Apps on it are asked to close first.").arg(shutdownPcDialog.pcName)
                wrapMode: Text.Wrap
            }

            CheckBox {
                id: forceBox
                visible: shutdownPcDialog.phase === "confirm"
                text: qsTr("Also close apps with unsaved work (it is lost)")
            }

            Label {
                visible: shutdownPcDialog.phase === "error" || shutdownPcDialog.phase === "done"
                Layout.maximumWidth: 420
                text: shutdownPcDialog.message
                wrapMode: Text.Wrap
            }
        }

        footer: DialogButtonBox {
            Button {
                id: confirmButton
                visible: shutdownPcDialog.phase === "confirm"
                flat: true
                text: shutdownPcDialog.restart ? (shutdownPcDialog.clients.length > 0 ? qsTr("Restart anyway") : qsTr("Restart"))
                                               : (shutdownPcDialog.clients.length > 0 ? qsTr("Shut down anyway") : qsTr("Shut down"))
                // No accept role: accepting would close the dialog before the result is shown
                DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
                Keys.onReturnPressed: clicked()
                Keys.onEnterPressed: clicked()
                onClicked: {
                    shutdownPcDialog.phase = "sending"
                    closeButton.forceActiveFocus(Qt.TabFocus)
                    computerModel.powerComputer(shutdownPcDialog.pcUuid, shutdownPcDialog.restart, forceBox.checked)
                }
            }
            Button {
                id: closeButton
                flat: true
                text: shutdownPcDialog.phase === "confirm" || shutdownPcDialog.phase === "checking" ? qsTr("Cancel") : qsTr("Close")
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
                Keys.onReturnPressed: clicked()
                Keys.onEnterPressed: clicked()
            }

            onRejected: shutdownPcDialog.close()
        }
    }

    NavigableMessageDialog {
        id: showPcDetailsDialog
        property string pcDetails : "";
        text: showPcDetailsDialog.pcDetails
        imageSrc: "qrc:/res/baseline-help_outline-24px.svg"
        standardButtons: Dialog.Ok
    }

    ScrollBar.vertical: ScrollBar {}
}
