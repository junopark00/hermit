import QtCore
import QtQuick
import QtQuick.Controls

import HermitTheme 1.0

// A settings group with an accordion header: the whole header row toggles the group, the
// chevron shows which way it will go, and the state is remembered per group. The header is
// focusable, so keyboard and gamepad users can open folded groups with Enter or Space.
GroupBox {
    id: box

    // Key under which the expanded state is stored, and the state used the first time.
    property string settingsKey
    property bool expandedByDefault: true
    property bool expanded: true

    readonly property int headerHeight: 44

    clip: true
    leftPadding: 16
    rightPadding: 16
    topPadding: headerHeight + (expanded ? 12 : 0)
    bottomPadding: expanded ? 16 : 0
    contentHeight: expanded ? (contentChildren.length > 0 ? contentChildren[0].implicitHeight : 0) : 0

    Settings {
        id: store
        category: "hermitSettingsGroups"
    }

    Component.onCompleted: {
        if (settingsKey) {
            var stored = store.value(settingsKey, expandedByDefault)
            expanded = stored === true || stored === "true"
        }
        else {
            expanded = expandedByDefault
        }
        contentItem.visible = Qt.binding(function() { return box.expanded })
    }

    onExpandedChanged: {
        if (settingsKey) {
            store.setValue(settingsKey, expanded)
        }
    }

    label: Rectangle {
        id: header
        x: 0
        width: box.width
        height: box.headerHeight
        color: headerArea.containsMouse || header.activeFocus ? HermitTheme.layerHover : HermitTheme.layer1
        border.width: 1
        border.color: header.activeFocus ? HermitTheme.accent : HermitTheme.borderSubtle

        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: headerTitle.text
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                // Holding the key must not fold and unfold the group repeatedly.
                if (event.isAutoRepeat) {
                    event.accepted = true
                    return
                }
                box.expanded = !box.expanded
                event.accepted = true
            }
        }

        Label {
            id: headerTitle
            anchors.left: parent.left
            anchors.leftMargin: 16
            anchors.right: chevron.left
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: box.title
            textFormat: Text.PlainText
            font.family: HermitTheme.fontFamily
            font.pointSize: 12
            font.weight: Font.DemiBold
            color: HermitTheme.textPrimary
            elide: Text.ElideRight
        }

        Image {
            id: chevron
            anchors.right: parent.right
            anchors.rightMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            source: "qrc:/res/hermit/chevron-down.svg"
            sourceSize.width: 16
            sourceSize.height: 16
            opacity: 0.85
            rotation: box.expanded ? 180 : 0
            Behavior on rotation { NumberAnimation { duration: 120 } }
        }

        MouseArea {
            id: headerArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: box.expanded = !box.expanded
        }
    }

    background: Rectangle {
        y: box.headerHeight - 1
        width: box.width
        height: Math.max(0, box.height - box.headerHeight + 1)
        visible: box.expanded
        color: HermitTheme.layer1
        border.width: 1
        border.color: HermitTheme.borderSubtle
    }
}
