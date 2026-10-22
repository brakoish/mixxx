// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabphrase.h"

#include <QCoreApplication>
#include <QVariant>
#include <algorithm>

#include "track/track.h"
#include "util/fpclassify.h"

namespace decklab {

namespace {

constexpr char kTrackProperty[] = "decklabPhrases";

QString tr(const char* text) {
    return QCoreApplication::translate("DeckLabPhrase", text);
}

// Stable ids of the labels, so names and colors cannot drift apart.
enum class Label {
    Unknown,
    Intro,
    Intro1,
    Intro2,
    Verse1,
    Verse2,
    Verse3,
    Verse4,
    Verse5,
    Verse6,
    Bridge,
    Chorus,
    Chorus1,
    Chorus2,
    Up1,
    Up2,
    Up3,
    Down,
    Outro,
    Outro1,
    Outro2,
};

// Phrase ids per mood follow crate-digger's rekordbox_anlz.ksy enums; the
// high-mood variants follow Deep Symmetry's flag table (k1 numbers intros,
// choruses and outros; k2 then k3 number the ups).
Label label(int mood, const SongStructureEntry& entry) {
    switch (mood) {
    case kPhraseMoodHigh:
        switch (entry.kind) {
        case 1:
            return entry.k1 == 1 ? Label::Intro1 : Label::Intro2;
        case 2:
            if (entry.k2 != 0) {
                return Label::Up3;
            }
            return entry.k3 == 0 ? Label::Up1 : Label::Up2;
        case 3:
            return Label::Down;
        case 5:
            return entry.k1 == 1 ? Label::Chorus1 : Label::Chorus2;
        case 6:
            return entry.k1 == 1 ? Label::Outro1 : Label::Outro2;
        default:
            return Label::Unknown;
        }
    case kPhraseMoodMid:
        switch (entry.kind) {
        case 1:
            return Label::Intro;
        case 2:
            return Label::Verse1;
        case 3:
            return Label::Verse2;
        case 4:
            return Label::Verse3;
        case 5:
            return Label::Verse4;
        case 6:
            return Label::Verse5;
        case 7:
            return Label::Verse6;
        case 8:
            return Label::Bridge;
        case 9:
            return Label::Chorus;
        case 10:
            return Label::Outro;
        default:
            return Label::Unknown;
        }
    case kPhraseMoodLow:
        switch (entry.kind) {
        case 1:
            return Label::Intro;
        case 2:
        case 3:
        case 4:
            return Label::Verse1; // rekordbox shows all three as "Verse 1"
        case 5:
        case 6:
        case 7:
            return Label::Verse2;
        case 8:
            return Label::Bridge;
        case 9:
            return Label::Chorus;
        case 10:
            return Label::Outro;
        default:
            return Label::Unknown;
        }
    default:
        return Label::Unknown;
    }
}

} // namespace

QString phraseName(int mood, const SongStructureEntry& entry) {
    switch (label(mood, entry)) {
    case Label::Unknown:
        return QString();
    case Label::Intro:
        return tr("Intro");
    case Label::Intro1:
        return tr("Intro 1");
    case Label::Intro2:
        return tr("Intro 2");
    case Label::Verse1:
        return tr("Verse 1");
    case Label::Verse2:
        return tr("Verse 2");
    case Label::Verse3:
        return tr("Verse 3");
    case Label::Verse4:
        return tr("Verse 4");
    case Label::Verse5:
        return tr("Verse 5");
    case Label::Verse6:
        return tr("Verse 6");
    case Label::Bridge:
        return tr("Bridge");
    case Label::Chorus:
        return tr("Chorus");
    case Label::Chorus1:
        return tr("Chorus 1");
    case Label::Chorus2:
        return tr("Chorus 2");
    case Label::Up1:
        return tr("Up 1");
    case Label::Up2:
        return tr("Up 2");
    case Label::Up3:
        return tr("Up 3");
    case Label::Down:
        return tr("Down");
    case Label::Outro:
        return tr("Outro");
    case Label::Outro1:
        return tr("Outro 1");
    case Label::Outro2:
        return tr("Outro 2");
    }
    return QString();
}

QColor phraseColor(int mood, const SongStructureEntry& entry) {
    const Label phrase = label(mood, entry);
    if (phrase == Label::Unknown) {
        return QColor(0x6b, 0x73, 0x80); // neutral: a real boundary, unnamed
    }
    // rekordbox shades each mood differently: pastel (low), saturated (mid)
    // and dark (high). Values after Deep Symmetry's beat-link; the intros are
    // shifted from red to rose so they never read as Deck Lab's live red.
    switch (mood) {
    case kPhraseMoodLow:
        switch (phrase) {
        case Label::Intro:
            return QColor(255, 170, 180);
        case Label::Verse1:
            return QColor(165, 160, 255);
        case Label::Verse2:
            return QColor(190, 160, 255);
        case Label::Bridge:
            return QColor(255, 250, 165);
        case Label::Chorus:
            return QColor(185, 225, 185);
        default:
            return QColor(145, 160, 180); // outro
        }
    case kPhraseMoodMid:
        switch (phrase) {
        case Label::Intro:
            return QColor(225, 70, 120);
        case Label::Verse1:
            return QColor(80, 110, 255);
        case Label::Verse2:
            return QColor(80, 85, 255);
        case Label::Verse3:
            return QColor(100, 80, 255);
        case Label::Verse4:
            return QColor(120, 80, 255);
        case Label::Verse5:
            return QColor(140, 80, 255);
        case Label::Verse6:
            return QColor(160, 80, 255);
        case Label::Bridge:
            return QColor(225, 215, 65);
        case Label::Chorus:
            return QColor(120, 195, 125);
        default:
            return QColor(115, 130, 150); // outro
        }
    default: // high
        switch (phrase) {
        case Label::Intro1:
            return QColor(200, 20, 90);
        case Label::Intro2:
            return QColor(215, 75, 135);
        case Label::Up1:
            return QColor(140, 50, 255);
        case Label::Up2:
            return QColor(105, 50, 255);
        case Label::Up3:
            return QColor(90, 50, 255);
        case Label::Down:
            return QColor(155, 115, 45);
        case Label::Chorus1:
        case Label::Chorus2:
            return QColor(15, 170, 0);
        case Label::Outro1:
            return QColor(80, 135, 195);
        default:
            return QColor(95, 135, 175); // outro 2
        }
    }
}

PhraseMapPointer buildPhraseMap(const SongStructure& structure,
        const QVector<double>& beatTimesMs,
        double offsetMs) {
    if (structure.mood < kPhraseMoodHigh || structure.mood > kPhraseMoodLow ||
            structure.entries.isEmpty() || beatTimesMs.size() < 2) {
        return nullptr;
    }
    const int beats = static_cast<int>(beatTimesMs.size());
    auto map = std::make_shared<PhraseMap>();
    map->beatSeconds.reserve(beats);
    double previous = -1;
    for (const double ms : beatTimesMs) {
        // Same clamp as the beat grid import: nothing before 1 ms.
        const double seconds = std::max(1.0, ms - offsetMs) / 1000.0;
        if (!(seconds >= previous)) {
            return nullptr; // a grid that runs backwards is not a grid
        }
        map->beatSeconds.append(seconds);
        previous = seconds;
    }
    int lastBeat = 0;
    for (const auto& entry : structure.entries) {
        // Every phrase must start inside the exported grid, after the one
        // before it. Anything else means the tag was not read correctly.
        if (entry.beat < 1 || entry.beat > beats || entry.beat <= lastBeat) {
            return nullptr;
        }
        lastBeat = entry.beat;
    }
    // The last phrase ends at the stored end beat, never past the grid.
    int endBeat = std::min(structure.endBeat, beats);
    if (endBeat <= lastBeat) {
        endBeat = beats;
    }
    if (endBeat <= lastBeat) {
        return nullptr; // the last phrase would be empty
    }
    const auto secondsAt = [&map](int beat) {
        return map->beatSeconds[beat - 1];
    };
    map->phrases.reserve(structure.entries.size());
    for (int i = 0; i < structure.entries.size(); ++i) {
        const auto& entry = structure.entries[i];
        Phrase phrase;
        phrase.name = phraseName(structure.mood, entry);
        phrase.color = phraseColor(structure.mood, entry);
        phrase.startBeat = entry.beat;
        phrase.endBeat = i + 1 < structure.entries.size() ? structure.entries[i + 1].beat : endBeat;
        phrase.startSeconds = secondsAt(phrase.startBeat);
        phrase.endSeconds = secondsAt(phrase.endBeat);
        if (entry.fill && entry.fillBeat > phrase.startBeat && entry.fillBeat < phrase.endBeat) {
            phrase.fillBeat = entry.fillBeat;
            phrase.fillSeconds = secondsAt(entry.fillBeat);
        }
        map->phrases.append(phrase);
    }
    return map;
}

PhrasePosition phrasePosition(const PhraseMap& map, double seconds) {
    // util_isfinite: the build uses -ffast-math, where std::isfinite and NaN
    // comparisons are unreliable.
    if (map.phrases.isEmpty() || map.beatSeconds.isEmpty() || !util_isfinite(seconds) ||
            seconds < 0) {
        return {};
    }
    // Current beat: the last grid beat at or before the position (1-based).
    constexpr double kTolerance = 1e-6;
    const int current = static_cast<int>(std::upper_bound(map.beatSeconds.cbegin(),
                                                 map.beatSeconds.cend(),
                                                 seconds + kTolerance) -
            map.beatSeconds.cbegin());
    if (current < map.phrases.first().startBeat) {
        return {};
    }
    const auto next = std::upper_bound(map.phrases.cbegin(),
            map.phrases.cend(),
            current,
            [](int beat, const Phrase& phrase) { return beat < phrase.startBeat; });
    const int index = static_cast<int>(next - map.phrases.cbegin()) - 1;
    const Phrase& phrase = map.phrases[index];
    if (current >= phrase.endBeat) {
        return {}; // after the last phrase
    }
    return {index, phrase.endBeat - current};
}

QString phraseCountdownText(int beatsLeft) {
    if (beatsLeft <= 0) {
        return QString();
    }
    return QStringLiteral("%1.%2").arg((beatsLeft - 1) / 4).arg((beatsLeft - 1) % 4 + 1);
}

void setTrackPhrases(Track* track, PhraseMapPointer map) {
    if (!track) {
        return;
    }
    if (map && !map->isEmpty()) {
        track->setProperty(kTrackProperty, QVariant::fromValue(map));
    } else {
        track->setProperty(kTrackProperty, QVariant());
    }
}

PhraseMapPointer trackPhrases(const Track* track) {
    if (!track) {
        return nullptr;
    }
    return track->property(kTrackProperty).value<PhraseMapPointer>();
}

} // namespace decklab
