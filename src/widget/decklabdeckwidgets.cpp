// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabdeckwidgets.h"

#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QStringList>
#include <QTimer>

#include "moc_decklabdeckwidgets.cpp"

namespace {
// Brand neutrals (ground, panel, raise, line, muted, text). Deck colors
// carry identity; live red is only for the end-of-track warning and clip.
const QColor kPanel(0x0d, 0x0f, 0x12);
const QColor kPanelPressed(0x20, 0x24, 0x2b);
const QColor kMiniPanel(0x16, 0x19, 0x1e);
const QColor kHairline(0x2c, 0x31, 0x3a);
const QColor kText(0xf2, 0xf4, 0xf7);
const QColor kMuted(0x9a, 0xa3, 0xaf);
const QColor kDim(0x6b, 0x73, 0x80);
const QColor kLampOff(0x2c, 0x31, 0x3a);
const QColor kLampLit(0xf2, 0xf4, 0xf7);
const QColor kDecimals(0xc9, 0xce, 0xd6);
const QColor kPlay(0x2f, 0xd2, 0x7a);
const QColor kCue(0xff, 0x7a, 0x1a);
const QColor kWarning(0xff, 0x3d, 0x2e);
const QColor kWarningDim(0x8c, 0x22, 0x19);
const QString kUiFamily = QStringLiteral("Barlow");
const QString kNumeralFamily = QStringLiteral("Barlow Semi Condensed");

QString tr(const char* text) {
    return QCoreApplication::translate("DeckLabDeckStrip", text);
}

int digitCell(const QFontMetrics& metrics) {
    int cell = 0;
    for (char digit = '0'; digit <= '9'; ++digit) {
        cell = std::max(cell, metrics.horizontalAdvance(QLatin1Char(digit)));
    }
    return cell;
}

// Fixed-width digit cells (tabular figures): a running clock or a moving
// tempo never shifts the rest of the readout sideways.
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

// Phrase countdowns are bars.beats; the unit is spelled out beside them.
QString barsCaption() {
    return QCoreApplication::translate("DeckLabDeckStrip", "BARS");
}

// Dark ink on light lamps, light ink on dark ones.
QColor inkOn(const QColor& color) {
    return color.lightnessF() > 0.55 ? QColor(0x0d, 0x0f, 0x12) : kText;
}

int baselineFor(const QRect& area, const QFontMetrics& metrics) {
    return area.top() + (area.height() + metrics.ascent() - metrics.descent()) / 2;
}

QFont chipCaptionFont(const QFont& base) {
    QFont font = base;
    font.setFamilies({kUiFamily, base.family()});
    font.setPixelSize(DeckLabStripRenderer::kLabel);
    font.setWeight(QFont::DemiBold);
    font.setLetterSpacing(QFont::PercentageSpacing, 106);
    return font;
}
} // namespace

void deckLabLoadFonts(const QString& resourcePath) {
    static bool loaded = false;
    if (loaded) {
        return;
    }
    loaded = true;
    QDir fonts(resourcePath);
    if (!fonts.cd(QStringLiteral("skins/DeckLab/fonts"))) {
        return;
    }
    const QStringList files = fonts.entryList(
            {QStringLiteral("*.ttf"), QStringLiteral("*.otf")}, QDir::Files | QDir::Readable);
    for (const QString& file : files) {
        QFontDatabase::addApplicationFont(fonts.filePath(file));
    }
}

QFont DeckLabStripRenderer::uiFont(int pixels, QFont::Weight weight) const {
    QFont font;
    font.setFamilies(m_fallback.isEmpty() ? QStringList{kUiFamily} : QStringList{kUiFamily, m_fallback});
    font.setPixelSize(std::max(kLabel, pixels));
    font.setWeight(weight);
    return font;
}

QFont DeckLabStripRenderer::numeralFont(int pixels) const {
    QFont font;
    font.setFamilies(m_fallback.isEmpty() ? QStringList{kNumeralFamily, kUiFamily}
                                          : QStringList{kNumeralFamily, kUiFamily, m_fallback});
    font.setPixelSize(std::max(kLabel, pixels));
    font.setWeight(QFont::DemiBold);
    return font;
}

QFont DeckLabStripRenderer::labelFont() const {
    QFont font = uiFont(kLabel, QFont::DemiBold);
    font.setLetterSpacing(QFont::PercentageSpacing, 106);
    return font;
}

void DeckLabStripRenderer::layout() {
    const int width = m_size.width();
    m_rects.fill(QRect());
    if (width <= 0) {
        return;
    }
    const auto set = [this](Part part, const QRect& area) {
        m_rects[static_cast<int>(part)] = area;
    };
    switch (m_mode) {
    case Mode::Full: {
        // r4-decks: four rows in the same 104 px (audit move 1), read in one
        // glance as on a club player:
        //  1. identity: badge, play state, title · artist;
        //  2. BPM (one decimal), tempo, key and the clock at 36/24 px;
        //  3. flat lamps (MASTER, SYNC, headphone cue), the tempo range or
        //     OVER, and lit exceptions (LOOP, SLIP, KEY, QNT OFF);
        //  4. the rekordbox phrase and countdown on the left, and a two-row
        //     phase meter on the right: the reference deck's beat above this
        //     deck's, so two decks in time light the same cells.
        const QFontMetrics hero(numeralFont(kHero));
        const QFontMetrics readout(numeralFont(kReadout));
        const int bpm = tabularWidth(hero, QStringLiteral("000")) +
                tabularWidth(readout, QStringLiteral(".0")) + 4;
        const int clock = tabularWidth(hero, QString::fromUtf8("−00:00")) + 4;
        // r5-decks: +10 for a key shift ("8A +1"), all the tempo can give and
        // keep OVER ±18.82% outlined at 399 px; a check mark that no longer
        // fits beside a shifted key is dropped, never squeezed.
        const int key = std::max(readout.horizontalAdvance(QStringLiteral("12A")),
                                readout.horizontalAdvance(QStringLiteral("F#m"))) +
                30;
        constexpr int kValuesTop = 24;
        constexpr int kValuesHeight = 32;
        const QRect time(width - 10 - clock, kValuesTop, clock + 10, kValuesHeight);
        const QRect keyArea(time.left() - 6 - key, kValuesTop, key, kValuesHeight);
        const QRect bpmArea(12, kValuesTop, bpm, kValuesHeight);
        set(Part::Identity, QRect(12, 1, 72, 26));
        set(Part::Title, QRect(86, 1, std::max(0, width - 96), 24));
        set(Part::Bpm, bpmArea);
        set(Part::Time, time);
        set(Part::Key, keyArea);
        set(Part::Tempo, QRect(bpmArea.right() + 7, kValuesTop,
                                 std::max(0, keyArea.left() - 6 - bpmArea.right() - 7), kValuesHeight));
        set(Part::Chips, QRect(12, 58, std::max(0, width - 22), 21));
        const int phase = phaseMeterWidth();
        set(Part::Phase, QRect(width - 10 - phase, 81, phase, 22));
        // Left of the phase meter: lit exceptions (LOOP, SLIP, KEY, QNT OFF)
        // and the rekordbox phrase, right-aligned against the meter, so a
        // USB track never loses its beat position.
        set(Part::Phrase, QRect(12, 81, std::max(0, width - 22 - phase - 14), 22));
        break;
    }
    case Mode::Compact: {
        const QFontMetrics hero(numeralFont(kHero));
        const int clock = tabularWidth(hero, QString::fromUtf8("−00:00")) + 4;
        const QRect time(width - 10 - clock, 50, clock + 10, 38);
        set(Part::Identity, QRect(10, 6, 64, 28));
        set(Part::Title, QRect(76, 6, std::max(0, width - 84), 28));
        // r4-decks: two thin rows, the reference deck's beat over this one's.
        set(Part::Phase, QRect(12, 37, width - 20, 12));
        set(Part::Bpm, QRect(10, 50, std::max(0, time.left() - 14), 38));
        set(Part::Time, time);
        // r5-decks: the key box is 10 px wider for a key shift.
        set(Part::Tempo, QRect(10, 89, std::max(0, width - 98), 26));
        set(Part::Key, QRect(width - 86, 89, 86, 26));
        set(Part::Chips, QRect(10, 116, width - 18, 22));
        // The phrase readout shares the lamp row: lit lamps keep their place
        // and the phrase takes the room that is left, right-aligned.
        if (!m_state.phrase.name.isEmpty()) {
            set(Part::Phrase, rect(Part::Chips));
        }
        break;
    }
    case Mode::Mini: {
        const int height = m_size.height();
        const QFontMetrics value(numeralFont(kTitle));
        const int clock = tabularWidth(value, QString::fromUtf8("−00:00")) + 4;
        const int bpm = tabularWidth(value, QStringLiteral("000.00")) + 4;
        const int key = std::max(value.horizontalAdvance(QStringLiteral("12A")),
                                value.horizontalAdvance(QStringLiteral("F#m"))) +
                4;
        const QRect time(width - 8 - clock, 0, clock + 8, height);
        const QRect bpmArea(time.left() - 12 - bpm, 0, bpm, height);
        const QRect keyArea(bpmArea.left() - 12 - key, 0, key, height);
        set(Part::Identity, QRect(4, 0, 58, height));
        set(Part::Title, QRect(66, 0, std::max(0, keyArea.left() - 12 - 66), height));
        set(Part::Key, keyArea);
        set(Part::Bpm, bpmArea);
        set(Part::Time, time);
        break;
    }
    }
}

