// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklablanes.h"

#include <QCoreApplication>
#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <algorithm>
#include <cmath>

#include "util/fpclassify.h"

namespace decklab::lanes {

Choice choose(const std::array<Deck, 4>& decks, const std::array<int, 2>& layers, bool fourDecks) {
    const auto onAir = [&decks](int deck) {
        return decks[deck - 1].loaded && decks[deck - 1].playing;
    };
    const auto layerRank = [&layers](int deck) {
        return deck == layers[0] ? 0 : deck == layers[1] ? 1 : 2;
    };
    // On air first, then the controller's active layers, then loaded decks.
    const auto score = [&](int deck) {
        return (onAir(deck) ? 4 : 0) + (layerRank(deck) < 2 ? 2 : 0) + (decks[deck - 1].loaded ? 1 : 0);
    };
    std::array<int, 4> order{1, 2, 3, 4};
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const int scoreA = score(a);
        const int scoreB = score(b);
        if (scoreA != scoreB) {
            return scoreA > scoreB;
        }
        if (layerRank(a) != layerRank(b)) {
            return layerRank(a) < layerRank(b);
        }
        return a < b;
    });
    // Screen order follows the controller: the left side's deck (1 or 3) on
    // top, the right side's (2 or 4) below; two of one side by number.
    int first = order[0];
    int second = order[1];
    const auto side = [](int deck) {
        return (deck - 1) % 2;
    };
    if (side(second) < side(first) || (side(second) == side(first) && second < first)) {
        std::swap(first, second);
    }
    Choice choice;
    choice.lanes = {first, second};
    if (fourDecks) {
        choice.row = {1, 2, 3, 4};
        choice.rowCount = 4;
        return choice;
    }
    choice.row = {first, second, 0, 0};
    choice.rowCount = 2;
    for (int deck = 1; deck <= 4; ++deck) {
        if (!choice.hasLane(deck) && onAir(deck)) {
            choice.row[choice.rowCount++] = deck;
        }
    }
    return choice;
}

} // namespace decklab::lanes

namespace {
// Brand tokens. Live red only for the end of a track.
const QColor kGround(0x0d, 0x0f, 0x12);
const QColor kPanel(0x16, 0x19, 0x1e);
const QColor kLine(0x2c, 0x31, 0x3a);
const QColor kMuted(0x9a, 0xa3, 0xaf);
const QColor kText(0xf2, 0xf4, 0xf7);
const QColor kDecimals(0xc9, 0xce, 0xd6);
const QColor kPlay(0x2f, 0xd2, 0x7a);
const QColor kLive(0xff, 0x3d, 0x2e);
// A cell this wide shows the play glyph, BPM decimals, full-size beat
// lamps, MASTER in words and the phrase.
constexpr int kWideCell = 300;

QString trLanes(const char* text) {
    return QCoreApplication::translate("DeckLabLaneDecks", text);
}

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

// Built once, after the application exists; never on a paint.
struct Fonts {
    QFont numeral18 = numeralFont(18);
    QFont numeral24 = numeralFont(24);
    QFont badge = numeralFont(16);
    QFont muted = uiFont(15, QFont::Medium);
    QFont label = [] {
        QFont font = uiFont(14, QFont::DemiBold);
        font.setLetterSpacing(QFont::PercentageSpacing, 106);
        return font;
    }();
    static const Fonts& get() {
        static const Fonts fonts;
        return fonts;
    }
};

// Fixed-width digits, so a running clock never shifts sideways.
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
        if (ch.isDigit()) {
            const int advance = metrics.horizontalAdvance(ch);
            painter.drawText(QPoint(x + (cell - advance) / 2, baseline), QString(ch));
            x += cell;
        } else {
            painter.drawText(QPoint(x, baseline), QString(ch));
            x += metrics.horizontalAdvance(ch);
        }
    }
}

int baselineIn(const QRect& area, const QFontMetrics& metrics) {
    return area.top() + (area.height() + metrics.capHeight()) / 2;
}

