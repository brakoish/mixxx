// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QColor>
#include <QMetaType>
#include <QString>
#include <QVector>
#include <memory>

class Track;

namespace decklab {

// Phrase ("song structure") analysis exactly as a rekordbox USB export stores
// it in the PSSI tag of a track's .EXT analysis file, after unmasking. Field
// meanings follow Deep Symmetry's crate-digger documentation.
struct SongStructureEntry {
    int index = 0; // 1-based phrase number
    int beat = 0;  // 1-based beat of rekordbox's own beat grid where it starts
    int kind = 0;  // phrase id; its meaning depends on the track's mood
    int k1 = 0;    // variant flags (high mood only)
    int k2 = 0;
    int k3 = 0;
    bool fill = false; // a fill-in runs from fillBeat to the phrase end
    int fillBeat = 0;
};

constexpr int kPhraseMoodHigh = 1;
constexpr int kPhraseMoodMid = 2;
constexpr int kPhraseMoodLow = 3;

struct SongStructure {
    int mood = 0;    // kPhraseMood*; anything else is not understood
    int endBeat = 0; // beat where the last phrase ends
    QVector<SongStructureEntry> entries;
};

// rekordbox's label for a phrase in this mood ("Intro 1", "Up 2", "Verse 3",
// "Chorus"...), translated. Empty for an id the mood does not define.
QString phraseName(int mood, const SongStructureEntry& entry);
// Band color for a phrase, after rekordbox's per-mood colors. Intros avoid
// Deck Lab's live red, which is reserved for the playhead and on-air state.
QColor phraseColor(int mood, const SongStructureEntry& entry);

struct Phrase {
    QString name; // empty when rekordbox's id is unknown to us
    QColor color;
    int startBeat = 0; // 1-based, rekordbox grid
    int endBeat = 0;   // exclusive: the next phrase's first beat, or the end
    double startSeconds = 0;
    double endSeconds = 0;
    int fillBeat = 0; // 0 without a fill-in
    double fillSeconds = 0;
};

// Phrases placed on the beat grid that was exported with them. Times are in
// seconds of the decoded track, after the same decoder offset as the
// imported beat grid.
struct PhraseMap {
    QVector<double> beatSeconds; // beat n of the grid at index n - 1
    QVector<Phrase> phrases;
    bool isEmpty() const {
        return phrases.isEmpty();
    }
};
using PhraseMapPointer = std::shared_ptr<const PhraseMap>;

// beatTimesMs: the PQTZ beat grid times in milliseconds, in file order.
// offsetMs: subtracted from every rekordbox time, exactly like the beat grid
// import. Returns nullptr when the structure does not fit the grid (unknown
// mood, a phrase outside the grid, phrases out of order): Deck Lab never
// shows a guessed structure.
PhraseMapPointer buildPhraseMap(const SongStructure& structure,
        const QVector<double>& beatTimesMs,
        double offsetMs);

struct PhrasePosition {
    int phrase = -1;   // index into PhraseMap::phrases; -1 outside all phrases
    int beatsLeft = 0; // whole beats from the current beat to the phrase end
    bool operator==(const PhrasePosition&) const = default;
};
// Where `seconds` falls: the current beat is the last grid beat at or before
// it. beatsLeft is 1 on the last beat of a phrase.
PhrasePosition phrasePosition(const PhraseMap& map, double seconds);

// Club-style "bars.beats" countdown: beatsLeft = bars * 4 + beats with beats
// counting 4..1, so 16 beats read "3.4" and the last beat reads "0.1".
// Empty when there is nothing to count.
QString phraseCountdownText(int beatsLeft);

// Phrases travel with the Track object read from a rekordbox export, so they
// last exactly as long as that track stays loaded. Null clears them.
void setTrackPhrases(Track* track, PhraseMapPointer map);
PhraseMapPointer trackPhrases(const Track* track);

} // namespace decklab

Q_DECLARE_METATYPE(decklab::PhraseMapPointer)
