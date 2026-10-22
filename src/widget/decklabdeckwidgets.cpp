// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabdeckwidgets.h"

#include <QCoreApplication>
#include <QEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QStringList>

#include "moc_decklabdeckwidgets.cpp"

namespace {
// Brand neutrals (ground, panel, raise, line, muted, text). Deck colors
// carry identity; live red is only for the end-of-track warning and clip.
const QColor kPanel(0x0d, 0x0f, 0x12);
const QColor kPanelPressed(0x20, 0x24, 0x2b);
const QColor kMiniPanel(0x16, 0x19, 0x1e);
const QColor kHairline(0x2c, 0x31, 0x3a);
const QColor kText(0xf2, 0xf4, 0xf7);
const QColor kSecondary(0x9a, 0xa3, 0xaf);
const QColor kDim(0x59, 0x60, 0x6b);
const QColor kLampOff(0x2c, 0x31, 0x3a);
const QColor kLampLit(0xf2, 0xf4, 0xf7);
const QColor kDecimals(0xc9, 0xce, 0xd6);
const QColor kPlay(0x2f, 0xd2, 0x7a);
const QColor kCue(0xff, 0x7a, 0x1a);
const QColor kWarning(0xff, 0x3d, 0x2e);
const QColor kWarningDim(0x5c, 0x1a, 0x15);

int digitCell(const QFontMetrics& metrics) {
    int cell = 0;
    for (char digit = '0'; digit <= '9'; ++digit) {
        cell = std::max(cell, metrics.horizontalAdvance(QLatin1Char(digit)));
    }
    return cell;
}

// Fixed-width digit cells: a running clock or a moving tempo never shifts
// the rest of the readout sideways.
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

QString wholePart(const QString& number) {
    const int dot = number.indexOf(QLatin1Char('.'));
    return dot < 0 ? number : number.left(dot);
}

QString fractionPart(const QString& number) {
    const int dot = number.indexOf(QLatin1Char('.'));
    return dot < 0 ? QString() : number.mid(dot);
}

// Dark ink on light lamps, light ink on dark ones.
QColor inkOn(const QColor& color) {
    return color.lightnessF() > 0.55 ? QColor(0x10, 0x10, 0x10) : QColor(0xf2, 0xf2, 0xf2);
}
} // namespace

void DeckLabStripRenderer::setFamily(const QString& family) {
    m_family = family;
}

QFont DeckLabStripRenderer::font(int pixels, QFont::Weight weight, bool caption) const {
    QFont result;
    if (!m_family.isEmpty()) {
        result.setFamily(m_family);
    }
    result.setPixelSize(std::max(1, pixels));
    result.setWeight(weight);
    if (caption) {
        result.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
    }
    return result;
}

int DeckLabStripRenderer::left() const {
    return m_mode == Mode::Mini ? 6 : m_mode == Mode::Compact ? 12 : 14;
}

QRect DeckLabStripRenderer::rect(Part part) const {
    const int width = m_size.width();
    const int height = m_size.height();
    // Full-mode columns scale with the card width so wider screens keep the
    // proportions of the 400 px design: badge, state and title; BPM, tempo,
    // key and clock; lamps with the beat meter on the right.
    const int bpmWidth = width * 250 / 1000;
    const int tempoLeft = 12 + bpmWidth + 4;
    const int tempoWidth = width * 240 / 1000;
    const int keyLeft = tempoLeft + tempoWidth + 4;
    const int keyWidth = width * 150 / 1000;
    const int timeLeft = keyLeft + keyWidth;
    const int phaseWidth = std::clamp(width * 31 / 100, 110, 200);
    switch (m_mode) {
    case Mode::Full:
        switch (part) {
        case Part::Identity:
            return QRect(12, 6, 72, 26);
        case Part::Title:
            return QRect(86, 6, std::max(0, width - 96), 26);
        case Part::Bpm:
            return QRect(12, 34, bpmWidth, 32);
        case Part::Tempo:
            return QRect(tempoLeft, 34, tempoWidth, 32);
        case Part::Key:
            return QRect(keyLeft, 34, keyWidth, 32);
        case Part::Time:
            return QRect(timeLeft, 34, width - timeLeft, 32);
        case Part::Chips:
            return QRect(12, 67, std::max(0, width - 24 - phaseWidth - 8), 18);
        case Part::Phase:
            return QRect(width - 10 - phaseWidth, 72, phaseWidth, 8);
        case Part::Captions:
            return QRect();
        }
        break;
    case Mode::Compact: {
        const int half = (width - 20) / 2;
        switch (part) {
        case Part::Phase:
            return QRect(12, 4, width - 20, 5);
        // Badge, state and title; big BPM and clock; tempo and key; lamps.
        case Part::Identity:
            return QRect(10, 11, 60, 24);
        case Part::Title:
            return QRect(76, 11, std::max(0, width - 84), 24);
        case Part::Bpm:
            return QRect(10, 37, half + 2, 32);
        case Part::Time:
            return QRect(12 + half, 37, width - 12 - half, 32);
        case Part::Tempo:
            return QRect(10, 70, 96, 20);
        case Part::Key:
            return QRect(width - 64, 70, 64, 20);
        case Part::Chips:
            return QRect(10, 92, width - 18, std::max(0, height - 92));
        case Part::Captions:
            return QRect();
        }
        break;
    }
    case Mode::Mini:
        switch (part) {
        case Part::Identity:
            return QRect(4, 0, 48, height);
        case Part::Title:
            return QRect(54, 0, std::max(0, width - 54 - 186), height);
        case Part::Key:
            return QRect(width - 184, 0, 44, height);
        case Part::Bpm:
            return QRect(width - 136, 0, 64, height);
        case Part::Time:
            return QRect(width - 70, 0, 70, height);
        case Part::Phase:
        case Part::Chips:
        case Part::Captions:
        case Part::Tempo:
            return QRect();
        }
        break;
    }
    return QRect();
}

