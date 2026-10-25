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

#include <QObject>
#include <QSet>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
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

  signals:
    // Library views select again (BaseSqlTableModel), the history reads
    // again, crate and playlist counts are rebuilt.
    void changed();

  private:
    MediaPresence() = default;
    RootProbe m_probe;
    QSet<qint64> m_away;
    QStringList m_awayRoots;
};

} // namespace decklab
