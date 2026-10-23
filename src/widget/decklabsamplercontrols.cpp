// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabsamplercontrols.h"

#include <QFileInfo>
#include <algorithm>
#include <cmath>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "mixer/basetrackplayer.h"
#include "mixer/playermanager.h"
#include "track/cue.h"
#include "track/track.h"
#include "util/fpclassify.h"

#include "moc_decklabsamplercontrols.cpp"

namespace {
const QString kSettingsGroup = QStringLiteral("[DeckLabSampler]");

ConfigKey modeKey(int sampler) {
    return ConfigKey(kSettingsGroup, QStringLiteral("pad_%1_mode").arg(sampler));
}

void pulse(ControlProxy* control) {
    control->set(1);
    control->set(0);
}

double toDb(double gain) {
    return util_isfinite(gain) && gain > 0 ? 20 * std::log10(gain) : -99.0;
}
} // namespace

struct DeckLabSamplerControls::Slot {
    QString group;
    ControlProxy* loaded = nullptr;
    ControlProxy* play = nullptr;
    ControlProxy* position = nullptr;
    ControlProxy* duration = nullptr;
    ControlProxy* rate = nullptr;
    ControlProxy* bpm = nullptr;
    ControlProxy* pregain = nullptr;
    ControlProxy* volume = nullptr;
    ControlProxy* repeat = nullptr;
    ControlProxy* quantize = nullptr;
    ControlProxy* sync = nullptr;
    ControlProxy* cuePoint = nullptr;
    ControlProxy* stutter = nullptr;
    ControlProxy* cueStop = nullptr;
    ControlProxy* startPlay = nullptr;
    ControlProxy* startStop = nullptr;
    ControlProxy* eject = nullptr;
    QString title;
    Mode mode = Mode::OneShot;
    bool wasLoaded = false;
    QMetaObject::Connection trackLoaded;
    std::vector<ControlProxy*> proxies;
};

DeckLabSamplerControls::DeckLabSamplerControls(UserSettingsPointer config,
        PlayerManagerInterface* players,
        QObject* parent)
        : QObject(parent),
          m_config(std::move(config)),
          m_players(players),
          m_count(new ControlProxy(QStringLiteral("[App]"),
                  QStringLiteral("num_samplers"),
                  this,
                  ControlFlag::NoWarnIfMissing)) {
    m_syncLoops = m_config && m_config->getValue(ConfigKey(kSettingsGroup, QStringLiteral("sync_loops")), 0) > 0;
    m_count->connectValueChanged(
            this,
            [this](double) {
                ensureSlots();
                notify(0);
            },
            Qt::QueuedConnection);
    ensureSlots();
}

DeckLabSamplerControls::~DeckLabSamplerControls() {
    for (const auto& slot : m_slots) {
        if (slot) {
            disconnect(slot->trackLoaded);
        }
    }
}

int DeckLabSamplerControls::count() const {
    const double value = m_count->get();
    return util_isfinite(value) && value > 0 ? static_cast<int>(value) : 0;
}

int DeckLabSamplerControls::available() const {
    return std::max(DeckLabSampler::kPads, count());
}

DeckLabSamplerControls::Slot* DeckLabSamplerControls::slot(int sampler) const {
    return sampler >= 1 && sampler <= static_cast<int>(m_slots.size()) ? m_slots[sampler - 1].get() : nullptr;
}

void DeckLabSamplerControls::ensureSlots() {
    // Samplers Mixxx adds later get controls then; a slot bound before its
    // sampler existed is bound again once it does.
    const int want = available();
    if (static_cast<int>(m_slots.size()) < want) {
        m_slots.resize(want);
    }
    for (int sampler = 1; sampler <= want; ++sampler) {
        const Slot* existing = slot(sampler);
        if (existing && existing->loaded->valid()) {
            continue;
        }
        if (existing &&
                !ControlObject::exists(ConfigKey(existing->group, QStringLiteral("track_loaded")))) {
            continue;
        }
        createSlot(sampler);
    }
}

