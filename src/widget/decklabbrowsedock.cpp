// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabbrowsedock.h"

#include <QCoreApplication>
#include <QGridLayout>
#include <QLocale>
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace decklab {

namespace {
QString translate(const char* text) {
    return QCoreApplication::translate("DeckLabBrowse", text);
}
QString dash() {
    return QStringLiteral("—");
}
} // namespace

DockDeck dockDeckFrom(const DeckLabStripState& state) {
    DockDeck deck;
    deck.deck = state.deck;
    deck.loaded = state.loaded;
    deck.playing = state.loaded && state.playing;
    deck.cue = state.loaded && state.cue;
    deck.endWarning = state.endWarning;
    if (!state.loaded) {
        return deck;
    }
    deck.title = state.title.trimmed();
    deck.key = state.key == dash() ? QString() : state.key;
    bool ok = false;
    const double bpm = state.bpm.toDouble(&ok);
    deck.bpm = ok && std::isfinite(bpm) && bpm > 0 ? QLocale().toString(bpm, 'f', 1) : QString();
    deck.remaining = state.remaining == dash() ? QString() : state.remaining;
    return deck;
}

DockWord dockWord(const DockDeck& deck) {
    if (!deck.loaded) {
        return DockWord::Empty;
    }
    if (deck.playing) {
        return DockWord::OnAir;
    }
    return deck.cue ? DockWord::Cued : DockWord::Paused;
}

QString dockWordText(DockWord word) {
    switch (word) {
    case DockWord::Empty:
        return translate("Empty");
    case DockWord::Cued:
        return translate("Cued");
    case DockWord::Paused:
        return translate("Paused");
    case DockWord::OnAir:
        return translate("On air");
    }
    return {};
}

QString dockLoadLine(const DockDeck& deck) {
    if (!deck.loaded) {
        return translate("empty deck");
    }
    return deck.title.isEmpty() ? translate("replaces the loaded track") : translate("replaces %1").arg(deck.title);
}

} // namespace decklab

using namespace decklab;

DeckLabDockTile::DeckLabDockTile(const QString& text, QWidget* parent)
        : DeckLabActionButton(text, parent) {
    m_refusal.setSingleShot(true);
    QObject::connect(&m_refusal, &QTimer::timeout, this, [this] {
        m_refusing = false;
        update();
    });
}

void DeckLabDockTile::setDeckState(const DockDeck& state) {
    if (m_state == state) {
        return;
    }
    m_state = state;
    update();
}

void DeckLabDockTile::setLoadReady(bool ready) {
    if (m_loadReady == ready) {
        return;
    }
    m_loadReady = ready;
    update();
}

void DeckLabDockTile::showRefusal(int milliseconds) {
    m_refusing = true;
    m_refusal.start(milliseconds);
    update();
}

void DeckLabDockTile::setFlash(bool flash) {
    if (m_flash == flash) {
        return;
    }
    m_flash = flash;
    if (m_state.endWarning) {
        update();
    }
}

void DeckLabDockTile::pinHeight(int height) {
    // The surface style sheet's QPushButton min-height would win over a
    // fixed height: pin it in the button's own sheet too.
    setFixedHeight(height);
    const QString sheet = QStringLiteral("min-height:%1px;max-height:%1px;padding-top:0px;padding-bottom:0px;")
                                  .arg(height - 2);
    if (styleSheet() != sheet) {
        setStyleSheet(sheet);
    }
}

void DeckLabDockTile::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), colors::ground);
    if (deck() > 0) {
        paintDeck(painter);
    } else {
        paintAction(painter);
    }
}

