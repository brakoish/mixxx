// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabsystem.h"

#include <portaudio.h>

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <QWidget>
#include <QtDebug>
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "control/controlobject.h"
#include "controllers/controller.h"
#include "controllers/controllermanager.h"
#include "controllers/controllermappinginfoenumerator.h"
#include "controllers/defs_controllers.h"
#include "controllers/legacycontrollermapping.h"
#include "controllers/legacycontrollermappingfilehandler.h"
#ifdef __HID__
#include "controllers/hid/hidcontroller.h"
#endif
#include "decklab/decklabdclient.h"
#include "decklab/decklabdownloads.h"
#include "decklab/decklabusbname.h"
#include "engine/engine.h"
#include "library/dao/directorydao.h"
#include "library/library_decl.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "mixer/basetrackplayer.h"
#include "mixer/playermanager.h"
#include "mixer/previewdeck.h"
#include "mixer/sampler.h"
#include "moc_decklabsystem.cpp"
#include "recording/defs_recording.h"
#include "soundio/sounddevice.h"
#include "soundio/sounddevicestatus.h"
#include "soundio/soundmanager.h"
#include "soundio/soundmanagerconfig.h"
#include "soundio/soundmanagerutil.h"
#include "track/track.h"
#include "track/trackref.h"
#include "util/fileinfo.h"
#include "waveform/visualplayposition.h"

namespace {
const QString kDeckLab = QStringLiteral("[DeckLab]");
const QString kRecording = QString::fromLatin1(RECORDING_PREF_KEY);
const QString kRecordingFolder = QStringLiteral("Deck Lab Recordings");
// Folders the utilities write to (apps/get-music, apps/sample-maker in the appliance repo).
const QString kDownloadFolder = QStringLiteral("Deck Lab Downloads");
const QString kPreviewDeck = QStringLiteral("[PreviewDeck1]");
// decklabd went away while a utility was open: after this long without it
// the utility counts as closed, so background jobs are not held forever.
constexpr int kUtilityLostMs = 15000;
// Deck Lab shows sixteen pads; PUT ON PAD fills the first empty one of those.
constexpr int kPads = 16;
constexpr qint64 kLoadingStaleMs = 20000;
constexpr qint64 kMinimumRecordingFreeMb = 1024;

double controlValue(const QString& group, const QString& item) {
    auto* control = ControlObject::getControl(ConfigKey(group, item), ControlFlag::NoWarnIfMissing);
    return control ? control->get() : 0.0;
}

void setControl(const QString& group, const QString& item, double value) {
    if (auto* control = ControlObject::getControl(ConfigKey(group, item), ControlFlag::NoWarnIfMissing)) {
        control->set(value);
    }
}

int libraryId(const TrackPointer& track) {
    return track ? track->getId().toVariant().toInt() : 0;
}

// Seconds of the track the preview deck's engine holds (its frames, not its tags).
double previewSeconds() {
    const double rate = controlValue(kPreviewDeck, QStringLiteral("track_samplerate"));
    const double samples = controlValue(kPreviewDeck, QStringLiteral("track_samples"));
    return rate > 0 && samples > 0 ? samples / mixxx::kEngineChannelOutputCount / rate : 0.0;
}

QString compact(const QString& text) {
    QString result;
    for (QChar c : text.toLower()) {
        if (c.isLetterOrNumber()) {
            result.append(c);
        }
    }
    return result;
}

// ControllerManager's config item for a device name.
QString controllerConfigItem(QString name) {
    return name.replace(QLatin1Char(' '), QLatin1Char('_'))
            .replace(QLatin1Char('/'), QLatin1Char('_'))
            .replace(QLatin1Char('\\'), QLatin1Char('_'));
}

int alsaCardNumber(const QString& text) {
    static const QRegularExpression card(QStringLiteral("^(?:plug)?(?:hw:)?(\\d+)(?:,\\d+)?$"));
    const auto match = card.match(text.trimmed());
    return match.hasMatch() ? match.captured(1).toInt() : -1;
}

// Sine burst written straight to one device while Mixxx's own streams are
// closed. Runs on a worker thread; returns an error text or empty.
QString playToneOnDevice(int deviceIndex, int baseChannel, int side, double sampleRate, double amplitude) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(deviceIndex);
    if (!info) {
        return QObject::tr("Output not found");
    }
    PaStreamParameters output = {};
    output.device = deviceIndex;
    output.sampleFormat = paFloat32;
    output.suggestedLatency = info->defaultHighOutputLatency;
    output.hostApiSpecificStreamInfo = nullptr;
    PaStream* stream = nullptr;
    PaError error = paInvalidChannelCount;
    const int needed = baseChannel + 2;
    QList<int> counts{needed};
    if (info->maxOutputChannels > needed) {
        counts.append(info->maxOutputChannels); // hw: devices may need all channels
    }
    QList<double> rates{sampleRate};
    if (info->defaultSampleRate > 0 && !qFuzzyCompare(info->defaultSampleRate, sampleRate)) {
        rates.append(info->defaultSampleRate);
    }
    int channels = needed;
    for (int count : std::as_const(counts)) {
        for (double rate : std::as_const(rates)) {
            if (rate <= 0) {
                continue;
            }
            output.channelCount = count;
            error = Pa_OpenStream(&stream, nullptr, &output, rate, 512, paNoFlag, nullptr, nullptr);
            if (error == paNoError) {
                channels = count;
                sampleRate = rate;
                break;
            }
        }
        if (error == paNoError) {
            break;
        }
    }
    if (error != paNoError) {
        // The detail goes to the log; the DJ gets what to do (F-20).
        qWarning() << "DeckLab test tone: cannot open the output:" << Pa_GetErrorText(error);
        return QObject::tr("Couldn't play on that output. Pick another one.");
    }
    error = Pa_StartStream(stream);
    if (error == paNoError) {
        // Left low, right high, both in between: easy to tell apart by ear.
        const double frequency = side == 0 ? 523.25 : side == 1 ? 783.99 : 659.25;
        // Quiet (-24 dBFS, -18 at most) with 50 ms fades: it may play on a club PA.
        const int frames = static_cast<int>(sampleRate * 0.9);
        const int fade = std::max(1, static_cast<int>(sampleRate * 0.05));
        constexpr int kChunk = 512;
        std::vector<float> buffer(static_cast<size_t>(kChunk * channels));
        for (int written = 0; written < frames; written += kChunk) {
            const int count = std::min(kChunk, frames - written);
            std::fill(buffer.begin(), buffer.end(), 0.0f);
            for (int frame = 0; frame < count; ++frame) {
                const int n = written + frame;
                const double envelope = std::min({1.0,
                        n / static_cast<double>(fade),
                        (frames - n) / static_cast<double>(fade)});
                const auto sample = static_cast<float>(
                        amplitude * envelope * std::sin(6.283185307179586 * frequency * n / sampleRate));
                float* slot = &buffer[static_cast<size_t>(frame * channels)];
                if (side != 1) {
                    slot[baseChannel] = sample;
                }
                if (side != 0) {
                    slot[baseChannel + 1] = sample;
                }
            }
            error = Pa_WriteStream(stream, buffer.data(), static_cast<unsigned long>(count));
            if (error != paNoError && error != paOutputUnderflowed) {
                break;
            }
        }
        Pa_StopStream(stream);
    }
    Pa_CloseStream(stream);
    if (error == paNoError || error == paOutputUnderflowed) {
        return QString();
    }
    qWarning() << "DeckLab test tone: playback failed:" << Pa_GetErrorText(error);
    return QObject::tr("Couldn't play on that output. Pick another one.");
}
} // namespace

