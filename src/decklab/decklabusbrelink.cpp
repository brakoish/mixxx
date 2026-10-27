// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabusbrelink.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QSqlError>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QtConcurrentRun>
#include <QtDebug>
#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

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
    // Play count: both added up (each row counted its own plays); last
    // played: the later; added: the earlier; rating and colour: the
    // survivor's unless it has none; grid and key: from the row the DJ
    // edited, else the one that was analysed.
    LibraryValues values = *keep;
    values.timesPlayed = keep->timesPlayed.toInt() + other->timesPlayed.toInt();
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

// -1 a query failed, 0 left (changed since planning), 1 done: the survivor
// takes the address of holder, which gets the survivor's old one. Both
// track_locations rows stay as they are.
int takeOver(QSqlDatabase& database, const RelinkRow& survivor, const RelinkRow& holder) {
    QSqlQuery point(database);
    point.prepare(QStringLiteral("UPDATE library SET location=:to WHERE id=:id AND location=:from"));
    point.bindValue(QStringLiteral(":to"), holder.locationId);
    point.bindValue(QStringLiteral(":id"), survivor.trackId);
    point.bindValue(QStringLiteral(":from"), survivor.locationId);
    if (!run(point)) {
        return -1;
    }
    if (point.numRowsAffected() != 1) {
        return 0;
    }
    point.bindValue(QStringLiteral(":to"), survivor.locationId);
    point.bindValue(QStringLiteral(":id"), holder.trackId);
    point.bindValue(QStringLiteral(":from"), holder.locationId);
    if (!run(point) || point.numRowsAffected() != 1) {
        return -1; // half done: everything rolls back
    }
    QSqlQuery present(database);
    present.prepare(QStringLiteral("UPDATE track_locations SET fs_deleted=0 WHERE id=:id"));
    present.bindValue(QStringLiteral(":id"), holder.locationId);
    return run(present) ? 1 : -1;
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
        // Not after a name made of digits only, where it would be a guess.
        const auto digit = [](QChar c) { return c.isDigit(); };
        const QStringView rest = QStringView(name).mid(base.size());
        return rest.isEmpty() ||
                (std::all_of(rest.begin(), rest.end(), digit) &&
                        !std::all_of(base.begin(), base.end(), digit));
    };
    return matches(volume.label, Qt::CaseSensitive) || matches(volume.uuid, Qt::CaseInsensitive);
}

