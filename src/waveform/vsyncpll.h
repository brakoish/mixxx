// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>
#include <QtGlobal>

/// Deck Lab: idle-aware vblank lock for the ST_PLL VSync mode.
///
/// How upstream's PLL sees the vblank on X11. The 3x3 shared GL window (shown
/// in the corner of the main window in this mode) runs with swap interval 1. While it repaints faster than the display can
/// show its frames (PLL init repaints on every frameSwapped), both of its back
/// buffers end up queued for presentation and every further paint waits inside
/// the driver until a vblank frees one. From then on one paint per frame keeps
/// that queue full, so each frameSwapped arrives just after a vblank. A paint
/// that does not wait (the queue drained because a frame was missed, or the
/// window was idle) returns at the GUI thread's own pace and carries no vblank
/// information. Upstream feeds such samples to the loop anyway, which then
/// locks onto its own timer and slowly drifts off the panel rate.
///
/// The waveform windows cannot serve as the reference: they swap once per
/// frame through QOpenGLContext::swapBuffers(), which never emits
/// QOpenGLWindow::frameSwapped and never waits (their queue is never full,
/// and filling it would add two frames of latency to every waveform).
///
/// So this keeps upstream's reference and adds:
/// - VSyncPll: upstream's loop filter, fed only with paints that waited for a
///   vblank, deduplicated per frame. Idle, it freewheels on its period; when
///   samples resume it pulls the phase back with a wider gain for a second
///   and leaves the period alone, so the slots move smoothly. Only slots
///   found before the real vblank cross it in one step (one repeated frame
///   instead of several).
/// - VSyncKeepalive: repaints the shared window only while something moves
///   (plus 500 ms) and for 250 ms once a minute while idle, and repaints once
///   more right away after a paint that did not wait, which refills the queue
///   within a frame.
/// - VSyncLockWatchdog: falls back to the timer loop when the loop does not
///   lock within 3 s of usable startup time.
/// - VSyncPll::nextSlotMicros: the slot schedule, kSlotLeadMicros after the
///   estimated vblank and at most one slot per vblank interval.
/// All pure, so the tests drive them with simulated clocks.

namespace mixxx {
namespace decklab {

/// VSyncThread::ST_PLL and ST_TIMER (checked in vsyncthread.h).
constexpr int kVSyncModePll = 5;
constexpr int kVSyncModeTimer = 6;

/// DECKLAB_VSYNC overrides [Waveform] VSync (and the GL swap interval).
enum class VSyncOverride {
    None,  ///< unset or empty: [Waveform] VSync decides (upstream behaviour)
    Pll,   ///< "pll": ST_PLL, swap interval 1
    Timer, ///< "timer": ST_TIMER, swap interval 0
};

/// Case-insensitive "pll" or "timer", surrounding blanks ignored. Empty means
/// None. Anything else is None and sets *pValid to false.
VSyncOverride parseVSyncOverride(const QString& value, bool* pValid = nullptr);
/// Reads DECKLAB_VSYNC and warns once about an invalid value.
VSyncOverride vsyncOverrideFromEnvironment();
/// The VSyncThread::VSyncMode to use for a configured [Waveform] VSync value.
int effectiveVSyncMode(int configuredMode, VSyncOverride vsyncOverride);
QString vsyncOverrideName(VSyncOverride vsyncOverride);

/// Phase-locked loop on vblank-referenced timestamps in microseconds (any
/// monotonic origin). Not thread safe.
class VSyncPll {
  public:
    // Upstream constants.
    static constexpr int kNumStableDeltasRequired = 20;
    static constexpr double kInitToleranceMicros = 2000.0;
    static constexpr double kAlpha = 0.01;
    static constexpr double kBeta = 0.5 * kAlpha * kAlpha;
    static constexpr double kInitialDeltaMicros = 16666.6;
    // Deck Lab additions.
    /// A referenced sample more than this many periods after the previous one
    /// resumes after a gap (idle, a stall).
    static constexpr double kGapFrames = 6.0;
    /// After a gap: samples that pull the phase back with a wider gain and
    /// leave the period measured before the gap alone. Each moves the slots
    /// by a tenth of the remaining error (60 of them leave 0.2%): a 1.5 ms
    /// freewheel error shifts the waveform by at most 0.15 ms of motion per
    /// frame instead of in one step.
    static constexpr int kRelockSamples = 60;
    static constexpr double kRelockAlpha = 0.1;
    /// Slots run this long after the estimated vblank (upstream: right after
    /// it). A freewheel error up to this size cannot move a slot across the
    /// vblank (a repeated frame), and the keepalive paint 5 ms after the slot
    /// still lands before the next vblank.
    static constexpr double kSlotLeadMicros = 1500.0;
    /// Slot intervals start this long before the estimated vblank, which lags
    /// the real one by the time a paint needs to return after it.
    static constexpr double kBoundaryMarginMicros = 1000.0;

    enum class Result {
        Initializing, ///< measuring the initial period (upstream init)
        Tracked,      ///< loop filter applied
        Relocked,     ///< first referenced sample after a gap
        Unreferenced, ///< paint did not wait for a vblank: ignored
        Duplicate,    ///< second sample within half a frame: ignored
    };

    struct Stats {
        quint64 tracked = 0;
        quint64 relocks = 0;
        quint64 snaps = 0;
        quint64 unreferenced = 0;
        quint64 duplicates = 0;
        quint64 initResets = 0;
    };

