// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabhistory.h"

#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>
#include <algorithm>
#include <initializer_list>

#include "library/dao/playlistdao.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/trackmodel.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "track/track.h"
#include "util/datetime.h"

namespace decklab::history {

namespace {

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
            std::function<bool()> startNewSet,
            std::function<int(TrackId)> deckOf)
            : m_collection(collection),
              m_startNewSet(std::move(startNewSet)),
              m_deckOf(std::move(deckOf)) {
        // Sets that start or go, and tracks taken out of a set, are rare:
        // read everything again once the event loop runs (never during
        // shutdown, when SetlogFeature tidies up the history).
        m_reload.setSingleShot(true);
        m_reload.setInterval(0);
        QObject::connect(&m_reload, &QTimer::timeout, &m_context, [] { Store::instance().reload(); });
        PlaylistDAO* playlists = dao();
        if (!playlists) {
            return;
        }
        QObject::connect(playlists,
                &PlaylistDAO::trackAdded,
                &m_context,
                [this](int playlistId, TrackId trackId, int position) {
                    trackAdded(playlistId, trackId, position);
                });
        QObject::connect(playlists, &PlaylistDAO::added, &m_context, [this](int playlistId) {
            if (isHistory(playlistId)) {
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
    }

    QVector<Session> sessions() override {
        QVector<Session> result;
        if (!m_collection) {
            return result;
        }
        QSqlQuery query(database());
        query.setForwardOnly(true);
        const QString sql = QStringLiteral(
                "SELECT Playlists.id, Playlists.name, Playlists.date_created, "
                "COUNT(PlaylistTracks.track_id), MIN(PlaylistTracks.pl_datetime_added), "
                "MAX(PlaylistTracks.pl_datetime_added) "
                "FROM Playlists LEFT JOIN PlaylistTracks ON PlaylistTracks.playlist_id = Playlists.id "
                "WHERE Playlists.hidden = %1 GROUP BY Playlists.id ORDER BY Playlists.id DESC")
                                    .arg(static_cast<int>(PlaylistDAO::PLHT_SET_LOG));
        if (!query.exec(sql)) {
            qWarning() << "DeckLab history: could not read the sets" << query.lastError().text();
            return result;
        }
        m_history.clear();
        while (query.next()) {
            Session session;
            session.id = query.value(0).toInt();
            session.name = query.value(1).toString();
            session.created = localFromUtc(query.value(2));
            session.count = query.value(3).toInt();
            session.first = localFromUtc(query.value(4));
            session.last = localFromUtc(query.value(5));
            m_history.insert(session.id);
            result.append(session);
        }
        return result;
    }

    QVector<Entry> entries(int sessionId) override {
        return read(sessionId, -1);
    }

    void playedLocations(int currentId, QSet<QString>* session, QSet<QString>* earlier) override {
        if (!m_collection) {
            return;
        }
        QSqlQuery query(database());
        query.setForwardOnly(true);
        const QString sql = QStringLiteral(
                "SELECT PlaylistTracks.playlist_id, track_locations.location FROM PlaylistTracks "
                "INNER JOIN Playlists ON Playlists.id = PlaylistTracks.playlist_id "
                "INNER JOIN library ON library.id = PlaylistTracks.track_id "
                "INNER JOIN track_locations ON track_locations.id = library.location "
                "WHERE Playlists.hidden = %1")
                                    .arg(static_cast<int>(PlaylistDAO::PLHT_SET_LOG));
        if (!query.exec(sql)) {
            qWarning() << "DeckLab history: could not read played tracks" << query.lastError().text();
            return;
        }
        while (query.next()) {
            const QString location = QDir::fromNativeSeparators(query.value(1).toString());
            (query.value(0).toInt() == currentId ? session : earlier)->insert(location);
        }
    }

    bool startNewSet() override {
        return m_startNewSet && m_startNewSet();
    }

  private:
    PlaylistDAO* dao() const {
        return m_collection && m_collection->internalCollection()
                ? &m_collection->internalCollection()->getPlaylistDAO()
                : nullptr;
    }
    QSqlDatabase database() const {
        return m_collection->internalCollection()->database();
    }
    bool isHistory(int playlistId) {
        if (m_history.contains(playlistId)) {
            return true;
        }
        if (m_other.contains(playlistId) || playlistId == kInvalidPlaylistId) {
            return false;
        }
        PlaylistDAO* playlists = dao();
        const bool history = playlists && playlists->getHiddenType(playlistId) == PlaylistDAO::PLHT_SET_LOG;
        (history ? m_history : m_other).insert(playlistId);
        return history;
    }
    // Tracks of a set in play order (position < 0), or the one at position.
    QVector<Entry> read(int sessionId, int position) {
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
                "WHERE PlaylistTracks.playlist_id = :id%1 ORDER BY PlaylistTracks.position")
                        .arg(position >= 0 ? QStringLiteral(" AND PlaylistTracks.position = :position")
                                           : QString()));
        query.bindValue(QStringLiteral(":id"), sessionId);
        if (position >= 0) {
            query.bindValue(QStringLiteral(":position"), position);
        }
        if (!query.exec()) {
            qWarning() << "DeckLab history: could not read a set" << query.lastError().text();
            return result;
        }
        while (query.next()) {
            Entry entry;
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
            entry.position = position;
            entry.trackId = trackId.toVariant().toInt();
            entry.played = QDateTime::currentDateTime();
        }
        entry.deck = m_deckOf ? m_deckOf(trackId) : audibleDeckOf(trackId);
        Store::instance().addPlayed(playlistId, entry);
    }

    QPointer<TrackCollectionManager> m_collection;
    std::function<bool()> m_startNewSet;
    std::function<int(TrackId)> m_deckOf;
    QSet<int> m_history; // history playlists seen
    QSet<int> m_other;   // playlists known not to be history
    QObject m_context;
    QTimer m_reload;
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
    m_currentId = -1;
    m_sessions.clear();
    m_session.clear();
    m_earlier.clear();
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
}

void Store::read(QVector<Session>* sessions, QSet<QString>* session, QSet<QString>* earlier) {
    *sessions = m_backend->sessions();
    m_currentId = sessions->isEmpty() ? -1 : sessions->first().id;
    for (auto& each : *sessions) {
        each.current = each.id == m_currentId;
    }
    session->clear();
    earlier->clear();
    m_backend->playedLocations(m_currentId, session, earlier);
    session->remove(QString());
    earlier->remove(QString());
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
    read(&m_sessions, &m_session, &m_earlier);
    m_entries.clear();
    m_loaded = true;
    Change change;
    change.history = true;
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

QVector<Entry> Store::entries(int sessionId) {
    ensureLoaded();
    if (!m_backend || sessionId < 0) {
        return {};
    }
    auto it = m_entries.find(sessionId);
    if (it == m_entries.end()) {
        QVector<Entry> rows = m_backend->entries(sessionId);
        for (auto& entry : rows) {
            entry.deck = m_decks.value(deckKey(sessionId, entry.position), entry.deck);
        }
        it = m_entries.insert(sessionId, rows);
    }
    return *it;
}

int Store::currentCount() const {
    const Session* current = session(m_currentId);
    return current ? current->count : 0;
}

void Store::addPlayed(int sessionId, const Entry& entry) {
    if (!m_backend) {
        return;
    }
    if (entry.deck > 0) {
        m_decks.insert(deckKey(sessionId, entry.position), entry.deck);
    }
    if (!m_loaded || sessionId != m_currentId) {
        // Not read yet, a set started elsewhere, or an earlier set edited:
        // read it all again (rare).
        reload();
        return;
    }
    Change change;
    change.history = true;
    if (!entry.location.isEmpty() && !m_session.contains(entry.location)) {
        m_session.insert(entry.location);
        change.locations.insert(entry.location);
    }
    for (auto& each : m_sessions) {
        if (each.id == sessionId) {
            ++each.count;
            each.last = entry.played;
            if (!each.first.isValid()) {
                each.first = entry.played;
            }
            break;
        }
    }
    auto it = m_entries.find(sessionId);
    if (it != m_entries.end() &&
            std::none_of(it->cbegin(), it->cend(), [&entry](const Entry& known) {
                return known.position == entry.position;
            })) {
        it->append(entry);
    }
    notify(change);
}

bool Store::startNewSet() {
    ensureLoaded();
    if (!m_backend || currentCount() == 0 || !m_backend->startNewSet()) {
        return false;
    }
    reload();
    return true;
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

std::unique_ptr<Backend> makeMixxxBackend(TrackCollectionManager* collection,
        std::function<bool()> startNewSet,
        std::function<int(TrackId)> deckOf) {
    return std::make_unique<MixxxBackend>(collection, std::move(startNewSet), std::move(deckOf));
}

QString clockText(const QDateTime& time) {
    return time.isValid() ? time.toString(QStringLiteral("HH:mm")) : QString();
}

QString durationText(qint64 seconds) {
    if (seconds < 60) {
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

QString sessionTitle(const Session& session) {
    if (session.current) {
        return tr("This set");
    }
    if (session.first.isValid() && session.last.isValid() && clockText(session.first) != clockText(session.last)) {
        return QString(clockText(session.first) + QStringLiteral(" – ") + clockText(session.last));
    }
    return clockText(session.first.isValid() ? session.first : session.created);
}

QString sessionDetail(const Session& session) {
    if (session.count <= 0) {
        return tr("Nothing played yet");
    }
    if (session.current) {
        return session.first.isValid() ? tr("Since %1 · %2").arg(clockText(session.first), countText(session.count))
                                       : countText(session.count);
    }
    const QString length = session.first.isValid() && session.last.isValid()
            ? durationText(session.first.secsTo(session.last))
            : QString();
    return length.isEmpty() ? countText(session.count) : QString(countText(session.count) + QStringLiteral(" · ") + length);
}

QVector<ListRow> sessionRows(const QVector<Session>& sessions, const QDate& today) {
    QVector<ListRow> rows;
    QDate shown;
    for (const Session& session : sessions) {
        if (!session.current) {
            // Earlier sets under the day they started.
            const QDate day = (session.first.isValid() ? session.first : session.created).date();
            if (!shown.isValid() || day != shown) {
                rows.append(ListRow{RowKind::Header, dayLabel(day, today), -1});
                shown = day;
            }
        }
        rows.append(ListRow{RowKind::Session, sessionTitle(session), session.id});
    }
    return rows;
}

int modelRowFor(const QAbstractItemModel& model,
        int positionColumn,
        int position,
        const QString& location,
        const std::function<QString(int row)>& locationOf) {
    const int rows = model.rowCount();
    if (positionColumn >= 0) {
        for (int row = 0; row < rows; ++row) {
            if (model.index(row, positionColumn).data().toInt() == position && locationOf(row) == location) {
                return row;
            }
        }
    }
    for (int row = 0; row < rows; ++row) {
        if (locationOf(row) == location) {
            return row;
        }
    }
    return -1;
}

} // namespace decklab::history
