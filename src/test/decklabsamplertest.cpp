// SPDX-License-Identifier: GPL-2.0-or-later
// The Sampler page (round 4, move 4) against generated samplers: no engine,
// no audio, no files. NativeDeckLabTest covers the page inside the real
// surface with Mixxx's [SamplerN] controls (decklabtest.cpp).
#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QSet>
#include <QTest>
#include <cmath>
#include <memory>
#include <vector>

#include "widget/decklabsampler.h"

namespace {

using Mode = DeckLabSamplerEngine::Mode;

// Generated samplers. Writes act at once, as Mixxx's controls do, and tell
// the page through the same listener.
class FakeSamplers final : public DeckLabSamplerEngine {
  public:
    struct Pad {
        bool loaded = false;
        QString title;
        bool playing = false;
        double position = 0;
        double duration = 0;
        double rate = 1;
        double bpm = 0;
        double gain = 0;
        Mode mode = Mode::OneShot;
        int triggers = 0;
        int stops = 0;
        int ejects = 0;
    };
    using DeckLabSamplerEngine::notify;
    std::vector<Pad> pads = std::vector<Pad>(64);
    int samplers = 16;
    bool quantizing = true;
    bool syncing = false;
    double fader = 1;

    Pad& at(int sampler) {
        return pads[sampler - 1];
    }
    const Pad& at(int sampler) const {
        return pads[sampler - 1];
    }
    void load(int sampler, const QString& title, double duration, double bpm = 0) {
        Pad& pad = at(sampler);
        pad.loaded = true;
        pad.title = title;
        pad.duration = duration;
        pad.bpm = bpm;
        notify(sampler);
    }
    int count() const override {
        return samplers;
    }
    bool loaded(int sampler) const override {
        return at(sampler).loaded;
    }
    QString title(int sampler) const override {
        return at(sampler).title;
    }
    bool playing(int sampler) const override {
        return at(sampler).loaded && at(sampler).playing;
    }
    double position(int sampler) const override {
        return at(sampler).position;
    }
    double duration(int sampler) const override {
        return at(sampler).duration;
    }
    double rate(int sampler) const override {
        return at(sampler).rate;
    }
    double bpm(int sampler) const override {
        return at(sampler).bpm;
    }
    double gainDb(int sampler) const override {
        return at(sampler).gain;
    }
    Mode mode(int sampler) const override {
        return at(sampler).mode;
    }
    bool quantize() const override {
        return quantizing;
    }
    bool syncLoops() const override {
        return syncing;
    }
    double level() const override {
        return fader;
    }
    double levelDb() const override {
        return fader > 0 ? 20 * std::log10(fader) : -99;
    }
    void trigger(int sampler) override {
        Pad& pad = at(sampler);
        ++pad.triggers;
        pad.playing = true;
        pad.position = 0;
        notify(sampler);
    }
    void stop(int sampler) override {
        Pad& pad = at(sampler);
        ++pad.stops;
        pad.playing = false;
        pad.position = 0;
        notify(sampler);
    }
    void eject(int sampler) override {
        Pad& pad = at(sampler);
        if (pad.playing) {
            return;
        }
        ++pad.ejects;
        pad = Pad{false, QString(), false, 0, 0, 1, 0, pad.gain, pad.mode, pad.triggers, pad.stops, pad.ejects};
        notify(sampler);
    }
    void setGainDb(int sampler, double db) override {
        at(sampler).gain = db;
        notify(sampler);
    }
    void setMode(int sampler, Mode mode) override {
        at(sampler).mode = mode;
        notify(sampler);
    }
    void setQuantize(bool on) override {
        quantizing = on;
    }
    void setSyncLoops(bool on) override {
        syncing = on;
    }
    void setLevel(double parameter) override {
        fader = parameter;
    }
};

// Counts paints per widget, to prove idle pads never repaint on the tick.
class PaintCounter final : public QObject {
  public:
    int count = 0;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Paint) {
            ++count;
        }
        return QObject::eventFilter(watched, event);
    }
};

void settle() {
    for (int pass = 0; pass < 4; ++pass) {
        QCoreApplication::processEvents();
    }
}

class DeckLabSamplerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        page = std::make_unique<DeckLabSampler>(&samplers);
        // The page below the 44 px rail on the 800x480 screen.
        page->resize(800, 436);
        page->show();
        settle();
    }
    void TearDown() override {
        page.reset();
    }
    DeckLabSamplerPad* pad(int index) const {
        return page->findChild<DeckLabSamplerPad*>(QStringLiteral("decklab-sample-%1").arg(index));
    }
    QPushButton* key(const char* name) const {
        return page->findChild<QPushButton*>(QString::fromLatin1(name));
    }
    void tick(const std::array<DeckLabSamplerDeck, 4>& decks = {}) {
        page->tick(decks);
        settle();
    }
    void capture(const QString& name) {
        const QString output = qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
        if (!output.isEmpty()) {
            EXPECT_TRUE(page->grab().save(QDir(output).filePath(name))) << name.toStdString();
        }
    }
    static QColor pixel(QWidget* widget, int x, int y) {
        return widget->grab().toImage().pixelColor(x, y);
    }
    FakeSamplers samplers;
    std::unique_ptr<DeckLabSampler> page;
    const QColor lit = QColor(0xf2, 0xf4, 0xf7);
    const QColor panel = QColor(0x16, 0x19, 0x1e);
};

