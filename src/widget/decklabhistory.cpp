// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabhistory.h"

#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSaveFile>
#include <QSqlQuery>
#include <QStringList>
#include <QTimer>
#include <algorithm>
#include <initializer_list>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

#include "decklab/decklabmedia.h"
#include "library/dao/playlistdao.h"
#include "library/dao/trackdao.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/trackmodel.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "track/track.h"
#include "util/datetime.h"

namespace decklab::history {

namespace {

// Round-5 builds before r5-fix2 kept the set starts here; read once, then
// moved into their own file (setStartsFile).
const ConfigKey kSetStartsKey(QStringLiteral("[DeckLab]"), QStringLiteral("HistorySetStarts"));
constexpr qint64 kDaySeconds = 24 * 60 * 60;

QString tr(const char* text) {
    return QCoreApplication::translate("DeckLabHistory", text);
}

QString countText(int count) {
    return count == 1 ? tr("1 track") : tr("%1 tracks").arg(QLocale().toString(count));
}

// SQLite's CURRENT_TIMESTAMP: "2026-10-23 21:04:12", UTC.
QDateTime localFromUtc(const QVariant& value) {
    if (value.isNull()) {
        return {};
    }
    const QString text = value.toString();
    QDateTime time = QDateTime::fromString(text, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    if (!time.isValid()) {
        time = QDateTime::fromString(text, Qt::ISODate);
    }
    return time.isValid() ? mixxx::localDateTimeFromUtc(time) : QDateTime();
}

int audibleDeckOf(TrackId trackId) {
    // The deck PlayerInfo says the room hears; else a deck holding the track.
    auto& info = PlayerInfo::instance();
    const auto holds = [&info, &trackId](int deck) {
        const TrackPointer track = info.getTrackInfo(PlayerManager::groupForDeck(deck - 1));
        return track && track->getId() == trackId;
    };
    const int audible = info.getCurrentPlayingDeck() + 1;
    if (audible >= 1 && audible <= 4 && holds(audible)) {
        return audible;
    }
    for (int deck = 1; deck <= 4; ++deck) {
        if (holds(deck)) {
            return deck;
        }
    }
    return 0;
}

class MixxxBackend final : public Backend {
  public:
    MixxxBackend(TrackCollectionManager* collection,
            UserSettingsPointer config,
            std::function<bool()> startNewSet,
            std::function<int(TrackId)> deckOf)
            : m_collection(collection),
              m_config(std::move(config)),
              m_file(m_config ? setStartsFile(m_config->getSettingsPath()) : QString()),
              m_startNewSet(std::move(startNewSet)),
              m_deckOf(std::move(deckOf)) {
        // Sets that start or go elsewhere, and tracks taken out of a set, are
        // rare: read everything again once the event loop runs (never during
        // shutdown, when SetlogFeature tidies up the history).
        m_reload.setSingleShot(true);
        m_reload.setInterval(0);
        QObject::connect(&m_reload, &QTimer::timeout, &m_context, [] {
            if (Store::instance().loaded()) {
                Store::instance().reload();
            }
        });
        // Tracks whose file moved (USB relink, Mixxx's relocate): checked
        // together, a moment later, against the locations the marks use.
        m_moved.setSingleShot(true);
        m_moved.setInterval(250);
        QObject::connect(&m_moved, &QTimer::timeout, &m_context, [this] { checkMoved(); });
        if (!m_collection || !m_collection->internalCollection()) {
            return;
        }
        PlaylistDAO* playlists = &m_collection->internalCollection()->getPlaylistDAO();
        QObject::connect(playlists,
                &PlaylistDAO::trackAdded,
                &m_context,
                [this](int playlistId, TrackId trackId, int position) {
                    trackAdded(playlistId, trackId, position);
                });
        QObject::connect(playlists, &PlaylistDAO::added, &m_context, [this](int playlistId) {
            if (m_startingSet) {
                // Ours ("New set"): the store updates itself, no second read.
                m_newPlaylist = playlistId;
                m_history.insert(playlistId);
            } else if (isHistory(playlistId)) {
                m_reload.start();
            }
        });
        QObject::connect(playlists, &PlaylistDAO::deleted, &m_context, [this](int playlistId) {
            if (playlistId == kInvalidPlaylistId || m_history.contains(playlistId)) {
                m_reload.start();
            }
        });
        const auto changed = [this](const QSet<int>& playlistIds) {
            for (const int playlistId : playlistIds) {
                if (m_history.contains(playlistId)) {
                    m_reload.start();
                    return;
                }
            }
        };
        QObject::connect(playlists, &PlaylistDAO::tracksRemoved, &m_context, changed);
        QObject::connect(playlists, &PlaylistDAO::tracksMoved, &m_context, changed);
        TrackDAO* tracks = &m_collection->internalCollection()->getTrackDAO();
        const auto moved = [this](const QSet<TrackId>& trackIds) {
            m_movedIds.unite(trackIds);
            m_moved.start();
        };
        QObject::connect(tracks, &TrackDAO::tracksChanged, &m_context, moved);
        QObject::connect(tracks, &TrackDAO::tracksRemoved, &m_context, moved);
        // r7-library: a USB stick in or out, or its tracks merged: the sets
        // are read again (decklabmedia.h).
        QObject::connect(&MediaPresence::instance(), &MediaPresence::changed, &m_context, [this] {
            m_reload.start();
        });
    }

    QVector<Playlist> playlists() override {
        QVector<Playlist> result;
        if (!m_collection) {
            return result;
        }
        QSqlQuery query(database());
        query.setForwardOnly(true);
        // r7-library: tracks on a USB stick that is out are counted apart.
        const QString away = mediaAwayCondition(database(), QStringLiteral("library.location"));
        const QString sql = QStringLiteral(
                "SELECT Playlists.id, Playlists.name, Playlists.date_created, "
                "COUNT(PlaylistTracks.track_id) - SUM(CASE WHEN %2 THEN 1 ELSE 0 END), "
                "MIN(PlaylistTracks.pl_datetime_added), MAX(PlaylistTracks.pl_datetime_added), "
                "SUM(CASE WHEN %2 THEN 1 ELSE 0 END) "
                "FROM Playlists LEFT JOIN PlaylistTracks ON PlaylistTracks.playlist_id = Playlists.id "
                "LEFT JOIN library ON library.id = PlaylistTracks.track_id "
                "WHERE Playlists.hidden = %1 GROUP BY Playlists.id ORDER BY Playlists.id DESC")
                                    .arg(QString::number(static_cast<int>(PlaylistDAO::PLHT_SET_LOG)), away);
        if (!query.exec(sql)) {
            qWarning() << "DeckLab history: could not read the sets" << query.lastError().text();
            m_readOk = false; // r5-fix3-history
            return result;
        }
        m_readOk = true;
        m_history.clear();
        while (query.next()) {
            Playlist playlist;
            playlist.id = query.value(0).toInt();
            playlist.name = query.value(1).toString();
            playlist.created = localFromUtc(query.value(2));
            playlist.count = query.value(3).toInt();
            playlist.first = localFromUtc(query.value(4));
            playlist.last = localFromUtc(query.value(5));
            playlist.away = query.value(6).toInt();
            m_history.insert(playlist.id);
            result.append(playlist);
        }
        return result;
    }

    QVector<Entry> entries(int playlistId) override {
        return read(playlistId, -1);
    }

    QVector<PlayedRow> playedRows() override {
        QVector<PlayedRow> result;
        if (!m_collection) {
            return result;
        }
        QSqlQuery query(database());
        query.setForwardOnly(true);
        const QString sql = QStringLiteral(
                "SELECT PlaylistTracks.playlist_id, library.id, track_locations.location FROM PlaylistTracks "
                "INNER JOIN Playlists ON Playlists.id = PlaylistTracks.playlist_id "
                "INNER JOIN library ON library.id = PlaylistTracks.track_id "
                "INNER JOIN track_locations ON track_locations.id = library.location "
                "WHERE Playlists.hidden = %1")
                                    .arg(static_cast<int>(PlaylistDAO::PLHT_SET_LOG));
        if (!query.exec(sql)) {
            qWarning() << "DeckLab history: could not read played tracks" << query.lastError().text();
            return result;
        }
        while (query.next()) {
            PlayedRow row;
            row.playlistId = query.value(0).toInt();
            row.trackId = query.value(1).toInt();
            row.location = QDir::fromNativeSeparators(query.value(2).toString());
            result.append(row);
        }
        return result;
    }

    int startNewSet() override {
        if (!m_startNewSet) {
            return -1;
        }
        // r5-fix3-history: only a playlist newer than every history playlist
        // before the call starts the set; when Mixxx could not create one,
        // nothing does (never this set's own playlist).
        const int before = newestHistoryPlaylist();
        m_startingSet = true;
        m_newPlaylist = -1;
        const bool started = m_startNewSet();
        m_startingSet = false;
        if (!started) {
            return -1;
        }
        if (m_newPlaylist < 0) {
            m_newPlaylist = newestHistoryPlaylist();
        }
        return m_newPlaylist > before ? m_newPlaylist : -1;
    }

    QVector<int> setStarts() override {
        if (m_file.isEmpty()) {
            return m_starts;
        }
        // r5-fix3-history: the file is read once; afterwards the starts in
        // memory are the truth for this session (a New set whose write
        // failed is never undone by a later reload), and a failed write is
        // tried again here.
        if (!m_startsRead) {
            m_startsRead = true;
            loadSetStarts();
        } else if (m_startsDirty && m_fileWritable && writeSetStarts(m_file, m_starts)) {
            m_startsDirty = false;
        }
        return m_starts;
    }

    bool saveSetStarts(const QVector<int>& starts) override {
        m_starts = starts;
        m_startsRead = true;
        if (m_file.isEmpty()) {
            return true;
        }
        // Their own small file, replaced atomically and synced: never
        // Mixxx's config save (a power cut during it can empty mixxx.cfg).
        m_startsDirty = !m_fileWritable || !writeSetStarts(m_file, starts);
        if (m_startsDirty) {
            qWarning() << "DeckLab history: could not save the set starts; kept for this session";
        }
        return !m_startsDirty;
    }

    bool readOk() const override {
        return m_readOk;
    }

    bool startsSaved() const override {
        return !m_startsDirty;
    }

  private:
    QSqlDatabase database() const {
        return m_collection->internalCollection()->database();
    }
    int newestHistoryPlaylist() {
        if (!m_collection) {
            return -1;
        }
        QSqlQuery query(database());
        if (query.exec(QStringLiteral("SELECT MAX(id) FROM Playlists WHERE hidden = %1")
                                .arg(static_cast<int>(PlaylistDAO::PLHT_SET_LOG))) &&
                query.next() && !query.value(0).isNull()) {
            return query.value(0).toInt();
        }
        return -1;
    }
    void loadSetStarts() {
        // r5-fix3-history: a file that is there but cannot be read (a passing
        // read error, a damaged file) is never taken for "no file yet": it is
        // read twice, then copied aside before anything replaces it, and the
        // session goes on from memory. Without a copy nothing writes it.
        if (QFileInfo::exists(m_file)) {
            auto starts = readSetStarts(m_file);
            if (!starts) {
                starts = readSetStarts(m_file);
            }
            if (starts) {
                m_starts = *starts;
                return;
            }
            const QString copy = m_file + QStringLiteral(".bad");
            QFile::remove(copy);
            m_fileWritable = QFile::copy(m_file, copy);
            qWarning() << "DeckLab history: the set starts file could not be read;"
                       << (m_fileWritable ? "a copy was kept beside it" : "it is left as it is");
            m_starts.clear();
            return;
        }
        // Once: a round-5 build kept them in the config. Move them into the
        // file; the config value goes with Mixxx's own next save.
        m_starts.clear();
        if (m_config && m_config->exists(kSetStartsKey)) {
            const QStringList parts = m_config->getValueString(kSetStartsKey).split(QLatin1Char(','), Qt::SkipEmptyParts);
            for (const QString& part : parts) {
                bool ok = false;
                const int id = part.trimmed().toInt(&ok);
                if (ok && id >= 0) {
                    m_starts.append(id);
                }
            }
            if (writeSetStarts(m_file, m_starts)) {
                m_config->remove(kSetStartsKey);
            } else {
                m_startsDirty = true;
            }
        }
    }
    bool isHistory(int playlistId) {
        if (m_history.contains(playlistId)) {
            return true;
        }
        if (m_other.contains(playlistId) || playlistId == kInvalidPlaylistId || !m_collection) {
            return false;
        }
        const bool history = m_collection->internalCollection()->getPlaylistDAO().getHiddenType(playlistId) ==
                PlaylistDAO::PLHT_SET_LOG;
        (history ? m_history : m_other).insert(playlistId);
        return history;
    }
    // A playlist's tracks in play order (position < 0), or the one at position.
    QVector<Entry> read(int playlistId, int position) {
        QVector<Entry> result;
        if (!m_collection) {
            return result;
        }
        QSqlQuery query(database());
        query.setForwardOnly(true);
        query.prepare(QStringLiteral(
                "SELECT PlaylistTracks.position, PlaylistTracks.pl_datetime_added, library.id, "
                "library.title, library.artist, library.bpm, library.key_id, library.key, "
                "track_locations.location FROM PlaylistTracks "
                "INNER JOIN library ON library.id = PlaylistTracks.track_id "
                "LEFT JOIN track_locations ON track_locations.id = library.location "
                "WHERE PlaylistTracks.playlist_id = :id%1 AND %2 ORDER BY PlaylistTracks.position")
                        .arg(position >= 0 ? QStringLiteral(" AND PlaylistTracks.position = :position")
                                           : QString(),
                                // r7-library: not tracks on a USB stick that is out.
                                mediaShownCondition(database(), QStringLiteral("library.location"))));
        query.bindValue(QStringLiteral(":id"), playlistId);
        if (position >= 0) {
            query.bindValue(QStringLiteral(":position"), position);
        }
        if (!query.exec()) {
            qWarning() << "DeckLab history: could not read a set" << query.lastError().text();
            return result;
        }
        while (query.next()) {
            Entry entry;
            entry.playlistId = playlistId;
            entry.position = query.value(0).toInt();
            entry.played = localFromUtc(query.value(1));
            entry.trackId = query.value(2).toInt();
            entry.title = query.value(3).toString().trimmed();
            entry.artist = query.value(4).toString().trimmed();
            entry.bpm = query.value(5).toDouble();
            const int key = query.value(6).toInt();
            entry.key = mixxx::track::io::key::ChromaticKey_IsValid(key)
                    ? static_cast<ChromaticKey>(key)
                    : mixxx::track::io::key::INVALID;
            entry.keyText = query.value(7).toString();
            entry.location = QDir::fromNativeSeparators(query.value(8).toString());
            if (entry.title.isEmpty()) {
                entry.title = QFileInfo(entry.location).completeBaseName();
            }
            result.append(entry);
        }
        return result;
    }
    void trackAdded(int playlistId, TrackId trackId, int position) {
        if (!isHistory(playlistId)) {
            return;
        }
        const QVector<Entry> rows = read(playlistId, position);
        Entry entry;
        if (!rows.isEmpty()) {
            entry = rows.first();
        } else {
            entry.playlistId = playlistId;
            entry.position = position;
            entry.trackId = trackId.toVariant().toInt();
            entry.played = QDateTime::currentDateTime();
        }
        entry.deck = m_deckOf ? m_deckOf(trackId) : audibleDeckOf(trackId);
        Store::instance().addPlayed(playlistId, entry);
    }
    void checkMoved() {
        // Only history tracks, and only when their file really moved: a
        // track saved after it played (play count, analysis) changes nothing.
        const auto& store = Store::instance();
        QHash<int, QString> known;
        for (const TrackId& id : std::as_const(m_movedIds)) {
            const int trackId = id.toVariant().toInt();
            const QString location = store.historyLocation(trackId);
            if (!location.isNull()) {
                known.insert(trackId, location);
            }
        }
        m_movedIds.clear();
        if (known.isEmpty() || !m_collection) {
            return;
        }
        QStringList ids;
        for (auto it = known.cbegin(); it != known.cend(); ++it) {
            ids << QString::number(it.key());
        }
        QSqlQuery query(database());
        query.setForwardOnly(true);
        if (!query.exec(QStringLiteral("SELECT library.id, track_locations.location FROM library "
                                       "LEFT JOIN track_locations ON track_locations.id = library.location "
                                       "WHERE library.id IN (%1)")
                                .arg(ids.join(QLatin1Char(','))))) {
            return;
        }
        int found = 0;
        bool moved = false;
        while (query.next()) {
            ++found;
            moved = moved || QDir::fromNativeSeparators(query.value(1).toString()) != known.value(query.value(0).toInt());
        }
        if (moved || found != known.size()) {
            Store::instance().reload();
        }
    }

    QPointer<TrackCollectionManager> m_collection;
    UserSettingsPointer m_config;
    QString m_file; // decklab-history-sets.json in the profile
    bool m_startsRead = false;  // r5-fix3-history: the file is read once
    bool m_startsDirty = false; // the starts in memory are not on disk yet
    bool m_fileWritable = true; // false: an unreadable file that could not be copied aside
    bool m_readOk = true;       // the last playlists() read worked
    std::function<bool()> m_startNewSet;
    std::function<int(TrackId)> m_deckOf;
    QVector<int> m_starts; // without settings (tests)
    QSet<int> m_history;   // history playlists seen
    QSet<int> m_other;     // playlists known not to be history
    QSet<TrackId> m_movedIds;
    bool m_startingSet = false;
    int m_newPlaylist = -1;
    QObject m_context;
    QTimer m_reload;
    QTimer m_moved;
};

} // namespace

Store& Store::instance() {
    static Store store;
    return store;
}

void Store::setBackend(std::unique_ptr<Backend> backend, QObject* owner) {
    QObject::disconnect(m_ownerGone);
    m_backend = std::move(backend);
    m_loaded = false;
    m_clockSuspect = false;
    m_currentId = -1;
    m_playlists.clear();
    m_starts.clear();
    m_sessions.clear();
    m_session.clear();
    m_earlier.clear();
    m_trackLocations.clear();
    m_entries.clear();
    m_decks.clear();
    if (owner && m_backend) {
        const Backend* installed = m_backend.get();
        m_ownerGone = QObject::connect(owner, &QObject::destroyed, [this, installed] {
            if (m_backend.get() == installed) {
                setBackend(nullptr);
            }
        });
    }
}

void Store::reset() {
    setBackend(nullptr);
    m_listeners.clear();
    m_dim = true;
    m_clock = ClockState::Unknown;
}

void Store::read() {
    m_playlists = m_backend->playlists();
    // The set starts: drop starts whose playlists are all gone; the first
    // run starts the set with this run's playlist (older ones stay single).
    QVector<int> starts = m_backend->setStarts();
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    const int newest = m_playlists.isEmpty() ? -1 : m_playlists.first().id;
    bool changed = false;
    // r5-fix3-history: only a read that worked prunes or seeds the starts (a
    // failed one looks like "no history" and would drop every set boundary).
    const bool readOk = m_backend->readOk();
    while (readOk && !starts.isEmpty() && starts.last() > newest) {
        starts.removeLast();
        changed = true;
    }
    if (readOk && starts.isEmpty() && newest >= 0) {
        starts.append(newest);
        changed = true;
    }
    if (changed) {
        m_backend->saveSetStarts(starts);
    }
    m_starts = starts;
    buildSessions();
    m_session.clear();
    m_earlier.clear();
    m_trackLocations.clear();
    const int currentStart = m_starts.isEmpty() ? -1 : m_starts.last();
    for (const PlayedRow& row : m_backend->playedRows()) {
        if (row.location.isEmpty()) {
            continue;
        }
        (currentStart >= 0 && row.playlistId >= currentStart ? m_session : m_earlier).insert(row.location);
        m_trackLocations.insert(row.trackId, row.location);
    }
    // Times later than now: the clock went back since (no real-time clock).
    const QDateTime soon = QDateTime::currentDateTime().addSecs(10 * 60);
    m_clockSuspect = std::any_of(m_playlists.cbegin(), m_playlists.cend(), [&soon](const Playlist& playlist) {
        return (playlist.last.isValid() && playlist.last > soon) || (playlist.created.isValid() && playlist.created > soon);
    });
}

void Store::buildSessions() {
    QVector<Session> ascending;
    const int firstStart = m_starts.isEmpty() ? -1 : m_starts.first();
    for (auto it = m_playlists.crbegin(); it != m_playlists.crend(); ++it) {
        const Playlist& playlist = *it;
        int key = playlist.id;
        bool legacy = firstStart < 0 || playlist.id < firstStart;
        if (!legacy) {
            key = *std::prev(std::upper_bound(m_starts.cbegin(), m_starts.cend(), playlist.id));
        }
        if (legacy || ascending.isEmpty() || ascending.last().legacy || ascending.last().id != key) {
            Session session;
            session.id = key;
            session.name = playlist.name;
            session.created = playlist.created;
            session.legacy = legacy;
            ascending.append(session);
        }
        Session& session = ascending.last();
        session.playlists.append(playlist.id);
        session.count += playlist.count;
        session.away += playlist.away;
        if (playlist.first.isValid() && (!session.first.isValid() || playlist.first < session.first)) {
            session.first = playlist.first;
        }
        if (playlist.last.isValid() && (!session.last.isValid() || playlist.last > session.last)) {
            session.last = playlist.last;
        }
    }
    m_currentId = -1;
    for (int n = 0; n < ascending.size(); ++n) {
        ascending[n].ordinal = n + 1;
        ascending[n].current = !ascending[n].legacy && !m_starts.isEmpty() && ascending[n].id == m_starts.last();
        if (ascending[n].current) {
            m_currentId = ascending[n].id;
        }
    }
    std::reverse(ascending.begin(), ascending.end());
    m_sessions = ascending;
}

void Store::ensureLoaded() {
    if (!m_loaded && m_backend) {
        reload();
    }
}

void Store::reload() {
    if (!m_backend) {
        return;
    }
    const bool first = !m_loaded;
    const QSet<QString> oldSession = m_session;
    const QSet<QString> oldEarlier = m_earlier;
    read();
    m_entries.clear();
    m_loaded = true;
    Change change;
    change.history = true;
    change.reloaded = true;
    const auto state = [](const QSet<QString>& session, const QSet<QString>& earlier, const QString& location) {
        return session.contains(location) ? 2 : earlier.contains(location) ? 1 : 0;
    };
    // Only rows whose mark really changed repaint (first load: all of them).
    const std::initializer_list<const QSet<QString>*> sets{&oldSession, &oldEarlier, &m_session, &m_earlier};
    for (const QSet<QString>* set : sets) {
        if (first && (set == &oldSession || set == &oldEarlier)) {
            continue;
        }
        for (const QString& location : *set) {
            if (state(oldSession, oldEarlier, location) != state(m_session, m_earlier, location)) {
                change.locations.insert(location);
            }
        }
    }
    notify(change);
}

Played Store::played(const QString& location) const {
    if (m_session.contains(location)) {
        return Played::Session;
    }
    if (m_earlier.contains(location)) {
        return Played::Earlier;
    }
    // Mixxx keeps a loaded file's location clean; an export's own list may
    // join "mount/" and "/Contents/..." with a doubled slash.
    if (location.contains(QLatin1String("//"))) {
        const QString clean = QDir::cleanPath(location);
        if (clean != location) {
            return played(clean);
        }
    }
    return Played::No;
}

Played Store::playedAt(const QModelIndex& index) const {
    if (!anyPlayed() || !index.isValid()) {
        return Played::No;
    }
    const auto* tracks = dynamic_cast<const TrackModel*>(index.model());
    return tracks ? played(tracks->getTrackLocation(index)) : Played::No;
}

const Session* Store::session(int id) const {
    for (const auto& each : m_sessions) {
        if (each.id == id) {
            return &each;
        }
    }
    return nullptr;
}

Session* Store::mutableSession(int id) {
    for (auto& each : m_sessions) {
        if (each.id == id) {
            return &each;
        }
    }
    return nullptr;
}

QVector<Entry> Store::entries(int sessionId) {
    ensureLoaded();
    const Session* set = session(sessionId);
    if (!m_backend || !set) {
        return {};
    }
    auto it = m_entries.find(sessionId);
    if (it == m_entries.end()) {
        QVector<Entry> rows;
        for (const int playlistId : set->playlists) {
            for (Entry entry : m_backend->entries(playlistId)) {
                entry.playlistId = playlistId;
                entry.deck = m_decks.value(deckKey(playlistId, entry.position), entry.deck);
                rows.append(entry);
            }
        }
        it = m_entries.insert(sessionId, rows);
    }
    return *it;
}

int Store::currentCount() const {
    const Session* current = session(m_currentId);
    return current ? current->count : 0;
}

void Store::addPlayed(int playlistId, const Entry& added) {
    if (!m_backend) {
        return;
    }
    Entry entry = added;
    entry.playlistId = playlistId;
    if (entry.deck > 0) {
        m_decks.insert(deckKey(playlistId, entry.position), entry.deck);
    }
    Session* current = mutableSession(m_currentId);
    const int currentStart = m_starts.isEmpty() ? -1 : m_starts.last();
    if (!m_loaded || !current || playlistId < currentStart) {
        // Not read yet, or an earlier set edited: read it all again (rare).
        reload();
        return;
    }
    if (!current->playlists.contains(playlistId)) {
        // A new history playlist inside this set (Mixxx's own "start new").
        current->playlists.append(playlistId);
        Playlist playlist;
        playlist.id = playlistId;
        playlist.created = entry.played;
        m_playlists.prepend(playlist);
    }
    Change change;
    change.history = true;
    change.added.append(entry);
    if (!entry.location.isEmpty() && !m_session.contains(entry.location)) {
        m_session.insert(entry.location);
        change.locations.insert(entry.location);
    }
    if (entry.trackId >= 0 && !entry.location.isEmpty()) {
        m_trackLocations.insert(entry.trackId, entry.location);
    }
    ++current->count;
    current->last = entry.played;
    if (!current->first.isValid()) {
        current->first = entry.played;
    }
    auto it = m_entries.find(current->id);
    if (it != m_entries.end() &&
            std::none_of(it->cbegin(), it->cend(), [&entry](const Entry& known) {
                return known.playlistId == entry.playlistId && known.position == entry.position;
            })) {
        it->append(entry);
    }
    notify(change);
}

bool Store::startNewSet() {
    ensureLoaded();
    if (!m_backend || currentCount() == 0) {
        return false;
    }
    const int playlistId = m_backend->startNewSet();
    // r5-fix3-history: never a playlist the history already holds.
    if (playlistId < 0 || std::any_of(m_playlists.cbegin(), m_playlists.cend(), [playlistId](const Playlist& each) {
            return each.id == playlistId;
        })) {
        return false;
    }
    // The new history playlist starts the next set; the last one stays as it
    // is. Everything needed is in memory: no second read of the history.
    m_starts.append(playlistId);
    m_backend->saveSetStarts(m_starts);
    Playlist playlist;
    playlist.id = playlistId;
    playlist.created = QDateTime::currentDateTime();
    m_playlists.prepend(playlist);
    for (auto& each : m_sessions) {
        each.current = false;
    }
    Session fresh;
    fresh.id = playlistId;
    fresh.playlists = {playlistId};
    fresh.created = playlist.created;
    fresh.current = true;
    fresh.ordinal = static_cast<int>(m_sessions.size()) + 1;
    m_sessions.prepend(fresh);
    m_currentId = playlistId;
    Change change;
    change.history = true;
    change.reloaded = true;
    change.locations = m_session;
    m_earlier.unite(m_session);
    m_session.clear();
    notify(change);
    return true;
}

void Store::setClockState(ClockState state) {
    if (m_clock == state) {
        return;
    }
    const bool trusted = clockTrusted();
    m_clock = state;
    if (trusted != clockTrusted() && m_loaded) {
        Change change;
        change.history = true;
        change.reloaded = true;
        notify(change);
    }
}

bool Store::clockTrusted() const {
    switch (m_clock) {
    case ClockState::Synced:
        return true;
    case ClockState::Unsynced:
        return false;
    case ClockState::Unknown:
        break;
    }
    return !m_clockSuspect;
}

void Store::listen(QObject* owner, std::function<void(const Change&)> listener) {
    m_listeners.append(Listener{QPointer<QObject>(owner), std::move(listener)});
}

void Store::notify(const Change& change) {
    m_listeners.erase(std::remove_if(m_listeners.begin(),
                              m_listeners.end(),
                              [](const Listener& listener) { return listener.owner.isNull(); }),
            m_listeners.end());
    const QVector<Listener> listeners = m_listeners;
    for (const auto& listener : listeners) {
        if (listener.owner && listener.callback) {
            listener.callback(change);
        }
    }
}

QString setStartsFile(const QString& settingsPath) {
    return settingsPath.isEmpty() ? QString()
                                  : QDir(settingsPath).filePath(QStringLiteral("decklab-history-sets.json"));
}

bool writeSetStarts(const QString& path, const QVector<int>& starts) {
    QJsonArray array;
    for (const int id : starts) {
        array.append(id);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("starts"), array);
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    // A new file beside the old one, synced, then renamed over it: the old
    // starts or the new ones survive a power cut, never an empty file.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (file.write(bytes) != bytes.size() || !file.flush()) {
        file.cancelWriting();
        return false;
    }
#ifdef Q_OS_UNIX
    if (::fsync(file.handle()) != 0) {
        file.cancelWriting();
        return false;
    }
#endif
    if (!file.commit()) {
        return false;
    }
#ifdef Q_OS_UNIX
    // The rename is in the folder: sync it too.
    const QByteArray folder = QFile::encodeName(QFileInfo(path).absolutePath());
    const int handle = ::open(folder.constData(), O_RDONLY | O_DIRECTORY);
    if (handle >= 0) {
        ::fsync(handle);
        ::close(handle);
    }
#endif
    return true;
}

std::optional<QVector<int>> readSetStarts(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        return std::nullopt;
    }
    QVector<int> starts;
    const QJsonArray array = document.object().value(QStringLiteral("starts")).toArray();
    for (const QJsonValue& value : array) {
        if (value.isDouble() && value.toInt(-1) >= 0) {
            starts.append(value.toInt());
        }
    }
    return starts;
}

