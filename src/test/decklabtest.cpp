// SPDX-License-Identifier: GPL-2.0-or-later
#include <QAction>
#include <QDir>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTest>
#include <QTextDocument>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "effects/effectchain.h"
#include "effects/effectslot.h"
#include "library/libraryfeature.h"
#include "library/librarycontrol.h"
#include "library/sidebarmodel.h"
#include "library/treeitem.h"
#include "test/signalpathtest.h"
#include "mixer/playermanager.h"
#include "waveform/waveformwidgetfactory.h"
#include "widget/decklabtempopresenter.h"
#include "widget/wdecklab.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarysidebar.h"
#include "widget/wsearchlineedit.h"
#include "widget/wwaveformviewer.h"
#ifdef __LILV__
#include "effects/backends/lv2/lv2effectprocessor.h"
#include "effects/backends/lv2/lv2manifest.h"
#endif

namespace {
class TestPlayers final : public PlayerManagerInterface {
  public:
    std::array<BaseTrackPlayer*,4> decks{};
    BaseTrackPlayer* getPlayer(const QString& group) const override {
        for(int n=0;n<4;++n)if(group==QStringLiteral("[Channel%1]").arg(n+1))return decks[n];
        return nullptr;
    }
    BaseTrackPlayer* getPlayer(const ChannelHandle&) const override {return nullptr;}
    BaseTrackPlayer* getDeckBase(int n) const override {return n>=0&&n<4?decks[n]:nullptr;}
    int numberOfDecks() const override {return 4;}
    PreviewDeck* getPreviewDeck(int) const override {return nullptr;}
    int numberOfPreviewDecks() const override {return 0;}
    Sampler* getSampler(int) const override {return nullptr;}
    int numberOfSamplers() const override {return 0;}
};
class TestSource final : public LibraryFeature {
  public:
    explicit TestSource(UserSettingsPointer config) : LibraryFeature(nullptr, config, QString()) {
        auto* root = model.setRootItem(TreeItem::newRoot(this));
        auto* genres = root->appendChild("Genres"); genres->appendChild("House"); genres->appendChild("Techno");
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
        config()->setValue(ConfigKey("[DeckLab]", "DeckLayout"), 2);
        fourthDeck = std::make_unique<Deck>(nullptr, config(), m_pEngineMixer.get(), m_pEffectsManager.get(),
                EngineChannel::CENTER, m_pEngineMixer->registerChannelGroup(QStringLiteral("[Channel4]")));
        addDeck(fourthDeck->getEngineDeck());
        WaveformWidgetFactory::createInstance()->setConfig(config());
        source = std::make_unique<TestSource>(config());
        model = std::make_unique<SidebarModel>(); model->addLibraryFeature(source.get());
        navigation = std::make_unique<LibraryControl>(nullptr);
        soundStatus = std::make_unique<ControlObject>(ConfigKey("[SoundManager]", "status"));
        players = std::make_unique<TestPlayers>(); players->decks={m_pMixerDeck1.get(),m_pMixerDeck2.get(),m_pMixerDeck3.get(),fourthDeck.get()};
        panel = std::make_unique<WDeckLab>(config(), nullptr, players.get(), m_pEffectsManager.get());
        sidebar = new WLibrarySidebar(panel.get()); sidebar->setModel(model.get());
        library = new WLibrary(panel.get());
        auto* search = new WSearchLineEdit(panel.get(), config());
        panel->bindBrowser(search, sidebar, library); navigation->bindSearchboxWidget(search);
        navigation->bindSidebarWidget(sidebar); navigation->bindLibraryWidget(library, nullptr);
        // Geometry slots only. A bare WWaveformViewer is opaque but cannot
        // paint until the skin parser creates its renderer. The separate full
        // application smoke check captures the actual configured viewers.
        for (int deck = 1; deck <= 4; ++deck) {
            auto* slot = new QWidget(panel.get()); slot->setStyleSheet("background:#000");
            panel->addWaveform(slot, deck);
            auto* overview = new QWidget(panel.get()); overview->setStyleSheet("background:#181818");
            panel->addOverview(overview, deck);
        }
        panel->resize(800, 480); panel->show(); QCoreApplication::processEvents();
    }
    void TearDown() override {
        panel.reset(); soundStatus.reset(); players.reset(); navigation.reset(); model.reset(); source.reset();
        fourthDeck.reset(); WaveformWidgetFactory::destroy(); BaseSignalPathTest::TearDown();
    }
    std::unique_ptr<TestSource> source;
    std::unique_ptr<SidebarModel> model;
    std::unique_ptr<WDeckLab> panel;
    std::unique_ptr<TestPlayers> players;
    std::unique_ptr<LibraryControl> navigation;
    std::unique_ptr<ControlObject> soundStatus;
    std::unique_ptr<Deck> fourthDeck;
    WLibrarySidebar* sidebar = nullptr;
    WLibrary* library = nullptr;
};

TEST_F(NativeDeckLabTest, FirstBrowsePressDoesNotOpenSource) {
    ControlObject::set(ConfigKey("[DeckLabNative]", "browse_press"), 1);
    ControlObject::set(ConfigKey("[DeckLabNative]", "browse_press"), 0);
    QCoreApplication::processEvents();
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

TEST_F(NativeDeckLabTest, NativeKnobSelectionDoesNotActivateOrLeaveFolder) {
    panel->browsePress(); panel->browsePress(); panel->browsePress();
    const auto root = sidebar->rootIndex(); const int before = source->activations;
    for (int n = 0; n < 20; ++n) ControlObject::set(ConfigKey("[Playlist]", "SelectPlaylist"), n % 2 ? -1 : 1);
    QTest::qWait(350);
    EXPECT_EQ(before, source->activations);
    EXPECT_EQ(root, sidebar->rootIndex());
    EXPECT_EQ(0, sidebar->currentIndex().row());
}

TEST_F(NativeDeckLabTest, EmptySamplerHasAnExplicitLoadAndCancelFlow) {
    panel->selectPage(2); auto* pad = panel->findChild<QPushButton*>("decklab-sample-16"); ASSERT_NE(nullptr, pad);
    pad->click(); EXPECT_EQ(1, panel->currentPage());
    auto* cancel = panel->findChild<QPushButton*>("decklab-load-2"); ASSERT_NE(nullptr, cancel);
    EXPECT_TRUE(cancel->text().contains("Cancel")); cancel->click(); EXPECT_EQ(2, panel->currentPage());
}

TEST_F(NativeDeckLabTest, UsbUtilityReturnsDirectlyToNativeSources) {
    panel->browsePress(); panel->browsePress(); panel->browsePress();
    const int before = source->activations;
    ASSERT_TRUE(sidebar->rootIndex().isValid());
    ControlObject::set(ConfigKey("[DeckLabNative]", "browse_sources"), 1);
    ControlObject::set(ConfigKey("[DeckLabNative]", "browse_sources"), 0);
    QCoreApplication::processEvents();
    EXPECT_EQ(1, panel->currentPage()); EXPECT_FALSE(sidebar->rootIndex().isValid());
    EXPECT_TRUE(sidebar->isVisible()); EXPECT_EQ(before, source->activations);
}

TEST_F(NativeDeckLabTest, TouchSearchUsesNativeKeyboardAndReturnsWaveforms) {
    panel->selectPage(1);
    panel->findChild<QPushButton*>("decklab-search")->click();
    auto* keyboard = panel->findChild<QWidget*>("decklab-keyboard"); ASSERT_NE(nullptr, keyboard);
    EXPECT_TRUE(keyboard->isVisible()); EXPECT_FALSE(panel->waveformContainer()->isVisible());
    for (auto* b : keyboard->findChildren<QPushButton*>()) if (b->text() == "Q") { b->click(); break; }
    auto* search = panel->findChild<WSearchLineEdit*>(); ASSERT_NE(nullptr, search);
    EXPECT_EQ(QStringLiteral("q"), search->lineEdit()->text());
    panel->findChild<QPushButton*>("decklab-search-done")->click();
    EXPECT_FALSE(keyboard->isVisible()); EXPECT_TRUE(panel->waveformContainer()->isVisible());
    EXPECT_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "play")));
}

