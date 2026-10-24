// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "preferences/usersettings.h"

// Deck Lab: Settings > Decks > "PLAY starts".
//
// Stock Mixxx moves the playhead when a quantized deck starts while another
// deck plays: the start is phase-seeked, by up to half a beat, so its next
// beat lands on the other deck's beat. A hot cue or cue preview started from
// pause is phase-seeked the same way. A CDJ starts exactly at the playhead,
// cue or hot cue; only Beat Sync lines the decks up.
//
// The option lives in the engine because every PLAY (touch, any controller
// mapping, CUE preview, hot cues) ends up as the same [ChannelN],play write.
// The engine default is Mixxx's behaviour; the native Deck Lab surface writes
// the CDJ default the first time it runs. Only primary decks are affected.
// A synchronized deck (sync lock leader or follower) that starts while
// another synced deck plays keeps the aligned start: with Quantize on, as in
// Mixxx, and whatever Quantize is set to for a follower under the CDJ BEAT
// SYNC rules (engine/sync/decklabcdjsync.h). A synced deck with no synced
// deck playing, such as a paused tempo master beside an unsynced deck in the
// headphones, starts exactly: it has nobody to follow, and it is never lined
// up with an unsynced deck. A synced sampler that plays does not count.
// Quantized seeks while a deck plays (hot cues, loops, beat jumps) are
// unchanged.
//
// A one-shot SYNC tap made while an unsynced deck is paused, with another
// deck to sync to, lines up its next start with the playing deck: Mixxx's
// beatsync and beatsync_tempo with Quantize on, beatsync_phase always. A cue
// or hot cue preview released back to pause leaves the tap for the next
// start; PLAY, a preview latched into play, a load or an eject uses it up.
// With no other deck playing the start stays exact.
namespace decklab {

enum class PlayStartAlign {
    // Mixxx: a quantized deck starts on the beat of another playing deck.
    Quantize = 0,
    // CDJ: PLAY starts exactly where the deck is, unless it syncs.
    SyncLockOnly = 1,
};

// What the native surface writes when the user has never chosen.
constexpr PlayStartAlign kNativePlayStartAlign = PlayStartAlign::SyncLockOnly;

// Saved choice, [DeckLab] PlayStartAlign in mixxx.cfg.
inline ConfigKey playStartAlignConfigKey() {
    return ConfigKey(QStringLiteral("[DeckLab]"), QStringLiteral("PlayStartAlign"));
}

// Live value read by every EngineBuffer, created by EngineMixer.
inline ConfigKey playStartAlignControlKey() {
    return ConfigKey(QStringLiteral("[DeckLab]"), QStringLiteral("play_start_align"));
}

inline PlayStartAlign playStartAlignFromValue(double value) {
    return value == static_cast<double>(PlayStartAlign::SyncLockOnly)
            ? PlayStartAlign::SyncLockOnly
            : PlayStartAlign::Quantize;
}

// The native surface's choice: the saved one, or the CDJ start written the
// first time (an existing value, either one, is never overwritten).
inline PlayStartAlign nativePlayStartAlign(const UserSettingsPointer& config) {
    if (!config->exists(playStartAlignConfigKey())) {
        config->setValue(playStartAlignConfigKey(), static_cast<int>(kNativePlayStartAlign));
    }
    return playStartAlignFromValue(config->getValue(
            playStartAlignConfigKey(), static_cast<int>(kNativePlayStartAlign)));
}

} // namespace decklab