std::unique_ptr<Backend> makeMixxxBackend(TrackCollectionManager* collection,
        UserSettingsPointer config,
        std::function<bool()> startNewSet,
        std::function<int(TrackId)> deckOf) {
    return std::make_unique<MixxxBackend>(collection, std::move(config), std::move(startNewSet), std::move(deckOf));
}

QString clockText(const QDateTime& time) {
    return time.isValid() ? time.toString(QStringLiteral("HH:mm")) : QString();
}

QString offsetText(qint64 seconds) {
    const qint64 minutes = std::max<qint64>(0, seconds) / 60;
    return QStringLiteral("+%1:%2").arg(minutes / 60).arg(minutes % 60, 2, 10, QLatin1Char('0'));
}

QString durationText(qint64 seconds) {
    if (seconds < 60 || seconds > kDaySeconds) {
        return QString();
    }
    const qint64 minutes = seconds / 60;
    if (minutes < 60) {
        return tr("%1 min").arg(minutes);
    }
    return tr("%1 h %2 min").arg(minutes / 60).arg(minutes % 60, 2, 10, QLatin1Char('0'));
}

QString dayLabel(const QDate& day, const QDate& today) {
    if (!day.isValid()) {
        return QString();
    }
    const qint64 ago = day.daysTo(today);
    if (ago == 0) {
        return tr("Today");
    }
    if (ago == 1) {
        return tr("Yesterday");
    }
    if (ago > 1 && ago < 7) {
        return QLocale().standaloneDayName(day.dayOfWeek(), QLocale::LongFormat);
    }
    if (day.year() == today.year()) {
        return QLocale().toString(day, QStringLiteral("ddd d MMM"));
    }
    return QLocale().toString(day, QStringLiteral("d MMM yyyy"));
}

