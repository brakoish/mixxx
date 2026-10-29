// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <functional>

#include "preferences/usersettings.h"

/// Screen sleep (decklabd protocol v1.3, decklabd/screensleep.py in the
/// appliance repo). The unit is idle most of the time, so the touchscreen's
/// backlight goes off after Settings → Display → Screen sleep
/// ([DeckLab] ScreenSleepMinutes) without a touch or the controller, and
/// only while nothing plays or records.
///
/// decklabd runs the timer, switches the backlight and holds the
/// touchscreen while it is dark, so the touch that wakes it presses
/// nothing; it sees every touch itself, on Get Music and Sample Maker too.
/// Deck Lab tells it what it can't see, over its own connection:
///
/// * `display.sleep {"after": seconds, "busy": bool}` when decklabd
///   connects, the setting changes or busy (a deck, sampler, the preview or
///   a recording runs) starts or stops. Busy keeps the screen on and wakes
///   it; the wait starts over when it stops. The policy ends, and the screen
///   wakes, when the connection closes.
/// * `display.activity` when a controller is used (Controller::
///   triggerActivity, any thread). A control that wakes the screen still
///   does its job.
///
/// A decklabd without screen sleep answers unknown_method: the screen stays
/// on, as before.
class DeckLabScreenSleep : public QObject {
    Q_OBJECT
  public:
    using Send = std::function<void(const QString& method, const QJsonObject& params)>;

    static constexpr int kDefaultMinutes = 15;
    /// Controller use is passed on at most this often. decklabd sleeps only
    /// after minutes without use, so the screen is still lit for one that
    /// comes sooner, and the first one after this wakes it.
    static constexpr int kActivityIntervalMs = 5000;

    /// 5, 10, 15, 30 and 60 minutes (the owner's choice, 2026-10-29).
    static QList<int> choices();
    /// The saved wait, one of choices(); the default when unset or invalid.
    static int savedMinutes(const UserSettingsPointer& config);
    static void saveMinutes(const UserSettingsPointer& config, int minutes);
    /// Any thread: a controller was used. Called by Controller::
    /// triggerActivity at most once a second per controller.
    static void controllerUsed();

    explicit DeckLabScreenSleep(Send send, QObject* parent = nullptr);

    /// decklabd connected (the policy goes again) or went away.
    void setReady(bool ready);
    void setMinutes(int minutes);
    int minutes() const {
        return m_minutes;
    }
    void setBusy(bool busy);
    bool busy() const {
        return m_busy;
    }
    /// GUI thread; controllerUsed() arrives here.
    void onControllerUsed();

  private:
    void sendPolicy(const QJsonObject& params);

    Send m_send;
    bool m_ready = false;
    bool m_busy = false;
    int m_minutes = kDefaultMinutes;
    QElapsedTimer m_lastActivity;
};

/// Carries DeckLabScreenSleep::controllerUsed() from the controller threads
/// to the GUI thread (queued). One instance, never destroyed: a controller
/// may still report while Mixxx shuts down.
class DeckLabControllerActivity : public QObject {
    Q_OBJECT
  public:
    static DeckLabControllerActivity* instance();

  signals:
    void used();
};
