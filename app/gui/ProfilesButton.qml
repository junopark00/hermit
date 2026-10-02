import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import ConnectionProfiles 1.0
import HermitTheme 1.0

// Toolbar button for connection profiles: apply one, save the current settings as one,
// or delete one.
NavigableToolButton {
    id: button

    iconSource: "qrc:/res/hermit/tune.svg"

    ToolTip.delay: 1000
    ToolTip.timeout: 3000
    ToolTip.visible: hovered && !profileMenu.visible
    ToolTip.text: ConnectionProfiles.current ? qsTr("Profile: %1").arg(ConnectionProfiles.current)
                                             : qsTr("Connection profiles")

    onClicked: {
        ConnectionProfiles.refresh()
        profileMenu.popup(button, 0, button.height)
    }

    Menu {
        id: profileMenu

        Instantiator {
            model: ConnectionProfiles.names
            delegate: MenuItem {
                text: modelData
                checkable: true
                checked: modelData === ConnectionProfiles.current
                onTriggered: {
                    ConnectionProfiles.apply(modelData)
                    appliedToast.text = qsTr("Applied %1: %2").arg(modelData).arg(ConnectionProfiles.describe(modelData))
                    appliedToast.open()
                }
            }
            onObjectAdded: (index, object) => profileMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => profileMenu.removeItem(object)
        }

        MenuItem {
            enabled: false
            visible: ConnectionProfiles.names.length === 0
            height: visible ? implicitHeight : 0
            text: qsTr("No profiles yet")
        }

        MenuSeparator {}

        MenuItem {
            text: qsTr("Save current settings as profile...")
            onTriggered: {
                nameField.text = ConnectionProfiles.current
                saveDialog.open()
            }
        }

        Menu {
            id: deleteMenu
            title: qsTr("Delete profile")
            enabled: ConnectionProfiles.names.length > 0

            Instantiator {
                model: ConnectionProfiles.names
                delegate: MenuItem {
                    text: modelData
                    onTriggered: {
                        deleteDialog.profileName = modelData
                        deleteDialog.open()
                    }
                }
                onObjectAdded: (index, object) => deleteMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => deleteMenu.removeItem(object)
            }
        }
    }

    NavigableDialog {
        id: saveDialog
        title: qsTr("Save connection profile")
        standardButtons: Dialog.Save | Dialog.Cancel
        parent: Overlay.overlay

        function updateSaveButton() {
            // A profile needs a name
            var save = standardButton(Dialog.Save)
            if (save) {
                save.enabled = nameField.text.trim().length > 0
            }
        }

        onOpened: {
            updateSaveButton()
            nameField.forceActiveFocus()
        }
        onAccepted: {
            if (ConnectionProfiles.saveCurrent(nameField.text)) {
                appliedToast.text = qsTr("Saved %1: %2").arg(nameField.text.trim()).arg(ConnectionProfiles.describe(nameField.text.trim()))
                appliedToast.open()
            }
        }

        ColumnLayout {
            spacing: 8

            Label {
                // The settings in ConnectionProfiles' list (connectionprofiles.cpp)
                text: qsTr("Saves resolution, frame rate, bitrate, display mode, V-Sync, frame pacing, codec, HDR, YUV 4:4:4, audio, the performance overlay and large packets. A profile with the same name is replaced.")
                wrapMode: Text.Wrap
                Layout.maximumWidth: 420
                color: HermitTheme.textSecondary
            }

            TextField {
                id: nameField
                Layout.fillWidth: true
                placeholderText: qsTr("For example: 1440p windowed")
                onTextChanged: saveDialog.updateSaveButton()
                onAccepted: {
                    if (text.trim().length > 0) {
                        saveDialog.accept()
                    }
                }
            }
        }
    }

    NavigableMessageDialog {
        id: deleteDialog
        property string profileName
        parent: Overlay.overlay
        standardButtons: Dialog.Yes | Dialog.No
        text: qsTr("Delete the profile '%1'?").arg(profileName)
        onAccepted: ConnectionProfiles.remove(profileName)
    }

    ToolTip {
        id: appliedToast
        parent: Overlay.overlay
        x: (parent.width - width) / 2
        y: parent.height - height - 40
        timeout: 3500
    }
}