TEST_F(NativeDeckLabTest, ToolRequestCanRepeatWithoutTimingRace) {
    ControlObject::set(ConfigKey("[DeckLabNative]", "tools_ready"), 1); QCoreApplication::processEvents();
    auto* usb = panel->findChild<QPushButton*>("decklab-usb"); ASSERT_NE(nullptr, usb);
    usb->click(); const double serial = ControlObject::get(ConfigKey("[DeckLabNative]", "tool_serial"));
    usb->click(); EXPECT_EQ(serial + 1, ControlObject::get(ConfigKey("[DeckLabNative]", "tool_serial")));
    EXPECT_EQ(1, ControlObject::get(ConfigKey("[DeckLabNative]", "tool_request")));
}

TEST_F(NativeDeckLabTest, SplitCueToggleReportsItsActualState) {
    auto* split = panel->findChild<QPushButton*>("decklab-head-split"); ASSERT_NE(nullptr, split);
    ControlObject::set(ConfigKey("[Master]", "headSplit"), 0); QCoreApplication::processEvents();
    EXPECT_EQ(QStringLiteral("Off"), split->text()); EXPECT_FALSE(split->isChecked());
    split->click(); QCoreApplication::processEvents();
    EXPECT_EQ(QStringLiteral("On"), split->text()); EXPECT_TRUE(split->isChecked());
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Master]", "headSplit")));
    ControlObject::set(ConfigKey("[Master]", "headSplit"), 0); QCoreApplication::processEvents();
    EXPECT_EQ(QStringLiteral("Off"), split->text());
}

TEST_F(NativeDeckLabTest, TempoRangeReflectsControllerChanges) {
    auto* range = panel->findChild<QComboBox*>("decklab-tempo-range-1"); ASSERT_NE(nullptr, range);
    for (const auto& change : {std::pair{.16, 1}, std::pair{.50, 2}, std::pair{.06, 3}, std::pair{.08, 0}}) {
        ControlObject::set(ConfigKey("[Channel1]", "rateRange"), change.first);
        QCoreApplication::processEvents(); EXPECT_EQ(change.second, range->currentIndex());
        EXPECT_DOUBLE_EQ(change.first, range->currentData().toDouble());
        EXPECT_DOUBLE_EQ(change.first, ControlObject::get(ConfigKey("[Channel1]", "rateRange")));
    }
    EXPECT_EQ(3, range->count());
}

TEST_F(NativeDeckLabTest, ActiveNavigationCannotBeUncheckedByReselectingIt) {
    auto* decks = panel->findChild<QPushButton*>("decklab-tab-0"); ASSERT_NE(nullptr, decks);
    decks->click(); decks->click(); EXPECT_TRUE(decks->isChecked()); EXPECT_EQ(0, panel->currentPage());
    auto* settings = panel->findChild<QPushButton*>("decklab-tab-4"); ASSERT_NE(nullptr, settings);
    settings->click(); settings->click(); EXPECT_TRUE(settings->isChecked()); EXPECT_FALSE(decks->isChecked());
    auto* category = panel->findChild<QPushButton*>("decklab-settings-category-2"); ASSERT_NE(nullptr, category);
    category->click(); category->click(); EXPECT_TRUE(category->isChecked());
    EXPECT_FALSE(panel->findChild<QPushButton*>("decklab-settings-category-0")->isChecked());
}

TEST_F(NativeDeckLabTest, InternalClockCannotClaimAConfiguredAudioOutput) {
    auto* status = panel->findChild<QPushButton*>("decklab-status"); ASSERT_NE(nullptr, status);
    ControlObject::set(ConfigKey("[SoundManager]", "status"), 2); QCoreApplication::processEvents();
    EXPECT_TRUE(status->toolTip().contains("Audio disconnected"));
    panel->setAudioOutputConfigured(true); EXPECT_FALSE(status->toolTip().contains("Audio disconnected"));
    ControlObject::set(ConfigKey("[SoundManager]", "status"), 0); QCoreApplication::processEvents();
    EXPECT_TRUE(status->toolTip().contains("Audio disconnected"));
}

TEST_F(NativeDeckLabTest, PagesFit800By480) {
    const QString output = qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
    for (int page = 0; page < 5; ++page) {
        panel->selectPage(page); QCoreApplication::processEvents();
        EXPECT_EQ(QSize(800, 480), panel->size()) << page;
        for (auto* button : panel->findChildren<QPushButton*>()) {
            if (!button->isVisible()) continue;
            // Settings deliberately scrolls. Every control must be reachable
            // in full, rather than every content child fitting simultaneously.
            QScrollArea* scroll = nullptr;
            for (auto* parent = button->parentWidget(); parent; parent = parent->parentWidget())
                if ((scroll = qobject_cast<QScrollArea*>(parent))) break;
            if (scroll) {
                scroll->ensureWidgetVisible(button, 0, 0); QCoreApplication::processEvents();
                EXPECT_TRUE(scroll->viewport()->rect().contains(
                        QRect(button->mapTo(scroll->viewport(), QPoint()), button->size())))
                        << qPrintable(button->text()) << " page " << page;
            }
            QRect bounds(button->mapTo(panel.get(), QPoint()), button->size());
            EXPECT_TRUE(panel->rect().contains(bounds)) << qPrintable(button->text()) << " page " << page;
            EXPECT_GE(button->height(), 38) << qPrintable(button->text());
        }
        for (auto* scroll : panel->findChildren<QScrollArea*>()) {
            scroll->verticalScrollBar()->setValue(0); scroll->horizontalScrollBar()->setValue(0);
        }
        QCoreApplication::processEvents();
        if (page == 0) {
            auto* cue = panel->findChild<QPushButton*>("decklab-1-cue_default");
            EXPECT_TRUE(cue->isVisible()); EXPECT_FALSE(cue->visibleRegion().isEmpty());
        }
        if (!output.isEmpty()) EXPECT_TRUE(panel->grab().save(QDir(output).filePath(QStringLiteral("screen-%1.png").arg(page))));
    }
}

