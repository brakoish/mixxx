// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <QAction>
#include <QAccessible>
#include <QAbstractItemDelegate>
#include <QAbstractItemView>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QDialog>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QProgressBar>
#include <QScrollArea>
#include <QScrollBar>
#include <QScroller>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTest>
#include <QTextDocument>
#include <QTextBrowser>

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
#include "track/beats.h"
#include "waveform/waveformwidgetfactory.h"
#include "widget/decklabtempopresenter.h"
#include "widget/wdecklab.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarysidebar.h"
#include "widget/wsearchlineedit.h"
#include "widget/wwaveformviewer.h"
#include "widget/woverview.h"
#include "widget/controlwidgetconnection.h"
#include "skin/legacy/skincontext.h"
#include "util/valuetransformer.h"
#ifdef __LILV__
#include "effects/backends/lv2/lv2effectprocessor.h"
#include "effects/backends/lv2/lv2manifest.h"
#endif

namespace {
void swipeTouch(QWidget* viewport, const QPoint& from, const QPoint& to, int holdBeforeRelease = 0) {
    static auto* device = QTest::createTouchDevice();
    auto touch = QTest::touchEvent(viewport, device, false);
    touch.press(0, from, viewport).commit();
    QTest::qWait(40);
    for (int step = 1; step <= 8; ++step) {
        touch.move(0, from + (to - from) * step / 8, viewport).commit();
        QTest::qWait(30);
    }
    if (holdBeforeRelease > 0) QTest::qWait(holdBeforeRelease);
    touch.release(0, to, viewport).commit();
    QTest::qWait(100);
    QScroller::scroller(viewport)->stop();
    QCoreApplication::processEvents();
}
void tapTouch(QWidget* viewport, const QPoint& point) {
    static auto* device = QTest::createTouchDevice();
    auto touch = QTest::touchEvent(viewport, device, false);
    touch.press(0, point, viewport).commit(); QTest::qWait(40);
    touch.release(0, point, viewport).commit(); QTest::qWait(100);
}

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
        config()->setValue(ConfigKey("[DeckLab]", "WaveformLayout"), 0);
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

TEST_F(NativeDeckLabTest, WholeTrackOverviewFollowsPositionAcrossDecksLayoutsAndResize) {
    QFile file(QDir(config()->getResourcePath()).filePath("skins/DeckLab/skin.xml"));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly)) << file.fileName().toStdString();
    QDomDocument skin; ASSERT_TRUE(skin.setContent(&file));
    const auto nodes=skin.elementsByTagName("Overviews").at(0).childNodes();
    std::array<WOverview*,4> overviews{};
    SkinContext context(config(),QString());
    for(int deck=1;deck<=4;++deck) {
        const QString group=QStringLiteral("[Channel%1]").arg(deck);
        const std::array<Deck*,4> decks{m_pMixerDeck1.get(),m_pMixerDeck2.get(),m_pMixerDeck3.get(),fourthDeck.get()};
        const auto track=decks[deck-1]->loadFakeTrack(false,128+deck);
        ProcessBuffer();
        QDomElement node;
        for(int n=0;n<nodes.size();++n) {
            auto candidate=nodes.at(n).toElement();
            if(candidate.firstChildElement("Channel").text()==QString::number(deck)) node=candidate;
        }
        ASSERT_FALSE(node.isNull());
        const auto connection=node.firstChildElement("Connection");
        EXPECT_EQ(group+",playposition",connection.firstChildElement("ConfigKey").text());
        ASSERT_EQ(QStringLiteral("false"),connection.firstChildElement("ConnectValueFromWidget").text());
        auto* overview=new WOverview(group,nullptr,config(),panel.get());
        overview->addAndSetDisplayConnection(std::make_unique<ControlParameterWidgetConnection>(overview,
                ConfigKey(group,"playposition"),nullptr,ControlParameterWidgetConnection::DIR_TO_WIDGET,
                ControlParameterWidgetConnection::EMIT_NEVER),WBaseWidget::ConnectionSide::None);
        overview->setup(node,context); overview->initWithTrack(track);
        auto* old=panel->findChild<QWidget*>(QStringLiteral("decklab-overview-%1").arg(deck)); delete old;
        panel->addOverview(overview,deck); overviews[deck-1]=overview;
        ControlObject::set(ConfigKey(group,"quantize"),0);
    }
    auto* layout=panel->findChild<QComboBox*>("decklab-deck-layout");
    auto* waves=panel->findChild<QComboBox*>("decklab-waveform-layout");
    for(int waveLayout : {0,1}) for(int deckLayout : {2,1,2}) {
        waves->setCurrentIndex(waveLayout); QMetaObject::invokeMethod(waves,"activated",Q_ARG(int,waveLayout));
        QMetaObject::invokeMethod(layout,"activated",Q_ARG(int,deckLayout)); QTest::qWait(100);
        for(int deck=1;deck<=4;++deck) for(double fraction : {.25,.75,.5}) {
            const QString group=QStringLiteral("[Channel%1]").arg(deck);
            ControlObject::set(ConfigKey(group,"playposition"),fraction); ProcessBuffer(); QTest::qWait(30);
            auto* overview=overviews[deck-1];
            EXPECT_NEAR(fraction,overview->getControlParameterDisplay(),.001);
            if(overview->isVisible()) {
                const QImage image=overview->grab().toImage(); const int x=int((overview->width()-1)*overview->getControlParameterDisplay());
                EXPECT_EQ(QColor(Qt::white),image.pixelColor(x,22));
                EXPECT_EQ(44,overview->height());
            }
            EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey(group,"play")));
        }
    }
}

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

TEST_F(NativeDeckLabTest, SettingsTouchSwipeRevealsDeckFourWithoutTogglingQuantize) {
    panel->selectPage(4); QTest::qWait(50);
    for (int category = 0; category < 5; ++category) {
        auto* scroll = panel->findChild<QScrollArea*>(QStringLiteral("decklab-settings-scroll-%1").arg(category));
        ASSERT_NE(nullptr, scroll);
        EXPECT_TRUE(QScroller::hasScroller(scroll->viewport()));
        EXPECT_NE(static_cast<Qt::GestureType>(0), QScroller::grabbedGesture(scroll->viewport()));
    }
    auto* scroll = panel->findChild<QScrollArea*>("decklab-settings-scroll-0");
    auto* viewport = scroll->viewport();
    auto* deckFour = panel->findChild<QComboBox*>("decklab-tempo-range-4"); ASSERT_NE(nullptr, deckFour);
    const auto fullyVisible = [viewport](QWidget* widget) {
        return viewport->rect().contains(QRect(widget->mapTo(viewport, QPoint()), widget->size()));
    };
    ASSERT_GT(scroll->verticalScrollBar()->maximum(), 0);
    ASSERT_FALSE(fullyVisible(deckFour));
    std::array<double, 4> before;
    for (int deck = 1; deck <= 4; ++deck)
        before[deck - 1] = ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), "quantize"));
    QPushButton* quantize = nullptr;
    for (auto* button : scroll->findChildren<QPushButton*>())
        if (button->text() == "Quantize") { quantize = button; break; }
    ASSERT_NE(nullptr, quantize);
    const QPoint start = quantize->mapTo(viewport, quantize->rect().center());
    swipeTouch(viewport, start, QPoint(start.x(), 10), 400);
    EXPECT_GT(scroll->verticalScrollBar()->value(), 0);
    for (int attempt = 0; attempt < 8 && !fullyVisible(deckFour); ++attempt)
        swipeTouch(viewport, QPoint(viewport->width() / 2, viewport->height() - 30),
                QPoint(viewport->width() / 2, 30));
    EXPECT_TRUE(fullyVisible(deckFour));
    for (int deck = 1; deck <= 4; ++deck)
        EXPECT_DOUBLE_EQ(before[deck - 1], ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), "quantize")));
    // Ordinary row taps remain commands after a swipe; no scrollbar or
    // ensureWidgetVisible call is used to reach the fourth deck.
    QPushButton* fourthQuantize = nullptr;
    for (auto* button : scroll->findChildren<QPushButton*>())
        if (button->text() == "Quantize") fourthQuantize = button;
    ASSERT_NE(nullptr, fourthQuantize); ASSERT_TRUE(fullyVisible(fourthQuantize));
    tapTouch(viewport, fourthQuantize->mapTo(viewport, fourthQuantize->rect().center()));
    EXPECT_DOUBLE_EQ(before[3] > 0 ? 0 : 1, ControlObject::get(ConfigKey("[Channel4]", "quantize")));
    QTest::mouseClick(fourthQuantize, Qt::LeftButton);
    EXPECT_DOUBLE_EQ(before[3], ControlObject::get(ConfigKey("[Channel4]", "quantize")));
}

