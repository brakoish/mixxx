// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QColor>
#include <QList>
#include <QPushButton>
#include <QString>
#include <QWidget>
#include <array>
#include <functional>

class QHBoxLayout;

// r4-decks (audit move 2): pad modes, so the screen pads behave like a
// controller's. One block of 2x4 pads per deck with a mode cluster:
//
//   [CUE ][LOOP]  [ A ][ B ][ C ][ D ]  (SYNC)
//   [ROLL][STEM]  [ E ][ F ][ G ][ H ]  (MORE)
//
//  - CUE:  hot cues A-H. A set cue jumps, an empty one sets a cue there,
//          paused or playing (Mixxx snaps it to the beat while QNT is on).
//  - LOOP: beat loops 1/4 .. 32. The active length is lit; touching it
//          again ends the loop. The armed length (beatloop_size) is outlined.
//  - ROLL: loop rolls 1/16 .. 8 that play while held (Mixxx's slip roll).
//  - STEM: the deck's stem pads on the top row (round 3's gestures: touch
//          mutes, hold solos while held, double tap brings all back), ALL
//          and a latching SOLO below. Shown only for a stem track.
//
// Everything fires on touch-down. Lit is filled (#F2F4F7, dark ink). Each
// deck remembers its own mode. The block is pure Qt: the surface supplies
// control reads and writes and the stale-touch context through Host, so it
// renders offscreen and is unit-tested without the engine.
//
// Performance: the surface pushes one DeckLabPadDeckState per deck refresh
// (change-driven, never per tick); an unchanged state costs one comparison,
// and a changed one repaints only the pads whose look changed.

// What the block shows for one deck.
struct DeckLabPadDeckState {
    int deck = 0;
    bool loaded = false;
    bool playing = false;
    // Hot cues: -1 empty, otherwise (saved loop << 32) | 0xRRGGBB.
    std::array<qint64, 8> cues{-1, -1, -1, -1, -1, -1, -1, -1};
    bool looping = false;
    double loopSize = -1;
    bool operator==(const DeckLabPadDeckState&) const = default;
};

// One painted pad, mode key or STEM-row key. A QPushButton so the surface's
// touch rules (focus, accessibility, the 44 px sheet) apply as before.
class DeckLabPadButton final : public QPushButton {
    Q_OBJECT
  public:
    enum class Kind {
        Cue,  // hot cue: colour badge, or a dim letter in a dashed outline
        Value // loop / roll length, mode keys, ALL and SOLO
    };
    explicit DeckLabPadButton(QWidget* parent = nullptr);
    void setKind(Kind kind);
    // Filled lamp (the active loop, a held roll, the current mode, SOLO).
    void setLit(bool lit);
    bool lit() const {
        return m_lit;
    }
    // Outlined: the armed loop length.
    void setArmed(bool armed);
    bool armed() const { // r5-decks: also the knob's cursor in the key sheet
        return m_armed;
    }
    // A small lamp in the corner: something lives in a mode not shown
    // (a running loop, muted stems).
    void setIndicator(bool on);
    bool indicator() const {
        return m_indicator;
    }
    // Hot cue look. Mirrors the round-3 properties cueSet / cueColor.
    void setCue(bool set, const QColor& color);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    Kind m_kind = Kind::Value;
    bool m_lit = false;
    bool m_armed = false;
    bool m_indicator = false;
};

class DeckLabPadBlock final : public QWidget {
    Q_OBJECT
  public:
    enum class Mode {
        Cue = 0,
        Loop = 1,
        Roll = 2,
        Stem = 3
    };
    Q_ENUM(Mode)
    struct Host {
        // The deck the block shows at this moment (the side's layer).
        std::function<int()> deck;
        // The surface's interaction epoch (page, layer and layout changes).
        std::function<quint64()> epoch;
        // The deck still holds the track the screen was refreshed for.
        std::function<bool(int deck)> current;
        std::function<double(int deck, const QString& item)> read;
        std::function<void(int deck, const QString& item, double value)> write;
        // An action changed engine state: refresh this deck now.
        std::function<void(int deck)> acted;
    };
    // A deck's stem pads (decklabstempads.h), shown on the top row in STEM
    // mode. Plain callbacks keep this file free of engine types.
    struct Stems {
        int deck = 0;
        QWidget* pads = nullptr;
        std::function<bool()> present;
        std::function<bool()> anyMuted;
        std::function<void()> restoreAll;
        std::function<void(bool)> setSoloLatch;
        std::function<void(bool)> setShown;
    };
    static constexpr int kPadHeight = 44;
    static constexpr int kGap = 4;
    static constexpr int kBlockHeight = 2 * kPadHeight + kGap;
    static constexpr int kModeKeyWidth = 48;
    static constexpr int kSideWidth = 56;
    static constexpr std::array<double, 8> kLoopSizes{0.25, 0.5, 1, 2, 4, 8, 16, 32};
    static constexpr std::array<double, 8> kRollSizes{0.0625, 0.125, 0.25, 0.5, 1, 2, 4, 8};

