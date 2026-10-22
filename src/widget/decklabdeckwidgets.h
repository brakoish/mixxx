// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QColor>
#include <QWidget>
#include <algorithm>
#include <array>

// Painted segment level meter for the Decks page. It is polled by the display
// tick instead of following engine-rate value signals, and it only requests a
// repaint when a lit segment or the clip lamp actually changes.
class DeckLabLevelMeter final : public QWidget {
    Q_OBJECT
  public:
    static constexpr int kSegments = 14;
    DeckLabLevelMeter(Qt::Orientation orientation, int channels, QWidget* parent = nullptr);
    // Returns true when the painted state changed.
    bool setLevels(double first, double second, bool clipping);
    double level(int channel) const {
        return m_levels[std::clamp(channel, 0, 1)];
    }
    int litSegments(int channel) const {
        return m_lit[std::clamp(channel, 0, 1)];
    }
    bool clipping() const {
        return m_clip;
    }
    int channels() const {
        return m_channels;
    }

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    Qt::Orientation m_orientation;
    int m_channels;
    std::array<double, 2> m_levels{};
    std::array<int, 2> m_lit{};
    bool m_clip = false;
};