TEST_F(NativeDeckLabTest, SixteenSamplerPadsRemainNearSquareAndFullyVisible) {
    panel->selectPage(2); QCoreApplication::processEvents();
    for (int slot = 1; slot <= 16; ++slot) {
        auto* pad = panel->findChild<QPushButton*>(QStringLiteral("decklab-sample-%1").arg(slot));
        ASSERT_NE(nullptr, pad); EXPECT_TRUE(pad->isVisible());
        EXPECT_GE(pad->width(), 110); EXPECT_GE(pad->height(), 110);
        EXPECT_NEAR(1.0, pad->width() / double(pad->height()), .2);
        EXPECT_TRUE(panel->rect().contains(QRect(pad->mapTo(panel.get(), QPoint()), pad->size())));
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
    EXPECT_EQ(QStringLiteral("urn:decklab:threeband-performance-native-v1"), native->getEffectSlot(3)->id());
    panel->releaseEffects();
    EXPECT_DOUBLE_EQ(.63, ControlObject::get(ConfigKey("[EffectRack1_EffectUnit3]", "mix")));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[DeckLabEffectUnit1]", "mix")));
}

TEST_F(NativeDeckLabTest, ApplicationDeactivateReleasesRmxWithoutChangingControllerFx) {
    m_pEffectsManager->setup(); m_pEffectsManager->ensureDeckLabEffects();
    m_pEngineMixer->prepareAudio(mixxx::audio::SampleRate(44100));
    m_pMixerDeck1->loadFakeTrack(false,128); ProcessBuffer(); QCoreApplication::processEvents();
    ControlObject::set(ConfigKey("[EffectRack1_EffectUnit3]","mix"),.63);
    panel->selectPage(3);
    auto* low=panel->findChild<QPushButton*>("decklab-fx-band-0"); ASSERT_NE(nullptr,low); ASSERT_TRUE(low->isEnabled());
    low->click(); EXPECT_DOUBLE_EQ(1,ControlObject::get(ConfigKey("[DeckLabEffectUnit1]","mix")));
    EXPECT_GT(ControlObject::get(ConfigKey("[DeckLabEffectUnit1_Effect4]","parameter15")),0);
    QEvent cancel(QEvent::ApplicationDeactivate); QCoreApplication::sendEvent(panel.get(),&cancel);
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[DeckLabEffectUnit1]","mix")));
    EXPECT_DOUBLE_EQ(.63,ControlObject::get(ConfigKey("[EffectRack1_EffectUnit3]","mix")));
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel1]","play")));
}

TEST_F(NativeDeckLabTest, LongMetadataCannotHideTempoOrExpandSurface) {
    auto track=m_pMixerDeck1->loadFakeTrack(false,128); track->setTitle(QString(200,QChar('W'))+"<b>Title</b>");
    ProcessBuffer(); QTest::qWait(150); panel->selectPage(0); QCoreApplication::processEvents();
    auto* title=panel->findChild<QLabel*>("decklab-wave-title-1");
    auto* numbers=panel->findChild<QLabel*>("decklab-wave-metrics-1"); ASSERT_NE(nullptr,title); ASSERT_NE(nullptr,numbers);
    EXPECT_EQ(Qt::PlainText,title->textFormat()); EXPECT_FALSE(numbers->isVisible());
    EXPECT_TRUE(panel->findChild<QLabel*>("decklab-numbers-1")->text().contains("128"));
    panel->selectPage(3); QTest::qWait(150);
    EXPECT_TRUE(numbers->isVisible());
    EXPECT_TRUE(numbers->text().contains("128")); EXPECT_EQ(QSize(800,480),panel->size());
}

TEST_F(NativeDeckLabTest, ControllerLayerChangeRefreshesPlayState) {
    m_pMixerDeck1->loadFakeTrack(false, 128); ProcessBuffer();
    ControlObject::set(ConfigKey("[Channel1]", "play"), 1);
    ControlObject::set(ConfigKey("[Channel3]", "play"), 0);
    QCoreApplication::processEvents();
    auto* play = panel->findChild<QPushButton*>("decklab-1-play"); ASSERT_NE(nullptr, play);
    EXPECT_TRUE(play->isChecked());
    EXPECT_EQ(QStringLiteral("Pause"), play->text());
    EXPECT_TRUE(panel->findChild<QLabel*>("decklab-wave-identity-1")->isVisible());
    ControlObject::set(ConfigKey("[DeckLabLeft]", "current"), 1);
    QCoreApplication::processEvents();
    EXPECT_FALSE(play->isChecked());
    EXPECT_EQ(QStringLiteral("Play"), play->text());
    EXPECT_TRUE(panel->findChild<QLabel*>("decklab-wave-identity-3")->isVisible());
    EXPECT_FALSE(panel->findChild<QLabel*>("decklab-wave-identity-1")->isVisible());
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel1]", "play")));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel3]", "play")));
}

TEST_F(NativeDeckLabTest, DecksTransportHasTruthfulStatesAndLargerTargets) {
    m_pMixerDeck1->loadFakeTrack(false, 128); ProcessBuffer(); QTest::qWait(150);
    auto* play = panel->findChild<QPushButton*>("decklab-1-play");
    auto* sync = panel->findChild<QPushButton*>("decklab-1-beatsync");
    auto* numbers = panel->findChild<QLabel*>("decklab-numbers-1");
    ASSERT_NE(nullptr, play); ASSERT_NE(nullptr, sync); ASSERT_NE(nullptr, numbers);
    EXPECT_EQ(QStringLiteral("Play"), play->text()); EXPECT_TRUE(play->isEnabled());
    EXPECT_EQ(QStringLiteral("PAUSED"), panel->findChild<QLabel*>("decklab-state-1")->text());
    EXPECT_TRUE(panel->findChild<QLabel*>("decklab-pitch-1")->text().contains("%"));
    EXPECT_EQ(44, play->height()); EXPECT_EQ(44, sync->height());
    EXPECT_EQ(40, panel->findChild<QPushButton*>("decklab-tab-0")->height());
    EXPECT_EQ(QStringLiteral("BEAT\nSYNC"), sync->text()); EXPECT_FALSE(sync->isCheckable());
    sync->click(); QCoreApplication::processEvents();
    EXPECT_TRUE(sync->property("actionFeedback").toBool());
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "sync_enabled")));
    QTest::qWait(700); EXPECT_FALSE(sync->property("actionFeedback").toBool());
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "play")));
    panel->selectPage(1); QCoreApplication::processEvents();
    EXPECT_EQ(40, panel->findChild<QPushButton*>("decklab-tab-0")->height());
}

