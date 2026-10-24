// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QString>
#include <QStringList>

// r5-polish: one friendly name for a USB stick everywhere (Browse, its
// breadcrumb and source tile, the rail's status sheet, Settings › Unit ›
// Storage, eject messages). Never the filesystem UUID the appliance mounts
// it under: the volume label, else "DJ USB" for a rekordbox export and
// "USB stick" for anything else. A name, once given, stays while the stick
// is in: a stick that arrives later with the same name gets the size it was
// sold as ("DJ USB 16 GB"), or a number when the sizes match ("DJ USB 2").
// Names are for display only; nothing is keyed by them.
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

/// Remembers which stick got which name, so a stick already shown keeps its
/// name when another one arrives or leaves (and gets it back when it
/// returns, if it is free). Thread-safe: the rekordbox device scan runs on
/// a worker thread.
class UsbNameBook {
  public:
    /// Friendly, distinct names for the sticks present, in their order.
    QStringList names(const QList<UsbNameSource>& sources);
    void clear();

  private:
    struct Entry {
        QString base;
        QString name;
        quint64 order = 0; // when the name was given
        quint64 seen = 0;  // the last call the stick was in
    };
    QMutex m_mutex;
    QHash<QString, Entry> m_entries; // by id
    quint64 m_next = 0;
    quint64 m_call = 0;
};
/// The application's book (Browse, the rekordbox feature, the rail and
/// Storage share it, so a stick has one name everywhere).
UsbNameBook& usbNameBook();
/// usbNameBook().names(sources).
QStringList usbNames(const QList<UsbNameSource>& sources);

/// decklabd's usb.list drives ({id, label, rekordbox, size_mb}).
QList<UsbNameSource> usbNameSources(const QJsonArray& drives);
/// The friendly name of `drive` among `all` (the drives listed with it).
QString usbDriveName(const QJsonObject& drive, const QJsonArray& all);

/// Linux: the label of the filesystem with this UUID, from udev's
/// /dev/disk/by-label links. Empty when it has none or elsewhere.
QString usbLabelForUuid(const QString& uuid);

/// The folders the appliance mounts sticks in (/media/decklab), or the
/// generated folders tests set (then no label lookup and no mount check).
QStringList usbMediaRoots();
bool usbMediaRootsAreForTesting();
void setUsbMediaRootsForTesting(const QStringList& roots);

} // namespace decklab
