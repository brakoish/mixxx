import QtQuick
import Mixxx 1.0 as Mixxx
import "Brand"

// Segment meter. The lit height is quantised to 8 steps, so engine-rate VU
// updates only change the scene when a segment changes.
Item {
    id: meter

    property string group: "[Main]"
    property bool stereo: false
    readonly property int segments: 8

    function lit(value) {
        return Math.round(Math.max(0, Math.min(1, value)) * segments) / segments;
    }

    Mixxx.ControlProxy {
        id: leftControl

        group: meter.group
        key: meter.stereo ? "vu_meter_left" : "vu_meter"
    }

    Mixxx.ControlProxy {
        id: rightControl

        group: meter.group
        key: "vu_meter_right"
    }

    Row {
        anchors.fill: parent
        spacing: 2

        Repeater {
            model: meter.stereo ? 2 : 1

            delegate: Rectangle {
                id: bar

                required property int index
                readonly property real level: meter.lit(index === 0 ? leftControl.value : rightControl.value)

                width: meter.stereo ? (meter.width - 2) / 2 : meter.width
                height: meter.height
                color: Brand.raise

                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: parent.height * bar.level
                    color: bar.level >= 1 ? Brand.live : Brand.play
                }
            }
        }
    }
}
