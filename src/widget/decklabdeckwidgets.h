// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QColor>
#include <QFont>
#include <QList>
#include <QPixmap>
#include <QPushButton>
#include <QRegion>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <algorithm>
#include <array>
#include <cmath>

#include "util/fpclassify.h"

class QPainter;
class QTimer;

// Deck identity colors. Badges, lane edges and the waveform pairing all use
// the same color so a DJ can match a readout to its waveform at a glance.
inline QColor deckLabDeckColor(int deck) {
    switch (deck) {
    case 1:
        return QColor(0x35, 0xc8, 0xf0); // cyan
    case 2:
        return QColor(0xf5, 0xa5, 0x24); // amber
    case 3:
        return QColor(0xb5, 0xe6, 0x1d); // lime
    case 4:
        return QColor(0xc7, 0x7d, 0xff); // violet
    default:
        return QColor(0x9a, 0xa3, 0xaf);
    }
}

// Beat counter for the phase meter. beatIndex counts beats from the stored
// grid's first marker and fraction is the position inside the current beat
// (the same quantity as the engine's beat_distance). Mixxx stores no downbeat
// or bar data, so the four segments are a beat count in groups of four, not
// a bar or phrase position.
struct DeckLabBeatPhase {
    int segment = -1;
    double fraction = 0;
};
inline DeckLabBeatPhase deckLabBeatPhase(long long beatIndex, double fraction) {
    // util_isfinite: the build uses -ffast-math, where std::isfinite is unreliable.
    if (!util_isfinite(fraction)) {
        return {};
    }
    // r5-decks: a playhead a hair before a beat (a frame-boundary rounding,
    // e.g. right after DOWNBEAT put a beat there) counts as on that beat.
    const double whole = std::floor(fraction + 1e-6);
    beatIndex += static_cast<long long>(whole);
    return {static_cast<int>(((beatIndex % 4) + 4) % 4), std::max(0.0, fraction - whole)};
}

// r4-decks: how far into the current beat, in eighths (0..7). The phase
// meter fills the lit beat by this much, so two decks in phase fill their
// cells to the same edge and a drift of an eighth of a beat shows.
inline int deckLabBeatStep(const DeckLabBeatPhase& phase) {
    if (phase.segment < 0 || !util_isfinite(phase.fraction)) {
        return -1;
    }
    return std::clamp(static_cast<int>(phase.fraction * 8), 0, 7);
}

// Phrase readout from a rekordbox export's phrase analysis: the current
// phrase and a "bars.beats" countdown to its end. Empty name: no readout.
struct DeckLabPhraseReadout {
    QString name;
    QString countdown;
    QColor color;
    bool operator==(const DeckLabPhraseReadout&) const = default;
};

// Everything one deck readout shows. Widgets repaint only the parts whose
// values differ from the previous state.
struct DeckLabStripState {
    int deck = 0;
    bool loaded = false;
    bool playing = false;
    bool cue = false; // paused on the main cue point
    QString title;
    QString artist;
    QString bpm;
    QString tempo;
    QString range;
    bool tempoCentered = false;
    // r4-decks: the tempo is beyond the fader range (Sync took it there), so
    // the next touch of the tempo fader would jump. Shown as OVER ±8%.
    bool overRange = false;
    QString key;
    QColor keyColor;
    int keyMatch = 0; // 1 compatible with the reference deck, -1 not, 0 none
    // r5-decks: the key shift in semitones from the track's own key
    // (Mixxx's [ChannelN],pitch), "+1" or "−0.5"; empty when unshifted. The
    // readout shows "8A +1".
    QString keyShift;
    // r5-decks: MATCH (sync_key) set this key and it still matches the deck
    // it was matched to: the key turns the match green, outlined.
    bool keyMatched = false;
    QString remaining;
    bool endWarning = false;
    bool master = false;
    bool sync = false;
    // A one-shot SYNC tap just matched tempo and beat: the SYNC lamp reads
    // MATCHED for a moment. Sync lock itself is `sync`.
    bool matched = false;
    bool pfl = false;
    bool slip = false;
    bool keylock = false;
    bool quantize = false;
    bool looping = false;
    QString loopSize;
    int phase = -1; // lit beat segment 0..3; -1 without a beat grid
    // r4-decks: the phase meter compares this deck with a reference deck
    // (the tempo master, else another playing deck), as a CDJ's does: the
    // reference's beat on the top row, this deck's below. Steps are eighths
    // of the beat (deckLabBeatStep). referenceDeck 0: nothing to compare.
    int phaseStep = -1;
    int referenceDeck = 0;
    int referencePhase = -1;
    int referenceStep = -1;
    bool referencePlaying = false;
    // rekordbox phrase readout. r4-decks: shown beside the phase meter,
    // never instead of it.
    DeckLabPhraseReadout phrase;
    bool operator==(const DeckLabStripState&) const = default;
};

// Loads the Deck Lab typefaces bundled with the skin (Barlow and Barlow Semi
// Condensed, OFL) once per process. Linux builds do not install res/fonts.
void deckLabLoadFonts(const QString& resourcePath);

