// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabusbrelink.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QMap>
#include <QSqlError>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QtConcurrentRun>
#include <QtDebug>
#include <algorithm>
#include <cmath>
#include <optional>

#include "decklab/decklabmedia.h"
#include "decklab/decklabusbname.h"
#include "library/dao/playlistdao.h"
#include "library/dao/trackdao.h"
#include "library/queryutil.h"
#include "library/relocatedtrack.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/trackset/crate/crateid.h"
#include "moc_decklabusbrelink.cpp"
#include "proto/keys.pb.h"
#include "track/cueinfo.h"
#include "track/globaltrackcache.h"
#include "track/trackref.h"
#include "util/fileinfo.h"

namespace decklab {

namespace {

QString cleanMount(const QString& mount) {
    QString clean = QDir::cleanPath(mount);
    while (clean.size() > 1 && clean.endsWith(QLatin1Char('/'))) {
        clean.chop(1);
    }
    return clean;
}

QString volumeKey(const UsbVolume& volume) {
    return volume.uuid + QLatin1Char('\n') + cleanMount(volume.mount);
}

// A single, non-empty path segment that a mount point could be named after.
bool usableName(const QString& name) {
    return !name.isEmpty() && name != QStringLiteral(".") && name != QStringLiteral("..") &&
            !name.contains(QLatin1Char('/')) && !name.contains(QChar(0));
}

bool hasParentStep(const QString& relative) {
    return relative.split(QLatin1Char('/')).contains(QStringLiteral(".."));
}

// One way to read a path as "<root>/<relative>", where root has the shape of
// a mount folder: /media/<a>/<b>, /run/media/<user>/<b> or /media/<b>.
struct MediaReading {
    QString root;
    QString relative; // empty when the path is the root itself
};

QList<MediaReading> mediaReadings(const QString& path) {
    QList<MediaReading> readings;
    if (!path.startsWith(QLatin1Char('/'))) {
        return readings;
    }
    // parts[0] is empty (leading slash).
    const QStringList parts = path.split(QLatin1Char('/'));
    const auto reading = [&parts, &readings](int rootSegments) {
        if (parts.size() < rootSegments + 1) {
            return;
        }
        for (int index = 1; index <= rootSegments; ++index) {
            if (!usableName(parts[index])) {
                return;
            }
        }
        readings.append({parts.mid(0, rootSegments + 1).join(QLatin1Char('/')),
                parts.mid(rootSegments + 1).join(QLatin1Char('/'))});
    };
    // /media/<user>/<name>/... (udisks2 on Debian/Ubuntu/Raspberry Pi OS,
    // and /media/decklab/<uuid> when the appliance mount root moved)
    if (parts.value(1) == QStringLiteral("media")) {
        reading(3);
    }
    // /run/media/<user>/<name>/... (udisks2 on Fedora/Arch)
    if (parts.value(1) == QStringLiteral("run") && parts.value(2) == QStringLiteral("media")) {
        reading(4);
    }
    // /media/<name>/... (usbmount and hand-made mounts)
    if (parts.value(1) == QStringLiteral("media")) {
        reading(2);
    }
    return readings;
}

// Never the current mount, nor a folder that contains it (for example
// /media/decklab for a label "decklab"): rows there belong to the current
// mount or to another stick.
bool excludedRoot(const QString& root, const QString& mount) {
    return root == mount || mount.startsWith(QString(root + QLatin1Char('/')));
}

bool run(QSqlQuery& query) {
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    return true;
}

// --- Merging one row into another (applyRelink) ---

struct CueRow {
    qint64 id = -1;
    int type = 0;
    double position = 0;
    double length = 0;
    int hotcue = -1;
};

std::optional<QList<CueRow>> readCues(const QSqlDatabase& database, qint64 trackId) {
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT id, type, position, length, hotcue FROM cues WHERE track_id=:id"));
    query.bindValue(QStringLiteral(":id"), trackId);
    if (!run(query)) {
        return std::nullopt;
    }
    QList<CueRow> cues;
    while (query.next()) {
        cues.append({query.value(0).toLongLong(),
                query.value(1).toInt(),
                query.value(2).toDouble(),
                query.value(3).toDouble(),
                query.value(4).toInt()});
    }
    return cues;
}

// Cues a track has once at most.
bool singleCue(int type) {
    return type == static_cast<int>(mixxx::CueType::MainCue) ||
            type == static_cast<int>(mixxx::CueType::Intro) ||
            type == static_cast<int>(mixxx::CueType::Outro) ||
            type == static_cast<int>(mixxx::CueType::N60dBSound);
}

// The survivor already has this cue: the same hot cue slot (a hot cue or a
// hot loop), the same single cue (main, intro, outro), or the same saved
// loop or jump within a frame. The survivor's own always wins.
bool survivorHas(const QList<CueRow>& survivor, const CueRow& cue) {
    for (const auto& own : survivor) {
        if (cue.hotcue >= 0 || own.hotcue >= 0) {
            if (cue.hotcue == own.hotcue) {
                return true;
            }
            continue;
        }
        if (own.type != cue.type) {
            continue;
        }
        if (singleCue(cue.type) ||
                (std::abs(own.position - cue.position) < 1 && std::abs(own.length - cue.length) < 1)) {
            return true;
        }
    }
    return false;
}

const QString kLibraryValues = QStringLiteral(
        "mixxx_deleted, timesplayed, last_played_at, played, datetime_added, rating, color, "
        "cuepoint, bpm, beats, beats_version, beats_sub_version, bpm_lock, key, key_id, keys, "
        "keys_version, keys_sub_version");

struct LibraryValues {
    bool hidden = false;
    QVariant timesPlayed;
    QVariant lastPlayedAt;
    QVariant played;
    QVariant dateAdded;
    QVariant rating;
    QVariant color;
    QVariant cuePoint;
    // The beat grid, taken as one.
    QVariant bpm;
    QVariant beats;
    QVariant beatsVersion;
    QVariant beatsSubVersion;
    QVariant bpmLock;
    // The key, taken as one.
    QVariant key;
    QVariant keyId;
    QVariant keys;
    QVariant keysVersion;
    QVariant keysSubVersion;
};

std::optional<LibraryValues> readLibraryValues(const QSqlDatabase& database, qint64 trackId, bool* error) {
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT %1 FROM library WHERE id=:id").arg(kLibraryValues));
    query.bindValue(QStringLiteral(":id"), trackId);
    if (!run(query)) {
        *error = true;
        return std::nullopt;
    }
    if (!query.next()) {
        return std::nullopt;
    }
    LibraryValues values;
    values.hidden = query.value(0).toInt() != 0;
    values.timesPlayed = query.value(1);
    values.lastPlayedAt = query.value(2);
    values.played = query.value(3);
    values.dateAdded = query.value(4);
    values.rating = query.value(5);
    values.color = query.value(6);
    values.cuePoint = query.value(7);
    values.bpm = query.value(8);
    values.beats = query.value(9);
    values.beatsVersion = query.value(10);
    values.beatsSubVersion = query.value(11);
    values.bpmLock = query.value(12);
    values.key = query.value(13);
    values.keyId = query.value(14);
    values.keys = query.value(15);
    values.keysVersion = query.value(16);
    values.keysSubVersion = query.value(17);
    return values;
}

// SQLite's CURRENT_TIMESTAMP ("2026-10-23 21:04:12"), or ISO 8601; both UTC.
QDateTime storedTime(const QVariant& value) {
    if (value.isNull()) {
        return QDateTime();
    }
    const QString text = value.toString().trimmed();
    QDateTime time = QDateTime::fromString(text, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    if (!time.isValid()) {
        time = QDateTime::fromString(text, Qt::ISODateWithMs);
    }
    return time;
}

// A locked tempo is the DJ's own; any analysed grid beats none.
int gridRank(const LibraryValues& values) {
    if (values.bpmLock.toInt() != 0) {
        return 2;
    }
    return values.beats.toByteArray().isEmpty() ? 0 : 1;
}

// A key the DJ set beats an analysed or tagged one, which beats none.
int keyRank(const LibraryValues& values) {
    const QByteArray blob = values.keys.toByteArray();
    if (!blob.isEmpty()) {
        mixxx::track::io::key::KeyMap map;
        if (map.ParseFromArray(blob.constData(), static_cast<int>(blob.size())) &&
                map.source() == mixxx::track::io::key::USER) {
            return 2;
        }
        return 1;
    }
    return values.keyId.toInt() != 0 || !values.key.toString().isEmpty() ? 1 : 0;
}

// -1 a query failed (roll back), 0 left as it is (changed since planning), 1 merged.
int mergeInto(QSqlDatabase& database,
        const RelinkRow& survivorRow,
        const QString& survivorLocation,
        const RelinkRow& row,
        RelinkResult* result) {
    const qint64 survivor = survivorRow.trackId;
    const qint64 merged = row.trackId;
    bool error = false;
    const auto keep = readLibraryValues(database, survivor, &error);
    const auto other = readLibraryValues(database, merged, &error);
    if (error) {
        return -1;
    }
    if (!keep || !other || keep->hidden || other->hidden) {
        return 0;
    }
    // Cues and loops the survivor lacks move over (never copied, never twice).
    const auto keepCues = readCues(database, survivor);
    const auto otherCues = readCues(database, merged);
    if (!keepCues || !otherCues) {
        return -1;
    }
    QList<CueRow> survivorCues = *keepCues;
    bool movedMainCue = false;
    QSqlQuery moveCue(database);
    moveCue.prepare(QStringLiteral("UPDATE cues SET track_id=:survivor WHERE id=:id AND track_id=:merged"));
    for (const auto& cue : *otherCues) {
        if (survivorHas(survivorCues, cue)) {
            continue;
        }
        moveCue.bindValue(QStringLiteral(":survivor"), survivor);
        moveCue.bindValue(QStringLiteral(":id"), cue.id);
        moveCue.bindValue(QStringLiteral(":merged"), merged);
        if (!run(moveCue)) {
            return -1;
        }
        survivorCues.append(cue);
        ++result->cues;
        movedMainCue |= cue.type == static_cast<int>(mixxx::CueType::MainCue) && cue.hotcue < 0;
    }
    // Crates: the survivor joins every crate the other row is in. A crate
    // holding both keeps the other row's entry where it is.
    QSqlQuery crates(database);
    crates.prepare(QStringLiteral(
            "SELECT crate_id FROM crate_tracks WHERE track_id=:merged AND crate_id NOT IN "
            "(SELECT crate_id FROM crate_tracks WHERE track_id=:survivor)"));
    crates.bindValue(QStringLiteral(":merged"), merged);
    crates.bindValue(QStringLiteral(":survivor"), survivor);
    if (!run(crates)) {
        return -1;
    }
    QList<qint64> crateIds;
    while (crates.next()) {
        crateIds.append(crates.value(0).toLongLong());
    }
    QSqlQuery moveCrate(database);
    moveCrate.prepare(QStringLiteral(
            "UPDATE crate_tracks SET track_id=:survivor WHERE crate_id=:crate AND track_id=:merged"));
    for (const qint64 crate : std::as_const(crateIds)) {
        moveCrate.bindValue(QStringLiteral(":survivor"), survivor);
        moveCrate.bindValue(QStringLiteral(":crate"), crate);
        moveCrate.bindValue(QStringLiteral(":merged"), merged);
        if (!run(moveCrate)) {
            return -1;
        }
        result->crateIds.insert(crate);
        ++result->crates;
    }
    // History, playlists and the Auto DJ queue point at the survivor, in
    // place (same playlist, same position, same time).
    QSqlQuery playlists(database);
    playlists.prepare(QStringLiteral("SELECT DISTINCT playlist_id FROM PlaylistTracks WHERE track_id=:merged"));
    playlists.bindValue(QStringLiteral(":merged"), merged);
    if (!run(playlists)) {
        return -1;
    }
    while (playlists.next()) {
        result->playlistIds.insert(playlists.value(0).toInt());
    }
    QSqlQuery entries(database);
    entries.prepare(QStringLiteral("UPDATE PlaylistTracks SET track_id=:survivor WHERE track_id=:merged"));
    entries.bindValue(QStringLiteral(":survivor"), survivor);
    entries.bindValue(QStringLiteral(":merged"), merged);
    if (!run(entries)) {
        return -1;
    }
    result->entries += entries.numRowsAffected();
    // Play count: the higher; last played: the later; added: the earlier;
    // rating and colour: the survivor's unless it has none; grid and key:
    // from the row the DJ edited, else the one that was analysed.
    LibraryValues values = *keep;
    values.timesPlayed = std::max(keep->timesPlayed.toInt(), other->timesPlayed.toInt());
    values.played = std::max(keep->played.toInt(), other->played.toInt());
    const QDateTime keepPlayed = storedTime(keep->lastPlayedAt);
    const QDateTime otherPlayed = storedTime(other->lastPlayedAt);
    if (otherPlayed.isValid() && (!keepPlayed.isValid() || otherPlayed > keepPlayed)) {
        values.lastPlayedAt = other->lastPlayedAt;
    }
    const QDateTime keepAdded = storedTime(keep->dateAdded);
    const QDateTime otherAdded = storedTime(other->dateAdded);
    if (otherAdded.isValid() && (!keepAdded.isValid() || otherAdded < keepAdded)) {
        values.dateAdded = other->dateAdded;
    }
    if (keep->rating.toInt() <= 0) {
        values.rating = other->rating;
    }
    if (keep->color.isNull()) {
        values.color = other->color;
    }
    if (movedMainCue) {
        values.cuePoint = other->cuePoint;
    }
    if (gridRank(*other) > gridRank(*keep)) {
        values.bpm = other->bpm;
        values.beats = other->beats;
        values.beatsVersion = other->beatsVersion;
        values.beatsSubVersion = other->beatsSubVersion;
        values.bpmLock = other->bpmLock;
    }
    if (keyRank(*other) > keyRank(*keep)) {
        values.key = other->key;
        values.keyId = other->keyId;
        values.keys = other->keys;
        values.keysVersion = other->keysVersion;
        values.keysSubVersion = other->keysSubVersion;
    }
    QSqlQuery update(database);
    update.prepare(QStringLiteral(
            "UPDATE library SET timesplayed=:timesplayed, last_played_at=:last_played_at, "
            "played=:played, datetime_added=:datetime_added, rating=:rating, color=:color, "
            "cuepoint=:cuepoint, bpm=:bpm, beats=:beats, beats_version=:beats_version, "
            "beats_sub_version=:beats_sub_version, bpm_lock=:bpm_lock, key=:key, key_id=:key_id, "
            "keys=:keys, keys_version=:keys_version, keys_sub_version=:keys_sub_version "
            "WHERE id=:id"));
    update.bindValue(QStringLiteral(":timesplayed"), values.timesPlayed);
    update.bindValue(QStringLiteral(":last_played_at"), values.lastPlayedAt);
    update.bindValue(QStringLiteral(":played"), values.played);
    update.bindValue(QStringLiteral(":datetime_added"), values.dateAdded);
    update.bindValue(QStringLiteral(":rating"), values.rating);
    update.bindValue(QStringLiteral(":color"), values.color);
    update.bindValue(QStringLiteral(":cuepoint"), values.cuePoint);
    update.bindValue(QStringLiteral(":bpm"), values.bpm);
    update.bindValue(QStringLiteral(":beats"), values.beats);
    update.bindValue(QStringLiteral(":beats_version"), values.beatsVersion);
    update.bindValue(QStringLiteral(":beats_sub_version"), values.beatsSubVersion);
    update.bindValue(QStringLiteral(":bpm_lock"), values.bpmLock);
    update.bindValue(QStringLiteral(":key"), values.key);
    update.bindValue(QStringLiteral(":key_id"), values.keyId);
    update.bindValue(QStringLiteral(":keys"), values.keys);
    update.bindValue(QStringLiteral(":keys_version"), values.keysVersion);
    update.bindValue(QStringLiteral(":keys_sub_version"), values.keysSubVersion);
    update.bindValue(QStringLiteral(":id"), survivor);
    if (!run(update)) {
        return -1;
    }
    // The merged row is hidden, never deleted: whatever stayed with it
    // (cues and crate entries the survivor already had) stays too.
    QSqlQuery hide(database);
    hide.prepare(QStringLiteral("UPDATE library SET mixxx_deleted=1 WHERE id=:id"));
    hide.bindValue(QStringLiteral(":id"), merged);
    if (!run(hide)) {
        return -1;
    }
    result->merged.append({survivor, merged, row.location, survivorLocation});
    return 1;
}

// -1 a query failed, 0 left (the row moved or the address is taken), 1 moved.
int relocate(QSqlDatabase& database, const RelinkRow& row, const QString& target) {
    QSqlQuery update(database);
    update.prepare(QStringLiteral(
            "UPDATE track_locations SET location=:new, directory=:directory, filename=:filename, "
            "fs_deleted=0 WHERE id=:id AND location=:old AND NOT EXISTS "
            "(SELECT 1 FROM track_locations WHERE location=:taken)"));
    const QFileInfo info(target);
    update.bindValue(QStringLiteral(":new"), target);
    update.bindValue(QStringLiteral(":directory"), info.path());
    update.bindValue(QStringLiteral(":filename"), info.fileName());
    update.bindValue(QStringLiteral(":id"), row.locationId);
    update.bindValue(QStringLiteral(":old"), row.location);
    update.bindValue(QStringLiteral(":taken"), target);
    if (!run(update)) {
        return -1;
    }
    return update.numRowsAffected() == 1 ? 1 : 0;
}

} // namespace

QList<UsbVolume> usbVolumesFromDrives(const QJsonArray& drives) {
    QList<UsbVolume> volumes;
    for (const auto& value : drives) {
        const QJsonObject drive = value.toObject();
        UsbVolume volume;
        volume.uuid = drive.value(QStringLiteral("id")).toString();
        volume.label = drive.value(QStringLiteral("label")).toString();
        volume.mount = drive.value(QStringLiteral("mount")).toString();
        if (!usableName(volume.uuid) || !volume.mount.startsWith(QLatin1Char('/'))) {
            continue;
        }
        volume.mount = cleanMount(volume.mount);
        if (volume.mount == QStringLiteral("/")) {
            continue;
        }
        volumes.append(volume);
    }
    return volumes;
}

bool isEarlierMountName(const UsbVolume& volume, const QString& name) {
    if (!usableName(name)) {
        return false;
    }
    const auto matches = [&name](const QString& base, Qt::CaseSensitivity sensitivity) {
        if (!usableName(base) || !name.startsWith(base, sensitivity)) {
            return false;
        }
        // udisks adds a number when the folder name is taken: "MY STICK1".
        const QStringView rest = QStringView(name).mid(base.size());
        return std::all_of(rest.begin(), rest.end(), [](QChar c) { return c.isDigit(); });
    };
    return matches(volume.label, Qt::CaseSensitive) || matches(volume.uuid, Qt::CaseInsensitive);
}

QStringList legacyRelativePaths(const UsbVolume& volume, const QString& location) {
    const QString mount = cleanMount(volume.mount);
    QStringList result;
    for (const auto& reading : mediaReadings(location)) {
        if (reading.relative.isEmpty() || hasParentStep(reading.relative) ||
                excludedRoot(reading.root, mount) ||
                !isEarlierMountName(volume, reading.root.section(QLatin1Char('/'), -1))) {
            continue;
        }
        if (!result.contains(reading.relative)) {
            result.append(reading.relative);
        }
    }
    return result;
}

qint64 regularFileSize(const QString& path) {
    const QFileInfo info(path);
    return info.isFile() ? info.size() : -1;
}

RelinkProbe fileSystemProbe() {
    RelinkProbe probe;
    probe.fileSize = regularFileSize;
    probe.resolve = [](const QString& path) {
        return QFileInfo(path).canonicalFilePath();
    };
    probe.isIn = mediaRootIsIn;
    return probe;
}

RelinkPlan planRelink(const UsbVolume& volume,
        const QList<RelinkRow>& rows,
        const QStringList& folders,
        const RelinkProbe& probe,
        const QSet<qint64>& busyTrackIds) {
    RelinkPlan plan;
    const QString mount = cleanMount(volume.mount);
    const QString prefix = mount + QLatin1Char('/');
    // Is this folder an earlier address of the volume? Asked once per folder.
    QHash<QString, bool> earlierRoots;
    const auto isEarlierRoot = [&](const QString& root) {
        const auto known = earlierRoots.constFind(root);
        if (known != earlierRoots.constEnd()) {
            return known.value();
        }
        bool earlier = false;
        if (!excludedRoot(root, mount)) {
            const QString resolved = probe.resolve ? probe.resolve(root) : QString();
            if (!resolved.isEmpty() && cleanMount(resolved) == mount) {
                // A link to the stick's mount (the appliance's
                // /media/<user>/<label>): the very same files.
                earlier = true;
            } else if (isEarlierMountName(volume, root.section(QLatin1Char('/'), -1))) {
                // An automounter's folder for this stick, unless another
                // filesystem is in it now (another stick with that label).
                earlier = !(probe.isIn && probe.isIn(root));
            }
        }
        earlierRoots.insert(root, earlier);
        return earlier;
    };

    // Every address in use, hidden rows included: a move never takes one.
    QSet<QString> taken;
    for (const auto& row : rows) {
        taken.insert(row.location);
    }
    struct Song {
        std::optional<RelinkRow> current; // the row at the current address
        QList<RelinkRow> earlier;         // rows at earlier addresses
    };
    QMap<QString, Song> songs; // by relative path, in a stable order
    for (const auto& row : rows) {
        if (row.hidden) {
            continue; // hidden by the DJ, or merged before: as it is
        }
        if (row.location.startsWith(prefix)) {
            const QString relative = row.location.mid(prefix.size());
            if (!relative.isEmpty() && !hasParentStep(relative)) {
                songs[relative].current = row;
            }
            continue;
        }
        for (const auto& reading : mediaReadings(row.location)) {
            if (!reading.relative.isEmpty() && !hasParentStep(reading.relative) &&
                    isEarlierRoot(reading.root)) {
                songs[reading.relative].earlier.append(row);
                break;
            }
        }
    }

    for (auto it = songs.cbegin(); it != songs.cend(); ++it) {
        const Song& song = it.value();
        const bool currentMissing = song.current && song.current->missing;
        if (song.earlier.isEmpty() && !currentMissing) {
            continue; // nothing to do: no file check
        }
        const QString target = prefix + it.key();
        const qint64 size = probe.fileSize ? probe.fileSize(target) : -1;
        if (size < 0) {
            // Not on the stick (any more): every row stays as it is.
            plan.missing += static_cast<int>(song.earlier.size());
            continue;
        }
        if (currentMissing) {
            plan.revived.append(*song.current);
        }
        // Only rows recorded with this file's size are this file.
        QList<RelinkRow> same;
        for (const auto& row : song.earlier) {
            if (row.fileSize == size) {
                same.append(row);
            } else {
                ++plan.sizeMismatch;
            }
        }
        if (same.isEmpty()) {
            continue;
        }
        if (song.current && song.current->fileSize > 0 && song.current->fileSize != size) {
            // The row at the current address describes another file.
            plan.sizeMismatch += static_cast<int>(same.size());
            continue;
        }
        bool busy = song.current && busyTrackIds.contains(song.current->trackId);
        for (const auto& row : std::as_const(same)) {
            busy = busy || busyTrackIds.contains(row.trackId);
        }
        if (busy) {
            ++plan.busy;
            continue;
        }
        RelinkGroup group;
        group.target = target;
        if (song.current) {
            // Survivor: the row at the stick's current address. Its location
            // is already right, so nothing that finds tracks by path (the
            // stick's rekordbox lists and folders, the library scan, the
            // played marks) needs it rewritten, and it is the row this
            // stick's tracks have loaded as since the appliance mounted it:
            // it holds the current analysis. What only the others have
            // (cues, crates, history, counts, a locked grid, a key the DJ
            // set) moves to it.
            group.survivor = *song.current;
            group.merged = same;
        } else {
            if (taken.contains(target)) {
                ++plan.blocked; // a hidden row holds the address
                continue;
            }
            // No row at the current address: the oldest row (first seen,
            // the longest history) moves there and the others merge into it.
            std::sort(same.begin(), same.end(), [](const RelinkRow& a, const RelinkRow& b) {
                return a.trackId < b.trackId;
            });
            group.survivor = same.takeFirst();
            group.relocate = true;
            group.merged = same;
        }
        plan.groups.append(group);
    }

    // Library folders at an earlier address follow too, when the folder is
    // on the stick and not already a library folder.
    QSet<QString> known;
    for (const QString& folder : folders) {
        known.insert(cleanMount(folder));
    }
    for (const QString& folder : folders) {
        const QString clean = cleanMount(folder);
        if (clean == mount || clean.startsWith(prefix)) {
            continue;
        }
        for (const auto& reading : mediaReadings(clean)) {
            if (hasParentStep(reading.relative) || !isEarlierRoot(reading.root)) {
                continue;
            }
            const QString target = reading.relative.isEmpty() ? mount : prefix + reading.relative;
            if (!known.contains(target) && probe.resolve && !probe.resolve(target).isEmpty()) {
                plan.folders.append({folder, target});
                known.insert(target);
            }
            break;
        }
    }
    return plan;
}

QList<RelinkRow> relinkCandidates(const QSqlDatabase& database) {
    QList<RelinkRow> rows;
    QSqlQuery query(database);
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
            "SELECT library.id, track_locations.id, track_locations.location, "
            "track_locations.filesize, library.mixxx_deleted, track_locations.fs_deleted "
            "FROM track_locations INNER JOIN library ON library.location=track_locations.id "
            "WHERE (track_locations.location >= '/media/' AND track_locations.location < '/media0') "
            "OR (track_locations.location >= '/run/media/' AND track_locations.location < '/run/media0')"));
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return rows;
    }
    while (query.next()) {
        RelinkRow row;
        row.trackId = query.value(0).toLongLong();
        row.locationId = query.value(1).toLongLong();
        row.location = query.value(2).toString();
        row.fileSize = query.value(3).toLongLong();
        row.hidden = query.value(4).toInt() != 0;
        row.missing = query.value(5).toInt() != 0;
        rows.append(row);
    }
    return rows;
}

