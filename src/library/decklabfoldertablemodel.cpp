// SPDX-License-Identifier: GPL-2.0-or-later
#include "library/decklabfoldertablemodel.h"

#include <QDir>
#include <QHash>
#include <QSet>
#include <QSqlQuery>
#include <algorithm>

#include "decklab/decklabdownloads.h"
#include "decklab/decklabmedia.h"
#include "library/dao/trackschema.h"
#include "library/queryutil.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "mixer/playerinfo.h"
#include "util/datetime.h" // r7-recent
#include "widget/decklabbrowse.h" // r7-recent: kRecentlyAddedDays
#include "moc_decklabfoldertablemodel.cpp"

namespace decklab {

QString songListSqlFilter() {
    const QString samples = samplesFolder();
    if (samples.isEmpty()) {
        return {};
    }
    // A plain prefix comparison: LIKE would treat '_' and '%' in a home
    // folder's name as wildcards and ignores case.
    QString prefix = QDir::cleanPath(samples) + QLatin1Char('/');
    prefix.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QStringLiteral(" AND substr(track_locations.location, 1, %1) <> '%2'")
            .arg(prefix.size())
            .arg(prefix);
}

int shownSongCount(const QSqlDatabase& database) {
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM library INNER JOIN track_locations "
                                   "ON library.location=track_locations.id "
                                   "WHERE mixxx_deleted=0 AND fs_deleted=0 AND ") +
                mediaShownCondition(database, QStringLiteral("library.location")) +
                songListSqlFilter()) ||
            !query.next()) {
        return -1;
    }
    return query.value(0).toInt();
}

QString recentRootOf(const QString& location, const QStringList& directories) {
    const QString media = mediaRootOf(location);
    if (!media.isEmpty()) {
        return media;
    }
    QString root;
    for (const QString& directory : directories) {
        const QString folder = QDir::cleanPath(directory);
        if (folder.size() > root.size() && location.startsWith(folder + QLatin1Char('/'))) {
            root = folder;
        }
    }
    return root;
}

QList<RecentTrack> recentlyAdded(const QList<AddedRow>& rows, const QStringList& directories, const QDate& today) {
    // Each root's first day, from every row it ever had.
    QHash<QString, QDateTime> first;
    QList<QString> roots;
    roots.reserve(rows.size());
    for (const AddedRow& row : rows) {
        roots.append(recentRootOf(row.location, directories));
        if (roots.last().isEmpty() || !row.added.isValid()) {
            continue;
        }
        auto found = first.find(roots.last());
        if (found == first.end()) {
            first.insert(roots.last(), row.added);
        } else if (row.added < found.value()) {
            found.value() = row.added;
        }
    }
    const QDate since = recentlyAddedSince(today);
    QList<RecentTrack> recent;
    for (int index = 0; index < rows.size(); ++index) {
        const AddedRow& row = rows.at(index);
        if (!row.listed || !row.added.isValid() || row.added.date() < since || isSampleLocation(row.location)) {
            continue;
        }
        const auto found = first.constFind(roots.at(index));
        if (found != first.cend() && found.value().date() == row.added.date() &&
                !isDownloadLocation(row.location)) {
            continue; // the starting collection
        }
        recent.append({row.location, row.added});
    }
    std::stable_sort(recent.begin(), recent.end(), [](const RecentTrack& a, const RecentTrack& b) {
        return a.added > b.added;
    });
    return recent;
}

QList<RecentTrack> readRecentlyAdded(const QSqlDatabase& database, const QDate& today) {
    QStringList directories;
    QSqlQuery folders(database);
    if (folders.exec(QStringLiteral("SELECT directory FROM directories"))) {
        while (folders.next()) {
            directories << folders.value(0).toString();
        }
    } else {
        LOG_FAILED_QUERY(folders);
    }
    QList<AddedRow> rows;
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("SELECT track_locations.location, library.datetime_added, "
                                   "library.mixxx_deleted = 0 AND track_locations.fs_deleted = 0 "
                                   "FROM library INNER JOIN track_locations "
                                   "ON library.location = track_locations.id"))) {
        LOG_FAILED_QUERY(query);
        return {};
    }
    while (query.next()) {
        const QDateTime stored = query.value(1).toDateTime();
        rows.append({query.value(0).toString(),
                stored.isValid() ? mixxx::localDateTimeFromUtc(stored) : QDateTime(),
                query.value(2).toBool()});
    }
    return recentlyAdded(rows, directories, today);
}

} // namespace decklab

namespace {
// Each model keeps its own temporary view on the library connection.
int s_views = 0;
// SQLite's host parameter limit is 999 on older builds.
constexpr int kLookupChunk = 400;
} // namespace

DeckLabFolderTableModel::DeckLabFolderTableModel(
        QObject* parent, TrackCollectionManager* pTrackCollectionManager)
        : BaseSqlTableModel(parent, pTrackCollectionManager, "mixxx.db.model.decklab_folder"),
          m_viewName(QStringLiteral("decklab_folder_%1").arg(++s_views)) {
}

DeckLabFolderTableModel::~DeckLabFolderTableModel() = default;

int DeckLabFolderTableModel::setLocations(const QStringList& locations, const QString& key) {
    m_locations = locations;
    m_key = key;
    if (!createView()) {
        return 0;
    }
    setTable(m_viewName,
            LIBRARYTABLE_ID,
            QStringList{LIBRARYTABLE_ID,
                    LIBRARYTABLE_PREVIEW,
                    LIBRARYTABLE_COVERART,
                    PLAYLISTTRACKSTABLE_POSITION},
            m_pTrackCollectionManager->internalCollection()->getTrackSource());
    setSearch(QString());
    setSort(fieldIndex(ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_POSITION), Qt::AscendingOrder);
    select();
    return rowCount();
}

