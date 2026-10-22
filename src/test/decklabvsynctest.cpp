// SPDX-License-Identifier: GPL-2.0-or-later
// Deck Lab vsync-lock: idle-aware PLL, slot schedule, keepalive policy,
// startup fallback and the DECKLAB_VSYNC override. Simulated clocks, no GL.
#include <gtest/gtest.h>

#include <QtGlobal>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <queue>
#include <thread>
#include <vector>

#include "waveform/vsyncpll.h"
#include "waveform/vsyncthread.h"

namespace {

using mixxx::decklab::VSyncKeepalive;
using mixxx::decklab::VSyncLockWatchdog;
using mixxx::decklab::VSyncOverride;
using mixxx::decklab::VSyncPll;

// The panel's nominal mode (wlr-randr) and its measured vblank rate: DRM vblank
// timestamps on the Pi 4 DSI panel gave 16646.09 us (60.0742 Hz) over 1,200
// frames.
constexpr double kNominalPeriod = 1e6 / 60.029;
constexpr double kMeasuredPeriod = 16646.09;

/// Deterministic uniform noise (the test must not depend on the standard
/// library's distributions).
class Noise {
  public:
    explicit Noise(std::uint64_t seed)
            : m_state(seed * 0x9e3779b97f4a7c15ULL + 1) {
    }
    double uniform(double lo, double hi) {
        m_state ^= m_state << 13;
        m_state ^= m_state >> 7;
        m_state ^= m_state << 17;
        const double unit = static_cast<double>(m_state >> 11) / 9007199254740992.0;
        return lo + (hi - lo) * unit;
    }