QStringList libraryFolders(const QSqlDatabase& database) {
    QStringList folders;
    QSqlQuery query(database);
    query.setForwardOnly(true);
    if (!query.exec(QStringLiteral("SELECT directory FROM directories"))) {
        LOG_FAILED_QUERY(query);
        return folders;
    }
    while (query.next()) {
        folders.append(query.value(0).toString());
    }
    return folders;
}

RelinkResult applyRelink(QSqlDatabase& database, const RelinkPlan& plan) {
    RelinkResult result;
    if (plan.isEmpty()) {
        result.ok = true;
        return result;
    }
    ScopedTransaction transaction(database);
    if (!transaction.active()) {
        return result;
    }
    for (const auto& group : plan.groups) {
        if (group.relocate) {
            const int moved = relocate(database, group.survivor, group.target);
            if (moved < 0) {
                return RelinkResult();
            }
            if (moved == 0) {
                continue; // changed since planning: the whole song waits
            }
            result.relocated.append({group.survivor.trackId, group.survivor.location, group.target});
        }
        for (const auto& row : group.merged) {
            if (mergeInto(database, group.survivor, group.target, row, &result) < 0) {
                return RelinkResult();
            }
        }
    }
    QSqlQuery revive(database);
    revive.prepare(QStringLiteral(
            "UPDATE track_locations SET fs_deleted=0 WHERE id=:id AND location=:location AND fs_deleted=1"));
    for (const auto& row : plan.revived) {
        revive.bindValue(QStringLiteral(":id"), row.locationId);
        revive.bindValue(QStringLiteral(":location"), row.location);
        if (!run(revive)) {
            return RelinkResult();
        }
        result.revived += revive.numRowsAffected() == 1 ? 1 : 0;
    }
    QSqlQuery folder(database);
    folder.prepare(QStringLiteral(
            "UPDATE directories SET directory=:new WHERE directory=:old AND NOT EXISTS "
            "(SELECT 1 FROM directories WHERE directory=:taken)"));
    for (const auto& [from, to] : plan.folders) {
        folder.bindValue(QStringLiteral(":new"), to);
        folder.bindValue(QStringLiteral(":old"), from);
        folder.bindValue(QStringLiteral(":taken"), to);
        if (!run(folder)) {
            return RelinkResult();
        }
        result.folders += folder.numRowsAffected() == 1 ? 1 : 0;
    }
    if (!transaction.commit()) {
        return RelinkResult();
    }
    result.ok = true;
    return result;
}

