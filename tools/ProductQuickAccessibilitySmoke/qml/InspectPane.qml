import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Inspect workspace (#586): the contextual inspector over typed DTOs.
//
// Every row is rendered from InspectorModel, which the host fills from the selection's
// own data - page, image, finding, separation or a correction operation's plan/result
// DTOs. This pane derives no fact and calls no mutator: the only thing it forwards is
// the "plan a fix for this finding" intent, which the host turns into a recipe
// selection.
Pane {
    id: root

    objectName: "inspectPane"

    property var host: editorHost
    readonly property var inspector: host ? host.inspector : null

    padding: 8

    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Inspect")
    Accessible.description: qsTr("Contextual facts for the current selection, bound to the document revision they describe.")

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        Label {
            objectName: "inspectSelectionTitle"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.bold: true
            text: root.inspector ? root.inspector.title : ""
            Accessible.name: qsTr("Inspected selection")
        }

        Label {
            objectName: "inspectIdentityLabel"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: {
                if (!root.host)
                    return ""
                if (!root.host.hasDocument)
                    return qsTr("No document is open.")
                return qsTr("Selection %1 (%2) in %3 at revision %4.")
                    .arg(root.inspector ? root.inspector.selectionId : "")
                    .arg(root.host.inspectionMode)
                    .arg(root.inspector ? root.inspector.documentKey : "")
                    .arg(root.inspector ? root.inspector.documentRevision.substring(0, 8) : "")
            }
            Accessible.name: qsTr("Selection identity")
        }

        RowLayout {
            Layout.fillWidth: true

            Button {
                objectName: "inspectPlanFixButton"
                Layout.fillWidth: true
                visible: root.inspector && root.inspector.hasCorrectiveOperation
                text: qsTr("Plan a fix for this finding")
                onClicked: {
                    if (root.inspector && root.inspector.correctiveOperationIds.length > 0)
                        root.inspector.requestCorrectiveOperation(root.inspector.correctiveOperationIds[0])
                }
                Accessible.name: qsTr("Plan a fix for the selected finding")
                Accessible.description: qsTr("Routes the selected finding's corrective operation to the Fix workspace.")
            }
        }

        Label {
            objectName: "inspectEmptyState"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: editorHost === null || !root.inspector || root.inspector.rowCount() === 0
            text: root.host && root.host.hasDocument
                ? qsTr("Select a page, image, finding or separation on the canvas, or a planned correction step, to inspect it.")
                : qsTr("Open a document to inspect it.")
            Accessible.name: qsTr("Inspection instructions")
        }

        ListView {
            id: factsView
            objectName: "inspectFactsView"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            focus: visible
            activeFocusOnTab: true
            model: root.inspector

            Accessible.role: Accessible.List
            Accessible.name: qsTr("Inspector facts")
            Accessible.description: qsTr("Typed facts for the selection, grouped by section.")

            section.property: "section"
            section.delegate: Label {
                required property string section
                width: factsView.width
                wrapMode: Text.WordWrap
                font.bold: true
                text: section
                Accessible.role: Accessible.StaticText
                Accessible.name: section
            }

            delegate: ItemDelegate {
                id: factRow
                width: factsView.width

                required property string propertyId
                required property string label
                required property string value
                required property string section

                text: "%1: %2".arg(factRow.label).arg(factRow.value)
                Accessible.role: Accessible.ListItem
                Accessible.name: "%1: %2".arg(factRow.label).arg(factRow.value)
                Accessible.description: qsTr("Fact %1 in section %2.").arg(factRow.propertyId).arg(factRow.section)

                contentItem: RowLayout {
                    spacing: 8
                    width: parent.width

                    Label {
                        Layout.preferredWidth: 170
                        elide: Text.ElideRight
                        text: factRow.label
                        Accessible.ignored: true
                    }

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: factRow.value
                        Accessible.ignored: true
                    }
                }
            }
        }
    }
}
