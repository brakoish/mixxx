// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <iterator>

#include <QFont>
#include <QDomDocument>

#include "control/controlobject.h"
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

TEST_F(WaveformMarkLabelTest, CompactMarkerLanesRequireOptInAndPreserveTwoDeckImages) {
    ControlObject cue(ConfigKey("[Channel1]","cue_point")); cue.set(44100);
    ControlObject loop(ConfigKey("[Channel1]","loop_start_position")); loop.set(44100);
    SkinContext context(config(),QString());
    for(int breadth:{55,110}) for(double scale:{1.0,2.0}) {
        QList<QImage> legacyImages;
        for(const QString& policy:{QString(),QStringLiteral("separate"),QStringLiteral("unknown")}) {
            QDomDocument document;
            ASSERT_TRUE(document.setContent(QStringLiteral("<Visual><CompactMarkerLabels>%1</CompactMarkerLabels>"
                    "<Mark><Control>cue_point</Control><Text>CUE</Text><Color>#ff6000</Color><Align>bottom</Align></Mark>"
                    "<Mark><Control>loop_start_position</Control><Text>LOOP IN</Text><Color>#6ee128</Color><Align>bottom</Align></Mark></Visual>").arg(policy)));
            WaveformMarkSet marks; marks.setup("[Channel1]",document.documentElement(),context,{});
            marks.setBreadth(breadth); marks.update();
            QList<QRectF> labels;
            int index=0;
            for(const auto& mark:marks) {
                const QImage image=mark->generateImage(scale); ASSERT_FALSE(image.isNull());
                const auto area=mark->m_label.area().translated(mark->getOffset(),0);
                ASSERT_FALSE(area.isEmpty()); EXPECT_GE(area.top(),0); EXPECT_LE(area.bottom(),breadth);
                labels << area;
                EXPECT_DOUBLE_EQ(44100,mark->getSamplePosition());
                EXPECT_FLOAT_EQ(.5f,mark->m_linePosition+mark->getOffset());
                EXPECT_EQ(int(breadth*scale),image.height());
                if(policy.isEmpty()) legacyImages << image;
                else if(breadth==110 || policy==QStringLiteral("unknown")) EXPECT_EQ(legacyImages[index],image);
                ++index;
            }
            ASSERT_EQ(2,labels.size());
            EXPECT_EQ(breadth==55 && policy!=QStringLiteral("separate"),labels[0].intersects(labels[1]));
        }
    }
}

TEST_F(WaveformMarkLabelTest, CompactMarkerOverflowOmitsOnlyLabelAndKeepsExactLine) {
    ControlObject cue(ConfigKey("[Channel1]","cue_point")); cue.set(44100);
    SkinContext context(config(),QString()); QDomDocument document;
    ASSERT_TRUE(document.setContent(QStringLiteral("<Visual><CompactMarkerLabels>separate</CompactMarkerLabels>"
            "<Mark><Control>cue_point</Control><Text>CUE</Text><Color>#ff6000</Color><Align>bottom</Align></Mark></Visual>")));
    WaveformMarkSet marks; marks.setup("[Channel1]",document.documentElement(),context,{});
    marks.setBreadth(55); marks.update(); ASSERT_EQ(1,std::distance(marks.begin(),marks.end()));
    const auto mark=*marks.begin();
    for(double scale:{1.0,2.0}) {
        mark->setLevel(0); const auto visible=mark->generateImage(scale); ASSERT_FALSE(mark->m_label.area().isEmpty());
        const float line=mark->m_linePosition; const float offset=mark->getOffset();
        mark->setLevel(20); const auto overflow=mark->generateImage(scale);
        EXPECT_TRUE(mark->m_label.area().isEmpty()); EXPECT_EQ(visible.size(),overflow.size());
        EXPECT_FLOAT_EQ(line,mark->m_linePosition); EXPECT_FLOAT_EQ(offset,mark->getOffset());
        EXPECT_GT(overflow.pixelColor(overflow.width()/2,overflow.height()/2).alpha(),0);
        EXPECT_DOUBLE_EQ(44100,mark->getSamplePosition());
    }
    // Tall views retain legacy clipping, including both a partly visible
    // label and a label entirely beyond the available lanes.
    mark->setBreadth(110);
    for(double scale:{1.0,2.0}) {
        mark->setSeparateCompactLabels(false); mark->setLevel(0); mark->generateImage(scale);
        const double increment=mark->m_label.area().height()+2;
        const int firstClipped=int((109.5-mark->m_label.area().height())/increment)+1;
        for(int level:{firstClipped,20}) {
            mark->setLevel(level); mark->setSeparateCompactLabels(false);
            const auto legacy=mark->generateImage(scale); const auto legacyArea=mark->m_label.area();
            ASSERT_LT(legacyArea.top(),0);
            if(level==firstClipped) ASSERT_GT(legacyArea.bottom(),0);
            mark->setSeparateCompactLabels(true); const auto optedIn=mark->generateImage(scale);
            EXPECT_EQ(legacy,optedIn); EXPECT_EQ(legacyArea,mark->m_label.area());
        }
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
