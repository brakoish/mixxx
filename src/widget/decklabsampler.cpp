// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabsampler.h"

#include <QCoreApplication>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <utility>

#include "util/fpclassify.h"
#include "widget/decklabdeckwidgets.h"

#include "moc_decklabsampler.cpp"

namespace {
// Brand tokens. Live red is only for the end-of-track warning.
const QColor kGround(0x0d, 0x0f, 0x12);
const QColor kPanel(0x16, 0x19, 0x1e);
const QColor kRaise(0x20, 0x24, 0x2b);
const QColor kLine(0x2c, 0x31, 0x3a);
const QColor kMuted(0x9a, 0xa3, 0xaf);
const QColor kText(0xf2, 0xf4, 0xf7);
const QColor kDecimals(0xc9, 0xce, 0xd6);
const QColor kOffFill(0x11, 0x14, 0x18);
const QColor kOffLine(0x1e, 0x22, 0x28);
const QColor kOffInk(0x4b, 0x52, 0x5c);
const QColor kPlay(0x2f, 0xd2, 0x7a);
const QColor kLive(0xff, 0x3d, 0x2e);
constexpr int kDecksHeight = 44;
constexpr int kLinkWidth = 78;
constexpr int kCompactLinkWidth = 44; // chevron only, beside three or four decks
constexpr int kSideWidth = 100;
const QChar kMinus(0x2212);

QString tr(const char* text) {
    return QCoreApplication::translate("DeckLabSampler", text);
}

// The page's own sheet: Barlow on every child; 44 px side and sheet keys
// (42 px plus the border), filled when on. Pads and the DECKS strip are
// painted and keep their own geometry whatever the surface sheet says.
const QString kSamplerStyle = QStringLiteral(R"(
 QWidget#decklab-sampler {background:#0D0F12;}
 QWidget#decklab-sampler QWidget {font-family:"Barlow";font-size:16px;font-weight:500;color:#F2F4F7;}
 QWidget#decklab-sampler QLabel {background:transparent;}
 QWidget#decklab-sampler QLabel[muted="true"] {color:#9AA3AF;}
 QWidget#decklab-sampler QLabel[heading="true"] {font-size:18px;font-weight:600;}
 QWidget#decklab-sampler QLabel[readout="true"] {font-family:"Barlow Semi Condensed";font-size:24px;font-weight:600;}
 DeckLabSamplerPad {min-height:0px;max-height:4000px;padding:0px;margin:0px;}
 DeckLabSamplerDecks {min-height:44px;max-height:44px;padding:0px;margin:0px;border:0px;}
 QWidget#decklab-sampler-side QPushButton, QWidget#decklab-sampler-sheet QPushButton {background:#16191E;color:#F2F4F7;
     border:1px solid #2C313A;border-radius:3px;padding:0px 8px;min-height:42px;max-height:42px;font-weight:600;}
 QWidget#decklab-sampler-side QPushButton:pressed, QWidget#decklab-sampler-sheet QPushButton:pressed {background:#20242B;}
 QWidget#decklab-sampler-side QPushButton:checked, QWidget#decklab-sampler-sheet QPushButton:checked {background:#F2F4F7;
     border-color:#F2F4F7;color:#0D0F12;}
 QWidget#decklab-sampler-side QPushButton:disabled, QWidget#decklab-sampler-sheet QPushButton:disabled {background:#111418;
     color:#4B525C;border-color:#1E2228;}
 QWidget#decklab-sampler-side QPushButton {padding:0px;}
 QWidget#decklab-sampler-side QPushButton#decklab-samples-stop {min-height:58px;max-height:58px;}
 QWidget#decklab-sampler-sheet {background:#16191E;border:1px solid #9AA3AF;border-radius:3px;}
 QWidget#decklab-sampler-sheet QPushButton[glyph="true"] {font-size:24px;font-weight:500;padding:0px;}
 )");

QFont uiFont(int pixels, QFont::Weight weight) {
    QFont font;
    font.setFamilies({QStringLiteral("Barlow")});
    font.setPixelSize(std::max(14, pixels));
    font.setWeight(weight);
    return font;
}

QFont numeralFont(int pixels) {
    QFont font;
    font.setFamilies({QStringLiteral("Barlow Semi Condensed"), QStringLiteral("Barlow")});
    font.setPixelSize(std::max(14, pixels));
    font.setWeight(QFont::DemiBold);
    return font;
}

QFont labelFont() {
    QFont font = uiFont(14, QFont::DemiBold);
    font.setLetterSpacing(QFont::PercentageSpacing, 106);
    return font;
}

// Fonts are built once (after the application exists), not on every paint.
struct Fonts {
    QFont body = uiFont(16, QFont::DemiBold);
    QFont medium = uiFont(16, QFont::Medium);
    QFont plus = uiFont(24, QFont::Medium);
    QFont numeral16 = numeralFont(16);
    QFont numeral18 = numeralFont(18);
    QFont numeral24 = numeralFont(24);
    QFont badge = uiFont(18, QFont::DemiBold);
    QFont label = labelFont();
    static const Fonts& get() {
        static const Fonts fonts;
        return fonts;
    }
};

// Fixed-width digit cells, so a running clock never shifts sideways.
int digitCell(const QFontMetrics& metrics) {
    int cell = 0;
    for (char digit = '0'; digit <= '9'; ++digit) {
        cell = std::max(cell, metrics.horizontalAdvance(QLatin1Char(digit)));
    }
    return cell;
}

int tabularWidth(const QFontMetrics& metrics, const QString& text) {
    const int cell = digitCell(metrics);
    int width = 0;
    for (const QChar ch : text) {
        width += ch.isDigit() ? cell : metrics.horizontalAdvance(ch);
    }
    return width;
}

void drawTabular(QPainter& painter, int x, int baseline, const QString& text) {
    const QFontMetrics metrics(painter.font());
    const int cell = digitCell(metrics);
    for (const QChar ch : text) {
        const int advance = metrics.horizontalAdvance(ch);
        if (ch.isDigit()) {
            painter.drawText(QPoint(x + (cell - advance) / 2, baseline), QString(ch));
            x += cell;
        } else {
            painter.drawText(QPoint(x, baseline), QString(ch));
            x += advance;
        }
    }
}

int baselineIn(const QRect& area, const QFontMetrics& metrics) {
    return area.top() + (area.height() + metrics.ascent() - metrics.descent()) / 2;
}

QString clockText(int seconds) {
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QString modeName(DeckLabSamplerEngine::Mode mode) {
    switch (mode) {
    case DeckLabSamplerEngine::Mode::Loop:
        return tr("loop");
    case DeckLabSamplerEngine::Mode::Gate:
        return tr("plays while held");
    case DeckLabSamplerEngine::Mode::OneShot:
        break;
    }
    return tr("one-shot");
}

QString dbText(double db) {
    if (!util_isfinite(db) || db <= -60) {
        return QStringLiteral("%1∞ dB").arg(kMinus);
    }
    const int whole = static_cast<int>(std::lround(db));
    if (whole == 0) {
        return tr("0 dB");
    }
    return (whole > 0 ? QStringLiteral("+") : QString(kMinus)) + QString::number(std::abs(whole)) + tr(" dB");
}

// The mode as a 14 px icon: one-shot an arrow, loop a circling arrow, gate
// a held bar. Painted, since Barlow has no such glyphs.
void paintModeIcon(QPainter& painter, const QRect& box, DeckLabSamplerEngine::Mode mode, const QColor& ink) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QPointF o(box.left(), box.top());
    QPen pen(ink, 2);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    switch (mode) {
    case DeckLabSamplerEngine::Mode::OneShot:
        painter.drawLine(o + QPointF(1.5, 7), o + QPointF(12, 7));
        painter.drawPolyline(QPolygonF({o + QPointF(8, 3), o + QPointF(12, 7), o + QPointF(8, 11)}));
        break;
    case DeckLabSamplerEngine::Mode::Loop: {
        painter.drawArc(QRectF(o + QPointF(2, 2), QSizeF(10, 10)), 100 * 16, 290 * 16);
        // Arrow head at the arc's start (top), pointing clockwise.
        painter.drawPolyline(QPolygonF({o + QPointF(4.5, 0.5), o + QPointF(7.5, 2.2), o + QPointF(5, 5)}));
        break;
    }
    case DeckLabSamplerEngine::Mode::Gate:
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
        painter.drawRect(QRectF(o + QPointF(4, 1), QSizeF(6, 12)));
        break;
    }
    painter.restore();
}

template<typename T>
void setIf(T* widget, void (T::*setter)(const QString&), QString (T::*getter)() const, const QString& value) {
    if (widget && (widget->*getter)() != value) {
        (widget->*setter)(value);
    }
}

void setCheckedIf(QAbstractButton* button, bool checked) {
    if (button && button->isChecked() != checked) {
        button->setChecked(checked);
    }
}

void setEnabledIf(QWidget* widget, bool enabled) {
    if (widget && widget->isEnabled() != enabled) {
        widget->setEnabled(enabled);
    }
}

// STOP ALL: an outlined key with a stop square over the words.
class DeckLabSamplerStopAll final : public QPushButton {
  public:
    using QPushButton::QPushButton;

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const bool on = isEnabled();
        const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(on ? kText : kOffLine, 1));
        painter.setBrush(!on ? kOffFill : isDown() ? kRaise : kPanel);
        painter.drawRoundedRect(frame, 3, 3);
        painter.setRenderHint(QPainter::Antialiasing, false);
        const QColor ink = on ? kText : kOffInk;
        const Fonts& fonts = Fonts::get();
        painter.setFont(fonts.body);
        const QFontMetrics metrics(fonts.body);
        const int square = 12;
        const int gap = 6;
        const int total = square + gap + metrics.height();
        const int top = (height() - total) / 2;
        painter.fillRect(QRect((width() - square) / 2, top, square, square), ink);
        painter.setPen(ink);
        painter.drawText(QRect(0, top + square + gap, width(), metrics.height()), Qt::AlignCenter, text());
    }
};
} // namespace

