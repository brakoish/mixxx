// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHash>
#include <QJsonArray>
#include <QSet>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <functional>
#include <memory>
#include <optional>

#include "controllers/controllermappinginfo.h"
#include "controllers/controllermappingmatcher.h"
#include "decklab/decklabcontrollers.h"
#include "decklab/decklabpreview.h"
#include "decklab/decklabquiet.h"
#include "decklab/decklabsetupmodel.h"
#include "decklab/decklabutilities.h"
#include "preferences/usersettings.h"

class BaseTrackPlayer;
class ControllerManager;
class DeckLabdClient;
class PlayerManagerInterface;
class SoundManager;
class TrackCollectionManager;

/// Application-wide Deck Lab system integration: the decklabd connection,
/// cached host state (USB, devices, thermal) and the in-process Deck Setup
/// operations (controller mapping, controller rescan, audio routing). It
/// outlives skin reloads; WDeckLab attaches to it. Every service is optional
/// so DJing and tests never depend on it.
class DeckLabSystem : public QObject {
    Q_OBJECT
  public:
    struct Services {
        UserSettingsPointer config;
        std::shared_ptr<SoundManager> sound;
        std::shared_ptr<ControllerManager> controllers;
        PlayerManagerInterface* players = nullptr;
        /// Library folders and the quiet rescan after a utility closes.
        std::shared_ptr<TrackCollectionManager> library;
    };
    /// One physical controller (r5-polish): its MIDI port and HID interfaces
    /// are one row; mixxxName is the interface Deck Lab uses.
    struct ControllerInfo {
        QString key;       // vid:pid[:serial] (decklabd) or the Mixxx name
        QString name;      // model name for display
        QString mixxxName; // empty while Mixxx has not enumerated it
        bool hid = false;  // the interface in use is HID (else MIDI)
        bool open = false; // that interface is open with a mapping made for it
        QString mapping; // configured mapping file
        std::optional<uint16_t> vendorId;
        std::optional<uint16_t> productId;
        std::optional<int> interfaceNumber;
        QJsonObject audioCard; // decklabd audio_card, may be empty
        QStringList interfaces; // every Mixxx name of this controller, mixxxName first
        QStringList suppressed; // interfaces Deck Lab never opens
    };
    /// Test seam for Deck Setup (r5-polish): fake Mixxx interfaces, and what
    /// opening one with a mapping, disabling one or a rescan would do (a
    /// rescan then finishes as ControllerManager's does). Never set by the
    /// application.
    struct ControllerHooks {
        std::function<QList<decklab::ControllerEndpoint>()> endpoints;
        std::function<bool(const QString& name, const QString& mappingPath, QString* error)> open;
        std::function<void(const decklab::ControllerEndpoint& endpoint)> disable;
        std::function<void()> rescan;
    };
    using Done = std::function<void(bool ok, const QString& message)>;

    explicit DeckLabSystem(Services services,
            QObject* parent = nullptr,
            const QString& socketPath = QString());
    ~DeckLabSystem() override;

    void start();
    DeckLabdClient* client() const {
        return m_client;
    }
    /// Get Music / Sample Maker lifecycle: deck state for them, rescan after them.
    DeckLabUtilities* utilities() const {
        return m_utilities;
    }
    /// Sample Maker's headphone preview on [PreviewDeck1] and PUT ON PAD.
    DeckLabPreview* preview() const {
        return m_preview;
    }
    bool isReady() const;
    UserSettingsPointer config() const {
        return m_services.config;
    }
    SoundManager* soundManager() const {
        return m_services.sound.get();
    }
    ControllerManager* controllerManager() const {
        return m_services.controllers.get();
    }