QRegion DeckLabStripRenderer::setState(const DeckLabStripState& next) {
    const DeckLabStripState previous = m_state;
    if (previous == next) {
        return QRegion();
    }
    m_state = next;
    if (previous.deck != next.deck || previous.loaded != next.loaded) {
        return QRegion(QRect(QPoint(), m_size));
    }
    QRegion dirty;
    const auto add = [this, &dirty](Part part) {
        const QRect area = rect(part);
        if (!area.isEmpty()) {
            dirty += area;
        }
    };
    if (previous.playing != next.playing || previous.cue != next.cue) {
        add(Part::Identity);
        add(Part::Phase);
    }
    if (previous.title != next.title || previous.artist != next.artist) {
        add(Part::Title);
    }
    if (previous.master != next.master || previous.sync != next.sync ||
            previous.range != next.range || previous.tempoCentered != next.tempoCentered ||
            previous.keyMatch != next.keyMatch) {
        add(Part::Captions);
        add(Part::Tempo);
        add(Part::Key);
        add(Part::Chips);
    }
    if (previous.bpm != next.bpm) {
        add(Part::Bpm);
    }
    if (previous.tempo != next.tempo) {
        add(Part::Tempo);
    }
    if (previous.key != next.key || previous.keyColor != next.keyColor) {
        add(Part::Key);
    }
    if (previous.remaining != next.remaining || previous.endWarning != next.endWarning) {
        add(Part::Time);
    }
    if (previous.phase != next.phase) {
        add(Part::Phase);
    }
    if (previous.pfl != next.pfl || previous.slip != next.slip || previous.keylock != next.keylock ||
            previous.quantize != next.quantize || previous.looping != next.looping ||
            previous.loopSize != next.loopSize) {
        add(Part::Chips);
    }
    return dirty;
}

QRegion DeckLabStripRenderer::setFlash(bool on) {
    if (m_flash == on) {
        return QRegion();
    }
    m_flash = on;
    return m_state.endWarning ? QRegion(rect(Part::Time)) : QRegion();
}

QString DeckLabStripRenderer::modesText() const {
    QStringList modes;
    if (m_state.pfl) {
        modes << QCoreApplication::translate("DeckLabDeckStrip", "HP");
    }
    if (m_state.looping) {
        modes << QCoreApplication::translate("DeckLabDeckStrip", "LOOP %1").arg(m_state.loopSize);
    }
    if (m_state.keylock) {
        modes << QCoreApplication::translate("DeckLabDeckStrip", "KEY");
    }
    if (m_state.quantize) {
        modes << QCoreApplication::translate("DeckLabDeckStrip", "QNT");
    }
    if (m_state.slip) {
        modes << QCoreApplication::translate("DeckLabDeckStrip", "SLIP");
    }
    if (m_state.sync) {
        modes << QCoreApplication::translate("DeckLabDeckStrip", "SYNC");
    }
    if (m_state.master) {
        modes << QCoreApplication::translate("DeckLabDeckStrip", "MASTER");
    }
    return modes.join(QLatin1Char(' '));
}

