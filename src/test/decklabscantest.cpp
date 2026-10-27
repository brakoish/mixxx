// SPDX-License-Identifier: GPL-2.0-or-later
// r7 fix: Mixxx's library scan never makes the GUI thread wait for a USB
// stick. A new file's tags and cover are read outside the global track
// cache, the scan commits what it adds in small batches, modified tracks
// are kept (not saved) while it writes, and a scan canceled for playback
// leaves every row whole and goes on where it stopped next time.
// Generated copies of a test file only: no stick, no audio, no personal paths.
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <atomic>
#include <chrono>
#include <future>
#include <utility>

#include "library/coverart.h"
#include "library/library_decl.h"
#include "library/library_prefs.h"
#include "library/scanner/libraryscanner.h"
#include "sources/soundsourceproxy.h"
#include "test/librarytest.h"
#include "track/globaltrackcache.h"
#include "track/track.h"
#include "util/fileaccess.h"

class DeckLabImportTest : public LibraryTest {
  protected:
    TrackDAO& dao() const {
        return internalCollection()->getTrackDAO();
    }
    void prepare() {
        dao().addTracksPrepare();
    }
    TrackPointer addFile(const QString& path) {
        return dao().addTracksAddFile(path, false);
    }
    void commit() {
        dao().addTracksCommit();
    }
    void finish() {
        dao().addTracksFinish(false);
    }
    void onReading(std::function<void(const QString&)> hook) {
        dao().m_readingFileForTesting = std::move(hook);
    }
    static TrackDAO& scannerDao(LibraryScanner& scanner) {
        return scanner.m_trackDao;
    }
    static void onScannerReading(LibraryScanner& scanner, std::function<void(const QString&)> hook) {
        scanner.m_trackDao.m_readingFileForTesting = std::move(hook);
    }
    // The scanner's add transaction holds writes it has not committed.
    static bool holdsWrites(const LibraryScanner& scanner) {
        return scanner.m_addWrites;
    }
    // Called right before the scanner looks up a track it adds (the
    // scanner's own hook commits its batch there if it would wait).
    static std::function<bool()>& beforeCacheLookup(LibraryScanner& scanner) {
        return scanner.m_trackDao.m_beforeCacheLookup;
    }
    // The scanner's batch commits fail while this says so.
    static void failCommits(LibraryScanner& scanner, std::function<bool()> fail) {
        scanner.m_trackDao.m_failCommitForTesting = std::move(fail);
    }
    // Holds a defer of saving modified tracks (as a scan does), let go at
    // the end even when the test fails half way.
    class DeferredSaves {
      public:
        DeferredSaves() {
            GlobalTrackCache::deferSavingModifiedTracks(true);
        }
        ~DeferredSaves() {
            end();
        }
        void end() {
            if (std::exchange(m_active, false)) {
                GlobalTrackCache::deferSavingModifiedTracks(false);
            }
        }

      private:
        bool m_active = true;
    };
    // The title in the file's tags, read afresh.
    QString fileTitle(const QString& path) const {
        const TrackPointer track = Track::newTemporary(path);
        SoundSourceProxy(track).updateTrackFromSource(
                SoundSourceProxy::UpdateTrackFromSourceMode::Once,
                SyncTrackMetadataParams::readFromUserSettings(*config()));
        return track->getTitle();
    }
    // A generated copy of a tagged test file with an embedded cover.
    QString copyFixture(const QTemporaryDir& dir, const QString& name) const {
        const QDir data(getTestDir().filePath(QStringLiteral("id3-test-data")));
        const QStringList covers = data.entryList(QStringList{QStringLiteral("cover-test*-png.mp3")}, QDir::Files);
        EXPECT_FALSE(covers.isEmpty());
        const QString target = QDir(dir.path()).filePath(name);
        EXPECT_TRUE(QFile::copy(data.filePath(covers.value(0)), target)) << target.toStdString();
        return target;
    }
    QVariant value(const QString& sql) const {
        QSqlQuery query(internalCollection()->database());
        EXPECT_TRUE(query.exec(sql)) << query.lastError().text().toStdString() << " " << sql.toStdString();
        return query.next() ? query.value(0) : QVariant();
    }
};

// A file on a slow stick is read while the GUI thread looks up another track
// that is not cached (a Browse selection, a preview): that lookup no longer
// waits for the read. The read is held up here (a stick stalling) until the
// lookup is done, three seconds at most.
TEST_F(DeckLabImportTest, AnotherTrackLoadsWhileANewFileIsRead) {
    QTemporaryDir dir;
    const QString slow = copyFixture(dir, QStringLiteral("Generated Slow.mp3"));
    const QString other = copyFixture(dir, QStringLiteral("Generated Other.mp3"));
    std::future<GlobalTrackCacheLookupResult> lookup;
    bool doneWhileReading = false;
    onReading([&](const QString& location) {
        if (location != mixxx::FileInfo(slow).location()) {
            return;
        }
        lookup = std::async(std::launch::async, [other] {
            const auto resolver = GlobalTrackCacheResolver(mixxx::FileAccess(mixxx::FileInfo(other)));
            return resolver.getLookupResult();
        });
        doneWhileReading = lookup.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
    });
    prepare();
    const TrackPointer track = addFile(slow);
    finish();
    onReading({});
    ASSERT_TRUE(lookup.valid());
    EXPECT_EQ(GlobalTrackCacheLookupResult::Miss, lookup.get());
    EXPECT_TRUE(doneWhileReading) << "the lookup waited for the scanner reading the stick";
    ASSERT_TRUE(track);
    EXPECT_TRUE(track->getId().isValid());
    QCoreApplication::processEvents(); // the other track leaves the cache
}

// The import keeps everything the file says, exactly as before: tags, the
// embedded cover, the file type; the new track is clean and in the library.
TEST_F(DeckLabImportTest, AnImportedFileKeepsItsTagsAndCover) {
    QTemporaryDir dir;
    const QString file = copyFixture(dir, QStringLiteral("Generated Cover.mp3"));
    const TrackPointer expected = Track::newTemporary(file);
    SoundSourceProxy(expected).updateTrackFromSource(
            SoundSourceProxy::UpdateTrackFromSourceMode::Once,
            SyncTrackMetadataParams::readFromUserSettings(*config()));
    ASSERT_FALSE(expected->getTitle().isEmpty());
    prepare();
    const TrackPointer track = addFile(file);
    finish();
    ASSERT_TRUE(track);
    EXPECT_FALSE(track->isDirty());
    EXPECT_TRUE(track->checkSourceSynchronized());
    EXPECT_EQ(expected->getTitle(), track->getTitle());
    EXPECT_EQ(expected->getArtist(), track->getArtist());
    EXPECT_EQ(expected->getAlbum(), track->getAlbum());
    EXPECT_EQ(expected->getType(), track->getType());
    EXPECT_EQ(expected->getSourceSynchronizedAt(), track->getSourceSynchronizedAt());
    EXPECT_EQ(CoverInfo::METADATA, track->getCoverInfo().type);
    EXPECT_EQ(CoverInfo::GUESSED, track->getCoverInfo().source);
    EXPECT_EQ(expected->getCoverInfo().imageDigest(), track->getCoverInfo().imageDigest());
    const QString id = track->getId().toString();
    EXPECT_EQ(expected->getTitle(), value(QStringLiteral("SELECT title FROM library WHERE id=%1").arg(id)).toString());
    EXPECT_EQ(static_cast<int>(CoverInfo::METADATA),
            value(QStringLiteral("SELECT coverart_type FROM library WHERE id=%1").arg(id)).toInt());
    EXPECT_EQ(expected->getCoverInfo().imageDigest(),
            value(QStringLiteral("SELECT coverart_digest FROM library WHERE id=%1").arg(id)).toByteArray());
}

