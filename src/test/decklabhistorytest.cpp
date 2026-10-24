// SPDX-License-Identifier: GPL-2.0-or-later
// Deck Lab round 5: track history and "played" marks (widget/decklabhistory.h,
// widget/decklabhistoryview.h). Generated data only: invented titles and
// files under a temporary folder that are never read.
#include <gtest/gtest.h>

#include <cmath>

#include <QAbstractItemDelegate>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QListWidget>
#include <QLocale>
#include <QObject>
#include <QPainter>
#include <QProxyStyle>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardItemModel>
#include <QStyleOptionViewItem>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>

#include "library/basetrackcache.h"
#include "library/basetracktablemodel.h"
#include "library/dao/playlistdao.h"
#include "library/dao/trackdao.h"
#include "library/dao/trackschema.h"
#include "library/trackcollection.h"
#include "library/playlisttablemodel.h"
#include "test/librarytest.h"
#include "widget/decklabbrowse.h"
#include "widget/decklabbrowsemix.h"
#include "widget/decklabhistory.h"
#include "widget/decklabhistoryview.h"
#include "widget/wtracktableview.h"

using namespace decklab::history;

namespace {

QDateTime at(int year, int month, int day, int hour, int minute) {
    return QDateTime(QDate(year, month, day), QTime(hour, minute));
}

// Painted colours: fully covered pixels, within antialiasing rounding.
bool near(const QColor& painted, const QColor& wanted) {
    return painted.alpha() >= 240 && std::abs(painted.red() - wanted.red()) <= 8 &&
            std::abs(painted.green() - wanted.green()) <= 8 && std::abs(painted.blue() - wanted.blue()) <= 8;
}

// A history with no database: what a backend reports, set by the test.
class GeneratedHistory final : public Backend {
  public:
    QVector<Playlist> lists; // newest first
    QHash<int, QVector<Entry>> tracks;
    QVector<int> starts;
    QVector<Playlist> playlists() override {
        return lists;
    }
    QVector<Entry> entries(int playlistId) override {
        return tracks.value(playlistId);
    }
    QVector<PlayedRow> playedRows() override {
        QVector<PlayedRow> rows;
        for (auto it = tracks.cbegin(); it != tracks.cend(); ++it) {
            for (const Entry& entry : it.value()) {
                rows.append(PlayedRow{it.key(), entry.trackId, entry.location});
            }
        }
        return rows;
    }
    int startNewSet() override {
        return -1;
    }
    QVector<int> setStarts() override {
        return starts;
    }
    void saveSetStarts(const QVector<int>& value) override {
        starts = value;
    }
};

// Records what a delegate hands the style to paint (the real paint path).
class RecordingStyle final : public QProxyStyle {
  public:
    QStyleOptionViewItem last;
    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter, const QWidget* widget) const override {
        if (element == CE_ItemViewItem) {
            if (const auto* item = qstyleoption_cast<const QStyleOptionViewItem*>(option)) {
                const_cast<RecordingStyle*>(this)->last = *item;
            }
        }
        QProxyStyle::drawControl(element, option, painter, widget);
    }
};

class DeckLabHistoryDbTest : public LibraryTest {
  protected:
    void TearDown() override {
        Store::instance().reset();
        BaseTrackTableModel::setApplyPlayedTrackColor(BaseTrackTableModel::kApplyPlayedTrackColorDefault);
    }
    // A library row for a generated file (never read).
    TrackId addTrack(const QString& title, double bpm, mixxx::track::io::key::ChromaticKey key) {
        const QString location = QDir(m_folder.path()).filePath(title + QStringLiteral(".wav"));
        QSqlQuery query(internalCollection()->database());
        query.prepare(QStringLiteral(
                "INSERT INTO track_locations (location, filename, directory, filesize, fs_deleted, "
                "needs_verification) VALUES (:location, :filename, :directory, 1000, 0, 0)"));
        query.bindValue(QStringLiteral(":location"), location);
        query.bindValue(QStringLiteral(":filename"), QFileInfo(location).fileName());
        query.bindValue(QStringLiteral(":directory"), QFileInfo(location).path());
        EXPECT_TRUE(query.exec()) << query.lastError().text().toStdString();
        const QVariant locationId = query.lastInsertId();
        query.prepare(QStringLiteral(
                "INSERT INTO library (location, title, artist, bpm, key_id) "
                "VALUES (:location, :title, 'Deck Lab Fixture', :bpm, :key)"));
        query.bindValue(QStringLiteral(":location"), locationId);
        query.bindValue(QStringLiteral(":title"), title);
        query.bindValue(QStringLiteral(":bpm"), bpm);
        query.bindValue(QStringLiteral(":key"), static_cast<int>(key));
        EXPECT_TRUE(query.exec()) << query.lastError().text().toStdString();
        m_locations.insert(title, location);
        return TrackId(query.lastInsertId());
    }
    QString location(const QString& title) const {
        return m_locations.value(title);
    }
    QString folder() const {
        return m_folder.path();
    }
    PlaylistDAO& playlists() {
        return internalCollection()->getPlaylistDAO();
    }
    int history(const QString& name) {
        return playlists().createPlaylist(name, PlaylistDAO::PLHT_SET_LOG);
    }
    std::unique_ptr<Backend> backend(UserSettingsPointer config = UserSettingsPointer()) {
        return makeMixxxBackend(
                trackCollectionManager(),
                std::move(config),
                [this] {
                    // What SetlogFeature::slotGetNewPlaylist does.
                    return history(QStringLiteral("2026-10-23 #%1").arg(++m_newSets)) != kInvalidPlaylistId;
                },
                [](TrackId) { return 2; });
    }
    int newSets() const {
        return m_newSets;
    }
    // The library's track cache, as MixxxLibraryFeature sets it up in the
    // application: track tables read titles and Mixxx's played flag from it.
    void connectLibraryCache() {
        const QStringList columns{LIBRARYTABLE_ID, LIBRARYTABLE_PLAYED, LIBRARYTABLE_TIMESPLAYED,
                LIBRARYTABLE_LAST_PLAYED_AT, LIBRARYTABLE_ALBUMARTIST, LIBRARYTABLE_ALBUM, LIBRARYTABLE_ARTIST,
                LIBRARYTABLE_TITLE, LIBRARYTABLE_YEAR, LIBRARYTABLE_RATING, LIBRARYTABLE_GENRE, LIBRARYTABLE_COMPOSER,
                LIBRARYTABLE_GROUPING, LIBRARYTABLE_TRACKNUMBER, LIBRARYTABLE_KEY, LIBRARYTABLE_KEY_ID, LIBRARYTABLE_BPM,
                LIBRARYTABLE_BPM_LOCK, LIBRARYTABLE_BEATS_VERSION, LIBRARYTABLE_DURATION, LIBRARYTABLE_BITRATE,
                LIBRARYTABLE_REPLAYGAIN, LIBRARYTABLE_FILETYPE, LIBRARYTABLE_DATETIMEADDED, TRACKLOCATIONSTABLE_LOCATION,
                TRACKLOCATIONSTABLE_DIRECTORY, TRACKLOCATIONSTABLE_FSDELETED, LIBRARYTABLE_COMMENT,
                LIBRARYTABLE_MIXXXDELETED, LIBRARYTABLE_COLOR, LIBRARYTABLE_COVERART_SOURCE, LIBRARYTABLE_COVERART_TYPE,
                LIBRARYTABLE_COVERART_LOCATION, LIBRARYTABLE_COVERART_COLOR, LIBRARYTABLE_COVERART_DIGEST,
                LIBRARYTABLE_COVERART_HASH, LIBRARYTABLE_WAVESUMMARYHEX};
        QStringList qualified;
        for (const QString& column : columns) {
            qualified << QString(mixxx::trackschema::tableForColumn(column) + QLatin1Char('.') + column);
        }
        QSqlQuery query(internalCollection()->database());
        EXPECT_TRUE(query.exec(QStringLiteral("CREATE TEMPORARY VIEW IF NOT EXISTS library_cache_view AS SELECT %1 "
                                              "FROM library INNER JOIN track_locations ON library.location = track_locations.id")
                                       .arg(qualified.join(QLatin1Char(',')))))
                << query.lastError().text().toStdString();
        m_cache = QSharedPointer<BaseTrackCache>(new BaseTrackCache(internalCollection(),
                QStringLiteral("library_cache_view"), LIBRARYTABLE_ID, columns,
                QStringList{LIBRARYTABLE_ARTIST, LIBRARYTABLE_TITLE}, true));
        internalCollection()->connectTrackSource(m_cache);
    }

