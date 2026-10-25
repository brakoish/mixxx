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
#include <QTest>

#include "decklab/decklabanalysis.h"
#include "decklab/decklabdownloads.h"
#include "decklab/decklabmedia.h"
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
    // r7: the DOWNLOADS tile counts the same from names alone (no stat of
    // every file on a stick plugged in mid-set).
    EXPECT_EQ((decklab::DownloadCounts{4, 1}), decklab::countDownloads(folders));
    // Pulling the stick: only the Pi card's downloads are left.
    ASSERT_TRUE(QDir(stick).removeRecursively());
    EXPECT_EQ(QStringList{QDir::cleanPath(onCard)}, decklab::downloadFolders({stick}));
    EXPECT_EQ(1, decklab::listDownloads(decklab::downloadFolders({stick})).size());
    EXPECT_EQ((decklab::DownloadCounts{1, 0}), decklab::countDownloads(decklab::downloadFolders({stick})));
}

// r7: one samples folder, ~/Music/Deck Lab Samples in the home folder: the
// path Sample Maker hard-codes and its systemd unit allows (and Get Music's
// ~/Music/New Music beside it). Never the desktop's music folder, which
// XDG_MUSIC_DIR or a localized name can move. TO PAD, PUT ON PAD, SAMPLES
// and the song lists' filter all use it.
TEST(DeckLabDownloadsTest, TheSamplesFolderIsInMusicInTheHomeFolder) {
    QTemporaryDir config;
    ASSERT_TRUE(config.isValid());
    const QString elsewhere = config.filePath(QStringLiteral("Elsewhere"));
    ASSERT_TRUE(QDir().mkpath(elsewhere));
    {
        QFile dirs(config.filePath(QStringLiteral("user-dirs.dirs")));
        ASSERT_TRUE(dirs.open(QIODevice::WriteOnly));
        dirs.write(QStringLiteral("XDG_MUSIC_DIR=\"%1\"\n").arg(elsewhere).toUtf8());
    }
    const bool hadConfig = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
    const QByteArray oldConfig = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(config.path()));
    struct Restore {
        bool had;
        QByteArray value;
        ~Restore() {
            if (had) {
                qputenv("XDG_CONFIG_HOME", value);
            } else {
                qunsetenv("XDG_CONFIG_HOME");
            }
        }
    } restore{hadConfig, oldConfig};
    const QString samples = QDir::home().filePath(QStringLiteral("Music/Deck Lab Samples"));
    EXPECT_EQ(QDir::home().filePath(QStringLiteral("Music")), decklab::musicFolder());
    EXPECT_EQ(samples, decklab::samplesFolder());
    EXPECT_EQ(QDir::home().filePath(QStringLiteral("Music/New Music")), decklab::cardDownloadsFolder());
    EXPECT_TRUE(decklab::isSampleLocation(samples + QStringLiteral("/Generated Hit.wav")));
    EXPECT_FALSE(decklab::isSampleLocation(elsewhere + QStringLiteral("/Deck Lab Samples/Generated Hit.wav")));
    QString literal = QDir::cleanPath(samples) + QLatin1Char('/');
    literal.replace(QLatin1Char('\''), QStringLiteral("''"));
    EXPECT_TRUE(decklab::songListSqlFilter().contains(literal)) << "the song lists leave out the same folder";
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

namespace {

// The analyzer as DeckLabBackgroundAnalysis's hooks see it: what started,
// paused, went on, stopped and was let go.
struct FakeAnalyzer {
    bool busy = false;
    QList<TrackId> left;
    QSet<TrackId> onPlayers;
    QList<TrackId> started;
    int pauses = 0;
    int resumes = 0;
    int stops = 0;
    int releases = 0;
    QHash<TrackId, QString> where; // unset: the unit's own music
    bool closing = false;

    DeckLabBackgroundAnalysis::Hooks hooks() {
        DeckLabBackgroundAnalysis::Hooks h;
        h.busy = [this] { return busy; };
        h.location = [this](TrackId id) { return where.value(id); };
        h.closing = [this] { return closing; };
        h.candidates = [this] { return left; };
        h.loaded = [this](TrackId id) { return onPlayers.contains(id); };
        h.start = [this](TrackId id) {
            started << id;
            return true;
        };
        h.pause = [this] { ++pauses; };
        h.resume = [this] { ++resumes; };
        h.stop = [this] { ++stops; };
        h.release = [this] { ++releases; };
        return h;
    }
};

TrackId id(int n) {
    return TrackId(QVariant(n));
}

} // namespace

