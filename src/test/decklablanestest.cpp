// SPDX-License-Identifier: GPL-2.0-or-later
// r4-waveforms: the compact waveform lanes on Sampler and Lab FX, against
// generated data (no engine, no audio, no GL): where lanes show, which decks
// get the two lanes, the DECKS row under them, and the two pages laid out in
// the 800x270 under the lanes. NativeDeckLabTest covers the real surface.
#include <gtest/gtest.h>

#include <QApplication>
#include <QHash>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QTest>
#include <memory>
#include <vector>

#include "widget/decklablabfx.h"
#include "widget/decklablanes.h"
#include "widget/decklabsampler.h"

namespace {

using decklab::lanes::Choice;
using decklab::lanes::Deck;
using decklab::lanes::Presentation;

void settle() {
    for (int pass = 0; pass < 5; ++pass) {
        QCoreApplication::processEvents();
    }
}

QRect area(QWidget* widget, QWidget* root) {
    return QRect(widget->mapTo(root, QPoint()), widget->size());
}

TEST(DeckLabLanesTest, PagesShowLanesFromOnePlace) {
    EXPECT_EQ(Presentation::Full, decklab::lanes::presentationFor(0));    // Decks
    EXPECT_EQ(Presentation::None, decklab::lanes::presentationFor(1));    // Browse: the deck dock
    EXPECT_EQ(Presentation::Compact, decklab::lanes::presentationFor(2)); // Sampler
    EXPECT_EQ(Presentation::Compact, decklab::lanes::presentationFor(3)); // Lab FX
    EXPECT_EQ(Presentation::None, decklab::lanes::presentationFor(4));    // Settings
    EXPECT_EQ(Presentation::None, decklab::lanes::presentationFor(-1));
}

TEST(DeckLabLanesTest, CompactLanesKeepThePageItsRoom) {
    // 800x480: 436 px under the rail; two 66 px lanes, a 2 px gap and the
    // 32 px DECKS row leave the page 270 px.
    EXPECT_EQ(66, decklab::lanes::compactLaneHeight(436));
    EXPECT_EQ(166, decklab::lanes::compactZoneHeight(436));
    EXPECT_GE(436 - decklab::lanes::compactZoneHeight(436), decklab::lanes::kPageMinimum);
    // 1024x600: lanes stop at 96 px; the page gets the rest.
    EXPECT_EQ(96, decklab::lanes::compactLaneHeight(556));
    EXPECT_EQ(226, decklab::lanes::compactZoneHeight(556));
    // Never under 56 px, however small the window.
    EXPECT_EQ(56, decklab::lanes::compactLaneHeight(300));
}

Choice pick(std::array<Deck, 4> decks, std::array<int, 2> layers, bool four) {
    return decklab::lanes::choose(decks, layers, four);
}

TEST(DeckLabLanesTest, LanesGoToDecksOnAirThenTheLayers) {
    const Deck loaded{true, false};
    const Deck onAir{true, true};
    const Deck empty{};
    // Two decks, nothing on air: the Decks page's two decks, left side on top.
    Choice two = pick({loaded, loaded, empty, empty}, {1, 2}, false);
    EXPECT_EQ((std::array<int, 2>{1, 2}), two.lanes);
    EXPECT_EQ(2, two.rowCount);
    // The controller's left layer on deck 3: deck 3 on top.
    two = pick({loaded, loaded, loaded, empty}, {3, 2}, false);
    EXPECT_EQ((std::array<int, 2>{3, 2}), two.lanes);
    // Four decks, decks 1 and 3 on air with the layers on 1 and 2: both
    // on-air decks get a lane (same side: by number), the row names all four.
    Choice four = pick({onAir, loaded, onAir, empty}, {1, 2}, true);
    EXPECT_EQ((std::array<int, 2>{1, 3}), four.lanes);
    EXPECT_EQ(4, four.rowCount);
    EXPECT_EQ((std::array<int, 4>{1, 2, 3, 4}), four.row);
    // Decks 2 and 3 on air: deck 3 (left side) on top of deck 2.
    four = pick({loaded, onAir, onAir, loaded}, {1, 2}, true);
    EXPECT_EQ((std::array<int, 2>{3, 2}), four.lanes);
    // Two-deck layout with a third deck on air: it joins the row.
    two = pick({onAir, onAir, onAir, empty}, {1, 2}, false);
    EXPECT_EQ((std::array<int, 2>{1, 2}), two.lanes);
    EXPECT_EQ(3, two.rowCount);
    EXPECT_EQ(3, two.row[2]);
}

TEST(DeckLabLanesTest, ADeckOnAirIsNeverOutOfSight) {
    // Every combination of loaded/playing decks, layers and layouts.
    const std::array<std::array<int, 2>, 4> layerSets{{{1, 2}, {3, 2}, {1, 4}, {3, 4}}};
    for (int bits = 0; bits < 256; ++bits) {
        std::array<Deck, 4> decks{};
        for (int deck = 0; deck < 4; ++deck) {
            decks[deck].loaded = bits & (1 << deck);
            decks[deck].playing = bits & (1 << (deck + 4));
        }
        for (const auto& layers : layerSets) {
            for (const bool four : {false, true}) {
                const Choice choice = pick(decks, layers, four);
                EXPECT_NE(choice.lanes[0], choice.lanes[1]);
                for (int deck = 1; deck <= 4; ++deck) {
                    if (!decks[deck - 1].loaded || !decks[deck - 1].playing) {
                        continue;
                    }
                    bool onRow = false;
                    for (int index = 0; index < choice.rowCount; ++index) {
                        onRow |= choice.row[index] == deck;
                    }
                    EXPECT_TRUE(choice.hasLane(deck) || onRow) << bits << " deck " << deck;
                }
                // Lane decks are always on the row too (their readout).
                for (const int deck : choice.lanes) {
                    bool onRow = false;
                    for (int index = 0; index < choice.rowCount; ++index) {
                        onRow |= choice.row[index] == deck;
                    }
                    EXPECT_TRUE(onRow) << bits;
                }
            }
        }
    }
}

DeckLabStripState state(int deck, bool playing, const QString& bpm, const QString& remaining, int phase) {
    DeckLabStripState s;
    s.deck = deck;
    s.loaded = true;
    s.playing = playing;
    s.bpm = bpm;
    s.remaining = remaining;
    s.phase = phase;
    return s;
}

TEST(DeckLabLanesTest, DecksRowRepaintsOnlyChangedCells) {
    DeckLabLaneDecks row;
    row.resize(800, decklab::lanes::kRowHeight);
    row.show();
    settle();
    EXPECT_EQ(32, row.height());
    EXPECT_TRUE(row.testAttribute(Qt::WA_TransparentForMouseEvents)) << "display only";
    Choice choice;
    choice.lanes = {1, 2};
    choice.row = {1, 2, 0, 0};
    choice.rowCount = 2;
    row.setChoice(choice);
    EXPECT_EQ(QRect(0, 0, 400, 32), row.cellRect(1));
    EXPECT_EQ(QRect(400, 0, 400, 32), row.cellRect(2));
    EXPECT_TRUE(row.cellRect(3).isEmpty());
    const QString minus = QString::fromUtf8("−");
    row.setDeck(1, state(1, true, QStringLiteral("128.0"), minus + QStringLiteral("3:25"), 0), .4);
    row.setDeck(2, state(2, false, QStringLiteral("126.0"), minus + QStringLiteral("5:02"), 2), .1);
    settle();
    const int before = row.cellUpdates();
    // The same values again: nothing repaints.
    row.setDeck(1, state(1, true, QStringLiteral("128.0"), minus + QStringLiteral("3:25"), 0), .4);
    EXPECT_EQ(before, row.cellUpdates());
    // A deck that is not on the row costs nothing.
    row.setDeck(3, state(3, true, QStringLiteral("124.0"), minus + QStringLiteral("1:00"), 1), .5);
    EXPECT_EQ(before, row.cellUpdates());
    // A new second: one cell.
    row.setDeck(1, state(1, true, QStringLiteral("128.0"), minus + QStringLiteral("3:24"), 1), .4);
    EXPECT_EQ(before + 1, row.cellUpdates());
    // The blink touches only cells inside the end warning.
    row.setFlash(true);
    EXPECT_EQ(before + 1, row.cellUpdates());
    DeckLabStripState ending = state(2, true, QStringLiteral("126.0"), minus + QStringLiteral("0:18"), 3);
    ending.endWarning = true;
    row.setDeck(2, ending, .97);
    EXPECT_EQ(before + 2, row.cellUpdates());
    row.setFlash(false);
    EXPECT_EQ(before + 3, row.cellUpdates());
    // Four decks: four 200 px cells.
    choice.row = {1, 2, 3, 4};
    choice.rowCount = 4;
    row.setChoice(choice);
    for (int deck = 1; deck <= 4; ++deck) {
        EXPECT_EQ(200, row.cellRect(deck).width()) << deck;
    }
}

TEST(DeckLabLanesTest, DecksRowPaintsLaneBarsAndRedOnlyAtTheEnd) {
    DeckLabLaneDecks row;
    row.resize(800, decklab::lanes::kRowHeight);
    row.show();
    Choice choice;
    choice.lanes = {1, 3};
    choice.row = {1, 2, 3, 4};
    choice.rowCount = 4;
    row.setChoice(choice);
    const QString minus = QString::fromUtf8("−");
    for (int deck = 1; deck <= 3; ++deck) {
        row.setDeck(deck, state(deck, deck != 2, QStringLiteral("127.0"), minus + QStringLiteral("2:00"), deck), .5);
    }
    settle();
    QImage image = row.grab().toImage();
    // Decks with a lane carry their colour along the top; the others don't.
    EXPECT_EQ(deckLabDeckColor(1), image.pixelColor(row.cellRect(1).center().x(), 1));
    EXPECT_EQ(deckLabDeckColor(3), image.pixelColor(row.cellRect(3).center().x(), 1));
    EXPECT_NE(deckLabDeckColor(2), image.pixelColor(row.cellRect(2).center().x(), 1));
    const QColor live(0xff, 0x3d, 0x2e);
    const auto reds = [&live](const QImage& picture) {
        int count = 0;
        for (int y = 0; y < picture.height(); ++y) {
            for (int x = 0; x < picture.width(); ++x) {
                count += picture.pixelColor(x, y) == live;
            }
        }
        return count;
    };
    EXPECT_EQ(0, reds(image)) << "live red only for the end of a track";
    DeckLabStripState ending = state(3, true, QStringLiteral("127.0"), minus + QStringLiteral("0:18"), 3);
    ending.endWarning = true;
    row.setDeck(3, ending, .97);
    settle();
    EXPECT_GT(reds(row.grab().toImage()), 0);
}

// Generated samplers for the Sampler page.
class Samplers final : public DeckLabSamplerEngine {
  public:
    struct Pad {
        bool loaded = false;
        QString title;
        bool playing = false;
        double bpm = 0;
    };
    using DeckLabSamplerEngine::notify;
    std::vector<Pad> pads = std::vector<Pad>(16);
    int count() const override {
        return 16;
    }
    bool loaded(int s) const override {
        return pads[s - 1].loaded;
    }
    QString title(int s) const override {
        return pads[s - 1].title;
    }
    bool playing(int s) const override {
        return pads[s - 1].loaded && pads[s - 1].playing;
    }
    double position(int) const override {
        return .3;
    }
    double duration(int) const override {
        return 8;
    }
    double rate(int) const override {
        return 1;
    }
    double bpm(int s) const override {
        return pads[s - 1].bpm;
    }
    double gainDb(int) const override {
        return 0;
    }
    Mode mode(int) const override {
        return Mode::OneShot;
    }
    bool quantize() const override {
        return true;
    }
    bool syncLoops() const override {
        return false;
    }
    double level() const override {
        return 1;
    }
    double levelDb() const override {
        return 0;
    }
    void trigger(int s) override {
        pads[s - 1].playing = true;
        notify(s);
    }
    void stop(int s) override {
        pads[s - 1].playing = false;
        notify(s);
    }
    void eject(int s) override {
        pads[s - 1] = Pad{};
        notify(s);
    }
    void setGainDb(int, double) override {
    }
    void setMode(int, Mode) override {
    }
    void setQuantize(bool) override {
    }
    void setSyncLoops(bool) override {
    }
    void setLevel(double) override {
    }
};

TEST(DeckLabLanesTest, SamplerUnderTheLanesKeepsItsPadsAndSheet) {
    Samplers samplers;
    samplers.pads[0] = {true, QStringLiteral("Signal Fire"), true, 124};
    samplers.pads[5] = {true, QStringLiteral("Demo Horn With A Long Generated Name"), false, 0};
    samplers.pads[10] = {true, QStringLiteral("Low Tide"), false, 118};
    DeckLabSampler page(&samplers);
    page.setDecksStripShown(false);
    EXPECT_FALSE(page.decksStripShown());
    // The page under compact lanes at 800x480.
    page.resize(800, 270);
    page.show();
    settle();
    page.tick({});
    settle();
    auto* strip = page.findChild<QWidget*>(QStringLiteral("decklab-sampler-decks"));
    ASSERT_NE(nullptr, strip);
    EXPECT_FALSE(strip->isVisible());
    for (int index = 0; index < 16; ++index) {
        auto* pad = page.pad(index);
        EXPECT_GE(pad->height(), 56) << index;
        EXPECT_GE(pad->width(), 160) << index;
        EXPECT_TRUE(page.rect().contains(area(pad, &page))) << index;
    }
    // 4 px between rows (a hidden sheet once swallowed them).
    for (int row = 1; row < 4; ++row) {
        EXPECT_GE(area(page.pad(4 * row), &page).top() - area(page.pad(4 * (row - 1)), &page).bottom(), 5) << row;
    }
    // A 62 px pad keeps its title on the number's line, readable.
    const QImage padImage = page.pad(5)->grab().toImage();
    EXPECT_EQ(page.pad(5)->height(), padImage.height());
    // The sheet fits two rows of pads, away from the pad, with 44 px keys.
    for (const int sampler : {6, 11}) {
        page.editPad(sampler);
        settle();
        auto* sheet = page.findChild<QWidget*>(QStringLiteral("decklab-sampler-sheet"));
        ASSERT_NE(nullptr, sheet);
        ASSERT_TRUE(sheet->isVisible()) << sampler;
        const QRect sheetArea = area(sheet, &page);
        EXPECT_TRUE(page.rect().contains(sheetArea)) << sampler;
        EXPECT_FALSE(sheetArea.intersects(area(page.pad(sampler - 1), &page))) << "the pad stays in view";
        for (auto* key : sheet->findChildren<QPushButton*>()) {
            if (!key->isVisible()) {
                continue;
            }
            EXPECT_GE(key->height(), 44) << key->objectName().toStdString();
            EXPECT_TRUE(sheetArea.contains(area(key, &page))) << key->objectName().toStdString();
        }
        // STOP and CLEAR at opposite corners.
        auto* stop = page.findChild<QPushButton*>(QStringLiteral("decklab-sampler-sheet-stop"));
        auto* clear = page.findChild<QPushButton*>(QStringLiteral("decklab-sampler-sheet-clear"));
        ASSERT_TRUE(stop && clear);
        EXPECT_LT(area(stop, &page).right(), area(clear, &page).left());
        EXPECT_LT(area(stop, &page).bottom(), area(clear, &page).top());
        // The title is elided to its own room, never under GAIN.
        auto* title = page.findChild<QLabel*>(QStringLiteral("decklab-sampler-sheet-title"));
        ASSERT_NE(nullptr, title);
        EXPECT_LE(title->fontMetrics().horizontalAdvance(title->text()), title->width()) << title->text().toStdString();
        page.closeSheet();
        settle();
    }
    // The strip is hidden, so the tick never touches it.
    std::array<DeckLabSamplerDeck, 4> decks{};
    decks[0].loaded = true;
    decks[0].bpm = 128;
    page.tick(decks);
    EXPECT_FALSE(static_cast<DeckLabSamplerDecks*>(strip)->decks()[0].loaded);
}

struct FakeFx {
    QHash<QString, double> values;
    qint64 clock = 1000;
    DeckLabLabFxHost host() {
        DeckLabLabFxHost h;
        h.value = [this](const QString& g, const QString& i) {
            return values.value(g + QLatin1Char(',') + i);
        };
        h.set = [this](const QString& g, const QString& i, double v) {
            values[g + QLatin1Char(',') + i] = v;
        };
        h.setParameter = [](const QString&, const QString&, double) {};
        h.unitReady = [](int) {
            return true;
        };
        h.now = [this] {
            return clock;
        };
        h.deck = [](int deck) {
            DeckLabLabFxDeck d;
            d.loaded = deck == 1;
            d.playing = deck == 1;
            d.title = QStringLiteral("Generated Track A");
            d.remaining = QString::fromUtf8("−0:18");
            d.endWarning = true;
            d.phase = 2;
            return d;
        };
        h.showPage = [](int) {};
        return h;
    }
};

TEST(DeckLabLanesTest, LabFxUnderTheLanesKeepsItsKeysAndNamesTheTarget) {
    FakeFx fake;
    fake.values[QStringLiteral("[Channel1],track_loaded")] = 1;
    fake.values[QStringLiteral("[Channel1],bpm")] = 128;
    fake.values[QStringLiteral("[Channel1],play")] = 1;
    DeckLabLabFx fx(fake.host());
    fx.setLanesShown(true);
    EXPECT_TRUE(fx.lanesShown());
    fx.resize(800, 270);
    fx.show();
    settle();
    fx.refresh();
    fake.clock += 100;
    fx.tick();
    settle();
    QList<QPushButton*> keys;
    for (auto* key : fx.findChildren<QPushButton*>()) {
        if (key->isVisible()) {
            keys << key;
        }
    }
    EXPECT_GE(keys.size(), 20);
    for (auto* key : keys) {
        EXPECT_GE(key->height(), 44) << key->objectName().toStdString();
        EXPECT_GE(key->width(), 44) << key->objectName().toStdString();
        EXPECT_TRUE(fx.rect().contains(area(key, &fx))) << key->objectName().toStdString();
        for (auto* other : keys) {
            if (other != key && !other->isAncestorOf(key) && !key->isAncestorOf(other)) {
                EXPECT_FALSE(area(key, &fx).adjusted(-3, -3, 3, 3).intersects(area(other, &fx)))
                        << key->objectName().toStdString() << " / " << other->objectName().toStdString();
            }
        }
    }
    EXPECT_GE(fx.pad()->height(), 96) << "the X-PAD keeps room under the lanes";
    // The footer names the target; the beat and the (red) clock are on the
    // DECKS row above, so the footer paints no live red.
    auto* footer = fx.findChild<QPushButton*>(QStringLiteral("decklab-fx-deck"));
    ASSERT_NE(nullptr, footer);
    const QImage image = footer->grab().toImage();
    int reds = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor c = image.pixelColor(x, y);
            reds += c.red() > 200 && c.green() < 90 && c.blue() < 80;
        }
    }
    EXPECT_EQ(0, reds);
    fx.setLanesShown(false);
    settle();
    const QImage full = footer->grab().toImage();
    int fullReds = 0;
    for (int y = 0; y < full.height(); ++y) {
        for (int x = 0; x < full.width(); ++x) {
            const QColor c = full.pixelColor(x, y);
            fullReds += c.red() > 200 && c.green() < 90 && c.blue() < 80;
        }
    }
    EXPECT_GT(fullReds, 0) << "on its own the footer shows the end-warning clock";
}

} // namespace

#ifdef DECKLAB_LANES_STANDALONE_TEST
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
