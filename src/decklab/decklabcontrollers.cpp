// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabcontrollers.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>
#include <numeric>

#include "controllers/controllermappingmatcher.h"

namespace decklab {

namespace {

QString compact(const QString& text) {
    QString result;
    for (QChar c : text.toLower()) {
        if (c.isLetterOrNumber()) {
            result.append(c);
        }
    }
    return result;
}

// "MixTrack Pro 3 MIDI 1" and the HID product "MixTrack Pro 3" name the
// same model.
QString modelKey(const ControllerEndpoint& endpoint) {
    if (endpoint.hid && !endpoint.product.trimmed().isEmpty()) {
        return compact(endpoint.product);
    }
    return compact(ControllerMappingMatcher::modelName(endpoint.name));
}

bool isMidiThrough(const ControllerEndpoint& endpoint) {
    return !endpoint.hid && endpoint.name.startsWith(QStringLiteral("MIDI Through"), Qt::CaseInsensitive);
}

std::optional<uint16_t> usbId(const QJsonValue& value) {
    bool ok = false;
    int id = -1;
    if (value.isDouble()) {
        id = value.toInt(-1);
        ok = id >= 0;
    } else if (value.isString()) {
        QString text = value.toString().trimmed();
        if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
            text = text.mid(2);
        }
        id = text.toInt(&ok, 16);
    }
    if (!ok || id < 0 || id > 0xffff) {
        return std::nullopt;
    }
    return static_cast<uint16_t>(id);
}

QString readText(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    // sysfs and procfs files report size 0: read what is there.
    return QString::fromUtf8(file.read(256 * 1024));
}

class UnionFind {
  public:
    explicit UnionFind(int count)
            : m_parent(count) {
        std::iota(m_parent.begin(), m_parent.end(), 0);
    }
    int find(int item) {
        while (m_parent[item] != item) {
            m_parent[item] = m_parent[m_parent[item]];
            item = m_parent[item];
        }
        return item;
    }
    void join(int a, int b) {
        a = find(a);
        b = find(b);
        if (a != b) {
            // The earlier interface stays the root: groups keep Mixxx's order.
            m_parent[std::max(a, b)] = std::min(a, b);
        }
    }

  private:
    std::vector<int> m_parent;
};

} // namespace

bool isHidMapping(const QString& path) {
    return path.endsWith(QStringLiteral(".hid.xml"), Qt::CaseInsensitive);
}

bool mappingFits(const ControllerEndpoint& endpoint, const QString& mapping) {
    if (mapping.trimmed().isEmpty()) {
        return false;
    }
    return endpoint.hid == isHidMapping(mapping);
}

bool isWorking(const ControllerEndpoint& endpoint) {
    return endpoint.open && mappingFits(endpoint, endpoint.mapping);
}

QString usbPortChain(const QString& location) {
    QString text = location.trimmed();
    const int slash = text.indexOf(QLatin1Char('/'));
    if (slash >= 0) {
        text = text.left(slash);
    }
    if (text.startsWith(QStringLiteral("usb-"), Qt::CaseInsensitive)) {
        // usb-<host controller>-<port chain>; the host part has no '-'
        // after its own prefix on Linux ("0000:01:00.0", "3f980000.usb").
        const int dash = text.lastIndexOf(QLatin1Char('-'));
        return dash > 3 ? text.mid(dash + 1) : QString();
    }
    // sysfs USB device name: <bus>-<port chain>
    static const QRegularExpression sysfs(QStringLiteral("^\\d+-([0-9.]+)$"));
    const auto match = sysfs.match(text);
    return match.hasMatch() ? match.captured(1) : QString();
}

