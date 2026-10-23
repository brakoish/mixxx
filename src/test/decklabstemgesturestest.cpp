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
    // A touch at the given time, then its release.
    Result tap(int stem, qint64 at, const Context& context = {}) {
        const Result result = gestures.press(stem, context, at);
        EXPECT_EQ(Result::None, gestures.release(context));
        return result;
    }
    Mutes muted{};
    int writes = 0;
    DeckLabStemGestures gestures{[this](int stem) { return muted[stem]; },
            [this](int stem, bool on) {
                muted[stem] = on;
                ++writes;
            }};
};

TEST_F(DeckLabStemGesturesTest, TouchTogglesOnPressAndDoubleTapRestoresAllStems) {
    EXPECT_EQ(Result::Toggled, gestures.press(1, {}, 0));
    EXPECT_EQ(1, writes) << "a touch acts on touch-down, not on release";
    EXPECT_EQ((Mutes{false, true, false, false}), muted);
    EXPECT_EQ(Result::None, gestures.release({}));
    EXPECT_EQ(1, writes) << "the release adds nothing to a tap";
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
    // Holding the restoring touch does not turn it into a solo.
    EXPECT_EQ(Result::RestoredAll, gestures.press(3, {}, 2600));
    EXPECT_EQ(Result::None, gestures.hold({}));
    EXPECT_EQ(Result::None, gestures.release({}));
    EXPECT_EQ((Mutes{}), muted);
}

TEST_F(DeckLabStemGesturesTest, HoldSolosWhileHeldAndReleaseRestoresThePreviousState) {
    muted = {false, true, false, false};
    EXPECT_EQ(Result::Toggled, gestures.press(1, {}, 0));
    EXPECT_EQ((Mutes{}), muted) << "touch-down already unmuted the pressed stem";
    EXPECT_EQ(Result::Soloed, gestures.hold({}));
    EXPECT_EQ(1, gestures.soloStem());
    EXPECT_EQ((Mutes{true, false, true, true}), muted) << "the held stem plays alone, even if it was muted";
    EXPECT_EQ(Result::None, gestures.hold({})) << "one solo per press";
    EXPECT_EQ(Result::SoloEnded, gestures.release({}));
    EXPECT_EQ((Mutes{false, true, false, false}), muted) << "the state from before the touch returns";
    EXPECT_FALSE(gestures.active());
    // A playing stem: touch-down mutes it, the hold unmutes it as the solo.
    EXPECT_EQ(Result::Toggled, gestures.press(2, {}, 5000));
    EXPECT_EQ((Mutes{false, true, true, false}), muted);
    gestures.hold({});
    EXPECT_EQ((Mutes{true, true, false, true}), muted);
    EXPECT_EQ(Result::SoloEnded, gestures.release({}));
    EXPECT_EQ((Mutes{false, true, false, false}), muted);
}

TEST_F(DeckLabStemGesturesTest, HoldIsNeverHalfOfADoubleTap) {
    EXPECT_EQ(Result::Toggled, tap(2, 100));
    EXPECT_EQ(Result::Toggled, gestures.press(2, {}, 2000));
    gestures.hold({});
    gestures.release({});
    EXPECT_EQ((Mutes{false, false, true, false}), muted);
    EXPECT_EQ(Result::Toggled, tap(2, 2100)) << "taps around a hold do not restore all";
    EXPECT_EQ((Mutes{}), muted);
}

TEST_F(DeckLabStemGesturesTest, StaleHoldsAndCancelledTouchesNeverSolo) {
    // The touch-down toggle is the action itself; a context change before
    // the hold time only prevents the solo.
    EXPECT_EQ(Result::Toggled, gestures.press(0, {2, 2}, 0));
    EXPECT_EQ((Mutes{true, false, false, false}), muted);
    EXPECT_EQ(Result::Ignored, gestures.hold({3, 2})) << "no solo after a page, layer or layout change";
    EXPECT_EQ(Result::None, gestures.release({3, 2}));
    EXPECT_EQ((Mutes{true, false, false, false}), muted);
    EXPECT_EQ(Result::Toggled, gestures.press(1, {3, 2}, 1000));
    EXPECT_EQ(Result::Ignored, gestures.hold({3, 3})) << "no solo after another track was loaded";
    EXPECT_EQ(Result::Ignored, gestures.cancel(false)) << "a cancel never toggles back";
    EXPECT_EQ(Result::None, gestures.release({3, 3}));
    EXPECT_EQ((Mutes{true, true, false, false}), muted);
    // A cancel also ends a double-tap sequence.
    EXPECT_EQ(Result::Toggled, gestures.press(1, {3, 3}, 1100));
    EXPECT_EQ(Result::Ignored, gestures.cancel(false));
    EXPECT_EQ(Result::Toggled, tap(1, 1200, {3, 3}));
    EXPECT_EQ((Mutes{true, true, false, false}), muted);
}

