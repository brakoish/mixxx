// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabscreensleep.h"

#include "moc_decklabscreensleep.cpp"

namespace {

const ConfigKey kMinutesKey(QStringLiteral("[DeckLab]"), QStringLiteral("ScreenSleepMinutes"));

} // namespace

DeckLabControllerActivity* DeckLabControllerActivity::instance() {
    static auto* relay = new DeckLabControllerActivity();
    return relay;
}

QList<int> DeckLabScreenSleep::choices() {
    return {5, 10, 15, 30, 60};
}

int DeckLabScreenSleep::savedMinutes(const UserSettingsPointer& config) {
    const int minutes = config ? config->getValue(kMinutesKey, kDefaultMinutes) : kDefaultMinutes;
    return choices().contains(minutes) ? minutes : kDefaultMinutes;
}

void DeckLabScreenSleep::saveMinutes(const UserSettingsPointer& config, int minutes) {
    if (!config || !choices().contains(minutes)) {
        return;
    }
    config->setValue(kMinutesKey, minutes);
    // Units are often switched off at the wall: keep the choice now.
    config->save();
}

void DeckLabScreenSleep::controllerUsed() {
    emit DeckLabControllerActivity::instance()->used();
}

DeckLabScreenSleep::DeckLabScreenSleep(Send send, QObject* parent)
        : QObject(parent),
          m_send(std::move(send)) {
    connect(DeckLabControllerActivity::instance(),
            &DeckLabControllerActivity::used,
            this,
            &DeckLabScreenSleep::onControllerUsed,
            Qt::QueuedConnection);
}

void DeckLabScreenSleep::setReady(bool ready) {
    m_ready = ready;
    m_lastActivity.invalidate();
    if (ready) {
        sendPolicy(QJsonObject{{QStringLiteral("after"), m_minutes * 60}, {QStringLiteral("busy"), m_busy}});
    }
}

void DeckLabScreenSleep::setMinutes(int minutes) {
    if (!choices().contains(minutes) || minutes == m_minutes) {
        return;
    }
    m_minutes = minutes;
    sendPolicy(QJsonObject{{QStringLiteral("after"), m_minutes * 60}});
}

void DeckLabScreenSleep::setBusy(bool busy) {
    if (busy == m_busy) {
        return;
    }
    m_busy = busy;
    sendPolicy(QJsonObject{{QStringLiteral("busy"), m_busy}});
}

void DeckLabScreenSleep::onControllerUsed() {
    // While something plays the screen never sleeps: nothing to tell.
    if (!m_ready || m_busy) {
        return;
    }
    if (m_lastActivity.isValid() && m_lastActivity.elapsed() < kActivityIntervalMs) {
        return;
    }
    m_lastActivity.start();
    m_send(QStringLiteral("display.activity"), QJsonObject());
}

void DeckLabScreenSleep::sendPolicy(const QJsonObject& params) {
    if (m_ready) {
        m_send(QStringLiteral("display.sleep"), params);
    }
}
