// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// usb-first, r7-library: one library entry per song on a stick, at the
// stick's current address.
//
// The appliance mounts every stick at /media/decklab/<FS-UUID> and links
// /media/<user>/<label> to it; before Deck Lab, the desktop automounter used
// /media/<user>/<label>, /media/<user>/<label>1 when that name was taken, or
// /media/<user>/<FS-UUID> for a stick without a label. Mixxx keys tracks by
// path, so one song could have a row at each of those addresses, each with
// its own cues, crates and history.
//
// When a stick is in and nothing plays (start-up, a stick mounted), rows at
// an earlier address of that stick follow it: a row without a twin moves to
// the current address; twins merge onto the row at the current address,
// keeping every cue, loop, crate, history entry, play count, rating, grid and
// key. "The same song" is strict: an earlier address of the same filesystem
// (a link to its mount, its label with udisks' number, or its FS UUID), the
// same relative path and the same file size. Never title or artist.
// Nothing is deleted: a merged row is hidden (Mixxx's mixxx_deleted) with
// whatever the survivor already had; the database is copied beside itself
// first; the stick is only read. Tracks loaded or cached are left for later.
//
// r7-library: Get Music's rename maps. When Get Music renames downloaded
// files (its ORGANIZE step), it lists {old, new, size} in a hidden
// .deck-lab-renames.json beside them. A library row of a renamed file
// follows it to the new name in the same pass, so its cues, crates, history
// and analysis stay with it.

#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>

class TrackCollectionManager;

namespace decklab {

// A USB filesystem as decklabd reports it (usb.list / usb.changed).
struct UsbVolume {
    QString uuid;  // filesystem UUID; the appliance mounts at /media/decklab/<uuid>
    QString label; // filesystem label, may be empty
    QString mount; // current mount point
    bool operator==(const UsbVolume&) const = default;
};

// Drives with a UUID and an absolute mount point; anything else is skipped.
QList<UsbVolume> usbVolumesFromDrives(const QJsonArray& drives);

// A folder name an automounter gave this volume: its label or its UUID
// (case-insensitive), alone or with the number udisks adds when the name is
// taken ("MY STICK1").
bool isEarlierMountName(const UsbVolume& volume, const QString& name);

// Relative paths `location` would have on this volume if it lies under a
// folder named after it (isEarlierMountName): /media/<user>/<name>,
// /run/media/<user>/<name> or /media/<name>. Never the current mount. Several
// readings are possible; the most specific comes first.
QStringList legacyRelativePaths(const UsbVolume& volume, const QString& location);

// One library row (track_locations joined with library).
struct RelinkRow {
    qint64 locationId = -1; // track_locations.id
    qint64 trackId = -1;    // library.id
    QString location;
    qint64 fileSize = 0;  // as last recorded; 0 or less when unknown
    bool hidden = false;  // library.mixxx_deleted: left exactly as it is
    bool missing = false; // track_locations.fs_deleted
};

// What planning asks the filesystem; injectable so tests need no stick.
struct RelinkProbe {
    // The size of a regular file, -1 when there is none.
    std::function<qint64(const QString& path)> fileSize;
    // The path with links resolved, empty when nothing is there.
    std::function<QString(const QString& path)> resolve;
    // A folder with something in it (decklab::mediaRootIsIn).
    std::function<bool(const QString& path)> isIn;
};
RelinkProbe fileSystemProbe();

// One song of the volume: the rows that are it, onto its current address.
struct RelinkGroup {
    QString target;          // <mount>/<relative path>
    RelinkRow survivor;      // keeps its library id
    bool relocate = false;   // the survivor moves from an earlier address to target
    // A renamed file the library scan already added at its new name: the
    // survivor (the DJ's row) takes over the address of merged.first().
    bool takeOver = false;
    QList<RelinkRow> merged; // merged into the survivor, then hidden
};

struct RelinkPlan {
    QList<RelinkGroup> groups;
    // Rows at the current address marked missing whose file is back.
    QList<RelinkRow> revived;
    // Library folders (Mixxx's directories) at an earlier address of the
    // volume, and where they are now.
    QList<QPair<QString, QString>> folders;
    int sizeMismatch = 0; // same path, another size: another file, left alone
    int missing = 0;      // earlier rows whose file is not on the stick: left missing
    int blocked = 0;      // the current address is held by a hidden row
    // Songs left because a track is loaded or cached right now; they are
    // tried again the next time the stick is seen.
    int busy = 0;
    int renamed = 0; // groups that follow a rename map
    bool isEmpty() const {
        return groups.isEmpty() && revived.isEmpty() && folders.isEmpty();
    }
};

// Returns the size of a regular file, or -1 when there is none.
qint64 regularFileSize(const QString& path);

// Pure planning: which rows of `volume` move or merge onto its current
// mount. Reads the filesystem through probe only; never writes anything.
// rows: every library row under /media or /run/media (relinkCandidates);
// folders: the library folders (libraryFolders).
RelinkPlan planRelink(const UsbVolume& volume,
        const QList<RelinkRow>& rows,
        const QStringList& folders,
        const RelinkProbe& probe,
        const QSet<qint64>& busyTrackIds = {});

// Library rows that live under /media or /run/media.
QList<RelinkRow> relinkCandidates(const QSqlDatabase& database);
// Every library row.
QList<RelinkRow> libraryRows(const QSqlDatabase& database);

// --- Get Music's rename maps ---

// One entry of a .deck-lab-renames.json: a file in that folder was renamed.
struct RenameEntry {
    QString oldName; // file names, never paths
    QString newName;
    qint64 size = 0;
    bool operator==(const RenameEntry&) const = default;
};
// The rename maps by folder (no trailing slash).
using RenameMaps = QHash<QString, QList<RenameEntry>>;

inline const QString kRenameMapName = QStringLiteral(".deck-lab-renames.json");
constexpr int kRenameMapEntryLimit = 5000;            // the newest ones
constexpr qint64 kRenameMapByteLimit = 4 * 1024 * 1024; // bigger maps are not read

// A map's entries, oldest first: a JSON list of {"old", "new", "size"}.
// Entries that are not plain file names and a positive size are skipped;
// nullopt for a file that is not such a list.
std::optional<QList<RenameEntry>> parseRenameMap(const QByteArray& json, int limit = kRenameMapEntryLimit);
// Where rename maps can matter: the folders of library rows inside a
// library folder, and the library folders themselves.
QStringList renameMapFolders(const QList<RelinkRow>& rows, const QStringList& folders);
// The maps in those folders (file reads; never writes).
RenameMaps readRenameMaps(const QStringList& folders);
// Pure planning: library rows of renamed files follow them. A row moves
// when it is not hidden, its old file is gone, and the new file has the
// size the map and the row recorded; renames in a row (a to b to c) are
// followed. A row the library scan added at the new name is merged into
// it (takeOver). A cycle of names is left alone.
RelinkPlan planRenames(const QList<RelinkRow>& rows,
        const RenameMaps& maps,
        const RelinkProbe& probe,
        const QSet<qint64>& busyTrackIds = {});
// Mixxx's library folders (the directories table).
QStringList libraryFolders(const QSqlDatabase& database);

struct RelinkResult {
    bool ok = false; // committed (or nothing to do)
    struct Moved {
        qint64 trackId = -1;
        QString from;
        QString to;
    };
    QList<Moved> relocated;
    struct Merged {
        qint64 survivor = -1;
        qint64 hidden = -1;
        QString hiddenLocation;
        QString location; // the survivor's
    };
    QList<Merged> merged;
    int revived = 0;
    int folders = 0;
    int cues = 0;    // cues and loops moved to a survivor
    int crates = 0;  // crate memberships moved to a survivor
    int entries = 0; // playlist and history entries pointed at a survivor
    QSet<qint64> crateIds;
    QSet<int> playlistIds;
};

// Applies a plan in one transaction: everything or nothing. Each step checks
// the row is still as planned and skips it otherwise. Never deletes a row.
RelinkResult applyRelink(QSqlDatabase& database, const RelinkPlan& plan);

// Copies the library database beside itself (SQLite's VACUUM INTO, a
// consistent snapshot) as "<file>.decklab-merge-<time>" and keeps the newest
// `keep` copies. Returns the copy's path; an empty path for a database in
// memory (tests: nothing to copy); nullopt when no copy could be made.
// Never inside a transaction.
std::optional<QString> backupLibraryDatabase(const QSqlDatabase& database, int keep = 2);

// Session glue: when a stick is in (start-up, decklabd's drive list, a mount
// seen in the media folders), rows at its earlier addresses follow it and
// tracks on sticks that are out are hidden (decklabmedia.h). File checks run
// on a worker thread; database work on the GUI thread, only while nothing
// plays.
class UsbRelinker final : public QObject {
    Q_OBJECT
  public:
    using BusyTracks = std::function<QSet<qint64>()>;
    using Playing = std::function<bool()>;
    explicit UsbRelinker(std::shared_ptr<TrackCollectionManager> library,
            QObject* parent = nullptr);
    ~UsbRelinker() override;
    // Default: tracks held anywhere in the global track cache.
    void setBusyTracks(BusyTracks busy) {
        m_busy = std::move(busy);
    }
    // Default: nothing plays. The appliance: any deck, sampler or preview.
    void setPlaying(Playing playing) {
        m_playing = std::move(playing);
    }
    // Default: the real filesystem.
    void setProbe(RelinkProbe probe) {
        m_probe = std::move(probe);
    }
    bool isRunning() const {
        return m_watcher.isRunning();
    }

