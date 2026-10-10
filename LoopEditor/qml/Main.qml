import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import QtQuick.Dialogs

import Loop.Quick

ApplicationWindow {
    id: window
    objectName: "loopMainWindow"

    Accessible.role: Accessible.Application
    Accessible.name: qsTr("Loop PDF editor")

    property var host: editorHost
    property var helpController: typeof operatorHelpController !== "undefined" ? operatorHelpController : null
    readonly property bool preferReducedMotion: host ? host.preferReducedMotion : false

    visible: true
    width: 1024
    height: 768
    minimumWidth: 800
    minimumHeight: 600

    OperatorHelpDialog {
        id: operatorHelp
        parent: Overlay.overlay
        helpController: window.helpController
        host: window.host
    }

    Shortcut {
        sequence: "F1"
        onActivated: {
            if (host && host.focusRestoration) host.focusRestoration.remember(window.activeFocusItem)
            operatorHelp.showWorkspace()
        }
    }

    Component.onCompleted: {
        if (helpController && helpController.welcomeNeeded)
            operatorHelp.open()
    }
    title: host && host.displayTitle.length > 0 ? host.displayTitle : qsTr("Loop")

    Dialog {
        id: passwordDialog
        objectName: "documentPasswordDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Open encrypted PDF")
        standardButtons: Dialog.Ok | Dialog.Cancel
        property var requestId: 0
        Accessible.role: Accessible.Dialog
        Accessible.name: title
        closePolicy: Popup.CloseOnEscape

        contentItem: ColumnLayout {
            Label {
                text: qsTr("Enter the document's user or owner password.")
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 340
            }
            TextField {
                id: documentPassword
                objectName: "documentPasswordField"
                Layout.fillWidth: true
                echoMode: TextInput.Password
                inputMethodHints: Qt.ImhHiddenText | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                Accessible.name: qsTr("Document password")
                Accessible.description: qsTr("Used only to open this document. The password is not saved.")
                onAccepted: passwordDialog.accept()
            }
        }
        onOpened: documentPassword.forceActiveFocus()
        onAccepted: {
            if (host) host.submitPassword(requestId, documentPassword.text)
            documentPassword.clear()
        }
        onRejected: {
            documentPassword.clear()
            if (host) host.cancelPassword(requestId)
        }
        onClosed: {
            documentPassword.clear()
            if (host && host.focusRestoration) host.focusRestoration.restore()
        }
    }

    Connections {
        target: host
        function onPasswordRequestChanged() {
            const requestId = host ? host.passwordRequestId : 0
            if (!requestId) {
                passwordDialog.close()
                documentPassword.clear()
                return
            }
            documentPassword.clear()
            passwordDialog.requestId = requestId
            if (host.focusRestoration) host.focusRestoration.remember(window.activeFocusItem)
            passwordDialog.open()
        }
    }

    Connections {
        target: host
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

    menuBar: ShellMenuBar {
        objectName: "shellMenuBar"
        host: window.host
        window: window
        openDialog: openDialog
        saveAsDialog: saveAsDialog
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

    Dialog {
        id: outputDialog
        objectName: "documentOutputDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: printing ? qsTr("Print to PDF") : qsTr("Export document")
        standardButtons: Dialog.Ok | Dialog.Cancel
        property bool printing: false
        property string revision: ""
        Accessible.name: title
        contentItem: ColumnLayout {
            Label {
                text: host ? host.previewFidelitySummary : ""
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 440
            }
            Label {
                text: outputDialog.printing ? qsTr("Print every page to a PDF file using the authoritative renderer.") :
                      qsTr("PDF copies the exact document bytes. PNG and TIFF export the current page.")
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            ComboBox {
                id: outputFormat
                visible: !outputDialog.printing
                model: [qsTr("PDF"), qsTr("PNG"), qsTr("TIFF")]
                Accessible.name: qsTr("Export format")
                Layout.fillWidth: true
            }
            SpinBox {
                id: outputResolution
                from: 72
                to: 600
                value: 300
                editable: true
                visible: outputDialog.printing || outputFormat.currentIndex !== 0
                Accessible.name: qsTr("Resolution in dots per inch")
            }
            CheckBox {
                id: outputAcknowledgment
                text: qsTr("I acknowledge fidelity warnings. This output is not print proof.")
                Accessible.name: text
                Layout.fillWidth: true
            }
        }
        onOpened: printing ? outputResolution.forceActiveFocus() : outputFormat.forceActiveFocus()
        onAccepted: outputFileDialog.open()
        onRejected: if (host && host.focusRestoration) host.focusRestoration.restore()
    }

    FileDialog {
        id: outputFileDialog
        title: outputDialog.title
        fileMode: FileDialog.SaveFile
        readonly property string format: outputDialog.printing ? "print-pdf" : ["pdf", "png", "tiff"][outputFormat.currentIndex]
        nameFilters: format === "png" ? [qsTr("PNG files (*.png)")] :
                     format === "tiff" ? [qsTr("TIFF files (*.tiff *.tif)")] : [qsTr("PDF files (*.pdf)")]
        onAccepted: {
            if (host) host.exportDocumentFileUrl(selectedFile, format, outputResolution.value, outputDialog.revision, outputAcknowledgment.checked)
            if (host && host.focusRestoration) host.focusRestoration.restore()
        }
        onRejected: if (host && host.focusRestoration) host.focusRestoration.restore()
    }

    Connections {
        target: host
        function onDocumentOutputRequested(printing) {
            if (host.focusRestoration) host.focusRestoration.remember(window.activeFocusItem)
            outputDialog.printing = printing
            outputDialog.revision = host.outputRevision()
            outputAcknowledgment.checked = false
            outputResolution.value = printing && !host.documentModel.canHighResolutionPrint ? 150 : 300
            outputDialog.open()
        }
        function onDocumentOutputFinished() { outputResultDialog.open() }
    }

    Dialog {
        id: outputResultDialog
        objectName: "documentOutputResultDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        title: qsTr("Output result")
        standardButtons: Dialog.Close
        Accessible.name: title
        contentItem: ColumnLayout {
            Label {
                Layout.preferredWidth: 500
                wrapMode: Text.WrapAnywhere
                text: {
                    const record = host ? host.outputRecord : ({})
                    if (record.error) return record.error
                    const artifact = record.artifact || ({})
                    const receipt = record.publicationReceipt || ({})
                    return qsTr("Saved artifact: %1\nRevision: %2\n%3\n%4\nThis output is not print proof.")
                        .arg(artifact.sha256 || "").arg(record.revision || "")
                        .arg(record.renderer || "")
                        .arg(receipt.plan_digest ? qsTr("Correction receipt plan: %1").arg(receipt.plan_digest) :
                             record.dpi ? qsTr("%1 dpi — %2").arg(record.dpi).arg(record.colorHandling) : qsTr("Original source bytes"))
                }
                Accessible.name: text
            }
        }
        onClosed: if (host && host.focusRestoration) host.focusRestoration.restore()
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

    FileDialog {
        id: preflightReportDialog
        title: qsTr("Export Preflight Report")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("JSON files (*.json)")]
        onAccepted: {
            if (host) {
                host.exportPreflightReportFileUrl(selectedFile)
            }
            if (host && host.focusRestoration) host.focusRestoration.restore()
        }
        onRejected: if (host && host.focusRestoration) host.focusRestoration.restore()
    }

    FileDialog {
        id: preflightProfileImportDialog
        title: qsTr("Import Preflight Profile")
        nameFilters: [qsTr("JSON files (*.json)")]
        onAccepted: {
            if (host) {
                host.importPreflightProfileFileUrl(selectedFile)
            }
            if (host && host.focusRestoration) host.focusRestoration.restore()
        }
        onRejected: if (host && host.focusRestoration) host.focusRestoration.restore()
    }

    FileDialog {
        id: preflightProfileExportDialog
        title: qsTr("Export Preflight Profile")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("JSON files (*.json)")]
        onAccepted: {
            if (host) {
                host.exportPreflightProfileFileUrl(selectedFile)
            }
            if (host && host.focusRestoration) host.focusRestoration.restore()
        }
        onRejected: if (host && host.focusRestoration) host.focusRestoration.restore()
    }

    FileDialog {
        id: preflightProfileSaveDialog
        title: qsTr("Save Preflight Profile Fork")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("JSON files (*.json)")]
        onAccepted: {
            if (host) {
                host.savePreflightProfileEdit(profileSaveVersionField.text, selectedFile)
            }
            if (host && host.focusRestoration) host.focusRestoration.restore()
        }
        onRejected: if (host && host.focusRestoration) host.focusRestoration.restore()
    }

    Connections {
        target: host
        function onPreflightReportExportRequested() {
            preflightReportDialog.open()
        }
        function onPreflightProfileImportRequested() {
            preflightProfileImportDialog.open()
        }
        function onPreflightProfileExportRequested() {
            preflightProfileExportDialog.open()
        }
        function onPreflightProfileSaveRequested() {
            profileSaveVersionField.text = host ? host.preflightProfileDraftVersion : ""
            preflightProfileSaveDialog.open()
        }
    }

    TextField {
        id: profileSaveVersionField
        visible: false
        Accessible.ignored: true
    }

    Shortcut {
        sequence: "Alt+Up"
        enabled: host && host.hasDocument && host.preflightStateName === "findings"
        onActivated: host.selectPreviousFinding()
    }

    Shortcut {
        sequence: "Alt+Down"
        enabled: host && host.hasDocument && host.preflightStateName === "findings"
        onActivated: host.selectNextFinding()
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
            window: window
            openDialog: openDialog
            saveAsDialog: saveAsDialog
            onHelpRequested: function(checkId, findingType) {
                if (window.host && window.host.focusRestoration) window.host.focusRestoration.remember(window.activeFocusItem)
                if (checkId.length > 0)
                    operatorHelp.showFinding(checkId, findingType)
                else
                    operatorHelp.showWorkspace()
            }
        }

        Pane {
            Layout.fillWidth: true
            padding: 6

            Accessible.role: Accessible.StatusBar
            Accessible.name: qsTr("Shell status")

            RowLayout {
                anchors.fill: parent
                spacing: 16

                Label {
                    text: host
                          ? qsTr("Document: %1").arg(host.documentShellStatus)
                          : qsTr("Document: NO_DOCUMENT")
                    Accessible.name: qsTr("Document status")
                }

                Label {
                    text: host
                          ? qsTr("Production: %1").arg(host.productionStateName)
                          : qsTr("Production: NOT_READY")
                    Accessible.name: qsTr("Production status")
                }

                Label {
                    text: host
                          ? qsTr("Preflight: %1").arg(host.preflightStateName)
                          : qsTr("Preflight: not-checked")
                    Accessible.name: qsTr("Preflight status")
                }

                Item { Layout.fillWidth: true }

                Label {
                    visible: host && host.hasDocument
                    text: qsTr("Page %1 / %2").arg(host.currentPage + 1).arg(host.pageCount)
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

                Label {
                    visible: host && host.documentState === "opening"
                    text: qsTr("Opening…")
                }
            }
        }
    }
}
