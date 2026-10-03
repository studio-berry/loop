import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Pane {
    id: root

    objectName: "preflightPane"

    property var host: editorHost
    property var findingsModel: host ? host.preflight.findingsModel : null
    readonly property bool preferReducedMotion: host ? host.preferReducedMotion : false

    padding: 8

    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Preflight findings")
    Accessible.description: qsTr("Revision-bound preflight findings. Select a finding to inspect evidence on the canvas.")

    // Token-name -> glyph for the canonical non-colour icon. The name is LoopLibQuick's
    // stateIconName(), delivered through EditorHost as preflightStateVisual.icon; each canonical
    // name maps 1:1 to exactly one glyph so the kinds the badge can reach (Checkmark,
    // FilledCircle, FilledSquare, Hatched, Outline) stay visually distinct by shape wherever their
    // colour tokens collide - in HighContrast, StateIncomplete and StateNotChecked are both white
    // (#194). The mapping is presentation only and decides no state semantics: the colour and the
    // accessible name remain Core's.
    readonly property var preflightIconGlyphs: ({
        "Checkmark": "\u2713",
        "FilledCircle": "\u25CF",
        "FilledSquare": "\u25A0",
        "FilledTriangle": "\u25B2",
        "Hatched": "\u25A8",
        "Outline": "\u25CB",
        "BadgeOverlay": "\u25C6"
    })

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        // Canonical state badge. The colour, the icon and the accessible name are rendered from
        // LoopLibQuick through EditorHost (preflightStateColor / preflightStateVisual); this file
        // decides nothing about pass or severity, and invents no state wording of its own. The icon
        // is preflightIconGlyphs[preflightStateVisual.icon], that token-name -> glyph lookup, so the
        // kinds stay distinct by shape where their colour tokens collide (HighContrast's white
        // StateIncomplete and StateNotChecked, #194).
        RowLayout {
            id: stateBadge
            objectName: "preflightStateBadge"
            Layout.fillWidth: true
            spacing: 8

            Label {
                objectName: "preflightStateLabel"
                id: stateBadgeIcon
                Layout.alignment: Qt.AlignTop
                font.pixelSize: 14
                color: root.host ? root.host.preflightStateColor : "transparent"
                text: root.host ? (root.preflightIconGlyphs[root.host.preflightStateVisual.icon] || "") : ""
                Accessible.ignored: true
            }

            Label {
                objectName: "preflightVerdict"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                activeFocusOnTab: true
                text: root.host ? root.host.preflight.verdictDescription : qsTr("No accepted verdict is available.")
                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("Preflight verdict")
                Accessible.description: text
                Accessible.focusable: true
                Accessible.focused: activeFocus
                onTextChanged: if (visible) Accessible.announce(text)
            }
        }

        Label {
            objectName: "preflightLimitations"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            activeFocusOnTab: true
            text: root.host ? root.host.preflight.limitationDescription : qsTr("Inspection limitations are unavailable.")
            Accessible.role: Accessible.StaticText
            Accessible.name: qsTr("Preflight limitations")
            Accessible.description: text
            Accessible.focusable: true
            Accessible.focused: activeFocus
            onTextChanged: if (visible) Accessible.announce(text)
        }

        Label {
            objectName: "preflightSelectedFinding"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            activeFocusOnTab: true
            text: root.host ? root.host.preflight.selectedFindingDescription : qsTr("No current preflight finding is selected.")
            Accessible.role: Accessible.StaticText
            Accessible.name: qsTr("Selected preflight finding")
            Accessible.description: text
            Accessible.focusable: true
            Accessible.focused: activeFocus
            onTextChanged: if (visible) Accessible.announce(text)
        }

        Label {
            objectName: "preflightJobStatus"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            activeFocusOnTab: true
            text: root.host ? root.host.preflight.jobDescription : qsTr("No preflight job has been submitted.")
            Accessible.role: Accessible.StaticText
            Accessible.name: qsTr("Preflight job")
            Accessible.description: text
            Accessible.focusable: true
            Accessible.focused: activeFocus
            onTextChanged: if (visible) Accessible.announce(text)
        }

        RowLayout {
            objectName: "preflightCertificateBadge"
            Layout.fillWidth: true
            spacing: 8

            Label {
                Layout.alignment: Qt.AlignTop
                font.pixelSize: 14
                color: root.host ? root.host.preflightCertificateStateColor : "transparent"
                text: root.host ? (root.preflightIconGlyphs[root.host.preflightCertificateStateVisual.icon] || "") : ""
                Accessible.ignored: true
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: root.host ? root.host.preflightCertificateSummary : ""
                Accessible.name: root.host ? root.host.preflightCertificateStateVisual.accessibleName : qsTr("Certified preflight status")
                Accessible.description: qsTr("Certified preflight is tamper-evident attribution, not a digital signature.")
            }
        }

        Label {
            objectName: "preflightCertificateTrustCopy"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Tamper-evident attribution only — not a digital signature.")
            Accessible.name: text
        }

        ComboBox {
            id: profileSelector
            objectName: "preflightProfileSelector"
            Layout.fillWidth: true
            model: root.host ? root.host.preflightProfiles : []
            textRole: "name"
            valueRole: "id"
            enabled: root.host && root.host.preflightStateName !== "running" && !(root.host && root.host.preflightProfileEditing)
            currentIndex: {
                if (!root.host)
                    return -1
                for (var i = 0; i < model.length; ++i) {
                    if (model[i].id === root.host.selectedPreflightProfileId)
                        return i
                }
                return -1
            }
            Accessible.name: qsTr("Preflight profile")
            Accessible.description: enabled ? qsTr("Select a bundled or local validated preflight profile.")
                : root.host && root.host.preflightProfileEditing ? qsTr("Save or cancel the current profile edit before selecting another profile.")
                : qsTr("The profile cannot change while preflight is running.")
            onActivated: function(index) {
                if (root.host && model[index])
                    root.host.selectPreflightProfile(model[index].id)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Button {
                objectName: "importPreflightProfileButton"
                text: qsTr("Import Profile")
                enabled: root.host && root.host.preflightStateName !== "running" && !root.host.preflightProfileEditing
                Accessible.name: qsTr("Import preflight profile")
                Accessible.description: enabled ? qsTr("Import and validate a preflight profile.")
                    : root.host && root.host.preflightProfileEditing ? qsTr("Save or cancel the current profile edit before importing a profile.")
                    : qsTr("Profiles cannot be imported while preflight is running.")
                onClicked: if (root.host) root.host.requestPreflightProfileImport()
            }

            Button {
                objectName: "customizePreflightProfileButton"
                text: qsTr("Customize Profile")
                enabled: root.host && root.host.preflightStateName !== "running" && !root.host.preflightProfileEditing
                Accessible.name: qsTr("Customize preflight profile")
                Accessible.description: enabled ? qsTr("Edit a copy of the selected preflight profile.")
                    : root.host && root.host.preflightProfileEditing ? qsTr("A profile edit is already in progress.")
                    : qsTr("Profiles cannot be edited while preflight is running.")
                onClicked: if (root.host) root.host.beginPreflightProfileEdit()
            }

            Button {
                objectName: "exportPreflightProfileButton"
                text: qsTr("Export Profile")
                enabled: root.host && root.host.preflightStateName !== "running"
                Accessible.name: qsTr("Export preflight profile")
                Accessible.description: enabled ? qsTr("Export the selected validated preflight profile.")
                    : qsTr("Profiles cannot be exported while preflight is running.")
                onClicked: if (root.host) root.host.requestPreflightProfileExport()
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: root.host && root.host.preflightProfileEditing
            spacing: 8

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Editing a copy of the selected profile. Save writes a fork with derived_from and a bumped version.")
            }

            Repeater {
                model: root.host ? root.host.preflightEditableChecks : []
                delegate: ColumnLayout {
                    Layout.fillWidth: true
                    required property var modelData
                    property string checkId: modelData.id

                    Label {
                        text: modelData.id
                        font.bold: true
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Severity") }
                        ComboBox {
                            id: severityCombo
                            Layout.fillWidth: true
                            model: ["error", "warning", "info"]
                            currentIndex: Math.max(0, ["error", "warning", "info"].indexOf(modelData.severity))
                            onActivated: function(index) {
                                if (root.host)
                                    root.host.setPreflightCheckField(checkId, "severity", severityCombo.model[index])
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Enabled") }
                        CheckBox {
                            checked: Boolean(modelData.enabled)
                            onToggled: if (root.host) root.host.setPreflightCheckField(checkId, "enabled", checked)
                        }
                    }

                    Repeater {
                        model: modelData.fields || []
                        delegate: RowLayout {
                            Layout.fillWidth: true
                            required property var modelData
                            Label { text: modelData.name }
                            TextField {
                                Layout.fillWidth: true
                                text: modelData.value === undefined || modelData.value === null ? "" : String(modelData.value)
                                onEditingFinished: if (root.host) root.host.setPreflightCheckField(checkId, modelData.name, text)
                            }
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Button {
                    objectName: "savePreflightProfileForkButton"
                    text: qsTr("Save Fork")
                    enabled: root.host && root.host.preflightProfileEditing
                    Accessible.name: qsTr("Save preflight profile fork")
                    Accessible.description: enabled ? qsTr("Save the edited copy as a new profile.") : qsTr("No profile edit is in progress.")
                    onClicked: if (root.host) root.host.requestPreflightProfileSave()
                }
                Button {
                    objectName: "cancelPreflightProfileEditButton"
                    text: qsTr("Cancel Edit")
                    enabled: root.host && root.host.preflightProfileEditing
                    Accessible.name: qsTr("Cancel preflight profile edit")
                    Accessible.description: enabled ? qsTr("Discard the current profile edit.") : qsTr("No profile edit is in progress.")
                    onClicked: if (root.host) root.host.cancelPreflightProfileEdit()
                }
            }
        }

        Repeater {
            model: root.host ? root.host.preflightVariables : []
            delegate: RowLayout {
                Layout.fillWidth: true
                required property var modelData

                Label {
                    text: modelData.required ? qsTr("%1 (required)").arg(modelData.name) : modelData.name
                    Accessible.name: modelData.description.length > 0 ? modelData.description : text
                }
                CheckBox {
                    objectName: "preflightVariableBoolean_" + modelData.name
                    visible: modelData.type === "boolean"
                    activeFocusOnTab: true
                    checked: Boolean(modelData.value)
                    text: modelData.description
                    Accessible.name: qsTr("Preflight variable %1").arg(modelData.name)
                    onToggled: if (root.host) root.host.setPreflightVariable(modelData.name, checked)
                }
                TextField {
                    objectName: "preflightVariableText_" + modelData.name
                    Layout.fillWidth: true
                    visible: modelData.type !== "boolean"
                    activeFocusOnTab: true
                    text: modelData.value === undefined || modelData.value === null ? "" : String(modelData.value)
                    inputMethodHints: modelData.type === "string" ? Qt.ImhNone : Qt.ImhFormattedNumbersOnly
                    Accessible.name: qsTr("Preflight variable %1").arg(modelData.name)
                    onEditingFinished: if (root.host) root.host.setPreflightVariable(modelData.name, text)
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true

            Button {
                objectName: "runPreflightButton"
                text: qsTr("Run Preflight")
                enabled: root.host && root.host.preflightRunUnavailableReason.length === 0
                Accessible.name: qsTr("Run preflight")
                Accessible.role: Accessible.Button
                Accessible.description: enabled ? qsTr("Runs the selected validated preflight profile.")
                    : root.host ? root.host.preflightRunUnavailableReason : qsTr("No document host is available.")
                onClicked: root.host.runPreflight()
            }

            Button {
                objectName: "cancelPreflightButton"
                text: qsTr("Cancel")
                enabled: root.host && root.host.preflightStateName === "running"
                Accessible.name: qsTr("Cancel preflight")
                Accessible.role: Accessible.Button
                Accessible.description: enabled ? qsTr("Cancels the running preflight job.")
                    : root.host ? root.host.preflight.cancelUnavailableReason : qsTr("No document host is available.")
                onClicked: root.host.cancelPreflight()
            }

            Button {
                objectName: "exportPreflightReportButton"
                text: qsTr("Export Report")
                enabled: root.host && root.host.hasPreflightReport
                Accessible.name: qsTr("Export preflight report")
                Accessible.role: Accessible.Button
                Accessible.description: enabled ? qsTr("Exports the retained normalized preflight report as JSON.")
                    : root.host ? root.host.preflight.exportUnavailableReason : qsTr("No document host is available.")
                onClicked: root.host.requestPreflightReportExport()
            }

            ProgressBar {
                objectName: "preflightProgress"
                Layout.fillWidth: true
                from: 0
                to: 100
                value: root.host ? root.host.preflight.progress : 0
                enabled: root.host && root.host.preflightStateName === "running"
                Accessible.name: qsTr("Preflight progress")
                Accessible.description: qsTr("Progress reported by the active preflight job.")
            }
        }

        Connections {
            target: root.findingsModel
            function onSelectedFindingIdChanged(findingId) {
                if (!root.findingsModel || findingId.length === 0)
                    return
                const row = root.findingsModel.rowForFindingId(findingId)
                if (row >= 0 && findingsView.currentIndex !== row)
                    findingsView.currentIndex = row
            }
        }

        ListView {
            id: findingsView
            objectName: "preflightFindingsView"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            focus: visible
            activeFocusOnTab: true
            keyNavigationEnabled: true
            highlightFollowsCurrentItem: true
            highlightMoveDuration: root.preferReducedMotion ? 0 : 120
            model: root.findingsModel
            currentIndex: -1

            Accessible.role: Accessible.List
            Accessible.name: qsTr("Preflight findings list")
            Accessible.description: qsTr("Use arrow keys to move between findings and Enter to navigate to evidence.")

            delegate: ItemDelegate {
                width: findingsView.width
                activeFocusOnTab: true
                text: "%1 — %2".arg(model.severity).arg(model.message)
                highlighted: model.selected
                Accessible.selected: model.selected
                Accessible.role: Accessible.ListItem
                Accessible.name: model.message
                Accessible.description: qsTr("Severity %1, scope %2, page %3, object %4, check %5, evidence %6")
                    .arg(model.severity).arg(model.scope).arg(model.page).arg(model.objectId).arg(model.checkId).arg(model.evidenceIds.join(", "))
                onClicked: {
                    findingsView.currentIndex = index
                    if (host) {
                        host.selectFinding(model.findingId)
                    }
                }
            }

            // ListView owns keyboard traversal; selection remains a stable finding-id
            // intent, so the host can reject stale or unsupported targets authoritatively.
            onCurrentIndexChanged: {
                if (currentIndex >= 0 && host && findingsModel) {
                    const findingId = findingsModel.findingIdAt(currentIndex)
                    if (findingId.length > 0)
                        host.selectFinding(findingId)
                }
            }

            Keys.onReturnPressed: {
                if (currentIndex >= 0 && host && findingsModel) {
                    host.selectFinding(findingsModel.findingIdAt(currentIndex))
                }
            }
        }
    }
}