QStringList DeckLabStripRenderer::overflowingParts() const {
    QStringList overflowing;
    if (!m_state.loaded) {
        return overflowing;
    }
    const bool mini = m_mode == Mode::Mini;
    const bool compact = m_mode == Mode::Compact;
    const int bigMin = mini ? 14 : compact ? 20 : 22;
    const QFontMetrics whole(font(bigMin, QFont::Bold));
    const QFontMetrics fraction(font(mini ? 14 : std::max(12, bigMin * 3 / 5), QFont::DemiBold));
    const int bpmWidth = mini ? tabularWidth(whole, m_state.bpm)
                              : tabularWidth(whole, wholePart(m_state.bpm)) +
                    tabularWidth(fraction, fractionPart(m_state.bpm));
    if (bpmWidth > rect(Part::Bpm).width() - 4) {
        overflowing << QStringLiteral("bpm");
    }
    // Long mixes (100+ minutes) may shrink the clock down to 20 px.
    const QFontMetrics clock(font(mini ? 14 : 20, mini ? QFont::DemiBold : QFont::Bold));
    if (tabularWidth(clock, m_state.remaining) > rect(Part::Time).width() - (mini ? 8 : 10)) {
        overflowing << QStringLiteral("time");
    }
    const QFontMetrics key(font(mini ? 14 : 16, QFont::Bold));
    if (key.horizontalAdvance(m_state.key) > rect(Part::Key).width() - (mini ? 2 : 14)) {
        overflowing << QStringLiteral("key");
    }
    if (!mini) {
        const QFontMetrics tempo(font(14, QFont::DemiBold));
        if (tabularWidth(tempo, m_state.tempo) > rect(Part::Tempo).width() - 4) {
            overflowing << QStringLiteral("tempo");
        }
    }
    return overflowing;
}

void DeckLabStripRenderer::paintBadge(QPainter& painter) const {
    const bool mini = m_mode == Mode::Mini;
    const QRect badge = mini ? QRect(6, (m_size.height() - 16) / 2, 22, 16)
            : m_mode == Mode::Compact ? QRect(12, 13, 24, 20)
                                      : QRect(14, 8, 26, 22);
    QColor color = deckLabDeckColor(m_state.deck);
    if (!m_state.loaded) {
        color = color.darker(170);
    }
    QPainterPath path;
    path.addRoundedRect(QRectF(badge), 3, 3);
    painter.fillPath(path, color);
    painter.setFont(font(mini ? 12 : m_mode == Mode::Compact ? 14 : 15, QFont::Bold));
    painter.setPen(inkOn(color));
    painter.drawText(badge, Qt::AlignCenter, QString::number(m_state.deck));
}