TEST_F(DeckLabSamplerTest, FourByFourGridWithTheDecksOnTopAndASideColumn) {
    QSet<int> columns;
    QSet<int> rows;
    for (int index = 1; index <= 16; ++index) {
        auto* p = pad(index);
        ASSERT_NE(nullptr, p) << index;
        EXPECT_TRUE(p->isVisible());
        EXPECT_GE(p->width(), 160) << index;
        EXPECT_GE(p->height(), 88) << index;
        EXPECT_TRUE(page->rect().contains(QRect(p->mapTo(page.get(), QPoint()), p->size())));
        columns.insert(p->mapTo(page.get(), QPoint()).x());
        rows.insert(p->mapTo(page.get(), QPoint()).y());
        // Row-major: pads 1-4 are the top row, four wide.
        EXPECT_EQ(pad(1 + (index - 1) % 4)->x(), p->x()) << index;
        EXPECT_EQ(pad(1 + 4 * ((index - 1) / 4))->y(), p->y()) << index;
    }
    EXPECT_EQ(4, columns.size());
    EXPECT_EQ(4, rows.size());
    auto* decks = page->findChild<DeckLabSamplerDecks*>(QStringLiteral("decklab-sampler-decks"));
    ASSERT_NE(nullptr, decks);
    EXPECT_EQ(QPoint(0, 0), decks->mapTo(page.get(), QPoint()));
    EXPECT_EQ(44, decks->height());
    EXPECT_EQ(page->width(), decks->width());
    EXPECT_LT(decks->geometry().bottom(), pad(1)->mapTo(page.get(), QPoint()).y());
    // Side keys: 44 px targets with 4 px between them, right of the grid.
    for (const char* name : {"decklab-sampler-qnt", "decklab-sampler-sync", "decklab-sampler-edit",
                 "decklab-sampler-fx", "decklab-samples-stop"}) {
        auto* b = key(name);
        ASSERT_NE(nullptr, b) << name;
        EXPECT_TRUE(b->isVisible()) << name;
        EXPECT_GE(b->height(), 44) << name;
        EXPECT_GE(b->width(), 44) << name;
        EXPECT_GT(b->mapTo(page.get(), QPoint()).x(), pad(4)->mapTo(page.get(), QPoint()).x() + pad(4)->width())
                << name;
    }
    EXPECT_EQ(key("decklab-sampler-qnt")->y(), key("decklab-sampler-sync")->y());
    EXPECT_GE(key("decklab-sampler-sync")->x() - key("decklab-sampler-qnt")->geometry().right() - 1, 4);
    EXPECT_FALSE(key("decklab-sampler-bank")->isVisible()) << "16 samplers: one bank, no BANK key";
    EXPECT_EQ(QStringLiteral("STOP ALL"), key("decklab-samples-stop")->text());
    EXPECT_FALSE(key("decklab-samples-stop")->isEnabled()) << "nothing plays";
    EXPECT_TRUE(key("decklab-sampler-qnt")->isChecked()) << "QNT shows the samplers' quantize";
    // An empty pad: its number and a "+", on the brand panel.
    EXPECT_EQ(QStringLiteral("Pad 1, empty: tap to load"), pad(1)->accessibleName());
    EXPECT_EQ(panel, pixel(pad(1), pad(1)->width() / 2, 10));
    capture(QStringLiteral("sampler-unit-empty.png"));
}

TEST_F(DeckLabSamplerTest, LoadedPadsFireOnTouchDownAndEmptyPadsOpenBrowseOnRelease) {
    samplers.load(1, QStringLiteral("Generated Horn"), 3);
    settle();
    QSignalSpy load(page.get(), &DeckLabSampler::loadRequested);
    QTest::mousePress(pad(1), Qt::LeftButton);
    EXPECT_EQ(1, samplers.at(1).triggers) << "a loaded pad plays on touch-down";
    QTest::mouseRelease(pad(1), Qt::LeftButton);
    EXPECT_EQ(1, samplers.at(1).triggers) << "the release does not trigger again";
    QTest::mousePress(pad(2), Qt::LeftButton);
    EXPECT_EQ(0, load.count()) << "the page never changes under a finger that is still down";
    QTest::mouseRelease(pad(2), Qt::LeftButton);
    ASSERT_EQ(1, load.count());
    EXPECT_EQ(2, load.at(0).at(0).toInt());
    // A touch that outlives a page change or a cancel does nothing.
    quint64 epoch = 0;
    page->setInteractionEpoch([&epoch] { return epoch; });
    QTest::mousePress(pad(3), Qt::LeftButton);
    ++epoch;
    QTest::mouseRelease(pad(3), Qt::LeftButton);
    EXPECT_EQ(1, load.count());
    // A pad loaded between touch and release is not replaced.
    QTest::mousePress(pad(4), Qt::LeftButton);
    samplers.load(4, QStringLiteral("Generated Clap"), 1);
    QTest::mouseRelease(pad(4), Qt::LeftButton);
    EXPECT_EQ(1, load.count());
    page->setInteractionEpoch({});
}

