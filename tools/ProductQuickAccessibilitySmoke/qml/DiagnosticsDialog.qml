import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Diagnostics collection (#239): the ADR-006 bundle surface. It shows the
// privacy boundary, takes the operator's parent folder and forwards the
// collection to EditorHost; the result label renders the host's own projection
// of the collector's outcome.
Dialog {
    id: root
    objectName: "diagnosticsDialog"

    property var host: null
    property var folderDialog: null

    readonly property var bundle: host ? host.diagnosticsBundle : ({})
    readonly property string chosenFolder: folderDialog && folderDialog.selectedFolder ? folderDialog.selectedFolder.toString() : ""

    title: qsTr("Collect diagnostics")
    modal: true
    width: Math.min(560, parent ? parent.width - 32 : 560)
    height: Math.min(520, parent ? parent.height - 32 : 520)
    anchors.centerIn: parent
    closePolicy: Popup.CloseOnEscape
    onClosed: if (host && host.focusRestoration) host.focusRestoration.restore()

    contentItem: ColumnLayout {
        Accessible.role: Accessible.Grouping
        Accessible.name: root.title
        spacing: 8

        Label {
            objectName: "diagnosticsPrivacyNotice"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("The bundle contains a versioned manifest, build and runtime metadata, the scrubbed application logs and a README. It never contains the PDF, document content, extracted text, your settings, recent files, environment variables or crash dumps. Inspect the folder before sharing it.")
            Accessible.name: text
        }

        RowLayout {
            Layout.fillWidth: true

            Button {
                objectName: "diagnosticsChooseFolderButton"
                text: qsTr("Choose folder…")
                Accessible.name: text
                onClicked: if (root.folderDialog) root.folderDialog.open()
            }

            Label {
                objectName: "diagnosticsChosenFolder"
                Layout.fillWidth: true
                elide: Text.ElideMiddle
                text: root.chosenFolder
                Accessible.name: qsTr("Bundle parent folder")
                Accessible.description: text
            }
        }

        Button {
            objectName: "diagnosticsCollectButton"
            Layout.fillWidth: true
            enabled: root.chosenFolder.length > 0
            text: qsTr("Collect bundle")
            Accessible.name: text
            onClicked: if (root.host && root.folderDialog) root.host.collectDiagnosticsFileUrl(root.folderDialog.selectedFolder)
        }

        Label {
            objectName: "diagnosticsResult"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: text.length > 0
            text: {
                if (!root.bundle.status || root.bundle.status === "idle")
                    return ""
                if (root.bundle.status === "failed")
                    return qsTr("The bundle could not be written: %1").arg(root.bundle.error)
                return qsTr("Bundle written to %1 (%2 files). Review it before sharing.").arg(root.bundle.directory).arg(root.bundle.fileCount)
            }
            Accessible.name: text
        }
    }
}
