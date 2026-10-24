// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <functional>
#include <optional>

// r5-polish: one Deck Setup row per physical controller. Mixxx enumerates a
// USB DJ controller once per interface (a MIDI port, one or more HID
// interfaces), and decklabd reports the USB device. This groups them, picks
// the interface Deck Lab uses and says which interfaces must stay closed.
// Pure data in, data out: no Controller objects, so it is unit-testable.
namespace decklab {

/// One Mixxx controller: a MIDI port or a HID interface.
struct ControllerEndpoint {
    QString name; // Mixxx controller name ("MixTrack Pro 3 MIDI 1", "MixTrack Pro 3 _3")
    bool hid = false;
    bool open = false;
    bool enabled = false; // [Controller] <name> = 1: Mixxx opens it at start and after a rescan
    QString mapping;      // configured mapping file, may be empty
    std::optional<uint16_t> vendorId;
    std::optional<uint16_t> productId;
    QString serial;
    std::optional<int> interfaceNumber;
    std::optional<uint16_t> usagePage; // HID only
    QString product;                   // HID product string ("MixTrack Pro 3")
    /// USB location ("usb-0000:01:00.0-1.3", the ALSA card's and hidraw's
    /// HID_PHYS form), empty when unknown (not Linux, or ambiguous).
    QString usbPath;
};

/// A controller decklabd reports: one physical USB device.
struct ReportedController {
    QString key; // vid:pid[:serial|:port]
    QString name;
    std::optional<uint16_t> vendorId;
    std::optional<uint16_t> productId;
    QString serial;
    QStringList midiPorts;
    bool hid = false;
    QJsonObject audioCard;
    QString usbPort; // sysfs USB device name ("1-1.3"), may be empty
};

struct ControllerGroup {
    QList<ControllerEndpoint> endpoints; // every interface of one device
    int primary = -1;                    // the interface the row stands for, or -1
    int reported = -1;                   // index into the reported list, or -1
    /// Interfaces Deck Lab never opens: a HID interface of a device whose
    /// MIDI side is the one in use (vendor-defined, or without a fitting
    /// .hid.xml mapping).
    QList<int> suppressed;
    const ControllerEndpoint* primaryEndpoint() const {
        return primary >= 0 && primary < endpoints.size() ? &endpoints.at(primary) : nullptr;
    }
};

/// Vendor-defined HID usage pages (0xFF00-0xFFFF): firmware, LEDs, screens.
inline bool isVendorDefinedUsagePage(uint16_t page) {
    return page >= 0xFF00;
}
/// True for "*.hid.xml" (case-insensitive).
bool isHidMapping(const QString& path);
/// The mapping can drive this interface: .hid.xml for HID, anything else
/// (.midi.xml) for MIDI. An empty mapping fits nothing.
bool mappingFits(const ControllerEndpoint& endpoint, const QString& mapping);
/// Open with a mapping made for its kind.
bool isWorking(const ControllerEndpoint& endpoint);

/// "usb-0000:01:00.0-1.3", "usb-0000:01:00.0-1.3/input3" or "1-1.3" -> "1.3":
/// the port chain both forms share. Empty when there is none.
QString usbPortChain(const QString& location);

/// Groups the interfaces into physical controllers and matches decklabd's
/// devices. `hidMappingMatches` says whether a bundled or user .hid.xml
/// mapping is made for a HID interface (asked only when it decides).
/// Groups follow the reported order, then Mixxx's.
QList<ControllerGroup> groupControllers(const QList<ControllerEndpoint>& endpoints,
        const QList<ReportedController>& reported,
        const std::function<bool(const ControllerEndpoint&)>& hidMappingMatches = {});

/// decklabd's devices.list "controllers" entries.
QList<ReportedController> reportedControllers(const QJsonObject& devices);

namespace usbpath {
/// HID_PHYS of a hidraw node without "/inputN": "/dev/hidraw0" ->
/// "usb-0000:01:00.0-1.3". `root` is "/" except in tests.
QString hidrawLocation(const QString& devicePath, const QString& root = QStringLiteral("/"));
/// ALSA sequencer port name -> its USB card's location, from
/// /proc/asound/seq/clients and /proc/asound/cards. Names two cards share
/// are left out (they can't be told apart).
QHash<QString, QString> midiPortLocations(const QString& root = QStringLiteral("/"));
} // namespace usbpath

} // namespace decklab