// r7: the analysis runs only while it is quiet. The moment it is not, the
// analyzer pauses where it is (TrackAnalysisScheduler's own pause: nothing
// is torn down, so the GUI never waits for the analyzer thread) and goes on
// with the same track after a quiet spell. One track at a time, newest
// first; one batch is counted across pauses; a track the analyzer finished
// (or could not read) is never queued again.
TEST(DeckLabBackgroundAnalysisTest, PausesWhenNotQuietAndGoesOnWhereItWas) {
    DeckLabBackgroundAnalysis analysis;
    analysis.setDelays(0, 0);
    FakeAnalyzer analyzer;
    analyzer.left = {id(1), id(2), id(3)};
    analysis.setHooks(analyzer.hooks());
    analysis.check();
    EXPECT_TRUE(analyzer.started.isEmpty()) << "nothing to look for before a wake";
    analysis.wake();
    analysis.check();
    EXPECT_EQ(QList<TrackId>{id(1)}, analyzer.started) << "one track at a time, newest first";
    EXPECT_EQ(id(1), analysis.current());
    EXPECT_EQ(DeckLabBackgroundAnalysis::State::Running, analysis.state());
    EXPECT_EQ(QStringLiteral("Analyzing 1 of 3"), analysis.progressText());
    analysis.trackFinished(id(1));
    analyzer.left.removeOne(id(1));
    EXPECT_EQ((QList<TrackId>{id(1), id(2)}), analyzer.started) << "the next starts at once";
    EXPECT_EQ(QStringLiteral("Analyzing 2 of 3"), analysis.progressText());

    // A deck plays: the analyzer pauses at once; nothing is stopped or torn down.
    analyzer.busy = true;
    analysis.playbackChanged();
    EXPECT_EQ(1, analyzer.pauses);
    EXPECT_EQ(0, analyzer.stops);
    EXPECT_EQ(id(2), analysis.current()) << "it keeps its place";
    EXPECT_EQ(DeckLabBackgroundAnalysis::State::Waiting, analysis.state());
    EXPECT_EQ(QStringLiteral("Analysis paused at 2 of 3"), analysis.progressText());
    analysis.playbackChanged();
    analysis.check();
    EXPECT_EQ(1, analyzer.pauses) << "one pause";
    // Waiting for longer than any stop timeout changes nothing.
    QTest::qWait(DeckLabBackgroundAnalysis::kCheckMs * 2 + 200);
    EXPECT_EQ(0, analyzer.stops);
    EXPECT_EQ(id(2), analysis.current());
    EXPECT_EQ(2, analyzer.started.size());

    // Quiet again, after the quiet spell: the same track goes on.
    analyzer.busy = false;
    analysis.setDelays(0, 60000);
    analysis.playbackChanged();
    analysis.check();
    EXPECT_EQ(0, analyzer.resumes) << "a quiet spell first";
    analysis.setDelays(0, 0);
    analysis.check();
    EXPECT_EQ(1, analyzer.resumes);
    EXPECT_EQ(2, analyzer.started.size()) << "not started again";
    EXPECT_EQ(QStringLiteral("Analyzing 2 of 3"), analysis.progressText());
    // Track 2 could not be read (it still has no BPM): never queued again.
    analysis.trackFinished(id(2));
    EXPECT_EQ(id(3), analyzer.started.last());
    analysis.trackFinished(id(3));
    analyzer.left = {id(2)};
    analysis.wake();
    analysis.check();
    EXPECT_EQ(3, analyzer.started.size());
    EXPECT_EQ(DeckLabBackgroundAnalysis::State::Idle, analysis.state());
    EXPECT_TRUE(analysis.progressText().isEmpty());
    EXPECT_GE(analyzer.stops, 1) << "nothing left: the analyzer's thread may exit";

    // A new download counts from one.
    analyzer.left = {id(4)};
    analysis.wake();
    analysis.check();
    EXPECT_EQ(id(4), analyzer.started.last());
    EXPECT_EQ(QStringLiteral("Analyzing 1 of 1"), analysis.progressText()) << "a new batch counts from one";
}

// r7: a library scan, a recording or an open utility keeps it waiting like
// play does (one rule, decklab/decklabquiet.h). An analyzed track is let go,
// so the library saves it, only once it is quiet: never beside a scan's
// write transaction or during a set.
TEST(DeckLabBackgroundAnalysisTest, ScansKeepItWaitingAndSavesWaitUntilQuiet) {
    DeckLabBackgroundAnalysis analysis;
    analysis.setDelays(0, 0);
    FakeAnalyzer analyzer;
    analyzer.left = {id(1), id(2)};
    analysis.setHooks(analyzer.hooks());
    analyzer.busy = true; // a scan runs
    analysis.wake();
    analysis.check();
    EXPECT_TRUE(analyzer.started.isEmpty()) << "never during a scan";
    EXPECT_EQ(0, analyzer.releases);
    analyzer.busy = false;
    analysis.check();
    ASSERT_EQ(QList<TrackId>{id(1)}, analyzer.started);
    const int released = analyzer.releases;
    // The scan starts again while track 1 finishes its save.
    analyzer.busy = true;
    analysis.playbackChanged();
    EXPECT_EQ(1, analyzer.pauses);
    analysis.trackFinished(id(1));
    EXPECT_EQ(1, analyzer.started.size()) << "the next waits";
    analysis.check();
    EXPECT_EQ(released, analyzer.releases) << "the analyzed track is kept while it is not quiet";
    analyzer.busy = false;
    analysis.check();
    EXPECT_GT(analyzer.releases, released);
    EXPECT_EQ(id(2), analyzer.started.last());
}

