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
// The current set is the newest history playlist (SetlogFeature makes a new
// one at start-up and for "Finish current and start new", and it is always
// the current one). Every other history playlist is an earlier set.
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

// One history playlist.
struct Session {
    int id = -1;
    QString name;      // Mixxx's name: "2026-10-23", "2026-10-23 #2"
    QDateTime created; // local time
    QDateTime first;   // local time of the first track; invalid when empty
    QDateTime last;    // local time of the latest track
    int count = 0;
    bool current = false;
};

// One track of a history playlist, in play order.
struct Entry {
    int position = 0;  // PlaylistTracks.position (1 = first played)
    QDateTime played;  // local time it joined the history
    int deck = 0;      // 1..4 when this unit saw it play (not stored by Mixxx)
    int trackId = -1;
    QString location;
    QString title;
    QString artist;
    double bpm = 0;
    ChromaticKey key = mixxx::track::io::key::INVALID;
    QString keyText; // the stored key text, when there is no key id
};

// Item data of the History rows. LocationRole is also set on the rows of
// "Mixes with deck N", so their marks follow the history too.
enum Role {
    RowRole = Qt::UserRole + 200, // int: RowKind
    SessionRole,                  // int: history playlist id
    DetailRole,                   // QString: a session's second line
    CountRole,                    // QString: a session's track count
    CurrentRole,                  // bool: the current set
    TimeRole,                     // QString: "21:04"
    DeckRole,                     // int: 1..4, 0 unknown
    BpmRole,                      // double
    KeyRole,                      // QString
    ArtistRole,                   // QString
    LocationRole,                 // QString: the file, for the played mark
    PositionRole,                 // int: PlaylistTracks.position
};

enum class RowKind {
    None = 0,
    Header,  // a date over earlier sets ("YESTERDAY")
    Session, // a set: opens its tracks
    Track,   // a track of a set: LOAD and Preview act on it
    Empty,   // "Nothing played yet"
};

// Where the history comes from: Mixxx's database in the application,
// generated data in tests.
class Backend {
  public:
    virtual ~Backend() = default;
    // Newest first. The first one is the current set.
    virtual QVector<Session> sessions() = 0;
    // Play order.
    virtual QVector<Entry> entries(int sessionId) = 0;
    // The locations of the current set's tracks and of every earlier set's.
    virtual void playedLocations(int currentId, QSet<QString>* session, QSet<QString>* earlier) = 0;
    // Mixxx's "Finish current and start new". false when it could not.
    virtual bool startNewSet() = 0;
};

// What a listener hears.
struct Change {
    QSet<QString> locations; // rows whose mark changed
    bool history = false;    // sets or their tracks changed
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

    int currentId() const {
        return m_currentId;
    }
    const QVector<Session>& sessions() const {
        return m_sessions;
    }
    const Session* session(int id) const;
    // Tracks of one set, play order. Cached; the current set's grows live.
    QVector<Entry> entries(int sessionId);
    // Tracks in the current set so far.
    int currentCount() const;

    // A track joined a history playlist (PlaylistDAO::trackAdded).
    void addPlayed(int sessionId, const Entry& entry);
    // Ends the current set (Mixxx starts a new history playlist; the old one
    // stays). false when the current set is still empty or it failed.
    bool startNewSet();

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
    void read(QVector<Session>* sessions, QSet<QString>* session, QSet<QString>* earlier);
    void notify(const Change& change);
    static qint64 deckKey(int sessionId, int position) {
        return (static_cast<qint64>(sessionId) << 32) | static_cast<quint32>(position);
    }

    std::unique_ptr<Backend> m_backend;
    QMetaObject::Connection m_ownerGone;
    bool m_loaded = false;
    bool m_dim = true;
    int m_currentId = -1;
    QVector<Session> m_sessions;
    QSet<QString> m_session;
    QSet<QString> m_earlier;
    QHash<int, QVector<Entry>> m_entries;
    QHash<qint64, int> m_decks; // (set, position) -> deck seen on this unit
    struct Listener {
        QPointer<QObject> owner;
        std::function<void(const Change&)> callback;
    };
    QVector<Listener> m_listeners;
};

// Mixxx's history playlists: one query for the sets, one per opened set,
// one for the played locations; PlaylistDAO's signals keep the store live.
// startNewSet: SetlogFeature's "Finish current and start new".
// deckOf: the deck a just-played track is on (default: PlayerInfo's audible
// deck, else a deck holding the track).
std::unique_ptr<Backend> makeMixxxBackend(TrackCollectionManager* collection,
        std::function<bool()> startNewSet,
        std::function<int(TrackId)> deckOf = {});

// "21:04" (24 h, as on a CDJ).
QString clockText(const QDateTime& time);
// "52 min", "4 h 09 min"; empty under a minute.
QString durationText(qint64 seconds);
// "Today", "Yesterday", "Saturday" (this week), "Sat 11 Oct", "11 Oct 2025".
QString dayLabel(const QDate& day, const QDate& today);
// "This set" for the current one, else its time span "21:04 – 02:13".
QString sessionTitle(const Session& session);
// "Since 21:04 · 14 tracks", "37 tracks · 4 h 09 min", "Nothing played yet".
QString sessionDetail(const Session& session);

// The sessions list: the current set first, then earlier sets under a date.
struct ListRow {
    RowKind kind = RowKind::None;
    QString text;
    int sessionId = -1;
};
QVector<ListRow> sessionRows(const QVector<Session>& sessions, const QDate& today);

// The row of a history playlist's table that holds position (the table LOAD
// and Preview read). The location must match; when the position moved, the
// first row with that location. -1 when none does.
int modelRowFor(const QAbstractItemModel& model,
        int positionColumn,
        int position,
        const QString& location,
        const std::function<QString(int row)>& locationOf);

} // namespace decklab::history
