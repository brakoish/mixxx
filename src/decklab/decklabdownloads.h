// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Deck Lab round 7 (r7-browse): the folders the utilities save music into and
// what is in them, newest first. Get Music (apps/get-music in the appliance
// repo: storage.py, spotify.py) saves into "Deck Lab Downloads" at the top of
// the chosen USB stick, or into ~/Music/New Music on the Pi card; a Spotify
// playlist or album goes into a folder named after it, its tracks numbered
// in playlist order ("01 - Artist - Title.m4a"). Sample Maker and the
// sampler's loop capture save into ~/Music/Deck Lab Samples.
//
// Plain directory reads, no library and no widgets: the Browse home's
// DOWNLOADS and SAMPLES tiles, their lists, the song lists' samples filter
// and the background analysis all read these.

#include <QDateTime>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QVector>

namespace decklab {

inline const QString kDownloadsFolderName = QStringLiteral("Deck Lab Downloads");
inline const QString kCardDownloadsFolderName = QStringLiteral("New Music");
inline const QString kSamplesFolderName = QStringLiteral("Deck Lab Samples");

/// ~/Music: the Music folder in the home folder, the one Get Music and
/// Sample Maker name (and their systemd units allow), never the desktop's
/// localized or XDG_MUSIC_DIR music folder. Tests point it at a generated
/// folder; empty restores the real one.
QString musicFolder();
void setMusicFolderForTesting(const QString& folder);
/// ~/Music/Deck Lab Samples and ~/Music/New Music (whether or not they
/// exist). The one definition of each: TO PAD, PUT ON PAD (sampler.load),
/// the SAMPLES tile and list, the song lists' filter and the library folders
/// all use these.
QString samplesFolder();
QString cardDownloadsFolder();

/// The download folders that exist now: each plugged-in stick's "Deck Lab
/// Downloads" in the order given, then the Pi card's. A stick that is not
/// plugged in is not in stickPaths, so it contributes nothing. Plain folders
/// only, never a link.
QStringList downloadFolders(const QStringList& stickPaths);

/// Audio files Mixxx plays (by extension).
bool isAudioFile(const QString& fileName);

/// When a file arrived: the newest of its birth, status change and
/// modification times (a downloader may date a file by its upload).
QDateTime arrivalTime(const QFileInfo& info);

struct DownloadEntry {
    QString path;   // the file, or the playlist folder
    QString name;   // the file's base name, or the folder's name
    bool folder = false;
    int tracks = 1; // a folder: the tracks in it
    QDateTime arrived; // a folder: its newest track
};
/// What the download folders hold: single downloads and playlist folders,
/// newest first (a folder by its newest track). Folders without tracks and
/// dot or system folders are left out.
QVector<DownloadEntry> listDownloads(const QStringList& folders);
/// The single downloads only (files at the top of the folders), newest first.
QStringList newestDownloads(const QStringList& folders);
/// Every download, playlist tracks too, newest first (the analysis order).
QStringList allDownloads(const QStringList& folders);
/// A playlist folder's tracks in playlist order: numbered names ("01 - …",
/// "100 - …") by their number, then any others in the order they arrived.
QStringList playlistOrder(const QString& folder);
/// The playlist number a file name starts with ("07 - Artist - Title.m4a"
/// is 7); 0 when it has none.
int playlistNumber(const QString& fileName);
/// Audio files directly in folder, newest first.
QStringList audioFilesNewestFirst(const QString& folder);

/// Whether location lies inside the samples folder.
bool isSampleLocation(const QString& location);

} // namespace decklab