TEST_F(NativeDeckLabTest, LayerChangeReleasesOnlyTheReplacedTouchCue) {
    m_pMixerDeck1->loadFakeTrack(false, 128);
    m_pMixerDeck2->loadFakeTrack(false, 130);
    m_pMixerDeck3->loadFakeTrack(false, 132); fourthDeck->loadFakeTrack(false, 134); ProcessBuffer(); QCoreApplication::processEvents();
    auto* left = panel->findChild<QPushButton*>("decklab-1-cue_default");
    auto* right = panel->findChild<QPushButton*>("decklab-2-cue_default");
    ASSERT_NE(nullptr, left); ASSERT_NE(nullptr, right);
    QTest::mousePress(left, Qt::LeftButton);
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel1]", "cue_default")));
    ControlObject::set(ConfigKey("[DeckLabRight]", "current"), 1); QCoreApplication::processEvents();
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel1]", "cue_default")));
    ControlObject::set(ConfigKey("[DeckLabLeft]", "current"), 1); QCoreApplication::processEvents();
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "cue_default")));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel3]", "cue_default")));
    QTest::mouseRelease(left, Qt::LeftButton);
    ControlObject::set(ConfigKey("[DeckLabRight]", "current"), 0); QCoreApplication::processEvents();
    QTest::mousePress(right, Qt::LeftButton);
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel2]", "cue_default")));
    ControlObject::set(ConfigKey("[DeckLabLeft]", "current"), 0); QCoreApplication::processEvents();
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel2]", "cue_default")));
    ControlObject::set(ConfigKey("[DeckLabRight]", "current"), 1); QCoreApplication::processEvents();
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel2]", "cue_default")));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel4]", "cue_default")));
    QTest::mouseRelease(right, Qt::LeftButton);
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel3]", "play")));
}

TEST_F(NativeDeckLabTest, SyncAcknowledgmentCannotTransferToAnotherLayer) {
    m_pMixerDeck1->loadFakeTrack(false, 128);
    m_pMixerDeck2->loadFakeTrack(false, 130);
    m_pMixerDeck3->loadFakeTrack(false, 132); fourthDeck->loadFakeTrack(false, 134); ProcessBuffer(); QCoreApplication::processEvents();
    auto* left = panel->findChild<QPushButton*>("decklab-1-beatsync");
    auto* right = panel->findChild<QPushButton*>("decklab-2-beatsync");
    ASSERT_NE(nullptr, left); ASSERT_NE(nullptr, right);
    left->click(); QCoreApplication::processEvents(); ASSERT_TRUE(left->property("actionFeedback").toBool());
    ControlObject::set(ConfigKey("[DeckLabRight]", "current"), 1); QCoreApplication::processEvents();
    EXPECT_TRUE(left->property("actionFeedback").toBool());
    ControlObject::set(ConfigKey("[DeckLabLeft]", "current"), 1); QCoreApplication::processEvents();
    EXPECT_FALSE(left->property("actionFeedback").toBool());
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel3]", "sync_enabled")));
    ControlObject::set(ConfigKey("[DeckLabLeft]", "current"), 0); QCoreApplication::processEvents();
    EXPECT_FALSE(left->property("actionFeedback").toBool());
    ControlObject::set(ConfigKey("[DeckLabRight]", "current"), 0); QCoreApplication::processEvents();
    right->click(); QCoreApplication::processEvents(); ASSERT_TRUE(right->property("actionFeedback").toBool());
    ControlObject::set(ConfigKey("[DeckLabLeft]", "current"), 1); QCoreApplication::processEvents();
    EXPECT_TRUE(right->property("actionFeedback").toBool());
    ControlObject::set(ConfigKey("[DeckLabRight]", "current"), 1); QCoreApplication::processEvents();
    EXPECT_FALSE(right->property("actionFeedback").toBool());
}

TEST_F(NativeDeckLabTest, DeckIdentityAndLabelledReadoutsFitWithoutStealingWaveformSpace) {
    auto track = m_pMixerDeck1->loadFakeTrack(false, 300);
    track->setKey(mixxx::track::io::key::E_MAJOR, mixxx::track::io::key::USER); ProcessBuffer();
    panel->setAudioOutputConfigured(true);
    ControlObject::set(ConfigKey("[SoundManager]", "status"), 2);
    ControlObject::set(ConfigKey("[DeckLabNative]", "tools_ready"), 1);
    ControlObject::set(ConfigKey("[Channel1]", "duration"), 7200);
    QTest::qWait(150);
    auto* numbers = panel->findChild<QLabel*>("decklab-numbers-1"); ASSERT_NE(nullptr, numbers);
    // These fixtures have no running DeckVisuals render loop to mirror key -> visual_key.
    ControlObject::set(ConfigKey("[Channel1]", "visual_key"), 5);
    QComboBox* format = nullptr;
    for (auto* combo : panel->findChildren<QComboBox*>())
        if (combo->itemText(0).startsWith("Camelot")) format = combo;
    ASSERT_NE(nullptr, format);
    for (int notation = 0; notation < 3; ++notation) {
        QMetaObject::invokeMethod(format, "activated", Q_ARG(int, notation));
        for (const auto& name : {"numbers", "key", "remaining", "pitch"}) {
            auto* label = panel->findChild<QLabel*>(QStringLiteral("decklab-%1-1").arg(name)); ASSERT_NE(nullptr, label);
            QTextDocument document; document.setDocumentMargin(0); document.setDefaultFont(label->font());
            document.setHtml(label->text()); document.setTextWidth(label->width());
            EXPECT_LE(document.idealWidth(), label->width());
            EXPECT_LE(document.size().height(), label->height());
        }
    }
    QMetaObject::invokeMethod(format, "activated", Q_ARG(int, 0));
    EXPECT_TRUE(panel->findChild<QLabel*>("decklab-key-1")->text().contains("12B"));
    for (int deck = 1; deck <= 4; ++deck) {
        auto* identity = panel->findChild<QLabel*>(QStringLiteral("decklab-wave-identity-%1").arg(deck));
        ASSERT_NE(nullptr, identity); EXPECT_EQ(QString::number(deck), identity->text()); EXPECT_EQ(32, identity->width());
        EXPECT_LE(identity->fontMetrics().tightBoundingRect(identity->text()).height(), 24);
    }
    EXPECT_EQ(QSize(800, 480), panel->size());
    EXPECT_EQ(440, panel->waveformContainer()->height());
    EXPECT_EQ(QSize(668,132), panel->findChild<QWidget*>("decklab-detail-1")->size());
    EXPECT_EQ(QSize(668,44), panel->findChild<QWidget*>("decklab-overview-1")->size());
    EXPECT_EQ(QSize(132,176), panel->findChild<QPushButton*>("decklab-1-play")->parentWidget()->size());
}

TEST_F(NativeDeckLabTest, TransportStateAndTargetRemainTruthfulInPlayingFixture) {
    m_pMixerDeck1->loadFakeTrack(false, 128);
    m_pMixerDeck2->loadFakeTrack(false, 130); ProcessBuffer(); QTest::qWait(150);
    auto* play = panel->findChild<QPushButton*>("decklab-1-play"); ASSERT_NE(nullptr, play);
    play->click(); QTest::qWait(150);
    EXPECT_EQ(QStringLiteral("Pause"), play->text()); EXPECT_TRUE(play->isChecked());
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel1]", "play")));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel2]", "play")));
    EXPECT_EQ(QStringLiteral("PLAYING"), panel->findChild<QLabel*>("decklab-state-1")->text());
    EXPECT_EQ(QStringLiteral("PAUSED"), panel->findChild<QLabel*>("decklab-state-2")->text());
    const QString output = qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
    if (!output.isEmpty()) EXPECT_TRUE(panel->grab().save(QDir(output).filePath("decks-fixture-playing.png")));
    play->click(); QTest::qWait(150);
    EXPECT_EQ(QStringLiteral("Play"), play->text()); EXPECT_FALSE(play->isChecked());
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "play")));
}

