#pragma once

#include <QByteArray>
#include <QString>
#include <vector>

/// Deck Lab appliance thread placement, driven by environment variables that
/// the kiosk unit (decklab-mixxx.service) sets. All are optional; when unset
/// Mixxx keeps upstream behaviour.
///
///   DECKLAB_AUDIO_CPU=<n>     pin the real-time audio callback thread that
///                             drives the engine to CPU n (Linux only).
///   DECKLAB_AUDIO_CPU=last    ... to the highest CPU this process may use
///                             (CPU 3 on a Pi 4 or Pi 5). Values that are not
///                             usable are ignored with a warning.
///   DECKLAB_AUDIO_PRIO=<1-95> raise that thread to SCHED_FIFO at this
///                             priority (never lowers it). The appliance uses
///                             ~70: above the generic threaded IRQ handlers
///                             (50), below the USB host controller IRQ thread.
///   DECKLAB_ANALYZER_IDLE=1   run analyzer worker threads at SCHED_IDLE,
///                             falling back to nice 19.
namespace mixxx {
namespace decklab {

/// Resolves a DECKLAB_AUDIO_CPU value against the CPUs this process may run
/// on (ascending or not): "last" selects the highest usable CPU, a plain
/// non-negative decimal number selects that CPU if it is usable. Returns -1
/// for an empty, malformed or unusable value.
int resolveAudioCpu(const QByteArray& value, const std::vector<int>& cpus);

/// CPUs the calling thread may run on: its affinity mask on Linux (online
/// CPUs allowed by cpusets), else 0 .. idealThreadCount() - 1.
std::vector<int> usableCpus();

/// DECKLAB_AUDIO_CPU resolved against usableCpus(), or -1 (with a warning
/// when set but unusable). Reads the environment: call from a non-real-time
/// thread.
int audioCpuFromEnvironment();

/// Pins the calling thread to one CPU with pthread_setaffinity_np.
/// One syscall; returns false (and an error text) where unsupported.
bool pinCurrentThreadToCpu(int cpu, QString* pError);

/// Highest SCHED_FIFO priority DECKLAB_AUDIO_PRIO may request.
constexpr int kMaxAudioPriority = 95;

/// Returns the priority in [1, kMaxAudioPriority] or -1 for an empty,
/// malformed or out-of-range value.
int parseAudioPriority(const QByteArray& value);

/// DECKLAB_AUDIO_PRIO validated, or -1 (with a warning when set but
/// invalid). Reads the environment: call from a non-real-time thread.
int audioPriorityFromEnvironment();

/// Raises the calling thread to SCHED_FIFO at priority. A thread that
/// already runs SCHED_FIFO/SCHED_RR at that priority or higher is left
/// alone. Returns false with an error text (e.g. EPERM without an rtprio
/// limit) and leaves the thread unchanged on failure. pPriority receives the
/// resulting real-time priority (0 for a non-real-time thread).
bool raiseCurrentThreadToFifo(int priority, QString* pError, int* pPriority);

/// True when DECKLAB_ANALYZER_IDLE is set to a non-zero integer.
bool analyzerIdleRequested();

/// Applies SCHED_IDLE (or nice 19 when that is refused) to the calling
/// thread if analyzerIdleRequested(). Logs the outcome once per thread.
void applyAnalyzerThreadPolicy();

} // namespace decklab
} // namespace mixxx
