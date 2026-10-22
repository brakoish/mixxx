import QtQuick
import Mixxx 1.0 as Mixxx
import "Brand"

// Deck readout, overview, hot cues and transport, bound to real controls.
// Text only changes when its string changes, so a paused deck draws nothing.
Item {
    id: strip

    required property string group
    required property int deck
    readonly property color deckColor: Brand.deckColor(deck)
    readonly property var player: Mixxx.PlayerManager.getPlayer(group)
    readonly property bool loaded: loadedControl.value > 0
    readonly property bool playing: playControl.value > 0
    // Real time left at the current tempo.
    readonly property real remaining: loaded
            ? Math.max(0, durationControl.value * (1 - positionControl.value)) / Math.max(0.01, rateRatioControl.value)
            : 0
    readonly property bool endWarning: playing && loaded && remaining < 30 && loopControl.value === 0
    property bool blinkOn: true

    // Mixxx ChromaticKey (1-12 major, 13-24 minor) to Lancelot.
    function lancelot(key) {
        const names = ["", "8B", "3B", "10B", "5B", "12B", "7B", "2B", "9B", "4B", "11B", "6B", "1B",
            "5A", "12A", "7A", "2A", "9A", "4A", "11A", "6A", "1A", "8A", "3A", "10A"];
        const index = Math.round(key);
        return index >= 1 && index <= 24 ? names[index] : "-";
    }

    function clock(seconds) {
        const whole = Math.floor(seconds);
        const s = whole % 60;
        return "−" + Math.floor(whole / 60) + ":" + (s < 10 ? "0" : "") + s;
    }

    function bpmParts(value) {
        if (!(value > 0))
            return ["-", ""];

        const text = value.toFixed(2);
        const dot = text.indexOf(".");
        return [text.substring(0, dot), text.substring(dot)];
    }

    function tempo(ratio) {
        const percent = (ratio - 1) * 100;
        return (percent >= 0 ? "+" : "−") + Math.abs(percent).toFixed(2) + "%";
    }

    function pulse(control) {
        control.value = 1;
        control.value = 0;
    }

    Mixxx.ControlProxy { id: playControl; group: strip.group; key: "play" }
    Mixxx.ControlProxy { id: cueControl; group: strip.group; key: "cue_default" }
    Mixxx.ControlProxy { id: cueIndicatorControl; group: strip.group; key: "cue_indicator" }
    Mixxx.ControlProxy { id: loadedControl; group: strip.group; key: "track_loaded" }
    Mixxx.ControlProxy { id: bpmControl; group: strip.group; key: "bpm" }
    Mixxx.ControlProxy { id: keyControl; group: strip.group; key: "visual_key" }
    Mixxx.ControlProxy { id: positionControl; group: strip.group; key: "playposition" }
    Mixxx.ControlProxy { id: durationControl; group: strip.group; key: "duration" }
    Mixxx.ControlProxy { id: rateRatioControl; group: strip.group; key: "rate_ratio" }
    Mixxx.ControlProxy { id: syncControl; group: strip.group; key: "sync_enabled" }
    Mixxx.ControlProxy { id: leaderControl; group: strip.group; key: "sync_leader" }
    Mixxx.ControlProxy { id: beatSyncControl; group: strip.group; key: "beatsync" }
    Mixxx.ControlProxy { id: loopControl; group: strip.group; key: "loop_enabled" }
    Mixxx.ControlProxy { id: loopToggleControl; group: strip.group; key: "beatloop_4_toggle" }
    Mixxx.ControlProxy { id: keylockControl; group: strip.group; key: "keylock" }
    Mixxx.ControlProxy { id: quantizeControl; group: strip.group; key: "quantize" }

    Timer {
        interval: 500
        repeat: true
        running: strip.endWarning
        onTriggered: strip.blinkOn = !strip.blinkOn
        onRunningChanged: strip.blinkOn = true
    }

    Rectangle {
        anchors.fill: parent
        color: Brand.panel
    }

    // Channel meter along the left edge.
    LevelMeter {
        x: 0
        y: 4
        width: 4
        height: readout.height
        group: strip.group
    }

    Column {
        id: column

        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 6
        anchors.topMargin: 4
        spacing: 4

        Item {
            id: readout

            objectName: "decklab-readout-" + strip.group
            width: column.width
            height: 100

            Rectangle {
                id: badge

                width: 24
                height: 24
                radius: 3
                color: strip.deckColor

                Text {
                    anchors.centerIn: parent
                    text: strip.deck
                    color: Brand.ground
                    font.family: Brand.numerals
                    font.pixelSize: 18
                    font.weight: Font.Bold
                }
            }

            Text {
                id: stateLabel

                x: badge.width + 8
                anchors.verticalCenter: badge.verticalCenter
                text: !strip.loaded ? qsTr("EMPTY")
                        : strip.playing ? qsTr("PLAY")
                        : cueIndicatorControl.value > 0 ? qsTr("CUE") : qsTr("PAUSE")
                color: !strip.loaded ? Brand.line
                        : strip.playing ? Brand.play
                        : cueIndicatorControl.value > 0 ? Brand.cue : Brand.muted
                font.family: Brand.ui
                font.pixelSize: 14
                font.weight: Font.DemiBold
                font.letterSpacing: 0.8
            }

            Text {
                id: titleText

                x: stateLabel.x + stateLabel.width + 8
                width: readout.width - x
                anchors.verticalCenter: badge.verticalCenter
                elide: Text.ElideRight
                textFormat: Text.PlainText
                text: !strip.loaded || !strip.player ? qsTr("No track") : strip.player.title
                color: strip.loaded ? Brand.text : Brand.muted
                font.family: Brand.ui
                font.pixelSize: 18
                font.weight: Font.DemiBold
            }

            // Value row: BPM, key and remaining time.
            Row {
                id: values

                y: 28
                height: 44
                spacing: 4

                Text {
                    id: bpmWhole

                    anchors.baseline: parent.bottom
                    anchors.baselineOffset: -6
                    text: strip.bpmParts(bpmControl.value)[0]
                    color: Brand.text
                    font.family: Brand.numerals
                    font.pixelSize: 36
                    font.weight: Font.DemiBold
                    font.features: ({ "tnum": 1 })
                }

                Text {
                    anchors.baseline: bpmWhole.baseline
                    text: strip.bpmParts(bpmControl.value)[1]
                    color: Brand.text
                    font.family: Brand.numerals
                    font.pixelSize: 24
                    font.weight: Font.DemiBold
                    font.features: ({ "tnum": 1 })
                }

                Text {
                    anchors.baseline: bpmWhole.baseline
                    leftPadding: 2
                    text: qsTr("BPM")
                    color: Brand.muted
                    font.family: Brand.ui
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                }

                Text {
                    anchors.baseline: bpmWhole.baseline
                    leftPadding: 14
                    text: qsTr("KEY")
                    color: Brand.muted
                    font.family: Brand.ui
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                }

                Text {
                    anchors.baseline: bpmWhole.baseline
                    text: strip.lancelot(keyControl.value)
                    color: Brand.text
                    font.family: Brand.numerals
                    font.pixelSize: 24
                    font.weight: Font.DemiBold
                }
            }

            Text {
                id: timeText

                anchors.right: parent.right
                // Same baseline as the BPM (an anchor may only use a sibling).
                anchors.baseline: values.bottom
                anchors.baselineOffset: -6
                text: strip.loaded ? strip.clock(strip.remaining) : "−-:--"
                color: strip.endWarning && strip.blinkOn ? Brand.live : Brand.text
                font.family: Brand.numerals
                font.pixelSize: 36
                font.weight: Font.DemiBold
                font.features: ({ "tnum": 1 })
            }

            // Lamp row: sync state, loop, key lock, tempo.
            Row {
                y: 76
                height: 22
                spacing: 6

                Rectangle {
                    visible: leaderControl.value > 0
                    width: leaderText.implicitWidth + 10
                    height: 20
                    radius: 3
                    color: strip.deckColor

                    Text {
                        id: leaderText

                        anchors.centerIn: parent
                        text: qsTr("MASTER")
                        color: Brand.ground
                        font.family: Brand.ui
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }
                }

                Rectangle {
                    visible: syncControl.value > 0
                    width: syncText.implicitWidth + 10
                    height: 20
                    radius: 3
                    color: "transparent"
                    border.width: 1
                    border.color: strip.deckColor

                    Text {
                        id: syncText

                        anchors.centerIn: parent
                        text: qsTr("SYNC")
                        color: strip.deckColor
                        font.family: Brand.ui
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }
                }

                Text {
                    visible: loopControl.value > 0
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("LOOP")
                    color: Brand.play
                    font.family: Brand.ui
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("TEMPO %1").arg(strip.tempo(rateRatioControl.value))
                    color: Brand.muted
                    font.family: Brand.ui
                    font.pixelSize: 15
                    font.weight: Font.Medium
                }
            }
        }

        // Overview with needle search (only while paused, as in the widget UI).
        Item {
            id: overviewBox

            width: column.width
            height: 36

            Rectangle {
                anchors.fill: parent
                color: Brand.ground
            }

            Mixxx.WaveformOverview {
                anchors.fill: parent
                visible: strip.loaded && strip.player !== null
                player: strip.player
                channels: Mixxx.WaveformOverview.Channels.BothChannels
                renderer: Mixxx.WaveformOverview.Renderer.RGB
                colorHigh: Brand.signalHigh
                colorMid: Brand.signalMid
                colorLow: Brand.signalLow
            }

            // Played part dimmed, like the brand mark.
            Rectangle {
                visible: strip.loaded
                width: Math.round(parent.width * Math.max(0, Math.min(1, positionControl.value)))
                height: parent.height
                color: "#990D0F12"
            }

            Rectangle {
                visible: strip.loaded
                x: Math.round(parent.width * Math.max(0, Math.min(1, positionControl.value))) - 1
                width: 2
                height: parent.height
                color: Brand.live
            }

            MouseArea {
                anchors.fill: parent
                enabled: strip.loaded && !strip.playing
                onPressed: (mouse) => {
                    positionControl.value = Math.max(0, Math.min(1, mouse.x / width));
                }
                onPositionChanged: (mouse) => {
                    positionControl.value = Math.max(0, Math.min(1, mouse.x / width));
                }
            }
        }

        // Hot cues A-H.
        Row {
            width: column.width
            height: 44
            spacing: 4

            Repeater {
                model: 8

                delegate: PadButton {
                    id: hotcue

                    required property int index
                    readonly property string prefix: "hotcue_" + (index + 1)

                    width: (column.width - 7 * 4) / 8
                    label: "ABCDEFGH"[index]
                    lit: statusControl.value > 0
                    solid: true
                    solidColor: Brand.rgb(colorControl.value)
                    onHit: activateControl.value = 1
                    onLift: activateControl.value = 0

                    Mixxx.ControlProxy {
                        id: statusControl

                        group: strip.group
                        key: hotcue.prefix + "_status"
                    }

                    Mixxx.ControlProxy {
                        id: colorControl

                        group: strip.group
                        key: hotcue.prefix + "_color"
                    }

                    Mixxx.ControlProxy {
                        id: activateControl

                        group: strip.group
                        key: hotcue.prefix + "_activate"
                    }
                }
            }
        }

        // Transport.
        Row {
            id: transport

            readonly property real buttonWidth: (column.width - 5 * 4) / 6

            width: column.width
            height: 44
            spacing: 4

            PadButton {
                objectName: "decklab-cue-" + strip.group
                width: transport.buttonWidth
                label: qsTr("CUE")
                lit: cueIndicatorControl.value > 0
                litColor: Brand.cue
                onHit: cueControl.value = 1
                onLift: cueControl.value = 0
            }

            PadButton {
                objectName: "decklab-play-" + strip.group
                width: transport.buttonWidth
                label: qsTr("PLAY")
                lit: strip.playing
                litColor: Brand.play
                // On press, not release: transport must feel instant.
                onHit: playControl.value = strip.playing ? 0 : 1
            }

            PadButton {
                width: transport.buttonWidth
                label: qsTr("SYNC")
                lit: syncControl.value > 0
                onHit: strip.pulse(beatSyncControl)
            }

            PadButton {
                width: transport.buttonWidth
                label: qsTr("LOOP 4")
                lit: loopControl.value > 0
                litColor: Brand.play
                onHit: strip.pulse(loopToggleControl)
            }

            PadButton {
                width: transport.buttonWidth
                label: qsTr("KEY LOCK")
                lit: keylockControl.value > 0
                onHit: keylockControl.value = keylockControl.value > 0 ? 0 : 1
            }

            PadButton {
                width: transport.buttonWidth
                label: qsTr("QNT")
                lit: quantizeControl.value > 0
                onHit: quantizeControl.value = quantizeControl.value > 0 ? 0 : 1
            }
        }
    }
}