  private:
    std::uint64_t m_state;
};

double wrapToHalfPeriod(double value, double period) {
    return value - std::round(value / period) * period;
}

/// Feeds vblank-referenced samples (vblank + paint latency + jitter) from
/// `from` (frame index) for `frames` frames. Returns the last sample time.
double feedLocked(VSyncPll* pPll,
        Noise* pNoise,
        double period,
        double phase,
        long from,
        long frames,
        double jitter) {
    double t = 0.0;
    for (long n = from; n < from + frames; ++n) {
        t = phase + static_cast<double>(n) * period + 200.0 + pNoise->uniform(0.0, jitter);
        pPll->addSample(t, true);
    }
    return t;
}

// ---------------------------------------------------------------------------
// DECKLAB_VSYNC

TEST(DeckLabVSyncTest, EnvironmentOverrideParsing) {
    bool valid = false;
    EXPECT_EQ(VSyncOverride::Pll, mixxx::decklab::parseVSyncOverride("pll", &valid));
    EXPECT_TRUE(valid);
    EXPECT_EQ(VSyncOverride::Pll, mixxx::decklab::parseVSyncOverride(" PLL ", &valid));
    EXPECT_TRUE(valid);
    EXPECT_EQ(VSyncOverride::Timer, mixxx::decklab::parseVSyncOverride("timer", &valid));
    EXPECT_TRUE(valid);
    EXPECT_EQ(VSyncOverride::Timer, mixxx::decklab::parseVSyncOverride("Timer\n", &valid));
    EXPECT_TRUE(valid);
    EXPECT_EQ(VSyncOverride::None, mixxx::decklab::parseVSyncOverride("", &valid));
    EXPECT_TRUE(valid);
    EXPECT_EQ(VSyncOverride::None, mixxx::decklab::parseVSyncOverride("   ", &valid));
    EXPECT_TRUE(valid);
    for (const char* bad : {"5", "6", "vsync", "pll5", "on", "1"}) {
        valid = true;
        EXPECT_EQ(VSyncOverride::None, mixxx::decklab::parseVSyncOverride(bad, &valid)) << bad;
        EXPECT_FALSE(valid) << bad;
    }

    // The override replaces [Waveform] VSync; unset keeps it (upstream).
    EXPECT_EQ(VSyncThread::ST_PLL,
            mixxx::decklab::effectiveVSyncMode(VSyncThread::ST_DEFAULT, VSyncOverride::Pll));
    EXPECT_EQ(VSyncThread::ST_TIMER,
            mixxx::decklab::effectiveVSyncMode(VSyncThread::ST_PLL, VSyncOverride::Timer));
    EXPECT_EQ(VSyncThread::ST_PLL,
            mixxx::decklab::effectiveVSyncMode(VSyncThread::ST_PLL, VSyncOverride::None));
    EXPECT_EQ(VSyncThread::ST_DEFAULT,
            mixxx::decklab::effectiveVSyncMode(VSyncThread::ST_DEFAULT, VSyncOverride::None));
    EXPECT_EQ(VSyncThread::ST_FREE,
            mixxx::decklab::effectiveVSyncMode(VSyncThread::ST_FREE, VSyncOverride::None));

    const QByteArray saved = qgetenv("DECKLAB_VSYNC");
    const bool wasSet = qEnvironmentVariableIsSet("DECKLAB_VSYNC");
    qunsetenv("DECKLAB_VSYNC");
    EXPECT_EQ(VSyncOverride::None, mixxx::decklab::vsyncOverrideFromEnvironment());
    qputenv("DECKLAB_VSYNC", "pll");
    EXPECT_EQ(VSyncOverride::Pll, mixxx::decklab::vsyncOverrideFromEnvironment());
    qputenv("DECKLAB_VSYNC", "timer");
    EXPECT_EQ(VSyncOverride::Timer, mixxx::decklab::vsyncOverrideFromEnvironment());
    qputenv("DECKLAB_VSYNC", "fast");
    EXPECT_EQ(VSyncOverride::None, mixxx::decklab::vsyncOverrideFromEnvironment());
    if (wasSet) {
        qputenv("DECKLAB_VSYNC", saved);
    } else {
        qunsetenv("DECKLAB_VSYNC");
    }
}

// ---------------------------------------------------------------------------
// Loop filter

TEST(DeckLabVSyncTest, LocksToThePanelRate) {
    for (const double period : {kNominalPeriod, kMeasuredPeriod}) {
        VSyncPll pll;
        Noise noise(7);
        const double phase = 123456.0;
        // Init sees the queue filling first: unqueued paints 5 ms apart.
        double t = phase - 20000.0;
        for (int i = 0; i < 3; ++i) {
            t += 5200.0;
            pll.addSample(t, false);
        }
        long frame = 0;
        while (pll.initializing() && frame < 200) {
            feedLocked(&pll, &noise, period, phase, frame++, 1, 300.0);
        }
        ASSERT_FALSE(pll.initializing()) << period;
        EXPECT_LE(frame, 30) << "init should need about 20 frames";
        EXPECT_NEAR(period, pll.deltaOut(), 30.0) << "init average";

        // 20 s of vblank-paced samples with +-150 us of jitter.
        feedLocked(&pll, &noise, period, phase, frame, 1200, 300.0);
        frame += 1200;
        EXPECT_NEAR(period, pll.deltaOut(), 0.05) << "period after 20 s";
        const double lastVsync = phase + static_cast<double>(frame - 1) * period + 200.0 + 150.0;
        EXPECT_LT(std::abs(wrapToHalfPeriod(pll.phaseOut() - lastVsync, period)), 100.0)
                << "phase after 20 s";
        EXPECT_EQ(0u, pll.stats().relocks);
    }
}

TEST(DeckLabVSyncTest, IgnoresPaintsThatDidNotWaitAndDuplicates) {
    VSyncPll pll;
    Noise noise(3);
    const double period = kMeasuredPeriod;
    feedLocked(&pll, &noise, period, 0.0, 0, 600, 200.0);
    ASSERT_FALSE(pll.initializing());
    const double phase = pll.phaseOut();
    const double delta = pll.deltaOut();

    // An unqueued paint 5 ms after a vblank: upstream would have pulled the
    // phase 50 us late and the period up. Ignored here.
    EXPECT_EQ(VSyncPll::Result::Unreferenced, pll.addSample(phase + 5200.0, false));
    EXPECT_EQ(phase, pll.phaseOut());
    EXPECT_EQ(delta, pll.deltaOut());
    // A second waited-for sample within the same frame is a duplicate.
    const double next = 600.0 * period + 300.0;
    EXPECT_EQ(VSyncPll::Result::Tracked, pll.addSample(next, true));
    EXPECT_EQ(VSyncPll::Result::Duplicate, pll.addSample(next + 2000.0, true));
    EXPECT_EQ(1u, pll.stats().unreferenced);
    EXPECT_EQ(1u, pll.stats().duplicates);
}

// ---------------------------------------------------------------------------
// Idle: freewheel, then resume

TEST(DeckLabVSyncTest, FreewheelPhaseErrorAfterIdleStaysUnderHalfAFrame) {
    for (const double period : {kNominalPeriod, kMeasuredPeriod}) {
        for (const double idleSeconds : {1.0, 10.0, 60.0, 300.0, 600.0}) {
            VSyncPll pll;
            Noise noise(11);
            const double phase = 5000.0;
            const long lockFrames = 1800; // 30 s
            feedLocked(&pll, &noise, period, phase, 0, lockFrames, 300.0);
            ASSERT_FALSE(pll.initializing());

            // No samples while idle. At resume, where does the freewheeled
            // estimate put the vblank, relative to the real one (plus the
            // paint latency the loop locked to)?
            const long resumeFrame = lockFrames + std::lround(idleSeconds * 1e6 / period);
            const double realVsync = phase + static_cast<double>(resumeFrame) * period + 350.0;
            const double freewheelError =
                    wrapToHalfPeriod(pll.predictedVsyncNear(realVsync) - realVsync, period);
            EXPECT_LT(std::abs(freewheelError), period / 2.0)
                    << "period " << period << " idle " << idleSeconds << " s";
            // After 30 s of lock the period is good to a few hundredths of a
            // microsecond: even 10 minutes of idle stay within the 1.5 ms
            // slot lead, so the resume needs no slot to cross a vblank.
            EXPECT_LT(std::abs(freewheelError), VSyncPll::kSlotLeadMicros)
                    << "period " << period << " idle " << idleSeconds << " s";
            std::printf("DECKLAB_TEST freewheel rate=%.4fHz idle=%.0fs error=%.0fus "
                        "period_error=%.4fus\n",
                    1e6 / period,
                    idleSeconds,
                    freewheelError,
                    pll.deltaOut() - period);
        }
    }
}

TEST(DeckLabVSyncTest, ResumePullsThePhaseBackGraduallyAndKeepsThePeriod) {
    VSyncPll pll;
    Noise noise(5);
    const double period = kMeasuredPeriod;
    const double phase = 1000.0;
    feedLocked(&pll, &noise, period, phase, 0, 1800, 300.0);
    const double delta = pll.deltaOut();

    // A large freewheel error: the real vblank comes 7 ms earlier than the
    // estimate after the idle stretch.
    const long resumeFrame = 1800 + 3600;
    const double estimate = pll.predictedVsyncNear(phase + resumeFrame * period);
    const double sample = estimate - 7000.0;
    // Unqueued paints right after the resume do not count...
    EXPECT_EQ(VSyncPll::Result::Unreferenced, pll.addSample(sample - 12000.0, false));
    EXPECT_EQ(VSyncPll::Result::Unreferenced, pll.addSample(sample - 6800.0, false));
    // ...the first paint that waited for a vblank starts the pull-in: a tenth
    // of the error per frame, the period untouched.
    EXPECT_EQ(VSyncPll::Result::Relocked, pll.addSample(sample, true));
    EXPECT_NEAR(-7000.0, pll.lastPhaseError(), 1.0);
    EXPECT_NEAR(estimate - 700.0, pll.phaseOut(), 1.0);
    EXPECT_EQ(delta, pll.deltaOut()) << "the period measured before the gap is kept";
    EXPECT_EQ(1u, pll.stats().relocks);

    double previousPhase = pll.phaseOut();
    double maxStep = 0.0;
    double lastError = 0.0;
    for (long n = 1; n <= VSyncPll::kRelockSamples + 300; ++n) {
        const double t = sample + n * period + noise.uniform(-150.0, 150.0);
        EXPECT_EQ(VSyncPll::Result::Tracked, pll.addSample(t, true));
        // How far the slot grid moves in one frame, beyond the period.
        maxStep = std::max(maxStep, std::abs(pll.phaseOut() - previousPhase - period));
        previousPhase = pll.phaseOut();
        if (n == VSyncPll::kRelockSamples - 1) {
            // The relock sample was the first of kRelockSamples.
            EXPECT_EQ(delta, pll.deltaOut()) << "period untouched during the pull-in";
            EXPECT_LT(std::abs(pll.lastPhaseError()), 0.01 * 7000.0 + 200.0);
        }
        lastError = pll.lastPhaseError();
    }
    // Never more than a tenth of the remaining error (plus jitter) in a frame.
    EXPECT_LT(maxStep, 0.1 * 6300.0 + 0.1 * 150.0);
    EXPECT_LT(std::abs(lastError), 400.0);
    EXPECT_NEAR(period, pll.deltaOut(), 0.05);
}

// ---------------------------------------------------------------------------
// Slot schedule

TEST(DeckLabVSyncTest, SlotScheduleFollowsCorrectionsAndNeverDoublesAnInterval) {
    const double period = 16000.0;
    const double anchor = 100000.0; // estimated vblank
    // On the grid anchor + offset + n * period, at or after now.
    EXPECT_DOUBLE_EQ(anchor + 3 * period,
            VSyncPll::nextSlotMicros(
                    anchor + 2 * period + 1.0, anchor, period, 0.0, anchor, false, 0.0));
    EXPECT_DOUBLE_EQ(anchor + 2 * period,
            VSyncPll::nextSlotMicros(anchor + 2 * period, anchor, period, 0.0, anchor, false, 0.0));
    EXPECT_DOUBLE_EQ(anchor + 2 * period + 2000.0,
            VSyncPll::nextSlotMicros(
                    anchor + 2 * period + 1.0, anchor, period, 2000.0, anchor, false, 0.0));
    // A phase correction to an earlier time takes effect at once (upstream
    // kept the later schedule until the estimate overtook it).
    EXPECT_DOUBLE_EQ(anchor - 3000.0 + 3 * period,
            VSyncPll::nextSlotMicros(anchor + 2 * period + 1.0,
                    anchor - 3000.0,
                    period,
                    0.0,
                    anchor - 3000.0,
                    false,
                    0.0));

    // The last slot ran 1 ms late, still in its own interval: next grid point.
    int skipped = -1;
    EXPECT_DOUBLE_EQ(anchor + 3 * period,
            VSyncPll::nextSlotMicros(anchor + 2 * period + 5000.0,
                    anchor,
                    period,
                    0.0,
                    anchor,
                    true,
                    anchor + 2 * period + 1000.0,
                    &skipped));
    EXPECT_EQ(0, skipped);
    // Slots 2 ms after the vblank. The last one (due at anchor + 2 periods +
    // 2 ms) was held up until just after the next vblank (the GUI thread was
    // blocked) and finished at once: the next grid point lies in that same
    // vblank interval and is skipped.
    EXPECT_DOUBLE_EQ(anchor + 4 * period + 2000.0,
            VSyncPll::nextSlotMicros(anchor + 3 * period + 600.0,
                    anchor,
                    period,
                    2000.0,
                    anchor,
                    true,
                    anchor + 3 * period + 100.0,
                    &skipped));
    EXPECT_EQ(1, skipped);
    // A slot that ran just before that vblank is in the previous interval.
    EXPECT_DOUBLE_EQ(anchor + 3 * period + 2000.0,
            VSyncPll::nextSlotMicros(anchor + 3 * period + 600.0,
                    anchor,
                    period,
                    2000.0,
                    anchor,
                    true,
                    anchor + 3 * period - 1500.0,
                    &skipped));
    EXPECT_EQ(0, skipped);

    // During a pull-in the slots still run on the old estimate, 8 ms after
    // the real vblank, while the boundaries already use the real one. A slot
    // held up until just after the real vblank shares its interval with the
    // next grid point, which is skipped; on the old estimate it would not be.
    const double realVblank = anchor - 8000.0;
    EXPECT_DOUBLE_EQ(anchor + 4 * period,
            VSyncPll::nextSlotMicros(realVblank + 3 * period + 900.0,
                    anchor,
                    period,
                    0.0,
                    realVblank,
                    true,
                    realVblank + 3 * period + 300.0,
                    &skipped));
    EXPECT_EQ(1, skipped);
    EXPECT_DOUBLE_EQ(anchor + 3 * period,
            VSyncPll::nextSlotMicros(realVblank + 3 * period + 900.0,
                    anchor,
                    period,
                    0.0,
                    anchor,
                    true,
                    realVblank + 3 * period + 300.0,
                    &skipped));
    EXPECT_EQ(0, skipped);

    // Degenerate period: no sleep.
    EXPECT_DOUBLE_EQ(5.0, VSyncPll::nextSlotMicros(5.0, anchor, 0.0, 0.0, anchor, false, 0.0));
}

// ---------------------------------------------------------------------------
// Keepalive policy

TEST(DeckLabVSyncTest, KeepaliveStopsWhenIdleAndRefillsTheQueue) {
    VSyncKeepalive keepalive;
    qint64 now = 1000;
    // Right after the lock: paints for 30 s even when idle (the period
    // settles), then stops.
    const qint64 lockedAt = now;
    while (keepalive.onSlot(now += 17, false)) {
        ASSERT_LT(now - lockedAt, 40000);
    }
    EXPECT_GE(now - lockedAt, VSyncKeepalive::kSettleMillis);
    EXPECT_LT(now - lockedAt, VSyncKeepalive::kSettleMillis + 34);
    EXPECT_EQ(1u, keepalive.stats().starts);
    EXPECT_EQ(1u, keepalive.stats().stops);
    for (int i = 0; i < 30; ++i) {
        EXPECT_FALSE(keepalive.onSlot(now += 17, false));
    }
    // A deck starts moving: paint every slot.
    EXPECT_TRUE(keepalive.onSlot(now += 17, true));
    EXPECT_EQ(2u, keepalive.stats().starts);
    // A paint that waited for a vblank needs no follow-up.
    EXPECT_FALSE(keepalive.onFrameSwapped(true));
    // Unqueued paints (resume, a dropped frame): refill, at most 4 per slot.
    int refills = 0;
    while (keepalive.onFrameSwapped(false)) {
        ++refills;
        ASSERT_LE(refills, 10);
    }
    EXPECT_EQ(VSyncKeepalive::kRefillPaintsPerSlot, refills);
    EXPECT_TRUE(keepalive.onSlot(now += 17, true));
    EXPECT_TRUE(keepalive.onFrameSwapped(false)) << "budget renews every slot";

    // Paused: keeps painting for 500 ms, then stops.
    const qint64 pausedAt = now;
    int paintingSlots = 0;
    while (keepalive.onSlot(now += 17, false)) {
        ++paintingSlots;
        ASSERT_LT(now - pausedAt, 1000);
    }
    EXPECT_GE(now - pausedAt, VSyncKeepalive::kIdleStopMillis);
    EXPECT_LT(now - pausedAt, VSyncKeepalive::kIdleStopMillis + 17);
    EXPECT_EQ(2u, keepalive.stats().stops);
    EXPECT_FALSE(keepalive.running());
    EXPECT_FALSE(keepalive.onFrameSwapped(false)) << "no refills while stopped";
    for (int i = 0; i < 600; ++i) {
        EXPECT_FALSE(keepalive.onSlot(now += 17, false));
    }
    // Play again.
    EXPECT_TRUE(keepalive.onSlot(now += 17, true));
    EXPECT_EQ(3u, keepalive.stats().starts);
    EXPECT_EQ(0u, keepalive.stats().refreshes);

    // Long idle: 250 ms of paints once a minute, counted from the last
    // activity, nothing in between.
    const qint64 lastActive = now;
    int idlePaints = 0;
    std::vector<qint64> refreshStarts;
    bool wasRunning = true;
    while (now - lastActive < 185000) {
        const bool paints = keepalive.onSlot(now += 17, false);
        if (paints) {
            ++idlePaints;
            if (!wasRunning) {
                refreshStarts.push_back(now - lastActive);
            }
        }
        wasRunning = paints;
    }
    ASSERT_EQ(3u, refreshStarts.size());
    EXPECT_NEAR(60000, refreshStarts[0], 17);
    EXPECT_NEAR(120000, refreshStarts[1], 34);
    EXPECT_NEAR(180000, refreshStarts[2], 51);
    // 500 ms after the activity plus three refreshes of 250 ms.
    EXPECT_LE(idlePaints, (500 + 3 * 250) / 17 + 4);
    EXPECT_EQ(3u, keepalive.stats().refreshes);
}

// ---------------------------------------------------------------------------
// Startup safety

TEST(DeckLabVSyncTest, WatchdogFallsBackWhenNoFrameSwappedArrives) {
    {
        // A driver that never delivers frameSwapped: 3 s of exposed time.
        VSyncLockWatchdog watchdog;
        qint64 elapsed = 0;
        while (watchdog.tick(100, true, true) == VSyncLockWatchdog::State::Waiting) {
            elapsed += 100;
            ASSERT_LT(elapsed, 10000);
        }
        EXPECT_EQ(VSyncLockWatchdog::State::FallBack, watchdog.state());
        EXPECT_GE(elapsed + 100, VSyncLockWatchdog::kDeadlineMillis);
        EXPECT_LE(elapsed + 100, VSyncLockWatchdog::kDeadlineMillis + 100);
        EXPECT_EQ(0u, watchdog.framesSwapped());
    }
    {
        // Skin loading blocks the GUI thread for 8 s: one late tick counts
        // 250 ms only, so the loop still gets its chance afterwards.
        VSyncLockWatchdog watchdog;
        EXPECT_EQ(VSyncLockWatchdog::State::Waiting, watchdog.tick(8000, true, true));
        EXPECT_EQ(VSyncLockWatchdog::kMaxTickMillis, watchdog.exposedMillis());
        for (int i = 0; i < 5; ++i) {
            watchdog.onFrameSwapped();
            EXPECT_EQ(VSyncLockWatchdog::State::Waiting, watchdog.tick(100, true, true));
        }
        // Locked: stays locked whatever comes later.
        EXPECT_EQ(VSyncLockWatchdog::State::Locked, watchdog.tick(100, true, false));
        EXPECT_EQ(VSyncLockWatchdog::State::Locked, watchdog.tick(5000, true, true));
    }
    {
        // A window that never gets exposed waits longer, then gives up too.
        VSyncLockWatchdog watchdog;
        qint64 elapsed = 0;
        while (watchdog.tick(100, false, true) == VSyncLockWatchdog::State::Waiting) {
            elapsed += 100;
            ASSERT_LT(elapsed, 20000);
        }
        EXPECT_GE(elapsed + 100, VSyncLockWatchdog::kHiddenDeadlineMillis);
        EXPECT_EQ(0, watchdog.exposedMillis());
    }
    {
        // Unstable frameSwapped (no vblank pacing): init keeps resetting.
        VSyncPll pll;
        VSyncLockWatchdog watchdog;
        Noise noise(9);
        double t = 0.0;
        qint64 elapsedMillis = 0;
        VSyncLockWatchdog::State state = VSyncLockWatchdog::State::Waiting;
        while (state == VSyncLockWatchdog::State::Waiting) {
            for (int i = 0; i < 20; ++i) {
                t += noise.uniform(1000.0, 9000.0);
                pll.addSample(t, false);
                watchdog.onFrameSwapped();
            }
            elapsedMillis += 100;
            state = watchdog.tick(100, true, pll.initializing());
            ASSERT_LT(elapsedMillis, 10000);
        }
        EXPECT_EQ(VSyncLockWatchdog::State::FallBack, state);
        EXPECT_TRUE(pll.initializing());
        EXPECT_GT(pll.stats().initResets, 10u);
    }
}

// ---------------------------------------------------------------------------
// Closed loop: VSyncThread schedule + factory keepalive + VSyncPll against a
// model of the X11 swap path (Mesa DRI3, copy-mode Present, swap interval 1,
// two back buffers per window). A swap is shown at the first vblank after it,
// or one after the window's previous pending swap; rendering waits until one of
// the two back buffers is free. The shared window's update() paints 5 ms later
// (Qt's xcb update timer). GUI work, jitter and stalls delay events.

struct ClosedLoopConfig {
    double period = kMeasuredPeriod;
    double seconds = 100.0;
    std::vector<std::pair<double, double>> play = {{2e6, 30e6}, {90e6, 100e6}};
    std::vector<std::pair<double, double>> stalls; // start, duration (us)
    double jitter = 300.0;
    double workPlay = 3000.0;
    double workIdle = 150.0;
    // Moves every vblank by vblankShift from vblankShiftAt on (during an idle
    // stretch): the freewheeled estimate is then that far off at the resume.
    double vblankShiftAt = -1.0;
    double vblankShift = 0.0;
    std::uint64_t seed = 3;
};

struct PlayWindowResult {
    int frames = 0;
    int repeats = 0;       // vblanks without a new waveform frame
    int queued = 0;        // frames shown later than the vblank after their swap
    double maxJump = 0.0;  // largest frame-to-frame change of (shown - predicted)
    double firstJumps = 0.0; // the same over the first 30 frames of the window
};

struct ClosedLoopResult {
    std::vector<PlayWindowResult> windows;
    double finalPeriod = 0.0;
    int idlePaints = 0; // keepalive paints more than 600 ms into an idle stretch
    int paints = 0;
    quint64 relocks = 0;
};

class SwapChain {
  public:
    SwapChain(double period, double phase)
            : m_period(period), m_phase(phase) {
    }
    void setPhase(double phase) {
        m_phase = phase;
    }
    long long vblankAfter(double t) const {
        return static_cast<long long>(std::floor((t - m_phase) / m_period)) + 1;
    }
    double vblankTime(long long n) const {
        return m_phase + static_cast<double>(n) * m_period;
    }
    /// When rendering can start: a back buffer must be free.
    double acquire(double t) {
        prune(t);
        if (m_busyUntil.size() < 2) {
            return t;
        }
        return *std::min_element(m_busyUntil.begin(), m_busyUntil.end());
    }
    /// The vblank that shows a swap made at t.
    long long swap(double t) {
        const long long target = std::max(vblankAfter(t), m_lastTarget + 1);
        m_lastTarget = target;
        prune(t);
        m_busyUntil.push_back(vblankTime(target));
        return target;
    }

