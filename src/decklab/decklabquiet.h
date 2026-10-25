// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

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

} // namespace decklab
