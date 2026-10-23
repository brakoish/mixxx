// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QtGlobal>
#include <algorithm>
#include <array>
#include <functional>

// Gesture rules for one deck's stem pads (stems workstream), kept apart from
// widgets and controls. Mute state is read and written through callbacks
// that the pads bind to Mixxx's [ChannelN_StemM],mute controls; nothing is
// cached here except what a held solo must restore.
//
// Per pad:
//  - a touch toggles that stem's mute at once, on touch-down, so a drop on
//    the one lands on the one;
//  - hold (kHoldMs) solos it while held: the other stems mute, and release
//    restores what was there before the touch (the touch-down toggle too);
//  - a second touch on the same pad within kDoubleTapMs of the first
//    unmutes every stem instead of toggling again.
//
// Every press is tied to a context: the surface's interaction epoch (page,
// layer and layout changes) and the deck's load serial. A hold in another
// context does nothing; a solo is always undone on release or cancel.
class DeckLabStemGestures {
  public:
    static constexpr int kStems = 4;
    static constexpr qint64 kHoldMs = 300;
    static constexpr qint64 kDoubleTapMs = 400;
    struct Context {
        quint64 epoch = 0;
        quint64 load = 0;
        bool operator==(const Context&) const = default;
    };
    enum class Result {
        None,
        Ignored,     // refused, stale, cancelled or out of range: no control writes
        Toggled,     // a touch flipped one mute
        RestoredAll, // a double tap unmuted every stem
        Soloed,      // a hold muted the other stems
        SoloEnded,   // release or cancel restored the pre-touch state
    };
    using Read = std::function<bool(int stem)>;
    using Write = std::function<void(int stem, bool muted)>;

    DeckLabStemGestures(Read muted, Write setMuted)
            : m_muted(std::move(muted)),
              m_setMuted(std::move(setMuted)) {
    }

    // Stems on the loaded track; 0 when it does not play as stems.
    void setCount(int count) {
        m_count = std::clamp(count, 0, kStems);
    }
    int count() const {
        return m_count;
    }
    bool active() const {
        return m_stem >= 0;
    }
    int pressedStem() const {
        return m_stem;
    }
    int soloStem() const {
        return m_solo ? m_stem : -1;
    }

    // A pointer went down on a pad: the mute flips now. One gesture at a
    // time. now: milliseconds, for the double tap.
    Result press(int stem, const Context& context, qint64 now) {
        if (active() || stem < 0 || stem >= m_count) {
            return Result::Ignored;
        }
        m_stem = stem;
        m_context = context;
        m_solo = false;
        m_pressMuted = m_muted(stem);
        if (m_tapStem == stem && m_tapContext == context && now - m_tapAt <= kDoubleTapMs) {
            for (int other = 0; other < m_count; ++other) {
                if (m_muted(other)) {
                    m_setMuted(other, false);
                }
            }
            m_tapStem = -1;
            // A restore is complete in itself: holding on does not solo.
            m_restored = true;
            return Result::RestoredAll;
        }
        m_restored = false;
        m_setMuted(stem, !m_pressMuted);
        m_tapStem = stem;
        m_tapContext = context;
        m_tapAt = now;
        return Result::Toggled;
    }

    // The hold time elapsed with the pointer still down.
    Result hold(const Context& current) {
        if (!active() || m_solo || m_restored) {
            return Result::None;
        }
        // A page, layer, layout or track change since the press: no solo.
        if (current != m_context || m_stem >= m_count) {
            return Result::Ignored;
        }
        m_solo = true;
        m_soloCount = m_count;
        for (int stem = 0; stem < m_soloCount; ++stem) {
            // The held stem returns to how it was before the touch-down.
            m_before[stem] = stem == m_stem ? m_pressMuted : m_muted(stem);
            m_written[stem] = stem != m_stem;
            if (m_muted(stem) != m_written[stem]) {
                m_setMuted(stem, m_written[stem]);
            }
        }
        // A hold is never the first half of a double tap.
        m_tapStem = -1;
        return Result::Soloed;
    }

    // The pointer came up. A tap already acted on touch-down; a solo ends
    // here, even after a page or layer change.
    Result release(const Context& current) {
        if (!active()) {
            return Result::None;
        }
        if (m_solo) {
            endSolo(current.load != m_context.load);
            finish();
            return Result::SoloEnded;
        }
        finish();
        return Result::None;
    }

    // The surface went away mid-gesture (hidden, touch cancel, application
    // deactivated, a track load). Undoes a solo; never toggles.
    // trackReplaced: the deck now holds another track. Mixxx resets stems on
    // load unless [Mixer Profile],stem_auto_reset is off, so the solo is
    // only undone where the engine kept the solo state.
    Result cancel(bool trackReplaced) {
        if (!active()) {
            return Result::None;
        }
        const bool solo = m_solo;
        if (solo) {
            endSolo(trackReplaced);
        }
        finish();
        m_tapStem = -1;
        return solo ? Result::SoloEnded : Result::Ignored;
    }

  private:
    void endSolo(bool trackReplaced) {
        if (trackReplaced) {
            for (int stem = 0; stem < m_soloCount; ++stem) {
                if (m_muted(stem) != m_written[stem]) {
                    return;
                }
            }
        }
        for (int stem = 0; stem < m_soloCount; ++stem) {
            // A mute changed meanwhile (controller, engine) is kept as it is.
            if (m_muted(stem) == m_written[stem] && m_written[stem] != m_before[stem]) {
                m_setMuted(stem, m_before[stem]);
            }
        }
    }
    void finish() {
        m_stem = -1;
        m_solo = false;
        m_restored = false;
        m_soloCount = 0;
    }

    Read m_muted;
    Write m_setMuted;
    int m_count = 0;
    int m_stem = -1;
    Context m_context;
    bool m_solo = false;
    bool m_restored = false;
    bool m_pressMuted = false;
    int m_soloCount = 0;
    std::array<bool, kStems> m_before{};
    std::array<bool, kStems> m_written{};
    int m_tapStem = -1;
    Context m_tapContext;
    qint64 m_tapAt = 0;
};