TEST_F(NativeDeckLabTest, SourcesTouchSwipeRevealsOffscreenRowsWithoutOpeningThem) {
    auto root = TreeItem::newRoot(source.get());
    for (int row = 0; row < 24; ++row) root->appendChild(QStringLiteral("Source %1").arg(row + 1));
    source->model.setRootItem(std::move(root));
    panel->browsePress(); panel->browsePress(); QTest::qWait(50);
    ASSERT_TRUE(sidebar->isVisible());
    auto* viewport = sidebar->viewport();
    EXPECT_EQ(QAbstractItemView::ScrollPerPixel, sidebar->verticalScrollMode());
    EXPECT_NE(static_cast<Qt::GestureType>(0), QScroller::grabbedGesture(viewport));
    const QPersistentModelIndex parent = sidebar->rootIndex();
    const QModelIndex last = model->index(23, 0, parent); ASSERT_TRUE(last.isValid());
    ASSERT_FALSE(viewport->rect().contains(sidebar->visualRect(last)));
    ASSERT_GT(sidebar->verticalScrollBar()->maximum(), 0);
    const int before = source->activations;
    for (int attempt = 0; attempt < 8 && !viewport->rect().contains(sidebar->visualRect(last)); ++attempt)
        swipeTouch(viewport, QPoint(viewport->width() / 2, viewport->height() - 25),
                QPoint(viewport->width() / 2, 25), attempt == 0 ? 400 : 0);
    EXPECT_GT(sidebar->verticalScrollBar()->value(), 0);
    EXPECT_TRUE(viewport->rect().contains(sidebar->visualRect(last)));
    EXPECT_EQ(before, source->activations); EXPECT_EQ(parent, sidebar->rootIndex());
    EXPECT_TRUE(sidebar->isVisible()); EXPECT_FALSE(library->isVisible());
    tapTouch(viewport, sidebar->visualRect(last).center());
    QCoreApplication::processEvents();
    EXPECT_EQ(before + 1, source->activations); EXPECT_TRUE(library->isVisible());
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
            // The same header is a display-only compact readout away from
            // Decks; it must not expose a 24px information action there.
            if (page != 0 && button->objectName().startsWith("decklab-info-hit-")) {
                const QString deck=button->objectName().mid(QStringLiteral("decklab-info-hit-").size());
                auto* affordance=panel->findChild<QLabel*>(QStringLiteral("decklab-info-affordance-")+deck);
                ASSERT_NE(nullptr,affordance); EXPECT_FALSE(affordance->isVisible());
                EXPECT_FALSE(button->isEnabled()); EXPECT_EQ(Qt::NoFocus,button->focusPolicy()); continue;
            }
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
            auto* more = panel->findChild<QPushButton*>("decklab-1-more");
            EXPECT_TRUE(more->isVisible()); EXPECT_FALSE(more->visibleRegion().isEmpty());
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

TEST_F(NativeDeckLabTest, WaveformLayoutPreferenceRoundTripsBothDeckCountsWithoutEngineWrites) {
    QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"), "activated", Q_ARG(int, 2));
    QTest::qWait(30);
    for (int deck = 1; deck <= 4; ++deck) {
        auto* title = panel->findChild<QLabel*>(QStringLiteral("decklab-wave-title-%1").arg(deck)); ASSERT_NE(nullptr, title);
        EXPECT_EQ(QStringLiteral("Browse tracks"), title->text());
        EXPECT_LE(title->fontMetrics().boundingRect(QRect(0, 0, title->width(), 1000), Qt::TextWordWrap, title->text()).height(), title->height());
    }
    QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"), "activated", Q_ARG(int, 1));
    m_pMixerDeck1->loadFakeTrack(false, 128);
    m_pMixerDeck2->loadFakeTrack(false, 130);
    m_pMixerDeck3->loadFakeTrack(false, 132); fourthDeck->loadFakeTrack(false, 134);
    ProcessBuffer(); QTest::qWait(150);
    auto* choice = panel->findChild<QComboBox*>("decklab-waveform-layout"); ASSERT_NE(nullptr, choice);
    EXPECT_EQ(0, choice->currentData().toInt());
    EXPECT_EQ(QStringLiteral("Stacked"), choice->itemText(0));
    EXPECT_EQ(QStringLiteral("Side by side"), choice->itemText(1));
    ControlObject::set(ConfigKey("[Channel1]", "play"), 1);
    std::vector<std::unique_ptr<ControlProxy>> observed; int writes = 0;
    for (int deck = 1; deck <= 4; ++deck)
        for (const auto& item : {"play", "rate", "sync_enabled", "waveform_zoom", "pfl"}) {
            observed.emplace_back(std::make_unique<ControlProxy>(QStringLiteral("[Channel%1]").arg(deck), QString::fromLatin1(item)));
            ASSERT_TRUE(observed.back()->connectValueChanged(panel.get(), [&writes](double) { ++writes; }, Qt::DirectConnection));
        }
    for (int mode : {1, 2, 1}) {
        QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"), "activated", Q_ARG(int, mode));
        for (int layout : {1, 0, 1, 0}) {
            choice->setCurrentIndex(layout);
            QMetaObject::invokeMethod(choice, "activated", Q_ARG(int, layout));
            QTest::qWait(30);
            EXPECT_EQ(layout, config()->getValue(ConfigKey("[DeckLab]", "WaveformLayout"), -1));
            const int count = mode == 2 ? 4 : 2;
            for (int deck = 1; deck <= count; ++deck) {
                auto* detail = panel->findChild<QWidget*>(QStringLiteral("decklab-detail-%1").arg(deck)); ASSERT_NE(nullptr, detail);
                ASSERT_TRUE(detail->isVisible());
                const QPoint at = detail->mapTo(panel.get(), QPoint());
                EXPECT_EQ(layout ? QSize(344, mode == 2 ? 66 : 176) : QSize(576, mode == 2 ? 55 : 110), detail->size());
                EXPECT_EQ(layout ? QPoint((deck - 1) % 2 * 344, 84 + (mode == 2 ? (deck - 1) / 2 * 110 : 0))
                                 : QPoint(112, 40 + (deck - 1) * (mode == 2 ? 55 : 110)), at);
                EXPECT_TRUE(panel->rect().contains(QRect(at, detail->size())));
                auto* overview = panel->findChild<QWidget*>(QStringLiteral("decklab-overview-%1").arg(deck));
                ASSERT_NE(nullptr, overview); EXPECT_TRUE(overview->isVisible());
                EXPECT_EQ(mode == 2 ? 200 : 400, overview->width()); EXPECT_EQ(44, overview->height());
            }
            EXPECT_EQ(0, writes);
            panel->selectPage(1); QCoreApplication::processEvents();
            EXPECT_FALSE(panel->findChild<QWidget*>("decklab-separate-wave-1")->isVisible());
            panel->selectPage(0); QCoreApplication::processEvents();
        }
    }
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel1]", "play")));
    // Stop the generated fixture explicitly before testing a canceled Play.
    ControlObject::set(ConfigKey("[Channel1]", "play"), 0); writes = 0;
    // An in-flight transport gesture cannot survive a presentation change.
    auto* more = panel->findChild<QPushButton*>("decklab-1-more"); more->click(); QCoreApplication::processEvents();
    auto* play = panel->findChild<QPushButton*>("decklab-1-play"); ASSERT_TRUE(play->isVisible());
    QTest::mousePress(play, Qt::LeftButton);
    choice->setCurrentIndex(1); QMetaObject::invokeMethod(choice, "activated", Q_ARG(int, 1));
    QTest::mouseRelease(play, Qt::LeftButton);
    EXPECT_EQ(0, writes);
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "play")));
    // A new surface reads the saved preference rather than silently resetting it.
    WDeckLab restored(config(), nullptr, players.get(), m_pEffectsManager.get());
    for (int deck = 1; deck <= 4; ++deck) {
        restored.addWaveform(new QWidget(&restored), deck);
        restored.addOverview(new QWidget(&restored), deck);
    }
    restored.resize(800, 480); restored.show(); restored.selectPage(0); QTest::qWait(30);
    EXPECT_EQ(1, restored.findChild<QComboBox*>("decklab-waveform-layout")->currentData().toInt());
    EXPECT_EQ(QSize(344,176), restored.findChild<QWidget*>("decklab-detail-1")->size());
    EXPECT_EQ(0, writes);
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
    // The 600ms feedback deadline is repainted by a 100ms UI timer. Wait for
    // its actual expiry rather than assuming CI dispatches that timer within
    // the single remaining tick of a fixed 700ms sleep.
    EXPECT_TRUE(QTest::qWaitFor([sync] { return !sync->property("actionFeedback").toBool(); }, 1500));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "sync_enabled")));
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
    EXPECT_EQ(QSize(576,110), panel->findChild<QWidget*>("decklab-detail-1")->size());
    EXPECT_EQ(QSize(400,44), panel->findChild<QWidget*>("decklab-overview-1")->size());
    EXPECT_EQ(QSize(400,44), panel->findChild<QPushButton*>("decklab-1-more")->parentWidget()->size());
    for(int deck=1;deck<=2;++deck) {
        auto* detail=panel->findChild<QWidget*>(QStringLiteral("decklab-detail-%1").arg(deck));
        EXPECT_EQ(QPoint(112,40+(deck-1)*110),detail->mapTo(panel.get(),QPoint()));
        auto* overview=panel->findChild<QWidget*>(QStringLiteral("decklab-overview-%1").arg(deck));
        EXPECT_EQ(QPoint((deck-1)*400,348),overview->mapTo(panel.get(),QPoint()));
    }
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
    EXPECT_TRUE(panel->findChild<QPushButton*>("decklab-1-more")->isVisible());
    ControlObject::set(ConfigKey("[DeckLabNative]", "layout_decks"), 4); QCoreApplication::processEvents();
    EXPECT_TRUE(panel->findChild<QPushButton*>("decklab-card-4-play")->isVisible());
    EXPECT_FALSE(panel->findChild<QPushButton*>("decklab-1-more")->isVisible());
    ControlObject::set(ConfigKey("[DeckLabNative]", "layout_decks"), 2); QTest::qWait(150);
    EXPECT_TRUE(panel->findChild<QPushButton*>("decklab-card-4-play")->isVisible());
    for(int side=1;side<=2;++side) {
        for(const auto& suffix:{"more","loop_halve","beatloop_activate","loop_double","quantize","keylock","beatsync"}) {
            auto* action=panel->findChild<QPushButton*>(QStringLiteral("decklab-%1-%2").arg(side).arg(QString::fromLatin1(suffix)));
            ASSERT_NE(nullptr,action); EXPECT_FALSE(action->isVisible()) << action->objectName().toStdString();
        }
        for(int cue=1;cue<=8;++cue) {
            auto* pad=panel->findChild<QPushButton*>(QStringLiteral("decklab-hotcue-%1-%2").arg(side).arg(cue));
            ASSERT_NE(nullptr,pad); EXPECT_FALSE(pad->isVisible());
        }
    }
    auto* play = panel->findChild<QPushButton*>("decklab-card-4-play"); ASSERT_TRUE(play->isEnabled());
    play->click(); QTest::qWait(150);
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel4]", "play")));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel2]", "play")));
    EXPECT_EQ(QStringLiteral("Pause"), play->text());
    const QString output = qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
    if (!output.isEmpty()) EXPECT_TRUE(panel->grab().save(QDir(output).filePath("decks-fixture-four.png")));
    for (int deck = 1; deck <= 4; ++deck) {
        for(const auto& suffix:{"cue_default","play","beatsync","more"}) {
            auto* action=panel->findChild<QPushButton*>(QStringLiteral("decklab-card-%1-%2").arg(deck).arg(QString::fromLatin1(suffix)));
            ASSERT_NE(nullptr,action); EXPECT_TRUE(action->isVisible());
            EXPECT_GE(action->height(),44); EXPECT_GE(action->width(),44);
            EXPECT_TRUE(panel->rect().contains(QRect(action->mapTo(panel.get(),QPoint()),action->size())));
        }
        auto* cue = panel->findChild<QPushButton*>(QStringLiteral("decklab-card-%1-cue_default").arg(deck));
        EXPECT_EQ(48, cue->height()); EXPECT_GE(cue->width(), 44);
        auto* detail = panel->findChild<QWidget*>(QStringLiteral("decklab-detail-%1").arg(deck)); ASSERT_NE(nullptr,detail);
        EXPECT_EQ(QSize(576,55),detail->size());
        EXPECT_EQ(QPoint(112,40+(deck-1)*55),detail->mapTo(panel.get(),QPoint()));
        auto* overview = panel->findChild<QWidget*>(QStringLiteral("decklab-overview-%1").arg(deck));
        ASSERT_NE(nullptr, overview); EXPECT_TRUE(overview->isVisible()); EXPECT_EQ(44, overview->height());
        EXPECT_EQ(200,overview->width());
        EXPECT_EQ(QPoint((deck-1)*200,388),overview->mapTo(panel.get(),QPoint()));
        EXPECT_EQ(432,cue->mapTo(panel.get(),QPoint()).y());
        EXPECT_TRUE(overview->testAttribute(Qt::WA_TransparentForMouseEvents)); EXPECT_FALSE(overview->acceptDrops());
        EXPECT_TRUE(panel->rect().contains(QRect(cue->mapTo(panel.get(), QPoint()), cue->size())));
    }
    EXPECT_EQ(QSize(800,480), panel->size());
    // A deliberate return to Two must remove all direct-card transports and
    // restore both physical-side rows, including the real cue pads.
    QMetaObject::invokeMethod(choice,"activated",Q_ARG(int,1)); QTest::qWait(150);
    for(int side=1;side<=2;++side) {
        for(const auto& suffix:{"more","loop_halve","beatloop_activate","loop_double","quantize","keylock","beatsync"}) {
            auto* action=panel->findChild<QPushButton*>(QStringLiteral("decklab-%1-%2").arg(side).arg(QString::fromLatin1(suffix)));
            ASSERT_NE(nullptr,action); EXPECT_TRUE(action->isVisible());
            EXPECT_GE(action->height(),44); EXPECT_GE(action->width(),44);
            EXPECT_TRUE(panel->rect().contains(QRect(action->mapTo(panel.get(),QPoint()),action->size())));
        }
        for(int cue=1;cue<=8;++cue) {
            auto* pad=panel->findChild<QPushButton*>(QStringLiteral("decklab-hotcue-%1-%2").arg(side).arg(cue));
            ASSERT_NE(nullptr,pad); EXPECT_TRUE(pad->isVisible());
        }
    }
    for(int deck=1;deck<=4;++deck) for(const auto& suffix:{"cue_default","play","beatsync","more"}) {
        auto* action=panel->findChild<QPushButton*>(QStringLiteral("decklab-card-%1-%2").arg(deck).arg(QString::fromLatin1(suffix)));
        ASSERT_NE(nullptr,action); EXPECT_FALSE(action->isVisible());
    }
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

