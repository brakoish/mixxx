// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabsetupmodel.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace decklab {

namespace {
QString tr(const char* text) {
    return QCoreApplication::translate("DeckLabSetup", text);
}

const AudioDevice* findDevice(const QList<AudioDevice>& devices, const QString& id) {
    for (const auto& device : devices) {
        if (device.id == id) {
            return &device;
        }
    }
    return nullptr;
}

const AudioDevice* firstOfKind(const QList<AudioDevice>& devices, AudioDevice::Kind kind) {
    const AudioDevice* best = nullptr;
    for (const auto& device : devices) {
        if (device.kind != kind || device.controllerCard || device.outputs < 2) {
            continue;
        }
        // Prefer the interface with more outputs: it can carry headphones too.
        if (!best || device.outputs > best->outputs) {
            best = &device;
        }
    }
    return best;
}
} // namespace

AudioDevice::Kind classifyAudioDevice(const QString& name, const QString& alsaHwDevice, bool usb) {
    const QString lower = name.toLower();
    if (lower.contains(QStringLiteral("bluez")) || lower.contains(QStringLiteral("bluetooth"))) {
        return AudioDevice::Kind::Network;
    }
    if (lower.contains(QStringLiteral("bcm2835")) && !usb) {
        return AudioDevice::Kind::BuiltinJack;
    }
    if (lower.contains(QStringLiteral("hdmi"))) {
        return AudioDevice::Kind::Hdmi;
    }
    if (alsaHwDevice.isEmpty() && !usb) {
        // "pipewire", "pulse", "default", "sysdefault", "dmix", JACK ...
        return AudioDevice::Kind::Network;
    }
    return AudioDevice::Kind::External;
}

QString prettyAudioName(const QString& deviceName) {
    const QString lower = deviceName.toLower();
    if (lower.contains(QStringLiteral("bcm2835"))) {
        return tr("Deck Lab headphone jack");
    }
    static const QRegularExpression hdmi(QStringLiteral("hdmi[^0-9]*(\\d)"));
    const auto hdmiMatch = hdmi.match(lower);
    if (hdmiMatch.hasMatch()) {
        return tr("HDMI %1").arg(hdmiMatch.captured(1).toInt() + 1);
    }
    if (lower == QStringLiteral("pipewire") || lower == QStringLiteral("pulse")) {
        return tr("System audio (Bluetooth)");
    }
    if (lower == QStringLiteral("default") || lower == QStringLiteral("sysdefault")) {
        return tr("System default");
    }
    QString name = deviceName.section(QLatin1Char(':'), 0, 0).trimmed();
    return name.isEmpty() ? deviceName : name;
}

bool isHighLatency(const AudioDevice& device) {
    return device.kind == AudioDevice::Kind::Network;
}

AudioPlan planAudio(const QList<AudioDevice>& devices, const QString& headphonesChoice) {
    AudioPlan plan;
    const AudioDevice* controller = nullptr;
    for (const auto& device : devices) {
        if (device.controllerCard && device.outputs >= 2 &&
                (!controller || device.outputs > controller->outputs)) {
            controller = &device;
        }
    }
    const auto chosenHeadphones = [&](const QString& mainDevice) {
        const AudioDevice* chosen = findDevice(devices, headphonesChoice);
        if (chosen && chosen->id != mainDevice && chosen->outputs >= 2) {
            return AudioRoute{chosen->id, 0};
        }
        return AudioRoute{};
    };
    if (controller) {
        plan.main = AudioRoute{controller->id, 0};
        if (controller->outputs >= 4) {
            plan.headphones = AudioRoute{controller->id, 2};
            plan.reason = tr("Your controller has its own sound card with 4 outputs: "
                             "Main on outputs 1–2, headphones on 3–4.");
        } else {
            plan.headphones = chosenHeadphones(controller->id);
            plan.reason = plan.headphones.isValid()
                    ? tr("Main plays through your controller. Headphones use the device you chose.")
                    : tr("Your controller has 2 outputs, used for Main. "
                         "Choose another output for headphone cueing, or play without.");
        }
        return plan;
    }
    const AudioDevice* main = firstOfKind(devices, AudioDevice::Kind::External);
    if (main) {
        plan.main = AudioRoute{main->id, 0};
        if (main->outputs >= 4) {
            plan.headphones = AudioRoute{main->id, 2};
            plan.reason = tr("Main on your audio interface outputs 1–2, headphones on 3–4.");
            return plan;
        }
        plan.reason = tr("Main plays through your USB audio device.");
    } else if ((main = firstOfKind(devices, AudioDevice::Kind::BuiltinJack))) {
        plan.main = AudioRoute{main->id, 0};
        plan.reason = tr("No audio interface found. Main plays through the Deck Lab headphone jack.");
    } else if ((main = firstOfKind(devices, AudioDevice::Kind::Hdmi))) {
        plan.main = AudioRoute{main->id, 0};
        plan.reason = tr("No audio interface found. Main plays through HDMI.");
    } else {
        plan.reason = tr("No audio output found. Connect a controller with audio or a USB sound card.");
        return plan;
    }
    plan.headphones = chosenHeadphones(main->id);
    return plan;
}

