// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabusbname.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace decklab {

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("DeckLabUsb", text);
}

// udev escapes characters such as spaces in /dev/disk link names ("\x20").
QString udevUnescape(const QString& name) {
    const QByteArray bytes = name.toUtf8();
    QByteArray out;
    for (int i = 0; i < bytes.size(); ++i) {
        bool ok = false;
        if (bytes[i] == '\\' && i + 3 < bytes.size() && bytes[i + 1] == 'x') {
            const int value = bytes.mid(i + 2, 2).toInt(&ok, 16);
            if (ok) {
                out.append(static_cast<char>(value));
                i += 3;
                continue;
            }
        }
        out.append(bytes[i]);
    }
    return QString::fromUtf8(out);
}

} // namespace

bool usbLabelIsPlaceholder(const QString& label, const QString& id) {
    const QString text = label.trimmed();
    if (text.isEmpty() || (!id.isEmpty() && text.compare(id.trimmed(), Qt::CaseInsensitive) == 0)) {
        return true;
    }
    // FAT/exFAT serials, NTFS serials and ext/btrfs UUIDs.
    static const QRegularExpression uuid(QStringLiteral(
            "^(?:[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}|[0-9A-Fa-f]{16}|"
            "[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12})$"));
    return uuid.match(text).hasMatch();
}

QString usbBaseName(const UsbNameSource& source) {
    if (!usbLabelIsPlaceholder(source.label, source.id)) {
        return source.label.trimmed();
    }
    return source.rekordbox ? tr("DJ USB") : tr("USB stick");
}

QString usbCapacityText(qint64 bytes) {
    if (bytes <= 0) {
        return QString();
    }
    // Flash is sold in powers of two (decimal GB); the filesystem is a little
    // smaller than the label, a few sticks a little larger: 5 % slack.
    const double gigabytes = static_cast<double>(bytes) / 1e9;
    double sold = 1;
    while (sold * 1.05 < gigabytes && sold < 65536) {
        sold *= 2;
    }
    if (sold >= 1024) {
        return tr("%1 TB").arg(sold / 1024);
    }
    return tr("%1 GB").arg(sold);
}

QStringList UsbNameBook::names(const QList<UsbNameSource>& sources) {
    const QMutexLocker locker(&m_mutex);
    ++m_call;
    const int count = static_cast<int>(sources.size());
    QStringList bases;
    QStringList sizes;
    for (const auto& source : sources) {
        bases.append(usbBaseName(source));
        sizes.append(usbCapacityText(source.bytes));
    }
    QStringList result;
    for (int index = 0; index < count; ++index) {
        result.append(QString());
    }
    QSet<QString> used; // case-folded names given out in this call
    // 1. A stick that already has a name keeps it, unless another stick here
    // holds it: the one in most recently (it is the one on screen) wins, then
    // the one named first.
    QList<int> known;
    QList<int> pending;
    for (int index = 0; index < count; ++index) {
        const auto entry = m_entries.constFind(sources.at(index).id);
        if (!sources.at(index).id.isEmpty() && entry != m_entries.constEnd() &&
                entry->base == bases.at(index)) {
            known.append(index);
        } else {
            pending.append(index);
        }
    }
    std::sort(known.begin(), known.end(), [this, &sources](int a, int b) {
        const Entry left = m_entries.value(sources.at(a).id);
        const Entry right = m_entries.value(sources.at(b).id);
        return left.seen != right.seen ? left.seen > right.seen : left.order < right.order;
    });
    for (int index : std::as_const(known)) {
        const QString name = m_entries.value(sources.at(index).id).name;
        if (used.contains(name.toCaseFolded())) {
            pending.append(index);
            continue;
        }
        result[index] = name;
        used.insert(name.toCaseFolded());
    }
    // 2. New sticks, in a stable order (by id): the plain name when it is
    // free, else the size when it tells them apart, else 2, 3, ...
    std::sort(pending.begin(), pending.end(), [&sources](int a, int b) {
        return sources.at(a).id < sources.at(b).id;
    });
    for (int index : std::as_const(pending)) {
        const QString& base = bases.at(index);
        QString name = base;
        if (used.contains(name.toCaseFolded())) {
            bool sameSize = sizes.at(index).isEmpty();
            for (int other = 0; other < count && !sameSize; ++other) {
                sameSize = other != index && bases.at(other).toCaseFolded() == base.toCaseFolded() &&
                        sizes.at(other) == sizes.at(index);
            }
            name = sameSize ? QString() : QString(base + QLatin1Char(' ') + sizes.at(index));
            for (int number = 2; name.isEmpty() || used.contains(name.toCaseFolded()); ++number) {
                name = base + QLatin1Char(' ') + QString::number(number);
            }
        }
        result[index] = name;
        used.insert(name.toCaseFolded());
        if (!sources.at(index).id.isEmpty()) {
            m_entries.insert(sources.at(index).id, Entry{base, name, m_next++, 0});
        }
    }
    for (const auto& source : sources) {
        const auto entry = m_entries.find(source.id);
        if (entry != m_entries.end()) {
            entry->seen = m_call;
        }
    }
    // Remember a bounded number of sticks (the most recent).
    while (m_entries.size() > 64) {
        auto oldest = m_entries.begin();
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (it->order < oldest->order) {
                oldest = it;
            }
        }
        m_entries.erase(oldest);
    }
    return result;
}

