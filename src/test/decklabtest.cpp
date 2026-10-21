// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDir>
#include <QLabel>
#include <QPushButton>
#include <QTest>

#include "control/controlobject.h"
#include "effects/effectchain.h"
#include "effects/effectslot.h"
#include "library/libraryfeature.h"
#include "library/sidebarmodel.h"
#include "library/treeitem.h"
#include "test/signalpathtest.h"
#include "waveform/waveformwidgetfactory.h"
#include "widget/decklabtempopresenter.h"
#include "widget/wdecklab.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarysidebar.h"
#include "widget/wsearchlineedit.h"
#include "widget/wwaveformviewer.h"

namespace {
class TestSource final : public LibraryFeature {
  public:
    explicit TestSource(UserSettingsPointer config) : LibraryFeature(nullptr, config, QString()) {
        auto* root = model.setRootItem(TreeItem::newRoot(this));
        root->appendChild("Genres")->appendChild("House");
    }
    QVariant title() override { return QStringLiteral("USB library"); }
    TreeItemModel* sidebarModel() const override { return const_cast<TreeItemModel*>(&model); }
    void activate() override { ++activations; }
    void activateChild(const QModelIndex&) override { ++activations; }
    int activations = 0;
    TreeItemModel model;
};
}

// Native controls and the actual SidebarModel/WLibrary widgets; no desktop,
// fake browser, audio hardware, Pi configuration, or personal media involved.
class NativeDeckLabTest : public BaseSignalPathTest {
  protected:
    void SetUp() override {
        BaseSignalPathTest::SetUp();
        WaveformWidgetFactory::instance()->setConfig(config());
        source = std::make_unique<TestSource>(config());
        model = std::make_unique<SidebarModel>(); model->addLibraryFeature(source.get());
        panel = std::make_unique<WDeckLab>(config(), nullptr, nullptr, m_pEffectsManager.get());
        sidebar = new WLibrarySidebar(panel.get()); sidebar->setModel(model.get());
        library = new WLibrary(panel.get());
        panel->bindBrowser(new WSearchLineEdit(panel.get(), config()), sidebar, library);
        // Empty native viewers exercise screen geometry. Their empty rendering
        // is not evidence of waveform pacing or physical touchscreen behavior.
        for (int deck = 1; deck <= 3; ++deck) panel->addWaveform(
                new WWaveformViewer(QStringLiteral("[Channel%1]").arg(deck), config(), panel.get()), deck);
        panel->resize(800, 480); panel->show(); QCoreApplication::processEvents();
    }
    void TearDown() override {
        panel.reset(); model.reset(); source.reset(); BaseSignalPathTest::TearDown();
    }
    std::unique_ptr<TestSource> source;
    std::unique_ptr<SidebarModel> model;
    std::unique_ptr<WDeckLab> panel;
    WLibrarySidebar* sidebar = nullptr;
    WLibrary* library = nullptr;
};

TEST_F(NativeDeckLabTest, FirstBrowsePressDoesNotOpenSource) {
    panel->browsePress();
    EXPECT_EQ(1, panel->currentPage());
    EXPECT_FALSE(sidebar->rootIndex().isValid());
    EXPECT_EQ(0, source->activations);
    panel->browsePress();
    EXPECT_TRUE(sidebar->rootIndex().isValid());
    EXPECT_EQ(1, source->activations);
}