QList<ReportedController> reportedControllers(const QJsonObject& devices) {
    QList<ReportedController> result;
    const QJsonArray controllers = devices.value(QStringLiteral("controllers")).toArray();
    for (const auto& value : controllers) {
        const QJsonObject device = value.toObject();
        ReportedController reported;
        reported.key = device.value(QStringLiteral("key")).toString();
        reported.name = device.value(QStringLiteral("name")).toString();
        reported.vendorId = usbId(device.value(QStringLiteral("vid")));
        reported.productId = usbId(device.value(QStringLiteral("pid")));
        reported.serial = device.value(QStringLiteral("serial")).toString();
        reported.hid = device.value(QStringLiteral("hid")).toBool();
        reported.audioCard = device.value(QStringLiteral("audio_card")).toObject();
        reported.usbPort = device.value(QStringLiteral("usb_port")).toString();
        for (const auto& port : device.value(QStringLiteral("midi_ports")).toArray()) {
            reported.midiPorts.append(port.toString());
        }
        result.append(reported);
    }
    return result;
}

QList<ControllerGroup> groupControllers(const QList<ControllerEndpoint>& endpoints,
        const QList<ReportedController>& reported,
        const std::function<bool(const ControllerEndpoint&)>& hidMappingMatches) {
    QList<ControllerEndpoint> items;
    for (const auto& endpoint : endpoints) {
        if (!isMidiThrough(endpoint)) {
            items.append(endpoint);
        }
    }
    const int count = static_cast<int>(items.size());
    UnionFind sets(count);
    // 1. Same USB location: interfaces of one device.
    for (int a = 0; a < count; ++a) {
        for (int b = a + 1; b < count; ++b) {
            const auto& left = items.at(a);
            const auto& right = items.at(b);
            if (!left.usbPath.isEmpty() && left.usbPath == right.usbPath) {
                sets.join(a, b);
            }
        }
    }
    // 2. No location (not Linux): HID interfaces with the same USB ids and
    // serial are one device.
    for (int a = 0; a < count; ++a) {
        for (int b = a + 1; b < count; ++b) {
            const auto& left = items.at(a);
            const auto& right = items.at(b);
            if (left.hid && right.hid && left.usbPath.isEmpty() && right.usbPath.isEmpty() &&
                    left.vendorId && left.productId && left.vendorId == right.vendorId &&
                    left.productId == right.productId && left.serial == right.serial) {
                sets.join(a, b);
            }
        }
    }
    const auto locationOf = [&](int root) {
        for (int index = 0; index < count; ++index) {
            if (sets.find(index) == root && !items.at(index).usbPath.isEmpty()) {
                return items.at(index).usbPath;
            }
        }
        return QString();
    };
    const auto hasKind = [&](int root, bool hid) {
        for (int index = 0; index < count; ++index) {
            if (sets.find(index) == root && items.at(index).hid == hid) {
                return true;
            }
        }
        return false;
    };
    // 3. Name fallback: a HID-only set joins the MIDI set of the same model
    // when their locations can't tell them apart. Each MIDI set takes one.
    for (int index = 0; index < count; ++index) {
        const auto& hid = items.at(index);
        const int hidRoot = sets.find(index);
        if (!hid.hid || hidRoot != index || hasKind(hidRoot, false)) {
            continue;
        }
        for (int other = 0; other < count; ++other) {
            const auto& midi = items.at(other);
            const int midiRoot = sets.find(other);
            if (midi.hid || midiRoot != other || hasKind(midiRoot, true)) {
                continue;
            }
            const QString hidLocation = locationOf(hidRoot);
            const QString midiLocation = locationOf(midiRoot);
            if (!hidLocation.isEmpty() && !midiLocation.isEmpty()) {
                continue; // both known and different (equal ones joined in 1.)
            }
            const bool sameIds = midi.vendorId && midi.productId && midi.vendorId == hid.vendorId &&
                    midi.productId == hid.productId;
            const QString model = modelKey(midi);
            if (sameIds || (!model.isEmpty() && model == modelKey(hid))) {
                sets.join(midiRoot, hidRoot);
                break;
            }
        }
    }
    // Sets in Mixxx's order.
    QList<int> roots;
    for (int index = 0; index < count; ++index) {
        const int root = sets.find(index);
        if (!roots.contains(root)) {
            roots.append(root);
        }
    }
    // 4. decklabd's devices claim sets: same USB port chain, then the MIDI
    // port name, then the USB ids.
    QList<int> claimedBy(roots.size(), -1);
    for (int device = 0; device < reported.size(); ++device) {
        const auto& report = reported.at(device);
        const QString reportChain = usbPortChain(report.usbPort);
        int best = -1;
        int bestScore = 0;
        for (int set = 0; set < roots.size(); ++set) {
            if (claimedBy.at(set) >= 0) {
                continue;
            }
            int score = 0;
            bool rejected = false;
            for (int index = 0; index < count; ++index) {
                if (sets.find(index) != roots.at(set)) {
                    continue;
                }
                const auto& endpoint = items.at(index);
                const QString chain = usbPortChain(endpoint.usbPath);
                if (!chain.isEmpty() && !reportChain.isEmpty()) {
                    if (chain != reportChain) {
                        rejected = true;
                    } else {
                        score = std::max(score, 8);
                    }
                }
                if (!endpoint.hid) {
                    for (const QString& port : report.midiPorts) {
                        if (port == endpoint.name) {
                            score = std::max(score, 4);
                        } else if (compact(ControllerMappingMatcher::modelName(port)) ==
                                compact(ControllerMappingMatcher::modelName(endpoint.name))) {
                            score = std::max(score, 2);
                        }
                    }
                }
                if (endpoint.vendorId && endpoint.productId && report.vendorId && report.productId &&
                        *endpoint.vendorId == *report.vendorId && *endpoint.productId == *report.productId) {
                    if (!endpoint.serial.isEmpty() && !report.serial.isEmpty() &&
                            endpoint.serial != report.serial) {
                        rejected = true;
                    } else {
                        score = std::max(score, 1);
                    }
                }
            }
            if (!rejected && score > bestScore) {
                best = set;
                bestScore = score;
            }
        }
        if (best >= 0) {
            claimedBy[best] = device;
        }
    }
    const auto groupFor = [&](int set) {
        ControllerGroup group;
        for (int index = 0; index < count; ++index) {
            if (sets.find(index) == roots.at(set)) {
                group.endpoints.append(items.at(index));
            }
        }
        group.reported = claimedBy.at(set);
        // The interface in use: working, then configured; a HID interface
        // with a mapping made for it beats an unconfigured MIDI port (it is
        // how Mixxx supports that device); then MIDI; then HID alone.
        bool hasMidi = false;
        for (const auto& endpoint : std::as_const(group.endpoints)) {
            hasMidi |= !endpoint.hid;
        }
        int bestRank = 100;
        for (int index = 0; index < group.endpoints.size(); ++index) {
            const auto& endpoint = group.endpoints.at(index);
            int rank;
            if (isWorking(endpoint)) {
                rank = endpoint.hid ? 1 : 0;
            } else if (mappingFits(endpoint, endpoint.mapping)) {
                rank = endpoint.hid ? 3 : 2;
            } else if (!endpoint.hid) {
                rank = 5;
            } else if (!hasMidi) {
                rank = 6;
            } else {
                rank = 7;
            }
            if (rank < bestRank) {
                bestRank = rank;
                group.primary = index;
            }
        }
        if (bestRank == 5 && hidMappingMatches) {
            // Asked only here: an unconfigured MIDI port and a HID interface.
            for (int index = 0; index < group.endpoints.size(); ++index) {
                const auto& endpoint = group.endpoints.at(index);
                if (endpoint.hid && hidMappingMatches(endpoint)) {
                    group.primary = index;
                    break;
                }
            }
        }
        const auto* primary = group.primaryEndpoint();
        if (primary && !primary->hid) {
            for (int index = 0; index < group.endpoints.size(); ++index) {
                const auto& endpoint = group.endpoints.at(index);
                if (index != group.primary && endpoint.hid && !mappingFits(endpoint, endpoint.mapping)) {
                    group.suppressed.append(index);
                }
            }
        }
        return group;
    };
    QList<ControllerGroup> result;
    for (int device = 0; device < reported.size(); ++device) {
        const int set = static_cast<int>(claimedBy.indexOf(device));
        if (set >= 0) {
            result.append(groupFor(set));
        } else {
            ControllerGroup group;
            group.reported = device;
            result.append(group);
        }
    }
    for (int set = 0; set < roots.size(); ++set) {
        if (claimedBy.at(set) < 0) {
            result.append(groupFor(set));
        }
    }
    return result;
}

