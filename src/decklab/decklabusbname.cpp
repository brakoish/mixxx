// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabusbname.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
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

QStringList usbNames(const QList<UsbNameSource>& sources) {
    QStringList names;
    QHash<QString, QList<int>> byName;
    for (int index = 0; index < sources.size(); ++index) {
        names.append(usbBaseName(sources.at(index)));
        byName[names.last().toCaseFolded()].append(index);
    }
    for (auto it = byName.cbegin(); it != byName.cend(); ++it) {
        QList<int> same = it.value();
        if (same.size() < 2) {
            continue;
        }
        QStringList sizes;
        for (int index : std::as_const(same)) {
            sizes.append(usbCapacityText(sources.at(index).bytes));
        }
        bool distinct = !sizes.contains(QString());
        for (int a = 0; a < sizes.size() && distinct; ++a) {
            distinct = sizes.count(sizes.at(a)) == 1;
        }
        if (distinct) {
            for (int n = 0; n < same.size(); ++n) {
                names[same.at(n)] += QLatin1Char(' ') + sizes.at(n);
            }
            continue;
        }
        // Same size too: 1, 2, ... in a stable order (by id), whatever order
        // the caller listed them in.
        std::sort(same.begin(), same.end(), [&sources](int a, int b) {
            return sources.at(a).id < sources.at(b).id;
        });
        for (int n = 0; n < same.size(); ++n) {
            names[same.at(n)] += QLatin1Char(' ') + QString::number(n + 1);
        }
    }
    return names;
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
