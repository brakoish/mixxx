// SPDX-License-Identifier: GPL-2.0-or-later
// Lab FX instrument rules (round 4, move 3) against a generated fake engine:
// no effects manager, no decks, no audio. WDeckLab binds the same page to
// the real Lab FX units (see NativeDeckLabTest in decklabtest.cpp).
#include <gtest/gtest.h>

#include <QApplication>
#include <QHash>
#include <QImage>
#include <QMouseEvent>
#include <QPushButton>
#include <QTest>
#include <QWindow>
#include <memory>

#include "widget/decklablabfx.h"

namespace {

using Effect = DeckLabLabFx::Effect;
using Lamp = DeckLabLabFx::Lamp;

const QString kSlot1 = QStringLiteral("[DeckLabEffectUnit1_Effect4]");
const QString kSlot2 = QStringLiteral("[DeckLabEffectUnit2_Effect4]");
const QString kUnit1 = QStringLiteral("[DeckLabEffectUnit1]");
const QString kUnit2 = QStringLiteral("[DeckLabEffectUnit2]");

class DeckLabLabFxTest : public ::testing::Test {
  protected:
    void SetUp() override {
        load(1, 128, true);
        load(2, 124, false);
        DeckLabLabFxHost host;
        host.value = [this](const QString& g, const QString& i) { return values.value(key(g, i)); };
        host.set = [this](const QString& g, const QString& i, double v) { values[key(g, i)] = v; };
        host.setParameter = [this](const QString& g, const QString& i, double v) { values[key(g, i)] = v; };
        host.unitReady = [this](int) { return ready; };
        host.now = [this] { return clock; };
        host.deck = [](int deck) {
            DeckLabLabFxDeck info;
            info.loaded = deck <= 2;
            info.title = QStringLiteral("Generated Track %1").arg(deck);
            info.remaining = QStringLiteral("−3:25");
            return info;
        };
        host.showPage = [this](int page) { pages.append(page); };
        fx = std::make_unique<DeckLabLabFx>(host);
        fx->resize(800, 436);
        fx->show();
        QCoreApplication::processEvents();
    }
    static QString key(const QString& g, const QString& i) {
        return g + QLatin1Char(',') + i;
    }
    void load(int deck, double bpm, bool playing) {
        const QString group = QStringLiteral("[Channel%1]").arg(deck);
        values[key(group, "track_loaded")] = 1;
        values[key(group, "bpm")] = bpm;
        values[key(group, "play")] = playing ? 1 : 0;
        values[key(group, "beat_distance")] = 0.25;
    }
    double get(const QString& group, const QString& item) const {
        return values.value(key(group, item), -1);
    }
    double parameter(const QString& slot, int number) const {
        return get(slot, QStringLiteral("parameter%1").arg(number));
    }
    QPushButton* button(const char* name) const {
        return fx->findChild<QPushButton*>(QString::fromLatin1(name));
    }
    QPushButton* targetKey(int target) const {
        return fx->findChild<QPushButton*>(QStringLiteral("decklab-fx-target-%1").arg(target));
    }
    QPoint at(double x, double y) const {
        QWidget* pad = fx->pad();
        return {int(x * pad->width()), int((1 - y) * pad->height())};
    }
    void press(double x, double y) {
        QTest::mousePress(fx->pad(), Qt::LeftButton, {}, at(x, y));
    }
    void release(double x, double y) {
        QTest::mouseRelease(fx->pad(), Qt::LeftButton, {}, at(x, y));
    }
    void ticks(int count, qint64 step = 100) {
        for (int n = 0; n < count; ++n) {
            clock += step;
            fx->tick();
        }
    }
    void tap(const char* name) {
        QTest::mousePress(button(name), Qt::LeftButton);
        QTest::mouseRelease(button(name), Qt::LeftButton);
    }

