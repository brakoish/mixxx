import QtQuick
import "Brand"

Rectangle {
    id: page

    property string title: ""

    color: Brand.ground

    Column {
        anchors.centerIn: parent
        spacing: 10

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: page.title
            color: Brand.text
            font.family: Brand.ui
            font.pixelSize: 24
            font.weight: Font.DemiBold
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 520
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: qsTr("Not part of the QML preview yet. This spike ports only the Decks screen to one GPU scene.")
            color: Brand.muted
            font.family: Brand.ui
            font.pixelSize: 16
            font.weight: Font.Medium
        }
    }
}