    /// vblankReferenced is ignored during init, like upstream: init only
    /// accepts a run of stable deltas, which unqueued paints do not produce.
    Result addSample(double phaseInMicros, bool vblankReferenced);

    bool initializing() const {
        return m_initCount < kNumStableDeltasRequired;
    }
    int initCount() const {
        return m_initCount;
    }
    /// Last estimated vblank (plus the paint latency).
    double phaseOut() const {
        return m_phaseOut;
    }
    /// Estimated frame period.
    double deltaOut() const {
        return m_deltaOut;
    }
    /// Phase error of the last tracked or relocked sample, wrapped to half a
    /// period, before the correction.
    double lastPhaseError() const {
        return m_lastPhaseError;
    }
    bool hasLastSample() const {
        return m_hasLastSample;
    }
    double lastSampleMicros() const {
        return m_lastSampleMicros;
    }
    /// Where the vblank was at the last accepted sample: that paint waited
    /// for it. Unlike phaseOut(), which the slots follow and which is pulled
    /// in gradually after a gap, this is right at once; the slot schedule
    /// uses it for the vblank interval boundaries.
    double vblankEstimate() const {
        return m_hasLastSample ? m_lastSampleMicros : m_phaseOut;
    }
    /// The freewheeled vblank estimate nearest to t.
    double predictedVsyncNear(double t) const;
    const Stats& stats() const {
        return m_stats;
    }

    /// The next slot: the first point at or after now on the grid
    /// anchor + offset + n * period. While that point lies in the same vblank
    /// interval as the last slot the GUI thread actually started (it ran late),
    /// it moves on by whole periods, so no interval gets two swaps. Intervals
    /// start kBoundaryMarginMicros before vblankAnchor + n * period; the caller
    /// passes the vblankEstimate() nearest to anchor. Unlike upstream's
    /// nextSwap logic this also follows a phase correction to an earlier time.
    static double nextSlotMicros(double now,
            double anchor,
            double period,
            double offset,
            double vblankAnchor,
            bool hasLastSlot,
            double lastSlotStart,
            int* pSkipped = nullptr);

  private:
    int m_initCount = 0;
    double m_initSum = 0.0;
    double m_phaseOut = 0.0;
    double m_deltaOut = kInitialDeltaMicros;
    double m_lastPhaseError = 0.0;
    bool m_hasLastSample = false;
    double m_lastSampleMicros = 0.0;
    int m_relockRemaining = 0;
    Stats m_stats;
};

/// When the shared GL window repaints to feed the PLL (GUI thread).
class VSyncKeepalive {
  public:
    /// Keep repainting this long after the last activity.
    static constexpr qint64 kIdleStopMillis = 500;
    /// Extra paints per slot to refill the swap queue after unqueued paints.
    static constexpr int kRefillPaintsPerSlot = 4;
    /// While idle, repaint for kRefreshMillis this often, so the freewheeled
    /// phase never drifts far (about 0.1 ms per minute after a normal lock;
    /// see the tests) and a resume never needs a large correction.
    static constexpr qint64 kIdleRefreshIntervalMillis = 60000;
    static constexpr qint64 kRefreshMillis = 250;

    struct Stats {
        quint64 starts = 0;
        quint64 stops = 0;
        quint64 refreshes = 0;
        quint64 slotPaints = 0;
        quint64 refillPaints = 0;
    };

    /// Once per VSync slot, after init. active: a waveform moved, changed or
    /// was forced in the last render pass, or a widget that renders every
    /// frame (spinny, GL VU meter) exists. Returns true when the shared window
    /// should repaint in this slot.
    bool onSlot(qint64 nowMillis, bool active);
    /// After each keepalive frameSwapped. Returns true to repaint once more
    /// right away: the paint did not wait for a vblank, so the queue is not
    /// full and its timestamp was not used.
    bool onFrameSwapped(bool blocked);

    bool running() const {
        return m_running;
    }
    const Stats& stats() const {
        return m_stats;
    }

  private:
    bool m_running = false;
    bool m_started = false;
    qint64 m_runUntilMillis = 0;
    qint64 m_nextRefreshMillis = 0;
    int m_refillBudget = 0;
    Stats m_stats;
};

/// Startup safety for ST_PLL: gives up when the loop has not locked after
/// kDeadlineMillis of usable time, i.e. while the GUI event loop ran and the
/// shared GL window was exposed (a driver that never delivers frameSwapped,
/// or delivers it without vblank pacing). Each tick counts at most
/// kMaxTickMillis, so a GUI thread blocked by skin loading does not count.
/// Unexposed time counts towards kHiddenDeadlineMillis only.
class VSyncLockWatchdog {
  public:
    static constexpr qint64 kDeadlineMillis = 3000;
    static constexpr qint64 kHiddenDeadlineMillis = 10000;
    static constexpr qint64 kMaxTickMillis = 250;

    enum class State {
        Waiting,
        Locked,
        FallBack,
    };

    void onFrameSwapped() {
        ++m_framesSwapped;
    }
    State tick(qint64 elapsedMillis, bool exposed, bool initializing);

    State state() const {
        return m_state;
    }
    qint64 exposedMillis() const {
        return m_exposedMillis;
    }
    qint64 totalMillis() const {
        return m_totalMillis;
    }
    quint64 framesSwapped() const {
        return m_framesSwapped;
    }

  private:
    State m_state = State::Waiting;
    qint64 m_exposedMillis = 0;
    qint64 m_totalMillis = 0;
    quint64 m_framesSwapped = 0;
};

} // namespace decklab
} // namespace mixxx
