#include "waveform/rendergate.h"

#include <QtGlobal>
#include <algorithm>

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

int RendererTimings::addCategory(const char* name) {
    for (std::size_t i = 0; i < m_categories.size(); ++i) {
        if (std::strcmp(m_categories[i].name, name) == 0) {
            return static_cast<int>(i);
        }
    }
    if (m_categories.size() >= static_cast<std::size_t>(kMaxCategories)) {
        return -1;
    }
    m_categories.push_back(Category{name, 0, {}});
    m_categories.back().samples.reserve(1024);
    m_total.reserve(1024);
    return static_cast<int>(m_categories.size()) - 1;
}

void RendererTimings::beginFrame() {
    for (auto& category : m_categories) {
        category.frameNanos = 0;
    }
}

void RendererTimings::endFrame() {
    qint64 total = 0;
    for (auto& category : m_categories) {
        category.samples.push_back(category.frameNanos);
        total += category.frameNanos;
    }
    m_total.push_back(total);
}

// static
qint64 RendererTimings::percentile95(
        const std::vector<qint64>& samples, std::vector<qint64>* pScratch) {
    if (samples.empty()) {
        return -1;
    }
    pScratch->assign(samples.begin(), samples.end());
    // Nearest rank: the smallest value with at least 95% of the samples at
    // or below it.
    const std::size_t rank = (samples.size() * 95 + 99) / 100;
    const auto nth = pScratch->begin() + static_cast<std::ptrdiff_t>(rank - 1);
    std::nth_element(pScratch->begin(), nth, pScratch->end());
    return *nth;
}

namespace {
qint64 meanOf(const std::vector<qint64>& samples) {
    if (samples.empty()) {
        return -1;
    }
    qint64 sum = 0;
    for (const qint64 value : samples) {
        sum += value;
    }
    return sum / static_cast<qint64>(samples.size());
}

QString millis(qint64 nanos) {
    return QString::number(static_cast<double>(nanos) / 1e6, 'f', 3);
}
} // namespace

qint64 RendererTimings::meanNanos(int category) const {
    if (category < 0 || category >= static_cast<int>(m_categories.size())) {
        return -1;
    }
    return meanOf(m_categories[static_cast<std::size_t>(category)].samples);
}

qint64 RendererTimings::p95Nanos(int category) const {
    if (category < 0 || category >= static_cast<int>(m_categories.size())) {
        return -1;
    }
    return percentile95(m_categories[static_cast<std::size_t>(category)].samples, &m_scratch);
}

qint64 RendererTimings::totalMeanNanos() const {
    return meanOf(m_total);
}

qint64 RendererTimings::totalP95Nanos() const {
    return percentile95(m_total, &m_scratch);
}

QString RendererTimings::report() const {
    QString result;
    for (std::size_t i = 0; i < m_categories.size(); ++i) {
        const auto& category = m_categories[i];
        bool used = false;
        for (const qint64 value : category.samples) {
            if (value > 0) {
                used = true;
                break;
            }
        }
        if (!used) {
            continue;
        }
        result += QLatin1String(category.name);
        result += QLatin1Char('=');
        result += millis(meanNanos(static_cast<int>(i)));
        result += QLatin1Char('/');
        result += millis(p95Nanos(static_cast<int>(i)));
        result += QLatin1Char(' ');
    }
    result += QStringLiteral("total=");
    result += millis(std::max<qint64>(0, totalMeanNanos()));
    result += QLatin1Char('/');
    result += millis(std::max<qint64>(0, totalP95Nanos()));
    return result;
}

void RendererTimings::reset() {
    for (auto& category : m_categories) {
        category.samples.clear();
        category.frameNanos = 0;
    }
    m_total.clear();
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

bool waveformCacheEnabledFromEnvironment() {
    bool ok = false;
    const int value = qEnvironmentVariableIntValue("DECKLAB_WAVEFORM_CACHE", &ok);
    return !ok || value != 0;
}

} // namespace decklab
} // namespace mixxx
