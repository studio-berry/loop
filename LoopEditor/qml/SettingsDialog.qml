import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shell settings (#239): the one settings surface. It renders the typed option
// rows of EditorHost's settings store and forwards every toggle and the
// confirmed factory reset through the store's typed invokables. It derives no
// value of its own and keeps no settings copy.
Dialog {
    id: root
    objectName: "settingsDialog"

    property var host: null

    readonly property var settings: host ? host.settings : null
    readonly property var optionRows: settings ? settings.options : []

    title: qsTr("Shell settings")
    modal: true
    width: Math.min(560, parent ? parent.width - 32 : 560)
    height: Math.min(640, parent ? parent.height - 32 : 640)
    anchors.centerIn: parent
    closePolicy: Popup.CloseOnEscape
    onClosed: {
        if (host) {
            host.cancelSettingsReset()
            if (host.focusRestoration)
                host.focusRestoration.restore()
        }
    }

    contentItem: ColumnLayout {
        Accessible.role: Accessible.Grouping
        Accessible.name: root.title
        spacing: 8

        Label {
            objectName: "settingsNotice"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: text.length > 0
            text: root.host ? root.host.settingsNotice : ""
            Accessible.name: qsTr("Settings notice")
            Accessible.description: text
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth

            ColumnLayout {
                width: parent.width
                spacing: 4

                Repeater {
                    model: root.optionRows

                    delegate: CheckBox {
                        id: optionBox
                        required property var modelData
                        objectName: "settingsOption_" + modelData.id
                        Layout.fillWidth: true
                        text: modelData.label
                        checked: modelData.enabled
                        onClicked: {
                            if (!root.settings || root.settings.setOptionEnabled(modelData.id, checked))
                                return
                            // Refused: re-assert the store's value so the row cannot
                            // show a change that did not persist.
                            checked = Qt.binding(function() { return optionBox.modelData.enabled })
                        }
                        Accessible.name: modelData.label
                        Accessible.description: modelData.description
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Render options change the interactive preview only; they never change the document.")
            Accessible.name: text
        }

        Button {
            objectName: "settingsResetButton"
            Layout.fillWidth: true
            visible: root.host && !root.host.settingsResetPending
            text: qsTr("Reset to factory settings…")
            Accessible.name: text
            onClicked: {
                if (root.host)
                    root.host.requestSettingsReset()
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: root.host && root.host.settingsResetPending

            Label {
                objectName: "settingsResetConfirmation"
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Reset every shell setting to its factory default? This cannot be undone.")
                Accessible.name: text
            }

            RowLayout {
                Layout.fillWidth: true

                Button {
                    objectName: "settingsResetConfirmButton"
                    Layout.fillWidth: true
                    text: qsTr("Reset now")
                    Accessible.name: text
                    onClicked: {
                        if (root.host)
                            root.host.confirmSettingsReset()
                    }
                }

                Button {
                    objectName: "settingsResetCancelButton"
                    Layout.fillWidth: true
                    text: qsTr("Cancel")
                    Accessible.name: text
                    onClicked: {
                        if (root.host)
                            root.host.cancelSettingsReset()
                    }
                }
            }
        }
    }
}
