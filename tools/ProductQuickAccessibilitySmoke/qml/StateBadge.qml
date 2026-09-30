import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Canonical state badge (#586).
//
// The kind, the colour role, the shape and the accessible name all come from
// LoopLibQuick through EditorHost. This component decides no state of its own: it
// renders the token-name -> glyph lookup so the states stay readable without colour,
// and it always speaks the accessible name the token layer chose.
RowLayout {
    id: root

    property var visual: null
    property color stateColor: "transparent"
    property string labelText: ""
    property string badgePrefix: "stateBadge"

    readonly property var iconGlyphs: ({
        "FilledCircle": "\u25CF",
        "FilledSquare": "\u25A0",
        "FilledTriangle": "\u25B2",
        "Hatched": "\u25A8",
        "Outline": "\u25CB",
        "Checkmark": "\u2714",
        "BadgeOverlay": "\u25C6",
        "DashedSquare": "\u25A2",
        "HalfFilled": "\u25E7",
        "Cross": "\u2716",
        "Slash": "\u2298",
        "Play": "\u25B6"
    })

    spacing: 6

    Label {
        objectName: root.badgePrefix + "Glyph"
        font.pixelSize: 13
        color: root.stateColor
        text: root.visual ? (root.iconGlyphs[root.visual.icon] || "") : ""
        Accessible.ignored: true
    }

    Label {
        objectName: root.badgePrefix + "Text"
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: root.labelText
        Accessible.name: root.visual && root.visual.accessibleName
            ? root.visual.accessibleName
            : root.labelText
    }
}
