import QtQuick
import Mixxx 1.0 as Mixxx
import Mixxx.Controls 1.0 as MixxxControls
import "Brand"

// One scrolling waveform lane: an 18 px deck tag and Mixxx's scene-graph
// waveform (allshader renderers on rendergraph_sg). The playhead, labels and
// touch handling are ordinary items in the same scene, so overlays stack
// normally and nothing is a native window.
Item {
    id: lane

    required property string group
    required property int deck
    readonly property color deckColor: Brand.deckColor(deck)
    property bool scratching: false
    readonly property bool loaded: loadedControl.value > 0

    // Touch scratch, as WWaveformViewer does it: the engine follows the
    // finger one-to-one (interleaved samples, hence x2) from where it landed.
    function scratchTarget(x) {
        return -x * waveform.audioSamplesPerPixel() * 2;
    }

    function beginScratch(x) {
        if (!lane.loaded)
            return;

        scratching = true;
        scratchPositionControl.value = scratchTarget(x);
        scratchEnableControl.value = 1;
    }

    function moveScratch(x) {
        if (scratching)
            scratchPositionControl.value = scratchTarget(x);
    }

    function endScratch() {
        if (!scratching)
            return;

        scratchEnableControl.value = 0;
        scratching = false;
    }

    Mixxx.ControlProxy {
        id: zoomControl

        group: lane.group
        key: "waveform_zoom"
    }

    Mixxx.ControlProxy {
        id: scratchEnableControl

        group: lane.group
        key: "scratch_position_enable"
    }

    Mixxx.ControlProxy {
        id: scratchPositionControl

        group: lane.group
        key: "scratch_position"
    }

    Mixxx.ControlProxy {
        id: loadedControl

        group: lane.group
        key: "track_loaded"
    }

    Rectangle {
        id: tag

        width: 18
        height: parent.height
        color: Brand.panel

        Rectangle {
            width: 4
            height: parent.height
            color: lane.deckColor
        }

        Text {
            anchors.centerIn: parent
            anchors.horizontalCenterOffset: 2
            text: lane.deck
            color: lane.deckColor
            font.family: Brand.numerals
            font.pixelSize: 16
            font.weight: Font.Bold
        }
    }

    Item {
        id: waveArea

        objectName: "decklab-wave-" + lane.group
        x: tag.width
        width: parent.width - tag.width
        height: parent.height
        clip: true

        MixxxControls.WaveformDisplay {
            id: waveform

            anchors.fill: parent
            group: lane.group
            // 1 (closest) to 10; the control starts at 0 before any skin sets it.
            zoom: zoomControl.value >= 1 ? zoomControl.value : 3
            backgroundColor: Brand.ground

            Mixxx.WaveformRendererEndOfTrack {
                color: Brand.live
                endOfTrackWarningTime: 30
            }

            Mixxx.WaveformRendererPreroll {
                color: "#1C1C22"
            }

            Mixxx.WaveformRendererMarkRange {
                Mixxx.WaveformMarkRange {
                    startControl: "loop_start_position"
                    endControl: "loop_end_position"
                    enabledControl: "loop_enabled"
                    color: "#2FD27A"
                    opacity: 0.25
                    disabledColor: "#9AA3AF"
                    disabledOpacity: 0.12
                }
            }

            Mixxx.WaveformRendererRGB {
                axesColor: Brand.line
                lowColor: Brand.signalLow
                midColor: Brand.signalMid
                highColor: Brand.signalHigh
                gainAll: 1.0
                gainLow: 1.0
                gainMid: 1.0
                gainHigh: 1.0
            }

            Mixxx.WaveformRendererBeat {
                color: Brand.beat
            }

            Mixxx.WaveformRendererMark {
                playMarkerColor: Brand.live
                playMarkerBackground: Brand.ground
                untilMark.showTime: false
                untilMark.showBeats: false
                defaultMark: Mixxx.WaveformMark {
                    align: "bottom|right"
                    color: "#35C8F0"
                    textColor: "#0D0F12"
                    text: " %1 "
                }

                Mixxx.WaveformMark {
                    control: "cue_point"
                    text: "CUE"
                    align: "top|right"
                    color: "#FF7A1A"
                    textColor: "#0D0F12"
                }

                Mixxx.WaveformMark {
                    control: "loop_start_position"
                    align: "top|left"
                    color: "#2FD27A"
                    textColor: "#0D0F12"
                }

                Mixxx.WaveformMark {
                    control: "loop_end_position"
                    align: "bottom|right"
                    color: "#2FD27A"
                    textColor: "#0D0F12"
                }
            }
        }

        // The red playhead, drawn in the same scene above the waveform.
        Rectangle {
            id: playhead

            x: Math.round(waveArea.width / 2) - 1
            width: 2
            height: parent.height
            color: Brand.live
        }

        Text {
            anchors.centerIn: parent
            visible: !lane.loaded
            text: qsTr("Load a track from Browse")
            color: Brand.muted
            font.family: Brand.ui
            font.pixelSize: 15
            font.weight: Font.Medium
        }

        Rectangle {
            id: scratchChip

            x: 8
            y: 6
            visible: lane.scratching
            width: scratchLabel.implicitWidth + 12
            height: 20
            radius: 3
            color: lane.deckColor

            Text {
                id: scratchLabel

                anchors.centerIn: parent
                text: qsTr("SCRATCH")
                color: Brand.ground
                font.family: Brand.ui
                font.pixelSize: 14
                font.weight: Font.DemiBold
                font.letterSpacing: 0.8
            }
        }

        Text {
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.top: parent.top
            anchors.topMargin: 4
            text: "×" + waveform.zoom.toFixed(1)
            color: Brand.muted
            font.family: Brand.numerals
            font.pixelSize: 14
            font.weight: Font.DemiBold
        }

        // One finger holds and scratches the deck; two fingers pinch-zoom.
        // Mouse input acts as the first finger.
        MultiPointTouchArea {
            id: touch

            property bool pinching: false
            property real pinchStartSpread: 1
            property real pinchStartZoom: 3

            function spread() {
                return Math.max(8, Math.sqrt(Math.pow(p2.x - p1.x, 2) + Math.pow(p2.y - p1.y, 2)));
            }

            function track() {
                if (p1.pressed && p2.pressed) {
                    lane.endScratch();
                    if (!pinching) {
                        pinching = true;
                        pinchStartSpread = spread();
                        pinchStartZoom = waveform.zoom;
                    }
                    // Fingers apart = zoom in (fewer samples per pixel).
                    zoomControl.value = Math.max(1, Math.min(10, pinchStartZoom * pinchStartSpread / spread()));
                    return;
                }
                if (pinching) {
                    // Lift both fingers before the deck can be grabbed again.
                    if (!p1.pressed && !p2.pressed)
                        pinching = false;

                    return;
                }
                const point = p1.pressed ? p1 : (p2.pressed ? p2 : null);
                if (point === null)
                    lane.endScratch();
                else if (lane.scratching)
                    lane.moveScratch(point.x);
                else
                    lane.beginScratch(point.x);
            }

            anchors.fill: parent
            minimumTouchPoints: 1
            maximumTouchPoints: 2
            mouseEnabled: true
            touchPoints: [
                TouchPoint {
                    id: p1
                },
                TouchPoint {
                    id: p2
                }
            ]
            onPressed: track()
            onUpdated: track()
            onReleased: track()
            onCanceled: {
                pinching = false;
                lane.endScratch();
            }
        }
    }
}
