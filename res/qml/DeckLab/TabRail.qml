import QtQuick
import Mixxx 1.0 as Mixxx
import "Brand"

Rectangle {
    id: rail

    property int page: 0
    readonly property var tabs: [qsTr("Decks"), qsTr("Browse"), qsTr("Sampler"), qsTr("Lab FX"), qsTr("Settings"), qsTr("USB")]
    readonly property var tabIds: ["decks", "browse", "sampler", "labfx", "settings", "usb"]

    signal pageRequested(int index)

    color: Brand.panel

    Image {
        id: mark

        x: 10
        width: 24
        height: 24
        anchors.verticalCenter: parent.verticalCenter
        source: "../../skins/DeckLab/decklab-icon-24.png"
        smooth: false
    }

    Row {
        id: tabRow

        x: mark.x + mark.width + 8
        height: parent.height

        Repeater {
            model: rail.tabs.length

            delegate: Item {
                id: tab

                required property int index

                objectName: "decklab-tab-" + rail.tabIds[index]
                width: label.implicitWidth + 26
                height: tabRow.height

                Text {
                    id: label

                    anchors.centerIn: parent
                    text: rail.tabs[tab.index]
                    color: tab.index === rail.page ? Brand.text : Brand.muted
                    font.family: Brand.ui
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }

                Rectangle {
                    visible: tab.index === rail.page
                    anchors.bottom: parent.bottom
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: parent.width - 12
                    height: 3
                    color: Brand.text
                }

                MouseArea {
                    anchors.fill: parent
                    onPressed: rail.pageRequested(tab.index)
                }
            }
        }
    }

    Text {
        id: previewTag

        anchors.right: meter.left
        anchors.rightMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        text: qsTr("QML PREVIEW")
        color: Brand.muted
        font.family: Brand.ui
        font.pixelSize: 14
        font.weight: Font.DemiBold
        font.letterSpacing: 0.8
    }

    LevelMeter {
        id: meter

        anchors.right: parent.right
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        width: 14
        height: 28
        group: "[Main]"
        stereo: true
    }

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Brand.line
    }
}