  public slots:
    void relinkDrives(const QJsonArray& drives);
    // The sticks mounted under /media/decklab now (start-up; works without
    // decklabd).
    void relinkMounted();
    // Get Music's rename maps (also with every stick and after every
    // library scan, which Deck Lab starts when a utility closes).
    void relinkRenames();
    // Hides or shows the tracks of sticks that went or came.
    void refreshMedia(bool force = false);
    // A stick mounted or taken out (its folder or link appears or goes in
    // /media, /media/decklab, /media/$USER or /run/media/$USER) refreshes
    // both, a moment later.
    void watchMediaFolders();

  signals:
    void relinked(int relocated, int merged);

  private:
    // true when a stick was added to the queue.
    bool queue(const QList<UsbVolume>& volumes);
    void startNext();
    void finish();
    void notify(const RelinkResult& result);
    void rewatch();
    std::shared_ptr<TrackCollectionManager> m_library;
    BusyTracks m_busy;
    Playing m_playing;
    RelinkProbe m_probe;
    QList<UsbVolume> m_pending;
    QSet<QString> m_done; // uuid + mount handled while the stick is in
    bool m_renamesPending = false;
    QFutureWatcher<RelinkPlan> m_watcher;
    std::optional<UsbVolume> m_running; // none: rename maps only
    bool m_runningRenames = false;
    QTimer m_retry; // a stick seen while something played: tried when it stops
    bool m_scanning = false; // Mixxx's library scan is running
    QFileSystemWatcher* m_folders = nullptr;
    QTimer m_settle; // media folder changes come in bursts
};

} // namespace decklab
