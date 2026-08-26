import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Pane {
    id: root

    property var host: editorHost
    property var inspectorModel: host ? host.inspector : null

    padding: 8

    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Inspector")
    Accessible.description: qsTr("Contextual properties for the current selection.")

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: host && host.inspectorTitle.length > 0 ? host.inspectorTitle : qsTr("No selection")
            Accessible.name: qsTr("Inspector title")
        }

        ListView {
            id: inspectorView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            focus: true
            activeFocusOnTab: true
            model: root.inspectorModel

            Accessible.name: qsTr("Inspector properties")
            Accessible.description: qsTr("Read-only properties for the current selection.")

            delegate: RowLayout {
                width: inspectorView.width
                spacing: 8

                Label {
                    Layout.preferredWidth: Math.max(96, implicitWidth)
                    text: model.label
                    Accessible.name: model.label
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: model.value
                    Accessible.name: qsTr("%1 value").arg(model.label)
                }
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: host && host.previewSummary.length > 0
            text: host ? qsTr("Preview: %1").arg(host.previewSummary) : ""
            Accessible.name: qsTr("Production preview status")
        }
    }
}
