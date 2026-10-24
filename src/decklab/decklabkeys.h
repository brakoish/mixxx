// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QMap>
#include <QString>

#include "proto/keys.pb.h"

class QAbstractItemDelegate;
class QTableView;

// r6-keys: one place that turns a stored musical key into the text Deck Lab
// shows, in the DJ's chosen "Key format" (Settings › Library).
//
// Keys reach the screen two ways: as a Mixxx key id (library tracks, the
// decks) or as text: file tags, a rekordbox export ("Am" or "8A", whatever
// rekordbox was set to), USB folders, older databases. The text can be in any
// of three notations: the Lancelot wheel (8A / 8B), Open Key (1m / 1d) or
// musical names (Am, F♯m, B♭). displayKey() reads any of them and prints the
// key in the chosen format; text it cannot read for certain is shown exactly
// as stored, never blank and never guessed.
//
// Cheap on the GUI thread: the 3 × 24 names are built once, and each distinct
// stored text is parsed once (a hash lookup afterwards), so painting a row
// costs one lookup, never a parse.
namespace decklab {

enum class KeyFormat {
    Lancelot = 0, // 8A / 8B, the default
    OpenKey = 1,  // 1m / 1d
    Musical = 2,  // Am / C
};

/// The format every Deck Lab surface shows. Lancelot until set.
KeyFormat keyFormat();
void setKeyFormat(KeyFormat format);
/// Sets the format and tells Mixxx's own tables (their key text, the key
/// sort order and [Library],key_notation) to use the same notation.
void applyKeyFormat(KeyFormat format);
/// [Key] KeyNotation values: "Lancelot", "OpenKey", "Traditional" (kept for
/// compatibility with Mixxx's preferences). Anything else (missing, the
/// desktop's combined or custom notations) reads as the closest of the
/// three: OpenKeyAndTraditional is Open Key, everything else Lancelot.
KeyFormat keyFormatFromConfig(const QString& value);
QString keyFormatConfigValue(KeyFormat format);
/// Mixxx's KeyUtils::KeyNotation for the format, as a number.
int keyFormatNotation(KeyFormat format);
/// The format for a KeyUtils::KeyNotation number set elsewhere (the desktop
/// preferences): false for Custom and anything unknown.
bool keyFormatFromNotation(int notation, KeyFormat* format);
/// The Settings option names: "Lancelot (8A)", "Open Key (1m)", "Musical (Am)".
QString keyFormatName(KeyFormat format);
/// Without the example ("Lancelot", "Open Key", "Musical"): for text set in
/// capitals, where "AM" would not read as a key.
QString keyFormatShortName(KeyFormat format);

/// Reads stored key text. Accepts Lancelot (1A-12B, any case, leading
/// zeros), Open Key (1m-12d) and musical names (Am, A minor, F#m, G♭, Bbmaj,
/// lower case for minor as Mixxx reads it), and combined forms whose parts
/// agree ("8A (Am)", "F♯/G♭", "1m / Am"). INVALID when unsure. Any thread.
mixxx::track::io::key::ChromaticKey parseKeyText(const QString& text);
/// parseKeyText() with a cache: GUI thread only.
mixxx::track::io::key::ChromaticKey parseKey(const QString& text);

/// The key in the chosen (or a given) format; empty for INVALID.
const QString& formatKey(mixxx::track::io::key::ChromaticKey key);
const QString& formatKey(mixxx::track::io::key::ChromaticKey key, KeyFormat format);
/// What to show for stored key text: the key in the chosen format, or the
/// text exactly as stored when it is not a key we can read. GUI thread.
QString displayKey(const QString& text);
/// A key id wins when it is valid; otherwise the stored text as above.
QString displayKey(mixxx::track::io::key::ChromaticKey key, const QString& text);

/// Position on the wheel for sorting, 1..24 (0 for INVALID): Lancelot order
/// (1A, 1B, 2A ...) for Lancelot, Open Key order (1d, 1m, 2d ...) for Open
/// Key and musical names. The same order Mixxx's library sort uses.
int keyWheelOrder(mixxx::track::io::key::ChromaticKey key);
int keyWheelOrder(mixxx::track::io::key::ChromaticKey key, KeyFormat format);

/// The names for KeyUtils::setNotation().
QMap<mixxx::track::io::key::ChromaticKey, QString> keyNotationMap(KeyFormat format);

/// Browse's KEY cell (the delegate WTrackTableView installs on the key
/// column), for tests that paint through it. Defined in
/// widget/wtracktableview.cpp.
QAbstractItemDelegate* makeKeyCellDelegate(QTableView* table, int keyColumn, int bpmColumn);

} // namespace decklab