bool isNumberedMountName(const UsbVolume& volume, const QString& name) {
    return isEarlierMountName(volume, name) && name != volume.label &&
            name.compare(volume.uuid, Qt::CaseInsensitive) != 0;
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
        const QSet<qint64>& busyTrackIds,
        const QStringList& otherSticks) {
    RelinkPlan plan;
    const QString mount = cleanMount(volume.mount);
    const QString prefix = mount + QLatin1Char('/');
    // File sizes on the stick, each looked up once.
    QHash<QString, qint64> sizes;
    const auto sizeOnStick = [&](const QString& path) {
        const auto known = sizes.constFind(path);
        if (known != sizes.constEnd()) {
            return known.value();
        }
        const qint64 size = probe.fileSize ? probe.fileSize(path) : -1;
        sizes.insert(path, size);
        return size;
    };
    QSet<QString> others;
    for (const QString& name : otherSticks) {
        if (!name.isEmpty()) {
            others.insert(name.toLower());
        }
    }
    // r7 fix: a numbered folder ("DJ2" for a stick labelled DJ) is this
    // stick's only when its rows show it. Another stick of that name (one
    // the unit has seen, or one with the same exports) would otherwise give
    // its rows to this one while it is out.
    const auto numberedFolderIsThisStick = [&](const QString& root, const QString& name) {
        if (others.contains(name.toLower())) {
            return false;
        }
        const QString rootPrefix = root + QLatin1Char('/');
        int listed = 0;
        int onStick = 0;
        int same = 0;
        for (const auto& row : rows) {
            if (row.hidden || !row.location.startsWith(rootPrefix)) {
                continue;
            }
            const QString relative = row.location.mid(rootPrefix.size());
            if (relative.isEmpty() || hasParentStep(relative)) {
                continue;
            }
            ++listed;
            const qint64 size = sizeOnStick(prefix + relative);
            if (size < 0) {
                continue; // not on this stick: no evidence either way
            }
            ++onStick;
            if (row.fileSize == size) {
                ++same;
            }
        }
        // Most of the folder's rows, too: a few songs that both sticks carry
        // say nothing about whose folder it is.
        return onStick > 0 && same * 100 >= onStick * kNumberedFolderEvidencePercent &&
                same * 2 > listed;
    };
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
                const QString name = root.section(QLatin1Char('/'), -1);
                earlier = !(probe.isIn && probe.isIn(root));
                if (earlier && isNumberedMountName(volume, name) && !numberedFolderIsThisStick(root, name)) {
                    earlier = false;
                    plan.otherFolders.append(root);
                }
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
        const qint64 size = sizeOnStick(target);
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
        bool busy = false;
        if (song.current && busyTrackIds.contains(song.current->trackId)) {
            plan.busyIds.insert(song.current->trackId);
            busy = true;
        }
        for (const auto& row : std::as_const(same)) {
            if (busyTrackIds.contains(row.trackId)) {
                plan.busyIds.insert(row.trackId);
                busy = true;
            }
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

namespace {

QList<RelinkRow> readRows(const QSqlDatabase& database, const QString& where) {
    QList<RelinkRow> rows;
    QSqlQuery query(database);
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
            "SELECT library.id, track_locations.id, track_locations.location, "
            "track_locations.filesize, library.mixxx_deleted, track_locations.fs_deleted "
            "FROM track_locations INNER JOIN library ON library.location=track_locations.id ") +
            where);
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

bool isMediaLocation(const QString& location) {
    return location.startsWith(QStringLiteral("/media/")) || location.startsWith(QStringLiteral("/run/media/"));
}

// A plain file name: no folder part, nothing that walks out of the folder.
bool isFileName(const QString& name) {
    return usableName(name) && !name.contains(QLatin1Char('\\'));
}

} // namespace

QList<RelinkRow> relinkCandidates(const QSqlDatabase& database) {
    return readRows(database,
            QStringLiteral("WHERE (track_locations.location >= '/media/' AND "
                           "track_locations.location < '/media0') OR (track_locations.location >= "
                           "'/run/media/' AND track_locations.location < '/run/media0')"));
}

QList<RelinkRow> libraryRows(const QSqlDatabase& database) {
    return readRows(database, QString());
}

std::optional<QList<RenameEntry>> parseRenameMap(const QByteArray& json, int limit) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        return std::nullopt;
    }
    const QJsonArray list = document.array();
    QList<RenameEntry> entries;
    // Appended oldest first; only the newest `limit` are read.
    for (qsizetype index = std::max<qsizetype>(0, list.size() - std::max(0, limit)); index < list.size(); ++index) {
        const QJsonObject item = list.at(index).toObject();
        const QJsonValue oldName = item.value(QStringLiteral("old"));
        const QJsonValue newName = item.value(QStringLiteral("new"));
        const QJsonValue size = item.value(QStringLiteral("size"));
        if (!oldName.isString() || !newName.isString() || !size.isDouble()) {
            continue;
        }
        const double bytes = size.toDouble();
        // A whole, positive number of bytes that a double holds exactly.
        if (!(bytes >= 1 && bytes <= 9007199254740992.0) || std::floor(bytes) != bytes) {
            continue;
        }
        RenameEntry entry{oldName.toString(), newName.toString(), static_cast<qint64>(bytes)};
        if (!isFileName(entry.oldName) || !isFileName(entry.newName) || entry.oldName == entry.newName) {
            continue;
        }
        entries.append(entry);
    }
    return entries;
}

