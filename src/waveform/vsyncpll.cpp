// SPDX-License-Identifier: GPL-2.0-or-later
#include "waveform/vsyncpll.h"

#include <QtDebug>
#include <algorithm>
#include <atomic>
#include <cmath>

namespace mixxx {
namespace decklab {

VSyncOverride parseVSyncOverride(const QString& value, bool* pValid) {
    const QString trimmed = value.trimmed();
    if (pValid) {
        *pValid = true;
    }
    if (trimmed.isEmpty()) {
        return VSyncOverride::None;
    }
    if (trimmed.compare(QLatin1String("pll"), Qt::CaseInsensitive) == 0) {
        return VSyncOverride::Pll;
    }
    if (trimmed.compare(QLatin1String("timer"), Qt::CaseInsensitive) == 0) {
        return VSyncOverride::Timer;
    }
    if (pValid) {
        *pValid = false;
    }
    return VSyncOverride::None;
}

VSyncOverride vsyncOverrideFromEnvironment() {
    const QString value = qEnvironmentVariable("DECKLAB_VSYNC");
    bool valid = true;
    const VSyncOverride result = parseVSyncOverride(value, &valid);
    if (!valid) {
        static std::atomic<bool> s_warned(false);
        if (!s_warned.exchange(true)) {
            qWarning().noquote() << QStringLiteral(
                    "DECKLAB_VSYNC=%1 is not pll or timer; using [Waveform] VSync")
                                            .arg(value);
        }
    }
    return result;
}

int effectiveVSyncMode(int configuredMode, VSyncOverride vsyncOverride) {
    switch (vsyncOverride) {
    case VSyncOverride::Pll:
        return kVSyncModePll;
    case VSyncOverride::Timer:
        return kVSyncModeTimer;
    case VSyncOverride::None:
        break;
    }
    return configuredMode;
}

QString vsyncOverrideName(VSyncOverride vsyncOverride) {
    switch (vsyncOverride) {
    case VSyncOverride::Pll:
        return QStringLiteral("pll");
    case VSyncOverride::Timer:
        return QStringLiteral("timer");
    case VSyncOverride::None:
        break;
    }
    return QString();
}

VSyncPll::Result VSyncPll::addSample(double phaseInMicros, bool vblankReferenced) {
    if (initializing()) {
        // Upstream init: the average of kNumStableDeltasRequired consecutive
        // deltas that each stay within kInitToleranceMicros of the running
        // average.
        const double delta = phaseInMicros - m_phaseOut;
        m_phaseOut = phaseInMicros;
        m_initSum += delta;
        m_initCount++;
        const double average = m_initSum / static_cast<double>(m_initCount);
        if (std::abs(delta - average) > kInitToleranceMicros) {
            m_initSum = 0.0;
            m_initCount = 0;
            m_stats.initResets++;
        }
        if (m_initCount == kNumStableDeltasRequired) {
            m_deltaOut = average;
        }
        m_hasLastSample = true;
        m_lastSampleMicros = phaseInMicros;
        return Result::Initializing;
    }
    if (!vblankReferenced) {
        m_stats.unreferenced++;
        return Result::Unreferenced;
    }
    if (m_hasLastSample && phaseInMicros - m_lastSampleMicros < 0.5 * m_deltaOut) {
        m_stats.duplicates++;
        return Result::Duplicate;
    }
    const double framesSinceLast = m_hasLastSample
            ? (phaseInMicros - m_lastSampleMicros) / m_deltaOut
            : 0.0;
    const bool gap = !m_hasLastSample || framesSinceLast > kGapFrames;

    // Predict the next vblank, then wrap the error to the nearest frame. After
    // a gap this skips the whole freewheeled stretch. Upstream wraps positive
    // errors only; within half a frame both are the same.
    m_phaseOut += m_deltaOut;
    double error = phaseInMicros - m_phaseOut;
    const double frames = std::round(error / m_deltaOut);
    m_phaseOut += frames * m_deltaOut;
    error -= frames * m_deltaOut;
    m_lastPhaseError = error;
    m_hasLastSample = true;
    m_lastSampleMicros = phaseInMicros;

    bool snap = false;
    if (gap) {
        // Resume after freewheeling: the slots kept running on the estimate,
        // so pull it back gradually (a step would show as a jump of the
        // waveform) and trust the period measured before the gap more than
        // these first samples.
        m_relockRemaining = kRelockSamples;
        m_stats.relocks++;
        // Unless the slots, kSlotLeadMicros after the estimate, now sit before
        // the real vblank: pulled in gradually they would dither across it
        // for several frames, each a repeated frame. Cross once instead.
        snap = error > kSlotLeadMicros;
    }
    if (snap) {
        m_phaseOut += error;
        m_stats.snaps++;
        m_relockRemaining--;
    } else if (m_relockRemaining > 0) {
        m_phaseOut += kRelockAlpha * error;
        m_relockRemaining--;
    } else {
        // Upstream loop filter, inspired by https://liquidsdr.org/blog/pll-simple-howto/
        m_phaseOut += kAlpha * error;
        m_deltaOut += kBeta * error;
    }
    if (gap) {
        return Result::Relocked;
    }
    m_stats.tracked++;
    return Result::Tracked;
}

double VSyncPll::predictedVsyncNear(double t) const {
    if (m_deltaOut <= 0.0) {
        return m_phaseOut;
    }
    return m_phaseOut + std::round((t - m_phaseOut) / m_deltaOut) * m_deltaOut;
}

// static
double VSyncPll::nextSlotMicros(double now,
        double anchor,
        double period,
        double offset,
        double vblankAnchor,
        bool hasLastSlot,
        double lastSlotStart,
        int* pSkipped) {
    if (pSkipped) {
        *pSkipped = 0;
    }
    if (!(period > 0.0)) {
        return now;
    }
    const double base = anchor + offset;
    double wake = base + std::ceil((now - base) / period) * period;
    if (hasLastSlot) {
        const double intervalStart = vblankAnchor - kBoundaryMarginMicros;
        const auto interval = [&](double t) {
            return std::floor((t - intervalStart) / period);
        };
        const double lastInterval = interval(lastSlotStart);
        // Bounded: a slot start can only be a few periods in the past.
        for (int i = 0; i < 4 && interval(wake) <= lastInterval; ++i) {
            wake += period;
            if (pSkipped) {
                ++*pSkipped;
            }
        }
    }
    return wake;
}

bool VSyncKeepalive::onSlot(qint64 nowMillis, bool active) {
    if (!m_started) {
        // The first slot after the loop locked.
        m_started = true;
        m_runUntilMillis = nowMillis + kSettleMillis;
        m_nextRefreshMillis = nowMillis + kIdleRefreshIntervalMillis;
    }
    if (active) {
        m_runUntilMillis = std::max(m_runUntilMillis, nowMillis + kIdleStopMillis);
        m_nextRefreshMillis = nowMillis + kIdleRefreshIntervalMillis;
    } else if (nowMillis >= m_nextRefreshMillis) {
        // Idle refresh: a few frames of vblank samples keep the freewheeled
        // phase within a fraction of a millisecond.
        m_runUntilMillis = std::max(m_runUntilMillis, nowMillis + kRefreshMillis);
        m_nextRefreshMillis = nowMillis + kIdleRefreshIntervalMillis;
        m_stats.refreshes++;
    }
    const bool wanted = nowMillis < m_runUntilMillis;
    if (wanted && !m_running) {
        m_stats.starts++;
    } else if (!wanted && m_running) {
        m_stats.stops++;
    }
    m_running = wanted;
    m_refillBudget = kRefillPaintsPerSlot;
    if (m_running) {
        m_stats.slotPaints++;
    }
    return m_running;
}

bool VSyncKeepalive::onFrameSwapped(bool blocked) {
    if (!m_running || blocked || m_refillBudget <= 0) {
        return false;
    }
    m_refillBudget--;
    m_stats.refillPaints++;
    return true;
}

VSyncLockWatchdog::State VSyncLockWatchdog::tick(
        qint64 elapsedMillis, bool exposed, bool initializing) {
    if (m_state != State::Waiting) {
        return m_state;
    }
    if (!initializing) {
        m_state = State::Locked;
        return m_state;
    }
    const qint64 counted = qBound<qint64>(0, elapsedMillis, kMaxTickMillis);
    m_totalMillis += counted;
    if (exposed) {
        m_exposedMillis += counted;
    }
    if (m_exposedMillis >= kDeadlineMillis || m_totalMillis >= kHiddenDeadlineMillis) {
        m_state = State::FallBack;
    }
    return m_state;
}

} // namespace decklab
} // namespace mixxx
