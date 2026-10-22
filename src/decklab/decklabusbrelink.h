// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QFutureWatcher>
#include <QJsonArray>
#include <QList>
#include <QObject>
#include <QSet>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>

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

// Relative paths `location` would have on this volume if it lies under one
// of the volume's earlier mount points: /media/<user>/<label|uuid>,
// /run/media/<user>/<label|uuid> or /media/<label|uuid>. Never the current
// mount. Several readings are possible; the most specific comes first.
QStringList legacyRelativePaths(const UsbVolume& volume, const QString& location);

// One library row (track_locations joined with library).
struct RelinkRow {
    qint64 locationId = -1; // track_locations.id
    qint64 trackId = -1;    // library.id
    QString location;
    qint64 fileSize = 0; // as last recorded; 0 or less when unknown
};

struct RelinkMove {
    RelinkRow row;
    QString newLocation;
};

struct RelinkPlan {
    QList<RelinkMove> moves;
    // Rows of this volume left alone: the old file is still reachable, the
    // file is missing or a different size on the stick, or another row
    // already claimed the same file.
    int skipped = 0;
    // Rows left alone because their track is loaded or cached right now;
    // they are retried the next time the drive list changes.
    int busy = 0;
};

// Returns the size of a regular file, or -1 when there is none. Injectable
// so the planner can be tested without a stick.
using FileSizeFunction = std::function<qint64(const QString& path)>;
qint64 regularFileSize(const QString& path);

// Pure planning: which rows of `volume` move to which path on its current
// mount. Reads file sizes only; never writes anything.
RelinkPlan planRelink(const UsbVolume& volume,
        const QList<RelinkRow>& rows,
        const FileSizeFunction& fileSize,
        const QSet<qint64>& busyTrackIds = {});

// Library rows that live under /media or /run/media.
QList<RelinkRow> relinkCandidates(const QSqlDatabase& database);

struct RelinkResult {
    QList<RelinkMove> moved;
    QStringList conflicts; // a row already exists at the new location: both kept
};

// Rewrites track_locations for each move in one transaction. A move only
// happens while the row still has its old location and no other row has the
// new one. Rows are never deleted and nothing on the stick is written.
RelinkResult applyRelink(QSqlDatabase& database, const QList<RelinkMove>& moves);

// Session glue: when decklabd reports drives, map library rows of known
// sticks from their old mount paths to the current one. File checks run on
// a worker thread; database updates on the GUI thread.
class UsbRelinker final : public QObject {
    Q_OBJECT
  public:
    using BusyTracks = std::function<QSet<qint64>()>;
    explicit UsbRelinker(std::shared_ptr<TrackCollectionManager> library,
            QObject* parent = nullptr);
    ~UsbRelinker() override;
    // Default: tracks held anywhere in the global track cache.
    void setBusyTracks(BusyTracks busy) {
        m_busy = std::move(busy);
    }
    bool isRunning() const {
        return m_watcher.isRunning();
    }

  public slots:
    void relinkDrives(const QJsonArray& drives);

  signals:
    void relinked(int moved, int conflicts);

  private:
    struct Job {
        UsbVolume volume;
        QList<RelinkRow> rows;
        QSet<qint64> busy;
    };
    void startNext();
    void finish();
    std::shared_ptr<TrackCollectionManager> m_library;
    BusyTracks m_busy;
    QList<UsbVolume> m_pending;
    QSet<QString> m_done; // uuid + mount handled in this session
    QFutureWatcher<RelinkPlan> m_watcher;
    UsbVolume m_running;
};

} // namespace decklab
