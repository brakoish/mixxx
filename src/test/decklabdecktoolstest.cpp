// SPDX-License-Identifier: GPL-2.0-or-later
// r5-decks: the key sheet (decklabkeysheet.h), GRID mode (decklabgridmode.h)
// and the readout's key shift (decklabdeckwidgets.h) against fake hosts.
// Widgets only: no engine, no controls, no audio. The real wiring to
// Mixxx's controls is covered by NativeDeckLabTest.
#include <gtest/gtest.h>

#include <QApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QStringList>
#include <QTest>
#include <memory>

#include "library/decklabknob.h"
#include "widget/decklabdeckwidgets.h"
#include "widget/decklabdecktools.h"
#include "widget/decklabgridmode.h"
#include "widget/decklabkeysheet.h"
#include "widget/decklabpads.h"
#include <QPainter>

namespace {

using KeyAction = DeckLabKeySheet::Action;
using GridAction = DeckLabGridPanel::Action;
using Part = DeckLabStripRenderer::Part;

void press(QPushButton* button) {
    button->setDown(true);
    emit button->pressed();
}
void release(QPushButton* button) {
    button->setDown(false);
    emit button->released();
}

DeckLabKeySheetState keyState(int deck) {
    DeckLabKeySheetState state;
    state.deck = deck;
    state.loaded = true;
    state.hasKey = true;
    state.target = deck == 1 ? 2 : 1;
    state.targetKey = QStringLiteral("3A");
    state.key = QStringLiteral("8A");
    return state;
}

struct FakeKeyHost {
    int deck = 1;
    bool current = true;
    QStringList acts;
    DeckLabKeySheet::Host host() {
        DeckLabKeySheet::Host h;
        h.deck = [this] { return deck; };
        h.current = [this](int) { return current; };
        h.act = [this](int which, KeyAction action) {
            acts << QStringLiteral("%1/%2").arg(which).arg(DeckLabKeySheet::actionName(action));
        };
        return h;
    }
};

class DeckLabKeySheetTest : public ::testing::Test {
  protected:
    void SetUp() override {
        sheet = std::make_unique<DeckLabKeySheet>(QStringLiteral("decklab-key-1"),
                DeckLabKeySheet::Shape::Pads, host.host());
        sheet->resize(329, DeckLabKeySheet::kHeight);
        sheet->setState(keyState(1));
        sheet->show();
        QCoreApplication::processEvents();
    }
    void TearDown() override {
        sheet.reset();
    }
    FakeKeyHost host;
    std::unique_ptr<DeckLabKeySheet> sheet;
};

TEST_F(DeckLabKeySheetTest, KeysFireOnTouchDownWithTheStaleTouchGuards) {
    auto* match = sheet->key(KeyAction::Match);
    ASSERT_NE(nullptr, match);
    EXPECT_EQ(QStringLiteral("decklab-key-1-match"), match->objectName());
    EXPECT_EQ(QStringLiteral("MATCH TO DECK 2 · 3A"), sheet->info()->text());
    press(match);
    EXPECT_EQ(QStringList{QStringLiteral("1/match")}, host.acts) << "touch-down, before any release";
    release(match);
    EXPECT_EQ(1, host.acts.size());
    for (const KeyAction action : {KeyAction::Down, KeyAction::Up, KeyAction::Reset, KeyAction::KeyLock}) {
        press(sheet->key(action));
        release(sheet->key(action));
    }
    EXPECT_EQ((QStringList{QStringLiteral("1/match"), QStringLiteral("1/down"), QStringLiteral("1/up"),
                      QStringLiteral("1/reset"), QStringLiteral("1/keylock")}),
            host.acts);
    // The layer moved under the sheet before it refreshed: nothing.
    host.acts.clear();
    host.deck = 3;
    press(sheet->key(KeyAction::Up));
    EXPECT_TRUE(host.acts.isEmpty());
    host.deck = 1;
    // Another track than the one the sheet shows: nothing.
    host.current = false;
    press(sheet->key(KeyAction::Match));
    EXPECT_TRUE(host.acts.isEmpty());
    // DONE always closes, whatever the deck holds.
    press(sheet->key(KeyAction::Done));
    EXPECT_EQ(QStringList{QStringLiteral("1/done")}, host.acts);
}

TEST_F(DeckLabKeySheetTest, MatchNeedsTwoKeysAndLightsWhileMatched) {
    auto* match = sheet->key(KeyAction::Match);
    EXPECT_TRUE(match->isEnabled());
    EXPECT_FALSE(match->lit());
    auto state = keyState(1);
    state.matched = true;
    state.matchedDeck = 2;
    state.matchedKey = QStringLiteral("3A");
    state.shift = QStringLiteral("+1");
    state.key = QStringLiteral("3A");
    sheet->setState(state);
    EXPECT_TRUE(match->lit());
    EXPECT_EQ(QStringLiteral("MATCHED · DECK 2 3A"), sheet->info()->text());
    EXPECT_TRUE(sheet->info()->styleSheet().contains(QStringLiteral("#2FD27A"))) << "the match green";
    // MATCHED names the deck that was matched, not today's target.
    state.target = 4;
    state.targetKey = QStringLiteral("9B");
    sheet->setState(state);
    EXPECT_EQ(QStringLiteral("MATCHED · DECK 2 3A"), sheet->info()->text());
    EXPECT_TRUE(match->accessibleName().contains(QStringLiteral("deck 2")));
    // No target, or no key there or here: MATCH is off and says why.
    state = keyState(1);
    state.target = 0;
    state.targetKey.clear();
    sheet->setState(state);
    EXPECT_FALSE(match->isEnabled());
    EXPECT_EQ(QStringLiteral("No other deck to match"), sheet->info()->text());
    EXPECT_TRUE(sheet->key(KeyAction::Up)->isEnabled()) << "a shift needs no other deck";
    state = keyState(1);
    state.hasKey = false;
    sheet->setState(state);
    EXPECT_FALSE(match->isEnabled());
    EXPECT_EQ(QStringLiteral("This track has no key"), sheet->info()->text());
    press(match);
    EXPECT_TRUE(host.acts.isEmpty());
    // KEY LOCK is a lamp.
    state = keyState(1);
    state.keylock = true;
    sheet->setState(state);
    EXPECT_TRUE(sheet->key(KeyAction::KeyLock)->lit());
    EXPECT_TRUE(sheet->key(KeyAction::KeyLock)->accessibleName().endsWith(QStringLiteral("on")));
    // Empty deck: only DONE.
    DeckLabKeySheetState empty;
    empty.deck = 1;
    sheet->setState(empty);
    for (const KeyAction action : {KeyAction::Down, KeyAction::Match, KeyAction::Up, KeyAction::Reset, KeyAction::KeyLock}) {
        EXPECT_FALSE(sheet->key(action)->isEnabled()) << DeckLabKeySheet::actionName(action).toStdString();
    }
    EXPECT_TRUE(sheet->key(KeyAction::Done)->isEnabled());
}

TEST_F(DeckLabKeySheetTest, TheKnobShowsItsCursorFirstAndFollowsTheLayout) {
    // The sheet claims the browse knob while it is shown.
    EXPECT_EQ(sheet.get(), decklab::knobClaimant());
    const auto owner = decklab::knobOwner();
    EXPECT_TRUE(owner.claimed);
    // No cursor for touch. A blind press only shows it, on DONE.
    EXPECT_FALSE(sheet->knobCursorShown());
    decklab::forwardKnob(owner, decklab::KnobAction::Press, 1);
    EXPECT_TRUE(host.acts.isEmpty()) << "a blind press never shifts a key";
    EXPECT_TRUE(sheet->knobCursorShown());
    EXPECT_EQ(KeyAction::Done, sheet->knobFocus());
    EXPECT_TRUE(sheet->key(KeyAction::Done)->armed());
    // Reading order: KEY LOCK, DONE, then ‹ −1, MATCH, +1 ›, RESET.
    EXPECT_EQ((QList<KeyAction>{KeyAction::KeyLock, KeyAction::Done, KeyAction::Down, KeyAction::Match,
                      KeyAction::Up, KeyAction::Reset}),
            sheet->knobOrder());
    decklab::forwardKnob(owner, decklab::KnobAction::Move, 2);
    EXPECT_EQ(KeyAction::Match, sheet->knobFocus());
    EXPECT_TRUE(sheet->key(KeyAction::Match)->armed());
    EXPECT_FALSE(sheet->key(KeyAction::Done)->armed());
    decklab::forwardKnob(owner, decklab::KnobAction::Press, 1);
    EXPECT_EQ(QStringList{QStringLiteral("1/match")}, host.acts);
    decklab::forwardKnob(owner, decklab::KnobAction::Move, 9);
    EXPECT_EQ(KeyAction::Reset, sheet->knobFocus()) << "stops at the last key";
    decklab::forwardKnob(owner, decklab::KnobAction::Move, -9);
    EXPECT_EQ(KeyAction::KeyLock, sheet->knobFocus());
    // Back (Shift+press) closes, cursor or not.
    decklab::forwardKnob(owner, decklab::KnobAction::Back, -1);
    EXPECT_EQ(QStringLiteral("1/done"), host.acts.last());
    // Shown again: the cursor hides until the knob is used again.
    sheet->hide();
    EXPECT_EQ(nullptr, decklab::knobClaimant()) << "a hidden sheet never keeps the knob";
    sheet->show();
    EXPECT_FALSE(sheet->knobCursorShown());
    host.acts.clear();
    decklab::forwardKnob(decklab::knobOwner(), decklab::KnobAction::Move, 1);
    EXPECT_EQ(KeyAction::Done, sheet->knobFocus()) << "the first detent shows the cursor, it does not move it";
    EXPECT_TRUE(host.acts.isEmpty());
}

TEST_F(DeckLabKeySheetTest, TheCursorHidesWhenTheKnobGoesElsewhereAndRevealsAgain) {
    const auto send = [this](QEvent::Type type, int key) {
        QKeyEvent event(type, key, Qt::NoModifier);
        QApplication::sendEvent(sheet.get(), &event);
    };
    send(QEvent::KeyPress, Qt::Key_Down); // reveal on DONE
    send(QEvent::KeyPress, Qt::Key_Down); // -1
    send(QEvent::KeyPress, Qt::Key_Down); // MATCH
    ASSERT_EQ(KeyAction::Match, sheet->knobFocus());
    ASSERT_TRUE(sheet->key(KeyAction::Match)->armed());
    // Another owner takes the knob (a menu, a dialog, a later claim): the
    // cursor hides at once.
    QWidget other;
    other.show();
    decklab::claimKnob(&other);
    QCoreApplication::processEvents();
    EXPECT_FALSE(sheet->knobCursorShown());
    EXPECT_FALSE(sheet->key(KeyAction::Match)->armed());
    // The knob comes back: its first press only shows the cursor (on DONE).
    other.hide();
    decklab::releaseKnob(&other);
    QCoreApplication::processEvents();
    ASSERT_EQ(sheet.get(), decklab::knobClaimant());
    send(QEvent::KeyPress, Qt::Key_Return);
    EXPECT_TRUE(host.acts.isEmpty()) << "no MATCH fired on a returning knob";
    EXPECT_TRUE(sheet->knobCursorShown());
    EXPECT_EQ(KeyAction::Done, sheet->knobFocus());
}

TEST_F(DeckLabKeySheetTest, AHeldKnobPressActsOnReleaseAndAPushTurnDoesNothing) {
    const auto send = [this](QEvent::Type type, int key, int times = 1) {
        for (int n = 0; n < times; ++n) {
            QKeyEvent event(type, key, Qt::NoModifier);
            QApplication::sendEvent(sheet.get(), &event);
        }
    };
    send(QEvent::KeyPress, Qt::Key_Down); // reveal on DONE
    send(QEvent::KeyPress, Qt::Key_Down, 2);
    ASSERT_EQ(KeyAction::Match, sheet->knobFocus());
    // The Numark overlay: browse_hold (Key_Select), then the press at once,
    // then turns eight times over while held, then the release.
    send(QEvent::KeyPress, Qt::Key_Select);
    send(QEvent::KeyPress, Qt::Key_Return);
    EXPECT_TRUE(host.acts.isEmpty()) << "a held press waits for its release";
    send(QEvent::KeyPress, Qt::Key_Down, 8);
    send(QEvent::KeyRelease, Qt::Key_Select);
    EXPECT_TRUE(host.acts.isEmpty()) << "a push-turn drops the press";
    EXPECT_EQ(KeyAction::Match, sheet->knobFocus()) << "and moves nothing";
    // Held and released without a turn: the press acts on the release.
    send(QEvent::KeyPress, Qt::Key_Select);
    send(QEvent::KeyPress, Qt::Key_Return);
    send(QEvent::KeyRelease, Qt::Key_Select);
    EXPECT_EQ(QStringList{QStringLiteral("1/match")}, host.acts);
}

TEST_F(DeckLabKeySheetTest, TargetsFitTheDeckAreaInBothShapes) {
    // Two decks: in the pad block's area (329 px beside SYNC / MORE).
    for (const KeyAction action : {KeyAction::Down, KeyAction::Match, KeyAction::Up, KeyAction::Reset,
                 KeyAction::KeyLock, KeyAction::Done}) {
        auto* key = sheet->key(action);
        EXPECT_EQ(44, key->height()) << DeckLabKeySheet::actionName(action).toStdString();
        EXPECT_GE(key->width(), 44) << DeckLabKeySheet::actionName(action).toStdString();
        EXPECT_TRUE(sheet->rect().contains(key->geometry()));
        EXPECT_GE(QFontInfo(key->font()).pixelSize(), 16);
    }
    EXPECT_GE(sheet->info()->width(), 150) << "the info line keeps room for MATCH TO DECK 2 · 12A";
    // Four decks: a card (about 191 px inside its margins), a short info
    // naming MATCH's deck or why it is off.
    FakeKeyHost cardHost;
    DeckLabKeySheet card(QStringLiteral("decklab-card-key-1"), DeckLabKeySheet::Shape::Card, cardHost.host());
    card.resize(191, DeckLabKeySheet::kHeight);
    card.setState(keyState(1));
    card.show();
    QCoreApplication::processEvents();
    EXPECT_EQ(nullptr, card.key(KeyAction::KeyLock)) << "KEY LOCK stays in MORE";
    ASSERT_NE(nullptr, card.info());
    EXPECT_EQ(QStringLiteral("TO 2 · 3A"), card.info()->text());
    for (const KeyAction action : {KeyAction::Down, KeyAction::Match, KeyAction::Up, KeyAction::Reset, KeyAction::Done}) {
        auto* key = card.key(action);
        EXPECT_EQ(44, key->height());
        EXPECT_GE(key->width(), 44);
        EXPECT_GE(key->width() - 4, QFontMetrics(key->font()).horizontalAdvance(key->text()))
                << key->text().toStdString() << " fits its key";
    }
    auto matched = keyState(1);
    matched.matched = true;
    matched.matchedDeck = 2;
    matched.matchedKey = QStringLiteral("3A");
    auto noKey = keyState(1);
    noKey.hasKey = false;
    auto noDeck = keyState(1);
    noDeck.target = 0;
    noDeck.targetKey.clear();
    auto wide = keyState(1);
    wide.targetKey = QStringLiteral("12A");
    const QList<std::pair<DeckLabKeySheetState, QString>> cases{{matched, QStringLiteral("MATCHED 2")},
            {noKey, QStringLiteral("NO KEY")}, {noDeck, QStringLiteral("NO DECK")}, {wide, QStringLiteral("TO 2 · 12A")}};
    for (const auto& [state, words] : cases) {
        card.setState(state);
        EXPECT_EQ(words, card.info()->text());
        EXPECT_GE(card.info()->width(), QFontMetrics(card.info()->font()).horizontalAdvance(words)) << words.toStdString();
    }
    EXPECT_EQ(92, DeckLabKeySheet::kHeight) << "the overview (44) and transport (48) rows";
}

TEST_F(DeckLabKeySheetTest, DocksInPlaceOfThePadsBesideSyncAndMore) {
    // The two-deck sheet is the pad block's panel: the mode keys and pads
    // give way, the SYNC / MORE column stays, a held roll ends.
    QStringList writes;
    DeckLabPadBlock::Host padHost;
    padHost.deck = [] { return 1; };
    padHost.epoch = [] { return quint64(1); };
    padHost.current = [](int) { return true; };
    padHost.read = [](int, const QString&) { return -1.0; };
    padHost.write = [&writes](int deck, const QString& item, double value) {
        writes << QStringLiteral("%1/%2=%3").arg(deck).arg(item).arg(value);
    };
    DeckLabPadBlock block(QStringLiteral("decklab-pads-1"), QStringLiteral("decklab-hotcue-1-"), padHost);
    auto* column = new QWidget;
    column->setObjectName(QStringLiteral("column"));
    block.setSideColumn(column);
    FakeKeyHost dockHost;
    auto* docked = new DeckLabKeySheet(QStringLiteral("decklab-key-1"), DeckLabKeySheet::Shape::Pads, dockHost.host());
    block.setPanel(docked);
    block.resize(391, DeckLabPadBlock::kBlockHeight);
    DeckLabPadDeckState state;
    state.deck = 1;
    state.loaded = true;
    block.setDeckState(state);
    block.show();
    QCoreApplication::processEvents();
    EXPECT_FALSE(docked->isVisible());
    // A roll is held when the sheet opens: it ends at once.
    block.setMode(DeckLabPadBlock::Mode::Roll);
    press(block.pad(2));
    ASSERT_TRUE(block.holding());
    block.showPanel(true);
    QCoreApplication::processEvents();
    EXPECT_FALSE(block.holding());
    EXPECT_EQ(QStringLiteral("1/beatlooproll_0.25_activate=0"), writes.last());
    EXPECT_TRUE(docked->isVisible());
    EXPECT_FALSE(block.pad(0)->isVisible());
    EXPECT_FALSE(block.modeKey(DeckLabPadBlock::Mode::Cue)->isVisible());
    EXPECT_TRUE(column->isVisible()) << "SYNC and MORE stay";
    EXPECT_EQ(DeckLabKeySheet::kHeight, docked->height());
    EXPECT_LE(docked->geometry().right(), column->geometry().left());
    // Closing brings the pads back in the deck's mode.
    block.showPanel(false);
    QCoreApplication::processEvents();
    EXPECT_FALSE(docked->isVisible());
    EXPECT_TRUE(block.pad(0)->isVisible());
    EXPECT_EQ(DeckLabPadBlock::Mode::Roll, block.mode());
}

// ---------------------------------------------------------------- GRID mode

DeckLabGridState gridState(int deck) {
    DeckLabGridState state;
    state.deck = deck;
    state.loaded = true;
    state.hasBeats = true;
    state.constantTempo = true;
    state.canHalve = true;
    state.bpm = QStringLiteral("128.00");
    state.beat = 0;
    return state;
}

struct FakeGridHost {
    bool current = true;
    QStringList acts;
    DeckLabGridPanel::Host host() {
        DeckLabGridPanel::Host h;
        h.current = [this](int) { return current; };
        h.act = [this](int which, GridAction action) {
            acts << QStringLiteral("%1/%2").arg(which).arg(DeckLabGridPanel::actionName(action));
        };
        return h;
    }
};

class DeckLabGridModeTest : public ::testing::Test {
  protected:
    void SetUp() override {
        panel = std::make_unique<DeckLabGridPanel>(QStringLiteral("decklab-grid-mode"), host.host());
        panel->resize(396, 230);
        panel->setState(gridState(1));
        panel->show();
        QCoreApplication::processEvents();
    }
    void TearDown() override {
        panel.reset();
    }
    FakeGridHost host;
    std::unique_ptr<DeckLabGridPanel> panel;
};

TEST_F(DeckLabGridModeTest, EveryKeyDrivesItsGridControlOnTouchDown) {
    const QList<std::pair<GridAction, QString>> keys{
            {GridAction::NudgeEarlier, QStringLiteral("beats_translate_earlier")},
            {GridAction::NudgeLater, QStringLiteral("beats_translate_later")},
            {GridAction::BeatEarlier, QStringLiteral("beat-earlier")},
            {GridAction::BeatLater, QStringLiteral("beat-later")},
            {GridAction::BpmDownCoarse, QStringLiteral("bpm-down-coarse")},
            {GridAction::BpmDownFine, QStringLiteral("beats_adjust_slower")},
            {GridAction::BpmUpFine, QStringLiteral("beats_adjust_faster")},
            {GridAction::BpmUpCoarse, QStringLiteral("bpm-up-coarse")},
            {GridAction::Halve, QStringLiteral("beats_set_halve")},
            {GridAction::Double, QStringLiteral("beats_set_double")},
            {GridAction::Tap, QStringLiteral("bpm_tap")},
            {GridAction::Downbeat, QStringLiteral("downbeat")},
            {GridAction::Lock, QStringLiteral("bpmlock")},
            {GridAction::Done, QStringLiteral("done")}};
    QStringList expected;
    for (const auto& [action, name] : keys) {
        auto* key = panel->key(action);
        ASSERT_NE(nullptr, key) << name.toStdString();
        EXPECT_EQ(QString(QStringLiteral("decklab-grid-mode-") + name), key->objectName());
        EXPECT_FALSE(key->accessibleName().isEmpty()) << name.toStdString();
        press(key);
        expected << QString(QStringLiteral("1/") + name);
        EXPECT_EQ(expected, host.acts) << "on touch-down: " << name.toStdString();
        release(key);
    }
    EXPECT_EQ(expected, host.acts) << "releases add nothing";
}

TEST_F(DeckLabGridModeTest, EditsWaitForAnUnlockedQuietGridAndSayWhy) {
    EXPECT_TRUE(panel->editable());
    EXPECT_TRUE(panel->status().startsWith(QStringLiteral("KNOB  NUDGE")));
    struct Case {
        const char* what;
        std::function<void(DeckLabGridState&)> change;
        const char* status;
    };
    const QList<Case> cases{
            {"locked", [](DeckLabGridState& s) { s.locked = true; }, "Grid locked. Touch LOCK to edit."},
            {"synced", [](DeckLabGridState& s) { s.synced = true; }, "SYNC is on. DONE, then turn SYNC off."},
            {"shared", [](DeckLabGridState& s) { s.sharedDeck = 2; },
                    "Deck 2 plays or syncs it too. DONE, then stop it."},
            {"no grid", [](DeckLabGridState& s) { s.hasBeats = false; }, "No beat grid yet. Analyse the track first."}};
    for (const auto& c : cases) {
        auto state = gridState(1);
        c.change(state);
        panel->setState(state);
        EXPECT_FALSE(panel->editable()) << c.what;
        EXPECT_EQ(QString::fromUtf8(c.status), panel->status()) << c.what;
        host.acts.clear();
        for (const GridAction action : {GridAction::NudgeLater, GridAction::Double, GridAction::Tap,
                     GridAction::Downbeat, GridAction::BpmUpFine}) {
            EXPECT_FALSE(panel->key(action)->isEnabled()) << c.what;
            press(panel->key(action));
        }
        EXPECT_TRUE(host.acts.isEmpty()) << c.what;
        EXPECT_TRUE(panel->key(GridAction::Done)->isEnabled()) << c.what;
    }
    // A locked grid can be unlocked; a busy alias cannot be touched at all.
    auto locked = gridState(1);
    locked.locked = true;
    panel->setState(locked);
    EXPECT_TRUE(panel->key(GridAction::Lock)->isEnabled());
    EXPECT_TRUE(panel->key(GridAction::Lock)->lit());
    auto shared = gridState(1);
    shared.sharedDeck = 3;
    panel->setState(shared);
    EXPECT_FALSE(panel->key(GridAction::Lock)->isEnabled());
    // Whole-beat moves and DOWNBEAT need a constant-tempo grid (Mixxx's
    // rule; rekordbox imports are marker grids), and the status says so.
    auto variable = gridState(1);
    variable.constantTempo = false;
    variable.canHalve = false;
    panel->setState(variable);
    EXPECT_FALSE(panel->key(GridAction::BeatEarlier)->isEnabled());
    EXPECT_FALSE(panel->key(GridAction::Downbeat)->isEnabled());
    EXPECT_FALSE(panel->key(GridAction::Halve)->isEnabled()) << "where Mixxx cannot halve it";
    EXPECT_TRUE(panel->key(GridAction::Double)->isEnabled());
    EXPECT_TRUE(panel->key(GridAction::NudgeEarlier)->isEnabled());
    EXPECT_TRUE(panel->status().startsWith(QStringLiteral("Variable-tempo grid")));
    // Another track than the one GRID mode opened for: nothing.
    panel->setState(gridState(1));
    host.acts.clear();
    host.current = false;
    press(panel->key(GridAction::NudgeLater));
    EXPECT_TRUE(host.acts.isEmpty());
}

TEST_F(DeckLabGridModeTest, APlayingDeckSaysEditingGridInTheDeckColour) {
    EXPECT_EQ(QStringLiteral("PAUSED"), panel->playText());
    auto playing = gridState(2);
    playing.playing = true;
    panel->setState(playing);
    EXPECT_EQ(QStringLiteral("PLAYING"), panel->playText());
    EXPECT_TRUE(panel->editable()) << "DJs fix grids live";
    const QImage image = panel->grab().toImage();
    // The deck colour along the top and filling the EDITING GRID chip.
    EXPECT_EQ(deckLabDeckColor(2), image.pixelColor(panel->width() / 2, 1));
    const QWidget* title = panel->findChild<QWidget*>(QStringLiteral("decklab-grid-mode-title"));
    ASSERT_NE(nullptr, title);
    int deckPixels = 0;
    for (int x = title->x() + 36; x < title->x() + 130; ++x) {
        deckPixels += image.pixelColor(x, title->y() + title->height() / 2 - 8) == deckLabDeckColor(2);
    }
    EXPECT_GT(deckPixels, 20) << "a filled deck-colour chip";
    EXPECT_TRUE(panel->accessibleDescription().contains(QStringLiteral("playing")));
}

TEST_F(DeckLabGridModeTest, TheKnobNudgesSwitchesToBpmAndLeaves) {
    EXPECT_EQ(panel.get(), decklab::knobClaimant());
    const auto owner = decklab::knobOwner();
    ASSERT_TRUE(owner.claimed);
    decklab::forwardKnob(owner, decklab::KnobAction::Move, 2);
    decklab::forwardKnob(owner, decklab::KnobAction::Move, -1);
    EXPECT_EQ((QStringList{QStringLiteral("1/beats_translate_later"), QStringLiteral("1/beats_translate_later"),
                      QStringLiteral("1/beats_translate_earlier")}),
            host.acts);
    // The keys the knob stands for carry its lamp.
    EXPECT_TRUE(panel->key(GridAction::NudgeLater)->armed());
    EXPECT_FALSE(panel->key(GridAction::BpmUpFine)->armed());
    // Press: the knob adjusts the BPM by 0.01.
    decklab::forwardKnob(owner, decklab::KnobAction::Press, 1);
    EXPECT_EQ(DeckLabGridPanel::Knob::Bpm, panel->knob());
    EXPECT_TRUE(panel->key(GridAction::BpmUpFine)->armed());
    EXPECT_FALSE(panel->key(GridAction::NudgeLater)->armed());
    EXPECT_TRUE(panel->status().startsWith(QStringLiteral("KNOB  BPM")));
    host.acts.clear();
    decklab::forwardKnob(owner, decklab::KnobAction::Move, 1);
    decklab::forwardKnob(owner, decklab::KnobAction::Move, -1);
    EXPECT_EQ((QStringList{QStringLiteral("1/beats_adjust_faster"), QStringLiteral("1/beats_adjust_slower")}), host.acts);
    // Shift+press (back): DONE.
    decklab::forwardKnob(owner, decklab::KnobAction::Back, -1);
    EXPECT_EQ(QStringLiteral("1/done"), host.acts.last());
    // A locked grid: the knob changes nothing.
    auto locked = gridState(1);
    locked.locked = true;
    panel->setState(locked);
    host.acts.clear();
    decklab::forwardKnob(decklab::knobOwner(), decklab::KnobAction::Move, 3);
    EXPECT_TRUE(host.acts.isEmpty());
}

TEST_F(DeckLabGridModeTest, ControlsFitTheDeckAreaAtBothLayouts) {
    for (const QSize size : {QSize(396, 244), QSize(397, 230)}) {
        panel->resize(size);
        QCoreApplication::processEvents();
        EXPECT_LE(panel->minimumHeight(), size.height());
        for (int index = 0; index < static_cast<int>(GridAction::Count); ++index) {
            auto* key = panel->key(static_cast<GridAction>(index));
            EXPECT_EQ(44, key->height()) << key->objectName().toStdString();
            EXPECT_GE(key->width(), 44) << key->objectName().toStdString();
            EXPECT_TRUE(panel->rect().contains(key->geometry())) << key->objectName().toStdString();
            EXPECT_GE(key->width() - 4, QFontMetrics(key->font()).horizontalAdvance(key->text()))
                    << key->text().toStdString() << " fits its key";
            EXPECT_GE(QFontInfo(key->font()).pixelSize(), 16);
        }
        // 4 px gaps between keys on a row.
        EXPECT_GE(panel->key(GridAction::NudgeLater)->x() -
                        (panel->key(GridAction::NudgeEarlier)->x() + panel->key(GridAction::NudgeEarlier)->width()),
                4);
    }
}

TEST_F(DeckLabGridModeTest, AnUnchangedStateRepaintsNothingAndAChangeOnlyItsPart) {
    struct PaintRegions : QObject {
        QRegion region;
        int count = 0;
        bool eventFilter(QObject*, QEvent* event) override {
            if (event->type() == QEvent::Paint) {
                ++count;
                region += static_cast<QPaintEvent*>(event)->region();
            }
            return false;
        }
    } paints;
    panel->grab();
    QCoreApplication::processEvents();
    panel->installEventFilter(&paints);
    panel->setState(gridState(1));
    QCoreApplication::processEvents();
    EXPECT_EQ(0, paints.count) << "an unchanged state repaints nothing";
    // A new beat repaints the readout, and only the readout.
    const QWidget* readout = panel->findChild<QWidget*>(QStringLiteral("decklab-grid-mode-readout"));
    const QWidget* title = panel->findChild<QWidget*>(QStringLiteral("decklab-grid-mode-title"));
    ASSERT_NE(nullptr, readout);
    ASSERT_NE(nullptr, title);
    auto next = gridState(1);
    next.beat = 1;
    panel->setState(next);
    QCoreApplication::processEvents();
    EXPECT_FALSE(paints.region.isEmpty()) << "the beat count moves";
    EXPECT_TRUE(QRegion(readout->geometry()).contains(paints.region.boundingRect())) << "never the whole panel";
    // PLAYING repaints the header's title, and only it.
    paints.region = QRegion();
    next.playing = true;
    panel->setState(next);
    QCoreApplication::processEvents();
    EXPECT_FALSE(paints.region.isEmpty());
    EXPECT_TRUE(QRegion(title->geometry()).contains(paints.region.boundingRect()));
    panel->removeEventFilter(&paints);
}

TEST_F(DeckLabGridModeTest, KnobLampsAndLegendFollowTheRealKnobOwner) {
    EXPECT_TRUE(panel->ownsKnob());
    EXPECT_TRUE(panel->key(GridAction::NudgeLater)->armed());
    EXPECT_TRUE(panel->status().startsWith(QStringLiteral("KNOB")));
    // A later claim takes the knob: no lamps, no legend, with no deck
    // refresh in between (the claim itself tells the panel).
    QWidget other;
    other.show();
    decklab::claimKnob(&other);
    QCoreApplication::processEvents();
    EXPECT_FALSE(panel->ownsKnob());
    EXPECT_FALSE(panel->key(GridAction::NudgeLater)->armed());
    EXPECT_FALSE(panel->status().startsWith(QStringLiteral("KNOB")));
    other.hide();
    decklab::releaseKnob(&other);
    QCoreApplication::processEvents();
    EXPECT_TRUE(panel->ownsKnob());
    EXPECT_TRUE(panel->key(GridAction::NudgeLater)->armed());
    EXPECT_TRUE(panel->status().startsWith(QStringLiteral("KNOB")));
}

TEST_F(DeckLabGridModeTest, AHeldKnobPressActsOnReleaseAndAPushTurnEditsNothing) {
    const auto send = [this](QEvent::Type type, int key, int times = 1) {
        for (int n = 0; n < times; ++n) {
            QKeyEvent event(type, key, Qt::NoModifier);
            QApplication::sendEvent(panel.get(), &event);
        }
    };
    send(QEvent::KeyPress, Qt::Key_Select);
    send(QEvent::KeyPress, Qt::Key_Return);
    send(QEvent::KeyPress, Qt::Key_Down, 8);
    send(QEvent::KeyRelease, Qt::Key_Select);
    EXPECT_TRUE(host.acts.isEmpty()) << "a push-turn edits nothing";
    EXPECT_EQ(DeckLabGridPanel::Knob::Nudge, panel->knob()) << "and the press is dropped";
    send(QEvent::KeyPress, Qt::Key_Select);
    send(QEvent::KeyPress, Qt::Key_Return);
    EXPECT_EQ(DeckLabGridPanel::Knob::Nudge, panel->knob()) << "a held press waits for its release";
    send(QEvent::KeyRelease, Qt::Key_Select);
    EXPECT_EQ(DeckLabGridPanel::Knob::Bpm, panel->knob());
    // Plain turns still adjust.
    send(QEvent::KeyPress, Qt::Key_Down);
    EXPECT_EQ(QStringList{QStringLiteral("1/beats_adjust_faster")}, host.acts);
}

// ------------------------------------------------------- the key readout

DeckLabStripState loadedStrip(int deck) {
    DeckLabStripState s;
    s.deck = deck;
    s.loaded = true;
    s.title = QStringLiteral("Generated Tone");
    s.artist = QStringLiteral("Deck Lab Fixture");
    s.bpm = QStringLiteral("128.0");
    s.tempo = QStringLiteral("+0.00%");
    s.tempoCentered = true;
    s.range = QStringLiteral("±8%");
    s.key = QStringLiteral("12A");
    s.keyColor = QColor(0x35, 0xc8, 0xf0);
    s.remaining = QStringLiteral("−3:20");
    s.quantize = true;
    return s;
}

TEST(DeckLabKeyReadoutTest, ShiftTextIsMixxxSemitones) {
    EXPECT_EQ(QString(), DeckLabDeckTools::shiftText(0));
    EXPECT_EQ(QString(), DeckLabDeckTools::shiftText(0.03));
    EXPECT_EQ(QStringLiteral("+1"), DeckLabDeckTools::shiftText(1));
    EXPECT_EQ(QStringLiteral("+1"), DeckLabDeckTools::shiftText(0.98));
    EXPECT_EQ(QString::fromUtf8("−2"), DeckLabDeckTools::shiftText(-2));
    EXPECT_EQ(QStringLiteral("+0.5"), DeckLabDeckTools::shiftText(0.5));
    EXPECT_EQ(QString::fromUtf8("−1.3"), DeckLabDeckTools::shiftText(-1.33));
}

TEST(DeckLabKeyReadoutTest, KeyAndShiftFitEveryReadoutWithoutTakingTheTempo) {
    // "12A +1", "F#m −0.5" and every Traditional name ("D♯m/E♭m" gives
    // way to "E♭m"): the key and its shift always fit; the tempo keeps its
    // OVER outline at ±18.82% in the two-deck readout.
    // Mixxx's Traditional names (src/track/keyutils.cpp), all 24.
    QStringList keys{QStringLiteral("12A"), QStringLiteral("F#m"), QStringLiteral("10m")};
    keys << QStringList{QStringLiteral(u"C"), QStringLiteral(u"D♭"), QStringLiteral(u"D"), QStringLiteral(u"E♭"),
            QStringLiteral(u"E"), QStringLiteral(u"F"), QStringLiteral(u"F♯/G♭"), QStringLiteral(u"G"),
            QStringLiteral(u"A♭"), QStringLiteral(u"A"), QStringLiteral(u"B♭"), QStringLiteral(u"B"),
            QStringLiteral(u"Cm"), QStringLiteral(u"C♯m"), QStringLiteral(u"Dm"), QStringLiteral(u"D♯m/E♭m"),
            QStringLiteral(u"Em"), QStringLiteral(u"Fm"), QStringLiteral(u"F♯m"), QStringLiteral(u"Gm"),
            QStringLiteral(u"G♯m"), QStringLiteral(u"Am"), QStringLiteral(u"B♭m"), QStringLiteral(u"Bm")};
    for (const auto mode : {DeckLabStripRenderer::Mode::Full, DeckLabStripRenderer::Mode::Compact}) {
        const bool full = mode == DeckLabStripRenderer::Mode::Full;
        for (const int width : full ? QList<int>{399, 400, 512, 640} : QList<int>{196, 199, 255}) {
            for (const QString& key : std::as_const(keys)) {
                for (const char* shift : {"", "+1", "−0.5", "+0.7", "−6"}) {
                    DeckLabStripRenderer renderer;
                    renderer.setMode(mode);
                    renderer.setSize(QSize(width, DeckLabStripRenderer::preferredHeight(mode)));
                    auto s = loadedStrip(1);
                    s.key = key;
                    s.keyShift = QString::fromUtf8(shift);
                    s.keyMatch = 1;
                    s.tempo = QString::fromUtf8("−18.82%");
                    s.tempoCentered = false;
                    s.overRange = true;
                    renderer.setState(s);
                    EXPECT_TRUE(renderer.overflowingParts().isEmpty())
                            << width << " " << key.toStdString() << " " << shift << ": "
                            << renderer.overflowingParts().join(',').toStdString();
                    EXPECT_GE(renderer.keyText().shiftPixels, DeckLabStripRenderer::kLabel);
                    EXPECT_GE(renderer.keyTarget().height(), 44);
                    EXPECT_GE(renderer.keyTarget().width(), 44);
                    EXPECT_GE(renderer.bpmTarget().height(), 44);
                    EXPECT_GE(renderer.bpmTarget().width(), 44);
                    EXPECT_FALSE(renderer.keyTarget().intersects(renderer.bpmTarget()));
                    EXPECT_FALSE(renderer.keyTarget().intersects(renderer.rect(Part::Time)));
                }
            }
        }
    }
}

TEST(DeckLabKeyReadoutTest, MatchedKeyTurnsGreenOutlinedAndRepaintsOnlyTheKey) {
    DeckLabDeckStrip strip(1);
    strip.resize(400, 104);
    auto s = loadedStrip(1);
    s.key = QStringLiteral("3A");
    strip.setState(s);
    strip.grab();
    const QRect key = strip.renderer().rect(Part::Key);
    const QColor green(0x2f, 0xd2, 0x7a);
    const auto countIn = [&strip](const QRect& area, const QColor& color) {
        const QImage image = strip.grab().toImage();
        int count = 0;
        for (int y = area.top(); y <= area.bottom(); ++y)
            for (int x = area.left(); x <= area.right(); ++x) count += image.pixelColor(x, y) == color;
        return count;
    };
    EXPECT_EQ(0, countIn(key, green));
    DeckLabStripRenderer renderer;
    renderer.setSize(QSize(400, 104));
    renderer.setState(s);
    auto matched = s;
    matched.keyShift = QStringLiteral("+1");
    matched.keyMatched = true;
    const QRegion dirty = renderer.setState(matched);
    EXPECT_TRUE(dirty.intersects(key));
    EXPECT_FALSE(dirty.intersects(renderer.rect(Part::Tempo)));
    EXPECT_FALSE(dirty.intersects(renderer.rect(Part::Time)));
    EXPECT_FALSE(dirty.intersects(renderer.rect(Part::Chips)));
    strip.setState(matched);
    EXPECT_GT(countIn(key.adjusted(-4, 0, 0, 0), green), 40) << "green key, shift and outline";
    // The outline: green on the box's left edge, left of the key text.
    const QImage image = strip.grab().toImage();
    bool edge = false;
    for (int y = key.top(); y <= key.bottom(); ++y) {
        edge |= image.pixelColor(key.left() - 2, y) == green || image.pixelColor(key.left() - 1, y) == green;
    }
    EXPECT_TRUE(edge);
    EXPECT_TRUE(strip.renderer().overflowingParts().isEmpty());
    // Painted only where the Key part repaints: drawing the matched state,
    // then only the dirty region of the next state (as the widget does),
    // leaves no green behind, also with a long clock and a wide shift.
    for (const char* clock : {"−3:20", "−120:00"}) {
        for (const char* shift : {"+1", "−0.5"}) {
            DeckLabStripRenderer partial;
            partial.setSize(QSize(400, 104));
            auto on = s;
            on.remaining = QString::fromUtf8(clock);
            on.keyShift = QString::fromUtf8(shift);
            on.keyMatched = true;
            partial.setState(on);
            QImage canvas(400, 104, QImage::Format_RGB32);
            {
                QPainter painter(&canvas);
                partial.paint(painter, canvas.rect(), false, false);
            }
            auto off = on;
            off.keyMatched = false;
            off.keyShift.clear();
            const QRegion region = partial.setState(off);
            {
                QPainter painter(&canvas);
                painter.setClipRegion(region);
                partial.paint(painter, region.boundingRect(), false, false);
            }
            int left = 0;
            for (int y = 0; y < canvas.height(); ++y)
                for (int x = 0; x < canvas.width(); ++x) left += canvas.pixelColor(x, y) == green;
            EXPECT_EQ(0, left) << clock << " " << shift << ": the outline is repainted away";
        }
    }
}

TEST(DeckLabKeyReadoutTest, KeyTouchOpensTheSheetAndBpmHoldOpensGrid) {
    QWidget host;
    auto* strip = new DeckLabDeckStrip(2, &host);
    strip->resize(400, 104);
    strip->setState(loadedStrip(2));
    host.show();
    QCoreApplication::processEvents();
    int keys = 0;
    int grids = 0;
    int clicks = 0;
    QObject::connect(strip, &DeckLabDeckStrip::keyTouched, &host, [&keys](int deck) { keys += deck == 2; });
    QObject::connect(strip, &DeckLabDeckStrip::bpmHeld, &host, [&grids](int deck) { grids += deck == 2; });
    QObject::connect(strip, &QPushButton::clicked, &host, [&clicks] { ++clicks; });
    const auto mouse = [strip](QEvent::Type type, const QPoint& at) {
        QMouseEvent event(type, at, strip->mapToGlobal(at), Qt::LeftButton,
                type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(strip, &event);
    };
    // KEY: on touch-down, and the readout never clicks.
    const QPoint key = strip->renderer().keyTarget().center();
    mouse(QEvent::MouseButtonPress, key);
    EXPECT_EQ(1, keys);
    EXPECT_FALSE(strip->isDown());
    mouse(QEvent::MouseButtonRelease, key);
    EXPECT_EQ(0, clicks);
    // A short tap on the BPM is still the readout's tap.
    const QPoint bpm = strip->renderer().bpmTarget().center();
    mouse(QEvent::MouseButtonPress, bpm);
    mouse(QEvent::MouseButtonRelease, bpm);
    EXPECT_EQ(1, clicks);
    EXPECT_EQ(0, grids);
    // Held: GRID, and the release no longer clicks.
    mouse(QEvent::MouseButtonPress, bpm);
    QTest::qWait(DeckLabDeckStrip::kGridHoldMs + 150);
    EXPECT_EQ(1, grids);
    mouse(QEvent::MouseButtonRelease, bpm);
    EXPECT_EQ(1, clicks);
    // A playing deck: a brush on KEY does nothing (round 4's stray-thumb
    // rule), a short hold opens the sheet; holding its BPM never opens GRID
    // (MORE › GRID is the way in for a live deck).
    auto live = loadedStrip(2);
    live.playing = true;
    strip->setState(live);
    mouse(QEvent::MouseButtonPress, key);
    EXPECT_EQ(1, keys) << "not on touch-down";
    mouse(QEvent::MouseButtonRelease, key);
    QTest::qWait(DeckLabDeckStrip::kLiveKeyHoldMs + 100);
    EXPECT_EQ(1, keys) << "a brush opens nothing";
    EXPECT_EQ(1, clicks);
    mouse(QEvent::MouseButtonPress, key);
    QTest::qWait(DeckLabDeckStrip::kLiveKeyHoldMs + 100);
    EXPECT_EQ(2, keys) << "a deliberate hold opens it";
    mouse(QEvent::MouseButtonRelease, key);
    EXPECT_EQ(1, clicks);
    mouse(QEvent::MouseButtonPress, bpm);
    QTest::qWait(DeckLabDeckStrip::kGridHoldMs + 150);
    mouse(QEvent::MouseButtonRelease, bpm);
    EXPECT_EQ(1, grids) << "no GRID from a live deck's readout";
    EXPECT_EQ(2, clicks) << "the readout's own tap (the surface ignores it while the deck plays, round 4)";
    keys = 1;
    clicks = 1;
    // The hold shows on the key while it counts, and goes when it ends.
    const QRect zone = strip->renderer().keyTarget();
    const auto outlined = [strip, &zone] {
        const QImage image = strip->grab().toImage();
        int count = 0;
        for (int y = zone.top() + 4; y <= zone.bottom() - 4; ++y) {
            bool light = false;
            for (int x = zone.left(); x <= zone.left() + 2; ++x) {
                const QColor pixel = image.pixelColor(x, y);
                light |= pixel.red() > 160 && pixel.green() > 160 && pixel.blue() > 160;
            }
            count += light;
        }
        return count;
    };
    EXPECT_EQ(0, outlined());
    int starts = 0;
    QObject::connect(strip, &DeckLabDeckStrip::keyHoldStarted, &host, [&starts](int) { ++starts; });
    mouse(QEvent::MouseButtonPress, key);
    EXPECT_EQ(1, starts);
    EXPECT_GT(outlined(), 10) << "visible feedback during the hold";
    // Sliding off the key cancels it.
    mouse(QEvent::MouseMove, QPoint(key.x(), 2));
    EXPECT_EQ(0, outlined());
    QTest::qWait(DeckLabDeckStrip::kLiveKeyHoldMs + 100);
    EXPECT_EQ(1, keys) << "a slide-off opens nothing";
    mouse(QEvent::MouseButtonRelease, QPoint(key.x(), 2));
    // Hidden mid-hold (a page or layer change), even if shown again before
    // the hold would end: nothing fires.
    mouse(QEvent::MouseButtonPress, key);
    strip->hide();
    strip->show();
    QTest::qWait(DeckLabDeckStrip::kLiveKeyHoldMs + 100);
    EXPECT_EQ(1, keys) << "a hidden strip's hold is gone";
    mouse(QEvent::MouseButtonRelease, key);
    // A paused deck's BPM hold that turns into a drag opens nothing.
    strip->setState(loadedStrip(2));
    mouse(QEvent::MouseButtonPress, bpm);
    mouse(QEvent::MouseMove, bpm + QPoint(30, 0));
    QTest::qWait(DeckLabDeckStrip::kGridHoldMs + 150);
    EXPECT_EQ(1, grids) << "a drag is not a hold";
    mouse(QEvent::MouseButtonRelease, bpm + QPoint(30, 0));
    clicks = 1;
    // An empty deck has no zones: the whole readout opens Browse.
    DeckLabStripState empty;
    empty.deck = 2;
    strip->setState(empty);
    EXPECT_TRUE(strip->renderer().keyTarget().isEmpty());
    mouse(QEvent::MouseButtonPress, key);
    mouse(QEvent::MouseButtonRelease, key);
    EXPECT_EQ(1, keys);
    EXPECT_EQ(2, clicks);
}

} // namespace
