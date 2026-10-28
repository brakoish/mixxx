// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabdownloads.h"

#include <QDir>
#include <QRegularExpression>
#include <algorithm>

#include "decklab/decklabmedia.h" // r7-recent: a stick's top folder

namespace decklab {

namespace {

QString& musicFolderOverride() {
    static QString folder;
    return folder;
}

const QStringList& audioSuffixes() {
    static const QStringList suffixes{QStringLiteral("mp3"), QStringLiteral("wav"), QStringLiteral("flac"),
            QStringLiteral("aif"), QStringLiteral("aiff"), QStringLiteral("m4a"), QStringLiteral("mp4"),
            QStringLiteral("ogg"), QStringLiteral("opus"), QStringLiteral("wv"), QStringLiteral("alac")};
    return suffixes;
}

bool plainFolder(const QString& path) {
    const QFileInfo info(path);
    return info.isDir() && !info.isSymLink();
}

bool skippedFolder(const QString& name) {
    static const QStringList system{QStringLiteral("System Volume Information"), QStringLiteral("$RECYCLE.BIN"),
            QStringLiteral("RECYCLER"), QStringLiteral("LOST.DIR"), QStringLiteral("lost+found")};
    return name.startsWith(QLatin1Char('.')) || system.contains(name, Qt::CaseInsensitive);
}

struct Arrived {
    QString path;
    QDateTime arrived;
};

QVector<Arrived> audioIn(const QString& folder) {
    QVector<Arrived> files;
    const QFileInfoList entries = QDir(folder).entryInfoList(QDir::Files | QDir::Readable, QDir::Name);
    for (const QFileInfo& entry : entries) {
        if (entry.isSymLink() || !isAudioFile(entry.fileName())) {
            continue;
        }
        files.append({entry.absoluteFilePath(), arrivalTime(entry)});
    }
    return files;
}

void newestFirst(QVector<Arrived>& files) {
    // Equal times keep the name order, so a list never shuffles between reads.
    std::stable_sort(files.begin(), files.end(), [](const Arrived& a, const Arrived& b) {
        return a.arrived > b.arrived;
    });
}

QStringList paths(const QVector<Arrived>& files) {
    QStringList out;
    out.reserve(files.size());
    for (const Arrived& file : files) {
        out.append(file.path);
    }
    return out;
}

} // namespace

QString musicFolder() {
    if (!musicFolderOverride().isEmpty()) {
        return musicFolderOverride();
    }
    // The utilities hard-code HOME/Music (apps/sample-maker, apps/get-music);
    // QStandardPaths' MusicLocation can be another folder.
    return QDir::home().filePath(QStringLiteral("Music"));
}

void setMusicFolderForTesting(const QString& folder) {
    musicFolderOverride() = folder;
}

QString samplesFolder() {
    const QString music = musicFolder();
    return music.isEmpty() ? QString() : QDir(music).filePath(kSamplesFolderName);
}

QString cardDownloadsFolder() {
    const QString music = musicFolder();
    return music.isEmpty() ? QString() : QDir(music).filePath(kCardDownloadsFolderName);
}

QStringList downloadFolders(const QStringList& stickPaths) {
    QStringList folders;
    for (const QString& stick : stickPaths) {
        const QString folder = QDir(stick).filePath(kDownloadsFolderName);
        if (plainFolder(folder)) {
            folders.append(QDir::cleanPath(folder));
        }
    }
    const QString card = cardDownloadsFolder();
    if (!card.isEmpty() && plainFolder(card)) {
        folders.append(QDir::cleanPath(card));
    }
    return folders;
}

bool isAudioFile(const QString& fileName) {
    return !fileName.startsWith(QLatin1Char('.')) &&
            audioSuffixes().contains(QFileInfo(fileName).suffix(), Qt::CaseInsensitive);
}

QDateTime arrivalTime(const QFileInfo& info) {
    QDateTime arrived = info.lastModified();
    for (const QDateTime& time : {info.birthTime(), info.metadataChangeTime()}) {
        if (time.isValid() && (!arrived.isValid() || time > arrived)) {
            arrived = time;
        }
    }
    return arrived;
}

QVector<DownloadEntry> listDownloads(const QStringList& folders) {
    QVector<DownloadEntry> entries;
    for (const QString& folder : folders) {
        for (const Arrived& file : audioIn(folder)) {
            DownloadEntry entry;
            entry.path = file.path;
            entry.name = QFileInfo(file.path).completeBaseName();
            entry.arrived = file.arrived;
            entries.append(entry);
        }
        const QFileInfoList children =
                QDir(folder).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable, QDir::Name);
        for (const QFileInfo& child : children) {
            if (child.isSymLink() || skippedFolder(child.fileName())) {
                continue;
            }
            const QVector<Arrived> tracks = audioIn(child.absoluteFilePath());
            if (tracks.isEmpty()) {
                continue;
            }
            DownloadEntry entry;
            entry.path = child.absoluteFilePath();
            entry.name = child.fileName();
            entry.folder = true;
            entry.tracks = static_cast<int>(tracks.size());
            for (const Arrived& track : tracks) {
                if (!entry.arrived.isValid() || track.arrived > entry.arrived) {
                    entry.arrived = track.arrived;
                }
            }
            entries.append(entry);
        }
    }
    std::stable_sort(entries.begin(), entries.end(), [](const DownloadEntry& a, const DownloadEntry& b) {
        return a.arrived > b.arrived;
    });
    return entries;
}