// Each batch is committed: what it added is there for everyone else (and
// the library is free for their writes) while the next batch goes on.
TEST_F(DeckLabImportTest, ACommittedBatchIsThereForEveryoneElse) {
    QTemporaryDir dir;
    const QString first = copyFixture(dir, QStringLiteral("Generated First.mp3"));
    const QString second = copyFixture(dir, QStringLiteral("Generated Second.mp3"));
    {
        // Another connection to the same library, as the GUI's is to the scanner's.
        QSqlDatabase other = QSqlDatabase::cloneDatabase(internalCollection()->database(), QStringLiteral("decklab-other"));
        ASSERT_TRUE(other.open());
        const auto rows = [&other]() {
            QSqlQuery query(other);
            return query.exec(QStringLiteral("SELECT COUNT(*) FROM library")) && query.next() ? query.value(0).toInt() : -1;
        };
        prepare();
        ASSERT_TRUE(addFile(first));
        EXPECT_LE(rows(), 0) << "nothing before the commit";
        commit();
        EXPECT_EQ(1, rows());
        ASSERT_TRUE(addFile(second));
        EXPECT_LE(rows(), 1);
        finish();
        EXPECT_EQ(2, rows());
        other.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("decklab-other"));
}

// A modified track whose last reference goes while a scan writes is kept,
// not saved (saving waits for the database with the cache locked while the
// scanner waits for the cache); it is saved once the scan ends.
TEST_F(DeckLabImportTest, ModifiedTracksAreSavedOnceTheScanEnds) {
    QTemporaryDir dir;
    const QString file = copyFixture(dir, QStringLiteral("Generated Edited.mp3"));
    TrackPointer track = getOrAddTrackByLocation(file);
    ASSERT_TRUE(track);
    const TrackId id = track->getId();
    const QString title = QStringLiteral("SELECT title FROM library WHERE id=%1").arg(id.toString());
    const QString before = value(title).toString();
    track->setTitle(QStringLiteral("Generated Edit"));
    ASSERT_TRUE(track->isDirty());
    DeferredSaves deferred;
    EXPECT_TRUE(GlobalTrackCache::isSavingModifiedTracksDeferred());
    track.reset();
    QCoreApplication::processEvents();
    TrackPointer kept;
    {
        const GlobalTrackCacheLocker locker;
        kept = locker.lookupTrackById(id);
    }
    ASSERT_TRUE(kept) << "kept in the cache while the scan writes";
    EXPECT_EQ(QStringLiteral("Generated Edit"), kept->getTitle());
    kept.reset();
    EXPECT_EQ(before, value(title).toString()) << "not saved beside the scan";
    deferred.end();
    EXPECT_FALSE(GlobalTrackCache::isSavingModifiedTracksDeferred());
    QCoreApplication::processEvents();
    EXPECT_EQ(QStringLiteral("Generated Edit"), value(title).toString());
    {
        const GlobalTrackCacheLocker locker;
        kept = locker.lookupTrackById(id);
    }
    EXPECT_FALSE(kept) << "saved and evicted";
}

// The real scanner on its own thread: it commits in batches while it adds
// (not once at the end), and every file it found is in the library after.
TEST_F(DeckLabImportTest, TheScanCommitsInBatches) {
    QTemporaryDir dir;
    constexpr int kFiles = 45;
    for (int n = 1; n <= kFiles; ++n) {
        copyFixture(dir, QStringLiteral("Generated %1.mp3").arg(n, 2, 10, QLatin1Char('0')));
    }
    ASSERT_EQ(DirectoryDAO::AddResult::Ok, trackCollectionManager()->addDirectory(mixxx::FileInfo(dir.path())));
    LibraryScanner scanner(dbConnectionPooler(), config());
    std::atomic<int> batches{0};
    std::atomic<int> batchesBefore30{-1};
    QObject::connect(&scannerDao(scanner), &TrackDAO::tracksAdded, &scannerDao(scanner), [&batches](const QSet<TrackId>&) {
        ++batches;
    }, Qt::DirectConnection);
    onScannerReading(scanner, [&](const QString& location) {
        QThread::msleep(15); // a stick, slower than the test machine's disk
        if (location.endsWith(QStringLiteral("Generated 30.mp3"))) {
            batchesBefore30 = batches.load();
        }
    });
    std::atomic<bool> canceled{true};
    QObject::connect(&scanner, &LibraryScanner::scanSummary, &scanner, [&canceled](const LibraryScanResultSummary& result) {
        canceled = result.canceled;
    }, Qt::DirectConnection);
    QSignalSpy finished(&scanner, &LibraryScanner::scanFinished);
    scanner.start();
    ASSERT_TRUE(scanner.scanQuietly());
    ASSERT_TRUE(finished.wait(30000));
    EXPECT_FALSE(canceled.load());
    EXPECT_GE(batchesBefore30.load(), 1) << "a batch was committed while the scan went on";
    EXPECT_GE(batches.load(), 3);
    EXPECT_EQ(kFiles, value(QStringLiteral("SELECT COUNT(*) FROM library")).toInt());
    EXPECT_EQ(kFiles, value(QStringLiteral("SELECT COUNT(*) FROM track_locations WHERE fs_deleted=0")).toInt());
    EXPECT_FALSE(GlobalTrackCache::isSavingModifiedTracksDeferred()) << "saves go on after the scan";
}

// While the scanner reads a file from a slow stick it holds no write lock:
// a write from another connection (the GUI thread saving a track or the
// history) goes through at once instead of waiting for SQLite's 5 s busy
// timeout and failing. The test library shares one cache between its
// connections, where a write beside an open write transaction fails at once.
TEST_F(DeckLabImportTest, TheScanHoldsNoWriteLockWhileItReadsASlowFile) {
    QTemporaryDir dir;
    constexpr int kFiles = 6;
    for (int n = 1; n <= kFiles; ++n) {
        copyFixture(dir, QStringLiteral("Generated %1.mp3").arg(n));
    }
    ASSERT_EQ(DirectoryDAO::AddResult::Ok, trackCollectionManager()->addDirectory(mixxx::FileInfo(dir.path())));
    value(QStringLiteral("CREATE TABLE decklab_generated_writes (n INTEGER)"));
    const QString main = internalCollection()->database().connectionName();
    LibraryScanner scanner(dbConnectionPooler(), config());
    std::atomic<int> reads{0};
    std::atomic<int> refused{0};
    onScannerReading(scanner, [&](const QString&) {
        const QString name = QStringLiteral("decklab-writer");
        {
            QSqlDatabase other = QSqlDatabase::cloneDatabase(main, name);
            if (!other.open()) {
                ++refused;
            } else {
                QSqlQuery write(other);
                if (!write.exec(QStringLiteral("INSERT INTO decklab_generated_writes (n) VALUES (1)"))) {
                    ++refused;
                }
                other.close();
            }
        }
        QSqlDatabase::removeDatabase(name);
        ++reads;
        QThread::msleep(250); // a slow stick
    });
    QSignalSpy finished(&scanner, &LibraryScanner::scanFinished);
    scanner.start();
    ASSERT_TRUE(scanner.scanQuietly());
    ASSERT_TRUE(finished.wait(30000));
    EXPECT_EQ(kFiles, reads.load());
    EXPECT_EQ(0, refused.load()) << "a write had to wait for the scanner reading a file";
    EXPECT_EQ(kFiles, value(QStringLiteral("SELECT COUNT(*) FROM decklab_generated_writes")).toInt());
    EXPECT_EQ(kFiles, value(QStringLiteral("SELECT COUNT(*) FROM library")).toInt());
}