// ---------------------------------------------------------------------------
// Pad

DeckLabSamplerPad::DeckLabSamplerPad(QWidget* parent)
        : QPushButton(parent) {
    setCheckable(true);
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    QSizePolicy policy = sizePolicy();
    policy.setRetainSizeWhenHidden(true);
    setSizePolicy(policy);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void DeckLabSamplerPad::setState(const DeckLabSamplerPadState& state) {
    if (state == m_state) {
        return;
    }
    m_state = state;
    const QString text = QString::number(state.number) + QChar(10) + state.title;
    if (this->text() != text) {
        setText(text);
    }
    if (isEnabled() != state.exists) {
        setEnabled(state.exists);
    }
    update();
}

bool DeckLabSamplerPad::setDrain(double remaining, int seconds) {
    remaining = util_isfinite(remaining) ? std::clamp(remaining, 0.0, 1.0) : 1.0;
    const int before = litWidth();
    const bool timeChanged = seconds != m_seconds;
    m_remaining = remaining;
    m_seconds = seconds;
    if (before == litWidth() && !timeChanged) {
        return false;
    }
    update();
    return true;
}

int DeckLabSamplerPad::litWidth() const {
    return static_cast<int>(std::lround(width() * m_remaining));
}

QString DeckLabSamplerPad::timeText(int seconds, bool remaining) {
    if (seconds < 0) {
        return QString();
    }
    return remaining ? QString(kMinus) + clockText(seconds) : clockText(seconds);
}

void DeckLabSamplerPad::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const Fonts& fonts = Fonts::get();
    const QRect area = rect();
    const bool enabled = isEnabled() && m_state.exists;
    const bool loaded = enabled && m_state.loaded;
    const bool lit = enabled && isChecked();
    const int w = width();
    const int h = height();
    // The ground shows in the rounded corners.
    painter.fillRect(area, kGround);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath body;
    body.addRoundedRect(QRectF(area).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    painter.fillPath(body, !enabled ? kOffFill : isDown() ? kRaise : kPanel);
    // Playing: the lit part is the length still to play; it drains to the
    // left. The top bar keeps saying "playing" once the lamp is nearly out.
    const QRect litRect = lit ? QRect(0, 0, litWidth(), h) : QRect();
    if (!litRect.isEmpty()) {
        painter.save();
        painter.setClipRect(litRect);
        painter.fillPath(body, kText);
        painter.restore();
    }
    if (lit) {
        painter.fillRect(QRect(1, 1, w - 2, 3), kText);
    }
    painter.setBrush(Qt::NoBrush);
    if (m_state.selected) {
        // The pad being edited: a 2 px white frame.
        painter.setPen(QPen(kText, 2));
        painter.drawRoundedRect(QRectF(area).adjusted(1, 1, -1, -1), 3, 3);
    } else {
        painter.setPen(QPen(!enabled ? kOffLine : lit ? kText : kLine, 1));
        painter.drawPath(body);
    }
    painter.setRenderHint(QPainter::Antialiasing, false);
    // Every element is drawn in the panel ink outside the lit part and in
    // ground ink inside it, so text stays readable as the lamp drains.
    const QRect unlitRect(litRect.isEmpty() ? 0 : litRect.right() + 1, 0, w, h);
    const auto inked = [&](const QColor& onPanel, auto&& draw) {
        if (litRect.isEmpty()) {
            draw(onPanel);
            return;
        }
        painter.save();
        painter.setClipRect(unlitRect);
        draw(onPanel);
        painter.restore();
        painter.save();
        painter.setClipRect(litRect);
        draw(kGround);
        painter.restore();
    };
    const QRect top(12, 6, w - 22, 20);
    painter.setFont(fonts.body);
    inked(enabled ? kMuted : kOffInk, [&](const QColor& ink) {
        painter.setPen(ink);
        painter.drawText(top, Qt::AlignLeft | Qt::AlignVCenter, QString::number(m_state.number));
    });
    if (!enabled) {
        return;
    }
    if (!loaded) {
        // Empty: the number and a "+"; a tap opens Browse to load it.
        painter.setFont(fonts.plus);
        painter.setPen(isDown() ? kText : kMuted);
        painter.drawText(QRect(0, 22, w, h - 30), Qt::AlignCenter, QStringLiteral("+"));
        return;
    }
    // The pad's gain as a 4 px bar on its left edge (0 dB is half).
    const int barTop = 8;
    const int barSpan = h - 16;
    const int barHeight = std::clamp(m_state.level * barSpan / 1000, 1, barSpan);
    inked(kMuted, [&](const QColor& ink) {
        painter.fillRect(QRect(3, barTop + barSpan - barHeight, 4, barHeight), ink);
    });
    // Mode icon and BPM, top right.
    const QRect icon(w - 10 - 14, top.top() + (top.height() - 14) / 2, 14, 14);
    inked(kMuted, [&](const QColor& ink) {
        paintModeIcon(painter, icon, m_state.mode, ink);
    });
    if (m_state.bpm > 0) {
        painter.setFont(fonts.numeral16);
        const QString bpm = QString::number(m_state.bpm) + tr(" BPM");
        const QRect bpmRect(top.left() + 24, top.top(), icon.left() - 6 - top.left() - 24, top.height());
        inked(kMuted, [&](const QColor& ink) {
            painter.setPen(ink);
            painter.drawText(bpmRect, Qt::AlignRight | Qt::AlignVCenter, bpm);
        });
    }
    // Title: 16 px SemiBold, two lines at most, the second elided.
    painter.setFont(fonts.body);
    const QFontMetrics metrics(fonts.body);
    const QRect titleRect(12, top.bottom() + 3, w - 22, std::max(0, h - 24 - (top.bottom() + 3)));
    const QString name = m_state.title;
    QString first = name;
    QString second;
    if (metrics.horizontalAdvance(name) > titleRect.width() && titleRect.height() >= 2 * metrics.height()) {
        int cut = static_cast<int>(name.size());
        while (cut > 1 && metrics.horizontalAdvance(name.left(cut)) > titleRect.width()) {
            --cut;
        }
        const int space = static_cast<int>(name.left(cut).lastIndexOf(QLatin1Char(' ')));
        if (space > 0) {
            cut = space;
        }
        first = name.left(cut).trimmed();
        second = metrics.elidedText(name.mid(cut).trimmed(), Qt::ElideRight, std::max(0, titleRect.width()));
    } else {
        first = metrics.elidedText(name, Qt::ElideRight, std::max(0, titleRect.width()));
    }
    const QString lines = second.isEmpty() ? first : first + QChar(10) + second;
    inked(kText, [&](const QColor& ink) {
        painter.setPen(ink);
        painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignTop, lines);
    });
    // Bottom row: "⋯" teaches the hold; the time left while playing, the
    // length while idle.
    const QRect bottom(12, h - 23, w - 22, 18);
    inked(kMuted, [&](const QColor& ink) {
        for (int dot = 0; dot < 3; ++dot) {
            painter.fillRect(QRect(bottom.left() + dot * 6, bottom.center().y(), 3, 3), ink);
        }
    });
    const QString time = timeText(m_seconds, lit);
    if (!time.isEmpty()) {
        painter.setFont(lit ? fonts.numeral18 : fonts.numeral16);
        const QFontMetrics timeMetrics(painter.font());
        const int width = tabularWidth(timeMetrics, time);
        const int baseline = baselineIn(bottom, timeMetrics);
        inked(lit ? kText : kMuted, [&](const QColor& ink) {
            painter.setPen(ink);
            drawTabular(painter, bottom.right() - width, baseline, time);
        });
    }
}

