import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Loop.Quick

Item {
    id: root
    objectName: "workspace"

    property var host: editorHost
    property var window: null
    property var openDialog: null
    property var saveAsDialog: null
    readonly property bool preferReducedMotion: host ? host.preferReducedMotion : false

    function workspaceIndex(workspaceValue) {
        switch (workspaceValue) {
        case EditorHost.Document: return 0
        case EditorHost.Preflight: return 1
        case EditorHost.ProductionPreview: return 2
        case EditorHost.Pages: return 3
        case EditorHost.Inspect: return 4
        case EditorHost.Fix: return 5
        case EditorHost.Compare: return 6
        default: return 0
        }
    }

    function setWorkspaceFromRail(workspaceValue) {
        if (!host || workspaceValue === EditorHost.Compare) {
            return
        }
        host.setWorkspace(workspaceValue)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ShellToolBar {
            Layout.fillWidth: true
            host: root.host
            window: root.window
            openDialog: root.openDialog
            saveAsDialog: root.saveAsDialog
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Pane {
                id: workspaceRail
                objectName: "workspaceRail"
                Layout.preferredWidth: 132
                Layout.fillHeight: true
                padding: 8

                focus: true
                activeFocusOnTab: true
                Accessible.role: Accessible.Grouping
                Accessible.name: qsTr("Workspace rail")
                Accessible.description: qsTr("Switch between document, production, preflight, inspection, and fix workspaces.")

                Keys.onPressed: function(event) {
                    if (!root.host) {
                        return
                    }

                    var direction = 0
                    if (event.key === Qt.Key_Up || event.key === Qt.Key_Left) {
                        direction = -1
                    } else if (event.key === Qt.Key_Down || event.key === Qt.Key_Right) {
                        direction = 1
                    } else if (event.key === Qt.Key_Home) {
                        root.host.setWorkspace(EditorHost.Document)
                        event.accepted = true
                        return
                    } else if (event.key === Qt.Key_End) {
                        root.host.setWorkspace(EditorHost.Fix)
                        event.accepted = true
                        return
                    }

                    if (direction === 0) {
                        return
                    }

                    var current = root.workspaceIndex(root.host.workspace)
                    var candidate = current + direction
                    while (candidate >= 0 && candidate < 6) {
                        var candidateWorkspace = [EditorHost.Document, EditorHost.Preflight,
                                                  EditorHost.ProductionPreview, EditorHost.Pages,
                                                  EditorHost.Inspect, EditorHost.Fix][candidate]
                        if (root.host.isWorkspaceEnabled(candidateWorkspace)) {
                            root.host.setWorkspace(candidateWorkspace)
                            event.accepted = true
                            return
                        }
                        candidate += direction
                    }
                }

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8

                    Repeater {
                        model: [
                            { label: qsTr("Document"), workspace: EditorHost.Document },
                            { label: qsTr("Preflight"), workspace: EditorHost.Preflight },
                            { label: qsTr("Production Preview"), workspace: EditorHost.ProductionPreview },
                            { label: qsTr("Pages / Production"), workspace: EditorHost.Pages },
                            { label: qsTr("Inspect"), workspace: EditorHost.Inspect },
                            { label: qsTr("Fix"), workspace: EditorHost.Fix },
                            { label: qsTr("Compare"), workspace: EditorHost.Compare }
                        ]

                        delegate: ToolButton {
                            required property var modelData

                            Layout.fillWidth: true
                            objectName: "workspaceButton_" + modelData.workspace
                            text: modelData.label
                            checkable: true
                            enabled: host ? host.isWorkspaceEnabled(modelData.workspace) : false
                            activeFocusOnTab: true
                            checked: host && host.workspace === modelData.workspace
                            onClicked: root.setWorkspaceFromRail(modelData.workspace)
                            Accessible.role: Accessible.Button
                            Accessible.name: modelData.workspace === EditorHost.Compare
                                ? qsTr("Compare workspace (product decision pending)")
                                : qsTr("%1 workspace").arg(modelData.label)
                            Accessible.description: modelData.workspace === EditorHost.Compare
                                ? qsTr("Compare is disabled until the product decision is approved.")
                                : ""
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            StackLayout {
                id: workspaceStack
                objectName: "workspaceStack"
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: host ? root.workspaceIndex(host.workspace) : 0
                Accessible.role: Accessible.Pane
                Accessible.name: qsTr("Workspace content")

                DocumentPane {
                    id: documentPane
                    host: root.host
                }

                PreflightPane {
                    host: root.host
                }

                WorkspacePlaceholderPane {
                    host: root.host
                    titleText: qsTr("Production Preview")
                    descriptionText: qsTr("Soft proofing and output preview will appear here.")
                    Accessible.name: qsTr("Production Preview workspace")
                }

                WorkspacePlaceholderPane {
                    host: root.host
                    titleText: qsTr("Pages / Production")
                    descriptionText: qsTr("Page assembly and production geometry will appear here.")
                    Accessible.name: qsTr("Pages and Production workspace")
                }

                WorkspacePlaceholderPane {
                    host: root.host
                    titleText: qsTr("Inspect")
                    descriptionText: qsTr("Dedicated inspection tools will appear here. Use the document inspector dock for contextual selection.")
                    Accessible.name: qsTr("Inspect workspace")
                }

                WorkspacePlaceholderPane {
                    host: root.host
                    titleText: qsTr("Fix")
                    descriptionText: qsTr("Bounded corrective operations will appear here.")
                    Accessible.name: qsTr("Fix workspace")
                }

                WorkspacePlaceholderPane {
                    host: root.host
                    titleText: qsTr("Compare")
                    descriptionText: qsTr("Compare remains deferred pending the product decision.")
                    Accessible.name: qsTr("Compare workspace placeholder")
                }
            }
        }
    }

    KeyNavigation.tab: documentPane
    KeyNavigation.backtab: workspaceRail

    Connections {
        target: root.host
        function onWorkspaceChanged() {
            if (root.host) {
                workspaceStack.currentIndex = root.workspaceIndex(root.host.workspace)
            }
        }
        function onPresentationChanged() {
            if (root.host && root.host.workspaceRequest >= 0) {
                root.host.acknowledgeWorkspaceRequest()
            }
        }
    }
}