int DeckLabStripRenderer::clockExcess() const {
    if (m_mode == Mode::Mini) {
        return 0;
    }
    const QRect time = rect(Part::Time);
    const int needed = tabularWidth(QFontMetrics(numeralFont(kHero)), m_state.remaining);
    return std::max(0, needed - (time.width() - 10));
}

QRegion DeckLabStripRenderer::setState(const DeckLabStripState& next) {
    const DeckLabStripState previous = m_state;
    if (previous == next) {
        return QRegion();
    }
    m_state = next;
    if (previous.phrase.name.isEmpty() != next.phrase.name.isEmpty()) {
        // The phrase readout appears or goes: the lamp row lays out again.
        layout();
        return QRegion(QRect(QPoint(), m_size));
    }
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
            previous.matched != next.matched || previous.range != next.range ||
            previous.overRange != next.overRange ||
            previous.pfl != next.pfl || previous.slip != next.slip || previous.keylock != next.keylock ||
            previous.quantize != next.quantize || previous.looping != next.looping ||
            previous.loopSize != next.loopSize) {
        add(Part::Chips);
        if (m_mode == Mode::Full) {
            add(Part::Phrase); // the exceptions share the phrase's row
        }
    }
    if (previous.bpm != next.bpm) {
        add(Part::Bpm);
    }
    if (previous.tempo != next.tempo || previous.tempoCentered != next.tempoCentered ||
            previous.overRange != next.overRange) {
        add(Part::Tempo);
    }
    if (previous.key != next.key || previous.keyColor != next.keyColor || previous.keyMatch != next.keyMatch ||
            previous.keyShift != next.keyShift || previous.keyMatched != next.keyMatched) {
        add(Part::Key);
        if (m_mode == Mode::Full && !rect(Part::Key).isEmpty()) {
            // r5-decks: the MATCH outline reaches 4 px into the gaps either
            // side of the key box (never the tempo or the clock), also where
            // a long clock moved the key aside.
            const QRect outline = rect(Part::Key).adjusted(-4, 0, 4, 0);
            dirty += outline;
            if (const int excess = clockExcess(); excess > 0) {
                dirty += outline.translated(-excess, 0);
            }
        }
    }
    if (previous.remaining.size() != next.remaining.size()) {
        // A clock that gains or loses a digit moves the key and tempo (full)
        // or decides whether the BPM decimals fit (compact): repaint all.
        return QRegion(QRect(QPoint(), m_size));
    }
    if (previous.remaining != next.remaining || previous.endWarning != next.endWarning) {
        dirty += rect(Part::Time).adjusted(-clockExcess(), 0, 0, 0);
    }
    if (previous.phase != next.phase || previous.phaseStep != next.phaseStep ||
            previous.referenceDeck != next.referenceDeck ||
            previous.referencePhase != next.referencePhase ||
            previous.referenceStep != next.referenceStep ||
            previous.referencePlaying != next.referencePlaying) {
        add(Part::Phase);
    }
    if (previous.phrase != next.phrase) {
        add(Part::Phrase);
    }
    return dirty;
}

QRegion DeckLabStripRenderer::setFlash(bool on) {
    if (m_flash == on) {
        return QRegion();
    }
    m_flash = on;
    return m_state.endWarning ? QRegion(rect(Part::Time).adjusted(-clockExcess(), 0, 0, 0)) : QRegion();
}

QString DeckLabStripRenderer::modesText() const {
    QStringList modes;
    if (m_state.pfl) {
        modes << tr("HP");
    }
    if (m_state.looping) {
        modes << tr("LOOP %1").arg(m_state.loopSize);
    }
    if (m_state.keylock) {
        modes << tr("KEY");
    }
    if (m_state.quantize) {
        modes << tr("QNT");
    }
    if (m_state.slip) {
        modes << tr("SLIP");
    }
    if (m_state.sync) {
        modes << tr("SYNC");
    }
    if (m_state.master) {
        modes << tr("MASTER");
    }
    return modes.join(QLatin1Char(' '));
}

QString DeckLabStripRenderer::roundedBpm(const QString& bpm) {
    bool ok = false;
    const double value = bpm.toDouble(&ok);
    return ok && util_isfinite(value) ? QString::number(qRound64(value)) : wholePart(bpm);
}