// The GUI thread adds a file it loads (a USB stick's track) while a scan
// writes a batch: it fills in its new track and writes its row. The
// scanner, about to wait for that track with the batch's first rows written
// and not committed, commits first: otherwise it would hold the write lock
// while waiting for the GUI, and the GUI's write would wait for it until
// SQLite's busy timeout and fail. Here another thread stands in for the
// GUI, and comes exactly when the scanner holds the write; the test library
// fails such a write at once instead of waiting.
TEST_F(DeckLabImportTest, TheScanCommitsBeforeItWaitsForATrackTheGuiAdds) {
    QTemporaryDir dir;
    constexpr int kFiles = 5;
    for (int n = 1; n <= kFiles; ++n) {
        copyFixture(dir, QStringLiteral("Generated %1.mp3").arg(n));
    }
    QTemporaryDir elsewhere;
    const QString loaded = copyFixture(elsewhere, QStringLiteral("Generated Loaded.mp3"));
    ASSERT_EQ(DirectoryDAO::AddResult::Ok, trackCollectionManager()->addDirectory(mixxx::FileInfo(dir.path())));
    value(QStringLiteral("CREATE TABLE decklab_generated_writes (n INTEGER)"));
    const QString main = internalCollection()->database().connectionName();
    LibraryScanner scanner(dbConnectionPooler(), config());
    // Quick reads: the files go into one batch, written one after the other.
    std::future<bool> gui;
    std::atomic<bool> fillingFirst{false};
    std::atomic<bool> cameWithWriteHeld{false};
    const std::function<bool()> scannersHook = beforeCacheLookup(scanner);
    ASSERT_TRUE(scannersHook);
    beforeCacheLookup(scanner) = [&] {
        // The second file of a batch: the first one's rows are written.
        if (!gui.valid() && holdsWrites(scanner)) {
            cameWithWriteHeld = true;
            std::promise<void> filling;
            auto filled = filling.get_future();
            gui = std::async(std::launch::async, [&main, loaded, filling = std::move(filling)]() mutable {
                // The new track is being filled in...
                const auto resolver = GlobalTrackCacheResolver(mixxx::FileAccess(mixxx::FileInfo(loaded)));
                filling.set_value();
                QThread::msleep(300); // its file is read meanwhile
                // ... and its row written.
                const QString name = QStringLiteral("decklab-gui");
                bool written = false;
                {
                    QSqlDatabase other = QSqlDatabase::cloneDatabase(main, name);
                    if (other.open()) {
                        QSqlQuery write(other);
                        written = write.exec(QStringLiteral("INSERT INTO decklab_generated_writes (n) VALUES (1)"));
                        other.close();
                    }
                }
                QSqlDatabase::removeDatabase(name);
                return written;
            });
            // Bounded: the GUI's track is being filled in before the
            // scanner looks it up.
            fillingFirst = filled.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        }
        return scannersHook();
    };
    QSignalSpy finished(&scanner, &LibraryScanner::scanFinished);
    scanner.start();
    ASSERT_TRUE(scanner.scanQuietly());
    ASSERT_TRUE(finished.wait(30000));
    ASSERT_TRUE(cameWithWriteHeld.load()) << "no batch of two or more files was written";
    ASSERT_TRUE(gui.valid());
    EXPECT_TRUE(fillingFirst.load());
    EXPECT_TRUE(gui.get()) << "the GUI's write waited for the scanner, which waited for the GUI";
    EXPECT_EQ(kFiles, value(QStringLiteral("SELECT COUNT(*) FROM library")).toInt());
    QCoreApplication::processEvents(); // the GUI's track leaves the cache
}

// As above, but the batch cannot be committed (a busy or broken library):
// the scanner does not wait for the GUI's track with the write held either.
// It stops the scan and leaves its file for the next one. The GUI's track
// here is filled in only once the scan is over (5 s at most).
TEST_F(DeckLabImportTest, AScanThatCannotCommitStopsRatherThanWaitForTheGui) {
    QTemporaryDir dir;
    constexpr int kFiles = 5;
    for (int n = 1; n <= kFiles; ++n) {
        copyFixture(dir, QStringLiteral("Generated %1.mp3").arg(n));
    }
    QTemporaryDir elsewhere;
    const QString loaded = copyFixture(elsewhere, QStringLiteral("Generated Loaded.mp3"));
    ASSERT_EQ(DirectoryDAO::AddResult::Ok, trackCollectionManager()->addDirectory(mixxx::FileInfo(dir.path())));
    LibraryScanner scanner(dbConnectionPooler(), config());
    std::atomic<bool> failing{false};
    failCommits(scanner, [&failing] {
        return failing.load();
    });
    std::atomic<bool> scanOver{false};
    std::atomic<bool> canceled{false};
    QObject::connect(&scanner, &LibraryScanner::scanSummary, &scanner, [&](const LibraryScanResultSummary& result) {
        canceled = result.canceled;
        scanOver = true;
    }, Qt::DirectConnection);
    std::future<bool> gui;
    const std::function<bool()> scannersHook = beforeCacheLookup(scanner);
    ASSERT_TRUE(scannersHook);
    std::atomic<int> skipped{0};
    beforeCacheLookup(scanner) = [&] {
        // The second file of a batch: the first one's rows are written.
        if (!gui.valid() && holdsWrites(scanner)) {
            failing = true; // from here on
            std::promise<void> filling;
            auto filled = filling.get_future();
            gui = std::async(std::launch::async, [&scanOver, loaded, filling = std::move(filling)]() mutable {
                const auto resolver = GlobalTrackCacheResolver(mixxx::FileAccess(mixxx::FileInfo(loaded)));
                filling.set_value();
                for (int ms = 0; ms < 5000 && !scanOver.load(); ms += 10) {
                    QThread::msleep(10);
                }
                return scanOver.load();
            });
            filled.wait_for(std::chrono::seconds(2));
        }
        const bool add = scannersHook();
        if (!add) {
            ++skipped;
        }
        return add;
    };
    QSignalSpy finished(&scanner, &LibraryScanner::scanFinished);
    scanner.start();
    ASSERT_TRUE(scanner.scanQuietly());
    ASSERT_TRUE(finished.wait(30000));
    ASSERT_TRUE(gui.valid()) << "no batch of two or more files was written";
    EXPECT_TRUE(gui.get()) << "the scanner waited for the GUI's track with its write held";
    EXPECT_EQ(1, skipped.load());
    EXPECT_TRUE(canceled.load());
    EXPECT_LT(value(QStringLiteral("SELECT COUNT(*) FROM library")).toInt(), kFiles);
    EXPECT_FALSE(GlobalTrackCache::areLibraryWritesHeld());
    QCoreApplication::processEvents(); // the GUI's track leaves the cache
}

// The scanner says it writes from right before the first write of each
// batch, so a kept track is never saved beside that write (the save would
// wait for SQLite's busy timeout on the GUI thread), and stops saying so
// when the scan is over.
TEST_F(DeckLabImportTest, KeptTracksWaitFromBeforeEachBatchsFirstWrite) {
    QTemporaryDir dir;
    constexpr int kFiles = 25; // more than one batch
    for (int n = 1; n <= kFiles; ++n) {
        copyFixture(dir, QStringLiteral("Generated %1.mp3").arg(n, 2, 10, QLatin1Char('0')));
    }
    ASSERT_EQ(DirectoryDAO::AddResult::Ok, trackCollectionManager()->addDirectory(mixxx::FileInfo(dir.path())));
    LibraryScanner scanner(dbConnectionPooler(), config());
    std::atomic<int> commits{0};
    QObject::connect(&scannerDao(scanner), &TrackDAO::tracksAdded, &scannerDao(scanner), [&](const QSet<TrackId>&) {
        ++commits;
    }, Qt::DirectConnection);
    const std::function<bool()> scannersHook = beforeCacheLookup(scanner);
    ASSERT_TRUE(scannersHook);
    std::atomic<int> adds{0};
    std::atomic<int> notHeld{0};
    beforeCacheLookup(scanner) = [&] {
        // Right before the track is written.
        ++adds;
        if (!GlobalTrackCache::areLibraryWritesHeld()) {
            ++notHeld;
        }
        return scannersHook();
    };
    QSignalSpy finished(&scanner, &LibraryScanner::scanFinished);
    scanner.start();
    ASSERT_TRUE(scanner.scanQuietly());
    ASSERT_TRUE(finished.wait(30000));
    EXPECT_EQ(kFiles, adds.load());
    EXPECT_GE(commits.load(), 2) << "one batch only";
    EXPECT_EQ(0, notHeld.load()) << "a write while kept tracks could be saved";
    EXPECT_FALSE(GlobalTrackCache::areLibraryWritesHeld()) << "after the scan";
    EXPECT_EQ(kFiles, value(QStringLiteral("SELECT COUNT(*) FROM library")).toInt());
}

