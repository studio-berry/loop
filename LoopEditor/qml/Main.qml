import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import QtQuick.Dialogs

import Loop.Quick

ApplicationWindow {
    id: window

    property var host: editorHost
    readonly property bool preferReducedMotion: host ? host.preferReducedMotion : false

    visible: true
    width: 1024
    height: 768
    title: host && host.displayTitle.length > 0 ? host.displayTitle : qsTr("Loop")

    property var commandMap: ({})
    property int commandEpoch: host ? host.commandEpoch : 0

    function rebuildCommands() {
        const next = {}
        if (!host) {
            commandMap = next
            return
        }

        const descriptors = host.commandDescriptors()
        for (let index = 0; index < descriptors.length; ++index) {
            const entry = descriptors[index]
            next[entry.id] = entry
        }
        commandMap = next
    }

    function commandEnabled(commandId) {
        const entry = commandMap[commandId]
        return !!entry && entry.enabled === true
    }

    function invoke(commandId) {
        if (!host) {
            return
        }
        host.invokeCommand(commandId)
    }

    function shortcutSequence(entry) {
        if (!entry || !entry.id) {
            return ""
        }
        return entry.shortcutText || ""
    }

    Connections {
        target: host
        function onCommandEpochChanged() {
            window.commandEpoch = host.commandEpoch
            window.rebuildCommands()
        }
        function onPresentationChanged() {
            if (host && host.displayTitle.length > 0) {
                window.title = host.displayTitle
            } else {
                window.title = qsTr("Loop")
            }
            if (host) {
                if (host.fullscreenRequested) {
                    window.visibility = Window.FullScreen
                } else if (window.visibility === Window.FullScreen) {
                    window.visibility = Window.Windowed
                }
            }
        }
    }

    Component.onCompleted: rebuildCommands()

    menuBar: MenuBar {
        Menu {
            title: qsTr("&File")
            Action {
                text: qsTr("&Open…")
                enabled: commandEnabled("actionOpen")
                shortcut: shortcutSequence(commandMap["actionOpen"])
                onTriggered: {
                    if (host && host.focusRestoration) {
                        host.focusRestoration.remember(window.activeFocusItem)
                    }
                    openDialog.open()
                }
            }
            Action {
                text: qsTr("&Close")
                enabled: commandEnabled("actionClose")
                shortcut: shortcutSequence(commandMap["actionClose"])
                onTriggered: invoke("actionClose")
            }
            Action {
                text: qsTr("&Save")
                enabled: commandEnabled("actionSave")
                shortcut: shortcutSequence(commandMap["actionSave"])
                onTriggered: invoke("actionSave")
            }
            Action {
                text: qsTr("Save &As…")
                enabled: commandEnabled("actionSave_As")
                shortcut: shortcutSequence(commandMap["actionSave_As"])
                onTriggered: {
                    if (host && host.focusRestoration) {
                        host.focusRestoration.remember(window.activeFocusItem)
                    }
                    saveAsDialog.open()
                }
            }
            MenuSeparator {}
            Action {
                text: qsTr("E&xit")
                enabled: commandEnabled("actionQuit")
                shortcut: shortcutSequence(commandMap["actionQuit"])
                onTriggered: invoke("actionQuit")
            }
        }

        Menu {
            title: qsTr("&Navigate")
            Action {
                text: qsTr("Previous &Page")
                enabled: commandEnabled("actionGoToPreviousPage")
                shortcut: shortcutSequence(commandMap["actionGoToPreviousPage"])
                onTriggered: invoke("actionGoToPreviousPage")
            }
            Action {
                text: qsTr("Next &Page")
                enabled: commandEnabled("actionGoToNextPage")
                shortcut: shortcutSequence(commandMap["actionGoToNextPage"])
                onTriggered: invoke("actionGoToNextPage")
            }
            Action {
                text: qsTr("&First Page")
                enabled: commandEnabled("actionGoToDocumentStart")
                shortcut: shortcutSequence(commandMap["actionGoToDocumentStart"])
                onTriggered: invoke("actionGoToDocumentStart")
            }
            Action {
                text: qsTr("&Last Page")
                enabled: commandEnabled("actionGoToDocumentEnd")
                shortcut: shortcutSequence(commandMap["actionGoToDocumentEnd"])
                onTriggered: invoke("actionGoToDocumentEnd")
            }
        }

        Menu {
            title: qsTr("&View")
            Action {
                text: qsTr("&Find…")
                enabled: commandEnabled("actionFind")
                shortcut: shortcutSequence(commandMap["actionFind"])
                onTriggered: invoke("actionFind")
            }
            MenuSeparator {}
            Action {
                text: qsTr("Zoom &In")
                enabled: commandEnabled("actionZoom_In")
                shortcut: shortcutSequence(commandMap["actionZoom_In"])
                onTriggered: invoke("actionZoom_In")
            }
            Action {
                text: qsTr("Zoom &Out")
                enabled: commandEnabled("actionZoom_Out")
                shortcut: shortcutSequence(commandMap["actionZoom_Out"])
                onTriggered: invoke("actionZoom_Out")
            }
            Action {
                text: qsTr("&Fit Page")
                enabled: commandEnabled("actionFitPage")
                shortcut: shortcutSequence(commandMap["actionFitPage"])
                onTriggered: invoke("actionFitPage")
            }
            Action {
                text: qsTr("Fit &Width")
                enabled: commandEnabled("actionFitWidth")
                shortcut: shortcutSequence(commandMap["actionFitWidth"])
                onTriggered: invoke("actionFitWidth")
            }
            Action {
                text: qsTr("Fit &Height")
                enabled: commandEnabled("actionFitHeight")
                shortcut: shortcutSequence(commandMap["actionFitHeight"])
                onTriggered: invoke("actionFitHeight")
            }
            MenuSeparator {}
            Action {
                text: qsTr("Rotate &Left")
                enabled: commandEnabled("actionRotateLeft")
                shortcut: shortcutSequence(commandMap["actionRotateLeft"])
                onTriggered: invoke("actionRotateLeft")
            }
            Action {
                text: qsTr("Rotate &Right")
                enabled: commandEnabled("actionRotateRight")
                shortcut: shortcutSequence(commandMap["actionRotateRight"])
                onTriggered: invoke("actionRotateRight")
            }
            MenuSeparator {}
            Action {
                text: qsTr("Continuous Layout")
                enabled: commandEnabled("actionPageLayoutContinuous")
                onTriggered: invoke("actionPageLayoutContinuous")
            }
            Action {
                text: qsTr("Single Page Layout")
                enabled: commandEnabled("actionPageLayoutSinglePage")
                onTriggered: invoke("actionPageLayoutSinglePage")
            }
            Action {
                text: qsTr("Two-Column Layout")
                enabled: commandEnabled("actionPageLayoutTwoColumns")
                onTriggered: invoke("actionPageLayoutTwoColumns")
            }
            Action {
                text: qsTr("Two-Page Layout")
                enabled: commandEnabled("actionPageLayoutTwoPages")
                onTriggered: invoke("actionPageLayoutTwoPages")
            }
            Action {
                text: qsTr("Fullscreen")
                enabled: commandEnabled("actionFullscreenMode")
                shortcut: shortcutSequence(commandMap["actionFullscreenMode"])
                onTriggered: invoke("actionFullscreenMode")
            }
        }

        Menu {
            title: qsTr("&Document")
            Action {
                text: qsTr("&Properties")
                enabled: commandEnabled("actionProperties")
                onTriggered: invoke("actionProperties")
            }
        }
    }

    FileDialog {
        id: openDialog
        title: qsTr("Open PDF")
        nameFilters: [qsTr("PDF files (*.pdf)")]
        onAccepted: {
            if (host) {
                host.openFileUrl(selectedFile)
            }
            if (host && host.focusRestoration) host.focusRestoration.restore()
        }
        onRejected: if (host && host.focusRestoration) host.focusRestoration.restore()
    }

    FileDialog {
        id: saveAsDialog
        title: qsTr("Save PDF As")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("PDF files (*.pdf)")]
        onAccepted: {
            if (host) {
                host.saveAsFileUrl(selectedFile)
            }
            if (host && host.focusRestoration) host.focusRestoration.restore()
        }
        onRejected: if (host && host.focusRestoration) host.focusRestoration.restore()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Pane {
            id: stateBanner
            Layout.fillWidth: true
            visible: statusLabel.text.length > 0
            padding: 8

            Accessible.role: Accessible.StatusBar
            Accessible.name: qsTr("Document status")

            Label {
                id: statusLabel
                width: parent.width
                wrapMode: Text.WordWrap
                text: {
                    if (!host) {
                        return ""
                    }
                    switch (host.documentState) {
                    case "empty":
                        return qsTr("No document open. Use File → Open or drop a PDF path on the command line.")
                    case "opening":
                        return qsTr("Opening document…")
                    case "closing":
                        return qsTr("Closing document…")
                    case "error":
                        return host.typedError.length > 0
                                ? qsTr("Could not open the document (%1).").arg(host.typedError)
                                : qsTr("Could not open the document.")
                    case "ready":
                        if (host.cancelled) {
                            return qsTr("The last operation was cancelled.")
                        }
                        if (host.incomplete) {
                            return qsTr("Document loaded with incomplete support (some features were not honoured).")
                        }
                        if (host.unsupported) {
                            return qsTr("Document requires capabilities this build does not support.")
                        }
                        return ""
                    default:
                        return ""
                    }
                }
            }
        }

        Workspace {
            Layout.fillWidth: true
            Layout.fillHeight: true
            host: window.host
        }

        Pane {
            Layout.fillWidth: true
            padding: 6

            Accessible.role: Accessible.StatusBar
            Accessible.name: qsTr("Page status")

            RowLayout {
                anchors.fill: parent
                spacing: 12

                Label {
                    text: host && host.hasDocument
                          ? qsTr("Page %1 / %2").arg(host.currentPage + 1).arg(host.pageCount)
                          : qsTr("No document")
                    Accessible.name: qsTr("Current page")
                }

                Label {
                    visible: host && host.hasDocument
                    text: qsTr("Zoom %1%").arg(Math.round(host.zoom * 100))
                    Accessible.name: qsTr("Current zoom")
                }

                Label {
                    visible: host && host.hasDocument && host.rotationDegrees !== 0
                    text: qsTr("Rotation %1°").arg(host.rotationDegrees)
                    Accessible.name: qsTr("Current rotation")
                }

                Item { Layout.fillWidth: true }

                Label {
                    visible: host && host.documentState === "opening"
                    text: qsTr("Opening…")
                }
            }
        }
    }
}