QString backupLibraryDatabase(const QSqlDatabase& database, int keep) {
    const QFileInfo file(database.databaseName());
    if (!file.isFile()) {
        return QString();
    }
    const QDir folder = file.dir();
    const QString stem = file.fileName() + QStringLiteral(".decklab-merge-");
    // Names sort by time.
    const QString base = folder.filePath(stem + QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    QString copy = base;
    for (int n = 2; QFileInfo::exists(copy); ++n) {
        copy = base + QLatin1Char('-') + QString::number(n);
    }
    QString literal = copy;
    literal.replace(QLatin1Char('\''), QStringLiteral("''"));
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("VACUUM INTO '%1'").arg(literal))) {
        LOG_FAILED_QUERY(query);
        QFile::remove(copy);
        return QString();
    }
    QStringList copies = folder.entryList(QStringList{stem + QLatin1Char('*')}, QDir::Files, QDir::Name);
    while (copies.size() > std::max(1, keep)) {
        QFile::remove(folder.filePath(copies.takeFirst()));
    }
    return copy;
}

UsbRelinker::UsbRelinker(std::shared_ptr<TrackCollectionManager> library, QObject* parent)
        : QObject(parent),
          m_library(std::move(library)),
          m_probe(fileSystemProbe()) {
    m_busy = [] {
        QSet<qint64> ids;
        const auto cached = GlobalTrackCacheLocker().getCachedTrackIds();
        for (const auto& id : cached) {
            ids.insert(id.toVariant().toLongLong());
        }
        return ids;
    };
    connect(&m_watcher, &QFutureWatcher<RelinkPlan>::finished, this, &UsbRelinker::finish);
    m_retry.setSingleShot(true);
    m_retry.setInterval(10000);
    connect(&m_retry, &QTimer::timeout, this, &UsbRelinker::startNext);
    m_settle.setSingleShot(true);
    m_settle.setInterval(700);
    connect(&m_settle, &QTimer::timeout, this, [this] {
        rewatch();
        relinkMounted();
    });
}