QStringList DeckLabStripRenderer::overflowingParts() const {
    QStringList overflowing;
    if (!m_state.loaded || m_size.isEmpty()) {
        return overflowing;
    }
    const bool mini = m_mode == Mode::Mini;
    const bool compact = m_mode == Mode::Compact;
    const QFontMetrics hero(numeralFont(mini ? kTitle : kHero));
    const QFontMetrics readout(numeralFont(mini ? kTitle : kReadout));
    // BPM: the whole part always; compact cards may drop the decimals and
    // then show the rounded value, which can be one digit longer (99.7: 100).
    const int compactWhole = std::max(tabularWidth(hero, wholePart(m_state.bpm)),
            tabularWidth(hero, roundedBpm(m_state.bpm)));
    const int bpm = mini ? tabularWidth(hero, m_state.bpm)
            : compact    ? compactWhole
                         : tabularWidth(hero, wholePart(m_state.bpm)) +
                    tabularWidth(readout, fractionPart(m_state.bpm));
    if (bpm > rect(Part::Bpm).width()) {
        overflowing << QStringLiteral("bpm");
    }
    const int clock = tabularWidth(hero, m_state.remaining);
    if (m_mode == Mode::Full && clock > rect(Part::Time).width() - 6 + clockExcess()) {
        overflowing << QStringLiteral("time");
    }
    // r5-decks: the key and its shift; the check mark steps aside for them.
    if ((mini ? readout.horizontalAdvance(m_state.key) : keyTextWidth()) > (mini ? rect(Part::Key).width() : keyRoom())) {
        overflowing << QStringLiteral("key");
    }
    if (compact &&
            rect(Part::Bpm).left() + 2 + compactWhole + 8 >
                    rect(Part::Time).right() - 8 - clock) {
        // Compact cards: the BPM and the clock share one row.
        overflowing << QStringLiteral("row");
    }
    return overflowing;
}

int DeckLabStripRenderer::keyRoom() const {
    return rect(Part::Key).width() - (m_mode == Mode::Compact ? 8 : 2);
}

DeckLabStripRenderer::KeyText DeckLabStripRenderer::keyText() const {
    const QFontMetrics readout(numeralFont(m_mode == Mode::Mini ? kTitle : kReadout));
    KeyText text;
    const auto measure = [this, &readout](const QString& key, int shiftPixels) {
        KeyText candidate;
        candidate.key = key;
        candidate.shiftPixels = shiftPixels;
        candidate.keyWidth = readout.horizontalAdvance(key);
        candidate.width = candidate.keyWidth;
        if (m_mode != Mode::Mini && !m_state.keyShift.isEmpty()) {
            candidate.width += 3 + QFontMetrics(numeralFont(shiftPixels)).horizontalAdvance(m_state.keyShift);
        }
        return candidate;
    };
    text = measure(m_state.key, kSecondary);
    if (m_mode == Mode::Mini || text.width <= keyRoom()) {
        return text;
    }
    const int slash = m_state.key.indexOf(QLatin1Char('/'));
    if (slash > 0) {
        text = measure(m_state.key.mid(slash + 1), kSecondary);
    }
    if (text.width > keyRoom() && !m_state.keyShift.isEmpty()) {
        text = measure(text.key, kLabel);
    }
    return text;
}

QRect DeckLabStripRenderer::keyTarget() const {
    // At least 44 px tall around the key, inside the readout.
    if (m_mode == Mode::Mini || !m_state.loaded) {
        return QRect();
    }
    QRect area = rect(Part::Key).translated(m_mode == Mode::Full ? -clockExcess() : 0, 0);
    const int grow = std::max(0, 44 - area.height());
    area.adjust(0, -grow / 2, 0, grow - grow / 2);
    if (m_mode == Mode::Compact) {
        // Compact cards: below the clock's row, never on the clock.
        area.moveTop(std::max(area.top(), rect(Part::Time).bottom() + 1));
    }
    return area.intersected(QRect(QPoint(), m_size));
}

QRect DeckLabStripRenderer::bpmTarget() const {
    if (m_mode == Mode::Mini || !m_state.loaded) {
        return QRect();
    }
    QRect area = rect(Part::Bpm);
    if (m_mode == Mode::Compact) {
        // Compact cards share the row with the clock: the BPM's own digits.
        area.setRight(std::min(area.right(), rect(Part::Time).left() - 4));
    }
    const int grow = std::max(0, 44 - area.height());
    area.adjust(0, -grow / 2, 0, grow - grow / 2);
    return area.intersected(QRect(QPoint(), m_size));
}

int DeckLabStripRenderer::chipWidth(const QString& text) const {
    return QFontMetrics(labelFont()).horizontalAdvance(text) + 14;
}

