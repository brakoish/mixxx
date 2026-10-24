// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>
#include <QWidget>
#include <array>
#include <functional>

class DeckLabPadButton;
class QKeyEvent;

// r5-decks (owner's pick #5, audit move 6): edit the beat grid on the page,
// like a club player's grid adjust mode, instead of in a dialog that hid the
// waveform being aligned.
//
// GRID mode turns one deck's readout and pad area into the grid controls;
// the deck's scrolling waveform and its grid stay in view above:
//
//   ▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬ deck colour ▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬
//   [1] EDITING GRID  PAUSED                     [LOCK][DONE]
//   [ ‹ GRID ][ GRID › ]  [ ‹ BEAT ][ BEAT › ]
//   [−0.1][−0.01]   128.00 BPM  ▮▯▯▯   [+0.01][+0.1]
//   [  ÷2  ][  ×2  ]  [  TAP  ][ DOWNBEAT ]
//   KNOB  NUDGE · press for BPM · Shift+press DONE
//
//  - ‹ GRID / GRID › nudge the grid 10 ms (beats_translate_earlier/later).
//  - ‹ BEAT / BEAT › slip the grid a whole beat (Mixxx's translate by
//    beats), so beat 1 of the phase meter moves; constant-tempo grids only.
//  - ÷2 / ×2 halve or double the grid (beats_set_halve / beats_set_double);
//    ÷2 only where Mixxx can halve the grid.
//  - TAP: the grid BPM from taps (bpm_tap), on touch-down.
//  - DOWNBEAT: a beat at the playhead (beats_translate_curpos), counted as
//    beat 1; constant-tempo grids only (a variable grid, e.g. a rekordbox
//    import, keeps its own count; the status line says so).
//  - −0.1 / −0.01 / +0.01 / +0.1: the grid BPM (beats_adjust_slower /
//    faster, 0.01 each).
//  - LOCK: the track's BPM lock (bpmlock); a locked grid refuses edits.
//
// A playing deck can be edited (DJs fix grids live), but never silently:
// the header reads EDITING GRID in the deck colour with PLAYING beside it.
// Edits wait while sync is on, or while the same track plays or syncs on
// another deck; the status line says why and what to do (DONE first: those
// controls are under the panel).
//
// The browse knob adjusts while GRID mode owns it (decklabknob.h claim):
// turn nudges the grid (or the BPM), press switches NUDGE / BPM, and
// Shift+press (back) is DONE. The knob lamps and legend show only while the
// panel really owns the knob.
//
// Pure Qt: the surface supplies the state and performs the actions, so the
// panel renders offscreen and is unit-tested without the engine.

struct DeckLabGridState {
    int deck = 0;
    bool loaded = false;
    bool playing = false;
    bool hasBeats = false;
    // Whole-beat slips and DOWNBEAT need a constant-tempo grid (Mixxx's
    // rule); ÷2 needs a grid Mixxx can halve.
    bool constantTempo = false;
    bool canHalve = false;
    bool locked = false;
    // Sync lock or tempo master: a grid edit would move other decks.
    bool synced = false;
    // Another deck plays or syncs the same track (0: none).
    int sharedDeck = 0;
    // The grid BPM at the playhead, two decimals ("128.00").
    QString bpm;
    // Beat of four at the playhead (the phase meter's count), -1 unknown.
    int beat = -1;
    bool operator==(const DeckLabGridState&) const = default;
};

class DeckLabGridPanel final : public QWidget {
    Q_OBJECT
  public:
    enum class Action {
        NudgeEarlier = 0,
        NudgeLater,
        BeatEarlier,
        BeatLater,
        BpmDownCoarse,
        BpmDownFine,
        BpmUpFine,
        BpmUpCoarse,
        Halve,
        Double,
        Tap,
        Downbeat,
        Lock,
        Done,
        Count
    };
    Q_ENUM(Action)
    enum class Knob {
        Nudge,
        Bpm
    };
    Q_ENUM(Knob)
    struct Host {
        // The deck still holds the track GRID mode was opened for.
        std::function<bool(int deck)> current;
        // Perform an action (the surface writes Mixxx's controls).
        std::function<void(int deck, DeckLabGridPanel::Action action)> act;
    };
    static constexpr int kRowHeight = 44;
    static constexpr int kGap = 4;
    static constexpr int kRule = 3;
    static constexpr int kStatusHeight = 20;
    // Fits the two-deck readout and pads (244 px) and a four-deck card (230):
    // the rule, four rows of keys and the status line, on the 4 px grid.
    static constexpr int kMinimumHeight = kRule + kGap + 4 * kRowHeight + 4 * kGap + kStatusHeight + kGap;

    // name: object name; keys are named after the Mixxx control they drive
    // ("<name>-beats_translate_earlier", "-bpm_tap", "-bpmlock", ...).
    explicit DeckLabGridPanel(const QString& name, Host host, QWidget* parent = nullptr);
    ~DeckLabGridPanel() override;

    void setState(const DeckLabGridState& state);
    const DeckLabGridState& state() const {
        return m_state;
    }
    DeckLabPadButton* key(Action action) const {
        return m_keys[static_cast<int>(action)];
    }
    Knob knob() const {
        return m_knob;
    }
    void setKnob(Knob knob);
    // The knob is really this panel's (no menu, dialog or later claim).
    bool ownsKnob() const;
    // Re-reads who owns the knob (lamps and legend follow it).
    void refreshKnob();
    // Edits are possible (a grid, unlocked, no sync, no busy alias).
    bool editable() const;
    // The status line: why edits wait, or the knob legend.
    QString status() const;
    // The header's state word: PLAYING or PAUSED.
    QString playText() const;
    static QString actionName(Action action);

  protected:
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    void pressed(Action action);
    void refresh();
    bool usable(Action action) const;
    void knobTurn(int steps);
    // Painted areas (header title, the BPM readout, the status line).
    QRect titleRect() const;
    QRect readoutRect() const;
    QRect statusRect() const;

    Host m_host;
    DeckLabGridState m_state;
    std::array<DeckLabPadButton*, static_cast<int>(Action::Count)> m_keys{};
    QWidget* m_readout = nullptr; // spacer the readout is painted over
    QWidget* m_title = nullptr;   // spacer the header title is painted over
    QWidget* m_statusLine = nullptr;
    Knob m_knob = Knob::Nudge;
    bool m_ownsKnob = false;
    QString m_shownStatus;
};