TEST_F(NativeDeckLabTest, SingleTapOpensFolderAndBackRestoresSelection) {
    panel->browsePress(); panel->browsePress();
    const QPersistentModelIndex genres = model->index(0, 0, sidebar->rootIndex());
    sidebar->selectIndex(genres); QCoreApplication::processEvents();
    const auto point = sidebar->visualRect(genres).center();
    QTest::mouseClick(sidebar->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QCoreApplication::processEvents();
    EXPECT_EQ(genres, sidebar->rootIndex());
    EXPECT_EQ(2, source->activations);
    panel->browsePress(); // House is a leaf and must display tracks, not an empty folder.
    EXPECT_TRUE(library->isVisible());
    panel->browseBack(); EXPECT_TRUE(sidebar->isVisible());
    panel->browseBack(); EXPECT_EQ(genres, sidebar->currentIndex());
    EXPECT_EQ(genres.parent(), sidebar->rootIndex());
}

TEST_F(NativeDeckLabTest, PlayingDeckCannotBeReplaced) {
    panel->selectPage(1); panel->browsePress(); panel->browsePress(); panel->browsePress();
    auto* load = panel->findChild<QPushButton*>("decklab-load-1"); ASSERT_NE(nullptr, load);
    ControlObject::set(ConfigKey("[Channel1]", "play"), 1); QCoreApplication::processEvents();
    EXPECT_FALSE(load->isEnabled());
    load->click(); EXPECT_EQ(1, panel->currentPage());
}

TEST_F(NativeDeckLabTest, ToolRequestCanRepeatWithoutTimingRace) {
    auto* usb = panel->findChild<QPushButton*>("decklab-usb"); ASSERT_NE(nullptr, usb);
    usb->click(); const double serial = ControlObject::get(ConfigKey("[DeckLabNative]", "tool_serial"));
    usb->click(); EXPECT_EQ(serial + 1, ControlObject::get(ConfigKey("[DeckLabNative]", "tool_serial")));
    EXPECT_EQ(1, ControlObject::get(ConfigKey("[DeckLabNative]", "tool_request")));
}

TEST_F(NativeDeckLabTest, PagesFit800By480) {
    const QString output = qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
    for (int page = 0; page < 5; ++page) {
        panel->selectPage(page); QCoreApplication::processEvents();
        EXPECT_EQ(QSize(800, 480), panel->size()) << page;
        for (auto* button : panel->findChildren<QPushButton*>()) {
            if (!button->isVisible()) continue;
            QRect bounds(button->mapTo(panel.get(), QPoint()), button->size());
            EXPECT_TRUE(panel->rect().contains(bounds)) << qPrintable(button->text()) << " page " << page;
            EXPECT_GE(button->height(), 38) << qPrintable(button->text());
        }
        if (!output.isEmpty()) EXPECT_TRUE(panel->grab().save(QDir(output).filePath(QStringLiteral("screen-%1.png").arg(page))));
    }
}

TEST_F(NativeDeckLabTest, IndependentRmxDoesNotBorrowControllerChains) {
    m_pEffectsManager->setup();
    auto standard = m_pEffectsManager->getEffectChain("[EffectRack1_EffectUnit3]");
    ASSERT_FALSE(standard.isNull());
    ControlObject::set(ConfigKey("[EffectRack1_EffectUnit3]", "mix"), .63);
    m_pEffectsManager->ensureDeckLabEffects();
    auto native = m_pEffectsManager->getEffectChain("[DeckLabEffectUnit1]");
    ASSERT_FALSE(native.isNull()); EXPECT_NE(standard, native);
    ASSERT_TRUE(native->getEffectSlot(3)->isLoaded());
    EXPECT_EQ(QStringLiteral("urn:decklab:threeband-performance"), native->getEffectSlot(3)->id());
    panel->releaseEffects();
    EXPECT_DOUBLE_EQ(.63, ControlObject::get(ConfigKey("[EffectRack1_EffectUnit3]", "mix")));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[DeckLabEffectUnit1]", "mix")));
}

TEST(DeckLabTempoTest, JitterIsSmoothedButPitchChangesImmediately) {
    DeckLabTempoPresenter display;
    const double initial = display.update(true, 141.97, 1, 141.97, 0);
    for (int n = 1; n < 8; ++n) EXPECT_EQ(initial, display.update(true, n % 2 ? 141.93 : 141.97, 1, 141.93, n * 100));
    EXPECT_NEAR(144.79, display.update(true, 141.97, 1.02, 144.8, 800), .02);
    EXPECT_EQ(130, display.update(true, 130, 1, 130, 900));
    EXPECT_EQ(0, display.update(false, 130, 1, 130, 1000));
}
