// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <functional>

/// Deck Lab utility windows (Get Music, Sample Maker) seen from Mixxx.
///
/// decklabd opens them in a kiosk browser and pushes `apps.changed
/// {"app": "<name>"|null}` (CONTRACT-decklabd.md). This class reacts to that:
///
/// * while a utility is open it publishes a tiny deck state file,
///   `$XDG_RUNTIME_DIR/decklab/deck-state.json` (tmpfs, once a second), so
///   Sample Maker only edits audio while nothing plays;
/// * when the utility closes it makes sure the folders the utilities write to
///   are library folders, then starts a quiet in-process library rescan once
///   it is quiet (decklab/decklabquiet.h: nothing plays or records, no other
///   scan, no utility open, and none of that for a quiet spell) and never
///   while a deck is loading a track (that wait is bounded). This replaces
///   the old Ctrl+Shift+L keystroke injection.
/// * r7 fix: Mixxx's start-up scan (RescanOnStartup) starts the same way,
///   without a notice, instead of with the application.
/// * r7 fix: a library scan that runs when a deck, sampler or the preview
///   starts playing, or a recording starts, is canceled without waiting
///   (what it added stays; nothing is marked missing) and runs again after
///   the next quiet spell, so it never reads a stick and writes the library
///   through a set.
///
/// Every system effect goes through Hooks, so the policy is testable without a
/// library, players or decklabd.
class DeckLabUtilities : public QObject {
    Q_OBJECT
  public:
    struct Hooks {
        std::function<bool()> playing;   // any deck, sampler or preview playing
        std::function<bool()> recording; // a mix recording is running
        std::function<bool()> loading;   // a track is being loaded into a player
        std::function<QStringList()> libraryRoots;
        std::function<bool(const QString&)> addLibraryRoot;
        /// Existing folders that utilities save into (download folders on
        /// mounted drives, ~/Music/New Music, ~/Music/Deck Lab Samples).
        std::function<QStringList()> utilityFolders;
        /// Starts a rescan without dialogs; false if none could start.
        std::function<bool()> startQuietScan;
        /// r7: background jobs may start (DeckLabSystem::settled: quiet for a
        /// quiet spell); unset counts as quiet.
        std::function<bool()> quiet;
        /// r7 fix: a library scan runs (any: start-up, after a utility, from
        /// Preferences), and cancels it without waiting.
        std::function<bool()> scanning;
        std::function<void()> interruptScan;
    };

    static constexpr int kPublishIntervalMs = 1000;
    static constexpr int kLoadingRetryMs = 500;
    static constexpr int kQuietRetryMs = 1000;
    static constexpr int kLoadingMaxWaitMs = 30000;
    static inline const QString kStateFileName = QStringLiteral("deck-state.json");

    explicit DeckLabUtilities(Hooks hooks,
            QObject* parent = nullptr,
            const QString& stateDirectory = QString());
    ~DeckLabUtilities() override;

    /// $XDG_RUNTIME_DIR/decklab (the directory Mixxx shares with decklabd).
    static QString defaultStateDirectory();
    QString stateFile() const;
    void setStateDirectory(const QString& directory);
    void setHooks(Hooks hooks);

    /// decklabd event `apps.changed`.
    void onAppsChanged(const QJsonObject& data);
    /// TrackCollectionManager::libraryScanSummary for a scan we may own.
    void onScanSummary(int newTracks, bool quiet, bool canceled = false);
    /// r7 fix: the scan Mixxx would start with (deferred by CoreServices):
    /// once it is quiet, without adding folders and without notices.
    void requestStartupScan();
    /// r7 fix: playing or recording changed. A scan that runs while a deck,
    /// sampler or the preview plays, or a recording runs, is canceled and
    /// asked for again (after the next quiet spell).
    void onActivity();

    QString openApp() const {
        return m_app;
    }
    bool refreshPending() const {
        return m_pending;
    }
    bool ownsScan() const {
        return m_scanOwned;
    }
    /// Writes the deck state file now; false when nothing is open or it failed.
    bool publishNow();

    /// Folders not yet inside a library root (same folder or below one).
    static QStringList missingRoots(const QStringList& folders, const QStringList& roots);

  signals:
    void notice(const QString& text);
    void refreshStarted();
    /// r7 fix: the utility window has gone (Get Music or Sample Maker may
    /// have written new files).
    void closed();

  private:
    void requestRefresh();
    void request(bool addRoots, bool announce);
    void tryRefresh();
    void removeStateFile();

    Hooks m_hooks;
    QString m_stateDirectory;
    QString m_app;
    QTimer m_publish;
    QTimer m_retry;
    QElapsedTimer m_waiting;
    bool m_pending = false;
    bool m_scanOwned = false;
    bool m_addRoots = false;      // the pending scan adds missing utility folders first
    bool m_announce = false;      // the pending scan says what it does
    bool m_scanAnnounced = false; // the scan we own says what it found
};