DeckLabSystem::DeckLabSystem(Services services, QObject* parent, const QString& socketPath)
        : QObject(parent),
          m_services(std::move(services)),
          m_client(new DeckLabdClient(this, socketPath)),
          m_utilities(new DeckLabUtilities(DeckLabUtilities::Hooks(), this)),
          m_preview(new DeckLabPreview(
                  DeckLabPreview::Hooks(),
                  [this](const QString& method, const QJsonObject& params) {
                      m_client->call(method, params);
                  },
                  this)) {
    m_utilities->setHooks(utilityHooks());
    m_preview->setHooks(previewHooks());
    connect(m_client, &DeckLabdClient::readyChanged, this, &DeckLabSystem::onReady);
    connect(m_client, &DeckLabdClient::eventReceived, this, &DeckLabSystem::onEvent);
    connect(m_utilities, &DeckLabUtilities::notice, this, &DeckLabSystem::notice);
    // Sample Maker reads deck-state.json before it decodes audio: a preview
    // that starts or stops is published at once, not on the next second.
    connect(m_preview, &DeckLabPreview::playingChanged, this, [this] { m_utilities->publishNow(); });
    if (auto* library = m_services.library.get()) {
        connect(library,
                &TrackCollectionManager::libraryScanSummary,
                this,
                [this](const LibraryScanResultSummary& result) {
                    m_utilities->onScanSummary(result.numNewTracks, result.quiet);
                });
        // r7: a scan holds one long write transaction; background jobs wait.
        connect(library, &TrackCollectionManager::libraryScanStarted, this, [this] { m_scanning = true; });
        connect(library, &TrackCollectionManager::libraryScanFinished, this, [this] { m_scanning = false; });
    }
    if (auto* players = m_services.players) {
        watchPlayers();
        connect(players, &PlayerManagerInterface::numberOfDecksChanged, this, [this](int) { watchPlayers(); });
        connect(players, &PlayerManagerInterface::numberOfSamplersChanged, this, [this](int) { watchPlayers(); });
    }
    if (auto* controllers = m_services.controllers.get()) {
        // r5-polish: fresh locations for the new device list, and no HID
        // interface left open beside the MIDI port in use.
        connect(
                controllers,
                &ControllerManager::devicesChanged,
                this,
                [this] {
                    forgetControllerCache();
                    closeStrayInterfaces();
                },
                Qt::QueuedConnection);
        connect(controllers,
                &ControllerManager::devicesChanged,
                this,
                &DeckLabSystem::controllersChanged,
                Qt::QueuedConnection);
        connect(controllers,
                &ControllerManager::rescanFinished,
                this,
                &DeckLabSystem::onRescanFinished,
                Qt::QueuedConnection);
    }
    if (auto* sound = m_services.sound.get()) {
        connect(sound, &SoundManager::devicesSetup, this, &DeckLabSystem::audioChanged);
    }
    migrateFirstRun();
}

DeckLabSystem::~DeckLabSystem() = default;

void DeckLabSystem::migrateFirstRun() {
    // r5-polish: units set up before first_run_deferred existed have a
    // remembered Deck Setup ([DeckLabDevices]) but may never have reached
    // Play in the first run: they are set up, so no Welcome and no
    // "Not finished".
    if (!m_services.config || firstRunDone() || firstRunDeferred() || deviceStore().all().isEmpty()) {
        return;
    }
    qInfo() << "DeckLab: a remembered Deck Setup exists; first run counts as done";
    m_services.config->setValue(ConfigKey(kDeckLab, QStringLiteral("first_run_done")), 1);
    m_services.config->save();
}

void DeckLabSystem::start() {
    m_client->start();
    // Mixxx may have opened its enabled controllers before Deck Lab attached.
    QTimer::singleShot(0, this, [this] { closeStrayInterfaces(); });
}

bool DeckLabSystem::isReady() const {
    return m_client->isReady();
}

void DeckLabSystem::onReady(bool ready) {
    if (ready) {
        m_client->call(QStringLiteral("usb.list"), {}, [this](const DeckLabdReply& reply) {
            if (reply.ok) {
                m_drives = reply.result.value(QStringLiteral("drives")).toArray();
                forgetMissingRecordingDrive();
                emit usbChanged(m_drives);
            }
        });
        m_client->call(QStringLiteral("devices.list"), {}, [this](const DeckLabdReply& reply) {
            if (reply.ok) {
                m_devices = reply.result;
                emit devicesChanged();
            }
        });
        m_client->call(QStringLiteral("system.info"), {}, [this](const DeckLabdReply& reply) {
            if (!reply.ok) {
                return;
            }
            const double temperature = reply.result.value(QStringLiteral("temp_c")).toDouble(0);
            m_undervoltage = reply.result.value(QStringLiteral("undervoltage_now")).toBool();
            if (m_thermal.isEmpty() && temperature >= 75) {
                m_thermal = QJsonObject{{QStringLiteral("temp_c"), temperature},
                        {QStringLiteral("level"), temperature >= 80 ? QStringLiteral("hot") : QStringLiteral("warm")}};
            }
            emit thermalChanged();
        });
    } else {
        m_drives = QJsonArray();
        emit usbChanged(m_drives);
    }
    emit readyChanged(ready);
}

void DeckLabSystem::onEvent(const QString& topic, const QJsonObject& data) {
    if (topic == QStringLiteral("usb.changed")) {
        m_drives = data.value(QStringLiteral("drives")).toArray();
        forgetMissingRecordingDrive();
        emit usbChanged(m_drives);
    } else if (topic == QStringLiteral("devices.changed")) {
        QStringList added;
        for (const auto& key : data.value(QStringLiteral("added")).toArray()) {
            added.append(key.toString());
        }
        onDevicesChanged(data, added);
    } else if (topic == QStringLiteral("thermal.changed")) {
        m_thermal = data;
        if (data.contains(QStringLiteral("undervoltage_now"))) {
            m_undervoltage = data.value(QStringLiteral("undervoltage_now")).toBool();
        }
        emit thermalChanged();
    } else if (topic == QStringLiteral("power.undervoltage")) {
        m_undervoltage = data.value(QStringLiteral("now")).toBool();
        emit thermalChanged();
    } else if (topic == QStringLiteral("session.quit_requested")) {
        emit quitRequested(data.value(QStringLiteral("reason")).toString());
    } else if (topic == QStringLiteral("apps.changed")) {
        m_utilities->onAppsChanged(data);
        m_preview->onUtilityChanged(!m_utilities->openApp().isEmpty());
    } else {
        m_preview->onEvent(topic, data);
    }
}

DeckLabUtilities::Hooks DeckLabSystem::utilityHooks() {
    DeckLabUtilities::Hooks hooks;
    hooks.playing = [this] { return anyPlaying(); };
    hooks.recording = [] { return controlValue(kRecording, QStringLiteral("status")) > 0; };
    hooks.loading = [this] { return anyLoading(); };
    hooks.libraryRoots = [this] {
        auto* library = m_services.library.get();
        return library ? library->internalCollection()->getRootDirStrings() : QStringList();
    };
    hooks.addLibraryRoot = [this](const QString& folder) {
        auto* library = m_services.library.get();
        if (!library) {
            return false;
        }
        const auto result = library->addDirectory(mixxx::FileInfo(folder));
        return result == DirectoryDAO::AddResult::Ok || result == DirectoryDAO::AddResult::AlreadyWatching;
    };
    hooks.utilityFolders = [this] { return utilityFolders(); };
    // r7: the rescan waits until it is quiet (decklab/decklabquiet.h).
    hooks.quiet = [this] { return quiet(); };
    hooks.startQuietScan = [this] {
        auto* library = m_services.library.get();
        return library && library->startLibraryScanQuietly();
    };
    return hooks;
}

