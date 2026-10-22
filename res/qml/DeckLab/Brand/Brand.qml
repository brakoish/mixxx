// Deck Lab brand tokens (provisional, PLAN 2026-10-22) for the QML preview.
pragma Singleton
import QtQuick

QtObject {
    // Neutrals
    readonly property color ground: "#0D0F12"
    readonly property color panel: "#16191E"
    readonly property color raise: "#20242B"
    readonly property color line: "#2C313A"
    readonly property color muted: "#9AA3AF"
    readonly property color text: "#F2F4F7"
    // Live red only for the playhead, on air, recording and clip.
    readonly property color live: "#FF3D2E"
    // Transport state colours kept from the Decks brief.
    readonly property color play: "#2FD27A"
    readonly property color cue: "#FF7A1A"
    readonly property var deckColors: ["#35C8F0", "#F5A524", "#B5E61D", "#C77DFF"]

    // Waveform colours from res/skins/DeckLab/skin.xml
    readonly property color signalLow: "#004EE4"
    readonly property color signalMid: "#C95A00"
    readonly property color signalHigh: "#F6E9D3"
    readonly property color beat: "#707070"

    readonly property string ui: "Barlow"
    readonly property string numerals: "Barlow Semi Condensed"

    // The OFL Barlow files ship with the widget skin; res/skins is installed whole.
    property FontLoader barlowMedium: FontLoader {
        source: "../../../skins/DeckLab/fonts/Barlow-Medium.ttf"
    }
    property FontLoader barlowSemiBold: FontLoader {
        source: "../../../skins/DeckLab/fonts/Barlow-SemiBold.ttf"
    }
    property FontLoader barlowBold: FontLoader {
        source: "../../../skins/DeckLab/fonts/Barlow-Bold.ttf"
    }
    property FontLoader numeralsMedium: FontLoader {
        source: "../../../skins/DeckLab/fonts/BarlowSemiCondensed-Medium.ttf"
    }
    property FontLoader numeralsSemiBold: FontLoader {
        source: "../../../skins/DeckLab/fonts/BarlowSemiCondensed-SemiBold.ttf"
    }
    property FontLoader numeralsBold: FontLoader {
        source: "../../../skins/DeckLab/fonts/BarlowSemiCondensed-Bold.ttf"
    }

    function deckColor(deck) {
        return deckColors[(deck - 1) % 4];
    }

    // #RRGGBB from a Mixxx RGB control value (e.g. hotcue_1_color).
    function rgb(value) {
        const hex = Math.max(0, Math.round(value)).toString(16);
        return "#" + ("000000" + hex).slice(-6);
    }
}