QStringList renameMapFolders(const QList<RelinkRow>& rows, const QStringList& folders) {
    QStringList roots;
    for (const QString& folder : folders) {
        const QString clean = cleanMount(folder);
        if (clean.startsWith(QLatin1Char('/')) && clean != QStringLiteral("/")) {
            roots.append(clean);
        }
    }
    QSet<QString> result(roots.cbegin(), roots.cend());
    for (const auto& row : rows) {
        if (row.hidden) {
            continue;
        }
        const qsizetype slash = row.location.lastIndexOf(QLatin1Char('/'));
        if (slash <= 0) {
            continue;
        }
        const QString folder = row.location.left(slash);
        if (result.contains(folder)) {
            continue;
        }
        for (const QString& root : std::as_const(roots)) {
            if (folder.startsWith(root + QLatin1Char('/'))) {
                result.insert(folder);
                break;
            }
        }
    }
    QStringList sorted(result.cbegin(), result.cend());
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}

RenameMaps readRenameMaps(const QStringList& folders) {
    RenameMaps maps;
    for (const QString& folder : folders) {
        QFile file(QDir(folder).filePath(kRenameMapName));
        const QFileInfo info(file);
        if (!info.isFile()) {
            continue;
        }
        if (info.size() > kRenameMapByteLimit || !file.open(QIODevice::ReadOnly)) {
            qWarning() << "Deck Lab rename map: not read" << info.filePath();
            continue;
        }
        const auto entries = parseRenameMap(file.read(kRenameMapByteLimit + 1));
        if (!entries) {
            qWarning() << "Deck Lab rename map: not a list of renames, ignored" << info.filePath();
            continue;
        }
        if (!entries->isEmpty()) {
            maps.insert(folder, *entries);
        }
    }
    return maps;
}

