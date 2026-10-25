// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabanalysis.h"

#include <QHash>
#include <QSqlQuery>
#include <algorithm>

#include "analyzer/analyzerprogress.h"
#include "analyzer/analyzerscheduledtrack.h"
#include "library/library.h"
#include "library/queryutil.h"
#include "moc_decklabanalysis.cpp"

namespace {
// SQLite's host parameter limit is 999 on older builds.
constexpr int kLookupChunk = 400;
} // namespace

DeckLabBackgroundAnalysis::DeckLabBackgroundAnalysis(QObject* parent)
        : QObject(parent),
          m_scheduler(TrackAnalysisScheduler::NullPointer()) {
    m_clock.start();
    m_timer.setInterval(kCheckMs);
    connect(&m_timer, &QTimer::timeout, this, &DeckLabBackgroundAnalysis::check);
    m_stopTimeout.setSingleShot(true);
    m_stopTimeout.setInterval(kStopTimeoutMs);
    // A batch that never reports its end after a stop counts as ended.
    connect(&m_stopTimeout, &QTimer::timeout, this, &DeckLabBackgroundAnalysis::batchFinished);
}

DeckLabBackgroundAnalysis::~DeckLabBackgroundAnalysis() = default;

void DeckLabBackgroundAnalysis::setHooks(Hooks hooks) {
    m_hooks = std::move(hooks);
}

void DeckLabBackgroundAnalysis::setDelays(int startupMs, int idleMs) {
    m_startupMs = std::max(0, startupMs);
    m_idleMs = std::max(0, idleMs);
}

void DeckLabBackgroundAnalysis::useLibrary(Library* library, UserSettingsPointer config) {
    m_library = library;
    m_config = std::move(config);
    m_hooks.start = [this](const QList<TrackId>& ids) {
        // The last batch's analyzer is still letting go: try again shortly.
        if (!m_library || ids.isEmpty() || m_scheduler) {
            return false;
        }
        int flags = AnalyzerModeFlags::WithBeats | AnalyzerModeFlags::LowPriority;
        if (!m_config ||
                m_config->getValue<bool>(
                        ConfigKey("[Library]", "EnableWaveformGenerationWithAnalysis"), true)) {
            flags |= AnalyzerModeFlags::WithWaveform;
        }
        // One thread: the Pi's other cores belong to the GUI and the audio.
        m_scheduler = m_library->createTrackAnalysisScheduler(1, static_cast<AnalyzerModeFlags>(flags));
        connect(m_scheduler.get(),
                &TrackAnalysisScheduler::trackProgress,
                this,
                [this](TrackId trackId, AnalyzerProgress progress) {
                    // Done (1) or failed (unknown); busy progress lies between.
                    if (progress >= kAnalyzerProgressDone || progress == kAnalyzerProgressUnknown) {
                        trackFinished(trackId);
                    }
                });
        connect(m_scheduler.get(),
                &TrackAnalysisScheduler::finished,
                this,
                &DeckLabBackgroundAnalysis::batchFinished);
        QList<AnalyzerScheduledTrack> tracks;
        tracks.reserve(ids.size());
        for (const TrackId& id : ids) {
            tracks.append(AnalyzerScheduledTrack(id));
        }
        m_scheduler->scheduleTracks(tracks);
        m_scheduler->resume();
        return true;
    };
    m_hooks.stop = [this] {
        if (m_scheduler) {
            m_scheduler->stop();
        }
    };
}

DeckLabBackgroundAnalysis::State DeckLabBackgroundAnalysis::state() const {
    if (m_running && !m_stopping) {
        return State::Running;
    }
    return m_total > m_done && (m_pending || m_stopping) ? State::Waiting : State::Idle;
}

QString DeckLabBackgroundAnalysis::progressText() const {
    switch (state()) {
    case State::Running:
        return tr("Analyzing %1 of %2").arg(std::min(m_done + 1, m_total)).arg(m_total);
    case State::Waiting:
        return tr("Analysis paused at %1 of %2").arg(std::min(m_done + 1, m_total)).arg(m_total);
    case State::Idle:
        break;
    }
    return {};
}

void DeckLabBackgroundAnalysis::wake() {
    m_pending = true;
    if (!m_timer.isActive()) {
        m_timer.start();
    }
}

void DeckLabBackgroundAnalysis::playbackChanged() {
    // Quiet is counted from the last change, so a stop starts the wait.
    m_quietSince = m_clock.elapsed();
    if (m_hooks.busy && m_hooks.busy()) {
        requestStop();
    }
}