int DeckLabStripRenderer::paintChip(QPainter& painter, int x, int y, const QString& text,
        bool on, const QColor& lit, bool outlined) const {
    const int width = chipWidth(text);
    const QRectF chip(x, y, width, 20);
    QPainterPath path;
    path.addRoundedRect(chip.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    if (on && outlined) {
        painter.setPen(QPen(lit, 1.5));
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
    painter.setFont(labelFont());
    painter.drawText(chip, Qt::AlignCenter, text);
    return width;
}

int DeckLabStripRenderer::lampWidth(const QString& text, bool headphones) const {
    // 6 px pad, 6 px dot, 5 px gap, the label (or a 14 px glyph), 7 px pad.
    const int label = headphones ? 14 : QFontMetrics(labelFont()).horizontalAdvance(text);
    return 6 + 6 + 5 + label + 7;
}

int DeckLabStripRenderer::paintLamp(QPainter& painter, int x, int y, const QString& text,
        Lamp look, const QColor& lit, bool headphones, int width) const {
    width = std::max(width, lampWidth(text, headphones));
    const QRectF chip(x, y, width, 20);
    QColor ink = kMuted;
    QColor dot = kLampOff;
    if (look != Lamp::Off) {
        QPainterPath path;
        path.addRoundedRect(chip.adjusted(0.75, 0.75, -0.75, -0.75), 3, 3);
        if (look == Lamp::Filled) {
            painter.fillPath(path, lit);
            ink = inkOn(lit);
        } else {
            painter.setPen(QPen(lit, 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(path);
            ink = lit;
        }
        dot = ink;
    }
    // The dot is the lamp's LED: dark when off, lit with the label when on.
    painter.setPen(Qt::NoPen);
    painter.setBrush(dot);
    painter.drawEllipse(QRectF(x + 6, y + 7, 6, 6));
    painter.setBrush(Qt::NoBrush);
    const QRectF label(x + 17, y, width - 17 - 7, 20);
    if (headphones) {
        paintHeadphones(painter, QRectF(label.left(), y + 3, 14, 14), ink);
    } else {
        painter.setFont(labelFont());
        painter.setPen(ink);
        painter.drawText(label, Qt::AlignVCenter | Qt::AlignLeft, text);
    }
    return width;
}

void DeckLabStripRenderer::paintHeadphones(QPainter& painter, const QRectF& box, const QColor& ink) {
    // Headband arc and two ear cups: the club symbol for headphone cue.
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal w = box.width();
    const qreal h = box.height();
    QPainterPath band;
    band.moveTo(box.left() + w * 0.14, box.top() + h * 0.62);
    band.cubicTo(box.left() + w * 0.14, box.top() - h * 0.06, box.right() - w * 0.14,
            box.top() - h * 0.06, box.right() - w * 0.14, box.top() + h * 0.62);
    painter.setPen(QPen(ink, std::max<qreal>(1.6, w * 0.13), Qt::SolidLine, Qt::RoundCap));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(band);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ink);
    painter.drawRoundedRect(QRectF(box.left(), box.top() + h * 0.52, w * 0.3, h * 0.48), 1.5, 1.5);
    painter.drawRoundedRect(QRectF(box.right() - w * 0.3, box.top() + h * 0.52, w * 0.3, h * 0.48), 1.5, 1.5);
    painter.restore();
}

QList<DeckLabStripRenderer::Exception> DeckLabStripRenderer::exceptions() const {
    // Club DJs leave quantize on: only its absence is worth a lamp.
    QList<Exception> lit;
    if (m_state.looping) {
        lit.append({tr("LOOP %1").arg(m_state.loopSize), false});
    }
    if (m_state.slip) {
        lit.append({tr("SLIP"), false});
    }
    if (m_state.keylock) {
        lit.append({tr("KEY"), false});
    }
    if (!m_state.quantize) {
        lit.append({tr("QNT OFF"), true});
    }
    return lit;
}

int DeckLabStripRenderer::phaseMeterWidth() const {
    // A 12 px row label, then four beat cells (about 30 px each at 800 px).
    return std::clamp(m_size.width() * 36 / 100, 120, 200);
}

int DeckLabStripRenderer::paintExceptions(QPainter& painter, const QRect& area, int reserve) const {
    // Lit exceptions, most important first, as many as fit before the room
    // the phrase needs.
    int x = area.left() + 2;
    const int right = area.left() + area.width() - reserve;
    const int top = area.top() + (area.height() - 20) / 2;
    for (const auto& lamp : exceptions()) {
        if (x + chipWidth(lamp.text) > right) {
            break;
        }
        x += paintChip(painter, x, top, lamp.text, true, kLampLit, lamp.outlined) + 4;
    }
    return x;
}

void DeckLabStripRenderer::paintBadge(QPainter& painter) const {
    const QRect badge = m_mode == Mode::Mini ? QRect(6, (m_size.height() - 22) / 2, 26, 22)
            : m_mode == Mode::Compact        ? QRect(12, 8, 26, 24)
                                             : QRect(14, 3, 30, 23);
    const QColor color = deckLabDeckColor(m_state.deck);
    QPainterPath path;
    painter.setFont(numeralFont(m_mode == Mode::Mini ? kBody : kTitle));
    if (!m_state.loaded) {
        // An empty deck keeps its identity: the badge outlined in the full
        // deck color (a darkened fill turned amber brown and lime olive).
        path.addRoundedRect(QRectF(badge).adjusted(0.75, 0.75, -0.75, -0.75), 3, 3);
        painter.setPen(QPen(color, 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
        painter.setPen(color);
        painter.drawText(badge, Qt::AlignCenter, QString::number(m_state.deck));
        return;
    }
    path.addRoundedRect(QRectF(badge), 3, 3);
    painter.fillPath(path, color);
    painter.setPen(inkOn(color));
    painter.drawText(badge, Qt::AlignCenter, QString::number(m_state.deck));
}

void DeckLabStripRenderer::paintTransport(QPainter& painter, const QRect& area) const {
    const QPoint center = area.center();
    if (!m_state.loaded) {
        // Empty deck: no glyph; the outlined badge already says so.
        return;
    }
    if (m_state.playing) {
        // PLAY: solid green triangle, the club-standard play lamp.
        const int size = m_mode == Mode::Mini ? 12 : 16;
        QPainterPath triangle;
        const QPointF origin(area.left() + 2, center.y() - size / 2.0);
        triangle.moveTo(origin);
        triangle.lineTo(origin.x(), origin.y() + size);
        triangle.lineTo(origin.x() + size * 0.9, origin.y() + size / 2.0);
        triangle.closeSubpath();
        painter.fillPath(triangle, kPlay);
    } else if (m_state.cue) {
        // CUE: orange, as on club players, when paused on the main cue. An
        // outlined chip, so it never reads as part of the amber deck 2 badge.
        const QFont label = labelFont();
        const int width = QFontMetrics(label).horizontalAdvance(tr("CUE")) + 8;
        const QRectF chip(area.left(), center.y() - 9.5, width, 19);
        QPainterPath path;
        path.addRoundedRect(chip.adjusted(0.75, 0.75, -0.75, -0.75), 3, 3);
        painter.setPen(QPen(kCue, 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
        painter.setFont(label);
        painter.setPen(kCue);
        painter.drawText(chip, Qt::AlignCenter, tr("CUE"));
    } else {
        // PAUSE: two bars.
        const int size = m_mode == Mode::Mini ? 12 : 16;
        const int top = center.y() - size / 2;
        painter.fillRect(QRect(area.left() + 2, top, 5, size), kDecimals);
        painter.fillRect(QRect(area.left() + 11, top, 5, size), kDecimals);
    }
}

void DeckLabStripRenderer::paintTitle(QPainter& painter, const QRect& area) const {
    if (area.width() <= 0) {
        return;
    }
    const QFont titleFont = uiFont(kTitle, QFont::DemiBold);
    const QFont artistFont = uiFont(kSecondary, QFont::Medium);
    const QFontMetrics titleMetrics(titleFont);
    const QFontMetrics artistMetrics(artistFont);
    const int baseline = baselineFor(area, titleMetrics);
    if (!m_state.loaded) {
        painter.setFont(artistFont);
        painter.setPen(kMuted);
        painter.drawText(QPoint(area.left(), baseline),
                artistMetrics.elidedText(m_mode == Mode::Compact ? tr("Tap to load") : tr("Empty"),
                        Qt::ElideRight, area.width()));
        return;
    }
    const QString title = m_state.title.isEmpty() ? tr("Untitled") : m_state.title;
    const int titleWidth = titleMetrics.horizontalAdvance(title);
    painter.setFont(titleFont);
    painter.setPen(kText);
    // Four-deck cards drop the artist before anything gets smaller.
    if (titleWidth >= area.width() || m_state.artist.isEmpty() || m_mode == Mode::Compact) {
        painter.drawText(QPoint(area.left(), baseline), titleMetrics.elidedText(title, Qt::ElideRight, area.width()));
        return;
    }
    painter.drawText(QPoint(area.left(), baseline), title);
    const QString artist = QStringLiteral("  ·  ") + m_state.artist;
    const int remaining = area.width() - titleWidth;
    if (remaining > artistMetrics.horizontalAdvance(QStringLiteral("  ·  W…"))) {
        painter.setFont(artistFont);
        painter.setPen(kMuted);
        painter.drawText(QPoint(area.left() + titleWidth, baseline),
                artistMetrics.elidedText(artist, Qt::ElideRight, remaining));
    }
}

void DeckLabStripRenderer::paintPhase(QPainter& painter) const {
    const QRect area = rect(Part::Phase);
    if (area.isEmpty() || !m_state.loaded) {
        return;
    }
    // r4-decks: a phase meter compared with the reference deck, as on a CDJ.
    // The reference's beat (the tempo master, else another playing deck) is
    // the top row in its deck color, this deck's beat the row below; in
    // phase the lit cells line up and fill to the same edge. Without a
    // reference (this deck is the master, or nothing else plays) the top row
    // stays dark.
    const bool compact = m_mode == Mode::Compact;
    const int rowHeight = compact ? 5 : 10;
    const int rowGap = 2;
    int left = area.left();
    if (!compact) {
        // Each row carries the number of the deck it shows, in its color.
        const QFont label = labelFont();
        const QFontMetrics metrics(label);
        const int labelWidth = 12;
        painter.setFont(label);
        const auto number = [&](int deck, int top) {
            if (deck <= 0) {
                return;
            }
            painter.setPen(deckLabDeckColor(deck));
            const QRect row(left, top, labelWidth, rowHeight);
            painter.drawText(QPoint(row.left() + (labelWidth - metrics.horizontalAdvance(QString::number(deck))) / 2,
                                     baselineFor(row, metrics)),
                    QString::number(deck));
        };
        number(m_state.referenceDeck, area.top());
        number(m_state.deck, area.top() + rowHeight + rowGap);
        left += labelWidth + 4;
    }
    const int width = area.left() + area.width() - left;
    paintPhaseRow(painter, QRect(left, area.top(), width, rowHeight), m_state.referenceDeck,
            m_state.referencePhase, m_state.referenceStep, m_state.referencePlaying);
    paintPhaseRow(painter, QRect(left, area.top() + rowHeight + rowGap, width, rowHeight),
            m_state.deck, m_state.phase, m_state.phaseStep, m_state.playing);
}

void DeckLabStripRenderer::paintPhaseRow(QPainter& painter, const QRect& cells, int deck,
        int beat, int step, bool playing) const {
    const int gap = 3;
    const int segment = (cells.width() - 3 * gap) / 4;
    const bool shown = deck > 0 && beat >= 0;
    QColor color = shown ? deckLabDeckColor(deck) : kMiniPanel;
    if (shown && !playing) {
        color = color.darker(170);
    }
    for (int index = 0; index < 4; ++index) {
        const QRect cell(cells.left() + index * (segment + gap), cells.top(), segment, cells.height());
        if (!shown) {
            painter.fillRect(cell, kMiniPanel);
            continue;
        }
        if (index != beat) {
            painter.fillRect(cell, kLampOff);
            continue;
        }
        // The current beat: the part already played is full color, the rest
        // of the beat dimmer, so the edge moves through the beat.
        if (step < 0) {
            painter.fillRect(cell, color);
            continue;
        }
        const int lit = std::max(1, cell.width() * (step + 1) / 8);
        painter.fillRect(cell, color.darker(260));
        painter.fillRect(QRect(cell.left(), cell.top(), lit, cell.height()), color);
    }
}

int DeckLabStripRenderer::phraseWidth() const {
    // Room for the longest common name ("CHORUS 2"), a two-digit count and
    // the BARS unit after it.
    const QFontMetrics label(labelFont());
    const int name = label.horizontalAdvance(QStringLiteral("CHORUS 2")) + 14;
    const int count = tabularWidth(QFontMetrics(numeralFont(kTitle)), QStringLiteral("00.0"));
    return name + 6 + count + 4 + label.horizontalAdvance(barsCaption()) + 2;
}

void DeckLabStripRenderer::paintPhrase(QPainter& painter, int left) const {
    const QRect area = rect(Part::Phrase).adjusted(std::max(0, left - rect(Part::Phrase).left()), 0, 0, 0);
    if (area.width() <= 0 || !m_state.loaded || m_state.phrase.name.isEmpty()) {
        return;
    }
    // The countdown sits right-aligned in a fixed cell of tabular figures,
    // so neither it nor the chip shifts as it runs; a 14 px BARS caption
    // follows it when there is room (bars.beats would otherwise read as a
    // decimal), and the phrase name is a chip in the phrase's color before.
    const QFont count = numeralFont(kTitle);
    const QFontMetrics countMetrics(count);
    const QFont label = labelFont();
    const QFontMetrics labelMetrics(label);
    const QString& countdown = m_state.phrase.countdown;
    const int cell = std::max(tabularWidth(countMetrics, QStringLiteral("00.0")),
            tabularWidth(countMetrics, countdown));
    if (cell > area.width()) {
        return;
    }
    const int baseline = baselineFor(area, countMetrics);
    const QString caption = barsCaption();
    const int captionWidth = labelMetrics.horizontalAdvance(caption);
    int right = area.left() + area.width();
    if (area.width() >= cell + 4 + captionWidth + 6 + 40) {
        painter.setFont(label);
        painter.setPen(kMuted);
        painter.drawText(QPoint(right - captionWidth, baseline), caption);
        right -= captionWidth + 4;
    }
    painter.setFont(count);
    painter.setPen(kText);
    drawTabular(painter, right - tabularWidth(countMetrics, countdown), baseline, countdown);
    const int room = right - area.left() - cell - 6;
    const QColor color = m_state.phrase.color.isValid() ? m_state.phrase.color : kDim;
    const QString text = labelMetrics.elidedText(
            m_state.phrase.name.toUpper(), Qt::ElideRight, std::max(0, room - 14));
    const int top = area.top() + (area.height() - 20) / 2;
    if (room >= 40 && !text.isEmpty() && text != QStringLiteral("…")) {
        // r4-decks: an outline in the phrase color with white text. Filled
        // color stays for deck identity and MASTER (a gold DOWN chip on deck
        // 2 read as the deck itself); the overview bands keep the fills.
        const int width = std::min(room, labelMetrics.horizontalAdvance(text) + 14);
        const QRectF chip(area.left() + room - width, top, width, 20);
        QPainterPath path;
        path.addRoundedRect(chip.adjusted(0.75, 0.75, -0.75, -0.75), 3, 3);
        painter.setPen(QPen(color, 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
        painter.setFont(label);
        painter.setPen(kText);
        painter.drawText(chip, Qt::AlignCenter, text);
    } else if (room >= 8) {
        // No room for the name: the phrase color still shows which it is.
        QPainterPath path;
        path.addRoundedRect(QRectF(area.left() + room - 8, top, 8, 20), 2, 2);
        painter.fillPath(path, color);
    }
}

int DeckLabStripRenderer::paintLamps(QPainter& painter) const {
    const QRect area = rect(Part::Chips);
    if (area.isEmpty()) {
        return area.left();
    }
    const QColor deckColor = deckLabDeckColor(m_state.deck);
    const int top = area.top() + (area.height() - 20) / 2;
    int x = area.left() + 2;
    const int right = area.left() + area.width();
    const QString over = tr("OVER %1").arg(m_state.range);
    if (m_mode == Mode::Full) {
        // r4-decks: flat lamps that look like lamps, not buttons. MASTER
        // fills and SYNC outlines in the deck's color; headphone cue fills
        // white. A one-shot match turns SYNC into MATCHED for a second, in a
        // slot wide enough for both, so nothing beside it moves.
        x += paintLamp(painter, x, top, tr("MASTER"), m_state.master ? Lamp::Filled : Lamp::Off, deckColor) + 4;
        const int syncWidth = std::max(lampWidth(tr("SYNC")), lampWidth(tr("MATCHED")));
        x += paintLamp(painter, x, top, m_state.matched ? tr("MATCHED") : tr("SYNC"),
                     m_state.sync || m_state.matched ? Lamp::Outlined : Lamp::Off, deckColor, false, syncWidth) +
                4;
        x += paintLamp(painter, x, top, QString(), m_state.pfl ? Lamp::Filled : Lamp::Off, kLampLit, true) + 8;
        // The tempo range as secondary text, or OVER when Sync took the
        // tempo beyond it: the next touch of the tempo fader would jump.
        if (m_state.overRange && !m_state.range.isEmpty()) {
            if (x + chipWidth(over) <= right) {
                x += paintChip(painter, x, top, over, true, kText, true) + 6;
            }
        } else if (!m_state.range.isEmpty()) {
            const QFont rangeFont = uiFont(kSecondary, QFont::Medium);
            const QFontMetrics metrics(rangeFont);
            if (x + metrics.horizontalAdvance(m_state.range) <= right) {
                painter.setFont(rangeFont);
                painter.setPen(kMuted);
                painter.drawText(QPoint(x, baselineFor(area, metrics)), m_state.range);
                x += metrics.horizontalAdvance(m_state.range) + 8;
            }
        }
        return x;
    }
    // Compact: only lit lamps, in order of importance, as many as fit. QNT
    // shows only when it is off (it is on all set long otherwise).
    struct Lit {
        QString text;
        bool on;
        QColor color;
        bool outlined;
        bool headphones;
    };
    QList<Lit> lamps{
            {over, m_state.overRange && !m_state.range.isEmpty(), kText, true, false},
            {tr("MASTER"), m_state.master, deckColor, false, false},
            {m_state.matched ? tr("MATCHED") : tr("SYNC"), m_state.sync || m_state.matched, deckColor, true, false},
            {QString(), m_state.pfl, kLampLit, false, true}};
    for (const auto& lamp : exceptions()) {
        lamps.append({lamp.text, true, kLampLit, lamp.outlined, false});
    }
    for (const auto& lamp : std::as_const(lamps)) {
        if (!lamp.on) {
            continue;
        }
        if (lamp.headphones) {
            if (x + 30 > right) {
                break;
            }
            QPainterPath path;
            path.addRoundedRect(QRectF(x, top, 30, 20).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
            painter.fillPath(path, lamp.color);
            paintHeadphones(painter, QRectF(x + 8, top + 3, 14, 14), inkOn(lamp.color));
            x += 34;
            continue;
        }
        if (x + chipWidth(lamp.text) > right) {
            break;
        }
        x += paintChip(painter, x, top, lamp.text, true, lamp.color, lamp.outlined) + 4;
    }
    return x;
}

void DeckLabStripRenderer::paintValues(QPainter& painter, const QRect& dirty) const {
    const bool mini = m_mode == Mode::Mini;
    const bool compact = m_mode == Mode::Compact;
    const QFont hero = numeralFont(mini ? kTitle : kHero);
    const QFont readout = numeralFont(mini ? kTitle : kReadout);
    const QFontMetrics heroMetrics(hero);
    const QFontMetrics readoutMetrics(readout);
    const QRect bpmArea = rect(Part::Bpm);
    const QRect timeArea = rect(Part::Time);
    const int heroBaseline = baselineFor(bpmArea, heroMetrics);
    // Only full readouts move the key and tempo aside for a long clock.
    const int excess = m_mode == Mode::Full ? clockExcess() : 0;
    // Clock: fixed size, right-aligned. Near the end it alternates between
    // live-red figures and a live-red box behind dark figures, so it never
    // dims: both phases keep at least 4.5:1 contrast.
    const int clockWidth = tabularWidth(heroMetrics, m_state.remaining);
    const int clockRight = timeArea.right() - (mini ? 0 : 8);
    if (timeArea.intersects(dirty) || excess > 0) {
        painter.setFont(hero);
        QColor ink = kText;
        if (m_state.endWarning && m_flash) {
            ink = kWarning;
        } else if (m_state.endWarning) {
            const QRect bounds = timeArea.adjusted(-excess, 0, 0, 0);
            const QRect box = QRect(clockRight - clockWidth - 4, timeArea.top() + 2, clockWidth + 8, timeArea.height() - 4)
                                      .intersected(bounds);
            QPainterPath path;
            path.addRoundedRect(QRectF(box), 3, 3);
            painter.fillPath(path, kWarning);
            ink = kPanel;
        }
        painter.setPen(ink);
        drawTabular(painter, clockRight - clockWidth, heroBaseline, m_state.remaining);
    }
    // BPM: big whole part, decimals at readout size; compact cards drop the
    // decimals rather than shrink when the clock needs the room, and then
    // round to the nearest whole BPM (127.96 reads 128, not 127).
    if (bpmArea.intersects(dirty)) {
        const QString truncated = mini ? m_state.bpm : wholePart(m_state.bpm);
        const QString fraction = mini ? QString() : fractionPart(m_state.bpm);
        const int fractionWidth = tabularWidth(readoutMetrics, fraction);
        const bool room = mini || !compact ||
                bpmArea.left() + 2 + tabularWidth(heroMetrics, truncated) + fractionWidth + 10 <
                        clockRight - clockWidth;
        const QString whole = room ? truncated : roundedBpm(m_state.bpm);
        painter.setFont(hero);
        painter.setPen(kText);
        drawTabular(painter, bpmArea.left() + 2, heroBaseline, whole);
        if (!mini && room) {
            painter.setFont(readout);
            painter.setPen(kDecimals);
            drawTabular(painter, bpmArea.left() + 2 + tabularWidth(heroMetrics, whole), heroBaseline, fraction);
        }
    }
    // Key: readout size in the key-wheel color, with a harmonic hint.
    // r5-decks: then the key shift ("8A +1", 15 px). After MATCH the key
    // and its shift turn the match green inside a green outline, while the
    // key still matches the deck it was matched to.
    const QRect keyArea = rect(Part::Key).translated(-excess, 0);
    if (keyArea.intersects(dirty) || excess > 0) {
        const bool matched = !mini && m_state.keyMatched;
        const QColor color = matched ? kPlay : m_state.keyColor.isValid() ? m_state.keyColor : kText;
        painter.setFont(readout);
        painter.setPen(color);
        const KeyText text = keyText();
        const int keyWidth = text.keyWidth;
        const int textWidth = text.width;
        const int baseline = compact ? baselineFor(keyArea, readoutMetrics) : heroBaseline;
        const int keyLeft = mini ? keyArea.right() - keyWidth
                : compact        ? keyArea.right() - 6 - textWidth
                                 : keyArea.left() + 2;
        painter.drawText(QPoint(keyLeft, baseline), text.key);
        if (!mini && !m_state.keyShift.isEmpty()) {
            painter.setFont(numeralFont(text.shiftPixels));
            painter.setPen(matched ? kPlay : kText);
            painter.drawText(QPoint(keyLeft + keyWidth + 3, baseline), m_state.keyShift);
        }
        if (matched) {
            // Inside the area the Key part repaints: the key box's rows, and
            // at most 4 px into the gaps beside it (two decks).
            const int capTop = baseline - readoutMetrics.capHeight();
            const int top = std::max(capTop - 4, keyArea.top());
            const int bottom = std::min(baseline + 4, keyArea.bottom());
            const int left = std::max(keyLeft - 4, keyArea.left() - (compact ? 0 : 3));
            const int right = std::min(keyLeft + textWidth + 4, keyArea.right() + (compact ? 0 : 3));
            QPainterPath box;
            box.addRoundedRect(QRectF(left, top, right - left + 1, bottom - top + 1).adjusted(0.75, 0.75, -0.75, -0.75),
                    3,
                    3);
            painter.setPen(QPen(kPlay, 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(box);
        } else if (!mini && m_state.keyMatch > 0) {
            // A compatible key gets a green check. An incompatible one shows
            // nothing: a grey cross read like a close button on every deck.
            // r5-decks: dropped when a shifted key needs its room.
            const int kx = compact ? keyLeft - 16 : keyLeft + textWidth + 6;
            if (compact ? kx >= keyArea.left() : kx + 12 <= keyArea.right() + 1) {
                QPainterPath check;
                check.moveTo(kx, baseline - 6);
                check.lineTo(kx + 4, baseline - 2);
                check.lineTo(kx + 11, baseline - 11);
                painter.setPen(QPen(kPlay, 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.drawPath(check);
            }
        }
    }
    if (mini) {
        return;
    }
    // Tempo: readout size, muted at 0.00%. The value is dropped, never
    // shrunk, if a very long clock takes its room. r4-decks: no unlabelled
    // centre dot (the muted 0.00% says it); beyond the fader range the value
    // is outlined in white, with OVER in the lamp row.
    QRect tempoArea = rect(Part::Tempo);
    if (!compact) {
        tempoArea.setRight(tempoArea.right() - excess);
    }
    if (tempoArea.intersects(dirty) || excess > 0) {
        const int width = tabularWidth(readoutMetrics, m_state.tempo);
        if (width <= tempoArea.width()) {
            const int baseline = compact ? baselineFor(tempoArea, readoutMetrics) : heroBaseline;
            const int left = tempoArea.left() + (m_state.overRange ? 5 : 2);
            if (m_state.overRange && width + 8 <= tempoArea.width()) {
                const int capTop = baseline - readoutMetrics.capHeight();
                QPainterPath box;
                box.addRoundedRect(QRectF(left - 4, capTop - 4, width + 8, readoutMetrics.capHeight() + 8)
                                           .adjusted(0.75, 0.75, -0.75, -0.75),
                        3,
                        3);
                painter.setPen(QPen(kText, 1.5));
                painter.setBrush(Qt::NoBrush);
                painter.drawPath(box);
            }
            painter.setFont(readout);
            painter.setPen(m_state.overRange ? kText : m_state.tempoCentered ? kMuted : kDecimals);
            drawTabular(painter, left, baseline, m_state.tempo);
        }
    }
}

void DeckLabStripRenderer::paint(QPainter& painter, const QRect& dirty, bool pressed, bool focused) const {
    const bool mini = m_mode == Mode::Mini;
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
    if (needs(Part::Identity)) {
        paintBadge(painter);
        const QRect transport = mini ? QRect(36, 0, 28, m_size.height())
                : m_mode == Mode::Compact ? QRect(40, 6, 34, 28)
                                          : QRect(48, 1, 36, 26);
        paintTransport(painter, transport);
    }
    if (needs(Part::Title)) {
        paintTitle(painter, rect(Part::Title).adjusted(0, 0, -2, 0));
    }
    if (!m_state.loaded) {
        if (m_mode == Mode::Full) {
            painter.setFont(uiFont(kBody, QFont::Medium));
            painter.setPen(kMuted);
            // Most DJs load from the controller; say so beside the touch way.
            painter.drawText(QRect(14, 36, m_size.width() - 24, m_size.height() - 40),
                    Qt::AlignCenter | Qt::TextWordWrap, tr("Tap to browse · or press LOAD on the controller"));
        }
    } else {
        if (needs(Part::Phase)) {
            paintPhase(painter);
        }
        // Compact cards share the lamp row: lamps first, the phrase after.
        // Full readouts: exceptions, then the phrase, beside the phase meter.
        int lampsEnd = 0;
        if (needs(Part::Chips)) {
            lampsEnd = paintLamps(painter);
        }
        if (needs(Part::Phrase)) {
            if (m_mode == Mode::Full) {
                const int reserve = m_state.phrase.name.isEmpty() ? 0 : phraseWidth() + 6;
                paintPhrase(painter, paintExceptions(painter, rect(Part::Phrase), reserve) + 4);
            } else {
                paintPhrase(painter, lampsEnd + 4);
            }
        }
        paintValues(painter, dirty);
    }
    if (focused) {
        painter.setPen(QPen(deckLabDeckColor(m_state.deck), 1));
        painter.setBrush(Qt::NoBrush);
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
    updateGeometry();
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

void DeckLabDeckStrip::mousePressEvent(QMouseEvent* event) {
    // r5-decks: two zones inside the readout. The KEY readout opens the key
    // sheet: on touch-down while the deck is paused, after a short hold while
    // it plays (a brush on a live deck never swaps its pads). The readout
    // itself never goes down there, so no track information follows.
    // Holding a paused deck's BPM opens GRID mode. Everywhere else the
    // readout is one target, as before.
    if (event->button() == Qt::LeftButton && isEnabled()) {
        const QPoint at = event->position().toPoint();
        const int deck = m_renderer.state().deck;
        m_pressAt = at;
        if (m_renderer.keyTarget().contains(at)) {
            event->accept();
            if (!m_renderer.state().playing) {
                // The surface checks the live play state too.
                emit keyTouched(deck, false);
                return;
            }
            if (!m_keyHold) {
                m_keyHold = new QTimer(this);
                m_keyHold->setSingleShot(true);
                m_keyHold->setInterval(kLiveKeyHoldMs);
                connect(m_keyHold, &QTimer::timeout, this, [this] {
                    const bool request = m_keyTouch && isVisible() && isEnabled() && m_renderer.state().loaded;
                    setKeyTouch(false);
                    if (request) {
                        emit keyTouched(m_renderer.state().deck, true);
                    }
                });
            }
            // The hold shows on the key (an outline) while it counts.
            setKeyTouch(true);
            m_keyHold->start();
            emit keyHoldStarted(deck);
            return;
        }
        if (m_renderer.bpmTarget().contains(at) && !m_renderer.state().playing) {
            if (!m_gridHold) {
                m_gridHold = new QTimer(this);
                m_gridHold->setSingleShot(true);
                m_gridHold->setInterval(kGridHoldMs);
                connect(m_gridHold, &QTimer::timeout, this, [this] {
                    // Same finger, same deck, still loaded and paused: cancel
                    // the press (no click on release) and open GRID.
                    if (!isDown() || !m_renderer.state().loaded || m_renderer.state().playing) {
                        return;
                    }
                    setDown(false);
                    emit bpmHeld(m_renderer.state().deck);
                });
            }
            m_gridHold->start();
        }
    }
    QPushButton::mousePressEvent(event);
}

void DeckLabDeckStrip::mouseMoveEvent(QMouseEvent* event) {
    const QPoint at = event->position().toPoint();
    if (m_keyTouch) {
        // A held KEY touch that slides off the key is not a request.
        if (!m_renderer.keyTarget().contains(at)) {
            setKeyTouch(false);
            m_keyHold->stop();
        }
        event->accept();
        return;
    }
    if (m_gridHold && m_gridHold->isActive() &&
            (!m_renderer.bpmTarget().contains(at) || (at - m_pressAt).manhattanLength() > 12)) {
        // A BPM hold that turns into a drag opens nothing.
        m_gridHold->stop();
    }
    QPushButton::mouseMoveEvent(event);
}

void DeckLabDeckStrip::mouseReleaseEvent(QMouseEvent* event) {
    if (m_gridHold) {
        m_gridHold->stop();
    }
    if (m_keyTouch || (m_keyHold && m_keyHold->isActive())) {
        // A KEY touch released before the hold: nothing (no click either).
        setKeyTouch(false);
        m_keyHold->stop();
        event->accept();
        return;
    }
    QPushButton::mouseReleaseEvent(event);
}

void DeckLabDeckStrip::cancelHolds() {
    if (m_gridHold) {
        m_gridHold->stop();
    }
    if (m_keyHold) {
        m_keyHold->stop();
    }
    setKeyTouch(false);
}

void DeckLabDeckStrip::setKeyTouch(bool on) {
    if (on == m_keyTouch) {
        return;
    }
    m_keyTouch = on;
    update(m_renderer.keyTarget());
}

void DeckLabDeckStrip::hideEvent(QHideEvent* event) {
    // r5-decks: a page, layer or layout change mid-hold: its release goes
    // elsewhere, so nothing held here may fire later.
    cancelHolds();
    QPushButton::hideEvent(event);
}

QSize DeckLabDeckStrip::sizeHint() const {
    const int height = DeckLabStripRenderer::preferredHeight(m_renderer.mode());
    return QSize(160, maximumHeight() < QWIDGETSIZE_MAX ? maximumHeight() : height);
}

QSize DeckLabDeckStrip::minimumSizeHint() const {
    return QSize(120, sizeHint().height());
}

void DeckLabDeckStrip::placeMeter() {
    m_meter->setGeometry(QRect(3, 6, 5, std::max(8, height() - 12)));
}

void DeckLabDeckStrip::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    // A focus frame only for keyboard focus; touch never shows one.
    m_renderer.paint(painter, event->rect(), isDown(), m_keyboardFocus && hasFocus());
    if (m_keyTouch) {
        // r5-decks: a live deck's KEY hold shows while it counts.
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(kText, 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(m_renderer.keyTarget()).adjusted(1, 1, -1, -1), 3, 3);
    }
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
    if (event->type() == QEvent::EnabledChange && !isEnabled()) {
        cancelHolds(); // r5-decks
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
    bool resized = chips.size() != m_chips.size();
    for (int index = 0; !resized && index < chips.size(); ++index) {
        resized = chips[index].text != m_chips[index].text;
    }
    m_chips = chips;
    if (resized) {
        updateGeometry();
    }
    update();
}

QSize DeckLabChipButton::sizeHint() const {
    const QFontMetrics metrics(chipCaptionFont(font()));
    int width = 10;
    if (!m_caption.isEmpty()) {
        width += metrics.horizontalAdvance(m_caption) + 8;
    }
    for (const auto& chip : m_chips) {
        width += std::max(24, metrics.horizontalAdvance(chip.text) + 14) + 4;
    }
    return QSize(width + 6, 44);
}

void DeckLabChipButton::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), isDown() ? kPanelPressed : kMiniPanel);
    painter.fillRect(QRect(0, height() - 1, width(), 1), kHairline);
    painter.fillRect(QRect(width() - 1, 0, 1, height()), kHairline);
    const QFont labels = chipCaptionFont(font());
    const QFontMetrics metrics(labels);
    painter.setFont(labels);
    const int contentWidth = sizeHint().width() - 6;
    int x = std::max(6, (width() - contentWidth) / 2 + 4);
    if (!m_caption.isEmpty()) {
        painter.setPen(isEnabled() ? kMuted : kDim);
        painter.drawText(QRect(x, 0, metrics.horizontalAdvance(m_caption), height()),
                Qt::AlignVCenter | Qt::AlignLeft, m_caption);
        x += metrics.horizontalAdvance(m_caption) + 8;
    }
    for (const auto& chip : m_chips) {
        const int chipWidth = std::max(24, metrics.horizontalAdvance(chip.text) + 14);
        const QRectF area(x, (height() - 24) / 2.0, chipWidth, 24);
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

DeckLabMark::DeckLabMark(const QString& iconPath, QWidget* parent)
        : QWidget(parent),
          m_icon(iconPath) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFixedSize(48, 44);
    setAccessibleName(QCoreApplication::translate("DeckLabDeckStrip", "Deck Lab"));
}

void DeckLabMark::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), kMiniPanel);
    painter.fillRect(QRect(0, height() - 1, width(), 1), kHairline);
    if (!m_icon.isNull()) {
        // The brand's pixel-grid mark at its native 24 px.
        painter.drawPixmap(QRect((width() - 24) / 2, (height() - 1 - 24) / 2, 24, 24), m_icon);
        return;
    }
    const double size = 26;
    const QPointF origin((width() - size) / 2, (height() - 1 - size) / 2);
    const auto box = [&origin, size](double x, double y, double w, double h) {
        return QRectF(origin.x() + x * size / 100, origin.y() + y * size / 100,
                w * size / 100, h * size / 100);
    };
    painter.fillRect(box(14, 32, 10, 36), QColor(0x59, 0x60, 0x6b));
    painter.fillRect(box(30, 22, 10, 56), QColor(0x59, 0x60, 0x6b));
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
        const double value = util_isfinite(raw[channel]) ? std::clamp(raw[channel], 0.0, 1.0) : 0.0;
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
                color = on ? kWarning : kWarningDim.darker(160);
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
