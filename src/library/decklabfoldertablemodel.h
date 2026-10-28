// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDate>
#include <QDateTime>
#include <QList>
#include <QSqlDatabase>
#include <QStringList>

#include "library/basesqltablemodel.h"

namespace decklab {

/// r7-browse: the song lists' filter term. The Library's track list (Tracks,
/// All tracks, song search) leaves samples out: tracks inside
/// ~/Music/Deck Lab Samples. They stay in the library, so the SAMPLES list
/// and the pads still load them. An SQL condition, " AND ..." (empty when
/// there is no music folder), for the library view's WHERE clause.
QString songListSqlFilter();

/// r7 fix: how many tracks the Library's track list shows (the home's
/// Library tile): not hidden, not missing, not on a USB stick that is out,
/// and no samples. -1 when the database can't say.
int shownSongCount(const QSqlDatabase& database);

/// r7-recent: Recently Added, one rule for the Library and a stick: the
/// tracks new to this unit in the last 30 days (since local midnight
/// kRecentlyAddedDays days ago), newest first. A root's starting collection
/// does not count: the tracks added on the local day of the earliest
/// datetime_added under the same root (a stick's first read, a library
/// folder's first scan) are left out, except Get Music's downloads
/// (isDownloadLocation), which always count. A root is a stick's media root
/// (mediaRootOf), else the longest library folder (Mixxx's directories)
/// holding the file; a file under neither has no first day. Samples stay
/// out, as from every song list.
struct AddedRow {
    QString location;
    QDateTime added; // local time
    bool listed = true; // not hidden, not missing (the baseline counts every row)
};
struct RecentTrack {
    QString location;
    QDateTime added; // local time
};
QString recentRootOf(const QString& location, const QStringList& directories);
QList<RecentTrack> recentlyAdded(const QList<AddedRow>& rows, const QStringList& directories, const QDate& today);
/// The library's rows and folders read from database, the rule applied. Any
/// thread with its own connection (Deck Lab reads it off the GUI thread).
QList<RecentTrack> readRecentlyAdded(const QSqlDatabase& database, const QDate& today);

} // namespace decklab

/// r7-browse: library tracks in an order Deck Lab gives: the DOWNLOADS lists
/// (newest first, or a playlist folder in playlist order) and SAMPLES. The
/// rows are Mixxx's own library rows, so BPM, key, covers and analysis come
/// from the library, and every load path (decks, pads, preview) works as in
/// any track table. Files the library does not know yet are left out. The
/// order is fixed (no header sorting); the position column numbers the files
/// as given. r7: hidden rows (mixxx_deleted) never show, and after a USB
/// merge or rename pass each file's row is found again by its location.
/// r7 fix: like every other list, rows marked missing (fs_deleted) and rows
/// on a USB stick that is out (decklab/decklabmedia.h) never show either:
/// an eject or a pull takes them off the list at once.
class DeckLabFolderTableModel final : public BaseSqlTableModel {
    Q_OBJECT
  public:
    DeckLabFolderTableModel(QObject* parent, TrackCollectionManager* pTrackCollectionManager);
    ~DeckLabFolderTableModel() override;

    /// Shows the library's tracks at these locations in this order. key names
    /// the list (where the table keeps its place). Returns the rows shown.
    int setLocations(const QStringList& locations, const QString& key);
    QStringList locations() const {
        return m_locations;
    }
    QString listKey() const {
        return m_key;
    }

    bool isColumnInternal(int column) override;
    Capabilities getCapabilities() const override;
    QString modelKey(bool noSearch) const override;

  protected:
    void initSortColumnMapping() override;
    void refreshForMedia() override;

  private:
    // The view of the library rows at m_locations, in that order.
    bool createView();

    const QString m_viewName;
    QStringList m_locations;
    QString m_key;
};
