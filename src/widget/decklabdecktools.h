// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QObject>
#include <QString>
#include <array>
#include <functional>

#include "track/track_decl.h"
#include "widget/decklabdeckwidgets.h"
#include "widget/decklabgridmode.h"
#include "widget/decklabkeysheet.h"

class DeckLabDeckStrip;
class DeckLabPadBlock;
class QGridLayout;
class QWidget;
namespace mixxx {
class Beats;
}

// r5-decks: the Decks page's key sheet (owner's pick #3) and GRID mode
// (pick #5, audit move 6), wired to Mixxx's controls. The widgets are pure Qt
// (decklabkeysheet.h, decklabgridmode.h); this is the surface side: where
// they dock, what they show and what their keys write. WDeckLab owns one and
// calls it from a few marked hooks (// r5-decks:), so the surface file only
// gains one line per hook.
//
// Placement (nothing is ever drawn over or under another deck's GL lane):
//  - key sheet, two decks: in place of the side's pads (DeckLabPadBlock
//    panel), beside SYNC / MORE;
//  - key sheet, four decks: in place of the card's overview and transport;
//  - GRID mode, two decks: the deck's readout, overview and pads (its column
//    of the deck row); four decks: half the deck row, the half under the
//    deck's lane column when the lanes are side by side.
//
// One knob owner at a time: opening a key sheet leaves GRID mode, entering
// GRID mode closes the key sheets.
//
// Contexts are checked before every relayout and on deck refreshes: a sheet
// closes and GRID mode ends when its deck leaves the screen (layer, layout,
// page) or its track changes.
class DeckLabDeckTools final : public QObject {
    Q_OBJECT
  public:
    struct Host {
        std::function<double(int deck, const QString& item)> read;
        std::function<void(int deck, const QString& item, double value)> write;
        std::function<void(int deck, const QString& item, std::function<void(double)> changed)> watch;
        // The deck shown on a side (two decks): 0 left, 1 right.
        std::function<int(int side)> displayedDeck;
        std::function<bool()> fourDecks;
        std::function<bool()> onDecks;
        // The waveform lanes are side by side (two columns).
        std::function<bool()> sideBySide;
        // The deck's player track now, and the one the readout last showed.
        std::function<TrackPointer(int deck)> track;
        std::function<TrackPointer(int deck)> shownTrack;
        // Refresh a deck's readout soon (coalesced), or relayout the page.
        std::function<void(int deck)> refresh;
        std::function<void()> relayout;
        std::function<QWidget*(int deck)> overview;
        std::function<QWidget*(int deck)> cardTransport;
        // The deck's readout state as last refreshed (no copy per tick).
        std::function<const DeckLabStripState&(int deck)> stripState;
        // The surface's interaction epoch (page, layer and layout changes).
        std::function<quint64()> epoch;
        // [DeckLab] settings that outlive the app (key lock ownership).
        std::function<QString(const QString& item)> setting;
        std::function<void(const QString& item, const QString& value)> setSetting;
    };
    // GRID mode zooms the lanes in to Mixxx's closest zoom while it is on
    // (the zoom control only; no GL reallocation) and restores the zoom.
    static constexpr double kGridZoom = 1.0;
    // MATCHED ends when the matched deck's pitch moves more than this
    // (semitones), so a drifting target never keeps the green look.
    static constexpr double kMatchDrift = 0.1;

    DeckLabDeckTools(Host host, QGridLayout* deckGrid, QObject* parent = nullptr);
    ~DeckLabDeckTools() override;

    // Wiring.
    void attachSide(int side, DeckLabPadBlock* pads);
    void attachCard(int deck, QWidget* card, DeckLabDeckStrip* strip);

    // Key sheet: the deck's sheet in its current layout. Leaves GRID mode.
    void openKeySheet(int deck);
    void closeKeySheets();
    bool keySheetOpen(int deck) const;
    DeckLabKeySheet* sideSheet(int side) const {
        return side >= 0 && side < 2 ? m_sideSheets[side] : nullptr;
    }
    DeckLabKeySheet* cardSheet(int deck) const {
        return deck >= 1 && deck <= 4 ? m_cardSheets[deck - 1] : nullptr;
    }

    // GRID mode for one deck at a time. Closes the key sheets.
    void enterGrid(int deck);
    enum class Relayout {
        Now,    // a touch or knob event: relayout at once
        Queued, // inside a deck refresh: relayout after it
        None    // inside the surface's own relayout: nothing more
    };
    void leaveGrid(Relayout relayout = Relayout::Now);
    int gridDeck() const {
        return m_grid.deck;
    }
    DeckLabGridPanel* gridPanel() const {
        return m_gridPanel;
    }

