// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabbrowsehome.h"

#include <QAbstractItemView>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QPainter>
#include <QStyle>
#include <algorithm>

#include "widget/decklabbrowsemix.h"

namespace decklab {

namespace {
QString translate(const char* text) {
    return QCoreApplication::translate("DeckLabBrowse", text);
}
} // namespace

SourceEntry missingStickEntry() {
    SourceEntry entry;
    entry.key = QStringLiteral("nostick");
    entry.name = translate("No USB stick");
    entry.line1 = translate("Plug one in to play its music");
    entry.line2 = translate("Its playlists, cues and phrases come too");
    entry.glyph = Glyph::Usb;
    entry.state = SourceState::Missing;
    entry.enabled = false;
    return entry;
}

HomeLayout arrangeHome(const QVector<SourceEntry>& sticks,
        const QVector<SourceEntry>& sources,
        const SourceEntry& more,
        const QVector<SourceEntry>& extras) {
    QVector<SourceEntry> all;
    if (sticks.isEmpty()) {
        all.append(missingStickEntry());
    } else {
        all += sticks;
    }
    int extrasAt = static_cast<int>(all.size());
    for (const SourceEntry& source : sources) {
        all.append(source);
        if (source.key == QStringLiteral("root:tracks")) {
            extrasAt = static_cast<int>(all.size());
        }
    }
    for (int n = 0; n < extras.size(); ++n) {
        all.insert(extrasAt + n, extras[n]);
    }
    if (!more.key.isEmpty()) {
        all.append(more);
    }
    const int tiles = extras.isEmpty() ? kHomeTiles : kHomeTilesWide;
    HomeLayout layout;
    for (int n = 0; n < all.size(); ++n) {
        if (n < tiles) {
            layout.tiles.append(all[n]);
        } else {
            layout.also.append(all[n]);
        }
    }
    return layout;
}

QString sourceStateText(SourceState state) {
    switch (state) {
    case SourceState::Reading:
        return translate("Reading…");
    case SourceState::Ready:
        return translate("Ready");
    case SourceState::Failed:
        return translate("Couldn't read");
    case SourceState::None:
    case SourceState::NotRead:
    case SourceState::Missing:
        break;
    }
    return {};
}

QString exportDateText(const QString& stickPath) {
    for (const auto* relative : {"PIONEER/rekordbox/export.pdb", ".PIONEER/rekordbox/export.pdb"}) {
        const QFileInfo pdb(QDir(stickPath).filePath(QString::fromLatin1(relative)));
        if (!pdb.isFile()) {
            continue;
        }
        const QDate date = pdb.lastModified().date();
        if (!date.isValid()) {
            return {};
        }
        const bool thisYear = date.year() == QDate::currentDate().year();
        return translate("exported %1")
                .arg(QLocale().toString(date,
                        thisYear ? QStringLiteral("d MMM") : QStringLiteral("d MMM yyyy")));
    }
    return {};
}

QString capacityText(qint64 totalBytes, qint64 availableBytes) {
    if (totalBytes <= 0) {
        return {};
    }
    const QLocale locale;
    const QString total = locale.formattedDataSize(totalBytes, 1, QLocale::DataSizeSIFormat);
    if (availableBytes < 0) {
        return total;
    }
    return translate("%1 · %2 free")
            .arg(total, locale.formattedDataSize(availableBytes, 1, QLocale::DataSizeSIFormat));
}

int stepToEnabledRow(int count, int from, int delta, const std::function<bool(int)>& enabled) {
    if (count <= 0) {
        return -1;
    }
    const auto usable = [&](int row) {
        return row >= 0 && row < count && enabled(row);
    };
    int at = from;
    const int step = delta >= 0 ? 1 : -1;
    for (int remaining = std::abs(delta); remaining > 0; --remaining) {
        int next = at + step;
        while (next >= 0 && next < count && !enabled(next)) {
            next += step;
        }
        if (next < 0 || next >= count) {
            break;
        }
        at = next;
    }
    if (usable(at)) {
        return at;
    }
    for (int row = 0; row < count; ++row) {
        if (enabled(row)) {
            return row;
        }
    }
    return from;
}

} // namespace decklab

using namespace decklab;

namespace {
constexpr int kTilePad = 14;
constexpr int kAlsoLead = 56;

int chipContentWidth(const QFont& base, const QModelIndex& index) {
    const QFontMetrics name(bodyFont(base));
    // Four pixels of slack: painting measures to the inclusive right edge.
    int width = 12 + 18 + 8 + name.horizontalAdvance(index.data().toString()) + 14 + 4;
    const QString count = index.data(DeckLabListDelegate::CountRole).toString();
    if (!count.isEmpty()) {
        width += 8 + QFontMetrics(numberFont(base, 15)).horizontalAdvance(count);
    }
    return width;
}
} // namespace