TEST_F(NativeDeckLabTest, TouchHotcuesSetAndJumpWithoutPreviewOrRetargeting) {
    auto first = m_pMixerDeck1->loadFakeTrack(false,128);
    m_pMixerDeck3->loadFakeTrack(false,132); ProcessBuffer();
    ControlObject::set(ConfigKey("[Channel1]","quantize"),0); QTest::qWait(150);
    auto* a=panel->findChild<QPushButton*>("decklab-hotcue-1-1");
    auto* b=panel->findChild<QPushButton*>("decklab-hotcue-1-2");
    ASSERT_NE(nullptr,a); ASSERT_NE(nullptr,b);
    EXPECT_EQ(QStringLiteral("A+"),a->text()); EXPECT_TRUE(a->isEnabled());
    for(int cue=1;cue<=8;++cue) {
        auto* pad=panel->findChild<QPushButton*>(QStringLiteral("decklab-hotcue-1-%1").arg(cue));
        ASSERT_NE(nullptr,pad); EXPECT_TRUE(pad->isVisible()); EXPECT_EQ(44,pad->height()); EXPECT_GE(pad->width(),44);
    }
    ControlObject::set(ConfigKey("[Channel1]","playposition"),.25); ProcessBuffer();
    a->click(); QCoreApplication::processEvents(); ProcessBuffer(); QTest::qWait(150);
    const double cuePosition=ControlObject::get(ConfigKey("[Channel1]","hotcue_1_position"));
    ASSERT_GE(cuePosition,0); EXPECT_EQ(QStringLiteral("A"),a->text());
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel1]","play")));
    ControlObject::set(ConfigKey("[Channel1]","playposition"),.75); ProcessBuffer();
    a->click(); QCoreApplication::processEvents(); ProcessBuffer();
    EXPECT_NEAR(cuePosition,m_pChannel1->getEngineBuffer()->getExactPlayPos().toEngineSamplePos(),4);
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel1]","play")));
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel1]","hotcue_1_activate")));
    ControlObject::set(ConfigKey("[Channel1]","play"),1); ProcessBuffer(); QTest::qWait(150);
    EXPECT_TRUE(a->isEnabled()); EXPECT_FALSE(b->isEnabled());
    a->click(); QCoreApplication::processEvents(); ProcessBuffer();
    EXPECT_DOUBLE_EQ(1,ControlObject::get(ConfigKey("[Channel1]","play")));
    EXPECT_LT(m_pChannel1->getEngineBuffer()->getExactPlayPos().toEngineSamplePos(),cuePosition+44100);
    ControlObject::set(ConfigKey("[Channel1]","play"),0); ProcessBuffer(); QTest::qWait(150);
    QTest::mousePress(a,Qt::LeftButton);
    ControlObject::set(ConfigKey("[DeckLabLeft]","current"),1); QCoreApplication::processEvents();
    QTest::mouseRelease(a,Qt::LeftButton);
    EXPECT_LT(ControlObject::get(ConfigKey("[Channel3]","hotcue_1_position")),0);
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel3]","play")));
    ControlObject::set(ConfigKey("[DeckLabLeft]","current"),0); QCoreApplication::processEvents();
    QTest::mousePress(a,Qt::LeftButton);
    auto replacement=m_pMixerDeck1->loadFakeTrack(false,140); ProcessBuffer(); QCoreApplication::processEvents();
    ASSERT_NE(first,replacement); QTest::mouseRelease(a,Qt::LeftButton);
    EXPECT_LT(ControlObject::get(ConfigKey("[Channel1]","hotcue_1_position")),0);
    QTest::qWait(150); QTest::mousePress(b,Qt::LeftButton);
    QEvent cancel(QEvent::TouchCancel); QCoreApplication::sendEvent(panel.get(),&cancel);
    QTest::mouseRelease(b,Qt::LeftButton);
    EXPECT_LT(ControlObject::get(ConfigKey("[Channel1]","hotcue_2_position")),0);
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel1]","play")));
}

TEST_F(NativeDeckLabTest, HotcueBadgesRenderSavedColorsAndKeepEmptySlotsQuiet) {
    m_pMixerDeck1->loadFakeTrack(false,128); ProcessBuffer(); QTest::qWait(150);
    // Reference colors are fixture data, never a rewrite of a user's cue palette.
    const std::array<int,8> colors{0xe53935,0x00c8e5,0xb6dc24,0xa75be8,0x2bcb58,0xff922b,0x365bdb,0xffde32};
    const auto renderedColor = [](QPushButton* pad) {
        const QImage image=pad->grab().toImage();
        return image.pixelColor((pad->width()-26)/2+3,(pad->height()-26)/2+3);
    };
    for(int cue=1;cue<=8;++cue) {
        auto* pad=panel->findChild<QPushButton*>(QStringLiteral("decklab-hotcue-1-%1").arg(cue)); ASSERT_NE(nullptr,pad);
        EXPECT_FALSE(pad->property("cueSet").toBool());
        pad->click(); ProcessBuffer();
        ControlObject::set(ConfigKey("[Channel1]",QStringLiteral("hotcue_%1_color").arg(cue)),colors[cue-1]);
    }
    QTest::qWait(150);
    for(int cue=1;cue<=8;++cue) {
        auto* pad=panel->findChild<QPushButton*>(QStringLiteral("decklab-hotcue-1-%1").arg(cue));
        EXPECT_TRUE(pad->property("cueSet").toBool());
        EXPECT_EQ(QColor::fromRgb(colors[cue-1]),renderedColor(pad));
        EXPECT_EQ(44,pad->height()); EXPECT_GE(pad->width(),44);
        EXPECT_EQ(QString(QChar('A'+cue-1)),pad->text());
        EXPECT_DOUBLE_EQ(colors[cue-1],ControlObject::get(ConfigKey("[Channel1]",QStringLiteral("hotcue_%1_color").arg(cue))));
    }
    const QString output=qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
    if(!output.isEmpty()) EXPECT_TRUE(panel->grab().save(QDir(output).filePath("hotcue-badges-stacked.png")));
    auto* waves=panel->findChild<QComboBox*>("decklab-waveform-layout"); ASSERT_NE(nullptr,waves);
    waves->setCurrentIndex(1); QMetaObject::invokeMethod(waves,"activated",Q_ARG(int,1)); QTest::qWait(150);
    if(!output.isEmpty()) EXPECT_TRUE(panel->grab().save(QDir(output).filePath("hotcue-badges-side.png")));
    auto* a=panel->findChild<QPushButton*>("decklab-hotcue-1-1");
    for(int color : {0x000000,0xffffff}) {
        ControlObject::set(ConfigKey("[Channel1]","hotcue_1_color"),color); QTest::qWait(150);
        EXPECT_EQ(QColor::fromRgb(color),renderedColor(a));
        // The badge contains a contrasting letter even for extreme imported colors.
        const QImage image=a->grab().toImage(); const QColor ink=color==0?Qt::white:Qt::black;
        int contrasting=0;
        for(int y=12;y<32;++y) for(int x=a->width()/2-7;x<a->width()/2+7;++x)
            if(image.pixelColor(x,y)==ink) ++contrasting;
        EXPECT_GT(contrasting,5);
    }
    m_pMixerDeck1->loadFakeTrack(false,132); ProcessBuffer(); QTest::qWait(150);
    EXPECT_FALSE(a->property("cueSet").toBool()); EXPECT_EQ(QStringLiteral("A+"),a->text());
    EXPECT_NE(QColor(Qt::white),renderedColor(a));
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel1]","play")));
}