// The real scanner, a stick that reads slowly, and the GUI looking up a
// track it does not have cached while the scan reads: the lookup is answered
// at once, not after the read.
TEST_F(DeckLabImportTest, AGuiLookupDuringAScanDoesNotWaitForTheStick) {
    QTemporaryDir dir;
    constexpr int kFiles = 3;
    for (int n = 1; n <= kFiles; ++n) {
        copyFixture(dir, QStringLiteral("Generated %1.mp3").arg(n));
    }
    QTemporaryDir elsewhere;
    const QString browsed = copyFixture(elsewhere, QStringLiteral("Generated Browsed.mp3"));
    ASSERT_EQ(DirectoryDAO::AddResult::Ok, trackCollectionManager()->addDirectory(mixxx::FileInfo(dir.path())));
    LibraryScanner scanner(dbConnectionPooler(), config());
    std::atomic<bool> reading{false};
    onScannerReading(scanner, [&](const QString&) {
        reading = true;
        QThread::msleep(400); // a slow stick
        reading = false;
    });
    QSignalSpy finished(&scanner, &LibraryScanner::scanFinished);
    scanner.start();
    ASSERT_TRUE(scanner.scanQuietly());
    ASSERT_TRUE(QTest::qWaitFor([&reading] { return reading.load(); }, 10000));
    // The lookup on a thread of its own (a track that is not kept leaves the
    // cache through the GUI thread's event loop).
    auto lookup = std::async(std::launch::async, [browsed] {
        const auto resolver = GlobalTrackCacheResolver(mixxx::FileAccess(mixxx::FileInfo(browsed)));
        return resolver.getLookupResult();
    });
    EXPECT_EQ(std::future_status::ready, lookup.wait_for(std::chrono::milliseconds(200)))
            << "the GUI waited for the scanner reading the stick";
    EXPECT_EQ(GlobalTrackCacheLookupResult::Miss, lookup.get());
    ASSERT_TRUE(finished.wait(30000));
    EXPECT_EQ(kFiles, value(QStringLiteral("SELECT COUNT(*) FROM library")).toInt());
    QCoreApplication::processEvents(); // the browsed track leaves the cache
}

// A stick that reads quickly and then stalls in one read (a worn stick, or
// three readers at once): the scanner holds no write lock through that read
// either. Each file is read only while the scanner's add transaction holds
// no write; what was read is written in a short batch after. A write from
// another connection at the start of every read goes through at once (the
// test library fails it at once if the scanner holds the lock).
TEST_F(DeckLabImportTest, AStallAfterFastReadsHoldsNoWriteLock) {
    QTemporaryDir dir;
    constexpr int kFiles = 6;
    for (int n = 1; n <= kFiles; ++n) {
        copyFixture(dir, QStringLiteral("Generated %1.mp3").arg(n));
    }
    ASSERT_EQ(DirectoryDAO::AddResult::Ok, trackCollectionManager()->addDirectory(mixxx::FileInfo(dir.path())));
    value(QStringLiteral("CREATE TABLE decklab_generated_writes (n INTEGER)"));
    const QString main = internalCollection()->database().connectionName();
    LibraryScanner scanner(dbConnectionPooler(), config());
    std::atomic<int> reads{0};
    std::atomic<int> refused{0};
    std::atomic<bool> refusedInStall{false};
    onScannerReading(scanner, [&](const QString& location) {
        const bool stall = location.endsWith(QStringLiteral("Generated 4.mp3"));
        const auto probe = [&main]() {
            const QString name = QStringLiteral("decklab-writer");
            bool written = false;
            {
                QSqlDatabase other = QSqlDatabase::cloneDatabase(main, name);
                if (other.open()) {
                    QSqlQuery write(other);
                    written = write.exec(QStringLiteral("INSERT INTO decklab_generated_writes (n) VALUES (1)"));
                    other.close();
                }
            }
            QSqlDatabase::removeDatabase(name);
            return written;
        };
        if (!probe()) {
            ++refused;
            refusedInStall = refusedInStall || stall;
        }
        ++reads;
        if (stall) {
            QThread::msleep(300); // the stick stalls in this read only
            if (!probe()) {
                ++refused;
                refusedInStall = true;
            }
        }
    });
    QSignalSpy finished(&scanner, &LibraryScanner::scanFinished);
    scanner.start();
    ASSERT_TRUE(scanner.scanQuietly());
    ASSERT_TRUE(finished.wait(30000));
    EXPECT_EQ(kFiles, reads.load());
    EXPECT_FALSE(refusedInStall.load()) << "the scanner held the write lock through the stalled read";
    EXPECT_EQ(0, refused.load()) << "a write had to wait for the scanner reading a file";
    EXPECT_EQ(kFiles + 1, value(QStringLiteral("SELECT COUNT(*) FROM decklab_generated_writes")).toInt());
    EXPECT_EQ(kFiles, value(QStringLiteral("SELECT COUNT(*) FROM library")).toInt());
}

// The scanner turns on the keeping of modified tracks while it writes (the
// only guard against saving under the cache's lock while the scanner waits
// for that lock with the library's write lock held). A modified track let
// go mid-scan is kept, and saved at the next batch's commit, before the
// scan ends: a power cut later in the scan loses no edit.
TEST_F(DeckLabImportTest, AModifiedTrackLetGoMidScanIsSavedAtTheNextCommit) {
    QTemporaryDir dir;
    constexpr int kFiles = 6;
    for (int n = 1; n <= kFiles; ++n) {
        copyFixture(dir, QStringLiteral("Generated %1.mp3").arg(n));
    }
    QTemporaryDir elsewhere;
    TrackPointer edited = getOrAddTrackByLocation(copyFixture(elsewhere, QStringLiteral("Generated Edited.mp3")));
    ASSERT_TRUE(edited);
    const QString title = QStringLiteral("SELECT title FROM library WHERE id=%1").arg(edited->getId().toString());
    edited->setTitle(QStringLiteral("Generated Edit"));
    ASSERT_TRUE(edited->isDirty());
    ASSERT_EQ(DirectoryDAO::AddResult::Ok, trackCollectionManager()->addDirectory(mixxx::FileInfo(dir.path())));
    const QString main = internalCollection()->database().connectionName();
    LibraryScanner scanner(dbConnectionPooler(), config());
    std::atomic<int> reads{0};
    std::atomic<bool> deferred{false};
    std::atomic<bool> savedMidScan{false};
    onScannerReading(scanner, [&](const QString&) {
        const int read = ++reads;
        if (read == 1) {
            deferred = GlobalTrackCache::isSavingModifiedTracksDeferred();
            edited.reset(); // its last reference goes (a deck loads another track)
        } else if (read == kFiles) {
            const QString name = QStringLiteral("decklab-reader");
            {
                QSqlDatabase other = QSqlDatabase::cloneDatabase(main, name);
                if (other.open()) {
                    QSqlQuery query(other);
                    savedMidScan = query.exec(title) && query.next() &&
                            query.value(0).toString() == QStringLiteral("Generated Edit");
                    other.close();
                }
            }
            QSqlDatabase::removeDatabase(name);
        }
        QThread::msleep(250); // a slow stick: a batch is committed meanwhile
    });
    QSignalSpy finished(&scanner, &LibraryScanner::scanFinished);
    scanner.start();
    ASSERT_TRUE(scanner.scanQuietly());
    ASSERT_TRUE(finished.wait(30000));
    EXPECT_TRUE(deferred.load()) << "the scan did not keep modified tracks while it wrote";
    EXPECT_TRUE(savedMidScan.load()) << "the edit waited for the end of the scan";
    QCoreApplication::processEvents();
    EXPECT_EQ(QStringLiteral("Generated Edit"), value(title).toString());
    EXPECT_FALSE(GlobalTrackCache::isSavingModifiedTracksDeferred());
}

