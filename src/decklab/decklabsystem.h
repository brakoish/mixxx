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
    struct ControllerInfo {
        QString key;       // vid:pid[:serial] (decklabd) or the Mixxx name
        QString name;      // model name for display
        QString mixxxName; // empty while Mixxx has not enumerated it
        bool hid = false;
        bool open = false;
        QString mapping; // configured mapping file
        std::optional<uint16_t> vendorId;
        std::optional<uint16_t> productId;
        std::optional<int> interfaceNumber;
        QJsonObject audioCard; // decklabd audio_card, may be empty
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
    QList<ControllerInfo> controllers() const;
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
    void playTestTone(const decklab::AudioRoute& route, int side, Done done);
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
    void onEvent(const QString& topic, const QJsonObject& data);
    void onDevicesChanged(const QJsonObject& data, const QStringList& added);
    void onRescanFinished();
    void reapply(const decklab::DeviceProfile& profile, bool afterRescan);
    void reapplyAudio(const decklab::DeviceProfile& profile);
    void forgetMissingRecordingDrive();
    bool preferencesOpen() const;
    DeckLabUtilities::Hooks utilityHooks();
    QStringList utilityFolders() const;
    void watchPlayers();
    void watchPlayer(BaseTrackPlayer* player);
    bool anyLoading() const;

    Services m_services;
    DeckLabdClient* m_client;
    DeckLabUtilities* m_utilities;
    QSet<BaseTrackPlayer*> m_watchedPlayers;
    QHash<BaseTrackPlayer*, qint64> m_loadingSince;
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
};
