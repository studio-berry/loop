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

    // Fidelity and origin stay visible even for exact fast-canvas pixels.
    Pane {
        id: fidelityBanner
        objectName: "renderFidelityBanner"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        padding: 8
        visible: root.host && root.host.hasDocument

        Accessible.role: Accessible.StatusBar
        Accessible.name: qsTr("Render fidelity status")
        Accessible.description: root.host ? root.host.previewFidelitySummary : ""

        RowLayout {
            anchors.fill: parent
            spacing: 8

            Label {
                id: fidelityLabel
                objectName: "renderFidelityMessage"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: root.host ? root.host.previewFidelitySummary : ""
                Accessible.name: qsTr("Render fidelity message")
                Accessible.description: text
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
