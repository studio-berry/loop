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

    // Persistent, non-modal render-fidelity indicator (issue #49). Unlike a
    // toast, this stays up for as long as the current page is approximated so
    // an operator cannot miss overprinted artwork that will drop out on
    // press. Mirrors Main.qml's stateBanner: a Pane + Label status bar, shown
    // only when there is something to say.
    Pane {
        id: fidelityBanner
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        padding: 8
        visible: root.host && root.host.hasDocument
                 && (!root.host.pageFidelityIsExact || root.host.pageFidelityIsAuthoritative)

        Accessible.role: Accessible.StatusBar
        Accessible.name: qsTr("Render fidelity status")

        RowLayout {
            anchors.fill: parent
            spacing: 8

            Label {
                id: fidelityLabel
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: root.host
                      ? qsTr("Approximate render: %1").arg(root.host.pageFidelityReason)
                      : ""
            }

            Button {
                id: fidelityToggle
                text: root.host && root.host.pageFidelityIsAuthoritative
                      ? qsTr("Return to fast preview")
                      : qsTr("Switch to accurate render")
                onClicked: {
                    if (root.host) {
                        root.host.toggleCurrentPageFidelity()
                    }
                }
            }
        }
    }
}
