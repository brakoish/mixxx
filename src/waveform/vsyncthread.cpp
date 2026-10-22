#include "vsyncthread.h"

#include "moc_vsyncthread.cpp"
#include "util/math.h"
#include "util/performancetimer.h"

namespace {

VSyncThread::VSyncMode defaultVSyncMode() {
#ifdef __APPLE__
    return VSyncThread::ST_PLL;
#else
    return VSyncThread::ST_TIMER;
#endif
}

} // namespace

VSyncThread::VSyncThread(QObject* pParent, VSyncThread::VSyncMode vSyncMode)
        : QThread(pParent),
          m_bDoRendering(true),
          m_syncIntervalTimeMicros(33333), // 30 FPS
          m_waitToSwapMicros(0),
          m_vSyncMode(vSyncMode == VSyncThread::ST_DEFAULT ? defaultVSyncMode() : vSyncMode),
          m_syncOk(false),
          m_droppedFrames(0),
          m_swapWait(0),
          m_displayFrameRate(60.0),
          m_vSyncPerRendering(1),
          m_pllInitializing(true),
          m_pllPendingUpdate(false),
          m_pllFallBackRequested(false),
          m_pllFellBack(false),
          m_lastSlotStartMicros(-1),
          m_skippedSlots(0),
          m_pllLogging(0.0) {
    m_pllTimer.start();
}

VSyncThread::~VSyncThread() {
    m_bDoRendering = false;
    m_semaVsyncSlot.release(m_vSyncMode.load() == ST_PLL ? 1 : 2); // Two slots, one for PLL
    wait();
    //delete m_glw;
}

void VSyncThread::run() {
    QThread::currentThread()->setObjectName("VSyncThread");

    m_waitToSwapMicros = m_syncIntervalTimeMicros;
    m_timer.start();

    //qDebug() << "VSyncThread::run()";
    switch (m_vSyncMode.load()) {
    case ST_FREE:
        m_syncIntervalTimeMicros = 1000;
        m_waitToSwapMicros = m_syncIntervalTimeMicros;
        runFree();
        break;
    case ST_PLL:
        runPLL();
        break;
    case ST_TIMER:
        runTimer();
        break;
    default:
        DEBUG_ASSERT(!"unsupported sync mode");
        break;
    }
}

void VSyncThread::runFree() {
    DEBUG_ASSERT(m_vSyncMode == ST_FREE);
    while (m_bDoRendering) {
        // for benchmark only!

        // renders the waveform, Possible delayed due to anti tearing
        emit vsyncRender();
        m_semaVsyncSlot.acquire();

        emit vsyncSwap(); // swaps the new waveform to front
        m_semaVsyncSlot.acquire();

        m_sinceLastSwap = m_timer.restart();
        usleep(m_waitToSwapMicros);
    }
}

