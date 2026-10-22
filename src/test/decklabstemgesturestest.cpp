// SPDX-License-Identifier: GPL-2.0-or-later
// Stem pad gesture rules against fake stem mute controls: no engine, no
// widgets, no audio. The pads bind the same rules to Mixxx's
// [ChannelN_StemM],mute controls (see decklabstemstest.cpp).
#include <gtest/gtest.h>

#include <array>

#include "widget/decklabstemgestures.h"

namespace {

using Result = DeckLabStemGestures::Result;
using Context = DeckLabStemGestures::Context;
using Mutes = std::array<bool, 4>;

class DeckLabStemGesturesTest : public ::testing::Test {
  protected:
    void SetUp() override {
        gestures.setCount(4);
    }
    // Press, then release over the pad after the given time.
    Result tap(int stem, qint64 at, const Context& context = {}) {
        EXPECT_TRUE(gestures.press(stem, context));
        return gestures.release(true, context, at);
    }
    Mutes muted{};
    int writes = 0;
    DeckLabStemGestures gestures{[this](int stem) { return muted[stem]; },
            [this](int stem, bool on) {
                muted[stem] = on;
                ++writes;
            }};
};

TEST_F(DeckLabStemGesturesTest, TapTogglesOnReleaseAndDoubleTapRestoresAllStems) {
    ASSERT_TRUE(gestures.press(1, {}));
    EXPECT_EQ(0, writes) << "a tap acts on release, never on press";
    EXPECT_EQ(Result::Toggled, gestures.release(true, {}, 100));
    EXPECT_EQ((Mutes{false, true, false, false}), muted);
    EXPECT_EQ(Result::Toggled, tap(1, 700)) << "a slow second tap toggles again";
    EXPECT_EQ((Mutes{}), muted);
    EXPECT_EQ(Result::Toggled, tap(2, 1200));
    EXPECT_EQ(Result::Toggled, tap(0, 1300)) << "quick taps on two pads are two toggles";
    EXPECT_EQ((Mutes{true, false, true, false}), muted);
    EXPECT_EQ(Result::Toggled, tap(3, 2000));
    EXPECT_EQ((Mutes{true, false, true, true}), muted);
    EXPECT_EQ(Result::RestoredAll, tap(3, 2000 + DeckLabStemGestures::kDoubleTapMs));
    EXPECT_EQ((Mutes{}), muted);
    EXPECT_EQ(Result::Toggled, tap(3, 2500)) << "a restore starts a new sequence";
    EXPECT_EQ((Mutes{false, false, false, true}), muted);
}

TEST_F(DeckLabStemGesturesTest, HoldSolosWhileHeldAndReleaseRestoresThePreviousState) {
    muted = {false, true, false, false};
    ASSERT_TRUE(gestures.press(1, {}));
    EXPECT_EQ(Result::Soloed, gestures.hold({}));
    EXPECT_EQ(1, gestures.soloStem());
    EXPECT_EQ((Mutes{true, false, true, true}), muted) << "the held stem plays alone, even if it was muted";
    EXPECT_EQ(Result::None, gestures.hold({})) << "one solo per press";
    EXPECT_EQ(Result::SoloEnded, gestures.release(true, {}, 5000));
    EXPECT_EQ((Mutes{false, true, false, false}), muted);
    EXPECT_FALSE(gestures.active());
    // Releasing elsewhere still ends the solo.
    ASSERT_TRUE(gestures.press(2, {}));
    gestures.hold({});
    EXPECT_EQ((Mutes{true, true, false, true}), muted);
    EXPECT_EQ(Result::SoloEnded, gestures.release(false, {}, 6000));
    EXPECT_EQ((Mutes{false, true, false, false}), muted);
}

TEST_F(DeckLabStemGesturesTest, HoldIsNeverHalfOfADoubleTap) {
    EXPECT_EQ(Result::Toggled, tap(2, 100));
    ASSERT_TRUE(gestures.press(2, {}));
    gestures.hold({});
    gestures.release(true, {}, 450);
    EXPECT_EQ((Mutes{false, false, true, false}), muted);
    EXPECT_EQ(Result::Toggled, tap(2, 500)) << "taps around a hold do not restore all";
    EXPECT_EQ((Mutes{}), muted);
}

TEST_F(DeckLabStemGesturesTest, StaleOrCancelledTapsWriteNothing) {
    ASSERT_TRUE(gestures.press(0, {1, 1}));
    EXPECT_EQ(Result::Ignored, gestures.release(true, {2, 1}, 100)) << "page, layer or layout changed";
    ASSERT_TRUE(gestures.press(0, {2, 1}));
    EXPECT_EQ(Result::Ignored, gestures.release(true, {2, 2}, 200)) << "another track was loaded";
    ASSERT_TRUE(gestures.press(0, {2, 2}));
    EXPECT_EQ(Result::Ignored, gestures.release(false, {2, 2}, 300)) << "released off the pad";
    ASSERT_TRUE(gestures.press(0, {2, 2}));
    EXPECT_EQ(Result::Ignored, gestures.hold({3, 2})) << "no solo after a context change";
    EXPECT_EQ(Result::Ignored, gestures.release(true, {3, 2}, 900));
    ASSERT_TRUE(gestures.press(1, {}));
    EXPECT_EQ(Result::Ignored, gestures.cancel(false)) << "a cancelled press never toggles";
    EXPECT_EQ(Result::None, gestures.release(true, {}, 950));
    EXPECT_EQ(0, writes);
    EXPECT_EQ((Mutes{}), muted);
}

TEST_F(DeckLabStemGesturesTest, PadsOnlyExistForStemsOnTheTrack) {
    gestures.setCount(2);
    EXPECT_FALSE(gestures.press(2, {}));
    EXPECT_FALSE(gestures.press(-1, {}));
    gestures.setCount(0);
    EXPECT_FALSE(gestures.press(0, {})) << "no pads without a stem track";
    gestures.setCount(4);
    ASSERT_TRUE(gestures.press(3, {}));
    EXPECT_FALSE(gestures.press(0, {})) << "one gesture at a time";
    gestures.setCount(0);
    EXPECT_EQ(Result::Ignored, gestures.release(true, {}, 100)) << "the stems went away mid-tap";
    EXPECT_EQ(0, writes);
}

TEST_F(DeckLabStemGesturesTest, SoloAlwaysEndsEvenAfterAPageOrLayerChange) {
    muted = {false, false, false, true};
    ASSERT_TRUE(gestures.press(0, {4, 1}));
    gestures.hold({4, 1});
    EXPECT_EQ((Mutes{false, true, true, true}), muted);
    EXPECT_EQ(Result::SoloEnded, gestures.release(true, {5, 1}, 2000));
    EXPECT_EQ((Mutes{false, false, false, true}), muted);
    // A cancel (hidden surface, touch cancel) ends it the same way.
    ASSERT_TRUE(gestures.press(3, {5, 1}));
    gestures.hold({5, 1});
    EXPECT_EQ((Mutes{true, true, true, false}), muted);
    EXPECT_EQ(Result::SoloEnded, gestures.cancel(false));
    EXPECT_EQ((Mutes{false, false, false, true}), muted);
    EXPECT_EQ(Result::None, gestures.release(true, {5, 1}, 2500)) << "the later release is spent";
}

TEST_F(DeckLabStemGesturesTest, TrackLoadedMidHoldFollowsTheEngineStemReset) {
    // Default: Mixxx unmutes every stem when a track loads. Nothing to undo.
    muted = {true, false, false, false};
    ASSERT_TRUE(gestures.press(0, {1, 1}));
    gestures.hold({1, 1});
    EXPECT_EQ((Mutes{false, true, true, true}), muted);
    muted = {};
    const int before = writes;
    EXPECT_EQ(Result::SoloEnded, gestures.cancel(true));
    EXPECT_EQ(before, writes);
    EXPECT_EQ((Mutes{}), muted) << "the new track must not inherit the old track's mutes";
    // The same when the release arrives after the load.
    muted = {true, false, false, false};
    ASSERT_TRUE(gestures.press(0, {1, 2}));
    gestures.hold({1, 2});
    muted = {};
    gestures.release(true, {1, 3}, 900);
    EXPECT_EQ((Mutes{}), muted);
    // stem_auto_reset off: the deck kept the solo, so the pre-hold state returns.
    muted = {true, false, false, false};
    ASSERT_TRUE(gestures.press(0, {1, 3}));
    gestures.hold({1, 3});
    EXPECT_EQ(Result::SoloEnded, gestures.cancel(true));
    EXPECT_EQ((Mutes{true, false, false, false}), muted);
}

TEST_F(DeckLabStemGesturesTest, MutesChangedElsewhereDuringASoloAreKept) {
    ASSERT_TRUE(gestures.press(1, {}));
    gestures.hold({});
    EXPECT_EQ((Mutes{true, false, true, true}), muted);
    // A controller unmutes stem 4 and mutes the soloed stem meanwhile.
    muted[3] = false;
    muted[1] = true;
    gestures.release(true, {}, 1000);
    EXPECT_EQ((Mutes{false, true, false, false}), muted);
}

} // namespace
