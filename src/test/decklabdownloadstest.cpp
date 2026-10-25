// SPDX-License-Identifier: GPL-2.0-or-later
// r7-browse: the DOWNLOADS and SAMPLES lists, the song lists without samples
// and the background analysis of new downloads. Generated folders, silent
// files and synthetic names only.
#include <gtest/gtest.h>

#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "decklab/decklabanalysis.h"
#include "decklab/decklabdownloads.h"
#include "library/basetrackcache.h"
#include "library/dao/trackschema.h"
#include "library/decklabfoldertablemodel.h"
#include "library/librarytablemodel.h"
#include "library/trackcollection.h"
#include "test/librarytest.h"
#include "track/track.h"
#include "widget/decklabbrowsehome.h"

namespace {

// The music folder points at a generated one for one test, whatever happens.
struct MusicFolder {
    explicit MusicFolder(const QString& folder) {
        decklab::setMusicFolderForTesting(folder);
    }
    ~MusicFolder() {
        decklab::setMusicFolderForTesting(QString());
    }
};

// A silent WAV file dated `modified` (later than its creation, so the
// modification time is when it arrived).
void writeFile(const QString& path, const QDateTime& modified) {
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).path()));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    QByteArray header;
    QDataStream out(&header, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    const quint32 bytes = 1600;
    out.writeRawData("RIFF", 4);
    out << quint32(36 + bytes);
    out.writeRawData("WAVE", 4);
    out.writeRawData("fmt ", 4);
    out << quint32(16) << quint16(1) << quint16(1) << quint32(8000) << quint32(16000) << quint16(2) << quint16(16);
    out.writeRawData("data", 4);
    out << bytes;
    file.write(header);
    file.write(QByteArray(int(bytes), '\0'));
    file.close();
    ASSERT_TRUE(file.open(QIODevice::ReadWrite));
    ASSERT_TRUE(file.setFileTime(modified, QFileDevice::FileModificationTime));
    file.close();
}

QStringList names(const QStringList& paths) {
    QStringList out;
    for (const QString& path : paths) out << QFileInfo(path).fileName();
    return out;
}

} // namespace

// Single downloads newest first; a Spotify playlist is one folder, placed by
// its newest track; a stick that is not plugged in contributes nothing.
TEST(DeckLabDownloadsTest, DownloadsListNewestFirstWithPlaylistsAsFolders) {
    QTemporaryDir root;
    const QString stick = root.filePath(QStringLiteral("media/decklab/1234-ABCD"));
    const QString music = root.filePath(QStringLiteral("home/dj/Music"));
    const MusicFolder folder(music);
    const QString onStick = QDir(stick).filePath(QStringLiteral("Deck Lab Downloads"));
    const QString onCard = QDir(music).filePath(QStringLiteral("New Music"));
    const QDateTime at = QDateTime::currentDateTime().addDays(1);
    writeFile(onStick + QStringLiteral("/Generated Single A [aaaaaaaaaaa].m4a"), at.addSecs(60));
    writeFile(onCard + QStringLiteral("/Generated Single B [bbbbbbbbbbb].m4a"), at.addSecs(180));
    writeFile(onStick + QStringLiteral("/Generated Playlist/01 - Fixture - One.m4a"), at.addSecs(120));
    writeFile(onStick + QStringLiteral("/Generated Playlist/02 - Fixture - Two.m4a"), at.addSecs(240));
    // Not music, hidden, empty: never listed.
    QFile cover(onStick + QStringLiteral("/cover.jpg"));
    EXPECT_TRUE(cover.open(QIODevice::WriteOnly));
    cover.close();
    writeFile(onStick + QStringLiteral("/.partial/Generated Hidden.m4a"), at.addSecs(300));
    ASSERT_TRUE(QDir().mkpath(onStick + QStringLiteral("/Empty Playlist")));

    const QString absent = root.filePath(QStringLiteral("media/decklab/9999-0000"));
    const QStringList folders = decklab::downloadFolders({stick, absent});
    EXPECT_EQ((QStringList{QDir::cleanPath(onStick), QDir::cleanPath(onCard)}), folders);
    EXPECT_EQ(QStringList{QDir::cleanPath(onCard)}, decklab::downloadFolders({}));

    const auto entries = decklab::listDownloads(folders);
    ASSERT_EQ(3, entries.size());
    EXPECT_TRUE(entries[0].folder);
    EXPECT_EQ(QStringLiteral("Generated Playlist"), entries[0].name);
    EXPECT_EQ(2, entries[0].tracks);
    EXPECT_EQ(QStringLiteral("Generated Single B [bbbbbbbbbbb]"), entries[1].name);
    EXPECT_FALSE(entries[1].folder);
    EXPECT_EQ(QStringLiteral("Generated Single A [aaaaaaaaaaa]"), entries[2].name);
    EXPECT_EQ((QStringList{QStringLiteral("Generated Single B [bbbbbbbbbbb].m4a"),
                      QStringLiteral("Generated Single A [aaaaaaaaaaa].m4a")}),
            names(decklab::newestDownloads(folders)));
    // The analysis takes every download, playlist tracks too, newest first.
    EXPECT_EQ((QStringList{QStringLiteral("02 - Fixture - Two.m4a"),
                      QStringLiteral("Generated Single B [bbbbbbbbbbb].m4a"),
                      QStringLiteral("01 - Fixture - One.m4a"),
                      QStringLiteral("Generated Single A [aaaaaaaaaaa].m4a")}),
            names(decklab::allDownloads(folders)));
    // Pulling the stick: only the Pi card's downloads are left.
    ASSERT_TRUE(QDir(stick).removeRecursively());
    EXPECT_EQ(QStringList{QDir::cleanPath(onCard)}, decklab::downloadFolders({stick}));
    EXPECT_EQ(1, decklab::listDownloads(decklab::downloadFolders({stick})).size());
}

