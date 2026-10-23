// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Deck Lab round 4 (r4-waveforms): the scrolling waveform lanes on the
// compact pages, as on the XDJ-RX3/XDJ-AZ: the two enlarged waveforms stay
// on top and the pads (Sampler) or the beat FX (Lab FX) work under them.
//
// - presentationFor() is the one place that says which page shows the lanes
//   and how. Decks keeps its full lanes; Sampler and Lab FX show two compact
//   lanes over a 32 px DECKS row; Browse (the deck dock is its LOAD target,
//   the owner's call) and Settings show none.
// - choose() picks the two decks that get a compact lane: on air first, then
//   the controller's active layers, then loaded decks. The DECKS row names
//   every other deck (all four in the four-deck layout, like the XDJ-AZ's
//   overviews), so a deck that is on air is never out of sight.
// - DeckLabLaneDecks is that row: what the lanes cannot show (BPM, the beat,
//   MASTER, the phrase and the time left, plus the position in the track),
//   repainting only the cells whose whole-unit values changed.
//
// The lanes themselves are the surface's GL viewers (WDeckLab::addWaveform);
// compact pages only give them another height, never another width or
// parent, so the waveform column cache and the GL windows are kept.

#include <QColor>
#include <QString>
#include <QWidget>
#include <algorithm>
#include <array>

#include "widget/decklabdeckwidgets.h"

namespace decklab::lanes {

enum class Presentation {
    None,    // no lanes on this page
    Full,    // the Decks page: every shown deck's lane, readouts below
    Compact, // two lanes over the DECKS row
};

// Page numbers as WDeckLab::selectPage: 0 Decks, 1 Browse, 2 Sampler,
// 3 Lab FX, 4 Settings. Browse later: return Compact for page 1 too.
constexpr Presentation presentationFor(int page) {
    switch (page) {
    case 0:
        return Presentation::Full;
    case 2: // Sampler: lanes over the 4x4 pads
    case 3: // Lab FX: lanes over the instrument
        return Presentation::Compact;
    default: // Browse (deck dock), Settings
        return Presentation::None;
    }
}

// The DECKS row under compact lanes, and the gap between two lanes (the
// surface's lane grid spacing).
constexpr int kRowHeight = 32;
constexpr int kLaneGap = 2;
// A compact lane is never shorter than this (beat grid, playhead and the
// waveform's shape still read at arm's length on a 133 ppi panel) and
// never taller than a Decks lane at 800x480 needs to be.
constexpr int kMinLane = 56;
constexpr int kMaxLane = 96;
// What the page under compact lanes needs at least: Lab FX keeps two
// columns of 44 px effect tiles (256 px) and Sampler pads stay 62 px.
constexpr int kPageMinimum = 270;

// Height of one compact lane (stacked) for this much room under the rail.
// 800x480: 66 px; 1024x600: 96 px.
constexpr int compactLaneHeight(int pageArea) {
    return std::clamp((pageArea - kPageMinimum - kRowHeight - kLaneGap) / 2, kMinLane, kMaxLane);
}
// The whole compact zone: two lanes, their gap and the DECKS row. Side by
// side lanes are one row of the same total height, so the zone (and the
// page under it) is the same in both arrangements.
constexpr int compactZoneHeight(int pageArea) {
    return 2 * compactLaneHeight(pageArea) + kLaneGap + kRowHeight;
}

struct Deck {
    bool loaded = false;
    bool playing = false;
};

struct Choice {
    // The two decks with a lane: top (left when side by side) first.
    std::array<int, 2> lanes{1, 2};
    // Decks on the DECKS row, in order; rowCount of them are used.
    std::array<int, 4> row{1, 2, 0, 0};
    int rowCount = 2;
    bool operator==(const Choice&) const = default;
    bool hasLane(int deck) const {
        return deck > 0 && (lanes[0] == deck || lanes[1] == deck);
    }
};

// layers: the decks the controller's left and right side play (the Decks
// page's two-deck view). Two-deck layout: the row is the lane decks plus any
// other deck that is on air. Four-deck layout: the row is decks 1-4.
Choice choose(const std::array<Deck, 4>& decks, const std::array<int, 2>& layers, bool fourDecks);

} // namespace decklab::lanes

// The DECKS row under compact lanes. Display only (the rail's Decks tab is
// the way back): a cell per deck with its badge, play state, BPM, four-beat
// lamp, MASTER, phrase, time left and position. Decks with a lane carry a
// deck-colour bar along the top of their cell.
class DeckLabLaneDecks final : public QWidget {
  public:
    explicit DeckLabLaneDecks(QWidget* parent = nullptr);
    void setChoice(const decklab::lanes::Choice& choice);
    const decklab::lanes::Choice& choice() const {
        return m_choice;
    }
    // A deck's readout state and position (0..1). A deck that is not on the
    // row is stored and costs nothing until it is.
    void setDeck(int deck, const DeckLabStripState& state, double position);
    // The end-of-track blink, from the surface's display tick.
    void setFlash(bool on);
    bool shows(int deck) const;
    QRect cellRect(int deck) const;
    const DeckLabStripState& state(int deck) const {
        return m_states[std::clamp(deck, 1, 4) - 1];
    }
    // Cells repainted since construction (tests: an unchanged tick is free).
    int cellUpdates() const {
        return m_cellUpdates;
    }
    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    // What a cell shows, in whole units, so an unchanged cell never repaints.
    struct Cell {
        bool loaded = false;
        bool playing = false;
        bool cue = false;
        bool master = false;
        bool endWarning = false;
        bool flash = false;
        bool lane = false;
        QString bpm;
        QString remaining;
        int phase = -1;
        int progress = -1; // pixels
        QString phrase;    // countdown, "bars.beats"
        QColor phraseColor;
        bool operator==(const Cell&) const = default;
    };
    Cell cellFor(int deck, int width) const;
    void refreshCell(int deck);
    void refreshAll();
    void paintCell(QPainter& painter, int deck, const QRect& area) const;
    decklab::lanes::Choice m_choice;
    std::array<DeckLabStripState, 4> m_states{};
    std::array<double, 4> m_positions{};
    std::array<Cell, 4> m_cells{};
    bool m_flash = false;
    int m_cellUpdates = 0;
};