DeckLabPreview::Hooks DeckLabSystem::previewHooks() {
    DeckLabPreview::Hooks hooks;
    hooks.decksPlaying = [this] { return decksOrSamplersPlaying(); };
    hooks.utilityOpen = [this] { return !m_utilities->openApp().isEmpty(); };
    const auto previewDeck = [this]() -> BaseTrackPlayer* {
        auto* players = m_services.players;
        return players && players->numberOfPreviewDecks() > 0 ? players->getPreviewDeck(0) : nullptr;
    };
    hooks.loadedTrack = [previewDeck] {
        auto* player = previewDeck();
        return player ? libraryId(player->getLoadedTrack()) : 0;
    };
    hooks.load = [this, previewDeck](int id, QString* error) {
        auto* player = previewDeck();
        auto* library = m_services.library.get();
        const TrackPointer track = library && player
                ? library->getTrackById(TrackId(QVariant(id)))
                : TrackPointer();
        if (!track) {
            *error = tr("That track is not in the library.");
            return false;
        }
#ifdef __STEM__
        player->slotLoadTrack(track, mixxx::StemChannelSelection(), false);
#else
        player->slotLoadTrack(track, false);
#endif
        return true;
    };
    hooks.duration = [] { return previewSeconds(); };
    hooks.position = [] {
        // The engine's position from the last audio callback; playposition is only 15 Hz.
        const auto visual = VisualPlayPosition::getVisualPlayPosition(kPreviewDeck);
        double fraction = visual ? visual->getEnginePlayPos() : -1.0;
        if (fraction < 0) {
            fraction = controlValue(kPreviewDeck, QStringLiteral("playposition"));
        }
        return fraction * previewSeconds();
    };
    hooks.playing = [] { return controlValue(kPreviewDeck, QStringLiteral("play")) > 0; };
    hooks.play = [](double start, double end, bool loop, double from) {
        // Mixxx's loop controls count engine samples (two per frame), so the loop is sample exact.
        const double rate = controlValue(kPreviewDeck, QStringLiteral("track_samplerate"));
        const double samples = controlValue(kPreviewDeck, QStringLiteral("track_samples"));
        if (!(rate > 0 && samples > 0)) {
            return;
        }
        const double perSecond = rate * mixxx::kEngineChannelOutputCount;
        setControl(kPreviewDeck, QStringLiteral("loop_enabled"), 0);
        setControl(kPreviewDeck, QStringLiteral("loop_end_position"), -1);
        setControl(kPreviewDeck, QStringLiteral("loop_start_position"), start * perSecond);
        setControl(kPreviewDeck, QStringLiteral("loop_end_position"), end * perSecond);
        if (loop) {
            setControl(kPreviewDeck, QStringLiteral("loop_enabled"), 1);
        }
        if (from >= 0) {
            setControl(kPreviewDeck, QStringLiteral("playposition"), from * perSecond / samples);
        }
        setControl(kPreviewDeck, QStringLiteral("play"), 1);
    };
    hooks.pause = [] {
        setControl(kPreviewDeck, QStringLiteral("play"), 0);
        setControl(kPreviewDeck, QStringLiteral("loop_enabled"), 0);
    };
    hooks.eject = [previewDeck] {
        if (auto* player = previewDeck()) {
            player->slotEjectTrack(1.0);
        }
    };
    hooks.samplesFolder = [] { return decklab::samplesFolder(); };
    hooks.emptyPad = [this] { return emptyPad(); };
    hooks.loadPad = [this](int pad, const QString& path, QString* error) {
        auto* players = m_services.players;
        auto* library = m_services.library.get();
        auto* sampler = players && pad >= 1 && pad <= players->numberOfSamplers()
                ? players->getSampler(pad - 1)
                : nullptr;
        const TrackPointer track = sampler && library
                ? library->getOrAddTrack(TrackRef::fromFilePath(path))
                : TrackPointer();
        if (!track) {
            *error = tr("The sample could not be added to the library.");
            return false;
        }
#ifdef __STEM__
        sampler->slotLoadTrack(track, mixxx::StemChannelSelection(), false);
#else
        sampler->slotLoadTrack(track, false);
#endif
        return true;
    };
    return hooks;
}

QStringList DeckLabSystem::utilityFolders() const {
    QStringList folders;
    const auto add = [&folders](const QString& path) {
        const QFileInfo info(path);
        // Plain folders only: never follow a link a utility did not create.
        if (info.isDir() && !info.isSymLink()) {
            folders.append(info.absoluteFilePath());
        }
    };
    // Get Music saves into "Deck Lab Downloads" at the top of the chosen drive;
    // only that folder becomes a library folder, never the whole drive.
    for (const auto& value : std::as_const(m_drives)) {
        const QString mount = value.toObject().value(QStringLiteral("mount")).toString();
        if (mount.startsWith(QLatin1Char('/'))) {
            add(QDir(mount).filePath(kDownloadFolder));
        }
    }
    // ~/Music/New Music and ~/Music/Deck Lab Samples, where the utilities save.
    for (const QString& folder : {decklab::cardDownloadsFolder(), decklab::samplesFolder()}) {
        if (!folder.isEmpty()) {
            add(folder);
        }
    }
    return folders;
}

void DeckLabSystem::watchPlayers() {
    auto* players = m_services.players;
    if (!players) {
        return;
    }
    for (int deck = 0; deck < players->numberOfDecks(); ++deck) {
        watchPlayer(players->getDeckBase(deck));
    }
    for (int sampler = 0; sampler < players->numberOfSamplers(); ++sampler) {
        watchPlayer(players->getSampler(sampler));
    }
    for (int preview = 0; preview < players->numberOfPreviewDecks(); ++preview) {
        watchPlayer(players->getPreviewDeck(preview));
    }
}

void DeckLabSystem::watchPlayer(BaseTrackPlayer* player) {
    if (!player || m_watchedPlayers.contains(player)) {
        return;
    }
    m_watchedPlayers.insert(player);
    // Loading starts with loadingTrack(new, old) and ends with newTrackLoaded,
    // or playerEmpty when it failed or the deck was ejected.
    connect(player,
            &BaseTrackPlayer::loadingTrack,
            this,
            [this, player](TrackPointer newTrack, TrackPointer) {
                if (newTrack) {
                    m_loadingSince.insert(player, QDateTime::currentMSecsSinceEpoch());
                } else {
                    m_loadingSince.remove(player);
                }
            });
    connect(player, &BaseTrackPlayer::newTrackLoaded, this, [this, player](TrackPointer) {
        m_loadingSince.remove(player);
    });
    connect(player, &BaseTrackPlayer::playerEmpty, this, [this, player] { m_loadingSince.remove(player); });
    if (player->getGroup() == kPreviewDeck) {
        connect(player, &BaseTrackPlayer::newTrackLoaded, this, [this](TrackPointer track) {
            m_preview->onPreviewLoaded(libraryId(track));
        });
        connect(player, &BaseTrackPlayer::playerEmpty, this, [this] { m_preview->onPreviewLoaded(0); });
    }
    connect(player, &QObject::destroyed, this, [this, player] {
        m_watchedPlayers.remove(player);
        m_loadingSince.remove(player);
    });
}

bool DeckLabSystem::anyLoading() const {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_loadingSince.cbegin(); it != m_loadingSince.cend(); ++it) {
        if (now - it.value() < kLoadingStaleMs) {
            return true;
        }
    }
    return false;
}

QString DeckLabSystem::thermalWarning() const {
    if (m_undervoltage) {
        return tr("Power supply voltage is too low. Use the official Raspberry Pi power supply.");
    }
    const QString level = m_thermal.value(QStringLiteral("level")).toString();
    const double temperature = m_thermal.value(QStringLiteral("temp_c")).toDouble(0);
    if (level == QStringLiteral("hot")) {
        return tr("Deck Lab is hot (%1 °C). Give it air; performance may drop.")
                .arg(temperature, 0, 'f', 0);
    }
    if (level == QStringLiteral("warm")) {
        return tr("Deck Lab is warm (%1 °C).").arg(temperature, 0, 'f', 0);
    }
    return QString();
}