TEST_F(NativeDeckLabTest, MorePanelKeepsHeldCueAndFullTrackInformationAccessible) {
    auto track=m_pMixerDeck1->loadFakeTrack(false,128);
    const QString title=QString(140,QChar('W'))+QStringLiteral(" full title");
    track->setTitle(title); track->setArtist(QStringLiteral("Full artist")); ProcessBuffer(); QTest::qWait(150);
    auto* more=panel->findChild<QPushButton*>("decklab-1-more"); ASSERT_NE(nullptr,more);
    more->click(); QCoreApplication::processEvents();
    auto* menu=panel->findChild<QMenu*>("decklab-1-more-menu"); ASSERT_NE(nullptr,menu); ASSERT_TRUE(menu->isVisible());
    auto* cue=panel->findChild<QPushButton*>("decklab-1-cue_default"); ASSERT_NE(nullptr,cue); ASSERT_TRUE(cue->isVisible());
    QTest::mousePress(cue,Qt::LeftButton); EXPECT_DOUBLE_EQ(1,ControlObject::get(ConfigKey("[Channel1]","cue_default")));
    ProcessBuffer(); QTest::qWait(150);
    EXPECT_TRUE(menu->isVisible()); EXPECT_DOUBLE_EQ(1,ControlObject::get(ConfigKey("[Channel1]","cue_default")));
    menu->close(); QCoreApplication::processEvents();
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel1]","cue_default")));
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel1]","play")));
    m_pMixerDeck3->loadFakeTrack(false,132); ProcessBuffer(); QTest::qWait(150);
    for(int change=0;change<3;++change) {
        more->click(); QCoreApplication::processEvents(); QTest::mousePress(cue,Qt::LeftButton);
        ProcessBuffer(); QTest::qWait(150);
        EXPECT_TRUE(menu->isVisible()); EXPECT_DOUBLE_EQ(1,ControlObject::get(ConfigKey("[Channel1]","cue_default")));
        if(change==0) ControlObject::set(ConfigKey("[DeckLabLeft]","current"),1);
        else if(change==1) panel->selectPage(1);
        else QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"),"activated",Q_ARG(int,2));
        QCoreApplication::processEvents(); ProcessBuffer(); QTest::qWait(20);
        EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel1]","cue_default")));
        EXPECT_FALSE(menu->isVisible());
        QTest::mouseRelease(cue,Qt::LeftButton);
        QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"),"activated",Q_ARG(int,1));
        ControlObject::set(ConfigKey("[DeckLabLeft]","current"),0); panel->selectPage(0); QTest::qWait(150);
    }
    more->click(); QCoreApplication::processEvents();
    auto* info=panel->findChild<QPushButton*>("decklab-1-track-info"); ASSERT_NE(nullptr,info); info->click(); QCoreApplication::processEvents();
    auto* dialog=panel->findChild<QDialog*>("decklab-track-info"); ASSERT_NE(nullptr,dialog);
    auto* text=dialog->findChild<QTextBrowser*>("decklab-track-info-text"); ASSERT_NE(nullptr,text);
    EXPECT_TRUE(text->toPlainText().contains(title)); EXPECT_TRUE(text->toPlainText().contains("Full artist"));
    EXPECT_TRUE(text->isReadOnly()); EXPECT_LE(dialog->width(),800); EXPECT_LE(dialog->height(),480);
    const QString output=qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
    if(!output.isEmpty()) EXPECT_TRUE(dialog->grab().save(QDir(output).filePath("track-info-dialog.png")));
    dialog->close(); QCoreApplication::processEvents();
}

TEST_F(NativeDeckLabTest, MoreAndDialogsKeepTouchGeometryAfterShownStylePolish) {
    m_pMixerDeck1->loadFakeTrack(false,128);
    m_pMixerDeck3->loadFakeTrack(false,132); ProcessBuffer(); QTest::qWait(150);
    auto checkButtons=[](QWidget* surface) {
        for(auto* action:surface->findChildren<QPushButton*>()) {
            action->style()->unpolish(action); action->style()->polish(action);
        }
        QCoreApplication::processEvents(); QTest::qWait(20);
        for(auto* action:surface->findChildren<QPushButton*>()) {
            EXPECT_TRUE(action->isVisible()) << action->objectName().toStdString();
            EXPECT_EQ(44,action->height()) << action->objectName().toStdString();
            EXPECT_GE(action->width(),44) << action->objectName().toStdString();
            EXPECT_TRUE(surface->rect().contains(action->mapTo(surface,QPoint(0,0))));
            EXPECT_TRUE(surface->rect().contains(action->mapTo(surface,QPoint(action->width()-1,action->height()-1))));
        }
    };
    auto* more=panel->findChild<QPushButton*>("decklab-1-more"); ASSERT_NE(nullptr,more);
    for(int layer:{0,1}) {
        ControlObject::set(ConfigKey("[DeckLabLeft]","current"),layer); QTest::qWait(150);
        EXPECT_TRUE(more->accessibleName().contains(QStringLiteral("Deck %1 ").arg(layer ? 3 : 1)));
        more->click(); QCoreApplication::processEvents(); QTest::qWait(20);
        auto* menu=panel->findChild<QMenu*>("decklab-1-more-menu"); ASSERT_NE(nullptr,menu); ASSERT_TRUE(menu->isVisible());
        checkButtons(menu);
        auto* info=panel->findChild<QPushButton*>("decklab-1-track-info"); ASSERT_NE(nullptr,info);
        info->click(); QCoreApplication::processEvents(); QTest::qWait(20);
        auto* dialog=panel->findChild<QDialog*>("decklab-track-info"); ASSERT_NE(nullptr,dialog); ASSERT_TRUE(dialog->isVisible());
        EXPECT_TRUE(dialog->windowFlags().testFlag(Qt::FramelessWindowHint));
        auto* heading=dialog->findChild<QLabel*>("decklab-track-info-heading"); ASSERT_NE(nullptr,heading);
        EXPECT_TRUE(heading->text().contains(QStringLiteral("Deck %1").arg(layer ? 3 : 1)));
        checkButtons(dialog);
        EXPECT_TRUE(panel->rect().contains(dialog->mapTo(panel.get(),QPoint(0,0))));
        EXPECT_TRUE(panel->rect().contains(dialog->mapTo(panel.get(),QPoint(dialog->width()-1,dialog->height()-1))));
        auto* close=dialog->findChild<QPushButton*>("decklab-track-info-close"); ASSERT_NE(nullptr,close);
        close->click(); QCoreApplication::processEvents();
    }
    more->click(); QCoreApplication::processEvents();
    auto* grid=panel->findChild<QPushButton*>("decklab-1-grid"); ASSERT_NE(nullptr,grid);
    grid->click(); QCoreApplication::processEvents(); QTest::qWait(20);
    auto* dialog=panel->findChild<QDialog*>("decklab-beatgrid-editor"); ASSERT_NE(nullptr,dialog); ASSERT_TRUE(dialog->isVisible());
    EXPECT_TRUE(dialog->windowFlags().testFlag(Qt::FramelessWindowHint));
    auto* heading=dialog->findChild<QLabel*>("decklab-beatgrid-title"); ASSERT_NE(nullptr,heading);
    EXPECT_TRUE(heading->text().contains("DECK LAB")); EXPECT_TRUE(heading->text().contains("BEATGRID"));
    checkButtons(dialog);
    // Checked styling must retain the same physical target size after polish.
    auto* lock=dialog->findChild<QPushButton*>("decklab-grid-bpmlock"); ASSERT_NE(nullptr,lock);
    lock->click(); QCoreApplication::processEvents(); checkButtons(dialog);
    auto* close=dialog->findChild<QPushButton*>("decklab-grid-close"); ASSERT_NE(nullptr,close); close->click(); QCoreApplication::processEvents();
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel1]","play")));
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel3]","play")));
}

TEST_F(NativeDeckLabTest, FullDeckReadoutOpensTrackInformationAndRejectsStaleGestures) {
    auto track=m_pMixerDeck1->loadFakeTrack(false,128);
    track->setTitle(QString(120,QChar('W'))+QStringLiteral(" full title"));
    m_pMixerDeck3->loadFakeTrack(false,132); ProcessBuffer(); QTest::qWait(150);
    auto* header=panel->findChild<QPushButton*>("decklab-info-hit-1"); ASSERT_NE(nullptr,header);
    ASSERT_GE(header->height(),44); ASSERT_GE(header->width(),44);
    EXPECT_EQ(QStringLiteral("Deck 1 full track information"),header->accessibleName());
    EXPECT_EQ(Qt::StrongFocus,header->focusPolicy());
    auto* accessible=QAccessible::queryAccessibleInterface(header); ASSERT_NE(nullptr,accessible);
    EXPECT_EQ(QAccessible::Button,accessible->role());
    auto* affordance=panel->findChild<QLabel*>("decklab-info-affordance-1"); ASSERT_NE(nullptr,affordance);
    EXPECT_TRUE(affordance->isVisible()); EXPECT_EQ(QStringLiteral("INFO"),affordance->text());
    auto* title=panel->findChild<QLabel*>("decklab-wave-title-1"); ASSERT_NE(nullptr,title);
    EXPECT_TRUE(title->testAttribute(Qt::WA_TransparentForMouseEvents));
    QTest::mouseClick(header,Qt::LeftButton,Qt::NoModifier,title->mapTo(header,title->rect().center()));
    QCoreApplication::processEvents(); QTest::qWait(20);
    auto* dialog=panel->findChild<QDialog*>("decklab-track-info"); ASSERT_NE(nullptr,dialog);
    EXPECT_TRUE(dialog->findChild<QTextBrowser*>("decklab-track-info-text")->toPlainText().contains(track->getTitle()));
    dialog->findChild<QPushButton*>("decklab-track-info-close")->click(); QTest::qWait(20);
    for(auto key:{Qt::Key_Return,Qt::Key_Space}) {
        header->setFocus(); QTest::keyClick(header,key); QTest::qWait(20);
        dialog=panel->findChild<QDialog*>("decklab-track-info"); ASSERT_NE(nullptr,dialog); ASSERT_TRUE(dialog->isVisible());
        dialog->findChild<QPushButton*>("decklab-track-info-close")->click(); QTest::qWait(20);
    }
    QTest::mousePress(header,Qt::LeftButton);
    ControlObject::set(ConfigKey("[DeckLabLeft]","current"),1); QCoreApplication::processEvents();
    QTest::mouseRelease(header,Qt::LeftButton); QTest::qWait(20);
    EXPECT_EQ(nullptr,panel->findChild<QDialog*>("decklab-track-info"));
    ControlObject::set(ConfigKey("[DeckLabLeft]","current"),0); QTest::qWait(150);
    QTest::mousePress(header,Qt::LeftButton);
    m_pMixerDeck1->loadFakeTrack(false,140); ProcessBuffer(); QCoreApplication::processEvents();
    QTest::mouseRelease(header,Qt::LeftButton); QTest::qWait(20);
    EXPECT_EQ(nullptr,panel->findChild<QDialog*>("decklab-track-info"));
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel1]","play")));
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel3]","play")));
}