QString channelPairLabel(int channelBase) {
    if (channelBase < 0) {
        return tr("Off");
    }
    return QStringLiteral("%1–%2").arg(channelBase + 1).arg(channelBase + 2);
}

double testToneAmplitude() {
    return std::pow(10.0, kTestToneDbfs / 20.0);
}

double testToneAmplitude(double dbfs) {
    return std::pow(10.0, std::min(dbfs, kTestToneBoostDbfs) / 20.0);
}

bool isPathOnMount(const QString& path, const QString& mountPath) {
    if (path.isEmpty() || mountPath.isEmpty()) {
        return false;
    }
    QString mount = QDir::cleanPath(mountPath);
    const QString file = QDir::cleanPath(path);
    if (file == mount) {
        return true;
    }
    if (!mount.endsWith(QLatin1Char('/'))) {
        mount.append(QLatin1Char('/'));
    }
    return file.startsWith(mount);
}

QStringList ejectBlockers(const QString& mountPath, const QList<LoadedTrack>& loaded) {
    QStringList blockers;
    for (const auto& track : loaded) {
        if (isPathOnMount(track.location, mountPath) && !blockers.contains(track.group)) {
            blockers.append(track.group);
        }
    }
    return blockers;
}

QString playerLabel(const QString& group) {
    static const QRegularExpression pattern(QStringLiteral("^\\[(Channel|Sampler|PreviewDeck)(\\d+)\\]$"));
    const auto match = pattern.match(group);
    if (!match.hasMatch()) {
        return group;
    }
    if (match.captured(1) == QStringLiteral("Channel")) {
        return tr("Deck %1").arg(match.captured(2));
    }
    if (match.captured(1) == QStringLiteral("Sampler")) {
        return tr("Sampler %1").arg(match.captured(2));
    }
    return tr("Preview");
}

QString ejectRefusal(const QString& driveName, const QStringList& blockers) {
    if (blockers.isEmpty()) {
        return QString();
    }
    QStringList labels;
    for (const auto& group : blockers) {
        labels.append(playerLabel(group));
    }
    return tr("Can't eject %1: %2 %3 a track from it. Load other tracks or eject those first, "
              "so the music can't stop.")
            .arg(driveName, labels.join(QStringLiteral(", ")),
                    labels.size() == 1 ? tr("is playing from") : tr("are playing from"));
}

QJsonObject DeviceProfile::toJson() const {
    return QJsonObject{{QStringLiteral("name"), name},
            {QStringLiteral("controller"), controller},
            {QStringLiteral("mapping"), mapping},
            {QStringLiteral("decks"), decks},
            {QStringLiteral("main"), mainDevice},
            {QStringLiteral("mainBase"), mainBase},
            {QStringLiteral("phones"), headphonesDevice},
            {QStringLiteral("phonesBase"), headphonesBase}};
}