TEST_F(DeckLabSamplerTest, APlayingPadIsALitLampThatDrainsAndShowsTimeLeft) {
    samplers.load(1, QStringLiteral("Generated Riser"), 8, 128);
    samplers.load(2, QStringLiteral("Generated Shot"), 2);
    settle();
    EXPECT_FALSE(pad(1)->isChecked());
    EXPECT_EQ(8, pad(1)->seconds()) << "idle: the length";
    QTest::mouseClick(pad(1), Qt::LeftButton);
    settle();
    EXPECT_TRUE(pad(1)->isChecked());
    EXPECT_TRUE(pad(1)->accessibleName().endsWith(QStringLiteral(", playing")));
    EXPECT_TRUE(key("decklab-samples-stop")->isEnabled());
    samplers.at(1).position = 0.25;
    tick();
    const int width = pad(1)->width();
    // Below the bottom row of text: only the lamp is painted there.
    const int clear = pad(1)->height() - 3;
    EXPECT_NEAR(0.75 * width, pad(1)->litWidth(), 1.0) << "the lit part is what is left to play";
    EXPECT_EQ(6, pad(1)->seconds());
    EXPECT_EQ(lit, pixel(pad(1), width / 2, clear));
    EXPECT_EQ(panel, pixel(pad(1), width - 8, clear)) << "the drained part is dark";
    EXPECT_EQ(lit, pixel(pad(1), width - 8, 2)) << "the top bar still says playing";
    capture(QStringLiteral("sampler-unit-playing.png"));
    // The tick repaints playing pads only, and only when they changed.
    std::vector<std::unique_ptr<PaintCounter>> counters;
    for (int index = 1; index <= 16; ++index) {
        counters.push_back(std::make_unique<PaintCounter>());
        pad(index)->installEventFilter(counters.back().get());
    }
    tick();
    tick();
    for (int index = 1; index <= 16; ++index) {
        EXPECT_EQ(0, counters[index - 1]->count) << "unchanged pad " << index << " repainted";
    }
    samplers.at(1).position = 0.5;
    tick();
    EXPECT_GE(counters[0]->count, 1) << "the playing pad drains";
    for (int index = 2; index <= 16; ++index) {
        EXPECT_EQ(0, counters[index - 1]->count) << "idle pad " << index << " repainted";
    }
    EXPECT_EQ(4, pad(1)->seconds());
    // At double speed the time left halves.
    samplers.at(1).rate = 2;
    tick();
    EXPECT_EQ(2, pad(1)->seconds());
    // When it ends, the lamp goes out.
    samplers.at(1).playing = false;
    tick();
    EXPECT_FALSE(pad(1)->isChecked());
    EXPECT_EQ(panel, pixel(pad(1), width / 2, clear));
    EXPECT_FALSE(key("decklab-samples-stop")->isEnabled());
}

