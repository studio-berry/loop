import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Pages / Production workspace (#586).
//
// The page inventory and document facts come from the shell's document model; the plate
// list and the production state come from the production model. Every control here
// forwards an existing catalog command - the shell plans, PageMaster writes - so this
// pane adds no second route to PDF truth.
Pane {
    id: root

    objectName: "pagesProductionPane"

    property var host: editorHost
    readonly property var documentModel: host ? host.documentModel : null
    readonly property var pagesModel: root.documentModel ? root.documentModel.pages : null

    padding: 8

    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Pages and Production")
    Accessible.description: qsTr("Page inventory, document facts, plates and the page-geometry and assembly commands.")

    function millimetres(points) {
        return (points * 25.4 / 72).toFixed(1) + " mm"
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        Label {
            objectName: "pagesProductionStateLabel"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.bold: true
            text: root.host && root.host.hasDocument
                ? qsTr("Production state: %1").arg(root.host.productionStateName)
                : qsTr("No document is open.")
            Accessible.name: qsTr("Production state")
        }

        Label {
            objectName: "pagesProductionDocumentFacts"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: root.documentModel !== null && root.host !== null && root.host.hasDocument
            text: {
                if (!root.documentModel)
                    return ""
                var facts = qsTr("%1 page(s). Producer %2. PDF %3.")
                    .arg(root.pagesModel ? root.pagesModel.rowCount() : 0)
                    .arg(root.documentModel.producer || qsTr("unknown"))
                    .arg(root.documentModel.version || qsTr("unknown"))
                if (root.documentModel.hasOptionalContent)
                    facts += " " + qsTr("The document carries optional content.")
                return facts
            }
            Accessible.name: qsTr("Document facts for production")
        }

        GroupBox {
            Layout.fillWidth: true
            title: qsTr("Page geometry and assembly")
            Accessible.name: qsTr("Page geometry and assembly commands")

            Flow {
                anchors.fill: parent
                spacing: 6

                Repeater {
                    model: [
                        { label: qsTr("Page geometry"), command: "actionPageGeometry" },
                        { label: qsTr("Insert page numbers"), command: "actionInsertPageNumbers" },
                        { label: qsTr("First page on the right"), command: "actionFirstPageOnRightSide" },
                        { label: qsTr("Continuous layout"), command: "actionPageLayoutContinuous" },
                        { label: qsTr("Two columns"), command: "actionPageLayoutTwoColumns" },
                        { label: qsTr("Two pages"), command: "actionPageLayoutTwoPages" }
                    ]

                    delegate: Button {
                        required property var modelData
                        text: modelData.label
                        enabled: root.host !== null && root.host.isCommandEnabled(modelData.command)
                        onClicked: if (root.host) root.host.invokeCommand(modelData.command)
                        Accessible.name: modelData.label
                        Accessible.description: qsTr("Runs the shell command %1.").arg(modelData.command)
                    }
                }
            }
        }

        ListView {
            id: pagesView
            objectName: "pagesProductionListView"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            focus: visible
            activeFocusOnTab: true
            model: root.pagesModel

            Accessible.role: Accessible.List
            Accessible.name: qsTr("Pages")
            Accessible.description: qsTr("Page inventory with size and rotation.")

            delegate: ItemDelegate {
                id: pageRow
                width: pagesView.width
                highlighted: root.host !== null && root.host.currentPage === pageRow.pageNumber - 1
                text: qsTr("Page %1 — %2 × %3, rotation %4°")
                    .arg(pageRow.pageNumber)
                    .arg(root.millimetres(pageRow.pageWidth))
                    .arg(root.millimetres(pageRow.pageHeight))
                    .arg(pageRow.pageRotation)
                onClicked: if (root.host) root.host.goToPage(pageRow.pageNumber - 1)

                Accessible.role: Accessible.ListItem
                Accessible.name: pageRow.text
                Accessible.description: qsTr("Activates page %1 on the canvas.").arg(pageRow.pageNumber)
            }
        }
    }
}