UsbRelinker::~UsbRelinker() {
    m_watcher.waitForFinished();
}

void UsbRelinker::relinkDrives(const QJsonArray& drives) {
    refreshMedia();
    const QList<UsbVolume> volumes = usbVolumesFromDrives(drives);
    // A stick that went (or moved) is looked at again when it is back.
    QSet<QString> present;
    for (const auto& volume : volumes) {
        present.insert(volumeKey(volume));
    }
    m_done.intersect(present);
    queue(volumes);
}

void UsbRelinker::relinkMounted() {
    refreshMedia();
    QList<UsbVolume> volumes;
    const bool testing = usbMediaRootsAreForTesting();
    for (const QString& root : usbMediaRoots()) {
        const QFileInfoList entries = QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo& entry : entries) {
            const QString path = cleanMount(entry.absoluteFilePath());
            // Only real mounts (generated folders in tests).
            if (!usableName(entry.fileName()) || (!testing && QStorageInfo(path).rootPath() != path)) {
                continue;
            }
            volumes.append({entry.fileName(), testing ? QString() : usbLabelForUuid(entry.fileName()), path});
        }
    }
    QSet<QString> present;
    for (const auto& volume : std::as_const(volumes)) {
        present.insert(volumeKey(volume));
    }
    m_done.intersect(present);
    queue(volumes);
}

