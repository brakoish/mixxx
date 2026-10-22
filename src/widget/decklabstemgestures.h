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
//  - tap toggles that stem's mute (on release, like the hot-cue pads);
//  - hold (kHoldMs) solos it while held: the other stems mute, and release
//    restores what was there before;
//  - a second tap on the same pad within kDoubleTapMs unmutes every stem.
//
// Every press is tied to a context: the surface's interaction epoch (page,
// layer and layout changes) and the deck's load serial. A release in another
// context is stale and does nothing, except that a solo is always undone.
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
        Ignored,     // stale, cancelled or out of range: no control writes
        Toggled,     // a tap flipped one mute
        RestoredAll, // a double tap unmuted every stem
        Soloed,      // a hold muted the other stems
        SoloEnded,   // release or cancel restored the pre-solo state
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

    // A pointer went down on a pad. One gesture at a time.
    bool press(int stem, const Context& context) {
        if (active() || stem < 0 || stem >= m_count) {
            return false;
        }
        m_stem = stem;
        m_context = context;
        m_solo = false;
        return true;
    }

    // The hold time elapsed with the pointer still down.
    Result hold(const Context& current) {
        if (!active() || m_solo) {
            return Result::None;
        }
        // A page, layer, layout or track change since the press: no solo.
        if (current != m_context || m_stem >= m_count) {
            return Result::Ignored;
        }
        m_solo = true;
        m_soloCount = m_count;
        for (int stem = 0; stem < m_soloCount; ++stem) {
            m_before[stem] = m_muted(stem);
            m_written[stem] = stem != m_stem;
            if (m_before[stem] != m_written[stem]) {
                m_setMuted(stem, m_written[stem]);
            }
        }
        // A hold is never the first half of a double tap.
        m_tapStem = -1;
        return Result::Soloed;
    }

    // The pointer came up; inside: over the pressed pad. now: milliseconds.
    Result release(bool inside, const Context& current, qint64 now) {
        if (!active()) {
            return Result::None;
        }
        if (m_solo) {
            // A solo always ends on release, even after a page or layer change.
            endSolo(current.load != m_context.load);
            finish();
            return Result::SoloEnded;
        }
        const int stem = m_stem;
        const Context pressed = m_context;
        finish();
        // Stale-release protection, as for the hot-cue pads: the release must
        // land on the pressed pad, in the same epoch, on the same track.
        if (!inside || current != pressed || stem >= m_count) {
            m_tapStem = -1;
            return Result::Ignored;
        }
        if (m_tapStem == stem && m_tapContext == current && now - m_tapAt <= kDoubleTapMs) {
            for (int other = 0; other < m_count; ++other) {
                if (m_muted(other)) {
                    m_setMuted(other, false);
                }
            }
            m_tapStem = -1;
            return Result::RestoredAll;
        }
        m_setMuted(stem, !m_muted(stem));
        m_tapStem = stem;
        m_tapContext = current;
        m_tapAt = now;
        return Result::Toggled;
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
        m_soloCount = 0;
    }

    Read m_muted;
    Write m_setMuted;
    int m_count = 0;
    int m_stem = -1;
    Context m_context;
    bool m_solo = false;
    int m_soloCount = 0;
    std::array<bool, kStems> m_before{};
    std::array<bool, kStems> m_written{};
    int m_tapStem = -1;
    Context m_tapContext;
    qint64 m_tapAt = 0;
};
