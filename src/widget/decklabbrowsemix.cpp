// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabbrowsemix.h"

#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QLocale>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <algorithm>
#include <cmath>

#include "widget/decklabhistoryview.h" // r5-history

namespace decklab {

namespace {
QString translate(const char* text) {
    return QCoreApplication::translate("DeckLabBrowse", text);
}

int closenessRank(ChromaticKey key, const MatchReference& reference) {
    switch (harmonicMatch(key, reference.key)) {
    case HarmonicMatch::Same:
        return 0;
    case HarmonicMatch::Relative:
    case HarmonicMatch::Adjacent:
        return 1;
    case HarmonicMatch::None:
        break;
    }
    // Only possible when the reference has no key: every key is as good.
    return 0;
}

double tempoDistance(double bpm, const MatchReference& reference) {
    if (!std::isfinite(bpm) || bpm <= 0 || reference.bpm <= 0) {
        return 1e9;
    }
    return std::abs(bpm - reference.bpm);
}
} // namespace

QVector<MixCandidate> collectMixCandidates(
        const QAbstractItemModel& model, int keyColumn, int bpmColumn, int limit) {
    QVector<MixCandidate> candidates;
    const int columns = model.columnCount();
    if (keyColumn < 0 || bpmColumn < 0 || keyColumn >= columns || bpmColumn >= columns) {
        return candidates;
    }
    const int rows = limit > 0 ? std::min(limit, model.rowCount()) : model.rowCount();
    candidates.reserve(rows);
    for (int row = 0; row < rows; ++row) {
        MixCandidate candidate;
        candidate.row = row;
        candidate.key = keyFromText(model.index(row, keyColumn).data().toString().trimmed());
        const QModelIndex bpmIndex = model.index(row, bpmColumn);
        bool ok = false;
        double bpm = bpmIndex.data(Qt::EditRole).toDouble(&ok);
        if (!ok) {
            bpm = bpmIndex.data().toString().toDouble(&ok);
        }
        candidate.bpm = ok && std::isfinite(bpm) && bpm > 0 ? bpm : 0;
        candidates.append(candidate);
    }
    return candidates;
}

bool mixesWith(const MixCandidate& candidate, const MatchReference& reference) {
    if (!reference.active()) {
        return false;
    }
    const bool key = reference.key == mixxx::track::io::key::INVALID ||
            harmonicMatch(candidate.key, reference.key) != HarmonicMatch::None;
    const TempoWindow window = reference.window();
    const bool tempo = !window.isValid() || tempoInWindow(candidate.bpm, window);
    return key && tempo;
}

int countMixes(const QVector<MixCandidate>& candidates, const MatchReference& reference, int skipRow) {
    if (!reference.active()) {
        return 0;
    }
    return static_cast<int>(std::count_if(candidates.cbegin(), candidates.cend(),
            [&reference, skipRow](const MixCandidate& candidate) {
                return candidate.row != skipRow && mixesWith(candidate, reference);
            }));
}

QVector<MixCandidate> rankMixes(const QVector<MixCandidate>& candidates, const MatchReference& reference, int skipRow) {
    QVector<MixCandidate> ranked;
    for (const MixCandidate& candidate : candidates) {
        if (candidate.row != skipRow && mixesWith(candidate, reference)) {
            ranked.append(candidate);
        }
    }
    std::stable_sort(ranked.begin(), ranked.end(), [&reference](const MixCandidate& a, const MixCandidate& b) {
        const int rankA = closenessRank(a.key, reference);
        const int rankB = closenessRank(b.key, reference);
        if (rankA != rankB) {
            return rankA < rankB;
        }
        const double distanceA = tempoDistance(a.bpm, reference);
        const double distanceB = tempoDistance(b.bpm, reference);
        if (std::abs(distanceA - distanceB) > 1e-9) {
            return distanceA < distanceB;
        }
        return a.row < b.row;
    });
    return ranked;
}

QString bpmStepText(double trackBpm, double referenceBpm) {
    if (!std::isfinite(trackBpm) || !std::isfinite(referenceBpm) || trackBpm <= 0 || referenceBpm <= 0) {
        return {};
    }
    const double delta = trackBpm - referenceBpm;
    if (std::abs(delta) < 0.05) {
        return translate("same BPM");
    }
    const double size = std::round(std::abs(delta) * 10.0) / 10.0;
    const bool whole = std::abs(size - std::round(size)) < 1e-9;
    const QString number = QLocale().toString(size, 'f', whole ? 0 : 1);
    return translate("%1%2 BPM").arg(QChar(delta > 0 ? 0x25b2 : 0x25bc)).arg(number);
}

QString mixRelationText(ChromaticKey key, double bpm, const MatchReference& reference) {
    QStringList parts;
    switch (harmonicMatch(key, reference.key)) {
    case HarmonicMatch::Same:
        parts << translate("same key");
        break;
    case HarmonicMatch::Relative:
        parts << translate("relative");
        break;
    case HarmonicMatch::Adjacent: {
        const int distance = (lancelot(key).number - lancelot(reference.key).number + 12) % 12;
        parts << (distance == 1 ? translate("+1 key") : translate("−1 key"));
        break;
    }
    case HarmonicMatch::None:
        break;
    }
    const QString tempo = bpmStepText(bpm, reference.bpm);
    if (!tempo.isEmpty()) {
        parts << tempo;
    }
    return parts.join(QStringLiteral(", "));
}

QString mixesTitle(int deck) {
    return translate("Mixes with deck %1").arg(deck);
}

MatchReference mixesReference(MatchReference reference) {
    reference.enabled = true;
    reference.bpm = std::round(reference.bpm * 4.0) / 4.0;
    return reference;
}

void paintMixRow(QPainter& painter, const QStyleOptionViewItem& option, const QModelIndex& index) {
    painter.setRenderHint(QPainter::Antialiasing);
    const QRect r = option.rect;
    const bool selected = option.state & QStyle::State_Selected;
    painter.fillRect(r, selected ? colors::raise : colors::ground);
    painter.setPen(colors::panel.lighter(115));
    painter.drawLine(r.bottomLeft(), r.bottomRight());
    if (selected) {
        painter.fillRect(QRect(r.left(), r.top(), 3, r.height()), colors::accent);
    }
    const QFont base = option.font;
    // Key pill: solid green for the same key, outlined for relative and +1/-1.
    const auto match = static_cast<HarmonicMatch>(index.data(DeckLabListDelegate::MixMatchRole).toInt());
    const QString key = index.data(DeckLabListDelegate::MixKeyRole).toString();
    const QRectF pill(r.left() + 12.5, r.center().y() - 12.5, 48, 26);
    if (match == HarmonicMatch::Same) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(colors::keyMatch);
    } else {
        painter.setPen(QPen(match == HarmonicMatch::None ? colors::edge : colors::keyMatch, 1.5));
        painter.setBrush(Qt::NoBrush);
    }
    painter.drawRoundedRect(pill, 13, 13);
    painter.setFont(numberFont(base, 16));
    painter.setPen(match == HarmonicMatch::Same ? colors::ground
                    : match == HarmonicMatch::None  ? colors::ink
                                                    : colors::keyMatch);
    painter.drawText(pill, Qt::AlignCenter, key.isEmpty() ? QStringLiteral("—") : key);
    // BPM and its signed difference to the reference.
    const double bpm = index.data(DeckLabListDelegate::MixBpmRole).toDouble();
    int x = r.left() + 72;
    painter.setFont(numberFont(base, 16));
    painter.setPen(colors::ink);
    painter.drawText(QRect(x, r.top(), 50, r.height()), Qt::AlignRight | Qt::AlignVCenter,
            bpm > 0 ? QLocale().toString(bpm, 'f', 1) : QStringLiteral("—"));
    x += 56;
    painter.setFont(numberFont(base, 15));
    painter.setPen(colors::muted);
    painter.drawText(QRect(x, r.top(), 46, r.height()), Qt::AlignLeft | Qt::AlignVCenter,
            index.data(DeckLabListDelegate::MixDeltaRole).toString());
    x += 52;
    // r5-history: a played track keeps its mark at the right edge; played in
    // this set, its title dims to the artist's tone (decklabhistoryview.h).
    const auto& marks = history::Store::instance();
    const history::Played played = marks.anyPlayed()
            ? marks.played(index.data(history::LocationRole).toString())
            : history::Played::No;
    const bool dimmed = played == history::Played::Session && marks.dimPlayed() && !selected;
    int rowRight = r.right() - 12;
    if (played != history::Played::No) {
        history::paintPlayedMark(painter,
                QRectF(rowRight - history::kMarkSize, r.center().y() - history::kMarkSize / 2.0,
                        history::kMarkSize, history::kMarkSize),
                played);
        rowRight -= history::kMarkSize + 10;
    }
    // The relation in words, right aligned.
    const QFont relationFont = secondaryFont(base);
    const QFontMetrics relationMetrics(relationFont);
    const QString relation = index.data(DeckLabListDelegate::MixRelationRole).toString();
    const int relationWidth = std::min(relationMetrics.horizontalAdvance(relation) + 2, std::max(0, (rowRight - x) / 2));
    const int relationLeft = rowRight - relationWidth;
    painter.setFont(relationFont);
    painter.setPen(match == HarmonicMatch::Same ? colors::keyMatch : colors::muted);
    painter.drawText(QRect(relationLeft, r.top(), relationWidth, r.height()), Qt::AlignRight | Qt::AlignVCenter,
            relationMetrics.elidedText(relation, Qt::ElideRight, relationWidth));
    // Title, then the artist while there is room.
    const int textRight = relationLeft - 12;
    const QFont titleFontRow = selected ? strongFont(base) : bodyFont(base);
    const QFontMetrics titleMetrics(titleFontRow);
    const QString title = titleMetrics.elidedText(index.data().toString(), Qt::ElideRight, std::max(0, textRight - x));
    painter.setFont(titleFontRow);
    painter.setPen(dimmed ? colors::muted : colors::ink); // r5-history
    painter.drawText(QRect(x, r.top(), std::max(0, textRight - x), r.height()), Qt::AlignLeft | Qt::AlignVCenter, title);
    const QString artist = index.data(DeckLabListDelegate::MixArtistRole).toString();
    const int artistLeft = x + titleMetrics.horizontalAdvance(title) + 10;
    if (!artist.isEmpty() && textRight - artistLeft > 40) {
        const QFontMetrics artistMetrics(secondaryFont(base));
        painter.setFont(secondaryFont(base));
        painter.setPen(colors::muted);
        painter.drawText(QRect(artistLeft, r.top(), textRight - artistLeft, r.height()), Qt::AlignLeft | Qt::AlignVCenter,
                artistMetrics.elidedText(artist, Qt::ElideRight, textRight - artistLeft));
    }
}

} // namespace decklab
