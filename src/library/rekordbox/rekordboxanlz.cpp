// SPDX-License-Identifier: GPL-2.0-or-later
#include "library/rekordbox/rekordboxanlz.h"

#include <rekordbox_anlz.h>

#include <QFile>
#include <QtDebug>
#include <exception>

namespace mixxx::rekordbox {

namespace {

// Real ANLZ files are well under a megabyte; anything this large is not one.
constexpr qint64 kMaxAnlzBytes = 32 * 1024 * 1024;

quint64 bigEndian32(const std::string& bytes, size_t pos) {
    quint64 value = 0;
    for (size_t i = 0; i < 4; ++i) {
        value = (value << 8) | static_cast<unsigned char>(bytes[pos + i]);
    }
    return value;
}

int phraseKind(rekordbox_anlz_t::track_mood_t mood, kaitai::kstruct* kind) {
    if (!kind) {
        return 0;
    }
    // The generated parser picks the kind's type from the mood, reading an
    // unknown mood as "mid"; mirror that exactly.
    switch (mood) {
    case rekordbox_anlz_t::TRACK_MOOD_HIGH:
        return static_cast<int>(static_cast<rekordbox_anlz_t::phrase_high_t*>(kind)->id());
    case rekordbox_anlz_t::TRACK_MOOD_LOW:
        return static_cast<int>(static_cast<rekordbox_anlz_t::phrase_low_t*>(kind)->id());
    default:
        return static_cast<int>(static_cast<rekordbox_anlz_t::phrase_mid_t*>(kind)->id());
    }
}

decklab::SongStructure songStructure(rekordbox_anlz_t::song_structure_tag_t* tag) {
    decklab::SongStructure structure;
    auto* body = tag ? tag->body() : nullptr;
    if (!body || !body->entries()) {
        return structure;
    }
    structure.mood = static_cast<int>(body->mood());
    structure.endBeat = body->end_beat();
    structure.entries.reserve(static_cast<int>(body->entries()->size()));
    for (const auto& entry : *body->entries()) {
        decklab::SongStructureEntry phrase;
        phrase.index = entry->index();
        phrase.beat = entry->beat();
        phrase.kind = phraseKind(body->mood(), entry->kind());
        phrase.k1 = entry->k1();
        phrase.k2 = entry->k2();
        phrase.k3 = entry->k3();
        phrase.fill = entry->fill() != 0;
        phrase.fillBeat = entry->beat_fill();
        structure.entries.append(phrase);
    }
    return structure;
}

} // namespace

AnlzExtract extractAnlz(rekordbox_anlz_t& anlz) {
    AnlzExtract extract;
    if (!anlz.sections()) {
        return extract;
    }
    for (const auto& section : *anlz.sections()) {
        switch (section->fourcc()) {
        case rekordbox_anlz_t::SECTION_TAGS_BEAT_GRID: {
            auto* grid = static_cast<rekordbox_anlz_t::beat_grid_tag_t*>(section->body());
            if (!grid || !grid->beats() || !extract.beatTimesMs.isEmpty()) {
                break; // the first grid wins, as in the beat grid import
            }
            extract.beatTimesMs.reserve(static_cast<int>(grid->beats()->size()));
            for (const auto& beat : *grid->beats()) {
                extract.beatTimesMs.append(static_cast<double>(beat->time()));
            }
        } break;
        case rekordbox_anlz_t::SECTION_TAGS_SONG_STRUCTURE: {
            if (!extract.songStructure) {
                extract.songStructure = songStructure(
                        static_cast<rekordbox_anlz_t::song_structure_tag_t*>(section->body()));
            }
        } break;
        default:
            break;
        }
    }
    return extract;
}

bool anlzFramingIsSane(const std::string& bytes) {
    // "PMAI", header length, file length, then tags of fourcc, header length
    // and tag length until the end of the file, as the generated parser reads.
    if (bytes.size() < 12 || bytes.compare(0, 4, "PMAI") != 0) {
        return false;
    }
    const quint64 lenHeader = bigEndian32(bytes, 4);
    if (lenHeader < 12 || lenHeader > bytes.size()) {
        return false;
    }
    size_t pos = static_cast<size_t>(lenHeader);
    while (pos < bytes.size()) {
        if (bytes.size() - pos < 12) {
            return false;
        }
        const quint64 lenTagHeader = bigEndian32(bytes, pos + 4);
        const quint64 lenTag = bigEndian32(bytes, pos + 8);
        if (lenTag < 12 || lenTag > bytes.size() - pos || lenTagHeader > lenTag) {
            return false;
        }
        pos += static_cast<size_t>(lenTag);
    }
    return true;
}

std::string readAnlzFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxAnlzBytes) {
        return {};
    }
    const QByteArray data = file.readAll();
    std::string bytes(data.constData(), static_cast<size_t>(data.size()));
    if (!anlzFramingIsSane(bytes)) {
        qWarning() << "Damaged rekordbox analysis file:" << path;
        return {};
    }
    return bytes;
}

bool parseAnlz(const std::string& bytes, AnlzExtract* pExtract) {
    if (pExtract) {
        *pExtract = AnlzExtract();
    }
    if (!anlzFramingIsSane(bytes)) {
        return false;
    }
    try {
        kaitai::kstream stream(bytes);
        rekordbox_anlz_t anlz(&stream);
        if (pExtract) {
            *pExtract = extractAnlz(anlz);
        }
        return true;
    } catch (const std::exception& error) {
        qWarning() << "Unreadable rekordbox analysis data:" << error.what();
        return false;
    }
}

} // namespace mixxx::rekordbox
