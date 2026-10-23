// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabbrowselegend.h"

#include <QCoreApplication>
#include <QPainter>
#include <algorithm>
#include <cstdlib>

#include "widget/decklabbrowse.h"

namespace decklab {

namespace {
QString translate(const char* text) {
    return QCoreApplication::translate("DeckLabBrowse", text);
}
} // namespace

QVector<LegendItem> legendItems(LegendLevel level) {
    const QString turn = translate("Turn");
    const QString press = translate("Press");
    const QString shiftPress = translate("Shift+Press");
    switch (level) {
    case LegendLevel::Home:
        return {{turn, translate("Choose")}, {press, translate("Open")}, {shiftPress + QStringLiteral(" ×2"), translate("Decks")}};
    case LegendLevel::HomeBackArmed:
        return {{turn, translate("Choose")}, {press, translate("Open")}, {shiftPress, translate("Again: decks")}};
    case LegendLevel::List:
        return {{turn, translate("Choose")}, {press, translate("Open")}, {shiftPress, translate("Back")}};
    case LegendLevel::Tracks:
        return {{turn, translate("Choose")}, {press, translate("Preview")}, {shiftPress, translate("Back")}};
    case LegendLevel::Menu:
        // The first item is a filled tag: the menu owns the knob now.
        return {{translate("Menu"), QString()}, {turn, translate("Choose")}, {press, translate("Select")},
                {shiftPress, translate("Close")}};
    }
    return {};
}

QString legendHint(LegendLevel level) {
    switch (level) {
    case LegendLevel::Tracks:
        return translate("Tap a deck to load");
    case LegendLevel::List:
        return translate("Hold back: sources");
    case LegendLevel::Home:
    case LegendLevel::HomeBackArmed:
    case LegendLevel::Menu:
        break;
    }
    return {};
}

int KnobAccelerator::steps(int delta, qint64 nowMs) {
    if (delta == 0) {
        return 0;
    }
    const int sign = delta > 0 ? 1 : -1;
    const qint64 gap = m_last < 0 ? 1000000 : nowMs - m_last;
    m_last = nowMs;
    if (sign == m_sign && gap >= 0 && gap < 45) {
        ++m_run;
    } else {
        m_run = 0;
    }
    m_sign = sign;
    if (m_run < 2) {
        return delta;
    }
    return delta * (gap < 20 ? 8 : 4);
}

void KnobAccelerator::reset() {
    m_last = -1;
    m_sign = 0;
    m_run = 0;
}

} // namespace decklab

using namespace decklab;

DeckLabKnobLegend::DeckLabKnobLegend(QWidget* parent)
        : QWidget(parent) {
    setFixedHeight(kHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_OpaquePaintEvent);
    m_flash.setSingleShot(true);
    m_flash.setInterval(150);
    QObject::connect(&m_flash, &QTimer::timeout, this, [this] {
        m_flashing = false;
        update();
    });
}

QSize DeckLabKnobLegend::sizeHint() const {
    return QSize(800, kHeight);
}

void DeckLabKnobLegend::setLevel(LegendLevel level) {
    if (m_level == level) {
        return;
    }
    m_level = level;
    QStringList words;
    for (const LegendItem& item : legendItems(level)) {
        words << (item.verb.isEmpty() ? item.key : item.key + QLatin1Char(' ') + item.verb);
    }
    const QString hint = legendHint(level);
    if (!hint.isEmpty()) {
        words << hint;
    }
    setAccessibleName(words.join(QStringLiteral(" · ")));
    update();
}

void DeckLabKnobLegend::flash() {
    m_flashing = true;
    m_flash.start();
    update();
}

void DeckLabKnobLegend::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor back = m_flashing ? colors::ink : colors::ground;
    const QColor fore = m_flashing ? colors::ground : colors::ink;
    painter.fillRect(rect(), back);
    painter.setPen(colors::edge);
    painter.drawLine(0, 0, width(), 0);
    const QFont label = labelFont(font());
    const QFontMetrics metrics(label);
    painter.setFont(label);
    const int capHeight = 18;
    const qreal capTop = (height() - capHeight) / 2.0 + 0.5;
    int x = 12;
    for (const LegendItem& item : legendItems(m_level)) {
        const int capWidth = metrics.horizontalAdvance(item.key) + 12;
        const QRectF cap(x + 0.5, capTop, capWidth, capHeight);
        if (item.verb.isEmpty()) {
            // A filled tag (MENU).
            painter.setPen(Qt::NoPen);
            painter.setBrush(fore);
            painter.drawRoundedRect(cap, 3, 3);
            painter.setPen(back);
            painter.drawText(cap, Qt::AlignCenter, item.key);
            x += capWidth + 16;
            continue;
        }
        // A key cap: an outlined key with its control's name, then the verb.
        painter.setPen(QPen(m_flashing ? colors::ground : colors::muted, 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(cap, 3, 3);
        painter.setPen(fore);
        painter.drawText(cap, Qt::AlignCenter, item.key);
        x += capWidth + 6;
        const int verbWidth = metrics.horizontalAdvance(item.verb);
        painter.drawText(QRect(x, 0, verbWidth + 2, height()), Qt::AlignLeft | Qt::AlignVCenter, item.verb);
        x += verbWidth + 20;
    }
    const QString hint = legendHint(m_level);
    if (!hint.isEmpty()) {
        const int hintWidth = metrics.horizontalAdvance(hint);
        const int left = width() - 12 - hintWidth;
        if (left > x) {
            painter.setPen(fore);
            painter.drawText(QRect(left, 0, hintWidth + 2, height()), Qt::AlignLeft | Qt::AlignVCenter, hint);
        }
    }
}