  private:
    QSharedPointer<BaseTrackCache> m_cache;
    QTemporaryDir m_folder;
    QHash<QString, QString> m_locations;
    int m_newSets = 0;
};

} // namespace

// The sets and their tracks are Mixxx's history playlists: newest first, the
// newest is the current set (the first run starts it), older playlists are
// one set each, every track in play order; other playlists are not history.
TEST_F(DeckLabHistoryDbTest, SetsAndPlayOrderComeFromMixxxHistoryPlaylists) {
    using mixxx::track::io::key::A_MINOR;
    const TrackId first = addTrack(QStringLiteral("Generated Tone 122"), 122, A_MINOR);
    const TrackId second = addTrack(QStringLiteral("Generated Tone 124"), 124, mixxx::track::io::key::E_MINOR);
    const TrackId tonight = addTrack(QStringLiteral("Generated Tone 126"), 126, mixxx::track::io::key::C_MAJOR);
    const TrackId crated = addTrack(QStringLiteral("Generated Tone 128"), 128, A_MINOR);
    const int earlier = history(QStringLiteral("2026-10-18"));
    ASSERT_TRUE(playlists().appendTrackToPlaylist(first, earlier));
    ASSERT_TRUE(playlists().appendTrackToPlaylist(second, earlier));
    const int crate = playlists().createPlaylist(QStringLiteral("Generated playlist"), PlaylistDAO::PLHT_NOT_HIDDEN);
    ASSERT_TRUE(playlists().appendTrackToPlaylist(crated, crate));
    const int current = history(QStringLiteral("2026-10-23"));
    ASSERT_TRUE(playlists().appendTrackToPlaylist(tonight, current));

    Store& store = Store::instance();
    store.setBackend(backend());
    EXPECT_FALSE(store.loaded());
    EXPECT_EQ(Played::No, store.played(location(QStringLiteral("Generated Tone 126")))); // nothing read yet
    store.ensureLoaded();
    ASSERT_EQ(2, store.sessions().size());
    EXPECT_EQ(current, store.currentId());
    EXPECT_TRUE(store.sessions().at(0).current);
    EXPECT_FALSE(store.sessions().at(1).current);
    EXPECT_TRUE(store.sessions().at(1).legacy);
    EXPECT_EQ(1, store.sessions().at(0).count);
    EXPECT_EQ(2, store.sessions().at(1).count);
    EXPECT_EQ(2, store.sessions().at(0).ordinal);
    EXPECT_TRUE(store.sessions().at(1).first.isValid());
    EXPECT_EQ(1, store.currentCount());
    // One lookup per row: this set, an earlier set, or neither.
    EXPECT_EQ(Played::Session, store.played(location(QStringLiteral("Generated Tone 126"))));
    EXPECT_EQ(Played::Earlier, store.played(location(QStringLiteral("Generated Tone 122"))));
    EXPECT_EQ(Played::No, store.played(location(QStringLiteral("Generated Tone 128"))));
    const QVector<Entry> set = store.entries(earlier);
    ASSERT_EQ(2, set.size());
    EXPECT_EQ(earlier, set.at(0).playlistId);
    EXPECT_EQ(1, set.at(0).position);
    EXPECT_EQ(2, set.at(1).position);
    EXPECT_EQ(first.toVariant().toInt(), set.at(0).trackId);
    EXPECT_EQ(QStringLiteral("Generated Tone 122"), set.at(0).title);
    EXPECT_EQ(QStringLiteral("Deck Lab Fixture"), set.at(0).artist);
    EXPECT_DOUBLE_EQ(122, set.at(0).bpm);
    EXPECT_EQ(A_MINOR, set.at(0).key);
    EXPECT_EQ(location(QStringLiteral("Generated Tone 122")), set.at(0).location);
    EXPECT_TRUE(set.at(0).played.isValid());
    // Mixxx does not store the deck: unknown for sets this unit did not see.
    EXPECT_EQ(0, set.at(0).deck);
}