DeviceProfile DeviceProfile::fromJson(const QString& key, const QJsonObject& object) {
    DeviceProfile profile;
    profile.key = key;
    profile.name = object.value(QStringLiteral("name")).toString();
    profile.controller = object.value(QStringLiteral("controller")).toString();
    profile.mapping = object.value(QStringLiteral("mapping")).toString();
    profile.decks = object.value(QStringLiteral("decks")).toInt(2) == 4 ? 4 : 2;
    profile.mainDevice = object.value(QStringLiteral("main")).toString();
    profile.mainBase = object.value(QStringLiteral("mainBase")).toInt(-1);
    profile.headphonesDevice = object.value(QStringLiteral("phones")).toString();
    profile.headphonesBase = object.value(QStringLiteral("phonesBase")).toInt(-1);
    return profile;
}

bool DeviceProfile::operator==(const DeviceProfile& other) const {
    return key == other.key && name == other.name && controller == other.controller &&
            mapping == other.mapping && decks == other.decks && mainDevice == other.mainDevice &&
            mainBase == other.mainBase && headphonesDevice == other.headphonesDevice &&
            headphonesBase == other.headphonesBase;
}

const QString DeviceStore::kGroup = QStringLiteral("[DeckLabDevices]");

DeviceStore::DeviceStore(UserSettingsPointer config)
        : m_config(std::move(config)) {
}

QString DeviceStore::configItem(const QString& key) {
    QString item = key.trimmed();
    static const QRegularExpression unsafe(QStringLiteral("[\\s\\[\\]]+"));
    item.replace(unsafe, QStringLiteral("_"));
    return item;
}

QString DeviceStore::deviceKey(std::optional<uint16_t> vendorId,
        std::optional<uint16_t> productId,
        const QString& serial,
        const QString& fallbackName) {
    if (vendorId && productId) {
        QString key = QStringLiteral("%1:%2")
                              .arg(static_cast<uint>(*vendorId), 4, 16, QLatin1Char('0'))
                              .arg(static_cast<uint>(*productId), 4, 16, QLatin1Char('0'));
        if (!serial.trimmed().isEmpty()) {
            key += QLatin1Char(':') + serial.trimmed();
        }
        return key;
    }
    return fallbackName.trimmed();
}

std::optional<DeviceProfile> DeviceStore::find(const QString& key) const {
    if (!m_config || key.trimmed().isEmpty()) {
        return std::nullopt;
    }
    const ConfigKey configKey(kGroup, configItem(key));
    if (!m_config->exists(configKey)) {
        return std::nullopt;
    }
    const auto document = QJsonDocument::fromJson(m_config->getValueString(configKey).toUtf8());
    if (!document.isObject()) {
        return std::nullopt;
    }
    return DeviceProfile::fromJson(key, document.object());
}

std::optional<DeviceProfile> DeviceStore::findByController(const QString& controllerName) const {
    if (controllerName.isEmpty()) {
        return std::nullopt;
    }
    if (auto direct = find(controllerName)) {
        return direct;
    }
    const auto profiles = all();
    for (const auto& profile : profiles) {
        if (profile.controller == controllerName) {
            return profile;
        }
    }
    return std::nullopt;
}

void DeviceStore::save(const DeviceProfile& profile) {
    if (!m_config || profile.key.trimmed().isEmpty()) {
        return;
    }
    m_config->setValue(ConfigKey(kGroup, configItem(profile.key)),
            QString::fromUtf8(QJsonDocument(profile.toJson()).toJson(QJsonDocument::Compact)));
}

bool DeviceStore::remove(const QString& key) {
    return m_config && m_config->remove(ConfigKey(kGroup, configItem(key)));
}

QList<DeviceProfile> DeviceStore::all() const {
    QList<DeviceProfile> profiles;
    if (!m_config) {
        return profiles;
    }
    const auto keys = m_config->getKeysWithGroup(kGroup);
    for (const auto& key : keys) {
        const auto document = QJsonDocument::fromJson(m_config->getValueString(key).toUtf8());
        if (document.isObject()) {
            profiles.append(DeviceProfile::fromJson(key.item, document.object()));
        }
    }
    return profiles;
}

} // namespace decklab
