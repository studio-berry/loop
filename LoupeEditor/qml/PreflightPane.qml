import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Pane {
    id: root

    property var host: editorHost
    property var findingsModel: host ? host.preflight.findingsModel : null
    property string profileError: ""

    padding: 8

    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Preflight findings")
    Accessible.description: qsTr("Revision-bound preflight findings. Select a finding to inspect evidence on the canvas.")

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                Layout.fillWidth: true
                // Fixed height, and the text is the only thing that changes.
                // A status row that grew a line when a run started would move
                // the findings list under the pointer at exactly the moment the
                // user is reading it.
                wrapMode: Text.NoWrap
                elide: Text.ElideRight
                text: host ? qsTr("Preflight status: %1").arg(host.preflightStateName) : ""
                Accessible.name: qsTr("Preflight status")
            }

            BusyIndicator {
                running: host ? host.preflightRunning : false
                visible: running
                implicitWidth: 20
                implicitHeight: 20
            }

            Button {
                id: runButton
                text: host && host.preflightRunning ? qsTr("Cancel") : qsTr("Run...")
                enabled: host ? host.hasDocument : false
                Accessible.name: text
                Accessible.description: host && host.preflightRunning
                    ? qsTr("Cancel the running preflight check.")
                    : qsTr("Choose a preflight profile and check this document against it.")
                onClicked: {
                    if (!host) {
                        return
                    }

                    if (host.preflightRunning) {
                        host.cancelPreflight()
                        return
                    }

                    if (host.focusRestoration) {
                        host.focusRestoration.remember(runButton)
                    }

                    profileDialog.open()
                }
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: text.length > 0
            color: palette.placeholderText
            // Only ever set by a failed start, and cleared by the next one.
            // A profile that could not be read is not a preflight result, so it
            // must not be reported through the status vocabulary.
            text: root.profileError
            Accessible.name: qsTr("Preflight profile error")
        }

        FileDialog {
            id: profileDialog
            title: qsTr("Choose preflight profile")
            nameFilters: [qsTr("Preflight profiles (*.json)")]
            onAccepted: {
                root.profileError = ""

                if (host && !host.runPreflight(selectedFile)) {
                    // runPreflight returns false without changing state: the
                    // profile could not be read, or its committed digest did
                    // not match and was not repaired.
                    root.profileError = qsTr("That profile could not be loaded or failed digest validation.")
                }

                if (host && host.focusRestoration) host.focusRestoration.restore()
            }
            onRejected: if (host && host.focusRestoration) host.focusRestoration.restore()
        }

        ListView {
            id: findingsView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            focus: true
            activeFocusOnTab: true
            model: root.findingsModel
            currentIndex: -1

            Accessible.name: qsTr("Preflight findings list")
            Accessible.description: qsTr("Use arrow keys to move between findings and Enter to navigate to evidence.")

            delegate: ItemDelegate {
                width: findingsView.width
                text: model.message
                highlighted: model.selected
                Accessible.name: model.message
                Accessible.description: qsTr("Severity %1 on page %2").arg(model.severity).arg(model.page)
                onClicked: {
                    findingsView.currentIndex = index
                    if (host) {
                        host.selectFinding(model.findingId)
                    }
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