// r7: a track a deck, sampler or the preview holds (or loads) is left to
// that player's own analysis: never started, and let go of (without waiting)
// when a player loads it while it is analyzed.
TEST(DeckLabBackgroundAnalysisTest, TracksOnAPlayerAreLeftToItsAnalysis) {
    DeckLabBackgroundAnalysis analysis;
    analysis.setDelays(0, 0);
    FakeAnalyzer analyzer;
    analyzer.left = {id(1), id(2), id(3)};
    analyzer.onPlayers = {id(1)};
    analysis.setHooks(analyzer.hooks());
    analysis.wake();
    analysis.check();
    EXPECT_EQ(QList<TrackId>{id(2)}, analyzer.started) << "track 1 is on a deck";
    EXPECT_EQ(QStringLiteral("Analyzing 1 of 2"), analysis.progressText());
    // The DJ loads track 2 to cue it while it is analyzed.
    analyzer.onPlayers.insert(id(2));
    analysis.check();
    EXPECT_EQ(1, analyzer.stops) << "two decoders on one file help nobody";
    EXPECT_EQ((QList<TrackId>{id(2), id(3)}), analyzer.started);
    EXPECT_EQ(id(3), analysis.current());
    analysis.trackFinished(id(2)); // a late word from the analyzer let go of
    EXPECT_EQ(id(3), analysis.current());
    analysis.trackFinished(id(3));
    EXPECT_EQ(DeckLabBackgroundAnalysis::State::Idle, analysis.state());
}

// r7: a paused track keeps its file open. On a USB stick that would keep
// the stick from being ejected for the rest of the set, so a track there is
// let go of instead (never waiting) and starts again after the next quiet
// spell, still counted in its batch. One on the unit's own music pauses.
TEST(DeckLabBackgroundAnalysisTest, ATrackOnAStickIsLetGoOfInsteadOfPaused) {
    DeckLabBackgroundAnalysis analysis;
    analysis.setDelays(0, 0);
    FakeAnalyzer analyzer;
    analyzer.left = {id(1), id(2)};
    analyzer.where.insert(id(1), QStringLiteral("/media/user/MY STICK/Deck Lab Downloads/Generated One.m4a"));
    analyzer.where.insert(id(2), QStringLiteral("/home/user/Music/Deck Lab Downloads/Generated Two.m4a"));
    analysis.setHooks(analyzer.hooks());
    analysis.wake();
    analysis.check();
    ASSERT_EQ(QList<TrackId>{id(1)}, analyzer.started);
    EXPECT_EQ(QStringLiteral("Analyzing 1 of 2"), analysis.progressText());

    // A deck plays: the stick's file is let go of.
    analyzer.busy = true;
    analysis.playbackChanged();
    EXPECT_EQ(0, analyzer.pauses);
    EXPECT_EQ(1, analyzer.stops) << "its file closes after one chunk";
    EXPECT_FALSE(analysis.current().isValid());
    EXPECT_EQ(DeckLabBackgroundAnalysis::State::Waiting, analysis.state());
    EXPECT_EQ(QStringLiteral("Analysis paused at 1 of 2"), analysis.progressText());
    analysis.check();
    EXPECT_EQ(1, analyzer.started.size()) << "never while a deck plays";

    // Quiet again: the same track starts again, still 1 of 2.
    analyzer.busy = false;
    analysis.check();
    EXPECT_EQ((QList<TrackId>{id(1), id(1)}), analyzer.started);
    EXPECT_EQ(QStringLiteral("Analyzing 1 of 2"), analysis.progressText());
    analysis.trackFinished(id(1));
    ASSERT_EQ(id(2), analysis.current());

    // The unit's own music: a pause, it keeps its place.
    analyzer.busy = true;
    analysis.playbackChanged();
    EXPECT_EQ(1, analyzer.pauses);
    EXPECT_EQ(1, analyzer.stops);
    EXPECT_EQ(id(2), analysis.current());
}