// Painting and partial-update geometry shared by the full deck readout, the
// compact four-deck readout and the one-line readout above waveforms on
// other pages. No layouts, style sheets or rich text are involved.
//
// On-device sizes are binding (800x480 at about 133 ppi): hero numerals 36,
// readouts 24, titles 18, body 16, secondary 15 (Medium, muted), labels 14
// (SemiBold, uppercase). Nothing is smaller and nothing is shrunk to fit:
// text is elided and lower-priority items are dropped instead.
class DeckLabStripRenderer {
  public:
    enum class Mode {
        Full,
        Compact,
        Mini
    };
    enum class Part {
        Phase,
        Identity,
        Title,
        Chips,
        Captions,
        Bpm,
        Tempo,
        Key,
        Time,
        Phrase
    };
    static constexpr int kHero = 36;
    static constexpr int kReadout = 24;
    static constexpr int kTitle = 18;
    static constexpr int kBody = 16;
    static constexpr int kSecondary = 15;
    static constexpr int kLabel = 14;
    static int preferredHeight(Mode mode) {
        return mode == Mode::Mini ? 30 : mode == Mode::Compact ? 138 : 104;
    }
    // Paints a headphone glyph (headphone cue lamp) in a box of this size.
    static void paintHeadphones(QPainter& painter, const QRectF& box, const QColor& ink);
    void setMode(Mode mode) {
        m_mode = mode;
        layout();
    }
    Mode mode() const {
        return m_mode;
    }
    void setSize(const QSize& size) {
        m_size = size;
        layout();
    }
    // Family used only when the bundled typefaces are missing.
    void setFamily(const QString& fallback) {
        m_fallback = fallback;
        layout();
    }
    // Stores the new state and returns the region that changed.
    QRegion setState(const DeckLabStripState& next);
    QRegion setFlash(bool on);
    bool flash() const {
        return m_flash;
    }
    const DeckLabStripState& state() const {
        return m_state;
    }
    QRect rect(Part part) const {
        return m_rects[static_cast<int>(part)];
    }
    void paint(QPainter& painter, const QRect& dirty, bool pressed, bool focused) const;
    // Lit deck modes as words, for accessibility and verification.
    QString modesText() const;
    // The whole BPM a compact card shows when its decimals do not fit:
    // rounded to the nearest beat per minute, never cut off.
    static QString roundedBpm(const QString& bpm);
    // Parts that cannot be shown at their binding size in this geometry.
    QStringList overflowingParts() const;
    // r5-decks: touch zones inside the readout, at least 44 px tall: the
    // KEY readout opens the key sheet, a long press on the BPM opens GRID.
    QRect keyTarget() const;
    QRect bpmTarget() const;
    // r5-decks: how the key and its shift are drawn. When they do not fit,
    // a two-name key takes its second name ("D♯m/E♭m" reads "E♭m") and then
    // the shift steps down to 14 px; a check mark that no longer fits is
    // dropped. Never squeezed, never over the tempo or the clock.
    struct KeyText {
        QString key;
        int shiftPixels = kSecondary;
        int keyWidth = 0;
        int width = 0;
    };
    KeyText keyText() const;
    int keyRoom() const;
    int keyTextWidth() const {
        return keyText().width;
    }
    QFont uiFont(int pixels, QFont::Weight weight) const;
    QFont numeralFont(int pixels) const;
    QFont labelFont() const;

  private:
    void layout();
    // Key box shifted left when a long clock needs the room.
    int clockExcess() const;
    void paintBadge(QPainter& painter) const;
    void paintTransport(QPainter& painter, const QRect& area) const;
    void paintTitle(QPainter& painter, const QRect& area) const;
    void paintPhase(QPainter& painter) const;
    // r4-decks: one row of the phase meter (four beat cells).
    void paintPhaseRow(QPainter& painter, const QRect& cells, int deck, int beat,
            int step, bool playing) const;
    int phaseMeterWidth() const;
    // left: first usable x (compact cards paint the lamps first).
    void paintPhrase(QPainter& painter, int left) const;
    int phraseWidth() const;
    void paintValues(QPainter& painter, const QRect& dirty) const;
    // Returns the x just after the last lamp painted.
    int paintLamps(QPainter& painter) const;
    int paintChip(QPainter& painter, int x, int y, const QString& text, bool on,
            const QColor& lit, bool outlined = false) const;
    int chipWidth(const QString& text) const;
    // r4-decks: a flat lamp. Off is a dark dot and a muted label with no
    // outline, so it never reads as a button; lit is a filled chip (or an
    // outlined one, SYNC) in the same footprint, so nothing moves.
    enum class Lamp {
        Off,
        Filled,
        Outlined
    };
    int lampWidth(const QString& text, bool headphones = false) const;
    int paintLamp(QPainter& painter, int x, int y, const QString& text, Lamp look,
            const QColor& lit, bool headphones = false, int width = 0) const;
    // Exceptions shown as lit lamps when the room allows: LOOP n, SLIP, KEY
    // and QNT OFF, most important first.
    struct Exception {
        QString text;
        bool outlined;
    };
    QList<Exception> exceptions() const;
    // Full readouts paint them on the last row, leaving `reserve` px at the
    // right for the phrase. Returns the x after the last one.
    int paintExceptions(QPainter& painter, const QRect& area, int reserve) const;
    Mode m_mode = Mode::Full;
    QSize m_size;
    QString m_fallback;
    DeckLabStripState m_state;
    bool m_flash = false;
    std::array<QRect, 10> m_rects{};
};

