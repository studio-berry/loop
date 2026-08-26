import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Loupe.Canvas

Item {
    id: root

    property var host: editorHost

    signal viewportGeometryChanged()

    LoupeCanvas {
        id: canvas
        anchors.fill: parent
        focus: true
        highContrast: root.host ? root.host.highContrast : false

        Accessible.name: qsTr("Document canvas")
        Accessible.description: canvas.accessibleDocumentSummary

        Component.onCompleted: {
            if (root.host) {
                root.host.attachCanvas(canvas)
            }
        }

        Component.onDestruction: {
            if (root.host) {
                root.host.detachCanvas()
            }
        }
    }
}
