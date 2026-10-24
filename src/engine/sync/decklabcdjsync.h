// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "preferences/configobject.h"

// r6-sync: Deck Lab's SYNC works like the BEAT SYNC of a Pioneer DJ
// CDJ-3000 (owner decision; CDJ-3000 Instruction Manual, "Beat Sync",
// pages 66 and 67). [DeckLab],cdj_sync is 0, Mixxx's own behaviour, until
// the native Deck Lab surface turns it on, so stock Mixxx and its tests are
// unchanged. EngineSync owns the control: it exists before any deck's
// controls do. While it is 1:
//  - beatsync on a deck is the BEAT SYNC button: a press turns sync lock on
//    or off (otherwise it is Mixxx's one-shot match). It never turns sync on
//    for a deck without a tempo (a track not analysed yet), so a later
//    analysis can never retune that deck on its own. With no tempo master
//    deck yet, the audible playing deck it matches becomes the master first
//    (keeping its tempo), so the synced deck follows it from then on. Every
//    controller mapping that sends beatsync gets the CDJ button unchanged.
//  - a synced deck that is not the tempo master ignores its tempo control
//    (rate and the rate_perm_* buttons): only the master moves the tempo,
//    for every synced deck. Mixxx otherwise lets any synced deck retune the
//    whole group, the deck on air included. When sync turns off the deck
//    keeps its tempo; a controller's soft takeover picks its fader up once
//    the fader passes that tempo, as on the CDJ.
namespace decklab::cdjsync {

inline ConfigKey controlKey() {
    return ConfigKey(QStringLiteral("[DeckLab]"), QStringLiteral("cdj_sync"));
}

} // namespace decklab::cdjsync
