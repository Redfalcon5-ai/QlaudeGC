import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls

// Status, controls and settings for the Jetson ROS 2 bag recorder.
ToolIndicatorPage {
    showExpand: true

    // Confirmation dialogs live in BagRecorderIndicator so they survive the drawer closing.
    signal stopRequested()
    signal stopDownloadRequested()

    QGCPalette { id: qgcPal }

    function _formatBytes(b) {
        if (b > 1073741824) return (b / 1073741824).toFixed(1) + " GB"
        if (b > 1048576)    return (b / 1048576).toFixed(0) + " MB"
        return b + " B"
    }

    readonly property real _fieldWidth: ScreenTools.defaultFontPixelWidth * 36

    contentComponent: Component {
        ColumnLayout {
            spacing: ScreenTools.defaultFontPixelHeight / 2

            SettingsGroupLayout {
                heading: qsTr("ROS 2 bag recording (Jetson)")

                QGCLabel {
                    Layout.fillWidth:   true
                    text:               tarBagRecorder.statusText
                    font.bold:          true
                }

                LabelledLabel {
                    visible:    tarBagRecorder.activeSessionId !== ""
                    label:      qsTr("Session")
                    labelText:  tarBagRecorder.activeSessionId
                }
                LabelledLabel {
                    visible:    tarBagRecorder.remoteBagPath !== ""
                    label:      qsTr("Bag on Jetson")
                    labelText:  tarBagRecorder.remoteBagPath
                }
                LabelledLabel {
                    visible:    tarBagRecorder.startedAt !== ""
                    label:      qsTr("Started (UTC)")
                    labelText:  tarBagRecorder.startedAt
                }
                LabelledLabel {
                    visible:    tarBagRecorder.freeBytes > 0
                    label:      qsTr("Jetson free space")
                    labelText:  _formatBytes(tarBagRecorder.freeBytes)
                }

                QGCLabel {
                    Layout.fillWidth:       true
                    Layout.maximumWidth:    ScreenTools.defaultFontPixelWidth * 60
                    visible:                tarBagRecorder.lastError !== ""
                    text:                   tarBagRecorder.lastError
                    color:                  qgcPal.colorRed
                    wrapMode:               Text.WordWrap
                }

                Flow {
                    Layout.fillWidth:       true
                    Layout.maximumWidth:    ScreenTools.defaultFontPixelWidth * 60
                    spacing:                ScreenTools.defaultFontPixelWidth

                    QGCButton {
                        text:       qsTr("Start recording bag")
                        enabled:    !tarBagRecorder.busy && tarBagRecorder.settingsProblem() === ""
                                    && ["unknown", "idle", "finalized", "failed"].indexOf(tarBagRecorder.state) >= 0
                        onClicked:  tarBagRecorder.startRecording()
                    }
                    QGCButton {
                        text:       qsTr("Stop recording bag")
                        enabled:    tarBagRecorder.state === "recording" || tarBagRecorder.state === "starting"
                        onClicked:  stopRequested()
                    }
                    QGCButton {
                        text:       qsTr("Stop and download")
                        enabled:    tarBagRecorder.state === "recording" || tarBagRecorder.state === "starting"
                        onClicked:  stopDownloadRequested()
                    }
                    QGCButton {
                        text:       qsTr("Download bag")
                        enabled:    tarBagRecorder.state === "finalized" && tarBagRecorder.transferState !== "downloading"
                                    && tarBagRecorder.transferState !== "preparing" && tarBagRecorder.transferState !== "verifying"
                        onClicked:  tarBagRecorder.downloadBag()
                    }
                    QGCButton {
                        text:       qsTr("Refresh status")
                        enabled:    tarBagRecorder.configured
                        onClicked:  tarBagRecorder.refreshStatus()
                    }
                }
            }

            SettingsGroupLayout {
                heading:    qsTr("Download")
                visible:    tarBagRecorder.transferState !== "none" || tarBagRecorder.pendingDownload

                QGCLabel {
                    Layout.fillWidth:       true
                    Layout.maximumWidth:    ScreenTools.defaultFontPixelWidth * 60
                    text:                   tarBagRecorder.pendingDownload ? qsTr("Waiting for the bag to finalize, then downloading...")
                                                                           : tarBagRecorder.transferDetail
                    wrapMode:               Text.WrapAnywhere
                    color:                  tarBagRecorder.transferState === "failed" ? qgcPal.colorRed
                                          : tarBagRecorder.transferState === "completed" ? qgcPal.colorGreen : qgcPal.text
                }

                // Indeterminate on purpose: scp does not report trustworthy byte progress.
                ProgressBar {
                    Layout.fillWidth:   true
                    visible:            tarBagRecorder.transferState === "downloading" || tarBagRecorder.transferState === "verifying"
                                        || tarBagRecorder.transferState === "preparing"
                    indeterminate:      true
                }

                RowLayout {
                    QGCButton {
                        text:       qsTr("Cancel download")
                        visible:    tarBagRecorder.transferState === "downloading" || tarBagRecorder.pendingDownload
                        onClicked:  tarBagRecorder.cancelDownload()
                    }
                    QGCButton {
                        text:       qsTr("Open folder")
                        onClicked:  tarBagRecorder.openDownloadFolder()
                    }
                }
            }

        }
    }

    expandedComponent: Component {
        ColumnLayout {
            spacing: ScreenTools.defaultFontPixelHeight / 2

            SettingsGroupLayout {
                heading:            qsTr("Jetson connection (SSH)")
                headingDescription: qsTr("Key-based login only. See jetson/SETUP.md. Independent of the MAVLink connection.")

                RowLayout {
                    QGCLabel { text: qsTr("Host (Tailscale name/IP or LAN IP)"); Layout.fillWidth: true }
                    QGCTextField {
                        Layout.preferredWidth:  _fieldWidth
                        Component.onCompleted:  text = tarBagRecorder.host
                        onEditingFinished:      tarBagRecorder.host = text
                    }
                }
                RowLayout {
                    QGCLabel { text: qsTr("Jetson user"); Layout.fillWidth: true }
                    QGCTextField {
                        Layout.preferredWidth:  _fieldWidth
                        Component.onCompleted:  text = tarBagRecorder.sshUser
                        onEditingFinished:      tarBagRecorder.sshUser = text
                    }
                }
                RowLayout {
                    QGCLabel { text: qsTr("SSH private key file (blank = default/agent)"); Layout.fillWidth: true }
                    QGCTextField {
                        Layout.preferredWidth:  _fieldWidth
                        Component.onCompleted:  text = tarBagRecorder.sshIdentityFile
                        onEditingFinished:      tarBagRecorder.sshIdentityFile = text
                    }
                }
                RowLayout {
                    QGCLabel { text: qsTr("Remote command"); Layout.fillWidth: true }
                    QGCTextField {
                        Layout.preferredWidth:  _fieldWidth
                        Component.onCompleted:  text = tarBagRecorder.remoteCommand
                        onEditingFinished:      tarBagRecorder.remoteCommand = text
                    }
                }
                RowLayout {
                    QGCLabel { text: qsTr("Vehicle label (this Jetson)"); Layout.fillWidth: true }
                    QGCTextField {
                        Layout.preferredWidth:  _fieldWidth
                        Component.onCompleted:  text = tarBagRecorder.vehicleLabel
                        onEditingFinished:      tarBagRecorder.vehicleLabel = text
                    }
                }
                QGCLabel {
                    Layout.fillWidth:       true
                    Layout.maximumWidth:    ScreenTools.defaultFontPixelWidth * 60
                    visible:                tarBagRecorder.settingsProblem() !== ""
                    text:                   tarBagRecorder.settingsProblem()
                    color:                  qgcPal.colorOrange
                    wrapMode:               Text.WordWrap
                }
            }

            SettingsGroupLayout {
                heading:            qsTr("Recording")
                headingDescription: qsTr("Records all topics (-a) minus this regex. Blank = no -x option.")

                RowLayout {
                    QGCLabel { text: qsTr("Exclude regex (-x)"); Layout.fillWidth: true }
                    QGCTextField {
                        Layout.preferredWidth:  _fieldWidth
                        Component.onCompleted:  text = tarBagRecorder.excludeRegex
                        onEditingFinished:      tarBagRecorder.excludeRegex = text
                    }
                }
                QGCLabel {
                    Layout.fillWidth:       true
                    Layout.maximumWidth:    ScreenTools.defaultFontPixelWidth * 60
                    text: {
                        var dependOnRegex = tarBagRecorder.excludeRegex     // re-evaluate when the regex changes
                        return tarBagRecorder.commandPreview()
                    }
                    font.family:            ScreenTools.fixedFontFamily
                    font.pointSize:         ScreenTools.smallFontPointSize
                    wrapMode:               Text.WrapAnywhere
                }
            }

            SettingsGroupLayout {
                heading: qsTr("Download to this PC")

                RowLayout {
                    QGCLabel { text: qsTr("Folder"); Layout.fillWidth: true }
                    QGCTextField {
                        Layout.preferredWidth:  _fieldWidth
                        Component.onCompleted:  text = tarBagRecorder.localDownloadFolder
                        onEditingFinished:      tarBagRecorder.localDownloadFolder = text
                    }
                }
                QGCCheckBox {
                    text:       qsTr("Verify SHA-256 after download (slower for large bags)")
                    checked:    tarBagRecorder.verifyChecksums
                    onClicked:  tarBagRecorder.verifyChecksums = checked
                }
            }

            SettingsGroupLayout {
                heading:            qsTr("Diagnostics")
                headingDescription: qsTr("Runs qgc-bagctl doctor on the Jetson (ROS setup, systemd, linger, disk).")

                QGCButton {
                    text:       qsTr("Run Jetson check")
                    enabled:    tarBagRecorder.settingsProblem() === "" && !tarBagRecorder.busy
                    onClicked:  tarBagRecorder.runDoctor()
                }
                QGCLabel {
                    Layout.fillWidth:       true
                    Layout.maximumWidth:    ScreenTools.defaultFontPixelWidth * 70
                    visible:                tarBagRecorder.doctorReport !== ""
                    text:                   tarBagRecorder.doctorReport
                    font.family:            ScreenTools.fixedFontFamily
                    font.pointSize:         ScreenTools.smallFontPointSize
                    wrapMode:               Text.WrapAnywhere
                }
            }
        }
    }
}