// ---------------------------------------------------------------------------
// DECKS strip

DeckLabSamplerDecks::DeckLabSamplerDecks(QWidget* parent)
        : QPushButton(parent) {
    setFocusPolicy(Qt::NoFocus);
    setFixedHeight(kDecksHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setText(tr("DECKS"));
    setAccessibleName(tr("Go to Decks"));
    setAccessibleDescription(tr("Each deck's tempo, beat and time left. Tap to go back to Decks."));
    m_shown = {1, 2};
}

QSize DeckLabSamplerDecks::sizeHint() const {
    return QSize(800, kDecksHeight);
}

int DeckLabSamplerDecks::linkWidth() const {
    return m_shown.size() > 2 ? kCompactLinkWidth : kLinkWidth;
}

QList<int> DeckLabSamplerDecks::shownDecks() const {
    return m_shown;
}

QRect DeckLabSamplerDecks::cellRect(int deck) const {
    const int index = static_cast<int>(m_shown.indexOf(deck));
    if (index < 0) {
        return QRect();
    }
    const int available = std::max(0, width() - linkWidth());
    const int cell = available / static_cast<int>(m_shown.size());
    const int left = index * cell;
    const int right = index == m_shown.size() - 1 ? available : left + cell;
    return QRect(left, 0, right - left, height());
}

DeckLabSamplerDecks::Cell DeckLabSamplerDecks::cellFor(int deck, int width) const {
    const DeckLabSamplerDeck& d = m_decks[deck - 1];
    Cell cell;
    cell.loaded = d.loaded;
    if (!d.loaded) {
        return cell;
    }
    cell.playing = d.playing;
    cell.master = d.master;
    cell.endWarning = d.endWarning && d.playing;
    cell.bpmTenths = util_isfinite(d.bpm) && d.bpm > 0 ? static_cast<int>(std::lround(d.bpm * 10)) : 0;
    cell.seconds = util_isfinite(d.remaining) && d.remaining >= 0 ? static_cast<int>(d.remaining) : -1;
    cell.phase = d.phase;
    const double position = util_isfinite(d.position) ? std::clamp(d.position, 0.0, 1.0) : 0.0;
    cell.progress = static_cast<int>(std::lround(position * std::max(0, width - 16)));
    return cell;
}

void DeckLabSamplerDecks::setDecks(const std::array<DeckLabSamplerDeck, 4>& decks) {
    m_decks = decks;
    // Decks 1 and 2 always; 3 and 4 while loaded. Built without allocating
    // unless the set of cells changes.
    std::array<int, 4> shown{};
    int count = 0;
    for (int deck = 1; deck <= 4; ++deck) {
        if (deck <= 2 || decks[deck - 1].loaded) {
            shown[count++] = deck;
        }
    }
    bool same = count == m_shown.size();
    for (int index = 0; same && index < count; ++index) {
        same = m_shown[index] == shown[index];
    }
    if (!same) {
        m_shown.clear();
        for (int index = 0; index < count; ++index) {
            m_shown.append(shown[index]);
        }
        for (int deck = 1; deck <= 4; ++deck) {
            m_cells[deck - 1] = cellFor(deck, cellRect(deck).width());
        }
        update();
        return;
    }
    for (int index = 0; index < count; ++index) {
        const int deck = shown[index];
        const QRect area = cellRect(deck);
        const Cell cell = cellFor(deck, area.width());
        if (cell == m_cells[deck - 1]) {
            continue;
        }
        m_cells[deck - 1] = cell;
        update(area);
    }
}

void DeckLabSamplerDecks::paintCell(QPainter& painter, int deck, const QRect& area) const {
    const Cell& cell = m_cells[deck - 1];
    const Fonts& fonts = Fonts::get();
    const QColor color = deckLabDeckColor(deck);
    const bool wide = area.width() >= 300;
    const int bodyTop = 0;
    const int bodyHeight = area.height() - 4; // the position line sits below
    int x = area.left() + 8;
    // Badge: the deck number on its color, as on the Decks readout. Four
    // decks share the strip in compact cells: tighter, so MASTER still fits
    // (as "M").
    const int badgeSize = wide ? 26 : 24;
    const QRect badge(x, bodyTop + (bodyHeight - badgeSize) / 2 + 1, badgeSize, badgeSize);
    painter.fillRect(badge, cell.loaded ? color : kLine);
    painter.setFont(fonts.badge);
    painter.setPen(cell.loaded ? kGround : kMuted);
    painter.drawText(badge, Qt::AlignCenter, QString::number(deck));
    x = badge.right() + 1 + (wide ? 8 : 6);
    // Divider to the next cell.
    painter.fillRect(QRect(area.right(), 8, 1, area.height() - 16), kLine);
    const QRect body(x, bodyTop, area.right() - 8 - x, bodyHeight);
    if (!cell.loaded) {
        painter.setFont(fonts.medium);
        painter.setPen(kMuted);
        painter.drawText(body, Qt::AlignLeft | Qt::AlignVCenter, tr("Empty"));
        return;
    }
    const QColor ink = cell.playing ? kText : kMuted;
    if (wide) {
        // Green play triangle while playing.
        if (cell.playing) {
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing, true);
            QPainterPath play;
            const QPointF origin(x, body.center().y() - 7 + 1);
            play.moveTo(origin);
            play.lineTo(origin.x(), origin.y() + 14);
            play.lineTo(origin.x() + 12, origin.y() + 7);
            play.closeSubpath();
            painter.fillPath(play, kPlay);
            painter.restore();
        }
        x += 12 + 8;
    }
    // BPM: wide cells show one decimal (dimmer), compact cells the rounded
    // whole tempo.
    const QFont& bpmFont = wide ? fonts.numeral24 : fonts.numeral18;
    const QFontMetrics bpmMetrics(bpmFont);
    const int baseline = baselineIn(body, bpmMetrics);
    painter.setFont(bpmFont);
    painter.setPen(ink);
    QString whole = cell.bpmTenths > 0 ? QString::number(wide ? cell.bpmTenths / 10 : (cell.bpmTenths + 5) / 10)
                                       : QStringLiteral("—");
    const int wholeBox = tabularWidth(bpmMetrics, QStringLiteral("000"));
    const int wholeWidth = tabularWidth(bpmMetrics, whole);
    drawTabular(painter, x + wholeBox - wholeWidth, baseline, whole);
    x += wholeBox;
    if (wide) {
        const QFontMetrics decimals(fonts.numeral18);
        painter.setFont(fonts.numeral18);
        painter.setPen(cell.playing ? kDecimals : kMuted);
        const QString fraction = cell.bpmTenths > 0 ? QStringLiteral(".%1").arg(cell.bpmTenths % 10) : QString();
        drawTabular(painter, x, baseline, fraction);
        x += tabularWidth(decimals, QStringLiteral(".0"));
    }
    x += wide ? 12 : 6;
    // Four-beat lamp: the current beat in the deck color (dimmer while
    // paused); no grid, no lamp.
    const int lamp = wide ? 14 : 7;
    const int gap = wide ? 4 : 2;
    const int lampHeight = 10;
    const int lampTop = body.center().y() - lampHeight / 2 + 1;
    for (int beat = 0; beat < 4; ++beat) {
        QColor fill = kLine;
        if (cell.phase < 0) {
            fill = kPanel;
        } else if (beat == cell.phase) {
            fill = cell.playing ? color : color.darker(170);
        }
        painter.fillRect(QRect(x + beat * (lamp + gap), lampTop, lamp, lampHeight), fill);
    }
    x += 4 * lamp + 3 * gap;
    // Time left, right aligned; live red inside the end warning.
    const QFont& timeFont = wide ? fonts.numeral24 : fonts.numeral18;
    const QFontMetrics timeMetrics(timeFont);
    const QString time = cell.seconds >= 0 ? QString(kMinus) + clockText(cell.seconds) : QStringLiteral("—");
    const int timeWidth = tabularWidth(timeMetrics, time);
    const int timeLeft = body.right() - timeWidth;
    painter.setFont(timeFont);
    painter.setPen(cell.endWarning ? kLive : ink);
    drawTabular(painter, timeLeft, baselineIn(body, timeMetrics), time);
    // MASTER: a filled chip in the deck color ("M" in a compact cell),
    // where there is room.
    if (cell.master) {
        const QFontMetrics label(fonts.label);
        const QString text = wide ? tr("MASTER") : tr("M");
        const int chipWidth = label.horizontalAdvance(text) + (wide ? 12 : 8);
        const int chipLeft = x + (wide ? 10 : 5);
        if (chipLeft + chipWidth + (wide ? 8 : 4) <= timeLeft) {
            const QRect chip(chipLeft, body.center().y() - 10 + 1, chipWidth, 20);
            painter.fillRect(chip, color);
            painter.setFont(fonts.label);
            painter.setPen(kGround);
            painter.drawText(chip, Qt::AlignCenter, text);
        }
    }
    // Position in the track along the bottom of the cell.
    if (cell.progress >= 0) {
        const QRect track(area.left() + 8, area.bottom() - 3, area.width() - 16, 2);
        painter.fillRect(track, kLine);
        painter.fillRect(QRect(track.left(), track.top(), cell.progress, 2), cell.playing ? color : color.darker(170));
    }
}

