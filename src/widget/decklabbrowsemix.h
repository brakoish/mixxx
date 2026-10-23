// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Deck Lab Browse, round 4 (audit library.md, signature move 5): "Mixes with
// deck N", rekordbox's Related Tracks made native. A live row at the top of
// a source's category list opens the tracks that mix with the reference deck
// of the Match bar (compatible key AND inside its BPM window and direction),
// sorted by closeness: same key first, then +1/-1 or relative, then by the
// absolute BPM difference. Every row says its relation in words.
//
// The candidates are read once from the source's own track model (no
// database access); recounting for a new reference is arithmetic only.

#include <QString>
#include <QVector>

#include "widget/decklabbrowse.h"

class QAbstractItemModel;
class QModelIndex;
class QPainter;
class QStyleOptionViewItem;

namespace decklab {

struct MixCandidate {
    int row = -1; // row in the source's track model
    ChromaticKey key = mixxx::track::io::key::INVALID;
    double bpm = 0;
};

// Key and BPM of up to `limit` rows of model (0 = all).
QVector<MixCandidate> collectMixCandidates(
        const QAbstractItemModel& model, int keyColumn, int bpmColumn, int limit = 0);
// The Match bar's rule: a compatible key (same, relative, +1, -1) and inside
// the BPM window. A reference without a key or a tempo leaves that part open.
bool mixesWith(const MixCandidate& candidate, const MatchReference& reference);
// skipRow: the reference deck's own track, never listed as mixing with itself.
int countMixes(const QVector<MixCandidate>& candidates, const MatchReference& reference, int skipRow = -1);
// Only the candidates that mix, closest first.
QVector<MixCandidate> rankMixes(
        const QVector<MixCandidate>& candidates, const MatchReference& reference, int skipRow = -1);
// "▼1 BPM", "▲0.5 BPM", "same BPM"; empty without a tempo.
QString bpmStepText(double trackBpm, double referenceBpm);
// "same key, ▼1 BPM", "+1 key, ▲2 BPM", "relative, same BPM".
QString mixRelationText(ChromaticKey key, double bpm, const MatchReference& reference);
// "Mixes with deck 1".
QString mixesTitle(int deck);
// The reference a Mixes list is built for: the BPM rounded to a quarter, so
// pitch drift only rebuilds the list when the window really moves.
MatchReference mixesReference(MatchReference reference);

// One track row of the Mixes list (DeckLabListDelegate::MixRole data).
void paintMixRow(QPainter& painter, const QStyleOptionViewItem& option, const QModelIndex& index);

} // namespace decklab
