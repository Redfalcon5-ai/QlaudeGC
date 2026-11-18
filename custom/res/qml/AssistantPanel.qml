import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls

// Claude chat sidebar. Created once by TarPlugin::createRootWindow on top of the main window content.
Rectangle {
    id:             panel
    anchors.top:    parent.top
    anchors.bottom: parent.bottom
    anchors.right:  parent.right
    anchors.topMargin: ScreenTools.toolbarHeight
    width:          Math.max(_minWidth, Math.min(tarAssistant.panelWidth, parent.width * 0.7))
    visible:        tarAssistant.panelVisible
    z:              1000
    color:          qgcPal.window

    readonly property real _margin:     ScreenTools.defaultFontPixelWidth
    readonly property real _minWidth:   ScreenTools.defaultFontPixelWidth * 32
    property bool _showSettings: false
    property bool _showContext:  false

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    // Swallow mouse/wheel events so the map underneath doesn't pan
    MouseArea {
        anchors.fill:   parent
        acceptedButtons: Qt.AllButtons
        onWheel:        (wheel) => wheel.accepted = true
    }

    // Left border + resize handle
    Rectangle {
        anchors.left:   parent.left
        anchors.top:    parent.top
        anchors.bottom: parent.bottom
        width:          1
        color:          qgcPal.groupBorder
    }
    MouseArea {
        anchors.left:   parent.left
        anchors.top:    parent.top
        anchors.bottom: parent.bottom
        width:          ScreenTools.defaultFontPixelWidth
        cursorShape:    Qt.SizeHorCursor
        property real _startX
        property real _startWidth
        onPressed:      (mouse) => { _startX = mapToGlobal(mouse.x, 0).x; _startWidth = panel.width }
        onPositionChanged: (mouse) => {
            if (pressed) {
                tarAssistant.panelWidth = Math.max(panel._minWidth, _startWidth + (_startX - mapToGlobal(mouse.x, 0).x))
            }
        }
    }

    ColumnLayout {
        anchors.fill:       parent
        anchors.margins:    _margin
        spacing:            _margin / 2

        // ---- Header ----
        RowLayout {
            Layout.fillWidth:   true
            spacing:            _margin

            QGCLabel {
                text:               qsTr("Claude")
                font.bold:          true
                font.pointSize:     ScreenTools.mediumFontPointSize
            }
            QGCLabel {
                Layout.fillWidth:   true
                text:               tarAssistant.busy ? tarAssistant.status : (tarAssistant.mode === 1 ? qsTr("can edit files") : qsTr("read only"))
                color:              tarAssistant.busy ? qgcPal.colorGreen : qgcPal.colorGrey
                elide:              Text.ElideRight
                font.pointSize:     ScreenTools.smallFontPointSize
            }
            QGCButton {
                text:       qsTr("New")
                enabled:    !tarAssistant.busy
                onClicked:  tarAssistant.newChat()
            }
            QGCButton {
                text:       _showSettings ? qsTr("Done") : qsTr("Settings")
                onClicked:  _showSettings = !_showSettings
            }
            QGCButton {
                text:       "✕"
                onClicked:  tarAssistant.panelVisible = false
            }
        }

        // ---- Settings ----
        GridLayout {
            Layout.fillWidth:   true
            visible:            _showSettings
            columns:            2
            columnSpacing:      _margin
            rowSpacing:         _margin / 2

            QGCLabel { text: qsTr("Files") }
            QGCComboBox {
                Layout.fillWidth:   true
                model:              [ qsTr("Read only"), qsTr("Can edit files") ]
                currentIndex:       tarAssistant.mode
                onActivated:        (index) => tarAssistant.mode = index
            }

            QGCLabel { text: qsTr("Folder") }
            RowLayout {
                Layout.fillWidth: true
                QGCTextField {
                    id:                 folderField
                    Layout.fillWidth:   true
                    Component.onCompleted: text = tarAssistant.workingFolder
                    onEditingFinished:  {
                        tarAssistant.workingFolder = text
                        text = tarAssistant.workingFolder
                    }
                }
                QGCButton {
                    text:       qsTr("Open")
                    onClicked:  tarAssistant.openWorkingFolder()
                }
            }

            QGCCheckBox {
                Layout.columnSpan:  2
                text:               qsTr("Give Claude live vehicle and router state with each message")
                checked:            tarAssistant.includeContext
                onClicked:          tarAssistant.includeContext = checked
            }

            QGCButton {
                Layout.columnSpan:  2
                text:               _showContext ? qsTr("Hide context preview") : qsTr("Preview context")
                onClicked: {
                    _showContext = !_showContext
                    if (_showContext) {
                        contextPreview.text = tarAssistant.contextSnapshot()
                    }
                }
            }

            ScrollView {
                Layout.columnSpan:      2
                Layout.fillWidth:       true
                Layout.preferredHeight: ScreenTools.defaultFontPixelHeight * 12
                visible:                _showContext
                clip:                   true

                TextArea {
                    id:             contextPreview
                    readOnly:       true
                    wrapMode:       TextEdit.Wrap
                    selectByMouse:  true
                    color:          qgcPal.text
                    font.family:    ScreenTools.fixedFontFamily
                    font.pointSize: ScreenTools.smallFontPointSize
                    background:     Rectangle { color: qgcPal.windowShade }
                }
            }

            QGCLabel {
                Layout.columnSpan:  2
                Layout.fillWidth:   true
                wrapMode:           Text.WordWrap
                color:              qgcPal.colorGrey
                font.pointSize:     ScreenTools.smallFontPointSize
                text: qsTr("Runs Claude Code at %1. Claude can read and propose parameter changes; each change needs your approval here. Shell commands are always blocked and Claude cannot arm or fly vehicles. Session cost so far: $%2")
                        .arg(tarAssistant.claudePath || qsTr("(not found)"))
                        .arg(tarAssistant.sessionCost.toFixed(3))
            }
        }

        Rectangle {
            Layout.fillWidth:   true
            height:             1
            color:              qgcPal.groupBorder
        }

        // ---- Conversation ----
        ListView {
            id:                 chatView
            Layout.fillWidth:   true
            Layout.fillHeight:  true
            clip:               true
            spacing:            _margin
            model:              tarAssistant.messages
            boundsBehavior:     Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { }

            property bool _stickToBottom: true
            onMovementEnded:    _stickToBottom = atYEnd
            onContentHeightChanged: if (_stickToBottom) Qt.callLater(positionViewAtEnd)
            onCountChanged:     if (_stickToBottom) Qt.callLater(positionViewAtEnd)
            Component.onCompleted: positionViewAtEnd()

            delegate: Item {
                width:  chatView.width - _margin
                height: loader.height

                required property int    index
                required property string kind
                required property string text
                required property string detail

                Loader {
                    id:     loader
                    width:  parent.width
                    sourceComponent: kind === "user" ? userBubble
                                   : kind === "assistant" ? assistantText
                                   : kind === "tool" ? toolLine
                                   : kind === "approval" ? approvalCard
                                   : noteText
                    property string entryKind:   parent.kind
                    property string entryText:   parent.text
                    property string entryDetail: parent.detail
                    property int    entryIndex:  parent.index
                }
            }

            // Empty state: suggestions
            Column {
                anchors.centerIn:   parent
                width:              parent.width - _margin * 2
                spacing:            _margin
                visible:            chatView.count === 0

                QGCLabel {
                    width:                  parent.width
                    horizontalAlignment:    Text.AlignHCenter
                    wrapMode:               Text.WordWrap
                    color:                  qgcPal.colorGrey
                    text:                   qsTr("Ask about your vehicles, their parameters, the MAVLink router or your project files.")
                }
                Repeater {
                    model: [ qsTr("Why won't the vehicle arm?"),
                             qsTr("Summarize the status of all connected vehicles."),
                             qsTr("Check my router setup. Is MAVSDK getting data?") ]
                    QGCButton {
                        width:      parent.width
                        text:       modelData
                        onClicked:  tarAssistant.send(modelData)
                    }
                }
            }
        }

        // ---- Model and reasoning effort (apply from the next message) ----
        RowLayout {
            Layout.fillWidth:   true
            Layout.fillHeight:  false
            spacing:            _margin / 2

            QGCComboBox {
                id:                 modelCombo
                Layout.fillWidth:   true
                sizeToContents:     false
                // A model name typed elsewhere (e.g. a full model ID) is kept as an extra entry
                readonly property var _values: {
                    const v = [ "", "opus", "sonnet", "haiku", "fable" ]
                    if (v.indexOf(tarAssistant.model) < 0) v.push(tarAssistant.model)
                    return v
                }
                model: _values.map(v => v === "" ? qsTr("Default model")
                                              : v === "opus" ? "Opus" : v === "sonnet" ? "Sonnet"
                                              : v === "haiku" ? "Haiku" : v === "fable" ? "Fable" : v)
                currentIndex:       _values.indexOf(tarAssistant.model)
                onActivated:        (index) => tarAssistant.model = _values[index]
            }

            QGCComboBox {
                Layout.fillWidth:   true
                sizeToContents:     false
                readonly property var _values: [ "", "low", "medium", "high", "xhigh", "max" ]
                model:              [ qsTr("Default effort"), qsTr("Low effort"), qsTr("Medium effort"), qsTr("High effort"),
                                      qsTr("Extra high effort"), qsTr("Max effort") ]
                currentIndex:       Math.max(0, _values.indexOf(tarAssistant.effort))
                onActivated:        (index) => tarAssistant.effort = _values[index]
            }
        }

        // ---- Input ----
        // fillHeight must be false explicitly: a layout inherits fillHeight from its children otherwise and
        // takes space from the conversation.
        RowLayout {
            Layout.fillWidth:   true
            Layout.fillHeight:  false
            spacing:            _margin / 2

            ScrollView {
                id:                     inputScroll
                Layout.fillWidth:       true
                Layout.preferredHeight: Math.min(Math.max(input.implicitHeight, ScreenTools.defaultFontPixelHeight * 2.5), ScreenTools.defaultFontPixelHeight * 8)

                TextArea {
                    id:                 input
                    wrapMode:           TextEdit.Wrap
                    placeholderText:    qsTr("Message Claude (Enter to send, Shift+Enter for a new line)")
                    placeholderTextColor: qgcPal.colorGrey
                    color:              qgcPal.textFieldText
                    selectByMouse:      true
                    font.pointSize:     ScreenTools.defaultFontPointSize
                    background: Rectangle {
                        color:          qgcPal.textField
                        border.color:   input.activeFocus ? qgcPal.buttonHighlight : qgcPal.groupBorder
                        radius:         ScreenTools.defaultFontPixelWidth / 2
                    }
                    // Up/Down recall sent messages (like a terminal) when the cursor is on the first/last line
                    property var    _history:       []
                    property int    _historyIndex:  -1
                    property string _draft:         ""

                    function _showHistory(index) {
                        _historyIndex = index
                        text = index < 0 ? _draft : _history[index]
                        cursorPosition = text.length
                    }

                    Keys.onPressed: (event) => {
                        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && !(event.modifiers & Qt.ShiftModifier)) {
                            event.accepted = true
                            panel._send()
                        } else if (event.key === Qt.Key_Up && !(event.modifiers & Qt.ShiftModifier)
                                   && text.lastIndexOf("
", cursorPosition - 1) < 0) {
                            if (_historyIndex < 0) {
                                _history = tarAssistant.sentHistory()
                                _draft = text
                            }
                            const next = _historyIndex < 0 ? _history.length - 1 : _historyIndex - 1
                            if (next >= 0) {
                                event.accepted = true
                                _showHistory(next)
                            }
                        } else if (event.key === Qt.Key_Down && !(event.modifiers & Qt.ShiftModifier)
                                   && _historyIndex >= 0 && text.indexOf("
", cursorPosition) < 0) {
                            event.accepted = true
                            _showHistory(_historyIndex + 1 < _history.length ? _historyIndex + 1 : -1)
                        }
                    }
                }
            }

            QGCButton {
                Layout.preferredHeight: inputScroll.height
                Layout.alignment:   Qt.AlignBottom
                text:               tarAssistant.busy ? qsTr("Stop") : qsTr("Send")
                enabled:            tarAssistant.busy || input.text.trim().length > 0
                onClicked:          tarAssistant.busy ? tarAssistant.stop() : panel._send()
            }
        }
    }

    function _send() {
        if (tarAssistant.busy || input.text.trim().length === 0) {
            return
        }
        chatView._stickToBottom = true
        tarAssistant.send(input.text)
        input.text = ""
        input._historyIndex = -1
        input._draft = ""
    }

    onVisibleChanged: if (visible) input.forceActiveFocus()

    // ---- Delegates ----
    Component {
        id: userBubble
        Item {
            readonly property string entryText:   parent ? parent.entryText : ""
            readonly property string entryDetail: parent ? parent.entryDetail : ""

            width:  parent ? parent.width : 0
            height: bubble.height
            Rectangle {
                id:             bubble
                anchors.right:  parent.right
                width:          Math.min(parent.width * 0.85, Math.max(userLabel.implicitWidth, detailLabel.visible ? detailLabel.implicitWidth : 0) + _margin * 2)
                height:         userLabel.height + (detailLabel.visible ? detailLabel.height : 0) + _margin
                radius:         ScreenTools.defaultFontPixelWidth
                color:          qgcPal.windowShade
                border.color:   qgcPal.groupBorder

                TextEdit {
                    id:                 userLabel
                    x:                  _margin
                    y:                  _margin / 2
                    width:              parent.width - _margin * 2
                    text:               entryText
                    readOnly:           true
                    selectByMouse:      true
                    wrapMode:           TextEdit.Wrap
                    color:              qgcPal.text
                    font.pointSize:     ScreenTools.defaultFontPointSize
                    font.family:        ScreenTools.normalFontFamily
                }
                QGCLabel {
                    id:                 detailLabel
                    anchors.top:        userLabel.bottom
                    anchors.right:      parent.right
                    anchors.rightMargin: _margin
                    visible:            entryDetail !== ""
                    text:               entryDetail
                    color:              qgcPal.colorGrey
                    font.pointSize:     ScreenTools.smallFontPointSize
                }
            }
        }
    }

    Component {
        id: assistantText
        TextEdit {
            readonly property string entryText: parent ? parent.entryText : ""

            width:              parent ? parent.width : 0
            text:               entryText
            textFormat:         TextEdit.MarkdownText
            readOnly:           true
            selectByMouse:      true
            wrapMode:           TextEdit.Wrap
            color:              qgcPal.text
            selectionColor:     qgcPal.buttonHighlight
            font.pointSize:     ScreenTools.defaultFontPointSize
            font.family:        ScreenTools.normalFontFamily
            onLinkActivated:    (link) => Qt.openUrlExternally(link)

            HoverHandler {
                enabled:        parent.hoveredLink !== ""
                cursorShape:    Qt.PointingHandCursor
            }
        }
    }

    // set_parameters request: nothing is sent to the vehicle until Approve is pressed
    Component {
        id: approvalCard
        Rectangle {
            readonly property string entryText:   parent ? parent.entryText : ""
            readonly property string entryDetail: parent ? parent.entryDetail : ""
            readonly property int    entryIndex:  parent ? parent.entryIndex : -1
            readonly property bool   _pending:    entryDetail === "pending"

            width:          parent ? parent.width : 0
            height:         cardColumn.height + _margin * 2
            radius:         ScreenTools.defaultFontPixelWidth / 2
            color:          qgcPal.windowShade
            border.color:   _pending ? qgcPal.colorOrange : qgcPal.groupBorder
            border.width:   _pending ? 2 : 1

            Column {
                id:         cardColumn
                x:          _margin
                y:          _margin
                width:      parent.width - _margin * 2
                spacing:    _margin / 2

                TextEdit {
                    width:          parent.width
                    text:           entryText
                    readOnly:       true
                    selectByMouse:  true
                    wrapMode:       TextEdit.Wrap
                    color:          qgcPal.text
                    font.family:    ScreenTools.fixedFontFamily
                    font.pointSize: ScreenTools.defaultFontPointSize
                }

                Row {
                    spacing:    _margin
                    visible:    _pending
                    QGCButton {
                        text:       qsTr("Approve")
                        primary:    true
                        onClicked:  tarAssistant.resolveApproval(entryIndex, true)
                    }
                    QGCButton {
                        text:       qsTr("Reject")
                        onClicked:  tarAssistant.resolveApproval(entryIndex, false)
                    }
                }

                QGCLabel {
                    visible:        !_pending
                    text:           entryDetail === "applied" ? qsTr("✓ Applied and confirmed by the vehicle")
                                  : entryDetail === "done" ? qsTr("✓ Done")
                                  : entryDetail === "rejected" ? qsTr("✗ Rejected")
                                  : entryDetail.charAt(0).toUpperCase() + entryDetail.slice(1)
                    color:          (entryDetail === "applied" || entryDetail === "done") ? qgcPal.colorGreen
                                  : (entryDetail === "rejected" || entryDetail === "expired" || entryDetail === "cancelled") ? qgcPal.colorGrey
                                  : qgcPal.colorOrange
                    font.pointSize: ScreenTools.smallFontPointSize
                }
            }
        }
    }

    Component {
        id: toolLine
        QGCLabel {
            readonly property string entryText:   parent ? parent.entryText : ""
            readonly property string entryDetail: parent ? parent.entryDetail : ""

            width:          parent ? parent.width : 0
            text:           "▸ " + entryText + (entryDetail !== "" ? "  (" + entryDetail + ")" : "")
            color:          entryDetail !== "" ? qgcPal.colorRed : qgcPal.colorGrey
            elide:          Text.ElideMiddle
            font.family:    ScreenTools.fixedFontFamily
            font.pointSize: ScreenTools.smallFontPointSize
        }
    }

    Component {
        id: noteText
        Column {
            readonly property string entryKind:   parent ? parent.entryKind : ""
            readonly property string entryText:   parent ? parent.entryText : ""
            readonly property string entryDetail: parent ? parent.entryDetail : ""

            width:      parent ? parent.width : 0
            spacing:    _margin / 2

            TextEdit {
                width:          parent.width
                text:           entryText
                readOnly:       true
                selectByMouse:  true
                wrapMode:       TextEdit.Wrap
                color:          entryKind === "error" ? qgcPal.colorRed : qgcPal.colorGrey
                font.pointSize: ScreenTools.smallFontPointSize
                font.family:    ScreenTools.normalFontFamily
                font.italic:    entryKind === "info"
            }

            QGCButton {
                visible:    entryDetail === "login"
                text:       qsTr("Sign in to Claude Code...")
                onClicked:  tarAssistant.openSignIn()
            }
        }
    }
}
