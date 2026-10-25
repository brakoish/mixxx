// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QSet>
#include <QSqlDatabase>
#include <QStringList>
#include <QTimer>
#include <functional>

#include "analyzer/trackanalysisscheduler.h"
#include "preferences/usersettings.h"
#include "track/trackid.h"

class Library;

/// r7-browse: new tracks (Get Music's downloads) are analyzed in the
/// background for BPM, key, beat grid and waveform, newest first, and only
/// while nothing plays. Mixxx's own TrackAnalysisScheduler does the work on
/// one low-priority analyzer thread (SCHED_IDLE on the appliance,
/// DECKLAB_ANALYZER_IDLE), never on the GUI thread; this class decides when.
///
///  * A batch starts once nothing has played for kIdleMs (never in the first
///    kStartupMs after Mixxx started).
///  * The moment a deck, sampler or preview plays (or a recording runs) the
///    batch stops: the analyzer lets go of its file after the chunk it is on.
///    What is left starts again the next time it is quiet.
///  * A track the analyzer finished (or could not read) is never queued
///    again this session.
///  * Progress counts the tracks of one batch across those pauses
///    ("Analyzing 3 of 12").
///
/// Every outside effect goes through Hooks, so the policy is testable without
/// a library, players or threads.
class DeckLabBackgroundAnalysis : public QObject {
    Q_OBJECT
  public:
    struct Hooks {
        std::function<bool()> busy;                       // anything plays or records
        std::function<QList<TrackId>()> candidates;       // still to analyze, newest first
        std::function<bool(const QList<TrackId>&)> start; // false: nothing started
        std::function<void()> stop;                       // asks the running batch to stop
    };
    enum class State {
        Idle,    // nothing to do
        Waiting, // tracks left, waiting for the decks to be quiet
        Running,
    };
    static constexpr int kStartupMs = 30000;
    static constexpr int kIdleMs = 15000;
    static constexpr int kCheckMs = 1000;
    static constexpr int kStopTimeoutMs = 5000;

    explicit DeckLabBackgroundAnalysis(QObject* parent = nullptr);
    ~DeckLabBackgroundAnalysis() override;

    void setHooks(Hooks hooks);
    Hooks hooks() const {
        return m_hooks;
    }
    void setDelays(int startupMs, int idleMs);
    /// Analyze with Mixxx's TrackAnalysisScheduler from this library (one
    /// low-priority thread, waveforms as the Analyze view makes them):
    /// replaces the start and stop hooks.
    void useLibrary(Library* library, UserSettingsPointer config);

    State state() const;
    int done() const {
        return m_done;
    }
    int total() const {
        return m_total;
    }
    /// "Analyzing 3 of 12", "Analysis paused at 3 of 12"; empty when idle.
    QString progressText() const;

    /// New tracks may be there (a library scan finished, a stick came in).
    void wake();
    /// A play control changed: a batch stops at once if anything plays.
    void playbackChanged();
    /// From the analyzer: one track finished, analyzed or not.
    void trackFinished(TrackId trackId);
    /// From the analyzer: the batch ended (done, or stopped).
    void batchFinished();
    /// Decides now; the timer does this once a second while there is work.
    void check();

    /// The library tracks among locations that still need a BPM, a key or a
    /// beat grid, in the order given. Read only.
    static QList<TrackId> tracksToAnalyze(const QSqlDatabase& database, const QStringList& locations);

  signals:
    void progressChanged();

  private:
    void requestStop();

    Hooks m_hooks;
    QTimer m_timer;
    QTimer m_stopTimeout;
    QElapsedTimer m_clock;
    int m_startupMs = kStartupMs;
    int m_idleMs = kIdleMs;
    qint64 m_quietSince = 0;
    bool m_pending = false;  // there may be tracks to analyze
    bool m_running = false;  // a batch started and has not reported its end
    bool m_stopping = false; // ... and was asked to stop
    int m_done = 0;
    int m_total = 0;
    QSet<TrackId> m_tried;   // finished, failed or skipped this session: never queued again
    QList<TrackId> m_batch;  // the running batch
    Library* m_library = nullptr;
    UserSettingsPointer m_config;
    TrackAnalysisScheduler::Pointer m_scheduler;
};
