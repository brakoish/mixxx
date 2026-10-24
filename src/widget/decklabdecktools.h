// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QObject>
#include <QPointer>
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

// r5-decks: the Decks page's key sheet (owner's pick #3) and GRID mode
// (pick #5, audit move 6), wired to Mixxx's controls. The widgets are pure Qt
// (decklabkeysheet.h, decklabgridmode.h); this is the surface side: where
// they dock, what they show and what their keys write. WDeckLab owns one and
// calls it from a few marked hooks (// r5-decks:), so the surface file only
// gains one line per hook.
//
// Placement (nothing is ever drawn over the GL lanes):
//  - key sheet, two decks: in place of the side's pads (DeckLabPadBlock
//    panel), beside SYNC / MORE;
//  - key sheet, four decks: in place of the card's overview and transport;
//  - GRID mode, two decks: the deck's readout, overview and pads (its column
//    of the deck row); four decks: the half of the deck row that holds it.
//
// Contexts are checked on every relayout and deck refresh: a sheet closes and
// GRID mode ends when its deck leaves the screen (layer, layout, page) or its
// track changes.
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
    };
    // GRID mode zooms the lanes in to Mixxx's closest zoom while it is on
    // (the zoom control only; no GL reallocation) and restores the zoom.
    static constexpr double kGridZoom = 1.0;

    DeckLabDeckTools(Host host, QGridLayout* deckGrid, QObject* parent = nullptr);
    ~DeckLabDeckTools() override;

    // Wiring.
    void attachSide(int side, DeckLabPadBlock* pads);
    void attachCard(int deck, QWidget* card, DeckLabDeckStrip* strip);

    // Key sheet: the deck's sheet in its current layout.
    void openKeySheet(int deck);
    void closeKeySheets();
    bool keySheetOpen(int deck) const;
    DeckLabKeySheet* sideSheet(int side) const {
        return side >= 0 && side < 2 ? m_sideSheets[side] : nullptr;
    }
    DeckLabKeySheet* cardSheet(int deck) const {
        return deck >= 1 && deck <= 4 ? m_cardSheets[deck - 1] : nullptr;
    }

    // GRID mode for one deck at a time.
    void enterGrid(int deck);
    // relayoutNow: false inside the surface's own relayout or refresh (the
    // relayout is queued instead).
    void leaveGrid(bool relayoutNow = true);
    int gridDeck() const {
        return m_grid.deck;
    }
    DeckLabGridPanel* gridPanel() const {
        return m_gridPanel;
    }
    // Bits for the surface's layout signature: entering or leaving GRID mode
    // relayouts the deck row.
    quint64 layoutSignature() const;

    // Hooks. refreshDeck: the key shift and MATCH result in the readout.
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

  private:
    struct Match {
        int target = 0;
        double pitch = 0;
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
    void ensureKeylock(int deck);
    bool keysMatch(int deck, int target) const;
    bool gridEditable(int deck) const;
    int sharedBusyDeck(int deck) const;
    bool current(int deck) const;
    bool sameTrack(int deck, const TrackWeakPointer& opened) const;
    void refreshOpen();
    // Closes what lost its context; true when GRID mode is still valid.
    bool validate();
    void closeSideSheet(int side);
    void closeCardSheet(int deck);
    const DeckLabStripState& strip(int deck) const;
    DeckLabKeySheetState keyState(int deck) const;
    DeckLabGridState gridState(int deck) const;

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
    DeckLabGridPanel* m_gridPanel = nullptr;
    GridSession m_grid;
};
