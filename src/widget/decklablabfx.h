// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Deck Lab: Lab FX as one instrument (round 4, audit move 3).
//
// Pioneer beat-FX model, with Teenage Engineering punch-in feel:
// - effect select (six tiles), beat ◀▶ with the value shown (a strip of
//   cells over the pad), channel select (decks 1-4 in deck colours, MASTER,
//   SMP for the samplers) and a lit ON key;
// - an X-PAD style XY pad: touch-down = on, lift = the effect's musical
//   return, double-tap = latch; no time limit while a finger is down;
// - LOW / MID / HIGH band lamps choose which bands get the effect;
// - RELEASE (tail rings out), ECHO OUT (track drops for a bar) and CUT
//   (dry now), and a mini readout of the target.
//
// It is only a user interface over the existing Lab FX units
// ([DeckLabEffectUnit1/2], slot 4: the three-band performance LV2). The DSP
// and the effect engine are unchanged; every write goes through
// DeckLabLabFxHost, so the page also runs against a fake in tests and
// offscreen harnesses.

#include <QPointF>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <array>
#include <functional>

class QEvent;
class QPushButton;

namespace decklab::labfx {
class Pad;
class Key;
class Readout;
class DeckReadout;
} // namespace decklab::labfx

// What the footer shows of the deck that sets the tempo.
struct DeckLabLabFxDeck {
    bool loaded = false;
    bool playing = false;
    QString title;
    QString remaining; // "−3:25", or "—"
    int phase = -1;    // lit beat 0..3; -1 without a beat grid
    bool endWarning = false;
    bool operator==(const DeckLabLabFxDeck&) const = default;
};

// Everything Lab FX needs from the surface. Lab FX never holds an engine
// pointer of its own.
struct DeckLabLabFxHost {
    std::function<double(const QString& group, const QString& item)> value;
    std::function<void(const QString& group, const QString& item, double value)> set;
    // Normalised (0..1) parameter write, as ControlProxy::setParameter.
    std::function<void(const QString& group, const QString& item, double value)> setParameter;
    // Lab FX unit 1 or 2 has its performance effect loaded.
    std::function<bool(int unit)> unitReady;
    // Monotonic milliseconds.
    std::function<qint64()> now;
    std::function<DeckLabLabFxDeck(int deck)> deck;
    // 0 Decks, 1 Browse, 2 Sampler.
    std::function<void(int page)> showPage;
};

class DeckLabLabFx final : public QWidget {
    Q_OBJECT
  public:
    enum class Effect {
        Echo,
        Reverb,
        Filter,
        Roll,
        Duck,
        Drive,
    };
    static constexpr int kEffects = 6;
    // Targets: decks 1-4, MASTER (every deck and sampler), SMP (samplers).
    static constexpr int kMaster = 5;
    static constexpr int kSampler = 6;
    static constexpr int kBeats = 6;
    enum class Lamp {
        Dry,
        On,
        Tail,
    };
    // Two touches this close (press to press) on the pad or on one effect
    // tile latch the effect.
    static constexpr qint64 kDoubleTapMs = 350;

    explicit DeckLabLabFx(DeckLabLabFxHost host, QWidget* parent = nullptr);
    ~DeckLabLabFx() override;

    // The instrument's controls, for touch, keyboard and tests.
    void selectEffect(Effect effect);
    void selectTarget(int target);
    void selectBeat(int index);
    void stepBeat(int delta);
    void setBand(int band, bool on); // 0 LOW, 1 MID, 2 HIGH
    // The ON key: on (latched) from dry or a tail, off from on. Off works
    // like lifting a finger from the pad.
    void toggleOn();
    void release(); // RELEASE: back to dry, the tail rings out
    void echoOut(); // ECHO OUT: the track drops for one bar under the tail
    void cut();     // CUT: dry now, no tail
    // A pad touch whose release went astray: a held effect returns as if
    // the finger lifted; a latched one keeps playing.
    void endHolds();
    // Lost-touch safety, fed every mouse press/release and TouchEnd the
    // application sees (WDeckLab::eventFilter).
    void noteTouch(QObject* watched, QEvent* event);
    // Availability after deck, sampler or engine changes.
    void refresh();
    // The surface's 100 ms effect tick: heartbeat, tempo, beat phase, tails.
    void tick();