RelinkPlan planRenames(const QList<RelinkRow>& rows,
        const RenameMaps& maps,
        const RelinkProbe& probe,
        const QSet<qint64>& busyTrackIds) {
    RelinkPlan plan;
    if (maps.isEmpty() || !probe.fileSize) {
        return plan;
    }
    // Every address in use, hidden rows included.
    QHash<QString, const RelinkRow*> byLocation;
    for (const auto& row : rows) {
        byLocation.insert(row.location, &row);
    }
    QStringList folders = maps.keys();
    std::sort(folders.begin(), folders.end());
    QSet<qint64> claimed;
    for (const QString& folder : std::as_const(folders)) {
        const QString prefix = folder + QLatin1Char('/');
        // The rows in this folder by file name, as the renames go.
        QHash<QString, const RelinkRow*> named;
        for (const auto& row : rows) {
            if (!row.hidden && row.location.startsWith(prefix) &&
                    !QStringView(row.location).mid(prefix.size()).contains(QLatin1Char('/'))) {
                named.insert(row.location.mid(prefix.size()), &row);
            }
        }
        QHash<const RelinkRow*, QString> moved; // row -> its file's name now
        for (const auto& entry : maps.value(folder)) {
            const auto it = named.find(entry.oldName);
            if (it == named.end() || it.value()->fileSize != entry.size) {
                continue; // no row there, or another file had that name
            }
            const RelinkRow* row = it.value();
            named.erase(it);
            named.insert(entry.newName, row);
            moved.insert(row, entry.newName);
        }
        QList<const RelinkRow*> order = moved.keys();
        std::sort(order.begin(), order.end(), [](const RelinkRow* a, const RelinkRow* b) {
            return a->trackId < b->trackId;
        });
        for (const RelinkRow* row : std::as_const(order)) {
            const QString target = prefix + moved.value(row);
            if (target == row->location || claimed.contains(row->trackId)) {
                continue; // back where it was (a cycle), or taken by another group
            }
            const qint64 size = probe.fileSize(target);
            if (size < 0) {
                ++plan.missing;
                continue;
            }
            if (size != row->fileSize) {
                ++plan.sizeMismatch;
                continue;
            }
            if (probe.fileSize(row->location) >= 0) {
                continue; // its old file is still there: a copy, or a name used again
            }
            const RelinkRow* holder = byLocation.value(target);
            if (holder && holder->hidden) {
                ++plan.blocked;
                continue;
            }
            if (holder && (claimed.contains(holder->trackId) ||
                                  (holder->fileSize > 0 && holder->fileSize != size))) {
                ++plan.sizeMismatch;
                continue;
            }
            if (busyTrackIds.contains(row->trackId) || (holder && busyTrackIds.contains(holder->trackId))) {
                for (const qint64 id : {row->trackId, holder ? holder->trackId : qint64(-1)}) {
                    if (busyTrackIds.contains(id)) {
                        plan.busyIds.insert(id);
                    }
                }
                ++plan.busy;
                continue;
            }
            RelinkGroup group;
            group.target = target;
            group.survivor = *row;
            if (holder) {
                // The scan saw the new name first and added a row for it.
                // The DJ's row (cues, crates, history, analysis) survives and
                // takes over its address, as Mixxx's own moved-file
                // detection keeps the old row; anything only the new row has
                // moves over.
                group.takeOver = true;
                group.merged = {*holder};
                claimed.insert(holder->trackId);
            } else {
                group.relocate = true;
            }
            claimed.insert(row->trackId);
            plan.groups.append(group);
            ++plan.renamed;
        }
    }
    return plan;
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
        if (group.takeOver && !group.merged.isEmpty()) {
            const RelinkRow& holder = group.merged.first();
            const int moved = takeOver(database, group.survivor, holder);
            if (moved < 0) {
                return RelinkResult();
            }
            if (moved == 0) {
                continue; // changed since planning
            }
            if (mergeInto(database, group.survivor, group.target, holder, &result) <= 0) {
                return RelinkResult(); // the rows changed under the pass: all of it waits
            }
            result.relocated.append({group.survivor.trackId, group.survivor.location, group.target});
            continue;
        }
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

namespace {

const QString kMergeCopyInfix = QStringLiteral(".decklab-merge-");
const QString kFirstMergeCopy = QStringLiteral("first");

// A numbered copy's number ("n000012-<time>"); -1 for an earlier build's
// copy named by its time only.
qint64 mergeCopyNumber(const QString& suffix) {
    if (!suffix.startsWith(QLatin1Char('n'))) {
        return -1;
    }
    bool ok = false;
    const qint64 number = suffix.mid(1, suffix.indexOf(QLatin1Char('-')) - 1).toLongLong(&ok);
    return ok ? number : -1;
}

} // namespace

std::optional<QString> backupLibraryDatabase(const QSqlDatabase& database) {
    // The file SQLite has open (Mixxx opens it through a "file:" URI).
    QSqlQuery list(database);
    if (!list.exec(QStringLiteral("PRAGMA database_list"))) {
        LOG_FAILED_QUERY(list);
        return std::nullopt;
    }
    QString path;
    while (list.next()) {
        if (list.value(1).toString() == QStringLiteral("main")) {
            path = list.value(2).toString();
        }
    }
    if (path.isEmpty()) {
        return QString(); // in memory (tests): nothing to copy beside it
    }
    const QFileInfo file(path);
    const QDir folder = file.dir();
    const QString stem = file.fileName() + kMergeCopyInfix;
    // r7 fix: the first copy is marked by its name, never by a time: the
    // Pi has no clock of its own, so a later copy's time can sort before an
    // earlier one's. The others are numbered; the time is for people.
    const QString first = folder.filePath(stem + kFirstMergeCopy);
    QStringList others = folder.entryList(QStringList{stem + QLatin1Char('*')}, QDir::Files, QDir::Name);
    others.removeAll(stem + kFirstMergeCopy);
    QString copy;
    if (!QFileInfo::exists(first)) {
        if (others.isEmpty()) {
            copy = first;
        } else if (QFile::rename(folder.filePath(others.first()), first)) {
            others.removeFirst(); // an earlier build's first copy, named by its time
        }
    }
    if (copy.isEmpty()) {
        qint64 next = 1;
        for (const QString& name : std::as_const(others)) {
            next = std::max(next, mergeCopyNumber(name.mid(stem.size())) + 1);
        }
        copy = folder.filePath(stem +
                QStringLiteral("n%1-%2")
                        .arg(next, 6, 10, QLatin1Char('0'))
                        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
    }
    QString literal = copy;
    literal.replace(QLatin1Char('\''), QStringLiteral("''"));
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("VACUUM INTO '%1'").arg(literal))) {
        LOG_FAILED_QUERY(query);
        QFile::remove(copy);
        return std::nullopt;
    }
    return copy;
}