// Played marks follow the history live: PlaylistDAO::trackAdded (the setlog
// adding the track that became audible) marks it at once and tells listeners
// which row changed; "New set" turns this set's marks into earlier marks with
// one change and no second read of the history.
TEST_F(DeckLabHistoryDbTest, MarksFollowTheHistoryLiveAndNewSetMovesThemToEarlier) {
    const TrackId opener = addTrack(QStringLiteral("Generated Tone 120"), 120, mixxx::track::io::key::D_MINOR);
    const TrackId next = addTrack(QStringLiteral("Generated Tone 121"), 121, mixxx::track::io::key::D_MINOR);
    const int current = history(QStringLiteral("2026-10-23"));
    ASSERT_TRUE(playlists().appendTrackToPlaylist(opener, current));
    Store& store = Store::instance();
    store.setBackend(backend());
    QObject owner;
    QList<Change> heard;
    store.listen(&owner, [&heard](const Change& change) { heard.append(change); });
    store.ensureLoaded();
    ASSERT_EQ(1, heard.size());
    EXPECT_TRUE(heard.first().locations.contains(location(QStringLiteral("Generated Tone 120"))));
    heard.clear();
    EXPECT_EQ(Played::No, store.played(location(QStringLiteral("Generated Tone 121"))));

    // Deck 2 became the audible deck: Mixxx's setlog appends its track.
    ASSERT_TRUE(playlists().appendTrackToPlaylist(next, current));
    ASSERT_EQ(1, heard.size());
    EXPECT_EQ(QSet<QString>{location(QStringLiteral("Generated Tone 121"))}, heard.first().locations);
    EXPECT_TRUE(heard.first().history);
    EXPECT_FALSE(heard.first().reloaded);
    ASSERT_EQ(1, heard.first().added.size());
    EXPECT_EQ(next.toVariant().toInt(), heard.first().added.first().trackId);
    EXPECT_EQ(Played::Session, store.played(location(QStringLiteral("Generated Tone 121"))));
    EXPECT_EQ(2, store.currentCount());
    const QVector<Entry> set = store.entries(current);
    ASSERT_EQ(2, set.size());
    EXPECT_EQ(QStringLiteral("Generated Tone 121"), set.last().title);
    EXPECT_EQ(2, set.last().deck);
    EXPECT_EQ(0, set.first().deck);

    // Another playlist changing is not history.
    const int crate = playlists().createPlaylist(QStringLiteral("Generated playlist"), PlaylistDAO::PLHT_NOT_HIDDEN);
    heard.clear();
    ASSERT_TRUE(playlists().appendTrackToPlaylist(opener, crate));
    QCoreApplication::processEvents();
    EXPECT_TRUE(heard.isEmpty());

    // New set: one change, and nothing read again afterwards.
    EXPECT_TRUE(store.startNewSet());
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    ASSERT_EQ(1, heard.size());
    EXPECT_TRUE(heard.first().reloaded);
    EXPECT_TRUE(heard.first().locations.contains(location(QStringLiteral("Generated Tone 121"))));
    EXPECT_EQ(1, newSets());
    EXPECT_NE(current, store.currentId());
    EXPECT_EQ(2, store.sessions().size());
    EXPECT_EQ(0, store.currentCount());
    EXPECT_EQ(Played::Earlier, store.played(location(QStringLiteral("Generated Tone 120"))));
    EXPECT_EQ(Played::Earlier, store.played(location(QStringLiteral("Generated Tone 121"))));
    // The deck this unit saw stays; an empty set never starts another.
    EXPECT_EQ(2, store.entries(current).last().deck);
    EXPECT_FALSE(store.startNewSet());
    EXPECT_EQ(1, newSets());
}