void DeckLabSamplerDecks::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    const QColor background = isDown() ? kRaise : kGround;
    painter.fillRect(event->rect(), background);
    for (const int deck : std::as_const(m_shown)) {
        const QRect area = cellRect(deck);
        if (area.intersects(event->rect())) {
            painter.save();
            painter.setClipRect(area);
            paintCell(painter, deck, area);
            painter.restore();
        }
    }
    // "DECKS ›": the way back (the chevron alone beside four decks; the
    // whole strip takes the tap either way).
    const QRect link(width() - linkWidth(), 0, linkWidth(), height() - 1);
    if (link.intersects(event->rect())) {
        const Fonts& fonts = Fonts::get();
        painter.fillRect(QRect(link.left(), 8, 1, link.height() - 16), kLine);
        painter.setFont(fonts.label);
        painter.setPen(kText);
        const QFontMetrics label(fonts.label);
        const QString text = linkWidth() == kLinkWidth ? tr("DECKS") : QString();
        const int textWidth = text.isEmpty() ? 0 : label.horizontalAdvance(text);
        const int total = textWidth + (text.isEmpty() ? 0 : 6) + 6;
        const int left = link.left() + (link.width() - total) / 2;
        if (!text.isEmpty()) {
            painter.drawText(QRect(left, link.top(), textWidth + 2, link.height()), Qt::AlignLeft | Qt::AlignVCenter, text);
        }
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(kText, 2);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        const QPointF tip(left + textWidth + (text.isEmpty() ? 0 : 6) + 5, link.center().y() + 0.5);
        painter.drawPolyline(QPolygonF({tip + QPointF(-4, -5), tip, tip + QPointF(-4, 5)}));
        painter.restore();
    }
    painter.fillRect(QRect(0, height() - 1, width(), 1), kLine);
}

// ---------------------------------------------------------------------------
// LEVEL

DeckLabSamplerLevel::DeckLabSamplerLevel(QWidget* parent)
        : QAbstractSlider(parent) {
    setOrientation(Qt::Vertical);
    setRange(0, 100);
    setSingleStep(1);
    setPageStep(5);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumHeight(80);
}

void DeckLabSamplerLevel::setValueText(const QString& text) {
    if (text == m_text) {
        return;
    }
    m_text = text;
    update();
}

QRect DeckLabSamplerLevel::track() const {
    return QRect(0, 30, width(), std::max(1, height() - 60));
}

