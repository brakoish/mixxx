// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabhistoryview.h"

#include <QCoreApplication>
#include <QListWidget>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <algorithm>
#include <array>

#include "library/trackmodel.h"
#include "track/keyutils.h"
#include "widget/decklabdeckwidgets.h"

namespace decklab::history {

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("DeckLabHistory", text);
}

void paintRowBase(QPainter& painter, const QRect& r, bool selected) {
    // Browse's list rows: line separators on the ground, a raised fill and a
    // white edge for the highlighted row.
    painter.fillRect(r, selected ? colors::raise : colors::ground);
    painter.setPen(colors::panel.lighter(115));
    painter.drawLine(r.bottomLeft(), r.bottomRight());
    if (selected) {
        painter.fillRect(QRect(r.left(), r.top(), 3, r.height()), colors::accent);
    }
}

void paintHeader(QPainter& painter, const QStyleOptionViewItem& option, const QModelIndex& index) {
    const QRect r = option.rect;
    painter.fillRect(r, colors::ground);
    const QFont font = labelFont(option.font);
    painter.setFont(font);
    painter.setPen(colors::muted);
    const QRect text = r.adjusted(16, 0, -16, -5);
    painter.drawText(text, Qt::AlignLeft | Qt::AlignBottom,
            QFontMetrics(font).elidedText(index.data().toString(), Qt::ElideRight, text.width()));
}

