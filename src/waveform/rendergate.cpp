#include "waveform/rendergate.h"

#include <QtGlobal>

RenderGate::Decision RenderGate::evaluate(const Frame& frame) {
    if (!frame.visible) {
        // A widget that becomes visible again may show stale content until
        // its next swap, so the first visible frame always renders.
        m_forced = true;
        ++m_stats.hidden;
        return Decision::Hidden;
    }

    Decision decision;
    if (!m_enabled) {
        decision = Decision::Ungated;
        ++m_stats.ungated;
    } else if (m_forced || frame.forced) {
        decision = Decision::Forced;
        ++m_stats.forced;
    } else if (frame.moving) {
        decision = Decision::Moving;
        ++m_stats.moving;
    } else if (!m_hasKey || frame.stateKey != m_lastKey) {
        decision = Decision::Changed;
        ++m_stats.changed;
    } else if (frame.nowMillis - m_lastRenderMillis >= m_safetyIntervalMillis ||
            frame.nowMillis < m_lastRenderMillis) {
        decision = Decision::Safety;
        ++m_stats.safety;
    } else {
        ++m_stats.skipped;
        return Decision::Skip;
    }

    m_forced = false;
    m_hasKey = true;
    m_lastKey = frame.stateKey;
    m_lastRenderMillis = frame.nowMillis;
    return decision;
}

namespace mixxx {
namespace decklab {

bool renderGateEnabledFromEnvironment() {
    bool ok = false;
    const int value = qEnvironmentVariableIntValue("DECKLAB_RENDER_GATE", &ok);
    return !ok || value != 0;
}

bool perfLogEnabledFromEnvironment() {
    bool ok = false;
    const int value = qEnvironmentVariableIntValue("DECKLAB_PERF_LOG", &ok);
    return ok && value != 0;
}

} // namespace decklab
} // namespace mixxx