void DeckLabStripRenderer::paintTransport(QPainter& painter, const QRect& area) const {
    const QPoint center = area.center();
    if (!m_state.loaded) {
        // Empty deck: a dim stop square.
        const int size = m_mode == Mode::Mini ? 8 : 10;
        painter.setPen(QPen(kDim, 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(area.left() + 3, center.y() - size / 2.0, size, size));
        return;
    }
    if (m_state.playing) {
        // PLAY: solid green triangle, the club-standard play lamp.
        const int size = m_mode == Mode::Mini ? 10 : 13;
        QPainterPath triangle;
        const QPointF origin(area.left() + 2, center.y() - size / 2.0);
        triangle.moveTo(origin);
        triangle.lineTo(origin.x(), origin.y() + size);
        triangle.lineTo(origin.x() + size * 0.9, origin.y() + size / 2.0);
        triangle.closeSubpath();
        painter.fillPath(triangle, kPlay);
    } else if (m_state.cue) {
        painter.setFont(font(m_mode == Mode::Mini ? 12 : 13, QFont::Bold));
        painter.setPen(kCue);
        painter.drawText(area, Qt::AlignVCenter | Qt::AlignLeft, QCoreApplication::translate("DeckLabDeckStrip", "CUE"));
    } else {
        // PAUSE: two bars.
        const int size = m_mode == Mode::Mini ? 10 : 13;
        const int top = center.y() - size / 2;
        painter.fillRect(QRect(area.left() + 2, top, 4, size), kDecimals);
        painter.fillRect(QRect(area.left() + 9, top, 4, size), kDecimals);
    }
}

void DeckLabStripRenderer::paintTitle(QPainter& painter, const QRect& area, int pixels) const {
    if (area.width() <= 0) {
        return;
    }
    const QFont titleFont = font(pixels, QFont::DemiBold);
    const QFont artistFont = font(pixels, QFont::Normal);
    const QFontMetrics titleMetrics(titleFont);
    const QFontMetrics artistMetrics(artistFont);
    const int baseline = area.top() + (area.height() + titleMetrics.ascent() - titleMetrics.descent()) / 2;
    if (!m_state.loaded) {
        painter.setFont(artistFont);
        painter.setPen(kSecondary);
        painter.drawText(QPoint(area.left(), baseline),
                artistMetrics.elidedText(m_mode != Mode::Compact ? QCoreApplication::translate("DeckLabDeckStrip", "No track loaded")
                                                                 : QCoreApplication::translate("DeckLabDeckStrip", "Tap to browse"),
                        Qt::ElideRight, area.width()));
        return;
    }
    const QString title = m_state.title.isEmpty() ? QCoreApplication::translate("DeckLabDeckStrip", "Untitled") : m_state.title;
    const int titleWidth = titleMetrics.horizontalAdvance(title);
    painter.setFont(titleFont);
    painter.setPen(kText);
    if (titleWidth >= area.width() || m_state.artist.isEmpty()) {
        painter.drawText(QPoint(area.left(), baseline), titleMetrics.elidedText(title, Qt::ElideRight, area.width()));
        return;
    }
    painter.drawText(QPoint(area.left(), baseline), title);
    const QString artist = QStringLiteral("  ·  ") + m_state.artist;
    const int remaining = area.width() - titleWidth;
    if (remaining > artistMetrics.horizontalAdvance(QStringLiteral("  ·  W…"))) {
        painter.setFont(artistFont);
        painter.setPen(kSecondary);
        painter.drawText(QPoint(area.left() + titleWidth, baseline),
                artistMetrics.elidedText(artist, Qt::ElideRight, remaining));
    }
}

void DeckLabStripRenderer::paintPhase(QPainter& painter) const {
    const QRect area = rect(Part::Phase);
    if (area.isEmpty()) {
        return;
    }
    const int gap = 4;
    const int segment = (area.width() - 3 * gap) / 4;
    for (int index = 0; index < 4; ++index) {
        const QRect cell(area.left() + index * (segment + gap), area.top(), segment, area.height());
        // The current beat lights in the deck's color, dimmer while paused.
        QColor color = kLampOff;
        if (m_state.loaded && m_state.phase < 0) {
            color = kMiniPanel;
        } else if (m_state.loaded && index == m_state.phase) {
            color = m_state.playing ? deckLabDeckColor(m_state.deck) : deckLabDeckColor(m_state.deck).darker(170);
        }
        painter.fillRect(cell, color);
    }
}

int DeckLabStripRenderer::paintChip(QPainter& painter, int x, int y, int height,
        const QString& text, bool on, const QColor& lit, bool alignRight, bool outlined) const {
    const QFont chipFont = font(12, QFont::Bold, true);
    const QFontMetrics metrics(chipFont);
    const int width = metrics.horizontalAdvance(text) + (height < 17 ? 8 : 12);
    const QRectF chip(alignRight ? x - width : x, y, width, height);
    QPainterPath path;
    path.addRoundedRect(chip.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    if (on && outlined) {
        painter.setPen(QPen(lit, 1));
        painter.drawPath(path);
        painter.setPen(lit);
    } else if (on) {
        painter.fillPath(path, lit);
        painter.setPen(inkOn(lit));
    } else {
        painter.setPen(QPen(kLampOff, 1));
        painter.drawPath(path);
        painter.setPen(kDim);
    }
    painter.setFont(chipFont);
    painter.drawText(chip, Qt::AlignCenter, text);
    return width;
}

void DeckLabStripRenderer::paint(QPainter& painter, const QRect& dirty, bool pressed, bool focused) const {
    const bool mini = m_mode == Mode::Mini;
    const bool compact = m_mode == Mode::Compact;
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.fillRect(dirty, pressed ? kPanelPressed : mini ? kMiniPanel : kPanel);
    if (!mini && dirty.top() <= 0) {
        painter.fillRect(QRect(0, 0, m_size.width(), 1), kHairline);
    }
    const auto needs = [this, &dirty](Part part) {
        const QRect area = rect(part);
        return !area.isEmpty() && area.intersects(dirty);
    };
    if (needs(Part::Phase)) {
        paintPhase(painter);
    }
    if (needs(Part::Identity)) {
        paintBadge(painter);
        const QRect transport = mini ? QRect(32, 0, 20, m_size.height())
                : compact            ? QRect(40, 11, 32, 24)
                                     : QRect(46, 6, 38, 26);
        paintTransport(painter, transport);
    }
    if (needs(Part::Title)) {
        paintTitle(painter, rect(Part::Title).adjusted(0, 0, -2, 0), m_mode == Mode::Full ? 15 : 14);
    }
    if (!m_state.loaded) {
        if (!mini && !compact && needs(Part::Bpm)) {
            painter.setFont(font(14, QFont::Normal));
            painter.setPen(kSecondary);
            painter.drawText(QRect(14, 34, m_size.width() - 24, m_size.height() - 38),
                    Qt::AlignCenter, QCoreApplication::translate("DeckLabDeckStrip", "Tap to browse and load a track"));
        }
        if (focused) {
            painter.setPen(QPen(deckLabDeckColor(m_state.deck), 1));
            painter.drawRect(QRect(QPoint(), m_size).adjusted(0, 0, -1, -1));
        }
        return;
    }
    // Lamps.
    if (!mini && needs(Part::Chips)) {
        const QRect area = rect(Part::Chips);
        if (compact) {
            int x = area.left();
            // Only lit lamps, in order of importance, as many as fit.
            struct Lamp {
                QString text;
                bool on;
                QColor color;
                bool outlined;
            };
            const QColor deckColor = deckLabDeckColor(m_state.deck);
            const std::array<Lamp, 7> lamps{{
                    {QCoreApplication::translate("DeckLabDeckStrip", "MASTER"), m_state.master, deckColor, false},
                    {QCoreApplication::translate("DeckLabDeckStrip", "SYNC"), m_state.sync, deckColor, true},
                    {QCoreApplication::translate("DeckLabDeckStrip", "HP"), m_state.pfl, kLampLit, false},
                    {QCoreApplication::translate("DeckLabDeckStrip", "LOOP %1").arg(m_state.loopSize), m_state.looping, kLampLit, false},
                    {QCoreApplication::translate("DeckLabDeckStrip", "KEY"), m_state.keylock, kLampLit, false},
                    {QCoreApplication::translate("DeckLabDeckStrip", "Q"), m_state.quantize, kLampLit, false},
                    {QCoreApplication::translate("DeckLabDeckStrip", "SLIP"), m_state.slip, kLampLit, false}}};
            const QFontMetrics metrics(font(12, QFont::Bold, true));
            for (const auto& lamp : lamps) {
                if (!lamp.on) {
                    continue;
                }
                const int width = metrics.horizontalAdvance(lamp.text) + 12;
                if (x + width > area.right()) {
                    break;
                }
                x += paintChip(painter, x, area.top() + 3, 18, lamp.text, true, lamp.color, false, lamp.outlined) + 3;
            }
        } else {
            // MASTER fills and SYNC outlines in the deck's color; HP and
            // SLIP light white.
            const QColor deckColor = deckLabDeckColor(m_state.deck);
            int x = area.left() + 2;
            const int top = area.top() + 1;
            x += paintChip(painter, x, top, 16, QCoreApplication::translate("DeckLabDeckStrip", "MASTER"), m_state.master, deckColor, false) + 4;
            x += paintChip(painter, x, top, 16, QCoreApplication::translate("DeckLabDeckStrip", "SYNC"), m_state.sync, deckColor, false, true) + 4;
            x += paintChip(painter, x, top, 16, QCoreApplication::translate("DeckLabDeckStrip", "HP"), m_state.pfl, kLampLit, false) + 4;
            if (m_state.slip) {
                paintChip(painter, x, top, 16, QCoreApplication::translate("DeckLabDeckStrip", "SLIP"), true, kLampLit, false);
            }
        }
    }
    // Values.
    const int valueBaseline = mini ? (m_size.height() + 10) / 2 : compact ? 63 : 60;
    if (needs(Part::Bpm)) {
        const QRect area = rect(Part::Bpm);
        if (mini) {
            painter.setFont(font(14, QFont::DemiBold));
            painter.setPen(kText);
            const QFontMetrics metrics(painter.font());
            drawTabular(painter, area.right() - tabularWidth(metrics, m_state.bpm), valueBaseline, m_state.bpm);
        } else {
            const QString whole = wholePart(m_state.bpm);
            const QString fraction = fractionPart(m_state.bpm);
            int pixels = compact ? 26 : 30;
            const int minimum = compact ? 20 : 22;
            int wholeWidth = 0;
            int fractionWidth = 0;
            for (; pixels >= minimum; --pixels) {
                wholeWidth = tabularWidth(QFontMetrics(font(pixels, QFont::Bold)), whole);
                fractionWidth = tabularWidth(QFontMetrics(font(pixels * 3 / 5, QFont::DemiBold)), fraction);
                if (wholeWidth + fractionWidth <= area.width() - 4 || pixels == minimum) {
                    break;
                }
            }
            painter.setFont(font(pixels, QFont::Bold));
            painter.setPen(kText);
            drawTabular(painter, area.left() + 2, valueBaseline, whole);
            painter.setFont(font(pixels * 3 / 5, QFont::DemiBold));
            painter.setPen(kDecimals);
            drawTabular(painter, area.left() + 2 + wholeWidth, valueBaseline, fraction);
        }
    }
    if (!mini && needs(Part::Tempo)) {
        const QRect area = rect(Part::Tempo);
        if (compact) {
            const int baseline = area.top() + 16;
            painter.setFont(font(14, QFont::DemiBold));
            painter.setPen(m_state.tempoCentered ? kSecondary : kText);
            const QFontMetrics metrics(painter.font());
            drawTabular(painter, area.left() + 2, baseline, m_state.tempo);
            const int x = area.left() + 2 + tabularWidth(metrics, m_state.tempo) + 5;
            painter.setPen(Qt::NoPen);
            painter.setBrush(m_state.tempoCentered ? kPlay : kLampOff);
            painter.drawEllipse(QRectF(x, baseline - 7, 6, 6));
            painter.setBrush(Qt::NoBrush);
            const QFont rangeFont = font(12, QFont::DemiBold);
            if (x + 11 + QFontMetrics(rangeFont).horizontalAdvance(m_state.range) <= area.right()) {
                painter.setFont(rangeFont);
                painter.setPen(kSecondary);
                painter.drawText(QPoint(x + 11, baseline), m_state.range);
            }
        } else {
            // Tempo %, its range and the tempo-center lamp (lit at 0.00%).
            int pixels = 17;
            for (; pixels > 14; --pixels) {
                if (tabularWidth(QFontMetrics(font(pixels, QFont::DemiBold)), m_state.tempo) <= area.width() - 40) {
                    break;
                }
            }
            painter.setFont(font(pixels, QFont::DemiBold));
            painter.setPen(m_state.tempoCentered ? kSecondary : kDecimals);
            const QFontMetrics metrics(painter.font());
            drawTabular(painter, area.left() + 2, valueBaseline, m_state.tempo);
            int x = area.left() + 2 + tabularWidth(metrics, m_state.tempo) + 5;
            const QFont rangeFont = font(12, QFont::DemiBold);
            const int rangeWidth = QFontMetrics(rangeFont).horizontalAdvance(m_state.range);
            if (x + rangeWidth + 12 <= area.right()) {
                painter.setFont(rangeFont);
                painter.setPen(kSecondary);
                painter.drawText(QPoint(x, valueBaseline), m_state.range);
                x += rangeWidth + 5;
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(m_state.tempoCentered ? kPlay : kLampOff);
            painter.drawEllipse(QRectF(x, valueBaseline - 7, 6, 6));
            painter.setBrush(Qt::NoBrush);
        }
    }
    if (needs(Part::Key)) {
        const QRect area = rect(Part::Key);
        const QColor color = m_state.keyColor.isValid() ? m_state.keyColor : kText;
        if (mini) {
            painter.setFont(font(14, QFont::Bold));
            painter.setPen(color);
            painter.drawText(area, Qt::AlignVCenter | Qt::AlignRight, m_state.key);
        } else if (compact) {
            const int baseline = area.top() + 16;
            painter.setFont(font(16, QFont::Bold));
            painter.setPen(color);
            const QFontMetrics metrics(painter.font());
            const int width = metrics.horizontalAdvance(m_state.key);
            painter.drawText(QPoint(area.right() - 6 - width, baseline), m_state.key);
            if (m_state.keyMatch != 0) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(m_state.keyMatch > 0 ? kPlay : kDim);
                painter.drawEllipse(QRectF(area.right() - 6 - width - 9, baseline - 7, 6, 6));
                painter.setBrush(Qt::NoBrush);
            }
        } else {
            int pixels = 22;
            for (; pixels > 16; --pixels) {
                if (QFontMetrics(font(pixels, QFont::Bold)).horizontalAdvance(m_state.key) <= area.width() - 4) {
                    break;
                }
            }
            painter.setFont(font(pixels, QFont::Bold));
            painter.setPen(color);
            painter.drawText(QPoint(area.left() + 2, valueBaseline), m_state.key);
            // Harmonic hint against the master deck: a check or a cross.
            if (m_state.keyMatch != 0) {
                const int kx = area.left() + 2 + QFontMetrics(painter.font()).horizontalAdvance(m_state.key) + 5;
                if (m_state.keyMatch > 0) {
                    QPainterPath check;
                    check.moveTo(kx, valueBaseline - 5);
                    check.lineTo(kx + 3, valueBaseline - 2);
                    check.lineTo(kx + 9, valueBaseline - 9);
                    painter.setPen(QPen(kPlay, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                    painter.drawPath(check);
                } else {
                    painter.setPen(QPen(kSecondary, 1.5));
                    painter.drawLine(QPointF(kx, valueBaseline - 9), QPointF(kx + 7, valueBaseline - 2));
                    painter.drawLine(QPointF(kx + 7, valueBaseline - 9), QPointF(kx, valueBaseline - 2));
                }
            }
        }
    }
    if (needs(Part::Time)) {
        const QRect area = rect(Part::Time);
        QColor color = kText;
        if (m_state.endWarning) {
            color = m_flash ? kWarning : kWarningDim;
        }
        const int right = mini ? area.right() - 6 : compact ? m_size.width() - 8 : m_size.width() - 10;
        int pixels = mini ? 14 : compact ? 26 : 30;
        const int minimum = mini ? 14 : 20;
        for (; pixels > minimum; --pixels) {
            if (tabularWidth(QFontMetrics(font(pixels, QFont::Bold)), m_state.remaining) <= right - area.left()) {
                break;
            }
        }
        painter.setFont(font(pixels, mini ? QFont::DemiBold : QFont::Bold));
        painter.setPen(color);
        const QFontMetrics metrics(painter.font());
        drawTabular(painter, right - tabularWidth(metrics, m_state.remaining), valueBaseline, m_state.remaining);
    }
    if (focused) {
        painter.setPen(QPen(deckLabDeckColor(m_state.deck), 1));
        painter.drawRect(QRect(QPoint(), m_size).adjusted(0, 0, -1, -1));
    }
}

DeckLabDeckStrip::DeckLabDeckStrip(int deck, QWidget* parent)
        : QPushButton(parent),
          m_meter(new DeckLabLevelMeter(Qt::Vertical, 1, this)) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    m_meter->setObjectName(QStringLiteral("decklab-level-%1").arg(deck));
    m_meter->setAccessibleName(tr("Deck %1 level").arg(deck));
    m_renderer.setFamily(font().family());
    DeckLabStripState initial;
    initial.deck = deck;
    m_renderer.setState(initial);
}

void DeckLabDeckStrip::setMode(DeckLabStripRenderer::Mode mode) {
    if (mode == m_renderer.mode()) {
        return;
    }
    m_renderer.setMode(mode);
    placeMeter();
    update();
}

void DeckLabDeckStrip::setState(const DeckLabStripState& state) {
    const QRegion dirty = m_renderer.setState(state);
    if (!dirty.isEmpty()) {
        update(dirty);
    }
}

void DeckLabDeckStrip::setFlash(bool on) {
    const QRegion dirty = m_renderer.setFlash(on);
    if (!dirty.isEmpty()) {
        update(dirty);
    }
}

QSize DeckLabDeckStrip::sizeHint() const {
    const int height = m_renderer.mode() == DeckLabStripRenderer::Mode::Compact ? 116 : 86;
    return QSize(160, maximumHeight() < QWIDGETSIZE_MAX ? maximumHeight() : height);
}

QSize DeckLabDeckStrip::minimumSizeHint() const {
    return QSize(120, sizeHint().height());
}

void DeckLabDeckStrip::placeMeter() {
    const bool full = m_renderer.mode() == DeckLabStripRenderer::Mode::Full;
    m_meter->setGeometry(QRect(3, 6, full ? 5 : 4, std::max(8, height() - 12)));
}

void DeckLabDeckStrip::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    // A focus frame only for keyboard focus; touch never shows one.
    m_renderer.paint(painter, event->rect(), isDown(), m_keyboardFocus && hasFocus());
}

void DeckLabDeckStrip::focusInEvent(QFocusEvent* event) {
    m_keyboardFocus = event->reason() == Qt::TabFocusReason ||
            event->reason() == Qt::BacktabFocusReason || event->reason() == Qt::ShortcutFocusReason;
    QPushButton::focusInEvent(event);
}

void DeckLabDeckStrip::focusOutEvent(QFocusEvent* event) {
    m_keyboardFocus = false;
    QPushButton::focusOutEvent(event);
}

void DeckLabDeckStrip::resizeEvent(QResizeEvent* event) {
    m_renderer.setSize(event->size());
    placeMeter();
    QPushButton::resizeEvent(event);
}

void DeckLabDeckStrip::changeEvent(QEvent* event) {
    if (event->type() == QEvent::FontChange) {
        m_renderer.setFamily(font().family());
        update();
    }
    QPushButton::changeEvent(event);
}

DeckLabMiniStrip::DeckLabMiniStrip(QWidget* parent)
        : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    m_renderer.setMode(DeckLabStripRenderer::Mode::Mini);
    m_renderer.setFamily(font().family());
}

void DeckLabMiniStrip::setState(const DeckLabStripState& state) {
    const QRegion dirty = m_renderer.setState(state);
    if (!dirty.isEmpty()) {
        update(dirty);
    }
}

void DeckLabMiniStrip::setFlash(bool on) {
    const QRegion dirty = m_renderer.setFlash(on);
    if (!dirty.isEmpty()) {
        update(dirty);
    }
}

void DeckLabMiniStrip::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    m_renderer.paint(painter, event->rect(), false, false);
}

