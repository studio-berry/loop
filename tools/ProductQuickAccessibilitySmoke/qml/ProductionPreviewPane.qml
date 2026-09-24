import QtQuick
import QtQuick.Controls
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
                        : (root.host.pageFidelityIsExact
                            ? qsTr("This page is rendered with exact overprint fidelity.")
                            : qsTr("This page is approximate: %1").arg(root.host.pageFidelityReason))
                    Accessible.name: qsTr("Page render fidelity")
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
                        objectName: "productionPreviewFidelityToggle"
                        text: qsTr("Use exact overprint render")
                        enabled: root.host && root.host.hasDocument
                        onClicked: if (root.host) root.host.toggleCurrentPageFidelity()
                        Accessible.name: qsTr("Switch this page to the exact overprint render")
                        Accessible.description: qsTr("Re-renders the current page only; the document stays open.")
                    }
                }
            }
        }

        GroupBox {
            Layout.fillWidth: true
            Layout.fillHeight: true
            title: qsTr("Plates and separations")
            Accessible.name: qsTr("Plates and separations")

            ListView {
                id: platesView
                objectName: "productionPreviewPlatesView"
                anchors.fill: parent
                clip: true
                focus: visible
                activeFocusOnTab: true
                model: root.production

                Accessible.role: Accessible.List
                Accessible.name: qsTr("Plates")
                Accessible.description: qsTr("Inks and plates for the current page.")

                delegate: ItemDelegate {
                    id: plateRow
                    width: platesView.width
                    text: plateRow.displayName
                    Accessible.role: Accessible.ListItem
                    Accessible.name: plateRow.displayName
                    Accessible.description: qsTr("Kind %1. Spot colour %2. Overprint %3. Prints %4.")
                        .arg(plateRow.kind)
                        .arg(plateRow.spotColorName)
                        .arg(plateRow.overprint ? qsTr("yes") : qsTr("no"))
                        .arg(plateRow.shouldPrint ? qsTr("yes") : qsTr("no"))
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
