// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "engine/engine.h"
#include "preferences/usersettings.h"

// Deck Lab: Settings > Deck > "Stem tracks".
//
// Mixxx opens a stem file (NI STEMS, *.stem.mp4) on a main deck in one of two
// ways: as four stems for stem mixing (the default, an 8-channel engine
// buffer with per-stem mute, volume and quick effects), or as one stereo
// signal summed from all four stems (Mixxx's own "Load pre-mixed stereo
// track"). Both decode the four stem streams; the file's mastered mixdown
// stream is never played (see SoundSourceSTEM::tryOpen). The stereo mix only
// saves the engine side: time-stretching, per-stem effects and stem mixing
// run on two channels instead of eight.
namespace decklab {

enum class StemPlayback {
    Stems = 0,
    StereoMix = 1,
};

inline ConfigKey stemPlaybackConfigKey() {
    return ConfigKey(QStringLiteral("[DeckLab]"), QStringLiteral("StemPlayback"));
}

inline StemPlayback stemPlayback(const UserSettingsPointer& config) {
    return config && config->getValue(stemPlaybackConfigKey(), 0) == 1
            ? StemPlayback::StereoMix
            : StemPlayback::Stems;
}

#ifdef __STEM__
// The stem selection a main deck load uses. An explicit selection (one stem,
// or Mixxx's "pre-mixed stereo" menu item) is always kept as requested.
inline mixxx::StemChannelSelection stemLoadSelection(
        const UserSettingsPointer& config, mixxx::StemChannelSelection requested) {
    if (!requested && stemPlayback(config) == StemPlayback::StereoMix) {
        return mixxx::StemChannel::All;
    }
    return requested;
}
#endif

} // namespace decklab