TEST_F(NativeDeckLabTest, UpperLayerTransportTargetsLoadedDecksThreeAndFour) {
    m_pMixerDeck1->loadFakeTrack(false, 128); m_pMixerDeck2->loadFakeTrack(false, 130);
    m_pMixerDeck3->loadFakeTrack(false, 132); fourthDeck->loadFakeTrack(false, 134);
    ProcessBuffer(); QCoreApplication::processEvents();
    ControlObject::set(ConfigKey("[DeckLabLeft]", "current"), 1);
    ControlObject::set(ConfigKey("[DeckLabRight]", "current"), 1); QCoreApplication::processEvents();
    panel->findChild<QPushButton*>("decklab-1-play")->click();
    panel->findChild<QPushButton*>("decklab-2-play")->click(); QTest::qWait(150);
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "play")));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel2]", "play")));
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel3]", "play")));
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel4]", "play")));
    EXPECT_TRUE(panel->findChild<QLabel*>("decklab-wave-identity-3")->isVisible());
    EXPECT_TRUE(panel->findChild<QLabel*>("decklab-wave-identity-4")->isVisible());
    EXPECT_TRUE(panel->findChild<QLabel*>("decklab-wave-identity-3")->isVisible());
    EXPECT_TRUE(panel->findChild<QLabel*>("decklab-wave-identity-4")->isVisible());
    EXPECT_FALSE(panel->findChild<QLabel*>("decklab-wave-identity-1")->isVisible());
    EXPECT_FALSE(panel->findChild<QLabel*>("decklab-wave-identity-2")->isVisible());
}

TEST_F(NativeDeckLabTest, TouchBeatSyncMatchesTempoAndPhaseOnceWithoutStopping) {
    m_pMixerDeck1->loadFakeTrack(false, 128); m_pMixerDeck2->loadFakeTrack(false, 130); ProcessBuffer();
    ControlObject::set(ConfigKey("[Channel1]", "quantize"), 0);
    ControlObject::set(ConfigKey("[Channel2]", "quantize"), 0);
    ControlObject::set(ConfigKey("[Channel1]", "playposition"), .11);
    ControlObject::set(ConfigKey("[Channel2]", "playposition"), .21); ProcessBuffer();
    ControlObject::set(ConfigKey("[Channel1]", "play"), 1);
    ControlObject::set(ConfigKey("[Channel2]", "play"), 1); ProcessBuffer();
    ASSERT_GT(std::abs(ControlObject::get(ConfigKey("[Channel1]", "beat_distance")) -
            ControlObject::get(ConfigKey("[Channel2]", "beat_distance"))), .01);
    QCoreApplication::processEvents(); panel->findChild<QPushButton*>("decklab-1-beatsync")->click();
    for (int n = 0; n < 8; ++n) ProcessBuffer();
    EXPECT_NEAR(130, ControlObject::get(ConfigKey("[Channel1]", "bpm")), 1e-6);
    EXPECT_NEAR(ControlObject::get(ConfigKey("[Channel1]", "beat_distance")),
            ControlObject::get(ConfigKey("[Channel2]", "beat_distance")), .001);
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "sync_enabled")));
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel1]", "play")));
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel2]", "play")));
    ControlObject::set(ConfigKey("[Channel2]", "rate_ratio"), 1.02);
    for (int n = 0; n < 8; ++n) ProcessBuffer();
    EXPECT_NEAR(132.6, ControlObject::get(ConfigKey("[Channel2]", "bpm")), 1e-6);
    EXPECT_NEAR(130, ControlObject::get(ConfigKey("[Channel1]", "bpm")), 1e-6);
}

TEST_F(NativeDeckLabTest, AutoLayoutShowsFourRealDecksAndRetainsLoadedUpperDecks) {
    auto* choice = panel->findChild<QComboBox*>("decklab-deck-layout"); ASSERT_NE(nullptr, choice);
    QMetaObject::invokeMethod(choice, "activated", Q_ARG(int, 0));
    panel->setAudioOutputConfigured(true); ControlObject::set(ConfigKey("[SoundManager]", "status"), 2);
    ControlObject::set(ConfigKey("[DeckLabNative]", "tools_ready"), 1);
    ControlObject::set(ConfigKey("[DeckLabNative]", "layout_decks"), 2);
    auto track = fourthDeck->loadFakeTrack(false, 134); track->setTitle("Fourth deck — actual target"); ProcessBuffer();
    QTest::qWait(150);
    // A known two-channel controller's software layers do not open four cards.
    EXPECT_FALSE(panel->findChild<QPushButton*>("decklab-card-4-play")->isVisible());
    EXPECT_TRUE(panel->findChild<QPushButton*>("decklab-1-play")->isVisible());
    ControlObject::set(ConfigKey("[DeckLabNative]", "layout_decks"), 4); QCoreApplication::processEvents();
    EXPECT_TRUE(panel->findChild<QPushButton*>("decklab-card-4-play")->isVisible());
    EXPECT_FALSE(panel->findChild<QPushButton*>("decklab-1-play")->isVisible());
    ControlObject::set(ConfigKey("[DeckLabNative]", "layout_decks"), 2); QTest::qWait(150);
    EXPECT_TRUE(panel->findChild<QPushButton*>("decklab-card-4-play")->isVisible());
    auto* play = panel->findChild<QPushButton*>("decklab-card-4-play"); ASSERT_TRUE(play->isEnabled());
    play->click(); QTest::qWait(150);
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel4]", "play")));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel2]", "play")));
    EXPECT_EQ(QStringLiteral("Pause"), play->text());
    const QString output = qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
    if (!output.isEmpty()) EXPECT_TRUE(panel->grab().save(QDir(output).filePath("decks-fixture-four.png")));
    for (int deck = 1; deck <= 4; ++deck) {
        auto* cue = panel->findChild<QPushButton*>(QStringLiteral("decklab-card-%1-cue_default").arg(deck));
        EXPECT_EQ(48, cue->height()); EXPECT_GE(cue->width(), 100);
        auto* overview = panel->findChild<QWidget*>(QStringLiteral("decklab-overview-%1").arg(deck));
        ASSERT_NE(nullptr, overview); EXPECT_TRUE(overview->isVisible()); EXPECT_EQ(44, overview->height());
        EXPECT_TRUE(overview->testAttribute(Qt::WA_TransparentForMouseEvents)); EXPECT_FALSE(overview->acceptDrops());
        EXPECT_TRUE(panel->rect().contains(QRect(cue->mapTo(panel.get(), QPoint()), cue->size())));
    }
    EXPECT_EQ(QSize(800,480), panel->size());
}

