#pragma once

#include <QString>
#include <QtGlobal>

namespace mixxx {
namespace decklab {

/// Deck Lab (pi4-perf): the legacy touchscreen companion talks to Mixxx
/// through a SysEx state bridge, the "Deck Lab touchscreen" mapping
/// (DeckLab.midi.xml with DeckLab.js) on the MIDI Through port. Its script
/// polls about 40 controls and sends a JSON SysEx report every 250 ms, which
/// keeps the controller thread busy (about 2% of a Pi 4 core) and wakes the
/// companion.
///
/// The native kiosk launcher sets DECKLAB_MIDI_BRIDGE=0 once decklabd
/// provides system state and tool launching. Then the bridge mapping is not
/// opened at startup. Unset or any other value keeps upstream behaviour, so
/// the legacy rollback (companion plus bridge) is unchanged. A user who
/// enables the mapping in the controller preferences still gets it.
inline bool midiStateBridgeDisabledFromEnvironment() {
    bool ok = false;
    const int value = qEnvironmentVariableIntValue("DECKLAB_MIDI_BRIDGE", &ok);
    return ok && value == 0;
}

/// True for the bridge mapping file, matched by file name so it does not
/// depend on the name of the MIDI Through port.
inline bool isMidiStateBridgeMapping(const QString& mappingFileName) {
    return mappingFileName.compare(QLatin1String("DeckLab.midi.xml"), Qt::CaseInsensitive) == 0;
}

} // namespace decklab
} // namespace mixxx
