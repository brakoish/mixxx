// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QtGlobal>
#include <algorithm>

// r7: when Deck Lab's background jobs may run. One definition for all of
// them: the background analysis of new downloads (decklabanalysis.h), the
// USB relinker's merge and rename passes (decklabusbrelink.h), the library
// rescan Deck Lab starts when a utility window closes (decklabutilities.h)
// and adding a TO PAD capture to the library (widget/decklabcapture.h).
//
// They wait while anything a DJ hears or records runs, while Mixxx's library
// scan holds its long write transaction (every other write would wait for it
// on the GUI thread), and while a utility window is open: Get Music and
// Sample Maker do heavy disk work of their own and talk to Deck Lab through
// decklabd, which must answer them at once.
//
// r7 fix: they start only once it has been quiet the whole time for
// kQuietSpellMs (QuietSpell, DeckLabSystem::settled): a moment of quiet
// between two tracks (a stop and start cut, a brake, the next deck a second
// later) is not enough. The instant check (isQuiet) still pauses or defers
// a job the moment something starts. Mixxx's start-up scan goes through the
// same rule, and a library scan that runs when playback starts is canceled
// and runs again after the next quiet spell (decklabutilities.h).

namespace decklab {

struct Activity {
    bool playing = false;     // a deck, sampler or preview plays
    bool recording = false;   // a mix recording runs
    bool scanning = false;    // Mixxx's library scan runs
    bool utilityOpen = false; // Get Music or Sample Maker is open
};

constexpr bool isQuiet(const Activity& activity) {
    return !activity.playing && !activity.recording && !activity.scanning && !activity.utilityOpen;
}

constexpr qint64 kQuietSpellMs = 8000;

// When it was last not quiet, on a monotonic clock in milliseconds.
class QuietSpell {
  public:
    // Something that keeps the jobs waiting started or stopped at `now`.
    void busy(qint64 now) {
        m_busyAt = std::max(m_busyAt, now);
    }
    // Quiet now (isQuiet) and for spellMs before.
    bool quietFor(bool quietNow, qint64 now, qint64 spellMs) {
        if (!quietNow) {
            busy(now);
            return false;
        }
        return now - m_busyAt >= spellMs;
    }

  private:
    qint64 m_busyAt = 0;
};

} // namespace decklab