void DeckLabMiniStrip::resizeEvent(QResizeEvent* event) {
    m_renderer.setSize(event->size());
    QWidget::resizeEvent(event);
}

void DeckLabMiniStrip::changeEvent(QEvent* event) {
    if (event->type() == QEvent::FontChange) {
        m_renderer.setFamily(font().family());
        update();
    }
    QWidget::changeEvent(event);
}

DeckLabChipButton::DeckLabChipButton(QWidget* parent)
        : QPushButton(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void DeckLabChipButton::setCaption(const QString& caption) {
    if (caption == m_caption) {
        return;
    }
    m_caption = caption;
    updateGeometry();
    update();
}

void DeckLabChipButton::setChips(const QList<Chip>& chips) {
    if (chips == m_chips) {
        return;
    }
    const bool resized = chips.size() != m_chips.size();
    m_chips = chips;
    if (resized) {
        updateGeometry();
    }
    update();
}

QSize DeckLabChipButton::sizeHint() const {
    QFont captionFont = font();
    captionFont.setPixelSize(13);
    captionFont.setWeight(QFont::DemiBold);
    QFont chipFont = font();
    chipFont.setPixelSize(12);
    chipFont.setWeight(QFont::Bold);
    const QFontMetrics captionMetrics(captionFont);
    const QFontMetrics chipMetrics(chipFont);
    int width = 10;
    if (!m_caption.isEmpty()) {
        width += captionMetrics.horizontalAdvance(m_caption) + 8;
    }
    for (const auto& chip : m_chips) {
        width += std::max(22, chipMetrics.horizontalAdvance(chip.text) + 12) + 4;
    }
    return QSize(width + 6, 44);
}

void DeckLabChipButton::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), isDown() ? kPanelPressed : kMiniPanel);
    painter.fillRect(QRect(0, height() - 1, width(), 1), kHairline);
    painter.fillRect(QRect(width() - 1, 0, 1, height()), kHairline);
    QFont captionFont = font();
    captionFont.setPixelSize(13);
    captionFont.setWeight(QFont::DemiBold);
    QFont chipFont = font();
    chipFont.setPixelSize(12);
    chipFont.setWeight(QFont::Bold);
    const QFontMetrics captionMetrics(captionFont);
    const QFontMetrics chipMetrics(chipFont);
    int contentWidth = sizeHint().width() - 6;
    int x = std::max(6, (width() - contentWidth) / 2 + 4);
    if (!m_caption.isEmpty()) {
        painter.setFont(captionFont);
        painter.setPen(isEnabled() ? kSecondary : kDim);
        painter.drawText(QRect(x, 0, captionMetrics.horizontalAdvance(m_caption), height()),
                Qt::AlignVCenter | Qt::AlignLeft, m_caption);
        x += captionMetrics.horizontalAdvance(m_caption) + 8;
    }
    painter.setFont(chipFont);
    for (const auto& chip : m_chips) {
        const int chipWidth = std::max(22, chipMetrics.horizontalAdvance(chip.text) + 12);
        const QRectF area(x, (height() - 22) / 2.0, chipWidth, 22);
        QPainterPath path;
        path.addRoundedRect(area.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        if (chip.on) {
            painter.fillPath(path, chip.lit);
            painter.setPen(inkOn(chip.lit));
        } else {
            painter.setPen(QPen(kLampOff, 1));
            painter.drawPath(path);
            painter.setPen(kDim);
        }
        painter.drawText(area, Qt::AlignCenter, chip.text);
        x += chipWidth + 4;
    }
}

