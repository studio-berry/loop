import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Pane {
    id: root

    property var host: editorHost
    property var findingsModel: host ? host.preflight.findingsModel : null

    padding: 8

    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Preflight findings")
    Accessible.description: qsTr("Revision-bound preflight findings. Select a finding to inspect evidence on the canvas.")

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: host ? qsTr("Preflight status: %1").arg(host.preflightStateName) : ""
            Accessible.name: qsTr("Preflight status")
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
                    if (host) {
                        host.selectFinding(model.findingId)
                    }
                }
            }

            Keys.onReturnPressed: {
                if (currentIndex >= 0 && host && findingsModel) {
                    const idx = findingsModel.index(currentIndex, 0)
                    host.selectFinding(findingsModel.data(idx, 257))
                }
            }
        }
    }
}
