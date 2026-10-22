// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <optional>

#include "preferences/usersettings.h"

// Pure Deck Setup policy: audio routing, eject safety and per-controller
// persistence. No hardware, sockets or widgets, so it is unit-testable.
namespace decklab {

struct AudioDevice {
    enum class Kind {
        External,    // USB interface/DAC or another real card (HAT DAC)
        BuiltinJack, // Pi 3.5 mm headphone jack (bcm2835)
        Hdmi,
        Network,     // PipeWire/Pulse/Bluetooth: high latency, never automatic
    };
    QString id;   // opaque, stable within one enumeration
    QString name; // human readable
    int outputs = 0;
    Kind kind = Kind::External;
    bool controllerCard = false; // audio interface built into the controller
};

struct AudioRoute {
    QString device;
    int channelBase = -1; // 0 = outputs 1-2
    bool isValid() const {
        return !device.isEmpty() && channelBase >= 0;
    }
    bool operator==(const AudioRoute& other) const {
        return device == other.device && channelBase == other.channelBase;
    }
};

struct AudioPlan {
    AudioRoute main;
    AudioRoute headphones;
    QString reason; // one sentence for the wizard
};

/// Classifies a PortAudio/ALSA device by its names.
AudioDevice::Kind classifyAudioDevice(const QString& name, const QString& alsaHwDevice, bool usb);
bool isHighLatency(const AudioDevice& device);
/// "MixTrack Pro 3: USB Audio" -> "MixTrack Pro 3", "bcm2835 Headphones: -"
/// -> "Deck Lab headphone jack", "pipewire" -> "System audio (Bluetooth)".
QString prettyAudioName(const QString& deviceName);

/// The Deck Setup policy:
///  - controller with 4+ outputs: Main 1-2, Headphones 3-4 on it;
///  - controller with 2 outputs: Main on it, headphones on `headphonesChoice`
///    (Pi jack or another device) or none;
///  - no controller audio: Main on an external DAC (Headphones 3-4 when it has
///    4+ outputs), else the Pi headphone jack, else HDMI.
/// Network/Bluetooth outputs are only used when chosen explicitly.
AudioPlan planAudio(const QList<AudioDevice>& devices, const QString& headphonesChoice = QString());

/// "Outputs 1–2" style label for a channel pair.
QString channelPairLabel(int channelBase);

struct LoadedTrack {
    QString group; // "[Channel1]", "[Sampler3]", "[PreviewDeck1]"
    QString location;
};
bool isPathOnMount(const QString& path, const QString& mountPath);
/// Players whose loaded track lives on the drive mounted at `mountPath`.
QStringList ejectBlockers(const QString& mountPath, const QList<LoadedTrack>& loaded);
/// "Deck 1" / "Sampler 3" / "Preview".
QString playerLabel(const QString& group);
/// User-facing refusal, empty when ejecting is safe.
QString ejectRefusal(const QString& driveName, const QStringList& blockers);

/// One remembered controller setup, saved in [DeckLabDevices].
struct DeviceProfile {
    QString key;        // vid:pid[:serial] or the controller name
    QString name;       // display name
    QString controller; // Mixxx controller name (port name)
    QString mapping;    // mapping file path
    int decks = 2;
    QString mainDevice; // SoundDeviceId name
    int mainBase = -1;
    QString headphonesDevice;
    int headphonesBase = -1;

    QJsonObject toJson() const;
    static DeviceProfile fromJson(const QString& key, const QJsonObject& object);
    bool operator==(const DeviceProfile& other) const;
};

class DeviceStore {
  public:
    static const QString kGroup; // "[DeckLabDevices]"
    explicit DeviceStore(UserSettingsPointer config);
    std::optional<DeviceProfile> find(const QString& key) const;
    /// Also matches by Mixxx controller name when the USB key is unknown.
    std::optional<DeviceProfile> findByController(const QString& controllerName) const;
    void save(const DeviceProfile& profile);
    bool remove(const QString& key);
    QList<DeviceProfile> all() const;

    /// Config item names cannot contain whitespace or brackets.
    static QString configItem(const QString& key);
    static QString deviceKey(std::optional<uint16_t> vendorId,
            std::optional<uint16_t> productId,
            const QString& serial,
            const QString& fallbackName);

  private:
    UserSettingsPointer m_config;
};

} // namespace decklab
