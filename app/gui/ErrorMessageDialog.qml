import QtQuick 2.0
import QtQuick.Controls 2.2

import SystemProperties 1.0

// Hermit: a Help button only for errors that come with a help text (such as a host that could
// not be added); it opens the troubleshooting section of the user guide.
NavigableMessageDialog {
    standardButtons: Dialog.Ok | ((helpText && SystemProperties.hasBrowser) ? Dialog.Help : 0)
    helpUrl: "https://github.com/junopark00/hermit/blob/main/docs/guide.md#troubleshooting"
}
