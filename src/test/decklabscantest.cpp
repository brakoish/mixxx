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

#include "library/coverart.h"
#include "library/library_decl.h"
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
    GlobalTrackCache::deferSavingModifiedTracks(true);
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
    GlobalTrackCache::deferSavingModifiedTracks(false);
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
// runs: it fills in its new track and writes its row. The scanner, about to
// wait for that track, commits its batch first: otherwise it would hold the
// write lock while waiting for the GUI, and the GUI's write would wait for it
// until SQLite's busy timeout and fail. Here another thread stands in for
// the GUI; the test library fails such a write at once instead of waiting.
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
    std::future<bool> gui;
    std::atomic<bool> fillingFirst{false};
    onScannerReading(scanner, [&](const QString& location) {
        if (!location.endsWith(QStringLiteral("Generated 3.mp3"))) {
            return; // quick reads: the batch holds files 1 and 2
        }
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
        // Bounded: a scanner that read in the cache's slot would never let
        // the other thread in.
        fillingFirst = filled.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    });
    QSignalSpy finished(&scanner, &LibraryScanner::scanFinished);
    scanner.start();
    ASSERT_TRUE(scanner.scanQuietly());
    ASSERT_TRUE(finished.wait(30000));
    ASSERT_TRUE(gui.valid());
    EXPECT_TRUE(fillingFirst.load()) << "the scanner read the file in the cache's slot";
    EXPECT_TRUE(gui.get()) << "the GUI's write waited for the scanner, which waited for the GUI";
    EXPECT_EQ(kFiles, value(QStringLiteral("SELECT COUNT(*) FROM library")).toInt());
    QCoreApplication::processEvents(); // the GUI's track leaves the cache
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

// Canceled for playback (LibraryScanner::requestCancel, which never waits):
// what it added stays, each track whole; nothing is marked missing; the next
// scan goes on where it stopped and adds no row twice.
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
        QThread::msleep(10);
        if (++read == 10) {
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
    EXPECT_EQ(10, value(QStringLiteral("SELECT COUNT(*) FROM library")).toInt());
    EXPECT_EQ(10, value(QStringLiteral("SELECT COUNT(*) FROM library INNER JOIN track_locations "
                                       "ON library.location=track_locations.id")).toInt())
            << "every row whole";
    EXPECT_EQ(10, value(QStringLiteral("SELECT COUNT(*) FROM track_locations")).toInt());
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