TEST_F(NativeDeckLabTest, EmptyDeckReadoutReturnsToBrowseWithoutRetargetingChangedTrack) {
    panel->selectPage(1); panel->browsePress(); panel->browsePress(); QCoreApplication::processEvents();
    const auto root=sidebar->rootIndex(); const auto selected=sidebar->currentIndex();
    const int activations=source->activations;
    panel->selectPage(0); QTest::qWait(150);
    auto* header=panel->findChild<QPushButton*>("decklab-info-hit-1"); ASSERT_NE(nullptr,header);
    auto* affordance=panel->findChild<QLabel*>("decklab-info-affordance-1"); ASSERT_NE(nullptr,affordance);
    ASSERT_TRUE(header->isEnabled()); ASSERT_GE(header->height(),44);
    EXPECT_EQ(QStringLiteral("BROWSE"),affordance->text()); EXPECT_TRUE(header->accessibleName().contains("open Browse"));
    EXPECT_LE(affordance->fontMetrics().horizontalAdvance(affordance->text()),affordance->contentsRect().width());
    header->click(); QCoreApplication::processEvents();
    EXPECT_EQ(1,panel->currentPage()); EXPECT_EQ(root,sidebar->rootIndex()); EXPECT_EQ(selected,sidebar->currentIndex());
    EXPECT_EQ(activations,source->activations);
    EXPECT_EQ(nullptr,panel->findChild<QDialog*>("decklab-track-info"));
    panel->selectPage(0); QTest::qWait(150);
    QTest::mousePress(header,Qt::LeftButton);
    m_pMixerDeck1->loadFakeTrack(false,128); ProcessBuffer(); QCoreApplication::processEvents();
    QTest::mouseRelease(header,Qt::LeftButton); QTest::qWait(20);
    EXPECT_EQ(0,panel->currentPage()); EXPECT_EQ(nullptr,panel->findChild<QDialog*>("decklab-track-info"));
    EXPECT_EQ(QStringLiteral("INFO"),affordance->text()); EXPECT_TRUE(header->accessibleName().contains("full track information"));
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel1]","play")));
}

TEST_F(NativeDeckLabTest, CompactDeckShowsActualFractionalLoopLength) {
    m_pMixerDeck1->loadFakeTrack(false,128); ProcessBuffer();
    ControlObject::set(ConfigKey("[Channel1]","beatloop_size"),.125);
    ControlObject::set(ConfigKey("[Channel1]","beatloop_activate"),1);
    ControlObject::set(ConfigKey("[Channel1]","beatloop_activate"),0); ProcessBuffer();
    ASSERT_DOUBLE_EQ(1,ControlObject::get(ConfigKey("[Channel1]","loop_enabled")));
    QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"),"activated",Q_ARG(int,2)); QTest::qWait(150);
    auto* modes=panel->findChild<QLabel*>("decklab-modes-1"); ASSERT_NE(nullptr,modes);
    ASSERT_TRUE(modes->isVisible()); EXPECT_TRUE(modes->text().contains("LOOP 1/8"));
}

TEST_F(NativeDeckLabTest, DeckAndMasterFeedbackFollowRealControlsInBothLayouts) {
    m_pMixerDeck1->loadFakeTrack(false,128); m_pMixerDeck2->loadFakeTrack(false,130);
    m_pMixerDeck3->loadFakeTrack(false,132); fourthDeck->loadFakeTrack(false,134); ProcessBuffer();
    auto* choice=panel->findChild<QComboBox*>("decklab-deck-layout"); ASSERT_NE(nullptr,choice);
    for(const auto& item:{"vu_meter_left","vu_meter_right","peak_indicator"})
        ASSERT_NE(nullptr,ControlObject::getControl(ConfigKey("[Main]",QString::fromLatin1(item))));
    ASSERT_EQ(ControlObject::getControl(ConfigKey("[Main]","vu_meter_left")),ControlObject::getControl(ConfigKey("[Master]","VuMeterL")));
    ASSERT_EQ(ControlObject::getControl(ConfigKey("[Main]","vu_meter_right")),ControlObject::getControl(ConfigKey("[Master]","VuMeterR")));
    ASSERT_EQ(ControlObject::getControl(ConfigKey("[Main]","peak_indicator")),ControlObject::getControl(ConfigKey("[Master]","PeakIndicator")));
    QCoreApplication::processEvents();
    for(int side=0;side<2;++side) {
        auto* meter=panel->findChild<QProgressBar*>(QStringLiteral("decklab-master-meter-%1").arg(side)); ASSERT_NE(nullptr,meter);
        const double level=ControlObject::get(ConfigKey("[Main]",side ? "vu_meter_right" : "vu_meter_left"));
        EXPECT_EQ(int(std::clamp(level,0.0,1.0)*100),meter->value());
    }
    for(int layout:{1,2}) {
        QMetaObject::invokeMethod(choice,"activated",Q_ARG(int,layout)); QCoreApplication::processEvents();
        for(int deck=1;deck<=4;++deck) {
            const QString group=QStringLiteral("[Channel%1]").arg(deck);
            ControlObject::set(ConfigKey(group,"play"),deck%2);
            ControlObject::set(ConfigKey(group,"pfl"),deck%2);
        }
        ProcessBuffer();
        for(int deck=1;deck<=4;++deck) {
            const QString group=QStringLiteral("[Channel%1]").arg(deck);
            ASSERT_NE(nullptr,ControlObject::getControl(ConfigKey(group,"vu_meter")));
            ControlObject::set(ConfigKey(group,"vu_meter"),.1*deck);
            ControlObject::set(ConfigKey(group,"peak_indicator"),deck==1 ? 1 : 0);
        }
        ControlObject::set(ConfigKey("[Main]","vu_meter_left"),.25);
        ControlObject::set(ConfigKey("[Main]","vu_meter_right"),.75);
        ControlObject::set(ConfigKey("[Main]","peak_indicator"),1);
        ASSERT_DOUBLE_EQ(.25,ControlObject::get(ConfigKey("[Main]","vu_meter_left")));
        ASSERT_DOUBLE_EQ(.75,ControlObject::get(ConfigKey("[Main]","vu_meter_right")));
        ASSERT_DOUBLE_EQ(1,ControlObject::get(ConfigKey("[Main]","peak_indicator")));
        ControlObject::set(ConfigKey("[Master]","headMix"),-1);
        ControlObject::set(ConfigKey("[Master]","headSplit"),0); QTest::qWait(150);
        for(int deck=1;deck<=(layout==2 ? 4 : 2);++deck) {
            auto* context=panel->findChild<QLabel*>(QStringLiteral("decklab-wave-context-%1").arg(deck)); ASSERT_NE(nullptr,context);
            ASSERT_TRUE(context->isVisible());
            EXPECT_TRUE(context->text().contains(deck%2 ? "PLAYING" : "PAUSED"));
            auto* pfl=layout==2 ? panel->findChild<QLabel*>(QStringLiteral("decklab-modes-%1").arg(deck)) : context;
            EXPECT_TRUE(pfl->text().contains(layout==2
                            ? (deck%2 ? "HP ON" : "HP OFF")
                            : (deck%2 ? "HP CUE ON" : "HP CUE OFF")));
            if(layout==2) {
                auto* bpm=panel->findChild<QLabel*>(QStringLiteral("decklab-numbers-%1").arg(deck)); ASSERT_NE(nullptr,bpm);
                bool numeric=false; bpm->text().toDouble(&numeric); EXPECT_TRUE(numeric);
                EXPECT_FALSE(bpm->text().contains("BPM"));
            } else {
                auto* source=panel->findChild<QLabel*>(QStringLiteral("decklab-source-%1").arg(deck)); ASSERT_NE(nullptr,source);
                EXPECT_TRUE(source->toolTip().contains("ELAP"));
                EXPECT_FALSE(source->toolTip().contains("PLAYING")); EXPECT_FALSE(source->toolTip().contains("PAUSED"));
                EXPECT_FALSE(source->toolTip().contains('%'));
            }
            auto* meter=panel->findChild<QProgressBar*>(QStringLiteral("decklab-level-%1").arg(deck)); ASSERT_NE(nullptr,meter);
            EXPECT_TRUE(meter->isVisible()); EXPECT_EQ(10*deck,meter->value());
            auto* level=panel->findChild<QLabel*>(QStringLiteral("decklab-level-label-%1").arg(deck)); ASSERT_NE(nullptr,level);
            EXPECT_EQ(deck==1 ? QStringLiteral("CLIP") : QStringLiteral("LVL"),level->text());
        }
        auto* left=panel->findChild<QProgressBar*>("decklab-master-meter-0"); ASSERT_NE(nullptr,left);
        auto* right=panel->findChild<QProgressBar*>("decklab-master-meter-1"); ASSERT_NE(nullptr,right);
        EXPECT_TRUE(left->isVisible()); EXPECT_TRUE(right->isVisible()); EXPECT_EQ(25,left->value()); EXPECT_EQ(75,right->value());
        auto* master=panel->findChild<QLabel*>("decklab-master-level-label"); ASSERT_NE(nullptr,master);
        EXPECT_EQ(QStringLiteral("MASTER CLIP"),master->text());
        auto* headphones=panel->findChild<QLabel*>("decklab-headphone-context"); ASSERT_NE(nullptr,headphones);
        EXPECT_EQ(QStringLiteral("HP CUE"),headphones->text());
        ControlObject::set(ConfigKey("[Master]","headMix"),1); QTest::qWait(150);
        EXPECT_EQ(QStringLiteral("HP MAIN"),headphones->text());
        ControlObject::set(ConfigKey("[Master]","headSplit"),1); QTest::qWait(150);
        EXPECT_EQ(QStringLiteral("HP SPLIT"),headphones->text());
        ControlObject::set(ConfigKey("[Main]","peak_indicator"),0);
        ControlObject::set(ConfigKey("[Channel1]","peak_indicator"),0); QCoreApplication::processEvents();
        EXPECT_EQ(QStringLiteral("MASTER"),master->text());
        EXPECT_EQ(QStringLiteral("LVL"),panel->findChild<QLabel*>("decklab-level-label-1")->text());
        auto* board=panel->findChild<QPushButton*>("decklab-performance-board-fx"); ASSERT_NE(nullptr,board);
        ASSERT_TRUE(board->isVisible()); EXPECT_GE(board->height(),44); board->click(); QCoreApplication::processEvents();
        auto* details=panel->findChild<QDialog*>("decklab-controller-fx-info"); ASSERT_NE(nullptr,details);
        EXPECT_TRUE(details->isVisible()); EXPECT_EQ(0,panel->currentPage());
        details->findChild<QPushButton*>("decklab-controller-fx-close")->click(); QCoreApplication::processEvents();
        auto* rmx=panel->findChild<QPushButton*>("decklab-performance-rmx-fx"); ASSERT_NE(nullptr,rmx);
        ASSERT_TRUE(rmx->isVisible()); EXPECT_GE(rmx->height(),44); rmx->click(); QCoreApplication::processEvents();
        EXPECT_TRUE(panel->findChild<QPushButton*>("decklab-tab-3")->isChecked());
        panel->selectPage(0); QCoreApplication::processEvents();
    }
}