void DeckLabSamplerControls::createSlot(int sampler) {
    auto replaced = std::move(m_slots[sampler - 1]);
    if (replaced) {
        disconnect(replaced->trackLoaded);
        for (auto* proxy : replaced->proxies) {
            proxy->deleteLater();
        }
    }
    auto next = std::make_unique<Slot>();
    Slot& s = *next;
    s.group = QStringLiteral("[Sampler%1]").arg(sampler);
    const auto bind = [this, &s](const char* item) {
        auto* proxy = new ControlProxy(s.group, QString::fromLatin1(item), this, ControlFlag::NoWarnIfMissing);
        s.proxies.push_back(proxy);
        return proxy;
    };
    s.loaded = bind("track_loaded");
    s.play = bind("play");
    s.position = bind("playposition");
    s.duration = bind("duration");
    s.rate = bind("rate_ratio");
    s.bpm = bind("bpm");
    s.pregain = bind("pregain");
    s.volume = bind("volume");
    s.repeat = bind("repeat");
    s.quantize = bind("quantize");
    s.sync = bind("sync_enabled");
    s.cuePoint = bind("cue_point");
    s.stutter = bind("play_stutter");
    s.cueStop = bind("cue_gotoandstop");
    s.startPlay = bind("start_play");
    s.startStop = bind("start_stop");
    s.eject = bind("eject");
    const int stored = m_config ? m_config->getValue(modeKey(sampler), 0) : 0;
    s.mode = static_cast<Mode>(std::clamp(stored, 0, 2));
    s.wasLoaded = s.loaded->get() > 0;
    m_slots[sampler - 1] = std::move(next);
    // A remembered LOOP pad repeats again (repeat is not saved by Mixxx).
    if (s.mode == Mode::Loop) {
        s.repeat->set(1);
        if (m_syncLoops) {
            s.sync->set(1);
        }
    }
    s.loaded->connectValueChanged(
            this, [this, sampler](double) { loadedChanged(sampler, false); }, Qt::QueuedConnection);
    s.play->connectValueChanged(
            this, [this, sampler](double) { notify(sampler); }, Qt::QueuedConnection);
    s.pregain->connectValueChanged(
            this, [this, sampler](double) { notify(sampler); }, Qt::QueuedConnection);
    if (m_players) {
        if (auto* player = m_players->getPlayer(s.group)) {
            s.trackLoaded = connect(player, &BaseTrackPlayer::newTrackLoaded, this, [this, sampler](TrackPointer) {
                loadedChanged(sampler, true);
            });
        }
    }
    refreshTitle(sampler);
}

void DeckLabSamplerControls::refreshTitle(int sampler) {
    Slot* s = slot(sampler);
    if (!s) {
        return;
    }
    QString title;
    auto* player = m_players ? m_players->getPlayer(s->group) : nullptr;
    if (const TrackPointer track = player ? player->getLoadedTrack() : TrackPointer()) {
        title = track->getTitle().trimmed();
        if (title.isEmpty()) {
            title = QFileInfo(track->getLocation()).completeBaseName();
        }
    }
    s->title = title;
}

void DeckLabSamplerControls::loadedChanged(int sampler, bool newTrack) {
    Slot* s = slot(sampler);
    if (!s) {
        return;
    }
    refreshTitle(sampler);
    const bool now = s->loaded->get() > 0;
    const bool arrived = now && !s->wasLoaded;
    s->wasLoaded = now;
    if (now && s->mode == Mode::Loop) {
        s->repeat->set(1);
        if (m_syncLoops) {
            s->sync->set(1);
        }
    }
    notify(sampler);
    // Browse returns to the page that asked for this load. A new track over
    // a loaded one only shows as newTrackLoaded.
    if (now && (arrived || newTrack)) {
        emit sampleLoaded(sampler);
    }
}

bool DeckLabSamplerControls::loaded(int sampler) const {
    const Slot* s = slot(sampler);
    return s && s->loaded->get() > 0;
}

QString DeckLabSamplerControls::title(int sampler) const {
    const Slot* s = slot(sampler);
    return s ? s->title : QString();
}

bool DeckLabSamplerControls::playing(int sampler) const {
    const Slot* s = slot(sampler);
    return s && s->loaded->get() > 0 && s->play->get() > 0;
}

double DeckLabSamplerControls::position(int sampler) const {
    const Slot* s = slot(sampler);
    return s ? s->position->get() : 0;
}

double DeckLabSamplerControls::duration(int sampler) const {
    const Slot* s = slot(sampler);
    return s ? s->duration->get() : 0;
}

