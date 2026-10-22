#pragma once

#include <QByteArray>
#include <QString>

/// Deck Lab appliance thread placement, driven by environment variables that
/// the kiosk unit (decklab-mixxx.service) sets. All are optional; when unset
/// Mixxx keeps upstream behaviour.
///
///   DECKLAB_AUDIO_CPU=<n>     pin the real-time audio callback thread that
///                             drives the engine to CPU n (Linux only).
///   DECKLAB_ANALYZER_IDLE=1   run analyzer worker threads at SCHED_IDLE,
///                             falling back to nice 19.
namespace mixxx {
namespace decklab {

/// Returns the CPU index in [0, cpuCount) or -1 when value is empty or not a
/// plain non-negative decimal number below cpuCount.
int parseCpuIndex(const QByteArray& value, int cpuCount);

/// DECKLAB_AUDIO_CPU validated against the configured CPUs, or -1.
/// Reads the environment: call from a non-real-time thread.
int audioCpuFromEnvironment();

/// Pins the calling thread to one CPU with pthread_setaffinity_np.
/// One syscall; returns false (and an error text) where unsupported.
bool pinCurrentThreadToCpu(int cpu, QString* pError);

/// True when DECKLAB_ANALYZER_IDLE is set to a non-zero integer.
bool analyzerIdleRequested();

/// Applies SCHED_IDLE (or nice 19 when that is refused) to the calling
/// thread if analyzerIdleRequested(). Logs the outcome once per thread.
void applyAnalyzerThreadPolicy();

} // namespace decklab
} // namespace mixxx