// A set spans restarts: Mixxx starts a new history playlist on every launch
// (Restart to detect, Restart app, a crash, a power blip), and this set goes
// on until the DJ holds New set. The starts live in the settings.
TEST_F(DeckLabHistoryDbTest, ASetSpansRestartsUntilNewSet) {
    QTemporaryDir settings;
    ASSERT_TRUE(settings.isValid());
    UserSettingsPointer config(new UserSettings(QDir(settings.path()).filePath(QStringLiteral("generated.cfg"))));
    const TrackId older = addTrack(QStringLiteral("Generated Tone 110"), 110, mixxx::track::io::key::A_MINOR);
    const TrackId opener = addTrack(QStringLiteral("Generated Tone 112"), 112, mixxx::track::io::key::A_MINOR);
    const TrackId afterRestart = addTrack(QStringLiteral("Generated Tone 114"), 114, mixxx::track::io::key::A_MINOR);
    const TrackId closer = addTrack(QStringLiteral("Generated Tone 116"), 116, mixxx::track::io::key::A_MINOR);
    const int legacy = history(QStringLiteral("2026-10-20"));
    ASSERT_TRUE(playlists().appendTrackToPlaylist(older, legacy));
    const int run1 = history(QStringLiteral("2026-10-23"));
    ASSERT_TRUE(playlists().appendTrackToPlaylist(opener, run1));
    Store& store = Store::instance();
    store.setBackend(backend(config));
    store.ensureLoaded();
    EXPECT_EQ(run1, store.currentId());
    EXPECT_EQ(QString::number(run1), config->getValueString(ConfigKey(QStringLiteral("[DeckLab]"), QStringLiteral("HistorySetStarts"))));

    // The app restarts mid-set: a new store, and Mixxx's new history playlist.
    store.setBackend(nullptr);
    const int run2 = history(QStringLiteral("2026-10-23 #2"));
    store.setBackend(backend(config));
    store.ensureLoaded();
    EXPECT_EQ(run1, store.currentId());
    ASSERT_EQ(2, store.sessions().size());
    EXPECT_EQ((QVector<int>{run1, run2}), store.sessions().first().playlists);
    EXPECT_EQ(Played::Session, store.played(location(QStringLiteral("Generated Tone 112"))));
    EXPECT_EQ(Played::Earlier, store.played(location(QStringLiteral("Generated Tone 110"))));
    ASSERT_TRUE(playlists().appendTrackToPlaylist(afterRestart, run2));
    EXPECT_EQ(Played::Session, store.played(location(QStringLiteral("Generated Tone 114"))));
    EXPECT_EQ(2, store.currentCount());
    const QVector<Entry> tonight = store.entries(run1);
    ASSERT_EQ(2, tonight.size());
    EXPECT_EQ(run1, tonight.at(0).playlistId);
    EXPECT_EQ(run2, tonight.at(1).playlistId);
    EXPECT_EQ(2, tonight.at(1).deck);

    // New set: both runs become one earlier set.
    EXPECT_TRUE(store.startNewSet());
    const int run3 = store.currentId();
    EXPECT_GT(run3, run2);
    EXPECT_EQ(QStringLiteral("%1,%2").arg(run1).arg(run3),
            config->getValueString(ConfigKey(QStringLiteral("[DeckLab]"), QStringLiteral("HistorySetStarts"))));
    ASSERT_TRUE(playlists().appendTrackToPlaylist(closer, run3));
    EXPECT_EQ(Played::Earlier, store.played(location(QStringLiteral("Generated Tone 112"))));
    EXPECT_EQ(Played::Session, store.played(location(QStringLiteral("Generated Tone 116"))));

    // Another restart keeps the new set, and the last one stays whole.
    store.setBackend(nullptr);
    const int run4 = history(QStringLiteral("2026-10-24"));
    store.setBackend(backend(config));
    store.ensureLoaded();
    EXPECT_EQ(run3, store.currentId());
    ASSERT_EQ(3, store.sessions().size());
    EXPECT_EQ((QVector<int>{run3, run4}), store.sessions().at(0).playlists);
    EXPECT_EQ((QVector<int>{run1, run2}), store.sessions().at(1).playlists);
    EXPECT_EQ(2, store.sessions().at(1).count);
    EXPECT_FALSE(store.sessions().at(1).legacy);
    EXPECT_TRUE(store.sessions().at(2).legacy);
    EXPECT_EQ(Played::Session, store.played(location(QStringLiteral("Generated Tone 116"))));
    EXPECT_EQ(Played::Earlier, store.played(location(QStringLiteral("Generated Tone 114"))));
}

// A history track whose file moved (USB relink, Mixxx's relocate) keeps its
// mark at the new location and stays loadable: the set's table row is found
// by library id. A save that moves nothing does not read the history again.
TEST_F(DeckLabHistoryDbTest, MovedTracksKeepTheirMarksAndLoadByLibraryId) {
    const TrackId moved = addTrack(QStringLiteral("Generated Tone 140"), 140, mixxx::track::io::key::A_MINOR);
    const int current = history(QStringLiteral("2026-10-23"));
    ASSERT_TRUE(playlists().appendTrackToPlaylist(moved, current));
    Store& store = Store::instance();
    store.setBackend(backend());
    store.ensureLoaded();
    QObject owner;
    QList<Change> heard;
    store.listen(&owner, [&heard](const Change& change) { heard.append(change); });
    const QString before = location(QStringLiteral("Generated Tone 140"));
    const QString after = QDir(folder()).filePath(QStringLiteral("relinked/Generated Tone 140.wav"));
    QSqlQuery query(internalCollection()->database());
    query.prepare(QStringLiteral("UPDATE track_locations SET location = :after WHERE location = :before"));
    query.bindValue(QStringLiteral(":after"), after);
    query.bindValue(QStringLiteral(":before"), before);
    ASSERT_TRUE(query.exec()) << query.lastError().text().toStdString();
    // What TrackDAO::slotDatabaseTracksRelocated announces.
    emit internalCollection()->getTrackDAO().tracksChanged(QSet<TrackId>{moved});
    QTest::qWait(400);
    EXPECT_EQ(Played::Session, store.played(after));
    EXPECT_EQ(Played::No, store.played(before));
    ASSERT_EQ(1, store.entries(current).size());
    EXPECT_EQ(after, store.entries(current).first().location);
    ASSERT_FALSE(heard.isEmpty());
    EXPECT_TRUE(heard.last().locations.contains(after));
    heard.clear();
    emit internalCollection()->getTrackDAO().tracksChanged(QSet<TrackId>{moved});
    QTest::qWait(400);
    EXPECT_TRUE(heard.isEmpty());
    // The set's own Mixxx table: the row by position and library id.
    PlaylistTableModel table(nullptr, trackCollectionManager(), "mixxx.db.model.decklab.history.test");
    table.selectPlaylist(current);
    table.select();
    ASSERT_EQ(1, table.rowCount());
    const auto idOf = [&table](int row) { return table.getTrackId(table.index(row, 0)).toVariant().toInt(); };
    const int position = table.columnIndexFromSortColumnId(TrackModel::SortColumnId::Position);
    EXPECT_EQ(0, modelRowFor(table, position, 1, moved.toVariant().toInt(), idOf));
    EXPECT_EQ(-1, modelRowFor(table, position, 1, moved.toVariant().toInt() + 99, idOf));
}