TEST_F(NativeDeckLabTest, LayerOrLayoutChangeCancelsPendingTransportPress) {
    m_pMixerDeck1->loadFakeTrack(false, 128); m_pMixerDeck3->loadFakeTrack(false, 132); ProcessBuffer();
    auto* play = panel->findChild<QPushButton*>("decklab-1-play");
    QTest::mousePress(play, Qt::LeftButton);
    ControlObject::set(ConfigKey("[DeckLabLeft]", "current"), 1); QCoreApplication::processEvents();
    QTest::mouseRelease(play, Qt::LeftButton);
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "play")));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel3]", "play")));
    ControlObject::set(ConfigKey("[DeckLabLeft]", "current"), 0); QCoreApplication::processEvents();
    QTest::mousePress(play, Qt::LeftButton);
    auto* choice = panel->findChild<QComboBox*>("decklab-deck-layout");
    QMetaObject::invokeMethod(choice, "activated", Q_ARG(int, 2)); QCoreApplication::processEvents();
    QTest::mouseRelease(play, Qt::LeftButton);
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "play")));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel3]", "play")));
}

TEST_F(NativeDeckLabTest, SideUtilitiesOperateRealLoopsAndModesWithoutRetargetingHeldActions) {
    m_pMixerDeck1->loadFakeTrack(false, 128);
    m_pMixerDeck3->loadFakeTrack(false, 132); ProcessBuffer();
    for (int deck : {1,3}) {
        const QString group = QStringLiteral("[Channel%1]").arg(deck);
        ControlObject::set(ConfigKey(group, "quantize"), 0);
        ControlObject::set(ConfigKey(group, "keylock"), 0);
        ControlObject::set(ConfigKey(group, "beatloop_size"), 4);
    }
    QTest::qWait(150);
    auto find = [this](const QString& action) {
        return panel->findChild<QPushButton*>(QStringLiteral("decklab-1-") + action);
    };
    auto* loop = find("beatloop_activate"); auto* half = find("loop_halve"); auto* twice = find("loop_double");
    auto* quantize = find("quantize"); auto* keylock = find("keylock"); auto* play = find("play");
    ASSERT_NE(nullptr, loop); ASSERT_NE(nullptr, half); ASSERT_NE(nullptr, twice);
    ASSERT_NE(nullptr, quantize); ASSERT_NE(nullptr, keylock); ASSERT_NE(nullptr, play);
    for (auto* action : {loop,half,twice,quantize,keylock,play}) {
        EXPECT_EQ(44, action->height()); EXPECT_GE(action->width(), 44); EXPECT_TRUE(action->isEnabled());
    }
    EXPECT_EQ(QStringLiteral("LOOP\n4"), loop->text()); EXPECT_FALSE(loop->isChecked());
    loop->click(); QCoreApplication::processEvents(); ProcessBuffer(); QTest::qWait(20);
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel1]", "loop_enabled")));
    EXPECT_TRUE(loop->isChecked()); EXPECT_TRUE(loop->accessibleName().contains("on"));
    half->click(); QCoreApplication::processEvents(); ProcessBuffer(); QTest::qWait(20);
    EXPECT_DOUBLE_EQ(2, ControlObject::get(ConfigKey("[Channel1]", "beatloop_size")));
    EXPECT_EQ(QStringLiteral("LOOP\n2"), loop->text());
    twice->click(); QCoreApplication::processEvents(); ProcessBuffer(); QTest::qWait(20);
    EXPECT_DOUBLE_EQ(4, ControlObject::get(ConfigKey("[Channel1]", "beatloop_size")));
    loop->click(); QCoreApplication::processEvents(); ProcessBuffer(); QTest::qWait(20);
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "loop_enabled")));
    EXPECT_FALSE(loop->isChecked());
    quantize->click(); keylock->click(); QCoreApplication::processEvents();
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel1]", "quantize")));
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel1]", "keylock")));
    EXPECT_TRUE(quantize->isChecked()); EXPECT_TRUE(keylock->isChecked());
    for (const auto& action : {"loop_halve", "beatloop_activate", "loop_double", "quantize", "keylock", "play"}) {
        ControlObject::set(ConfigKey("[DeckLabLeft]", "current"), 0); QCoreApplication::processEvents();
        auto* button = find(QString::fromLatin1(action));
        QTest::mousePress(button, Qt::LeftButton);
        ControlObject::set(ConfigKey("[DeckLabLeft]", "current"), 1); QCoreApplication::processEvents();
        QTest::mouseRelease(button, Qt::LeftButton);
        EXPECT_DOUBLE_EQ(4, ControlObject::get(ConfigKey("[Channel3]", "beatloop_size")));
        EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel3]", "loop_enabled")));
        EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel3]", "quantize")));
        EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel3]", "keylock")));
        EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel3]", "play")));
        if (button->isCheckable()) EXPECT_FALSE(button->isChecked());
    }
    EXPECT_EQ(QStringLiteral("Play"), play->text());
    ControlObject::set(ConfigKey("[DeckLabLeft]", "current"), 0); QCoreApplication::processEvents();
    ControlObject::set(ConfigKey("[Channel1]", "beatloop_size"), .125); QTest::qWait(150);
    EXPECT_EQ(QStringLiteral("LOOP\n1/8"), loop->text());
    ControlObject::set(ConfigKey("[DeckLabLeft]", "current"), 1); QCoreApplication::processEvents();
    QTest::qWait(kUnreplaceDelay + 1);
    m_pMixerDeck3->slotEjectTrack(1); ProcessBuffer(); QTest::qWait(150);
    EXPECT_FALSE(loop->isEnabled()); EXPECT_FALSE(quantize->isEnabled()); EXPECT_FALSE(keylock->isEnabled());
}

TEST_F(NativeDeckLabTest, BrowseLoadCancelsChangedTargetAndOffersFourDirectDestinations) {
    m_pMixerDeck1->loadFakeTrack(false,128); m_pMixerDeck3->loadFakeTrack(false,132); ProcessBuffer();
    std::unique_ptr<ControlObject> missing3, missing4;
    if (!ControlObject::getControl(ConfigKey("[Channel3]", "LoadSelectedTrack"))) missing3 = std::make_unique<ControlObject>(ConfigKey("[Channel3]", "LoadSelectedTrack"));
    if (!ControlObject::getControl(ConfigKey("[Channel4]", "LoadSelectedTrack"))) missing4 = std::make_unique<ControlObject>(ConfigKey("[Channel4]", "LoadSelectedTrack"));
    ControlProxy third("[Channel3]", "LoadSelectedTrack"), fourth("[Channel4]", "LoadSelectedTrack");
    int thirdRequests=0, fourthRequests=0;
    ASSERT_TRUE(third.connectValueChanged(panel.get(), [&thirdRequests](double v) { if (v > 0) ++thirdRequests; }, Qt::DirectConnection));
    ASSERT_TRUE(fourth.connectValueChanged(panel.get(), [&fourthRequests](double v) { if (v > 0) ++fourthRequests; }, Qt::DirectConnection));
    // Prove these observers receive dispatch before relying on a zero count.
    for (const auto& group : {"[Channel3]", "[Channel4]"}) {
        ControlObject::set(ConfigKey(group, "LoadSelectedTrack"), 1);
        ControlObject::set(ConfigKey(group, "LoadSelectedTrack"), 0);
    }
    ASSERT_EQ(1, thirdRequests); ASSERT_EQ(1, fourthRequests);
    thirdRequests = 0; fourthRequests = 0;
    panel->openCollection(); auto* load = panel->findChild<QPushButton*>("decklab-load-1");
    // The fixture has no collection table. Enable the button to model a valid
    // selection on both paused targets and inspect only destination dispatch.
    load->setEnabled(true); QTest::mousePress(load, Qt::LeftButton);
    ControlObject::set(ConfigKey("[DeckLabLeft]", "current"), 1); QCoreApplication::processEvents();
    load->setEnabled(true); QTest::mouseRelease(load, Qt::LeftButton);
    EXPECT_EQ(0, thirdRequests); EXPECT_EQ(0, fourthRequests);
    auto* choice=panel->findChild<QComboBox*>("decklab-deck-layout");
    QMetaObject::invokeMethod(choice, "activated", Q_ARG(int,2)); QCoreApplication::processEvents();
    auto* load4=panel->findChild<QPushButton*>("decklab-load-4"); ASSERT_TRUE(load4->isVisible());
    EXPECT_EQ(QStringLiteral("Load 4"),load4->text()); load4->setEnabled(true); load4->click();
    QCoreApplication::processEvents();
    EXPECT_EQ(1,fourthRequests); EXPECT_EQ(0,thirdRequests);
}