DeckLabBrowseDelegate::DeckLabBrowseDelegate(QObject* parent)
        : DeckLabListDelegate(parent) {
}

int DeckLabBrowseDelegate::columnsFor(int tiles) {
    return tiles > kHomeTiles ? 3 : 2;
}

QSize DeckLabBrowseDelegate::tileSize(const QSize& viewport, int columns) {
    // QListView's flow layout pads every item with the spacing on all four
    // sides: two tiles take 2w + 4s across, two tile rows and the chip row
    // 2h + 44 + 6s down. One pixel to spare keeps two tiles on a row.
    columns = std::max(1, columns);
    const int width = std::max(120, (viewport.width() - 2 * columns * kSpacing) / columns - 1);
    const int height = std::clamp((viewport.height() - 6 * kSpacing - kChipHeight) / 2, 96, 200);
    return QSize(width, height);
}

QRect DeckLabBrowseDelegate::retryRect(const QRect& tile) {
    const int width = std::min(136, tile.width() / 2);
    return QRect(tile.right() - kTilePad - width + 1, tile.bottom() - 10 - 44 + 1, width, 44);
}

QSize DeckLabBrowseDelegate::viewportSize(const QStyleOptionViewItem& option) const {
    if (const auto* view = qobject_cast<const QAbstractItemView*>(option.widget)) {
        return view->viewport()->size();
    }
    if (const auto* view = qobject_cast<const QAbstractItemView*>(parent())) {
        return view->viewport()->size();
    }
    return QSize(800, 304);
}

QSize DeckLabBrowseDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    switch (index.data(TileRole).toInt()) {
    case 1: {
        int tiles = 0;
        for (int row = 0; index.model() && row < index.model()->rowCount(index.parent()); ++row) {
            tiles += index.model()->index(row, 0, index.parent()).data(TileRole).toInt() == 1;
        }
        return tileSize(viewportSize(option), columnsFor(tiles));
    }
    case 2: {
        const int lead = index.data(AlsoLeadRole).toBool() ? kAlsoLead : 0;
        return QSize(lead + chipContentWidth(option.font, index), kChipHeight);
    }
    default:
        break;
    }
    if (index.data(MixRole).toBool()) {
        return QSize(option.rect.width(), 44);
    }
    if (index.data(KeyRole).toString() == QStringLiteral("mixes")) {
        return QSize(option.rect.width(), 60);
    }
    return DeckLabListDelegate::sizeHint(option, index);
}

