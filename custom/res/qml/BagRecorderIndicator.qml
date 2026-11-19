import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls

// Toolbar control for the Jetson ROS 2 bag recorder: a labeled Start/Stop button plus a status label that
// opens the details drawer. The state shown is whatever the Jetson reports (see TarBagRecorder).
Item {
    id:             control
    width:          row.width + ScreenTools.defaultFontPixelWidth
    anchors.top:    parent.top
    anchors.bottom: parent.bottom

    property bool showIndicator: true

    readonly property string _state:    tarBagRecorder.state
    readonly property bool   _active:   _state === "starting" || _state === "recording" || _state === "stopping"
    readonly property bool   _xfer:     tarBagRecorder.transferState === "downloading"
                                        || tarBagRecorder.transferState === "preparing"
                                        || tarBagRecorder.transferState === "verifying"

    QGCPalette { id: qgcPal }

    function _stateColor() {
        if (_state === "recording")                         return qgcPal.colorRed
        if (_state === "failed" || tarBagRecorder.transferState === "failed") return qgcPal.colorOrange
        if (_state === "unknown")                           return qgcPal.colorGrey
        if (_state === "finalized")                         return qgcPal.colorGreen
        return qgcPal.text
    }

    Row {
        id:                 row
        anchors.top:        parent.top
        anchors.bottom:     parent.bottom
        spacing:            ScreenTools.defaultFontPixelWidth / 2

        QGCButton {
            id:                     actionButton
            anchors.verticalCenter: parent.verticalCenter
            pointSize:              ScreenTools.smallFontPointSize
            text:                   control._state === "recording" || control._state === "starting" ? qsTr("Stop recording bag")
                                    : control._state === "stopping"                                  ? qsTr("Stopping...")
                                    : control._state === "finalized" && !control._xfer
                                      && tarBagRecorder.transferState !== "completed"                ? qsTr("Download bag")
                                    : control._xfer                                                  ? qsTr("Downloading...")
                                    :                                                                  qsTr("Start recording bag")
            enabled:                !tarBagRecorder.busy && control._state !== "stopping" && !control._xfer
            primary:                control._state === "recording"

            onClicked: {
                if (!tarBagRecorder.configured) {
                    mainWindow.showIndicatorDrawer(bagPage, control)
                    return
                }
                if (control._state === "recording" || control._state === "starting") {
                    stopDialogFactory.open()
                } else if (control._state === "finalized" && tarBagRecorder.transferState !== "completed") {
                    tarBagRecorder.downloadBag()
                } else {
                    var problem = tarBagRecorder.settingsProblem()
                    if (problem !== "") {
                        mainWindow.showIndicatorDrawer(bagPage, control)
                    } else {
                        tarBagRecorder.startRecording()
                    }
                }
            }
        }

        QGCLabel {
            anchors.verticalCenter: parent.verticalCenter
            text:                   tarBagRecorder.statusText
            color:                  control._stateColor()
            font.bold:              control._state === "recording"
        }
    }

    MouseArea {
        // Clicking the status text (not the button) opens the details drawer.
        anchors.top:    parent.top
        anchors.bottom: parent.bottom
        anchors.right:  parent.right
        width:          row.width - actionButton.width
        onClicked:      mainWindow.showIndicatorDrawer(bagPage, control)
    }

    Component {
        id: bagPage

        BagRecorderIndicatorPage {
            onStopRequested:            { mainWindow.closeIndicatorDrawer(); stopDialogFactory.open() }
            onStopDownloadRequested:    { mainWindow.closeIndicatorDrawer(); stopDownloadDialogFactory.open() }
        }
    }

    QGCPopupDialogFactory {
        id:                 stopDialogFactory
        dialogComponent:    stopDialogComponent
    }

    QGCPopupDialogFactory {
        id:                 stopDownloadDialogFactory
        dialogComponent:    stopDownloadDialogComponent
    }

    Component {
        id: stopDownloadDialogComponent

        QGCPopupDialog {
            title:      qsTr("Stop recording and download?")
            buttons:    Dialog.Ok | Dialog.Cancel

            onAccepted: tarBagRecorder.stopAndDownload()

            QGCLabel {
                Layout.maximumWidth:    ScreenTools.defaultFontPixelWidth * 60
                wrapMode:               Text.WordWrap
                text:                   qsTr("Stops the recording, waits for the bag to finalize, then copies it to this PC.")
            }
        }
    }

    Component {
        id: stopDialogComponent

        QGCPopupDialog {
            title:      qsTr("Stop recording bag?")
            buttons:    Dialog.Ok | Dialog.Cancel

            onAccepted: tarBagRecorder.stopRecording()

            QGCLabel {
                Layout.maximumWidth:    ScreenTools.defaultFontPixelWidth * 60
                wrapMode:               Text.WordWrap
                text:                   qsTr("The Jetson will stop the ROS 2 recording gracefully and finalize the bag. You can then download it.")
            }
        }
    }
}