TEST_F(DeckLabSamplerTest, HoldOpensTheDockedSheetAndClearActsOnReleaseWhenStopped) {
    samplers.load(2, QStringLiteral("Generated Vocal"), 4, 120);
    settle();
    QTest::mousePress(pad(2), Qt::LeftButton);
    EXPECT_EQ(1, samplers.at(2).triggers);
    EXPECT_FALSE(page->sheetOpen());
    QTest::qWait(DeckLabSampler::kHoldMs + 150);
    ASSERT_TRUE(page->sheetOpen()) << "a held pad opens its edit sheet";
    EXPECT_EQ(2, page->sheetSampler());
    QTest::mouseRelease(pad(2), Qt::LeftButton);
    EXPECT_TRUE(page->sheetOpen());
    settle();
    auto* sheet = page->findChild<QWidget*>(QStringLiteral("decklab-sampler-sheet"));
    ASSERT_NE(nullptr, sheet);
    EXPECT_TRUE(sheet->isVisible());
    // Docked over the lower half, away from the pad on the top row.
    EXPECT_GT(sheet->mapTo(page.get(), QPoint()).y(), pad(2)->mapTo(page.get(), QPoint()).y() + pad(2)->height());
    EXPECT_TRUE(pad(2)->isVisible());
    EXPECT_FALSE(pad(9)->isVisible());
    EXPECT_FALSE(pad(16)->isVisible());
    EXPECT_TRUE(page->rect().contains(QRect(sheet->mapTo(page.get(), QPoint()), sheet->size())));
    EXPECT_EQ(QStringLiteral("PAD 2"), page->findChild<QLabel*>(QStringLiteral("decklab-sampler-sheet-number"))->text());
    for (auto* b : sheet->findChildren<QPushButton*>()) {
        if (b->isVisible()) {
            EXPECT_GE(b->height(), 44) << b->objectName().toStdString();
        }
    }
    capture(QStringLiteral("sampler-unit-sheet.png"));
    auto* clear = key("decklab-sampler-sheet-clear");
    auto* stop = key("decklab-sampler-sheet-stop");
    EXPECT_FALSE(clear->isEnabled()) << "never clear a playing pad";
    EXPECT_TRUE(stop->isEnabled());
    QTest::mouseClick(stop, Qt::LeftButton);
    EXPECT_EQ(1, samplers.at(2).stops);
    tick();
    EXPECT_TRUE(clear->isEnabled());
    QTest::mousePress(clear, Qt::LeftButton);
    EXPECT_EQ(0, samplers.at(2).ejects) << "destructive: nothing happens on touch-down";
    QTest::mouseRelease(clear, Qt::LeftButton);
    EXPECT_EQ(1, samplers.at(2).ejects);
    EXPECT_FALSE(page->sheetOpen());
    settle();
    EXPECT_EQ(QStringLiteral("Pad 2, empty: tap to load"), pad(2)->accessibleName());
    EXPECT_TRUE(pad(9)->isVisible());
}

TEST_F(DeckLabSamplerTest, EditKeyOpensTheSheetWithoutPlayingAndTheKnobSetsGain) {
    samplers.load(13, QStringLiteral("Generated Loop"), 8, 124);
    settle();
    EXPECT_FALSE(page->knob(decklab::KnobAction::Move, 1)) << "a closed sheet leaves the knob to Browse";
    QTest::mouseClick(key("decklab-sampler-edit"), Qt::LeftButton);
    EXPECT_TRUE(page->editMode());
    EXPECT_TRUE(key("decklab-sampler-edit")->isChecked());
    QTest::mouseClick(pad(13), Qt::LeftButton);
    EXPECT_EQ(0, samplers.at(13).triggers) << "EDIT: a tap selects, never plays";
    ASSERT_TRUE(page->sheetOpen());
    settle();
    // The pad is on the bottom row: the sheet docks over the top half.
    auto* sheet = page->findChild<QWidget*>(QStringLiteral("decklab-sampler-sheet"));
    EXPECT_LT(sheet->mapTo(page.get(), QPoint()).y() + sheet->height(), pad(13)->mapTo(page.get(), QPoint()).y());
    EXPECT_FALSE(pad(1)->isVisible());
    EXPECT_TRUE(pad(13)->isVisible());
    // MODE: LOOP.
    QTest::mouseClick(key("decklab-sampler-sheet-mode-1"), Qt::LeftButton);
    EXPECT_EQ(Mode::Loop, samplers.at(13).mode);
    EXPECT_TRUE(key("decklab-sampler-sheet-mode-1")->isChecked());
    EXPECT_FALSE(key("decklab-sampler-sheet-mode-0")->isChecked());
    EXPECT_EQ(Mode::Loop, pad(13)->state().mode);
    // GAIN: buttons and the knob, 1 dB a step, -12..+12 dB.
    QTest::mouseClick(key("decklab-sampler-sheet-gain-up"), Qt::LeftButton);
    EXPECT_DOUBLE_EQ(1, samplers.at(13).gain);
    EXPECT_TRUE(page->knob(decklab::KnobAction::Move, 2));
    EXPECT_DOUBLE_EQ(3, samplers.at(13).gain);
    EXPECT_EQ(QStringLiteral("+3 dB"), page->findChild<QLabel*>(QStringLiteral("decklab-sampler-sheet-gain"))->text());
    EXPECT_TRUE(page->knob(decklab::KnobAction::Move, -40));
    EXPECT_DOUBLE_EQ(-12, samplers.at(13).gain);
    EXPECT_EQ(0, pad(13)->state().level) << "the level bar follows the gain";
    // Press (or back) closes the sheet. r7-sampler: EDIT stays on until
    // its own key turns it off.
    EXPECT_TRUE(page->knob(decklab::KnobAction::Press, 1));
    EXPECT_FALSE(page->sheetOpen());
    EXPECT_TRUE(page->editMode());
    EXPECT_TRUE(key("decklab-sampler-edit")->isChecked());
    EXPECT_TRUE(pad(1)->isVisible());
    QTest::mouseClick(key("decklab-sampler-edit"), Qt::LeftButton);
    EXPECT_FALSE(page->editMode());
    EXPECT_FALSE(key("decklab-sampler-edit")->isChecked());
}

