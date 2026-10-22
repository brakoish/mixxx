// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabdeckwidgets.h"

#include <QCoreApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QStringList>

#include "moc_decklabdeckwidgets.cpp"

namespace {
// Matte black / graphite with white type. Color only carries state.
const QColor kPanel(0x13, 0x13, 0x13);
const QColor kPanelPressed(0x1e, 0x1e, 0x1e);
const QColor kMiniPanel(0x0e, 0x0e, 0x0e);
const QColor kHairline(0x2a, 0x2a, 0x2a);
const QColor kText(0xf2, 0xf2, 0xf2);
const QColor kSecondary(0xa6, 0xa6, 0xa6);
const QColor kCaption(0x80, 0x80, 0x80);
const QColor kDim(0x5e, 0x5e, 0x5e);
const QColor kLampOff(0x34, 0x34, 0x34);
const QColor kPlay(0x3d, 0xdc, 0x84);
const QColor kCue(0xff, 0x9f, 0x1a);
const QColor kMaster(0xf5, 0xa6, 0x23);
const QColor kSync(0x4c, 0xc2, 0xff);
const QColor kHeadphones(0xe9, 0xe9, 0xe9);
const QColor kSlip(0xc0, 0x84, 0xfc);
const QColor kLoop(0x6e, 0xe1, 0x28);
const QColor kWarning(0xff, 0x4d, 0x4d);
const QColor kWarningDim(0x7c, 0x22, 0x22);

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
    // Full-mode value columns scale with the card width so wider screens
    // keep the same proportions as the 400 px design.
    const int bpmWidth = width * 345 / 1000;
    const int tempoLeft = 12 + bpmWidth + 4;
    const int tempoWidth = width * 205 / 1000;
    const int keyLeft = tempoLeft + tempoWidth + 2;
    const int keyWidth = width * 150 / 1000;
    const int timeLeft = keyLeft + keyWidth;
    switch (m_mode) {
    case Mode::Full:
        switch (part) {
        case Part::Phase:
            return QRect(14, 4, width - 24, 6);
        case Part::Identity:
            return QRect(12, 12, 66, 24);
        case Part::Chips:
            return QRect(width - 112, 12, 104, 24);
        case Part::Title:
            return QRect(78, 12, std::max(0, width - 112 - 4 - 78), 24);
        case Part::Captions:
            return QRect(12, 36, width - 20, 17);
        case Part::Bpm:
            return QRect(12, 53, bpmWidth, height - 53);
        case Part::Tempo:
            return QRect(tempoLeft, 53, tempoWidth, height - 53);
        case Part::Key:
            return QRect(keyLeft, 53, keyWidth, height - 53);
        case Part::Time:
            return QRect(timeLeft, 53, width - timeLeft, height - 53);
        }
        break;
    case Mode::Compact: {
        const int half = (width - 20) / 2;
        switch (part) {
        case Part::Phase:
            return QRect(12, 4, width - 20, 5);
        case Part::Identity:
            return QRect(10, 11, 60, 24);
        case Part::Captions:
            return QRect(width - 116, 11, 112, 24);
        case Part::Title:
            return QRect(10, 36, width - 16, 20);
        case Part::Bpm:
            return QRect(10, 57, half + 2, 32);
        case Part::Time:
            return QRect(12 + half, 57, width - 12 - half, 32);
        case Part::Tempo:
            return QRect(10, 90, 78, height - 90);
        case Part::Key:
            return QRect(width - 56, 90, 56, height - 90);
        case Part::Chips:
            return QRect(88, 90, std::max(0, width - 56 - 88), height - 90);
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
    if (tabularWidth(whole, m_state.remaining) > rect(Part::Time).width() - (mini ? 8 : 10)) {
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
                                      : QRect(14, 14, 26, 20);
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
        painter.setFont(font(12, QFont::DemiBold, true));
        painter.setPen(kDim);
        painter.drawText(area, Qt::AlignVCenter | Qt::AlignLeft, QCoreApplication::translate("DeckLabDeckStrip", "EMPTY"));
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
        painter.fillRect(QRect(area.left() + 2, top, 4, size), QColor(0xd4, 0xd4, 0xd4));
        painter.fillRect(QRect(area.left() + 9, top, 4, size), QColor(0xd4, 0xd4, 0xd4));
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
                artistMetrics.elidedText(m_mode == Mode::Full ? QCoreApplication::translate("DeckLabDeckStrip", "No track loaded")
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
        QColor color(0x22, 0x22, 0x22);
        if (m_state.loaded && m_state.phase < 0) {
            color = QColor(0x1a, 0x1a, 0x1a);
        } else if (m_state.loaded && index == m_state.phase) {
            color = m_state.playing ? kText : QColor(0x8c, 0x8c, 0x8c);
        }
        painter.fillRect(cell, color);
    }
}

int DeckLabStripRenderer::paintChip(QPainter& painter, int x, int y, int height,
        const QString& text, bool on, const QColor& lit, bool alignRight) const {
    const QFont chipFont = font(12, QFont::Bold, true);
    const QFontMetrics metrics(chipFont);
    const int width = metrics.horizontalAdvance(text) + 12;
    const QRectF chip(alignRight ? x - width : x, y, width, height);
    QPainterPath path;
    path.addRoundedRect(chip.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    if (on) {
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
                : compact            ? QRect(40, 11, 30, 24)
                                     : QRect(46, 12, 32, 24);
        paintTransport(painter, transport);
    }
    if (needs(Part::Title)) {
        paintTitle(painter, rect(Part::Title).adjusted(0, 0, -2, 0), 14);
    }
    if (!m_state.loaded) {
        if (!mini && !compact && needs(Part::Bpm)) {
            painter.setFont(font(14, QFont::Normal));
            painter.setPen(kSecondary);
            painter.drawText(QRect(14, 40, m_size.width() - 24, m_size.height() - 44),
                    Qt::AlignCenter, QCoreApplication::translate("DeckLabDeckStrip", "Tap to browse and load a track"));
        }
        if (focused) {
            painter.setPen(QPen(deckLabDeckColor(m_state.deck), 1));
            painter.drawRect(QRect(QPoint(), m_size).adjusted(0, 0, -1, -1));
        }
        return;
    }
    // Lamps and captions.
    if (!mini && needs(Part::Captions)) {
        if (compact) {
            const QRect area = rect(Part::Captions);
            int x = area.right();
            x -= paintChip(painter, x, area.top() + 3, 18, QCoreApplication::translate("DeckLabDeckStrip", "MASTER"), m_state.master, kMaster, true) + 4;
            paintChip(painter, x, area.top() + 3, 18, QCoreApplication::translate("DeckLabDeckStrip", "SYNC"), m_state.sync, kSync, true);
        } else {
            const QRect area = rect(Part::Captions);
            const QFont captionFont = font(12, QFont::DemiBold, true);
            const QFontMetrics metrics(captionFont);
            const int baseline = area.top() + 12;
            painter.setFont(captionFont);
            painter.setPen(kCaption);
            painter.drawText(QPoint(14, baseline), QCoreApplication::translate("DeckLabDeckStrip", "BPM"));
            int x = 14 + metrics.horizontalAdvance(QCoreApplication::translate("DeckLabDeckStrip", "BPM")) + 8;
            x += paintChip(painter, x, area.top() + 1, 15, QCoreApplication::translate("DeckLabDeckStrip", "SYNC"), m_state.sync, kSync, false) + 4;
            paintChip(painter, x, area.top() + 1, 15, QCoreApplication::translate("DeckLabDeckStrip", "MASTER"), m_state.master, kMaster, false);
            painter.setFont(captionFont);
            painter.setPen(kCaption);
            const QRect tempo = rect(Part::Tempo);
            painter.drawText(QPoint(tempo.left() + 2, baseline), QCoreApplication::translate("DeckLabDeckStrip", "TEMPO"));
            int tx = tempo.left() + 2 + metrics.horizontalAdvance(QCoreApplication::translate("DeckLabDeckStrip", "TEMPO")) + 6;
            painter.setPen(kSecondary);
            painter.drawText(QPoint(tx, baseline), m_state.range);
            tx += metrics.horizontalAdvance(m_state.range) + 6;
            // Tempo-center lamp: lit at 0.00%.
            painter.setPen(Qt::NoPen);
            painter.setBrush(m_state.tempoCentered ? kPlay : kLampOff);
            painter.drawEllipse(QRectF(tx, baseline - 7, 7, 7));
            painter.setBrush(Qt::NoBrush);
            const QRect key = rect(Part::Key);
            painter.setPen(kCaption);
            painter.drawText(QPoint(key.left() + 2, baseline), QCoreApplication::translate("DeckLabDeckStrip", "KEY"));
            if (m_state.keyMatch != 0) {
                const int kx = key.left() + 2 + metrics.horizontalAdvance(QCoreApplication::translate("DeckLabDeckStrip", "KEY")) + 6;
                if (m_state.keyMatch > 0) {
                    // Compatible with the reference deck: a small check.
                    QPainterPath check;
                    check.moveTo(kx, baseline - 4);
                    check.lineTo(kx + 3, baseline - 1);
                    check.lineTo(kx + 9, baseline - 8);
                    painter.setPen(QPen(kPlay, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                    painter.drawPath(check);
                } else {
                    painter.setPen(QPen(QColor(0x9a, 0x4a, 0x4a), 1.5));
                    painter.drawLine(QPointF(kx, baseline - 8), QPointF(kx + 7, baseline - 1));
                    painter.drawLine(QPointF(kx + 7, baseline - 8), QPointF(kx, baseline - 1));
                }
            }
            painter.setPen(kCaption);
            const QString remain = QCoreApplication::translate("DeckLabDeckStrip", "REMAIN");
            painter.drawText(QPoint(m_size.width() - 10 - metrics.horizontalAdvance(remain), baseline), remain);
        }
    }
    if (!mini && needs(Part::Chips)) {
        const QRect area = rect(Part::Chips);
        if (compact) {
            int x = area.left();
            const QList<std::pair<QString, std::pair<bool, QColor>>> lamps{
                    {QCoreApplication::translate("DeckLabDeckStrip", "HP"), {m_state.pfl, kHeadphones}},
                    {QCoreApplication::translate("DeckLabDeckStrip", "LOOP %1").arg(m_state.loopSize), {m_state.looping, kLoop}},
                    {QCoreApplication::translate("DeckLabDeckStrip", "KEY"), {m_state.keylock, QColor(0xd7, 0x44, 0x48)}},
                    {QCoreApplication::translate("DeckLabDeckStrip", "Q"), {m_state.quantize, QColor(0xd7, 0x44, 0x48)}},
                    {QCoreApplication::translate("DeckLabDeckStrip", "SLIP"), {m_state.slip, kSlip}}};
            const QFontMetrics metrics(font(12, QFont::Bold, true));
            for (const auto& lamp : lamps) {
                if (!lamp.second.first) {
                    continue;
                }
                const int width = metrics.horizontalAdvance(lamp.first) + 12;
                if (x + width > area.right()) {
                    break;
                }
                x += paintChip(painter, x, area.top() + 3, 18, lamp.first, true, lamp.second.second, false) + 3;
            }
        } else {
            int x = area.right();
            x -= paintChip(painter, x, area.top() + 3, 18, QCoreApplication::translate("DeckLabDeckStrip", "HP"), m_state.pfl, kHeadphones, true) + 4;
            if (m_state.slip) {
                paintChip(painter, x, area.top() + 3, 18, QCoreApplication::translate("DeckLabDeckStrip", "SLIP"), true, kSlip, true);
            }
        }
    }
    // Values.
    const int valueBaseline = mini ? (m_size.height() + 10) / 2 : compact ? 84 : 81;
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
            painter.setPen(QColor(0xc4, 0xc4, 0xc4));
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
        } else {
            painter.setFont(font(18, QFont::DemiBold));
            painter.setPen(m_state.tempoCentered ? kSecondary : kText);
            drawTabular(painter, area.left() + 2, valueBaseline, m_state.tempo);
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
                painter.setBrush(m_state.keyMatch > 0 ? kPlay : QColor(0x7a, 0x3a, 0x3a));
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
        const int minimum = mini ? 14 : compact ? 20 : 22;
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
    m_renderer.paint(painter, event->rect(), isDown(), hasFocus() && focusPolicy() != Qt::NoFocus);
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
    painter.fillRect(rect(), isDown() ? QColor(0x26, 0x26, 0x26) : QColor(0x15, 0x15, 0x15));
    painter.fillRect(QRect(0, height() - 1, width(), 1), QColor(0x26, 0x26, 0x26));
    painter.fillRect(QRect(width() - 1, 0, 1, height()), QColor(0x26, 0x26, 0x26));
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
        painter.setPen(isEnabled() ? QColor(0xa6, 0xa6, 0xa6) : QColor(0x55, 0x55, 0x55));
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
    painter.fillRect(rect(), QColor(0x0c, 0x0c, 0x0c));
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
                color = on ? QColor(0xe5, 0x48, 0x4d) : QColor(0x3a, 0x1c, 0x1d);
            } else if (segment >= kSegments - 4) {
                color = on ? QColor(0xf5, 0xa6, 0x23) : QColor(0x2e, 0x26, 0x18);
            } else {
                color = on ? QColor(0x3d, 0xdc, 0x84) : QColor(0x18, 0x2a, 0x20);
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
