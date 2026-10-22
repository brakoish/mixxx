// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QColor>
#include <QFont>
#include <QList>
#include <QPushButton>
#include <QRegion>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <algorithm>
#include <array>
#include <cmath>

class QPainter;

// Deck identity colors. Badges, lane edges and the waveform pairing all use
// the same color so a DJ can match a readout to its waveform at a glance.
inline QColor deckLabDeckColor(int deck) {
    switch (deck) {
    case 1:
        return QColor(0x5b, 0x9c, 0xff);
    case 2:
        return QColor(0xff, 0x6f, 0x91);
    case 3:
        return QColor(0xb4, 0x8c, 0xff);
    case 4:
        return QColor(0x4f, 0xd1, 0xc5);
    default:
        return QColor(0x88, 0x88, 0x88);
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
    if (!std::isfinite(fraction)) {
        return {};
    }
    const double whole = std::floor(fraction);
    beatIndex += static_cast<long long>(whole);
    return {static_cast<int>(((beatIndex % 4) + 4) % 4), fraction - whole};
}

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
    QString key;
    QColor keyColor;
    int keyMatch = 0; // 1 compatible with the reference deck, -1 not, 0 none
    QString remaining;
    bool endWarning = false;
    bool master = false;
    bool sync = false;
    bool pfl = false;
    bool slip = false;
    bool keylock = false;
    bool quantize = false;
    bool looping = false;
    QString loopSize;
    int phase = -1; // lit beat segment 0..3; -1 without a beat grid
    bool operator==(const DeckLabStripState&) const = default;
};

// Painting and partial-update geometry shared by the full deck readout, the
// compact four-deck readout and the one-line readout above waveforms on
// other pages. No layouts, style sheets or rich text are involved.
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
        Time
    };
    void setMode(Mode mode) {
        m_mode = mode;
    }
    Mode mode() const {
        return m_mode;
    }
    void setSize(const QSize& size) {
        m_size = size;
    }
    void setFamily(const QString& family);
    // Stores the new state and returns the region that changed.
    QRegion setState(const DeckLabStripState& next);
    QRegion setFlash(bool on);
    bool flash() const {
        return m_flash;
    }
    const DeckLabStripState& state() const {
        return m_state;
    }
    QRect rect(Part part) const;
    void paint(QPainter& painter, const QRect& dirty, bool pressed, bool focused) const;
    // Lit deck modes as words, for accessibility and verification.
    QString modesText() const;
    // Parts whose text cannot be shown at its minimum readable size.
    QStringList overflowingParts() const;

  private:
    QFont font(int pixels, QFont::Weight weight, bool caption = false) const;
    int left() const;
    void paintBadge(QPainter& painter) const;
    void paintTransport(QPainter& painter, const QRect& area) const;
    void paintTitle(QPainter& painter, const QRect& area, int pixels) const;
    void paintPhase(QPainter& painter) const;
    int paintChip(QPainter& painter, int x, int y, int height, const QString& text, bool on,
            const QColor& lit, bool alignRight) const;
    Mode m_mode = Mode::Full;
    QSize m_size;
    QString m_family;
    DeckLabStripState m_state;
    bool m_flash = false;
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

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    void placeMeter();
    DeckLabStripRenderer m_renderer;
    DeckLabLevelMeter* m_meter;
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