// r7-sampler: EDIT stays on across pads and sheets. While on, a tap on a
// loaded pad opens (or switches) its sheet and never plays it, gates
// included; an empty pad still loads from Browse. It ends on its own key or
// when the page hides; holding a pad still edits with EDIT off.
TEST_F(DeckLabSamplerTest, EditStaysOnAcrossPadsAndATapNeverPlays) {
    samplers.load(1, QStringLiteral("Generated Horn"), 3, 120);
    samplers.load(6, QStringLiteral("Generated Loop"), 8, 124);
    samplers.at(6).mode = Mode::Loop;
    samplers.load(11, QStringLiteral("Generated Gate"), 2);
    samplers.at(11).mode = Mode::Gate;
    settle();
    QSignalSpy load(page.get(), &DeckLabSampler::loadRequested);
    auto* edit = key("decklab-sampler-edit");
    QTest::mouseClick(edit, Qt::LeftButton);
    ASSERT_TRUE(page->editMode());
    EXPECT_TRUE(edit->isChecked()) << "EDIT is lit while it is on";
    EXPECT_TRUE(pad(1)->state().editing) << "loaded pads show the edit look";
    EXPECT_TRUE(pad(6)->state().editing);
    EXPECT_FALSE(pad(2)->state().editing) << "an empty pad keeps its +";
    EXPECT_TRUE(pad(1)->accessibleDescription().startsWith(QStringLiteral("EDIT is on: tap to edit")));
    // The look: a dashed outline and an EDIT mark where the hold dots were.
    const QImage editing = pad(1)->grab().toImage();
    int outline = 0;
    for (int x = 12; x < editing.width() - 12; ++x) {
        outline += editing.pixelColor(x, 3) != panel || editing.pixelColor(x, 4) != panel;
    }
    EXPECT_GT(outline, 20) << "the dashed outline along the top";
    capture(QStringLiteral("sampler-unit-edit-mode.png"));
    // Pad 1: its sheet, no sound.
    QTest::mouseClick(pad(1), Qt::LeftButton);
    ASSERT_TRUE(page->sheetOpen());
    EXPECT_EQ(1, page->sheetSampler());
    EXPECT_EQ(0, samplers.at(1).triggers);
    // Pad 6 (top half, still in view): the sheet switches, no sound.
    QTest::mouseClick(pad(6), Qt::LeftButton);
    EXPECT_EQ(6, page->sheetSampler());
    EXPECT_EQ(0, samplers.at(6).triggers);
    EXPECT_FALSE(samplers.at(6).playing);
    EXPECT_TRUE(page->editMode());
    EXPECT_EQ(QStringLiteral("EDIT on: tap\na pad to edit it"),
            page->findChild<QLabel*>(QStringLiteral("decklab-sampler-sheet-hint"))->text())
            << "the hint says what a tap does";
    // DONE closes the sheet; EDIT stays on for the next pad.
    QTest::mouseClick(key("decklab-sampler-sheet-done"), Qt::LeftButton);
    EXPECT_FALSE(page->sheetOpen());
    EXPECT_TRUE(page->editMode());
    EXPECT_TRUE(edit->isChecked());
    // A gate pad edits too and never sounds, held or not.
    QTest::mousePress(pad(11), Qt::LeftButton);
    EXPECT_FALSE(samplers.at(11).playing);
    EXPECT_EQ(11, page->sheetSampler());
    QTest::qWait(DeckLabSampler::kHoldMs + 150);
    QTest::mouseRelease(pad(11), Qt::LeftButton);
    EXPECT_EQ(0, samplers.at(11).triggers);
    // A long hold in EDIT never plays a pad either.
    QTest::mouseClick(key("decklab-sampler-sheet-done"), Qt::LeftButton);
    QTest::mousePress(pad(1), Qt::LeftButton);
    QTest::qWait(DeckLabSampler::kHoldMs + 150);
    QTest::mouseRelease(pad(1), Qt::LeftButton);
    EXPECT_EQ(0, samplers.at(1).triggers);
    QTest::mouseClick(key("decklab-sampler-sheet-done"), Qt::LeftButton);
    // An empty pad still opens Browse to load it (on release).
    QTest::mousePress(pad(2), Qt::LeftButton);
    EXPECT_EQ(0, load.count());
    QTest::mouseRelease(pad(2), Qt::LeftButton);
    ASSERT_EQ(1, load.count());
    EXPECT_EQ(2, load.takeFirst().at(0).toInt());
    // EDIT again: off, and pads play on touch-down as before.
    QTest::mouseClick(edit, Qt::LeftButton);
    EXPECT_FALSE(page->editMode());
    EXPECT_FALSE(edit->isChecked());
    EXPECT_FALSE(pad(1)->state().editing);
    EXPECT_EQ(QStringLiteral("Knob: gain\nBack: close"),
            page->findChild<QLabel*>(QStringLiteral("decklab-sampler-sheet-hint"))->text());
    QTest::mouseClick(pad(1), Qt::LeftButton);
    EXPECT_EQ(1, samplers.at(1).triggers);
    // Turning it off closes an open sheet; leaving the page ends EDIT.
    QTest::mouseClick(edit, Qt::LeftButton);
    QTest::mouseClick(pad(6), Qt::LeftButton);
    ASSERT_TRUE(page->sheetOpen());
    QTest::mouseClick(edit, Qt::LeftButton);
    EXPECT_FALSE(page->sheetOpen());
    QTest::mouseClick(edit, Qt::LeftButton);
    ASSERT_TRUE(page->editMode());
    page->hide();
    EXPECT_FALSE(page->editMode());
    page->show();
    settle();
    EXPECT_FALSE(key("decklab-sampler-edit")->isChecked());
    EXPECT_FALSE(pad(1)->state().editing);
    // Holding a pad still edits with EDIT off (the sample plays from the
    // touch, as before).
    samplers.at(1).playing = false;
    QTest::mousePress(pad(1), Qt::LeftButton);
    EXPECT_EQ(2, samplers.at(1).triggers);
    QTest::qWait(DeckLabSampler::kHoldMs + 150);
    QTest::mouseRelease(pad(1), Qt::LeftButton);
    EXPECT_TRUE(page->sheetOpen());
    EXPECT_FALSE(page->editMode()) << "a hold opens one sheet; it does not turn EDIT on";
}

