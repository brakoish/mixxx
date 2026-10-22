#include "soundio/decklabthreadpolicy.h"

#include <QStringList>
#include <QThread>
#include <QtDebug>
#include <algorithm>

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

int resolveAudioCpu(const QByteArray& value, const std::vector<int>& cpus) {
    const QByteArray trimmed = value.trimmed();
    if (trimmed.isEmpty() || cpus.empty()) {
        return -1;
    }
    if (trimmed == "last") {
        int last = -1;
        for (const int cpu : cpus) {
            last = std::max(last, cpu);
        }
        return last;
    }
    for (const char c : trimmed) {
        if (c < '0' || c > '9') {
            return -1;
        }
    }
    bool ok = false;
    const int cpu = trimmed.toInt(&ok, 10);
    if (!ok) {
        return -1;
    }
    for (const int usable : cpus) {
        if (usable == cpu) {
            return cpu;
        }
    }
    return -1;
}

std::vector<int> usableCpus() {
    std::vector<int> cpus;
#ifdef __LINUX__
    cpu_set_t set;
    CPU_ZERO(&set);
    if (pthread_getaffinity_np(pthread_self(), sizeof(set), &set) == 0) {
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (CPU_ISSET(cpu, &set)) {
                cpus.push_back(cpu);
            }
        }
    }
    if (!cpus.empty()) {
        return cpus;
    }
#endif
    const int count = std::max(1, QThread::idealThreadCount());
    for (int cpu = 0; cpu < count; ++cpu) {
        cpus.push_back(cpu);
    }
    return cpus;
}

int audioCpuFromEnvironment() {
    if (!qEnvironmentVariableIsSet("DECKLAB_AUDIO_CPU")) {
        return -1;
    }
    const QByteArray value = qgetenv("DECKLAB_AUDIO_CPU");
    const std::vector<int> cpus = usableCpus();
    const int cpu = resolveAudioCpu(value, cpus);
    if (cpu < 0) {
        QStringList usable;
        for (const int each : cpus) {
            usable << QString::number(each);
        }
        qWarning() << "DECKLAB_AUDIO_CPU ignored:" << value
                   << "is not \"last\" or one of the usable CPUs"
                   << usable.join(QLatin1Char(','));
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