// A file moved inside a library folder, and a scan canceled for playback
// after it committed the file's new row. The next scan finds that row
// present (not added by it), yet still relinks the old row (with its cues,
// crate and play count) to the new place, as an uncanceled scan would.
TEST_F(DeckLabImportTest, AMovedFileIsRelinkedAfterACanceledScan) {
    QTemporaryDir dir;
    const QDir root(dir.path());
    ASSERT_TRUE(root.mkpath(QStringLiteral("A")));
    ASSERT_TRUE(root.mkpath(QStringLiteral("B")));
    const QString before = copyFixture(dir, QStringLiteral("A/Generated 00 Moved.mp3"));
    ASSERT_EQ(DirectoryDAO::AddResult::Ok, trackCollectionManager()->addDirectory(mixxx::FileInfo(dir.path())));
    LibraryScanner scanner(dbConnectionPooler(), config());
    std::atomic<bool> canceled{true};
    QObject::connect(&scanner, &LibraryScanner::scanSummary, &scanner, [&canceled](const LibraryScanResultSummary& result) {
        canceled = result.canceled;
    }, Qt::DirectConnection);
    QSignalSpy finished(&scanner, &LibraryScanner::scanFinished);
    scanner.start();
    ASSERT_TRUE(scanner.scanQuietly());
    ASSERT_TRUE(finished.wait(30000));
    ASSERT_FALSE(canceled.load());
    const int id = value(QStringLiteral("SELECT library.id FROM library INNER JOIN track_locations "
                                        "ON library.location=track_locations.id WHERE track_locations.location='%1'")
                                 .arg(mixxx::FileInfo(before).location()))
                           .toInt();
    ASSERT_GT(id, 0);
    value(QStringLiteral("INSERT INTO crates (name) VALUES ('Generated Crate')"));
    value(QStringLiteral("INSERT INTO crate_tracks (crate_id, track_id) "
                         "SELECT id, %1 FROM crates WHERE name='Generated Crate'")
                    .arg(id));
    value(QStringLiteral("INSERT INTO cues (track_id, type, position, hotcue) VALUES (%1, 1, 1000, 0)").arg(id));
    value(QStringLiteral("UPDATE library SET timesplayed=3 WHERE id=%1").arg(id));

    // Moved on a laptop, with more new files beside it; the scan is
    // canceled as soon as the batch with the moved file is committed.
    const QString after = root.filePath(QStringLiteral("B/Generated 00 Moved.mp3"));
    ASSERT_TRUE(QFile::rename(before, after));
    constexpr int kOthers = 5;
    for (int n = 1; n <= kOthers; ++n) {
        copyFixture(dir, QStringLiteral("B/Generated %1.mp3").arg(n, 2, 10, QLatin1Char('0')));
    }
    std::atomic<bool> movedRead{false};
    onScannerReading(scanner, [&](const QString& location) {
        if (location.endsWith(QStringLiteral("B/Generated 00 Moved.mp3"))) {
            movedRead = true;
        }
    });
    QObject::connect(&scannerDao(scanner), &TrackDAO::tracksAdded, &scannerDao(scanner), [&](const QSet<TrackId>&) {
        if (movedRead) {
            scanner.requestCancel(); // a deck started
        }
    }, Qt::DirectConnection);
    ASSERT_TRUE(QTest::qWaitFor([&scanner] { return !scanner.isScanning(); }, 5000));
    ASSERT_TRUE(scanner.scanQuietly());
    ASSERT_TRUE(finished.wait(30000));
    ASSERT_TRUE(canceled.load());
    const QString newRow = QStringLiteral("SELECT COUNT(*) FROM track_locations WHERE location='%1'")
                                   .arg(mixxx::FileInfo(after).location());
    ASSERT_EQ(1, value(newRow).toInt()) << "the moved file's new row was committed before the cancel";

    // Quiet again: the next scan finishes.
    onScannerReading(scanner, {});
    movedRead = false;
    ASSERT_TRUE(QTest::qWaitFor([&scanner] { return !scanner.isScanning(); }, 5000));
    ASSERT_TRUE(scanner.scanQuietly());
    ASSERT_TRUE(finished.wait(30000));
    ASSERT_FALSE(canceled.load());
    EXPECT_EQ(mixxx::FileInfo(after).location(),
            value(QStringLiteral("SELECT track_locations.location FROM library INNER JOIN track_locations "
                                 "ON library.location=track_locations.id WHERE library.id=%1")
                            .arg(id))
                    .toString())
            << "the old row moved with its file";
    EXPECT_EQ(0, value(QStringLiteral("SELECT fs_deleted FROM track_locations WHERE location='%1'")
                               .arg(mixxx::FileInfo(after).location()))
                         .toInt());
    EXPECT_EQ(1, value(QStringLiteral("SELECT COUNT(*) FROM library WHERE location IN "
                                      "(SELECT id FROM track_locations WHERE filename='Generated 00 Moved.mp3')"))
                         .toInt())
            << "one row for the file, not a bare new one beside a missing old one";
    EXPECT_EQ(1, value(QStringLiteral("SELECT COUNT(*) FROM crate_tracks WHERE track_id=%1").arg(id)).toInt());
    EXPECT_EQ(1, value(QStringLiteral("SELECT COUNT(*) FROM cues WHERE track_id=%1").arg(id)).toInt());
    EXPECT_EQ(3, value(QStringLiteral("SELECT timesplayed FROM library WHERE id=%1").arg(id)).toInt());
    EXPECT_EQ(kOthers + 1, value(QStringLiteral("SELECT COUNT(*) FROM library")).toInt());
    EXPECT_EQ(0, value(QStringLiteral("SELECT COUNT(*) FROM settings WHERE name LIKE 'decklab.scan.%'")).toInt())
            << "every scan has finished";
}

// Canceled for playback (LibraryScanner::requestCancel, which never waits):
// what it committed stays, each track whole; what it had read but not
// written is left; nothing is marked missing; the next scan goes on where it
// stopped and adds no row twice.
TEST_F(DeckLabImportTest, ACanceledScanLeavesWholeRowsAndGoesOnNextTime) {
    QTemporaryDir dir;
    constexpr int kFiles = 45;
    for (int n = 1; n <= kFiles; ++n) {
        copyFixture(dir, QStringLiteral("Generated %1.mp3").arg(n, 2, 10, QLatin1Char('0')));
    }
    ASSERT_EQ(DirectoryDAO::AddResult::Ok, trackCollectionManager()->addDirectory(mixxx::FileInfo(dir.path())));
    LibraryScanner scanner(dbConnectionPooler(), config());
    std::atomic<int> read{0};
    onScannerReading(scanner, [&](const QString&) {
        if (++read == 25) {
            scanner.requestCancel(); // a deck started (any thread)
        }
    });
    std::atomic<bool> canceled{false};
    QObject::connect(&scanner, &LibraryScanner::scanSummary, &scanner, [&canceled](const LibraryScanResultSummary& result) {
        canceled = result.canceled;
    }, Qt::DirectConnection);
    QSignalSpy finished(&scanner, &LibraryScanner::scanFinished);
    scanner.start();
    ASSERT_TRUE(scanner.scanQuietly());
    ASSERT_TRUE(finished.wait(30000));
    EXPECT_TRUE(canceled.load());
    // The first batch (20 files) was written before the cancel; files read
    // after it never are.
    const int rows = value(QStringLiteral("SELECT COUNT(*) FROM library")).toInt();
    EXPECT_GE(rows, 20);
    EXPECT_LT(rows, 25);
    EXPECT_EQ(rows, value(QStringLiteral("SELECT COUNT(*) FROM library INNER JOIN track_locations "
                                         "ON library.location=track_locations.id")).toInt())
            << "every row whole";
    EXPECT_EQ(rows, value(QStringLiteral("SELECT COUNT(*) FROM track_locations")).toInt());
    EXPECT_EQ(0, value(QStringLiteral("SELECT COUNT(*) FROM track_locations WHERE fs_deleted=1")).toInt())
            << "a canceled scan marks nothing missing";
    EXPECT_FALSE(GlobalTrackCache::isSavingModifiedTracksDeferred());

    // Quiet again: the next scan reads the folder it had not finished.
    onScannerReading(scanner, {});
    ASSERT_TRUE(QTest::qWaitFor([&scanner] { return !scanner.isScanning(); }, 5000));
    ASSERT_TRUE(scanner.scanQuietly());
    ASSERT_TRUE(finished.wait(30000));
    EXPECT_FALSE(canceled.load());
    EXPECT_EQ(kFiles, value(QStringLiteral("SELECT COUNT(*) FROM library")).toInt());
    EXPECT_EQ(kFiles, value(QStringLiteral("SELECT COUNT(DISTINCT location) FROM track_locations")).toInt());
    EXPECT_EQ(0, value(QStringLiteral("SELECT COUNT(*) FROM track_locations WHERE fs_deleted=1")).toInt());
}