// Get Music numbers a list's tracks in playlist order ("01 - ", three digits
// from 100 tracks on); older, unnumbered files follow in the order they came.
TEST(DeckLabDownloadsTest, PlaylistFoldersKeepPlaylistOrder) {
    QTemporaryDir root;
    const QString list = root.filePath(QStringLiteral("Deck Lab Downloads/Generated List"));
    const QDateTime at = QDateTime::currentDateTime().addDays(1);
    writeFile(list + QStringLiteral("/010 - Fixture - Ten.m4a"), at.addSecs(10));
    writeFile(list + QStringLiteral("/Generated Later [ddddddddddd].m4a"), at.addSecs(90));
    writeFile(list + QStringLiteral("/002 - Fixture - Two.m4a"), at.addSecs(50));
    writeFile(list + QStringLiteral("/100 - Fixture - Hundred.m4a"), at.addSecs(20));
    writeFile(list + QStringLiteral("/Generated Earlier [ccccccccccc].m4a"), at.addSecs(5));
    writeFile(list + QStringLiteral("/001 - Fixture - One.m4a"), at.addSecs(70));
    EXPECT_EQ((QStringList{QStringLiteral("001 - Fixture - One.m4a"),
                      QStringLiteral("002 - Fixture - Two.m4a"),
                      QStringLiteral("010 - Fixture - Ten.m4a"),
                      QStringLiteral("100 - Fixture - Hundred.m4a"),
                      QStringLiteral("Generated Earlier [ccccccccccc].m4a"),
                      QStringLiteral("Generated Later [ddddddddddd].m4a")}),
            names(decklab::playlistOrder(list)));
    EXPECT_EQ(7, decklab::playlistNumber(QStringLiteral("07 - Fixture - Seven.m4a")));
    EXPECT_EQ(120, decklab::playlistNumber(QStringLiteral("120 - Fixture.m4a")));
    EXPECT_EQ(0, decklab::playlistNumber(QStringLiteral("2024 - Fixture.m4a")));
    EXPECT_EQ(0, decklab::playlistNumber(QStringLiteral("7 - Fixture.m4a")));
}

