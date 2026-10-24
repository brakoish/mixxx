// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// r4-system: painted instruments shared by the System home, Deck Setup and
// the offscreen render harness: the live tile (Signature 3), the Deck Setup
// progress line and the sound check's tone pad (Signature 5). Header-only,
// no signals: they paint state they are given.

#include <QBoxLayout>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>
#include <algorithm>
#include <cmath>

#include "decklab/decklabui.h"

using decklab::ui::capsFont;
using decklab::ui::Glyph;
using decklab::ui::kDim;
using decklab::ui::kGround;
using decklab::ui::kLine;
using decklab::ui::kMuted;
using decklab::ui::kPanel;
using decklab::ui::kRaise;
using decklab::ui::kText;
using decklab::ui::Lamp;
using decklab::ui::numberFont;
using decklab::ui::paintGlyph;
using decklab::ui::paintLamp;
using decklab::ui::TouchButton;
using decklab::ui::uiFont;
using decklab::ui::vbox;

/// r4-system (Signature 3): a System home tile as an instrument, not a link:
/// a 14 px caps label, one lamp, the live value in hero or readout type,
/// capacity bars for drives, a signal fan for Wi-Fi, and room for inline
/// controls (brightness, hold-to-confirm power). A tap opens its page.
class DeckLabLiveTile : public TouchButton {
  public:
    struct Bar {
        QString name;
        double used = 0; // 0..1
        QString detail;
        bool record = false;
        bool locked = false;
    };
    DeckLabLiveTile(const QString& label, QWidget* parent, const QString& objectName)
            : TouchButton(label, parent, 0),
              m_label(label) {
        // Grows with the grid (186 x 176 at 800 x 480); no fixed touch height.
        setObjectName(objectName);
        setAccessibleName(label);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMinimumHeight(140);
        setMaximumHeight(QWIDGETSIZE_MAX);
    }
    void set(Lamp lamp, const QString& hero, const QString& readout, const QStringList& lines) {
        m_lamp = lamp;
        m_hero = hero;
        m_readout = readout;
        m_lines = lines;
        QStringList spoken{hero, readout};
        spoken.append(lines);
        spoken.removeAll(QString());
        setAccessibleDescription(spoken.join(QStringLiteral(", ")));
        update();
    }
    void setBars(const QList<Bar>& bars) {
        m_bars = bars;
        update();
    }
    QList<Bar> bars() const {
        return m_bars;
    }
    void setSignal(int percent) {
        m_signal = percent;
        update();
    }
    /// Tiles without a state (Screen, Power, About) show no lamp.
    void setLampVisible(bool visible) {
        m_lampVisible = visible;
        update();
    }
    /// Inline controls sit below the value area.
    QVBoxLayout* controls() {
        if (!m_controls) {
            m_controls = vbox(this, 8, 4);
            m_controls->setContentsMargins(8, 76, 8, 8);
            m_controls->addStretch(1);
        }
        return m_controls;
    }
    QString hero() const {
        return m_hero;
    }
    QString readout() const {
        return m_readout;
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(QColor(isDown() ? kText : kLine));
        painter.setBrush(QColor(isDown() ? kRaise : kPanel));
        painter.drawRoundedRect(frame, 4, 4);
        painter.setFont(capsFont(font(), 14));
        painter.setPen(QColor(kMuted));
        painter.drawText(QRect(14, 8, width() - 44, 22), Qt::AlignLeft | Qt::AlignVCenter, m_label);
        if (m_lampVisible) {
            paintLamp(painter, QPointF(width() - 18, 19), 4, m_lamp);
        }
        int y = 34;
        int left = 14;
        if (m_signal >= 0) {
            // One fan whose arcs light with the signal (Y-10).
            const QPointF base(left + 15, y + 30);
            const int lit = m_signal >= 67 ? 3 : m_signal >= 34 ? 2 : m_signal > 0 ? 1 : 0;
            for (int arc = 0; arc < 3; ++arc) {
                const double radius = 8 + arc * 8;
                QPainterPath path;
                const QRectF box(base.x() - radius, base.y() - radius, radius * 2, radius * 2);
                path.arcMoveTo(box, 45);
                path.arcTo(box, 45, 90);
                QPen pen(QColor(arc < lit ? kText : kLine), 2.5);
                pen.setCapStyle(Qt::RoundCap);
                painter.setPen(pen);
                painter.setBrush(Qt::NoBrush);
                painter.drawPath(path);
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(lit ? kText : kLine));
            painter.drawEllipse(base, 2.5, 2.5);
            left += 40;
        }
        if (!m_hero.isEmpty()) {
            painter.setFont(numberFont(font(), 36));
            painter.setPen(QColor(isEnabled() ? kText : kDim));
            painter.drawText(QRect(left, y, width() - left - 12, 40), Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(painter.font()).elidedText(m_hero, Qt::ElideRight, width() - left - 12));
            y += 42;
        } else if (!m_readout.isEmpty()) {
            painter.setFont(numberFont(font(), 24));
            painter.setPen(QColor(isEnabled() ? kText : kDim));
            painter.drawText(QRect(left, y, width() - left - 12, 32), Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(painter.font()).elidedText(m_readout, Qt::ElideRight, width() - left - 12));
            y += 34;
        }
        left = 14;
        painter.setFont(uiFont(font(), 15, QFont::Medium));
        for (const auto& bar : std::as_const(m_bars)) {
            painter.setPen(QColor(kText));
            painter.drawText(QRect(left, y, width() - 28, 20), Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(painter.font()).elidedText(bar.name, Qt::ElideRight, width() - 28 - (bar.record ? 14 : 0)));
            if (bar.record) {
                // The recording drive: a white dot (red while it records is the rail's job).
                paintLamp(painter, QPointF(width() - 18, y + 10), 3.5, Lamp::On);
            }
            const QRectF track(left, y + 23, width() - 28, 6);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(kLine));
            painter.drawRoundedRect(track, 3, 3);
            painter.setBrush(QColor(kText));
            painter.drawRoundedRect(QRectF(track.left(), track.top(), track.width() * std::clamp(bar.used, 0.0, 1.0), 6), 3, 3);
            painter.setPen(QColor(kMuted));
            painter.drawText(QRect(left, y + 31, width() - 28, 20), Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(painter.font()).elidedText(bar.detail, Qt::ElideRight, width() - 28));
            y += 56;
        }
        for (int index = 0; index < m_lines.size(); ++index) {
            painter.setPen(QColor(index == 0 && m_hero.isEmpty() && m_readout.isEmpty() ? kText : kMuted));
            painter.drawText(QRect(left, y, width() - 28, 21), Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(painter.font()).elidedText(m_lines.at(index), Qt::ElideRight, width() - 28));
            y += 22;
        }
    }

  private:
    QString m_label;
    Lamp m_lamp = Lamp::Off;
    QString m_hero;
    QString m_readout;
    QStringList m_lines;
    QList<Bar> m_bars;
    int m_signal = -1;
    bool m_lampVisible = true;
    QVBoxLayout* m_controls = nullptr;
};

