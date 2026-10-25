// SPDX-License-Identifier: GPL-2.0-or-later
#include "library/decklabfoldertablemodel.h"

#include <QDir>
#include <QHash>
#include <QSet>
#include <QSqlQuery>

#include "decklab/decklabdownloads.h"
#include "library/dao/trackschema.h"
#include "library/queryutil.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "mixer/playerinfo.h"
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
    const QString create = QStringLiteral(
            "CREATE TEMPORARY VIEW %1 AS SELECT library.%2 AS %2, '' AS %3, "
            "library.%4 AS %5, %6 AS %7 FROM library WHERE library.id IN (%8)")
                                   .arg(m_viewName,
                                           LIBRARYTABLE_ID,
                                           LIBRARYTABLE_PREVIEW,
                                           LIBRARYTABLE_COVERART_DIGEST,
                                           LIBRARYTABLE_COVERART,
                                           position,
                                           PLAYLISTTRACKSTABLE_POSITION,
                                           known.isEmpty() ? QStringLiteral("-1")
                                                           : known.join(QLatin1Char(',')));
    if (!query.exec(create)) {
        LOG_FAILED_QUERY(query);
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
