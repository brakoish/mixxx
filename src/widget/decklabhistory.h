// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Deck Lab Browse, round 5: track history and "played" marks.
//
// The data is Mixxx's own history: the "setlog" playlists (PlaylistDAO,
// hidden type PLHT_SET_LOG). No new database. A track counts as played by
// Mixxx's rule: SetlogFeature adds it to the current history playlist when
// PlayerInfo says it became the track on the audible deck (and not again
// within the duplicate distance). PlaylistDAO::trackAdded is that moment.
//
// A set spans restarts. Mixxx starts a new history playlist on every launch
// (a controller replug's Restart to detect, Power › Restart app, a crash or a
// power blip all do), so a set is every history playlist from the one where
// the DJ last held "New set" on: the set starts are kept in the Deck Lab
// settings ([DeckLab] HistorySetStarts, history playlist ids, never times).
// Playlists from before the first start (older history) stay one set each.
//
// Store keeps the played locations of the current set and of earlier sets in
// two hash sets, so a row asks "played?" with one lookup while it paints:
// never a database query. Listeners hear which locations changed and repaint
// only those rows. GUI thread only.

#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVariant>
#include <QVector>
#include <functional>
#include <memory>

#include "preferences/usersettings.h"
#include "proto/keys.pb.h"
#include "track/trackid.h"

class QAbstractItemModel;
class QModelIndex;
class QObject;
class TrackCollectionManager;

namespace decklab::history {

using ChromaticKey = mixxx::track::io::key::ChromaticKey;

enum class Played {
    No,
    Earlier, // in an earlier set's history (a subtler mark)
    Session, // in the current set's history (dimmed row and a check)
};

// One Mixxx history playlist (one run of the application).
struct Playlist {
    int id = -1;
    QString name;      // Mixxx's name: "2026-10-23", "2026-10-23 #2"
    QDateTime created; // local time
    QDateTime first;   // local time of the first track; invalid when empty
    QDateTime last;    // local time of the latest track
    int count = 0;
};

// One set: the history playlists from one "New set" to the next, or one
// older playlist from before Deck Lab marked sets.
struct Session {
    int id = -1;            // its set start (or the older playlist's id)
    QVector<int> playlists; // ascending
    QString name;
    QDateTime created; // local time
    QDateTime first;   // local time of the first track; invalid when empty
    QDateTime last;    // local time of the latest track
    int count = 0;
    int ordinal = 0; // 1 = the oldest set
    bool current = false;
    bool legacy = false;
};

// One track of a set, in play order.
struct Entry {
    int playlistId = -1; // the history playlist it is in
    int position = 0;    // PlaylistTracks.position (1 = first in that playlist)
    QDateTime played;    // local time it joined the history
    int deck = 0;        // 1..4 when this unit saw it play (not stored by Mixxx)
    int trackId = -1;
    QString location;
    QString title;
    QString artist;
    double bpm = 0;
    ChromaticKey key = mixxx::track::io::key::INVALID;
    QString keyText; // the stored key text, when there is no key id
};

// One track of the history, for the marks.
struct PlayedRow {
    int playlistId = -1;
    int trackId = -1;
    QString location;
};

// Item data of the History rows. LocationRole is also set on the rows of
// "Mixes with deck N", so their marks follow the history too.
enum Role {
    RowRole = Qt::UserRole + 200, // int: RowKind
    SessionRole,                  // int: set id
    DetailRole,                   // QString: a set's second line
    CountRole,                    // QString: a set's track count
    CurrentRole,                  // bool: the current set
    TimeRole,                     // QString: "21:04", "+0:12" or "#3"
    DeckRole,                     // int: 1..4, 0 unknown
    BpmRole,                      // double
    KeyRole,                      // QString
    ArtistRole,                   // QString
    LocationRole,                 // QString: the file, for the played mark
    PositionRole,                 // int: PlaylistTracks.position
    PlaylistRole,                 // int: the history playlist of a track row
    TrackIdRole,                  // int: library id of a track row
};

enum class RowKind {
    None = 0,
    Header,  // a date over earlier sets ("YESTERDAY")
    Session, // a set: opens its tracks
    Track,   // a track of a set: LOAD and Preview act on it
    Empty,   // "Nothing played yet"
    NewSet,  // "New set": press twice (the knob's hold to confirm)
};

// Whether the history's times can be shown as clock times. A Pi 4 has no
// real-time clock: until it syncs, times written at start-up can be days off.
enum class ClockState {
    Unknown,  // no system service: judged from the history itself
    Synced,   // decklabd: synchronized, or a real-time clock
    Unsynced, // decklabd: not synchronized and no real-time clock
};

// Where the history comes from: Mixxx's database in the application,
// generated data in tests.
class Backend {
  public:
    virtual ~Backend() = default;
    // Newest first.
    virtual QVector<Playlist> playlists() = 0;
    // One playlist's tracks in play order.
    virtual QVector<Entry> entries(int playlistId) = 0;
    // Every history track (playlist, library id, location).
    virtual QVector<PlayedRow> playedRows() = 0;
    // Mixxx's "Finish current and start new": the new history playlist's id,
    // -1 when it could not.
    virtual int startNewSet() = 0;
    // The set starts, kept across restarts.
    virtual QVector<int> setStarts() = 0;
    virtual void saveSetStarts(const QVector<int>& starts) = 0;
};

// What a listener hears.
struct Change {
    QSet<QString> locations; // rows whose mark changed
    bool history = false;    // sets or their tracks changed
    bool reloaded = false;   // ... so much that lists are built again
    QVector<Entry> added;    // tracks that joined the current set
};

class Store {
  public:
    static Store& instance();