void pruneLibraryBackups(const QString& copy, int keep) {
    if (copy.isEmpty()) {
        return;
    }
    const QFileInfo file(copy);
    const QString name = file.fileName();
    const qsizetype stemEnd = name.indexOf(kMergeCopyInfix);
    if (stemEnd < 0) {
        return;
    }
    const QString stem = name.left(stemEnd) + kMergeCopyInfix;
    const QDir folder = file.dir();
    // The first copy (the library before the first merge) is never one of
    // them. The others go oldest first by their number (an earlier build's
    // copies, named by time only, before every numbered one).
    QStringList copies = folder.entryList(QStringList{stem + QLatin1Char('*')}, QDir::Files, QDir::Name);
    copies.removeAll(stem + kFirstMergeCopy);
    std::stable_sort(copies.begin(), copies.end(), [&stem](const QString& left, const QString& right) {
        return mergeCopyNumber(left.mid(stem.size())) < mergeCopyNumber(right.mid(stem.size()));
    });
    while (copies.size() > std::max(1, keep)) {
        QFile::remove(folder.filePath(copies.takeFirst()));
    }
}

RelinkResult applyRelinkWithBackup(QSqlDatabase& database, const RelinkPlan& plan) {
    // r7 fix: a copy first only when rows move or merge. Finding files
    // again and moving library folders are safe without one, and such a
    // pass (a stick back after a scan marked its tracks missing) must not
    // push the copy from before a merge out.
    std::optional<QString> copy = QString();
    if (!plan.groups.isEmpty()) {
        copy = backupLibraryDatabase(database);
    }
    if (!copy) {
        qWarning() << "Deck Lab USB merge: the library could not be copied first; nothing changed";
        return RelinkResult();
    }
    const RelinkResult result = applyRelink(database, plan);
    if (!result.ok) {
        qWarning() << "Deck Lab USB merge: the changes were rolled back";
    }
    if (result.ok && !(result.relocated.isEmpty() && result.merged.isEmpty())) {
        pruneLibraryBackups(*copy);
    } else if (!copy->isEmpty()) {
        QFile::remove(*copy); // nothing moved or merged: the copy says nothing new
    }
    return result;
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
    if (m_library) {
        // Mixxx's library scan writes in one long transaction: wait for it.
        connect(m_library.get(), &TrackCollectionManager::libraryScanStarted, this, [this] {
            m_scanning = true;
        });
        connect(m_library.get(), &TrackCollectionManager::libraryScanFinished, this, [this] {
            m_scanning = false;
            refreshMedia(); // files found missing or found again
            // Deck Lab scans when a utility closes: Get Music may have
            // renamed downloads.
            m_renamesPending = true;
            startNext();
        });
        // r7: the start-up scan began before this existed (its scanStarted
        // never comes here); its scanFinished does.
        m_scanning = m_library->isLibraryScanRunning();
    }
    m_retry.setSingleShot(true);
    // r7 fix: shorter than the quiet spell, so a pass starts soon after it.
    m_retry.setInterval(2000);
    connect(&m_retry, &QTimer::timeout, this, &UsbRelinker::startNext);
    m_busyRetry.setSingleShot(true);
    m_busyRetry.setInterval(5000);
    connect(&m_busyRetry, &QTimer::timeout, this, &UsbRelinker::retryBusy);
    // r7: counts a stick change left for later are rebuilt once it is quiet.
    connect(&MediaPresence::instance(), &MediaPresence::settled, this, [this] {
        if (std::exchange(m_countsPending, false)) {
            emitCounts();
        }
    });
    m_mediaRefresh.setSingleShot(true);
    connect(&m_mediaRefresh, &QTimer::timeout, this, [this] { refreshMedia(); });
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
    refreshMediaSoon();
    const QList<UsbVolume> volumes = usbVolumesFromDrives(drives);
    // A stick that went (or moved) is looked at again when it is back.
    QSet<QString> present;
    for (const auto& volume : volumes) {
        present.insert(volumeKey(volume));
    }
    m_done.intersect(present);
    // A stick that came: its pass follows the rename maps too.
    m_renamesPending = queue(volumes) || m_renamesPending;
    startNext();
}