// Two lines (name, detail) after a 24 px glyph; count and chevron at right.
void paintTwoLines(QPainter& painter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index,
        bool enabled,
        bool chevron) {
    const QRect r = option.rect;
    const bool selected = enabled && (option.state & QStyle::State_Selected);
    paintRowBase(painter, r, selected);
    int x = r.left() + 16;
    const bool current = index.data(CurrentRole).toBool();
    paintGlyph(painter, QRectF(x, r.center().y() - 12, 24, 24), Glyph::History,
            !enabled ? colors::dim : current || selected ? colors::ink : colors::muted);
    x += 40;
    int right = r.right() - (chevron ? 30 : 14);
    if (chevron) {
        paintGlyph(painter, QRectF(r.right() - 26, r.center().y() - 7, 14, 14), Glyph::Chevron, colors::muted);
    }
    const QString count = index.data(CountRole).toString();
    if (!count.isEmpty()) {
        const QFont countFont = numberFont(option.font, 15);
        const int countWidth = QFontMetrics(countFont).horizontalAdvance(count) + 4;
        painter.setFont(countFont);
        painter.setPen(colors::muted);
        painter.drawText(QRect(right - countWidth, r.top(), countWidth, r.height()), Qt::AlignRight | Qt::AlignVCenter, count);
        right -= countWidth + 12;
    }
    const QFont nameFont = strongFont(option.font);
    const QFont lineFont = secondaryFont(option.font);
    const QFontMetrics nameMetrics(nameFont);
    const QFontMetrics lineMetrics(lineFont);
    const QString detail = index.data(DetailRole).toString();
    const int lines = detail.isEmpty() ? nameMetrics.height() : nameMetrics.height() + lineMetrics.height();
    const int top = r.top() + (r.height() - lines) / 2;
    const int width = std::max(0, right - x);
    painter.setFont(nameFont);
    painter.setPen(enabled ? colors::ink : colors::muted);
    painter.drawText(QRect(x, top, width, nameMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
            nameMetrics.elidedText(index.data().toString(), Qt::ElideRight, width));
    if (!detail.isEmpty()) {
        painter.setFont(lineFont);
        painter.setPen(colors::muted);
        painter.drawText(QRect(x, top + nameMetrics.height(), width, lineMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                lineMetrics.elidedText(detail, Qt::ElideRight, width));
    }
}

// A track of a set: time, deck badge, title over artist, BPM, key and (in an
// earlier set) the mark when it was played in this set too.
void paintTrack(QPainter& painter, const QStyleOptionViewItem& option, const QModelIndex& index) {
    const QRect r = option.rect;
    const bool selected = option.state & QStyle::State_Selected;
    paintRowBase(painter, r, selected);
    const QFont base = option.font;
    int x = r.left() + 14;
    painter.setFont(numberFont(base, 16));
    painter.setPen(selected ? colors::ink : colors::muted);
    painter.drawText(QRect(x, r.top(), 48, r.height()), Qt::AlignLeft | Qt::AlignVCenter, index.data(TimeRole).toString());
    x += 52;
    // The deck it played on, in the deck's colour (the dock's badge).
    const int deck = index.data(DeckRole).toInt();
    const QRectF badge(x, r.center().y() - 11, 24, 22);
    if (deck >= 1 && deck <= 4) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(deckLabDeckColor(deck));
        painter.drawRoundedRect(badge, 3, 3);
        painter.setFont(numberFont(base, 15));
        painter.setPen(colors::ground);
        painter.drawText(badge, Qt::AlignCenter, QString::number(deck));
    } else {
        // Played before this unit started: Mixxx does not keep the deck.
        painter.setPen(QPen(colors::edge, 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(badge.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        painter.setFont(numberFont(base, 15));
        painter.setPen(colors::dim);
        painter.drawText(badge, Qt::AlignCenter, QStringLiteral("–"));
    }
    x += 36;
    int right = r.right() - 12;
    if (!index.data(CurrentRole).toBool()) {
        const Played played = Store::instance().played(index.data(LocationRole).toString());
        if (played == Played::Session) {
            paintPlayedMark(painter, QRectF(right - kMarkSize, r.center().y() - kMarkSize / 2.0, kMarkSize, kMarkSize), played);
            right -= kMarkSize + 10;
        }
    }
    const QFont numbers = numberFont(base, 16);
    painter.setFont(numbers);
    painter.setPen(colors::muted);
    const QString key = index.data(KeyRole).toString();
    painter.drawText(QRect(right - 42, r.top(), 42, r.height()), Qt::AlignRight | Qt::AlignVCenter,
            key.isEmpty() ? QStringLiteral("—") : key);
    right -= 52;
    const double bpm = index.data(BpmRole).toDouble();
    painter.setPen(colors::ink);
    painter.drawText(QRect(right - 54, r.top(), 54, r.height()), Qt::AlignRight | Qt::AlignVCenter,
            bpm > 0 ? QLocale().toString(bpm, 'f', 1) : QStringLiteral("—"));
    right -= 66;
    const QFont titleFontRow = selected ? strongFont(base) : bodyFont(base);
    const QFont artistFont = secondaryFont(base);
    const QFontMetrics titleMetrics(titleFontRow);
    const QFontMetrics artistMetrics(artistFont);
    const QString artist = index.data(ArtistRole).toString();
    const int lines = artist.isEmpty() ? titleMetrics.height() : titleMetrics.height() + artistMetrics.height();
    const int top = r.top() + (r.height() - lines) / 2;
    const int width = std::max(0, right - x);
    painter.setFont(titleFontRow);
    painter.setPen(colors::ink);
    painter.drawText(QRect(x, top, width, titleMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
            titleMetrics.elidedText(index.data().toString(), Qt::ElideRight, width));
    if (!artist.isEmpty()) {
        painter.setFont(artistFont);
        painter.setPen(colors::muted);
        painter.drawText(QRect(x, top + titleMetrics.height(), width, artistMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                artistMetrics.elidedText(artist, Qt::ElideRight, width));
    }
}

} // namespace

void paintPlayedMark(QPainter& painter, const QRectF& box, Played played) {
    if (played == Played::No) {
        return;
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF disc = box.adjusted(1, 1, -1, -1);
    const qreal s = disc.width();
    QPainterPath check;
    check.moveTo(disc.left() + s * 0.28, disc.top() + s * 0.52);
    check.lineTo(disc.left() + s * 0.44, disc.top() + s * 0.68);
    check.lineTo(disc.left() + s * 0.73, disc.top() + s * 0.35);
    if (played == Played::Session) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(colors::muted);
        painter.drawEllipse(disc);
        painter.setPen(QPen(colors::ground, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    } else {
        painter.setPen(QPen(colors::dim, 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(disc.adjusted(0.75, 0.75, -0.75, -0.75));
        painter.setPen(QPen(colors::dim, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    }
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(check);
    painter.restore();
}

QIcon playedIcon(Played played) {
    if (played == Played::No) {
        return QIcon();
    }
    // Made once and never freed: pixmaps must not outlive the application.
    static auto* icons = new std::array<QIcon, 2>();
    QIcon& icon = (*icons)[played == Played::Session ? 1 : 0];
    if (icon.isNull()) {
        for (const qreal ratio : {1.0, 2.0}) {
            QPixmap pixmap(QSize(kMarkSize, kMarkSize) * ratio);
            pixmap.setDevicePixelRatio(ratio);
            pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap);
            paintPlayedMark(painter, QRectF(0, 0, kMarkSize, kMarkSize), played);
            painter.end();
            for (const QIcon::Mode mode : {QIcon::Normal, QIcon::Active, QIcon::Selected}) {
                icon.addPixmap(pixmap, mode);
            }
        }
    }
    return icon;
}

void styleTrackCell(QStyleOptionViewItem* option,
        const QModelIndex& index,
        const QColor& before,
        bool secondary,
        const QColor& mixxxPlayedColor) {
    // Mixxx's played colour lasts until Mixxx restarts and ignores "New set";
    // in Deck Lab the history decides.
    if (mixxxPlayedColor.isValid() && before.isValid() && before != mixxxPlayedColor &&
            option->palette.color(QPalette::Text) == mixxxPlayedColor) {
        option->palette.setColor(QPalette::Text, before);
    }
    const Store& store = Store::instance();
    const Played played = store.playedAt(index);
    if (played == Played::No) {
        return;
    }
    // Dimmed: primary text takes the secondary tone. Secondary text (15 px)
    // stays muted: nothing that small goes darker (brand).
    if (played == Played::Session && store.dimPlayed() && !secondary) {
        option->palette.setColor(QPalette::Text, colors::muted);
    }
    const auto* tracks = dynamic_cast<const TrackModel*>(index.model());
    if (tracks && tracks->columnIndexFromSortColumnId(TrackModel::SortColumnId::Title) == index.column()) {
        option->features |= QStyleOptionViewItem::HasDecoration;
        option->icon = playedIcon(played);
        option->decorationSize = QSize(kMarkSize, kMarkSize);
        option->decorationPosition = QStyleOptionViewItem::Right;
        option->decorationAlignment = Qt::AlignCenter;
    }
}

QString entryKeyText(const Entry& entry) {
    if (entry.key != mixxx::track::io::key::INVALID) {
        const QString text = KeyUtils::keyToString(entry.key);
        if (!text.isEmpty()) {
            return text;
        }
    }
    return entry.keyText;
}

int fillSessions(QListWidget* list, const QVector<Session>& sessions, const QDate& today, int keepSessionId) {
    list->clear();
    int keep = -1;
    int first = -1;
    for (const ListRow& row : sessionRows(sessions, today)) {
        auto* item = new QListWidgetItem(row.text, list);
        item->setData(RowRole, static_cast<int>(row.kind));
        if (row.kind != RowKind::Session) {
            item->setFlags(Qt::NoItemFlags);
            continue;
        }
        const auto session = std::find_if(sessions.cbegin(), sessions.cend(), [&row](const Session& each) {
            return each.id == row.sessionId;
        });
        if (session == sessions.cend()) {
            continue;
        }
        const QString detail = sessionDetail(*session);
        item->setData(SessionRole, session->id);
        item->setData(DetailRole, detail);
        item->setData(CountRole, session->count > 0 ? QLocale().toString(session->count) : QString());
        item->setData(CurrentRole, session->current);
        item->setData(Qt::AccessibleTextRole, QString(row.text + QStringLiteral(", ") + detail));
        const int at = list->count() - 1;
        if (first < 0) {
            first = at;
        }
        if (session->id == keepSessionId) {
            keep = at;
        }
    }
    if (sessions.isEmpty()) {
        auto* item = new QListWidgetItem(tr("No history yet"), list);
        item->setData(RowRole, static_cast<int>(RowKind::Empty));
        item->setData(DetailRole, tr("Tracks join when they play on a deck the room hears"));
        item->setFlags(Qt::NoItemFlags);
    }
    return keep >= 0 ? keep : first;
}

int fillTracks(QListWidget* list, const QVector<Entry>& entries, bool currentSet, int keepPosition) {
    list->clear();
    int keep = -1;
    for (const Entry& entry : entries) {
        auto* item = new QListWidgetItem(entry.title, list);
        const QString time = clockText(entry.played);
        item->setData(RowRole, static_cast<int>(RowKind::Track));
        item->setData(TimeRole, time);
        item->setData(DeckRole, entry.deck);
        item->setData(BpmRole, entry.bpm);
        item->setData(KeyRole, entryKeyText(entry));
        item->setData(ArtistRole, entry.artist);
        item->setData(LocationRole, entry.location);
        item->setData(PositionRole, entry.position);
        item->setData(CurrentRole, currentSet);
        QStringList spoken{entry.title};
        if (!entry.artist.isEmpty()) {
            spoken << entry.artist;
        }
        if (!time.isEmpty()) {
            spoken << tr("played %1").arg(time);
        }
        if (entry.deck > 0) {
            spoken << tr("deck %1").arg(entry.deck);
        }
        item->setData(Qt::AccessibleTextRole, spoken.join(QStringLiteral(", ")));
        if (entry.position == keepPosition) {
            keep = list->count() - 1;
        }
    }
    if (entries.isEmpty()) {
        auto* item = new QListWidgetItem(currentSet ? tr("Nothing played yet") : tr("No tracks in this set"), list);
        item->setData(RowRole, static_cast<int>(RowKind::Empty));
        item->setData(DetailRole, currentSet ? tr("A track joins when it plays on a deck the room hears") : QString());
        item->setFlags(Qt::NoItemFlags);
        return -1;
    }
    if (keep < 0) {
        keep = currentSet ? list->count() - 1 : 0;
    }
    return keep;
}

int historyRowHeight(const QModelIndex& index) {
    switch (static_cast<RowKind>(index.data(RowRole).toInt())) {
    case RowKind::Header:
        return kHeaderRowHeight;
    case RowKind::Session:
    case RowKind::Empty:
        return kSessionRowHeight;
    case RowKind::Track:
        return kTrackRowHeight;
    case RowKind::None:
        break;
    }
    return 0;
}

void paintHistoryRow(QPainter& painter, const QStyleOptionViewItem& option, const QModelIndex& index) {
    painter.setRenderHint(QPainter::Antialiasing);
    switch (static_cast<RowKind>(index.data(RowRole).toInt())) {
    case RowKind::Header:
        paintHeader(painter, option, index);
        return;
    case RowKind::Session:
        paintTwoLines(painter, option, index, true, true);
        return;
    case RowKind::Empty:
        paintTwoLines(painter, option, index, false, false);
        return;
    case RowKind::Track:
        paintTrack(painter, option, index);
        return;
    case RowKind::None:
        break;
    }
}

} // namespace decklab::history

DeckLabHistoryDelegate::DeckLabHistoryDelegate(QObject* parent)
        : DeckLabBrowseDelegate(parent) {
}

void DeckLabHistoryDelegate::paint(QPainter* painter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const {
    if (decklab::history::historyRowHeight(index) > 0) {
        painter->save();
        decklab::history::paintHistoryRow(*painter, option, index);
        painter->restore();
        return;
    }
    DeckLabBrowseDelegate::paint(painter, option, index);
}

QSize DeckLabHistoryDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    const int height = decklab::history::historyRowHeight(index);
    return height > 0 ? QSize(option.rect.width(), height) : DeckLabBrowseDelegate::sizeHint(option, index);
}