    // name: object name of the block; children are "<name>-mode-cue",
    // "<name>-stem-all" and so on. padPrefix + 1..8 names the pads (the
    // round-3 hot cue names, so CUE mode keeps its contract).
    DeckLabPadBlock(const QString& name, const QString& padPrefix, Host host,
            QWidget* parent = nullptr);
    ~DeckLabPadBlock() override;

    void addStems(const Stems& stems);
    // A deck's stem presence or mutes changed.
    void stemsChanged();
    // Two-deck rows put SYNC and MORE in a column at the right.
    void setSideColumn(QWidget* column);
    // Side blocks sit 4 px below the overview; the MORE sheet needs none.
    void setOuterMargins(int left, int top, int right, int bottom);
    // r5-decks: a panel (the deck's key sheet, decklabkeysheet.h) shown in
    // place of the mode keys and pads, beside the SYNC/MORE column, so the
    // sheet docks in the deck area and never floats over the waveforms.
    // Showing it ends a held roll and drops the SOLO latch.
    void setPanel(QWidget* panel);
    void showPanel(bool shown);
    bool panelShown() const {
        return m_panelShown;
    }

    void setDeckState(const DeckLabPadDeckState& state);
    const DeckLabPadDeckState& deckState() const {
        return m_state;
    }
    // The mode shown for the current deck (STEM falls back to CUE while the
    // deck has no stems; the choice is kept for its next stem track).
    Mode mode() const;
    Mode chosenMode(int deck) const;
    void setMode(Mode mode);
    // Another block showing the same deck (the two-deck row and the
    // four-deck MORE) chose a mode: keep one choice per deck. No signal.
    void setChosenMode(int deck, Mode mode);
    DeckLabPadButton* pad(int index) const {
        return index >= 0 && index < 8 ? m_pads[index] : nullptr;
    }
    DeckLabPadButton* modeKey(Mode mode) const {
        return m_modeKeys[static_cast<int>(mode)];
    }
    DeckLabPadButton* allKey() const {
        return m_all;
    }
    DeckLabPadButton* soloKey() const {
        return m_solo;
    }
    bool soloLatched() const {
        return m_soloLatched;
    }
    // A roll is held right now.
    bool holding() const {
        return m_rollDeck > 0;
    }
    // Ends a held roll (hide, context change, lost touch). Always safe.
    void endHolds();
    static QString sizeText(double beats);
    static QString modeName(Mode mode);

  signals:
    void modeChanged(int deck, DeckLabPadBlock::Mode mode);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    void padPressed(int index);
    void padReleased(int index);
    void allPressed();
    void soloPressed();
    void modePressed(Mode mode);
    void applyMode(bool force);
    void refreshPads(bool force);
    void refreshModeKeys();
    const Stems* stemsFor(int deck) const;
    bool stemsPresent(int deck) const;
    void setSoloLatched(bool on);
    void watchHold(bool on);
    bool shownAndCurrent(int deck) const;

    QString m_name;
    Host m_host;
    DeckLabPadDeckState m_state;
    std::array<Mode, 4> m_modes{Mode::Cue, Mode::Cue, Mode::Cue, Mode::Cue};
    Mode m_applied = Mode::Cue;
    bool m_appliedOnce = false;
    QList<Stems> m_stems;
    std::array<DeckLabPadButton*, 8> m_pads{};
    std::array<DeckLabPadButton*, 4> m_modeKeys{};
    DeckLabPadButton* m_all = nullptr;
    DeckLabPadButton* m_solo = nullptr;
    QWidget* m_modeCluster = nullptr;
    QWidget* m_padArea = nullptr;
    std::array<QHBoxLayout*, 2> m_rows{};
    QWidget* m_side = nullptr;
    QWidget* m_panel = nullptr; // r5-decks
    bool m_panelShown = false;
    bool m_soloLatched = false;
    // The held roll: deck and size it was started on, ended on release.
    int m_rollDeck = 0;
    double m_rollSize = 0;
    int m_rollPad = -1;
    bool m_filtering = false;
};
