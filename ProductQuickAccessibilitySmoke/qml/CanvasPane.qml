import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Loop.Canvas

Item {
    id: root
    objectName: "canvasPane"

    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Document canvas pane")

    property var host: editorHost
    property alias canvasItem: canvas

    signal viewportGeometryChanged()

    LoopCanvas {
        id: canvas
        objectName: "documentCanvas"
        anchors.fill: parent
        focus: visible
        activeFocusOnTab: true
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

    Label {
        objectName: "inspectionModeIndicator"
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 8
        visible: root.host && root.host.inspectionMode !== "page"
        text: root.host ? qsTr("Inspection: %1").arg(root.host.inspectionMode) : ""
        padding: 6
        Accessible.role: Accessible.StatusBar
        Accessible.name: qsTr("Inspection mode")
        Accessible.description: qsTr("The selected finding's registered evidence mode.")
    }

    // Persistent, non-modal render-fidelity indicator (issue #49). Unlike a
    // toast, this stays up for as long as the current page is approximated so
    // an operator cannot miss overprinted artwork that will drop out on
    // press. Mirrors Main.qml's stateBanner: a Pane + Label status bar, shown
    // only when there is something to say.
    Pane {
        id: fidelityBanner
        objectName: "renderFidelityBanner"
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
                text: {
                    if (!root.host) {
                        return ""
                    }
                    if (root.host.pageFidelityIsAuthoritative && root.host.pageFidelityIsExact) {
                        return qsTr("Accurate render")
                    }
                    const reason = root.host.pageFidelityReason
                    return reason.length > 0
                           ? qsTr("Approximate render: %1").arg(reason)
                           : qsTr("Approximate render")
                }
                Accessible.name: qsTr("Render fidelity message")
            }

            Button {
                id: fidelityToggle
                objectName: "renderFidelityToggle"
                text: root.host && root.host.pageFidelityIsAuthoritative
                      ? qsTr("Return to fast preview")
                      : qsTr("Switch to accurate render")
                onClicked: {
                    if (root.host) {
                        root.host.toggleCurrentPageFidelity()
                    }
                }
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Change render fidelity")
            }
        }
    }
}