// DOWNLOADS and SAMPLES follow the Library; with them the home has six tiles,
// three a row.
TEST(DeckLabDownloadsTest, HomeTilesWithDownloadsAndSamplesAreThreeARow) {
    using decklab::SourceEntry;
    const auto source = [](const QString& key) {
        SourceEntry entry;
        entry.key = key;
        entry.name = key;
        return entry;
    };
    const auto keys = [](const QVector<SourceEntry>& entries) {
        QStringList out;
        for (const auto& entry : entries) out << entry.key;
        return out;
    };
    const QVector<SourceEntry> roots{source(QStringLiteral("root:tracks")), source(QStringLiteral("root:playlist")),
            source(QStringLiteral("root:crates")), source(QStringLiteral("root:recordings")),
            source(QStringLiteral("root:history"))};
    const QVector<SourceEntry> extras{source(QStringLiteral("downloads")), source(QStringLiteral("samples"))};
    auto layout = decklab::arrangeHome({source(QStringLiteral("usb:/a"))}, roots, source(QStringLiteral("more")), extras);
    EXPECT_EQ((QStringList{"usb:/a", "root:tracks", "downloads", "samples", "root:playlist", "root:crates"}),
            keys(layout.tiles));
    EXPECT_EQ((QStringList{"root:recordings", "root:history", "more"}), keys(layout.also));
    layout = decklab::arrangeHome({}, roots, source(QStringLiteral("more")), extras);
    EXPECT_EQ((QStringList{"nostick", "root:tracks", "downloads", "samples", "root:playlist", "root:crates"}),
            keys(layout.tiles));
    // Without them, the home is as before: four tiles, two a row.
    layout = decklab::arrangeHome({}, roots, source(QStringLiteral("more")));
    EXPECT_EQ(4, layout.tiles.size());
    EXPECT_EQ(2, DeckLabBrowseDelegate::columnsFor(4));
    EXPECT_EQ(3, DeckLabBrowseDelegate::columnsFor(6));
    const QSize tile = DeckLabBrowseDelegate::tileSize(QSize(800, 304), 3);
    EXPECT_LE(3 * tile.width() + 6 * DeckLabBrowseDelegate::kSpacing, 800);
    EXPECT_GE(tile.width(), 250);
    EXPECT_GE(tile.height(), 96);
}

// The analysis runs only while nothing plays: it stops the moment something
// plays, goes on after a quiet spell, counts one batch across pauses and
// never queues a track the analyzer already finished (or could not read).
TEST(DeckLabBackgroundAnalysisTest, StopsWhenSomethingPlaysAndGoesOnWhenQuiet) {
    DeckLabBackgroundAnalysis analysis;
    analysis.setDelays(0, 0);
    bool busy = false;
    QList<TrackId> left{TrackId(QVariant(1)), TrackId(QVariant(2)), TrackId(QVariant(3))};
    QList<QList<TrackId>> started;
    int stops = 0;
    DeckLabBackgroundAnalysis::Hooks hooks;
    hooks.busy = [&busy] { return busy; };
    hooks.candidates = [&left] { return left; };
    hooks.start = [&started](const QList<TrackId>& ids) {
        started << ids;
        return true;
    };
    hooks.stop = [&stops] { ++stops; };
    analysis.setHooks(hooks);
    analysis.check();
    EXPECT_TRUE(started.isEmpty()) << "nothing to look for before a wake";
    analysis.wake();
    analysis.check();
    ASSERT_EQ(1, started.size());
    EXPECT_EQ(left, started[0]);
    EXPECT_EQ(DeckLabBackgroundAnalysis::State::Running, analysis.state());
    EXPECT_EQ(QStringLiteral("Analyzing 1 of 3"), analysis.progressText());
    analysis.trackFinished(TrackId(QVariant(1)));
    left.removeOne(TrackId(QVariant(1)));
    EXPECT_EQ(QStringLiteral("Analyzing 2 of 3"), analysis.progressText());

    // A deck plays: the batch stops at once and nothing starts while it plays.
    busy = true;
    analysis.playbackChanged();
    EXPECT_EQ(1, stops);
    EXPECT_EQ(DeckLabBackgroundAnalysis::State::Waiting, analysis.state());
    EXPECT_EQ(QStringLiteral("Analysis paused at 2 of 3"), analysis.progressText());
    analysis.playbackChanged();
    EXPECT_EQ(1, stops) << "one stop per batch";
    analysis.batchFinished();
    analysis.check();
    EXPECT_EQ(1, started.size());

    // Quiet again: what is left goes on, counted in the same batch.
    busy = false;
    analysis.playbackChanged();
    analysis.check();
    ASSERT_EQ(2, started.size());
    EXPECT_EQ((QList<TrackId>{TrackId(QVariant(2)), TrackId(QVariant(3))}), started[1]);
    EXPECT_EQ(QStringLiteral("Analyzing 2 of 3"), analysis.progressText());
    // Track 2 could not be read (it still has no BPM): never queued again.
    analysis.trackFinished(TrackId(QVariant(2)));
    analysis.trackFinished(TrackId(QVariant(3)));
    left = {TrackId(QVariant(2))};
    analysis.batchFinished();
    analysis.check();
    EXPECT_EQ(2, started.size());
    EXPECT_EQ(DeckLabBackgroundAnalysis::State::Idle, analysis.state());
    EXPECT_TRUE(analysis.progressText().isEmpty());

    // A new download waits for a quiet minute after the last stop.
    left = {TrackId(QVariant(4))};
    analysis.setDelays(0, 60000);
    analysis.playbackChanged();
    analysis.wake();
    analysis.check();
    EXPECT_EQ(2, started.size());
    analysis.setDelays(0, 0);
    analysis.check();
    ASSERT_EQ(3, started.size());
    EXPECT_EQ(QStringLiteral("Analyzing 1 of 1"), analysis.progressText()) << "a new batch counts from one";
    // A batch that ends without a word on a track (it could not even load)
    // never brings that track back.
    analysis.batchFinished();
    analysis.check();
    EXPECT_EQ(3, started.size());
    EXPECT_EQ(DeckLabBackgroundAnalysis::State::Idle, analysis.state());
}

