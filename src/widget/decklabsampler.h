// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Deck Lab Sampler (round 4, move 4): the samplers as a 4x4 instrument with
// the decks in view.
//
// - A 4x4 grid of pads, four wide like the EP-133 K.O. II and the DJS-1000.
//   A loaded pad fires on touch-down; an empty pad opens Browse on release.
// - A playing pad is a filled lamp that drains from right to left, so the
//   lit part is the length still to play, with the time left in figures.
// - A 44 px DECKS strip on top: each deck's play state, BPM, four-beat lamp,
//   time left and position, so a sample lands on the 1. Tap it for Decks.
// - Side column: QNT, SYNC, EDIT, FX (the Lab FX sampler send), LEVEL for
//   all pads, BANK (only with more than 16 samplers) and STOP ALL.
// - Hold a pad (or turn EDIT on and tap it) for the docked edit sheet: MODE
//   (one-shot, loop, gate), GAIN in dB, STOP, REPLACE (Browse loads another
//   sample into the pad) and CLEAR (ejects the sample). REPLACE and CLEAR act
//   on release, and never while the pad plays.
// - r7-sampler: EDIT stays on until it is tapped again or the page hides.
//   While on, every tap on a loaded pad opens (or switches) its sheet and
//   never plays it; loaded pads show an EDIT mark. An empty pad still opens
//   Browse to load it.
//
// The page reads and writes samplers only through DeckLabSamplerEngine, so
// it is tested and rendered with generated data; decklabsamplercontrols.h is
// the Mixxx implementation. Nothing here runs a timer: the owner calls tick()
// from its existing display tick while the page is visible, and only pads
// that are playing (or just stopped) repaint.

#include <QAbstractSlider>
#include <QPushButton>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <array>
#include <functional>

#include "library/decklabknob.h"

class QGridLayout;
class QLabel;

// What the DECKS strip shows for one deck. The owner fills it on its tick.
struct DeckLabSamplerDeck {
    bool loaded = false;
    bool playing = false;
    bool master = false;     // tempo master (sync leader)
    bool endWarning = false; // inside the end-of-track warning time
    double bpm = 0;          // shown tempo; 0 without one
    double position = 0;     // 0..1 in the track
    double remaining = -1;   // seconds left at the current tempo; -1 unknown
    int phase = -1;          // lit beat 0..3; -1 without a beat grid
    bool operator==(const DeckLabSamplerDeck&) const = default;
};

// Everything the Sampler page reads from or writes to the samplers. Samplers
// are numbered from 1, as on screen.
class DeckLabSamplerEngine {
  public:
    enum class Mode {
        OneShot = 0, // touch plays from the top (again while playing)
        Loop = 1,    // touch starts a repeating sample; touch again stops it
        Gate = 2,    // plays while the pad is held
    };
    virtual ~DeckLabSamplerEngine() = default;
    // Samplers configured in Mixxx; 0 when unknown. The page always shows at
    // least one bank of 16.
    virtual int count() const = 0;
    virtual bool loaded(int sampler) const = 0;
    virtual QString title(int sampler) const = 0;
    virtual bool playing(int sampler) const = 0;
    virtual double position(int sampler) const = 0; // 0..1
    virtual double duration(int sampler) const = 0; // seconds at normal speed
    virtual double rate(int sampler) const = 0;     // playback speed ratio
    virtual double bpm(int sampler) const = 0;      // 0 without a beat grid
    virtual double gainDb(int sampler) const = 0;   // the pad's own gain
    virtual Mode mode(int sampler) const = 0;
    virtual bool quantize() const = 0;  // every sampler starts in phase
    virtual bool syncLoops() const = 0; // loop pads follow the master tempo
    virtual double level() const = 0;   // all samplers' volume, 0..1 fader
    virtual double levelDb() const = 0;
    // Touch-down: plays the pad from its cue, or restarts it if playing.
    virtual void trigger(int sampler) = 0;
    // Back to the cue, stopped, so the next touch starts from the top.
    virtual void stop(int sampler) = 0;
    virtual void eject(int sampler) = 0;
    virtual void setGainDb(int sampler, double db) = 0;
    virtual void setMode(int sampler, Mode mode) = 0;
    virtual void setQuantize(bool on) = 0;
    virtual void setSyncLoops(bool on) = 0;
    virtual void setLevel(double parameter) = 0;
    // The page listens here: the sampler whose state changed, or 0 when the
    // number of samplers (or several of them) changed.
    void setListener(std::function<void(int)> listener) {
        m_listener = std::move(listener);
    }