QString sessionTitle(const Session& session, bool clockTrusted) {
    if (session.current) {
        return tr("This set");
    }
    if (!clockTrusted) {
        return tr("Set %1").arg(session.ordinal);
    }
    if (session.first.isValid() && session.last.isValid() && clockText(session.first) != clockText(session.last)) {
        return QString(clockText(session.first) + QStringLiteral(" – ") + clockText(session.last));
    }
    return clockText(session.first.isValid() ? session.first : session.created);
}

QString sessionDetail(const Session& session, bool clockTrusted) {
    if (session.count <= 0) {
        // r7-library: what was played is on a USB stick that is out.
        return session.away > 0 ? tr("Played from a USB stick that is out") : tr("Nothing played yet");
    }
    const QString length = session.first.isValid() && session.last.isValid()
            ? durationText(session.first.secsTo(session.last))
            : QString();
    if (session.current && clockTrusted && session.first.isValid()) {
        return tr("Since %1 · %2").arg(clockText(session.first), countText(session.count));
    }
    return length.isEmpty() ? countText(session.count) : QString(countText(session.count) + QStringLiteral(" · ") + length);
}

QVector<ListRow> sessionRows(const QVector<Session>& sessions, const QDate& today, bool clockTrusted) {
    QVector<ListRow> rows;
    QDate shown;
    bool earlier = false;
    for (const Session& session : sessions) {
        if (session.current) {
            rows.append(ListRow{RowKind::Session, sessionTitle(session, clockTrusted), session.id});
            rows.append(ListRow{RowKind::NewSet, tr("New set"), -1});
            continue;
        }
        if (session.count <= 0 && session.away > 0) {
            continue; // r7-library: back with its USB stick
        }
        if (!clockTrusted) {
            // No clock to trust: earlier sets by order, under one label.
            if (!earlier) {
                rows.append(ListRow{RowKind::Header, tr("Earlier sets"), -1});
                earlier = true;
            }
        } else {
            // Earlier sets under the day they started.
            const QDate day = (session.first.isValid() ? session.first : session.created).date();
            if (!shown.isValid() || day != shown) {
                rows.append(ListRow{RowKind::Header, dayLabel(day, today), -1});
                shown = day;
            }
        }
        rows.append(ListRow{RowKind::Session, sessionTitle(session, clockTrusted), session.id});
    }
    return rows;
}

int modelRowFor(const QAbstractItemModel& model,
        int positionColumn,
        int position,
        int trackId,
        const std::function<int(int row)>& trackIdOf) {
    if (trackId < 0) {
        return -1;
    }
    const int rows = model.rowCount();
    if (positionColumn >= 0) {
        for (int row = 0; row < rows; ++row) {
            if (model.index(row, positionColumn).data().toInt() == position && trackIdOf(row) == trackId) {
                return row;
            }
        }
    }
    for (int row = 0; row < rows; ++row) {
        if (trackIdOf(row) == trackId) {
            return row;
        }
    }
    return -1;
}

} // namespace decklab::history