    QHash<QString, double> values;
    qint64 clock = 10000;
    bool ready = true;
    QList<int> pages;
    std::unique_ptr<DeckLabLabFx> fx;
};

TEST_F(DeckLabLabFxTest, TouchDownPlaysAndLiftReturnsTheMusicalWay) {
    EXPECT_EQ(Lamp::Dry, fx->lamp());
    // ECHO: the band levers send into the beat delay on touch-down.
    press(0.55, 0.8);
    EXPECT_TRUE(fx->isOn()) << "the effect responds on touch-down";
    EXPECT_EQ(1, get(kSlot1, "enabled"));
    EXPECT_EQ(1, get(kUnit1, "mix"));
    EXPECT_EQ(1, get(kUnit1, "group_[Channel1]_enable"));
    EXPECT_EQ(0, get(kUnit1, "group_[Channel2]_enable"));
    for (int band = 1; band <= 3; ++band) EXPECT_EQ(1, parameter(kSlot1, band));
    EXPECT_NEAR(1.0 / 3, parameter(kSlot1, 4), 1e-9) << "lever mode 1: echo";
    EXPECT_GT(parameter(kSlot1, 15), 0) << "armed with a heartbeat";
    EXPECT_EQ(0, parameter(kSlot1, 13));
    // Lift: the tail rings out (release type 1) and the dry track stays.
    release(0.55, 0.8);
    EXPECT_FALSE(fx->isOn());
    EXPECT_EQ(Lamp::Tail, fx->lamp());
    EXPECT_NEAR(1.0 / 3, parameter(kSlot1, 13), 1e-9);
    EXPECT_EQ(1, get(kSlot1, "enabled")) << "the tail plays on the same unit";
    ticks(25); // one bar at 128 BPM is 1.9 s, clamped to 2 s
    EXPECT_EQ(Lamp::Dry, fx->lamp());
    EXPECT_EQ(0, get(kSlot1, "enabled"));
    EXPECT_EQ(0, get(kUnit1, "mix"));
    EXPECT_EQ(0, get(kUnit1, "group_[Channel1]_enable"));
    // FILTER: lift goes straight back to dry, no echo of the filter.
    fx->selectEffect(Effect::Filter);
    press(0.9, 0.7);
    EXPECT_NEAR(4.0 / 5, parameter(kSlot1, 9), 1e-9) << "isolator mode 4: filter";
    EXPECT_GT(parameter(kSlot1, 6), 0.5) << "right of centre is high-pass";
    release(0.9, 0.7);
    EXPECT_EQ(Lamp::Dry, fx->lamp());
    EXPECT_EQ(0, get(kSlot1, "enabled"));
}

TEST_F(DeckLabLabFxTest, DoubleTapLatchesAndATileTapNeverSounds) {
    // One tap on a tile only chooses it.
    tap("decklab-fx-effect-1");
    EXPECT_EQ(Effect::Reverb, fx->effect());
    EXPECT_FALSE(fx->isOn()) << "choosing an effect is silent";
    EXPECT_NE(1, get(kSlot1, "enabled"));
    // A second tap within the window latches it on.
    clock += 200;
    tap("decklab-fx-effect-1");
    EXPECT_TRUE(fx->isLatched());
    EXPECT_NEAR(2.0 / 3, parameter(kSlot1, 4), 1e-9) << "lever mode 2: reverb";
    // ON stops it: like a finger lifting, the reverb tail rings out.
    tap("decklab-fx-on");
    EXPECT_EQ(Lamp::Tail, fx->lamp());
    ticks(30);
    // On the pad: tap, tap = latched; lifting keeps it on.
    fx->selectEffect(Effect::Filter);
    clock += 1000;
    press(0.8, 0.6);
    clock += 80;
    release(0.8, 0.6);
    EXPECT_FALSE(fx->isOn());
    clock += 120;
    press(0.81, 0.6);
    clock += 90;
    release(0.81, 0.6);
    EXPECT_TRUE(fx->isLatched()) << "double-tap on the pad latches";
    EXPECT_EQ(1, get(kSlot1, "enabled"));
    // A touch on a latched effect moves it and keeps it latched.
    clock += 1000;
    press(0.2, 0.5);
    EXPECT_LT(parameter(kSlot1, 6), 0.5) << "now low-pass";
    release(0.2, 0.5);
    EXPECT_TRUE(fx->isLatched());
    // Two slow taps are not a double-tap.
    tap("decklab-fx-cut");
    clock += 1000;
    press(0.8, 0.6);
    clock += 80;
    release(0.8, 0.6);
    clock += 600;
    press(0.8, 0.6);
    clock += 80;
    release(0.8, 0.6);
    EXPECT_FALSE(fx->isOn());
}

TEST_F(DeckLabLabFxTest, PadColumnsAreTheBeatCellsAndTheStripSteps) {
    EXPECT_EQ(QStringList({"1/16", "1/8", "1/4", "1/2", "3/4", "1"}), fx->beatLabels());
    EXPECT_EQ(3, fx->beatIndex());
    // The finger in a column picks that beat, live.
    QWidget* cell = button("decklab-fx-beat-0");
    const double first = (cell->geometry().center().x() - fx->pad()->x()) / double(fx->pad()->width());
    press(first, 0.6);
    EXPECT_EQ(0, fx->beatIndex());
    EXPECT_NEAR((0.0625 - .0625) / .9375, parameter(kSlot1, 12), 1e-9);
    const QWidget* last = button("decklab-fx-beat-5");
    QTest::mouseMove(fx->pad(), at((last->geometry().center().x() - fx->pad()->x()) / double(fx->pad()->width()), 0.6));
    EXPECT_EQ(5, fx->beatIndex());
    EXPECT_NEAR(1.0, parameter(kSlot1, 12), 1e-9) << "one beat";
    release(0.9, 0.6);
    // Beat ◀▶ step and stop at the ends.
    tap("decklab-fx-beat-right");
    EXPECT_EQ(5, fx->beatIndex());
    EXPECT_FALSE(button("decklab-fx-beat-right")->isEnabled());
    tap("decklab-fx-beat-left");
    EXPECT_EQ(4, fx->beatIndex());
    EXPECT_TRUE(fx->readoutTitle().contains("3/4 BEAT"));
    // ROLL has its own beats (Mixxx roll sizes); FILTER has none.
    fx->selectEffect(Effect::Roll);
    EXPECT_EQ(QStringList({"1/16", "1/8", "1/4", "1/2", "1", "2"}), fx->beatLabels());
    fx->selectEffect(Effect::Filter);
    EXPECT_FALSE(button("decklab-fx-beat-2")->isEnabled());
    EXPECT_EQ(QStringLiteral("FILTER"), fx->readoutTitle());
}

TEST_F(DeckLabLabFxTest, NoTimeLimitWhileAFingerIsDown) {
    press(0.5, 0.5);
    double heartbeat = parameter(kSlot1, 15);
    for (int second = 0; second < 60; ++second) {
        ticks(10);
        ASSERT_TRUE(fx->isOn()) << "still on after " << second + 1 << " s";
        EXPECT_NE(heartbeat, parameter(kSlot1, 15));
        heartbeat = parameter(kSlot1, 15);
    }
    release(0.5, 0.5);
    EXPECT_FALSE(fx->isOn());
}

TEST_F(DeckLabLabFxTest, LeavingThePageKeepsEffectsAndLostTouchesStillEnd) {
    // Latched: keeps playing (and its heartbeat) while the page is hidden.
    tap("decklab-fx-on");
    ASSERT_TRUE(fx->isLatched());
    fx->hide();
    ticks(50);
    EXPECT_TRUE(fx->isLatched());
    EXPECT_EQ(1, get(kSlot1, "enabled"));
    EXPECT_TRUE(fx->tabLit()) << "the rail tab shows it";
    fx->show();
    tap("decklab-fx-cut");
    // Held: the page goes away under the finger; the hold stays until the
    // finger's release reaches the window.
    press(0.5, 0.5);
    fx->hide();
    ticks(10);
    EXPECT_TRUE(fx->isHeld());
    EXPECT_TRUE(fx->isOn());
    QWindow window;
    QMouseEvent lift(QEvent::MouseButtonRelease, QPointF(5, 5), QPointF(5, 5), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    fx->noteTouch(&window, &lift);
    EXPECT_FALSE(fx->isHeld());
    EXPECT_FALSE(fx->isOn()) << "the finger lifted: ECHO rings out";
    EXPECT_EQ(Lamp::Tail, fx->lamp());
    fx->show();
    tap("decklab-fx-cut");
    // Visible page: a press reaching another control ends a hold whose
    // release went astray; a latched effect only stops following the finger.
    QWidget elsewhere;
    QMouseEvent touch(QEvent::MouseButtonPress, QPointF(5, 5), QPointF(5, 5), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    press(0.5, 0.5);
    fx->noteTouch(fx->pad(), &touch);
    EXPECT_TRUE(fx->isHeld()) << "the pad's own events are not foreign";
    fx->noteTouch(&elsewhere, &touch);
    EXPECT_FALSE(fx->isHeld());
    EXPECT_FALSE(fx->isOn());
    tap("decklab-fx-cut");
    tap("decklab-fx-on");
    press(0.5, 0.5);
    fx->noteTouch(&elsewhere, &touch);
    EXPECT_TRUE(fx->isLatched());
}

TEST_F(DeckLabLabFxTest, ChangingTargetRingsTheOldDeckOutOnItsOwnUnit) {
    tap("decklab-fx-on");
    ASSERT_TRUE(fx->isLatched());
    tap("decklab-fx-target-2");
    EXPECT_EQ(2, fx->target());
    EXPECT_FALSE(fx->isOn()) << "the new target starts dry";
    EXPECT_EQ(Lamp::Tail, fx->lamp());
    EXPECT_EQ(1, get(kSlot1, "enabled")) << "deck 1's tail rings on unit 1";
    EXPECT_NEAR(1.0 / 3, parameter(kSlot1, 13), 1e-9);
    // Deck 2 plays on unit 2 while deck 1's tail rings out.
    press(0.5, 0.5);
    EXPECT_EQ(1, get(kSlot2, "enabled"));
    EXPECT_EQ(1, get(kUnit2, "group_[Channel2]_enable"));
    EXPECT_EQ(1, get(kSlot1, "enabled"));
    release(0.5, 0.5);
    tap("decklab-fx-cut");
    // MASTER: every deck and sampler on unit 1. SMP: the samplers on unit 2.
    tap("decklab-fx-target-5");
    tap("decklab-fx-on");
    for (int deck = 1; deck <= 4; ++deck)
        EXPECT_EQ(1, get(kUnit1, QStringLiteral("group_[Channel%1]_enable").arg(deck)));
    EXPECT_EQ(1, get(kUnit1, "group_[Sampler16]_enable"));
    tap("decklab-fx-cut");
    tap("decklab-fx-target-6");
    EXPECT_FALSE(fx->available()) << "no sample loaded";
    values[key("[Sampler3]", "track_loaded")] = 1;
    fx->refresh();
    tap("decklab-fx-on");
    EXPECT_EQ(1, get(kUnit2, "group_[Sampler3]_enable"));
    EXPECT_EQ(0, get(kUnit2, "group_[Channel1]_enable"));
}

TEST_F(DeckLabLabFxTest, EchoOutDropsTheTrackForABarThenFadesItBack) {
    tap("decklab-fx-on");
    tap("decklab-fx-echo-out");
    EXPECT_NEAR(2.0 / 3, parameter(kSlot1, 13), 1e-9) << "release type 2: the track drops";
    ticks(21);
    EXPECT_EQ(1, parameter(kSlot1, 13)) << "release type 3: the track fades back in over a beat";
    EXPECT_EQ(1, get(kSlot1, "enabled"));
    ticks(7);
    EXPECT_EQ(0, get(kSlot1, "enabled"));
    EXPECT_EQ(Lamp::Dry, fx->lamp());
    // CUT is dry at once, tail and all.
    tap("decklab-fx-on");
    tap("decklab-fx-release");
    EXPECT_EQ(Lamp::Tail, fx->lamp());
    tap("decklab-fx-cut");
    EXPECT_EQ(Lamp::Dry, fx->lamp());
    EXPECT_EQ(0, get(kSlot1, "enabled"));
    EXPECT_EQ(0, get(kUnit1, "mix"));
}

TEST_F(DeckLabLabFxTest, RollNeedsAPlayingDeckAndSlidesBetweenSizes) {
    fx->selectEffect(Effect::Roll);
    fx->selectTarget(2); // paused
    press(0.5, 0.5);
    EXPECT_FALSE(fx->isOn()) << "nothing to roll on a paused deck";
    EXPECT_TRUE(fx->padHint().contains("PLAY DECK 2"));
    release(0.5, 0.5);
    fx->selectTarget(1); // playing
    fx->selectBeat(2);
    tap("decklab-fx-on");
    EXPECT_EQ(1, get("[Channel1]", "beatlooproll_0.25_activate"));
    fx->selectBeat(1);
    EXPECT_EQ(1, get("[Channel1]", "beatlooproll_0.125_activate"));
    EXPECT_EQ(0, get("[Channel1]", "beatlooproll_0.25_activate")) << "the new size first, then the old one off";
    EXPECT_EQ(0, parameter(kSlot1, 1)) << "the slot runs dry under a roll";
    tap("decklab-fx-on");
    EXPECT_EQ(0, get("[Channel1]", "beatlooproll_0.125_activate"));
    EXPECT_FALSE(fx->isWet());
}

TEST_F(DeckLabLabFxTest, GuiStallReturnsDryLikeTheDspDeadman) {
    tap("decklab-fx-on");
    ticks(3);
    EXPECT_TRUE(fx->isLatched());
    clock += 2000; // the GUI thread stalled longer than the DSP's 1.5 s
    fx->tick();
    EXPECT_FALSE(fx->isOn());
    EXPECT_EQ(0, get(kSlot1, "enabled"));
}

TEST_F(DeckLabLabFxTest, EmptyStatesOfferBrowseOrTheSamplerPage) {
    fx->selectTarget(3);
    EXPECT_FALSE(fx->available());
    EXPECT_FALSE(button("decklab-fx-on")->isEnabled());
    auto* action = button("decklab-fx-pad-action");
    ASSERT_TRUE(action->isVisible());
    EXPECT_EQ(QStringLiteral("BROWSE"), action->text());
    QTest::mousePress(action, Qt::LeftButton);
    EXPECT_TRUE(pages.isEmpty()) << "the page never changes under a finger that is down";
    QTest::mouseRelease(action, Qt::LeftButton);
    EXPECT_EQ(QList<int>{1}, pages);
    fx->selectTarget(6);
    EXPECT_EQ(QStringLiteral("SAMPLER ›"), action->text());
    QTest::mouseClick(action, Qt::LeftButton);
    EXPECT_EQ(2, pages.last());
    // The footer is the Sampler page link (it replaces the Samples tab).
    QTest::mouseClick(button("decklab-fx-deck"), Qt::LeftButton);
    EXPECT_EQ(2, pages.last());
    ready = false;
    fx->selectTarget(1);
    EXPECT_FALSE(fx->available());
    EXPECT_FALSE(action->isVisible()) << "no action can fix a missing effect";
}

TEST_F(DeckLabLabFxTest, BrandLampsAndNoRedOutsideTheWordmark) {
    const QColor lit(0xf2, 0xf4, 0xf7), panel(0x16, 0x19, 0x1e);
    auto* on = button("decklab-fx-on");
    EXPECT_EQ(panel, on->grab().toImage().pixelColor(4, on->height() / 2)) << "DRY: an outline";
    tap("decklab-fx-on");
    EXPECT_EQ(lit, on->grab().toImage().pixelColor(4, on->height() / 2)) << "ON: a filled lamp";
    EXPECT_TRUE(fx->isLatched());
    const QImage page = fx->grab().toImage();
    const QRect wordmark = fx->findChild<QWidget*>("decklab-fx-wordmark")->geometry();
    for (int y = 0; y < page.height(); y += 2) {
        for (int x = 0; x < page.width(); x += 2) {
            if (wordmark.contains(x, y)) continue;
            const QColor c = page.pixelColor(x, y);
            ASSERT_FALSE(c.red() > 200 && c.green() < 90 && c.blue() < 90) << "live red at " << x << "," << y;
        }
    }
    // Every key is a 44 px touch target.
    for (auto* key : fx->findChildren<QPushButton*>()) {
        if (!key->isVisible()) continue;
        EXPECT_GE(key->height(), 44) << key->objectName().toStdString();
        EXPECT_GE(key->width(), 44) << key->objectName().toStdString();
    }
}

// r6-fxstrip: the 2-deck layout offers decks 1 and 2, MASTER and SMP, and
// deck 3 or 4 only while it is in use. Every slot keeps its place.
TEST_F(DeckLabLabFxTest, TwoDeckLayoutOffersDecksThreeAndFourOnlyWhileInUse) {
    std::array<QRect, 6> slots;
    for (int n = 0; n < 6; ++n) slots[n] = targetKey(n + 1)->geometry();
    const QRect on = button("decklab-fx-on")->geometry();
    const QRect readout = fx->findChild<QWidget*>("decklab-fx-readout")->geometry();
    const auto unmoved = [&] {
        for (int n = 0; n < 6; ++n) EXPECT_EQ(slots[n], targetKey(n + 1)->geometry()) << "target " << n + 1;
        EXPECT_EQ(on, button("decklab-fx-on")->geometry());
        EXPECT_EQ(readout, fx->findChild<QWidget*>("decklab-fx-readout")->geometry());
    };
    fx->setDeckLayout(2);
    EXPECT_EQ(2, fx->deckLayout());
    for (int target : {1, 2, 5, 6}) {
        EXPECT_TRUE(fx->targetShown(target)) << target;
        EXPECT_TRUE(targetKey(target)->isVisible()) << target;
    }
    for (int target : {3, 4}) {
        EXPECT_FALSE(fx->targetShown(target)) << target;
        EXPECT_FALSE(targetKey(target)->isVisible()) << target;
    }
    unmoved();
    // A deck loaded from the controller gets its key back (the surface
    // refreshes Lab FX on every load), in its own slot.
    load(3, 126, false);
    fx->refresh();
    EXPECT_TRUE(targetKey(3)->isVisible());
    EXPECT_FALSE(targetKey(4)->isVisible());
    EXPECT_EQ(QStringLiteral("3"), targetKey(3)->text());
    unmoved();
    // It plays deck 3 on deck 3's unit.
    tap("decklab-fx-target-3");
    EXPECT_EQ(3, fx->target());
    tap("decklab-fx-on");
    EXPECT_EQ(1, get(kUnit1, "group_[Channel3]_enable"));
    EXPECT_EQ(0, get(kUnit1, "group_[Channel1]_enable"));
    tap("decklab-fx-cut");
    // Ejected while it is the target: the key stays (never a silent
    // retarget); choosing another deck lets it go.
    values[key("[Channel3]", "track_loaded")] = 0;
    fx->refresh();
    EXPECT_EQ(3, fx->target());
    EXPECT_TRUE(targetKey(3)->isVisible()) << "the target keeps its key";
    tap("decklab-fx-target-1");
    EXPECT_FALSE(targetKey(3)->isVisible());
    // Four decks: all six keys, same places.
    fx->setDeckLayout(4);
    for (int target = 1; target <= 6; ++target) {
        EXPECT_TRUE(fx->targetShown(target)) << target;
        EXPECT_TRUE(targetKey(target)->isVisible()) << target;
    }
    unmoved();
}

TEST_F(DeckLabLabFxTest, TwoDeckLayoutNeverHidesOrRetargetsASoundingDeck) {
    const QColor violet(0xc7, 0x7d, 0xff);
    // ECHO latched on deck 4 with four decks, then the layout drops to two.
    load(4, 126, true);
    fx->refresh();
    tap("decklab-fx-target-4");
    tap("decklab-fx-on");
    ASSERT_TRUE(fx->isLatched());
    fx->setDeckLayout(2);
    EXPECT_EQ(4, fx->target()) << "never retargeted";
    EXPECT_TRUE(fx->isLatched()) << "still sounding";
    EXPECT_EQ(1, get(kUnit2, "group_[Channel4]_enable"));
    QPushButton* fourth = targetKey(4);
    ASSERT_TRUE(fourth->isVisible());
    EXPECT_EQ(violet, fourth->grab().toImage().pixelColor(4, fourth->height() / 2))
            << "lit: the selected key fills with its deck colour";
    // Deck 1 next: deck 4's tail rings out on its own unit, and its key
    // stays while it rings, even once the track is gone.
    tap("decklab-fx-target-1");
    EXPECT_EQ(Lamp::Tail, fx->lamp());
    values[key("[Channel4]", "track_loaded")] = 0;
    fx->refresh();
    EXPECT_EQ(1, get(kSlot2, "enabled")) << "the tail plays on";
    EXPECT_TRUE(fourth->isVisible()) << "a deck still ringing out keeps its key";
    ticks(40);
    EXPECT_EQ(Lamp::Dry, fx->lamp());
    EXPECT_FALSE(fourth->isVisible()) << "dry, empty and not the target: the key goes";
    EXPECT_EQ(1, fx->target());
}

TEST_F(DeckLabLabFxTest, TwoDeckLayoutTouchPlaysTheSelectedDeckAfterALayerFlip) {
    fx->setDeckLayout(2);
    load(3, 126, true);
    fx->refresh();
    tap("decklab-fx-on");
    ASSERT_TRUE(fx->isLatched());
    ASSERT_EQ(1, fx->target());
    // The controller's left layer flips to deck 3: the surface passes on the
    // deck change and the (unchanged) layout. Lab FX has no notion of a
    // layer, so nothing moves.
    fx->refresh();
    fx->setDeckLayout(2);
    EXPECT_EQ(1, fx->target());
    EXPECT_TRUE(fx->isLatched());
    EXPECT_EQ(1, get(kUnit1, "group_[Channel1]_enable"));
    tap("decklab-fx-on"); // lift: deck 1's tail rings out
    EXPECT_EQ(Lamp::Tail, fx->lamp());
    clock += 1000;
    press(0.5, 0.5); // engage() refreshes first: still deck 1
    EXPECT_TRUE(fx->isOn());
    EXPECT_EQ(1, fx->target());
    EXPECT_EQ(1, get(kUnit1, "group_[Channel1]_enable")) << "the deck the DJ selected";
    EXPECT_EQ(0, get(kUnit1, "group_[Channel3]_enable")) << "deck 3 never gets the effect";
    release(0.5, 0.5);
    tap("decklab-fx-cut");
}

} // namespace

#ifdef DECKLAB_LABFX_STANDALONE_TEST
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