TEST_F(NativeDeckLabTest, BrowseToolsMatchPlayingFourthDeckAndGuardStaleUnload) {
    m_pMixerDeck1->loadFakeTrack(false, 128);
    fourthDeck->loadFakeTrack(false, 134); ProcessBuffer(); QCoreApplication::processEvents();
    panel->openCollection();
    auto* tools = panel->findChild<QPushButton*>("decklab-browse-tools"); ASSERT_NE(nullptr, tools);
    auto openTools = [this, tools]() -> QMenu* {
        tools->click(); QCoreApplication::processEvents();
        for (auto* menu : panel->findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly))
            if (menu->isVisible()) return menu;
        return nullptr;
    };
    ControlProxy eject("[Channel4]", "eject");
    int ejectRequests = 0;
    ASSERT_TRUE(eject.connectValueChanged(panel.get(), [&ejectRequests](double v) {
        if (v > 0) ++ejectRequests;
    }, Qt::DirectConnection));

    ControlObject::set(ConfigKey("[Channel4]", "play"), 1); ProcessBuffer(); QCoreApplication::processEvents();
    auto* menu = openTools(); ASSERT_NE(nullptr, menu);
    QAction* match = nullptr;
    for (auto* action : menu->actions())
        if (action->text() == QStringLiteral("Match Deck 4")) match = action;
    ASSERT_NE(nullptr, match); ASSERT_TRUE(match->isEnabled()); match->trigger();
    auto* search = panel->findChild<WSearchLineEdit*>(); ASSERT_NE(nullptr, search);
    EXPECT_TRUE(search->currentText().contains(QStringLiteral("~bpm:134.00")));
    auto* unload = menu->findChild<QAction*>("decklab-unload-4"); ASSERT_NE(nullptr, unload);
    EXPECT_FALSE(unload->isEnabled());
    EXPECT_TRUE(fourthDeck->getEngineDeck()->getEngineBuffer()->isTrackLoaded());
    menu->close(); QCoreApplication::processEvents();

    ControlObject::set(ConfigKey("[Channel4]", "play"), 0); ProcessBuffer(); QCoreApplication::processEvents();
    menu = openTools(); ASSERT_NE(nullptr, menu);
    unload = menu->findChild<QAction*>("decklab-unload-4"); ASSERT_NE(nullptr, unload);
    ASSERT_TRUE(unload->isEnabled());
    ControlObject::set(ConfigKey("[Channel4]", "play"), 1); ProcessBuffer(); QCoreApplication::processEvents();
    unload->trigger(); ProcessBuffer(); QCoreApplication::processEvents();
    // The UI must refuse the request itself, beyond the engine's own guard.
    EXPECT_EQ(0, ejectRequests);
    EXPECT_TRUE(fourthDeck->getEngineDeck()->getEngineBuffer()->isTrackLoaded());
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel4]", "play")));
    menu->close(); QCoreApplication::processEvents();

    ControlObject::set(ConfigKey("[Channel4]", "play"), 0); ProcessBuffer(); QCoreApplication::processEvents();
    // Treat the fresh action as one eject, outside Mixxx's double-eject window.
    QTest::qWait(kUnreplaceDelay + 1);
    menu = openTools(); ASSERT_NE(nullptr, menu);
    unload = menu->findChild<QAction*>("decklab-unload-4"); ASSERT_NE(nullptr, unload);
    ASSERT_TRUE(unload->isEnabled()); unload->trigger(); ProcessBuffer(); QCoreApplication::processEvents();
    EXPECT_EQ(1, ejectRequests);
    EXPECT_FALSE(fourthDeck->getEngineDeck()->getEngineBuffer()->isTrackLoaded());
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel4]", "track_loaded")));
    EXPECT_FALSE(fourthDeck->getLoadedTrack());
    EXPECT_TRUE(m_pMixerDeck1->getEngineDeck()->getEngineBuffer()->isTrackLoaded());
    menu->close();
}

TEST_F(NativeDeckLabTest, LayoutRoundTripsKeepModeAndPitchReadoutsInsideEachDeck) {
    m_pMixerDeck1->loadFakeTrack(false,128); m_pMixerDeck2->loadFakeTrack(false,130);
    m_pMixerDeck3->loadFakeTrack(false,132); fourthDeck->loadFakeTrack(false,134); ProcessBuffer();
    auto* choice=panel->findChild<QComboBox*>("decklab-deck-layout");
    for(int index : {2,1,2,1,2}) {
        QMetaObject::invokeMethod(choice,"activated",Q_ARG(int,index));
        panel->selectPage(1); panel->selectPage(0); QCoreApplication::processEvents();
        for(int deck=1;deck<=4;++deck) {
            const QString group=QStringLiteral("[Channel%1]").arg(deck);
            ControlObject::set(ConfigKey(group,"loop_start_position"),0);
            ControlObject::set(ConfigKey(group,"loop_end_position"),44100);
            for(const auto& key:{"loop_enabled","keylock","quantize","sync_enabled"}) ControlObject::set(ConfigKey(group,key),1);
        }
        QTest::qWait(150);
        for(int deck=1;deck<=4;++deck) {
            auto* pitch=panel->findChild<QLabel*>(QStringLiteral("decklab-pitch-%1").arg(deck));
            auto* modes=panel->findChild<QLabel*>(QStringLiteral("decklab-modes-%1").arg(deck));
            if(!pitch->isVisible()) continue;
            EXPECT_TRUE(modes->isVisible()); EXPECT_TRUE(pitch->text().contains("PITCH"));
            EXPECT_TRUE(modes->text().contains("SYNC LOCK"));
            if(index==2) { EXPECT_TRUE(modes->text().contains("LOOP")); EXPECT_TRUE(modes->text().contains("KEYLOCK")); }
            else {
                EXPECT_TRUE(panel->findChild<QPushButton*>(QStringLiteral("decklab-%1-quantize").arg(deck))->isChecked());
                EXPECT_TRUE(panel->findChild<QPushButton*>(QStringLiteral("decklab-%1-keylock").arg(deck))->isChecked());
            }
            EXPECT_LE(modes->fontMetrics().horizontalAdvance(modes->text()),modes->width());
            EXPECT_TRUE(panel->rect().contains(QRect(pitch->mapTo(panel.get(),QPoint()),pitch->size())));
            QList<QLabel*> readouts;
            for (const auto& name : {"numbers", "key", "remaining"}) {
                auto* label = panel->findChild<QLabel*>(QStringLiteral("decklab-%1-%2").arg(name).arg(deck));
                ASSERT_NE(nullptr, label); ASSERT_TRUE(label->isVisible()); readouts.append(label);
                EXPECT_TRUE(label->parentWidget()->rect().contains(label->geometry()))
                        << label->objectName().toStdString() << " layout " << index;
                QTextDocument document; document.setDocumentMargin(0); document.setDefaultFont(label->font());
                document.setHtml(label->text()); document.adjustSize(); document.setTextWidth(-1);
                EXPECT_LE(document.idealWidth(), label->contentsRect().width())
                        << label->objectName().toStdString() << " layout " << index;
            }
            for (int first = 0; first < readouts.size(); ++first) {
                for (int second = first + 1; second < readouts.size(); ++second) {
                    ASSERT_EQ(readouts[first]->parentWidget(), readouts[second]->parentWidget());
                    EXPECT_FALSE(readouts[first]->geometry().intersects(readouts[second]->geometry()))
                            << readouts[first]->objectName().toStdString() << " / "
                            << readouts[second]->objectName().toStdString() << " layout " << index;
                }
            }
        }
    }
    const QString output=qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
    if(!output.isEmpty()) EXPECT_TRUE(panel->grab().save(QDir(output).filePath("decks-fixture-four-modes.png")));
}