QString wholeBpm(const QString& bpm) {
    const int dot = static_cast<int>(bpm.indexOf(QLatin1Char('.')));
    return dot < 0 ? bpm : bpm.left(dot);
}

QString roundedBpm(const QString& bpm) {
    bool ok = false;
    const double value = bpm.toDouble(&ok);
    return ok && util_isfinite(value) ? QString::number(std::llround(value)) : wholeBpm(bpm);
}
} // namespace

DeckLabLaneDecks::DeckLabLaneDecks(QWidget* parent)
        : QWidget(parent) {
    setObjectName(QStringLiteral("decklab-lane-decks"));
    setFixedHeight(decklab::lanes::kRowHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_OpaquePaintEvent);
    // Display only: the rail's Decks tab is the way back to the decks.
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    setAccessibleName(trLanes("Decks"));
    setAccessibleDescription(trLanes("Each deck's tempo, beat, phrase and time left, under its waveform"));
    for (int deck = 1; deck <= 4; ++deck) {
        m_states[deck - 1].deck = deck;
    }
}

QSize DeckLabLaneDecks::sizeHint() const {
    return QSize(800, decklab::lanes::kRowHeight);
}

bool DeckLabLaneDecks::shows(int deck) const {
    for (int index = 0; index < m_choice.rowCount; ++index) {
        if (m_choice.row[index] == deck) {
            return true;
        }
    }
    return false;
}

QRect DeckLabLaneDecks::cellRect(int deck) const {
    int index = -1;
    for (int n = 0; n < m_choice.rowCount; ++n) {
        if (m_choice.row[n] == deck) {
            index = n;
        }
    }
    if (index < 0 || m_choice.rowCount <= 0) {
        return QRect();
    }
    const int left = index * width() / m_choice.rowCount;
    const int right = (index + 1) * width() / m_choice.rowCount;
    return QRect(left, 0, right - left, height());
}

void DeckLabLaneDecks::setChoice(const decklab::lanes::Choice& choice) {
    if (choice == m_choice) {
        return;
    }
    m_choice = choice;
    refreshAll();
}

void DeckLabLaneDecks::setDeck(int deck, const DeckLabStripState& state, double position) {
    if (deck < 1 || deck > 4) {
        return;
    }
    // Implicitly shared strings: storing the state copies no text.
    m_states[deck - 1] = state;
    m_positions[deck - 1] = util_isfinite(position) ? std::clamp(position, 0.0, 1.0) : 0.0;
    if (shows(deck)) {
        refreshCell(deck);
    }
}

void DeckLabLaneDecks::setFlash(bool on) {
    if (on == m_flash) {
        return;
    }
    m_flash = on;
    for (int index = 0; index < m_choice.rowCount; ++index) {
        if (m_states[m_choice.row[index] - 1].endWarning) {
            refreshCell(m_choice.row[index]);
        }
    }
}

DeckLabLaneDecks::Cell DeckLabLaneDecks::cellFor(int deck, int width) const {
    const DeckLabStripState& state = m_states[deck - 1];
    Cell cell;
    cell.lane = m_choice.hasLane(deck);
    cell.loaded = state.loaded;
    if (!state.loaded) {
        return cell;
    }
    cell.playing = state.playing;
    cell.cue = state.cue;
    cell.master = state.master;
    cell.endWarning = state.endWarning;
    cell.flash = state.endWarning && m_flash;
    cell.bpm = state.bpm;
    cell.remaining = state.remaining;
    cell.phase = state.phase;
    cell.progress = static_cast<int>(std::lround(m_positions[deck - 1] * std::max(0, width - 16)));
    if (width >= kWideCell && !state.phrase.name.isEmpty()) {
        cell.phrase = state.phrase.countdown;
        cell.phraseColor = state.phrase.color;
    }
    return cell;
}

