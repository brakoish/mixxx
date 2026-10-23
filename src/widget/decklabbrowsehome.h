// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Deck Lab Browse, round 4 (audit library.md, signature move 1): the Source
// home. The first Browse screen is a 2x2 grid of large source tiles and an
// "Also" row of chips. Each tile says its name, what is inside and its state;
// an inserted USB stick always comes first. The tiles are rows of the Browse
// list widget laid out left to right, so the knob walks them in reading order
// and every list behaviour (stale-tap guards, touch scrolling, focus) is the
// one the other Browse levels use.

#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>
#include <functional>

#include "widget/decklabbrowse.h"

namespace decklab {

// What a source tile says about its state (StateRole).
enum class SourceState {
    None,    // nothing to report (the Library, playlists, a plain stick)
    NotRead, // a rekordbox stick whose export has not been read yet
    Reading, // its export is being read
    Ready,   // its export has been read
    Failed,  // its export could not be read: "Try again"
    Missing, // the "plug in a USB stick" tile when no stick is in
};

struct SourceEntry {
    QString key; // KeyRole: "usb:<path>", "root:<icon>", "more", "nostick"
    QString name;
    QString line1; // SubtitleRole
    QString line2; // DetailRole
    QString count; // CountRole: shown on a chip ("Recordings 3")
    Glyph glyph = Glyph::None;
    SourceState state = SourceState::None;
    bool readOnly = false;
    bool enabled = true;
};

struct HomeLayout {
    QVector<SourceEntry> tiles; // reading order, at most kHomeTiles
    QVector<SourceEntry> also;  // the chips after them
};

constexpr int kHomeTiles = 4;

// The home in reading order: every stick first (a "plug one in" tile when
// none is in), then the library's own sources in sidebar order, then "More
// sources". The first four become tiles, the rest chips on the "Also" row.
HomeLayout arrangeHome(const QVector<SourceEntry>& sticks,
        const QVector<SourceEntry>& sources,
        const SourceEntry& more);
// The tile shown while no USB stick is in.
SourceEntry missingStickEntry();
// State words on a tile: "READY", "READING…", "COULDN'T READ"; empty for
// states that say nothing.
QString sourceStateText(SourceState state);
// "exported 21 Oct": the date of the stick's rekordbox export; empty if none.
QString exportDateText(const QString& stickPath);
// "29.8 GB · 12.1 GB free"; empty without a size.
QString capacityText(qint64 totalBytes, qint64 availableBytes);
// The row delta rows from `from`, skipping rows enabled() rejects; `from`
// itself when no other row qualifies. Stops at both ends.
int stepToEnabledRow(int count, int from, int delta, const std::function<bool(int)>& enabled);

} // namespace decklab

// Paints Browse list rows: the home's tiles and chips, the "Mixes with deck
// N" category row and its track rows, and every other row as before.
class DeckLabBrowseDelegate : public DeckLabListDelegate {
  public:
    static constexpr int kSpacing = 4;
    static constexpr int kChipHeight = 44;
    explicit DeckLabBrowseDelegate(QObject* parent = nullptr);
    void paint(QPainter* painter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    // Two tiles per row and two rows over one chip row, filling the viewport
    // (QListView's flow layout puts the spacing around every item). The home
    // turns its vertical scroll bar off so the layout keeps the full width.
    static QSize tileSize(const QSize& viewport);
    // The "Try again" target inside a tile whose stick could not be read.
    static QRect retryRect(const QRect& tile);

  private:
    void paintTile(QPainter& painter, const QStyleOptionViewItem& option, const QModelIndex& index) const;
    void paintChip(QPainter& painter, const QStyleOptionViewItem& option, const QModelIndex& index) const;
    void paintAccentRow(QPainter& painter, const QStyleOptionViewItem& option, const QModelIndex& index) const;
    QSize viewportSize(const QStyleOptionViewItem& option) const;
};
