import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ToolBar {
    id: root
    objectName: "shellToolBar"

    Accessible.role: Accessible.ToolBar
    Accessible.name: qsTr("Document toolbar")
    Accessible.description: qsTr("Commands for opening, editing, viewing, and moving through the document.")
    activeFocusOnTab: true

    property var host: editorHost
    property var window: null
    property var openDialog: null
    property var saveAsDialog: null
    property var commandMap: ({})

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
        if (commandId === "actionOpen" && root.openDialog) {
            if (host.focusRestoration && root.window) {
                host.focusRestoration.remember(root.window.activeFocusItem)
            }
            root.openDialog.open()
            return
        }
        if (commandId === "actionSave_As" && root.saveAsDialog) {
            if (host.focusRestoration && root.window) {
                host.focusRestoration.remember(root.window.activeFocusItem)
            }
            root.saveAsDialog.open()
            return
        }
        host.invokeCommand(commandId)
    }

    function shortcutText(commandId) {
        const entry = commandMap[commandId]
        return entry ? (entry.shortcutText || "") : ""
    }

    Component.onCompleted: rebuildCommands()

    Connections {
        target: host
        function onCommandEpochChanged() {
            root.rebuildCommands()
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 6

        ToolButton {
            objectName: "openDocumentButton"
            text: qsTr("Open")
            enabled: root.commandEnabled("actionOpen")
            activeFocusOnTab: true
            onClicked: root.invoke("actionOpen")
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Open document")
        }
        ToolButton {
            objectName: "saveDocumentButton"
            text: qsTr("Save")
            enabled: root.commandEnabled("actionSave")
            activeFocusOnTab: true
            onClicked: root.invoke("actionSave")
            Accessible.name: qsTr("Save document")
        }
        ToolButton {
            objectName: "saveAsDocumentButton"
            text: qsTr("Export")
            enabled: root.commandEnabled("actionSave_As")
            activeFocusOnTab: true
            onClicked: root.invoke("actionSave_As")
            Accessible.name: qsTr("Export document")
        }
        ToolSeparator {}
        ToolButton {
            objectName: "undoButton"
            text: qsTr("Undo")
            enabled: root.commandEnabled("actionUndo")
            activeFocusOnTab: true
            onClicked: root.invoke("actionUndo")
            Accessible.name: qsTr("Undo")
        }
        ToolButton {
            objectName: "redoButton"
            text: qsTr("Redo")
            enabled: root.commandEnabled("actionRedo")
            activeFocusOnTab: true
            onClicked: root.invoke("actionRedo")
            Accessible.name: qsTr("Redo")
        }
        ToolSeparator {}
        ToolButton {
            objectName: "selectToolButton"
            text: qsTr("Select")
            checkable: true
            checked: true
            activeFocusOnTab: true
            Accessible.name: qsTr("Select tool")
        }
        ToolButton {
            objectName: "handToolButton"
            text: qsTr("Hand")
            checkable: true
            activeFocusOnTab: true
            Accessible.name: qsTr("Hand tool")
        }
        ToolSeparator {}
        ToolButton {
            objectName: "zoomInButton"
            text: qsTr("Zoom In")
            enabled: root.commandEnabled("actionZoom_In")
            activeFocusOnTab: true
            onClicked: root.invoke("actionZoom_In")
            Accessible.name: qsTr("Zoom in")
        }
        ToolButton {
            objectName: "zoomOutButton"
            text: qsTr("Zoom Out")
            enabled: root.commandEnabled("actionZoom_Out")
            activeFocusOnTab: true
            onClicked: root.invoke("actionZoom_Out")
            Accessible.name: qsTr("Zoom out")
        }
        ToolSeparator {}
        ToolButton {
            objectName: "preflightWorkspaceButton"
            text: qsTr("Preflight")
            enabled: root.host && root.host.isWorkspaceEnabled(EditorHost.Preflight)
            activeFocusOnTab: true
            onClicked: if (root.host) root.host.setWorkspace(EditorHost.Preflight)
            Accessible.name: qsTr("Open preflight workspace")
        }
        ToolButton {
            objectName: "previewWorkspaceButton"
            text: qsTr("Preview")
            enabled: root.host && root.host.isWorkspaceEnabled(EditorHost.ProductionPreview)
            activeFocusOnTab: true
            onClicked: if (root.host) root.host.setWorkspace(EditorHost.ProductionPreview)
            Accessible.name: qsTr("Open production preview workspace")
        }

        Item { Layout.fillWidth: true }
    }
}
