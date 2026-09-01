import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Loupe.Quick

Item {
    id: root

    property var host: editorHost
    readonly property bool preferReducedMotion: host ? host.preferReducedMotion : false

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Pane {
            id: workspaceRail
            Layout.preferredWidth: 120
            Layout.fillHeight: true
            padding: 8

            focus: true
            Accessible.role: Accessible.Grouping
            Accessible.name: qsTr("Workspace rail")

            ColumnLayout {
                anchors.fill: parent
                spacing: 8

                ToolButton {
                    id: documentButton
                    Layout.fillWidth: true
                    text: qsTr("Document")
                    checkable: true
                    checked: workspaceStack.currentIndex === 0
                    onClicked: workspaceStack.currentIndex = 0
                    Accessible.name: qsTr("Document workspace")
                    Accessible.role: Accessible.Button
                }

                ToolButton {
                    id: preflightButton
                    Layout.fillWidth: true
                    text: qsTr("Preflight")
                    checkable: true
                    checked: workspaceStack.currentIndex === 1
                    onClicked: workspaceStack.currentIndex = 1
                    Accessible.name: qsTr("Preflight workspace")
                    Accessible.role: Accessible.Button
                }

                ToolButton {
                    id: inspectorButton
                    Layout.fillWidth: true
                    text: qsTr("Inspector")
                    checkable: true
                    checked: workspaceStack.currentIndex === 2
                    onClicked: workspaceStack.currentIndex = 2
                    Accessible.name: qsTr("Inspector workspace")
                    Accessible.role: Accessible.Button
                }

                Item { Layout.fillHeight: true }
            }
        }

        StackLayout {
            id: workspaceStack
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: 0

            DocumentPane {
                id: documentPane
                host: root.host
            }

            PreflightPane {
                host: root.host
            }

            InspectorPane {
                host: root.host
            }
        }
    }

    KeyNavigation.tab: documentPane

    Connections {
        target: root.host
        function onPresentationChanged() {
            if (root.host && root.host.workspaceRequest >= 0) {
                workspaceStack.currentIndex = root.host.workspaceRequest
                root.host.acknowledgeWorkspaceRequest()
            }
        }
    }
}