    // Live performance state; system actions refuse while music plays.
    bool anyPlaying() const;
    /// r7: what keeps Deck Lab's background jobs waiting (decklab/decklabquiet.h):
    /// playing, recording, Mixxx's library scan, an open utility window.
    decklab::Activity activity() const;
    /// Nothing does: the background analysis, the USB relinker, the rescan
    /// after a utility and TO PAD's library add may run.
    bool quiet() const {
        return decklab::isQuiet(activity());
    }
    /// r7: the first pad (1-based) a new sample may take: nothing loaded or
    /// loading on it, and not the pad a Load pad trip is filling. 0 when
    /// every pad is taken. PUT ON PAD and TO PAD both ask this.
    int emptyPad() const;
    /// The rule itself, for any player manager (a surface without a system):
    /// the first of `pads` pads with a player, nothing loaded (track_loaded),
    /// nothing loading (the player holds its new track at once, or `loading`
    /// says so) and not `target`.
    static int firstEmptyPad(PlayerManagerInterface* players,
            int pads,
            int target,
            const std::function<bool(BaseTrackPlayer*)>& loading = {});
    /// The pad a Load pad trip is filling, 0 when none (WDeckLab sets it).
    void setPadTarget(std::function<int()> target) {
        m_padTarget = std::move(target);
    }
    /// A utility Deck Lab opened answered "started": decklabd announces only
    /// a new window, not one that was still open after it restarted.
    void utilityLaunched(const QString& app);
    /// A deck or sampler plays (the preview deck does not count).
    bool decksOrSamplersPlaying() const;
    QList<decklab::LoadedTrack> loadedTracks() const;
    /// Test hook replacing PlayerManager lookups.
    void setLoadedTracksProvider(std::function<QList<decklab::LoadedTrack>()> provider);
    bool audioOutputConfigured() const;

    // Cached decklabd state.
    QJsonArray usbDrives() const {
        return m_drives;
    }
    QJsonObject devices() const {
        return m_devices;
    }
    QJsonObject thermal() const {
        return m_thermal;
    }
    bool undervoltage() const {
        return m_undervoltage;
    }
    /// Empty when there is nothing to warn about.
    QString thermalWarning() const;

    // Storage.
    /// Empty when safe, else the user-facing reason. Covers loaded tracks and
    /// an active recording on that drive.
    QString ejectRefusal(const QJsonObject& drive) const;
    void ejectDrive(const QJsonObject& drive, Done done);
    void useDriveForRecordings(const QJsonObject& drive, Done done);
    QString recordingDirectory() const;
    static QString recordingFolderFor(const QString& mountPath);

    // Deck Setup.
    /// One entry per physical controller (decklabd's devices and Mixxx's
    /// MIDI ports and HID interfaces, grouped).
    QList<ControllerInfo> controllers() const;
    /// Mixxx's controllers as plain interface records.
    QList<decklab::ControllerEndpoint> controllerEndpoints() const;
    void setControllerHooksForTesting(ControllerHooks hooks);
    /// Closes and disables interfaces Deck Lab must never have open: a HID
    /// interface beside the MIDI port in use (an earlier build could enable
    /// one with a MIDI mapping, and Mixxx then reopened it at every start).
    /// Returns the names it disabled (twins: once each).
    QStringList closeStrayInterfaces();
    QList<MappingInfo> mappings(bool hid) const;
    QList<ControllerMappingMatcher::Match> suggestMappings(const ControllerInfo& controller) const;
    int suggestedDeckCount(const QString& mappingPath) const;
    bool applyMapping(const QString& controllerName, const QString& mappingPath, QString* error);
    bool canRescanControllers(QString* reason) const;
    bool rescanControllers(QString* reason);
    bool isRescanning() const {
        return m_rescanning;
    }
    /// Offers the supervisor restart; refused while anything plays.
    bool restartToDetect(QString* reason);

    QString audioApi() const;
    /// Developer captures/tests: present these outputs instead of PortAudio's.
    void setAudioDevicesOverride(const QList<decklab::AudioDevice>& devices) {
        m_audioOverride = devices;
        m_hasAudioOverride = true;
    }
    QList<decklab::AudioDevice> audioDevices(const ControllerInfo* controller) const;
    decklab::AudioPlan currentAudioPlan() const;
    bool applyAudio(const decklab::AudioPlan& plan, QString* error);
    /// side: 0 = left only, 1 = right only, -1 = both channels of the pair.
    /// dbfs: the peak level (sound check: -24, then -18 on a second tap).
    void playTestTone(const decklab::AudioRoute& route, int side, Done done,
            double dbfs = decklab::kTestToneDbfs);
    bool isTestingTone() const {
        return m_toneRunning;
    }

