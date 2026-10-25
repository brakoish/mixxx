// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabanalysis.h"

#include <QHash>
#include <QSqlQuery>
#include <algorithm>
#include <utility>

#include "analyzer/analyzerprogress.h"
#include "analyzer/analyzerscheduledtrack.h"
#include "decklab/decklabmedia.h"
#include "decklab/decklabsetupmodel.h"
#include "library/library.h"
#include "library/queryutil.h"
#include "library/trackcollectionmanager.h"
#include "moc_decklabanalysis.cpp"
#include "track/globaltrackcache.h"
#include "track/track.h"

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
    m_hooks.start = [this](TrackId id) {
        if (!m_library || !id.isValid()) {
            return false;
        }
        if (!m_scheduler) {
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
                        if (progress < kAnalyzerProgressDone && progress != kAnalyzerProgressUnknown) {
                            return;
                        }
                        if (trackId == m_current && m_holding) {
                            m_held.append(std::move(m_holding));
                        }
                        trackFinished(trackId);
                    });
            // Ended without a word on our track: it could not even be loaded.
            connect(m_scheduler.get(), &TrackAnalysisScheduler::finished, this, [this] {
                if (m_current.isValid()) {
                    trackFinished(m_current);
                }
            });
        }
        // Kept while it is analyzed and after: the save to the library runs
        // when the last reference goes, and that waits until it is quiet.
        m_holding = GlobalTrackCacheLocker().lookupTrackById(id);
        if (!m_holding && m_library->trackCollectionManager()) {
            m_holding = m_library->trackCollectionManager()->getTrackById(id);
        }
        m_scheduler->scheduleTrack(AnalyzerScheduledTrack(id));
        m_scheduler->resume();
        return true;
    };
    // TrackAnalysisScheduler's own pause: the worker sleeps between chunks.
    m_hooks.pause = [this] {
        if (m_scheduler) {
            m_scheduler->suspend();
        }
    };
    m_hooks.resume = [this] {
        if (m_scheduler) {
            m_scheduler->resume();
        }
    };
    m_hooks.stop = [this] {
        // A track the analyzer was finishing may still change: its save
        // waits until it is quiet, like an analyzed one.
        if (m_holding) {
            m_held.append(std::move(m_holding));
        }
        parkScheduler();
    };
    m_hooks.closing = [this] {
        sweepParked();
        return !m_parked.empty();
    };
    m_hooks.release = [this] {
        m_held.clear();
        sweepParked();
    };
}

void DeckLabBackgroundAnalysis::parkScheduler() {
    if (!m_scheduler) {
        return;
    }
    // Stopping only raises a flag: the worker lets go of its file after the
    // chunk it is on (or its save) and exits by itself. The scheduler is
    // deleted once its thread is gone, so nothing here waits for it.
    disconnect(m_scheduler.get(), nullptr, this, nullptr);
    m_scheduler->stop();
    connect(m_scheduler.get(),
            &TrackAnalysisScheduler::finished,
            this,
            &DeckLabBackgroundAnalysis::sweepParked,
            Qt::QueuedConnection);
    m_parked.push_back(std::move(m_scheduler));
    m_scheduler = TrackAnalysisScheduler::NullPointer();
}

void DeckLabBackgroundAnalysis::sweepParked() {
    m_parked.erase(std::remove_if(m_parked.begin(),
                           m_parked.end(),
                           [](const TrackAnalysisScheduler::Pointer& parked) {
                               return !parked || !parked->hasRunningWorkers();
                           }),
            m_parked.end());
}

DeckLabBackgroundAnalysis::State DeckLabBackgroundAnalysis::state() const {
    if (m_current.isValid()) {
        return m_paused ? State::Waiting : State::Running;
    }
    return m_total > m_done && (m_pending || !m_queue.isEmpty()) ? State::Waiting : State::Idle;
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
        pause();
    }
}

void DeckLabBackgroundAnalysis::pause() {
    if (!m_current.isValid() || m_paused) {
        return;
    }
    if (!decklab::mediaRootOf(m_currentLocation).isEmpty()) {
        // A file left open on a USB stick for a whole set would keep the
        // stick from being ejected: let go of the track instead. It starts
        // again, from its beginning, after the next quiet spell.
        const TrackId id = m_current;
        letGoOfCurrent();
        m_queue.prepend(id);
        if (!m_timer.isActive()) {
            m_timer.start();
        }
        emit progressChanged();
        return;
    }
    m_paused = true;
    if (m_hooks.pause) {
        m_hooks.pause();
    }
    if (!m_timer.isActive()) {
        m_timer.start();
    }
    emit progressChanged();
}