DeckLabMark::DeckLabMark(QWidget* parent)
        : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFixedSize(48, 44);
    setAccessibleName(QCoreApplication::translate("DeckLabDeckStrip", "Deck Lab"));
}

void DeckLabMark::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), kMiniPanel);
    painter.fillRect(QRect(0, height() - 1, width(), 1), kHairline);
    const double size = 26;
    const QPointF origin((width() - size) / 2, (height() - 1 - size) / 2);
    const auto box = [&origin, size](double x, double y, double w, double h) {
        return QRectF(origin.x() + x * size / 100, origin.y() + y * size / 100,
                w * size / 100, h * size / 100);
    };
    painter.fillRect(box(14, 32, 10, 36), kDim);
    painter.fillRect(box(30, 22, 10, 56), kDim);
    painter.fillRect(box(60, 14, 10, 72), kText);
    painter.fillRect(box(76, 30, 10, 40), kText);
    painter.fillRect(box(47, 6, 6, 90), kWarning);
}

DeckLabLevelMeter::DeckLabLevelMeter(Qt::Orientation orientation, int channels, QWidget* parent)
        : QWidget(parent),
          m_orientation(orientation),
          m_channels(std::clamp(channels, 1, 2)) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
}

bool DeckLabLevelMeter::setLevels(double first, double second, bool clipping) {
    const std::array<double, 2> raw{first, second};
    std::array<int, 2> lit{};
    for (int channel = 0; channel < m_channels; ++channel) {
        const double value = std::isfinite(raw[channel]) ? std::clamp(raw[channel], 0.0, 1.0) : 0.0;
        m_levels[channel] = value;
        lit[channel] = std::clamp(int(std::ceil(value * kSegments - 0.25)), 0, kSegments);
    }
    if (lit == m_lit && clipping == m_clip) {
        return false;
    }
    m_lit = lit;
    m_clip = clipping;
    update();
    return true;
}

