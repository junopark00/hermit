import QtQuick
import QtQuick.Window

import HermitTheme 1.0

// Hermit: a narrow chevron handle at the left or right edge of the stream window (the side set in
// the panel), shown while the pointer can reach it: always in remote desktop mouse mode, in game
// mouse mode only while the mouse is not captured (StreamPanel::sync()). A click opens the stream
// panel on that side; press and hold, then drag, to move it up or down. It never takes focus from
// the stream window. The pointer hidden over the stream shows again here: the MouseArea sets its
// own cursor (and Qt falls back to the arrow) for this separate window.
Window {
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    width: 20
    height: 60
    color: "transparent"

    onClosing: (close) => { close.accepted = false }

    readonly property bool onLeft: panel.handleSide === 1

    // Hidden in the middle of a drag (the panel opened, the stream lost focus): the drag is over
    onVisibleChanged: {
        if (!visible) {
            area.dragging = false
        }
    }

    Rectangle {
        anchors.fill: parent
        color: area.containsMouse || area.dragging ? HermitTheme.layer2 : HermitTheme.layer1
        opacity: area.containsMouse || area.dragging ? 1.0 : 0.7
        border.color: area.dragging ? HermitTheme.accent : HermitTheme.borderStrong
        border.width: 1

        // Chevron pointing into the stream, where the panel opens from
        Canvas {
            id: chevron
            anchors.centerIn: parent
            width: 8
            height: 14
            onPaint: {
                var ctx = getContext("2d")
                ctx.reset()
                ctx.strokeStyle = HermitTheme.accent
                ctx.lineWidth = 2
                ctx.lineCap = "square"
                ctx.beginPath()
                if (onLeft) {
                    ctx.moveTo(1, 1); ctx.lineTo(width - 1, height / 2); ctx.lineTo(1, height - 1)
                } else {
                    ctx.moveTo(width - 1, 1); ctx.lineTo(1, height / 2); ctx.lineTo(width - 1, height - 1)
                }
                ctx.stroke()
            }
            Connections {
                target: panel
                function onHandleSideChanged() { chevron.requestPaint() }
            }
        }

        MouseArea {
            id: area
            property bool dragging: false

            anchors.fill: parent
            hoverEnabled: true
            cursorShape: dragging ? Qt.SizeVerCursor : Qt.PointingHandCursor
            pressAndHoldInterval: 400
            onClicked: panel.openPanel()
            onPressAndHold: {
                dragging = true
                panel.beginHandleDrag()
            }
            onPositionChanged: {
                if (dragging) {
                    panel.dragHandle()
                }
            }
            onReleased: {
                if (dragging) {
                    dragging = false
                    panel.endHandleDrag()
                }
            }
            onCanceled: {
                if (dragging) {
                    dragging = false
                    panel.endHandleDrag()
                }
            }
        }
    }
}