// Mixxx's track tables through Deck Lab's real cell delegate: the history
// decides the look, not Mixxx's per-start played colour, highlighted rows
// included; the title cell carries the mark; dim off keeps the text bright.
TEST_F(DeckLabHistoryDbTest, TrackTableCellsTakeTheHistoryLookNotMixxxPlayedColour) {
    const TrackId tonight = addTrack(QStringLiteral("Generated Tone 150"), 150, mixxx::track::io::key::A_MINOR);
    const TrackId lastWeek = addTrack(QStringLiteral("Generated Tone 151"), 151, mixxx::track::io::key::A_MINOR);
    const TrackId never = addTrack(QStringLiteral("Generated Tone 152"), 152, mixxx::track::io::key::A_MINOR);
    QSqlQuery query(internalCollection()->database());
    ASSERT_TRUE(query.exec(QStringLiteral("UPDATE library SET played = 1"))); // Mixxx: all played this run
    const int legacy = history(QStringLiteral("2026-10-16"));
    ASSERT_TRUE(playlists().appendTrackToPlaylist(lastWeek, legacy));
    const int current = history(QStringLiteral("2026-10-23"));
    ASSERT_TRUE(playlists().appendTrackToPlaylist(tonight, current));
    const int crate = playlists().createPlaylist(QStringLiteral("Generated playlist"), PlaylistDAO::PLHT_NOT_HIDDEN);
    for (const TrackId& id : {tonight, lastWeek, never}) {
        ASSERT_TRUE(playlists().appendTrackToPlaylist(id, crate));
    }
    Store& store = Store::instance();
    store.setBackend(backend());
    store.ensureLoaded();
    BaseTrackTableModel::setApplyPlayedTrackColor(true);
    connectLibraryCache();
    PlaylistTableModel model(nullptr, trackCollectionManager(), "mixxx.db.model.decklab.history.cells");
    model.selectPlaylist(crate);
    model.select();
    ASSERT_EQ(3, model.rowCount());
    const int title = model.columnIndexFromSortColumnId(TrackModel::SortColumnId::Title);
    const int artist = model.columnIndexFromSortColumnId(TrackModel::SortColumnId::Artist);
    const auto rowOf = [&model, title](const QString& text) {
        for (int row = 0; row < model.rowCount(); ++row) {
            if (model.index(row, title).data().toString() == text) return row;
        }
        return -1;
    };
    const QColor ink(0xf2, 0xf4, 0xf7);
    RecordingStyle style; // outlives the view
    QTableView view;
    view.setModel(&model);
    QPalette palette = view.palette();
    palette.setColor(QPalette::Text, ink);
    palette.setColor(QPalette::HighlightedText, ink);
    view.setPalette(palette);
    view.setStyle(&style);
    QAbstractItemDelegate* titleCell = makeTrackCellDelegate(&view, false);
    QAbstractItemDelegate* artistCell = makeTrackCellDelegate(&view, true);
    const auto paint = [&](QAbstractItemDelegate* delegate, const QString& text, int column, bool selected) {
        const int row = rowOf(text);
        EXPECT_GE(row, 0) << text.toStdString();
        QStyleOptionViewItem option;
        option.initFrom(&view);
        option.widget = &view;
        option.rect = QRect(0, 0, 320, 44);
        option.palette = view.palette();
        option.font = view.font();
        option.state = QStyle::State_Enabled | (selected ? QStyle::State_Selected : QStyle::State_None);
        QImage image(320, 44, QImage::Format_ARGB32_Premultiplied);
        QPainter painter(&image);
        delegate->paint(&painter, option, model.index(row, column));
        return style.last;
    };
    // Mixxx says played; the precondition the rest relies on.
    ASSERT_EQ(QColor(WTrackTableView::kDefaultTrackPlayedColor), model.index(rowOf(QStringLiteral("Generated Tone 152")), title).data(Qt::ForegroundRole).value<QColor>());
    const QStyleOptionViewItem neverCell = paint(titleCell, QStringLiteral("Generated Tone 152"), title, false);
    EXPECT_EQ(ink, neverCell.palette.color(QPalette::Text));
    EXPECT_FALSE(neverCell.features & QStyleOptionViewItem::HasDecoration);
    const QStyleOptionViewItem earlierCell = paint(titleCell, QStringLiteral("Generated Tone 151"), title, false);
    EXPECT_EQ(ink, earlierCell.palette.color(QPalette::Text));
    ASSERT_TRUE(earlierCell.features & QStyleOptionViewItem::HasDecoration);
    EXPECT_EQ(playedIcon(Played::Earlier).cacheKey(), earlierCell.icon.cacheKey());
    EXPECT_EQ(QStyleOptionViewItem::Right, earlierCell.decorationPosition);
    const QStyleOptionViewItem sessionCell = paint(titleCell, QStringLiteral("Generated Tone 150"), title, false);
    EXPECT_EQ(decklab::colors::muted, sessionCell.palette.color(QPalette::Text));
    EXPECT_EQ(playedIcon(Played::Session).cacheKey(), sessionCell.icon.cacheKey());
    // Highlighted: white, never blended with Mixxx's played colour.
    const QStyleOptionViewItem selectedCell = paint(titleCell, QStringLiteral("Generated Tone 150"), title, true);
    EXPECT_EQ(ink, selectedCell.palette.color(QPalette::Normal, QPalette::HighlightedText));
    EXPECT_TRUE(selectedCell.features & QStyleOptionViewItem::HasDecoration);
    // Secondary cells stay muted and carry no mark.
    const QStyleOptionViewItem artistOf = paint(artistCell, QStringLiteral("Generated Tone 150"), artist, false);
    EXPECT_EQ(decklab::colors::muted, artistOf.palette.color(QPalette::Text));
    EXPECT_FALSE(artistOf.features & QStyleOptionViewItem::HasDecoration);
    // "Dim played tracks" off: the mark stays, the text does not dim.
    store.setDimPlayed(false);
    const QStyleOptionViewItem undimmed = paint(titleCell, QStringLiteral("Generated Tone 150"), title, false);
    EXPECT_EQ(ink, undimmed.palette.color(QPalette::Text));
    EXPECT_TRUE(undimmed.features & QStyleOptionViewItem::HasDecoration);
    store.setDimPlayed(true);
}

