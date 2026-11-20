import QtQuick
import QtQuick.Layouts

// Text about a control rather than part of it -- a caption under a plot, a
// scope note, a reason (the Mac's `meta` role in `Ink.tertiary`): small,
// dim, wrapping to the rail's width.
Text {
    Layout.fillWidth: true
    color: Theme.dim
    font: Theme.forText(Theme.captionFont, text)
    wrapMode: Text.WordWrap
    lineHeight: 1.2
}
