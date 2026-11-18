import QtQuick
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls

ToolIndicatorPage {
    showExpand: true

    signal configureRequested()

    QGCPalette { id: qgcPal }

    function _formatBytes(bytes) {
        if (bytes > 1048576) {
            return (bytes / 1048576).toFixed(1) + " MB"
        }
        if (bytes > 1024) {
            return (bytes / 1024).toFixed(1) + " kB"
        }
        return bytes + " B"
    }

    contentComponent: Component {
        ColumnLayout {
            spacing: ScreenTools.defaultFontPixelHeight / 2

            SettingsGroupLayout {
                heading: qsTr("MAVLink Router")

                QGCCheckBoxSlider {
                    Layout.fillWidth:   true
                    text:               qsTr("Enabled")
                    checked:            tarRouter.enabled
                    onClicked:          tarRouter.enabled = checked
                }

                Repeater {
                    model: tarRouter.configErrors

                    QGCLabel {
                        Layout.fillWidth:   true
                        text:               modelData
                        color:              qgcPal.colorRed
                        wrapMode:           Text.WordWrap
                    }
                }

                QGCLabel {
                    visible:    tarRouter.running && tarRouter.endpoints.length === 0
                    text:       qsTr("Running - no traffic yet")
                }

                QGCButton {
                    text:       qsTr("Configure...")
                    onClicked:  configureRequested()
                }
            }

            Repeater {
                model: tarRouter.endpoints

                SettingsGroupLayout {
                    Layout.fillWidth:   true
                    heading:            modelData.name + "  (" + modelData.kind + ")"

                    LabelledLabel {
                        label:      qsTr("Endpoint")
                        labelText:  modelData.detail
                    }

                    LabelledLabel {
                        label:      qsTr("Status")
                        labelText:  modelData.connected ? qsTr("Up") : qsTr("Down")
                    }

                    LabelledLabel {
                        label:      qsTr("Heard from (sys ids)")
                        labelText:  modelData.systems.length > 0 ? modelData.systems : qsTr("none")
                    }

                    LabelledLabel {
                        label:      qsTr("Forwarding from (sys ids)")
                        labelText:  modelData.sentSystems.length > 0 ? modelData.sentSystems : qsTr("none yet")
                    }

                    LabelledLabel {
                        label:      qsTr("Rx / Tx msgs/s")
                        labelText:  modelData.rxRate + " / " + modelData.txRate
                    }

                    LabelledLabel {
                        label:      qsTr("Rx / Tx total")
                        labelText:  _formatBytes(modelData.rxBytes) + " / " + _formatBytes(modelData.txBytes)
                    }

                    LabelledLabel {
                        label:      qsTr("CRC errors")
                        labelText:  modelData.crcErrors
                        visible:    modelData.crcErrors > 0
                    }
                }
            }
        }
    }

    expandedComponent: Component {
        ColumnLayout {
            spacing: ScreenTools.defaultFontPixelHeight / 2

            SettingsGroupLayout {
                heading:            qsTr("Configuration")
                headingDescription: qsTr("mavlink-router main.conf format")

                QGCLabel {
                    Layout.fillWidth:   true
                    text:               tarRouter.configPath
                    wrapMode:           Text.WrapAnywhere
                }

                RowLayout {
                    QGCButton {
                        text:       qsTr("Edit File")
                        onClicked:  tarRouter.openConfigFile()
                    }

                    QGCButton {
                        text:       qsTr("Open Folder")
                        onClicked:  tarRouter.openConfigFolder()
                    }

                    QGCButton {
                        text:       qsTr("Reload")
                        enabled:    tarRouter.enabled
                        onClicked:  tarRouter.reload()
                    }
                }
            }

            SettingsGroupLayout {
                heading: qsTr("Router Log")

                QGCFlickable {
                    Layout.preferredWidth:  ScreenTools.defaultFontPixelWidth * 60
                    Layout.preferredHeight: ScreenTools.defaultFontPixelHeight * 12
                    contentHeight:          logText.height
                    clip:                   true

                    Component.onCompleted: contentY = Math.max(0, contentHeight - height)

                    QGCLabel {
                        id:             logText
                        width:          parent.width
                        text:           tarRouter.log.join("\n")
                        font.family:    ScreenTools.fixedFontFamily
                        font.pointSize: ScreenTools.smallFontPointSize
                        wrapMode:       Text.WrapAnywhere
                    }
                }
            }
        }
    }
}
