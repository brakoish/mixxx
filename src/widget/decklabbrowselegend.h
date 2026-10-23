// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Deck Lab Browse, round 4 (audit library.md, signature move 4, part 2): the
// screen says what the browse knob does. A 24 px legend in the Teenage
// Engineering manner, a key-cap glyph per control and a 14 px uppercase verb,
// whose verbs follow the level: TURN choose, PRESS open (or preview in a
// track list), SHIFT+PRESS back. While a menu or dialog really owns the knob
// (decklabknob.h) it says MENU; a knob event that went elsewhere, or a Back
// with nowhere to go, flashes it for 150 ms.
//
// KnobAccelerator (C-03): sustained fast turns in long lists move further.

#include <QString>
#include <QTimer>
#include <QVector>
#include <QWidget>

namespace decklab {

enum class LegendLevel {
    Home,          // the source home: press opens, Back twice goes to Decks
    HomeBackArmed, // Back was pressed on the home once
    List,          // categories, values, folders, playlists
    Tracks,        // a track list or "Mixes with deck N": press previews
    Menu,          // a menu or dialog owns the knob
};

struct LegendItem {
    QString key;  // on the key cap: "TURN", "PRESS", "SHIFT+PRESS"
    QString verb; // after it: "CHOOSE", "OPEN", "PREVIEW", "BACK"
    bool operator==(const LegendItem&) const = default;
};

QVector<LegendItem> legendItems(LegendLevel level);
// Right-aligned note: "TAP A DECK TO LOAD", "HOLD BACK: SOURCES"; may be empty.
QString legendHint(LegendLevel level);

// Velocity acceleration for the browse knob. Ticks closer than 45 ms in one
// direction build a run; from the third such tick a step moves 4 rows (8
// when the ticks come under 20 ms apart). A pause or a change of direction
// starts over, so slow, deliberate turns always move one row per detent.
class KnobAccelerator {
  public:
    int steps(int delta, qint64 nowMs);
    void reset();

  private:
    qint64 m_last = -1;
    int m_sign = 0;
    int m_run = 0;
};

} // namespace decklab

class DeckLabKnobLegend : public QWidget {
  public:
    static constexpr int kHeight = 24;
    explicit DeckLabKnobLegend(QWidget* parent = nullptr);
    void setLevel(decklab::LegendLevel level);
    decklab::LegendLevel level() const {
        return m_level;
    }
    // Inverts the legend for 150 ms: a tick or a Back that did nothing here.
    void flash();
    bool flashing() const {
        return m_flashing;
    }
    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    decklab::LegendLevel m_level = decklab::LegendLevel::Home;
    bool m_flashing = false;
    QTimer m_flash;
};