// r7-sampler: REPLACE, beside CLEAR, sends Browse to load another sample into
// this pad ("Load pad N"). On release, never while the pad plays.
TEST_F(DeckLabSamplerTest, ReplaceLoadsAnotherSampleIntoThisPadFromBrowse) {
    samplers.load(7, QStringLiteral("Generated Vocal"), 4, 120);
    samplers.load(3, QStringLiteral("Generated Horn"), 3);
    settle();
    QSignalSpy load(page.get(), &DeckLabSampler::loadRequested);
    page->editPad(7);
    ASSERT_TRUE(page->sheetOpen());
    settle();
    auto* replace = key("decklab-sampler-sheet-replace");
    auto* clear = key("decklab-sampler-sheet-clear");
    ASSERT_NE(nullptr, replace);
    EXPECT_TRUE(replace->isVisible());
    EXPECT_TRUE(replace->isEnabled());
    EXPECT_GE(replace->height(), 44);
    EXPECT_GE(replace->width(), 44);
    EXPECT_EQ(clear->y(), replace->y()) << "beside CLEAR";
    EXPECT_LT(replace->x(), clear->x());
    auto* sheet = page->findChild<QWidget*>(QStringLiteral("decklab-sampler-sheet"));
    EXPECT_TRUE(QRect(QPoint(), sheet->size()).contains(QRect(replace->mapTo(sheet, QPoint()), replace->size())));
    EXPECT_EQ(QStringLiteral("Replace pad 7: load another sample from Browse"), replace->accessibleName());
    capture(QStringLiteral("sampler-unit-replace.png"));
    QTest::mousePress(replace, Qt::LeftButton);
    EXPECT_EQ(0, load.count()) << "nothing happens on touch-down";
    QTest::mouseRelease(replace, Qt::LeftButton);
    ASSERT_EQ(1, load.count());
    EXPECT_EQ(7, load.takeFirst().at(0).toInt()) << "Browse loads into this pad";
    EXPECT_EQ(0, samplers.at(7).ejects) << "the sample stays until the new one loads";
    // Never while the pad plays.
    samplers.trigger(7);
    tick();
    EXPECT_FALSE(replace->isEnabled());
    EXPECT_FALSE(clear->isEnabled());
    EXPECT_EQ(QStringLiteral("Replace pad 7: stop it first"), replace->accessibleName());
    QTest::mouseClick(replace, Qt::LeftButton);
    EXPECT_EQ(0, load.count());
    // The sheet follows its pad: pad 3's REPLACE loads pad 3.
    samplers.stop(7);
    page->editPad(3);
    tick();
    QTest::mouseClick(key("decklab-sampler-sheet-replace"), Qt::LeftButton);
    ASSERT_EQ(1, load.count());
    EXPECT_EQ(3, load.takeFirst().at(0).toInt());
}

