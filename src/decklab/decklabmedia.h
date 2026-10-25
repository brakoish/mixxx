// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// r7-library: like a CDJ, Deck Lab shows only the media that is plugged in.
// Library rows on a USB stick that is out are left out of every list the DJ
// browses (Tracks, crates, playlists, history, search, Mixes, Missing tracks)
// and come back when the stick is plugged in again. Nothing is deleted or
// rewritten: the rows, their cues, crates and history stay as they are.
//
// How: the track_locations ids of those rows are kept in a temporary table
// of the GUI's database connection (temp.decklab_media_away), and the
// library's views add mediaShownCondition() to their WHERE. A refresh reads
// track_locations and looks at one folder per media root (whether it is
// there, and one directory entry): nothing is written to the database file
// and no stick is scanned. GUI thread only.
//
// r7: a stick plugged in mid-set stays cheap. While it is not quiet
// (decklabquiet.h) only the track list on screen reads again; every other
// list, the crate and playlist counts and the history's sets wait until
// they are shown or until it is quiet (settled()).

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <functional>

namespace decklab {

// The folder a removable-media location belongs to: /media/<a>/<b> for
// /media/<a>/<b>/..., /media/<a> for a file right inside it, and
// /run/media/<user>/<name> for /run/media/<user>/<name>/... Empty for every
// other location (the unit's own music never counts as media).
QString mediaRootOf(const QString& location);

// A media root is in when it is a folder (links followed) with at least one
// entry: a stick's mount, or the appliance's link to one. An empty folder an
// automounter left behind, a dangling link or nothing at all is out.
bool mediaRootIsIn(const QString& root);

// SQL, true for library rows that are not on media that is out.
// locationColumn names library.location (a track_locations id). Creates the
// temporary table on database first, so views over it can be created.
QString mediaShownCondition(const QSqlDatabase& database, const QString& locationColumn);
// SQL, true for library rows on media that is out.
QString mediaAwayCondition(const QSqlDatabase& database, const QString& locationColumn);

class MediaPresence final : public QObject {
    Q_OBJECT
  public:
    using RootProbe = std::function<bool(const QString& root)>;
    static MediaPresence& instance();

    // Reads which media roots are in and fills the temporary table of
    // database again. Returns true when other rows are hidden than before;
    // then changed() is emitted, and always with force (the library changed
    // under the views: a merge or a relink).
    bool refresh(QSqlDatabase database, bool force = false);
    // Media roots that are out, sorted (for the log and tests).
    QStringList awayRoots() const {
        return m_awayRoots;
    }
    // Library locations hidden because their media is out.
    int awayCount() const {
        return static_cast<int>(m_away.size());
    }
    // Tests decide which roots are in; null restores the real check. What
    // was hidden is forgotten.
    void setRootProbeForTesting(RootProbe probe) {
        m_probe = std::move(probe);
        m_away.clear();
        m_awayRoots.clear();
    }

    // r7: whether background work may run now (DeckLabSystem::quiet); null
    // (tests, no system) counts as quiet: every list reads again at once.
    void setQuiet(std::function<bool()> quiet) {
        m_quiet = std::move(quiet);
    }
    // A change now reaches only the list on screen.
    bool deferring() const {
        return m_quiet && !m_quiet();
    }
    // The track list on screen (Library::slotShowTrackModel). A list a
    // change left stale reads again as it is shown.
    void setShown(QObject* model);
    QObject* shown() const {
        return m_shown;
    }
    // Something waits for settled().
    bool settling() const {
        return m_settle.isActive();
    }
    static constexpr int kSettleCheckMs = 2000;

  signals:
    // Library views select again (BaseSqlTableModel: the one on screen; the
    // others too while it is quiet), the history reads again, crate and
    // playlist counts are rebuilt.
    void changed();
    // r7: it is quiet again after a change that was deferred: what waited
    // reads again now.
    void settled();
    // r7: model became the list on screen.
    void shownChanged(QObject* model);

  private:
    MediaPresence();
    void checkSettled();
    RootProbe m_probe;
    QSet<qint64> m_away;
    QStringList m_awayRoots;
    std::function<bool()> m_quiet;
    QPointer<QObject> m_shown;
    QTimer m_settle;
};

} // namespace decklab
