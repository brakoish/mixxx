// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>

#include <QFont>

#include "test/mixxxtest.h"
#include "waveform/waveformmarklabel.h"

class WaveformMarkLabelTest : public MixxxTest {};

TEST_F(WaveformMarkLabelTest, EdgeLabelsStayInsideWithoutChangingWidth) {
    QFont font;
    font.setPixelSize(12);
    for (const double scale : {1.0, 2.0}) {
        WaveformMarkLabel centered;
        centered.prerender(QPointF(75, 20), QPixmap(), QStringLiteral("CUE"),
                font, Qt::white, Qt::black, 200, scale);
        for (const double edge : {-25.0, 195.0}) {
            WaveformMarkLabel label;
            label.prerender(QPointF(edge, 20), QPixmap(), QStringLiteral("CUE"),
                    font, Qt::white, Qt::black, 200, scale);
            EXPECT_GE(label.area().left(), 0);
            EXPECT_LE(label.area().right(), 200);
            EXPECT_DOUBLE_EQ(centered.area().width(), label.area().width());
            EXPECT_DOUBLE_EQ(centered.area().height(), label.area().height());
        }
    }
}

TEST_F(WaveformMarkLabelTest, OverwideLabelsRemainInsideAtBothEdges) {
    QFont font;
    font.setPixelSize(12);
    for (const double edge : {-80.0, 30.0}) {
        WaveformMarkLabel label;
        label.prerender(QPointF(edge, 20), QPixmap(),
                QStringLiteral("Long existing hot cue title"),
                font, Qt::white, Qt::black, 32, 1);
        EXPECT_GE(label.area().left(), 0);
        EXPECT_LE(label.area().right(), 32);
        EXPECT_GT(label.area().width(), 0);
    }
}
