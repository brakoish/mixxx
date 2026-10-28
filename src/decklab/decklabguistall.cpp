// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabguistall.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QTimer>
#include <QtDebug>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

#if defined(__linux__) && defined(__GLIBC__)
#include <cerrno>
#include <cstring>
#include <ctime>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#define DECKLAB_GUI_STALL_WATCH 1
#endif

namespace {

std::atomic<int> s_reports{0};

#ifdef DECKLAB_GUI_STALL_WATCH
constexpr int kBeatMs = 250;
constexpr qint64 kMaxReportBytes = 256 * 1024;
constexpr int kStackFrames = 64;

std::atomic<qint64> s_lastBeatMs{0};
std::atomic<bool> s_armed{false}; // the event loop has run since start()
int s_fd = -1;
pthread_t s_guiThread;
QThread* s_watch = nullptr;
QTimer* s_beat = nullptr;
QString s_path;
std::mutex s_mutex;
std::condition_variable s_wake;
bool s_stopping = false;

qint64 nowMs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<qint64>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

void writeText(const char* text) {
    std::size_t left = std::strlen(text);
    while (left > 0) {
        const ssize_t written = ::write(s_fd, text, left);
        if (written <= 0) {
            return;
        }
        text += written;
        left -= static_cast<std::size_t>(written);
    }
}

void onStallSignal(int) {
    // On the GUI thread, which waits somewhere: async-signal-safe calls
    // only. backtrace() loaded its unwinder in start().
    const int savedErrno = errno;
    if (s_fd >= 0) {
        void* frames[kStackFrames];
        const int count = backtrace(frames, kStackFrames);
        writeText("Deck Lab: GUI thread stack\n");
        backtrace_symbols_fd(frames, count, s_fd);
        writeText("\n");
    }
    errno = savedErrno;
}

void watch(int stallMs) {
    const auto step = std::chrono::milliseconds(std::clamp(stallMs / 4, 50, 1000));
    std::unique_lock lock(s_mutex);
    bool reported = false;
    while (!s_stopping) {
        s_wake.wait_for(lock, step);
        if (s_stopping || !s_armed.load()) {
            continue;
        }
        const qint64 stalled = nowMs() - s_lastBeatMs.load();
        if (stalled < stallMs) {
            reported = false;
            continue;
        }
        if (reported) {
            continue; // once per stall
        }
        reported = true;
        const off_t before = ::lseek(s_fd, 0, SEEK_END);
        if (before < 0 || ::pthread_kill(s_guiThread, SIGUSR2) != 0) {
            continue;
        }
        // The handler runs as soon as the GUI thread is scheduled.
        s_wake.wait_for(lock, std::chrono::milliseconds(300));
        QByteArray stack;
        QFile report(s_path);
        if (report.open(QIODevice::ReadOnly) && report.seek(before)) {
            stack = report.read(kMaxReportBytes);
        }
        qWarning().noquote() << "Deck Lab: the GUI thread has not run for" << stalled << "ms;"
                             << QString::fromUtf8(stack).trimmed();
        s_reports.fetch_add(1);
    }
}
#endif

} // namespace

bool DeckLabGuiStallWatch::start(const QString& reportPath, int stallMs) {
#ifdef DECKLAB_GUI_STALL_WATCH
    if (s_watch || stallMs <= 0 || reportPath.isEmpty() || !QCoreApplication::instance()) {
        return false;
    }
    // A long report from an earlier run starts over.
    const int flags = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC |
            (QFileInfo(reportPath).size() > kMaxReportBytes ? O_TRUNC : 0);
    s_fd = ::open(QFile::encodeName(reportPath).constData(), flags, 0644);
    if (s_fd < 0) {
        qWarning() << "Deck Lab: no GUI stall report file" << reportPath;
        return false;
    }
    s_path = reportPath;
    // backtrace() loads its unwinder on first use, which is not safe in a
    // signal handler: load it now.
    void* frame[1];
    backtrace(frame, 1);
    struct sigaction action = {};
    action.sa_handler = onStallSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGUSR2, &action, nullptr);
    s_guiThread = ::pthread_self();
    s_lastBeatMs.store(nowMs());
    s_armed.store(false);
    s_reports.store(0);
    s_beat = new QTimer(QCoreApplication::instance());
    s_beat->setInterval(kBeatMs);
    QObject::connect(s_beat, &QTimer::timeout, [] {
        s_lastBeatMs.store(nowMs());
        s_armed.store(true);
    });
    s_beat->start();
    {
        const std::lock_guard lock(s_mutex);
        s_stopping = false;
    }
    s_watch = QThread::create(watch, stallMs);
    s_watch->setObjectName(QStringLiteral("GuiStallWatch"));
    s_watch->start(QThread::LowPriority);
    return true;
#else
    Q_UNUSED(reportPath);
    Q_UNUSED(stallMs);
    return false;
#endif
}

void DeckLabGuiStallWatch::stop() {
#ifdef DECKLAB_GUI_STALL_WATCH
    if (!s_watch) {
        return;
    }
    {
        const std::lock_guard lock(s_mutex);
        s_stopping = true;
    }
    s_wake.notify_all();
    s_watch->wait();
    delete s_watch;
    s_watch = nullptr;
    delete s_beat;
    s_beat = nullptr;
    struct sigaction action = {};
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR2, &action, nullptr);
    ::close(s_fd);
    s_fd = -1;
    s_armed.store(false);
#endif
}

bool DeckLabGuiStallWatch::running() {
#ifdef DECKLAB_GUI_STALL_WATCH
    return s_watch != nullptr;
#else
    return false;
#endif
}

int DeckLabGuiStallWatch::reportsForTesting() {
    return s_reports.load();
}
