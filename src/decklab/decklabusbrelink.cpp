// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabusbrelink.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QtConcurrentRun>
#include <QtDebug>

#include "library/dao/trackdao.h"
#include "library/queryutil.h"
#include "library/relocatedtrack.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "moc_decklabusbrelink.cpp"
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

QStringList legacyRelativePaths(const UsbVolume& volume, const QString& location) {
    QStringList names;
    for (const QString& name : {volume.label, volume.uuid}) {
        if (usableName(name) && !names.contains(name)) {
            names.append(name);
        }
    }
    if (names.isEmpty() || !location.startsWith(QLatin1Char('/'))) {
        return {};
    }
    const QString mount = cleanMount(volume.mount);
    const QStringList parts = location.split(QLatin1Char('/'));
    // parts[0] is empty (leading slash). A reading is "root segments, then
    // the relative path"; the relative path keeps at least one segment.
    QStringList result;
    const auto consider = [&](int rootSegments) {
        if (parts.size() <= rootSegments + 1) {
            return;
        }
        const QString root = parts.mid(0, rootSegments + 1).join(QLatin1Char('/'));
        const QString relative = parts.mid(rootSegments + 1).join(QLatin1Char('/'));
        // Never the current mount, nor a folder that contains it (for
        // example /media/decklab for a label "decklab"): rows there belong
        // to the current mount or to another stick.
        if (root == mount || mount.startsWith(QString(root + QLatin1Char('/'))) ||
                relative.isEmpty() ||
                relative.split(QLatin1Char('/')).contains(QStringLiteral(".."))) {
            return;
        }
        if (!result.contains(relative)) {
            result.append(relative);
        }
    };
    const auto isName = [&names, &parts](int index) {
        return index < parts.size() && names.contains(parts[index]);
    };
    const auto isSegment = [&parts](int index) {
        return index < parts.size() && usableName(parts[index]);
    };
    // /media/<user>/<name>/... (udisks2 on Debian/Ubuntu/Raspberry Pi OS,
    // and /media/decklab/<uuid> when the appliance mount root moved)
    if (parts.value(1) == QStringLiteral("media") && isSegment(2) && isName(3)) {
        consider(3);
    }
    // /run/media/<user>/<name>/... (udisks2 on Fedora/Arch)
    if (parts.value(1) == QStringLiteral("run") && parts.value(2) == QStringLiteral("media") &&
            isSegment(3) && isName(4)) {
        consider(4);
    }
    // /media/<name>/... (usbmount and hand-made mounts)
    if (parts.value(1) == QStringLiteral("media") && isName(2)) {
        consider(2);
    }
    return result;
}

qint64 regularFileSize(const QString& path) {
    const QFileInfo info(path);
    return info.isFile() ? info.size() : -1;
}

RelinkPlan planRelink(const UsbVolume& volume,
        const QList<RelinkRow>& rows,
        const FileSizeFunction& fileSize,
        const QSet<qint64>& busyTrackIds) {
    RelinkPlan plan;
    const QString mount = cleanMount(volume.mount);
    QSet<QString> claimed;
    for (const auto& row : rows) {
        const QStringList relatives = legacyRelativePaths(volume, row.location);
        if (relatives.isEmpty()) {
            continue; // not a path this volume ever had
        }
        if (busyTrackIds.contains(row.trackId)) {
            ++plan.busy;
            continue;
        }
        // The old path still answers: another stick sits there, or the same
        // one is mounted twice. Leave the row exactly as it is.
        if (fileSize(row.location) >= 0) {
            ++plan.skipped;
            continue;
        }
        QString target;
        for (const QString& relative : relatives) {
            const QString candidate = mount + QLatin1Char('/') + relative;
            const qint64 size = fileSize(candidate);
            if (size < 0) {
                continue;
            }
            // Same relative path but a different file (another stick with
            // the same label, or the file was replaced): not the same track.
            if (row.fileSize > 0 && size != row.fileSize) {
                continue;
            }
            target = candidate;
            break;
        }
        if (target.isEmpty() || claimed.contains(target)) {
            ++plan.skipped;
            continue;
        }
        claimed.insert(target);
        plan.moves.append({row, target});
    }
    return plan;
}

QList<RelinkRow> relinkCandidates(const QSqlDatabase& database) {
    QList<RelinkRow> rows;
    QSqlQuery query(database);
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
            "SELECT library.id, track_locations.id, track_locations.location, "
            "track_locations.filesize FROM track_locations "
            "INNER JOIN library ON library.location=track_locations.id "
            "WHERE track_locations.location LIKE '/media/%' "
            "OR track_locations.location LIKE '/run/media/%'"));
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
        rows.append(row);
    }
    return rows;
}