// r7: Eject asks first. A track on that stick is let go of (it comes back
// with the stick), no other track on it starts for a while, and the answer
// says whether the analyzer still has a file open.
TEST(DeckLabBackgroundAnalysisTest, AStickBeingEjectedIsLeftAlone) {
    const QString stick = QStringLiteral("/media/decklab/1234-ABCD");
    DeckLabBackgroundAnalysis analysis;
    analysis.setDelays(0, 0);
    FakeAnalyzer analyzer;
    analyzer.left = {id(1), id(2), id(3)};
    analyzer.where.insert(id(1), stick + QStringLiteral("/Deck Lab Downloads/Generated One.m4a"));
    analyzer.where.insert(id(2), stick + QStringLiteral("/Deck Lab Downloads/Generated Two.m4a"));
    analyzer.where.insert(id(3), QStringLiteral("/media/decklab/1234-ABCDE/Deck Lab Downloads/Generated Three.m4a"));
    analysis.setHooks(analyzer.hooks());
    analysis.wake();
    analysis.check();
    ASSERT_EQ(id(1), analysis.current());

    analyzer.closing = true;
    EXPECT_TRUE(analysis.letGo(stick)) << "its file is still open";
    EXPECT_EQ(1, analyzer.stops);
    EXPECT_FALSE(analysis.current().isValid());
    analyzer.closing = false;
    EXPECT_FALSE(analysis.letGo(stick));
    EXPECT_EQ(1, analyzer.stops) << "nothing more to let go of";
    analysis.check();
    EXPECT_EQ((QList<TrackId>{id(1), id(3)}), analyzer.started) << "track 2 is on the stick too";
    analysis.trackFinished(id(3));
    EXPECT_EQ(DeckLabBackgroundAnalysis::State::Idle, analysis.state());
    const int stops = analyzer.stops;
    EXPECT_FALSE(analysis.letGo(QStringLiteral("/media/decklab/9999-0000"))) << "another stick: nothing to do";
    EXPECT_EQ(stops, analyzer.stops);
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

// r7: DOWNLOADS and SAMPLES never show a hidden row, and after a USB merge
// or rename pass each file's row is found again by its location. A rename
// takeover gives the DJ's row the address of the row the scan added at the
// new name, and hides that one.
TEST_F(DeckLabDownloadsDbTest, FolderListsFollowARenameTakeover) {
    QTemporaryDir root;
    const QString renamed = root.filePath(QStringLiteral("Deck Lab Downloads/01 Generated Renamed.m4a"));
    const QString old = root.filePath(QStringLiteral("Deck Lab Downloads/Generated Renamed.m4a"));
    const TrackId mine = addTrack(old, QStringLiteral("Generated Renamed"), 124, QStringLiteral("8A"), true);
    const TrackId scanned = addTrack(renamed, QStringLiteral("Generated Renamed"), 0, QString(), false);
    connectLibraryCache();
    DeckLabFolderTableModel list(nullptr, trackCollectionManager());
    ASSERT_EQ(1, list.setLocations({renamed}, QStringLiteral("downloads:tracks")));
    EXPECT_EQ(scanned, list.getTrackId(list.index(0, 0)));
    // The takeover, as the relinker applies it.
    QSqlQuery query(internalCollection()->database());
    const auto locationOf = [&query](TrackId track) {
        EXPECT_TRUE(query.exec(QStringLiteral("SELECT location FROM library WHERE id=%1").arg(track.toString())));
        return query.next() ? query.value(0).toLongLong() : -1;
    };
    const qint64 oldAddress = locationOf(mine);
    const qint64 newAddress = locationOf(scanned);
    ASSERT_TRUE(query.exec(QStringLiteral("UPDATE library SET location=%1 WHERE id=%2")
                                   .arg(newAddress)
                                   .arg(mine.toString())));
    ASSERT_TRUE(query.exec(QStringLiteral("UPDATE library SET location=%1, mixxx_deleted=1 WHERE id=%2")
                                   .arg(oldAddress)
                                   .arg(scanned.toString())));
    // Even before the list is built again, the hidden row is gone from it.
    list.select();
    EXPECT_EQ(0, list.rowCount()) << "a hidden row never shows (LOAD would get a missing file)";
    // The pass ends with a forced media refresh: the file's row is found again.
    decklab::MediaPresence::instance().refresh(internalCollection()->database(), true);
    ASSERT_EQ(1, list.rowCount());
    EXPECT_EQ(mine, list.getTrackId(list.index(0, 0)));
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
    QHash<TrackId, QString> where;
    EXPECT_EQ((QList<TrackId>{keylessId, freshId}),
            DeckLabBackgroundAnalysis::tracksToAnalyze(internalCollection()->database(),
                    {unknown, keyless, done, fresh},
                    &where));
    EXPECT_EQ(2, where.size());
    EXPECT_EQ(fresh, where.value(freshId));
    EXPECT_EQ(keyless, where.value(keylessId));
}
