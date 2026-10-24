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

class QAbstractItemDelegate;
class QListWidget;
class QListWidgetItem;
class QModelIndex;
class QPainter;
class QStyleOptionViewItem;
class QTableView;

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
// decides the played look, not Mixxx's per-start "played" colour, which is
// undone in Text and HighlightedText (before: the cell's text colour before
// the model's colours; highlighted: the view's selected text colour). Only
// the title cell looks the row up (once per row): played in this set, its
// text takes the muted tone and the mark sits at its right edge.
void styleTrackCell(QStyleOptionViewItem* option,
        const QModelIndex& index,
        const QColor& before,
        const QColor& highlighted,
        const QColor& mixxxPlayedColor);
// The Deck Lab cell delegate of a track table (defined in
// wtracktableview.cpp), for tests that paint through it.
QAbstractItemDelegate* makeTrackCellDelegate(QTableView* table, bool secondary);

// Fill the Browse list. Each returns the row to highlight.
// The sets: the current one first, "New set", then earlier ones.
int fillSessions(QListWidget* list,
        const QVector<Session>& sessions,
        const QDate& today,
        int keepSessionId,
        bool clockTrusted = true);
// One set row's text, count and accessible text, in place.
void updateSessionItem(QListWidgetItem* item, const Session& session, bool clockTrusted = true);
// A set's tracks in play order; the current set opens on its latest track.
int fillTracks(QListWidget* list,
        const QVector<Entry>& entries,
        bool currentSet,
        int keepPosition,
        bool clockTrusted = true,
        int keepPlaylist = -1);
// One track row at the end of a filled list (a track joined the set).
QListWidgetItem* appendTrack(QListWidget* list, const QVector<Entry>& entries, bool currentSet, bool clockTrusted);
// When it played: "21:04"; without a trusted clock "+0:12" after the set's
// first track while the times run forward within a day, else "#3".
QString entryTimeText(const QVector<Entry>& entries, int index, bool clockTrusted);

// Rows whose RowRole is set.
void paintHistoryRow(QPainter& painter, const QStyleOptionViewItem& option, const QModelIndex& index);
int historyRowHeight(const QModelIndex& index);

// "8A" in the chosen key format (decklab/decklabkeys.h), else the stored key text.
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