bool DeckLabSystem::anyPlaying() const {
    return decksOrSamplersPlaying() || controlValue(kPreviewDeck, QStringLiteral("play")) > 0;
}

decklab::Activity DeckLabSystem::activity() const {
    decklab::Activity activity;
    activity.playing = anyPlaying();
    // [Recording] status: 2 recording, 3 recording into the next split file.
    const int recording = static_cast<int>(controlValue(kRecording, QStringLiteral("status")));
    activity.recording = recording == 2 || recording == 3;
    activity.scanning = m_scanning;
    activity.utilityOpen = !m_utilities->openApp().isEmpty();
    return activity;
}

int DeckLabSystem::emptyPad() const {
    const auto loading = [this](BaseTrackPlayer* sampler) { return m_loadingSince.contains(sampler); };
    return firstEmptyPad(m_services.players, kPads, m_padTarget ? m_padTarget() : 0, loading);
}

int DeckLabSystem::firstEmptyPad(PlayerManagerInterface* players,
        int pads,
        int target,
        const std::function<bool(BaseTrackPlayer*)>& loading) {
    for (int pad = 1; players && pad <= pads; ++pad) {
        const QString group = QStringLiteral("[Sampler%1]").arg(pad);
        auto* sampler = players->getPlayer(group);
        if (!sampler || pad == target || controlValue(group, QStringLiteral("track_loaded")) > 0) {
            continue;
        }
        // A pad that is loading already holds its new track (track_loaded
        // is still 0 then).
        if (sampler->getLoadedTrack() || (loading && loading(sampler))) {
            continue;
        }
        return pad;
    }
    return 0;
}

bool DeckLabSystem::decksOrSamplersPlaying() const {
    const int decks = m_services.players ? m_services.players->numberOfDecks() : 4;
    for (int deck = 1; deck <= std::max(4, decks); ++deck) {
        if (controlValue(QStringLiteral("[Channel%1]").arg(deck), QStringLiteral("play")) > 0) {
            return true;
        }
    }
    const int samplers = m_services.players ? m_services.players->numberOfSamplers() : 16;
    for (int sampler = 1; sampler <= std::max(16, samplers); ++sampler) {
        if (controlValue(QStringLiteral("[Sampler%1]").arg(sampler), QStringLiteral("play")) > 0) {
            return true;
        }
    }
    return false;
}

void DeckLabSystem::setLoadedTracksProvider(std::function<QList<decklab::LoadedTrack>()> provider) {
    m_loadedTracksProvider = std::move(provider);
}

QList<decklab::LoadedTrack> DeckLabSystem::loadedTracks() const {
    if (m_loadedTracksProvider) {
        return m_loadedTracksProvider();
    }
    QList<decklab::LoadedTrack> tracks;
    auto* players = m_services.players;
    if (!players) {
        return tracks;
    }
    const auto add = [&tracks](const QString& group, BaseTrackPlayer* player) {
        if (!player) {
            return;
        }
        if (TrackPointer track = player->getLoadedTrack()) {
            tracks.append(decklab::LoadedTrack{group, track->getLocation()});
        }
    };
    for (int deck = 0; deck < players->numberOfDecks(); ++deck) {
        add(QStringLiteral("[Channel%1]").arg(deck + 1), players->getDeckBase(deck));
    }
    for (int sampler = 0; sampler < players->numberOfSamplers(); ++sampler) {
        add(QStringLiteral("[Sampler%1]").arg(sampler + 1), players->getSampler(sampler));
    }
    for (int preview = 0; preview < players->numberOfPreviewDecks(); ++preview) {
        add(QStringLiteral("[PreviewDeck%1]").arg(preview + 1), players->getPreviewDeck(preview));
    }
    return tracks;
}

bool DeckLabSystem::audioOutputConfigured() const {
    auto* sound = m_services.sound.get();
    return sound && !sound->getConfig().getOutputs().isEmpty() &&
            controlValue(QStringLiteral("[SoundManager]"), QStringLiteral("status")) == 2;
}

QString DeckLabSystem::recordingFolderFor(const QString& mountPath) {
    return QDir(mountPath).filePath(kRecordingFolder);
}

QString DeckLabSystem::recordingDirectory() const {
    return m_services.config
            ? m_services.config->getValueString(ConfigKey(kRecording, QStringLiteral("Directory")))
            : QString();
}

QString DeckLabSystem::ejectRefusal(const QJsonObject& drive) const {
    const QString mount = drive.value(QStringLiteral("mount")).toString();
    // r5-polish: the friendly name, as everywhere else.
    const QString name = decklab::usbDriveName(drive, m_drives);
    const QString refusal = decklab::ejectRefusal(name, decklab::ejectBlockers(mount, loadedTracks()));
    if (!refusal.isEmpty()) {
        return refusal;
    }
    if (controlValue(kRecording, QStringLiteral("status")) > 0 &&
            decklab::isPathOnMount(recordingDirectory(), mount)) {
        return tr("Can't eject %1 while your mix is recording to it. Stop recording first.").arg(name);
    }
    return QString();
}

void DeckLabSystem::ejectDrive(const QJsonObject& drive, Done done) {
    const QString refusal = ejectRefusal(drive);
    if (!refusal.isEmpty()) {
        done(false, refusal);
        return;
    }
    const QString id = drive.value(QStringLiteral("id")).toString();
    m_client->call(QStringLiteral("usb.eject"),
            QJsonObject{{QStringLiteral("id"), id}},
            [this, id, done](const DeckLabdReply& reply) {
                if (!reply.ok) {
                    done(false, reply.errorMessage.isEmpty() ? tr("Eject failed") : reply.errorMessage);
                    return;
                }
                if (m_services.config &&
                        m_services.config->getValueString(ConfigKey(kDeckLab, QStringLiteral("RecordingDrive"))) == id) {
                    forgetMissingRecordingDrive();
                }
                done(true, tr("Ejecting… remove the drive when it disappears from this list."));
            });
}

void DeckLabSystem::useDriveForRecordings(const QJsonObject& drive, Done done) {
    const QString id = drive.value(QStringLiteral("id")).toString();
    const QString mount = drive.value(QStringLiteral("mount")).toString();
    if (id.isEmpty() || mount.isEmpty() || !m_services.config) {
        done(false, tr("This drive can't be used."));
        return;
    }
    if (controlValue(kRecording, QStringLiteral("status")) > 0) {
        done(false, tr("Stop the current recording first."));
        return;
    }
    if (drive.value(QStringLiteral("free_mb")).toDouble(0) < kMinimumRecordingFreeMb) {
        done(false, tr("This drive has less than 1 GB free."));
        return;
    }
    m_client->call(QStringLiteral("usb.set_writable"),
            QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("writable"), true}},
            [this, id, mount, done](const DeckLabdReply& reply) {
                if (!reply.ok || reply.result.value(QStringLiteral("read_only")).toBool(true)) {
                    done(false, reply.errorMessage.isEmpty() ? tr("The drive could not be made writable.") : reply.errorMessage);
                    return;
                }
                auto config = m_services.config;
                const QString folder = recordingFolderFor(mount);
                if (!QDir().mkpath(folder)) {
                    done(false, tr("Could not create “%1” on the drive.").arg(kRecordingFolder));
                    return;
                }
                const ConfigKey directory(kRecording, QStringLiteral("Directory"));
                const ConfigKey before(kDeckLab, QStringLiteral("RecordingDirectoryBefore"));
                if (config->getValueString(ConfigKey(kDeckLab, QStringLiteral("RecordingDrive"))).isEmpty()) {
                    config->setValue(before, config->getValueString(directory));
                }
                // RecordingManager reads the directory when recording starts:
                // no restart needed.
                config->setValue(directory, folder);
                config->setValue(ConfigKey(kRecording, QStringLiteral("Encoding")), QStringLiteral("WAV"));
                config->setValue(ConfigKey(kDeckLab, QStringLiteral("RecordingDrive")), id);
                config->save();
                done(true, tr("Recordings go to %1.").arg(folder));
            });
}