void UsbRelinker::relinkMounted() {
    refreshMediaSoon();
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
    // Start-up, or a media folder changed.
    m_renamesPending = true;
    startNext();
}

void UsbRelinker::relinkRenames() {
    m_renamesPending = true;
    startNext();
}

void UsbRelinker::refreshMediaSoon() {
    constexpr qint64 kBurstMs = 1500;
    if (m_mediaRefresh.isActive()) {
        return; // the second one is due anyway
    }
    if (!m_mediaRefreshed.isValid() || m_mediaRefreshed.elapsed() >= kBurstMs) {
        refreshMedia();
        return;
    }
    m_mediaRefresh.start(static_cast<int>(kBurstMs - m_mediaRefreshed.elapsed()));
}

void UsbRelinker::refreshMedia(bool force) {
    TrackCollection* collection = m_library ? m_library->internalCollection() : nullptr;
    if (!collection) {
        return;
    }
    m_mediaRefresh.stop();
    m_mediaRefreshed.start();
    if (!MediaPresence::instance().refresh(collection->database(), force)) {
        return;
    }
    // r7: rebuilding every crate's and playlist's count reads the database
    // on the GUI thread; mid-set it waits until it is quiet.
    if (MediaPresence::instance().deferring()) {
        m_countsPending = true;
        return;
    }
    emitCounts();
}

