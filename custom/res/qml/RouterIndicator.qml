import QtQuick
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls

// Toolbar indicator for the built-in MAVLink router. Always shown; colour reflects router state.
Item {
    id:             control
    width:          row.width
    anchors.top:    parent.top
    anchors.bottom: parent.bottom

    property bool showIndicator: true

    property bool _running:     tarRouter.running
    property int  _active:      tarRouter.activeEndpoints
    property bool _hasErrors:   tarRouter.configErrors.length > 0

    QGCPalette { id: qgcPal }

    Row {
        id:             row
        anchors.top:    parent.top
        anchors.bottom: parent.bottom
        spacing:        ScreenTools.defaultFontPixelWidth / 2

        QGCColoredImage {
            anchors.top:        parent.top
            anchors.bottom:     parent.bottom
            width:              height
            source:             "/InstrumentValueIcons/network.svg"
            fillMode:           Image.PreserveAspectFit
            sourceSize.height:  height
            color:              _hasErrors ? qgcPal.colorRed
                                           : (!_running ? qgcPal.colorGrey
                                                        : (_active > 0 ? qgcPal.colorGreen : qgcPal.text))
        }

        QGCLabel {
            anchors.verticalCenter: parent.verticalCenter
            text:                   _running ? _active : qsTr("Off")
            color:                  qgcPal.text
        }
    }

    MouseArea {
        anchors.fill:   parent
        onClicked:      mainWindow.showIndicatorDrawer(routerPage, control)
    }

    Component {
        id: routerPage

        RouterIndicatorPage {
            onConfigureRequested: {
                mainWindow.closeIndicatorDrawer()
                configDialogFactory.open()
            }
        }
    }

    // Lives on the toolbar item (not the drawer page) so the dialog survives the drawer closing.
    QGCPopupDialogFactory {
        id:                 configDialogFactory
        dialogComponent:    configDialogComponent
    }

    Component {
        id: configDialogComponent

        RouterConfigDialog { }
    }
}