  private:
    void prune(double t) {
        m_busyUntil.erase(std::remove_if(m_busyUntil.begin(),
                                  m_busyUntil.end(),
                                  [t](double until) { return until <= t; }),
                m_busyUntil.end());
    }
    double m_period;
    double m_phase;
    std::vector<double> m_busyUntil;
    long long m_lastTarget = -(1LL << 40);
};

ClosedLoopResult runClosedLoop(const ClosedLoopConfig& config) {
    enum class Kind {
        Thread, // VSyncThread::runPLL computes its next wake-up
        Slot,   // VSyncThread wakes, GUI runs swapAndRender()
        Paint,  // GUI paints the shared window (update() + 5 ms)
    };
    struct Event {
        double time;
        long seq;
        Kind kind;
        bool operator>(const Event& other) const {
            return time != other.time ? time > other.time : seq > other.seq;
        }
    };
    std::priority_queue<Event, std::vector<Event>, std::greater<>> events;
    long seq = 0;
    const auto push = [&](double time, Kind kind) {
        events.push({time, seq++, kind});
    };

    Noise noise(config.seed);
    const double vblankPhase = 1234.5;
    SwapChain shared(config.period, vblankPhase);
    SwapChain waveform(config.period, vblankPhase);
    bool shifted = false;
    VSyncPll pll;
    VSyncKeepalive keepalive;

    const auto playing = [&](double t) {
        return std::any_of(config.play.begin(), config.play.end(), [t](const auto& range) {
            return t >= range.first && t < range.second;
        });
    };
    const auto stall = [&](double t) {
        for (const auto& [start, duration] : config.stalls) {
            if (t >= start && t < start + 20000.0) {
                return duration;
            }
        }
        return 0.0;
    };
    const auto deepIdle = [&](double t) {
        if (t < (VSyncKeepalive::kSettleMillis + 1000) * 1000.0) {
            return false; // startup: lock, then the settle time
        }
        return std::none_of(config.play.begin(), config.play.end(), [t](const auto& range) {
            return t >= range.first && t < range.second + 600000.0;
        });
    };

    double guiFree = 0.0;
    bool updatePending = false;
    bool pllPending = false;
    bool renderPassActive = false;
    bool hasLastSlot = false;
    double lastSlotStart = 0.0;
    double timerRestart = -1.0;
    double offset = 0.0;
    double offsetAdjustedAt = 0.0;
    bool hasPendingFrame = false;
    double pendingPredicted = 0.0;
    struct Shown {
        double swapAt;
        double error; // shown at - predicted
        long long vblank;
        bool queued;
    };
    std::vector<Shown> shown;
    ClosedLoopResult result;

    const auto requestUpdate = [&](double t) {
        if (!updatePending) {
            updatePending = true;
            push(t + 5000.0, Kind::Paint);
        }
    };

    push(0.0, Kind::Thread);
    requestUpdate(0.0);
    const double end = config.seconds * 1e6;
    while (!events.empty()) {
        const Event event = events.top();
        events.pop();
        const double t = event.time;
        if (t > end) {
            break;
        }
        if (!shifted && config.vblankShiftAt >= 0.0 && t >= config.vblankShiftAt) {
            shifted = true;
            shared.setPhase(vblankPhase + config.vblankShift);
            waveform.setPhase(vblankPhase + config.vblankShift);
        }
        switch (event.kind) {
        case Kind::Thread: {
            const double vblankAnchor = pll.vblankEstimate() +
                    std::round((pll.phaseOut() - pll.vblankEstimate()) / pll.deltaOut()) *
                            pll.deltaOut();
            const double wake = VSyncPll::nextSlotMicros(t,
                    pll.phaseOut(),
                    pll.deltaOut(),
                    offset + VSyncPll::kSlotLeadMicros,
                    vblankAnchor,
                    !pll.initializing() && hasLastSlot,
                    lastSlotStart);
            push(std::max(wake, t), Kind::Slot);
            break;
        }
        case Kind::Slot: {
            const double sinceLastSwap = timerRestart < 0 ? 0.0 : t - timerRestart;
            timerRestart = t;
            if (pll.initializing() && !pllPending) {
                push(t + 5.0, Kind::Thread);
                break;
            }
            double s = std::max(t, guiFree) + noise.uniform(0.0, config.jitter);
            s += stall(s);
            lastSlotStart = s;
            hasLastSlot = true;
            // swapAndRender(): keepalive on the last render pass...
            if (keepalive.onSlot(static_cast<qint64>(s / 1000.0), renderPassActive)) {
                requestUpdate(s);
                if (deepIdle(s)) {
                    ++result.idlePaints;
                }
            }
            // ...swapSelf()...
            if (hasPendingFrame) {
                const long long vblank = waveform.swap(s);
                shown.push_back({s,
                        waveform.vblankTime(vblank) - pendingPredicted,
                        vblank,
                        vblank > waveform.vblankAfter(s)});
                hasPendingFrame = false;
            }
            // ...renderSelf(): a playing deck renders its next frame for the
            // position at the next slot (fromTimerToNextSync).
            double work = config.workIdle;
            renderPassActive = playing(s);
            if (renderPassActive) {
                const double freeAt = waveform.acquire(s);
                work = (freeAt - s) + config.workPlay;
                hasPendingFrame = true;
                pendingPredicted = t + pll.deltaOut();
            }
            guiFree = s + work;
            pllPending = false;
            push(guiFree, Kind::Thread);
            // Upstream drop counting and offset adaptation.
            if (!pll.initializing() && sinceLastSwap > pll.deltaOut() * 1.5) {
                const double sinceAdjust = t - offsetAdjustedAt;
                if (sinceAdjust > 100000.0) {
                    if (sinceAdjust < 1000000.0) {
                        offset = std::fmod(offset + pll.deltaOut() / 8.0, pll.deltaOut());
                    }
                    offsetAdjustedAt = t;
                }
            }
            break;
        }
        case Kind::Paint: {
            double s = std::max(t, guiFree) + noise.uniform(0.0, config.jitter / 3.0);
            s += stall(s);
            const double freeAt = shared.acquire(s);
            const double swapped = freeAt + 200.0;
            shared.swap(swapped);
            updatePending = false;
            guiFree = swapped + 20.0;
            ++result.paints;
            // slotFrameSwapped()
            const bool blocked = swapped - s >= 2000.0;
            const auto pllResult = pll.addSample(guiFree, blocked);
            if (pllResult == VSyncPll::Result::Tracked ||
                    pllResult == VSyncPll::Result::Relocked) {
                pllPending = true;
            }
            if (pll.initializing()) {
                requestUpdate(guiFree);
            } else if (keepalive.onFrameSwapped(blocked)) {
                requestUpdate(guiFree);
            }
            break;
        }
        }
    }

    for (const auto& [from, to] : config.play) {
        PlayWindowResult window;
        const Shown* pPrevious = nullptr;
        for (const Shown& frame : shown) {
            if (frame.swapAt < from || frame.swapAt >= to) {
                continue;
            }
            ++window.frames;
            if (frame.queued) {
                ++window.queued;
            }
            if (pPrevious) {
                if (frame.vblank - pPrevious->vblank > 1) {
                    ++window.repeats;
                }
                const double jump = std::abs(frame.error - pPrevious->error);
                window.maxJump = std::max(window.maxJump, jump);
                if (window.frames <= 30) {
                    window.firstJumps = std::max(window.firstJumps, jump);
                }
            }
            pPrevious = &frame;
        }
        result.windows.push_back(window);
    }
    result.finalPeriod = pll.deltaOut();
    result.relocks = pll.stats().relocks;
    return result;
}

void expectSmooth(const ClosedLoopResult& result, const std::string& what) {
    for (std::size_t i = 0; i < result.windows.size(); ++i) {
        const PlayWindowResult& window = result.windows[i];
        EXPECT_GT(window.frames, 500) << what << " window " << i;
        EXPECT_EQ(0, window.repeats) << what << " window " << i;
        EXPECT_EQ(0, window.queued) << what << " window " << i;
        // The shown position minus the predicted one changes by microseconds
        // from frame to frame, also over the first frames after the resume:
        // no visible jump (a hitch would be a whole frame, ~16,600 us).
        EXPECT_LT(window.maxJump, 500.0) << what << " window " << i;
        EXPECT_LT(window.firstJumps, 500.0) << what << " window " << i;
    }
}

TEST(DeckLabVSyncTest, ClosedLoopLocksIdlesAndResumesWithoutAJump) {
    for (const double period : {kNominalPeriod, kMeasuredPeriod}) {
        ClosedLoopConfig config;
        config.period = period;
        const ClosedLoopResult result = runClosedLoop(config);
        const std::string what = "period " + std::to_string(period);
        expectSmooth(result, what);
        EXPECT_NEAR(period, result.finalPeriod, 0.05) << what;
        EXPECT_EQ(0, result.idlePaints) << what << ": keepalive must stop when idle";
        // Startup, play 2-30 s and 90-100 s (38 s at 60 Hz plus refills),
        // nothing during the 60 s idle stretch.
        EXPECT_LT(result.paints, 40 * 61) << what;
        EXPECT_EQ(1u, result.relocks) << what << ": the first play falls into the settle time";
    }
}

TEST(DeckLabVSyncTest, ClosedLoopStartsIdleAndPlaysLater) {
    // Locked at startup, idle through the settle time and beyond, then play:
    // the freewheel after the settle is good enough for a smooth start.
    ClosedLoopConfig config;
    config.seconds = 75.0;
    config.play = {{50e6, 75e6}};
    const ClosedLoopResult result = runClosedLoop(config);
    expectSmooth(result, "idle start");
    EXPECT_NEAR(kMeasuredPeriod, result.finalPeriod, 0.05);
    EXPECT_EQ(1u, result.relocks);
}

TEST(DeckLabVSyncTest, ClosedLoopResumesAfterAFreewheelError) {
    // The vblank moves while idle, so the freewheeled estimate is off at the
    // resume. Up to the slot lead (10 minutes of idle after a normal lock,
    // see FreewheelPhaseErrorAfterIdleStaysUnderHalfAFrame), the slots keep
    // running where they were and the estimate is pulled back gradually: no
    // repeated, queued or jumping frame.
    for (const double shift : {1400.0, -1400.0, 700.0, -700.0, -3000.0}) {
        ClosedLoopConfig config;
        config.vblankShiftAt = 60e6;
        config.vblankShift = shift;
        const ClosedLoopResult result = runClosedLoop(config);
        const std::string what = "vblank shift " + std::to_string(shift);
        expectSmooth(result, what);
        EXPECT_NEAR(kMeasuredPeriod, result.finalPeriod, 0.05) << what;
        EXPECT_EQ(1u, result.relocks) << what;
    }
}

TEST(DeckLabVSyncTest, ClosedLoopResumesAfterTheFreewheelDriftedFar) {
    // An error beyond the lead that puts the slots just before the vblank
    // (hours of idle on a poor period estimate) costs at most one repeated
    // frame while the slots cross back, never a queued frame, and the lock
    // recovers.
    for (const double shift : {3000.0, 5000.0, 7000.0, 8000.0, -7000.0, -8000.0}) {
        ClosedLoopConfig config;
        config.vblankShiftAt = 60e6;
        config.vblankShift = shift;
        const ClosedLoopResult result = runClosedLoop(config);
        const std::string what = "vblank shift " + std::to_string(shift);
        ASSERT_EQ(2u, result.windows.size());
        EXPECT_LE(result.windows[1].repeats, 1) << what;
        EXPECT_EQ(0, result.windows[1].queued) << what;
        std::printf("DECKLAB_TEST resume shift=%.0fus repeats=%d queued=%d max_jump=%.0fus\n",
                shift,
                result.windows[1].repeats,
                result.windows[1].queued,
                result.windows[1].maxJump);
        EXPECT_NEAR(kMeasuredPeriod, result.finalPeriod, 0.05) << what;
    }
}

TEST(DeckLabVSyncTest, ClosedLoopRecoversFromStallsAndSlowFrames) {
    ClosedLoopConfig config;
    config.workPlay = 9000.0; // four decks on a slow GPU
    config.stalls = {{12e6, 40000.0}, {95e6, 25000.0}};
    const ClosedLoopResult result = runClosedLoop(config);
    // A stall longer than a frame repeats frames (nothing can draw), but the
    // lock comes back at once and no frame ends up queued.
    for (const PlayWindowResult& window : result.windows) {
        EXPECT_LE(window.repeats, 3);
        EXPECT_EQ(0, window.queued);
    }
    EXPECT_NEAR(kMeasuredPeriod, result.finalPeriod, 0.05);
    EXPECT_EQ(0, result.idlePaints);
}

TEST(DeckLabVSyncTest, ClosedLoopHandlesHighJitter) {
    ClosedLoopConfig config;
    config.jitter = 1500.0;
    config.seed = 17;
    const ClosedLoopResult result = runClosedLoop(config);
    expectSmooth(result, "jitter 1.5 ms");
    EXPECT_NEAR(kMeasuredPeriod, result.finalPeriod, 0.1);
}

// ---------------------------------------------------------------------------
// The real VSyncThread without GL: PLL samples come from this test thread.

TEST(DeckLabVSyncTest, ThreadEmitsSlotsOnlyAfterLockAndFreewheels) {
    VSyncThread thread(nullptr, VSyncThread::ST_PLL);
    thread.setSyncIntervalTimeMicros(16666);
    std::atomic<int> slotCount(0);
    QObject::connect(
            &thread,
            &VSyncThread::vsyncSwapAndRender,
            &thread,
            [&thread, &slotCount] {
                thread.markSlotStart();
                slotCount++;
                thread.vsyncSlotFinished();
            },
            Qt::DirectConnection);
    thread.start();

    using Clock = std::chrono::steady_clock;
    const auto period = std::chrono::microseconds(16667);
    auto next = Clock::now() + period;
    // Init: no slots until 20 stable deltas.
    for (int i = 0; i < 10; ++i) {
        std::this_thread::sleep_until(next);
        next += period;
        thread.updatePLL(true);
    }
    EXPECT_TRUE(thread.pllInitializing());
    EXPECT_EQ(0, slotCount.load());
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (thread.pllInitializing() && Clock::now() < deadline) {
        std::this_thread::sleep_until(next);
        next += period;
        thread.updatePLL(true);
    }
    ASSERT_FALSE(thread.pllInitializing()) << "no lock within 10 s of paced samples";
    const VSyncThread::PllSnapshot locked = thread.pllSnapshot();
    EXPECT_NEAR(16667.0, locked.periodMicros, 1500.0);

    // Locked: slots at the measured rate; then no samples (idle) and the
    // loop keeps slotting on its own (freewheel).
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_until(next);
        next += period;
        thread.updatePLL(true);
    }
    const int slotsLocked = slotCount.load();
    EXPECT_GT(slotsLocked, 15);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const int freewheelSlots = slotCount.load() - slotsLocked;
    EXPECT_GT(freewheelSlots, 15) << "slots continue without samples";
    EXPECT_LT(freewheelSlots, 45);
    EXPECT_EQ(VSyncThread::ST_PLL, thread.vsyncMode());
    EXPECT_FALSE(thread.pllSnapshot().fellBack);
}