void UsbNameBook::clear() {
    const QMutexLocker locker(&m_mutex);
    m_entries.clear();
    m_next = 0;
    m_call = 0;
}

UsbNameBook& usbNameBook() {
    static UsbNameBook book;
    return book;
}

QStringList usbNames(const QList<UsbNameSource>& sources) {
    return usbNameBook().names(sources);
}

namespace {
QStringList& mediaTestRoots() {
    static QStringList roots;
    return roots;
}
QMutex& mediaRootsMutex() {
    static QMutex mutex;
    return mutex;
}
} // namespace

QStringList usbMediaRoots() {
    const QMutexLocker locker(&mediaRootsMutex());
    return mediaTestRoots().isEmpty() ? QStringList{QStringLiteral("/media/decklab")} : mediaTestRoots();
}

bool usbMediaRootsAreForTesting() {
    const QMutexLocker locker(&mediaRootsMutex());
    return !mediaTestRoots().isEmpty();
}

void setUsbMediaRootsForTesting(const QStringList& roots) {
    const QMutexLocker locker(&mediaRootsMutex());
    mediaTestRoots() = roots;
}

QList<UsbNameSource> usbNameSources(const QJsonArray& drives) {
    QList<UsbNameSource> sources;
    for (const auto& value : drives) {
        const QJsonObject drive = value.toObject();
        UsbNameSource source;
        source.id = drive.value(QStringLiteral("id")).toString();
        source.label = drive.value(QStringLiteral("label")).toString();
        source.rekordbox = drive.value(QStringLiteral("rekordbox")).toBool();
        const double megabytes = drive.value(QStringLiteral("size_mb")).toDouble(-1);
        source.bytes = megabytes > 0 ? static_cast<qint64>(megabytes * 1024 * 1024) : -1;
        sources.append(source);
    }
    return sources;
}

QString usbDriveName(const QJsonObject& drive, const QJsonArray& all) {
    const QString id = drive.value(QStringLiteral("id")).toString();
    const QStringList names = usbNames(usbNameSources(all));
    for (int index = 0; index < all.size(); ++index) {
        if (all.at(index).toObject().value(QStringLiteral("id")).toString() == id && !id.isEmpty()) {
            return names.at(index);
        }
    }
    return usbBaseName(usbNameSources(QJsonArray{drive}).value(0));
}

QString usbLabelForUuid(const QString& uuid) {
#if defined(Q_OS_LINUX)
    if (uuid.isEmpty()) {
        return QString();
    }
    const QString device = QFileInfo(QDir(QStringLiteral("/dev/disk/by-uuid")), uuid).canonicalFilePath();
    if (device.isEmpty()) {
        return QString();
    }
    const QDir labels(QStringLiteral("/dev/disk/by-label"));
    const QStringList links = labels.entryList(QDir::AllEntries | QDir::System | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QString& link : links) {
        if (QFileInfo(labels.filePath(link)).canonicalFilePath() == device) {
            return udevUnescape(link).trimmed();
        }
    }
#else
    Q_UNUSED(uuid);
#endif
    return QString();
}

} // namespace decklab