TEST_F(NativeDeckLabTest, ControllerFxDetailsReadAllRealUnitsWithoutControlWrites) {
    m_pEffectsManager->setup();
    const auto manifests=m_pEffectsManager->getBackendManager()->getManifestsForBackend(EffectBackendType::BuiltIn);
    ASSERT_GE(manifests.size(),3);
    std::vector<ConfigKey> keys;
    for(int unit=1;unit<=4;++unit) {
        const QString group=QStringLiteral("[EffectRack1_EffectUnit%1]").arg(unit);
        auto chain=m_pEffectsManager->getEffectChain(group); ASSERT_FALSE(chain.isNull());
        ASSERT_GE(chain->getEffectSlots().size(),3);
        for(const auto& item:{"enabled","mix"}) keys.emplace_back(group,QString::fromLatin1(item));
        ControlObject::set(ConfigKey(group,"enabled"),unit%2);
        ControlObject::set(ConfigKey(group,"mix"),.1*unit);
        for(int deck=1;deck<=4;++deck) {
            const ConfigKey key(group,QStringLiteral("group_[Channel%1]_enable").arg(deck));
            ASSERT_NE(nullptr,ControlObject::getControl(key)); keys.push_back(key);
            ControlObject::set(key,deck==unit ? 1 : 0);
        }
        for(int slot=0;slot<3;++slot) {
            chain->getEffectSlot(slot)->loadEffectWithDefaults(manifests[slot]);
            const QString effect=QStringLiteral("[EffectRack1_EffectUnit%1_Effect%2]").arg(unit).arg(slot+1);
            const ConfigKey enabled(effect,"enabled"); keys.push_back(enabled);
            ControlObject::set(enabled,slot%2 ? 0 : 1);
        }
        ControlObject::set(ConfigKey(group,"enabled"),unit%2);
        ControlObject::set(ConfigKey(group,"mix"),.1*unit);
    }
    QCoreApplication::processEvents();
    std::vector<double> snapshot;
    std::vector<std::unique_ptr<ControlProxy>> observed;
    int writes=0;
    for(const auto& key:keys) {
        snapshot.push_back(ControlObject::get(key));
        observed.emplace_back(std::make_unique<ControlProxy>(key.group,key.item));
        ASSERT_TRUE(observed.back()->connectValueChanged(panel.get(),[&writes](double){++writes;},Qt::DirectConnection));
    }
    auto* board=panel->findChild<QPushButton*>("decklab-performance-board-fx"); ASSERT_NE(nullptr,board);
    board->click(); QCoreApplication::processEvents(); QTest::qWait(20);
    auto* dialog=panel->findChild<QDialog*>("decklab-controller-fx-info"); ASSERT_NE(nullptr,dialog); ASSERT_TRUE(dialog->isVisible());
    EXPECT_TRUE(dialog->windowFlags().testFlag(Qt::FramelessWindowHint)); EXPECT_EQ(0,panel->currentPage());
    auto* text=dialog->findChild<QTextBrowser*>("decklab-controller-fx-text"); ASSERT_NE(nullptr,text); EXPECT_TRUE(text->isReadOnly());
    EXPECT_EQ(Qt::NoTextInteraction,text->textInteractionFlags());
    EXPECT_TRUE(QScroller::hasScroller(text->viewport()));
    EXPECT_NE(static_cast<Qt::GestureType>(0),QScroller::grabbedGesture(text->viewport()));
    const QString detail=text->toPlainText();
    for(int unit=1;unit<=4;++unit) {
        EXPECT_TRUE(detail.contains(QStringLiteral("Unit %1 · %2 · Mix %3%").arg(unit).arg(unit%2 ? "Enabled" : "Bypassed").arg(unit*10)));
        EXPECT_TRUE(detail.contains(QStringLiteral("Routed decks: %1").arg(unit)));
    }
    for(int slot=0;slot<3;++slot)
        EXPECT_TRUE(detail.contains(QStringLiteral("Slot %1: %2 [%3]").arg(slot+1).arg(manifests[slot]->name(),slot%2 ? "OFF" : "ON")));
    auto* ownership=dialog->findChild<QLabel*>("decklab-controller-fx-ownership"); ASSERT_NE(nullptr,ownership);
    EXPECT_TRUE(ownership->text().contains("Read only"));
    EXPECT_TRUE(ownership->text().contains("RMX controls are separate, on the FX screen."));
    auto* refresh=dialog->findChild<QPushButton*>("decklab-controller-fx-refresh"); ASSERT_NE(nullptr,refresh);
    auto* close=dialog->findChild<QPushButton*>("decklab-controller-fx-close"); ASSERT_NE(nullptr,close);
    for(auto* action:{refresh,close}) { action->style()->unpolish(action); action->style()->polish(action); }
    QCoreApplication::processEvents(); EXPECT_EQ(44,refresh->height()); EXPECT_EQ(44,close->height());
    refresh->click(); QCoreApplication::processEvents(); EXPECT_EQ(0,writes);
    for(size_t index=0;index<keys.size();++index) EXPECT_DOUBLE_EQ(snapshot[index],ControlObject::get(keys[index]));
    // An external controller change is reflected by Refresh, without the view
    // writing that control back or changing any neighbouring unit.
    ControlObject::set(ConfigKey("[EffectRack1_EffectUnit4]","mix"),.73); const int externalWrites=writes;
    refresh->click(); QCoreApplication::processEvents(); EXPECT_EQ(externalWrites,writes);
    EXPECT_TRUE(text->toPlainText().contains("Unit 4 · Bypassed · Mix 73%"));
    const QString output=qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
    if(!output.isEmpty()) EXPECT_TRUE(dialog->grab().save(QDir(output).filePath("controller-fx-details.png")));
    close->click(); QCoreApplication::processEvents(); EXPECT_EQ(externalWrites,writes);
    QTest::qWait(150);
    auto* summary=panel->findChild<QLabel*>("decklab-performance-effects"); ASSERT_NE(nullptr,summary);
    EXPECT_TRUE(summary->text().startsWith("FX1"));
    QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"),"activated",Q_ARG(int,2)); QTest::qWait(150);
    EXPECT_EQ(QStringLiteral("FX 1 ON · 2 DRY\nFX 3 ON · 4 DRY"),summary->text());
    for(const auto& line:summary->text().split('\n'))
        EXPECT_LE(summary->fontMetrics().horizontalAdvance(line),summary->contentsRect().width());
    ControlObject::set(ConfigKey("[EffectRack1_EffectUnit4]","enabled"),1); const int enabledWrites=writes;
    QTest::qWait(150); EXPECT_EQ(enabledWrites,writes);
    EXPECT_EQ(QStringLiteral("FX 1 ON · 2 DRY\nFX 3 ON · 4 ON"),summary->text());
    EXPECT_TRUE(summary->toolTip().contains("FX 4; decks 4;"));
    // A real MIDI step below one percent is still wet. Display rounding must
    // never turn it into a false DRY indication on either summary path.
    ControlObject::set(ConfigKey("[EffectRack1_EffectUnit1]","mix"),1.0/127);
    ControlObject::set(ConfigKey("[EffectRack1_EffectUnit3]","mix"),1.0/127);
    ControlObject::set(ConfigKey("[EffectRack1_EffectUnit4]","mix"),.001); const int lowMixWrites=writes;
    QTest::qWait(150); EXPECT_EQ(lowMixWrites,writes);
    EXPECT_EQ(QStringLiteral("FX 1 ON · 2 DRY\nFX 3 ON · 4 ON"),summary->text());
    EXPECT_TRUE(summary->toolTip().contains("<1%"));
    QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"),"activated",Q_ARG(int,1)); QTest::qWait(150);
    EXPECT_TRUE(summary->text().section('\n',0,0).endsWith("<1%"));
    EXPECT_FALSE(summary->text().section('\n',0,0).contains("DRY"));
    QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"),"activated",Q_ARG(int,2)); QTest::qWait(150);
    ControlObject::set(ConfigKey("[EffectRack1_EffectUnit3]","mix"),0); const int dryWrites=writes;
    QTest::qWait(150); EXPECT_EQ(dryWrites,writes);
    EXPECT_EQ(QStringLiteral("FX 1 ON · 2 DRY\nFX 3 DRY · 4 ON"),summary->text());
    ControlObject::set(ConfigKey("[EffectRack1_EffectUnit4]","mix"),0); const int fourthDryWrites=writes;
    QTest::qWait(150); EXPECT_EQ(fourthDryWrites,writes);
    EXPECT_EQ(QStringLiteral("FX 1 ON · 2 DRY\nFX 3 DRY · 4 DRY"),summary->text());
}

TEST_F(NativeDeckLabTest, ControllerFxSummaryIncludesFourthSlotOnlyWithoutControlWrites) {
    m_pEffectsManager->setup();
    const auto manifests=m_pEffectsManager->getBackendManager()->getManifestsForBackend(EffectBackendType::BuiltIn);
    ASSERT_GT(manifests.size(),0);
    std::vector<ConfigKey> keys;
    for(int unit=1;unit<=4;++unit) {
        const QString group=QStringLiteral("[EffectRack1_EffectUnit%1]").arg(unit);
        const auto chain=m_pEffectsManager->getEffectChain(group); ASSERT_FALSE(chain.isNull());
        ASSERT_GE(chain->getEffectSlots().size(),4);
        if(unit==1 || unit==3) chain->getEffectSlots().at(3)->loadEffectWithDefaults(manifests[0]);
        for(int slot=0;slot<chain->getEffectSlots().size();++slot) {
            if(slot!=3 || (unit!=1 && unit!=3))
                chain->getEffectSlots().at(slot)->loadEffectWithDefaults(EffectManifestPointer());
            const ConfigKey key(QStringLiteral("[EffectRack1_EffectUnit%1_Effect%2]").arg(unit).arg(slot+1),"enabled");
            keys.push_back(key);
            ControlObject::set(key,(unit==1 || unit==3) && slot==3 ? 1 : 0);
        }
        for(const auto& item:{"enabled","mix"}) keys.emplace_back(group,QString::fromLatin1(item));
        ControlObject::set(ConfigKey(group,"enabled"),1);
        ControlObject::set(ConfigKey(group,"mix"),.5);
        for(int deck=1;deck<=4;++deck) {
            const ConfigKey key(group,QStringLiteral("group_[Channel%1]_enable").arg(deck)); keys.push_back(key);
            ControlObject::set(key,deck==unit ? 1 : 0);
        }
    }
    QCoreApplication::processEvents();
    int writes=0; std::vector<double> snapshot;
    std::vector<std::unique_ptr<ControlProxy>> observed;
    for(const auto& key:keys) {
        snapshot.push_back(ControlObject::get(key));
        observed.emplace_back(std::make_unique<ControlProxy>(key.group,key.item));
        ASSERT_TRUE(observed.back()->connectValueChanged(panel.get(),[&writes](double){++writes;},Qt::DirectConnection));
    }
    auto* choice=panel->findChild<QComboBox*>("decklab-deck-layout"); ASSERT_NE(nullptr,choice);
    auto* summary=panel->findChild<QLabel*>("decklab-performance-effects"); ASSERT_NE(nullptr,summary);
    QMetaObject::invokeMethod(choice,"activated",Q_ARG(int,2)); QTest::qWait(150);
    EXPECT_EQ(QStringLiteral("FX 1 ON · 2 DRY\nFX 3 ON · 4 DRY"),summary->text());
    for(int unit:{1,3})
        EXPECT_TRUE(summary->toolTip().contains(QStringLiteral("FX %1; decks %1; %2; 50%").arg(unit).arg(manifests[0]->name())));
    QMetaObject::invokeMethod(choice,"activated",Q_ARG(int,1)); QTest::qWait(150);
    EXPECT_TRUE(summary->text().section('\n',0,0).endsWith("50%"));
    EXPECT_FALSE(summary->text().section('\n',0,0).contains("DRY"));
    panel->findChild<QPushButton*>("decklab-performance-board-fx")->click(); QCoreApplication::processEvents();
    auto* dialog=panel->findChild<QDialog*>("decklab-controller-fx-info"); ASSERT_NE(nullptr,dialog);
    auto* text=dialog->findChild<QTextBrowser*>("decklab-controller-fx-text"); ASSERT_NE(nullptr,text);
    EXPECT_TRUE(text->toPlainText().contains(QStringLiteral("Slot 4: %1 [ON]").arg(manifests[0]->name())));
    dialog->findChild<QPushButton*>("decklab-controller-fx-refresh")->click(); QTest::qWait(150);
    EXPECT_EQ(0,writes);
    for(size_t index=0;index<keys.size();++index) EXPECT_DOUBLE_EQ(snapshot[index],ControlObject::get(keys[index]));
}

