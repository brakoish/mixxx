// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QSqlDatabase>
#include <QStringList>
#include <QTimer>
#include <functional>
#include <vector>

#include "analyzer/trackanalysisscheduler.h"
#include "preferences/usersettings.h"
#include "track/track_decl.h"
#include "track/trackid.h"

class Library;

/// r7-browse: new tracks (Get Music's downloads) are analyzed in the
/// background for BPM, key, beat grid and waveform, newest first, and only
/// while it is quiet (decklab/decklabquiet.h: nothing plays or records, no
/// library scan, no utility window). Mixxx's own TrackAnalysisScheduler does
/// the work on one low-priority analyzer thread (SCHED_IDLE on the
/// appliance, DECKLAB_ANALYZER_IDLE), never on the GUI thread; this class
/// decides when.
///
///  * One track at a time. The next starts once nothing has kept it waiting
///    for kIdleMs (never in the first kStartupMs after Mixxx started).
///  * The moment it is not quiet the analyzer pauses before its next chunk
///    and keeps its place; it goes on after the next quiet spell. A track on
///    a USB stick is let go of instead (its file closes after one chunk, so
///    the stick can be ejected mid-set) and starts again after the next
///    quiet spell. Nothing on the GUI thread ever waits for the analyzer
///    thread.
///  * A stick about to be ejected (letGo) is left alone: a track on it is
///    let go of, and none on it starts for kLeavingMs.
///  * A track a deck, sampler or the preview holds (or is loading) is left
///    to that player's own analysis: never started, and dropped if a player
///    loads it while it is analyzed.
///  * An analyzed track is kept until it is quiet, so its save to the
///    library (when the last reference goes) never runs during a set or
///    beside a library scan.
///  * A track the analyzer finished (or could not read) is never queued
///    again this session.
///  * Progress counts the tracks of one batch across pauses
///    ("Analyzing 3 of 12").
///
/// Every outside effect goes through Hooks, so the policy is testable without
/// a library, players or threads.
class DeckLabBackgroundAnalysis : public QObject {
    Q_OBJECT
  public:
    struct Hooks {
        std::function<bool()> busy;                       // not quiet (decklab/decklabquiet.h)
        std::function<QList<TrackId>()> candidates;       // still to analyze, newest first
        std::function<bool(TrackId)> loaded;              // a deck, sampler or preview holds or loads it
        std::function<bool(TrackId)> start;               // analyze this track; false: not now
        std::function<void()> pause;                      // the analyzer sleeps before its next chunk
        std::function<void()> resume;                     // ... and goes on where it was
        std::function<void()> stop;                       // lets go of the analyzer and its track; never waits
        std::function<void()> release;                    // quiet: analyzed tracks may be saved now
        std::function<QString(TrackId)> location;         // a candidate's file; empty when unknown
        std::function<bool()> closing;                    // a let-go analyzer still has its file open
    };
    enum class State {
        Idle,    // nothing to do
        Waiting, // tracks left (or one paused), waiting for it to be quiet
        Running,
    };
    static constexpr int kStartupMs = 30000;
    static constexpr int kIdleMs = 15000;
    static constexpr int kCheckMs = 1000;
    static constexpr int kLeavingMs = 60000;

    explicit DeckLabBackgroundAnalysis(QObject* parent = nullptr);
    ~DeckLabBackgroundAnalysis() override;

    void setHooks(Hooks hooks);
    Hooks hooks() const {
        return m_hooks;
    }
    void setDelays(int startupMs, int idleMs);
    /// Analyze with Mixxx's TrackAnalysisScheduler from this library (one
    /// low-priority thread, waveforms as the Analyze view makes them):
    /// replaces the start, pause, resume, stop and release hooks.
    void useLibrary(Library* library, UserSettingsPointer config);

    State state() const;
    int done() const {
        return m_done;
    }
    int total() const {
        return m_total;
    }
    /// The track being analyzed (paused or not); invalid when none.
    TrackId current() const {
        return m_current;
    }
    /// "Analyzing 3 of 12", "Analysis paused at 3 of 12"; empty when idle.
    QString progressText() const;

    /// New tracks may be there (a library scan finished, a stick came in).
    void wake();
    /// Something that keeps it waiting changed (play, a recording, a scan,
    /// a utility): the analyzer pauses at once if it is not quiet, and the
    /// quiet spell counts from now.
    void playbackChanged();
    /// From the analyzer: one track finished, analyzed or not.
    void trackFinished(TrackId trackId);
    /// The USB stick mounted at `mount` is about to be ejected: a track on it
    /// is let go of (not counted as tried: it comes back with the stick) and
    /// none on it starts for a while. True while the analyzer still has a
    /// file open; never waits.
    bool letGo(const QString& mount);
    /// Decides now; the timer does this once a second while there is work.
    void check();

    /// The library tracks among locations that still need a BPM, a key or a
    /// beat grid, in the order given, and where each one is (`where`, when
    /// given). Read only.
    static QList<TrackId> tracksToAnalyze(const QSqlDatabase& database,
            const QStringList& locations,
            QHash<TrackId, QString>* where = nullptr);

  signals:
    void progressChanged();

  private:
    TrackId next();
    void pause();
    void letGoOfCurrent();
    bool leaving(const QString& location);
    void parkScheduler();
    void sweepParked();

    Hooks m_hooks;
    QTimer m_timer;
    QElapsedTimer m_clock;
    int m_startupMs = kStartupMs;
    int m_idleMs = kIdleMs;
    qint64 m_quietSince = 0;
    bool m_pending = false; // there may be tracks to analyze that are not queued yet
    TrackId m_current;      // being analyzed
    QString m_currentLocation; // ... its file (hooks.location)
    QHash<QString, qint64> m_leaving; // mounts being ejected -> until (m_clock)
    bool m_paused = false;  // ... and paused because it is not quiet
    bool m_analyzerUp = false; // started and not let go of since (stop)
    int m_done = 0;
    int m_total = 0;
    QSet<TrackId> m_tried;  // finished, failed or skipped this session: never queued again
    QList<TrackId> m_queue; // the rest of this batch, newest first
    Library* m_library = nullptr;
    UserSettingsPointer m_config;
    TrackAnalysisScheduler::Pointer m_scheduler;
    // Stopped schedulers whose thread has not exited yet; deleted once it
    // has, so deleting one never waits for its thread.
    std::vector<TrackAnalysisScheduler::Pointer> m_parked;
    TrackPointer m_holding;     // the track being analyzed
    QList<TrackPointer> m_held; // analyzed, saved once it is quiet
};
