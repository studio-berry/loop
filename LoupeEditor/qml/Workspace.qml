import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Loupe.Quick

Item {
    id: root

    property var host: editorHost

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Pane {
            Layout.preferredWidth: 48
            Layout.fillHeight: true
            padding: 8

            ColumnLayout {
                anchors.fill: parent
                spacing: 8

                ToolButton {
                    Layout.fillWidth: true
                    text: qsTr("Document")
                    enabled: false
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Workspace rail stub (P4-S8+)")
                }

                ToolButton {
                    Layout.fillWidth: true
                    text: qsTr("Preflight")
                    enabled: false
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Preflight workspace is not part of the P4-S7 slice")
                }

                Item { Layout.fillHeight: true }
            }
        }

        CanvasPane {
            id: canvasPane
            Layout.fillWidth: true
            Layout.fillHeight: true
            host: root.host
        }
    }
}