void DeckLabSystem::forgetMissingRecordingDrive() {
    auto config = m_services.config;
    if (!config) {
        return;
    }
    const ConfigKey driveKey(kDeckLab, QStringLiteral("RecordingDrive"));
    const QString id = config->getValueString(driveKey);
    if (id.isEmpty() || controlValue(kRecording, QStringLiteral("status")) > 0) {
        return;
    }
    for (const auto& drive : std::as_const(m_drives)) {
        if (drive.toObject().value(QStringLiteral("id")).toString() == id) {
            return;
        }
    }
    // Never let RecordingManager recreate a vanished mount point on the SD card.
    config->setValue(ConfigKey(kRecording, QStringLiteral("Directory")),
            config->getValueString(ConfigKey(kDeckLab, QStringLiteral("RecordingDirectoryBefore"))));
    config->remove(driveKey);
    config->save();
}

QList<decklab::ControllerEndpoint> DeckLabSystem::controllerEndpoints() const {
    if (m_controllerHooks.endpoints) {
        return m_controllerHooks.endpoints();
    }
    QList<decklab::ControllerEndpoint> result;
    auto* manager = m_services.controllers.get();
    if (!manager) {
        return result;
    }
    const auto all = manager->getControllers();
    for (Controller* controller : all) {
        if (!controller->isInputDevice()) {
            continue;
        }
        decklab::ControllerEndpoint endpoint;
        endpoint.name = controller->getName();
        endpoint.hid = controller->getDataRepresentationProtocol() == DataRepresentationProtocol::HID;
        endpoint.open = controller->isOpen();
        endpoint.mapping = manager->getConfiguredMappingFileForDevice(endpoint.name);
        endpoint.enabled = m_services.config &&
                m_services.config->getValue(
                        ConfigKey(QStringLiteral("[Controller]"), controllerConfigItem(endpoint.name)), 0) == 1;
        endpoint.vendorId = controller->getVendorId();
        endpoint.productId = controller->getProductId();
        endpoint.serial = controller->getSerialNumber();
        if (const auto number = controller->getUsbInterfaceNumber()) {
            endpoint.interfaceNumber = static_cast<int>(*number);
        }
#ifdef __HID__
        if (const auto* hid = qobject_cast<const HidController*>(controller)) {
            endpoint.usagePage = hid->getUsagePage();
            endpoint.product = hid->getProductString();
            const QString path = hid->getDevicePath();
            endpoint.devicePath = path;
            if (!m_hidLocations.contains(path)) {
                m_hidLocations.insert(path, decklab::usbpath::hidrawLocation(path));
            }
            endpoint.usbPath = m_hidLocations.value(path);
        }
#endif
        if (!endpoint.hid) {
            if (!m_midiLocations) {
                m_midiLocations = decklab::usbpath::midiPortLocations();
            }
            endpoint.usbPath = m_midiLocations->value(endpoint.name);
        }
        result.append(endpoint);
    }
    return result;
}

void DeckLabSystem::setControllerHooksForTesting(ControllerHooks hooks) {
    m_controllerHooks = std::move(hooks);
    forgetControllerCache();
}

void DeckLabSystem::forgetControllerCache() {
    m_midiLocations.reset();
    m_hidLocations.clear();
    m_hidMatches.clear();
}

bool DeckLabSystem::hidMappingMatches(const decklab::ControllerEndpoint& endpoint) const {
    const QString key = QStringLiteral("%1/%2/%3/%4/%5")
                                .arg(endpoint.name)
                                .arg(endpoint.vendorId.value_or(0))
                                .arg(endpoint.productId.value_or(0))
                                .arg(endpoint.interfaceNumber.value_or(-1))
                                .arg(endpoint.usagePage.value_or(0));
    const auto cached = m_hidMatches.constFind(key);
    if (cached != m_hidMatches.constEnd()) {
        return cached.value();
    }
    ControllerInfo probe;
    probe.mixxxName = endpoint.name;
    probe.name = endpoint.product;
    probe.hid = true;
    probe.vendorId = endpoint.vendorId;
    probe.productId = endpoint.productId;
    probe.interfaceNumber = endpoint.interfaceNumber;
    const auto matches = suggestMappings(probe);
    const bool made = !matches.isEmpty() && matches.first().score >= ControllerMappingMatcher::kConfidentScore;
    m_hidMatches.insert(key, made);
    return made;
}

QList<decklab::ControllerGroup> DeckLabSystem::controllerGroups() const {
    return decklab::groupControllers(controllerEndpoints(),
            decklab::reportedControllers(m_devices),
            [this](const decklab::ControllerEndpoint& endpoint) { return hidMappingMatches(endpoint); });
}

QList<DeckLabSystem::ControllerInfo> DeckLabSystem::controllers() const {
    QList<ControllerInfo> result;
    const auto reported = decklab::reportedControllers(m_devices);
    const auto groups = controllerGroups();
    for (const auto& group : groups) {
        ControllerInfo info;
        const decklab::ReportedController* report =
                group.reported >= 0 && group.reported < reported.size() ? &reported.at(group.reported) : nullptr;
        const decklab::ControllerEndpoint* primary = group.primaryEndpoint();
        if (report) {
            info.key = report->key;
            info.name = report->name;
            info.vendorId = report->vendorId;
            info.productId = report->productId;
            info.audioCard = report->audioCard;
            // Not enumerated by Mixxx yet: MIDI when it has ports.
            info.hid = report->hid && report->midiPorts.isEmpty();
        }
        if (primary) {
            info.mixxxName = primary->name;
            info.hid = primary->hid;
            info.open = decklab::isWorking(*primary);
            info.mapping = primary->mapping;
            info.interfaceNumber = primary->interfaceNumber;
            if (info.key.isEmpty()) {
                info.key = decklab::DeviceStore::deviceKey(
                        primary->vendorId, primary->productId, primary->serial, primary->name);
            }
            if (info.name.isEmpty()) {
                info.name = primary->hid && !primary->product.trimmed().isEmpty()
                        ? primary->product.trimmed()
                        : ControllerMappingMatcher::modelName(primary->name);
            }
            info.interfaces.append(primary->name);
        }
        for (int index = 0; index < group.endpoints.size(); ++index) {
            const auto& endpoint = group.endpoints.at(index);
            if (index != group.primary) {
                info.interfaces.append(endpoint.name);
            }
            if (group.suppressed.contains(index)) {
                info.suppressed.append(endpoint.name);
            }
            // USB ids from any interface (the HID side has them, MIDI does not).
            if (!info.vendorId && endpoint.vendorId && endpoint.productId) {
                info.vendorId = endpoint.vendorId;
                info.productId = endpoint.productId;
            }
        }
        result.append(info);
    }
    return result;
}

bool DeckLabSystem::disableInterface(const decklab::ControllerEndpoint& endpoint) {
    if (m_controllerHooks.disable) {
        m_controllerHooks.disable(endpoint);
        return true;
    }
    auto manager = m_services.controllers;
    if (!manager) {
        return false;
    }
    // Preferences' "disable" (close it, enabled = 0, no mapping saved), on the
    // controller thread. Queued, never blocking: a controller opening there
    // may itself wait for this thread (screens). The device is looked up
    // there, so a rescan in between can't leave a stale pointer. Twins share
    // a name: the HID device path picks the right one (no path: every one).
    const QString name = endpoint.name;
    const QString path = endpoint.devicePath;
    QMetaObject::invokeMethod(
            manager.get(),
            [manager, name, path]() {
                const auto all = manager->getControllers();
                for (Controller* controller : all) {
                    if (controller->getName() != name) {
                        continue;
                    }
#ifdef __HID__
                    const auto* hid = qobject_cast<const HidController*>(controller);
                    if (!path.isEmpty() && (!hid || hid->getDevicePath() != path)) {
                        continue;
                    }
#endif
                    manager->slotApplyMapping(controller, nullptr, false);
                }
            },
            Qt::QueuedConnection);
    return true;
}

