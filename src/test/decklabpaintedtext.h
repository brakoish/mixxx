// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// r6-keys (tests): the text a delegate or widget really paints. A paint
// device whose engine keeps every text run QPainter hands it and draws
// nothing, so a test can say "the row shows 8A" without reading pixels.

#include <QAbstractItemDelegate>
#include <QPaintDevice>
#include <QPaintEngine>
#include <QPainter>
#include <QStringList>
#include <QStyleOptionViewItem>
#include <QTextItem>
#include <climits>
#include <cmath>

class DeckLabPaintedText final : public QPaintDevice {
  public:
    DeckLabPaintedText() = default;
    QPaintEngine* paintEngine() const override {
        return &m_engine;
    }
    QStringList texts() const {
        return m_engine.texts;
    }
    QString joined() const {
        return m_engine.texts.join(QLatin1Char('|'));
    }
    /// Everything one delegate paints for one index.
    static QStringList of(const QAbstractItemDelegate& delegate,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) {
        DeckLabPaintedText device;
        {
            QPainter painter(&device);
            delegate.paint(&painter, option, index);
        }
        return device.texts();
    }

  protected:
    int metric(PaintDeviceMetric metric) const override {
        switch (metric) {
        case PdmWidth:
            return 1280;
        case PdmHeight:
            return 800;
        case PdmWidthMM:
            return 338;
        case PdmHeightMM:
            return 212;
        case PdmNumColors:
            return INT_MAX;
        case PdmDepth:
            return 32;
        case PdmDpiX:
        case PdmDpiY:
        case PdmPhysicalDpiX:
        case PdmPhysicalDpiY:
            return 96;
        case PdmDevicePixelRatio:
            return 1;
        case PdmDevicePixelRatioScaled:
            return static_cast<int>(QPaintDevice::devicePixelRatioFScale());
        default:
            return 0;
        }
    }

  private:
    class Engine final : public QPaintEngine {
      public:
        Engine()
                : QPaintEngine(QPaintEngine::AllFeatures) {
        }
        QStringList texts;
        bool begin(QPaintDevice*) override {
            return true;
        }
        bool end() override {
            return true;
        }
        void updateState(const QPaintEngineState&) override {
        }
        void drawTextItem(const QPointF& at, const QTextItem& item) override {
            // One run per font: "C♯m" may come as "C", "♯", "m", each
            // starting where the last one ended on the same baseline.
            const bool follows = m_lastWasText && std::abs(at.y() - m_lastEnd.y()) < 0.5 &&
                    std::abs(at.x() - m_lastEnd.x()) < 0.5;
            if (follows && !texts.isEmpty()) {
                texts.last() += item.text();
            } else {
                texts.append(item.text());
            }
            m_lastEnd = QPointF(at.x() + item.width(), at.y());
            m_lastWasText = true;
        }
        void drawPixmap(const QRectF&, const QPixmap&, const QRectF&) override {
            m_lastWasText = false;
        }
        void drawImage(const QRectF&, const QImage&, const QRectF&, Qt::ImageConversionFlags) override {
            m_lastWasText = false;
        }
        void drawTiledPixmap(const QRectF&, const QPixmap&, const QPointF&) override {
            m_lastWasText = false;
        }
        void drawPath(const QPainterPath&) override {
            m_lastWasText = false;
        }
        void drawPolygon(const QPointF*, int, PolygonDrawMode) override {
            m_lastWasText = false;
        }
        void drawPolygon(const QPoint*, int, PolygonDrawMode) override {
            m_lastWasText = false;
        }
        void drawRects(const QRectF*, int) override {
            m_lastWasText = false;
        }
        void drawRects(const QRect*, int) override {
            m_lastWasText = false;
        }
        void drawLines(const QLineF*, int) override {
            m_lastWasText = false;
        }
        void drawLines(const QLine*, int) override {
            m_lastWasText = false;
        }
        void drawEllipse(const QRectF&) override {
            m_lastWasText = false;
        }
        void drawEllipse(const QRect&) override {
            m_lastWasText = false;
        }
        void drawPoints(const QPointF*, int) override {
            m_lastWasText = false;
        }
        void drawPoints(const QPoint*, int) override {
            m_lastWasText = false;
        }
        Type type() const override {
            return QPaintEngine::User;
        }

      private:
        bool m_lastWasText = false;
        QPointF m_lastEnd;
    };
    mutable Engine m_engine;
};