    Effect effect() const {
        return m_effect;
    }
    int target() const {
        return m_target;
    }
    int beatIndex() const;
    double beatValue() const;
    bool band(int band) const {
        return m_bands[band];
    }
    Lamp lamp() const;
    bool isOn() const {
        return m_on;
    }
    bool isLatched() const {
        return m_on && m_latched;
    }
    bool isHeld() const {
        return m_held;
    }
    // Any unit wet or ringing out.
    bool isWet() const;
    // The rail's Lab FX tab: lit while on, blinking with the beat while the
    // tail rings out.
    bool tabLit() const;
    bool available() const;
    QString readoutTitle() const;    // "ECHO · 1/2 BEAT"
    QString readoutSubtitle() const; // "DECK 1 · 128.0 BPM"
    QString padHint() const;
    // Beat values on the strip for the selected effect, e.g. "1/2".
    QStringList beatLabels() const;
    static QString effectName(Effect effect);
    QWidget* pad() const;

  signals:
    void stateChanged();

  protected:
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    friend class decklab::labfx::Pad;
    struct Unit {
        bool enabled = false; // slot on and mix up: wet, or ringing out
        bool releasing = false;
        bool mute = false;      // ECHO OUT: the track drops under the tail
        bool returning = false; // ECHO OUT: the track fades back in
        qint64 releaseAt = 0;
        int target = 0;
        unsigned int heartbeat = 0;
        std::array<double, 16> written{};
    };

    void build();
    void layoutChildren();
    void syncKeys();
    void padPressed(QPointF position);
    void padMoved(QPointF position);
    void padReleased();
    void engage(bool latch);
    void lift();
    void applyVoice();
    void route(int unit, int target);
    void writeParameter(int unit, int parameter, double normalized, bool force = false);
    void beginRelease(int unit, bool mute);
    void cancelRelease(int unit);
    void stopUnit(int unit);
    void startRolls(double size);
    void stopRolls();
    QStringList rollGroups() const;
    int unitFor(int target) const;
    int referenceDeck(int target) const;
    double referenceBpm(int target) const;
    bool targetLoaded(int target) const;
    bool usesBeat(Effect effect) const;
    bool usesBands(Effect effect) const;
    int columnAt(double x) const;
    double columnCentre(int column) const;
    double depth() const;
    void setSpot(QPointF spot);
    QPointF spot() const;
    void changed();
    void refreshReadouts();

    DeckLabLabFxHost m_host;
    Effect m_effect = Effect::Echo;
    int m_target = 1;
    // Beat cell per effect family: delay effects (1/16 .. 1, the DSP's
    // range) and rolls (1/16 .. 2, Mixxx's beatlooproll sizes).
    std::array<int, 2> m_beat{3, 2};
    std::array<bool, 3> m_bands{true, true, true};
    // Where the pad was last played, per effect: X (bipolar effects; beat
    // effects take X from the beat cell) and Y (depth).
    std::array<QPointF, kEffects> m_spots{};
    // The voice: what sounds now, and on which unit.
    bool m_on = false;
    bool m_latched = false;
    bool m_held = false;
    int m_voiceUnit = 0;
    int m_voiceTarget = 0;
    Effect m_voiceEffect = Effect::Echo;
    QPointF m_finger;
    QStringList m_rollGroups;
    QString m_rollItem;
    std::array<Unit, 2> m_units{};
    qint64 m_lastTick = -1;
    // Double-tap detection.
    qint64 m_lastPadPress = -100000;
    qint64 m_lastPadRelease = -100000;
    QPointF m_lastPadPoint;
    int m_lastTile = -1;
    qint64 m_lastTilePress = -100000;
    bool m_tailBlink = false;
    bool m_available = false;
    bool m_ready = false;
    // What the header readout last showed: effect, beat, target, loaded, BPM x10.
    std::array<int, 5> m_header{-1, -1, -1, -1, -1};

    // Pre-built control names: no string formatting on the touch path.
    std::array<QString, 2> m_unitGroups;
    std::array<QString, 2> m_slotGroups;
    std::array<QString, 16> m_parameterNames;
    std::array<QString, 4> m_deckRoutes;
    std::array<QString, 16> m_samplerRoutes;

    QWidget* m_wordmark = nullptr;
    decklab::labfx::Readout* m_readout = nullptr;
    decklab::labfx::Key* m_onKey = nullptr;
    std::array<decklab::labfx::Key*, 6> m_targets{};
    std::array<decklab::labfx::Key*, kEffects> m_tiles{};
    decklab::labfx::Key* m_beatLeft = nullptr;
    decklab::labfx::Key* m_beatRight = nullptr;
    std::array<decklab::labfx::Key*, kBeats> m_beatCells{};
    decklab::labfx::Pad* m_pad = nullptr;
    decklab::labfx::Key* m_padAction = nullptr;
    std::array<decklab::labfx::Key*, 3> m_bandKeys{};
    decklab::labfx::Key* m_releaseKey = nullptr;
    decklab::labfx::Key* m_echoOutKey = nullptr;
    decklab::labfx::Key* m_cutKey = nullptr;
    decklab::labfx::DeckReadout* m_deckReadout = nullptr;
    QRect m_bandCaption;
};