QStringList DeckLabSystem::closeStrayInterfaces() {
    QStringList closed;
    if (!m_controllerHooks.endpoints && !m_services.controllers) {
        return closed;
    }
    const auto groups = controllerGroups();
    for (const auto& group : groups) {
        for (int index : group.suppressed) {
            const auto& endpoint = group.endpoints.at(index);
            // Open now, or Mixxx would open it at the next start or rescan.
            const bool armed = endpoint.open || (endpoint.enabled && !endpoint.mapping.isEmpty());
            if (armed && disableInterface(endpoint)) {
                qInfo() << "DeckLab: closed" << endpoint.name << "beside the MIDI port in use"
                        << (group.primaryEndpoint() ? group.primaryEndpoint()->name : QString());
                closed.append(endpoint.name);
            }
        }
    }
    if (!closed.isEmpty() && m_services.config && !m_controllerHooks.disable) {
        // Keep the repair across a crash: save once the controller thread has
        // applied it.
        QTimer::singleShot(1000, this, [this] {
            if (m_services.config) {
                m_services.config->save();
            }
        });
    }
    return closed;
}

QString DeckLabSystem::mixxxNameForKey(const QString& key) const {
    const auto all = controllers();
    for (const auto& info : all) {
        if (info.key == key) {
            return info.mixxxName;
        }
    }
    return QString();
}

QList<MappingInfo> DeckLabSystem::mappings(bool hid) const {
    const QString extension = hid ? QString::fromLatin1(HID_MAPPING_EXTENSION)
                                  : QString::fromLatin1(MIDI_MAPPING_EXTENSION);
    QList<MappingInfo> result;
    if (auto* manager = m_services.controllers.get()) {
        auto user = manager->getMainThreadUserMappingEnumerator();
        auto system = manager->getMainThreadSystemMappingEnumerator();
        if (user) {
            user->loadSupportedMappings();
            result += user->getMappingsByExtension(extension);
        }
        if (system) {
            result += system->getMappingsByExtension(extension);
            return result;
        }
    }
    if (!m_services.config) {
        return result;
    }
    // No ControllerManager (tests) or not initialized yet: read directly,
    // once per extension.
    if (!m_ownMappings.contains(extension)) {
        MappingInfoEnumerator user(userMappingsPath(m_services.config));
        MappingInfoEnumerator system(resourceMappingsPath(m_services.config));
        m_ownMappings.insert(extension,
                user.getMappingsByExtension(extension) + system.getMappingsByExtension(extension));
    }
    if (result.isEmpty()) {
        return m_ownMappings.value(extension);
    }
    return result;
}

QList<ControllerMappingMatcher::Match> DeckLabSystem::suggestMappings(const ControllerInfo& controller) const {
    ControllerMappingMatcher::Device device;
    device.name = controller.mixxxName.isEmpty() ? controller.name : controller.mixxxName;
    device.vendorId = controller.vendorId;
    device.productId = controller.productId;
    device.interfaceNumber = controller.interfaceNumber;
    device.hid = controller.hid;
    QString overrides;
    if (m_services.config) {
        overrides = QDir(m_services.config->getSettingsPath()).filePath(QStringLiteral("decklab-controller-overrides.json"));
    }
    auto matches = ControllerMappingMatcher::rank(device,
            mappings(controller.hid),
            ControllerMappingMatcher::loadOverrides(overrides));
    if (m_services.config) {
        // A user's own mapping for the same model (e.g. a Deck Lab overlay)
        // wins a tie with the bundled one.
        const QString userPath = QDir(userMappingsPath(m_services.config)).absolutePath();
        for (auto& match : matches) {
            if (QFileInfo(match.mapping.getPath()).absolutePath() == userPath) {
                match.score += 3;
            }
        }
        std::stable_sort(matches.begin(), matches.end(), [](const auto& left, const auto& right) {
            return left.score > right.score;
        });
    }
    return matches;
}

int DeckLabSystem::suggestedDeckCount(const QString& mappingPath) const {
    if (mappingPath.isEmpty() || !m_services.config) {
        return 2;
    }
    return ControllerMappingMatcher::suggestedDeckCount(
            MappingInfo(mappingPath), resourceMappingsPath(m_services.config));
}

bool DeckLabSystem::applyMapping(const QString& controllerName, const QString& mappingPath, QString* error) {
    // r5-polish: never open a HID interface beside the MIDI port in use, and
    // never give an interface a mapping made for the other kind (a MIDI
    // mapping on a HID interface still opens the device).
    const auto groups = controllerGroups();
    for (const auto& group : groups) {
        for (int index = 0; index < group.endpoints.size(); ++index) {
            const auto& endpoint = group.endpoints.at(index);
            if (endpoint.name != controllerName) {
                continue;
            }
            const auto* primary = group.primaryEndpoint();
            if (group.suppressed.contains(index)) {
                qWarning() << "DeckLab: refused to open" << controllerName << "beside"
                           << (primary ? primary->name : QString());
                if (error) {
                    *error = tr("Deck Lab plays %1 through its MIDI port, so its HID interface stays closed.")
                                     .arg(ControllerMappingMatcher::modelName(primary ? primary->name : controllerName));
                }
                return false;
            }
            if (!mappingPath.isEmpty() && !decklab::mappingFits(endpoint, mappingPath)) {
                qWarning() << "DeckLab: refused" << QFileInfo(mappingPath).fileName() << "for" << controllerName;
                if (error) {
                    *error = endpoint.hid ? tr("That mapping is for a MIDI controller. Choose a HID mapping.")
                                          : tr("That mapping is for a HID controller. Choose a MIDI mapping.");
                }
                return false;
            }
        }
    }
    if (m_controllerHooks.open) {
        if (!m_controllerHooks.open(controllerName, mappingPath, error)) {
            return false;
        }
        emit controllersChanged();
        return true;
    }
    auto manager = m_services.controllers;
    if (!manager || !m_services.config) {
        if (error) {
            *error = tr("Controller support is not running.");
        }
        return false;
    }
    Controller* target = nullptr;
    const auto all = manager->getControllers();
    for (Controller* controller : all) {
        if (controller->getName() == controllerName) {
            target = controller;
        }
    }
    if (!target) {
        if (error) {
            *error = tr("%1 is not detected. Tap Detect again.").arg(controllerName);
        }
        return false;
    }
    const QFileInfo file(mappingPath);
    std::shared_ptr<LegacyControllerMapping> mapping;
    if (file.exists()) {
        mapping = LegacyControllerMappingFileHandler::loadMapping(
                file, QDir(resourceMappingsPath(m_services.config)));
    }
    if (!mapping) {
        if (error) {
            *error = tr("The mapping %1 could not be read.").arg(file.fileName());
        }
        return false;
    }
    mapping->loadSettings(m_services.config, target->getName());
    mapping->setDirty(false);
    // Same path as Preferences → Controllers: the controller thread clones
    // the mapping, saves it as the device's mapping, enables and opens it.
    const auto apply = [manager, target, mapping]() {
        manager->slotApplyMapping(target, mapping, true);
    };
    if (manager->thread() == QThread::currentThread()) {
        apply();
    } else {
        QMetaObject::invokeMethod(manager.get(), apply, Qt::BlockingQueuedConnection);
    }
    m_services.config->save();
    emit controllersChanged();
    return true;
}