void UsbRelinker::refreshMedia(bool force) {
    TrackCollection* collection = m_library ? m_library->internalCollection() : nullptr;
    if (!collection) {
        return;
    }
    if (!MediaPresence::instance().refresh(collection->database(), force)) {
        return;
    }
    // Crate and playlist counts leave out tracks on sticks that are out.
    QSqlQuery query(collection->database());
    query.setForwardOnly(true);
    QSet<CrateId> crates;
    if (query.exec(QStringLiteral("SELECT id FROM crates"))) {
        while (query.next()) {
            crates.insert(CrateId(query.value(0)));
        }
    }
    QSet<int> playlists;
    if (query.exec(QStringLiteral("SELECT id FROM Playlists WHERE hidden=%1")
                               .arg(static_cast<int>(PlaylistDAO::PLHT_NOT_HIDDEN)))) {
        while (query.next()) {
            playlists.insert(query.value(0).toInt());
        }
    }
    if (!crates.isEmpty()) {
        emit collection->crateSummaryChanged(crates);
    }
    if (!playlists.isEmpty()) {
        emit collection->getPlaylistDAO().playlistContentChanged(playlists);
    }
}

void UsbRelinker::watchMediaFolders() {
    if (!m_folders) {
        m_folders = new QFileSystemWatcher(this);
        connect(m_folders, &QFileSystemWatcher::directoryChanged, this, [this] { m_settle.start(); });
    }
    rewatch();
}