// The sets list: this set and "New set" first, then earlier sets under the
// day they began, each saying what it holds.
TEST(DeckLabBrowseTest, HistoryGroupsEarlierSetsUnderTheirDayAndSaysWhatEachHolds) {
    const QDate today(2026, 10, 23);
    Session current;
    current.id = 9;
    current.current = true;
    current.count = 14;
    current.first = at(2026, 10, 23, 21, 4);
    current.last = at(2026, 10, 23, 22, 31);
    Session lastNight;
    lastNight.id = 8;
    lastNight.count = 37;
    lastNight.first = at(2026, 10, 22, 21, 0);
    lastNight.last = at(2026, 10, 23, 1, 30);
    Session warmUp;
    warmUp.id = 7;
    warmUp.count = 1;
    warmUp.first = at(2026, 10, 22, 18, 0);
    warmUp.last = warmUp.first;
    Session longAgo;
    longAgo.id = 3;
    longAgo.count = 50;
    longAgo.first = at(2025, 12, 31, 22, 0);
    longAgo.last = at(2026, 1, 1, 3, 15);
    const auto rows = sessionRows({current, lastNight, warmUp, longAgo}, today);
    ASSERT_EQ(7, rows.size());
    EXPECT_EQ(RowKind::Session, rows[0].kind);
    EXPECT_EQ(9, rows[0].sessionId);
    EXPECT_EQ(QStringLiteral("This set"), rows[0].text);
    EXPECT_EQ(RowKind::NewSet, rows[1].kind);
    EXPECT_EQ(RowKind::Header, rows[2].kind);
    EXPECT_EQ(QStringLiteral("Yesterday"), rows[2].text);
    EXPECT_EQ(8, rows[3].sessionId);
    EXPECT_EQ(7, rows[4].sessionId);
    EXPECT_EQ(RowKind::Header, rows[5].kind);
    EXPECT_EQ(QLocale().toString(QDate(2025, 12, 31), QStringLiteral("d MMM yyyy")), rows[5].text);
    EXPECT_EQ(3, rows[6].sessionId);
    EXPECT_EQ(QStringLiteral("21:00 – 01:30"), sessionTitle(lastNight));
    EXPECT_EQ(QStringLiteral("18:00"), sessionTitle(warmUp));
    EXPECT_EQ(QStringLiteral("Since 21:04 · 14 tracks"), sessionDetail(current));
    EXPECT_EQ(QStringLiteral("37 tracks · 4 h 30 min"), sessionDetail(lastNight));
    EXPECT_EQ(QStringLiteral("1 track"), sessionDetail(warmUp));
    Session fresh;
    fresh.current = true;
    EXPECT_EQ(QStringLiteral("Nothing played yet"), sessionDetail(fresh));
    EXPECT_EQ(QStringLiteral("Today"), dayLabel(today, today));
    EXPECT_EQ(QLocale().standaloneDayName(QDate(2026, 10, 20).dayOfWeek(), QLocale::LongFormat),
            dayLabel(QDate(2026, 10, 20), today));
    EXPECT_EQ(QLocale().toString(QDate(2026, 10, 11), QStringLiteral("ddd d MMM")), dayLabel(QDate(2026, 10, 11), today));
    EXPECT_EQ(QString(), durationText(59));
    EXPECT_EQ(QStringLiteral("50 min"), durationText(3000));
    EXPECT_EQ(QStringLiteral("2 h 05 min"), durationText(7500));
    EXPECT_EQ(QString(), durationText(25 * 3600)); // a clock jump, not a set
    EXPECT_EQ(QStringLiteral("21:04"), clockText(current.first));
}

// A Pi 4 has no real-time clock: until it syncs, the history shows order and
// offsets instead of clock times and days.
TEST(DeckLabBrowseTest, HistoryWithoutATrustedClockShowsOrderNotClockTimes) {
    Session current;
    current.id = 9;
    current.current = true;
    current.count = 2;
    current.ordinal = 3;
    current.first = at(2026, 10, 20, 14, 10);
    current.last = at(2026, 10, 23, 1, 30); // the clock jumped mid-set
    Session earlier;
    earlier.id = 8;
    earlier.count = 12;
    earlier.ordinal = 2;
    earlier.first = at(2026, 10, 19, 21, 0);
    earlier.last = at(2026, 10, 19, 23, 0);
    Session oldest = earlier;
    oldest.id = 4;
    oldest.ordinal = 1;
    const auto rows = sessionRows({current, earlier, oldest}, QDate(2026, 10, 23), false);
    ASSERT_EQ(5, rows.size());
    EXPECT_EQ(RowKind::Header, rows[2].kind);
    EXPECT_EQ(QStringLiteral("Earlier sets"), rows[2].text);
    EXPECT_EQ(QStringLiteral("Set 2"), rows[3].text);
    EXPECT_EQ(QStringLiteral("Set 1"), rows[4].text);
    EXPECT_EQ(QStringLiteral("2 tracks"), sessionDetail(current, false)); // no "Since", no 59 h
    EXPECT_EQ(QStringLiteral("12 tracks · 2 h 00 min"), sessionDetail(earlier, false));
    QVector<Entry> forward;
    for (int n = 0; n < 3; ++n) {
        Entry entry;
        entry.position = n + 1;
        entry.played = at(2026, 10, 20, 14, 10).addSecs(n * 420);
        entry.title = QStringLiteral("Generated Tone %1").arg(120 + n);
        forward.append(entry);
    }
    QListWidget list;
    fillTracks(&list, forward, true, -1, false);
    EXPECT_EQ(QStringLiteral("+0:00"), list.item(0)->data(TimeRole).toString());
    EXPECT_EQ(QStringLiteral("+0:14"), list.item(2)->data(TimeRole).toString());
    QVector<Entry> jumped = forward;
    jumped[2].played = at(2026, 10, 20, 12, 0); // backwards: the clock was set
    fillTracks(&list, jumped, true, -1, false);
    EXPECT_EQ(QStringLiteral("#1"), list.item(0)->data(TimeRole).toString());
    EXPECT_EQ(QStringLiteral("#3"), list.item(2)->data(TimeRole).toString());
    // The store: a time later than now means the clock went back since.
    auto generated = std::make_unique<GeneratedHistory>();
    Playlist future;
    future.id = 5;
    future.created = QDateTime::currentDateTime().addDays(2);
    generated->lists = {future};
    Store& store = Store::instance();
    store.setBackend(std::move(generated));
    store.ensureLoaded();
    EXPECT_FALSE(store.clockTrusted());
    store.setClockState(ClockState::Synced);
    EXPECT_TRUE(store.clockTrusted());
    store.setClockState(ClockState::Unsynced);
    EXPECT_FALSE(store.clockTrusted());
    store.reset();
}