void DeckLabLaneDecks::refreshCell(int deck) {
    const QRect area = cellRect(deck);
    const Cell cell = cellFor(deck, area.width());
    if (cell == m_cells[deck - 1]) {
        return;
    }
    m_cells[deck - 1] = cell;
    ++m_cellUpdates;
    update(area);
}

void DeckLabLaneDecks::refreshAll() {
    for (int deck = 1; deck <= 4; ++deck) {
        m_cells[deck - 1] = shows(deck) ? cellFor(deck, cellRect(deck).width()) : Cell();
    }
    ++m_cellUpdates;
    update();
}

void DeckLabLaneDecks::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    refreshAll();
}

void DeckLabLaneDecks::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    painter.fillRect(event->rect(), kPanel);
    for (int index = 0; index < m_choice.rowCount; ++index) {
        const int deck = m_choice.row[index];
        const QRect area = cellRect(deck);
        if (!area.intersects(event->rect())) {
            continue;
        }
        painter.save();
        painter.setClipRect(area);
        paintCell(painter, deck, area);
        painter.restore();
    }
}

void DeckLabLaneDecks::paintCell(QPainter& painter, int deck, const QRect& area) const {
    const Cell& cell = m_cells[deck - 1];
    const Fonts& fonts = Fonts::get();
    const QColor color = deckLabDeckColor(deck);
    const bool wide = area.width() >= kWideCell;
    // A deck with a lane above: its colour along the top, matching the
    // lane's edge.
    if (cell.lane) {
        painter.fillRect(QRect(area.left(), 0, area.width(), 3), color);
    }
    if (area.left() > 0) {
        painter.fillRect(QRect(area.left(), 6, 1, area.height() - 10), kLine);
    }
    const QRect body(area.left(), 3, area.width(), area.height() - 6);
    int x = area.left() + (wide ? 8 : 6);
    // Badge: filled in the deck colour when loaded, outlined when empty.
    const QRect badge(x, body.center().y() - 10, wide ? 26 : 22, 21);
    painter.setFont(fonts.badge);
    if (cell.loaded) {
        painter.fillRect(badge, color);
        painter.setPen(kGround);
    } else {
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(color, 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(badge).adjusted(0.75, 0.75, -0.75, -0.75));
        painter.restore();
        painter.setPen(color);
    }
    painter.drawText(badge, Qt::AlignCenter, QString::number(deck));
    x = badge.right() + 1 + (wide ? 8 : 6);
    if (!cell.loaded) {
        painter.setFont(fonts.muted);
        painter.setPen(kMuted);
        painter.drawText(QRect(x, body.top(), area.right() - 6 - x, body.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                trLanes("Empty"));
        return;
    }
    const QColor ink = cell.playing ? kText : kMuted;
    if (wide) {
        // Green play triangle while playing, two muted bars while paused.
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        const double top = body.center().y() - 6.5;
        if (cell.playing) {
            QPainterPath play;
            play.moveTo(x, top);
            play.lineTo(x, top + 13);
            play.lineTo(x + 11, top + 6.5);
            play.closeSubpath();
            painter.fillPath(play, kPlay);
        } else {
            painter.fillRect(QRectF(x, top, 4, 13), kMuted);
            painter.fillRect(QRectF(x + 7, top, 4, 13), kMuted);
        }
        painter.restore();
        x += 11 + 8;
    }
    // BPM: wide cells the one-decimal readout, narrow cells the rounded
    // whole tempo (never cut).
    const QFont& bpmFont = wide ? fonts.numeral24 : fonts.numeral18;
    const QFontMetrics bpmMetrics(bpmFont);
    const int baseline = baselineIn(body, bpmMetrics);
    const QString whole = cell.bpm.isEmpty() ? QStringLiteral("—") : wide ? wholeBpm(cell.bpm) : roundedBpm(cell.bpm);
    const int wholeBox = tabularWidth(bpmMetrics, QStringLiteral("000"));
    painter.setFont(bpmFont);
    painter.setPen(ink);
    drawTabular(painter, x + wholeBox - tabularWidth(bpmMetrics, whole), baseline, whole);
    x += wholeBox;
    if (wide) {
        const int dot = static_cast<int>(cell.bpm.indexOf(QLatin1Char('.')));
        const QString fraction = dot < 0 ? QString() : cell.bpm.mid(dot);
        painter.setFont(fonts.numeral18);
        painter.setPen(cell.playing ? kDecimals : kMuted);
        drawTabular(painter, x, baseline, fraction);
        x += tabularWidth(QFontMetrics(fonts.numeral18), QStringLiteral(".0"));
    }
    x += wide ? 10 : 6;
    // Four-beat lamp: the current beat in the deck colour (dimmer while
    // paused); no grid, no lamp.
    const int lamp = wide ? 12 : 7;
    const int gap = wide ? 3 : 2;
    const int lampTop = body.center().y() - 4;
    for (int beat = 0; beat < 4; ++beat) {
        QColor fill = cell.phase < 0 ? kPanel : kLine;
        if (cell.phase == beat) {
            fill = cell.playing ? color : color.darker(170);
        }
        painter.fillRect(QRect(x + beat * (lamp + gap), lampTop, lamp, 9), fill);
    }
    x += 4 * lamp + 3 * gap + (wide ? 10 : 5);
    // Time left, right aligned; live red near the end, blinking into a red
    // box with dark figures, as on the Decks readout.
    const QFont& timeFont = wide ? fonts.numeral24 : fonts.numeral18;
    const QFontMetrics timeMetrics(timeFont);
    const QString time = cell.remaining.isEmpty() ? QStringLiteral("—") : cell.remaining;
    const int timeWidth = tabularWidth(timeMetrics, time);
    const int timeLeft = area.right() - (wide ? 8 : 6) - timeWidth;
    painter.setFont(timeFont);
    if (cell.endWarning && cell.flash) {
        painter.fillRect(QRect(timeLeft - 4, body.top() + 2, timeWidth + 8, body.height() - 4), kLive);
        painter.setPen(kPanel);
    } else {
        painter.setPen(cell.endWarning ? kLive : ink);
    }
    drawTabular(painter, timeLeft, baselineIn(body, timeMetrics), time);
    const int room = timeLeft - (wide ? 10 : 5);
    // MASTER: a filled chip in the deck colour ("M" in a narrow cell).
    if (cell.master) {
        const QFontMetrics label(fonts.label);
        const QString text = wide ? trLanes("MASTER") : trLanes("M");
        const int chipWidth = label.horizontalAdvance(text) + (wide ? 12 : 8);
        if (x + chipWidth <= room) {
            const QRect chip(x, body.center().y() - 9, chipWidth, 19);
            painter.fillRect(chip, color);
            painter.setFont(fonts.label);
            painter.setPen(kGround);
            painter.drawText(chip, Qt::AlignCenter, text);
            x = chip.right() + 1 + 8;
        }
    }
    // The rekordbox phrase the scrolling lane cannot show: its colour and
    // the "bars.beats" countdown, where there is room.
    if (!cell.phrase.isEmpty()) {
        const QFontMetrics countdown(fonts.numeral18);
        const int needed = 10 + 5 + tabularWidth(countdown, cell.phrase);
        if (x + needed <= room) {
            painter.fillRect(QRect(x, body.center().y() - 7, 10, 15),
                    cell.phraseColor.isValid() ? cell.phraseColor : kMuted);
            painter.setFont(fonts.numeral18);
            painter.setPen(kText);
            drawTabular(painter, x + 15, baselineIn(body, countdown), cell.phrase);
        }
    }
    // Where the deck is in its track, along the bottom: the slim overview.
    if (cell.progress >= 0) {
        const QRect track(area.left() + 8, area.bottom() - 2, area.width() - 16, 2);
        painter.fillRect(track, kLine);
        painter.fillRect(QRect(track.left(), track.top(), cell.progress, 2), cell.playing ? color : color.darker(170));
    }
}
