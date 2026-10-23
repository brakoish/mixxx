// SPDX-License-Identifier: GPL-2.0-or-later
// r4-decks: the pad block (decklabpads.h) against a fake host, and the deck
// readout's four rows (decklabdeckwidgets.h). Widgets only: no engine, no
// controls, no audio. The real surface wiring is covered by
// NativeDeckLabTest and DeckLabStemsTest.
#include <gtest/gtest.h>

#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QImage>
#include <QMouseEvent>
#include <QPushButton>
#include <QStringList>
#include <QTouchEvent>
#include <QVBoxLayout>
#include <memory>

#include "widget/decklabdeckwidgets.h"
#include "widget/decklabpads.h"

namespace {

using Mode = DeckLabPadBlock::Mode;

// A host with a fake control store and a write log.
struct FakeHost {
    int deck = 1;
    bool current = true;
    QHash<QString, double> values;
    QStringList writes;
    int acted = 0;
    DeckLabPadBlock::Host host() {
        DeckLabPadBlock::Host h;
        h.deck = [this] { return deck; };
        h.epoch = [] { return quint64(1); };
        h.current = [this](int) { return current; };
        h.read = [this](int which, const QString& item) {
            return values.value(QStringLiteral("%1/%2").arg(which).arg(item), -1);
        };
        h.write = [this](int which, const QString& item, double value) {
            writes << QStringLiteral("%1/%2=%3").arg(which).arg(item).arg(value);
        };
        h.acted = [this](int) { ++acted; };
        return h;
    }
};

// Stand-in for a deck's DeckLabStemPads: just the hooks.
struct FakeStems {
    QWidget* row = nullptr;
    bool present = true;
    bool muted = false;
    bool latched = false;
    int restored = 0;
    DeckLabPadBlock::Stems hooks(int deck) {
        DeckLabPadBlock::Stems s;
        s.deck = deck;
        s.pads = row;
        s.present = [this] { return present; };
        s.anyMuted = [this] { return muted; };
        s.restoreAll = [this] { ++restored; };
        s.setSoloLatch = [this](bool on) { latched = on; };
        s.setShown = [this](bool on) { row->setVisible(on); };
        return s;
    }
};

DeckLabPadDeckState loaded(int deck) {
    DeckLabPadDeckState state;
    state.deck = deck;
    state.loaded = true;
    state.loopSize = 4;
    return state;
}

class DeckLabPadsTest : public ::testing::Test {
  protected:
    void SetUp() override {
        host.deck = 1;
        block = std::make_unique<DeckLabPadBlock>(QStringLiteral("decklab-pads-1"),
                QStringLiteral("decklab-hotcue-1-"), host.host());
        block->resize(392, DeckLabPadBlock::kBlockHeight);
        block->setDeckState(loaded(1));
        block->show();
        QCoreApplication::processEvents();
    }
    void press(QPushButton* button) {
        button->setDown(true);
        emit button->pressed();
    }
    void release(QPushButton* button) {
        button->setDown(false);
        emit button->released();
    }
    FakeHost host;
    std::unique_ptr<DeckLabPadBlock> block;
};

TEST_F(DeckLabPadsTest, CuePadsFireOnTouchDownWithRoundThreeGuards) {
    ASSERT_EQ(Mode::Cue, block->mode());
    auto* a = block->pad(0);
    EXPECT_EQ(QStringLiteral("decklab-hotcue-1-1"), a->objectName());
    EXPECT_EQ(QStringLiteral("A"), a->text());
    EXPECT_EQ(QStringLiteral("Set cue A here"), a->toolTip());
    // Empty on screen and in the engine: set, on touch-down.
    press(a);
    EXPECT_EQ((QStringList{QStringLiteral("1/hotcue_1_set=1"), QStringLiteral("1/hotcue_1_set=0")}), host.writes);
    release(a);
    EXPECT_EQ(2, host.writes.size()) << "the release adds nothing";
    // Set on screen and in the engine: jump.
    host.writes.clear();
    auto state = loaded(1);
    state.cues[0] = 0xe53935;
    block->setDeckState(state);
    host.values.insert(QStringLiteral("1/hotcue_1_position"), 44100);
    EXPECT_TRUE(a->property("cueSet").toBool());
    EXPECT_EQ(QStringLiteral("#e53935"), a->property("cueColor").toString());
    press(a);
    release(a);
    EXPECT_EQ((QStringList{QStringLiteral("1/hotcue_1_goto=1"), QStringLiteral("1/hotcue_1_goto=0")}), host.writes);
    // Screen and engine disagree (a controller cleared it): refresh only.
    host.writes.clear();
    host.values.remove(QStringLiteral("1/hotcue_1_position"));
    press(a);
    EXPECT_TRUE(host.writes.isEmpty());
    // The layer moved under the pads before they refreshed: nothing.
    host.values.insert(QStringLiteral("1/hotcue_1_position"), 44100);
    host.deck = 3;
    press(a);
    EXPECT_TRUE(host.writes.isEmpty());
    host.deck = 1;
    // Another track is loaded than the one the screen shows: nothing.
    host.current = false;
    press(a);
    EXPECT_TRUE(host.writes.isEmpty());
    host.current = true;
    // Without a track every pad is disabled.
    DeckLabPadDeckState empty;
    empty.deck = 1;
    block->setDeckState(empty);
    EXPECT_FALSE(a->isEnabled());
    EXPECT_TRUE(block->modeKey(Mode::Loop)->isEnabled()) << "modes still switch";
}

TEST_F(DeckLabPadsTest, LoopModeTogglesLengthsAndLightsTheActiveOne) {
    press(block->modeKey(Mode::Loop));
    ASSERT_EQ(Mode::Loop, block->mode());
    EXPECT_TRUE(block->modeKey(Mode::Loop)->lit());
    EXPECT_FALSE(block->modeKey(Mode::Cue)->lit());
    const QStringList labels{QStringLiteral("1/4"), QStringLiteral("1/2"), QStringLiteral("1"), QStringLiteral("2"),
            QStringLiteral("4"), QStringLiteral("8"), QStringLiteral("16"), QStringLiteral("32")};
    for (int index = 0; index < 8; ++index) {
        EXPECT_EQ(labels[index], block->pad(index)->text()) << index;
    }
    // beatloop_size 4, no loop: the 4 is armed, nothing lit.
    EXPECT_FALSE(block->pad(4)->lit());
    press(block->pad(0));
    release(block->pad(0));
    EXPECT_EQ((QStringList{QStringLiteral("1/beatloop_0.25_toggle=1"), QStringLiteral("1/beatloop_0.25_toggle=0")}), host.writes);
    auto state = loaded(1);
    state.looping = true;
    state.loopSize = 0.25;
    block->setDeckState(state);
    EXPECT_TRUE(block->pad(0)->lit());
    EXPECT_TRUE(block->pad(0)->accessibleName().contains(QStringLiteral("on")));
    EXPECT_FALSE(block->pad(4)->lit());
    // In CUE mode the LOOP key carries a lamp: a loop is running.
    press(block->modeKey(Mode::Cue));
    EXPECT_TRUE(block->modeKey(Mode::Loop)->indicator());
    state.looping = false;
    block->setDeckState(state);
    EXPECT_FALSE(block->modeKey(Mode::Loop)->indicator());
}

TEST_F(DeckLabPadsTest, RollsPlayWhileHeldAndAlwaysEnd) {
    block->setMode(Mode::Roll);
    EXPECT_EQ(QStringLiteral("1/16"), block->pad(0)->text());
    EXPECT_EQ(QStringLiteral("8"), block->pad(7)->text());
    const QString on = QStringLiteral("1/beatlooproll_0.25_activate=1");
    const QString off = QStringLiteral("1/beatlooproll_0.25_activate=0");
    auto* pad = block->pad(2);
    // Held: on at touch-down, off at release.
    press(pad);
    EXPECT_EQ(QStringList{on}, host.writes);
    EXPECT_TRUE(block->holding());
    EXPECT_TRUE(pad->lit());
    release(pad);
    EXPECT_EQ((QStringList{on, off}), host.writes);
    EXPECT_FALSE(block->holding());
    EXPECT_FALSE(pad->lit());
    // Every way the touch can go astray ends the roll exactly once.
    const std::array<std::function<void()>, 5> astray{
            [this] { block->hide(); },
            [this] { auto state = loaded(3); block->setDeckState(state); },
            [] { QTouchEvent cancel(QEvent::TouchCancel); QCoreApplication::sendEvent(qApp, &cancel); },
            [] { QEvent deactivate(QEvent::ApplicationDeactivate); QCoreApplication::sendEvent(qApp, &deactivate); },
            [this] {
                // One contact drives the pointer: a press elsewhere means the
                // holding finger is gone.
                QWidget elsewhere;
                QMouseEvent down(QEvent::MouseButtonPress, QPointF(1, 1), QPointF(1, 1), Qt::LeftButton,
                        Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(&elsewhere, &down);
            }};
    for (std::size_t way = 0; way < astray.size(); ++way) {
        block->show();
        block->setDeckState(loaded(1));
        host.deck = 1;
        block->setMode(Mode::Roll);
        host.writes.clear();
        press(block->pad(2));
        ASSERT_TRUE(block->holding()) << way;
        astray[way]();
        EXPECT_FALSE(block->holding()) << way;
        EXPECT_EQ((QStringList{on, off}), host.writes) << way;
        release(block->pad(2));
        EXPECT_EQ(2, host.writes.size()) << way << ": the late release adds nothing";
    }
    // A destroyed block never leaves a roll behind.
    block->show();
    block->setDeckState(loaded(1));
    block->setMode(Mode::Roll);
    host.writes.clear();
    press(block->pad(2));
    block.reset();
    EXPECT_EQ((QStringList{on, off}), host.writes);
}

TEST_F(DeckLabPadsTest, ModesAreRememberedPerDeckAndStemIsOnlyForStemTracks) {
    FakeStems one;
    FakeStems three;
    one.row = new QWidget(block.get());
    three.row = new QWidget(block.get());
    three.present = false;
    block->addStems(one.hooks(1));
    block->addStems(three.hooks(3));
    EXPECT_FALSE(one.row->isVisible());
    // Deck 1 holds a stem track: STEM is offered.
    block->stemsChanged();
    auto* stem = block->modeKey(Mode::Stem);
    EXPECT_TRUE(stem->isEnabled());
    EXPECT_EQ(QStringLiteral("STEM"), stem->text());
    press(stem);
    ASSERT_EQ(Mode::Stem, block->mode());
    EXPECT_TRUE(one.row->isVisible());
    EXPECT_FALSE(three.row->isVisible());
    for (int index = 0; index < 8; ++index) {
        EXPECT_FALSE(block->pad(index)->isVisible()) << index;
    }
    EXPECT_TRUE(block->allKey()->isVisible());
    EXPECT_TRUE(block->soloKey()->isVisible());
    // The layer turns to deck 3, a normal track: no STEM there, CUE shows.
    host.deck = 3;
    block->setDeckState(loaded(3));
    EXPECT_EQ(Mode::Cue, block->mode());
    EXPECT_FALSE(block->modeKey(Mode::Stem)->isEnabled());
    EXPECT_TRUE(block->modeKey(Mode::Stem)->text().isEmpty()) << "STEM appears only for stem tracks";
    EXPECT_FALSE(one.row->isVisible());
    press(block->modeKey(Mode::Stem));
    EXPECT_EQ(Mode::Cue, block->mode()) << "no STEM mode without stems";
    block->setMode(Mode::Roll);
    // Back to deck 1: its own mode, STEM, again.
    host.deck = 1;
    block->setDeckState(loaded(1));
    EXPECT_EQ(Mode::Stem, block->mode());
    EXPECT_TRUE(one.row->isVisible());
    // Deck 1 loads a normal track: CUE shows, the choice is kept for its
    // next stem track.
    one.present = false;
    block->stemsChanged();
    EXPECT_EQ(Mode::Cue, block->mode());
    EXPECT_EQ(Mode::Stem, block->chosenMode(1));
    EXPECT_FALSE(one.row->isVisible());
    EXPECT_TRUE(block->pad(0)->isVisible());
    one.present = true;
    block->stemsChanged();
    EXPECT_EQ(Mode::Stem, block->mode());
    // Deck 3 kept ROLL.
    EXPECT_EQ(Mode::Roll, block->chosenMode(3));
}

TEST_F(DeckLabPadsTest, AllAndSoloDriveTheStemPads) {
    FakeStems one;
    one.row = new QWidget(block.get());
    block->addStems(one.hooks(1));
    block->setMode(Mode::Stem);
    ASSERT_EQ(Mode::Stem, block->mode());
    press(block->allKey());
    EXPECT_EQ(1, one.restored);
    // ALL is refused when the pads no longer show the touched deck.
    host.deck = 3;
    press(block->allKey());
    EXPECT_EQ(1, one.restored);
    host.deck = 1;
    // SOLO latches, lit, until touched again or the mode changes.
    press(block->soloKey());
    EXPECT_TRUE(one.latched);
    EXPECT_TRUE(block->soloKey()->lit());
    EXPECT_TRUE(block->soloLatched());
    press(block->soloKey());
    EXPECT_FALSE(one.latched);
    press(block->soloKey());
    ASSERT_TRUE(one.latched);
    block->setMode(Mode::Cue);
    EXPECT_FALSE(one.latched) << "leaving STEM drops the latch";
    EXPECT_FALSE(block->soloKey()->lit());
    // Muted stems light a lamp on the STEM key while another mode shows.
    one.muted = true;
    block->stemsChanged();
    EXPECT_TRUE(block->modeKey(Mode::Stem)->indicator());
    block->setMode(Mode::Stem);
    EXPECT_FALSE(block->modeKey(Mode::Stem)->indicator());
    press(block->soloKey());
    ASSERT_TRUE(one.latched);
    block->hide();
    EXPECT_FALSE(one.latched) << "hiding drops the latch";
}

TEST_F(DeckLabPadsTest, TouchTargetsAndNoLayoutWorkForAnUnchangedDeck) {
    block->setOuterMargins(4, 4, 4, 0);
    EXPECT_EQ(DeckLabPadBlock::kBlockHeight + 4, block->height());
    // The two-deck card width at 800 px, with SYNC and MORE beside the pads.
    auto* column = new QWidget;
    auto* box = new QVBoxLayout(column);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(4);
    for (const char* text : {"SYNC", "MORE"}) {
        auto* key = new QPushButton(QString::fromLatin1(text), column);
        key->setFixedHeight(44);
        box->addWidget(key);
    }
    block->setSideColumn(column);
    block->resize(399, block->height());
    QCoreApplication::processEvents();
    QList<QWidget*> keys;
    for (int index = 0; index < 8; ++index) keys << block->pad(index);
    for (const Mode mode : {Mode::Cue, Mode::Loop, Mode::Roll, Mode::Stem}) keys << block->modeKey(mode);
    for (auto* key : keys) {
        EXPECT_EQ(44, key->height()) << key->objectName().toStdString();
        EXPECT_GE(key->width(), 44) << key->objectName().toStdString();
        EXPECT_TRUE(block->rect().contains(QRect(key->mapTo(block.get(), QPoint()), key->size())))
                << key->objectName().toStdString();
        EXPECT_EQ(16, key->font().pixelSize()) << key->objectName().toStdString();
    }
    // Neighbouring pads keep at least a 4 px gap.
    EXPECT_GE(block->pad(1)->x() - (block->pad(0)->x() + block->pad(0)->width()), 4);
    EXPECT_GE(block->pad(4)->y() - (block->pad(0)->y() + block->pad(0)->height()), 4);
    // A refresh with an unchanged state writes nothing to any widget.
    struct Counter final : QObject {
        int count = 0;
        bool eventFilter(QObject*, QEvent* event) override {
            const auto type = event->type();
            count += type == QEvent::LayoutRequest || type == QEvent::StyleChange || type == QEvent::Polish ||
                    type == QEvent::UpdateRequest || type == QEvent::Paint;
            return false;
        }
    } counter;
    QList<QWidget*> watched{block.get()};
    watched.append(block->findChildren<QWidget*>());
    for (auto* widget : watched) widget->installEventFilter(&counter);
    auto state = loaded(1);
    state.cues[2] = 0x2bcb58;
    block->setDeckState(state);
    QCoreApplication::processEvents();
    EXPECT_GT(counter.count, 0) << "a real change repaints";
    counter.count = 0;
    for (int n = 0; n < 5; ++n) block->setDeckState(state);
    QCoreApplication::processEvents();
    EXPECT_EQ(0, counter.count);
    for (auto* widget : watched) widget->removeEventFilter(&counter);
}

DeckLabStripState playing(int deck) {
    DeckLabStripState s;
    s.deck = deck;
    s.loaded = true;
    s.playing = true;
    s.title = QStringLiteral("Generated Tone");
    s.artist = QStringLiteral("Deck Lab Fixture");
    s.bpm = QStringLiteral("128.0");
    s.tempo = QStringLiteral("0.00%");
    s.tempoCentered = true;
    s.range = QString::fromUtf8("±8%");
    s.key = QStringLiteral("8A");
    s.remaining = QString::fromUtf8("−3:25");
    s.quantize = true;
    s.phase = 1;
    s.phaseStep = 4;
    return s;
}

using Part = DeckLabStripRenderer::Part;

TEST(DeckLabReadoutTest, FourRowsFitTheSame104PixelsAtEveryWidth) {
    for (const int width : {399, 400, 512, 640}) {
        DeckLabStripRenderer renderer;
        renderer.setSize(QSize(width, 104));
        auto s = playing(1);
        s.phrase = {QStringLiteral("Chorus 1"), QStringLiteral("8.3"), QColor(0x2b, 0xcb, 0x58)};
        renderer.setState(s);
        const QRect title = renderer.rect(Part::Title);
        const QRect values = renderer.rect(Part::Bpm);
        const QRect lamps = renderer.rect(Part::Chips);
        const QRect phase = renderer.rect(Part::Phase);
        const QRect phrase = renderer.rect(Part::Phrase);
        // Four rows, top to bottom, inside the readout.
        EXPECT_LE(title.bottom(), values.top() + 2) << width;
        EXPECT_LE(values.bottom(), lamps.top()) << width;
        EXPECT_LE(lamps.bottom(), phase.top()) << width;
        EXPECT_LE(phase.bottom(), 103) << width;
        // The phrase never takes the phase meter's place (rekordbox tracks).
        EXPECT_FALSE(phase.isEmpty()) << width;
        EXPECT_FALSE(phrase.isEmpty()) << width;
        EXPECT_FALSE(phrase.intersects(phase)) << width;
        EXPECT_GE(phase.width(), 120) << width;
        EXPECT_TRUE(renderer.overflowingParts().isEmpty()) << width;
    }
}

TEST(DeckLabReadoutTest, PhaseMeterComparesWithTheReferenceAndRepaintsOnlyItself) {
    DeckLabDeckStrip strip(2);
    strip.resize(400, 104);
    strip.grab();
    auto s = playing(2);
    s.referenceDeck = 1;
    s.referencePhase = 3;
    s.referenceStep = 7;
    s.referencePlaying = true;
    strip.setState(s);
    const QRect phase = strip.renderer().rect(Part::Phase);
    const QImage image = strip.grab().toImage();
    // Top row: the reference deck's beat, in its colour; bottom: this deck's.
    bool reference = false;
    bool own = false;
    for (int x = phase.left(); x <= phase.right(); ++x) {
        reference |= image.pixelColor(x, phase.top() + 4) == deckLabDeckColor(1);
        own |= image.pixelColor(x, phase.bottom() - 4) == deckLabDeckColor(2);
    }
    EXPECT_TRUE(reference);
    EXPECT_TRUE(own);
    // The beat filling up is a change of the meter alone.
    DeckLabStripRenderer renderer;
    renderer.setSize(QSize(400, 104));
    renderer.setState(s);
    auto next = s;
    next.phaseStep = 5;
    next.referenceStep = 0;
    next.referencePhase = 0;
    const QRegion dirty = renderer.setState(next);
    EXPECT_FALSE(dirty.isEmpty());
    EXPECT_EQ(QRegion(phase), dirty);
    // Nothing to compare with (this deck is the master): the top row is dark.
    auto master = playing(2);
    master.master = true;
    strip.setState(master);
    const QImage alone = strip.grab().toImage();
    for (int x = phase.left(); x <= phase.right(); ++x) {
        EXPECT_NE(deckLabDeckColor(1), alone.pixelColor(x, phase.top() + 4)) << x;
    }
}

TEST(DeckLabReadoutTest, TempoBeyondTheFaderRangeIsFlaggedOver) {
    DeckLabDeckStrip strip(1);
    strip.resize(400, 104);
    strip.grab();
    auto s = playing(1);
    s.sync = true;
    s.tempo = QString::fromUtf8("−18.82%");
    s.tempoCentered = false;
    strip.setState(s);
    const QRect tempo = strip.renderer().rect(Part::Tempo);
    const auto whiteIn = [&strip](const QRect& area) {
        const QImage image = strip.grab().toImage();
        int white = 0;
        for (int y = area.top(); y <= area.bottom(); ++y)
            for (int x = area.left(); x <= area.right(); ++x) white += image.pixelColor(x, y) == QColor(0xf2, 0xf4, 0xf7);
        return white;
    };
    const int before = whiteIn(QRect(tempo.left(), tempo.top(), 3, tempo.height()));
    auto over = s;
    over.overRange = true;
    DeckLabStripRenderer renderer;
    renderer.setSize(QSize(400, 104));
    renderer.setState(s);
    const QRegion dirty = renderer.setState(over);
    EXPECT_TRUE(dirty.intersects(tempo));
    EXPECT_TRUE(dirty.intersects(renderer.rect(Part::Chips)));
    strip.setState(over);
    EXPECT_GT(whiteIn(QRect(tempo.left(), tempo.top(), 3, tempo.height())), before) << "a white outline";
    EXPECT_TRUE(strip.renderer().overflowingParts().isEmpty());
}

TEST(DeckLabReadoutTest, LampsAreFlatWhenOffAndFilledWhenLit) {
    DeckLabDeckStrip strip(1);
    strip.resize(400, 104);
    strip.grab();
    auto s = playing(1);
    strip.setState(s);
    const QRect lamps = strip.renderer().rect(Part::Chips);
    const QColor ground(0x0d, 0x0f, 0x12);
    // MASTER off: no outline around it, only a dot and a muted label.
    const QImage off = strip.grab().toImage();
    const int top = lamps.top() + (lamps.height() - 20) / 2;
    for (int x = lamps.left() + 4; x < lamps.left() + 60; ++x) {
        EXPECT_EQ(ground, off.pixelColor(x, top + 1)) << x;
    }
    s.master = true;
    strip.setState(s);
    const QImage lit = strip.grab().toImage();
    EXPECT_EQ(deckLabDeckColor(1), lit.pixelColor(lamps.left() + 40, top + 2));
    // Exceptions: QNT OFF only when quantize is off.
    s.quantize = false;
    strip.setState(s);
    EXPECT_TRUE(strip.renderer().overflowingParts().isEmpty());
    EXPECT_FALSE(strip.modesText().contains(QStringLiteral("QNT")));
}

TEST(DeckLabReadoutTest, EmptyDeckBadgeKeepsItsColour) {
    DeckLabDeckStrip strip(2);
    strip.resize(400, 104);
    strip.grab();
    DeckLabStripState s;
    s.deck = 2;
    strip.setState(s);
    const QImage image = strip.grab().toImage();
    // The outline is the full deck colour (a darkened fill read as brown).
    bool outline = false;
    for (int x = 20; x < 38 && !outline; ++x) outline = image.pixelColor(x, 3) == deckLabDeckColor(2);
    EXPECT_TRUE(outline);
    EXPECT_EQ(QColor(0x0d, 0x0f, 0x12), image.pixelColor(20, 20)) << "no fill inside";
}

} // namespace