void DeckLabDockTile::paintAction(QPainter& painter) {
    // Sampler pad mode: "Load pad 5 / Sampler" and "Cancel / Back to pads".
    const bool enabled = isEnabled();
    const bool ready = tone() == Tone::Ready && enabled;
    const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(ready ? colors::ink : colors::edge, ready ? 2 : 1));
    painter.setBrush(isDown() || ready ? colors::raise : colors::ground);
    painter.drawRoundedRect(frame.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    const QFont primary = strongFont(font());
    const QFont secondary = secondaryFont(font());
    const QFontMetrics primaryMetrics(primary);
    const QFontMetrics secondaryMetrics(secondary);
    const int left = 14;
    const int right = width() - 12;
    const bool twoLines = !detail().isEmpty() && height() >= primaryMetrics.height() + secondaryMetrics.height() + 6;
    const int top = twoLines ? (height() - primaryMetrics.height() - secondaryMetrics.height()) / 2
                             : (height() - primaryMetrics.height()) / 2;
    int textRight = right;
    if (ready) {
        paintGlyph(painter, QRectF(right - 14, top + (primaryMetrics.height() - 14) / 2.0, 14, 14), Glyph::Chevron, colors::ink);
        textRight -= 22;
    }
    painter.setFont(primary);
    painter.setPen(enabled ? colors::ink : colors::dim);
    painter.drawText(QRect(left, top, std::max(0, textRight - left), primaryMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
            primaryMetrics.elidedText(text().toUpper(), Qt::ElideRight, std::max(0, textRight - left)));
    if (twoLines) {
        painter.setFont(secondary);
        painter.setPen(enabled ? (ready ? colors::ink : colors::muted) : colors::dim);
        painter.drawText(QRect(left, top + primaryMetrics.height(), std::max(0, right - left), secondaryMetrics.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                secondaryMetrics.elidedText(detail(), Qt::ElideRight, std::max(0, right - left)));
    }
}

void DeckLabDockTile::paintDeck(QPainter& painter) {
    const DockDeck& state = m_state;
    const bool onAir = tone() == Tone::OnAir || (state.loaded && state.playing);
    const DockWord word = onAir ? DockWord::OnAir : dockWord(state);
    const bool ready = m_loadReady && isEnabled() && !onAir;
    const bool compact = width() < 240;
    const QColor deckColor = deckLabDeckColor(deck());

    // Frame: line outline; ready to load = raised fill (B-29); a refused tap
    // on an on-air deck = live-red edge and wash, right where it was tapped.
    const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (m_refusing) {
        QColor wash = colors::onAir;
        wash.setAlpha(38);
        painter.setPen(QPen(colors::onAir, 2));
        painter.setBrush(wash);
        painter.drawRoundedRect(frame.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    } else {
        painter.setPen(QPen(colors::edge, 1));
        painter.setBrush(ready ? (isDown() ? colors::raise.lighter(125) : colors::raise) : colors::ground);
        painter.drawRoundedRect(frame, 3, 3);
    }
    // Deck identity: the 6 px colour bar is always in full colour.
    painter.setPen(Qt::NoPen);
    painter.setBrush(deckColor);
    painter.drawRect(QRectF(1, 1, 6, height() - 2));

    const bool tall = height() >= 60;
    const int left = 14;
    const int right = width() - 10;
    const int rowOne = tall ? 8 : 5;
    const int rowHeight = 22;
    const int rowTwo = height() - (tall ? 28 : 25);

    // Row one: badge, state word, and the remaining time (or LOAD).
    const QRectF badge(left, rowOne + 1, 22, 20);
    painter.setBrush(deckColor);
    painter.drawRoundedRect(badge, 3, 3);
    painter.setFont(numberFont(font(), 15));
    painter.setPen(colors::ground);
    painter.drawText(badge, Qt::AlignCenter, QString::number(deck()));
    int x = static_cast<int>(badge.right()) + 8;

    const QFont wordFont = labelFont(font());
    const QFontMetrics wordMetrics(wordFont);
    const QString wordText = dockWordText(word);
    const int wordWidth = wordMetrics.horizontalAdvance(wordText);
    const QFont loadFont = strongFont(font());
    const QString loadText = translate("Load").toUpper();
    const int loadWidth = QFontMetrics(loadFont).horizontalAdvance(loadText);
    // A narrow tile ready to load says LOAD instead of its state word when
    // both do not fit; the second line still says what it replaces.
    const bool loadInsteadOfWord = ready && compact && x + wordWidth + 10 + loadWidth + 14 > right;
    painter.setFont(wordFont);
    if (loadInsteadOfWord) {
        painter.setFont(loadFont);
        painter.setPen(colors::ink);
        painter.drawText(QRect(x, rowOne, loadWidth + 1, rowHeight), Qt::AlignLeft | Qt::AlignVCenter, loadText);
        paintGlyph(painter, QRectF(x + loadWidth + 3, rowOne + 5, 12, 12), Glyph::Chevron, colors::ink);
        x += loadWidth + 18;
    } else if (word == DockWord::OnAir) {
        // Only ON AIR uses live red, as an outline.
        const QRectF box(x, rowOne + 1, wordWidth + 12, 20);
        painter.setPen(QPen(colors::onAir, 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(box.adjusted(0.75, 0.75, -0.75, -0.75), 3, 3);
        painter.setPen(colors::onAir);
        painter.drawText(box, Qt::AlignCenter, wordText);
        x = static_cast<int>(box.right()) + 8;
    } else {
        painter.setPen(colors::ink);
        painter.drawText(QRect(x, rowOne, wordWidth + 2, rowHeight), Qt::AlignLeft | Qt::AlignVCenter, wordText);
        x += wordWidth + 10;
    }

    int lineRight = right;
    if (ready && !compact) {
        // LOAD across the tile's right side: a 44 px white-outlined target.
        const QRectF pill(right - 100, (height() - 44) / 2.0, 100, 44);
        painter.setPen(QPen(colors::ink, 2));
        painter.setBrush(isDown() ? colors::ink : colors::ground);
        painter.drawRoundedRect(pill.adjusted(1, 1, -1, -1), 3, 3);
        painter.setFont(loadFont);
        painter.setPen(isDown() ? colors::ground : colors::ink);
        const qreal blockLeft = pill.center().x() - (loadWidth + 18) / 2.0;
        painter.drawText(QRectF(blockLeft, pill.top(), loadWidth + 2, pill.height()), Qt::AlignLeft | Qt::AlignVCenter,
                loadText);
        paintGlyph(painter, QRectF(blockLeft + loadWidth + 6, pill.center().y() - 6, 12, 12), Glyph::Chevron,
                isDown() ? colors::ground : colors::ink);
        lineRight = static_cast<int>(pill.left()) - 10;
    } else if (ready && !loadInsteadOfWord) {
        // Narrow tiles: LOAD replaces the time on row one.
        if (right - 14 - loadWidth >= x) {
            painter.setFont(loadFont);
            painter.setPen(colors::ink);
            painter.drawText(QRect(right - 14 - loadWidth, rowOne, loadWidth + 1, rowHeight), Qt::AlignLeft | Qt::AlignVCenter,
                    loadText);
            paintGlyph(painter, QRectF(right - 11, rowOne + 5, 12, 12), Glyph::Chevron, colors::ink);
        }
    } else if (ready) {
        // LOAD is already on row one, in place of the state word.
    } else if (!state.remaining.isEmpty()) {
        const QFont timeFont = numberFont(font(), tall ? 18 : 16);
        const QFontMetrics timeMetrics(timeFont);
        const int timeWidth = timeMetrics.horizontalAdvance(state.remaining);
        if (right - timeWidth >= x) {
            painter.setFont(timeFont);
            // The end-of-track warning blinks the time in live red.
            painter.setPen(state.endWarning && m_flash ? colors::onAir : colors::ink);
            painter.drawText(QRect(right - timeWidth, rowOne - 1, timeWidth + 1, rowHeight + 2), Qt::AlignRight | Qt::AlignVCenter,
                    state.remaining);
        }
    }

    // Row two: what loading would replace, the refusal, or the track.
    const int lineLeft = left;
    const int lineWidth = std::max(0, lineRight - lineLeft);
    const QFont lineFont = secondaryFont(font());
    const QFontMetrics lineMetrics(lineFont);
    if (m_refusing) {
        painter.setFont(strongFont(font()));
        painter.setPen(colors::ink);
        painter.drawText(QRect(lineLeft, rowTwo, lineWidth, rowHeight), Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(strongFont(font())).elidedText(translate("On air. Pause it first."), Qt::ElideRight, lineWidth));
        return;
    }
    if (ready) {
        painter.setFont(lineFont);
        painter.setPen(colors::ink);
        painter.drawText(QRect(lineLeft, rowTwo, lineWidth, rowHeight), Qt::AlignLeft | Qt::AlignVCenter,
                lineMetrics.elidedText(dockLoadLine(state), Qt::ElideRight, lineWidth));
        return;
    }
    if (!state.loaded) {
        return;
    }
    QStringList facts;
    if (!state.key.isEmpty()) {
        facts << state.key;
    }
    if (!state.bpm.isEmpty()) {
        facts << state.bpm;
    }
    const QString meta = facts.join(QStringLiteral(" · "));
    const QFont metaFont = numberFont(font(), 15);
    const QFontMetrics metaMetrics(metaFont);
    const int metaWidth = meta.isEmpty() || compact ? 0 : metaMetrics.horizontalAdvance(meta) + 12;
    const QFont titleFontRow = bodyFont(font());
    const QFontMetrics titleMetrics(titleFontRow);
    const QString title = titleMetrics.elidedText(state.title.isEmpty() ? translate("Untitled") : state.title,
            Qt::ElideRight,
            std::max(0, lineWidth - metaWidth));
    painter.setFont(titleFontRow);
    painter.setPen(colors::ink);
    painter.drawText(QRect(lineLeft, rowTwo, std::max(0, lineWidth - metaWidth), rowHeight), Qt::AlignLeft | Qt::AlignVCenter, title);
    if (metaWidth > 0) {
        // Key in white, never in a key colour that could read as live red.
        painter.setFont(metaFont);
        painter.setPen(colors::muted);
        painter.drawText(QRect(lineLeft + titleMetrics.horizontalAdvance(title) + 12, rowTwo, metaWidth, rowHeight),
                Qt::AlignLeft | Qt::AlignVCenter,
                meta);
    }
}

DeckLabDeckDock::DeckLabDeckDock(QWidget* parent)
        : QWidget(parent) {
    m_grid = new QGridLayout(this);
    for (int slot = 0; slot < 4; ++slot) {
        m_tiles[slot] = new DeckLabDockTile(translate("Load %1").arg(slot + 1), this);
        m_tiles[slot]->setDeck(slot + 1);
    }
    relayout();
}

void DeckLabDeckDock::setArrangement(Arrangement arrangement) {
    if (m_arrangement == arrangement) {
        return;
    }
    m_arrangement = arrangement;
    relayout();
}

void DeckLabDeckDock::setShownCount(int count) {
    count = count > 2 ? 4 : 2;
    if (m_count == count) {
        return;
    }
    m_count = count;
    relayout();
}

void DeckLabDeckDock::relayout() {
    for (auto* tile : m_tiles) {
        m_grid->removeWidget(tile);
    }
    for (int column = 0; column < 4; ++column) {
        m_grid->setColumnStretch(column, 0);
    }
    const bool bar = m_arrangement == Arrangement::Bar;
    const int height = bar ? kBarTile : kPaneTile;
    if (bar) {
        m_grid->setContentsMargins(kGap, kGap, kGap, 0);
        m_grid->setHorizontalSpacing(kGap);
        m_grid->setVerticalSpacing(0);
    } else {
        m_grid->setContentsMargins(0, 0, 0, 0);
        m_grid->setHorizontalSpacing(6);
        m_grid->setVerticalSpacing(6);
    }
    const int columns = bar ? m_count : (m_count > 2 ? 2 : 1);
    for (int slot = 0; slot < 4; ++slot) {
        auto* tile = m_tiles[slot];
        const bool shown = slot < m_count;
        tile->pinHeight(height);
        if (shown) {
            m_grid->addWidget(tile, slot / columns, slot % columns);
        }
        tile->setVisible(shown);
    }
    for (int column = 0; column < columns; ++column) {
        m_grid->setColumnStretch(column, 1);
    }
    const int rows = (m_count + columns - 1) / columns;
    setFixedHeight(bar ? kGap + height : rows * height + (rows - 1) * 6);
}
