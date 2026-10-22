// Deck Lab QML preview (gpu-screen spike): the Decks screen as ONE Qt Quick
// scene. Waveforms, readouts, overlays and touch targets share a single GL
// surface, so on EGLFS/KMS there is no X server, no compositor and no native
// child window per waveform. Run with --qml and DECKLAB_QML_MAIN=qml/DeckLab/main.qml.
import QtQuick
import QtQuick.Window
import Mixxx 1.0 as Mixxx
import "Brand"

Window {
    id: root

    // Design canvas: the 800x480 DSI panel. Other panels letterbox it.
    readonly property real designWidth: 800
    readonly property real designHeight: 480
    property int page: 0

    width: 800
    height: 480
    visible: true
    color: Brand.ground
    title: qsTr("Deck Lab")
    // eglfs and linuxfb have exactly one full-screen surface.
    visibility: (Qt.platform.pluginName === "eglfs" || Qt.platform.pluginName === "linuxfb")
            ? Window.FullScreen : Window.Windowed

    Item {
        id: stage

        objectName: "decklab-stage"
        width: root.designWidth
        height: root.designHeight
        anchors.centerIn: parent
        scale: Math.min(root.width / root.designWidth, root.height / root.designHeight)
        clip: true

        TabRail {
            id: rail

            width: parent.width
            height: 44
            page: root.page
            onPageRequested: (index) => {
                root.page = index;
            }
        }

        Item {
            id: decksPage

            objectName: "decklab-decks-page"
            y: rail.height
            width: parent.width
            height: parent.height - rail.height
            visible: root.page === 0

            Column {
                id: lanes

                width: parent.width
                spacing: 2

                WaveLane {
                    group: "[Channel1]"
                    deck: 1
                    width: lanes.width
                    height: 95
                }

                WaveLane {
                    group: "[Channel2]"
                    deck: 2
                    width: lanes.width
                    height: 95
                }
            }

            Row {
                id: strips

                y: lanes.height + 4
                width: parent.width
                height: parent.height - y

                DeckStrip {
                    group: "[Channel1]"
                    deck: 1
                    width: strips.width / 2
                    height: strips.height
                }

                DeckStrip {
                    group: "[Channel2]"
                    deck: 2
                    width: strips.width / 2
                    height: strips.height
                }
            }
        }

        PlaceholderPage {
            y: rail.height
            width: parent.width
            height: parent.height - rail.height
            visible: root.page !== 0
            title: rail.tabs[root.page]
        }
    }
}
