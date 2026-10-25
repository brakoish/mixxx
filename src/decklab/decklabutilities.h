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
///   scan, no utility open; as soon as it is) and never while a deck is
///   loading a track (that wait is bounded). This replaces the old
///   Ctrl+Shift+L keystroke injection.
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
        /// r7: background jobs may run (DeckLabSystem::quiet); unset counts as quiet.
        std::function<bool()> quiet;
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
    void onScanSummary(int newTracks, bool quiet);

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

  private:
    void requestRefresh();
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
};
