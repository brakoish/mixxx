// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>

/// Deck Lab diagnostic: when the GUI thread has not run its event loop for a
/// while (8 s by default, [DeckLab] GuiStallReportMs, 0 = off), its stack is
/// written once per stall to a small file beside Mixxx's log and copied into
/// the log, so a frozen screen names the call it waits in. The black box sees
/// only that the thread waits (a futex); this says on what.
///
/// A watch thread checks a heartbeat the GUI thread's event loop beats; on a
/// stall it signals the GUI thread (SIGUSR2), whose handler only unwinds its
/// own stack into the file (async-signal-safe writes). Linux with glibc only;
/// elsewhere start() does nothing.
class DeckLabGuiStallWatch {
  public:
    /// Call on the GUI thread once its event loop is about to run. reportPath
    /// is the file the stack goes to (kept below 256 KiB).
    static bool start(const QString& reportPath, int stallMs);
    /// Stops the watch thread (before the application quits).
    static void stop();
    static bool running();
    /// Stalls reported since start() (tests).
    static int reportsForTesting();
};