    // The backend lives until it is replaced, or until owner is destroyed.
    void setBackend(std::unique_ptr<Backend> backend, QObject* owner = nullptr);
    bool hasBackend() const {
        return m_backend != nullptr;
    }
    // Sets and played locations, read once (Browse calls this when shown).
    void ensureLoaded();
    bool loaded() const {
        return m_loaded;
    }
    // Reads everything again and tells listeners what changed.
    void reload();

    // One hash lookup each.
    Played played(const QString& location) const;
    // A row of a Mixxx track model (TrackModel::getTrackLocation, which is
    // read from the row's own data). Returns at once while nothing is played.
    Played playedAt(const QModelIndex& index) const;
    bool anyPlayed() const {
        return !m_session.isEmpty() || !m_earlier.isEmpty();
    }
    // The location the history knows for a library track; null if none.
    QString historyLocation(int trackId) const {
        return m_trackLocations.value(trackId);
    }

    int currentId() const {
        return m_currentId;
    }
    // Newest first.
    const QVector<Session>& sessions() const {
        return m_sessions;
    }
    const Session* session(int id) const;
    // Tracks of one set, play order. Cached; the current set's grows live.
    QVector<Entry> entries(int sessionId);
    // Tracks in the current set so far.
    int currentCount() const;

    // A track joined a history playlist (PlaylistDAO::trackAdded).
    void addPlayed(int playlistId, const Entry& entry);
    // Ends the current set: Mixxx starts a new history playlist and it
    // starts the next set; the old set stays. No second read of the history.
    // false when the current set is still empty or it failed.
    bool startNewSet();

    void setClockState(ClockState state);
    ClockState clockState() const {
        return m_clock;
    }
    // Clock times and days can be shown (synced, or nothing looks wrong).
    bool clockTrusted() const;

    // Settings › Library › "Dim played tracks".
    void setDimPlayed(bool dim) {
        m_dim = dim;
    }
    bool dimPlayed() const {
        return m_dim;
    }

    // Called after every change, while owner lives.
    void listen(QObject* owner, std::function<void(const Change&)> listener);

    // Drops the backend, the data and the listeners.
    void reset();

  private:
    Store() = default;
    void read();
    void buildSessions();
    Session* mutableSession(int id);
    void notify(const Change& change);
    static qint64 deckKey(int playlistId, int position) {
        return (static_cast<qint64>(playlistId) << 32) | static_cast<quint32>(position);
    }

    std::unique_ptr<Backend> m_backend;
    QMetaObject::Connection m_ownerGone;
    bool m_loaded = false;
    bool m_dim = true;
    bool m_clockSuspect = false;
    ClockState m_clock = ClockState::Unknown;
    int m_currentId = -1;
    QVector<Playlist> m_playlists; // newest first
    QVector<int> m_starts;         // ascending
    QVector<Session> m_sessions;   // newest first
    QSet<QString> m_session;
    QSet<QString> m_earlier;
    QHash<int, QString> m_trackLocations;
    QHash<int, QVector<Entry>> m_entries;
    QHash<qint64, int> m_decks; // (playlist, position) -> deck seen on this unit
    struct Listener {
        QPointer<QObject> owner;
        std::function<void(const Change&)> callback;
    };
    QVector<Listener> m_listeners;
};

// Mixxx's history playlists: one query for the playlists, one per opened
// playlist, one for the played tracks; PlaylistDAO's signals keep the store
// live and TrackDAO's catch tracks that moved (relink, relocate).
// config: where the set starts are kept (null: in memory only).
// startNewSet: SetlogFeature's "Finish current and start new".
// deckOf: the deck a just-played track is on (default: PlayerInfo's audible
// deck, else a deck holding the track).
std::unique_ptr<Backend> makeMixxxBackend(TrackCollectionManager* collection,
        UserSettingsPointer config,
        std::function<bool()> startNewSet,
        std::function<int(TrackId)> deckOf = {});

// "21:04" (24 h, as on a CDJ).
QString clockText(const QDateTime& time);
// "+0:12": how long after the set's first track (clock not trusted).
QString offsetText(qint64 seconds);
// "52 min", "4 h 09 min"; empty under a minute or over a day (a clock jump).
QString durationText(qint64 seconds);
// "Today", "Yesterday", "Saturday" (this week), "Sat 11 Oct", "11 Oct 2025".
QString dayLabel(const QDate& day, const QDate& today);
// "This set" for the current one, else its time span "21:04 – 02:13", or
// "Set 12" when the clock can't be trusted.
QString sessionTitle(const Session& session, bool clockTrusted = true);
// "Since 21:04 · 14 tracks", "37 tracks · 4 h 09 min", "Nothing played yet".
QString sessionDetail(const Session& session, bool clockTrusted = true);

// The sessions list: the current set first, then "New set", then earlier
// sets under the day they began (one "EARLIER SETS" label without a clock).
struct ListRow {
    RowKind kind = RowKind::None;
    QString text;
    int sessionId = -1;
};
QVector<ListRow> sessionRows(const QVector<Session>& sessions, const QDate& today, bool clockTrusted = true);

// The row of a history playlist's table that holds a track (the table LOAD
// and Preview read): the same position and library id, else the same id
// elsewhere. Never another track; -1 when none. Ids survive a relocation.
int modelRowFor(const QAbstractItemModel& model,
        int positionColumn,
        int position,
        int trackId,
        const std::function<int(int row)>& trackIdOf);

} // namespace decklab::history