void UsbRelinker::rewatch() {
    if (!m_folders) {
        return;
    }
    // A folder that appears later (/media/decklab on the first mount) is
    // picked up through /media.
    QStringList candidates{QStringLiteral("/media")};
    candidates << usbMediaRoots();
    const QString user = QString::fromLocal8Bit(qgetenv("USER"));
    if (usableName(user)) {
        candidates << QStringLiteral("/media/") + user << QStringLiteral("/run/media/") + user;
    }
    QStringList wanted;
    for (const QString& folder : std::as_const(candidates)) {
        if (QFileInfo(folder).isDir() && !wanted.contains(folder)) {
            wanted.append(folder);
        }
    }
    const QStringList watched = m_folders->directories();
    for (const QString& folder : watched) {
        if (!wanted.contains(folder)) {
            m_folders->removePath(folder);
        }
    }
    for (const QString& folder : std::as_const(wanted)) {
        if (!watched.contains(folder)) {
            m_folders->addPath(folder);
        }
    }
}

void UsbRelinker::queue(const QList<UsbVolume>& volumes) {
    for (const auto& volume : volumes) {
        const QString key = volumeKey(volume);
        if (m_done.contains(key) || (m_watcher.isRunning() && volumeKey(m_running) == key)) {
            continue;
        }
        const bool queued = std::any_of(m_pending.cbegin(), m_pending.cend(), [&key](const UsbVolume& pending) {
            return volumeKey(pending) == key;
        });
        if (!queued) {
            m_pending.append(volume);
        }
    }
    startNext();
}

