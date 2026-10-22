import QtQuick
import "Brand"

// 44 px touch target. On = raised fill, white text, outline in litColor.
// solid: a set hot cue fills with its own colour instead.
Rectangle {
    id: button

    property string label: ""
    property bool lit: false
    property color litColor: Brand.text
    property bool solid: false
    property color solidColor: Brand.raise
    readonly property bool down: touch.pressed

    signal hit()
    signal lift()

    height: 44
    radius: 4
    color: solid && lit ? solidColor : (lit || down ? "#2A3039" : Brand.raise)
    border.width: lit || down ? 2 : 1
    border.color: lit ? litColor : (down ? Brand.text : Brand.line)

    Text {
        anchors.centerIn: parent
        width: parent.width - 6
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideRight
        text: button.label
        color: button.solid && button.lit ? Brand.ground : (button.lit || button.down ? Brand.text : Brand.muted)
        font.family: Brand.ui
        font.pixelSize: 16
        font.weight: Font.DemiBold
    }

    MouseArea {
        id: touch

        anchors.fill: parent
        onPressed: button.hit()
        onReleased: button.lift()
        onCanceled: button.lift()
    }
}