// A scan canceled for playback leaves the rows it inserted to the moved-file
// relink of the next scan that finishes, and nothing else. A TO PAD sample
// and a track loaded from Browse (rows the GUI adds after the cancel), and a
// row the canceled scan inserted that was analysed or is loaded since, stay
// as they are, even when a missing row (a stick that is out) has their file
// name and length. A file the canceled scan found moved is still relinked.
TEST_F(DeckLabImportTest, AfterACanceledScanOnlyItsUnusedRowsAreRelinked) {
    QTemporaryDir dir;
    const QDir root(dir.path());
    ASSERT_TRUE(root.mkpath(QStringLiteral("A")));
    ASSERT_TRUE(root.mkpath(QStringLiteral("B")));
    ASSERT_TRUE(root.mkpath(QStringLiteral("Samples")));
    // Rows of a stick that is out, with the same file names (and lengths).
    QTemporaryDir away;
    const QStringList names{
            QStringLiteral("Generated Pad.mp3"),
            QStringLiteral("Generated Plain.mp3"),
            QStringLiteral("Generated Browsed.mp3"),
            QStringLiteral("Generated Analysed.mp3"),
            QStringLiteral("Generated Held.mp3")};
    for (const QString& name : names) {
        ASSERT_TRUE(getOrAddTrackByLocation(copyFixture(away, name)));
    }
    QCoreApplication::processEvents(); // they leave the cache
    for (const QString& name : names) {
        ASSERT_TRUE(QFile::remove(QDir(away.path()).filePath(name)));
    }
    const QString awayRows = QStringLiteral("SELECT COUNT(*) FROM track_locations WHERE fs_deleted=1 AND location LIKE '%1/%'")
                                     .arg(mixxx::FileInfo(away.path()).location());
    const QString before = copyFixture(dir, QStringLiteral("A/Generated 00 Moved.mp3"));
    ASSERT_EQ(DirectoryDAO::AddResult::Ok, trackCollectionManager()->addDirectory(mixxx::FileInfo(dir.path())));
    LibraryScanner scanner(dbConnectionPooler(), config());
    std::atomic<bool> canceled{true};
    QObject::connect(&scanner, &LibraryScanner::scanSummary, &scanner, [&canceled](const LibraryScanResultSummary& result) {
        canceled = result.canceled;
    }, Qt::DirectConnection);
    QSignalSpy finished(&scanner, &LibraryScanner::scanFinished);
    scanner.start();
    ASSERT_TRUE(scanner.scanQuietly());
    ASSERT_TRUE(finished.wait(30000));
    ASSERT_FALSE(canceled.load());
    ASSERT_EQ(names.size(), value(awayRows).toInt()) << "the stick's rows are missing";
    const auto idAt = [this](const QString& path) {
        return value(QStringLiteral("SELECT library.id FROM library INNER JOIN track_locations "
                                    "ON library.location=track_locations.id WHERE track_locations.location='%1'")
                             .arg(mixxx::FileInfo(path).location()))
                .toInt();
    };
    const int movedId = idAt(before);
    ASSERT_GT(movedId, 0);
    value(QStringLiteral("INSERT INTO cues (track_id, type, position, hotcue) VALUES (%1, 1, 1000, 0)").arg(movedId));

    // Moved, with two new files beside it; canceled once all three are in.
    const QString after = root.filePath(QStringLiteral("B/Generated 00 Moved.mp3"));
    ASSERT_TRUE(QFile::rename(before, after));
    const QString analysed = copyFixture(dir, QStringLiteral("B/Generated Analysed.mp3"));
    const QString held = copyFixture(dir, QStringLiteral("B/Generated Held.mp3"));
    std::atomic<int> readInB{0};
    onScannerReading(scanner, [&](const QString& location) {
        if (location.contains(QStringLiteral("/B/"))) {
            ++readInB;
        }
    });
    QObject::connect(&scannerDao(scanner), &TrackDAO::tracksAdded, &scannerDao(scanner), [&](const QSet<TrackId>&) {
        if (readInB.load() >= 3) {
            scanner.requestCancel(); // a deck started
        }
    }, Qt::DirectConnection);
    ASSERT_TRUE(QTest::qWaitFor([&scanner] { return !scanner.isScanning(); }, 5000));
    ASSERT_TRUE(scanner.scanQuietly());
    ASSERT_TRUE(finished.wait(30000));
    ASSERT_TRUE(canceled.load());
    const int newMovedId = idAt(after);
    const int analysedId = idAt(analysed);
    const int heldId = idAt(held);
    ASSERT_GT(newMovedId, 0);
    ASSERT_GT(analysedId, 0);
    ASSERT_GT(heldId, 0);
    EXPECT_GE(value(QStringLiteral("SELECT COUNT(*) FROM settings WHERE name LIKE 'decklab.scan.added.%'")).toInt(), 1)
            << "the canceled scan's rows are recorded";

    // Mid-set, after the cancel: one of its rows is analysed, another is on
    // a deck; TO PAD saves a sample and a track is loaded from Browse, both
    // inside the library folders.
    value(QStringLiteral("UPDATE library SET beats_sub_version='generated=1' WHERE id=%1").arg(analysedId));
    TrackPointer onDeck = getOrAddTrackByLocation(held);
    ASSERT_TRUE(onDeck);
    ASSERT_EQ(heldId, onDeck->getId().toVariant().toInt());
    // One more TO PAD sample, with nothing of its own (no locked grid, no
    // analysis): only rows the canceled scan inserted are relinked.
    const QString pad = copyFixture(dir, QStringLiteral("Samples/Generated Pad.mp3"));
    const QString plain = copyFixture(dir, QStringLiteral("Samples/Generated Plain.mp3"));
    const QString browsed = copyFixture(dir, QStringLiteral("B/Generated Browsed.mp3"));
    ASSERT_TRUE(getOrAddTrackByLocation(pad));
    ASSERT_TRUE(getOrAddTrackByLocation(plain));
    ASSERT_TRUE(getOrAddTrackByLocation(browsed));
    QCoreApplication::processEvents(); // they leave the cache
    const int padId = idAt(pad);
    const int plainId = idAt(plain);
    const int browsedId = idAt(browsed);
    ASSERT_GT(padId, 0);
    ASSERT_GT(plainId, 0);
    ASSERT_GT(browsedId, 0);
    value(QStringLiteral("UPDATE library SET bpm_lock=1 WHERE id=%1").arg(padId));
    value(QStringLiteral("INSERT INTO track_analysis (track_id, type, description, version, data_checksum) "
                         "VALUES (%1, 0, 'Generated', 'Generated', '0')")
                    .arg(browsedId));

    // Quiet again: the next scan finishes.
    onScannerReading(scanner, {});
    readInB = -1000;
    ASSERT_TRUE(QTest::qWaitFor([&scanner] { return !scanner.isScanning(); }, 5000));
    ASSERT_TRUE(scanner.scanQuietly());
    ASSERT_TRUE(finished.wait(30000));
    ASSERT_FALSE(canceled.load());
    EXPECT_EQ(movedId, idAt(after)) << "the old row moved with its file";
    EXPECT_EQ(0, value(QStringLiteral("SELECT COUNT(*) FROM library WHERE id=%1").arg(newMovedId)).toInt());
    EXPECT_EQ(1, value(QStringLiteral("SELECT COUNT(*) FROM cues WHERE track_id=%1").arg(movedId)).toInt());
    EXPECT_EQ(padId, idAt(pad)) << "the TO PAD sample keeps its row";
    EXPECT_EQ(1, value(QStringLiteral("SELECT bpm_lock FROM library WHERE id=%1").arg(padId)).toInt());
    EXPECT_EQ(plainId, idAt(plain)) << "the plain TO PAD sample keeps its row";
    EXPECT_EQ(browsedId, idAt(browsed)) << "the Browse load keeps its row";
    EXPECT_EQ(1, value(QStringLiteral("SELECT COUNT(*) FROM track_analysis WHERE track_id=%1").arg(browsedId)).toInt());
    EXPECT_EQ(analysedId, idAt(analysed)) << "the analysed row is left alone";
    EXPECT_EQ(QStringLiteral("generated=1"),
            value(QStringLiteral("SELECT beats_sub_version FROM library WHERE id=%1").arg(analysedId)).toString());
    EXPECT_EQ(heldId, idAt(held)) << "the row on a deck is left alone";
    EXPECT_EQ(heldId, onDeck->getId().toVariant().toInt());
    EXPECT_EQ(names.size(), value(awayRows).toInt()) << "the stick's rows wait for the stick";
    EXPECT_EQ(0, value(QStringLiteral("SELECT COUNT(*) FROM settings WHERE name LIKE 'decklab.scan.%'")).toInt())
            << "every scan has finished";
    onDeck.reset();
    QCoreApplication::processEvents();
}