void DeckLabSamplerLevel::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const Fonts& fonts = Fonts::get();
    painter.fillRect(rect(), kGround);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(kLine, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    painter.setRenderHint(QPainter::Antialiasing, false);
    // 14 px label on the ground; the value in figures below.
    painter.setFont(fonts.label);
    painter.setPen(kText);
    painter.drawText(QRect(0, 6, width(), 20), Qt::AlignCenter, tr("LEVEL"));
    painter.setFont(fonts.numeral16);
    painter.drawText(QRect(0, height() - 26, width(), 20), Qt::AlignCenter, m_text);
    const QRect area = track().adjusted(0, 6, 0, -6);
    const int span = maximum() - minimum();
    const double fraction = span > 0 ? double(value() - minimum()) / span : 0;
    const int y = area.bottom() - static_cast<int>(std::lround(fraction * area.height()));
    const int groove = 6;
    const QRect grooveRect(area.center().x() - groove / 2, area.top(), groove, area.height());
    painter.fillRect(grooveRect, kLine);
    painter.fillRect(QRect(grooveRect.left(), y, groove, area.bottom() - y + 1), kText);
    const QRect handle(area.center().x() - 20, y - 6, 40, 12);
    painter.fillRect(handle, isSliderDown() ? kMuted : kText);
    painter.fillRect(QRect(handle.left() + 6, handle.center().y(), handle.width() - 12, 1), kGround);
}

void DeckLabSamplerLevel::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    m_dragging = true;
    m_pressY = static_cast<int>(event->position().y());
    m_pressValue = value();
    setSliderDown(true);
    update();
}

void DeckLabSamplerLevel::mouseMoveEvent(QMouseEvent* event) {
    if (!m_dragging) {
        return;
    }
    const int span = std::max(1, track().height() - 12);
    const double delta = double(m_pressY - event->position().y()) / span * (maximum() - minimum());
    setValue(m_pressValue + static_cast<int>(std::lround(delta)));
}

void DeckLabSamplerLevel::mouseReleaseEvent(QMouseEvent*) {
    if (!m_dragging) {
        return;
    }
    m_dragging = false;
    setSliderDown(false);
    update();
}

// ---------------------------------------------------------------------------
// Page

DeckLabSampler::DeckLabSampler(DeckLabSamplerEngine* engine, QWidget* parent)
        : QWidget(parent),
          m_engine(engine) {
    setObjectName(QStringLiteral("decklab-sampler"));
    setAttribute(Qt::WA_StyledBackground);
    setStyleSheet(kSamplerStyle);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_decks = new DeckLabSamplerDecks(this);
    m_decks->setObjectName(QStringLiteral("decklab-sampler-decks"));
    connect(m_decks, &QPushButton::pressed, this, [this] { endGate(); });
    connect(m_decks, &QPushButton::clicked, this, [this] { emit decksRequested(); });
    layout->addWidget(m_decks);
    auto* body = new QWidget(this);
    auto* row = new QHBoxLayout(body);
    row->setContentsMargins(4, 4, 4, 4);
    row->setSpacing(4);
    auto* grid = new QWidget(body);
    grid->setObjectName(QStringLiteral("decklab-sampler-grid"));
    m_grid = new QGridLayout(grid);
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setSpacing(4);
    for (int index = 0; index < kPads; ++index) {
        auto* pad = new DeckLabSamplerPad(grid);
        pad->setObjectName(QStringLiteral("decklab-sample-%1").arg(index + 1));
        connect(pad, &QPushButton::pressed, this, [this, index] { padPressed(index); });
        connect(pad, &QPushButton::released, this, [this, index] { padReleased(index); });
        connect(pad, &QPushButton::clicked, this, [this, index] { padClicked(index); });
        m_pads[index] = pad;
        m_grid->addWidget(pad, index / 4, index % 4);
    }
    for (int line = 0; line < 4; ++line) {
        m_grid->setColumnStretch(line, 1);
        m_grid->setRowStretch(line, 1);
    }
    m_sheet = buildSheet();
    m_sheet->setParent(grid);
    m_grid->addWidget(m_sheet, 2, 0, 2, 4);
    m_sheetRow = 2;
    m_sheet->hide();
    row->addWidget(grid, 1);
    row->addWidget(buildSide());
    layout->addWidget(body, 1);
    m_hold.setSingleShot(true);
    m_hold.setInterval(kHoldMs);
    connect(&m_hold, &QTimer::timeout, this, [this] { holdElapsed(); });
    m_engine->setListener([this](int sampler) { samplerChanged(sampler); });
    refreshBanks();
    refreshPads();
    refreshSide();
}

DeckLabSampler::~DeckLabSampler() {
    m_engine->setListener({});
}

QWidget* DeckLabSampler::buildSide() {
    auto* side = new QWidget(this);
    side->setObjectName(QStringLiteral("decklab-sampler-side"));
    side->setFixedWidth(kSideWidth);
    auto* column = new QVBoxLayout(side);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(4);
    const auto key = [this, side](const QString& text, const QString& name, const QString& accessible,
                             const QString& description) {
        auto* b = new QPushButton(text, side);
        b->setObjectName(name);
        b->setFocusPolicy(Qt::NoFocus);
        b->setCheckable(true);
        b->setAccessibleName(accessible);
        b->setAccessibleDescription(description);
        b->setToolTip(description);
        b->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        // A touch anywhere else ends a gate whose release went astray.
        connect(b, &QPushButton::pressed, this, [this] { endGate(); });
        return b;
    };
    m_quantize = key(tr("QNT"), QStringLiteral("decklab-sampler-qnt"), tr("Quantize pads"),
            tr("Pads start in phase with the master beat. Needs a BPM."));
    m_sync = key(tr("SYNC"), QStringLiteral("decklab-sampler-sync"), tr("Sync loop pads"),
            tr("Loop pads follow the master tempo."));
    m_edit = key(tr("EDIT"), QStringLiteral("decklab-sampler-edit"), tr("Edit pads"),
            tr("Tap a pad to edit it instead of playing it. You can also hold a pad."));
    m_fx = key(tr("FX"), QStringLiteral("decklab-sampler-fx"), tr("Send pads 1 to 4 through Lab FX"),
            tr("Pads 1 to 4 go through Lab FX while it is on."));
    connect(m_quantize, &QPushButton::clicked, this, [this] {
        m_engine->setQuantize(!m_engine->quantize());
        refreshSide();
    });
    connect(m_sync, &QPushButton::clicked, this, [this] {
        m_engine->setSyncLoops(!m_engine->syncLoops());
        refreshSide();
    });
    connect(m_edit, &QPushButton::clicked, this, [this] {
        if (m_editMode || sheetOpen()) {
            closeSheet();
        } else {
            m_editMode = true;
        }
        refreshSide();
    });
    connect(m_fx, &QPushButton::clicked, this, [this] {
        m_fxRouting = !m_fxRouting;
        refreshSide();
        emit fxRoutingRequested(m_fxRouting);
    });
    auto* toggles = new QGridLayout;
    toggles->setContentsMargins(0, 0, 0, 0);
    toggles->setSpacing(4);
    toggles->addWidget(m_quantize, 0, 0);
    toggles->addWidget(m_sync, 0, 1);
    toggles->addWidget(m_edit, 1, 0);
    toggles->addWidget(m_fx, 1, 1);
    column->addLayout(toggles);
    m_level = new DeckLabSamplerLevel(side);
    m_level->setObjectName(QStringLiteral("decklab-sampler-level"));
    m_level->setAccessibleName(tr("All pads level"));
    connect(m_level, &QAbstractSlider::sliderPressed, this, [this] { endGate(); });
    connect(m_level, &QAbstractSlider::valueChanged, this, [this](int value) {
        m_engine->setLevel(value / 100.0);
        m_shownLevel = -1;
        refreshSide();
    });
    column->addWidget(m_level, 1);
    m_bankButton = new QPushButton(side);
    m_bankButton->setObjectName(QStringLiteral("decklab-sampler-bank"));
    m_bankButton->setFocusPolicy(Qt::NoFocus);
    m_bankButton->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    connect(m_bankButton, &QPushButton::pressed, this, [this] { endGate(); });
    connect(m_bankButton, &QPushButton::clicked, this, [this] {
        closeSheet();
        m_bank = (m_bank + 1) % std::max(1, m_banks);
        refreshBanks();
        refreshPads();
        refreshSide();
    });
    column->addWidget(m_bankButton);
    m_stopAll = new DeckLabSamplerStopAll(tr("STOP ALL"), side);
    m_stopAll->setObjectName(QStringLiteral("decklab-samples-stop"));
    m_stopAll->setFocusPolicy(Qt::NoFocus);
    m_stopAll->setAccessibleName(tr("Stop all samples"));
    m_stopAll->setFixedHeight(60);
    m_stopAll->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    connect(m_stopAll, &QPushButton::pressed, this, [this] { endGate(); });
    connect(m_stopAll, &QPushButton::clicked, this, [this] {
        const int last = std::max(kPads, m_engine->count());
        for (int sampler = 1; sampler <= last; ++sampler) {
            if (m_engine->playing(sampler)) {
                m_engine->stop(sampler);
            }
        }
        refreshSide();
    });
    column->addWidget(m_stopAll);
    return side;
}