TEST_F(NativeDeckLabTest, LatchedRmxFollowsTempoChangesWithoutAnotherTouch) {
    m_pEffectsManager->setup(); m_pEffectsManager->ensureDeckLabEffects();
    m_pEngineMixer->prepareAudio(mixxx::audio::SampleRate(44100));
    m_pMixerDeck1->loadFakeTrack(false, 128); ProcessBuffer(); QCoreApplication::processEvents();
    panel->selectPage(3); panel->findChild<QPushButton*>("decklab-fx-band-0")->click();
    ControlObject::set(ConfigKey("[Channel1]", "bpm"), 140);
    QTest::qWait(150);
    EXPECT_DOUBLE_EQ(140, ControlObject::get(ConfigKey("[DeckLabEffectUnit1_Effect4]", "parameter11")));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "play")));
}

TEST_F(NativeDeckLabTest, TransitionStartersApplyOwnedRmxAndOpenItsEditor) {
    m_pEffectsManager->setup(); m_pEffectsManager->ensureDeckLabEffects();
    m_pEngineMixer->prepareAudio(mixxx::audio::SampleRate(44100));
    m_pMixerDeck1->loadFakeTrack(false, 128); ProcessBuffer(); QCoreApplication::processEvents();
    panel->selectPage(3);
    panel->findChild<QPushButton*>("decklab-fx-view-2")->click();
    panel->findChild<QPushButton*>("decklab-transition-0")->click();
    EXPECT_EQ(0, panel->findChild<QStackedWidget*>("decklab-fx-pages")->currentIndex());
    for (int band = 1; band <= 3; ++band)
        EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[DeckLabEffectUnit1_Effect4]", QStringLiteral("parameter%1").arg(band))));
    panel->findChild<QPushButton*>("decklab-fx-view-2")->click();
    panel->findChild<QPushButton*>("decklab-transition-3")->click();
    EXPECT_EQ(1, panel->findChild<QStackedWidget*>("decklab-fx-pages")->currentIndex());
    EXPECT_DOUBLE_EQ(4, ControlObject::get(ConfigKey("[DeckLabEffectUnit1_Effect4]", "parameter9")));
    EXPECT_NEAR(.7, ControlObject::get(ConfigKey("[DeckLabEffectUnit1_Effect4]", "parameter6")), 1e-6);
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "play")));
}

TEST_F(NativeDeckLabTest, RollPadsRequireAPlayingDeck) {
    m_pEffectsManager->setup(); m_pEffectsManager->ensureDeckLabEffects();
    m_pEngineMixer->prepareAudio(mixxx::audio::SampleRate(44100));
    m_pMixerDeck1->loadFakeTrack(false, 128); ProcessBuffer(); QCoreApplication::processEvents();
    panel->selectPage(3);
    auto* roll = panel->findChild<QPushButton*>("decklab-roll-0"); ASSERT_NE(nullptr, roll);
    EXPECT_FALSE(roll->isEnabled());
    ControlObject::set(ConfigKey("[Channel1]", "play"), 1); QCoreApplication::processEvents();
    EXPECT_TRUE(roll->isEnabled());
    ControlObject::set(ConfigKey("[Channel1]", "play"), 0); QCoreApplication::processEvents();
    EXPECT_FALSE(roll->isEnabled());
}

#ifdef __LILV__
TEST_F(NativeDeckLabTest, Lv2PreparationUsesSelectedRateWithoutAudio) {
    auto manifest = m_pEffectsManager->getBackendManager()->getManifest(
            "urn:decklab:threeband-performance-native-v1", EffectBackendType::LV2).staticCast<LV2Manifest>();
    ASSERT_FALSE(manifest.isNull());
    LV2EffectGroupState state(mixxx::EngineParameters(mixxx::audio::SampleRate(96000), 512));
    ASSERT_NE(nullptr, state.lilvInstance(manifest->getPlugin(), mixxx::EngineParameters(mixxx::audio::SampleRate(96000), 512)));
    for (int rate : {44100, 48000, 96000, 48000}) {
        EXPECT_TRUE(state.prepare(mixxx::audio::SampleRate(rate)));
        EXPECT_EQ(mixxx::audio::SampleRate(rate), state.sampleRate());
        auto* instance = state.lilvInstance(manifest->getPlugin(), mixxx::EngineParameters(mixxx::audio::SampleRate(rate), 512));
        ASSERT_NE(nullptr, instance);
        EXPECT_FALSE(state.prepare(mixxx::audio::SampleRate(rate)));
        EXPECT_EQ(instance, state.lilvInstance(manifest->getPlugin(), mixxx::EngineParameters(mixxx::audio::SampleRate(rate), 512)));
        m_pEngineMixer->prepareAudio(mixxx::audio::SampleRate(rate));
        EXPECT_EQ(mixxx::audio::SampleRate(rate), m_pEffectsManager->preparedSampleRate());
    }
}
#endif

TEST(DeckLabTempoTest, JitterIsSmoothedButPitchChangesImmediately) {
    DeckLabTempoPresenter display;
    const double initial = display.update(true, 141.97, 1, 141.97, 0);
    for (int n = 1; n < 8; ++n) EXPECT_EQ(initial, display.update(true, n % 2 ? 141.93 : 141.97, 1, 141.93, n * 100));
    EXPECT_DOUBLE_EQ(144.81, display.update(true, 141.97, 1.02, 144.8, 800));
    EXPECT_EQ(130, display.update(true, 130, 1, 130, 900));
    EXPECT_EQ(0, display.update(false, 130, 1, 130, 1000));
}