TEST_F(DeckLabSamplerTest, LoopPadsStopOnASecondTouchAndGatePadsPlayWhileHeld) {
    samplers.load(5, QStringLiteral("Generated Loop"), 4, 128);
    samplers.at(5).mode = Mode::Loop;
    samplers.load(6, QStringLiteral("Generated Gate"), 4);
    samplers.at(6).mode = Mode::Gate;
    settle();
    QTest::mouseClick(pad(5), Qt::LeftButton);
    EXPECT_TRUE(samplers.at(5).playing);
    QTest::mouseClick(pad(5), Qt::LeftButton);
    EXPECT_FALSE(samplers.at(5).playing) << "a second touch stops a loop";
    EXPECT_EQ(1, samplers.at(5).triggers);
    EXPECT_EQ(1, samplers.at(5).stops);
    QTest::mousePress(pad(6), Qt::LeftButton);
    EXPECT_TRUE(samplers.at(6).playing);
    QTest::qWait(DeckLabSampler::kHoldMs + 150);
    EXPECT_FALSE(page->sheetOpen()) << "holding a gate plays it; it never opens the sheet";
    EXPECT_TRUE(samplers.at(6).playing);
    QTest::mouseRelease(pad(6), Qt::LeftButton);
    EXPECT_FALSE(samplers.at(6).playing) << "a gate stops on release";
    // A gate whose release never arrives stops at the next touch elsewhere.
    QTest::mousePress(pad(6), Qt::LeftButton);
    EXPECT_TRUE(samplers.at(6).playing);
    QTest::mousePress(key("decklab-sampler-qnt"), Qt::LeftButton);
    EXPECT_FALSE(samplers.at(6).playing);
    QTest::mouseRelease(key("decklab-sampler-qnt"), Qt::LeftButton);
    // ...and when the page hides.
    QTest::mousePress(pad(6), Qt::LeftButton);
    EXPECT_TRUE(samplers.at(6).playing);
    page->hide();
    EXPECT_FALSE(samplers.at(6).playing);
    page->show();
    QTest::mouseRelease(pad(6), Qt::LeftButton);
    EXPECT_FALSE(samplers.at(6).playing);
}

TEST_F(DeckLabSamplerTest, SideKeysToggleQuantizeSyncLevelAndTheLabFxSend) {
    QTest::mouseClick(key("decklab-sampler-qnt"), Qt::LeftButton);
    EXPECT_FALSE(samplers.quantizing);
    EXPECT_FALSE(key("decklab-sampler-qnt")->isChecked());
    QTest::mouseClick(key("decklab-sampler-sync"), Qt::LeftButton);
    EXPECT_TRUE(samplers.syncing);
    EXPECT_TRUE(key("decklab-sampler-sync")->isChecked()) << "on is a filled lamp";
    EXPECT_EQ(lit, pixel(key("decklab-sampler-sync"), 3, key("decklab-sampler-sync")->height() / 2));
    QSignalSpy fx(page.get(), &DeckLabSampler::fxRoutingRequested);
    QTest::mouseClick(key("decklab-sampler-fx"), Qt::LeftButton);
    ASSERT_EQ(1, fx.count());
    EXPECT_TRUE(fx.at(0).at(0).toBool());
    EXPECT_TRUE(key("decklab-sampler-fx")->isChecked());
    // Lab FX can turn the send off from its own page.
    page->setFxRouting(false);
    EXPECT_FALSE(key("decklab-sampler-fx")->isChecked());
    // LEVEL moves with the finger from where it lands, never jumping.
    auto* level = page->findChild<DeckLabSamplerLevel*>(QStringLiteral("decklab-sampler-level"));
    ASSERT_NE(nullptr, level);
    EXPECT_EQ(100, level->value());
    const QPoint middle(level->width() / 2, level->height() / 2);
    QTest::mousePress(level, Qt::LeftButton, Qt::NoModifier, middle);
    EXPECT_DOUBLE_EQ(1.0, samplers.fader) << "a touch alone changes nothing";
    QTest::mouseMove(level, middle + QPoint(0, level->height() / 4));
    QTest::mouseRelease(level, Qt::LeftButton, Qt::NoModifier, middle + QPoint(0, level->height() / 4));
    EXPECT_LT(samplers.fader, 0.85);
    EXPECT_GT(samplers.fader, 0.4);
}

TEST_F(DeckLabSamplerTest, StopAllStopsEveryPlayingPadInEveryBank) {
    samplers.samplers = 32;
    samplers.notify(0);
    samplers.load(3, QStringLiteral("Generated A"), 4);
    samplers.load(20, QStringLiteral("Generated B"), 4);
    settle();
    samplers.trigger(3);
    samplers.trigger(20);
    tick();
    auto* stop = key("decklab-samples-stop");
    EXPECT_TRUE(stop->isEnabled());
    QTest::mouseClick(stop, Qt::LeftButton);
    EXPECT_FALSE(samplers.at(3).playing);
    EXPECT_FALSE(samplers.at(20).playing) << "a pad in another bank stops too";
    tick();
    EXPECT_FALSE(stop->isEnabled());
}