double DeckLabSamplerControls::rate(int sampler) const {
    const Slot* s = slot(sampler);
    return s ? s->rate->get() : 1;
}

double DeckLabSamplerControls::bpm(int sampler) const {
    const Slot* s = slot(sampler);
    return s ? s->bpm->get() : 0;
}

double DeckLabSamplerControls::gainDb(int sampler) const {
    const Slot* s = slot(sampler);
    if (!s || !s->pregain->valid()) {
        return 0;
    }
    return toDb(s->pregain->get());
}

DeckLabSamplerEngine::Mode DeckLabSamplerControls::mode(int sampler) const {
    const Slot* s = slot(sampler);
    return s ? s->mode : Mode::OneShot;
}

bool DeckLabSamplerControls::quantize() const {
    bool any = false;
    for (const auto& s : m_slots) {
        if (!s || !s->quantize->valid()) {
            continue;
        }
        if (s->quantize->get() <= 0) {
            return false;
        }
        any = true;
    }
    return any;
}

bool DeckLabSamplerControls::syncLoops() const {
    return m_syncLoops;
}

double DeckLabSamplerControls::level() const {
    const Slot* s = slot(1);
    return s && s->volume->valid() ? s->volume->getParameter() : 1.0;
}

double DeckLabSamplerControls::levelDb() const {
    const Slot* s = slot(1);
    return s && s->volume->valid() ? toDb(s->volume->get()) : 0.0;
}

void DeckLabSamplerControls::trigger(int sampler) {
    Slot* s = slot(sampler);
    if (!s || s->loaded->get() <= 0) {
        return;
    }
    // Without a cue point a stutter has nowhere to go: start from the top.
    if (s->cuePoint->get() < 0) {
        pulse(s->startPlay);
        return;
    }
    // play_stutter restarts a playing sample at its cue. A stopped one
    // would resume where it stopped, and one that played out could not
    // start at all (the engine refuses play at the end), so rewind first.
    if (s->play->get() <= 0) {
        pulse(s->cueStop);
    }
    pulse(s->stutter);
}

void DeckLabSamplerControls::stop(int sampler) {
    Slot* s = slot(sampler);
    if (!s) {
        return;
    }
    pulse(s->cuePoint->get() < 0 ? s->startStop : s->cueStop);
}

void DeckLabSamplerControls::eject(int sampler) {
    Slot* s = slot(sampler);
    // Never under a playing sample.
    if (!s || s->play->get() > 0) {
        return;
    }
    pulse(s->eject);
}

void DeckLabSamplerControls::setGainDb(int sampler, double db) {
    Slot* s = slot(sampler);
    if (!s || !util_isfinite(db)) {
        return;
    }
    s->pregain->set(std::pow(10.0, std::clamp(db, -12.0, 12.0) / 20));
    // The proxy does not hear its own writes.
    notify(sampler);
}

void DeckLabSamplerControls::setMode(int sampler, Mode mode) {
    Slot* s = slot(sampler);
    if (!s) {
        return;
    }
    s->mode = mode;
    if (m_config) {
        m_config->setValue(modeKey(sampler), static_cast<int>(mode));
    }
    s->repeat->set(mode == Mode::Loop ? 1 : 0);
    if (m_syncLoops) {
        s->sync->set(mode == Mode::Loop ? 1 : 0);
    }
    notify(sampler);
}

void DeckLabSamplerControls::setQuantize(bool on) {
    for (const auto& s : m_slots) {
        if (s) {
            s->quantize->set(on ? 1 : 0);
        }
    }
}

void DeckLabSamplerControls::setSyncLoops(bool on) {
    m_syncLoops = on;
    if (m_config) {
        m_config->setValue(ConfigKey(kSettingsGroup, QStringLiteral("sync_loops")), on ? 1 : 0);
    }
    for (const auto& s : m_slots) {
        if (s && s->mode == Mode::Loop) {
            s->sync->set(on ? 1 : 0);
        }
    }
}

void DeckLabSamplerControls::setLevel(double parameter) {
    const double value = util_isfinite(parameter) ? std::clamp(parameter, 0.0, 1.0) : 1.0;
    for (const auto& s : m_slots) {
        if (s) {
            s->volume->setParameter(value);
        }
    }
}