bool DeckLabSystem::preferencesOpen() const {
    const auto windows = QApplication::topLevelWidgets();
    for (QWidget* window : windows) {
        if (window->isVisible() && window->inherits("DlgPreferences")) {
            return true;
        }
    }
    return false;
}

bool DeckLabSystem::canRescanControllers(QString* reason) const {
    if (!m_services.controllers && !m_controllerHooks.rescan) {
        if (reason) {
            *reason = tr("Controller support is not running.");
        }
        return false;
    }
    if (preferencesOpen()) {
        if (reason) {
            *reason = tr("Close Advanced settings first.");
        }
        return false;
    }
    return true;
}

bool DeckLabSystem::rescanControllers(QString* reason) {
    if (m_rescanning) {
        return true;
    }
    if (!canRescanControllers(reason)) {
        return false;
    }
    m_rescanning = true;
    if (m_controllerHooks.rescan) {
        m_controllerHooks.rescan();
        QTimer::singleShot(0, this, &DeckLabSystem::onRescanFinished);
        return true;
    }
    m_services.controllers->rescanDevices();
    return true;
}

void DeckLabSystem::onRescanFinished() {
    m_rescanning = false;
    forgetControllerCache();
    closeStrayInterfaces();
    emit controllersChanged();
    emit controllersRescanned();
    const auto pending = m_pendingReapply;
    m_pendingReapply.clear();
    for (const auto& profile : pending) {
        reapply(profile, true);
    }
}

bool DeckLabSystem::restartToDetect(QString* reason) {
    if (anyPlaying()) {
        if (reason) {
            *reason = tr("Stop all decks and samplers first.");
        }
        return false;
    }
    if (!m_client->isReady()) {
        if (reason) {
            *reason = tr("System service offline.");
        }
        return false;
    }
    m_client->call(QStringLiteral("session.restart_mixxx"),
            QJsonObject{{QStringLiteral("reason"), QStringLiteral("detect controller")}});
    return true;
}

void DeckLabSystem::restartDeckLab() {
    if (m_client->isReady()) {
        m_client->call(QStringLiteral("session.restart_mixxx"),
                QJsonObject{{QStringLiteral("reason"), QStringLiteral("user restart")}},
                [this](const DeckLabdReply& reply) {
                    if (!reply.ok) {
                        emit gracefulQuitRequested();
                    }
                });
        return;
    }
    emit gracefulQuitRequested();
}

QString DeckLabSystem::audioApi() const {
    auto* sound = m_services.sound.get();
    if (!sound) {
        return QString();
    }
    QString api = sound->getConfig().getAPI();
    const QList<QString> apis = sound->getHostAPIList();
    if (api.isEmpty() || api == SoundManagerConfig::kDefaultAPI || !apis.contains(api)) {
        api = apis.contains(QString::fromLatin1(MIXXX_PORTAUDIO_ALSA_STRING))
                ? QString::fromLatin1(MIXXX_PORTAUDIO_ALSA_STRING)
                : (apis.isEmpty() ? QString() : apis.first());
    }
    return api;
}

QList<decklab::AudioDevice> DeckLabSystem::audioDevices(const ControllerInfo* controller) const {
    if (m_hasAudioOverride) {
        return m_audioOverride;
    }
    QList<decklab::AudioDevice> result;
    auto* sound = m_services.sound.get();
    if (!sound) {
        return result;
    }
    const QJsonArray cards = m_devices.value(QStringLiteral("audio_cards")).toArray();
    const auto devices = sound->getDeviceList(audioApi(), true, false);
    for (const auto& device : devices) {
        const int outputs = device->getNumOutputChannels();
        if (outputs < 2) {
            continue;
        }
        const SoundDeviceId id = device->getDeviceId();
        const int cardNumber = alsaCardNumber(id.alsaHwDevice);
        const QString cardName = id.name.section(QLatin1Char(':'), 0, 0).trimmed();
        const auto sameCard = [&](QJsonObject card) {
            // A controller's audio_card has no index; resolve it via audio_cards.
            if (!card.contains(QStringLiteral("index"))) {
                for (const auto& value : cards) {
                    const QJsonObject listed = value.toObject();
                    if (listed.value(QStringLiteral("alsa_id")) == card.value(QStringLiteral("alsa_id"))) {
                        card = listed;
                    }
                }
            }
            if (card.contains(QStringLiteral("index")) && cardNumber >= 0) {
                return card.value(QStringLiteral("index")).toInt(-1) == cardNumber;
            }
            const int number = alsaCardNumber(card.value(QStringLiteral("alsa_id")).toString());
            if (number >= 0 && cardNumber >= 0) {
                return number == cardNumber;
            }
            const QString compactCard = compact(cardName);
            return !compactCard.isEmpty() &&
                    (compactCard == compact(card.value(QStringLiteral("name")).toString()) ||
                            compactCard == compact(card.value(QStringLiteral("alsa_id")).toString()));
        };
        bool usb = false;
        for (const auto& card : cards) {
            if (sameCard(card.toObject())) {
                usb = card.toObject().value(QStringLiteral("usb")).toBool();
            }
        }
        decklab::AudioDevice entry;
        entry.id = id.name;
        entry.name = decklab::prettyAudioName(id.name);
        entry.outputs = outputs;
        entry.kind = decklab::classifyAudioDevice(id.name, id.alsaHwDevice, usb);
        if (controller) {
            if (!controller->audioCard.isEmpty()) {
                entry.controllerCard = sameCard(controller->audioCard);
            } else {
                const QString model = compact(ControllerMappingMatcher::modelName(
                        controller->mixxxName.isEmpty() ? controller->name : controller->mixxxName));
                entry.controllerCard = !model.isEmpty() && compact(cardName) == model;
            }
        }
        result.append(entry);
    }
    return result;
}

decklab::AudioPlan DeckLabSystem::currentAudioPlan() const {
    decklab::AudioPlan plan;
    auto* sound = m_services.sound.get();
    if (!sound) {
        return plan;
    }
    const auto outputs = sound->getConfig().getOutputs();
    for (auto it = outputs.cbegin(); it != outputs.cend(); ++it) {
        const auto type = it.value().getType();
        const decklab::AudioRoute route{it.key().name, it.value().getChannelGroup().getChannelBase()};
        if (type == AudioPathType::Main) {
            plan.main = route;
        } else if (type == AudioPathType::Headphones) {
            plan.headphones = route;
        }
    }
    return plan;
}

bool DeckLabSystem::applyAudio(const decklab::AudioPlan& plan, QString* error) {
    auto* sound = m_services.sound.get();
    if (!sound) {
        if (error) {
            *error = tr("Audio is not running.");
        }
        return false;
    }
    const QString api = audioApi();
    SoundManagerConfig config = sound->getConfig();
    if (config.getAPI() != api) {
        config.setAPI(api);
        config.clearInputs();
    }
    // Buffer size, sample rate and inputs stay as they are.
    config.clearOutputs();
    const auto devices = sound->getDeviceList(api, true, false);
    const auto add = [&](const decklab::AudioRoute& route, AudioPathType type) {
        if (!route.isValid()) {
            return true;
        }
        for (const auto& device : devices) {
            if (device->getDeviceId().name == route.device &&
                    device->getNumOutputChannels() >= route.channelBase + 2) {
                config.addOutput(device->getDeviceId(),
                        AudioOutput(type,
                                static_cast<unsigned char>(route.channelBase),
                                mixxx::audio::ChannelCount::stereo(),
                                0));
                return true;
            }
        }
        return false;
    };
    if (!add(plan.main, AudioPathType::Main) || !add(plan.headphones, AudioPathType::Headphones)) {
        if (error) {
            *error = tr("That output is no longer connected.");
        }
        return false;
    }
    const SoundDeviceStatus status = sound->setConfig(config);
    if (status != SoundDeviceStatus::Ok) {
        if (error) {
            *error = sound->getLastErrorMessage(status);
        }
        return false;
    }
    emit audioChanged();
    return true;
}

