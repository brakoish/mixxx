#pragma once

#include <QtGlobal>
#include <cstring>

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

namespace mixxx {
namespace decklab {

/// DECKLAB_RENDER_GATE=0 disables render-on-demand (field kill switch).
bool renderGateEnabledFromEnvironment();
/// DECKLAB_PERF_LOG=1 logs render/skip counters every 10 s.
bool perfLogEnabledFromEnvironment();

} // namespace decklab
} // namespace mixxx
