import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Loop.Quick

Item {
    id: root
    objectName: "documentPane"

    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Document workspace")
    Accessible.description: qsTr("Page navigation, document outline, search, PDF canvas, and contextual inspection.")

    property var host: editorHost
    property var documentModel: host ? host.documentModel : null

    function revealSearch() {
        tabBar.currentIndex = 2
        searchField.forceActiveFocus()
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Pane {
            Layout.preferredWidth: 230
            Layout.fillHeight: true
            padding: 8

            ColumnLayout {
                anchors.fill: parent
                spacing: 8

                TabBar {
                    id: tabBar
                    objectName: "documentTabBar"
                    Layout.fillWidth: true
                    activeFocusOnTab: true

                    TabButton {
                        objectName: "pagesTabButton"
                        text: qsTr("Pages")
                        Accessible.name: qsTr("Pages tab")
                    }
                    TabButton {
                        objectName: "outlineTabButton"
                        text: qsTr("Outline")
                        Accessible.name: qsTr("Outline tab")
                    }
                    TabButton {
                        objectName: "searchTabButton"
                        text: qsTr("Search")
                        Accessible.name: qsTr("Search tab")
                    }
                }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: tabBar.currentIndex

                    ListView {
                        id: pagesView
                        objectName: "pagesView"
                        clip: true
                        focus: visible && tabBar.currentIndex === 0
                        activeFocusOnTab: true
                        keyNavigationEnabled: true
                        highlightFollowsCurrentItem: true
                        model: root.documentModel ? root.documentModel.pages : null
                        Accessible.role: Accessible.List
                        Accessible.name: qsTr("Page thumbnails")

                        delegate: ItemDelegate {
                            width: pagesView.width
                            activeFocusOnTab: true
                            text: qsTr("Page %1  %2 × %3").arg(pageNumber).arg(Math.round(pageWidth)).arg(Math.round(pageHeight))
                            highlighted: root.host && root.host.currentPage === index
                            Accessible.role: Accessible.ListItem
                            Accessible.name: text
                            onClicked: if (root.host)
                                root.host.goToPage(index)
                        }
                    }

                    TreeView {
                        id: outlineView
                        objectName: "outlineView"
                        clip: true
                        focus: visible && tabBar.currentIndex === 1
                        activeFocusOnTab: true
                        keyNavigationEnabled: true
                        model: root.documentModel ? root.documentModel.outline : null
                        Accessible.role: Accessible.Tree
                        Accessible.name: qsTr("Document outline")

                        delegate: ItemDelegate {
                            width: outlineView.width
                            activeFocusOnTab: true
                            text: model.display !== undefined ? model.display : title
                            Accessible.role: Accessible.TreeItem
                            Accessible.name: text
                            enabled: page >= 0
                            onClicked: if (root.host && page >= 0)
                                root.host.goToOutlinePage(page)
                        }

                        Label {
                            anchors.centerIn: parent
                            visible: outlineView.rows === 0
                            text: qsTr("No outline")
                        }
                    }

                    ColumnLayout {
                        spacing: 8

                        RowLayout {
                            Layout.fillWidth: true
                            TextField {
                                id: searchField
                                objectName: "searchField"
                                Layout.fillWidth: true
                                placeholderText: qsTr("Find in document")
                                focus: visible && tabBar.currentIndex === 2
                                activeFocusOnTab: true
                                Accessible.name: qsTr("Search text")
                                onAccepted: if (root.documentModel)
                                    root.documentModel.search(text)
                            }
                            Button {
                                objectName: "searchButton"
                                text: qsTr("Find")
                                enabled: searchField.text.length > 0 && !!root.documentModel
                                activeFocusOnTab: true
                                Accessible.name: qsTr("Find in document")
                                onClicked: root.documentModel.search(searchField.text)
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Button {
                                text: qsTr("Previous")
                                enabled: root.host && root.host.commandEpoch >= 0 && root.host.isCommandEnabled("actionFindPrevious")
                                onClicked: if (root.host)
                                    root.host.invokeCommand("actionFindPrevious")
                            }
                            Button {
                                text: qsTr("Next")
                                enabled: root.host && root.host.commandEpoch >= 0 && root.host.isCommandEnabled("actionFindNext")
                                onClicked: if (root.host)
                                    root.host.invokeCommand("actionFindNext")
                            }
                            Label {
                                Layout.fillWidth: true
                                text: resultsView.count > 0 ? qsTr("%1 result(s)").arg(resultsView.count) : qsTr("No results")
                            }
                        }

                            ListView {
                                id: resultsView
                                objectName: "searchResultsView"
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                                clip: true
                                activeFocusOnTab: true
                                keyNavigationEnabled: true
                                model: root.documentModel ? root.documentModel.searchResults : null
                                Accessible.role: Accessible.List
                            Accessible.name: qsTr("Search results")

                            delegate: ItemDelegate {
                                    width: resultsView.width
                                    activeFocusOnTab: true
                                    text: qsTr("Page %1: %2").arg(page + 1).arg(context)
                                    Accessible.role: Accessible.ListItem
                                Accessible.name: text
                                onClicked: if (root.host)
                                    root.host.goToPage(page)
                            }
                        }
                    }
                }
            }
        }

        CanvasPane {
            id: canvasPane
            Layout.fillWidth: true
            Layout.fillHeight: true
            host: root.host
            Accessible.name: qsTr("Document canvas pane")
        }

        InspectorPane {
            id: inspectorPane
            Layout.preferredWidth: 280
            Layout.fillHeight: true
            host: root.host
            Accessible.name: qsTr("Contextual inspector dock")
        }
    }

    Connections {
        target: root.host
        function onPresentationChanged() {
            if (root.host && root.host.searchPanelVisible) {
                root.revealSearch()
                root.host.acknowledgeSearchPanel()
            }
        }
    }
}