void DeckLabBackgroundAnalysis::requestStop() {
    if (!m_running || m_stopping) {
        return;
    }
    m_stopping = true;
    m_pending = true; // what is left goes on once it is quiet
    if (m_hooks.stop) {
        m_hooks.stop();
    }
    m_stopTimeout.start();
    if (!m_timer.isActive()) {
        m_timer.start();
    }
    emit progressChanged();
}

void DeckLabBackgroundAnalysis::trackFinished(TrackId trackId) {
    if (!trackId.isValid() || m_tried.contains(trackId)) {
        return;
    }
    m_tried.insert(trackId);
    if (m_running) {
        m_done = std::min(m_done + 1, m_total);
        emit progressChanged();
    }
}

void DeckLabBackgroundAnalysis::batchFinished() {
    if (!m_running) {
        return;
    }
    // A batch that ran to its end has tried every track it was given (one
    // the analyzer could not even load reports nothing): none comes back.
    if (!m_stopping) {
        for (const TrackId& id : std::as_const(m_batch)) {
            m_tried.insert(id);
        }
    }
    m_batch.clear();
    m_running = false;
    m_stopping = false;
    m_stopTimeout.stop();
    // Look again: tracks may have come in meanwhile, or the batch stopped.
    m_pending = true;
    // Its thread has let go; the scheduler is deleted later by Qt, and a late
    // signal from it never reaches the next batch.
    if (m_scheduler) {
        disconnect(m_scheduler.get(), nullptr, this, nullptr);
        m_scheduler.reset();
    }
    if (!m_timer.isActive()) {
        m_timer.start();
    }
    emit progressChanged();
}

void DeckLabBackgroundAnalysis::check() {
    const qint64 now = m_clock.elapsed();
    if (m_hooks.busy && m_hooks.busy()) {
        m_quietSince = now;
        requestStop();
        return;
    }
    if (m_running) {
        return; // its progress and its end come from the analyzer
    }
    if (!m_pending) {
        m_timer.stop();
        return;
    }
    if (now < m_startupMs || now - m_quietSince < m_idleMs) {
        return;
    }
    QList<TrackId> ids = m_hooks.candidates ? m_hooks.candidates() : QList<TrackId>();
    ids.erase(std::remove_if(ids.begin(),
                      ids.end(),
                      [this](const TrackId& id) { return !id.isValid() || m_tried.contains(id); }),
            ids.end());
    if (ids.isEmpty() || !m_hooks.start) {
        m_pending = false;
        m_timer.stop();
        if (m_total || m_done) {
            m_total = 0;
            m_done = 0;
            emit progressChanged();
        }
        return;
    }
    if (m_done >= m_total) {
        m_done = 0; // a new batch counts from one
    }
    m_total = m_done + static_cast<int>(ids.size());
    if (!m_hooks.start(ids)) {
        return; // the analyzer is not free yet: the next check tries again
    }
    m_pending = false;
    m_running = true;
    m_batch = ids;
    emit progressChanged();
}

QList<TrackId> DeckLabBackgroundAnalysis::tracksToAnalyze(
        const QSqlDatabase& database, const QStringList& locations) {
    QHash<QString, TrackId> found;
    for (int from = 0; from < locations.size(); from += kLookupChunk) {
        const QStringList chunk = locations.mid(from, kLookupChunk);
        QStringList marks;
        for (int n = 0; n < chunk.size(); ++n) {
            marks << QStringLiteral("?");
        }
        QSqlQuery query(database);
        query.prepare(QStringLiteral(
                "SELECT library.id, track_locations.location FROM library "
                "INNER JOIN track_locations ON library.location = track_locations.id "
                "WHERE library.mixxx_deleted = 0 AND track_locations.fs_deleted = 0 "
                "AND (library.bpm IS NULL OR library.bpm <= 0 OR library.beats IS NULL "
                "OR library.key IS NULL OR library.key = '') "
                "AND track_locations.location IN (%1)")
                        .arg(marks.join(QLatin1Char(','))));
        for (const QString& location : chunk) {
            query.addBindValue(location);
        }
        if (!query.exec()) {
            LOG_FAILED_QUERY(query);
            continue;
        }
        while (query.next()) {
            found.insert(query.value(1).toString(), TrackId(query.value(0)));
        }
    }
    QList<TrackId> ids;
    for (const QString& location : locations) {
        const auto it = found.constFind(location);
        if (it != found.cend()) {
            ids.append(it.value());
        }
    }
    return ids;
}