TEST_F(DeckLabStemGesturesTest, PadsOnlyExistForStemsOnTheTrack) {
    gestures.setCount(2);
    EXPECT_EQ(Result::Ignored, gestures.press(2, {}, 0));
    EXPECT_EQ(Result::Ignored, gestures.press(-1, {}, 0));
    gestures.setCount(0);
    EXPECT_EQ(Result::Ignored, gestures.press(0, {}, 0)) << "no pads without a stem track";
    EXPECT_EQ(0, writes);
    gestures.setCount(4);
    EXPECT_EQ(Result::Toggled, gestures.press(3, {}, 0));
    EXPECT_EQ(Result::Ignored, gestures.press(0, {}, 0)) << "one gesture at a time";
    gestures.setCount(0);
    EXPECT_EQ(Result::Ignored, gestures.hold({})) << "the stems went away mid-hold";
    EXPECT_EQ(Result::None, gestures.release({}));
    EXPECT_EQ(1, writes);
}

TEST_F(DeckLabStemGesturesTest, SoloAlwaysEndsEvenAfterAPageOrLayerChange) {
    muted = {false, false, false, true};
    gestures.press(0, {4, 1}, 0);
    gestures.hold({4, 1});
    EXPECT_EQ((Mutes{false, true, true, true}), muted);
    EXPECT_EQ(Result::SoloEnded, gestures.release({5, 1}));
    EXPECT_EQ((Mutes{false, false, false, true}), muted);
    // A cancel (hidden surface, touch cancel) ends it the same way.
    gestures.press(3, {5, 1}, 1000);
    gestures.hold({5, 1});
    EXPECT_EQ((Mutes{true, true, true, false}), muted);
    EXPECT_EQ(Result::SoloEnded, gestures.cancel(false));
    EXPECT_EQ((Mutes{false, false, false, true}), muted);
    EXPECT_EQ(Result::None, gestures.release({5, 1})) << "the later release is spent";
}

TEST_F(DeckLabStemGesturesTest, TrackLoadedMidHoldFollowsTheEngineStemReset) {
    // Default: Mixxx unmutes every stem when a track loads. Nothing to undo.
    muted = {true, false, false, false};
    gestures.press(0, {1, 1}, 0);
    gestures.hold({1, 1});
    EXPECT_EQ((Mutes{false, true, true, true}), muted);
    muted = {};
    const int before = writes;
    EXPECT_EQ(Result::SoloEnded, gestures.cancel(true));
    EXPECT_EQ(before, writes);
    EXPECT_EQ((Mutes{}), muted) << "the new track must not inherit the old track's mutes";
    // The same when the release arrives after the load.
    muted = {true, false, false, false};
    gestures.press(0, {1, 2}, 1000);
    gestures.hold({1, 2});
    muted = {};
    gestures.release({1, 3});
    EXPECT_EQ((Mutes{}), muted);
    // stem_auto_reset off: the deck kept the solo, so the pre-touch state returns.
    muted = {true, false, false, false};
    gestures.press(0, {1, 3}, 2000);
    gestures.hold({1, 3});
    EXPECT_EQ(Result::SoloEnded, gestures.cancel(true));
    EXPECT_EQ((Mutes{true, false, false, false}), muted);
}

TEST_F(DeckLabStemGesturesTest, MutesChangedElsewhereDuringASoloAreKept) {
    gestures.press(1, {}, 0);
    gestures.hold({});
    EXPECT_EQ((Mutes{true, false, true, true}), muted);
    // A controller unmutes stem 4 and mutes the soloed stem meanwhile.
    muted[3] = false;
    muted[1] = true;
    gestures.release({});
    EXPECT_EQ((Mutes{false, true, false, false}), muted);
}

} // namespace