void VSyncThread::runPLL() {
    DEBUG_ASSERT(m_vSyncMode == ST_PLL);
    qint64 offsetAdjustedAt = 0;
    qint64 offset = 0;
    double multiplierAdjustedPllPhaseOut = 0.0;
    bool anchorValid = false;
    while (m_bDoRendering) {
        if (m_pllFallBackRequested.load()) {
            // Startup safety (Deck Lab): the loop never locked. The factory
            // already logged why and stopped the keepalive paints.
            m_vSyncMode = ST_TIMER;
            m_pllFellBack = true;
            m_waitToSwapMicros = m_syncIntervalTimeMicros;
            m_timer.start();
            runTimer();
            return;
        }

        // Use a phase-locked-loop on the QOpenGLWindow::frameSwapped signal
        // to determine when the vsync occurs
        double pllPhaseOut;
        double pllDeltaOut;
        double multiplierAdjustedPllDeltaOut;
        double vblankAnchor;
        qint64 now;

        {
            std::scoped_lock lock(m_pllMutex);
            // last estimated vsync
            pllPhaseOut = m_pll.phaseOut();
            // estimated frame interval
            pllDeltaOut = m_pll.deltaOut();
            // where the vblank really was at the last sample (Deck Lab)
            vblankAnchor = m_pll.vblankEstimate();
            now = m_pllTimer.elapsed().toIntegerMicros();

            // Calculate the nearest integer number of PLL intervals that
            // correspond with the interval based on the frame rate from the
            // user settings. E.g. if the PLL is running at 60 fps, and the user
            // settings is between 25 and 40 fps, we do run effectively at 30
            // fps (multiplier is 2)
            //
            // Note this also is applied when running at 120 fps (ProMotion)
            const auto multiplier = std::max<qint64>(1,
                    std::llround(static_cast<double>(m_syncIntervalTimeMicros) /
                            pllDeltaOut));

            // Update the multiplierAdjustedPllPhaseOut to pllPhaseOut, if
            // pllPhaseOut has moved by a multiple of multiplier intervals.
            const qint64 framesSinceAnchor = std::llround(
                    (pllPhaseOut - multiplierAdjustedPllPhaseOut) / pllDeltaOut);
            if (!anchorValid ||
                    ((framesSinceAnchor % multiplier) + multiplier) % multiplier == 0) {
                multiplierAdjustedPllPhaseOut = pllPhaseOut;
                anchorValid = true;
            }

            multiplierAdjustedPllDeltaOut = pllDeltaOut * static_cast<double>(multiplier);
            vblankAnchor += std::round((multiplierAdjustedPllPhaseOut - vblankAnchor) /
                                    pllDeltaOut) *
                    pllDeltaOut;
        }

        // Deck Lab: wake on the grid of the current estimate (upstream only
        // followed corrections to a later phase), a little after the vblank,
        // and never twice in the vblank interval of a slot the GUI thread
        // started late.
        const qint64 lastSlotStart = m_lastSlotStartMicros.load();
        int skipped = 0;
        const double wake = mixxx::decklab::VSyncPll::nextSlotMicros(
                static_cast<double>(now),
                multiplierAdjustedPllPhaseOut,
                multiplierAdjustedPllDeltaOut,
                static_cast<double>(offset) + mixxx::decklab::VSyncPll::kSlotLeadMicros,
                vblankAnchor,
                !pllInitializing() && lastSlotStart >= 0,
                static_cast<double>(lastSlotStart),
                &skipped);
        if (skipped > 0) {
            m_skippedSlots += static_cast<quint64>(skipped);
        }
        const qint64 sleepUntilSwap = std::max<qint64>(0, std::llround(wake) - now);
        usleep(static_cast<unsigned long>(sleepUntilSwap));

        const qint64 periodMicros = std::llround(multiplierAdjustedPllDeltaOut);
        m_sinceLastSwap = m_timer.restart();
        m_waitToSwapMicros = static_cast<int>(periodMicros);

        // Signal to swap the gl widgets (waveforms, spinnies, vumeters)
        // and render them for the next swap
        if (!pllInitializing() || m_pllPendingUpdate) {
            emit vsyncSwapAndRender();
            m_semaVsyncSlot.acquire();
            m_pllPendingUpdate = false;
        }

        if (m_sinceLastSwap.toIntegerMicros() > periodMicros * 3 / 2) {
            // Too much time passed since last swap: consider frame dropped.
            // Automatically adjust the time offset between the PLL and our signal,
            // ideally settling on an offset with no or little frame drops.
            const auto sinceLastOffsetAdjust = now - offsetAdjustedAt;
            // Don't adjust too often (max once every 100 ms)
            if (sinceLastOffsetAdjust > 100000) {
                // And don't adjust (immediately) if we have been running
                // without drops for over 1 second
                if (sinceLastOffsetAdjust < 1000000) {
                    offset = (offset + std::llround(pllDeltaOut) / 8) % periodMicros;
                }
                offsetAdjustedAt = now;
            }
            m_droppedFrames++;
            qDebug() << "DROP";
        }
    }
}

void VSyncThread::runTimer() {
    DEBUG_ASSERT(m_vSyncMode == ST_TIMER);

    while (m_bDoRendering) {
        emit vsyncRender(); // renders the new waveform.

        // wait until rendering was scheduled. It might be delayed due a
        // pending swap (depends one driver vSync settings)
        m_semaVsyncSlot.acquire();

        // qDebug() << "ST_TIMER                      " << lastMicros << restMicros;
        int remainingForSwap = m_waitToSwapMicros -
                static_cast<int>(m_timer.elapsed().toIntegerMicros());
        // waiting for interval by sleep
        if (remainingForSwap > 100) {
            usleep(remainingForSwap);
        }

        // swaps the new waveform to front in case of gl-wf
        emit vsyncSwap();

        // wait until swap occurred. It might be delayed due to driver vSync
        // settings.
        m_semaVsyncSlot.acquire();

        // <- Assume we are VSynced here ->
        m_sinceLastSwap = m_timer.restart();
        int lastSwapTime = static_cast<int>(m_sinceLastSwap.toIntegerMicros());
        if (remainingForSwap < 0) {
            // Our swapping call was already delayed
            // The real swap might happens on the following VSync, depending on driver settings
            m_droppedFrames++; // Count as Real Time Error
        }
        // try to stay in right intervals
        m_waitToSwapMicros = m_syncIntervalTimeMicros +
                ((m_waitToSwapMicros - lastSwapTime) % m_syncIntervalTimeMicros);
    }
}

