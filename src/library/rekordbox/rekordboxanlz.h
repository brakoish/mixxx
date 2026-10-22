// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>
#include <QVector>
#include <optional>
#include <string>

#include "decklab/decklabphrase.h"

class rekordbox_anlz_t;

namespace mixxx::rekordbox {

// What Deck Lab takes from one rekordbox ANLZ analysis file, without exposing
// the generated Kaitai types. Fields stay empty when the tag is absent.
struct AnlzExtract {
    // PQTZ beat grid (in .DAT files): beat times in milliseconds, file order.
    QVector<double> beatTimesMs;
    // PSSI song structure (in .EXT files), unmasked.
    std::optional<decklab::SongStructure> songStructure;
};

// Reads the tags above from an already parsed file.
AnlzExtract extractAnlz(rekordbox_anlz_t& anlz);

// True when the file header and every tag fit inside the file. The generated
// parser allocates whatever length a tag claims before reading it, so a
// damaged length could otherwise ask for gigabytes on a 4 GB Pi.
bool anlzFramingIsSane(const std::string& bytes);

// Reads a whole ANLZ file (they are well under a megabyte) and checks its
// framing. Empty when the file is missing, too large or damaged.
std::string readAnlzFile(const QString& path);

// Parses a complete ANLZ file image. Returns false for malformed data and
// never throws: a damaged file on a stick must not take the deck down.
bool parseAnlz(const std::string& bytes, AnlzExtract* pExtract);

} // namespace mixxx::rekordbox
