#pragma once

#include <QPair>
#include <QSemaphore>
#include <QThread>
#include <atomic>
#include <mutex>

#include "util/performancetimer.h"
#include "waveform/isynctimeprovider.h"
#include "waveform/vsyncpll.h"

class WGLWidget;

class VSyncThread : public QThread, public VSyncTimeProvider {
    Q_OBJECT
  public:
    enum VSyncMode {
        ST_DEFAULT = 0,
        ST_MESA_VBLANK_MODE_1_DEPRECATED, // 1
        ST_SGI_VIDEO_SYNC_DEPRECATED,     // 2
        ST_OML_SYNC_CONTROL_DEPRECATED,   // 3
        ST_FREE,                          // 4
        ST_PLL,                           // 5
        ST_TIMER,                         // 6
        ST_COUNT                          // Dummy Type at last, counting possible types
    };

    static_assert(mixxx::decklab::kVSyncModePll == ST_PLL);
    static_assert(mixxx::decklab::kVSyncModeTimer == ST_TIMER);

    /// ST_PLL loop state for the Deck Lab perf log.
    struct PllSnapshot {
        bool initializing = true;
        bool fellBack = false;
        double periodMicros = 0.0;
        double lastPhaseErrorMicros = 0.0;
        mixxx::decklab::VSyncPll::Stats stats;
        quint64 skippedSlots = 0;
    };

    VSyncThread(QObject* pParent, VSyncMode vSyncMode);
    ~VSyncThread();

    void run() override;

    bool waitForVideoSync(WGLWidget* glw);
    int elapsed();
    void setSyncIntervalTimeMicros(int usSyncTimer);
    int droppedFrames();
    void setSwapWait(int sw);
    // VSyncTimerProvider
    std::chrono::microseconds fromTimerToNextSync(const PerformanceTimer& timer) override;
    void vsyncSlotFinished();
    void getAvailableVSyncTypes(QList<QPair<int, QString>>* list);
    void setupSync(WGLWidget* glw, int index);
    void waitUntilSwap(WGLWidget* glw);
    mixxx::Duration sinceLastSwap() const;
    // VSyncTimerProvider
    std::chrono::microseconds getSyncInterval() const override {
        return std::chrono::microseconds(m_syncIntervalTimeMicros);
    }
    /// After a frameSwapped of the shared GL window (GUI thread).
    /// vblankReferenced: the paint waited for a vblank (Deck Lab, see
    /// waveform/vsyncpll.h). Other samples only count during init.
    void updatePLL(bool vblankReferenced = true);
    bool pllInitializing() const;
    /// Start of a vsyncSwapAndRender slot on the GUI thread (ST_PLL).
    void markSlotStart();
    /// Leaves the PLL loop for the timer loop (startup safety, GUI thread).
    /// The swap interval stays as the GL contexts were created.
    void fallBackToTimer();
    PllSnapshot pllSnapshot();
    VSyncMode vsyncMode() const {
        return m_vSyncMode.load();
    }
  signals:
    void vsyncSwapAndRender();
    void vsyncRender();
    void vsyncSwap();
  private:
    void runFree();
    void runPLL();
    void runTimer();

    bool m_bDoRendering;
    int m_syncIntervalTimeMicros;
    int m_waitToSwapMicros;
    std::atomic<VSyncMode> m_vSyncMode;
    bool m_syncOk;
    int m_droppedFrames;
    int m_swapWait;
    PerformanceTimer m_timer;
    QSemaphore m_semaVsyncSlot;
    double m_displayFrameRate;
    int m_vSyncPerRendering;
    mixxx::Duration m_sinceLastSwap;
    // phase locked loop
    std::mutex m_pllMutex;
    PerformanceTimer m_pllTimer;
    mixxx::decklab::VSyncPll m_pll;          // guarded by m_pllMutex
    std::atomic<bool> m_pllInitializing;
    std::atomic<bool> m_pllPendingUpdate;
    std::atomic<bool> m_pllFallBackRequested;
    std::atomic<bool> m_pllFellBack;
    std::atomic<qint64> m_lastSlotStartMicros; // m_pllTimer, -1 before the first slot
    std::atomic<quint64> m_skippedSlots;
    double m_pllLogging;
};
