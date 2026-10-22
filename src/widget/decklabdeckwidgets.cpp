// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabdeckwidgets.h"

#include <QPaintEvent>
#include <QPainter>
#include <cmath>

#include "moc_decklabdeckwidgets.cpp"

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
        lit[channel] = int(std::ceil(value * kSegments - 0.25));
        lit[channel] = std::clamp(lit[channel], 0, kSegments);
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
