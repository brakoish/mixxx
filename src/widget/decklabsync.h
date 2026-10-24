// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>
#include <functional>

// r6-sync: the Decks page's SYNC, MASTER and INST DOUBLES, as on a Pioneer
// DJ CDJ-3000 (owner decision; CDJ-3000 Instruction Manual, "Beat Sync",
// pages 66 and 67):
//  - SYNC tap: BEAT SYNC on or off. On, the deck follows the tempo master's
//    tempo and keeps its beat (Mixxx sync lock) and SYNC stays lit; off, the
//    deck keeps the tempo it has. The tap writes the deck's beatsync, which
//    the engine turns into that toggle in native Deck Lab (engine/sync/
//    decklabcdjsync.h), so a controller's SYNC and the touch SYNC are the
//    same button. No tempo yet (a track not analysed): sync stays off.
//  - MASTER: one tempo master, Mixxx's sync leader. It is picked for you: the
//    deck on air when sync first goes on elsewhere (it keeps its tempo), else
//    Mixxx's soft leader (the synced deck that plays). MASTER in the deck's
//    MORE sheet makes a playing deck the master, or hands the master on from
//    the master itself. A deck leads only while it plays, as a CDJ hands the
//    master on when the master pauses.
//  - SYNC hold (and INST DOUBLES in MORE): only onto a stopped deck, empty or
//    cued, the tempo master's track (else a playing deck's) loads at the same
//    position, playing if it plays (Mixxx's CloneFromDeck). Never onto a
//    playing deck: there a hold does nothing at all, not even BEAT SYNC on
//    release (owner rule; a CDJ's hold never toggles Beat Sync either). On a
//    stopped deck with nothing to copy the hold's release is still a tap, as
//    on the Numark overlay, so a slow press is never a dead button. The CDJ
//    has no such limit (its LOAD LOCK setting only blocks loads while
//    playing); Deck Lab never replaces a track on air.
// The tempo fader rules live in the engine (decklabcdjsync.h): a synced deck
// that is not the master ignores its fader; after sync is off, the fader
// takes over only once it passes the tempo (a controller's soft takeover).
//
// Pure control logic over the host's reads and writes: no widgets, no timers,
// a few control reads per call.
class DeckLabSync final {
  public:
    struct Host {
        std::function<double(int deck, const QString& item)> read;
        std::function<void(int deck, const QString& item, double value)> write;
    };

    explicit DeckLabSync(Host host);

    // SYNC tap: BEAT SYNC on or off. False when there is no track.
    bool tap(int deck);
    bool synced(int deck) const;
    bool master(int deck) const;

    // MASTER: a loaded deck with a tempo that plays can take the master; the
    // master can always hand it on (Mixxx keeps it when no other synced deck
    // plays).
    bool canMaster(int deck) const;
    bool toggleMaster(int deck);

    // INST DOUBLES: the deck it would copy, 0 when it cannot (the deck plays,
    // or no other loaded deck plays or is the master). The master if it
    // plays, else a playing synced deck, else a playing deck, else a stopped
    // master.
    int doublesSource(int deck) const;
    bool canDouble(int deck) const {
        return doublesSource(deck) > 0;
    }
    // Loads the source's track onto this stopped deck. False: nothing done.
    bool instantDoubles(int deck);
    // SYNC held: INST DOUBLES on a stopped deck. True when the hold used up
    // the press (it loaded a track, or the deck plays: a hold on a playing
    // deck does nothing), so its release is no tap.
    bool hold(int deck);

  private:
    bool loaded(int deck) const;
    bool playing(int deck) const;
    bool hasTempo(int deck) const;

    Host m_host;
};