QWidget* DeckLabSampler::buildSheet() {
    auto* sheet = new QWidget(this);
    sheet->setObjectName(QStringLiteral("decklab-sampler-sheet"));
    sheet->setAttribute(Qt::WA_StyledBackground);
    sheet->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    auto* column = new QVBoxLayout(sheet);
    column->setContentsMargins(12, 10, 12, 10);
    column->setSpacing(8);
    const auto button = [this, sheet](const QString& text, const QString& name) {
        auto* b = new QPushButton(text, sheet);
        b->setObjectName(name);
        b->setFocusPolicy(Qt::NoFocus);
        b->setAccessibleName(text);
        connect(b, &QPushButton::pressed, this, [this] { endGate(); });
        return b;
    };
    const auto label = [sheet](const QString& text, const char* role) {
        auto* l = new QLabel(text, sheet);
        if (role) {
            l->setProperty(role, true);
        }
        return l;
    };
    // Identity and DONE.
    auto* identity = new QHBoxLayout;
    identity->setSpacing(10);
    m_sheetNumber = label(QString(), "muted");
    m_sheetNumber->setObjectName(QStringLiteral("decklab-sampler-sheet-number"));
    identity->addWidget(m_sheetNumber);
    m_sheetTitle = label(QString(), "heading");
    m_sheetTitle->setObjectName(QStringLiteral("decklab-sampler-sheet-title"));
    m_sheetTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    identity->addWidget(m_sheetTitle, 1);
    auto* done = button(tr("DONE"), QStringLiteral("decklab-sampler-sheet-done"));
    done->setMinimumWidth(96);
    connect(done, &QPushButton::clicked, this, [this] { closeSheet(); });
    identity->addWidget(done);
    column->addLayout(identity);
    column->addStretch(1);
    // MODE and GAIN.
    auto* controls = new QHBoxLayout;
    controls->setSpacing(4);
    controls->addWidget(label(tr("MODE"), "muted"));
    controls->addSpacing(6);
    const std::array<QString, 3> modes{tr("ONE-SHOT"), tr("LOOP"), tr("GATE")};
    for (int mode = 0; mode < 3; ++mode) {
        auto* b = button(modes[mode], QStringLiteral("decklab-sampler-sheet-mode-%1").arg(mode));
        b->setCheckable(true);
        b->setMinimumWidth(100);
        b->setAccessibleDescription(modeName(static_cast<DeckLabSamplerEngine::Mode>(mode)));
        connect(b, &QPushButton::clicked, this, [this, mode] {
            const int sampler = sheetSampler();
            if (sampler > 0 && m_engine->loaded(sampler)) {
                m_engine->setMode(sampler, static_cast<DeckLabSamplerEngine::Mode>(mode));
            }
            refreshSheet();
            if (m_sheetPad >= 0) {
                refreshPad(m_sheetPad);
            }
        });
        m_sheetModes[mode] = b;
        controls->addWidget(b);
    }
    controls->addStretch(1);
    controls->addWidget(label(tr("GAIN"), "muted"));
    controls->addSpacing(6);
    auto* down = button(QString(kMinus), QStringLiteral("decklab-sampler-sheet-gain-down"));
    down->setAccessibleName(tr("Gain down 1 dB"));
    down->setProperty("glyph", true);
    down->setFixedWidth(48);
    down->setAutoRepeat(true);
    down->setAutoRepeatDelay(400);
    down->setAutoRepeatInterval(150);
    connect(down, &QPushButton::clicked, this, [this] { stepGain(-1); });
    controls->addWidget(down);
    m_sheetGain = label(QString(), "readout");
    m_sheetGain->setObjectName(QStringLiteral("decklab-sampler-sheet-gain"));
    m_sheetGain->setAlignment(Qt::AlignCenter);
    m_sheetGain->setFixedWidth(92);
    controls->addWidget(m_sheetGain);
    auto* up = button(QStringLiteral("+"), QStringLiteral("decklab-sampler-sheet-gain-up"));
    up->setAccessibleName(tr("Gain up 1 dB"));
    up->setProperty("glyph", true);
    up->setFixedWidth(48);
    up->setAutoRepeat(true);
    up->setAutoRepeatDelay(400);
    up->setAutoRepeatInterval(150);
    connect(up, &QPushButton::clicked, this, [this] { stepGain(1); });
    controls->addWidget(up);
    column->addLayout(controls);
    column->addStretch(1);
    // STOP, the knob hint, CLEAR (destructive: on release, never while
    // playing, at the far end from STOP).
    auto* actions = new QHBoxLayout;
    actions->setSpacing(8);
    m_sheetStop = button(tr("STOP"), QStringLiteral("decklab-sampler-sheet-stop"));
    m_sheetStop->setMinimumWidth(120);
    connect(m_sheetStop, &QPushButton::clicked, this, [this] {
        const int sampler = sheetSampler();
        if (sampler > 0) {
            m_engine->stop(sampler);
        }
        refreshSheet();
    });
    actions->addWidget(m_sheetStop);
    auto* hint = label(tr("Knob: gain · Back: close"), "muted");
    hint->setObjectName(QStringLiteral("decklab-sampler-sheet-hint"));
    hint->setAlignment(Qt::AlignCenter);
    hint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    actions->addWidget(hint, 1);
    m_sheetClear = button(tr("CLEAR"), QStringLiteral("decklab-sampler-sheet-clear"));
    m_sheetClear->setMinimumWidth(120);
    connect(m_sheetClear, &QPushButton::clicked, this, [this] {
        const int sampler = sheetSampler();
        if (sampler > 0 && m_engine->loaded(sampler) && !m_engine->playing(sampler)) {
            m_engine->eject(sampler);
            closeSheet();
        } else {
            refreshSheet();
        }
    });
    actions->addWidget(m_sheetClear);
    column->addLayout(actions);
    return sheet;
}

void DeckLabSampler::setInteractionEpoch(std::function<quint64()> epoch) {
    m_epoch = std::move(epoch);
}

quint64 DeckLabSampler::epoch() const {
    return (m_epoch ? m_epoch() : 0) + m_hides;
}

bool DeckLabSampler::exists(int sampler) const {
    return sampler >= 1 && sampler <= std::max(kPads, m_engine->count());
}