void DeckLabSystem::playTestTone(const decklab::AudioRoute& route, int side, Done done, double dbfs) {
    auto* sound = m_services.sound.get();
    if (!sound || !route.isValid()) {
        done(false, tr("Choose an output first."));
        return;
    }
    if (m_toneRunning) {
        done(false, tr("A test is already playing."));
        return;
    }
    if (anyPlaying()) {
        done(false, tr("Stop all decks and samplers to test outputs."));
        return;
    }
    int deviceIndex = -1;
    const auto devices = sound->getDeviceList(audioApi(), true, false);
    for (const auto& device : devices) {
        if (device->getDeviceId().name == route.device &&
                device->getNumOutputChannels() >= route.channelBase + 2) {
            deviceIndex = device->getDeviceId().portAudioIndex;
        }
    }
    if (deviceIndex < 0) {
        done(false, tr("That output is no longer connected."));
        return;
    }
    double rate = sound->getConfig().getSampleRate().toDouble();
    if (rate <= 0) {
        rate = 48000;
    }
    m_toneRunning = true;
    // Mixxx's streams must release the device; nothing is playing.
    sound->closeActiveConfig();
    auto result = std::make_shared<QString>();
    const int base = route.channelBase;
    const double amplitude = decklab::testToneAmplitude(dbfs);
    QThread* worker = QThread::create([deviceIndex, base, side, rate, amplitude, result] {
        *result = playToneOnDevice(deviceIndex, base, side, rate, amplitude);
    });
    connect(worker, &QThread::finished, this, [this, worker, result, done] {
        worker->deleteLater();
        if (auto* sound = m_services.sound.get()) {
            sound->setupDevices();
        }
        m_toneRunning = false;
        emit audioChanged();
        done(result->isEmpty(), result->isEmpty() ? QString() : *result);
    });
    worker->start();
}

void DeckLabSystem::rememberSetup(const decklab::DeviceProfile& profile) {
    if (!m_services.config) {
        return;
    }
    if (!profile.key.trimmed().isEmpty()) {
        deviceStore().save(profile);
    }
    m_services.config->setValue(ConfigKey(kDeckLab, QStringLiteral("DeckLayout")), profile.decks == 4 ? 4 : 2);
    m_services.config->save();
}

bool DeckLabSystem::firstRunDone() const {
    return m_services.config &&
            m_services.config->getValue(ConfigKey(kDeckLab, QStringLiteral("first_run_done")), 0) == 1;
}

void DeckLabSystem::markFirstRunDone() {
    if (!m_services.config) {
        return;
    }
    m_services.config->setValue(ConfigKey(kDeckLab, QStringLiteral("first_run_done")), 1);
    m_services.config->remove(ConfigKey(kDeckLab, QStringLiteral("first_run_deferred")));
    m_services.config->save();
    emit firstRunChanged();
}

bool DeckLabSystem::firstRunDeferred() const {
    return m_services.config &&
            m_services.config->getValue(ConfigKey(kDeckLab, QStringLiteral("first_run_deferred")), 0) == 1;
}

void DeckLabSystem::deferFirstRun() {
    if (!m_services.config || firstRunDone()) {
        return;
    }
    m_services.config->setValue(ConfigKey(kDeckLab, QStringLiteral("first_run_deferred")), 1);
    m_services.config->save();
    emit firstRunChanged();
}

void DeckLabSystem::onDevicesChanged(const QJsonObject& data, const QStringList& added) {
    m_devices = data;
    forgetControllerCache();
    emit devicesChanged();
    const QJsonArray reported = data.value(QStringLiteral("controllers")).toArray();
    for (const QString& key : added) {
        QJsonObject device;
        for (const auto& value : reported) {
            if (value.toObject().value(QStringLiteral("key")).toString() == key) {
                device = value.toObject();
            }
        }
        if (device.isEmpty()) {
            continue; // an audio card or other device
        }
        const QString name = device.value(QStringLiteral("name")).toString();
        auto profile = deviceStore().find(key);
        if (!profile) {
            for (const auto& port : device.value(QStringLiteral("midi_ports")).toArray()) {
                if (!profile) {
                    profile = deviceStore().findByController(port.toString());
                }
            }
        }
        emit controllerArrived(key, name, profile.has_value());
        if (profile) {
            reapply(*profile, false);
        } else if (!anyPlaying()) {
            // A new controller: enumerate it now so Deck Setup can enable it.
            QString reason;
            rescanControllers(&reason);
        }
    }
}

void DeckLabSystem::reapply(const decklab::DeviceProfile& profile, bool afterRescan) {
    const auto all = controllers();
    // r5-polish: one entry per physical controller; the profile's Mixxx name
    // may be any of its interfaces (an earlier build saved the HID one).
    const auto isProfile = [&profile](const ControllerInfo& info) {
        return info.key == profile.key ||
                (!profile.controller.isEmpty() && info.interfaces.contains(profile.controller));
    };
    if (!afterRescan && m_services.controllers) {
        // A replugged controller needs fresh MIDI/HID streams even when Mixxx
        // still lists the old device. Re-enumerate when that can't disturb a
        // playing set: idle, or no other controller is running.
        bool otherOpen = false;
        for (const auto& info : all) {
            otherOpen |= info.open && !isProfile(info);
        }
        if (anyPlaying() && otherOpen) {
            emit notice(tr("%1 is back. Stop the music, then open Deck Setup to reconnect it.").arg(profile.name));
            return;
        }
        QString reason;
        if (rescanControllers(&reason)) {
            if (!m_pendingReapply.contains(profile)) {
                m_pendingReapply.append(profile);
            }
            return;
        }
        emit notice(reason);
    }
    const ControllerInfo* current = nullptr;
    for (const auto& info : all) {
        if (isProfile(info) && !info.mixxxName.isEmpty() && !current) {
            current = &info;
        }
    }
    if (!current) {
        emit notice(tr("%1 was not detected. Open Deck Setup and tap Detect again or Restart to detect.")
                            .arg(profile.name));
        return;
    }
    if (!current->open && !profile.mapping.isEmpty()) {
        QString error;
        if (!applyMapping(current->mixxxName, profile.mapping, &error)) {
            emit notice(error);
            return;
        }
    }
    reapplyAudio(profile);
    emit layoutRequested(profile.decks);
    emit notice(tr("%1 is ready.").arg(profile.name.isEmpty() ? current->name : profile.name));
}

void DeckLabSystem::reapplyAudio(const decklab::DeviceProfile& profile) {
    auto* sound = m_services.sound.get();
    if (!sound || profile.mainDevice.isEmpty()) {
        return;
    }
    decklab::AudioPlan wanted;
    wanted.main = decklab::AudioRoute{profile.mainDevice, profile.mainBase};
    wanted.headphones = decklab::AudioRoute{profile.headphonesDevice, profile.headphonesBase};
    const auto current = currentAudioPlan();
    if (current.main == wanted.main && current.headphones == wanted.headphones && audioOutputConfigured()) {
        return;
    }
    if (anyPlaying() && audioOutputConfigured()) {
        // Never move live audio mid-mix; Deck Setup offers it later.
        emit notice(tr("%1 audio is available. Audio stays on the current output until the music stops.")
                            .arg(profile.name));
        return;
    }
    // A hot-plugged card needs a fresh PortAudio device list.
    sound->clearAndQueryDevices();
    QString error;
    if (!applyAudio(wanted, &error)) {
        // Card not found: keep whatever plays now and say why.
        sound->setupDevices();
        emit notice(error);
    }
}
