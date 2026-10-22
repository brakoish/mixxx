// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabsignals.h"

#include <QSocketNotifier>
#include <QTimer>
#include <QtDebug>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

#include "moc_decklabsignals.cpp"
#include "util/cmdlineargs.h"

namespace {
#ifdef Q_OS_UNIX
int s_pipe[2] = {-1, -1};
volatile sig_atomic_t s_requested = 0;

void onTerminationSignal(int signalNumber) {
    // Async-signal-safe only: a flag and one byte into the pipe.
    s_requested = 1;
    const unsigned char byte = static_cast<unsigned char>(signalNumber);
    const ssize_t written = ::write(s_pipe[1], &byte, 1);
    (void)written;
}
#endif
DeckLabTerminationSignals* s_instance = nullptr;
} // namespace

DeckLabTerminationSignals::DeckLabTerminationSignals(QObject* parent)
        : QObject(parent) {
}

DeckLabTerminationSignals* DeckLabTerminationSignals::install(QObject* parent) {
    if (s_instance) {
        return s_instance;
    }
#ifdef Q_OS_UNIX
    if (::pipe(s_pipe) != 0) {
        qWarning() << "Deck Lab: no termination pipe; SIGTERM keeps its default action";
        return nullptr;
    }
    for (int fd : s_pipe) {
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    s_instance = new DeckLabTerminationSignals(parent);
    s_instance->m_notifier = new QSocketNotifier(s_pipe[0], QSocketNotifier::Read, s_instance);
    connect(s_instance->m_notifier,
            &QSocketNotifier::activated,
            s_instance,
            &DeckLabTerminationSignals::readPipe);
    struct sigaction action = {};
    action.sa_handler = onTerminationSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &action, nullptr);
    // --debug-assert-break raises SIGINT for a debugger; leave that alone.
    if (!CmdlineArgs::Instance().getDebugAssertBreak()) {
        sigaction(SIGINT, &action, nullptr);
    }
    connect(s_instance, &QObject::destroyed, [] { s_instance = nullptr; });
    return s_instance;
#else
    Q_UNUSED(parent);
    return nullptr;
#endif
}

bool DeckLabTerminationSignals::requested() {
#ifdef Q_OS_UNIX
    return s_requested != 0;
#else
    return false;
#endif
}

void DeckLabTerminationSignals::arm() {
    m_armed = true;
    if (m_pendingSignal) {
        const int signalNumber = m_pendingSignal;
        m_pendingSignal = 0;
        QTimer::singleShot(0, this, [this, signalNumber] { deliver(signalNumber); });
    }
}

void DeckLabTerminationSignals::readPipe() {
#ifdef Q_OS_UNIX
    unsigned char buffer[16];
    int last = 0;
    ssize_t count = 0;
    while ((count = ::read(s_pipe[0], buffer, sizeof(buffer))) > 0) {
        last = buffer[count - 1];
    }
    if (!last) {
        return;
    }
    if (!m_armed) {
        m_pendingSignal = last;
        qInfo() << "Deck Lab: termination signal" << last << "received during startup";
        return;
    }
    deliver(last);
#endif
}

void DeckLabTerminationSignals::deliver(int signalNumber) {
    if (m_delivered) {
        return;
    }
    m_delivered = true;
#ifdef Q_OS_UNIX
    // A second signal keeps its default action, e.g. Ctrl+C twice.
    struct sigaction fallback = {};
    fallback.sa_handler = SIG_DFL;
    sigemptyset(&fallback.sa_mask);
    sigaction(SIGTERM, &fallback, nullptr);
    sigaction(SIGINT, &fallback, nullptr);
#endif
    qInfo() << "Deck Lab: closing gracefully after signal" << signalNumber;
    emit terminationRequested(signalNumber);
}
