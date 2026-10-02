import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.2
import QtQuick.Window 2.2

import StreamingPreferences 1.0
import ComputerManager 1.0
import SdlGamepadKeyNavigation 1.0
import SystemProperties 1.0
import SessionHistory 1.0
import HermitTheme 1.0

Flickable {
    id: settingsPage
    objectName: qsTr("Settings")

    signal languageChanged()

    // Both columns share a centred area of at most this width, so wide windows do not stretch
    // the settings to the edges or leave one side empty.
    readonly property int layoutWidth: Math.min(width, 1280)

    boundsBehavior: Flickable.OvershootBounds

    contentWidth: settingsColumn1.width > settingsColumn2.width ? settingsColumn1.width : settingsColumn2.width
    contentHeight: settingsColumn1.height > settingsColumn2.height ? settingsColumn1.height : settingsColumn2.height

    ScrollBar.vertical: ScrollBar {
        anchors {
            left: parent.right
            leftMargin: -10
        }
    }

    function isChildOfFlickable(item) {
        while (item) {
            if (item.parent === contentItem) {
                return true
            }

            item = item.parent
        }
        return false
    }

    NumberAnimation on contentY {
        id: autoScrollAnimation
        duration: 100
    }

    Window.onActiveFocusItemChanged: {
        var item = Window.activeFocusItem
        if (item) {
            // Ignore non-child elements like the toolbar buttons
            if (!isChildOfFlickable(item)) {
                return
            }

            // Map the focus item's position into our content item's coordinate space
            var pos = item.mapToItem(contentItem, 0, 0)

            // Ensure some extra space is visible around the element we're scrolling to
            var scrollMargin = height > 100 ? 50 : 0

            if (pos.y - scrollMargin < contentY) {
                autoScrollAnimation.from = contentY
                autoScrollAnimation.to = Math.max(pos.y - scrollMargin, 0)
                autoScrollAnimation.start()
            }
            else if (pos.y + item.height + scrollMargin > contentY + height) {
                autoScrollAnimation.from = contentY
                autoScrollAnimation.to = Math.min(pos.y + item.height + scrollMargin - height, contentHeight - height)
                autoScrollAnimation.start()
            }
        }
    }

    StackView.onActivated: {
        // This enables Tab and BackTab based navigation rather than arrow keys.
        // It is required to shift focus between controls on the settings page.
        SdlGamepadKeyNavigation.setUiNavMode(true)

        // Highlight the first item if a gamepad is connected
        if (SdlGamepadKeyNavigation.getConnectedGamepads() > 0) {
            // A folded group hides its controls, so focus its header instead.
            if (basicSettingsGroupBox.expanded) {
                resolutionComboBox.forceActiveFocus(Qt.TabFocus)
            }
            else {
                basicSettingsGroupBox.label.forceActiveFocus(Qt.TabFocus)
            }
        }
    }

    StackView.onDeactivating: {
        SdlGamepadKeyNavigation.setUiNavMode(false)

        // Save the prefs so the Session can observe the changes
        StreamingPreferences.save()
    }

    Component.onDestruction: {
        // Also save preferences on destruction, since we won't get a
        // deactivating callback if the user just closes Hermit
        StreamingPreferences.save()
    }

    Column {
        padding: 10
        id: settingsColumn1
        x: (settingsPage.width - settingsPage.layoutWidth) / 2
        width: settingsPage.layoutWidth / 2
        spacing: 15

        CollapsibleGroupBox {
            id: basicSettingsGroupBox
            settingsKey: "basic"
            expandedByDefault: true
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: qsTr("Basic Settings")
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                Label {
                    width: parent.width
                    id: resFPStitle
                    text: qsTr("Resolution and FPS")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                Label {
                    width: parent.width
                    id: resFPSdesc
                    text: qsTr("Setting values too high for your PC or network connection may cause lag, stuttering, or errors.")
                    font.pointSize: 9
                    wrapMode: Text.Wrap
                }

                Row {
                    spacing: 5
                    width: parent.width

                    AutoResizingComboBox {
                        property int lastIndexValue

                        function addDetectedResolution(friendlyNamePrefix, rect) {
                            insertResolution(friendlyNamePrefix+" ("+rect.width+"x"+rect.height+")", rect)
                        }

                        // Inserts a preset in order of size, unless the list already has that size
                        function insertResolution(text, rect) {
                            var indexToAdd = 0
                            for (var j = 0; j < resolutionComboBox.count; j++) {
                                var existing_width = parseInt(resolutionListModel.get(j).video_width);
                                var existing_height = parseInt(resolutionListModel.get(j).video_height);

                                if (rect.width === existing_width && rect.height === existing_height) {
                                    // Duplicate entry, skip
                                    indexToAdd = -1
                                    break
                                }
                                else if (rect.width * rect.height > existing_width * existing_height) {
                                    // Candidate entrypoint after this entry
                                    indexToAdd = j + 1
                                }
                            }

                            // Insert this display's resolution if it's not a duplicate
                            if (indexToAdd >= 0) {
                                resolutionListModel.insert(indexToAdd,
                                                           {
                                                               "text": text,
                                                               "video_width": ""+rect.width,
                                                               "video_height": ""+rect.height,
                                                               "is_custom": false
                                                           })
                            }
                        }

                        // ignore setting the index at first, and actually set it when the component is loaded
                        Component.onCompleted: {
                            // Refresh display data before using it to build the list
                            SystemProperties.refreshDisplays()

                            // Add native and safe area resolutions for all attached displays
                            var done = false
                            for (var displayIndex = 0; !done; displayIndex++) {
                                var screenRect = SystemProperties.getNativeResolution(displayIndex);
                                var safeAreaRect = SystemProperties.getSafeAreaResolution(displayIndex);

                                if (screenRect.width === 0) {
                                    // Exceeded max count of displays
                                    done = true
                                    break
                                }

                                addDetectedResolution(qsTr("Native"), screenRect)
                                addDetectedResolution(qsTr("Native (Excluding Notch)"), safeAreaRect)
                            }

                            // Prune resolutions that are over the decoder's maximum
                            var max_pixels = SystemProperties.maximumResolution.width * SystemProperties.maximumResolution.height;
                            if (max_pixels > 0) {
                                for (var j = 0; j < resolutionComboBox.count; j++) {
                                    var existing_width = parseInt(resolutionListModel.get(j).video_width);
                                    var existing_height = parseInt(resolutionListModel.get(j).video_height);

                                    if (existing_width * existing_height > max_pixels) {
                                        resolutionListModel.remove(j)
                                        j--
                                    }
                                }
                            }

                            // Hermit: presets with the aspect ratio of the display this window is on
                            // (the primary display if unknown), for 21:9, 16:10, 3:2 and other
                            // displays. 2160 lines only if 4K is offered; sizes already in the list
                            // (or within 1% of one) and sizes over the decoder's maximum are left out.
                            var offers4K = false
                            var existingSizes = []
                            for (var k = 0; k < resolutionListModel.count; k++) {
                                var entry = resolutionListModel.get(k)
                                existingSizes.push(entry.video_width + "x" + entry.video_height)
                                if (entry.video_width === "3840" && entry.video_height === "2160") {
                                    offers4K = true
                                }
                            }
                            var aspectSizes = SystemProperties.getDisplayAspectResolutions(Math.round(Screen.width * Screen.devicePixelRatio),
                                                                                           Math.round(Screen.height * Screen.devicePixelRatio),
                                                                                           offers4K, existingSizes)
                            for (var a = 0; a < aspectSizes.length; a++) {
                                var size = aspectSizes[a]
                                if (max_pixels <= 0 || size.width * size.height <= max_pixels) {
                                    insertResolution(qsTr("%1x%2 (display aspect)").arg(size.width).arg(size.height), size)
                                }
                            }

                            // load the saved width/height, and iterate through the ComboBox until a match is found
                            // and set it to that index.
                            var saved_width = StreamingPreferences.width
                            var saved_height = StreamingPreferences.height
                            var index_set = false
                            for (var i = 0; i < resolutionListModel.count; i++) {
                                var el_width = parseInt(resolutionListModel.get(i).video_width);
                                var el_height = parseInt(resolutionListModel.get(i).video_height);

                                if (saved_width === el_width && saved_height === el_height) {
                                    currentIndex = i
                                    index_set = true
                                    break
                                }
                            }

                            if (!index_set) {
                                // We did not find a match. This must be a custom resolution.
                                resolutionListModel.append({
                                                               "text": qsTr("Custom")+" ("+StreamingPreferences.width+"x"+StreamingPreferences.height+")",
                                                               "video_width": ""+StreamingPreferences.width,
                                                               "video_height": ""+StreamingPreferences.height,
                                                               "is_custom": true
                                                           })
                                currentIndex = resolutionListModel.count - 1
                            }
                            else {
                                resolutionListModel.append({
                                                               "text": qsTr("Custom"),
                                                               "video_width": "",
                                                               "video_height": "",
                                                               "is_custom": true
                                                           })
                            }

                            // Since we don't call activate() here, we need to trigger
                            // width calculation manually
                            recalculateWidth()

                            lastIndexValue = currentIndex
                        }

                        id: resolutionComboBox
                        maximumWidth: parent.width / 2
                        textRole: "text"
                        model: ListModel {
                            id: resolutionListModel
                            // Other elements may be added at runtime
                            // based on attached display resolution
                            ListElement {
                                text: qsTr("720p")
                                video_width: "1280"
                                video_height: "720"
                                is_custom: false
                            }
                            ListElement {
                                text: qsTr("1080p")
                                video_width: "1920"
                                video_height: "1080"
                                is_custom: false
                            }
                            ListElement {
                                text: qsTr("1440p")
                                video_width: "2560"
                                video_height: "1440"
                                is_custom: false
                            }
                            ListElement {
                                text: qsTr("4K")
                                video_width: "3840"
                                video_height: "2160"
                                is_custom: false
                            }
                        }

                        function updateBitrateForSelection() {
                            var selectedWidth = parseInt(resolutionListModel.get(currentIndex).video_width)
                            var selectedHeight = parseInt(resolutionListModel.get(currentIndex).video_height)

                            // Only modify the bitrate if the values actually changed
                            if (StreamingPreferences.width !== selectedWidth || StreamingPreferences.height !== selectedHeight) {
                                StreamingPreferences.width = selectedWidth
                                StreamingPreferences.height = selectedHeight

                                if (StreamingPreferences.autoAdjustBitrate) {
                                    StreamingPreferences.bitrateKbps = StreamingPreferences.getDefaultBitrate(StreamingPreferences.width,
                                                                                                              StreamingPreferences.height,
                                                                                                              StreamingPreferences.fps,
                                                                                                              StreamingPreferences.enableYUV444);
                                    slider.value = StreamingPreferences.bitrateKbps
                                }
                            }

                            lastIndexValue = currentIndex
                        }

                        // ::onActivated must be used, as it only listens for when the index is changed by a human
                        onActivated : {
                            if (resolutionListModel.get(currentIndex).is_custom) {
                                customResolutionDialog.open()
                            }
                            else {
                                updateBitrateForSelection()
                            }
                        }

                        NavigableDialog {
                            id: customResolutionDialog
                            standardButtons: Dialog.Ok | Dialog.Cancel
                            onOpened: {
                                // Force keyboard focus on the textbox so keyboard navigation works
                                widthField.forceActiveFocus()

                                // standardButton() was added in Qt 5.10, so we must check for it first
                                if (customResolutionDialog.standardButton) {
                                    customResolutionDialog.standardButton(Dialog.Ok).enabled = customResolutionDialog.isInputValid()
                                }
                            }

                            onClosed: {
                                widthField.clear()
                                heightField.clear()
                            }

                            onRejected: {
                                resolutionComboBox.currentIndex = resolutionComboBox.lastIndexValue
                            }

                            function isInputValid() {
                                // If we have text in either textbox that isn't valid,
                                // reject the input.
                                if ((!widthField.acceptableInput && widthField.text) ||
                                        (!heightField.acceptableInput && heightField.text)) {
                                    return false
                                }

                                // The textboxes need to have text or placeholder text
                                if ((!widthField.text && !widthField.placeholderText) ||
                                        (!heightField.text && !heightField.placeholderText)) {
                                    return false
                                }

                                return true
                            }

                            onAccepted: {
                                // Reject if there's invalid input
                                if (!isInputValid()) {
                                    reject()
                                    return
                                }

                                var width = widthField.text ? widthField.text : widthField.placeholderText
                                var height = heightField.text ? heightField.text : heightField.placeholderText

                                // Find and update the custom entry
                                for (var i = 0; i < resolutionListModel.count; i++) {
                                    if (resolutionListModel.get(i).is_custom) {
                                        resolutionListModel.setProperty(i, "video_width", width)
                                        resolutionListModel.setProperty(i, "video_height", height)
                                        resolutionListModel.setProperty(i, "text", qsTr("Custom")+" ("+width+"x"+height+")")

                                        // Now update the bitrate using the custom resolution
                                        resolutionComboBox.currentIndex = i
                                        resolutionComboBox.updateBitrateForSelection()

                                        // Update the combobox width too
                                        resolutionComboBox.recalculateWidth()
                                        break
                                    }
                                }
                            }

                            ColumnLayout {
                                Label {
                                    text: qsTr("A Shell host sets its virtual display to this resolution.") + "\n\n" +
                                          qsTr("Resolutions that are not supported by your client or host PC may cause streaming errors.") + "\n"
                                    wrapMode: Label.WordWrap
                                    Layout.maximumWidth: 300
                                }

                                Label {
                                    text: qsTr("Enter a custom resolution:")
                                    font.bold: true
                                }

                                RowLayout {
                                    TextField {
                                        id: widthField
                                        maximumLength: 5
                                        inputMethodHints: Qt.ImhDigitsOnly
                                        placeholderText: resolutionListModel.get(resolutionComboBox.currentIndex).video_width
                                        validator: IntValidator{bottom:256; top:8192}
                                        focus: true

                                        onTextChanged: {
                                            // standardButton() was added in Qt 5.10, so we must check for it first
                                            if (customResolutionDialog.standardButton) {
                                                customResolutionDialog.standardButton(Dialog.Ok).enabled = customResolutionDialog.isInputValid()
                                            }
                                        }

                                        Keys.onReturnPressed: {
                                            customResolutionDialog.accept()
                                        }

                                        Keys.onEnterPressed: {
                                            customResolutionDialog.accept()
                                        }
                                    }

                                    Label {
                                        text: "x"
                                        font.bold: true
                                    }

                                    TextField {
                                        id: heightField
                                        maximumLength: 5
                                        inputMethodHints: Qt.ImhDigitsOnly
                                        placeholderText: resolutionListModel.get(resolutionComboBox.currentIndex).video_height
                                        validator: IntValidator{bottom:256; top:8192}

                                        onTextChanged: {
                                            // standardButton() was added in Qt 5.10, so we must check for it first
                                            if (customResolutionDialog.standardButton) {
                                                customResolutionDialog.standardButton(Dialog.Ok).enabled = customResolutionDialog.isInputValid()
                                            }
                                        }

                                        Keys.onReturnPressed: {
                                            customResolutionDialog.accept()
                                        }

                                        Keys.onEnterPressed: {
                                            customResolutionDialog.accept()
                                        }
                                    }
                                }
                            }
                        }
                    }

                    AutoResizingComboBox {
                        property int lastIndexValue

                        function updateBitrateForSelection() {
                            // Only modify the bitrate if the values actually changed
                            var selectedFps = parseInt(model.get(fpsComboBox.currentIndex).video_fps)
                            if (StreamingPreferences.fps !== selectedFps) {
                                StreamingPreferences.fps = selectedFps

                                if (StreamingPreferences.autoAdjustBitrate) {
                                    StreamingPreferences.bitrateKbps = StreamingPreferences.getDefaultBitrate(StreamingPreferences.width,
                                                                                                              StreamingPreferences.height,
                                                                                                              StreamingPreferences.fps,
                                                                                                              StreamingPreferences.enableYUV444);
                                    slider.value = StreamingPreferences.bitrateKbps
                                }
                            }

                            lastIndexValue = currentIndex
                        }

                        NavigableDialog {
                            function isInputValid() {
                                // If we have text that isn't valid, reject the input.
                                if (!fpsField.acceptableInput && fpsField.text) {
                                    return false
                                }

                                // The textbox needs to have text or placeholder text
                                if (!fpsField.text && !fpsField.placeholderText) {
                                    return false
                                }

                                return true
                            }

                            id: customFpsDialog
                            standardButtons: Dialog.Ok | Dialog.Cancel
                            onOpened: {
                                // Force keyboard focus on the textbox so keyboard navigation works
                                fpsField.forceActiveFocus()

                                // standardButton() was added in Qt 5.10, so we must check for it first
                                if (customFpsDialog.standardButton) {
                                    customFpsDialog.standardButton(Dialog.Ok).enabled = customFpsDialog.isInputValid()
                                }
                            }

                            onClosed: {
                                fpsField.clear()
                            }

                            onRejected: {
                                fpsComboBox.currentIndex = fpsComboBox.lastIndexValue
                            }

                            onAccepted: {
                                // Reject if there's invalid input
                                if (!isInputValid()) {
                                    reject()
                                    return
                                }

                                var fps = fpsField.text ? fpsField.text : fpsField.placeholderText

                                // Find and update the custom entry
                                for (var i = 0; i < fpsListModel.count; i++) {
                                    if (fpsListModel.get(i).is_custom) {
                                        fpsListModel.setProperty(i, "video_fps", fps)
                                        fpsListModel.setProperty(i, "text", qsTr("Custom (%1 FPS)").arg(fps))

                                        // Now update the bitrate using the custom resolution
                                        fpsComboBox.currentIndex = i
                                        fpsComboBox.updateBitrateForSelection()

                                        // Update the combobox width too
                                        fpsComboBox.recalculateWidth()
                                        break
                                    }
                                }
                            }

                            ColumnLayout {
                                Label {
                                    text: qsTr("Enter a custom frame rate:")
                                    font.bold: true
                                }

                                RowLayout {
                                    TextField {
                                        id: fpsField
                                        maximumLength: 4
                                        inputMethodHints: Qt.ImhDigitsOnly
                                        placeholderText: fpsListModel.get(fpsComboBox.currentIndex).video_fps
                                        validator: IntValidator{bottom:10; top:9999}
                                        focus: true

                                        onTextChanged: {
                                            // standardButton() was added in Qt 5.10, so we must check for it first
                                            if (customFpsDialog.standardButton) {
                                                customFpsDialog.standardButton(Dialog.Ok).enabled = customFpsDialog.isInputValid()
                                            }
                                        }

                                        Keys.onReturnPressed: {
                                            customFpsDialog.accept()
                                        }

                                        Keys.onEnterPressed: {
                                            customFpsDialog.accept()
                                        }
                                    }
                                }
                            }
                        }

                        function addRefreshRateOrdered(fpsListModel, refreshRate, description, custom) {
                            var indexToAdd = 0
                            for (var j = 0; j < fpsListModel.count; j++) {
                                var existing_fps = parseInt(fpsListModel.get(j).video_fps);

                                if (refreshRate === existing_fps || (custom && fpsListModel.get(j).is_custom)) {
                                    // Duplicate entry, skip
                                    indexToAdd = -1
                                    break
                                }
                                else if (refreshRate > existing_fps) {
                                    // Candidate entrypoint after this entry
                                    indexToAdd = j + 1
                                }
                            }

                            // Insert this frame rate if it's not a duplicate
                            if (indexToAdd >= 0) {
                                // Custom values always go at the end of the list
                                if (custom) {
                                    indexToAdd = fpsListModel.count
                                }

                                fpsListModel.insert(indexToAdd,
                                                    {
                                                        "text": description,
                                                        "video_fps": ""+refreshRate,
                                                        "is_custom": custom
                                                    })
                            }

                            return indexToAdd
                        }

                        function reinitialize() {
                            // Add native refresh rate for all attached displays
                            var done = false
                            for (var displayIndex = 0; !done; displayIndex++) {
                                var refreshRate = SystemProperties.getRefreshRate(displayIndex);
                                if (refreshRate === 0) {
                                    // Exceeded max count of displays
                                    done = true
                                    break
                                }

                                addRefreshRateOrdered(fpsListModel, refreshRate, qsTr("%1 FPS").arg(refreshRate), false)
                            }

                            var saved_fps = StreamingPreferences.fps
                            var found = false
                            for (var i = 0; i < model.count; i++) {
                                var el_fps = parseInt(model.get(i).video_fps);

                                // Look for a matching frame rate
                                if (saved_fps === el_fps) {
                                    currentIndex = i
                                    found = true
                                    break
                                }
                            }

                            // If we didn't find one, add a custom frame rate for the current value
                            if (!found) {
                                currentIndex = addRefreshRateOrdered(model, saved_fps, qsTr("Custom (%1 FPS)").arg(saved_fps), true)
                            }
                            else {
                                addRefreshRateOrdered(model, "", qsTr("Custom"), true)
                            }

                            recalculateWidth()

                            lastIndexValue = currentIndex
                        }

                        // ignore setting the index at first, and actually set it when the component is loaded
                        Component.onCompleted: {
                            reinitialize()
                            languageChanged.connect(reinitialize)
                        }

                        model: ListModel {
                            id: fpsListModel
                            // Other elements may be added at runtime
                            ListElement {
                                text: qsTr("30 FPS")
                                video_fps: "30"
                                is_custom: false
                            }
                            ListElement {
                                text: qsTr("60 FPS")
                                video_fps: "60"
                                is_custom: false
                            }
                        }

                        id: fpsComboBox
                        maximumWidth: parent.width / 2
                        textRole: "text"
                        // ::onActivated must be used, as it only listens for when the index is changed by a human
                        onActivated : {
                            if (model.get(currentIndex).is_custom) {
                                customFpsDialog.open()
                            }
                            else {
                                updateBitrateForSelection()
                            }
                        }
                    }
                }

                Label {
                    width: parent.width
                    id: bitrateTitle
                    text: qsTr("Video bitrate:")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                Label {
                    width: parent.width
                    id: bitrateDesc
                    text: qsTr("Lower the bitrate on slower connections. Raise the bitrate to increase image quality.")
                    font.pointSize: 9
                    wrapMode: Text.Wrap
                }

                Row {
                    width: parent.width
                    spacing: 8

                    Slider {
                        id: slider

                        value: StreamingPreferences.bitrateKbps

                        stepSize: 500
                        from : 500
                        to: StreamingPreferences.unlockBitrate ? 500000 : 150000

                        snapMode: "SnapOnRelease"
                        width: Math.min(bitrateDesc.implicitWidth, parent.width - bitrateField.width - bitrateUnit.width - 2 * parent.spacing)
                        anchors.verticalCenter: parent.verticalCenter

                        onValueChanged: {
                            bitrateTitle.text = qsTr("Video bitrate: %1 Mbps").arg(value / 1000.0)
                            StreamingPreferences.bitrateKbps = value
                            if (!bitrateField.activeFocus) {
                                bitrateField.text = bitrateField.format(value)
                            }
                        }

                        onMoved: {
                            StreamingPreferences.autoAdjustBitrate = false
                        }

                        Component.onCompleted: {
                            // Refresh the text after translations change
                            languageChanged.connect(valueChanged)
                        }
                    }

                    // Hermit: exact value in Mbps (the slider is hard to stop on a precise value)
                    TextField {
                        id: bitrateField
                        width: 72
                        anchors.verticalCenter: parent.verticalCenter
                        horizontalAlignment: TextInput.AlignRight
                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                        selectByMouse: true
                        validator: DoubleValidator {
                            bottom: 0.5
                            top: slider.to / 1000
                            decimals: 1
                            notation: DoubleValidator.StandardNotation
                        }
                        text: format(StreamingPreferences.bitrateKbps)

                        function format(kbps) {
                            return (Math.round(kbps / 100) / 10).toString()
                        }

                        function apply() {
                            var mbps = parseFloat(text.replace(",", "."))
                            if (isNaN(mbps)) {
                                text = format(StreamingPreferences.bitrateKbps)
                                return
                            }
                            var kbps = Math.round(Math.min(Math.max(mbps, 0.5), slider.to / 1000) * 10) * 100
                            StreamingPreferences.autoAdjustBitrate = false
                            slider.value = kbps
                            text = format(kbps)
                        }

                        onEditingFinished: apply()
                        Keys.onReturnPressed: { apply(); focus = false }
                        Keys.onEnterPressed: { apply(); focus = false }

                        ToolTip.delay: 1000
                        ToolTip.timeout: 5000
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Type the bitrate in Mbps")
                    }

                    Label {
                        id: bitrateUnit
                        text: "Mbps"
                        anchors.verticalCenter: parent.verticalCenter
                        font.pointSize: 10
                    }
                }

                // Hermit: automatic bitrate during the stream (Shell hosts)
                CheckBox {
                    width: parent.width
                    text: qsTr("Adjust the bitrate automatically to the network (Shell host)")
                    font.pointSize: 12
                    checked: StreamingPreferences.adaptiveBitrate
                    onCheckedChanged: StreamingPreferences.adaptiveBitrate = checked
                    ToolTip.delay: 1000
                    ToolTip.timeout: 15000
                    ToolTip.visible: hovered || activeFocus
                    ToolTip.text: qsTr("While streaming, lowers the bitrate when frames are lost or the round trip rises, and raises it again up to the bitrate above when the network is calm, without reconnecting.")
                }

                // Hermit: recommendation from the formula in StreamingPreferences::getRecommendedBitrate()
                Label {
                    width: parent.width
                    font.pointSize: 9
                    wrapMode: Text.Wrap
                    color: HermitTheme.textHelper
                    property int h264: StreamingPreferences.getRecommendedBitrate(StreamingPreferences.width, StreamingPreferences.height, StreamingPreferences.fps, StreamingPreferences.enableYUV444, false) / 1000
                    property int efficient: StreamingPreferences.getRecommendedBitrate(StreamingPreferences.width, StreamingPreferences.height, StreamingPreferences.fps, StreamingPreferences.enableYUV444, true) / 1000
                    text: StreamingPreferences.videoCodecConfig === StreamingPreferences.VCC_FORCE_H264 ?
                              qsTr("Recommended for %1x%2 at %3 FPS: %4 Mbps").arg(StreamingPreferences.width).arg(StreamingPreferences.height).arg(StreamingPreferences.fps).arg(h264) :
                          (StreamingPreferences.videoCodecConfig === StreamingPreferences.VCC_AUTO ?
                              qsTr("Recommended for %1x%2 at %3 FPS: %4 Mbps (HEVC/AV1: %5 Mbps)").arg(StreamingPreferences.width).arg(StreamingPreferences.height).arg(StreamingPreferences.fps).arg(h264).arg(efficient) :
                              qsTr("Recommended for %1x%2 at %3 FPS: %4 Mbps").arg(StreamingPreferences.width).arg(StreamingPreferences.height).arg(StreamingPreferences.fps).arg(efficient))
                }

                Label {
                    width: parent.width
                    id: windowModeTitle
                    text: qsTr("Display mode")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                    visible: SystemProperties.hasDesktopEnvironment
                }

                AutoResizingComboBox {
                    function createModel() {
                        var model = Qt.createQmlObject('import QtQuick 2.0; ListModel {}', parent, '')

                        model.append({
                                         text: qsTr("Fullscreen"),
                                         val: StreamingPreferences.WM_FULLSCREEN
                                     })

                        model.append({
                                         text: qsTr("Borderless windowed"),
                                         val: StreamingPreferences.WM_FULLSCREEN_DESKTOP
                                     })

                        model.append({
                                         text: qsTr("Windowed"),
                                         val: StreamingPreferences.WM_WINDOWED
                                     })


                        // Set the recommended option based on the OS
                        for (var i = 0; i < model.count; i++) {
                            var thisWm = model.get(i).val;
                            if (thisWm === StreamingPreferences.recommendedFullScreenMode) {
                                model.get(i).text += " " + qsTr("(Recommended)")
                                model.move(i, 0, 1)
                                break
                            }
                        }

                        return model
                    }


                    // This is used on initialization and upon retranslation
                    function reinitialize() {
                        if (!visible) {
                            // Do nothing if the control won't even be visible
                            return
                        }

                        model = createModel()
                        currentIndex = 0

                        // Set the current value based on the saved preferences
                        var savedWm = StreamingPreferences.windowMode
                        for (var i = 0; i < model.count; i++) {
                             var thisWm = model.get(i).val;
                             if (savedWm === thisWm) {
                                 currentIndex = i
                                 break
                             }
                        }

                        activated(currentIndex)
                    }

                    Component.onCompleted: {
                        reinitialize()
                        languageChanged.connect(reinitialize)
                    }

                    id: windowModeComboBox
                    visible: SystemProperties.hasDesktopEnvironment
                    enabled: !SystemProperties.rendererAlwaysFullScreen
                    hoverEnabled: true
                    textRole: "text"
                    onActivated: {
                        StreamingPreferences.windowMode = model.get(currentIndex).val
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Fullscreen generally provides the best performance, but borderless windowed may work better with features like macOS Spaces, Alt+Tab, screenshot tools, on-screen overlays, etc.")
                }

                Row {
                    spacing: 5
                    width: parent.width

                    CheckBox {
                        id: vsyncCheck
                        hoverEnabled: true
                        text: qsTr("V-Sync")
                        font.pointSize:  12
                        checked: StreamingPreferences.enableVsync
                        onCheckedChanged: {
                            StreamingPreferences.enableVsync = checked
                        }

                        ToolTip.delay: 1000
                        ToolTip.timeout: 5000
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Disabling V-Sync allows sub-frame rendering latency, but it can display visible tearing")
                    }

                    CheckBox {
                        id: framePacingCheck
                        hoverEnabled: true
                        text: qsTr("Frame pacing")
                        font.pointSize:  12
                        enabled: StreamingPreferences.enableVsync
                        // Hermit: shown off while V-Sync is off (it has no effect then), as in the
                        // stream panel; the setting itself is kept for when V-Sync is back on
                        checked: StreamingPreferences.enableVsync && StreamingPreferences.framePacing
                        onToggled: {
                            StreamingPreferences.framePacing = checked
                        }
                        ToolTip.delay: 1000
                        ToolTip.timeout: 5000
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Frame pacing reduces micro-stutter by delaying frames that come in too early")
                    }
                }

                CheckBox {
                    id: enableHdr
                    width: parent.width
                    text: qsTr("Enable HDR")
                    font.pointSize: 12

                    enabled: SystemProperties.supportsHdr
                    checked: enabled && StreamingPreferences.enableHdr
                    onCheckedChanged: {
                        StreamingPreferences.enableHdr = checked
                    }

                    // Updating StreamingPreferences.videoCodecConfig is handled above

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: enabled ?
                                      qsTr("The stream will be HDR-capable, but some games may require an HDR monitor on your host PC to enable HDR mode.")
                                    :
                                      qsTr("HDR streaming is not supported on this PC.")
                }
            }
        }

        CollapsibleGroupBox {
            id: audioSettingsGroupBox
            settingsKey: "audio"
            expandedByDefault: false
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: qsTr("Audio Settings")
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                Label {
                    width: parent.width
                    id: resAudioTitle
                    text: qsTr("Audio configuration")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        var saved_audio = StreamingPreferences.audioConfig
                        currentIndex = 0
                        for (var i = 0; i < audioListModel.count; i++) {
                            var el_audio = audioListModel.get(i).val;
                            if (saved_audio === el_audio) {
                                currentIndex = i
                                break
                            }
                        }
                        activated(currentIndex)
                    }

                    id: audioComboBox
                    textRole: "text"
                    model: ListModel {
                        id: audioListModel
                        ListElement {
                            text: qsTr("Stereo")
                            val: StreamingPreferences.AC_STEREO
                        }
                        ListElement {
                            text: qsTr("5.1 surround sound")
                            val: StreamingPreferences.AC_51_SURROUND
                        }
                        ListElement {
                            text: qsTr("7.1 surround sound")
                            val: StreamingPreferences.AC_71_SURROUND
                        }
                    }
                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated : {
                        StreamingPreferences.audioConfig = audioListModel.get(currentIndex).val
                    }
                }


                CheckBox {
                    id: audioPcCheck
                    width: parent.width
                    text: qsTr("Mute host PC speakers while streaming")
                    font.pointSize: 12
                    checked: !StreamingPreferences.playAudioOnHost
                    onCheckedChanged: {
                        StreamingPreferences.playAudioOnHost = !checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("You must restart any game currently in progress for this setting to take effect")
                }

                CheckBox {
                    id: muteOnFocusLossCheck
                    width: parent.width
                    text: qsTr("Mute audio stream when Hermit is not the active window")
                    font.pointSize: 12
                    visible: SystemProperties.hasDesktopEnvironment
                    checked: StreamingPreferences.muteOnFocusLoss
                    onCheckedChanged: {
                        StreamingPreferences.muteOnFocusLoss = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Mutes Hermit's audio when you Alt+Tab out of the stream or click on a different window.")
                }
            }
        }

        CollapsibleGroupBox {
            id: hostSettingsGroupBox
            settingsKey: "host"
            expandedByDefault: true
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: qsTr("Host Settings")
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                CheckBox {
                    id: optimizeGameSettingsCheck
                    width: parent.width
                    text: qsTr("Optimize game settings for streaming")
                    font.pointSize:  12
                    checked: StreamingPreferences.gameOptimizations
                    onCheckedChanged: {
                        StreamingPreferences.gameOptimizations = checked
                    }
                }

                CheckBox {
                    id: quitAppAfter
                    width: parent.width
                    text: qsTr("Quit app on host PC after ending stream")
                    font.pointSize: 12
                    checked: StreamingPreferences.quitAppAfter
                    onCheckedChanged: {
                        StreamingPreferences.quitAppAfter = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("This will close the app or game you are streaming when you end your stream. You will lose any unsaved progress!")
                }
            }
        }

        // Hermit: what the client adds around a stream (not host settings)
        CollapsibleGroupBox {
            id: convenienceSettingsGroupBox
            settingsKey: "convenience"
            expandedByDefault: true
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: qsTr("Streaming conveniences")
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                CheckBox {
                    id: clipboardSyncCheck
                    width: parent.width
                    text: qsTr("Sync clipboard with the host")
                    font.pointSize: 12
                    visible: SystemProperties.hasDesktopEnvironment
                    checked: StreamingPreferences.clipboardSync
                    onCheckedChanged: {
                        StreamingPreferences.clipboardSync = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 15000
                    ToolTip.visible: hovered || activeFocus
                    ToolTip.text: qsTr("Text you copy locally is sent to the host while streaming, and what you copy on the host is brought back when you switch away from the stream window. With a Shell host, images and files (up to 256 MB) are synced too; files move when the stream starts and when you switch back to the stream window. Requires the clipboard (and, for files, file transfer) permissions on the host.")
                }

                Row {
                    spacing: 5
                    width: parent.width
                    visible: SystemProperties.hasDesktopEnvironment

                    Label {
                        id: clipboardRateTitle
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Image and file transfer speed limit")
                        font.pointSize: 12
                        enabled: clipboardSyncCheck.checked
                    }

                    AutoResizingComboBox {
                        id: clipboardRateComboBox
                        maximumWidth: parent.width - clipboardRateTitle.width - parent.spacing
                        enabled: clipboardSyncCheck.checked
                        textRole: "text"
                        model: ListModel {
                            id: clipboardRateListModel
                            ListElement {
                                text: "10 Mbps"
                                val: 10
                            }
                            ListElement {
                                text: "20 Mbps"
                                val: 20
                            }
                            ListElement {
                                text: "30 Mbps"
                                val: 30
                            }
                            ListElement {
                                text: "50 Mbps"
                                val: 50
                            }
                            ListElement {
                                text: "100 Mbps"
                                val: 100
                            }
                            ListElement {
                                text: qsTr("Unlimited")
                                val: 0
                            }
                        }

                        // ignore setting the index at first, and actually set it when the component is loaded
                        Component.onCompleted: {
                            var saved_rate = StreamingPreferences.clipboardRateMbps
                            currentIndex = 2
                            for (var i = 0; i < clipboardRateListModel.count; i++) {
                                if (clipboardRateListModel.get(i).val === saved_rate) {
                                    currentIndex = i
                                    break
                                }
                            }
                            recalculateWidth()
                        }

                        // ::onActivated must be used, as it only listens for when the index is changed by a human
                        onActivated: {
                            StreamingPreferences.clipboardRateMbps = clipboardRateListModel.get(currentIndex).val
                        }

                        ToolTip.delay: 1000
                        ToolTip.timeout: 5000
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Caps how fast clipboard images and files move in either direction, so a large transfer does not take bandwidth from the stream. Press Ctrl+Alt+Shift+T while streaming to cancel a transfer.")
                    }
                }

                CheckBox {
                    id: autoReconnectCheck
                    width: parent.width
                    text: qsTr("Reconnect automatically when the connection drops")
                    font.pointSize: 12
                    checked: StreamingPreferences.autoReconnect
                    onCheckedChanged: {
                        StreamingPreferences.autoReconnect = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 15000
                    ToolTip.visible: hovered || activeFocus
                    ToolTip.text: qsTr("If the network drops during a stream, connects to the same app again after 5 seconds, up to 3 times in a row. Not used when you end the stream yourself or the app exits on the host.")
                }

                CheckBox {
                    id: sessionSummaryCheck
                    width: parent.width
                    text: qsTr("Show a summary after each session")
                    font.pointSize: 12
                    checked: StreamingPreferences.showSessionSummary
                    onCheckedChanged: {
                        StreamingPreferences.showSessionSummary = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 15000
                    ToolTip.visible: hovered || activeFocus
                    ToolTip.text: qsTr("After a stream of at least 30 seconds, shows frame rate, network loss and latency compared with your previous sessions. Every session is also added to a history file (CSV) that opens in any spreadsheet app.")
                }

                Button {
                    text: qsTr("Open session history folder")
                    font.pointSize: 11
                    flat: true
                    onClicked: {
                        Qt.openUrlExternally(SessionHistory.folderUrl)
                    }
                }
            }
        }

        CollapsibleGroupBox {
            id: uiSettingsGroupBox
            settingsKey: "ui"
            expandedByDefault: false
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: qsTr("UI Settings")
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                Label {
                    width: parent.width
                    id: languageTitle
                    text: qsTr("Language")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        var saved_language = StreamingPreferences.language
                        currentIndex = 0
                        for (var i = 0; i < languageListModel.count; i++) {
                            var el_language = languageListModel.get(i).val;
                            if (saved_language === el_language) {
                                currentIndex = i
                                break
                            }
                        }

                        activated(currentIndex)
                    }

                    id: languageComboBox
                    textRole: "text"
                    model: ListModel {
                        id: languageListModel
                        ListElement {
                            text: qsTr("Automatic")
                            val: StreamingPreferences.LANG_AUTO
                        }
                        ListElement {
                            text: "Deutsch" // German
                            val: StreamingPreferences.LANG_DE
                        }
                        ListElement {
                            text: "English"
                            val: StreamingPreferences.LANG_EN
                        }
                        ListElement {
                            text: "Français" // French
                            val: StreamingPreferences.LANG_FR
                        }
                        ListElement {
                            text: "简体中文" // Simplified Chinese
                            val: StreamingPreferences.LANG_ZH_CN
                        }
                        ListElement {
                            text: "Norwegian Bokmål"
                            val: StreamingPreferences.LANG_NB_NO
                        }
                        ListElement {
                            text: "русский" // Russian
                            val: StreamingPreferences.LANG_RU
                        }
                        ListElement {
                            text: "Español" // Spanish
                            val: StreamingPreferences.LANG_ES
                        }
                        ListElement {
                            text: "日本語" // Japanese
                            val: StreamingPreferences.LANG_JA
                        }
                        ListElement {
                            text: "Tiếng Việt" // Vietnamese
                            val: StreamingPreferences.LANG_VI
                        }
                        ListElement {
                            text: "ภาษาไทย" // Thai
                            val: StreamingPreferences.LANG_TH
                        }
                        ListElement {
                            text: "한국어" // Korean
                            val: StreamingPreferences.LANG_KO
                        }
                        ListElement {
                            text: "Magyar" // Hungarian
                            val: StreamingPreferences.LANG_HU
                        }
                        ListElement {
                            text: "Nederlands" // Dutch
                            val: StreamingPreferences.LANG_NL
                        }
                        ListElement {
                            text: "Svenska" // Swedish
                            val: StreamingPreferences.LANG_SV
                        }
                        ListElement {
                            text: "Türkçe" // Turkish
                            val: StreamingPreferences.LANG_TR
                        }
                        /* ListElement {
                            text: "Українська" // Ukrainian
                            val: StreamingPreferences.LANG_UK
                        } */
                        ListElement {
                            text: "繁體中文" // Traditional Chinese
                            val: StreamingPreferences.LANG_ZH_TW
                        }
                        ListElement {
                            text: "Português" // Portuguese
                            val: StreamingPreferences.LANG_PT
                        }
                        ListElement {
                            text: "Português do Brasil" // Brazilian Portuguese
                            val: StreamingPreferences.LANG_PT_BR
                        }
                        ListElement {
                            text: "Ελληνικά" // Greek
                            val: StreamingPreferences.LANG_EL
                        }
                        ListElement {
                            text: "Italiano" // Italian
                            val: StreamingPreferences.LANG_IT
                        }
                        /* ListElement {
                            text: "हिन्दी, हिंदी" // Hindi
                            val: StreamingPreferences.LANG_HI
                        } */
                        ListElement {
                            text: "Język polski" // Polish
                            val: StreamingPreferences.LANG_PL
                        }
                        ListElement {
                            text: "Čeština" // Czech
                            val: StreamingPreferences.LANG_CS
                        }
                        /* ListElement {
                            text: "עִבְרִית" // Hebrew
                            val: StreamingPreferences.LANG_HE
                        } */
                        /* ListElement {
                            text: "کرمانجیی خواروو" // Central Kurdish
                            val: StreamingPreferences.LANG_CKB
                        } */
                        /* ListElement {
                            text: "Lietuvių kalba" // Lithuanian
                            val: StreamingPreferences.LANG_LT
                        } */
                        /* ListElement {
                            text: "Eesti" // Estonian
                            val: StreamingPreferences.LANG_ET
                        } */
                        ListElement {
                            text: "Български" // Bulgarian
                            val: StreamingPreferences.LANG_BG
                        }
                        /* ListElement {
                            text: "Esperanto"
                            val: StreamingPreferences.LANG_EO
                        } */
                        ListElement {
                            text: "தமிழ்" // Tamil
                            val: StreamingPreferences.LANG_TA
                        }
                    }
                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated : {
                        // Retranslating is expensive, so only do it if the language actually changed
                        var new_language = languageListModel.get(currentIndex).val
                        if (StreamingPreferences.language !== new_language) {
                            StreamingPreferences.language = languageListModel.get(currentIndex).val
                            if (!StreamingPreferences.retranslate()) {
                                ToolTip.show(qsTr("You must restart Hermit for this change to take effect"), 5000)
                            }
                            else {
                                // Force the back operation to pop any AppView pages that exist.
                                // The AppView stops working after retranslate() for some reason.
                                window.clearOnBack = true

                                // Signal other controls to adjust their text
                                languageChanged()
                            }
                        }
                    }
                }

                Label {
                    width: parent.width
                    id: uiDisplayModeTitle
                    text: qsTr("GUI display mode")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                    visible: SystemProperties.hasDesktopEnvironment
                }

                AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        if (!visible) {
                            // Do nothing if the control won't even be visible
                            return
                        }

                        var saved_uidisplaymode = StreamingPreferences.uiDisplayMode
                        currentIndex = 0
                        for (var i = 0; i < uiDisplayModeListModel.count; i++) {
                            var el_uidisplaymode = uiDisplayModeListModel.get(i).val;
                            if (saved_uidisplaymode === el_uidisplaymode) {
                                currentIndex = i
                                break
                            }
                        }

                        activated(currentIndex)
                    }

                    id: uiDisplayModeComboBox
                    visible: SystemProperties.hasDesktopEnvironment
                    textRole: "text"
                    model: ListModel {
                        id: uiDisplayModeListModel
                        ListElement {
                            text: qsTr("Windowed")
                            val: StreamingPreferences.UI_WINDOWED
                        }
                        ListElement {
                            text: qsTr("Maximized")
                            val: StreamingPreferences.UI_MAXIMIZED
                        }   
                        ListElement {
                            text: qsTr("Fullscreen")
                            val: StreamingPreferences.UI_FULLSCREEN
                        }
                    }
                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated : {
                        StreamingPreferences.uiDisplayMode = uiDisplayModeListModel.get(currentIndex).val
                    }
                }

                CheckBox {
                    id: connectionWarningsCheck
                    width: parent.width
                    text: qsTr("Show connection quality warnings")
                    font.pointSize: 12
                    checked: StreamingPreferences.connectionWarnings
                    onCheckedChanged: {
                        StreamingPreferences.connectionWarnings = checked
                    }
                }

                CheckBox {
                    id: configurationWarningsCheck
                    width: parent.width
                    text: qsTr("Show configuration warnings")
                    font.pointSize: 12
                    checked: StreamingPreferences.configurationWarnings
                    onCheckedChanged: {
                        StreamingPreferences.configurationWarnings = checked
                    }
                }

                CheckBox {
                    id: keepAwakeCheck
                    width: parent.width
                    text: qsTr("Keep the display awake while streaming")
                    font.pointSize: 12
                    checked: StreamingPreferences.keepAwake
                    onCheckedChanged: {
                        StreamingPreferences.keepAwake = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Prevents the screensaver from starting or the display from going to sleep while streaming.")
                }
            }
        }
    }

    Column {
        padding: 10
        rightPadding: 20
        anchors.left: settingsColumn1.right
        id: settingsColumn2
        width: settingsPage.layoutWidth / 2
        spacing: 15

        CollapsibleGroupBox {
            id: inputSettingsGroupBox
            settingsKey: "input"
            expandedByDefault: false
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: qsTr("Input Settings")
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                CheckBox {
                    id: absoluteMouseCheck
                    hoverEnabled: true
                    width: parent.width
                    text: qsTr("Optimize mouse for remote desktop instead of games")
                    font.pointSize:  12
                    checked: StreamingPreferences.absoluteMouseMode
                    onCheckedChanged: {
                        StreamingPreferences.absoluteMouseMode = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 10000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("This enables seamless mouse control without capturing the client's mouse cursor. It is ideal for remote desktop usage but will not work in most games.") + " " +
                                  qsTr("You can toggle this while streaming using Ctrl+Alt+Shift+M.")
                }

                Row {
                    spacing: 5
                    width: parent.width

                    CheckBox {
                        id: captureSysKeysCheck
                        hoverEnabled: true
                        text: qsTr("Capture system keyboard shortcuts")
                        font.pointSize: 12
                        enabled: SystemProperties.hasDesktopEnvironment
                        checked: StreamingPreferences.captureSysKeysMode !== StreamingPreferences.CSK_OFF || !SystemProperties.hasDesktopEnvironment

                        ToolTip.delay: 1000
                        ToolTip.timeout: 10000
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("This enables the capture of system-wide keyboard shortcuts like Alt+Tab that would normally be handled by the client OS while streaming.") + "\n\n" +
                                      qsTr("NOTE: Certain keyboard shortcuts like Ctrl+Alt+Del on Windows cannot be intercepted by any application, including Hermit.")
                    }

                    AutoResizingComboBox {
                        // ignore setting the index at first, and actually set it when the component is loaded
                        Component.onCompleted: {
                            if (!visible) {
                                // Do nothing if the control won't even be visible
                                return
                            }

                            var saved_syskeysmode = StreamingPreferences.captureSysKeysMode
                            currentIndex = 0
                            for (var i = 0; i < captureSysKeysModeListModel.count; i++) {
                                var el_syskeysmode = captureSysKeysModeListModel.get(i).val;
                                if (saved_syskeysmode === el_syskeysmode) {
                                    currentIndex = i
                                    break
                                }
                            }

                            activated(currentIndex)
                        }

                        enabled: captureSysKeysCheck.checked && captureSysKeysCheck.enabled
                        textRole: "text"
                        model: ListModel {
                            id: captureSysKeysModeListModel
                            ListElement {
                                text: qsTr("in fullscreen")
                                val: StreamingPreferences.CSK_FULLSCREEN
                            }
                            ListElement {
                                text: qsTr("always")
                                val: StreamingPreferences.CSK_ALWAYS
                            }
                        }

                        function updatePref() {
                            if (!enabled) {
                                StreamingPreferences.captureSysKeysMode = StreamingPreferences.CSK_OFF
                            }
                            else {
                                StreamingPreferences.captureSysKeysMode = captureSysKeysModeListModel.get(currentIndex).val
                            }
                        }

                        // ::onActivated must be used, as it only listens for when the index is changed by a human
                        onActivated: {
                            updatePref()
                        }

                        // This handles transition of the checkbox state
                        onEnabledChanged: {
                            updatePref()
                        }
                    }
                }

                CheckBox {
                    id: absoluteTouchCheck
                    hoverEnabled: true
                    width: parent.width
                    text: qsTr("Use touchscreen as a virtual trackpad")
                    font.pointSize:  12
                    checked: !StreamingPreferences.absoluteTouchMode
                    onCheckedChanged: {
                        StreamingPreferences.absoluteTouchMode = !checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("When checked, the touchscreen acts like a trackpad. When unchecked, the touchscreen will directly control the mouse pointer.")
                }

                CheckBox {
                    id: swapMouseButtonsCheck
                    hoverEnabled: true
                    width: parent.width
                    text: qsTr("Swap left and right mouse buttons")
                    font.pointSize:  12
                    checked: StreamingPreferences.swapMouseButtons
                    onCheckedChanged: {
                        StreamingPreferences.swapMouseButtons = checked
                    }
                }

                CheckBox {
                    id: reverseScrollButtonsCheck
                    hoverEnabled: true
                    width: parent.width
                    text: qsTr("Reverse mouse scrolling direction")
                    font.pointSize: 12
                    checked: StreamingPreferences.reverseScrollDirection
                    onCheckedChanged: {
                        StreamingPreferences.reverseScrollDirection = checked
                    }
                }
            }
        }

        CollapsibleGroupBox {
            id: gamepadSettingsGroupBox
            settingsKey: "gamepad"
            expandedByDefault: false
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: qsTr("Gamepad Settings")
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                CheckBox {
                    id: swapFaceButtonsCheck
                    width: parent.width
                    text: qsTr("Swap A/B and X/Y gamepad buttons")
                    font.pointSize: 12
                    checked: StreamingPreferences.swapFaceButtons
                    onCheckedChanged: {
                        StreamingPreferences.swapFaceButtons = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("This switches gamepads into a Nintendo-style button layout")
                }

                CheckBox {
                    id: singleControllerCheck
                    width: parent.width
                    text: qsTr("Force gamepad #1 always connected")
                    font.pointSize:  12
                    checked: !StreamingPreferences.multiController
                    onCheckedChanged: {
                        StreamingPreferences.multiController = !checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Forces a single gamepad to always stay connected to the host, even if no gamepads are actually connected to this PC.") + " " +
                                  qsTr("Only enable this option when streaming a game that doesn't support gamepads being connected after startup.")
                }

                CheckBox {
                    id: gamepadMouseCheck
                    hoverEnabled: true
                    width: parent.width
                    text: qsTr("Enable mouse control with gamepads by holding the 'Start' button")
                    font.pointSize: 12
                    checked: StreamingPreferences.gamepadMouse
                    onCheckedChanged: {
                        StreamingPreferences.gamepadMouse = checked
                    }
                }

                CheckBox {
                    id: backgroundGamepadCheck
                    width: parent.width
                    text: qsTr("Process gamepad input when Hermit is in the background")
                    font.pointSize: 12
                    visible: SystemProperties.hasDesktopEnvironment
                    checked: StreamingPreferences.backgroundGamepad
                    onCheckedChanged: {
                        StreamingPreferences.backgroundGamepad = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Allows Hermit to capture gamepad inputs even if it's not the current window in focus")
                }
            }
        }

        CollapsibleGroupBox {
            id: advancedSettingsGroupBox
            settingsKey: "advanced"
            expandedByDefault: false
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: qsTr("Advanced Settings")
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                Label {
                    width: parent.width
                    id: resVDSTitle
                    text: qsTr("Video decoder")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        var saved_vds = StreamingPreferences.videoDecoderSelection
                        currentIndex = 0
                        for (var i = 0; i < decoderListModel.count; i++) {
                            var el_vds = decoderListModel.get(i).val;
                            if (saved_vds === el_vds) {
                                currentIndex = i
                                break
                            }
                        }
                        activated(currentIndex)
                    }

                    id: decoderComboBox
                    textRole: "text"
                    model: ListModel {
                        id: decoderListModel
                        ListElement {
                            text: qsTr("Automatic (Recommended)")
                            val: StreamingPreferences.VDS_AUTO
                        }
                        ListElement {
                            text: qsTr("Force software decoding")
                            val: StreamingPreferences.VDS_FORCE_SOFTWARE
                        }
                        ListElement {
                            text: qsTr("Force hardware decoding")
                            val: StreamingPreferences.VDS_FORCE_HARDWARE
                        }
                    }
                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated: {
                        if (enabled) {
                            StreamingPreferences.videoDecoderSelection = decoderListModel.get(currentIndex).val
                        }
                    }
                }

                Label {
                    width: parent.width
                    id: resVCCTitle
                    text: qsTr("Video codec")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        var saved_vcc = StreamingPreferences.videoCodecConfig

                        // Default to Automatic (relevant if HDR is enabled,
                        // where we will match none of the codecs in the list)
                        currentIndex = 0

                        for(var i = 0; i < codecListModel.count; i++) {
                            var el_vcc = codecListModel.get(i).val;
                            if (saved_vcc === el_vcc) {
                                currentIndex = i
                                break
                            }
                        }

                        activated(currentIndex)
                    }

                    id: codecComboBox
                    textRole: "text"
                    model: ListModel {
                        id: codecListModel
                        ListElement {
                            text: qsTr("Automatic (Recommended)")
                            val: StreamingPreferences.VCC_AUTO
                        }
                        ListElement {
                            text: qsTr("H.264")
                            val: StreamingPreferences.VCC_FORCE_H264
                        }
                        ListElement {
                            text: qsTr("HEVC (H.265)")
                            val: StreamingPreferences.VCC_FORCE_HEVC
                        }
                        ListElement {
                            text: qsTr("AV1")
                            val: StreamingPreferences.VCC_FORCE_AV1
                        }
                    }
                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated : {
                        if (enabled) {
                            StreamingPreferences.videoCodecConfig = codecListModel.get(currentIndex).val
                        }
                    }
                }

                Label {
                    width: parent.width
                    id: rendererTitle
                    text: qsTr("Renderer")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                    visible: SystemProperties.isDarwin
                }

                AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        var saved_rs = StreamingPreferences.rendererSelection

                        // Default to Automatic
                        currentIndex = 0

                        for(var i = 0; i < rendererListModel.count; i++) {
                            var el_rs = rendererListModel.get(i).val;
                            if (saved_rs === el_rs) {
                                currentIndex = i
                                break
                            }
                        }

                        activated(currentIndex)
                    }

                    id: rendererComboBox
                    visible: SystemProperties.isDarwin
                    textRole: "text"
                    model: ListModel {
                        id: rendererListModel
                        ListElement {
                            text: qsTr("Automatic (Recommended)")
                            val: StreamingPreferences.RS_AUTO
                        }
                        ListElement {
                            text: "Vulkan"
                            val: StreamingPreferences.RS_VULKAN
                        }
                        ListElement {
                            text: "Metal"
                            val: StreamingPreferences.RS_METAL
                        }
                        ListElement {
                            text: "AVSampleBufferDisplayLayer"
                            val: StreamingPreferences.RS_AVSBDL
                        }
                    }
                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated : {
                        StreamingPreferences.rendererSelection = rendererListModel.get(currentIndex).val
                    }
                }

                CheckBox {
                    id: enableYUV444
                    width: parent.width
                    text: qsTr("Enable YUV 4:4:4")
                    font.pointSize: 12

                    checked: StreamingPreferences.enableYUV444
                    onCheckedChanged: {
                        // This is called on init, so only reset to default bitrate when checked state changes.
                        if (StreamingPreferences.enableYUV444 != checked) {
                            StreamingPreferences.enableYUV444 = checked
                            if (StreamingPreferences.autoAdjustBitrate) {
                                StreamingPreferences.bitrateKbps = StreamingPreferences.getDefaultBitrate(StreamingPreferences.width,
                                                                                                          StreamingPreferences.height,
                                                                                                          StreamingPreferences.fps,
                                                                                                          StreamingPreferences.enableYUV444);
                                slider.value = StreamingPreferences.bitrateKbps
                            }
                        }
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: enabled ?
                                      qsTr("Good for streaming desktop and text-heavy games, but not recommended for fast-paced games.")
                                    :
                                      qsTr("YUV 4:4:4 is not supported on this PC.")
                }

                CheckBox {
                    id: unlockBitrate
                    width: parent.width
                    text: qsTr("Unlock bitrate limit (Experimental)")
                    font.pointSize: 12

                    checked: StreamingPreferences.unlockBitrate
                    onCheckedChanged: {
                        StreamingPreferences.unlockBitrate = checked
                        StreamingPreferences.bitrateKbps = Math.min(StreamingPreferences.bitrateKbps, slider.to)
                        slider.value = StreamingPreferences.bitrateKbps
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("This unlocks extremely high video bitrates for use with Shell hosts. It should only be used when streaming over an Ethernet LAN connection.")
                }

                CheckBox {
                    id: enableMdns
                    width: parent.width
                    text: qsTr("Automatically find PCs on the local network (Recommended)")
                    font.pointSize: 12
                    checked: StreamingPreferences.enableMdns
                    onCheckedChanged: {
                        // This is called on init, so only do the work if we've
                        // actually changed the value.
                        if (StreamingPreferences.enableMdns != checked) {
                            StreamingPreferences.enableMdns = checked

                            // Restart polling so the mDNS change takes effect
                            if (window.pollingActive) {
                                ComputerManager.stopPollingAsync()
                                ComputerManager.startPolling()
                            }
                        }
                    }
                }

                CheckBox {
                    id: largeRemotePacketsCheck
                    width: parent.width
                    text: qsTr("Use full-size video packets over the internet (1392 bytes)")
                    font.pointSize: 12
                    checked: StreamingPreferences.largeRemotePackets
                    onCheckedChanged: {
                        StreamingPreferences.largeRemotePackets = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 15000
                    ToolTip.visible: hovered || activeFocus
                    ToolTip.text: qsTr("Video packets are normally limited to 1024 bytes when streaming over the internet. Larger packets mean about 26% fewer packets per frame. Turn this off again if the stream breaks up or fails to start: the network path may not carry full-size packets. VPN connections always keep the 1024 byte limit.")
                }

            }
        }

        // Hermit: the performance overlay (upstream's toggle moved here from Advanced Settings),
        // with a choice of metrics and text size.
        CollapsibleGroupBox {
            id: overlaySettingsGroupBox
            settingsKey: "overlay"
            expandedByDefault: false
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: qsTr("Performance Overlay")
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                CheckBox {
                    id: showPerformanceOverlay
                    width: parent.width
                    text: qsTr("Show performance stats while streaming")
                    font.pointSize: 12
                    checked: StreamingPreferences.showPerformanceOverlay
                    onCheckedChanged: {
                        StreamingPreferences.showPerformanceOverlay = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Display real-time stream performance information while streaming.") + "\n\n" +
                                  qsTr("You can toggle it at any time while streaming using Ctrl+Alt+Shift+S or Select+L1+R1+X.")
                }

                Label {
                    width: parent.width
                    topPadding: 6
                    text: qsTr("Metrics to show")
                    font.pointSize: 11
                    wrapMode: Text.Wrap
                }

                GridLayout {
                    width: parent.width
                    columns: width > 420 ? 2 : 1
                    columnSpacing: 12
                    rowSpacing: 0

                    Repeater {
                        // Bits of StatsOverlay::Metric (app/streaming/video/statsoverlay.h).
                        model: [
                            { bit: 1 << 0, text: qsTr("Video (resolution, FPS, codec)") },
                            { bit: 1 << 2, text: qsTr("Bitrate") },
                            { bit: 1 << 3, text: qsTr("Network loss") },
                            { bit: 1 << 5, text: qsTr("Round trip time") },
                            { bit: 1 << 6, text: qsTr("Host latency") },
                            { bit: 1 << 10, text: qsTr("Estimated total latency") },
                            { bit: 1 << 1, text: qsTr("Received / decoded / rendered FPS") },
                            { bit: 1 << 4, text: qsTr("Jitter drops") },
                            { bit: 1 << 7, text: qsTr("Decode time") },
                            { bit: 1 << 8, text: qsTr("Queue delay") },
                            { bit: 1 << 9, text: qsTr("Render time") },
                            { bit: 1 << 11, text: qsTr("Decoder") }
                        ]
                        delegate: CheckBox {
                            Layout.fillWidth: true
                            text: modelData.text
                            font.pointSize: 11
                            checked: (StreamingPreferences.overlayMetrics & modelData.bit) !== 0
                            onToggled: {
                                StreamingPreferences.overlayMetrics = checked ?
                                            (StreamingPreferences.overlayMetrics | modelData.bit) :
                                            (StreamingPreferences.overlayMetrics & ~modelData.bit)
                            }
                        }
                    }
                }

                Row {
                    spacing: 10
                    topPadding: 4

                    Label {
                        text: qsTr("Text size")
                        font.pointSize: 11
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    AutoResizingComboBox {
                        id: overlayTextSizeComboBox
                        textRole: "text"
                        Component.onCompleted: recalculateWidth()
                        model: [
                            { text: qsTr("Small"), size: 16 },
                            { text: qsTr("Medium"), size: 20 },
                            { text: qsTr("Large"), size: 26 }
                        ]
                        currentIndex: {
                            var size = StreamingPreferences.overlayTextSize
                            return size <= 16 ? 0 : (size >= 26 ? 2 : 1)
                        }
                        onActivated: (index) => {
                            StreamingPreferences.overlayTextSize = model[index].size
                        }
                    }

                    Button {
                        text: qsTr("Restore defaults")
                        flat: true
                        font.pointSize: 11
                        anchors.verticalCenter: parent.verticalCenter
                        onClicked: {
                            // StatsOverlay::kDefaultMetrics: video, bitrate, network loss, round trip,
                            // host latency and estimated total latency.
                            StreamingPreferences.overlayMetrics = (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 10)
                            StreamingPreferences.overlayTextSize = 20
                        }
                    }
                }
            }
        }
    }
}
