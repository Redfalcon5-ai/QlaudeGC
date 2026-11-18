import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls

// Graphical editor for the router's main.conf. Edits a JS copy of the config; Save validates, writes the
// file and restarts the router (TarRouter.saveConfigFromEdit).
QGCPopupDialog {
    id:         root
    title:      qsTr("MAVLink Router Configuration")
    buttons:    Dialog.Save | Dialog.Cancel

    ListModel { id: epModel }
    property string _tcpPort:       ""
    property string _error:         ""
    property string _status:        ""
    property var    _serialPorts:   tarRouter.serialPorts()

    readonly property var _typeKeys:    [ "udpClient", "udpServer", "tcpClient", "uart" ]
    readonly property var _typeNames:   [ qsTr("UDP - send to (Normal)"), qsTr("UDP - listen (Server)"),
                                          qsTr("TCP - connect to"), qsTr("Serial port") ]
    readonly property var _bauds:       [ 9600, 19200, 38400, 57600, 115200, 230400, 460800, 500000, 921600, 1500000 ]
    readonly property real _fieldWidth: ScreenTools.defaultFontPixelWidth * 22

    function _newEndpoint(type, name, address, port) {
        return { type: type, name: name, address: address, port: port, device: "", baud: 57600,
                 flowControl: false, retryTimeout: 5, allowMsgIdOut: "", blockMsgIdOut: "",
                 allowSrcSysOut: "", blockSrcSysOut: "" }
    }

    function _nameUsed(name) {
        for (var i = 0; i < epModel.count; i++) {
            if (epModel.get(i).name.toLowerCase() === name.toLowerCase()) {
                return true
            }
        }
        return false
    }

    // Adds a preset with a unique name.
    function _add(e) {
        var name = e.name
        var n = 2
        while (_nameUsed(name)) {
            name = e.name + n
            n++
        }
        e.name = name
        _append(e)
    }

    // ListModel roles are typed from the first row, so always append complete, typed rows (delegates
    // read their initial field text when the row is created).
    function _append(e) {
        epModel.append({ type: String(e.type), name: String(e.name), address: String(e.address), port: Number(e.port),
                         device: String(e.device), baud: Number(e.baud), flowControl: Boolean(e.flowControl),
                         retryTimeout: Number(e.retryTimeout), allowMsgIdOut: String(e.allowMsgIdOut),
                         blockMsgIdOut: String(e.blockMsgIdOut), allowSrcSysOut: String(e.allowSrcSysOut || ""),
                         blockSrcSysOut: String(e.blockSrcSysOut || "") })
    }

    function _collect() {
        var list = []
        for (var i = 0; i < epModel.count; i++) {
            var e = epModel.get(i)
            list.push({ type: e.type, name: e.name, address: e.address, port: e.port, device: e.device,
                        baud: e.baud, flowControl: e.flowControl, retryTimeout: e.retryTimeout,
                        allowMsgIdOut: e.allowMsgIdOut, blockMsgIdOut: e.blockMsgIdOut,
                        allowSrcSysOut: e.allowSrcSysOut, blockSrcSysOut: e.blockSrcSysOut })
        }
        return list
    }

    readonly property int _perVehicleBasePort: 14540

    // System ids of the vehicles QGC currently has, sorted.
    function _connectedVehicleIds() {
        var ids = []
        var vehicles = QGroundControl.multiVehicleManager.vehicles
        for (var i = 0; i < vehicles.count; i++) {
            ids.push(vehicles.get(i).id)
        }
        return ids.sort(function(a, b) { return a - b })
    }

    // One MAVSDK endpoint per connected vehicle: port 14540 + system id, filtered to that vehicle only.
    function _addMavsdkPerVehicle() {
        var ids = _connectedVehicleIds()
        if (ids.length === 0) {
            _error = qsTr("No vehicles are connected. Connect them first, or add a MAVSDK endpoint and fill in 'Only these vehicles' by hand.")
            return
        }
        var added = []
        for (var i = 0; i < ids.length; i++) {
            var name = "mavsdk_v" + ids[i]
            if (_nameUsed(name)) {
                continue
            }
            var e = _newEndpoint("udpClient", name, "127.0.0.1", _perVehicleBasePort + ids[i])
            e.allowSrcSysOut = String(ids[i])
            _append(e)
            added.push(qsTr("vehicle %1 -> udp://:%2").arg(ids[i]).arg(_perVehicleBasePort + ids[i]))
        }
        _error = ""
        _status = added.length > 0 ? qsTr("Added (press Save to apply): ") + added.join(", ")
                                   : qsTr("Every connected vehicle already has a mavsdk_v<id> endpoint")
    }

    // (Re)fills the editor from the active main.conf.
    function _loadActive() {
        var cfg = tarRouter.loadConfigForEdit()
        epModel.clear()
        for (var i = 0; i < cfg.endpoints.length; i++) {
            _append(cfg.endpoints[i])
        }
        _tcpPort = String(cfg.tcpServerPort)
        tcpPortField.text = _tcpPort
        _error = cfg.loadErrors.length > 0
                 ? qsTr("Current file has errors (fix and save):\n") + cfg.loadErrors.join("\n")
                 : ""
    }

    Component.onCompleted: _loadActive()

    onAccepted: {
        var result = tarRouter.saveConfigFromEdit({ tcpServerPort: parseInt(_tcpPort), endpoints: _collect() })
        if (result !== "") {
            _error = result
            preventClose = true
        }
    }

    ColumnLayout {
        spacing: ScreenTools.defaultFontPixelHeight / 2

        QGCLabel {
            Layout.fillWidth:       true
            Layout.maximumWidth:    ScreenTools.defaultFontPixelWidth * 80
            wrapMode:               Text.WordWrap
            text:                   qsTr("QGroundControl's own links (e.g. the drone on UDP 14550) are bridged into the router automatically. Add the other programs and devices that should share the vehicle connection.")
        }

        SettingsGroupLayout {
            Layout.fillWidth:   true
            heading:            qsTr("Config file")
            headingDescription: qsTr("mavlink-router main.conf format - existing mavlink-router configs can be imported")

            RowLayout {
                spacing: ScreenTools.defaultFontPixelWidth

                QGCButton {
                    text:       qsTr("Import...")
                    onClicked:  importDialog.openForLoad()
                }

                QGCButton {
                    text:       qsTr("Export active config...")
                    onClicked:  exportDialog.openForSave()
                }
            }

            QGCLabel {
                Layout.fillWidth:       true
                Layout.maximumWidth:    ScreenTools.defaultFontPixelWidth * 80
                visible:                root._status !== ""
                text:                   root._status
                color:                  QGroundControl.globalPalette.colorGreen
                wrapMode:               Text.WrapAnywhere
            }
        }

        SettingsGroupLayout {
            Layout.fillWidth:   true
            heading:            qsTr("General")

            RowLayout {
                QGCLabel { text: qsTr("TCP server port (0 = off)"); Layout.fillWidth: true }
                QGCTextField {
                    id:                     tcpPortField
                    Layout.preferredWidth:  root._fieldWidth
                    numericValuesOnly:      true
                    onTextChanged:          root._tcpPort = text
                }
            }
        }

        Repeater {
            model: epModel

            SettingsGroupLayout {
                id:                 epGroup
                Layout.fillWidth:   true
                heading:            qsTr("Endpoint: ") + model.name

                required property int   index
                required property var   model
                property string type:   model.type

                function set(role, value) { epModel.setProperty(index, role, value) }
                property bool   isUdp:  type === "udpClient" || type === "udpServer"
                property bool   isTcp:  type === "tcpClient"
                property bool   isUart: type === "uart"

                RowLayout {
                    QGCLabel { text: qsTr("Name"); Layout.fillWidth: true }
                    QGCTextField {
                        Layout.preferredWidth:  root._fieldWidth
                        Component.onCompleted: text = epGroup.model.name
                        onTextChanged:          epGroup.set("name", text)
                    }
                }

                RowLayout {
                    QGCLabel { text: qsTr("Type"); Layout.fillWidth: true }
                    QGCComboBox {
                        Layout.preferredWidth:  root._fieldWidth
                        model:                  root._typeNames
                        currentIndex:           Math.max(0, root._typeKeys.indexOf(epGroup.type))
                        onActivated: (comboIndex) => {
                            epGroup.set("type", root._typeKeys[comboIndex])
                            if (epGroup.type === "udpServer" && epGroup.model.address === "") {
                                addressField.text = "0.0.0.0"
                            }
                        }
                    }
                }

                RowLayout {
                    visible: epGroup.isUdp || epGroup.isTcp
                    QGCLabel {
                        Layout.fillWidth:   true
                        text:               epGroup.type === "udpServer" ? qsTr("Listen address") : qsTr("Address")
                    }
                    QGCTextField {
                        Layout.preferredWidth:  root._fieldWidth
                        id:                     addressField
                        Component.onCompleted: text = epGroup.model.address
                        placeholderText:        "127.0.0.1"
                        onTextChanged:          epGroup.set("address", text)
                    }
                }

                RowLayout {
                    visible: epGroup.isUdp || epGroup.isTcp
                    QGCLabel { text: qsTr("Port"); Layout.fillWidth: true }
                    QGCTextField {
                        Layout.preferredWidth:  root._fieldWidth
                        Component.onCompleted: text = epGroup.model.port > 0 ? String(epGroup.model.port) : ""
                        numericValuesOnly:      true
                        onTextChanged:          epGroup.set("port", parseInt(text) || 0)
                    }
                }

                RowLayout {
                    visible: epGroup.isTcp
                    QGCLabel { text: qsTr("Reconnect every (s)"); Layout.fillWidth: true }
                    QGCTextField {
                        Layout.preferredWidth:  root._fieldWidth
                        Component.onCompleted: text = String(epGroup.model.retryTimeout)
                        numericValuesOnly:      true
                        onTextChanged:          epGroup.set("retryTimeout", parseInt(text) || 5)
                    }
                }

                RowLayout {
                    visible: epGroup.isUart
                    QGCLabel {
                        Layout.fillWidth:   true
                        text:               root._serialPorts.length > 0
                                            ? qsTr("Device (found: %1)").arg(root._serialPorts.join(", "))
                                            : qsTr("Device (no ports found)")
                    }
                    QGCTextField {
                        Layout.preferredWidth:  root._fieldWidth
                        Component.onCompleted: text = epGroup.model.device
                        placeholderText:        "COM5"
                        onTextChanged:          epGroup.set("device", text)
                    }
                }

                RowLayout {
                    visible: epGroup.isUart
                    QGCLabel { text: qsTr("Baud"); Layout.fillWidth: true }
                    QGCComboBox {
                        Layout.preferredWidth:  root._fieldWidth
                        model:                  root._bauds
                        currentIndex:           Math.max(0, root._bauds.indexOf(epGroup.model.baud))
                        onActivated: (comboIndex) => epGroup.set("baud", root._bauds[comboIndex])
                    }
                }

                QGCCheckBox {
                    visible:    epGroup.isUart
                    text:       qsTr("Hardware flow control (RTS/CTS)")
                    checked:    epGroup.model.flowControl
                    onClicked:  epGroup.set("flowControl", checked)
                }

                RowLayout {
                    QGCLabel { text: qsTr("Only these vehicles (system ids, blank = all)"); Layout.fillWidth: true }
                    QGCTextField {
                        Layout.preferredWidth:  root._fieldWidth
                        Component.onCompleted: text = epGroup.model.allowSrcSysOut
                        placeholderText:        "e.g. 1, 2"
                        onTextChanged:          epGroup.set("allowSrcSysOut", text)
                    }
                }

                RowLayout {
                    QGCLabel { text: qsTr("Never these vehicles (system ids)"); Layout.fillWidth: true }
                    QGCTextField {
                        Layout.preferredWidth:  root._fieldWidth
                        Component.onCompleted: text = epGroup.model.blockSrcSysOut
                        placeholderText:        "e.g. 3"
                        onTextChanged:          epGroup.set("blockSrcSysOut", text)
                    }
                }

                RowLayout {
                    QGCLabel { text: qsTr("Only send msg ids (blank = all)"); Layout.fillWidth: true }
                    QGCTextField {
                        Layout.preferredWidth:  root._fieldWidth
                        Component.onCompleted: text = epGroup.model.allowMsgIdOut
                        placeholderText:        "e.g. 0, 30, 33"
                        onTextChanged:          epGroup.set("allowMsgIdOut", text)
                    }
                }

                RowLayout {
                    QGCLabel { text: qsTr("Never send msg ids"); Layout.fillWidth: true }
                    QGCTextField {
                        Layout.preferredWidth:  root._fieldWidth
                        Component.onCompleted: text = epGroup.model.blockMsgIdOut
                        placeholderText:        "e.g. 105"
                        onTextChanged:          epGroup.set("blockMsgIdOut", text)
                    }
                }

                QGCButton {
                    text:       qsTr("Remove endpoint")
                    onClicked:  epModel.remove(epGroup.index)
                }
            }
        }

        SettingsGroupLayout {
            Layout.fillWidth:   true
            heading:            qsTr("Add endpoint")

            Flow {
                Layout.fillWidth:   true
                Layout.maximumWidth: ScreenTools.defaultFontPixelWidth * 80
                spacing:            ScreenTools.defaultFontPixelWidth

                QGCButton {
                    text:       qsTr("MAVSDK, all vehicles (UDP 14540)")
                    onClicked:  root._add(root._newEndpoint("udpClient", "mavsdk", "127.0.0.1", 14540))
                }
                QGCButton {
                    text:       qsTr("MAVSDK per vehicle (14540 + id)")
                    onClicked:  root._addMavsdkPerVehicle()
                }
                QGCButton {
                    text:       qsTr("UDP send to...")
                    onClicked:  root._add(root._newEndpoint("udpClient", "udp", "127.0.0.1", 14551))
                }
                QGCButton {
                    text:       qsTr("UDP listen on...")
                    onClicked:  root._add(root._newEndpoint("udpServer", "udpserver", "0.0.0.0", 14560))
                }
                QGCButton {
                    text:       qsTr("TCP connect to...")
                    onClicked:  root._add(root._newEndpoint("tcpClient", "tcp", "127.0.0.1", 5761))
                }
                QGCButton {
                    text:       qsTr("Serial port")
                    onClicked:  root._add(root._newEndpoint("uart", "serial", "", 0))
                }
            }
        }

        QGCLabel {
            Layout.fillWidth:       true
            Layout.maximumWidth:    ScreenTools.defaultFontPixelWidth * 80
            visible:                root._error !== ""
            text:                   root._error
            color:                  QGroundControl.globalPalette.colorRed
            wrapMode:               Text.WordWrap
        }
    }

    QGCFileDialog {
        id:             importDialog
        title:          qsTr("Import router config")
        folder:         tarRouter.exportFolder
        nameFilters:    [ qsTr("Router config (*.conf)"), qsTr("All Files (*)") ]

        onAcceptedForLoad: (file) => {
            var result = tarRouter.importConfig(file)
            if (result === "") {
                root._loadActive()
                root._status = qsTr("Imported and applied: %1").arg(file)
            } else {
                root._status = ""
                root._error = result
            }
            close()
        }
    }

    QGCFileDialog {
        id:             exportDialog
        title:          qsTr("Export active router config")
        folder:         tarRouter.exportFolder
        nameFilters:    [ qsTr("Router config (*.conf)") ]
        defaultSuffix:  "conf"

        onAcceptedForSave: (file) => {
            var result = tarRouter.exportConfig(file)
            if (result === "") {
                root._error = ""
                root._status = qsTr("Saved active config to %1").arg(file)
            } else {
                root._status = ""
                root._error = result
            }
            close()
        }
    }
}