void UsbRelinker::emitCounts() {
    TrackCollection* collection = m_library ? m_library->internalCollection() : nullptr;
    if (!collection) {
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

namespace {
const ConfigKey kSticksSeen(QStringLiteral("[DeckLab]"), QStringLiteral("UsbSticksSeen"));
} // namespace

QHash<QString, QStringList> UsbRelinker::sticksSeen() const {
    QHash<QString, QStringList> seen;
    if (!m_config) {
        return seen;
    }
    // {"<uuid>": ["<label>", ...]}
    const QJsonObject object =
            QJsonDocument::fromJson(m_config->getValueString(kSticksSeen).toUtf8()).object();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        QStringList labels;
        for (const auto& label : it.value().toArray()) {
            if (label.isString() && !label.toString().isEmpty()) {
                labels.append(label.toString());
            }
        }
        seen.insert(it.key().toLower(), labels);
    }
    return seen;
}

void UsbRelinker::rememberSticks(const QList<UsbVolume>& volumes) {
    if (!m_config) {
        return;
    }
    QHash<QString, QStringList> seen = sticksSeen();
    bool changed = false;
    for (const auto& volume : volumes) {
        const QString uuid = volume.uuid.toLower();
        if (uuid.isEmpty()) {
            continue;
        }
        if (!seen.contains(uuid)) {
            seen.insert(uuid, {});
            changed = true;
        }
        QStringList& labels = seen[uuid];
        if (!volume.label.isEmpty() && !labels.contains(volume.label)) {
            labels.append(volume.label);
            changed = true;
        }
    }
    if (!changed) {
        return;
    }
    QJsonObject object;
    for (auto it = seen.constBegin(); it != seen.constEnd(); ++it) {
        object.insert(it.key(), QJsonArray::fromStringList(it.value()));
    }
    m_config->setValue(kSticksSeen, QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
    m_config->save();
}

QStringList UsbRelinker::otherSticks(const UsbVolume& volume) const {
    QStringList names;
    const QHash<QString, QStringList> seen = sticksSeen();
    for (auto it = seen.constBegin(); it != seen.constEnd(); ++it) {
        if (it.key() == volume.uuid.toLower()) {
            continue;
        }
        names.append(it.key());
        names.append(it.value());
    }
    return names;
}

bool UsbRelinker::queue(const QList<UsbVolume>& volumes) {
    // r7 fix: every stick that comes in is remembered (UUID and label), so a
    // numbered folder of another stick's name is never taken for it later.
    rememberSticks(volumes);
    bool added = false;
    for (const auto& volume : volumes) {
        const QString key = volumeKey(volume);
        if (m_done.contains(key) || (m_watcher.isRunning() && m_running && volumeKey(*m_running) == key)) {
            continue;
        }
        const bool queued = std::any_of(m_pending.cbegin(), m_pending.cend(), [&key](const UsbVolume& pending) {
            return volumeKey(pending) == key;
        });
        if (!queued) {
            m_pending.append(volume);
            added = true;
        }
    }
    return added;
}

bool UsbRelinker::scanning() const {
    return m_scanning || (m_library && m_library->isLibraryScanRunning());
}

void UsbRelinker::startNext() {
    if (m_watcher.isRunning() || (m_pending.isEmpty() && !m_renamesPending) || !m_library || scanning()) {
        return;
    }
    // Only while it is quiet: tried again when it is.
    if (m_quiet && !m_quiet()) {
        m_retry.start();
        return;
    }
    TrackCollection* collection = m_library->internalCollection();
    if (!collection) {
        return;
    }
    const QSqlDatabase database = collection->database();
    const QList<RelinkRow> rows = libraryRows(database);
    const QStringList folders = libraryFolders(database);
    const QSet<qint64> busy = m_busy ? m_busy() : QSet<qint64>();
    m_running = m_pending.isEmpty() ? std::nullopt : std::optional<UsbVolume>(m_pending.takeFirst());
    // Every pass follows the rename maps too: one copy, one transaction.
    m_runningRenames = m_renamesPending;
    m_renamesPending = false;
    const std::optional<UsbVolume> volume = m_running;
    const bool renames = m_runningRenames;
    const RelinkProbe probe = m_probe;
    const QStringList others = volume ? otherSticks(*volume) : QStringList();
    // Only stat() calls, directory and rename map reads on the worker: the
    // stick is read, never written.
    m_watcher.setFuture(QtConcurrent::run([volume, renames, rows, folders, probe, busy, others] {
        RelinkPlan plan;
        if (volume) {
            QList<RelinkRow> media;
            for (const auto& row : rows) {
                if (isMediaLocation(row.location)) {
                    media.append(row);
                }
            }
            plan = planRelink(*volume, media, folders, probe, busy, others);
        }
        if (renames) {
            // After the stick's own moves: a renamed file's row follows it.
            const RelinkPlan renamed = planRenames(
                    rows, readRenameMaps(renameMapFolders(rows, folders)), probe, busy);
            plan.groups += renamed.groups;
            plan.renamed = renamed.renamed;
            plan.sizeMismatch += renamed.sizeMismatch;
            plan.missing += renamed.missing;
            plan.blocked += renamed.blocked;
            plan.busy += renamed.busy;
            plan.busyIds += renamed.busyIds;
        }
        return plan;
    }));
}

void UsbRelinker::finish() {
    const std::optional<UsbVolume> volume = m_running;
    RelinkPlan plan = m_watcher.result();
    if (scanning() || (m_quiet && !m_quiet())) {
        // A deck started (or a library scan, a recording, a utility) while
        // the stick was read: later, planned afresh.
        if (volume) {
            m_pending.prepend(*volume);
        }
        m_renamesPending = m_renamesPending || m_runningRenames;
        if (!scanning()) {
            m_retry.start();
        }
        return;
    }
    // A track may have been loaded while the worker checked files: its song waits.
    const QSet<qint64> busy = m_busy ? m_busy() : QSet<qint64>();
    QList<RelinkGroup> groups;
    for (const auto& group : std::as_const(plan.groups)) {
        bool inUse = false;
        if (busy.contains(group.survivor.trackId)) {
            plan.busyIds.insert(group.survivor.trackId);
            inUse = true;
        }
        for (const auto& row : group.merged) {
            if (busy.contains(row.trackId)) {
                plan.busyIds.insert(row.trackId);
                inUse = true;
            }
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
        result = applyRelinkWithBackup(database, plan);
        if (result.ok) {
            notify(result);
        }
    }
    const int relocated = static_cast<int>(result.relocated.size());
    const int merged = static_cast<int>(result.merged.size());
    // Every stick pass is logged; a pass of rename maps only when it found work.
    if (volume || !plan.isEmpty() || plan.sizeMismatch + plan.missing + plan.blocked + plan.busy > 0) {
        qInfo() << "Deck Lab USB merge" << (volume ? volume->uuid : QStringLiteral("(rename maps)"))
                << "->" << (volume ? volume->mount : QString()) << ":" << relocated << "moved,"
                << merged << "merged (" << result.cues << "cues," << result.crates << "crate entries,"
                << result.entries << "history and playlist entries )," << plan.renamed
                << "following a rename map," << result.revived << "found again," << result.folders
                << "library folders moved," << plan.sizeMismatch << "same path but another size,"
                << plan.missing << "file not there," << plan.blocked
                << "address held by a hidden track," << plan.busy << "in use";
    }
    if (!plan.otherFolders.isEmpty()) {
        qInfo() << "Deck Lab USB merge: left alone, not shown to be this stick:" << plan.otherFolders;
    }
    if (volume && plan.busy == 0 && result.ok) {
        m_done.insert(volumeKey(*volume));
    }
    if (plan.busy > 0) {
        // r7: the songs left because a track was in use (the analysis or a
        // deck held it) are tried again once none of those tracks is, a
        // rename-only pass too.
        m_busyIds.unite(plan.busyIds);
        m_busyRenames = m_busyRenames || m_runningRenames;
        if (volume && std::none_of(m_busyVolumes.cbegin(), m_busyVolumes.cend(), [&volume](const UsbVolume& known) {
                return volumeKey(known) == volumeKey(*volume);
            })) {
            m_busyVolumes.append(*volume);
        }
        m_busyRetry.start();
    }
    // The views select again: rows moved, merged or found again.
    refreshMedia(result.ok && !plan.isEmpty());
    emit relinked(relocated, merged);
    startNext();
}

void UsbRelinker::retryBusy() {
    if (m_busyIds.isEmpty() && m_busyVolumes.isEmpty() && !m_busyRenames) {
        return;
    }
    const QSet<qint64> held = m_busy ? m_busy() : QSet<qint64>();
    if (held.intersects(m_busyIds)) {
        m_busyRetry.start(); // still in use: a cheap look again later
        return;
    }
    m_busyIds.clear();
    m_renamesPending = std::exchange(m_busyRenames, false) || m_renamesPending;
    queue(std::exchange(m_busyVolumes, {}));
    startNext(); // and only while it is quiet
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