// Files moved on the stick, read by a scan that finishes. Each batch it
// commits shows in Browse while it goes on: the DJ loads one of the moved
// files on a deck (and keeps it there), and another one that is analysed on
// load. Neither row is deleted for its file's old missing row (the deck's
// track keeps its id, the analysis its track), and the old rows stay
// missing; the moved file nobody touched is still relinked.
TEST_F(DeckLabImportTest, AMovedFileInUseDuringTheScanKeepsItsNewRow) {
    QTemporaryDir dir;
    const QDir root(dir.path());
    ASSERT_TRUE(root.mkpath(QStringLiteral("A")));
    ASSERT_TRUE(root.mkpath(QStringLiteral("B")));
    const QString moved = QStringLiteral("Generated Moved.mp3");
    const QString held = QStringLiteral("Generated Held.mp3");
    const QString analysed = QStringLiteral("Generated Analysed.mp3");
    const QStringList names{moved, held, analysed};
    const auto at = [&root](const QString& folder, const QString& name) {
        return root.filePath(folder + QLatin1Char('/') + name);
    };
    for (const QString& name : names) {
        copyFixture(dir, QStringLiteral("A/") + name);
    }
    ASSERT_EQ(DirectoryDAO::AddResult::Ok, trackCollectionManager()->addDirectory(mixxx::FileInfo(dir.path())));
    LibraryScanner scanner(dbConnectionPooler(), config());
    std::atomic<bool> canceled{true};
    std::atomic<int> scans{0};
    QObject::connect(&scanner, &LibraryScanner::scanSummary, &scanner, [&](const LibraryScanResultSummary& result) {
        canceled = result.canceled;
        ++scans;
    }, Qt::DirectConnection);
    scanner.start();
    ASSERT_TRUE(scanner.scanQuietly());
    ASSERT_TRUE(QTest::qWaitFor([&scans] { return scans.load() == 1; }, 30000));
    ASSERT_FALSE(canceled.load());
    const auto idAt = [this](const QString& path) {
        return value(QStringLiteral("SELECT library.id FROM library INNER JOIN track_locations "
                                    "ON library.location=track_locations.id WHERE track_locations.location='%1'")
                             .arg(mixxx::FileInfo(path).location()))
                .toInt();
    };
    const int oldMovedId = idAt(at(QStringLiteral("A"), moved));
    const int oldHeldId = idAt(at(QStringLiteral("A"), held));
    const int oldAnalysedId = idAt(at(QStringLiteral("A"), analysed));
    ASSERT_GT(oldMovedId, 0);
    ASSERT_GT(oldHeldId, 0);
    ASSERT_GT(oldAnalysedId, 0);
    for (const QString& name : names) {
        ASSERT_TRUE(QFile::rename(at(QStringLiteral("A"), name), at(QStringLiteral("B"), name)));
    }

    // The scan stops once the rows of the two files the DJ wants are
    // committed, until the GUI has them (10 s at most).
    std::atomic<int> wantedRead{0};
    onScannerReading(scanner, [&](const QString& location) {
        if (location.endsWith(QStringLiteral("/B/") + held) ||
                location.endsWith(QStringLiteral("/B/") + analysed)) {
            ++wantedRead;
        }
    });
    std::atomic<bool> committed{false};
    std::atomic<bool> inUse{false};
    QObject::connect(&scannerDao(scanner), &TrackDAO::tracksAdded, &scannerDao(scanner), [&](const QSet<TrackId>&) {
        if (wantedRead.load() >= 2 && !committed.exchange(true)) {
            for (int ms = 0; ms < 10000 && !inUse.load(); ms += 10) {
                QThread::msleep(10);
            }
        }
    }, Qt::DirectConnection);
    ASSERT_TRUE(QTest::qWaitFor([&scanner] { return !scanner.isScanning(); }, 5000));
    ASSERT_TRUE(scanner.scanQuietly());
    ASSERT_TRUE(QTest::qWaitFor([&committed] { return committed.load(); }, 30000));
    TrackPointer onDeck = getOrAddTrackByLocation(at(QStringLiteral("B"), held));
    ASSERT_TRUE(onDeck);
    const int heldId = onDeck->getId().toVariant().toInt();
    TrackPointer loaded = getOrAddTrackByLocation(at(QStringLiteral("B"), analysed));
    ASSERT_TRUE(loaded);
    const int analysedId = loaded->getId().toVariant().toInt();
    EXPECT_NE(oldHeldId, heldId) << "a new row";
    EXPECT_NE(oldAnalysedId, analysedId) << "a new row";
    value(QStringLiteral("INSERT INTO track_analysis (track_id, type, description, version, data_checksum) "
                         "VALUES (%1, 0, 'Generated', 'Generated', '0')")
                    .arg(analysedId));
    loaded.reset(); // the deck loads the next track
    inUse = true;
    ASSERT_TRUE(QTest::qWaitFor([&scans] { return scans.load() == 2; }, 30000));
    ASSERT_FALSE(canceled.load());

    EXPECT_EQ(oldMovedId, idAt(at(QStringLiteral("B"), moved))) << "the untouched moved file is relinked";
    EXPECT_EQ(heldId, idAt(at(QStringLiteral("B"), held))) << "the row on a deck was deleted";
    EXPECT_EQ(heldId, onDeck->getId().toVariant().toInt());
    EXPECT_EQ(analysedId, idAt(at(QStringLiteral("B"), analysed))) << "the analysed row was deleted";
    EXPECT_EQ(1, value(QStringLiteral("SELECT COUNT(*) FROM track_analysis WHERE track_id=%1").arg(analysedId)).toInt());
    for (const int oldId : {oldHeldId, oldAnalysedId}) {
        EXPECT_EQ(1, value(QStringLiteral("SELECT track_locations.fs_deleted FROM library INNER JOIN track_locations "
                                          "ON library.location=track_locations.id WHERE library.id=%1")
                                   .arg(oldId))
                             .toInt())
                << "the old row stays missing";
    }
    EXPECT_EQ(5, value(QStringLiteral("SELECT COUNT(*) FROM library")).toInt());
    onDeck.reset();
    QCoreApplication::processEvents();
}