RelinkResult applyRelink(QSqlDatabase& database, const QList<RelinkMove>& moves) {
    RelinkResult result;
    if (moves.isEmpty()) {
        return result;
    }
    ScopedTransaction transaction(database);
    QSqlQuery existing(database);
    existing.prepare(QStringLiteral("SELECT id FROM track_locations WHERE location=:location"));
    QSqlQuery update(database);
    update.prepare(QStringLiteral(
            "UPDATE track_locations SET location=:new, directory=:directory, "
            "filename=:filename, fs_deleted=0 WHERE id=:id AND location=:old"));
    for (const auto& move : moves) {
        existing.bindValue(QStringLiteral(":location"), move.newLocation);
        if (!existing.exec()) {
            LOG_FAILED_QUERY(existing);
            continue;
        }
        if (existing.next()) {
            // The stick's file is already a library row of its own (loaded
            // at the new path before). Keep both; never merge or delete here.
            result.conflicts.append(move.newLocation);
            continue;
        }
        const QFileInfo target(move.newLocation);
        update.bindValue(QStringLiteral(":new"), move.newLocation);
        update.bindValue(QStringLiteral(":directory"), target.path());
        update.bindValue(QStringLiteral(":filename"), target.fileName());
        update.bindValue(QStringLiteral(":id"), move.row.locationId);
        update.bindValue(QStringLiteral(":old"), move.row.location);
        if (!update.exec()) {
            LOG_FAILED_QUERY(update);
            continue;
        }
        if (update.numRowsAffected() == 1) {
            result.moved.append(move);
        }
    }
    if (!transaction.commit()) {
        result.moved.clear();
    }
    return result;
}

UsbRelinker::UsbRelinker(std::shared_ptr<TrackCollectionManager> library, QObject* parent)
        : QObject(parent),
          m_library(std::move(library)) {
    m_busy = [] {
        QSet<qint64> ids;
        const auto cached = GlobalTrackCacheLocker().getCachedTrackIds();
        for (const auto& id : cached) {
            ids.insert(id.toVariant().toLongLong());
        }
        return ids;
    };
    connect(&m_watcher, &QFutureWatcher<RelinkPlan>::finished, this, &UsbRelinker::finish);
}

UsbRelinker::~UsbRelinker() {
    m_watcher.waitForFinished();
}

void UsbRelinker::relinkDrives(const QJsonArray& drives) {
    for (const auto& volume : usbVolumesFromDrives(drives)) {
        const QString key = volumeKey(volume);
        if (m_done.contains(key) || (m_watcher.isRunning() && volumeKey(m_running) == key)) {
            continue;
        }
        bool queued = false;
        for (const auto& pending : std::as_const(m_pending)) {
            queued |= volumeKey(pending) == key;
        }
        if (!queued) {
            m_pending.append(volume);
        }
    }
    startNext();
}

void UsbRelinker::startNext() {
    while (!m_watcher.isRunning() && !m_pending.isEmpty() && m_library) {
        const UsbVolume volume = m_pending.takeFirst();
        TrackCollection* collection = m_library->internalCollection();
        if (!collection) {
            return;
        }
        QList<RelinkRow> rows;
        for (const auto& row : relinkCandidates(collection->database())) {
            if (!legacyRelativePaths(volume, row.location).isEmpty()) {
                rows.append(row);
            }
        }
        if (rows.isEmpty()) {
            m_done.insert(volumeKey(volume));
            continue;
        }
        const QSet<qint64> busy = m_busy ? m_busy() : QSet<qint64>();
        m_running = volume;
        // Only stat() calls on the worker: the stick is read, never written.
        m_watcher.setFuture(QtConcurrent::run([volume, rows, busy] {
            return planRelink(volume, rows, regularFileSize, busy);
        }));
    }
}

void UsbRelinker::finish() {
    const UsbVolume volume = m_running;
    RelinkPlan plan = m_watcher.result();
    // A track may have been loaded while the worker checked files: leave it.
    const QSet<qint64> busy = m_busy ? m_busy() : QSet<qint64>();
    QList<RelinkMove> moves;
    for (const auto& move : std::as_const(plan.moves)) {
        if (busy.contains(move.row.trackId)) {
            ++plan.busy;
        } else {
            moves.append(move);
        }
    }
    int moved = 0;
    int conflicts = 0;
    TrackCollection* collection = m_library ? m_library->internalCollection() : nullptr;
    if (collection) {
        QSqlDatabase database = collection->database();
        const RelinkResult result = applyRelink(database, moves);
        moved = static_cast<int>(result.moved.size());
        conflicts = static_cast<int>(result.conflicts.size());
        if (!result.moved.isEmpty()) {
            QList<RelocatedTrack> relocated;
            relocated.reserve(result.moved.size());
            for (const auto& move : result.moved) {
                relocated.append(RelocatedTrack(
                        TrackRef::fromFileInfo(mixxx::FileInfo(move.row.location),
                                TrackId(QVariant(move.row.trackId))),
                        TrackRef::fromFileInfo(mixxx::FileInfo(move.newLocation))));
            }
            // Library views refresh these rows like after a folder relink.
            collection->getTrackDAO().slotDatabaseTracksRelocated(relocated);
        }
        qInfo() << "Deck Lab USB relink" << volume.uuid << "->" << volume.mount << ":" << moved
                << "moved," << conflicts << "kept as separate rows," << plan.skipped
                << "left alone," << plan.busy << "in use";
    }
    if (plan.busy == 0) {
        m_done.insert(volumeKey(volume));
    }
    emit relinked(moved, conflicts);
    startNext();
}

} // namespace decklab