DownloadCounts countDownloads(const QStringList& folders) {
    // Names and the directory entries' own types only: no stat per file.
    const auto audio = [](const QString& folder) {
        int count = 0;
        const QStringList names = QDir(folder).entryList(QDir::Files | QDir::NoSymLinks, QDir::NoSort);
        for (const QString& name : names) {
            count += isAudioFile(name) ? 1 : 0;
        }
        return count;
    };
    DownloadCounts counts;
    for (const QString& folder : folders) {
        counts.tracks += audio(folder);
        const QStringList children =
                QDir(folder).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::NoSort);
        for (const QString& child : children) {
            if (skippedFolder(child)) {
                continue;
            }
            const int tracks = audio(QDir(folder).filePath(child));
            if (tracks > 0) {
                counts.tracks += tracks;
                ++counts.playlists;
            }
        }
    }
    return counts;
}

QStringList newestDownloads(const QStringList& folders) {
    QVector<Arrived> files;
    for (const QString& folder : folders) {
        files += audioIn(folder);
    }
    newestFirst(files);
    return paths(files);
}

QStringList allDownloads(const QStringList& folders) {
    QVector<Arrived> files;
    for (const QString& folder : folders) {
        files += audioIn(folder);
        const QFileInfoList children =
                QDir(folder).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable, QDir::Name);
        for (const QFileInfo& child : children) {
            if (!child.isSymLink() && !skippedFolder(child.fileName())) {
                files += audioIn(child.absoluteFilePath());
            }
        }
    }
    newestFirst(files);
    return paths(files);
}

int playlistNumber(const QString& fileName) {
    // Get Music numbers a list's tracks with two digits, three from 100 on.
    static const QRegularExpression numbered(QStringLiteral("^(\\d{2,3}) - "));
    const QRegularExpressionMatch match = numbered.match(fileName);
    return match.hasMatch() ? match.captured(1).toInt() : 0;
}

QStringList playlistOrder(const QString& folder) {
    QVector<Arrived> files = audioIn(folder);
    // Unnumbered tracks (saved before numbering, or added by hand) follow in
    // the order they arrived: a list downloads one track at a time, in order.
    std::stable_sort(files.begin(), files.end(), [](const Arrived& a, const Arrived& b) {
        const int left = playlistNumber(QFileInfo(a.path).fileName());
        const int right = playlistNumber(QFileInfo(b.path).fileName());
        if ((left > 0) != (right > 0)) {
            return left > 0;
        }
        if (left > 0) {
            return left < right;
        }
        return a.arrived < b.arrived;
    });
    return paths(files);
}

QStringList audioFilesNewestFirst(const QString& folder) {
    QVector<Arrived> files = audioIn(folder);
    newestFirst(files);
    return paths(files);
}

bool isSampleLocation(const QString& location) {
    const QString samples = samplesFolder();
    return !samples.isEmpty() && location.startsWith(QDir::cleanPath(samples) + QLatin1Char('/'));
}

bool isDownloadLocation(const QString& location) {
    const QString stick = mediaRootOf(location);
    if (!stick.isEmpty() &&
            location.startsWith(stick + QLatin1Char('/') + kDownloadsFolderName + QLatin1Char('/'))) {
        return true;
    }
    const QString card = cardDownloadsFolder();
    return !card.isEmpty() && location.startsWith(QDir::cleanPath(card) + QLatin1Char('/'));
}

} // namespace decklab