void DeckLabFolderTableModel::refreshForMedia() {
    // r7: a merge or rename pass may have moved a file's address to another
    // row (and hidden the one shown): find each file's row again.
    if (initialized() && createView()) {
        select();
    }
}

bool DeckLabFolderTableModel::createView() {
    const QStringList& locations = m_locations;
    // The library's row for each file (read only: a file the library does
    // not know yet waits for the next library scan).
    QHash<QString, qint64> ids;
    for (int from = 0; from < locations.size(); from += kLookupChunk) {
        const QStringList chunk = locations.mid(from, kLookupChunk);
        QStringList marks;
        for (int n = 0; n < chunk.size(); ++n) {
            marks << QStringLiteral("?");
        }
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("SELECT library.id, track_locations.location FROM library "
                                     "INNER JOIN track_locations ON library.location = track_locations.id "
                                     "WHERE library.mixxx_deleted = 0 AND track_locations.location IN (%1)")
                        .arg(marks.join(QLatin1Char(','))));
        for (const QString& location : chunk) {
            query.addBindValue(location);
        }
        if (!query.exec()) {
            LOG_FAILED_QUERY(query);
            continue;
        }
        while (query.next()) {
            ids.insert(query.value(1).toString(), query.value(0).toLongLong());
        }
    }
    // Position n is the n-th file as given (a playlist folder's number).
    QStringList cases;
    QStringList known;
    QSet<qint64> seen;
    for (int index = 0; index < locations.size(); ++index) {
        const auto found = ids.constFind(locations.at(index));
        if (found == ids.cend() || seen.contains(found.value())) {
            continue;
        }
        seen.insert(found.value());
        cases << QStringLiteral("WHEN %1 THEN %2").arg(found.value()).arg(index + 1);
        known << QString::number(found.value());
    }
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("DROP VIEW IF EXISTS %1").arg(m_viewName))) {
        LOG_FAILED_QUERY(query);
    }
    const QString position = cases.isEmpty()
            ? QStringLiteral("0")
            : QStringLiteral("CASE library.id %1 END").arg(cases.join(QLatin1Char(' ')));
    // A row hidden later (mixxx_deleted: merged into another) leaves the
    // list at the next select, even before the list is built again; so
    // does a row found missing or on a stick that went (r7 fix).
    const QString create = QStringLiteral(
            "CREATE TEMPORARY VIEW %1 AS SELECT library.%2 AS %2, '' AS %3, "
            "library.%4 AS %5, %6 AS %7 FROM library WHERE library.id IN (%8) "
            "AND library.mixxx_deleted = 0 AND EXISTS (SELECT 1 FROM track_locations "
            "WHERE track_locations.id = library.location AND track_locations.fs_deleted = 0) AND %9")
                                   .arg(m_viewName,
                                           LIBRARYTABLE_ID,
                                           LIBRARYTABLE_PREVIEW,
                                           LIBRARYTABLE_COVERART_DIGEST,
                                           LIBRARYTABLE_COVERART,
                                           position,
                                           PLAYLISTTRACKSTABLE_POSITION,
                                           known.isEmpty() ? QStringLiteral("-1")
                                                           : known.join(QLatin1Char(',')),
                                           decklab::mediaShownCondition(m_database,
                                                   QStringLiteral("library.location")));
    if (!query.exec(create)) {
        LOG_FAILED_QUERY(query);
        return false;
    }
    return true;
}

void DeckLabFolderTableModel::initSortColumnMapping() {
    BaseSqlTableModel::initSortColumnMapping();
    // The position column is the list's own order (the "#" column).
    const int position = fieldIndex(ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_POSITION);
    m_columnIndexBySortColumnId[static_cast<int>(TrackModel::SortColumnId::Position)] = position;
    if (position >= 0) {
        m_sortColumnIdByColumnIndex.insert(position, TrackModel::SortColumnId::Position);
    }
}

bool DeckLabFolderTableModel::isColumnInternal(int column) {
    // As the Library's own track list.
    return column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_ID) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_URL) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_CUEPOINT) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_SAMPLERATE) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_MIXXXDELETED) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_HEADERPARSED) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_PLAYED) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_KEY_ID) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BPM_LOCK) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BEATS_VERSION) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_CHANNELS) ||
            column == fieldIndex(ColumnCache::COLUMN_TRACKLOCATIONSTABLE_DIRECTORY) ||
            column == fieldIndex(ColumnCache::COLUMN_TRACKLOCATIONSTABLE_FSDELETED) ||
            (PlayerInfo::instance().numPreviewDecks() == 0 &&
                    column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_PREVIEW)) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART_SOURCE) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART_TYPE) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART_LOCATION) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART_COLOR) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART_DIGEST) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART_HASH);
}

TrackModel::Capabilities DeckLabFolderTableModel::getCapabilities() const {
    // No Sorting: the list keeps the order it was given (newest first, or
    // playlist order). Nothing here removes, hides or reorders files.
    return Capability::AddToTrackSet |
            Capability::AddToAutoDJ |
            Capability::EditMetadata |
            Capability::LoadToDeck |
            Capability::LoadToSampler |
            Capability::LoadToPreviewDeck |
            Capability::ResetPlayed |
            Capability::Analyze |
            Capability::Properties;
}

QString DeckLabFolderTableModel::modelKey(bool noSearch) const {
    const QString key = QStringLiteral("decklab_folder:") + m_key;
    return noSearch ? key : key + QStringLiteral("#") + currentSearch();
}