    // Hooks. updateLayers, before its signature: closes what lost its
    // context, so one relayout covers it (true: GRID mode is still on).
    bool validate();
    // Bits for the surface's layout signature: entering or leaving GRID mode
    // relayouts the deck row.
    quint64 layoutSignature() const;
    // refreshDeck: the key shift and MATCH result in the readout.
    void fillKey(int deck, DeckLabStripState& state);
    // applyDeck / refreshDeckClock (after the deck's readout state is
    // stored): an open sheet or GRID panel follows. Nothing open: no work.
    void showState(int deck);
    // updateLayers' relayout pass, after the cards and pads are placed.
    void applyLayout();

    // Mixxx's sync_key target for this deck, as EngineSync picks it: the
    // sync leader, else the first playing synced deck, else the first
    // playing deck, else the first loaded deck with a tempo. 0: none.
    int keySyncTarget(int deck) const;
    // Semitones as the readout writes them: "+1", "−2", "+0.5"; empty at 0.
    static QString shiftText(double semitones);
    // Key lock was turned on by the sheet (RESET or the next track turns it
    // back off).
    bool keylockBySheet(int deck) const;

  private:
    struct Match {
        int target = 0;
        double pitch = 0;
        double targetPitch = 0;
        TrackWeakPointer track;
    };
    struct GridSession {
        int deck = 0;
        TrackWeakPointer track;
        bool four = false;
        double zoomBefore = 0;
    };
    void keyAction(int deck, DeckLabKeySheet::Action action);
    void gridAction(int deck, DeckLabGridPanel::Action action);
    void pulse(int deck, const QString& item);
    // Turns key lock on if it is off and remembers that the sheet did it.
    // Returns true when it turned it on.
    bool ensureKeylock(int deck);
    // Key lock off if the sheet turned it on, then pitch_adjust back to
    // `adjust` (Mixxx's unlock mode decides what the unlock itself leaves).
    void restoreKeylock(int deck, double adjust);
    void forgetKeylock(int deck);
    void saveKeylockOwner(int deck);
    void restoreSavedKeylocks();
    static QString trackKey(const TrackPointer& track);
    bool keysMatch(int deck, int target) const;
    bool gridEditable(int deck) const;
    int sharedBusyDeck(int deck) const;
    bool current(int deck) const;
    bool sameTrack(int deck, const TrackWeakPointer& opened) const;
    bool matchHolds(int deck) const;
    void refreshOpen();
    void closeSideSheet(int side);
    void closeCardSheet(int deck);
    // Ends the session: hides the panel and restores the zoom (unless the
    // next session takes it over).
    void endSession(bool restoreZoom);
    const DeckLabStripState& strip(int deck) const;
    DeckLabKeySheetState keyState(int deck) const;
    DeckLabGridState gridState(int deck) const;
    bool canHalve(const std::shared_ptr<const mixxx::Beats>& beats) const;

    Host m_host;
    QGridLayout* m_deckGrid = nullptr;
    std::array<DeckLabPadBlock*, 2> m_sidePads{};
    std::array<DeckLabKeySheet*, 2> m_sideSheets{};
    std::array<int, 2> m_sideSheetDeck{};
    // The track each open sheet was opened for: a new track closes it.
    std::array<TrackWeakPointer, 2> m_sideSheetTrack{};
    std::array<TrackWeakPointer, 4> m_cardSheetTrack{};
    std::array<QWidget*, 4> m_cards{};
    std::array<DeckLabKeySheet*, 4> m_cardSheets{};
    std::array<bool, 4> m_cardSheetOpen{};
    std::array<Match, 4> m_matches{};
    // The track the sheet turned key lock on for (empty: not the sheet's),
    // and the pitch_adjust the deck had before. Saved in [DeckLab] settings
    // too, so a restart or skin rebuild still gives key lock back.
    std::array<TrackWeakPointer, 4> m_keylockTrack{};
    std::array<bool, 4> m_keylockBySheet{};
    std::array<double, 4> m_keylockAdjust{};
    // A pitch written for −1 / +1, checked once on the next refresh (a race
    // with the engine's pending rate update can lose it).
    std::array<double, 4> m_pendingPitch{};
    std::array<bool, 4> m_pitchPending{};
    // A live deck's KEY hold: what it started on.
    struct KeyHold {
        int deck = 0;
        TrackWeakPointer track;
        quint64 epoch = 0;
    };
    KeyHold m_keyHold;
    DeckLabGridPanel* m_gridPanel = nullptr;
    GridSession m_grid;
    // ÷2 is offered where Mixxx can halve the grid; decided once per grid.
    mutable std::weak_ptr<const mixxx::Beats> m_halveBeats;
    mutable bool m_halveOk = false;
};
