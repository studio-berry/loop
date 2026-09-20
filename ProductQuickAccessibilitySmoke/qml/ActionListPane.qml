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

    // Governed-correction lifecycle (#586). The state, its shape, its colour and its
    // accessible name come from LoopLibQuick through EditorHost; this pane renders them and
    // routes the operator's own review decision back through the host.
    readonly property string lifecycleState: host ? host.fixLifecycleStateName : "idle"
    readonly property var planIdentity: host ? host.fixPlanIdentity : null
    readonly property var preview: host ? host.fixPreview : null
    readonly property var recheck: host ? host.fixRecheck : null
    readonly property var signOff: host ? host.fixSignOff : null
    readonly property string rollbackId: pendingRollbackId

    property string pendingRollbackId: ""

    padding: 8

    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Fix")
    Accessible.description: qsTr("Plan, review, approve and execute a governed correction, then read its recheck, sign-off and rollback evidence.")

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

        StateBadge {
            objectName: "fixLifecycleBadge"
            Layout.fillWidth: true
            visual: root.host ? root.host.fixLifecycleVisual : null
            stateColor: root.host ? root.host.fixLifecycleColor : "transparent"
            labelText: root.host ? root.host.fixLifecycleSummary : ""
            badgePrefix: "fixLifecycle"
        }

        Label {
            objectName: "fixPlanIdentityLabel"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: root.planIdentity !== null && root.planIdentity.planDigest.length > 0
            text: root.planIdentity === null ? "" : qsTr("Plan %1 for revision %2 · source %3 · recipe %4 · review %5 · plan matches this revision: %6")
                .arg(String(root.planIdentity.planDigest).substring(0, 12))
                .arg(String(root.planIdentity.plannedRevision).substring(0, 8))
                .arg(String(root.planIdentity.sourceSha256).substring(0, 12))
                .arg(root.planIdentity.recipeId)
                .arg(root.planIdentity.reviewDecision)
                .arg(root.planIdentity.planIsCurrent ? qsTr("yes") : qsTr("no"))
            Accessible.name: qsTr("Correction plan identity")
        }

        RowLayout {
            Layout.fillWidth: true

            Button {
                objectName: "fixApprovePlanButton"
                text: qsTr("Approve plan")
                enabled: root.host !== null && root.lifecycleState === "preview-ready"
                onClicked: if (root.host) root.host.approveActionListPlan()
                Accessible.name: qsTr("Approve the planned correction")
                Accessible.description: qsTr("Binds the approval to this plan digest and revision.")
            }

            Button {
                objectName: "fixRejectPlanButton"
                text: qsTr("Reject plan")
                enabled: root.host !== null && ["preview-ready", "approved"].indexOf(root.lifecycleState) >= 0
                onClicked: if (root.host) root.host.rejectActionListPlan()
                Accessible.name: qsTr("Reject the planned correction")
            }

            Button {
                objectName: "fixExecutePlanButton"
                text: qsTr("Execute approved plan")
                enabled: root.host !== null && root.host.fixExecutionArmed
                onClicked: if (root.host) root.host.executeApprovedActionListPlan()
                Accessible.name: qsTr("Execute the approved correction plan")
            }

            Button {
                objectName: "fixReplanButton"
                text: qsTr("Replan")
                enabled: root.host !== null && root.lifecycleState !== "idle"
                onClicked: if (root.host) root.host.replanActionList()
                Accessible.name: qsTr("Discard the plan and start again")
            }

            Button {
                objectName: "fixOpenPreviewButton"
                text: qsTr("Production preview")
                enabled: root.host !== null
                onClicked: if (root.host) root.host.setWorkspace(EditorHost.ProductionPreview)
                Accessible.name: qsTr("Open the Production Preview workspace")
            }
        }

        GroupBox {
            objectName: "fixPreviewGroup"
            Layout.fillWidth: true
            title: qsTr("Preview")
            Accessible.name: qsTr("Correction preview")

            ColumnLayout {
                anchors.fill: parent
                spacing: 3

                Label {
                    objectName: "fixPreviewSummary"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: {
                        if (!root.preview || !root.preview.available)
                            return qsTr("No preview yet. Plan the recipe to produce one.")
                        return qsTr("Technical %1 · visual %2 · %3 page(s) changed · source %4 → candidate %5")
                            .arg(root.preview.technicalStatus)
                            .arg(root.preview.visualStatus)
                            .arg(root.preview.changedPageCount)
                            .arg(String(root.preview.sourceSha256).substring(0, 12))
                            .arg(String(root.preview.candidateSha256).substring(0, 12))
                    }
                    Accessible.name: qsTr("Preview summary")
                }

                Label {
                    objectName: "fixPreviewIncomplete"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible: root.preview !== null && root.preview.available && root.preview.incomplete
                    text: qsTr("The preview is incomplete. It is not approval evidence.")
                    Accessible.name: qsTr("Preview incomplete")
                }
            }
        }

        GroupBox {
            objectName: "fixRecheckGroup"
            Layout.fillWidth: true
            title: qsTr("Recheck")
            Accessible.name: qsTr("Correction recheck")

            ColumnLayout {
                anchors.fill: parent
                spacing: 3

                Label {
                    objectName: "fixRecheckSummary"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: {
                        if (!root.recheck || !root.recheck.available)
                            return qsTr("No recheck has run for this revision.")
                        return qsTr("Run status %1 · postflight %2 · inspection complete: %3 · cleared %4 · remaining %5 · introduced %6 · not fully rechecked %7")
                            .arg(root.recheck.status)
                            .arg(root.recheck.pass ? qsTr("passed") : qsTr("not passed"))
                            .arg(root.recheck.verdictState === "" ? qsTr("not reduced") : root.recheck.verdictState)
                            .arg(root.recheck.resolved.length)
                            .arg(root.recheck.unchanged.length)
                            .arg(root.recheck.introduced.length)
                            .arg(root.recheck.incomplete.length)
                    }
                    Accessible.name: qsTr("Recheck summary")
                }

                Label {
                    objectName: "fixRecheckIntroduced"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    visible: root.recheck !== null && root.recheck.hasIntroducedFindings
                    text: qsTr("The fix introduced findings. Publication was refused.")
                    Accessible.name: qsTr("Introduced findings")
                }
            }
        }

        GroupBox {
            objectName: "fixSignOffGroup"
            Layout.fillWidth: true
            title: qsTr("Sign off")
            Accessible.name: qsTr("Publication sign-off")

            ColumnLayout {
                anchors.fill: parent
                spacing: 3

                Label {
                    objectName: "fixSignOffSummary"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: {
                        if (!root.signOff)
                            return ""
                        return qsTr("Publication sign-off: %1 · plan %2 · published %3 · certified preflight: %4")
                            .arg(root.signOff.status)
                            .arg(String(root.signOff.planDigest).substring(0, 12))
                            .arg(String(root.signOff.publishedSha256).substring(0, 12))
                            .arg(root.signOff.certificateStateName)
                    }
                    Accessible.name: qsTr("Publication sign-off summary")
                    Accessible.description: root.signOff ? root.signOff.certificateSummary : ""
                }
            }
        }

        GroupBox {
            objectName: "fixRollbackGroup"
            Layout.fillWidth: true
            title: qsTr("Recorded revisions")
            Accessible.name: qsTr("Rollback to a recorded revision")

            ColumnLayout {
                anchors.fill: parent
                spacing: 3

                Label {
                    objectName: "fixRollbackSummary"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: root.host ? root.host.fixRollbackSummary : ""
                    Accessible.name: qsTr("Rollback availability")
                }

                Repeater {
                    model: root.host ? root.host.fixRollbackPoints : []

                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true

                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("Revision %1 · %2 · %3 bytes")
                                .arg(String(modelData.documentRevisionDigest).substring(0, 12))
                                .arg(modelData.operationId)
                                .arg(modelData.artifactBytes)
                            Accessible.name: qsTr("Recorded revision %1")
                                .arg(String(modelData.documentRevisionDigest).substring(0, 12))
                        }

                        Button {
                            text: qsTr("Return to this revision")
                            enabled: root.host !== null && root.host.fixRollbackAvailable
                            onClicked: {
                                root.pendingRollbackId = modelData.rollbackId
                                rollbackConfirm.open()
                            }
                            Accessible.name: qsTr("Return to recorded revision %1")
                                .arg(String(modelData.documentRevisionDigest).substring(0, 12))
                        }
                    }
                }
            }
        }

        Dialog {
            id: rollbackConfirm
            objectName: "fixRollbackConfirmDialog"
            title: qsTr("Return to a recorded revision")
            modal: true
            anchors.centerIn: parent
            standardButtons: Dialog.Ok | Dialog.Cancel
            Accessible.name: qsTr("Confirm returning to a recorded revision")

            Label {
                width: 320
                wrapMode: Text.WordWrap
                text: qsTr("A new revision is written next to the document and a rolled-back event is appended. Existing history entries are kept and the open document is not overwritten.")
            }

            onAccepted: if (root.host) root.host.requestFixRollback(root.pendingRollbackId)
            onRejected: root.pendingRollbackId = ""
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

        GroupBox {
            Layout.fillWidth: true
            title: qsTr("Recipe steps")
            visible: root.host && root.host.actionListSteps.length > 0
            Accessible.name: qsTr("Schema-driven Action List recipe editor")

            ScrollView {
                anchors.fill: parent
                clip: true

                Column {
                    width: parent.width
                    spacing: 6

                    Repeater {
                        model: root.host ? root.host.actionListSteps : []

                        delegate: ColumnLayout {
                            required property var modelData
                            property var stepData: modelData
                            Layout.fillWidth: true
                            spacing: 3

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Step %1 — %2").arg(stepData.id).arg(stepData.operation)
                                font.bold: true
                            }

                            Repeater {
                                model: stepData.parameterSchema && stepData.parameterSchema.properties
                                    ? Object.keys(stepData.parameterSchema.properties) : []

                                delegate: RowLayout {
                                    required property string modelData
                                    Layout.fillWidth: true
                                    property string parameterName: modelData
                                    property var parameterSchema: stepData.parameterSchema.properties[parameterName]

                                    Label {
                                        Layout.preferredWidth: 150
                                        text: qsTr("%1 (%2)").arg(parameterName).arg(parameterSchema.type || qsTr("value"))
                                        elide: Text.ElideRight
                                    }

                                    CheckBox {
                                        visible: parameterSchema.type === "boolean"
                                        checked: Boolean(stepData.parameters[parameterName])
                                        text: qsTr("Enabled")
                                        onToggled: if (root.host) root.host.setActionListStepParameter(stepData.index, parameterName, checked)
                                    }

                                    TextField {
                                        Layout.fillWidth: true
                                        visible: parameterSchema.type !== "boolean"
                                        text: stepData.parameters[parameterName] === undefined
                                            ? "" : String(stepData.parameters[parameterName])
                                        placeholderText: parameterSchema.enum ? parameterSchema.enum.join(" | ") : ""
                                        onEditingFinished: if (root.host)
                                            root.host.setActionListStepParameter(stepData.index, parameterName, text)
                                    }
                                }
                            }
                        }
                    }
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
                enabled: root.host && root.host.hasDocument && root.host.actionList.validationReady && !root.busy
                onClicked: root.host.planActionList()
                Accessible.name: qsTr("Plan Action List")
            }

            Button {
                objectName: "saveActionListRecipeButton"
                text: qsTr("Save recipe")
                enabled: root.host && root.host.selectedActionListRecipeId.length > 0 && !root.busy
                onClicked: root.host.saveActionListRecipe()
                Accessible.name: qsTr("Save edited Action List recipe")
            }

            Button {
                objectName: "confirmActionListButton"
                text: qsTr("Approve and run")
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

        Label {
            Layout.fillWidth: true
            visible: root.host && root.host.actionList.planDigest.length > 0
            text: qsTr("Plan digest: %1").arg(root.host.actionList.planDigest)
            wrapMode: Text.WordWrap
            Accessible.name: qsTr("Action List plan digest")
        }

        Label {
            Layout.fillWidth: true
            visible: root.host && root.host.actionList.governedStatus.length > 0
            text: qsTr("Publication sign-off: %1").arg(root.host.actionList.governedStatus)
            Accessible.name: qsTr("Action List publication sign-off status")
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
                id: stepRow
                width: stepsView.width
                text: "%1 — %2 (%3 ms)".arg(model.stepId, model.statusName, model.durationMs)
                Accessible.role: Accessible.ListItem
                Accessible.name: text
                Accessible.description: qsTr("Operation %1. Parameters %2. Diagnostics %3.")
                    .arg(model.operationId).arg(model.resolvedParameters).arg(JSON.stringify(model.diagnostics))
                onClicked: if (root.host) root.host.inspectActionListStep(stepRow.index)

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
                        visible: model.findingDelta && model.findingDelta.resolved && model.findingDelta.resolved.length > 0
                        text: qsTr("Cleared (%1): %2")
                            .arg(model.findingDelta.resolved.length)
                            .arg(model.findingDelta.resolved.join(", "))
                        color: "#15803d"
                        Accessible.name: qsTr("Cleared preflight findings")
                    }

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        visible: model.findingDelta && model.findingDelta.unchanged && model.findingDelta.unchanged.length > 0
                        text: qsTr("Remaining (%1): %2")
                            .arg(model.findingDelta.unchanged.length)
                            .arg(model.findingDelta.unchanged.join(", "))
                        color: "#a16207"
                        Accessible.name: qsTr("Remaining preflight findings")
                    }

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        visible: model.findingDelta && model.findingDelta.introduced && model.findingDelta.introduced.length > 0
                        text: qsTr("Introduced (%1): %2")
                            .arg(model.findingDelta.introduced.length)
                            .arg(model.findingDelta.introduced.join(", "))
                        color: "#b91c1c"
                        font.bold: true
                        Accessible.name: qsTr("Introduced preflight findings")
                    }

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        visible: model.findingDelta && model.findingDelta.incomplete && model.findingDelta.incomplete.length > 0
                        text: qsTr("Not fully rechecked (%1): %2")
                            .arg(model.findingDelta.incomplete.length)
                            .arg(model.findingDelta.incomplete.join(", "))
                        color: "#a16207"
                        font.bold: true
                        Accessible.name: qsTr("Incomplete preflight recheck")
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