bool DeckLabSampler::anyPlaying() const {
    const int last = std::max(kPads, m_engine->count());
    for (int sampler = 1; sampler <= last; ++sampler) {
        if (m_engine->playing(sampler)) {
            return true;
        }
    }
    return false;
}

void DeckLabSampler::padPressed(int index) {
    if (m_gatePad >= 0 && m_gatePad != index) {
        endGate();
    }
    m_hold.stop();
    m_holdPad = -1;
    const int sampler = samplerAt(index);
    Press& press = m_presses[index];
    press.valid = true;
    press.epoch = epoch();
    press.loaded = exists(sampler) && m_engine->loaded(sampler);
    if (!press.loaded) {
        // Empty: Browse opens on release, never under a finger still down.
        return;
    }
    if (m_editMode) {
        openSheet(index);
        return;
    }
    // Loaded pads act on touch-down.
    switch (m_engine->mode(sampler)) {
    case DeckLabSamplerEngine::Mode::Gate:
        m_engine->trigger(sampler);
        m_gatePad = index;
        m_gateEpoch = press.epoch;
        // A gate is held to play, so holding it never opens the sheet.
        return;
    case DeckLabSamplerEngine::Mode::Loop:
        if (m_engine->playing(sampler)) {
            m_engine->stop(sampler);
        } else {
            m_engine->trigger(sampler);
        }
        break;
    case DeckLabSamplerEngine::Mode::OneShot:
        m_engine->trigger(sampler);
        break;
    }
    m_holdPad = index;
    m_hold.start();
}

void DeckLabSampler::padReleased(int index) {
    if (m_holdPad == index) {
        m_hold.stop();
        m_holdPad = -1;
    }
    if (m_gatePad == index) {
        endGate();
    }
}

void DeckLabSampler::padClicked(int index) {
    const Press press = std::exchange(m_presses[index], Press{});
    const int sampler = samplerAt(index);
    if (press.valid && !press.loaded && press.epoch == epoch() && exists(sampler) &&
            !m_engine->loaded(sampler)) {
        emit loadRequested(sampler);
        return;
    }
    refreshPad(index);
}

void DeckLabSampler::holdElapsed() {
    const int index = std::exchange(m_holdPad, -1);
    if (index < 0 || !m_pads[index]->isDown() || m_presses[index].epoch != epoch()) {
        return;
    }
    openSheet(index);
}

void DeckLabSampler::endGate() {
    const int index = std::exchange(m_gatePad, -1);
    if (index >= 0) {
        m_engine->stop(samplerAt(index));
    }
}

void DeckLabSampler::openSheet(int index) {
    if (index < 0 || index >= kPads) {
        return;
    }
    const int previous = std::exchange(m_sheetPad, index);
    if (previous >= 0 && previous != index) {
        refreshPad(previous);
    }
    // Docked over the half of the grid away from the pad, so the pad and
    // the finger on it stay in view.
    const int firstRow = index / 4 < 2 ? 2 : 0;
    if (firstRow != m_sheetRow) {
        m_grid->removeWidget(m_sheet);
        m_grid->addWidget(m_sheet, firstRow, 0, 2, 4);
        m_sheetRow = firstRow;
    }
    for (int pad = 0; pad < kPads; ++pad) {
        const int padRow = pad / 4;
        m_pads[pad]->setVisible(padRow < firstRow || padRow >= firstRow + 2);
    }
    m_sheet->show();
    m_sheet->raise();
    refreshPad(index);
    refreshSheet();
    refreshSide();
}

void DeckLabSampler::closeSheet() {
    const int previous = std::exchange(m_sheetPad, -1);
    m_editMode = false;
    // isHidden(), not isVisible(): this also runs while the page hides.
    if (!m_sheet->isHidden()) {
        m_sheet->hide();
    }
    for (auto* pad : m_pads) {
        if (pad->isHidden()) {
            pad->show();
        }
    }
    if (previous >= 0) {
        refreshPad(previous);
    }
    refreshSide();
}

bool DeckLabSampler::sheetOpen() const {
    return m_sheetPad >= 0;
}

int DeckLabSampler::sheetSampler() const {
    return m_sheetPad >= 0 ? samplerAt(m_sheetPad) : 0;
}

void DeckLabSampler::showPad(int sampler) {
    if (sampler < 1 || !exists(sampler)) {
        return;
    }
    const int bank = (sampler - 1) / kPads;
    if (bank == m_bank) {
        return;
    }
    closeSheet();
    m_bank = bank;
    refreshBanks();
    refreshPads();
    refreshSide();
}

void DeckLabSampler::editPad(int sampler) {
    if (sampler < 1 || !exists(sampler)) {
        return;
    }
    showPad(sampler);
    const int index = sampler - 1 - m_bank * kPads;
    if (m_engine->loaded(sampler)) {
        openSheet(index);
    }
}

bool DeckLabSampler::knob(decklab::KnobAction action, double steps) {
    if (!sheetOpen()) {
        return false;
    }
    switch (action) {
    case decklab::KnobAction::Move:
        if (util_isfinite(steps) && steps != 0) {
            stepGain(std::clamp(std::round(steps), -24.0, 24.0));
        }
        return true;
    case decklab::KnobAction::Press:
    case decklab::KnobAction::Back:
        closeSheet();
        return true;
    case decklab::KnobAction::Enter:
        return true;
    }
    return true;
}

void DeckLabSampler::stepGain(double db) {
    const int sampler = sheetSampler();
    if (sampler <= 0 || !m_engine->loaded(sampler)) {
        return;
    }
    const double now = std::round(m_engine->gainDb(sampler));
    m_engine->setGainDb(sampler, std::clamp(now + db, -12.0, 12.0));
    refreshSheet();
    refreshPad(m_sheetPad);
}

void DeckLabSampler::tick(const std::array<DeckLabSamplerDeck, 4>& decks) {
    m_decks->setDecks(decks);
    // Only playing pads (and a pad that just stopped) repaint.
    for (int index = 0; index < kPads; ++index) {
        const int sampler = samplerAt(index);
        const bool playing = m_pads[index]->state().loaded && m_engine->playing(sampler);
        if (playing != m_pads[index]->isChecked()) {
            refreshPad(index);
        } else if (playing) {
            refreshDrain(index);
        }
    }
    // A gate whose release went astray (touch cancel, page change) stops.
    if (m_gatePad >= 0 && (!m_pads[m_gatePad]->isDown() || m_gateEpoch != epoch())) {
        endGate();
    }
    if (m_sheetPad >= 0) {
        refreshSheetState();
    }
    refreshSide();
}

void DeckLabSampler::setFxRouting(bool on) {
    if (on == m_fxRouting) {
        return;
    }
    m_fxRouting = on;
    setCheckedIf(m_fx, on);
}

void DeckLabSampler::samplerChanged(int sampler) {
    if (!isVisible()) {
        // No work on a hidden page: everything refreshes when it shows.
        m_dirty = true;
        return;
    }
    if (sampler <= 0) {
        refreshBanks();
        refreshPads();
        refreshSide();
        return;
    }
    const int index = sampler - 1 - m_bank * kPads;
    if (index >= 0 && index < kPads) {
        refreshPad(index);
    }
    if (sampler == sheetSampler()) {
        refreshSheet();
    }
    setEnabledIf(m_stopAll, anyPlaying());
}

void DeckLabSampler::refreshPads() {
    for (int index = 0; index < kPads; ++index) {
        refreshPad(index);
    }
}

