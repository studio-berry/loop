import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Production Preview workspace (#586): the identity-bound before/after preview.
//
// The pane never renders a second PDF path. It presents the state the host already owns -
// the production/preview state, the page's render fidelity, the plate model, and the
// technical/visual preview DTOs of the planned or executed candidate - and it says out
// loud when what is on screen does not describe the open revision.
Pane {
    id: root

    objectName: "productionPreviewPane"

    property var host: editorHost
    readonly property var previewIdentity: host ? host.previewIdentity : null
    readonly property var fixPreview: host ? host.fixPreview : null
    readonly property var production: host ? host.production : null
    readonly property bool stale: root.previewIdentity ? root.previewIdentity.stale : false

    padding: 8

    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Production Preview")
    Accessible.description: qsTr("Identity-bound production preview: render fidelity, plates, and the before/after preview of a planned correction.")

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        Label {
            objectName: "productionPreviewIdentity"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.bold: true
            text: {
                if (!root.host || !root.host.hasDocument)
                    return qsTr("No document is open.")
                return qsTr("Preview of %1 at revision %2.")
                    .arg(root.previewIdentity ? root.previewIdentity.documentKey : "")
                    .arg(root.previewIdentity ? root.previewIdentity.documentRevision.substring(0, 8) : "")
            }
            Accessible.name: qsTr("Preview identity")
        }

        // The stale banner is the whole point of this surface: a preview that does not
        // describe the open revision is never presented as current approval evidence.
        Label {
            objectName: "productionPreviewStaleBanner"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: root.stale
            text: root.stale
                ? qsTr("Not current evidence: %1").arg(root.previewIdentity.staleReason)
                : ""
            Accessible.name: qsTr("Preview is not current evidence")
            Accessible.description: root.stale ? root.previewIdentity.staleReason : ""
        }

        GroupBox {
            Layout.fillWidth: true
            title: qsTr("Render state")
            Accessible.name: qsTr("Production render state")

            ColumnLayout {
                anchors.fill: parent
                spacing: 4

                Label {
                    objectName: "productionPreviewSummary"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: root.host ? root.host.previewSummary : ""
                    Accessible.name: qsTr("Production preview summary")
                }

                Label {
                    objectName: "productionPreviewDetail"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible: root.previewIdentity && root.previewIdentity.previewDetail.length > 0
                    text: root.previewIdentity ? root.previewIdentity.previewDetail : ""
                    Accessible.name: qsTr("Production preview detail")
                }

                Label {
                    objectName: "productionPreviewFidelity"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: !root.host
                        ? ""
                        : qsTr("Fidelity %1, origin %2.")
                            .arg(root.host.previewFidelityStateName)
                            .arg(root.host.previewFidelityOriginName)
                    Accessible.name: qsTr("Page render fidelity and origin")
                    Accessible.description: text
                    activeFocusOnTab: true
                }

                StateBadge {
                    objectName: "productionPreviewFidelityBadge"
                    Layout.fillWidth: true
                    visual: root.host ? root.host.previewFidelityVisual : null
                    stateColor: root.host ? root.host.previewFidelityColor : "transparent"
                    labelText: root.host ? root.host.previewFidelitySummary : ""
                    badgePrefix: "productionPreviewFidelityBadge"
                }

                Label {
                    objectName: "productionPreviewProductionState"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: root.host ? qsTr("Production state: %1").arg(root.host.productionStateName) : ""
                    Accessible.name: qsTr("Production state")
                }

                RowLayout {
                    Layout.fillWidth: true

                    Button {
                        objectName: "productionPreviewProveButton"
                        text: qsTr("Proof this page (authoritative render)")
                        enabled: root.host && root.host.previewRequiresAuthoritative
                        onClicked: if (root.host) root.host.ensureAuthoritativePreview()
                        Accessible.name: qsTr("Switch this page to the authoritative overprint render")
                        Accessible.description: qsTr("Moves the current page to the output-preview render before any print claim. The document stays open.")
                    }

                    Button {
                        objectName: "productionPreviewFidelityToggle"
                        text: root.host && root.host.pageFidelityIsAuthoritative
                              ? qsTr("Return to the fast preview")
                              : qsTr("Use exact overprint render")
                        enabled: root.host && root.host.hasDocument
                        onClicked: if (root.host) root.host.toggleCurrentPageFidelity()
                        Accessible.name: qsTr("Change this page's render fidelity")
                        Accessible.description: qsTr("Re-renders the current page only; the document stays open.")
                    }
                }

                Label {
                    objectName: "productionPreviewSoftProofState"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: !root.host
                        ? ""
                        : qsTr("Soft proof %1: %2")
                            .arg(root.host.softProofStateName)
                            .arg(root.host.softProofSummary)
                    Accessible.name: qsTr("Soft proof state")
                    Accessible.description: text
                }

                Label {
                    objectName: "productionPreviewSoftProofProfile"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible: root.host !== null && root.host.softProofProfileId.length > 0
                    text: root.host
                        ? qsTr("Output-intent profile %1 (identifier %2, type %3).")
                            .arg(root.host.softProofProfileName)
                            .arg(root.host.softProofProfileId)
                            .arg(root.host.softProofProfileTypeName)
                        : ""
                    Accessible.name: qsTr("Soft-proof profile identity")
                    Accessible.description: text
                }

                RowLayout {
                    Layout.fillWidth: true

                    Button {
                        objectName: "productionPreviewSoftProofToggle"
                        text: root.host && root.host.softProofActive
                              ? qsTr("Stop the soft proof")
                              : qsTr("Simulate the output intent (soft proof)")
                        enabled: root.host && root.host.hasDocument
                        onClicked: if (root.host) root.host.toggleSoftProof()
                        Accessible.name: qsTr("Toggle the soft-proof simulation of the output intent")
                        Accessible.description: qsTr("Renders the canvas through Core's colour management under proofing settings. A simulation of print, never recorded as approval.")
                    }
                }
            }
        }

        GroupBox {
            Layout.fillWidth: true
            title: qsTr("Display colour")
            Accessible.name: qsTr("Display colour modes")

            ColumnLayout {
                anchors.fill: parent
                spacing: 4

                ComboBox {
                    objectName: "productionPreviewDisplayColorMode"
                    Layout.fillWidth: true
                    enabled: root.host !== null
                    model: [ "normal", "invert", "grayscale", "high-contrast", "bitonal", "custom" ]
                    currentIndex: {
                        if (!root.host)
                            return 0
                        const index = model.indexOf(root.host.displayColorModeName)
                        return index < 0 ? 0 : index
                    }
                    onActivated: if (root.host) root.host.setDisplayColorMode(model[currentIndex])
                    Accessible.name: qsTr("Display colour mode")
                    Accessible.description: qsTr("Presentation only: filters the canvas pixels and asks for no render.")
                }

                Label {
                    objectName: "productionPreviewDisplayColorState"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: !root.host || !root.host.displayColorModeActive
                        ? qsTr("No display colour mode is active.")
                        : qsTr("Display colour simulation (%1): presentation only, never a production render.")
                            .arg(root.host.displayColorModeName)
                    Accessible.name: qsTr("Display colour state")
                    Accessible.description: text
                }

                RowLayout {
                    Layout.fillWidth: true

                    Button {
                        objectName: "productionPreviewCustomForeground"
                        text: qsTr("Custom foreground")
                        enabled: root.host !== null
                        onClicked: customForegroundDialog.open()
                        Accessible.name: qsTr("Choose the custom mode's foreground colour")
                    }

                    Button {
                        objectName: "productionPreviewCustomBackground"
                        text: qsTr("Custom background")
                        enabled: root.host !== null
                        onClicked: customBackgroundDialog.open()
                        Accessible.name: qsTr("Choose the custom mode's background colour")
                    }
                }
            }
        }

        ColorDialog {
            id: customForegroundDialog
            title: qsTr("Custom mode foreground")
            selectedColor: root.host ? root.host.displayColorCustomForeground : "black"
            onAccepted: if (root.host) root.host.setDisplayColorCustomColors(selectedColor, root.host.displayColorCustomBackground)
        }

        ColorDialog {
            id: customBackgroundDialog
            title: qsTr("Custom mode background")
            selectedColor: root.host ? root.host.displayColorCustomBackground : "white"
            onAccepted: if (root.host) root.host.setDisplayColorCustomColors(root.host.displayColorCustomForeground, selectedColor)
        }

        GroupBox {
            Layout.fillWidth: true
            Layout.fillHeight: true
            title: qsTr("Plates and separations")
            Accessible.name: qsTr("Plates and separations")

            ColumnLayout {
                anchors.fill: parent
                spacing: 4

                Label {
                    objectName: "productionPreviewPlateIsolation"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: root.host ? root.host.plateIsolationSummary : ""
                    Accessible.name: qsTr("Plate isolation state")
                    Accessible.description: text
                }

                ListView {
                    id: platesView
                    objectName: "productionPreviewPlatesView"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    focus: visible
                    activeFocusOnTab: true
                    model: root.host ? root.host.productionPlates : []

                    Accessible.role: Accessible.List
                    Accessible.name: qsTr("Plates")
                    Accessible.description: qsTr("Inks and plates for the current page. Each plate can be shown alone or hidden.")

                    delegate: ItemDelegate {
                        id: plateRow
                        width: platesView.width
                        text: plateRow.modelData ? plateRow.modelData.displayName : ""
                        Accessible.role: Accessible.ListItem
                        Accessible.name: plateRow.modelData ? plateRow.modelData.displayName : ""
                        Accessible.description: qsTr("Kind %1. Spot colour %2. Shown %3. Isolated %4.")
                            .arg(plateRow.modelData ? plateRow.modelData.kind : "")
                            .arg(plateRow.modelData ? plateRow.modelData.spotColorName : "")
                            .arg(plateRow.modelData && plateRow.modelData.visible ? qsTr("yes") : qsTr("no"))
                            .arg(plateRow.modelData && plateRow.modelData.isolated ? qsTr("yes") : qsTr("no"))

                        CheckBox {
                            objectName: "productionPreviewPlateVisibleCheck"
                            anchors.verticalCenter: parent.verticalCenter
                            checked: plateRow.modelData ? plateRow.modelData.visible : false
                            onToggled: if (root.host && plateRow.modelData) root.host.setPlateVisible(plateRow.modelData.id, checked)
                            Accessible.name: qsTr("Show plate %1").arg(plateRow.modelData ? plateRow.modelData.displayName : "")
                        }

                        Button {
                            objectName: "productionPreviewPlateIsolate"
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Isolate")
                            onClicked: if (root.host && plateRow.modelData) root.host.isolatePlate(plateRow.modelData.id)
                            Accessible.name: qsTr("Show only plate %1").arg(plateRow.modelData ? plateRow.modelData.displayName : "")
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true

                    Button {
                        objectName: "productionPreviewShowAllPlates"
                        text: qsTr("Show all plates")
                        enabled: root.host !== null
                        onClicked: if (root.host) root.host.showAllPlates()
                        Accessible.name: qsTr("Show every plate")
                    }

                    Button {
                        objectName: "productionPreviewInkCoverage"
                        text: qsTr("Measure ink at the page centre")
                        enabled: root.host && root.host.hasDocument
                        onClicked: if (root.host) root.host.requestInkCoverageAtPoint(root.host.currentPageCentreX, root.host.currentPageCentreY)
                        Accessible.name: qsTr("Measure ink coverage at the page centre")
                        Accessible.description: qsTr("Renders the current page through Core's separation-accurate path and reports the ink coverage at one point.")
                    }
                }

                Label {
                    objectName: "productionPreviewInkCoverageReadout"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: {
                        if (!root.host || !root.host.inkCoverageReadout || !root.host.inkCoverageReadout.available)
                            return root.host && root.host.inkCoverageReadout && root.host.inkCoverageReadout.reason
                                ? qsTr("Ink coverage is not measured: %1.").arg(root.host.inkCoverageReadout.reason)
                                : qsTr("Ink coverage has not been measured at a point.")
                        return qsTr("Ink coverage at (%1, %2) pt: %3 total across %4 plate(s).")
                            .arg(Number(root.host.inkCoverageReadout.pageX).toFixed(1))
                            .arg(Number(root.host.inkCoverageReadout.pageY).toFixed(1))
                            .arg(Number(root.host.inkCoverageReadout.totalInkCoverage).toFixed(3))
                            .arg(root.host.inkCoverageReadout.channelCount)
                    }
                    Accessible.name: qsTr("Ink coverage readout")
                    Accessible.description: text
                }
            }
        }

        GroupBox {
            Layout.fillWidth: true
            title: qsTr("Before / after preview of the planned correction")
            Accessible.name: qsTr("Correction preview")

            ColumnLayout {
                anchors.fill: parent
                spacing: 4

                Label {
                    objectName: "productionPreviewCorrectionIdentity"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: {
                        if (!root.fixPreview || !root.fixPreview.available)
                            return qsTr("No planned correction has a preview for this document.")
                        return qsTr("Source %1 → candidate %2 under plan %3.")
                            .arg(String(root.fixPreview.sourceSha256).substring(0, 12))
                            .arg(String(root.fixPreview.candidateSha256).substring(0, 12))
                            .arg(String(root.fixPreview.planDigest).substring(0, 12))
                    }
                    Accessible.name: qsTr("Correction preview identity")
                }

                Label {
                    objectName: "productionPreviewCorrectionPlanState"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible: root.fixPreview !== null && root.fixPreview.available
                    text: root.fixPreview && !root.fixPreview.planIsCurrent
                        ? qsTr("This preview belongs to a different document revision and is not current evidence.")
                        : qsTr("Technical preview %1, visual preview %2, %3 page(s) changed.")
                            .arg(root.fixPreview ? root.fixPreview.technicalStatus : "")
                            .arg(root.fixPreview ? root.fixPreview.visualStatus : "")
                            .arg(root.fixPreview ? root.fixPreview.changedPageCount : 0)
                    Accessible.name: qsTr("Correction preview state")
                }

                Label {
                    objectName: "productionPreviewCorrectionIncomplete"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible: root.fixPreview !== null && root.fixPreview.available && root.fixPreview.incomplete
                    text: qsTr("The preview did not complete for every step. It cannot stand as approval evidence.")
                    Accessible.name: qsTr("Correction preview is incomplete")
                }

                Button {
                    objectName: "productionPreviewOpenFixButton"
                    text: qsTr("Open the Fix workspace")
                    enabled: root.host !== null
                    onClicked: if (root.host) root.host.setWorkspace(EditorHost.Fix)
                    Accessible.name: qsTr("Open the Fix workspace")
                }
            }
        }
    }
}