    decklab::DeviceStore deviceStore() const {
        return decklab::DeviceStore(m_services.config);
    }
    void rememberSetup(const decklab::DeviceProfile& profile);
    /// [DeckLab] first_run_done: the first-run flow has been completed.
    bool firstRunDone() const;
    void markFirstRunDone();
    /// r5-polish: [DeckLab] first_run_deferred: "Set up later" was tapped.
    /// The flow no longer opens at start; Settings offers Finish setup.
    bool firstRunDeferred() const;
    void deferFirstRun();
    /// Opens by itself at start: neither done nor deferred.
    bool firstRunDue() const {
        return !firstRunDone() && !firstRunDeferred();
    }
    /// Mixxx controller name for decklabd controller key, if enumerated.
    QString mixxxNameForKey(const QString& key) const;

    /// Restart Deck Lab: decklabd's supervisor restart, else a graceful quit
    /// (the kiosk unit restarts it).
    void restartDeckLab();

  signals:
    void readyChanged(bool ready);
    void usbChanged(const QJsonArray& drives);
    void thermalChanged();
    void devicesChanged();
    void controllersChanged();
    void controllersRescanned();
    /// r5-polish: first_run_done or first_run_deferred changed.
    void firstRunChanged();
    void audioChanged();
    /// A controller decklabd reports as new. `known` ones are re-applied
    /// silently; others should be offered Deck Setup.
    void controllerArrived(const QString& key, const QString& name, bool known);
    void notice(const QString& text);
    void layoutRequested(int decks);
    void quitRequested(const QString& reason);
    void gracefulQuitRequested();

  private:
    void onReady(bool ready);
    void askOpenUtility();
    void onEvent(const QString& topic, const QJsonObject& data);
    void onDevicesChanged(const QJsonObject& data, const QStringList& added);
    void onRescanFinished();
    void reapply(const decklab::DeviceProfile& profile, bool afterRescan);
    void reapplyAudio(const decklab::DeviceProfile& profile);
    void forgetMissingRecordingDrive();
    bool preferencesOpen() const;
    DeckLabUtilities::Hooks utilityHooks();
    DeckLabPreview::Hooks previewHooks();
    QStringList utilityFolders() const;
    void watchPlayers();
    void watchPlayer(BaseTrackPlayer* player);
    bool anyLoading() const;
    QList<decklab::ControllerGroup> controllerGroups() const;
    bool hidMappingMatches(const decklab::ControllerEndpoint& endpoint) const;
    void forgetControllerCache();
    bool disableInterface(const decklab::ControllerEndpoint& endpoint);
    /// An upgrade from a build without first_run_deferred: a unit with a
    /// remembered Deck Setup counts as set up.
    void migrateFirstRun();

    Services m_services;
    DeckLabdClient* m_client;
    DeckLabUtilities* m_utilities;
    DeckLabPreview* m_preview;
    QSet<BaseTrackPlayer*> m_watchedPlayers;
    QHash<BaseTrackPlayer*, qint64> m_loadingSince;
    std::function<int()> m_padTarget;
    bool m_scanning = false;   // Mixxx's library scan runs
    bool m_wasReady = false;   // decklabd answered before: a ready is a reconnect
    std::function<QList<decklab::LoadedTrack>()> m_loadedTracksProvider;
    QJsonArray m_drives;
    QJsonObject m_devices;
    QJsonObject m_thermal;
    bool m_undervoltage = false;
    bool m_rescanning = false;
    bool m_toneRunning = false;
    QList<decklab::DeviceProfile> m_pendingReapply;
    QList<decklab::AudioDevice> m_audioOverride;
    bool m_hasAudioOverride = false;
    mutable QHash<QString, QList<MappingInfo>> m_ownMappings;
    ControllerHooks m_controllerHooks;
    // USB locations and HID mapping matches, per interface, until the
    // controllers change (read from /proc and /sys, never per frame).
    mutable std::optional<QHash<QString, QString>> m_midiLocations;
    mutable QHash<QString, QString> m_hidLocations;
    mutable QHash<QString, bool> m_hidMatches;
};
