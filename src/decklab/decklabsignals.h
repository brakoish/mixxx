// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QObject>

class QSocketNotifier;

/// SIGTERM/SIGINT become a Qt signal (self-pipe + QSocketNotifier), so
/// `systemctl --user stop`, decklabd's session.restart_mixxx and OS shutdown
/// run Mixxx's normal graceful close path without a confirmation dialog.
class DeckLabTerminationSignals : public QObject {
    Q_OBJECT
  public:
    /// Installs the handlers once per process. Returns nullptr where POSIX
    /// signals are unavailable.
    static DeckLabTerminationSignals* install(QObject* parent);
    /// True once a termination signal arrived; skips exit confirmations.
    static bool requested();
    /// Call when the event loop is about to run. A signal that arrived during
    /// startup is delivered then.
    void arm();

  signals:
    void terminationRequested(int signalNumber);

  private:
    explicit DeckLabTerminationSignals(QObject* parent);
    void readPipe();
    void deliver(int signalNumber);

    QSocketNotifier* m_notifier = nullptr;
    bool m_armed = false;
    bool m_delivered = false;
    int m_pendingSignal = 0;
};
