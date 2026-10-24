// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QString>
#include <QWidget>
#include <array>
#include <functional>

class DeckLabPadButton;
class QKeyEvent;
class QLabel;

// r5-decks (owner's pick #3): key sync and key shift for one deck, in one
// touch, like a club player's KEY SYNC and KEY SHIFT.
//
// The deck readout shows the key and any shift ("8A +1"). Touching the KEY
// readout (held a moment on a playing deck), or KEY in MORE, docks this
// sheet in the deck area:
//
//   two decks (in place of the pads,         four decks (in place of the
//   beside SYNC / MORE):                     overview and transport rows):
//   [ MATCH TO DECK 2 · 9A ][KEY LOCK][DONE]   [‹ −1][MATCH][+1 ›]
//   [ ‹ −1 ][ MATCH ][ +1 › ][ RESET ]         [RESET][TO 2 · 9A][DONE]
//
// It is never a window over the waveforms (native GL windows), so nothing
// is drawn over the lanes. Every key fires on touch-down with the pads'
// stale-touch guards (the shown deck, its track). The keys drive Mixxx's own
// controls (sync_key, pitch, pitch_up / pitch_down, reset_key, keylock)
// through the host; the key results are Mixxx's, not a reimplementation.
//
// The browse knob drives the sheet while it is shown (decklabknob.h claim).
// The first knob event (turn or press) only shows the cursor, on DONE; then a
// turn moves it in reading order and a press activates. Back (Shift+press)
// closes. A blind press never shifts a key.
//
// Pure Qt: the surface supplies the state and performs the actions, so the
// sheet renders offscreen and is unit-tested without the engine.

struct DeckLabKeySheetState {
    int deck = 0;
    bool loaded = false;
    // This track has a key (MATCH needs one on both decks).
    bool hasKey = false;
    bool keylock = false;
    // The deck MATCH follows: Mixxx's sync_key target (the sync leader, else
    // a playing deck, else a loaded one). 0: none.
    int target = 0;
    QString targetKey;
    // This deck's key now and its shift from the track's key ("+1").
    QString key;
    QString shift;
    // MATCH set the key and it still matches the deck it matched (which can
    // differ from today's target), in that deck's key now.
    bool matched = false;
    int matchedDeck = 0;
    QString matchedKey;
    bool operator==(const DeckLabKeySheetState&) const = default;
};

class DeckLabKeySheet final : public QWidget {
    Q_OBJECT
  public:
    enum class Shape {
        Pads, // two decks: in the side's pad block, with an info line
        Card  // four decks: a card's lower rows, a short info between keys
    };
    enum class Action {
        Down = 0,
        Match = 1,
        Up = 2,
        Reset = 3,
        KeyLock = 4,
        Done = 5
    };
    Q_ENUM(Action)
    struct Host {
        // The deck this sheet shows at this moment (0: none).
        std::function<int()> deck;
        // The deck still holds the track the sheet was refreshed for.
        std::function<bool(int deck)> current;
        // Perform an action (the surface writes Mixxx's controls).
        std::function<void(int deck, DeckLabKeySheet::Action action)> act;
    };
    static constexpr int kRowHeight = 44;
    static constexpr int kGap = 4;
    static constexpr int kHeight = 2 * kRowHeight + kGap;

    // name: object name; keys are "<name>-down", "-match", "-up", "-reset",
    // "-keylock" and "-done"; the info line is "<name>-info".
    DeckLabKeySheet(const QString& name, Shape shape, Host host, QWidget* parent = nullptr);
    ~DeckLabKeySheet() override;

    Shape shape() const {
        return m_shape;
    }
    void setState(const DeckLabKeySheetState& state);
    const DeckLabKeySheetState& state() const {
        return m_state;
    }
    // nullptr for KEY LOCK in the four-deck card (it stays in MORE).
    DeckLabPadButton* key(Action action) const {
        return m_keys[static_cast<int>(action)];
    }
    QLabel* info() const {
        return m_info;
    }
    // The key the browse knob points at, and whether its cursor shows.
    Action knobFocus() const {
        return m_focus;
    }
    bool knobCursorShown() const {
        return m_knobUsed;
    }
    // The info line: "MATCH TO DECK 2 · 9A", "MATCHED · DECK 2 3A", or why
    // MATCH is off. The card's short form: "TO 2 · 9A", "MATCHED 2", "NO KEY".
    QString infoText() const;
    QString shortInfoText() const;
    // The knob's order: reading order of this shape's layout.
    QList<Action> knobOrder() const;
    static QString actionName(Action action);

  protected:
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

  private:
    void pressed(Action action);
    void refresh();
    bool usable(Action action) const;
    void moveFocus(int steps);
    void showFocus();
    // The first knob event only shows the cursor; true when it did.
    bool revealCursor();

    Shape m_shape;
    Host m_host;
    DeckLabKeySheetState m_state;
    std::array<DeckLabPadButton*, 6> m_keys{};
    QLabel* m_info = nullptr;
    bool m_infoMatched = false;
    Action m_focus = Action::Done;
    // The knob's cursor shows once the knob is used, not for touch.
    bool m_knobUsed = false;
};