/// r4-system (Signature 5): the stepper as a progress line, white because it
/// isn't live. The step names sit on it, a dot marks where you are, done
/// steps get a check.
class DeckLabProgressLine : public QWidget {
  public:
    explicit DeckLabProgressLine(QWidget* parent)
            : QWidget(parent) {
        setObjectName(QStringLiteral("decklab-setup-progress"));
        setFixedHeight(44);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    void setSteps(const QStringList& steps) {
        m_steps = steps;
        update();
    }
    void setCurrent(int current) {
        m_current = current;
        setAccessibleName(m_steps.value(current));
        update();
    }
    QStringList steps() const {
        return m_steps;
    }
    int current() const {
        return m_current;
    }

    /// r5-polish: the 14 px caps label face, Barlow SemiBold with minimal
    /// tracking, so all seven first-run step names fit at 800 px.
    QFont stepFont() const {
        QFont caps = decklab::ui::uiFont(font(), 14, QFont::DemiBold);
        caps.setCapitalization(QFont::AllUppercase);
        caps.setLetterSpacing(QFont::PercentageSpacing, 101);
        return caps;
    }
    /// Each step's box: its label's width plus an equal share of the rest,
    /// so a long name (CONTROLLER) gets the room a short one (TIME) leaves.
    QList<QRectF> stepCells() const {
        QList<QRectF> cells;
        if (m_steps.isEmpty()) {
            return cells;
        }
        const QFontMetricsF metrics(stepFont());
        QList<double> widths;
        double total = 0;
        for (const QString& step : m_steps) {
            widths.append(metrics.horizontalAdvance(step.toUpper()) + 2);
            total += widths.last();
        }
        const double spare = width() - total;
        const auto count = static_cast<double>(m_steps.size());
        double left = 0;
        for (const double labelWidth : std::as_const(widths)) {
            // Too narrow for every label: shrink in proportion (labels elide).
            const double cell = spare >= 0 ? labelWidth + spare / count : labelWidth * width() / total;
            cells.append(QRectF(left, 0, cell, height()));
            left += cell;
        }
        return cells;
    }
    /// The labels as painted (elided only when the line is too narrow).
    QStringList shownSteps() const {
        QStringList shown;
        const QFontMetricsF metrics(stepFont());
        const auto cells = stepCells();
        for (int index = 0; index < m_steps.size(); ++index) {
            shown.append(metrics.elidedText(m_steps.at(index).toUpper(), Qt::ElideRight, cells.at(index).width()));
        }
        return shown;
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        if (m_steps.isEmpty()) {
            return;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setFont(stepFont());
        const auto cells = stepCells();
        const QStringList shown = shownSteps();
        const int count = static_cast<int>(m_steps.size());
        const double lineY = 30;
        const double first = cells.first().center().x();
        const double last = cells.last().center().x();
        // The line: played part dim (like the overview), the rest white.
        const double here = cells.at(std::clamp(m_current, 0, count - 1)).center().x();
        painter.fillRect(QRectF(first, lineY - 1, std::max(0.0, here - first), 2), QColor(kDim));
        painter.fillRect(QRectF(here, lineY - 1, std::max(0.0, last - here), 2), QColor(kText));
        for (int index = 0; index < count; ++index) {
            const QRectF cell = cells.at(index);
            const double center = cell.center().x();
            const bool done = index < m_current;
            const bool current = index == m_current;
            painter.setPen(QColor(current ? kText : kMuted));
            painter.drawText(QRectF(cell.left(), 4, cell.width(), 20), Qt::AlignCenter, shown.at(index));
            if (current) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(kText));
                painter.drawEllipse(QPointF(center, lineY), 6, 6);
            } else if (done) {
                paintGlyph(painter, QRectF(center - 7, lineY - 7, 14, 14), Glyph::Check, QColor(kMuted));
            } else {
                painter.setPen(QPen(QColor(kText), 1.5));
                painter.setBrush(QColor(kGround));
                painter.drawEllipse(QPointF(center, lineY), 3.5, 3.5);
            }
        }
    }