namespace {

class DeckLabDownloadsDbTest : public LibraryTest {
  protected:
    // A library row for a generated file.
    TrackId addTrack(const QString& location, const QString& title, double bpm, const QString& key, bool beats) {
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
                "INSERT INTO library (location, title, artist, bpm, key, beats, header_parsed, mixxx_deleted) "
                "VALUES (:location, :title, 'Deck Lab Fixture', :bpm, :key, :beats, 1, 0)"));
        query.bindValue(QStringLiteral(":location"), locationId);
        query.bindValue(QStringLiteral(":title"), title);
        query.bindValue(QStringLiteral(":bpm"), bpm);
        query.bindValue(QStringLiteral(":key"), key.isEmpty() ? QVariant() : QVariant(key));
        query.bindValue(QStringLiteral(":beats"), beats ? QVariant(QByteArray("grid")) : QVariant());
        EXPECT_TRUE(query.exec()) << query.lastError().text().toStdString();
        return TrackId(query.lastInsertId());
    }
    // The library's track cache, as MixxxLibraryFeature sets it up.
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
    static QStringList locations(BaseSqlTableModel& model) {
        QStringList out;
        for (int row = 0; row < model.rowCount(); ++row) out << model.getTrackLocation(model.index(row, 0));
        return out;
    }

  private:
    QSharedPointer<BaseTrackCache> m_cache;
};

} // namespace