// Tap target for one deck: the readout opens full track information (or
// Browse for an empty deck). Painted; never relayouts while values change.
class DeckLabLevelMeter;
class DeckLabDeckStrip final : public QPushButton {
    Q_OBJECT
  public:
    DeckLabDeckStrip(int deck, QWidget* parent = nullptr);
    void setMode(DeckLabStripRenderer::Mode mode);
    DeckLabStripRenderer::Mode mode() const {
        return m_renderer.mode();
    }
    void setState(const DeckLabStripState& state);
    void setFlash(bool on);
    const DeckLabStripState& state() const {
        return m_renderer.state();
    }
    const DeckLabStripRenderer& renderer() const {
        return m_renderer;
    }
    DeckLabLevelMeter* meter() const {
        return m_meter;
    }
    QString modesText() const {
        return m_renderer.modesText();
    }
    // The readout's height is set by the presentation, never by button style
    // padding or text metrics.
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    // r5-decks: how long the BPM readout of a paused deck is held to open
    // GRID mode (a playing deck enters GRID from MORE only), and how long a
    // playing deck's KEY readout is held to open the key sheet (round 4's
    // stray-thumb rule: a brush on a live deck never swaps its pads).
    static constexpr int kGridHoldMs = 600;
    static constexpr int kLiveKeyHoldMs = 400;

  signals:
    // r5-decks: the loaded deck's KEY readout asks for the key sheet: on
    // touch-down while paused, after kLiveKeyHoldMs while playing. The touch
    // is the sheet's: the readout does not go down or click.
    void keyTouched(int deck);
    // r5-decks: a paused deck's BPM readout was held for kGridHoldMs (GRID
    // mode). The press is cancelled, so its release opens nothing.
    void bpmHeld(int deck);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

  private:
    void placeMeter();
    DeckLabStripRenderer m_renderer;
    DeckLabLevelMeter* m_meter;
    bool m_keyboardFocus = false;
    // r5-decks: single shots, running only while a readout zone is held.
    QTimer* m_gridHold = nullptr;
    QTimer* m_keyHold = nullptr;
    bool m_keyTouch = false; // a KEY touch the strip owns (no button press)
};

// One-line readout above a waveform on Browse and FX. Display only.
class DeckLabMiniStrip final : public QWidget {
    Q_OBJECT
  public:
    explicit DeckLabMiniStrip(QWidget* parent = nullptr);
    void setState(const DeckLabStripState& state);
    void setFlash(bool on);
    const DeckLabStripState& state() const {
        return m_renderer.state();
    }

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    DeckLabStripRenderer m_renderer;
};

// Rail button made of lamps: a caption and lit/unlit chips. Used for the
// controller FX units and for the audio/overload status.
class DeckLabChipButton final : public QPushButton {
    Q_OBJECT
  public:
    struct Chip {
        QString text;
        QColor lit;
        bool on = false;
        bool operator==(const Chip&) const = default;
    };
    explicit DeckLabChipButton(QWidget* parent = nullptr);
    void setCaption(const QString& caption);
    // Repaints only when a chip changed.
    void setChips(const QList<Chip>& chips);
    const QList<Chip>& chips() const {
        return m_chips;
    }
    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QString m_caption;
    QList<Chip> m_chips;
};

// Brand mark at the left of the tab rail: waveform bars split by a red
// playhead (played bars graphite, unplayed white). Display only.
class DeckLabMark final : public QWidget {
  public:
    // iconPath: the brand's pixel-grid mark; painted from geometry if missing.
    explicit DeckLabMark(const QString& iconPath, QWidget* parent = nullptr);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QPixmap m_icon;
};

// Painted segment level meter for the Decks page. It is polled by the display
// tick instead of following engine-rate value signals, and it only requests a
// repaint when a lit segment or the clip lamp actually changes.
class DeckLabLevelMeter final : public QWidget {
    Q_OBJECT
  public:
    static constexpr int kSegments = 14;
    DeckLabLevelMeter(Qt::Orientation orientation, int channels, QWidget* parent = nullptr);
    // Returns true when the painted state changed.
    bool setLevels(double first, double second, bool clipping);
    double level(int channel) const {
        return m_levels[std::clamp(channel, 0, 1)];
    }
    int litSegments(int channel) const {
        return m_lit[std::clamp(channel, 0, 1)];
    }
    bool clipping() const {
        return m_clip;
    }
    int channels() const {
        return m_channels;
    }

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    Qt::Orientation m_orientation;
    int m_channels;
    std::array<double, 2> m_levels{};
    std::array<int, 2> m_lit{};
    bool m_clip = false;
};
