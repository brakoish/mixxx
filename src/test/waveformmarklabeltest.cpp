// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>

#include <QFont>
#include <QDomDocument>

#include "test/mixxxtest.h"
#include "waveform/waveformmarklabel.h"
#include "waveform/renderers/waveformmarkset.h"

class WaveformMarkLabelTest : public MixxxTest {};

TEST_F(WaveformMarkLabelTest, HotcueIdentifiersPreserveNumericDefaultAndSavedLabels) {
    EXPECT_EQ(QStringLiteral("1"), WaveformMarkSet::formatHotcueLabel(0, QString(), false));
    EXPECT_EQ(QStringLiteral("8: Drop"), WaveformMarkSet::formatHotcueLabel(7, QStringLiteral("Drop"), false));
    EXPECT_EQ(QStringLiteral("A"), WaveformMarkSet::formatHotcueLabel(0, QString(), true));
    EXPECT_EQ(QStringLiteral("H: Drop"), WaveformMarkSet::formatHotcueLabel(7, QStringLiteral("Drop"), true));
    EXPECT_EQ(QStringLiteral("9: Later"), WaveformMarkSet::formatHotcueLabel(8, QStringLiteral("Later"), true));
    EXPECT_EQ(QStringLiteral("CUE"), WaveformMarkSet::formatHotcueLabel(-1, QStringLiteral("CUE"), true));
    const QString savedLabel = QString::fromUtf8("Chorus · deuxième");
    EXPECT_EQ(QStringLiteral("B: ") + savedLabel, WaveformMarkSet::formatHotcueLabel(1, savedLabel, true));
    EXPECT_EQ(QString::fromUtf8("Chorus · deuxième"), savedLabel);
}

TEST_F(WaveformMarkLabelTest, LetterIdentifiersRequireExplicitSkinOptIn) {
    SkinContext context(config(), QString());
    for (const QString& format : {QString(), QStringLiteral("letters"), QStringLiteral("unknown")}) {
        QDomDocument document;
        const QString xml = format.isEmpty() ? QStringLiteral("<Visual/>") :
                                              QStringLiteral("<Visual><HotcueLabelFormat>%1</HotcueLabelFormat></Visual>").arg(format);
        ASSERT_TRUE(document.setContent(xml));
        WaveformMarkSet marks;
        marks.setup(QStringLiteral("[Channel1]"), document.documentElement(), context, {});
        EXPECT_EQ(format == QStringLiteral("letters") ? QStringLiteral("A: Intro") : QStringLiteral("1: Intro"),
                marks.hotcueLabel(0, QStringLiteral("Intro")));
    }
}

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
