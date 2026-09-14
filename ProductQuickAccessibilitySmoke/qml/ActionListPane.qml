import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Pane {
    id: root

    objectName: "actionListPane"

    property var host: editorHost
    property var stepsModel: host ? host.actionList.stepsModel : null
    readonly property bool preferReducedMotion: host ? host.preferReducedMotion : false
    readonly property bool busy: host && ["validating", "planning", "running"].indexOf(host.actionListStateName) >= 0

    padding: 8

    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Action List")
    Accessible.description: qsTr("Validate, plan, and execute reusable repair recipes against the open document.")

    FileDialog {
        id: importDialog
        title: qsTr("Import Action List recipe")
        nameFilters: [qsTr("JSON recipes (*.json)")]
        fileMode: FileDialog.OpenFile
        onAccepted: if (root.host) root.host.importActionListRecipe(selectedFile)
    }

    FileDialog {
        id: exportDialog
        title: qsTr("Export Action List recipe")
        nameFilters: [qsTr("JSON recipes (*.json)")]
        fileMode: FileDialog.SaveFile
        onAccepted: if (root.host) root.host.exportActionListRecipe(selectedFile)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: host ? (host.actionList.operatorSummary.length > 0
                ? host.actionList.operatorSummary
                : qsTr("Action List status: %1").arg(host.actionListStateName)) : ""
            Accessible.name: qsTr("Action List status")
        }

        RowLayout {
            Layout.fillWidth: true

            ComboBox {
                id: recipeSelector
                objectName: "actionListRecipeSelector"
                Layout.fillWidth: true
                model: root.host ? root.host.actionListRecipes : []
                textRole: "name"
                valueRole: "id"
                enabled: root.host && !root.busy
                currentIndex: {
                    if (!root.host)
                        return -1
                    for (var i = 0; i < model.length; ++i) {
                        if (model[i].id === root.host.selectedActionListRecipeId)
                            return i
                    }
                    return -1
                }
                Accessible.name: qsTr("Action List recipe")
                onActivated: function(index) {
                    if (root.host && model[index])
                        root.host.selectActionListRecipe(model[index].id)
                }
            }

            Button {
                text: qsTr("Import")
                enabled: root.host && !root.busy
                onClicked: importDialog.open()
                Accessible.name: qsTr("Import Action List recipe")
            }

            Button {
                text: qsTr("Export")
                enabled: root.host && root.host.selectedActionListRecipeId.length > 0 && !root.busy
                onClicked: exportDialog.open()
                Accessible.name: qsTr("Export Action List recipe")
            }
        }

        Repeater {
            model: root.host ? root.host.actionListBindings : []
            delegate: RowLayout {
                Layout.fillWidth: true
                required property var modelData

                Label {
                    text: modelData.name
                    Accessible.name: qsTr("Binding %1").arg(modelData.name)
                }
                TextField {
                    Layout.fillWidth: true
                    enabled: !root.busy
                    text: modelData.value === undefined || modelData.value === null ? "" : String(modelData.value)
                    Accessible.name: qsTr("Binding value for %1").arg(modelData.name)
                    onEditingFinished: if (root.host) root.host.setActionListBinding(modelData.name, text)
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true

            Button {
                objectName: "validateActionListButton"
                text: qsTr("Validate")
                enabled: root.host && root.host.hasDocument && !root.busy
                onClicked: root.host.validateActionListRecipe()
                Accessible.name: qsTr("Validate Action List recipe")
            }

            Button {
                objectName: "planActionListButton"
                text: qsTr("Plan")
                enabled: root.host && root.host.hasDocument && !root.busy
                onClicked: root.host.planActionList()
                Accessible.name: qsTr("Plan Action List")
            }

            Button {
                objectName: "confirmActionListButton"
                text: qsTr("Confirm and Run")
                enabled: root.host && host.actionListStateName === "planned" && !root.busy
                onClicked: root.host.confirmActionListPlan()
                Accessible.name: qsTr("Confirm Action List plan and run")
            }

            Button {
                objectName: "discardActionListPlanButton"
                text: qsTr("Discard Plan")
                enabled: root.host && host.actionListStateName === "planned" && !root.busy
                onClicked: root.host.discardActionListPlan()
                Accessible.name: qsTr("Discard Action List plan")
            }

            Button {
                objectName: "cancelActionListButton"
                text: qsTr("Cancel")
                enabled: root.busy
                onClicked: root.host.cancelActionList()
                Accessible.name: qsTr("Cancel Action List")
            }

            ProgressBar {
                objectName: "actionListProgress"
                Layout.fillWidth: true
                from: 0
                to: 100
                value: root.host ? root.host.actionList.progress : 0
                enabled: root.busy
                Accessible.name: qsTr("Action List progress")
            }
        }

        Label {
            Layout.fillWidth: true
            visible: root.host && root.host.actionList.recipeHash.length > 0
            text: qsTr("Recipe hash: %1").arg(root.host.actionList.recipeHash)
            wrapMode: Text.WordWrap
            Accessible.name: qsTr("Action List recipe hash")
        }

        ListView {
            id: stepsView
            objectName: "actionListStepsView"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            focus: visible
            activeFocusOnTab: true
            model: root.stepsModel

            Accessible.role: Accessible.List
            Accessible.name: qsTr("Action List step diagnostics")
            Accessible.description: qsTr("Planned or executed steps with distinct status, diagnostics, and affected scope.")

            delegate: ItemDelegate {
                width: stepsView.width
                text: "%1 — %2 (%3 ms)".arg(model.stepId, model.statusName, model.durationMs)
                Accessible.role: Accessible.ListItem
                Accessible.name: text
                Accessible.description: qsTr("Operation %1. Parameters %2. Diagnostics %3.")
                    .arg(model.operationId).arg(model.resolvedParameters).arg(JSON.stringify(model.diagnostics))

                contentItem: ColumnLayout {
                    spacing: 4
                    width: parent.width

                    Label {
                        Layout.fillWidth: true
                        text: "%1 — %2".arg(model.stepId, model.statusName)
                        font.bold: true
                        color: {
                            switch (model.statusName) {
                            case "pending": return "#6b7280"
                            case "running": return "#2563eb"
                            case "succeeded": return "#15803d"
                            case "skipped": return "#a16207"
                            case "failed": return "#b91c1c"
                            case "cancelled": return "#7c3aed"
                            default: return palette.text
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("Operation: %1").arg(model.operationId)
                    }

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        visible: model.resolvedParameters.length > 0
                        text: qsTr("Parameters: %1").arg(model.resolvedParameters)
                    }

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        visible: model.affectedScope && model.affectedScope.length > 0
                        text: qsTr("Affected scope: %1").arg(JSON.stringify(model.affectedScope))
                    }

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        visible: model.diagnostics && model.diagnostics.length > 0
                        text: qsTr("Diagnostics: %1").arg(JSON.stringify(model.diagnostics))
                    }
                }
            }
        }
    }
}