TEST_F(NativeDeckLabTest, LinkedZoomFollowsTheLastStableExternalChangeWithoutFeedback) {
    int changes=0; std::vector<std::unique_ptr<ControlProxy>> zooms;
    for(int deck=1;deck<=4;++deck) {
        zooms.emplace_back(std::make_unique<ControlProxy>(QStringLiteral("[Channel%1]").arg(deck),"waveform_zoom"));
        ASSERT_TRUE(zooms.back()->connectValueChanged(panel.get(),[&changes](double){++changes;}));
    }
    ControlObject::set(ConfigKey("[Channel3]","waveform_zoom"),7); QTest::qWait(20);
    for(int deck=1;deck<=4;++deck) EXPECT_DOUBLE_EQ(7,ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck),"waveform_zoom")));
    const int before=changes;
    ControlObject::set(ConfigKey("[Channel2]","waveform_zoom"),6);
    ControlObject::set(ConfigKey("[Channel4]","waveform_zoom"),8); QTest::qWait(20);
    for(int deck=1;deck<=4;++deck) EXPECT_DOUBLE_EQ(8,ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck),"waveform_zoom")));
    EXPECT_LE(changes-before,8);
    const int settled=changes; QTest::qWait(50); EXPECT_EQ(settled,changes);
    QTest::qWait(150);
    auto* zoom=panel->findChild<QLabel*>("decklab-performance-zoom"); ASSERT_NE(nullptr,zoom);
    EXPECT_EQ(QStringLiteral("LINKED ZOOM"),zoom->text());
    EXPECT_TRUE(zoom->toolTip().contains("current value 8"));
}

TEST_F(NativeDeckLabTest, MasterLockMissingBeatGridAndSavedLoopIndicatorsUseRealState) {
    auto ungridded=m_pMixerDeck1->loadFakeTrack(false,0);
    m_pMixerDeck2->loadFakeTrack(false,130); ProcessBuffer(); QTest::qWait(150);
    ASSERT_FALSE(ungridded->getBeats());
    auto* context=panel->findChild<QLabel*>("decklab-wave-context-1"); ASSERT_NE(nullptr,context);
    EXPECT_TRUE(context->text().contains(QString::fromUtf8("BEAT —")));
    EXPECT_FALSE(context->text().contains("BAR"));
    m_pMixerDeck1->loadFakeTrack(false,128); ProcessBuffer();
    ControlObject::set(ConfigKey("[Channel1]","sync_leader"),1); ProcessBuffer(); QTest::qWait(150);
    ASSERT_GT(ControlObject::get(ConfigKey("[Channel1]","sync_leader")),0);
    EXPECT_TRUE(context->text().contains("MASTER"));
    EXPECT_TRUE(panel->findChild<QLabel*>("decklab-modes-1")->text().contains("SYNC LOCK"));
    auto* sync=panel->findChild<QPushButton*>("decklab-2-beatsync"); ASSERT_NE(nullptr,sync); sync->click(); QCoreApplication::processEvents();
    EXPECT_TRUE(sync->property("actionFeedback").toBool()); EXPECT_FALSE(sync->isCheckable());
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel2]","sync_enabled")));
    ControlObject::set(ConfigKey("[Channel2]","loop_start_position"),0);
    ControlObject::set(ConfigKey("[Channel2]","loop_end_position"),88200);
    ControlObject::set(ConfigKey("[Channel2]","loop_enabled"),1); ProcessBuffer(); QTest::qWait(150);
    auto* loopPad=panel->findChild<QPushButton*>("decklab-hotcue-2-2"); ASSERT_NE(nullptr,loopPad); loopPad->click();
    QCoreApplication::processEvents(); ProcessBuffer(); QTest::qWait(150);
    EXPECT_EQ(int(mixxx::CueType::Loop),int(ControlObject::get(ConfigKey("[Channel2]","hotcue_2_type"))));
    EXPECT_TRUE(loopPad->text().contains(QChar(0x21bb))); EXPECT_TRUE(loopPad->accessibleName().contains("saved loop"));
    ControlObject::set(ConfigKey("[Channel2]","loop_enabled"),0); ProcessBuffer();
    loopPad->click(); QCoreApplication::processEvents(); ProcessBuffer();
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel2]","play")));
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel2]","loop_enabled")));
}

TEST_F(NativeDeckLabTest, NativeComboPopupRowsHaveTouchHeight) {
    for(auto* combo:panel->findChildren<QComboBox*>()) {
        QStyleOptionViewItem option; option.initFrom(combo->view()); option.font=combo->font();
        for(int row=0;row<combo->count();++row) {
            const QModelIndex index=combo->model()->index(row,combo->modelColumn(),combo->rootModelIndex());
            EXPECT_GE(combo->view()->itemDelegateForIndex(index)->sizeHint(option,index).height(),44)
                    << combo->objectName().toStdString() << " row " << row;
        }
    }
}

TEST_F(NativeDeckLabTest, FourthDeckCuePopupTargetsActualDecksAndCancelsChangedContext) {
    m_pMixerDeck3->loadFakeTrack(false,132); fourthDeck->loadFakeTrack(false,134); ProcessBuffer();
    auto* layout=panel->findChild<QComboBox*>("decklab-deck-layout");
    QMetaObject::invokeMethod(layout,"activated",Q_ARG(int,2)); QTest::qWait(150);
    auto* more3=panel->findChild<QPushButton*>("decklab-card-3-more"); ASSERT_NE(nullptr,more3); more3->click(); QCoreApplication::processEvents();
    auto* menu3=panel->findChild<QMenu*>("decklab-card-3-more-menu"); ASSERT_NE(nullptr,menu3); ASSERT_TRUE(menu3->isVisible());
    EXPECT_GE(menu3->width(),352);
    for(int cue=1;cue<=8;++cue) {
        auto* pad=panel->findChild<QPushButton*>(QStringLiteral("decklab-card-hotcue-3-%1").arg(cue)); ASSERT_NE(nullptr,pad);
        EXPECT_GE(pad->width(),44); EXPECT_EQ(44,pad->height());
        EXPECT_TRUE(menu3->rect().contains(QRect(pad->mapTo(menu3,QPoint()),pad->size())));
    }
    auto* cue3=panel->findChild<QPushButton*>("decklab-card-hotcue-3-1"); ASSERT_NE(nullptr,cue3);
    EXPECT_TRUE(cue3->isVisible()); EXPECT_GE(cue3->width(),44); EXPECT_EQ(44,cue3->height());
    cue3->click(); QCoreApplication::processEvents(); ProcessBuffer();
    EXPECT_GE(ControlObject::get(ConfigKey("[Channel3]","hotcue_1_position")),0);
    EXPECT_LT(ControlObject::get(ConfigKey("[Channel4]","hotcue_1_position")),0);
    ControlObject::set(ConfigKey("[Channel3]","hotcue_1_color"),0); QTest::qWait(150);
    EXPECT_EQ(QStringLiteral("#000000"),cue3->property("cueColor").toString()); menu3->close();
    auto* more4=panel->findChild<QPushButton*>("decklab-card-4-more"); ASSERT_NE(nullptr,more4); more4->click(); QCoreApplication::processEvents();
    auto* menu4=panel->findChild<QMenu*>("decklab-card-4-more-menu"); ASSERT_NE(nullptr,menu4); ASSERT_TRUE(menu4->isVisible());
    EXPECT_GE(menu4->width(),352);
    auto* cue4=panel->findChild<QPushButton*>("decklab-card-hotcue-4-1"); ASSERT_NE(nullptr,cue4);
    QTest::mousePress(cue4,Qt::LeftButton);
    ControlObject::set(ConfigKey("[DeckLabLeft]","current"),1); QCoreApplication::processEvents();
    QTest::mouseRelease(cue4,Qt::LeftButton);
    EXPECT_FALSE(menu4->isVisible()); EXPECT_LT(ControlObject::get(ConfigKey("[Channel4]","hotcue_1_position")),0);
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel3]","play")));
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel4]","play")));
}

