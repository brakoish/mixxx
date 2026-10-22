// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabphraseview.h"

#include <QCoreApplication>
#include <QEvent>
#include <QPaintEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>

#include "moc_decklabphraseview.cpp"
#include "util/fpclassify.h"

namespace {
const QColor kGround(0x0d, 0x0f, 0x12);
const QColor kPlayed(0, 0, 0, 120);
const QColor kPlayhead(0xff, 0x3d, 0x2e); // live red: the playhead only
} // namespace

DeckLabPhraseReadout deckLabPhraseReadout(const decklab::PhraseMap* map, double seconds) {
    if (!map || map->isEmpty()) {
        return {};
    }
    const auto position = decklab::phrasePosition(*map, seconds);
    if (position.phrase < 0 || position.phrase >= map->phrases.size()) {
        return {};
    }
    const auto& phrase = map->phrases[position.phrase];
    DeckLabPhraseReadout readout;
    // An id rekordbox added after our table still marks a real boundary.
    readout.name = phrase.name.isEmpty()
            ? QCoreApplication::translate("DeckLabPhrase", "Phrase")
            : phrase.name;
    readout.countdown = decklab::phraseCountdownText(position.beatsLeft);
    readout.color = phrase.color;
    return readout;
}

DeckLabPhraseBands::DeckLabPhraseBands(QWidget* overview)
        : QWidget(overview) {
    setObjectName(QStringLiteral("decklab-phrase-bands"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::NoFocus);
    if (overview) {
        overview->installEventFilter(this);
    }
    hide();
    place();
}

void DeckLabPhraseBands::setPlayback(
        const decklab::PhraseMapPointer& map, double durationSeconds, double position) {
    const bool show = map && !map->isEmpty() && util_isfinite(durationSeconds) &&
            durationSeconds > 0;
    if (!show) {
        if (m_map || !isHidden()) {
            m_map.reset();
            m_bands = QImage();
            m_playhead = -1;
            hide();
        }
        return;
    }
    if (map != m_map || durationSeconds != m_duration) {
        m_map = map;
        m_duration = durationSeconds;
        m_bands = QImage();
        m_playhead = -1;
        place();
        QWidget::show();
        raise();
        update();
    }
    const int x = util_isfinite(position)
            ? xFor(std::clamp(position, 0.0, 1.0) * m_duration)
            : -1;
    if (x == m_playhead) {
        return;
    }
    const int previous = m_playhead;
    m_playhead = x;
    // The played shade runs from the left edge to the playhead: repaint the
    // stretch between the old and the new position only.
    const int from = std::min(previous < 0 ? 0 : previous, x < 0 ? 0 : x) - 2;
    const int to = std::max(previous, x) + 2;
    update(QRect(from, 0, to - from + 1, height()));
}

QList<int> DeckLabPhraseBands::bandEdges() const {
    QList<int> edges;
    if (m_map) {
        for (const auto& phrase : m_map->phrases) {
            edges.append(xFor(phrase.startSeconds));
        }
    }
    return edges;
}

bool DeckLabPhraseBands::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        m_bands = QImage();
        place();
    }
    return QWidget::eventFilter(watched, event);
}

void DeckLabPhraseBands::place() {
    const QWidget* overview = parentWidget();
    if (!overview) {
        return;
    }
    setGeometry(0, std::max(0, overview->height() - kHeight), overview->width(), kHeight);
}

int DeckLabPhraseBands::xFor(double seconds) const {
    if (!(m_duration > 0) || width() <= 0) {
        return -1;
    }
    const double fraction = std::clamp(seconds / m_duration, 0.0, 1.0);
    return static_cast<int>(std::lround(fraction * (width() - 1)));
}

void DeckLabPhraseBands::renderBands() {
    m_bands = QImage(size(), QImage::Format_RGB32);
    m_bands.fill(kGround);
    if (!m_map || width() <= 0) {
        return;
    }
    QPainter painter(&m_bands);
    // Row 0 stays ground: a hairline between the waveform and the bands.
    const int top = 1;
    const int bandHeight = height() - top;
    for (const auto& phrase : m_map->phrases) {
        const int left = xFor(phrase.startSeconds);
        const int right = xFor(phrase.endSeconds);
        if (left < 0 || right < left) {
            continue;
        }
        // One ground pixel closes each band, so equal neighbours stay apart.
        const int bandWidth = std::max(1, right - left - 1);
        painter.fillRect(QRect(left, top, bandWidth, bandHeight), phrase.color);
        if (phrase.fillBeat > 0) {
            // Fill-in: the upper half lightens from where the fill starts.
            const int fill = xFor(phrase.fillSeconds);
            if (fill >= left && fill < left + bandWidth) {
                painter.fillRect(QRect(fill, top, left + bandWidth - fill, bandHeight / 2),
                        phrase.color.lighter(150));
            }
        }
    }
}

void DeckLabPhraseBands::paintEvent(QPaintEvent* event) {
    if (m_bands.size() != size()) {
        renderBands();
    }
    QPainter painter(this);
    const QRect dirty = event->rect();
    painter.drawImage(dirty, m_bands, dirty);
    if (m_playhead >= 0) {
        painter.fillRect(QRect(0, 0, m_playhead, height()).intersected(dirty), kPlayed);
        painter.fillRect(QRect(m_playhead - 1, 0, 2, height()).intersected(dirty), kPlayhead);
    }
}