TEST_F(DeckLabSamplerTest, MoreThanSixteenSamplersBecomeBanks) {
    samplers.samplers = 20;
    samplers.notify(0);
    settle();
    auto* bank = key("decklab-sampler-bank");
    ASSERT_TRUE(bank->isVisible());
    EXPECT_EQ(QStringLiteral("BANK A"), bank->text());
    EXPECT_GE(bank->height(), 44);
    QTest::mouseClick(bank, Qt::LeftButton);
    EXPECT_EQ(1, page->bank());
    EXPECT_EQ(QStringLiteral("BANK B"), bank->text());
    EXPECT_EQ(QStringLiteral("Pad 17, empty: tap to load"), pad(1)->accessibleName()) << "numbered 1-64 everywhere";
    EXPECT_TRUE(pad(4)->isEnabled());
    EXPECT_FALSE(pad(5)->isEnabled()) << "sampler 21 is not configured";
    QSignalSpy load(page.get(), &DeckLabSampler::loadRequested);
    QTest::mouseClick(pad(1), Qt::LeftButton);
    ASSERT_EQ(1, load.count());
    EXPECT_EQ(17, load.at(0).at(0).toInt());
    QTest::mouseClick(pad(5), Qt::LeftButton);
    EXPECT_EQ(1, load.count());
    capture(QStringLiteral("sampler-unit-bank-b.png"));
    QTest::mouseClick(bank, Qt::LeftButton);
    EXPECT_EQ(0, page->bank());
    // Lab FX (or anyone) can open the bank that holds a pad.
    page->showPad(18);
    EXPECT_EQ(1, page->bank());
}

TEST_F(DeckLabSamplerTest, DecksStripShowsEachDeckAndTakesYouBackToDecks) {
    auto* strip = page->findChild<DeckLabSamplerDecks*>(QStringLiteral("decklab-sampler-decks"));
    ASSERT_NE(nullptr, strip);
    EXPECT_EQ((QList<int>{1, 2}), strip->shownDecks());
    std::array<DeckLabSamplerDeck, 4> decks{};
    decks[0] = {true, true, true, false, 128.0, 0.3, 205, 2};
    decks[1] = {true, false, false, false, 126.4, 0.6, 32, 0};
    tick(decks);
    EXPECT_EQ((QList<int>{1, 2}), strip->shownDecks());
    EXPECT_GE(strip->cellRect(1).width(), 300) << "two decks: wide cells";
    // The current beat of deck 1 lights in its color: one 14 px lamp more
    // of deck 1's cyan than with no beat grid.
    const QRect cell = strip->cellRect(1);
    const auto cyan = [strip, cell] {
        const QImage image = strip->grab().toImage();
        int count = 0;
        for (int x = cell.left(); x < cell.right(); ++x) {
            count += image.pixelColor(x, cell.center().y()) == QColor(0x35, 0xc8, 0xf0);
        }
        return count;
    };
    const int withBeat = cyan();
    capture(QStringLiteral("sampler-unit-decks.png"));
    decks[0].phase = -1;
    tick(decks);
    EXPECT_EQ(14, withBeat - cyan());
    decks[0].phase = 2;
    tick(decks);
    // An unchanged tick repaints nothing; a new beat repaints one cell.
    PaintCounter counter;
    strip->installEventFilter(&counter);
    tick(decks);
    EXPECT_EQ(0, counter.count);
    decks[0].phase = 3;
    tick(decks);
    EXPECT_EQ(1, counter.count);
    // Decks 3 and 4 get cells while loaded.
    decks[2] = {true, true, false, false, 128.0, 0.1, 300, 1};
    tick(decks);
    EXPECT_EQ((QList<int>{1, 2, 3}), strip->shownDecks());
    QSignalSpy back(page.get(), &DeckLabSampler::decksRequested);
    QTest::mouseClick(strip, Qt::LeftButton);
    EXPECT_EQ(1, back.count());
}

TEST_F(DeckLabSamplerTest, HidingThePageEndsHoldsAndClosesTheSheet) {
    samplers.load(1, QStringLiteral("Generated Horn"), 3);
    settle();
    page->editPad(1);
    ASSERT_TRUE(page->sheetOpen());
    page->hide();
    EXPECT_FALSE(page->sheetOpen());
    page->show();
    settle();
    for (int index = 1; index <= 16; ++index) {
        EXPECT_TRUE(pad(index)->isVisible()) << index;
    }
    // A hold that outlives the page never opens the sheet.
    QTest::mousePress(pad(1), Qt::LeftButton);
    page->hide();
    page->show();
    QTest::qWait(DeckLabSampler::kHoldMs + 150);
    EXPECT_FALSE(page->sheetOpen());
    QTest::mouseRelease(pad(1), Qt::LeftButton);
}

} // namespace
