// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// r5-polish: one friendly name for a USB stick everywhere (Browse, its
// breadcrumb and source tile, the rail's status sheet, Settings › Unit ›
// Storage, eject messages). Never the filesystem UUID the appliance mounts
// it under: the volume label, else "DJ USB" for a rekordbox export and
// "USB stick" for anything else. Two of the same name get their sizes
// ("DJ USB 32 GB"), or numbers when the sizes match ("DJ USB 1").
namespace decklab {

struct UsbNameSource {
    QString id;             // filesystem UUID (the mount folder), or the mount path
    QString label;          // filesystem label; empty (or the id) when there is none
    bool rekordbox = false; // holds a rekordbox USB export
    qint64 bytes = -1;      // capacity, -1 when unknown
};

/// No name: empty, the id itself (decklabd's fallback) or a filesystem
/// UUID or serial ("6A3F-1C2B", "0123456789ABCDEF", 8-4-4-4-12 hex).
bool usbLabelIsPlaceholder(const QString& label, const QString& id = QString());
/// The label, or "DJ USB" / "USB stick".
QString usbBaseName(const UsbNameSource& source);
/// The size it was sold as: "32 GB", "1 TB". Empty when unknown.
QString usbCapacityText(qint64 bytes);
/// Friendly, distinct names, in the same order as `sources`.
QStringList usbNames(const QList<UsbNameSource>& sources);

/// decklabd's usb.list drives ({id, label, rekordbox, size_mb}).
QList<UsbNameSource> usbNameSources(const QJsonArray& drives);
/// The friendly name of `drive` among `all` (the drives listed with it).
QString usbDriveName(const QJsonObject& drive, const QJsonArray& all);

/// Linux: the label of the filesystem with this UUID, from udev's
/// /dev/disk/by-label links. Empty when it has none or elsewhere.
QString usbLabelForUuid(const QString& uuid);

} // namespace decklab