  private:
    QStringList m_steps;
    int m_current = 0;
};

/// A big tone pad (LEFT, RIGHT, TEST) whose meter moves with the tone.
class DeckLabTonePad : public TouchButton {
  public:
    DeckLabTonePad(const QString& text, QWidget* parent, const QString& objectName)
            : TouchButton(text, parent, 64) {
        setObjectName(objectName);
        setAccessibleName(text);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    void setLevel(double level) {
        level = std::clamp(level, 0.0, 1.0);
        if (std::abs(level - m_level) > 0.01) {
            m_level = level;
            update();
        }
    }
    void setPlaying(bool playing) {
        m_playing = playing;
        if (!playing) {
            m_level = 0;
        }
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(QColor(isDown() || m_playing ? kText : kLine));
        painter.setBrush(QColor(isDown() || m_playing ? kRaise : kPanel));
        painter.drawRoundedRect(frame, 4, 4);
        painter.setFont(capsFont(font(), 16));
        painter.setPen(QColor(isEnabled() ? kText : kDim));
        painter.drawText(QRect(0, 6, width(), 26), Qt::AlignCenter, text());
        // 12 segments over -48..0 dBFS: a -24 dBFS tone lights half.
        constexpr int kSegments = 12;
        const double gap = 3;
        const double segmentWidth = (width() - 28 - gap * (kSegments - 1)) / kSegments;
        const int lit = static_cast<int>(std::round(m_level * kSegments));
        for (int segment = 0; segment < kSegments; ++segment) {
            const QRectF box(14 + segment * (segmentWidth + gap), height() - 20, segmentWidth, 8);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(segment < lit ? kText : kLine));
            painter.drawRoundedRect(box, 1.5, 1.5);
        }
    }

  private:
    double m_level = 0;
    bool m_playing = false;
};