void UsbRelinker::startNext() {
    if (m_watcher.isRunning() || m_pending.isEmpty() || !m_library) {
        return;
    }
    // Never while anything plays: tried again when it stops.
    if (m_playing && m_playing()) {
        m_retry.start();
        return;
    }
    TrackCollection* collection = m_library->internalCollection();
    if (!collection) {
        return;
    }
    const QSqlDatabase database = collection->database();
    const QList<RelinkRow> rows = relinkCandidates(database);
    const QStringList folders = libraryFolders(database);
    const QSet<qint64> busy = m_busy ? m_busy() : QSet<qint64>();
    m_running = m_pending.takeFirst();
    const UsbVolume volume = m_running;
    const RelinkProbe probe = m_probe;
    // Only stat() calls and directory reads on the worker: the stick is
    // read, never written.
    m_watcher.setFuture(QtConcurrent::run([volume, rows, folders, probe, busy] {
        return planRelink(volume, rows, folders, probe, busy);
    }));
}

void UsbRelinker::finish() {
    const UsbVolume volume = m_running;
    RelinkPlan plan = m_watcher.result();
    if (m_playing && m_playing()) {
        // A deck started while the stick was read: later, planned afresh.
        m_pending.prepend(volume);
        m_retry.start();
        return;
    }
    // A track may have been loaded while the worker checked files: its song waits.
    const QSet<qint64> busy = m_busy ? m_busy() : QSet<qint64>();
    QList<RelinkGroup> groups;
    for (const auto& group : std::as_const(plan.groups)) {
        bool inUse = busy.contains(group.survivor.trackId);
        for (const auto& row : group.merged) {
            inUse = inUse || busy.contains(row.trackId);
        }
        if (inUse) {
            ++plan.busy;
        } else {
            groups.append(group);
        }
    }
    plan.groups = groups;
    RelinkResult result;
    result.ok = true;
    TrackCollection* collection = m_library ? m_library->internalCollection() : nullptr;
    if (collection && !plan.isEmpty()) {
        QSqlDatabase database = collection->database();
        const QString copy = backupLibraryDatabase(database);
        if (copy.isEmpty()) {
            qWarning() << "Deck Lab USB merge: the library could not be copied first; nothing changed";
            result.ok = false;
        } else {
            result = applyRelink(database, plan);
            if (result.ok) {
                notify(result);
            } else {
                qWarning() << "Deck Lab USB merge: the changes were rolled back";
            }
        }
    }
    const int relocated = static_cast<int>(result.relocated.size());
    const int merged = static_cast<int>(result.merged.size());
    qInfo() << "Deck Lab USB merge" << volume.uuid << "->" << volume.mount << ":" << relocated
            << "moved," << merged << "merged (" << result.cues << "cues," << result.crates
            << "crate entries," << result.entries << "history and playlist entries ),"
            << result.revived << "found again," << result.folders << "library folders moved,"
            << plan.sizeMismatch << "same path but another size," << plan.missing
            << "not on the stick," << plan.blocked << "address held by a hidden track,"
            << plan.busy << "in use";
    if (plan.busy == 0 && result.ok) {
        m_done.insert(volumeKey(volume));
    }
    // The views select again: rows moved, merged or found again.
    refreshMedia(result.ok && !plan.isEmpty());
    emit relinked(relocated, merged);
    startNext();
}

