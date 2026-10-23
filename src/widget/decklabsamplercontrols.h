// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QMetaObject>
#include <QObject>
#include <QString>
#include <memory>
#include <vector>

#include "preferences/usersettings.h"
#include "widget/decklabsampler.h"

class ControlProxy;
class PlayerManagerInterface;

// The Sampler page's engine on Mixxx's [SamplerN] controls (round 4, move 4).
//
// - Pads follow [App],num_samplers (the Deck Lab skin asks for 16); more
//   samplers, from a controller mapping or another skin, become banks.
// - A touch plays from the cue (play_stutter, after a rewind when stopped,
//   so a sample that played out is never dead), STOP goes back to the cue,
//   CLEAR is eject.
// - LOOP sets the sampler's repeat; SYNC sets sync_enabled on loop pads; QNT
//   sets quantize on every sampler; LEVEL sets every sampler's volume; GAIN
//   is the pad's pregain.
// - The pad modes and SYNC are remembered in the user's settings
//   ([DeckLabSampler] pad_N_mode, sync_loops).
// Watches are queued and only notify the page, which defers its work while
// hidden.
class DeckLabSamplerControls final : public QObject, public DeckLabSamplerEngine {
    Q_OBJECT
  public:
    DeckLabSamplerControls(UserSettingsPointer config,
            PlayerManagerInterface* players,
            QObject* parent = nullptr);
    ~DeckLabSamplerControls() override;

    int count() const override;
    bool loaded(int sampler) const override;
    QString title(int sampler) const override;
    bool playing(int sampler) const override;
    double position(int sampler) const override;
    double duration(int sampler) const override;
    double rate(int sampler) const override;
    double bpm(int sampler) const override;
    double gainDb(int sampler) const override;
    Mode mode(int sampler) const override;
    bool quantize() const override;
    bool syncLoops() const override;
    double level() const override;
    double levelDb() const override;
    void trigger(int sampler) override;
    void stop(int sampler) override;
    void eject(int sampler) override;
    void setGainDb(int sampler, double db) override;
    void setMode(int sampler, Mode mode) override;
    void setQuantize(bool on) override;
    void setSyncLoops(bool on) override;
    void setLevel(double parameter) override;

  signals:
    // A sample finished loading into this sampler.
    void sampleLoaded(int sampler);

  private:
    struct Slot;
    Slot* slot(int sampler) const;
    int available() const;
    void ensureSlots();
    void createSlot(int sampler);
    void loadedChanged(int sampler, bool newTrack);
    void refreshTitle(int sampler);
    UserSettingsPointer m_config;
    PlayerManagerInterface* m_players;
    ControlProxy* m_count;
    std::vector<std::unique_ptr<Slot>> m_slots;
    bool m_syncLoops = false;
};