namespace usbpath {

QString hidrawLocation(const QString& devicePath, const QString& root) {
    const QString node = devicePath.section(QLatin1Char('/'), -1);
    if (!node.startsWith(QStringLiteral("hidraw"))) {
        return QString();
    }
    const QString uevent = readText(QDir(root).filePath(
            QStringLiteral("sys/class/hidraw/") + node + QStringLiteral("/device/uevent")));
    for (const QString& line : uevent.split(QLatin1Char('\n'))) {
        if (line.startsWith(QStringLiteral("HID_PHYS="))) {
            const QString phys = line.mid(9).trimmed();
            if (!phys.startsWith(QStringLiteral("usb-"))) {
                return QString();
            }
            return phys.section(QLatin1Char('/'), 0, 0);
        }
    }
    return QString();
}

QHash<QString, QString> midiPortLocations(const QString& root) {
    QHash<QString, QString> result;
    const QDir proc(QDir(root).filePath(QStringLiteral("proc/asound")));
    // Card index -> "usb-0000:01:00.0-1.3" from the long name
    // ("Numark MixTrack Pro 3 at usb-0000:01:00.0-1.3, full speed").
    QHash<int, QString> cardLocation;
    static const QRegularExpression cardLine(QStringLiteral("^\\s*(\\d+)\\s+\\["));
    static const QRegularExpression usbAt(QStringLiteral("\\bat\\s+(usb-[^\\s,]+)"));
    const QStringList cards = readText(proc.filePath(QStringLiteral("cards"))).split(QLatin1Char('\n'));
    for (int line = 0; line < cards.size(); ++line) {
        const auto card = cardLine.match(cards.at(line));
        if (!card.hasMatch() || line + 1 >= cards.size()) {
            continue;
        }
        const auto location = usbAt.match(cards.at(line + 1));
        if (location.hasMatch()) {
            cardLocation.insert(card.captured(1).toInt(), location.captured(1));
        }
    }
    if (cardLocation.isEmpty()) {
        return result;
    }
    // Kernel sequencer clients are 16 + 4 * card (+ device).
    static const QRegularExpression client(QStringLiteral("^Client\\s+(\\d+)\\s*:\\s*\"(.*)\"\\s*\\[([^\\]]*)\\]"));
    static const QRegularExpression port(QStringLiteral("^\\s+Port\\s+(\\d+)\\s*:\\s*\"(.*)\""));
    QSet<QString> ambiguous;
    QString location;
    for (const QString& line : readText(proc.filePath(QStringLiteral("seq/clients"))).split(QLatin1Char('\n'))) {
        const auto clientMatch = client.match(line);
        if (clientMatch.hasMatch()) {
            const int number = clientMatch.captured(1).toInt();
            const bool kernel = clientMatch.captured(3).trimmed().startsWith(QStringLiteral("Kernel"));
            location = kernel && number >= 16 && number < 128 ? cardLocation.value((number - 16) / 4) : QString();
            continue;
        }
        const auto portMatch = port.match(line);
        if (!portMatch.hasMatch() || location.isEmpty()) {
            continue;
        }
        const QString name = portMatch.captured(2);
        if (result.contains(name) && result.value(name) != location) {
            ambiguous.insert(name);
        }
        result.insert(name, location);
    }
    for (const QString& name : std::as_const(ambiguous)) {
        result.remove(name);
    }
    return result;
}

} // namespace usbpath

} // namespace decklab
