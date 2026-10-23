// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Deck Lab Browse, round 4 (audit library.md, signature move 3): the deck
// dock is the LOAD target. A 64 px row of deck tiles (two at 400 px or four
// at 200 px) replaces the mini strips under Browse and the LOAD buttons of
// the right pane. Each tile shows the deck's colour bar and badge, a state
// word (EMPTY, CUED, PAUSED, ON AIR in a live-red outline), the title, key,
// BPM and remaining time. With a track highlighted, every loadable tile shows
// LOAD and says what it replaces; an on-air tile refuses in place.
//
// The tiles keep the LOAD buttons' names (decklab-load-N), text ("Load N"),
// detail line and tone, so the stale-tap guards and every LOAD path of the
// surface work unchanged.

#include <QTimer>
#include <QWidget>
#include <array>

#include "widget/decklabbrowse.h"
#include "widget/decklabdeckwidgets.h"

class QGridLayout;

namespace decklab {

// What a dock tile shows of its deck. Changes repaint only that tile.
struct DockDeck {
    int deck = 0;
    bool loaded = false;
    bool playing = false;
    bool cue = false;
    bool endWarning = false;
    QString title;
    QString key;
    QString bpm; // one decimal
    QString remaining;
    bool operator==(const DockDeck&) const = default;
};
DockDeck dockDeckFrom(const DeckLabStripState& state);

enum class DockWord {
    Empty,
    Cued,
    Paused,
    OnAir,
};
DockWord dockWord(const DockDeck& deck);
// "Empty", "Cued", "Paused", "On air" (painted uppercase).
QString dockWordText(DockWord word);
// Second line of a tile ready to load: "replaces Night Drive" or "empty deck".
QString dockLoadLine(const DockDeck& deck);

} // namespace decklab

class DeckLabDockTile : public DeckLabActionButton {
  public:
    explicit DeckLabDockTile(const QString& text, QWidget* parent = nullptr);
    void setDeckState(const decklab::DockDeck& state);
    const decklab::DockDeck& deckState() const {
        return m_state;
    }
    // A track is highlighted and this deck can take it: LOAD shows.
    void setLoadReady(bool ready);
    bool loadReady() const {
        return m_loadReady;
    }
    // A refused tap on an on-air deck: the tile says so where it was tapped.
    void showRefusal(int milliseconds = 2400);
    bool refusing() const {
        return m_refusing;
    }
    // End-of-track warning blink of the remaining time.
    void setFlash(bool flash);
    void pinHeight(int height);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    void paintDeck(QPainter& painter);
    void paintAction(QPainter& painter);

    decklab::DockDeck m_state;
    bool m_loadReady = false;
    bool m_refusing = false;
    bool m_flash = false;
    QTimer m_refusal;
};

class DeckLabDeckDock : public QWidget {
  public:
    enum class Arrangement {
        Bar,  // one row along the bottom of Browse (64 px tiles)
        Pane, // the right pane while the search keyboard is up (52 px)
    };
    static constexpr int kBarTile = 64;
    static constexpr int kPaneTile = 52;
    static constexpr int kGap = 4;
    explicit DeckLabDeckDock(QWidget* parent = nullptr);
    DeckLabDockTile* tile(int slot) const {
        return slot >= 0 && slot < 4 ? m_tiles[slot] : nullptr;
    }
    void setArrangement(Arrangement arrangement);
    Arrangement arrangement() const {
        return m_arrangement;
    }
    // Two or four tiles (four-deck layout); the others hide.
    void setShownCount(int count);
    int shownCount() const {
        return m_count;
    }

  private:
    void relayout();

    std::array<DeckLabDockTile*, 4> m_tiles{};
    QGridLayout* m_grid = nullptr;
    Arrangement m_arrangement = Arrangement::Bar;
    int m_count = 2;
};