// Samples stay out of the song lists (Tracks, All tracks, song search) but
// stay in the library: SAMPLES lists them, newest first, and they load.
TEST_F(DeckLabDownloadsDbTest, SamplesStayOutOfSongListsButLoadFromSamples) {
    QTemporaryDir music;
    const MusicFolder folder(music.path());
    const QDateTime at = QDateTime::currentDateTime().addDays(1);
    const QString song = music.filePath(QStringLiteral("New Music/Generated Song.wav"));
    const QString hit = music.filePath(QStringLiteral("Deck Lab Samples/Generated Hit.wav"));
    const QString loop = music.filePath(QStringLiteral("Deck Lab Samples/Generated Loop.wav"));
    writeFile(song, at);
    writeFile(hit, at.addSecs(60));
    writeFile(loop, at.addSecs(120));
    addTrack(song, QStringLiteral("Generated Song"), 124, QStringLiteral("8A"), true);
    addTrack(hit, QStringLiteral("Generated Hit"), 0, QString(), false);
    addTrack(loop, QStringLiteral("Generated Loop"), 124, QStringLiteral("8A"), true);
    connectLibraryCache();

    LibraryTableModel tracks(nullptr, trackCollectionManager(), "mixxx.db.model.library");
    tracks.select();
    EXPECT_EQ(QStringList{song}, locations(tracks));
    tracks.search(QStringLiteral("Generated"));
    EXPECT_EQ(QStringList{song}, locations(tracks));
    tracks.search(QStringLiteral("Loop"));
    EXPECT_TRUE(locations(tracks).isEmpty());
    // An OR search keeps them out of every alternative.
    tracks.search(QStringLiteral("Hit | Song"));
    EXPECT_EQ(QStringList{song}, locations(tracks));

    // SAMPLES: the library's rows, in the order given; a file the library
    // does not know yet is left out.
    DeckLabFolderTableModel samples(nullptr, trackCollectionManager());
    const QStringList newest = decklab::audioFilesNewestFirst(decklab::samplesFolder());
    EXPECT_EQ((QStringList{loop, hit}), newest);
    EXPECT_EQ(2, samples.setLocations(newest + QStringList{music.filePath(QStringLiteral("Deck Lab Samples/Not Scanned.wav"))},
                         QStringLiteral("samples")));
    EXPECT_EQ((QStringList{loop, hit}), locations(samples));
    const int position = samples.fieldIndex(ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_POSITION);
    ASSERT_GE(position, 0);
    EXPECT_EQ(1, samples.index(0, position).data().toInt());
    EXPECT_EQ(2, samples.index(1, position).data().toInt());
    EXPECT_TRUE(samples.hasCapabilities(TrackModel::Capability::LoadToSampler));
    EXPECT_TRUE(samples.hasCapabilities(TrackModel::Capability::LoadToDeck));
    EXPECT_FALSE(samples.hasCapabilities(TrackModel::Capability::Sorting)) << "the order is the list's own";
    EXPECT_FALSE(samples.hasCapabilities(TrackModel::Capability::RemoveFromDisk));
    const TrackPointer pad = samples.getTrack(samples.index(1, 0));
    ASSERT_TRUE(pad);
    EXPECT_EQ(hit, pad->getLocation());
    // The same model shows another list: a playlist in playlist order.
    EXPECT_EQ(1, samples.setLocations({song}, QStringLiteral("downloads:tracks")));
    EXPECT_EQ(QStringList{song}, locations(samples));
    QSqlQuery count(internalCollection()->database());
    ASSERT_TRUE(count.exec(QStringLiteral("SELECT COUNT(*) FROM library WHERE mixxx_deleted = 0")) && count.next());
    EXPECT_EQ(3, count.value(0).toInt()) << "samples stay in the library";
}

// The analysis takes the downloads the library knows that still lack a BPM,
// a key or a beat grid, newest first.
TEST_F(DeckLabDownloadsDbTest, AnalysisTakesUnanalyzedDownloadsNewestFirst) {
    QTemporaryDir root;
    const QString done = root.filePath(QStringLiteral("Deck Lab Downloads/Generated Done.m4a"));
    const QString fresh = root.filePath(QStringLiteral("Deck Lab Downloads/Generated Fresh.m4a"));
    const QString keyless = root.filePath(QStringLiteral("Deck Lab Downloads/Generated List/01 - Fixture - Keyless.m4a"));
    addTrack(done, QStringLiteral("Generated Done"), 124, QStringLiteral("8A"), true);
    const TrackId freshId = addTrack(fresh, QStringLiteral("Generated Fresh"), 0, QString(), false);
    const TrackId keylessId = addTrack(keyless, QStringLiteral("Generated Keyless"), 120, QString(), true);
    const QString unknown = root.filePath(QStringLiteral("Deck Lab Downloads/Generated Unknown.m4a"));
    EXPECT_EQ((QList<TrackId>{keylessId, freshId}),
            DeckLabBackgroundAnalysis::tracksToAnalyze(internalCollection()->database(),
                    {unknown, keyless, done, fresh}));
}