TEST(DeckLabVSyncTest, ThreadFallsBackToTheTimerLoop) {
    VSyncThread thread(nullptr, VSyncThread::ST_PLL);
    thread.setSyncIntervalTimeMicros(16666);
    std::atomic<int> pllSlots(0);
    std::atomic<int> timerRenders(0);
    std::atomic<int> timerSwaps(0);
    QObject::connect(
            &thread,
            &VSyncThread::vsyncSwapAndRender,
            &thread,
            [&thread, &pllSlots] {
                pllSlots++;
                thread.vsyncSlotFinished();
            },
            Qt::DirectConnection);
    QObject::connect(
            &thread,
            &VSyncThread::vsyncRender,
            &thread,
            [&thread, &timerRenders] {
                timerRenders++;
                thread.vsyncSlotFinished();
            },
            Qt::DirectConnection);
    QObject::connect(
            &thread,
            &VSyncThread::vsyncSwap,
            &thread,
            [&thread, &timerSwaps] {
                timerSwaps++;
                thread.vsyncSlotFinished();
            },
            Qt::DirectConnection);
    thread.start();

    // No frameSwapped at all: the loop never locks and never slots.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_TRUE(thread.pllInitializing());
    EXPECT_EQ(0, pllSlots.load());
    EXPECT_EQ(VSyncThread::ST_PLL, thread.vsyncMode());

    thread.fallBackToTimer();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((thread.vsyncMode() != VSyncThread::ST_TIMER || timerSwaps.load() < 10) &&
            std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(VSyncThread::ST_TIMER, thread.vsyncMode());
    EXPECT_GE(timerSwaps.load(), 10);
    EXPECT_GE(timerRenders.load(), 10);
    EXPECT_EQ(0, pllSlots.load());
    EXPECT_TRUE(thread.pllSnapshot().fellBack);
}

} // namespace
