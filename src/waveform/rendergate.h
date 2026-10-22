#pragma once

#include <QString>
#include <QtGlobal>
#include <cstring>
#include <vector>

/// Deck Lab render-on-demand support for scrolling waveforms.
///
/// Upstream renders and swaps every visible waveform at the configured frame
/// rate, even when its deck is paused. On the Pi every swap of a
/// QOpenGLWindow child crosses XWayland and the compositor, so a paused
/// two-deck layout kept the GUI thread at ~54% of a core. The factory now asks
/// a RenderGate per waveform whether the next frame can differ from the last
/// one that was swapped. All inputs are cheap: a few atomic ControlObject
/// reads and a 64-bit fingerprint of everything the renderers draw.

/// Order-sensitive 64-bit fingerprint of plain values. Each step is a
/// bijection of the running state for a fixed input, so changing a single input
/// value always changes the result. Several simultaneous changes collide only
/// with ~2^-64 probability, and the gate's safety refresh bounds even that.
class RenderStateHasher {
  public:
    void addWord(quint64 value) {
        m_state = (m_state ^ value) * kPrime;
    }
    void addInt(qint64 value) {
        addWord(static_cast<quint64>(value));
    }
    void addBool(bool value) {
        addWord(value ? 0x9e3779b97f4a7c15ULL : 0x7f4a7c159e3779b9ULL);
    }
    void addDouble(double value) {
        quint64 bits;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(bits));
        addWord(bits);
    }
    void addPointer(const void* pointer) {
        addWord(static_cast<quint64>(reinterpret_cast<quintptr>(pointer)));
    }
    quint64 value() const {
        return m_state;
    }

  private:
    static constexpr quint64 kPrime = 0x100000001b3ULL;
    quint64 m_state = 0xcbf29ce484222325ULL;
};

/// Pure decision logic (no Qt widgets) so it can be unit tested.
/// Not thread safe: owned and evaluated by the GUI thread.
class RenderGate {
  public:
    /// At most 2 Hz while paused and visible.
    static constexpr qint64 kDefaultSafetyIntervalMillis = 500;

    enum class Decision {
        Hidden,  ///< not visible: neither rendered nor counted as skipped
        Skip,    ///< nothing changed since the last swapped frame
        Forced,  ///< explicit request, first frame, re-shown, expose/resize
        Moving,  ///< deck playing/scratching or a renderer animates
        Changed, ///< drawn state fingerprint differs from the last frame
        Safety,  ///< periodic refresh while idle
        Ungated, ///< gate disabled: upstream behaviour, render every frame
    };

    struct Frame {
        bool visible = true;
        bool moving = false;
        bool forced = false;
        quint64 stateKey = 0;
        qint64 nowMillis = 0;
    };

    struct Stats {
        quint64 hidden = 0;
        quint64 skipped = 0;
        quint64 forced = 0;
        quint64 moving = 0;
        quint64 changed = 0;
        quint64 safety = 0;
        quint64 ungated = 0;
        quint64 rendered() const {
            return forced + moving + changed + safety + ungated;
        }
    };

    explicit RenderGate(qint64 safetyIntervalMillis = kDefaultSafetyIntervalMillis)
            : m_safetyIntervalMillis(safetyIntervalMillis) {
    }

    static bool renders(Decision decision) {
        return decision != Decision::Hidden && decision != Decision::Skip;
    }

    /// Decides for one frame. A rendering decision records the frame as the
    /// new reference, assuming the caller renders and swaps it.
    Decision evaluate(const Frame& frame);

    /// The next visible frame renders regardless of state (c).
    void requestRender() {
        m_forced = true;
    }

    /// false restores upstream behaviour: every visible frame renders.
    void setEnabled(bool enabled) {
        m_enabled = enabled;
    }
    bool isEnabled() const {
        return m_enabled;
    }

    const Stats& stats() const {
        return m_stats;
    }
    void resetStats() {
        m_stats = Stats();
    }

  private:
    bool m_enabled = true;
    // The first visible frame always renders.
    bool m_forced = true;
    bool m_hasKey = false;
    quint64 m_lastKey = 0;
    qint64 m_lastRenderMillis = 0;
    qint64 m_safetyIntervalMillis;
    Stats m_stats;
};

/// Deck Lab perf log (DECKLAB_PERF_LOG=1): CPU time that each renderer of a
/// waveform takes per rendered frame (preprocess plus GL command submission),
/// reported as mean and 95th percentile over the log window. Not thread safe;
/// owned by the GUI thread. Allocation free once the sample vectors reached
/// their working size.
class RendererTimings {
  public:
    static constexpr int kMaxCategories = 16;

    /// Registers a category and returns its index, or the index of an
    /// existing category with the same name, or -1 if the table is full.
    int addCategory(const char* name);
    int categoryCount() const {
        return static_cast<int>(m_categories.size());
    }

    void beginFrame();
    void add(int category, qint64 nanos) {
        if (category >= 0 && category < static_cast<int>(m_categories.size())) {
            m_categories[static_cast<std::size_t>(category)].frameNanos += nanos;
        }
    }
    void endFrame();

    int frames() const {
        return static_cast<int>(m_total.size());
    }
    /// Mean and 95th percentile of the per-frame total of one category, in
    /// nanoseconds, over the frames since the last reset(). -1 for none.
    qint64 meanNanos(int category) const;
    qint64 p95Nanos(int category) const;
    qint64 totalMeanNanos() const;
    qint64 totalP95Nanos() const;

    /// "signal=0.210/0.330 beat=0.021/0.040 ... total=0.400/0.520" in
    /// milliseconds (mean/p95). Categories that never took time are omitted.
    QString report() const;
    void reset();

    /// The 95th percentile (nearest rank) of a sample set, -1 if empty.
    static qint64 percentile95(const std::vector<qint64>& samples, std::vector<qint64>* pScratch);

  private:
    struct Category {
        const char* name;
        qint64 frameNanos;
        std::vector<qint64> samples;
    };
    std::vector<Category> m_categories;
    std::vector<qint64> m_total;
    mutable std::vector<qint64> m_scratch;
};

namespace mixxx {
namespace decklab {

/// DECKLAB_RENDER_GATE=0 disables render-on-demand (field kill switch).
bool renderGateEnabledFromEnvironment();
/// DECKLAB_PERF_LOG=1 logs render/skip counters every 10 s.
bool perfLogEnabledFromEnvironment();
/// DECKLAB_WAVEFORM_CACHE=0 restores the upstream RGB waveform renderer that
/// rebuilds its geometry every frame (field kill switch, pi4-perf).
bool waveformCacheEnabledFromEnvironment();

} // namespace decklab
} // namespace mixxx
