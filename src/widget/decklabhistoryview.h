// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Deck Lab Browse, round 5: how the history looks (decklabhistory.h is the
// data). The History view's rows (date headers, sets, tracks in play order
// with the time, the deck and its colour, BPM and key) and the "played" mark
// every Browse track list carries: a dimmed row and a check for the current
// set, a quieter ring for an earlier set. Never red (red is live only).

#include <QColor>
#include <QDate>
#include <QIcon>
#include <QRectF>
#include <QSize>

#include "widget/decklabbrowsehome.h"
#include "widget/decklabhistory.h"

class QListWidget;
class QModelIndex;
class QPainter;
class QStyleOptionViewItem;

namespace decklab::history {

constexpr int kMarkSize = 18;
constexpr int kHeaderRowHeight = 32;
constexpr int kSessionRowHeight = 60;
constexpr int kTrackRowHeight = 48;

// The mark in an 18 px box: a muted disc with a dark check (current set),
// a dim ring with a dim check (earlier set). Nothing for Played::No.
void paintPlayedMark(QPainter& painter, const QRectF& box, Played played);
// The same mark as an item decoration (pixmaps made once, kept for every
// icon mode so a selected row never tints it).
QIcon playedIcon(Played played);

// A Deck Lab cell of a Mixxx track table (wtracktableview.cpp): the history
// decides the played look, not Mixxx's per-start "played" colour. before is
// the text colour the cell had before the model's colours were applied.
// Played in this set, primary text takes the muted tone (secondary text is
// muted already); the title cell gets the mark at its right edge.
void styleTrackCell(QStyleOptionViewItem* option,
        const QModelIndex& index,
        const QColor& before,
        bool secondary,
        const QColor& mixxxPlayedColor);

// Fill the Browse list. Each returns the row to highlight.
// The sets: the current one first, then earlier ones under their day.
int fillSessions(QListWidget* list, const QVector<Session>& sessions, const QDate& today, int keepSessionId);
// A set's tracks in play order; the current set opens on its latest track.
int fillTracks(QListWidget* list, const QVector<Entry>& entries, bool currentSet, int keepPosition);

// Rows whose RowRole is set.
void paintHistoryRow(QPainter& painter, const QStyleOptionViewItem& option, const QModelIndex& index);
int historyRowHeight(const QModelIndex& index);

// "8A" in the configured notation, else the stored key text.
QString entryKeyText(const Entry& entry);

} // namespace decklab::history

// The Browse list's delegate with the History rows added; every other row
// is painted by DeckLabBrowseDelegate as before.
class DeckLabHistoryDelegate : public DeckLabBrowseDelegate {
  public:
    explicit DeckLabHistoryDelegate(QObject* parent = nullptr);
    void paint(QPainter* painter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};