void DeckLabBackgroundAnalysis::letGoOfCurrent() {
    m_current = TrackId();
    m_currentLocation.clear();
    m_paused = false;
    m_analyzerUp = false;
    if (m_hooks.stop) {
        m_hooks.stop();
    }
}

bool DeckLabBackgroundAnalysis::letGo(const QString& mount) {
    if (!mount.isEmpty()) {
        m_leaving.insert(mount, m_clock.elapsed() + kLeavingMs);
        if (m_current.isValid() && decklab::isPathOnMount(m_currentLocation, mount)) {
            letGoOfCurrent();
            emit progressChanged();
        }
    }
    return m_hooks.closing && m_hooks.closing();
}

bool DeckLabBackgroundAnalysis::leaving(const QString& location) {
    const qint64 now = m_clock.elapsed();
    bool on = false;
    for (auto it = m_leaving.begin(); it != m_leaving.end();) {
        if (it.value() <= now) {
            it = m_leaving.erase(it);
            continue;
        }
        on = on || decklab::isPathOnMount(location, it.key());
        ++it;
    }
    return on;
}

void DeckLabBackgroundAnalysis::trackFinished(TrackId trackId) {
    if (!trackId.isValid()) {
        return;
    }
    m_tried.insert(trackId);
    if (trackId != m_current) {
        return;
    }
    m_current = TrackId();
    m_currentLocation.clear();
    m_paused = false;
    m_done = std::min(m_done + 1, m_total);
    // The next one starts at once while it is quiet (no "paused" between).
    check();
    emit progressChanged();
}

TrackId DeckLabBackgroundAnalysis::next() {
    while (true) {
        if (m_queue.isEmpty()) {
            if (!m_pending) {
                return TrackId();
            }
            m_pending = false;
            m_queue = m_hooks.candidates ? m_hooks.candidates() : QList<TrackId>();
            if (m_done >= m_total) {
                m_done = 0; // a new batch counts from one
            }
            if (m_queue.isEmpty()) {
                return TrackId();
            }
        }
        const TrackId id = m_queue.takeFirst();
        // A player's own analysis takes a track it holds; it comes back with
        // the next batch if it still needs one. So does one on a stick that
        // is being ejected.
        if (!id.isValid() || m_tried.contains(id) || (m_hooks.loaded && m_hooks.loaded(id))) {
            continue;
        }
        if (!m_leaving.isEmpty() && leaving(m_hooks.location ? m_hooks.location(id) : QString())) {
            continue;
        }
        return id;
    }
}

void DeckLabBackgroundAnalysis::check() {
    const qint64 now = m_clock.elapsed();
    if (m_current.isValid() && m_hooks.loaded && m_hooks.loaded(m_current)) {
        // A deck (or the preview) loaded the track being analyzed: two
        // decoders on one file help nobody. Let go of it, without waiting.
        letGoOfCurrent();
        emit progressChanged();
    }
    if (m_hooks.busy && m_hooks.busy()) {
        m_quietSince = now;
        pause();
        return;
    }
    if (m_hooks.release) {
        m_hooks.release();
    }
    if (now < m_startupMs || now - m_quietSince < m_idleMs) {
        if ((m_current.isValid() || m_pending || !m_queue.isEmpty()) && !m_timer.isActive()) {
            m_timer.start();
        }
        return;
    }
    if (m_current.isValid()) {
        if (m_paused) {
            m_paused = false;
            if (m_hooks.resume) {
                m_hooks.resume();
            }
            emit progressChanged();
        }
        return; // its end comes from the analyzer
    }
    const TrackId id = next();
    if (!id.isValid()) {
        // Nothing left: the analyzer's thread may exit.
        m_timer.stop();
        if (std::exchange(m_analyzerUp, false) && m_hooks.stop) {
            m_hooks.stop();
        }
        if (m_total || m_done) {
            m_total = 0;
            m_done = 0;
            emit progressChanged();
        }
        return;
    }
    if (!m_hooks.start || !m_hooks.start(id)) {
        m_queue.prepend(id); // the analyzer is not free: the next check tries again
        if (!m_timer.isActive()) {
            m_timer.start();
        }
        return;
    }
    m_analyzerUp = true;
    m_current = id;
    m_currentLocation = m_hooks.location ? m_hooks.location(id) : QString();
    m_total = m_done + 1 + static_cast<int>(m_queue.size());
    if (!m_timer.isActive()) {
        m_timer.start();
    }
    emit progressChanged();
}

QList<TrackId> DeckLabBackgroundAnalysis::tracksToAnalyze(
        const QSqlDatabase& database, const QStringList& locations, QHash<TrackId, QString>* where) {
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
            if (where) {
                where->insert(it.value(), location);
            }
        }
    }
    return ids;
}