void UsbRelinker::notify(const RelinkResult& result) {
    TrackCollection* collection = m_library ? m_library->internalCollection() : nullptr;
    if (!collection) {
        return;
    }
    TrackDAO& tracks = collection->getTrackDAO();
    if (!result.relocated.isEmpty()) {
        QList<RelocatedTrack> relocated;
        relocated.reserve(result.relocated.size());
        for (const auto& moved : result.relocated) {
            relocated.append(RelocatedTrack(
                    TrackRef::fromFileInfo(mixxx::FileInfo(moved.from), TrackId(QVariant(moved.trackId))),
                    TrackRef::fromFileInfo(mixxx::FileInfo(moved.to))));
        }
        // Library views refresh these rows like after a folder relink.
        tracks.slotDatabaseTracksRelocated(relocated);
    }
    QSet<TrackId> changed;
    QSet<TrackId> hidden;
    for (const auto& merged : result.merged) {
        changed.insert(TrackId(QVariant(merged.survivor)));
        hidden.insert(TrackId(QVariant(merged.hidden)));
    }
    if (!hidden.isEmpty()) {
        emit tracks.tracksRemoved(hidden);
    }
    tracks.slotDatabaseTracksChanged(changed);
    QSet<CrateId> crates;
    for (const qint64 crate : result.crateIds) {
        crates.insert(CrateId(QVariant(crate)));
    }
    if (!crates.isEmpty()) {
        emit collection->crateSummaryChanged(crates);
    }
    if (!result.playlistIds.isEmpty()) {
        emit collection->getPlaylistDAO().playlistContentChanged(result.playlistIds);
    }
}

} // namespace decklab