void DeckLabLevelMeter::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), kPanel);
    const bool vertical = m_orientation == Qt::Vertical;
    const int length = vertical ? height() : width();
    const int thickness = vertical ? width() : height();
    const int laneGap = m_channels > 1 ? 1 : 0;
    const int lane = std::max(1, (thickness - laneGap) / m_channels);
    const double step = length / double(kSegments);
    for (int channel = 0; channel < m_channels; ++channel) {
        for (int segment = 0; segment < kSegments; ++segment) {
            const bool top = segment == kSegments - 1;
            const bool on = segment < m_lit[channel] || (top && m_clip);
            QColor color;
            if (top) {
                color = on ? kWarning : kWarningDim;
            } else if (segment >= kSegments - 4) {
                color = on ? QColor(0xe8, 0xd4, 0x4d) : QColor(0x2c, 0x2a, 0x1a);
            } else {
                color = on ? kPlay : QColor(0x16, 0x2a, 0x20);
            }
            const int from = int(std::round(segment * step));
            const int to = int(std::round((segment + 1) * step)) - 1;
            const int offset = channel * (lane + laneGap);
            const QRect cell = vertical
                    ? QRect(offset, length - to - 1, lane, std::max(1, to - from))
                    : QRect(from, offset, std::max(1, to - from), lane);
            painter.fillRect(cell, color);
        }
    }
}
