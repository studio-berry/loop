import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    objectName: "operatorHelpDialog"
    property var helpController: null
    property var host: null
    property string sampleError: ""
    property string findingType: ""
    readonly property var catalog: helpController ? helpController.catalog : ({})
    readonly property var guide: helpController ? helpController.guide : ({})
    readonly property var checkHelp: helpController
        ? helpController.findingHelp(checkSelector.currentText, findingType) : ({})
    title: helpController && helpController.welcomeNeeded ? qsTr("Welcome to Loop") : qsTr("Operator help")
    modal: true
    width: Math.min(720, parent ? parent.width - 32 : 720)
    height: Math.min(680, parent ? parent.height - 32 : 680)
    anchors.centerIn: parent
    closePolicy: Popup.CloseOnEscape
    onClosed: if (host && host.focusRestoration) host.focusRestoration.restore()

    function showWorkspace() {
        workspaceSelector.currentIndex = host ? host.workspace : 0
        sections.currentIndex = 1
        open()
    }

    function showFinding(checkId, type) {
        findingType = type
        const index = catalog.registry ? catalog.registry.indexOf(checkId) : -1
        checkSelector.currentIndex = index
        sections.currentIndex = 2
        open()
    }

    function openSample(id) {
        sampleError = ""
        const sample = helpController.prepareSample(id)
        if (!sample.ok) {
            sampleError = sample.error
            return
        }
        if (!host.importPreflightProfileFileUrl(sample.profile)) {
            sampleError = qsTr("The sample profile could not be imported. Read the document status for details.")
            return
        }
        if (sample.recipe && !host.importActionListRecipe(sample.recipe)) {
            sampleError = qsTr("The correction recipe could not be imported. Read the document status for details.")
            return
        }
        host.openFileUrl(sample.pdf)
        host.setWorkspace(1)
        close()
    }

    contentItem: ColumnLayout {
        Accessible.role: Accessible.Grouping
        Accessible.name: root.title
        spacing: 8
        Label {
            Layout.fillWidth: true
            visible: root.helpController && root.helpController.diagnostic.length > 0
            text: root.helpController ? root.helpController.diagnostic : ""
            wrapMode: Text.WordWrap
            Accessible.name: text
        }
        TabBar {
            id: sections
            objectName: "operatorHelpSections"
            Layout.fillWidth: true
            TabButton { text: qsTr("Getting started") }
            TabButton { text: qsTr("Workspaces") }
            TabButton { text: qsTr("Findings") }
            TabButton { text: qsTr("Sample jobs") }
        }
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: sections.currentIndex
            ScrollView {
                clip: true
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width
                    spacing: 12
                    Repeater {
                        model: root.guide.getting_started || []
                        Label {
                            required property string modelData
                            Layout.fillWidth: true
                            text: modelData
                            wrapMode: Text.WordWrap
                            activeFocusOnTab: true
                            Accessible.name: text
                        }
                    }
                    Button {
                        objectName: "onboardingSamplesButton"
                        text: qsTr("Choose a sample job")
                        onClicked: sections.currentIndex = 3
                    }
                }
            }
            ColumnLayout {
                ComboBox {
                    id: workspaceSelector
                    objectName: "workspaceHelpSelector"
                    Layout.fillWidth: true
                    model: root.guide.workspaces || []
                    textRole: "name"
                    Accessible.name: qsTr("Workspace help topic")
                }
                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: availableWidth
                    clip: true
                    Label {
                        width: parent.width
                        text: root.helpController ? root.helpController.workspaceHelp(workspaceSelector.currentIndex) : ""
                        wrapMode: Text.WordWrap
                        activeFocusOnTab: true
                        Accessible.name: text
                    }
                }
                Button {
                    text: qsTr("Go to %1").arg(workspaceSelector.currentText)
                    enabled: root.host && root.host.isWorkspaceEnabled(workspaceSelector.currentIndex)
                    onClicked: {
                        root.host.setWorkspace(workspaceSelector.currentIndex)
                        root.close()
                    }
                }
            }
            ColumnLayout {
                ComboBox {
                    id: checkSelector
                    objectName: "findingHelpSelector"
                    Layout.fillWidth: true
                    model: root.catalog.registry || []
                    Accessible.name: qsTr("Finding check help")
                    onActivated: root.findingType = ""
                }
                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: availableWidth
                    clip: true
                    ColumnLayout {
                        width: parent.width
                        spacing: 12
                        Label {
                            objectName: "findingHelpCoverage"
                            Layout.fillWidth: true
                            text: qsTr("Coverage: %1").arg(root.checkHelp.coverage || qsTr("unavailable"))
                            wrapMode: Text.WordWrap
                            activeFocusOnTab: true
                            Accessible.name: text
                        }
                        Label {
                            objectName: "findingHelpMeasures"
                            Layout.fillWidth: true
                            text: qsTr("Measures: %1").arg(root.checkHelp.measures || qsTr("No catalog measures are available."))
                            wrapMode: Text.WordWrap
                            activeFocusOnTab: true
                            Accessible.name: text
                        }
                        Label {
                            objectName: "findingHelpLimitations"
                            Layout.fillWidth: true
                            text: qsTr("Limitations: %1").arg(root.checkHelp.limitations || qsTr("No catalog limitations are available; do not infer coverage."))
                            wrapMode: Text.WordWrap
                            activeFocusOnTab: true
                            Accessible.name: text
                        }
                        Repeater {
                            model: root.checkHelp.severity || []
                            Label {
                                required property var modelData
                                Layout.fillWidth: true
                                text: "%1: %2".arg(modelData.finding_type).arg(modelData.condition)
                                wrapMode: Text.WordWrap
                                activeFocusOnTab: true
                                Accessible.name: text
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            text: root.catalog.claim || ""
                            wrapMode: Text.WordWrap
                            activeFocusOnTab: true
                            Accessible.name: text
                        }
                        Label {
                            objectName: "operatorHelpNotCovered"
                            Layout.fillWidth: true
                            text: qsTr("Not covered:\n%1").arg((root.catalog.not_covered || []).join("\n"))
                            wrapMode: Text.WordWrap
                            activeFocusOnTab: true
                            Accessible.name: text
                        }
                    }
                }
            }
            ScrollView {
                clip: true
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width
                    spacing: 12
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Original synthetic jobs, licensed MIT. Opening a job copies its PDF, profile and any recipe into your local sample directory, then selects that learning profile. Choose your production profile again before inspecting a real job.")
                        wrapMode: Text.WordWrap
                        Accessible.name: text
                    }
                    Label {
                        objectName: "sampleOpenError"
                        Layout.fillWidth: true
                        visible: root.sampleError.length > 0
                        text: root.sampleError
                        wrapMode: Text.WordWrap
                        Accessible.name: text
                    }
                    Repeater {
                        model: root.helpController ? root.helpController.sampleJobs : []
                        GroupBox {
                            required property var modelData
                            Layout.fillWidth: true
                            title: modelData.title
                            ColumnLayout {
                                anchors.fill: parent
                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.description
                                    wrapMode: Text.WordWrap
                                    Accessible.name: text
                                }
                                Button {
                                    objectName: "openSample_" + modelData.id
                                    text: qsTr("Open sample")
                                    Accessible.name: qsTr("Open %1 sample").arg(modelData.title)
                                    enabled: root.host && root.host.isCommandEnabled("actionOpen") && root.host.documentShellStatus !== "MODIFIED"
                                    onClicked: root.openSample(modelData.id)
                                }
                            }
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Save or close an edited job before opening a sample. Opening a sample does not approve or certify any correction.")
                        wrapMode: Text.WordWrap
                        Accessible.name: text
                    }
                }
            }
        }
        Button {
            objectName: "dismissOperatorWelcome"
            text: qsTr("Continue to workspace")
            onClicked: {
                if (root.helpController && !root.helpController.acknowledgeWelcome()) {
                    root.sampleError = qsTr("The welcome preference could not be saved. The guide will remain available at startup.")
                    sections.currentIndex = 3
                    return
                }
                root.close()
            }
        }
        Label {
            text: qsTr("Open help again with F1 or the workspace Help button.")
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            Accessible.name: text
        }
    }
}
