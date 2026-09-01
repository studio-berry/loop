import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

ApplicationWindow {
    id: smokeWindow

    visible: true
    width: 640
    height: 360
    title: "Loop Quick Shell Smoke"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 16

        Label {
            Layout.fillWidth: true
            text: "Qt Quick shell backend smoke"
            font.pixelSize: 24
        }

        Label {
            Layout.fillWidth: true
            text: "The harness exercises the imports and scene graph used by the staged shell."
            wrapMode: Text.WordWrap
        }

        Button {
            Layout.alignment: Qt.AlignLeft
            text: "Focusable control"
            focus: true
        }
    }
}
