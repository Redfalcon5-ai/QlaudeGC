import QtQuick

import QGroundControl
import QGroundControl.Controls

// Toolbar button that shows/hides the Claude chat sidebar.
Item {
    id:             control
    width:          icon.width
    anchors.top:    parent.top
    anchors.bottom: parent.bottom

    property bool showIndicator: true

    QGCPalette { id: qgcPal }

    QGCColoredImage {
        id:                 icon
        anchors.top:        parent.top
        anchors.bottom:     parent.bottom
        width:              height
        source:             "/InstrumentValueIcons/chat-bubble-dots.svg"
        fillMode:           Image.PreserveAspectFit
        sourceSize.height:  height
        color:              tarAssistant.busy ? qgcPal.colorGreen
                                              : (tarAssistant.panelVisible ? qgcPal.buttonHighlight : qgcPal.text)

        SequentialAnimation on opacity {
            running:    tarAssistant.busy
            loops:      Animation.Infinite
            onRunningChanged: if (!running) icon.opacity = 1
            NumberAnimation { to: 0.4; duration: 600 }
            NumberAnimation { to: 1.0; duration: 600 }
        }
    }

    MouseArea {
        anchors.fill:   parent
        onClicked:      tarAssistant.panelVisible = !tarAssistant.panelVisible
    }
}
