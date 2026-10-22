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
        // Title row; BPM, tempo, key and clock; lamps with the beat meter.
        const QFontMetrics hero(numeralFont(kHero));
        const QFontMetrics readout(numeralFont(kReadout));
        const int bpm = tabularWidth(hero, QStringLiteral("000")) +
                tabularWidth(readout, QStringLiteral(".00")) + 4;
        const int clock = tabularWidth(hero, QString::fromUtf8("−00:00")) + 4;
        const int key = std::max(readout.horizontalAdvance(QStringLiteral("12A")),
                                readout.horizontalAdvance(QStringLiteral("F#m"))) +
                20;
        const QRect time(width - 10 - clock, 36, clock + 10, 40);
        const QRect keyArea(time.left() - 6 - key, 36, key, 40);
        const QRect bpmArea(12, 36, bpm, 40);
        set(Part::Identity, QRect(12, 6, 72, 28));
        set(Part::Title, QRect(86, 6, std::max(0, width - 96), 28));
        set(Part::Bpm, bpmArea);
        set(Part::Time, time);
        set(Part::Key, keyArea);
        set(Part::Tempo, QRect(bpmArea.right() + 7, 36,
                                 std::max(0, keyArea.left() - 6 - bpmArea.right() - 7), 40));
        const int phase = std::clamp(width * 30 / 100, 110, 200);
        // A phrase readout (rekordbox exports only) takes the beat meter's
        // place, and a little more room when the meter's is too small.
        const int phrase = m_state.phrase.name.isEmpty() ? 0 : std::max(phase, phraseWidth());
        if (phrase > 0) {
            set(Part::Phrase, QRect(width - 10 - phrase, 80, phrase, 22));
        } else {
            set(Part::Phase, QRect(width - 10 - phase, 88, phase, 8));
        }
        set(Part::Chips, QRect(12, 80, std::max(0, width - 22 - std::max(phase, phrase) - 10), 22));
        break;
    }
    case Mode::Compact: {
        const QFontMetrics hero(numeralFont(kHero));
        const int clock = tabularWidth(hero, QString::fromUtf8("−00:00")) + 4;
        const QRect time(width - 10 - clock, 46, clock + 10, 40);
        set(Part::Identity, QRect(10, 6, 62, 28));
        set(Part::Title, QRect(74, 6, std::max(0, width - 82), 28));
        set(Part::Phase, QRect(12, 38, width - 20, 6));
        set(Part::Bpm, QRect(10, 46, std::max(0, time.left() - 14), 40));
        set(Part::Time, time);
        set(Part::Tempo, QRect(10, 88, std::max(0, width - 88), 28));
        set(Part::Key, QRect(width - 76, 88, 76, 28));
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
    if (previous.master != next.master || previous.sync != next.sync || previous.range != next.range ||
            previous.pfl != next.pfl || previous.slip != next.slip || previous.keylock != next.keylock ||
            previous.quantize != next.quantize || previous.looping != next.looping ||
            previous.loopSize != next.loopSize) {
        add(Part::Chips);
    }
    if (previous.bpm != next.bpm) {
        add(Part::Bpm);
    }
    if (previous.tempo != next.tempo || previous.tempoCentered != next.tempoCentered) {
        add(Part::Tempo);
        if (m_mode == Mode::Full) {
            add(Part::Chips); // the tempo-center lamp
        }
    }
    if (previous.key != next.key || previous.keyColor != next.keyColor || previous.keyMatch != next.keyMatch) {
        add(Part::Key);
    }
    if (previous.remaining.size() != next.remaining.size()) {
        // A clock that gains or loses a digit moves the key and tempo (full)
        // or decides whether the BPM decimals fit (compact): repaint all.
        return QRegion(QRect(QPoint(), m_size));
    }
    if (previous.remaining != next.remaining || previous.endWarning != next.endWarning) {
        dirty += rect(Part::Time).adjusted(-clockExcess(), 0, 0, 0);
    }
    if (previous.phase != next.phase) {
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

QStringList DeckLabStripRenderer::overflowingParts() const {
    QStringList overflowing;
    if (!m_state.loaded || m_size.isEmpty()) {
        return overflowing;
    }
    const bool mini = m_mode == Mode::Mini;
    const bool compact = m_mode == Mode::Compact;
    const QFontMetrics hero(numeralFont(mini ? kTitle : kHero));
    const QFontMetrics readout(numeralFont(mini ? kTitle : kReadout));
    // BPM: the whole part always; decimals may be dropped in compact cards.
    const int bpm = mini ? tabularWidth(hero, m_state.bpm)
            : compact    ? tabularWidth(hero, wholePart(m_state.bpm))
                         : tabularWidth(hero, wholePart(m_state.bpm)) +
                    tabularWidth(readout, fractionPart(m_state.bpm));
    if (bpm > rect(Part::Bpm).width()) {
        overflowing << QStringLiteral("bpm");
    }
    const int clock = tabularWidth(hero, m_state.remaining);
    if (m_mode == Mode::Full && clock > rect(Part::Time).width() - 6 + clockExcess()) {
        overflowing << QStringLiteral("time");
    }
    if (readout.horizontalAdvance(m_state.key) > rect(Part::Key).width() - (mini ? 0 : 18)) {
        overflowing << QStringLiteral("key");
    }
    if (compact &&
            rect(Part::Bpm).left() + 2 + tabularWidth(hero, wholePart(m_state.bpm)) + 8 >
                    rect(Part::Time).right() - 8 - clock) {
        // Compact cards: the BPM and the clock share one row.
        overflowing << QStringLiteral("row");
    }
    return overflowing;
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

void DeckLabStripRenderer::paintBadge(QPainter& painter) const {
    const QRect badge = m_mode == Mode::Mini ? QRect(6, (m_size.height() - 22) / 2, 26, 22)
            : m_mode == Mode::Compact        ? QRect(12, 8, 26, 24)
                                             : QRect(14, 8, 30, 26);
    QColor color = deckLabDeckColor(m_state.deck);
    if (!m_state.loaded) {
        color = color.darker(170);
    }
    QPainterPath path;
    path.addRoundedRect(QRectF(badge), 3, 3);
    painter.fillPath(path, color);
    painter.setFont(numeralFont(m_mode == Mode::Mini ? kBody : kTitle));
    painter.setPen(inkOn(color));
    painter.drawText(badge, Qt::AlignCenter, QString::number(m_state.deck));
}

void DeckLabStripRenderer::paintTransport(QPainter& painter, const QRect& area) const {
    const QPoint center = area.center();
    if (!m_state.loaded) {
        // Empty deck: a dim stop square.
        painter.setPen(QPen(kDim, 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(area.left() + 3, center.y() - 5, 10, 10));
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
        // CUE: orange, as on club players, when paused on the main cue.
        painter.setFont(labelFont());
        painter.setPen(kCue);
        painter.drawText(area, Qt::AlignVCenter | Qt::AlignLeft, tr("CUE"));
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
                artistMetrics.elidedText(m_mode == Mode::Compact ? tr("Tap to browse") : tr("No track loaded"),
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
    const int gap = 4;
    const int segment = (area.width() - 3 * gap) / 4;
    for (int index = 0; index < 4; ++index) {
        const QRect cell(area.left() + index * (segment + gap), area.top(), segment, area.height());
        // The current beat lights in the deck's color, dimmer while paused.
        QColor color = kLampOff;
        if (m_state.phase < 0) {
            color = kMiniPanel;
        } else if (index == m_state.phase) {
            color = m_state.playing ? deckLabDeckColor(m_state.deck)
                                    : deckLabDeckColor(m_state.deck).darker(170);
        }
        painter.fillRect(cell, color);
    }
}

int DeckLabStripRenderer::phraseWidth() const {
    // Room for the longest common name ("CHORUS 2") and a two-digit count.
    const int name = QFontMetrics(labelFont()).horizontalAdvance(QStringLiteral("CHORUS 2")) + 14;
    const int count = tabularWidth(QFontMetrics(numeralFont(kTitle)), QStringLiteral("00.0"));
    return name + 6 + count + 2;
}

void DeckLabStripRenderer::paintPhrase(QPainter& painter, int left) const {
    const QRect area = rect(Part::Phrase).adjusted(std::max(0, left - rect(Part::Phrase).left()), 0, 0, 0);
    if (area.width() <= 0 || !m_state.loaded || m_state.phrase.name.isEmpty()) {
        return;
    }
    // The countdown sits right-aligned in a fixed cell of tabular figures,
    // so neither it nor the chip shifts as it runs; the phrase name is a
    // chip in the phrase's color just before it.
    const QFont count = numeralFont(kTitle);
    const QFontMetrics countMetrics(count);
    const QString& countdown = m_state.phrase.countdown;
    const int cell = std::max(tabularWidth(countMetrics, QStringLiteral("00.0")),
            tabularWidth(countMetrics, countdown));
    if (cell > area.width()) {
        return;
    }
    painter.setFont(count);
    painter.setPen(kText);
    drawTabular(painter, area.left() + area.width() - tabularWidth(countMetrics, countdown),
            baselineFor(area, countMetrics), countdown);
    const int room = area.width() - cell - 6;
    const QColor color = m_state.phrase.color.isValid() ? m_state.phrase.color : kDim;
    const QFont label = labelFont();
    const QFontMetrics labelMetrics(label);
    const QString text = labelMetrics.elidedText(
            m_state.phrase.name.toUpper(), Qt::ElideRight, std::max(0, room - 14));
    const int top = area.top() + (area.height() - 20) / 2;
    if (room >= 40 && !text.isEmpty() && text != QStringLiteral("…")) {
        const int width = std::min(room, labelMetrics.horizontalAdvance(text) + 14);
        const QRectF chip(area.left() + room - width, top, width, 20);
        QPainterPath path;
        path.addRoundedRect(chip.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        painter.fillPath(path, color);
        painter.setFont(label);
        painter.setPen(inkOn(color));
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
    if (m_mode == Mode::Full) {
        // MASTER fills and SYNC outlines in the deck's color; HP and SLIP
        // light white; the tempo range follows as secondary text.
        x += paintChip(painter, x, top, tr("MASTER"), m_state.master, deckColor) + 4;
        x += paintChip(painter, x, top, tr("SYNC"), m_state.sync, deckColor, true) + 4;
        x += paintChip(painter, x, top, tr("HP"), m_state.pfl, kLampLit) + 4;
        if (m_state.slip && x + chipWidth(tr("SLIP")) <= area.right()) {
            x += paintChip(painter, x, top, tr("SLIP"), true, kLampLit) + 4;
        }
        // Tempo range, then the tempo-center lamp (lit at 0.00%), sit under
        // the tempo readout so the value row keeps its full size.
        const QFont rangeFont = uiFont(kSecondary, QFont::Medium);
        const QFontMetrics metrics(rangeFont);
        x += 4;
        if (!m_state.range.isEmpty() && x + metrics.horizontalAdvance(m_state.range) <= area.right()) {
            painter.setFont(rangeFont);
            painter.setPen(kMuted);
            painter.drawText(QPoint(x, baselineFor(area, metrics)), m_state.range);
            x += metrics.horizontalAdvance(m_state.range) + 6;
        }
        if (x + 8 <= area.right()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(m_state.tempoCentered ? kPlay : kLampOff);
            painter.drawEllipse(QRectF(x, area.center().y() - 3.5, 8, 8));
            painter.setBrush(Qt::NoBrush);
            x += 8;
        }
        return x;
    }
    // Compact: only lit lamps, in order of importance, as many as fit.
    struct Lamp {
        QString text;
        bool on;
        QColor color;
        bool outlined;
    };
    const std::array<Lamp, 7> lamps{{
            {tr("MASTER"), m_state.master, deckColor, false},
            {tr("SYNC"), m_state.sync, deckColor, true},
            {tr("HP"), m_state.pfl, kLampLit, false},
            {tr("LOOP %1").arg(m_state.loopSize), m_state.looping, kLampLit, false},
            {tr("KEY"), m_state.keylock, kLampLit, false},
            {tr("QNT"), m_state.quantize, kLampLit, false},
            {tr("SLIP"), m_state.slip, kLampLit, false}}};
    for (const auto& lamp : lamps) {
        if (!lamp.on) {
            continue;
        }
        if (x + chipWidth(lamp.text) > area.right()) {
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
    // Clock: fixed size, right-aligned; flashes live red near the end.
    const int clockWidth = tabularWidth(heroMetrics, m_state.remaining);
    const int clockRight = timeArea.right() - (mini ? 0 : 8);
    if (timeArea.intersects(dirty) || excess > 0) {
        painter.setFont(hero);
        painter.setPen(m_state.endWarning ? (m_flash ? kWarning : kWarningDim) : kText);
        drawTabular(painter, clockRight - clockWidth, heroBaseline, m_state.remaining);
    }
    // BPM: big whole part, decimals at readout size; compact cards drop the
    // decimals rather than shrink when the clock needs the room.
    if (bpmArea.intersects(dirty)) {
        const QString whole = mini ? m_state.bpm : wholePart(m_state.bpm);
        painter.setFont(hero);
        painter.setPen(kText);
        drawTabular(painter, bpmArea.left() + 2, heroBaseline, whole);
        if (!mini) {
            const int wholeWidth = tabularWidth(heroMetrics, whole);
            const QString fraction = fractionPart(m_state.bpm);
            const int fractionWidth = tabularWidth(readoutMetrics, fraction);
            const bool room = !compact ||
                    bpmArea.left() + 2 + wholeWidth + fractionWidth + 10 < clockRight - clockWidth;
            if (room) {
                painter.setFont(readout);
                painter.setPen(kDecimals);
                drawTabular(painter, bpmArea.left() + 2 + wholeWidth, heroBaseline, fraction);
            }
        }
    }
    // Key: readout size in the key-wheel color, with a harmonic hint.
    const QRect keyArea = rect(Part::Key).translated(-excess, 0);
    if (keyArea.intersects(dirty) || excess > 0) {
        const QColor color = m_state.keyColor.isValid() ? m_state.keyColor : kText;
        painter.setFont(readout);
        painter.setPen(color);
        const int keyWidth = readoutMetrics.horizontalAdvance(m_state.key);
        const int baseline = compact ? baselineFor(keyArea, readoutMetrics) : heroBaseline;
        const int keyLeft = mini ? keyArea.right() - keyWidth
                : compact        ? keyArea.right() - 6 - keyWidth
                                 : keyArea.left() + 2;
        painter.drawText(QPoint(keyLeft, baseline), m_state.key);
        if (!mini && m_state.keyMatch != 0) {
            const int kx = compact ? keyLeft - 16 : keyLeft + keyWidth + 6;
            if (m_state.keyMatch > 0) {
                QPainterPath check;
                check.moveTo(kx, baseline - 6);
                check.lineTo(kx + 4, baseline - 2);
                check.lineTo(kx + 11, baseline - 11);
                painter.setPen(QPen(kPlay, 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.drawPath(check);
            } else {
                painter.setPen(QPen(kMuted, 2));
                painter.drawLine(QPointF(kx, baseline - 11), QPointF(kx + 9, baseline - 2));
                painter.drawLine(QPointF(kx + 9, baseline - 11), QPointF(kx, baseline - 2));
            }
        }
    }
    if (mini) {
        return;
    }
    // Tempo: readout size. Compact readouts put the tempo-center lamp after
    // it; full readouts show it in the lamp row. The value is dropped, never
    // shrunk, if a very long clock takes its room.
    QRect tempoArea = rect(Part::Tempo);
    if (!compact) {
        tempoArea.setRight(tempoArea.right() - excess);
    }
    if (tempoArea.intersects(dirty) || excess > 0) {
        const int width = tabularWidth(readoutMetrics, m_state.tempo);
        if (width <= tempoArea.width()) {
            const int baseline = compact ? baselineFor(tempoArea, readoutMetrics) : heroBaseline;
            painter.setFont(readout);
            painter.setPen(m_state.tempoCentered ? kMuted : kDecimals);
            drawTabular(painter, tempoArea.left() + 2, baseline, m_state.tempo);
            const int x = tempoArea.left() + 2 + width + 6;
            if (compact && x + 8 <= tempoArea.right()) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(m_state.tempoCentered ? kPlay : kLampOff);
                painter.drawEllipse(QRectF(x, baseline - 9, 8, 8));
                painter.setBrush(Qt::NoBrush);
            }
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
                : m_mode == Mode::Compact ? QRect(42, 6, 30, 28)
                                          : QRect(50, 6, 34, 28);
        paintTransport(painter, transport);
    }
    if (needs(Part::Title)) {
        paintTitle(painter, rect(Part::Title).adjusted(0, 0, -2, 0));
    }
    if (!m_state.loaded) {
        if (m_mode == Mode::Full) {
            painter.setFont(uiFont(kBody, QFont::Medium));
            painter.setPen(kMuted);
            painter.drawText(QRect(14, 36, m_size.width() - 24, m_size.height() - 40),
                    Qt::AlignCenter, tr("Tap to browse and load a track"));
        }
    } else {
        if (needs(Part::Phase)) {
            paintPhase(painter);
        }
        // Compact cards share the lamp row: lamps first, the phrase after.
        int lampsEnd = 0;
        if (needs(Part::Chips)) {
            lampsEnd = paintLamps(painter);
        }
        if (needs(Part::Phrase)) {
            paintPhrase(painter, m_mode == Mode::Compact ? lampsEnd + 4 : 0);
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