void DeckLabBrowseDelegate::paint(QPainter* painter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const {
    switch (index.data(TileRole).toInt()) {
    case 1:
        painter->save();
        paintTile(*painter, option, index);
        painter->restore();
        return;
    case 2:
        painter->save();
        paintChip(*painter, option, index);
        painter->restore();
        return;
    default:
        break;
    }
    if (index.data(MixRole).toBool()) {
        painter->save();
        paintMixRow(*painter, option, index);
        painter->restore();
        return;
    }
    if (index.data(KeyRole).toString() == QStringLiteral("mixes")) {
        painter->save();
        paintAccentRow(*painter, option, index);
        painter->restore();
        return;
    }
    DeckLabListDelegate::paint(painter, option, index);
}

void DeckLabBrowseDelegate::paintTile(QPainter& painter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const {
    painter.setRenderHint(QPainter::Antialiasing);
    const QRect r = option.rect;
    const bool selected = option.state & QStyle::State_Selected;
    const bool enabled = index.flags() & Qt::ItemIsEnabled;
    const auto state = static_cast<SourceState>(index.data(StateRole).toInt());
    // The tile: ground fill and a line outline. The knob's highlight is a
    // 2 px white outline (never red); the "plug one in" tile is dashed.
    painter.fillRect(r, colors::ground);
    if (selected && enabled) {
        painter.setPen(QPen(colors::accent, 2));
        painter.setBrush(colors::ground);
        painter.drawRoundedRect(QRectF(r).adjusted(1, 1, -1, -1), 4, 4);
    } else {
        QPen pen(colors::edge, 1, enabled ? Qt::SolidLine : Qt::DashLine);
        painter.setPen(pen);
        painter.setBrush(colors::ground);
        painter.drawRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    }
    const QColor glyphColor = enabled ? colors::ink : colors::muted;
    paintGlyph(painter, QRectF(r.left() + kTilePad, r.top() + kTilePad, 24, 24),
            static_cast<Glyph>(index.data(GlyphRole).toInt()), glyphColor);
    const int textLeft = r.left() + kTilePad + 24 + 12;
    const int right = r.right() - kTilePad;
    // r7-browse: in a narrow tile (three a row) the two lines start under
    // the glyph, so they keep their words.
    const int lineLeft = r.width() < 300 ? r.left() + kTilePad : textLeft;

    // State, top right: a white lamp (filled when read) and a 14 px word.
    int nameRight = right;
    const QString word = sourceStateText(state);
    if (!word.isEmpty()) {
        const QFont wordFont = labelFont(option.font);
        const QFontMetrics metrics(wordFont);
        const int wordWidth = metrics.horizontalAdvance(word);
        const int wordLeft = right - wordWidth;
        const QRect wordBox(wordLeft, r.top() + kTilePad, wordWidth + 1, 24);
        if (state == SourceState::Failed) {
            paintGlyph(painter, QRectF(wordLeft - 22, wordBox.center().y() - 8, 16, 16), Glyph::Warning, colors::ink);
            nameRight = wordLeft - 30;
        } else {
            const QPointF lamp(wordLeft - 12, wordBox.center().y() + 0.5);
            painter.setPen(QPen(colors::ink, 1.5));
            painter.setBrush(state == SourceState::Ready ? QBrush(colors::ink) : Qt::NoBrush);
            painter.drawEllipse(lamp, 4.5, 4.5);
            nameRight = wordLeft - 26;
        }
        painter.setFont(wordFont);
        painter.setPen(colors::ink);
        painter.drawText(wordBox, Qt::AlignLeft | Qt::AlignVCenter, word);
    }
    const QFont nameFont = titleFont(option.font);
    const QFontMetrics nameMetrics(nameFont);
    const int nameTop = r.top() + kTilePad + (24 - nameMetrics.height()) / 2;
    painter.setFont(nameFont);
    painter.setPen(enabled ? colors::ink : colors::muted);
    painter.drawText(QRect(textLeft, nameTop, std::max(0, nameRight - textLeft), nameMetrics.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            nameMetrics.elidedText(index.data().toString(), Qt::ElideRight, std::max(0, nameRight - textLeft)));

    // What is inside: two 15 px lines.
    const QFont lineFont = secondaryFont(option.font);
    const QFontMetrics lineMetrics(lineFont);
    const QRect retry = retryRect(r);
    int y = nameTop + nameMetrics.height() + 4;
    painter.setFont(lineFont);
    painter.setPen(colors::muted);
    for (const int role : {static_cast<int>(SubtitleRole), static_cast<int>(DetailRole)}) {
        const QString line = index.data(role).toString();
        if (line.isEmpty() || y + lineMetrics.height() > r.bottom() - 6) {
            continue;
        }
        int lineRight = right;
        if (state == SourceState::Failed && y + lineMetrics.height() > retry.top()) {
            lineRight = retry.left() - 8;
        }
        painter.drawText(QRect(lineLeft, y, std::max(0, lineRight - lineLeft), lineMetrics.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                lineMetrics.elidedText(line, Qt::ElideRight, std::max(0, lineRight - lineLeft)));
        y += lineMetrics.height();
    }

    // Bottom row: the read-only tag, or "Try again" for a failed read.
    const QFont tagFont = labelFont(option.font);
    const QFontMetrics tagMetrics(tagFont);
    if (state == SourceState::Failed) {
        painter.setPen(QPen(colors::ink, 1.5));
        painter.setBrush(colors::ground);
        painter.drawRoundedRect(QRectF(retry).adjusted(0.75, 0.75, -0.75, -0.75), 3, 3);
        paintGlyph(painter, QRectF(retry.left() + 12, retry.center().y() - 8, 16, 16), Glyph::Retry, colors::ink);
        painter.setFont(tagFont);
        painter.setPen(colors::ink);
        painter.drawText(retry.adjusted(36, 0, -8, 0), Qt::AlignLeft | Qt::AlignVCenter,
                tagMetrics.elidedText(QCoreApplication::translate("DeckLabBrowse", "Try again"),
                        Qt::ElideRight,
                        std::max(0, retry.width() - 44)));
    }
    if (index.data(ReadOnlyRole).toBool() && y + 26 <= r.bottom() - 6) {
        const QString tag = QCoreApplication::translate("DeckLabBrowse", "Read-only");
        const QRect box(lineLeft, r.bottom() - 10 - 24 + 1, tagMetrics.horizontalAdvance(tag) + 16, 24);
        if (state != SourceState::Failed || box.right() < retry.left() - 8) {
            painter.setPen(QPen(colors::edge, 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(QRectF(box).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
            painter.setFont(tagFont);
            painter.setPen(colors::ink);
            painter.drawText(box, Qt::AlignCenter, tag);
        }
    }
}

void DeckLabBrowseDelegate::paintChip(QPainter& painter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const {
    painter.setRenderHint(QPainter::Antialiasing);
    QRect r = option.rect;
    painter.fillRect(r, colors::ground);
    const bool selected = option.state & QStyle::State_Selected;
    const bool enabled = index.flags() & Qt::ItemIsEnabled;
    if (index.data(AlsoLeadRole).toBool()) {
        painter.setFont(secondaryFont(option.font));
        painter.setPen(colors::muted);
        painter.drawText(QRect(r.left() + 4, r.top(), kAlsoLead - 8, r.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                QCoreApplication::translate("DeckLabBrowse", "Also"));
        r.setLeft(r.left() + kAlsoLead);
    }
    if (selected && enabled) {
        painter.setPen(QPen(colors::accent, 2));
        painter.setBrush(colors::ground);
        painter.drawRoundedRect(QRectF(r).adjusted(1, 1, -1, -1), 4, 4);
    } else {
        painter.setPen(QPen(colors::edge, 1));
        painter.setBrush(colors::ground);
        painter.drawRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    }
    const QColor ink = enabled ? colors::ink : colors::dim;
    paintGlyph(painter, QRectF(r.left() + 12, r.center().y() - 9, 18, 18),
            static_cast<Glyph>(index.data(GlyphRole).toInt()), ink);
    int x = r.left() + 12 + 18 + 8;
    const QFont name = bodyFont(option.font);
    const QFontMetrics nameMetrics(name);
    const QString count = index.data(CountRole).toString();
    const QFont countFont = numberFont(option.font, 15);
    const int countWidth = count.isEmpty() ? 0 : QFontMetrics(countFont).horizontalAdvance(count) + 8;
    const int nameWidth = std::max(0, r.right() - 14 - countWidth - x);
    painter.setFont(name);
    painter.setPen(ink);
    const QString text = nameMetrics.elidedText(index.data().toString(), Qt::ElideRight, nameWidth);
    painter.drawText(QRect(x, r.top(), nameWidth, r.height()), Qt::AlignLeft | Qt::AlignVCenter, text);
    if (!count.isEmpty()) {
        x += nameMetrics.horizontalAdvance(text) + 8;
        painter.setFont(countFont);
        painter.setPen(colors::muted);
        painter.drawText(QRect(x, r.top(), countWidth, r.height()), Qt::AlignLeft | Qt::AlignVCenter, count);
    }
}

void DeckLabBrowseDelegate::paintAccentRow(QPainter& painter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const {
    // "Mixes with deck 1": a live row in the reference deck's colour at the
    // top of a source's category list.
    painter.setRenderHint(QPainter::Antialiasing);
    const QRect r = option.rect;
    const bool selected = option.state & QStyle::State_Selected;
    const bool enabled = index.flags() & Qt::ItemIsEnabled;
    painter.fillRect(r, selected ? colors::raise : colors::ground);
    painter.setPen(colors::panel.lighter(115));
    painter.drawLine(r.bottomLeft(), r.bottomRight());
    QColor accent = index.data(AccentRole).value<QColor>();
    if (!accent.isValid()) {
        accent = colors::muted;
    }
    if (!enabled) {
        accent.setAlpha(140);
    }
    painter.fillRect(QRect(r.left(), r.top(), 6, r.height()), accent);
    if (selected) {
        painter.fillRect(QRect(r.left() + 6, r.top(), 3, r.height()), colors::accent);
    }
    paintGlyph(painter, QRectF(r.left() + 18, r.center().y() - 12, 24, 24), Glyph::Mixes, accent);
    const int x = r.left() + 18 + 24 + 14;
    int right = r.right() - 30;
    paintGlyph(painter, QRectF(r.right() - 26, r.center().y() - 7, 14, 14), Glyph::Chevron, colors::muted);
    const QString count = index.data(CountRole).toString();
    if (!count.isEmpty()) {
        const QFont countFont = numberFont(option.font, 18);
        const QFontMetrics metrics(countFont);
        const int width = metrics.horizontalAdvance(count) + 4;
        painter.setFont(countFont);
        painter.setPen(enabled ? colors::ink : colors::dim);
        painter.drawText(QRect(right - width, r.top(), width, r.height()), Qt::AlignRight | Qt::AlignVCenter, count);
        right -= width + 12;
    }
    const QFont nameFont = strongFont(option.font);
    const QFont lineFont = secondaryFont(option.font);
    const QFontMetrics nameMetrics(nameFont);
    const QFontMetrics lineMetrics(lineFont);
    const int top = r.top() + (r.height() - nameMetrics.height() - lineMetrics.height()) / 2;
    const int width = std::max(0, right - x);
    painter.setFont(nameFont);
    painter.setPen(accent);
    painter.drawText(QRect(x, top, width, nameMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
            nameMetrics.elidedText(index.data().toString().toUpper(), Qt::ElideRight, width));
    painter.setFont(lineFont);
    painter.setPen(enabled ? colors::muted : colors::dim);
    painter.drawText(QRect(x, top + nameMetrics.height(), width, lineMetrics.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            lineMetrics.elidedText(index.data(SubtitleRole).toString(), Qt::ElideRight, width));
}
