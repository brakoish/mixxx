#pragma once

#include <gtest/gtest_prod.h>

#include <QElapsedTimer>
#include <QList>
#include <QMutex>
#include <QScopedPointer>
#include <QSet>
#include <QSemaphore>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <atomic>

#include "library/dao/analysisdao.h"
#include "library/dao/cuedao.h"
#include "library/dao/directorydao.h"
#include "library/dao/libraryhashdao.h"
#include "library/dao/playlistdao.h"
#include "library/dao/trackdao.h"
#include "library/scanner/scannerglobal.h"
#include "track/track_decl.h"
#include "util/db/dbconnectionpool.h"

class ScannerTask;
class LibraryScannerDlg;
class QString;
struct LibraryScanResultSummary;

class LibraryScanner : public QThread {
    FRIEND_TEST(LibraryScannerTest, ScannerRoundtrip);
    friend class DeckLabImportTest;
    Q_OBJECT
  public:
    LibraryScanner(
            mixxx::DbConnectionPoolPtr pDbConnectionPool,
            const UserSettingsPointer& pConfig);
    ~LibraryScanner() override;

  public slots:
    // Call from any thread to start a scan. Does nothing if a scan is already
    // in progress.
    // The autoscan flag is used for the summary report. Receivers of scanSummary()
    // can use this to decide whether to show the summary dialog, for example hide
    // it for the automatic scan during startup.
    void scan(bool autoscan = false);

    // Deck Lab: like scan(true), but neither the progress dialog nor the
    // summary dialog appears (the kiosk shows its own notice). Returns false
    // when a scan is already starting or running.
    bool scanQuietly();

    // Deck Lab: a scan is starting, running or being canceled. Any thread;
    // right also before scanStarted() reaches a receiver that connected late.
    bool isScanning() const {
        return m_state != IDLE;
    }

    // Call from any thread to cancel the scan.
    void slotCancel();

    // Deck Lab: cancels a running scan without waiting for it (any thread).
    // It ends as after Cancel: the tracks it added stay, nothing is marked
    // missing, a folder it had not finished is read again next time, and
    // its summary says canceled.
    void requestCancel();

  signals:
    void scanStarted();
    void scanFinished();
    void scanSummary(const LibraryScanResultSummary& result);
    void progressHashing(const QString&);
    void progressLoading(const QString& path);
    void progressCoverArt(const QString& file);
    void trackAdded(TrackPointer pTrack);
    void tracksChanged(const QSet<TrackId>& changedTrackIds);
    void tracksRelocated(const QList<RelocatedTrack>& relocatedTracks);

    // Emitted by scan() to invoke slotStartScan in the scanner thread's event
    // loop.
    void startScan();

  protected:
    void run() override;

  public slots:
    void queueTask(ScannerTask* pTask);

  private slots:
    void slotStartScan();
    void slotFinishHashedScan();
    void slotFinishUnhashedScan();

    // ScannerTask signal handlers.
    void slotDirectoryHashedAndScanned(const QString& directoryPath,
                                   bool newDirectory, mixxx::cache_key_t hash);
    void slotDirectoryUnchanged(const QString& directoryPath,
            const QStringList& presentTrackLocations);
    void slotTrackExists(const QString& trackPath);
    void slotAddNewTrack(const QString& trackPath);

  private:
    enum ScannerState {
        IDLE,
        STARTING,
        SCANNING,
        CANCELING,
        FINISHED
    };

    void cancelAndQuit();
    void cancel();

    // Allowed State transitions:
    // IDLE -> STARTING
    // STARTING -> IDLE
    // STARTING -> SCANNING
    // SCANNING -> FINISHED
    // FINISHED -> IDLE
    // every state can change to CANCELING
    // CANCELING -> IDLE
    bool changeScannerState(LibraryScanner::ScannerState newState);

    void cleanUpScan();

    // Deck Lab: what a scan adds, read and waiting to be written: a file
    // read into a temporary track, or (no track) a folder's new hash.
    struct PendingAdd {
        TrackPointer track;
        QString path;
        bool newDirectory = false;
        mixxx::cache_key_t hash = 0;
    };
    // Deck Lab: the add transaction is written and committed in small
    // batches, and files are read only while it holds no write.
    void queueAdd(PendingAdd add);
    void flushAdds();
    // False if the commit failed: the writes stay pending.
    bool commitAdds();
    // Commits (tried a few times) before a file is read; false if it still
    // failed: the scan is canceled and the file is not read.
    bool commitAddsBeforeRead();
    void setAddWrites(bool writes);
    void addReadTrack(const QString& trackPath, const TrackPointer& pImportedTrack);
    void saveDirectoryHash(const QString& directoryPath,
            bool newDirectory,
            mixxx::cache_key_t hash);
    // Deck Lab: rows a scan that did not finish inserted count as added
    // for the moved-file relink of the next scan that does, unless they
    // are in use or hold anything of their own since.
    void recordAddedRows();
    QStringList addedTracksSinceUnfinishedScan() const;
    // Deck Lab: GlobalTrackCache keeps modified tracks instead of saving
    // them while this scan writes (no lock-order inversion with the GUI).
    void deferSavingModifiedTracks(bool defer);

    mixxx::DbConnectionPoolPtr m_pDbConnectionPool;

    // The pool of threads used for worker tasks.
    QThreadPool m_pool;

    // The library scanner thread's DAOs.
    LibraryHashDAO m_libraryHashDao;
    CueDAO m_cueDao;
    PlaylistDAO m_playlistDao;
    DirectoryDAO m_directoryDao;
    AnalysisDao m_analysisDao;
    TrackDAO m_trackDao;

    // Global scanner state for scan currently in progress.
    ScannerGlobalPointer m_scannerGlobal;
    // Deck Lab: guards setting and clearing m_scannerGlobal (scanner thread)
    // against copying it from another thread (cancel, requestCancel).
    QMutex m_scannerGlobalMutex;

    // The Semaphore guards the state transitions queued to the
    // Qt even Queue in the way, that you cannot start a
    // new scan while the old one is canceled
    QSemaphore m_stateSema;
    // this is accessed main and LibraryScanner thread
    volatile ScannerState m_state;

    QSet<QString> m_previouslyMissingTracks;
    int m_numRelocatedTracks;

    QList<mixxx::FileInfo> m_libraryRootDirs;
    std::unique_ptr<LibraryScannerDlg> m_pProgressDlg;

    bool m_canceled;
    bool m_manualScan;
    // Set by the caller's thread before startScan, read in the scanner thread.
    std::atomic<bool> m_quietScan{false};
    // Deck Lab: requestCancel() came before the scan had its global state.
    std::atomic<bool> m_cancelRequested{false};

    // Deck Lab: the next batch of the add transaction (scanner thread).
    QList<PendingAdd> m_pendingAdds;
    QElapsedTimer m_pendingSince;  // since the first one waits
    QTimer m_addCommitTimer{this}; // writes a batch that waits for more
    bool m_addWrites = false;      // the transaction holds writes
    QSet<TrackId> m_recordedAdds;  // inserted rows recorded but not committed
    bool m_savingDeferred = false;
};