int VSyncThread::elapsed() {
    return static_cast<int>(m_timer.elapsed().toIntegerMicros());
}

void VSyncThread::setSyncIntervalTimeMicros(int syncTime) {
    if (m_vSyncMode != ST_FREE) {
        m_syncIntervalTimeMicros = syncTime;
        m_vSyncPerRendering = static_cast<int>(
                round(m_displayFrameRate * m_syncIntervalTimeMicros / 1000));
    }
}

std::chrono::microseconds VSyncThread::fromTimerToNextSync(const PerformanceTimer& timer) {
    int difference = static_cast<int>(m_timer.difference(timer).toIntegerMicros());
    // int math is fine here, because we do not expect times > 4.2 s
    int toNextSync = difference + m_waitToSwapMicros;
    while (toNextSync < 0) {
        // this function is called during rendering. A negative value indicates
        // an attempt to render an outdated frame. Render the next frame instead
        toNextSync += m_syncIntervalTimeMicros;
    }
    return std::chrono::microseconds(toNextSync);
}

int VSyncThread::droppedFrames() {
    return m_droppedFrames;
}

void VSyncThread::vsyncSlotFinished() {
    m_semaVsyncSlot.release();
}

void VSyncThread::getAvailableVSyncTypes(QList<QPair<int, QString>>* pList) {
    for (int i = (int)VSyncThread::ST_TIMER; i < (int)VSyncThread::ST_COUNT; i++) {
        //if (isAvailable(type))  // TODO
        {
            enum VSyncMode mode = (enum VSyncMode)i;

            QString name;
            switch (mode) {
            case VSyncThread::ST_TIMER:
                name = tr("Timer (Fallback)");
                break;
            case VSyncThread::ST_MESA_VBLANK_MODE_1_DEPRECATED:
                name = tr("MESA vblank_mode = 1");
                break;
            case VSyncThread::ST_SGI_VIDEO_SYNC_DEPRECATED:
                name = tr("Wait for Video sync");
                break;
            case VSyncThread::ST_OML_SYNC_CONTROL_DEPRECATED:
                name = tr("Sync Control");
                break;
            case VSyncThread::ST_FREE:
                name = tr("Free + 1 ms (for benchmark only)");
                break;
            case VSyncThread::ST_PLL:
                name = tr("frameSwapped-signal driven phase locked loop");
                break;
            default:
                break;
            }
            QPair<int, QString > pair = QPair<int, QString >(i, name);
            pList->append(pair);
        }
    }
}

mixxx::Duration VSyncThread::sinceLastSwap() const {
    return m_sinceLastSwap;
}

bool VSyncThread::pllInitializing() const {
    return m_pllInitializing.load();
}

void VSyncThread::markSlotStart() {
    m_lastSlotStartMicros = m_pllTimer.elapsed().toIntegerMicros();
}

void VSyncThread::fallBackToTimer() {
    m_pllFallBackRequested = true;
}

VSyncThread::PllSnapshot VSyncThread::pllSnapshot() {
    PllSnapshot snapshot;
    {
        std::scoped_lock lock(m_pllMutex);
        snapshot.initializing = m_pll.initializing();
        snapshot.periodMicros = m_pll.deltaOut();
        snapshot.lastPhaseErrorMicros = m_pll.lastPhaseError();
        snapshot.stats = m_pll.stats();
    }
    snapshot.fellBack = m_pllFellBack.load();
    snapshot.skippedSlots = m_skippedSlots.load();
    return snapshot;
}

void VSyncThread::updatePLL(bool vblankReferenced) {
    std::scoped_lock lock(m_pllMutex);

    // Phase-lock-looped to estimate the vsync based on the
    // QOpenGLWindow::frameSwapped signal. The math is in
    // mixxx::decklab::VSyncPll: the upstream loop filter plus the Deck Lab
    // idle handling.
    const double pllPhaseIn = m_pllTimer.elapsed().toDoubleMicros();
    const auto result = m_pll.addSample(pllPhaseIn, vblankReferenced);
    m_pllInitializing = m_pll.initializing();
    if (result != mixxx::decklab::VSyncPll::Result::Tracked &&
            result != mixxx::decklab::VSyncPll::Result::Relocked) {
        return;
    }

    if (pllPhaseIn > m_pllLogging) {
        if (m_pllLogging == 0) {
            m_pllLogging = pllPhaseIn;
        } else {
            qDebug() << "phase-locked-loop:" << std::llround(m_pll.phaseOut())
                     << m_pll.deltaOut() << m_pll.lastPhaseError();
        }
        // log every 10 seconds
        m_pllLogging += 10000000.0;
    }

    m_pllPendingUpdate = true;
}