  protected:
    void notify(int sampler) const {
        if (m_listener) {
            m_listener(sampler);
        }
    }

  private:
    std::function<void(int)> m_listener;
};

// One pad as painted. Values are whole numbers so an unchanged pad never
// repaints.
struct DeckLabSamplerPadState {
    int number = 0;        // the sampler shown, numbered from 1
    bool exists = true;    // configured; a bank's spare pads are dark
    bool loaded = false;
    QString title;
    DeckLabSamplerEngine::Mode mode = DeckLabSamplerEngine::Mode::OneShot;
    int bpm = 0;           // whole BPM; 0 none
    int level = 500;       // gain bar 0..1000 (0 dB = 500)
    bool selected = false; // the edit sheet's pad
    bool editing = false;  // r7-sampler: EDIT is on, a tap edits this pad
    bool operator==(const DeckLabSamplerPadState&) const = default;
};

// A sampler pad. Painted; the pad is "checked" while it plays (the filled
// lamp), and clicks never toggle that. text() is "number\ntitle".
class DeckLabSamplerPad final : public QPushButton {
    Q_OBJECT
  public:
    explicit DeckLabSamplerPad(QWidget* parent = nullptr);
    void setState(const DeckLabSamplerPadState& state);
    const DeckLabSamplerPadState& state() const {
        return m_state;
    }
    // remaining: 0..1 of the sample still to play; seconds: the time left
    // while playing, the length while idle (-1 unknown). Returns true when
    // the drain moved by a pixel or the time changed (a repaint was asked).
    bool setDrain(double remaining, int seconds);
    int seconds() const {
        return m_seconds;
    }
    // Width of the lit part in pixels, while lit.
    int litWidth() const;
    static QString timeText(int seconds, bool remaining);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void nextCheckState() override {
    }

  private:
    DeckLabSamplerPadState m_state;
    double m_remaining = 1;
    int m_seconds = -1; // time left while playing, the length while idle
};

// The DECKS strip: each deck's badge, play state, BPM, four-beat lamp, time
// left and position. Tap it to go back to Decks. Repaints only changed cells.
class DeckLabSamplerDecks final : public QPushButton {
    Q_OBJECT
  public:
    explicit DeckLabSamplerDecks(QWidget* parent = nullptr);
    void setDecks(const std::array<DeckLabSamplerDeck, 4>& decks);
    const std::array<DeckLabSamplerDeck, 4>& decks() const {
        return m_decks;
    }
    // Decks with a cell: 1 and 2 always, 3 and 4 while loaded.
    QList<int> shownDecks() const;
    QRect cellRect(int deck) const;
    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    // What a cell shows, in whole units, so unchanged cells never repaint.
    struct Cell {
        bool loaded = false;
        bool playing = false;
        bool master = false;
        bool endWarning = false;
        int bpmTenths = 0;
        int seconds = -1;
        int phase = -1;
        int progress = -1; // pixels
        bool operator==(const Cell&) const = default;
    };
    Cell cellFor(int deck, int width) const;
    int linkWidth() const;
    void paintCell(QPainter& painter, int deck, const QRect& area) const;
    std::array<DeckLabSamplerDeck, 4> m_decks{};
    std::array<Cell, 4> m_cells{};
    QList<int> m_shown;
};

// LEVEL for all pads: a vertical fader that moves with the finger from where
// it lands (never jumps to the touch).
class DeckLabSamplerLevel final : public QAbstractSlider {
    Q_OBJECT
  public:
    explicit DeckLabSamplerLevel(QWidget* parent = nullptr);
    void setValueText(const QString& text);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

  private:
    QRect track() const;
    QString m_text;
    bool m_dragging = false;
    int m_pressY = 0;
    int m_pressValue = 0;
};