void DeckLabSampler::refreshPad(int index) {
    if (index < 0 || index >= kPads) {
        return;
    }
    auto* pad = m_pads[index];
    const int sampler = samplerAt(index);
    DeckLabSamplerPadState state;
    state.number = sampler;
    state.exists = exists(sampler);
    state.loaded = state.exists && m_engine->loaded(sampler);
    state.selected = index == m_sheetPad;
    if (state.loaded) {
        state.title = m_engine->title(sampler).trimmed();
        if (state.title.isEmpty()) {
            state.title = tr("Untitled");
        }
        state.mode = m_engine->mode(sampler);
        const double bpm = m_engine->bpm(sampler);
        state.bpm = util_isfinite(bpm) && bpm > 0 ? static_cast<int>(std::lround(bpm)) : 0;
        const double gain = m_engine->gainDb(sampler);
        state.level = util_isfinite(gain)
                ? static_cast<int>(std::lround(std::clamp((gain + 12) / 24, 0.0, 1.0) * 1000))
                : 500;
    }
    const bool playing = state.loaded && m_engine->playing(sampler);
    pad->setState(state);
    setCheckedIf(pad, playing);
    refreshDrain(index);
    // Spoken names change with what is loaded and played, never per tick.
    const QString name = !state.exists ? tr("Pad %1, not available").arg(sampler)
            : !state.loaded            ? tr("Pad %1, empty: tap to load").arg(sampler)
                                       : tr("Pad %1, %2, %3").arg(sampler).arg(state.title, modeName(state.mode)) +
                    (playing ? tr(", playing") : QString());
    setIf<QWidget>(pad, &QWidget::setAccessibleName, &QWidget::accessibleName, name);
    const QString description = !state.loaded ? tr("Tap to load from Browse")
            : state.mode == DeckLabSamplerEngine::Mode::Gate
            ? tr("Plays while held. Turn EDIT on to edit.")
            : state.mode == DeckLabSamplerEngine::Mode::Loop
            ? tr("Touch to start, touch again to stop. Hold a pad to edit.")
            : tr("Touch to play from the top. Hold a pad to edit.");
    setIf<QWidget>(pad, &QWidget::setAccessibleDescription, &QWidget::accessibleDescription, description);
    setIf<QWidget>(pad, &QWidget::setToolTip, &QWidget::toolTip, state.loaded ? state.title : description);
}

void DeckLabSampler::refreshDrain(int index) {
    auto* pad = m_pads[index];
    if (!pad->state().loaded) {
        pad->setDrain(1, -1);
        return;
    }
    const int sampler = samplerAt(index);
    const double duration = m_engine->duration(sampler);
    const bool known = util_isfinite(duration) && duration > 0;
    if (!pad->isChecked()) {
        pad->setDrain(1, known ? static_cast<int>(std::lround(duration)) : -1);
        return;
    }
    const double raw = m_engine->position(sampler);
    const double position = util_isfinite(raw) ? std::clamp(raw, 0.0, 1.0) : 0.0;
    const double rawRate = m_engine->rate(sampler);
    const double rate = util_isfinite(rawRate) && rawRate > 0 ? rawRate : 1.0;
    const double left = known ? duration * (1 - position) / rate : -1;
    pad->setDrain(known ? 1 - position : 1, known ? static_cast<int>(std::ceil(left - 1e-6)) : -1);
}

void DeckLabSampler::refreshSheet() {
    if (m_sheetPad < 0) {
        return;
    }
    const int sampler = sheetSampler();
    const bool loaded = m_engine->loaded(sampler);
    if (!loaded) {
        // Ejected (here or from a controller): nothing left to edit.
        closeSheet();
        return;
    }
    setIf<QLabel>(m_sheetNumber, &QLabel::setText, &QLabel::text, tr("PAD %1").arg(sampler));
    const QString title = m_engine->title(sampler).trimmed();
    const QFontMetrics metrics(uiFont(18, QFont::DemiBold));
    setIf<QLabel>(m_sheetTitle,
            &QLabel::setText,
            &QLabel::text,
            metrics.elidedText(title.isEmpty() ? tr("Untitled") : title,
                    Qt::ElideRight,
                    std::max(40, m_sheetTitle->width())));
    const auto mode = static_cast<int>(m_engine->mode(sampler));
    for (int index = 0; index < 3; ++index) {
        setCheckedIf(m_sheetModes[index], index == mode);
    }
    setIf<QLabel>(m_sheetGain, &QLabel::setText, &QLabel::text, dbText(m_engine->gainDb(sampler)));
    refreshSheetState();
}

void DeckLabSampler::refreshSheetState() {
    // On the tick while the sheet is open: STOP and CLEAR follow playback.
    const int sampler = sheetSampler();
    if (sampler <= 0) {
        return;
    }
    if (!m_engine->loaded(sampler)) {
        closeSheet();
        return;
    }
    const bool playing = m_engine->playing(sampler);
    if (m_sheetStop->isEnabled() == playing && m_sheetClear->isEnabled() == !playing) {
        return;
    }
    setEnabledIf(m_sheetStop, playing);
    setEnabledIf(m_sheetClear, !playing);
    m_sheetClear->setAccessibleName(playing ? tr("Clear pad %1: stop it first").arg(sampler)
                                            : tr("Clear pad %1 (ejects the sample)").arg(sampler));
}

void DeckLabSampler::refreshSide() {
    setCheckedIf(m_quantize, m_engine->quantize());
    setCheckedIf(m_sync, m_engine->syncLoops());
    setCheckedIf(m_edit, m_editMode || sheetOpen());
    setCheckedIf(m_fx, m_fxRouting);
    setEnabledIf(m_stopAll, anyPlaying());
    if (!m_level->isSliderDown()) {
        const int value = static_cast<int>(std::lround(std::clamp(m_engine->level(), 0.0, 1.0) * 100));
        if (value != m_level->value()) {
            const QSignalBlocker blocker(m_level);
            m_level->setValue(value);
        }
    }
    const double db = m_engine->levelDb();
    const int shown = util_isfinite(db) ? static_cast<int>(std::lround(std::max(db, -61.0))) : -61;
    if (shown != m_shownLevel) {
        m_shownLevel = shown;
        m_level->setValueText(dbText(db));
        m_level->setAccessibleDescription(tr("All pads, %1").arg(dbText(db)));
    }
}

void DeckLabSampler::refreshBanks() {
    const int samplers = std::max(kPads, m_engine->count());
    m_banks = std::max(1, (samplers + kPads - 1) / kPads);
    m_bank = std::clamp(m_bank, 0, m_banks - 1);
    const bool show = m_banks > 1;
    if (m_bankButton->isVisibleTo(this) != show) {
        m_bankButton->setVisible(show);
    }
    const QString text = tr("BANK %1").arg(QChar('A' + m_bank));
    setIf<QAbstractButton>(m_bankButton, &QAbstractButton::setText, &QAbstractButton::text, text);
    const int first = m_bank * kPads + 1;
    m_bankButton->setAccessibleName(tr("Bank %1, pads %2 to %3; tap for the next bank")
                                            .arg(QChar('A' + m_bank))
                                            .arg(first)
                                            .arg(first + kPads - 1));
}

void DeckLabSampler::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_dirty) {
        m_dirty = false;
        refreshBanks();
        refreshPads();
    }
    refreshSide();
}

void DeckLabSampler::hideEvent(QHideEvent* event) {
    // A touch must never act on a page that is gone: holds, gates and the
    // edit sheet end here.
    ++m_hides;
    m_hold.stop();
    m_holdPad = -1;
    endGate();
    closeSheet();
    QWidget::hideEvent(event);
}
