import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Loupe.Canvas

Item {
    id: root

    property var host: editorHost

    signal viewportGeometryChanged()

    LoupeCanvas {
        id: canvas
        anchors.fill: parent
        focus: true
        highContrast: root.host ? root.host.highContrast : false

        Accessible.name: qsTr("Document canvas")
        Accessible.description: canvas.accessibleDocumentSummary

        Component.onCompleted: {
            if (root.host) {
                root.host.attachCanvas(canvas)
            }
        }

        Component.onDestruction: {
            if (root.host) {
                root.host.detachCanvas()
            }
        }
    }

  onWidthChanged: publishGeometry()
  onHeightChanged: publishGeometry()

  function publishGeometry() {
      if (!host || width <= 0 || height <= 0) {
          return
      }

      const screen = Window.window ? Window.window.screen : null
      const pixelPerMM = screen ? screen.physicalDotsPerInchX / 25.4 : 96 / 25.4
      const devicePixelRatio = screen ? screen.devicePixelRatio : 1.0
      host.setViewportGeometry(pixelPerMM, devicePixelRatio, Math.round(width), Math.round(height))
      viewportGeometryChanged()
  }

  Component.onCompleted: publishGeometry()
}
