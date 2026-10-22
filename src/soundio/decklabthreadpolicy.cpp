#include "soundio/decklabthreadpolicy.h"

#include <QThread>
#include <QtDebug>

#ifdef __LINUX__
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace mixxx {
namespace decklab {

int parseCpuIndex(const QByteArray& value, int cpuCount) {
    const QByteArray trimmed = value.trimmed();
    if (trimmed.isEmpty() || cpuCount <= 0) {
        return -1;
    }
    for (const char c : trimmed) {
        if (c < '0' || c > '9') {
            return -1;
        }
    }
    bool ok = false;
    const int cpu = trimmed.toInt(&ok, 10);
    if (!ok || cpu < 0 || cpu >= cpuCount) {
        return -1;
    }
    return cpu;
}

int audioCpuFromEnvironment() {
    if (!qEnvironmentVariableIsSet("DECKLAB_AUDIO_CPU")) {
        return -1;
    }
    const QByteArray value = qgetenv("DECKLAB_AUDIO_CPU");
#ifdef __LINUX__
    const long configured = sysconf(_SC_NPROCESSORS_CONF);
    const int cpuCount = configured > 0 ? static_cast<int>(configured) : QThread::idealThreadCount();
#else
    const int cpuCount = QThread::idealThreadCount();
#endif
    const int cpu = parseCpuIndex(value, cpuCount);
    if (cpu < 0) {
        qWarning() << "DECKLAB_AUDIO_CPU ignored: expected a CPU index below"
                   << cpuCount << "but got" << value;
    }
    return cpu;
}

bool pinCurrentThreadToCpu(int cpu, QString* pError) {
#ifdef __LINUX__
    if (cpu < 0 || cpu >= CPU_SETSIZE) {
        if (pError) {
            *pError = QStringLiteral("invalid CPU index");
        }
        return false;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    const int result = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (result != 0) {
        if (pError) {
            *pError = QString::fromLocal8Bit(strerror(result));
        }
        return false;
    }
    return true;
#else
    Q_UNUSED(cpu);
    if (pError) {
        *pError = QStringLiteral("not supported on this platform");
    }
    return false;
#endif
}

bool analyzerIdleRequested() {
    bool ok = false;
    const int value = qEnvironmentVariableIntValue("DECKLAB_ANALYZER_IDLE", &ok);
    return ok && value != 0;
}

void applyAnalyzerThreadPolicy() {
    if (!analyzerIdleRequested()) {
        return;
    }
#ifdef __LINUX__
    // Linux scheduling attributes are per thread. SCHED_IDLE threads only run
    // when no SCHED_OTHER/FIFO work wants the CPU, so track analysis can never
    // delay the GUI or the audio callback. An unprivileged thread may always
    // lower itself to SCHED_IDLE; if a sandbox refuses it, nice 19 is the
    // closest fallback.
    sched_param param{};
    param.sched_priority = 0;
    const int result = pthread_setschedparam(pthread_self(), SCHED_IDLE, &param);
    if (result == 0) {
        qInfo() << "DECKLAB_ANALYZER_IDLE: analyzer thread"
                << QThread::currentThread()->objectName() << "runs at SCHED_IDLE";
        return;
    }
    const auto tid = static_cast<id_t>(syscall(SYS_gettid));
    if (setpriority(PRIO_PROCESS, tid, 19) == 0) {
        qInfo() << "DECKLAB_ANALYZER_IDLE: SCHED_IDLE refused ("
                << strerror(result) << "), analyzer thread"
                << QThread::currentThread()->objectName() << "runs at nice 19";
        return;
    }
    qWarning() << "DECKLAB_ANALYZER_IDLE: could not lower analyzer thread priority:"
               << strerror(errno);
#else
    QThread::currentThread()->setPriority(QThread::IdlePriority);
#endif
}

} // namespace decklab
} // namespace mixxx
