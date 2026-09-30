import QtQuick
import QtQuick.Controls

Rectangle {
    objectName: "canvasBenchmarkRoot"
    width: 320
    height: 240
    color: "#264d73"

    // This control is transparent so the S21 color surface stays unchanged.
    // It gives the S22 qualification probe a real Quick focus and accessibility
    // object without turning the benchmark into product QML.
    property string bridgeAccessibleName: bridgeProbeButton.Accessible.name
    property string bridgeAccessibleDescription: bridgeProbeButton.Accessible.description
    property int bridgeAccessibleRole: bridgeProbeButton.Accessible.role
    property bool bridgeProbeActiveFocus: bridgeProbeButton.activeFocus

    Button {
        id: bridgeProbeButton
        objectName: "bridgeProbeButton"
        anchors.top: parent.top
        anchors.left: parent.left
        width: 1
        height: 1
        opacity: 0
        focus: true
        text: qsTr("Quick action")

        Accessible.name: qsTr("Quick action")
        Accessible.description: qsTr("Activate the Quick action.")
        Accessible.role: Accessible.Button
    }

    Rectangle {
        anchors.centerIn: parent
        width: 96
        height: 64
        color: "#d7e8f7"
        border.color: "#102a43"
        border.width: 2
    }
}