// The Sampler page.
class DeckLabSampler final : public QWidget {
    Q_OBJECT
  public:
    static constexpr int kPads = 16;
    // A touch held this long on a loaded pad opens its edit sheet.
    static constexpr int kHoldMs = 500;
    explicit DeckLabSampler(DeckLabSamplerEngine* engine, QWidget* parent = nullptr);
    ~DeckLabSampler() override;
    // A counter that changes whenever a touch must no longer act (page or
    // layer change, touch cancel); the page also counts its own hides.
    void setInteractionEpoch(std::function<quint64()> epoch);
    // The display tick, while the page is visible.
    void tick(const std::array<DeckLabSamplerDeck, 4>& decks);
    // r4-waveforms: in Deck Lab the waveform lanes and their DECKS row sit
    // above the page and replace this strip; hidden, the pads take its
    // height. Shown by default (the page on its own).
    void setDecksStripShown(bool shown);
    bool decksStripShown() const;
    // The Lab FX sampler send, owned by the surface; the FX lamp shows it.
    void setFxRouting(bool on);
    bool fxRouting() const {
        return m_fxRouting;
    }
    // Shows the bank that holds this sampler.
    void showPad(int sampler);
    // Opens the edit sheet for this sampler (its bank first).
    void editPad(int sampler);
    void closeSheet();
    bool sheetOpen() const;
    // The sampler the sheet edits; 0 when closed.
    int sheetSampler() const;
    int bank() const {
        return m_bank;
    }
    int banks() const {
        return m_banks;
    }
    bool editMode() const {
        return m_editMode;
    }
    // r7-sampler: EDIT on or off. Off closes the sheet.
    void setEditMode(bool on);
    // r7-sampler: a pad's MODE from outside the sheet (a captured loop
    // arrives as a LOOP pad).
    void setPadMode(int sampler, DeckLabSamplerEngine::Mode mode);
    bool anyPlaying() const;
    // The browse knob while the edit sheet is open: turn = GAIN in 1 dB
    // steps, press = done, back = close. False when the page does not take
    // it (the sheet is closed): Browse keeps its meaning.
    bool knob(decklab::KnobAction action, double steps);
    // The engine's listener.
    void samplerChanged(int sampler);
    DeckLabSamplerPad* pad(int index) const {
        return m_pads[index];
    }

  signals:
    // An empty pad was tapped, or REPLACE (r7-sampler): load this sampler
    // from Browse.
    void loadRequested(int sampler);
    void decksRequested();
    // The FX lamp was tapped: send pads 1-4 through Lab FX (or stop).
    void fxRoutingRequested(bool on);

  protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    struct Press {
        bool loaded = false;
        bool valid = false;
        quint64 epoch = 0;
    };
    int samplerAt(int index) const {
        return m_bank * kPads + index + 1;
    }
    bool exists(int sampler) const;
    quint64 epoch() const;
    QWidget* buildSide();
    QWidget* buildSheet();
    void padPressed(int index);
    void padReleased(int index);
    void padClicked(int index);
    void holdElapsed();
    void endGate();
    void openSheet(int index);
    void refreshPad(int index);
    void refreshPads();
    void refreshDrain(int index);
    void refreshSheet();
    void refreshSheetState();
    void refreshSide();
    void refreshHint(); // r7-sampler
    void refreshBanks();
    void stepGain(double db);
    DeckLabSamplerEngine* m_engine;
    std::array<DeckLabSamplerPad*, kPads> m_pads{};
    std::array<Press, kPads> m_presses{};
    DeckLabSamplerDecks* m_decks = nullptr;
    QGridLayout* m_grid = nullptr;
    QPushButton* m_quantize = nullptr;
    QPushButton* m_sync = nullptr;
    QPushButton* m_edit = nullptr;
    QPushButton* m_fx = nullptr;
    QPushButton* m_bankButton = nullptr;
    QPushButton* m_stopAll = nullptr;
    DeckLabSamplerLevel* m_level = nullptr;
    QWidget* m_sheet = nullptr;
    QLabel* m_sheetNumber = nullptr;
    QLabel* m_sheetTitle = nullptr;
    QLabel* m_sheetGain = nullptr;
    std::array<QPushButton*, 3> m_sheetModes{};
    QPushButton* m_sheetStop = nullptr;
    QPushButton* m_sheetClear = nullptr;
    QPushButton* m_sheetReplace = nullptr; // r7-sampler
    QLabel* m_sheetHint = nullptr;         // r7-sampler
    QTimer m_hold;
    int m_holdPad = -1;
    int m_sheetPad = -1;
    int m_sheetRow = -1; // first grid row the sheet covers
    int m_gatePad = -1;
    quint64 m_gateEpoch = 0;
    int m_bank = 0;
    int m_banks = 1;
    bool m_editMode = false;
    bool m_fxRouting = false;
    bool m_decksShown = true; // r4-waveforms
    bool m_dirty = true;
    int m_shownLevel = -1;
    std::function<quint64()> m_epoch;
    quint64 m_hides = 0;
};