TEST_F(NativeDeckLabTest, BeatgridEditorUsesRealGridControlsAndRefusesUnsafeRelease) {
    auto track=m_pMixerDeck1->loadFakeTrack(false,128); track->setTitle("Grid target");
    m_pMixerDeck3->loadFakeTrack(false,132); ProcessBuffer();
    // bpmlock requests toggle regardless of the requested value; establish
    // fixture state through the track and verify the native control follows.
    track->setBpmLocked(false); QTest::qWait(150);
    ASSERT_FALSE(track->isBpmLocked());
    ASSERT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel1]","bpmlock")));
    auto open=[this] {
        panel->findChild<QPushButton*>("decklab-1-more")->click(); QCoreApplication::processEvents();
        panel->findChild<QPushButton*>("decklab-1-grid")->click(); QCoreApplication::processEvents();
        return panel->findChild<QDialog*>("decklab-beatgrid-editor");
    };
    auto* dialog=open(); ASSERT_NE(nullptr,dialog); ASSERT_TRUE(dialog->isVisible());
    EXPECT_TRUE(dialog->findChild<QLabel*>("decklab-beatgrid-title")->text().contains("Grid target"));
    auto button=[dialog](const QString& control) { return dialog->findChild<QPushButton*>(QStringLiteral("decklab-grid-")+control); };
    auto* later=button("beats_translate_later"); auto* align=button("beats_translate_curpos");
    auto* half=button("beats_set_halve"); auto* twice=button("beats_set_double"); auto* lock=button("bpmlock");
    ASSERT_NE(nullptr,later); ASSERT_NE(nullptr,align); ASSERT_NE(nullptr,half); ASSERT_NE(nullptr,twice); ASSERT_NE(nullptr,lock);
    ASSERT_FALSE(lock->isChecked());
    for(auto* action:{later,align,half,twice}) ASSERT_TRUE(action->isEnabled());
    const auto reference=mixxx::audio::FramePos(44100);
    const double oldBeat=track->getBeats()->findClosestBeat(reference).toEngineSamplePos();
    later->click(); QCoreApplication::processEvents(); ProcessBuffer();
    EXPECT_GT(track->getBeats()->findClosestBeat(reference).toEngineSamplePos(),oldBeat);
    ControlObject::set(ConfigKey("[Channel1]","playposition"),.317); ProcessBuffer();
    const auto position=m_pChannel1->getEngineBuffer()->getExactPlayPos();
    ASSERT_TRUE(align->isEnabled()); align->click(); QCoreApplication::processEvents(); ProcessBuffer();
    EXPECT_NEAR(position.toEngineSamplePos(),track->getBeats()->findClosestBeat(position).toEngineSamplePos(),2);
    ASSERT_TRUE(half->isEnabled()); half->click(); QCoreApplication::processEvents(); ProcessBuffer(); EXPECT_NEAR(64,track->getBpm(),.001);
    ASSERT_TRUE(twice->isEnabled()); twice->click(); QCoreApplication::processEvents(); ProcessBuffer(); EXPECT_NEAR(128,track->getBpm(),.001);
    QTest::mousePress(twice,Qt::LeftButton); ControlObject::set(ConfigKey("[Channel1]","sync_enabled"),1); ProcessBuffer();
    ASSERT_GT(ControlObject::get(ConfigKey("[Channel1]","sync_enabled")),0);
    QTest::mouseRelease(twice,Qt::LeftButton); EXPECT_NEAR(128,track->getBpm(),.001);
    EXPECT_GT(ControlObject::get(ConfigKey("[Channel1]","sync_enabled")),0);
    ControlObject::set(ConfigKey("[Channel1]","sync_enabled"),0); ProcessBuffer(); QTest::qWait(150);
    ASSERT_TRUE(twice->isEnabled());
    QTest::mousePress(twice,Qt::LeftButton); ControlObject::set(ConfigKey("[Channel1]","sync_leader"),1); ProcessBuffer();
    ASSERT_GT(ControlObject::get(ConfigKey("[Channel1]","sync_leader")),0);
    QTest::mouseRelease(twice,Qt::LeftButton); EXPECT_NEAR(128,track->getBpm(),.001);
    ControlObject::set(ConfigKey("[Channel1]","sync_enabled"),0); ProcessBuffer(); QTest::qWait(150);
    ASSERT_TRUE(twice->isEnabled());
    QTest::mousePress(twice,Qt::LeftButton); ControlObject::set(ConfigKey("[Channel1]","bpmlock"),1);
    QTest::mouseRelease(twice,Qt::LeftButton); EXPECT_NEAR(128,track->getBpm(),.001); EXPECT_TRUE(lock->isChecked());
    ASSERT_TRUE(lock->isEnabled()); ASSERT_TRUE(track->isBpmLocked());
    lock->click(); QCoreApplication::processEvents();
    ASSERT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel1]","bpmlock")));
    ASSERT_FALSE(track->isBpmLocked()); ASSERT_FALSE(lock->isChecked());
    ASSERT_TRUE(twice->isEnabled());
    QTest::mousePress(twice,Qt::LeftButton); ControlObject::set(ConfigKey("[Channel1]","play"),1);
    QTest::mouseRelease(twice,Qt::LeftButton); EXPECT_NEAR(128,track->getBpm(),.001);
    ControlObject::set(ConfigKey("[Channel1]","play"),0); ProcessBuffer(); QTest::qWait(150);
    QTest::mousePress(twice,Qt::LeftButton); auto replacement=m_pMixerDeck1->loadFakeTrack(false,144); ProcessBuffer();
    QTest::mouseRelease(twice,Qt::LeftButton); EXPECT_NEAR(144,replacement->getBpm(),.001); QTest::qWait(150);
    EXPECT_EQ(nullptr,panel->findChild<QDialog*>("decklab-beatgrid-editor"));
    dialog=open(); ASSERT_NE(nullptr,dialog);
    ControlObject::set(ConfigKey("[DeckLabLeft]","current"),1); QTest::qWait(150);
    EXPECT_EQ(nullptr,panel->findChild<QDialog*>("decklab-beatgrid-editor"));
    ControlObject::set(ConfigKey("[DeckLabLeft]","current"),0); QCoreApplication::processEvents();
    dialog=open(); ASSERT_NE(nullptr,dialog); panel->selectPage(1); QTest::qWait(150);
    EXPECT_EQ(nullptr,panel->findChild<QDialog*>("decklab-beatgrid-editor"));
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel1]","play")));
}

TEST_F(NativeDeckLabTest, SharedTrackGridEditingRequiresEveryAliasedDeckToBeQuiet) {
    auto shared=Track::newTemporary(getTestDir().filePath(QStringLiteral("sine-30.wav")));
    loadTrack(m_pMixerDeck1.get(),shared); loadTrack(m_pMixerDeck2.get(),shared);
    ASSERT_EQ(shared,m_pMixerDeck1->getLoadedTrack()); ASSERT_EQ(shared,m_pMixerDeck2->getLoadedTrack());
    ASSERT_TRUE(shared->trySetBpm(128)); ProcessBuffer();
    shared->setBpmLocked(false); QTest::qWait(150);
    ASSERT_FALSE(shared->isBpmLocked());
    ASSERT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel1]","bpmlock")));
    ASSERT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel2]","bpmlock")));
    panel->findChild<QPushButton*>("decklab-1-more")->click(); QCoreApplication::processEvents();
    panel->findChild<QPushButton*>("decklab-1-grid")->click(); QCoreApplication::processEvents();
    auto* dialog=panel->findChild<QDialog*>("decklab-beatgrid-editor"); ASSERT_NE(nullptr,dialog);
    auto* half=dialog->findChild<QPushButton*>("decklab-grid-beats_set_halve");
    auto* later=dialog->findChild<QPushButton*>("decklab-grid-beats_translate_later");
    auto* lock=dialog->findChild<QPushButton*>("decklab-grid-bpmlock");
    ASSERT_NE(nullptr,half); ASSERT_NE(nullptr,later); ASSERT_NE(nullptr,lock);
    ASSERT_TRUE(half->isEnabled()); ASSERT_TRUE(later->isEnabled()); ASSERT_TRUE(lock->isEnabled());
    ASSERT_FALSE(lock->isChecked());
    for(int busy=0;busy<3;++busy) {
        const QString control=busy==0 ? QStringLiteral("play") : busy==1 ? QStringLiteral("sync_enabled") : QStringLiteral("sync_leader");
        ControlObject::set(ConfigKey("[Channel2]",control),1); ProcessBuffer(); QTest::qWait(150);
        ASSERT_GT(ControlObject::get(ConfigKey("[Channel2]",control)),0);
        EXPECT_FALSE(half->isEnabled()); EXPECT_FALSE(later->isEnabled()); EXPECT_FALSE(lock->isEnabled());
        const auto reference=mixxx::audio::FramePos(44100);
        const double beat=shared->getBeats()->findClosestBeat(reference).toEngineSamplePos();
        // Force enabled to exercise the release guard independently of UI state.
        for(auto* b:{half,later,lock}) { b->setEnabled(true); b->click(); }
        QCoreApplication::processEvents(); ProcessBuffer();
        EXPECT_NEAR(128,shared->getBpm(),.001);
        EXPECT_DOUBLE_EQ(beat,shared->getBeats()->findClosestBeat(reference).toEngineSamplePos());
        EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel1]","bpmlock")));
        EXPECT_GT(ControlObject::get(ConfigKey("[Channel2]",control)),0);
        ControlObject::set(ConfigKey("[Channel2]","play"),0);
        ControlObject::set(ConfigKey("[Channel2]","sync_enabled"),0); ProcessBuffer(); QTest::qWait(150);
    }
    ASSERT_TRUE(half->isEnabled()); half->click(); QCoreApplication::processEvents(); ProcessBuffer();
    EXPECT_NEAR(64,shared->getBpm(),.001);
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel1]","play")));
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel2]","play")));
    dialog->close(); QCoreApplication::processEvents();
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
    EXPECT_DOUBLE_EQ(4, ControlObject::get(ConfigKey("[Channel1]", "beatloop_size")));
    for(double size:{4.0,0.125}) {
        ControlObject::set(ConfigKey("[Channel1]","beatloop_size"),size); ProcessBuffer(); QTest::qWait(150);
        loop->click(); ProcessBuffer(); QTest::qWait(20);
        ASSERT_DOUBLE_EQ(1,ControlObject::get(ConfigKey("[Channel1]","loop_enabled")));
        loop->click(); ProcessBuffer(); QTest::qWait(20);
        EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel1]","loop_enabled")));
        EXPECT_DOUBLE_EQ(size,ControlObject::get(ConfigKey("[Channel1]","beatloop_size")));
        panel->findChild<QPushButton*>("decklab-waveform_zoom_down")->click();
        panel->findChild<QPushButton*>("decklab-waveform_zoom_up")->click(); QTest::qWait(150);
        EXPECT_DOUBLE_EQ(size,ControlObject::get(ConfigKey("[Channel1]","beatloop_size")));
        EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel1]","loop_enabled")));
    }
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
            EXPECT_TRUE(modes->isVisible()); EXPECT_TRUE(pitch->text().contains(index==2 ? "%" : "PITCH"));
            EXPECT_TRUE(modes->text().contains(index==2 ? "SYNC" : "SYNC LOCK"));
            if(index==2) { EXPECT_TRUE(modes->text().contains("LOOP")); EXPECT_TRUE(modes->text().contains("KEY")); }
            else {
                EXPECT_TRUE(panel->findChild<QPushButton*>(QStringLiteral("decklab-%1-quantize").arg(deck))->isChecked());
                EXPECT_TRUE(panel->findChild<QPushButton*>(QStringLiteral("decklab-%1-keylock").arg(deck))->isChecked());
            }
            const auto modeLines=modes->text().split(QLatin1Char('\n'));
            for(const auto& line:modeLines) EXPECT_LE(modes->fontMetrics().horizontalAdvance(line),modes->contentsRect().width());
            EXPECT_LE(modes->fontMetrics().height()*modeLines.size(),modes->contentsRect().height());
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
                    EXPECT_FALSE(QRect(readouts[first]->mapTo(panel.get(),QPoint()),readouts[first]->size()).intersects(
                            QRect(readouts[second]->mapTo(panel.get(),QPoint()),readouts[second]->size())))
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
