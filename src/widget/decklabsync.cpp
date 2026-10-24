// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabsync.h"

#include <utility>

#include "util/fpclassify.h"

namespace {
const QString kBeatSync = QStringLiteral("beatsync");
const QString kSyncEnabled = QStringLiteral("sync_enabled");
const QString kSyncLeader = QStringLiteral("sync_leader");
const QString kLoaded = QStringLiteral("track_loaded");
const QString kPlay = QStringLiteral("play");
const QString kLocalBpm = QStringLiteral("local_bpm");
const QString kCloneFromDeck = QStringLiteral("CloneFromDeck");
} // namespace

DeckLabSync::DeckLabSync(Host host)
        : m_host(std::move(host)) {
}

bool DeckLabSync::loaded(int deck) const {
    return deck >= 1 && deck <= 4 && m_host.read(deck, kLoaded) > 0;
}

bool DeckLabSync::playing(int deck) const {
    return deck >= 1 && deck <= 4 && m_host.read(deck, kPlay) > 0;
}

bool DeckLabSync::hasTempo(int deck) const {
    const double bpm = m_host.read(deck, kLocalBpm);
    return util_isfinite(bpm) && bpm > 0;
}

bool DeckLabSync::synced(int deck) const {
    return loaded(deck) && m_host.read(deck, kSyncEnabled) > 0;
}

bool DeckLabSync::master(int deck) const {
    return loaded(deck) && m_host.read(deck, kSyncLeader) > 0;
}

bool DeckLabSync::tap(int deck) {
    if (!loaded(deck)) {
        return false;
    }
    // The deck's own BEAT SYNC: the engine toggles sync lock on a press
    // (decklabcdjsync.h), exactly as a controller's SYNC does.
    m_host.write(deck, kBeatSync, 1);
    m_host.write(deck, kBeatSync, 0);
    return true;
}

bool DeckLabSync::canMaster(int deck) const {
    if (master(deck)) {
        return true;
    }
    return loaded(deck) && playing(deck) && hasTempo(deck);
}

bool DeckLabSync::toggleMaster(int deck) {
    if (!canMaster(deck)) {
        return false;
    }
    // Mixxx's leader request makes a playing deck the (soft) leader and
    // turns its sync on; 0 on the leader hands it to another playing synced
    // deck, if there is one.
    m_host.write(deck, kSyncLeader, master(deck) ? 0 : 1);
    return true;
}

int DeckLabSync::doublesSource(int deck) const {
    if (deck < 1 || deck > 4 || playing(deck)) {
        return 0;
    }
    // The tempo master if it plays, else a playing deck (a synced one
    // first), else a stopped master: the same order as the Numark overlay's
    // hold (DeckLabNumark.instantDoubles), so touch and controller agree.
    int source = 0;
    int best = 0;
    for (int other = 1; other <= 4; ++other) {
        if (other == deck || !loaded(other)) {
            continue;
        }
        const bool leads = m_host.read(other, kSyncLeader) > 0;
        int rank = leads ? 1 : 0;
        if (playing(other)) {
            rank = leads ? 4 : m_host.read(other, kSyncEnabled) > 0 ? 3 : 2;
        }
        if (rank > best) {
            source = other;
            best = rank;
        }
    }
    return source;
}

bool DeckLabSync::instantDoubles(int deck) {
    const int source = doublesSource(deck);
    if (source <= 0) {
        return false;
    }
    // The explicit deck number: CloneFromDeck 0 would let Mixxx pick.
    m_host.write(deck, kCloneFromDeck, source);
    return true;
}

bool DeckLabSync::hold(int deck) {
    if (loaded(deck) && playing(deck)) {
        return true; // never a track replaced, and no BEAT SYNC on release
    }
    return instantDoubles(deck);
}