// LOAD and Preview read the set's own Mixxx table: the row with the same
// position and library id, else the same id elsewhere, never another track.
TEST(DeckLabBrowseTest, HistoryRowFindsItsTrackInTheSetsTableAndNeverAnother) {
    QStandardItemModel model(0, 2);
    const QList<int> ids{11, 12, 11};
    for (int row = 0; row < ids.size(); ++row) {
        model.appendRow({new QStandardItem(QString::number(row + 1)), new QStandardItem(QString::number(ids[row]))});
    }
    const auto idOf = [&model](int row) { return model.index(row, 1).data().toInt(); };
    EXPECT_EQ(2, modelRowFor(model, 0, 3, 11, idOf)); // played twice: the second time
    EXPECT_EQ(1, modelRowFor(model, 0, 2, 12, idOf));
    EXPECT_EQ(0, modelRowFor(model, 0, 2, 11, idOf)); // position moved: same track
    EXPECT_EQ(-1, modelRowFor(model, 0, 1, 13, idOf));
    EXPECT_EQ(-1, modelRowFor(model, 0, 1, -1, idOf));
    EXPECT_EQ(0, modelRowFor(model, -1, 3, 11, idOf)); // no position column
}

// The rows: a set's tracks in play order with time, deck, BPM and key; the
// current set opens on its latest track, an earlier one on its first; date
// headers and empty rows are never highlighted; a track joins at the end.
TEST(DeckLabBrowseTest, HistoryRowsKeepPlayOrderAndOpenOnTheLatestTrack) {
    QVector<Entry> entries;
    for (int n = 1; n <= 3; ++n) {
        Entry entry;
        entry.playlistId = n < 3 ? 5 : 6; // a restart between tracks 2 and 3
        entry.position = n < 3 ? n : 1;
        entry.played = at(2026, 10, 23, 21, n * 7);
        entry.deck = n == 2 ? 0 : n;
        entry.trackId = 100 + n;
        entry.title = QStringLiteral("Generated Tone %1").arg(119 + n);
        entry.artist = QStringLiteral("Deck Lab Fixture");
        entry.bpm = 119 + n;
        entry.key = mixxx::track::io::key::A_MINOR;
        entry.location = QStringLiteral("/generated/%1.wav").arg(n);
        entries.append(entry);
    }
    QListWidget list;
    EXPECT_EQ(2, fillTracks(&list, entries, true, -1));
    ASSERT_EQ(3, list.count());
    EXPECT_EQ(QStringLiteral("Generated Tone 120"), list.item(0)->text());
    EXPECT_EQ(static_cast<int>(RowKind::Track), list.item(0)->data(RowRole).toInt());
    EXPECT_EQ(QStringLiteral("21:07"), list.item(0)->data(TimeRole).toString());
    EXPECT_EQ(1, list.item(0)->data(DeckRole).toInt());
    EXPECT_EQ(0, list.item(1)->data(DeckRole).toInt());
    EXPECT_EQ(6, list.item(2)->data(PlaylistRole).toInt());
    EXPECT_EQ(103, list.item(2)->data(TrackIdRole).toInt());
    EXPECT_DOUBLE_EQ(122, list.item(2)->data(BpmRole).toDouble());
    EXPECT_FALSE(list.item(0)->data(KeyRole).toString().isEmpty());
    EXPECT_EQ(QStringLiteral("/generated/3.wav"), list.item(2)->data(LocationRole).toString());
    EXPECT_EQ(0, fillTracks(&list, entries, false, -1));
    EXPECT_EQ(1, fillTracks(&list, entries, false, 2, true, 5));
    EXPECT_EQ(2, fillTracks(&list, entries, false, 1, true, 6)); // position 1 of the second run
    Entry joined = entries.last();
    joined.position = 2;
    joined.title = QStringLiteral("Generated Tone 123");
    entries.append(joined);
    QListWidgetItem* added = appendTrack(&list, entries, false, true);
    ASSERT_NE(nullptr, added);
    EXPECT_EQ(4, list.count());
    EXPECT_EQ(QStringLiteral("Generated Tone 123"), list.item(3)->text());
    EXPECT_EQ(-1, fillTracks(&list, {}, true, -1));
    ASSERT_EQ(1, list.count());
    EXPECT_FALSE(list.item(0)->flags() & Qt::ItemIsEnabled);

    Session current;
    current.id = 5;
    current.current = true;
    current.count = 3;
    current.first = entries.first().played;
    current.last = entries.last().played;
    Session earlier;
    earlier.id = 4;
    earlier.count = 12;
    earlier.first = at(2026, 10, 22, 21, 0);
    earlier.last = at(2026, 10, 22, 23, 0);
    EXPECT_EQ(0, fillSessions(&list, {current, earlier}, QDate(2026, 10, 23), -1));
    ASSERT_EQ(4, list.count());
    EXPECT_EQ(QStringLiteral("3"), list.item(0)->data(CountRole).toString());
    EXPECT_TRUE(list.item(1)->flags() & Qt::ItemIsEnabled); // "New set": this set has tracks
    EXPECT_FALSE(list.item(2)->flags() & Qt::ItemIsEnabled); // "Yesterday"
    EXPECT_EQ(3, fillSessions(&list, {current, earlier}, QDate(2026, 10, 23), 4));
    EXPECT_EQ(QStringLiteral("12 tracks · 2 h 00 min"), list.item(3)->data(DetailRole).toString());
    current.count = 0;
    fillSessions(&list, {current, earlier}, QDate(2026, 10, 23), -1);
    EXPECT_FALSE(list.item(1)->flags() & Qt::ItemIsEnabled); // nothing to end yet
}