// A batch whose commit fails stays pending: it is committed before the next
// file is read, so no read ever holds the library's write lock, and the scan
// goes on and adds everything. A write from another connection at the start
// of every read goes through at once (the test library fails it at once if
// the scanner holds the lock).
TEST_F(DeckLabImportTest, AFailedBatchCommitIsTriedAgainBeforeTheNextRead) {
    QTemporaryDir dir;
    constexpr int kFiles = 6;
    for (int n = 1; n <= kFiles; ++n) {
        copyFixture(dir, QStringLiteral("Generated %1.mp3").arg(n));
    }
    ASSERT_EQ(DirectoryDAO::AddResult::Ok, trackCollectionManager()->addDirectory(mixxx::FileInfo(dir.path())));
    value(QStringLiteral("CREATE TABLE decklab_generated_writes (n INTEGER)"));
    const QString main = internalCollection()->database().connectionName();
    LibraryScanner scanner(dbConnectionPooler(), config());
    std::atomic<int> failing{2};
    std::atomic<int> failed{0};
    failCommits(scanner, [&] {
        if (failing.fetch_sub(1) > 0) {
            ++failed;
            return true; // a busy or broken library
        }
        return false;
    });
    std::atomic<int> reads{0};
    std::atomic<int> refused{0};
    onScannerReading(scanner, [&](const QString&) {
        const QString name = QStringLiteral("decklab-writer");
        {
            QSqlDatabase other = QSqlDatabase::cloneDatabase(main, name);
            if (!other.open()) {
                ++refused;
            } else {
                QSqlQuery write(other);
                if (!write.exec(QStringLiteral("INSERT INTO decklab_generated_writes (n) VALUES (1)"))) {
                    ++refused;
                }
                other.close();
            }
        }
        QSqlDatabase::removeDatabase(name);
        ++reads;
        QThread::msleep(250); // a slow stick: batches are written meanwhile
    });
    std::atomic<bool> canceled{true};
    QObject::connect(&scanner, &LibraryScanner::scanSummary, &scanner, [&canceled](const LibraryScanResultSummary& result) {
        canceled = result.canceled;
    }, Qt::DirectConnection);
    QSignalSpy finished(&scanner, &LibraryScanner::scanFinished);
    scanner.start();
    ASSERT_TRUE(scanner.scanQuietly());
    ASSERT_TRUE(finished.wait(30000));
    EXPECT_EQ(2, failed.load()) << "no commit was tried";
    EXPECT_FALSE(canceled.load());
    EXPECT_EQ(kFiles, reads.load());
    EXPECT_EQ(0, refused.load()) << "a file was read with the failed batch's write held";
    EXPECT_EQ(kFiles, value(QStringLiteral("SELECT COUNT(*) FROM library")).toInt());
}

// A batch that cannot be committed at all: the scan stops instead of reading
// the next file with the write held, and says it was canceled (it runs again
// later).
TEST_F(DeckLabImportTest, AScanWhoseBatchCannotBeCommittedStopsBeforeTheNextRead) {
    QTemporaryDir dir;
    constexpr int kFiles = 6;
    for (int n = 1; n <= kFiles; ++n) {
        copyFixture(dir, QStringLiteral("Generated %1.mp3").arg(n));
    }
    ASSERT_EQ(DirectoryDAO::AddResult::Ok, trackCollectionManager()->addDirectory(mixxx::FileInfo(dir.path())));
    value(QStringLiteral("CREATE TABLE decklab_generated_writes (n INTEGER)"));
    const QString main = internalCollection()->database().connectionName();
    LibraryScanner scanner(dbConnectionPooler(), config());
    std::atomic<int> failed{0};
    failCommits(scanner, [&failed] {
        ++failed;
        return true;
    });
    std::atomic<int> reads{0};
    std::atomic<int> refused{0};
    onScannerReading(scanner, [&](const QString&) {
        const QString name = QStringLiteral("decklab-writer");
        {
            QSqlDatabase other = QSqlDatabase::cloneDatabase(main, name);
            if (!other.open()) {
                ++refused;
            } else {
                QSqlQuery write(other);
                if (!write.exec(QStringLiteral("INSERT INTO decklab_generated_writes (n) VALUES (1)"))) {
                    ++refused;
                }
                other.close();
            }
        }
        QSqlDatabase::removeDatabase(name);
        ++reads;
        QThread::msleep(250); // a slow stick: a batch is written meanwhile
    });
    std::atomic<bool> canceled{false};
    QObject::connect(&scanner, &LibraryScanner::scanSummary, &scanner, [&canceled](const LibraryScanResultSummary& result) {
        canceled = result.canceled;
    }, Qt::DirectConnection);
    QSignalSpy finished(&scanner, &LibraryScanner::scanFinished);
    scanner.start();
    ASSERT_TRUE(scanner.scanQuietly());
    ASSERT_TRUE(finished.wait(30000));
    EXPECT_GE(failed.load(), 3) << "the commit was tried before the next read";
    EXPECT_TRUE(canceled.load());
    EXPECT_LT(reads.load(), kFiles) << "the scan read on with the write held";
    EXPECT_EQ(0, refused.load()) << "a file was read with the failed batch's write held";
    EXPECT_FALSE(GlobalTrackCache::isSavingModifiedTracksDeferred());
}

// A modified track kept during a scan may be in use again (a deck, a
// preview, on the stick the scanner reads): it is saved mid-scan without
// writing its file, even with tag export on. Its tags are written once its
// last reference goes, after the scan.
TEST_F(DeckLabImportTest, AKeptTrackIsSavedMidScanWithoutWritingItsFile) {
    config()->setValue(mixxx::library::prefs::kSyncTrackMetadataConfigKey, 1);
    const auto exportOff = qScopeGuard([this] {
        config()->setValue(mixxx::library::prefs::kSyncTrackMetadataConfigKey, 0);
    });
    QTemporaryDir dir;
    const QString file = copyFixture(dir, QStringLiteral("Generated Exported.mp3"));
    const QString tagged = fileTitle(file);
    ASSERT_NE(QStringLiteral("Generated Edit"), tagged);
    TrackPointer track = getOrAddTrackByLocation(file);
    ASSERT_TRUE(track);
    const TrackId id = track->getId();
    const QString title = QStringLiteral("SELECT title FROM library WHERE id=%1").arg(id.toString());
    track->setTitle(QStringLiteral("Generated Edit"));
    DeferredSaves deferred;
    track.reset(); // a deck loads another track
    QCoreApplication::processEvents();
    GlobalTrackCache::saveDeferredTracksSoon(); // a batch is committed
    QCoreApplication::processEvents();
    EXPECT_EQ(QStringLiteral("Generated Edit"), value(title).toString()) << "saved mid-scan";
    EXPECT_EQ(tagged, fileTitle(file)) << "its file was written while it may be in use";
    TrackPointer kept;
    {
        const GlobalTrackCacheLocker locker;
        kept = locker.lookupTrackById(id);
    }
    ASSERT_TRUE(kept) << "kept for its tag export";
    EXPECT_FALSE(kept->isDirty());
    EXPECT_TRUE(kept->isMarkedForMetadataExport());
    kept.reset();
    deferred.end(); // the scan ends
    EXPECT_TRUE(QTest::qWaitFor([&] { return fileTitle(file) == QStringLiteral("Generated Edit"); }, 5000))
            << "its tags were not written once it was let go";
    {
        const GlobalTrackCacheLocker locker;
        kept = locker.lookupTrackById(id);
    }
    EXPECT_FALSE(kept) << "saved and evicted";
}

// A scan of unchanged folders commits nothing. A modified track kept
// meanwhile is still saved within a few seconds, while the scan goes on;
// never while the scanner holds uncommitted writes.
TEST_F(DeckLabImportTest, AKeptTrackIsSavedWithinSecondsWhenTheScanCommitsNothing) {
    QTemporaryDir dir;
    const QString file = copyFixture(dir, QStringLiteral("Generated Edited.mp3"));
    TrackPointer track = getOrAddTrackByLocation(file);
    ASSERT_TRUE(track);
    const QString title = QStringLiteral("SELECT title FROM library WHERE id=%1").arg(track->getId().toString());
    const QString before = value(title).toString();
    track->setTitle(QStringLiteral("Generated Edit"));
    DeferredSaves deferred;
    const auto writesDone = qScopeGuard([] {
        GlobalTrackCache::setLibraryWritesHeld(false);
    });
    GlobalTrackCache::setLibraryWritesHeld(true); // a batch is being written
    track.reset();
    QTest::qWait(3000);
    EXPECT_EQ(before, value(title).toString()) << "saved beside the scanner's write";
    GlobalTrackCache::setLibraryWritesHeld(false); // unchanged folders from here
    EXPECT_TRUE(QTest::qWaitFor([&] { return value(title).toString() == QStringLiteral("Generated Edit"); }, 5000))
            << "the edit waited for the end of the scan";
    EXPECT_TRUE(GlobalTrackCache::isSavingModifiedTracksDeferred());
    deferred.end();
    QCoreApplication::processEvents();
}