// The mark: a muted disc with a dark check (this set), a dim ring (an earlier
// set), never red; a selected row never tints it; the Mixes rows carry both.
TEST(DeckLabBrowseTest, PlayedMarksAreQuietNeverRedAndReachTheMixesRows) {
    const auto render = [](Played played) {
        QImage image(kMarkSize, kMarkSize, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        paintPlayedMark(painter, QRectF(0, 0, kMarkSize, kMarkSize), played);
        return image;
    };
    const QImage session = render(Played::Session);
    const QImage earlier = render(Played::Earlier);
    const QImage none = render(Played::No);
    int sessionInk = 0;
    int earlierInk = 0;
    for (int y = 0; y < kMarkSize; ++y) {
        for (int x = 0; x < kMarkSize; ++x) {
            const QColor a = session.pixelColor(x, y);
            const QColor b = earlier.pixelColor(x, y);
            EXPECT_FALSE(a.alpha() > 0 && a.red() > 200 && a.green() < 120) << "red in the mark";
            EXPECT_FALSE(b.alpha() > 0 && b.red() > 200 && b.green() < 120) << "red in the mark";
            sessionInk += a.alpha() > 0;
            earlierInk += b.alpha() > 0;
            EXPECT_EQ(0, none.pixelColor(x, y).alpha());
        }
    }
    EXPECT_GT(earlierInk, 20);         // the ring is drawn
    EXPECT_GT(sessionInk, earlierInk); // a filled disc against a ring
    EXPECT_EQ(0, earlier.pixelColor(kMarkSize / 2 - 4, kMarkSize / 2 + 3).alpha()); // hollow inside
    // The ring itself, at the left edge of the circle, in the dim tone.
    EXPECT_TRUE(near(earlier.pixelColor(1, kMarkSize / 2), decklab::colors::dim))
            << earlier.pixelColor(1, kMarkSize / 2).name(QColor::HexArgb).toStdString();
    EXPECT_EQ(decklab::colors::muted.name(), session.pixelColor(kMarkSize / 2 - 3, kMarkSize / 2 - 3).name());
    const QIcon icon = playedIcon(Played::Session);
    ASSERT_FALSE(icon.isNull());
    EXPECT_EQ(icon.pixmap(QSize(kMarkSize, kMarkSize), QIcon::Normal).toImage(),
            icon.pixmap(QSize(kMarkSize, kMarkSize), QIcon::Selected).toImage());
    EXPECT_TRUE(playedIcon(Played::No).isNull());

    // Mixes rows of tracks played in this set and in an earlier one.
    auto generated = std::make_unique<GeneratedHistory>();
    Playlist older;
    older.id = 1;
    Playlist tonight;
    tonight.id = 2;
    generated->lists = {tonight, older};
    Entry played;
    played.position = 1;
    played.location = QStringLiteral("/generated/played.wav");
    generated->tracks.insert(2, {played});
    Entry before;
    before.position = 1;
    before.location = QStringLiteral("/generated/before.wav");
    generated->tracks.insert(1, {before});
    Store::instance().setBackend(std::move(generated));
    Store::instance().ensureLoaded();
    EXPECT_EQ(Played::Earlier, Store::instance().played(before.location));
    // An export's list may double a slash where Mixxx keeps the path clean.
    EXPECT_EQ(Played::Session, Store::instance().played(QStringLiteral("/generated//played.wav")));
    EXPECT_EQ(Played::No, Store::instance().played(QStringLiteral("/generated//other.wav")));
    const auto mixRow = [](const QString& location) {
        QListWidget list;
        auto* item = new QListWidgetItem(QStringLiteral("Generated Tone 124"), &list);
        item->setData(DeckLabListDelegate::MixRole, true);
        item->setData(DeckLabListDelegate::MixKeyRole, QStringLiteral("8A"));
        item->setData(DeckLabListDelegate::MixBpmRole, 124.0);
        item->setData(LocationRole, location);
        QImage row(520, 44, QImage::Format_ARGB32_Premultiplied);
        row.fill(Qt::black);
        QPainter painter(&row);
        QStyleOptionViewItem option;
        option.rect = row.rect();
        option.font = list.font();
        decklab::paintMixRow(painter, option, list.model()->index(0, 0));
        painter.end();
        return row;
    };
    const QImage sessionRow = mixRow(played.location);
    EXPECT_EQ(decklab::colors::muted.name(),
            sessionRow.pixelColor(sessionRow.width() - 12 - kMarkSize / 2 - 4, sessionRow.height() / 2 - 4).name());
    const QImage earlierRow = mixRow(before.location);
    // The ring at the mark's left edge; its inside stays the row's ground.
    EXPECT_TRUE(near(earlierRow.pixelColor(earlierRow.width() - 12 - kMarkSize, earlierRow.height() / 2 - 2),
            decklab::colors::dim));
    EXPECT_EQ(decklab::colors::ground.name(),
            earlierRow.pixelColor(earlierRow.width() - 12 - kMarkSize / 2 - 4, earlierRow.height() / 2 + 3).name());
    Store::instance().reset();
}
