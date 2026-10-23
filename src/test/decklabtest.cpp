// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <QAction>
#include <QAccessible>
#include <QAbstractItemDelegate>
#include <QAbstractItemView>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFontDatabase>
#include <QFontInfo>
#include <QDialog>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QPushButton>
#include <QProgressBar>
#include <QScrollArea>
#include <QScrollBar>
#include <QScroller>
#include <QStandardItemModel>
#include <QSet>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QWindow>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "decklab/decklaboverlay.h"
#include "decklab/decklabsetupwizard.h"
#include "decklab/decklabsystem.h"
#include "decklab/decklabsystemui.h"
#include "decklab/decklabui.h"
#include "effects/effectchain.h"
#include "effects/effectslot.h"
#include "library/decklabknob.h"
#include "library/libraryfeature.h"
#include "library/librarycontrol.h"
#include "library/sidebarmodel.h"
#include "library/treeitem.h"
#include "test/signalpathtest.h"
#include "mixer/playermanager.h"
#include "track/beats.h"
#include "track/keyutils.h"
#include "waveform/waveformwidgetfactory.h"
#include "widget/decklabbrowse.h"
#include "widget/decklabdeckwidgets.h"
#include "widget/decklablabfx.h"
#include "widget/decklabtempopresenter.h"
#include "widget/wdecklab.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarysidebar.h"
#include "widget/wsearchlineedit.h"
#include "widget/wwaveformviewer.h"
#include "widget/woverview.h"
#include "widget/controlwidgetconnection.h"
#include "skin/legacy/skincontext.h"
#include "util/fpclassify.h"
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
// Deck state changes are coalesced into one queued refresh; let both the
// control notification and that refresh run.
void settle() {
    for (int pass = 0; pass < 3; ++pass) QCoreApplication::processEvents();
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
    explicit TestSource(UserSettingsPointer config, const QString& icon = QString(),
            const QString& name = QStringLiteral("USB library"), bool sample = true)
            : LibraryFeature(nullptr, config, icon), name(name) {
        auto* root = model.setRootItem(TreeItem::newRoot(this));
        if (!sample) return;
        auto* genres = root->appendChild("Genres"); genres->appendChild("House"); genres->appendChild("Techno");
    }
    QVariant title() override { return name; }
    TreeItemModel* sidebarModel() const override { return const_cast<TreeItemModel*>(&model); }
    void activate() override { ++activations; }
    void activateChild(const QModelIndex&) override { ++activations; }
    int activations = 0;
    TreeItemModel model;
    QString name;
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
        // Screenshots use the bundled typeface, as the appliance does.
        static const bool fontsLoaded = [this] {
            for (const auto& name : {"OpenSans-Regular.ttf", "OpenSans-Semibold.ttf", "OpenSans-Bold.ttf"})
                QFontDatabase::addApplicationFont(QDir(config()->getResourcePath()).filePath(QStringLiteral("fonts/") + QString::fromLatin1(name)));
            return true;
        }();
        Q_UNUSED(fontsLoaded);
        source = std::make_unique<TestSource>(config());
        // A generated stand-in for Mixxx's collection root ("tracks" feature).
        collection = std::make_unique<TestSource>(config(), QStringLiteral("tracks"), QStringLiteral("Collection"));
        // Desktop roots the appliance keeps under "More sources", generated.
        autoDj = std::make_unique<TestSource>(config(), QStringLiteral("autodj"), QStringLiteral("Auto DJ"), false);
        rekordbox = std::make_unique<TestSource>(config(), QStringLiteral("rekordbox"), QStringLiteral("Rekordbox"), false);
        computer = std::make_unique<TestSource>(config(), QStringLiteral("computer"), QStringLiteral("Computer"), false);
        model = std::make_unique<SidebarModel>(); model->addLibraryFeature(source.get());
        model->addLibraryFeature(collection.get());
        model->addLibraryFeature(autoDj.get());
        model->addLibraryFeature(rekordbox.get());
        model->addLibraryFeature(computer.get());
        // USB sticks come from a generated, initially empty mount folder.
        ASSERT_TRUE(QDir().mkpath(media.filePath(QStringLiteral("decklab"))));
        decklab::setUsbStickRootsForTesting({media.filePath(QStringLiteral("decklab"))});
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
        collection.reset(); autoDj.reset(); rekordbox.reset(); computer.reset();
        decklab::setUsbStickRootsForTesting({});
        fourthDeck.reset(); WaveformWidgetFactory::destroy(); BaseSignalPathTest::TearDown();
    }
    std::unique_ptr<TestSource> source;
    std::unique_ptr<TestSource> collection;
    std::unique_ptr<TestSource> autoDj;
    std::unique_ptr<TestSource> rekordbox;
    std::unique_ptr<TestSource> computer;
    QTemporaryDir media;
    std::unique_ptr<SidebarModel> model;
    std::unique_ptr<WDeckLab> panel;
    std::unique_ptr<TestPlayers> players;
    std::unique_ptr<LibraryControl> navigation;
    std::unique_ptr<ControlObject> soundStatus;
    std::unique_ptr<Deck> fourthDeck;
    WLibrarySidebar* sidebar = nullptr;
    WLibrary* library = nullptr;
    DeckLabDeckStrip* strip(int deck) const {
        return panel->findChild<DeckLabDeckStrip*>(QStringLiteral("decklab-info-hit-%1").arg(deck));
    }
    Deck* deckPlayer(int deck) const {
        const std::array<Deck*, 4> all{m_pMixerDeck1.get(), m_pMixerDeck2.get(), m_pMixerDeck3.get(), fourthDeck.get()};
        return all[deck - 1];
    }
    // Generated fixture tracks only: synthetic names, keys and tempos.
    TrackPointer loadGenerated(int deck, double bpm, mixxx::track::io::key::ChromaticKey key = mixxx::track::io::key::INVALID) {
        auto track = deckPlayer(deck)->loadFakeTrack(false, bpm);
        track->setTitle(QStringLiteral("Generated Tone %1").arg(bpm, 0, 'f', 0));
        track->setArtist(QStringLiteral("Deck Lab Fixture"));
        if (key != mixxx::track::io::key::INVALID) {
            track->setKey(key, mixxx::track::io::key::USER);
            ControlObject::set(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), "visual_key"), static_cast<double>(key));
        }
        return track;
    }
    void capture(const QString& name) {
        const QString output = qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
        if (!output.isEmpty()) EXPECT_TRUE(panel->grab().save(QDir(output).filePath(name))) << name.toStdString();
    }
    QListWidget* browseList() const {
        return panel->findChild<QListWidget*>(QStringLiteral("decklab-browse-categories"));
    }
    QStringList listTexts() const {
        QStringList texts;
        if (auto* list = browseList()) for (int row = 0; row < list->count(); ++row) texts << list->item(row)->text();
        return texts;
    }
    bool selectRow(const QString& text) const {
        auto* list = browseList();
        for (int row = 0; list && row < list->count(); ++row) {
            if (list->item(row)->text() == text) { list->setCurrentRow(row); return true; }
        }
        return false;
    }
    QStringList crumbs() const {
        auto* crumb = static_cast<DeckLabBreadcrumb*>(panel->findChild<QWidget*>(QStringLiteral("decklab-breadcrumb")));
        return crumb ? crumb->segments() : QStringList();
    }
    // A generated stick folder at media/decklab/<uuid>.
    QString makeStick(const QString& uuid, bool rekordboxExport) {
        const QString path = media.filePath(QStringLiteral("decklab/") + uuid);
        EXPECT_TRUE(QDir().mkpath(path));
        if (rekordboxExport) {
            EXPECT_TRUE(QDir().mkpath(path + QStringLiteral("/PIONEER/rekordbox")));
            QFile pdb(path + QStringLiteral("/PIONEER/rekordbox/export.pdb"));
            EXPECT_TRUE(pdb.open(QIODevice::WriteOnly));
        }
        return path;
    }
    // Mixxx's file browser tree ("Computer") down to path, as generated items.
    void mirrorFolder(const QString& path) {
        auto root = TreeItem::newRoot(computer.get());
        TreeItem* at = root->appendChild(QStringLiteral("/"), QStringLiteral("/"));
        QString walked = QStringLiteral("/");
        for (const QString& part : QDir::cleanPath(path).split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            walked += part + QLatin1Char('/');
            at = at->appendChild(part, walked);
        }
        computer->model.setRootItem(std::move(root));
    }
    // The rekordbox feature's row for a stick (imported or not yet read).
    void addRekordboxDevice(const QString& label, const QString& path, bool imported) {
        auto root = TreeItem::newRoot(rekordbox.get());
        auto* device = root->appendChild(label, QVariant(QList<QString>{path,
                imported ? QStringLiteral("::isNotRecordboxDevice::") : QStringLiteral("::isRecordboxDevice::")}));
        if (imported) device->appendChild(QStringLiteral("Generated Playlist"), QVariant(QList<QString>{path + QStringLiteral("/1"),
                QStringLiteral("::isNotRecordboxDevice::")}));
        rekordbox->model.setRootItem(std::move(root));
    }
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
                // The playhead is the brand's live red.
                EXPECT_EQ(QColor(0xff,0x3d,0x2e),image.pixelColor(x,22));
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
    // The source list is the first screen.
    EXPECT_TRUE(browseList()->isVisible()); EXPECT_FALSE(sidebar->isVisible());
    EXPECT_EQ(QStringList{QStringLiteral("Sources")}, crumbs());
    EXPECT_EQ(before, source->activations);
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
    // The rail's USB opens Browse now; Settings keeps the storage tool.
    auto* storage = panel->findChild<QPushButton*>("decklab-tool-10"); ASSERT_NE(nullptr, storage);
    storage->click(); const double serial = ControlObject::get(ConfigKey("[DeckLabNative]", "tool_serial"));
    storage->click(); EXPECT_EQ(serial + 1, ControlObject::get(ConfigKey("[DeckLabNative]", "tool_serial")));
    EXPECT_EQ(10, ControlObject::get(ConfigKey("[DeckLabNative]", "tool_request")));
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
    auto* status = panel->findChild<DeckLabChipButton*>("decklab-status"); ASSERT_NE(nullptr, status);
    auto* settingsTab = panel->findChild<QPushButton*>("decklab-tab-4"); ASSERT_NE(nullptr, settingsTab);
    ControlObject::set(ConfigKey("[SoundManager]", "status"), 2); QCoreApplication::processEvents();
    EXPECT_TRUE(status->toolTip().contains("No sound output"));
    // The status never points at a page that doesn't exist; Fix opens Deck Setup.
    EXPECT_FALSE(status->toolTip().contains(QString::fromUtf8("Settings →")));
    ASSERT_FALSE(status->chips().isEmpty());
    EXPECT_EQ(QStringLiteral("NO AUDIO"), status->chips().last().text);
    // Status lamps are white inverted chips, never deck 2's amber.
    EXPECT_EQ(QColor(0xf2, 0xf4, 0xf7), status->chips().last().lit);
    const QRect zone = status->geometry();
    const QRect tab = settingsTab->geometry();
    panel->setAudioOutputConfigured(true); QCoreApplication::processEvents();
    EXPECT_FALSE(status->toolTip().contains("No sound output"));
    // A fixed status zone: lamps lighting or going out never move the tabs.
    EXPECT_FALSE(status->isHidden());
    EXPECT_EQ(zone, status->geometry());
    EXPECT_EQ(tab, settingsTab->geometry());
    ControlObject::set(ConfigKey("[SoundManager]", "status"), 0); QCoreApplication::processEvents();
    EXPECT_TRUE(status->toolTip().contains("No sound output"));
}

TEST_F(NativeDeckLabTest, PagesFit800By480) {
    const QString output = qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
    for (int page = 0; page < 5; ++page) {
        panel->selectPage(page); QCoreApplication::processEvents();
        EXPECT_EQ(QSize(800, 480), panel->size()) << page;
        for (auto* button : panel->findChildren<QPushButton*>()) {
            if (!button->isVisible()) continue;
            // Deck readouts are tap targets only on Decks; other pages show
            // display-only one-line readouts instead.
            EXPECT_FALSE(page != 0 && button->objectName().startsWith("decklab-info-hit-")) << page;
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
    // r4-labfx: ON latches the selected effect (ECHO) on deck 1.
    auto* on=panel->findChild<QPushButton*>("decklab-fx-on"); ASSERT_NE(nullptr,on); ASSERT_TRUE(on->isEnabled());
    on->click(); EXPECT_DOUBLE_EQ(1,ControlObject::get(ConfigKey("[DeckLabEffectUnit1]","mix")));
    EXPECT_GT(ControlObject::get(ConfigKey("[DeckLabEffectUnit1_Effect4]","parameter15")),0);
    QEvent cancel(QEvent::ApplicationDeactivate); QCoreApplication::sendEvent(panel.get(),&cancel);
    // Leaving the application rings Lab FX out (PF-X24): dry at once with
    // the tail ringing (release type 1), then the unit goes off.
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[DeckLabEffectUnit1_Effect4]","parameter1")));
    EXPECT_DOUBLE_EQ(1,ControlObject::get(ConfigKey("[DeckLabEffectUnit1_Effect4]","parameter13")));
    EXPECT_TRUE(QTest::qWaitFor([] { return ControlObject::get(ConfigKey("[DeckLabEffectUnit1]","mix")) == 0; }, 4000));
    EXPECT_DOUBLE_EQ(.63,ControlObject::get(ConfigKey("[EffectRack1_EffectUnit3]","mix")));
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel1]","play")));
}

TEST_F(NativeDeckLabTest, LongMetadataCannotHideTempoOrExpandSurface) {
    auto track=m_pMixerDeck1->loadFakeTrack(false,128); track->setTitle(QString(200,QChar('W'))+"<b>Title</b>");
    track->setArtist(QString(80,QChar('M'))); ProcessBuffer(); QTest::qWait(150);
    panel->selectPage(0); QCoreApplication::processEvents();
    ASSERT_TRUE(strip(1)->isVisible());
    // Painted readouts never parse markup: it is literal title text, elided.
    EXPECT_TRUE(strip(1)->state().title.endsWith("<b>Title</b>"));
    EXPECT_TRUE(strip(1)->state().bpm.contains("128"));
    EXPECT_TRUE(strip(1)->renderer().overflowingParts().isEmpty()) << strip(1)->renderer().overflowingParts().join(',').toStdString();
    // Readout heights follow the on-device text scale (36/24/18/16/14 px).
    EXPECT_EQ(104, strip(1)->height());
    auto* mini=panel->findChild<DeckLabMiniStrip*>("decklab-mini-1"); ASSERT_NE(nullptr,mini);
    EXPECT_FALSE(mini->isVisible());
    panel->selectPage(1); QTest::qWait(150);
    EXPECT_TRUE(mini->isVisible()); EXPECT_EQ(30, mini->height());
    EXPECT_TRUE(mini->state().bpm.contains("128")); EXPECT_EQ(QSize(800,480),panel->size());
    // r4-labfx: Lab FX is one full-height instrument; its footer reads the deck.
    panel->selectPage(3); QTest::qWait(150);
    EXPECT_FALSE(mini->isVisible());
    auto* footer = panel->findChild<QPushButton*>("decklab-fx-deck"); ASSERT_NE(nullptr, footer);
    EXPECT_TRUE(footer->isVisible()); EXPECT_EQ(QSize(800,480),panel->size());
}

TEST_F(NativeDeckLabTest, DeckTextFollowsTheOnDeviceScale) {
    // The skin bundles Barlow and Barlow Semi Condensed (OFL).
    const QStringList families = QFontDatabase::families();
    EXPECT_TRUE(families.contains(QStringLiteral("Barlow")));
    EXPECT_TRUE(families.contains(QStringLiteral("Barlow Semi Condensed")));
    loadGenerated(1, 128, mixxx::track::io::key::A_MINOR); ProcessBuffer(); QTest::qWait(150);
    panel->selectPage(0);
    QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"), "activated", Q_ARG(int, 1));
    QTest::qWait(100); settle();
    ASSERT_EQ(DeckLabStripRenderer::Mode::Full, strip(1)->mode());
    // Hero numerals 36, readouts 24, labels 14 SemiBold; nothing smaller.
    const auto& renderer = strip(1)->renderer();
    EXPECT_EQ(36, renderer.numeralFont(DeckLabStripRenderer::kHero).pixelSize());
    EXPECT_EQ(24, renderer.numeralFont(DeckLabStripRenderer::kReadout).pixelSize());
    EXPECT_EQ(14, renderer.labelFont().pixelSize());
    EXPECT_EQ(QFont::DemiBold, renderer.labelFont().weight());
    EXPECT_EQ(14, renderer.uiFont(9, QFont::Medium).pixelSize());
    EXPECT_EQ(QStringLiteral("Barlow Semi Condensed"), QFontInfo(renderer.numeralFont(DeckLabStripRenderer::kHero)).family());
    // Buttons and tabs: 16 px SemiBold Barlow, 44 px targets.
    for (const char* name : {"decklab-tab-0", "decklab-usb", "decklab-1-quantize", "decklab-1-more", "decklab-hotcue-1-1"}) {
        auto* button = panel->findChild<QPushButton*>(QString::fromLatin1(name)); ASSERT_NE(nullptr, button) << name;
        EXPECT_EQ(16, button->font().pixelSize()) << name;
        EXPECT_EQ(QStringLiteral("Barlow"), QFontInfo(button->font()).family()) << name;
        EXPECT_GE(button->height(), 44) << name;
    }
    // A surface or skin sheet with button min/max heights (as in the real
    // application) must not squash readouts or 44 px targets.
    panel->setStyleSheet(panel->styleSheet() +
            QStringLiteral(" WDeckLab QPushButton {min-height:20px;max-height:30px;padding:0px;}"));
    QTest::qWait(100);
    const char* const targets[] = {"decklab-tab-0", "decklab-tab-4", "decklab-usb", "decklab-performance-board-fx",
            "decklab-1-quantize", "decklab-1-more", "decklab-2-beatsync", "decklab-hotcue-1-1", "decklab-hotcue-2-8"};
    for (const char* name : targets) {
        auto* button = panel->findChild<QPushButton*>(QString::fromLatin1(name)); ASSERT_NE(nullptr, button) << name;
        EXPECT_TRUE(button->isVisible()) << name;
        EXPECT_EQ(44, button->height()) << name;
    }
    EXPECT_EQ(104, strip(1)->height());
    EXPECT_EQ(104, strip(2)->height());
    // Four decks drop the artist and BPM decimals before anything shrinks.
    QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"), "activated", Q_ARG(int, 2));
    QTest::qWait(100);
    ASSERT_EQ(DeckLabStripRenderer::Mode::Compact, strip(1)->mode());
    EXPECT_EQ(138, strip(1)->height());
    for (const char* name : {"decklab-card-1-cue_default", "decklab-card-4-play", "decklab-card-3-more"}) {
        auto* button = panel->findChild<QPushButton*>(QString::fromLatin1(name)); ASSERT_NE(nullptr, button) << name;
        EXPECT_TRUE(button->isVisible()) << name;
        EXPECT_EQ(44, button->height()) << name;
    }
    EXPECT_EQ(36, strip(1)->renderer().numeralFont(DeckLabStripRenderer::kHero).pixelSize());
    EXPECT_TRUE(strip(1)->renderer().overflowingParts().isEmpty());
    EXPECT_EQ(QSize(800, 480), panel->size());
}

TEST_F(NativeDeckLabTest, ControllerLayerChangeRefreshesPlayState) {
    m_pMixerDeck1->loadFakeTrack(false, 128); ProcessBuffer();
    ControlObject::set(ConfigKey("[Channel1]", "play"), 1);
    ControlObject::set(ConfigKey("[Channel3]", "play"), 0);
    settle();
    auto* play = panel->findChild<QPushButton*>("decklab-1-play"); ASSERT_NE(nullptr, play);
    EXPECT_TRUE(play->isChecked());
    // A fixed play/pause glyph; the lamp and the spoken name carry the state.
    EXPECT_TRUE(play->accessibleName().contains("playing"));
    EXPECT_TRUE(strip(1)->isVisible()); EXPECT_TRUE(strip(1)->state().playing);
    ControlObject::set(ConfigKey("[DeckLabLeft]", "current"), 1);
    QCoreApplication::processEvents();
    EXPECT_FALSE(play->isChecked());
    EXPECT_TRUE(play->accessibleName().contains("Deck 3 play/pause, paused"));
    EXPECT_TRUE(strip(3)->isVisible());
    EXPECT_FALSE(strip(1)->isVisible());
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel1]", "play")));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel3]", "play")));
}

TEST_F(NativeDeckLabTest, WaveformLayoutPreferenceRoundTripsBothDeckCountsWithoutEngineWrites) {
    QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"), "activated", Q_ARG(int, 2));
    QTest::qWait(30);
    for (int deck = 1; deck <= 4; ++deck) {
        // Empty compact readouts guide to Browse instead of showing blanks.
        ASSERT_NE(nullptr, strip(deck)); EXPECT_TRUE(strip(deck)->isVisible());
        EXPECT_EQ(DeckLabStripRenderer::Mode::Compact, strip(deck)->mode());
        EXPECT_FALSE(strip(deck)->state().loaded);
        EXPECT_TRUE(strip(deck)->accessibleName().contains("open Browse"));
    }
    QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"), "activated", Q_ARG(int, 1));
    for (int deck = 1; deck <= 4; ++deck) loadGenerated(deck, 126 + 2 * deck);
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
            QList<QRect> areas;
            for (int deck = 1; deck <= count; ++deck) {
                auto* detail = panel->findChild<QWidget*>(QStringLiteral("decklab-detail-%1").arg(deck)); ASSERT_NE(nullptr, detail);
                ASSERT_TRUE(detail->isVisible());
                const QRect area(detail->mapTo(panel.get(), QPoint()), detail->size());
                EXPECT_TRUE(panel->rect().contains(area));
                // Stacked lanes span the full width beside an 18 px deck tag;
                // side by side they halve it.
                EXPECT_NEAR(layout ? 381 : 782, area.width(), 3) << "mode " << mode << " layout " << layout;
                EXPECT_GE(area.height(), mode == 2 ? (layout ? 95 : 45) : (layout ? 180 : 90));
                for (const auto& other : areas) EXPECT_FALSE(other.intersects(area));
                areas << area;
                auto* overview = panel->findChild<QWidget*>(QStringLiteral("decklab-overview-%1").arg(deck));
                ASSERT_NE(nullptr, overview); EXPECT_TRUE(overview->isVisible());
                EXPECT_NEAR(mode == 2 ? 200 : 400, overview->width(), 1); EXPECT_EQ(44, overview->height());
            }
            EXPECT_EQ(0, writes);
            panel->selectPage(1); QCoreApplication::processEvents();
            // Off Decks: waveforms with one-line readouts, no cards.
            EXPECT_FALSE(strip(1)->isVisible());
            EXPECT_TRUE(panel->findChild<QWidget*>("decklab-mini-1")->isVisible());
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
    EXPECT_NEAR(381, restored.findChild<QWidget*>("decklab-detail-1")->width(), 3);
    EXPECT_EQ(0, writes);
}

TEST_F(NativeDeckLabTest, DecksTransportHasTruthfulStatesAndLargerTargets) {
    m_pMixerDeck1->loadFakeTrack(false, 128); ProcessBuffer(); QTest::qWait(150);
    auto* play = panel->findChild<QPushButton*>("decklab-1-play");
    auto* sync = panel->findChild<QPushButton*>("decklab-1-beatsync");
    ASSERT_NE(nullptr, play); ASSERT_NE(nullptr, sync); ASSERT_NE(nullptr, strip(1));
    EXPECT_FALSE(play->isChecked()); EXPECT_TRUE(play->isEnabled());
    EXPECT_TRUE(play->accessibleName().contains("paused"));
    EXPECT_TRUE(strip(1)->state().loaded); EXPECT_FALSE(strip(1)->state().playing);
    EXPECT_TRUE(strip(1)->state().tempo.contains("%"));
    EXPECT_EQ(44, play->height()); EXPECT_EQ(44, sync->height());
    EXPECT_EQ(44, panel->findChild<QPushButton*>("decklab-tab-0")->height());
    // SYNC: a tap matches once (the readout says MATCHED); a hold locks.
    EXPECT_EQ(QStringLiteral("SYNC"), sync->text()); EXPECT_FALSE(sync->isCheckable());
    sync->click(); QCoreApplication::processEvents();
    EXPECT_TRUE(sync->property("actionFeedback").toBool());
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "sync_enabled")));
    settle(); EXPECT_TRUE(strip(1)->state().matched);
    // The 1 s feedback deadline is repainted by a 100ms UI timer. Wait for
    // its actual expiry rather than assuming CI dispatches that timer within
    // the single remaining tick of a fixed sleep.
    EXPECT_TRUE(QTest::qWaitFor([sync] { return !sync->property("actionFeedback").toBool(); }, 2000));
    settle(); EXPECT_FALSE(strip(1)->state().matched);
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "sync_enabled")));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "play")));
    panel->selectPage(1); QCoreApplication::processEvents();
    EXPECT_EQ(44, panel->findChild<QPushButton*>("decklab-tab-0")->height());
}

TEST_F(NativeDeckLabTest, IdleDecksRefreshWithoutStyleOrLayoutWork) {
    loadGenerated(1, 128); loadGenerated(2, 130); ProcessBuffer();
    ControlObject::set(ConfigKey("[Channel1]", "quantize"), 0);
    QTest::qWait(400);
    // Counts the expensive side effects of presentation writes: repolish,
    // relayout and polish. A refresh with unchanged state must cause none.
    struct Counter final : QObject {
        int style = 0, layout = 0, polish = 0;
        bool eventFilter(QObject*, QEvent* event) override {
            if (event->type() == QEvent::StyleChange) ++style;
            else if (event->type() == QEvent::LayoutRequest) ++layout;
            else if (event->type() == QEvent::Polish || event->type() == QEvent::PolishRequest) ++polish;
            return false;
        }
    } counter;
    QList<QWidget*> watched{panel.get()};
    watched.append(panel->findChildren<QWidget*>());
    for (auto* widget : watched) widget->installEventFilter(&counter);
    QTest::qWait(650); // six display ticks and more than a dozen meter ticks
    EXPECT_EQ(0, counter.style); EXPECT_EQ(0, counter.layout); EXPECT_EQ(0, counter.polish);
    // Control traffic that marks the deck dirty without changing anything shown
    // (an empty cue's color) runs the refresh but writes nothing.
    ASSERT_LT(ControlObject::get(ConfigKey("[Channel1]", "hotcue_8_position")), 0);
    for (int color : {0x123456, 0x654321}) {
        ControlObject::set(ConfigKey("[Channel1]", "hotcue_8_color"), color); settle();
    }
    QTest::qWait(150);
    EXPECT_EQ(0, counter.style); EXPECT_EQ(0, counter.layout);
    // A real change still reaches the screen through the coalesced refresh.
    auto* quantize = panel->findChild<QPushButton*>("decklab-1-quantize"); ASSERT_NE(nullptr, quantize);
    ControlObject::set(ConfigKey("[Channel1]", "quantize"), 1); settle();
    EXPECT_TRUE(quantize->isChecked());
    // A playing deck advances its clock, beat meter and tempo by repainting
    // only the changed parts of its painted readout: still no relayout or
    // repolish anywhere on the surface.
    ControlObject::set(ConfigKey("[Channel1]", "play"), 1); settle(); QTest::qWait(150);
    counter.style = counter.layout = counter.polish = 0;
    QStringList clocks; QList<int> phases;
    for (int step = 0; step < 12; ++step) {
        for (int n = 0; n < 8; ++n) ProcessBuffer();
        QTest::qWait(110);
        clocks << strip(1)->state().remaining; phases << strip(1)->state().phase;
    }
    clocks.removeDuplicates();
    EXPECT_GE(clocks.size(), 2) << clocks.join(',').toStdString();
    EXPECT_GE(QSet<int>(phases.begin(), phases.end()).size(), 2);
    EXPECT_EQ(0, counter.style); EXPECT_EQ(0, counter.layout); EXPECT_EQ(0, counter.polish);
    ControlObject::set(ConfigKey("[Channel1]", "play"), 0); ProcessBuffer();
    for (auto* widget : watched) widget->removeEventFilter(&counter);
}

TEST_F(NativeDeckLabTest, LayerChangeReleasesOnlyTheReplacedTouchCue) {
    m_pMixerDeck1->loadFakeTrack(false, 128);
    m_pMixerDeck2->loadFakeTrack(false, 130);
    m_pMixerDeck3->loadFakeTrack(false, 132); fourthDeck->loadFakeTrack(false, 134); ProcessBuffer(); settle();
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
    m_pMixerDeck3->loadFakeTrack(false, 132); fourthDeck->loadFakeTrack(false, 134); ProcessBuffer(); settle();
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
    loadGenerated(1, 300, mixxx::track::io::key::E_MAJOR); ProcessBuffer();
    panel->setAudioOutputConfigured(true);
    ControlObject::set(ConfigKey("[SoundManager]", "status"), 2);
    ControlObject::set(ConfigKey("[DeckLabNative]", "tools_ready"), 1);
    ControlObject::set(ConfigKey("[Channel1]", "duration"), 7200);
    QTest::qWait(150);
    QComboBox* format = nullptr;
    for (auto* combo : panel->findChildren<QComboBox*>())
        if (combo->itemText(0).contains("8A")) format = combo;
    ASSERT_NE(nullptr, format);
    for (int notation = 0; notation < 3; ++notation) {
        QMetaObject::invokeMethod(format, "activated", Q_ARG(int, notation)); settle();
        EXPECT_FALSE(strip(1)->state().key.isEmpty());
        EXPECT_TRUE(strip(1)->renderer().overflowingParts().isEmpty())
                << notation << " " << strip(1)->renderer().overflowingParts().join(',').toStdString();
    }
    QMetaObject::invokeMethod(format, "activated", Q_ARG(int, 0)); settle();
    EXPECT_EQ(QStringLiteral("12B"), strip(1)->state().key);
    EXPECT_TRUE(strip(1)->state().keyColor.isValid());
    EXPECT_TRUE(strip(1)->state().remaining.contains("120:00"));
    // Deck identity: the readout badge and the waveform edge share the deck color.
    const QImage readout = strip(1)->grab().toImage();
    EXPECT_EQ(deckLabDeckColor(1), readout.pixelColor(20, 24));
    for (int deck = 1; deck <= 2; ++deck) {
        auto* edge = panel->findChild<QWidget*>(QStringLiteral("decklab-lane-edge-%1").arg(deck)); ASSERT_NE(nullptr, edge);
        EXPECT_EQ(deckLabDeckColor(deck), edge->grab().toImage().pixelColor(1, edge->height() / 2));
    }
    EXPECT_EQ(QSize(800, 480), panel->size());
    EXPECT_EQ(436, panel->waveformContainer()->height());
    // Full-width waveforms: no side columns take their space.
    for (int deck = 1; deck <= 2; ++deck) {
        auto* detail = panel->findChild<QWidget*>(QStringLiteral("decklab-detail-%1").arg(deck));
        EXPECT_EQ(QSize(782, 95), detail->size());
        EXPECT_EQ(QPoint(18, 44 + (deck - 1) * 97), detail->mapTo(panel.get(), QPoint()));
        auto* overview = panel->findChild<QWidget*>(QStringLiteral("decklab-overview-%1").arg(deck));
        EXPECT_NEAR(400, overview->width(), 1); EXPECT_EQ(44, overview->height());
        EXPECT_NEAR((deck - 1) * 400, overview->mapTo(panel.get(), QPoint()).x(), 2);
        EXPECT_EQ(340, overview->mapTo(panel.get(), QPoint()).y());
        EXPECT_EQ(104, strip(deck)->height()); EXPECT_EQ(236, strip(deck)->mapTo(panel.get(), QPoint()).y());
    }
    auto* utilities = panel->findChild<QPushButton*>("decklab-1-more")->parentWidget();
    EXPECT_NEAR(400, utilities->width(), 1); EXPECT_EQ(48, utilities->height());
    EXPECT_EQ(432, utilities->mapTo(panel.get(), QPoint()).y());
}

TEST_F(NativeDeckLabTest, TransportStateAndTargetRemainTruthfulInPlayingFixture) {
    loadGenerated(1, 128, mixxx::track::io::key::A_MINOR);
    loadGenerated(2, 130, mixxx::track::io::key::C_MAJOR); ProcessBuffer(); QTest::qWait(150);
    auto* play = panel->findChild<QPushButton*>("decklab-1-play"); ASSERT_NE(nullptr, play);
    play->click(); QTest::qWait(150);
    EXPECT_TRUE(play->accessibleName().contains("playing")); EXPECT_TRUE(play->isChecked());
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel1]", "play")));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel2]", "play")));
    EXPECT_TRUE(strip(1)->state().playing);
    EXPECT_FALSE(strip(2)->state().playing); EXPECT_TRUE(strip(2)->state().loaded);
    capture(QStringLiteral("decks-fixture-playing.png"));
    play->click(); QTest::qWait(150);
    EXPECT_TRUE(play->accessibleName().contains("paused")); EXPECT_FALSE(play->isChecked());
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "play")));
}

TEST_F(NativeDeckLabTest, UpperLayerTransportTargetsLoadedDecksThreeAndFour) {
    m_pMixerDeck1->loadFakeTrack(false, 128); m_pMixerDeck2->loadFakeTrack(false, 130);
    m_pMixerDeck3->loadFakeTrack(false, 132); fourthDeck->loadFakeTrack(false, 134);
    ProcessBuffer(); settle();
    ControlObject::set(ConfigKey("[DeckLabLeft]", "current"), 1);
    ControlObject::set(ConfigKey("[DeckLabRight]", "current"), 1); QCoreApplication::processEvents();
    panel->findChild<QPushButton*>("decklab-1-play")->click();
    panel->findChild<QPushButton*>("decklab-2-play")->click(); QTest::qWait(150);
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "play")));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel2]", "play")));
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel3]", "play")));
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel4]", "play")));
    EXPECT_TRUE(strip(3)->isVisible()); EXPECT_TRUE(strip(4)->isVisible());
    EXPECT_TRUE(panel->findChild<QWidget*>("decklab-detail-3")->isVisible());
    EXPECT_TRUE(panel->findChild<QWidget*>("decklab-detail-4")->isVisible());
    EXPECT_FALSE(strip(1)->isVisible()); EXPECT_FALSE(strip(2)->isVisible());
    EXPECT_FALSE(panel->findChild<QWidget*>("decklab-detail-1")->isVisible());
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
    settle(); panel->findChild<QPushButton*>("decklab-1-beatsync")->click();
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
    for (int deck = 1; deck <= 3; ++deck) loadGenerated(deck, 120 + 4 * deck, mixxx::track::io::key::ChromaticKey(deck * 3));
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
    // Four-deck Play acts on touch-down; the release adds nothing.
    QTest::mousePress(play, Qt::LeftButton);
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel4]", "play")));
    QTest::mouseRelease(play, Qt::LeftButton); QTest::qWait(150);
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel4]", "play")));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel2]", "play")));
    EXPECT_TRUE(play->isChecked()); EXPECT_TRUE(play->accessibleName().contains("Deck 4 play/pause, playing"));
    // Fixed labels in the four-deck card.
    EXPECT_EQ(QStringLiteral("CUE"), panel->findChild<QPushButton*>("decklab-card-4-cue_default")->text());
    EXPECT_EQ(QStringLiteral("SYNC"), panel->findChild<QPushButton*>("decklab-card-4-beatsync")->text());
    EXPECT_EQ(QStringLiteral("MORE"), panel->findChild<QPushButton*>("decklab-card-4-more")->text());
    capture(QStringLiteral("decks-fixture-four.png"));
    int previousTop = -1;
    for (int deck = 1; deck <= 4; ++deck) {
        for(const auto& suffix:{"cue_default","play","beatsync","more"}) {
            auto* action=panel->findChild<QPushButton*>(QStringLiteral("decklab-card-%1-%2").arg(deck).arg(QString::fromLatin1(suffix)));
            ASSERT_NE(nullptr,action); EXPECT_TRUE(action->isVisible());
            EXPECT_EQ(44,action->height()); EXPECT_GE(action->width(),44);
            EXPECT_TRUE(panel->rect().contains(QRect(action->mapTo(panel.get(),QPoint()),action->size())));
        }
        auto* cue = panel->findChild<QPushButton*>(QStringLiteral("decklab-card-%1-cue_default").arg(deck));
        EXPECT_EQ(436, cue->mapTo(panel.get(),QPoint()).y());
        // Four full-width waveforms, stacked in deck order.
        auto* detail = panel->findChild<QWidget*>(QStringLiteral("decklab-detail-%1").arg(deck)); ASSERT_NE(nullptr,detail);
        EXPECT_EQ(782, detail->width()); EXPECT_NEAR(50, detail->height(), 1);
        const QPoint at = detail->mapTo(panel.get(),QPoint());
        EXPECT_EQ(18, at.x()); EXPECT_GT(at.y(), previousTop); previousTop = at.y();
        auto* overview = panel->findChild<QWidget*>(QStringLiteral("decklab-overview-%1").arg(deck));
        ASSERT_NE(nullptr, overview); EXPECT_TRUE(overview->isVisible()); EXPECT_EQ(44, overview->height());
        EXPECT_NEAR(200,overview->width(),1);
        EXPECT_NEAR((deck-1)*200,overview->mapTo(panel.get(),QPoint()).x(),2);
        EXPECT_EQ(388,overview->mapTo(panel.get(),QPoint()).y());
        EXPECT_FALSE(overview->acceptDrops());
        EXPECT_EQ(DeckLabStripRenderer::Mode::Compact, strip(deck)->mode());
        EXPECT_EQ(138, strip(deck)->height());
        EXPECT_TRUE(strip(deck)->renderer().overflowingParts().isEmpty());
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
        // Adjacent touch targets keep at least a 4 px gap.
        auto* sync=panel->findChild<QPushButton*>(QStringLiteral("decklab-%1-beatsync").arg(side));
        auto* keylock=panel->findChild<QPushButton*>(QStringLiteral("decklab-%1-keylock").arg(side));
        EXPECT_GE(sync->mapTo(panel.get(),QPoint()).x()-(keylock->mapTo(panel.get(),QPoint()).x()+keylock->width()),4);
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
    EXPECT_EQ(QStringLiteral("A"),a->text()); EXPECT_TRUE(a->isEnabled());
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
    // Every pad stays live while the deck plays: empty pads set a cue there.
    EXPECT_TRUE(a->isEnabled()); EXPECT_TRUE(b->isEnabled());
    a->click(); QCoreApplication::processEvents(); ProcessBuffer();
    EXPECT_DOUBLE_EQ(1,ControlObject::get(ConfigKey("[Channel1]","play")));
    EXPECT_LT(m_pChannel1->getEngineBuffer()->getExactPlayPos().toEngineSamplePos(),cuePosition+44100);
    ControlObject::set(ConfigKey("[Channel1]","play"),0); ProcessBuffer(); QTest::qWait(150);
    // Touch-down acts on the deck the pads show at that moment; a layer
    // change before the release cannot retarget or repeat it.
    ControlObject::set(ConfigKey("[Channel1]","playposition"),.75); ProcessBuffer();
    QTest::mousePress(a,Qt::LeftButton); ProcessBuffer();
    EXPECT_NEAR(cuePosition,m_pChannel1->getEngineBuffer()->getExactPlayPos().toEngineSamplePos(),4);
    ControlObject::set(ConfigKey("[DeckLabLeft]","current"),1); QCoreApplication::processEvents();
    QTest::mouseRelease(a,Qt::LeftButton);
    EXPECT_LT(ControlObject::get(ConfigKey("[Channel3]","hotcue_1_position")),0);
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel3]","play")));
    ControlObject::set(ConfigKey("[DeckLabLeft]","current"),0); QCoreApplication::processEvents();
    // A new track: its pads start empty and a touch sets on the new track.
    auto replacement=m_pMixerDeck1->loadFakeTrack(false,140); ProcessBuffer(); QCoreApplication::processEvents();
    ASSERT_NE(first,replacement); QTest::qWait(150);
    EXPECT_LT(ControlObject::get(ConfigKey("[Channel1]","hotcue_1_position")),0);
    EXPECT_FALSE(a->property("cueSet").toBool());
    // A cancelled touch after touch-down: the cue was already set (that was
    // the action); the cancel and the late release add nothing.
    QTest::mousePress(b,Qt::LeftButton); ProcessBuffer();
    const double setOnPress=ControlObject::get(ConfigKey("[Channel1]","hotcue_2_position"));
    EXPECT_GE(setOnPress,0);
    QEvent cancel(QEvent::TouchCancel); QCoreApplication::sendEvent(panel.get(),&cancel);
    QTest::mouseRelease(b,Qt::LeftButton); ProcessBuffer();
    EXPECT_DOUBLE_EQ(setOnPress,ControlObject::get(ConfigKey("[Channel1]","hotcue_2_position")));
    EXPECT_LT(ControlObject::get(ConfigKey("[Channel1]","hotcue_1_position")),0);
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
    EXPECT_FALSE(a->property("cueSet").toBool()); EXPECT_EQ(QStringLiteral("A"),a->text());
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
        EXPECT_TRUE(heading->text().contains(QStringLiteral("DECK %1").arg(layer ? 3 : 1)));
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
    EXPECT_TRUE(heading->text().contains("DECK 1")); EXPECT_TRUE(heading->text().contains("GRID"));
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
    // The whole readout is one target; nothing painted inside takes the touch.
    for(auto* child:header->findChildren<QWidget*>()) EXPECT_TRUE(child->testAttribute(Qt::WA_TransparentForMouseEvents));
    QTest::mouseClick(header,Qt::LeftButton,Qt::NoModifier,header->rect().center());
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
    // While the deck plays, a stray tap on its readout never covers the
    // decks; Track info stays one tap away in MORE.
    QTest::qWait(150);
    ControlObject::set(ConfigKey("[Channel1]","play"),1); ProcessBuffer(); settle();
    EXPECT_TRUE(header->accessibleDescription().contains("MORE"));
    QTest::mouseClick(header,Qt::LeftButton,Qt::NoModifier,header->rect().center()); QTest::qWait(20);
    header->setFocus(); QTest::keyClick(header,Qt::Key_Return); QTest::qWait(20);
    EXPECT_EQ(nullptr,panel->findChild<QDialog*>("decklab-track-info"));
    EXPECT_DOUBLE_EQ(1,ControlObject::get(ConfigKey("[Channel1]","play")));
    panel->findChild<QPushButton*>("decklab-1-more")->click(); QCoreApplication::processEvents();
    panel->findChild<QPushButton*>("decklab-1-track-info")->click(); QCoreApplication::processEvents(); QTest::qWait(20);
    dialog=panel->findChild<QDialog*>("decklab-track-info"); ASSERT_NE(nullptr,dialog); ASSERT_TRUE(dialog->isVisible());
    dialog->findChild<QPushButton*>("decklab-track-info-close")->click(); QTest::qWait(20);
    ControlObject::set(ConfigKey("[Channel1]","play"),0); ProcessBuffer(); settle();
    // Four-deck cards: the same guard, and Track info in each card's MORE.
    QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"),"activated",Q_ARG(int,2)); QTest::qWait(150);
    ControlObject::set(ConfigKey("[Channel3]","play"),1); ProcessBuffer(); settle();
    QTest::mouseClick(strip(3),Qt::LeftButton,Qt::NoModifier,strip(3)->rect().center()); QTest::qWait(20);
    EXPECT_EQ(nullptr,panel->findChild<QDialog*>("decklab-track-info"));
    panel->findChild<QPushButton*>("decklab-card-3-more")->click(); QCoreApplication::processEvents();
    auto* cardInfo=panel->findChild<QPushButton*>("decklab-card-3-track-info"); ASSERT_NE(nullptr,cardInfo);
    EXPECT_TRUE(cardInfo->isVisible()); EXPECT_EQ(44,cardInfo->height());
    cardInfo->click(); QCoreApplication::processEvents(); QTest::qWait(20);
    dialog=panel->findChild<QDialog*>("decklab-track-info"); ASSERT_NE(nullptr,dialog);
    EXPECT_TRUE(dialog->findChild<QLabel*>("decklab-track-info-heading")->text().contains("DECK 3"));
    dialog->findChild<QPushButton*>("decklab-track-info-close")->click(); QTest::qWait(20);
    ControlObject::set(ConfigKey("[Channel3]","play"),0); ProcessBuffer(); settle();
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel1]","play")));
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel3]","play")));
}

TEST_F(NativeDeckLabTest, EmptyDeckReadoutReturnsToBrowseWithoutRetargetingChangedTrack) {
    panel->selectPage(1); panel->browsePress(); panel->browsePress(); QCoreApplication::processEvents();
    const auto root=sidebar->rootIndex(); const auto selected=sidebar->currentIndex();
    const int activations=source->activations;
    panel->selectPage(0); QTest::qWait(150);
    auto* header=panel->findChild<QPushButton*>("decklab-info-hit-1"); ASSERT_NE(nullptr,header);
    ASSERT_TRUE(header->isEnabled()); ASSERT_GE(header->height(),44);
    EXPECT_FALSE(strip(1)->state().loaded); EXPECT_TRUE(header->accessibleName().contains("open Browse"));
    header->click(); QCoreApplication::processEvents();
    EXPECT_EQ(1,panel->currentPage()); EXPECT_EQ(root,sidebar->rootIndex()); EXPECT_EQ(selected,sidebar->currentIndex());
    EXPECT_EQ(activations,source->activations);
    EXPECT_EQ(nullptr,panel->findChild<QDialog*>("decklab-track-info"));
    panel->selectPage(0); QTest::qWait(150);
    QTest::mousePress(header,Qt::LeftButton);
    m_pMixerDeck1->loadFakeTrack(false,128); ProcessBuffer(); QCoreApplication::processEvents();
    QTest::mouseRelease(header,Qt::LeftButton); QTest::qWait(20);
    EXPECT_EQ(0,panel->currentPage()); EXPECT_EQ(nullptr,panel->findChild<QDialog*>("decklab-track-info"));
    EXPECT_TRUE(strip(1)->state().loaded); EXPECT_TRUE(header->accessibleName().contains("full track information"));
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel1]","play")));
}

TEST_F(NativeDeckLabTest, CompactDeckShowsActualFractionalLoopLength) {
    m_pMixerDeck1->loadFakeTrack(false,128); ProcessBuffer();
    ControlObject::set(ConfigKey("[Channel1]","beatloop_size"),.125);
    ControlObject::set(ConfigKey("[Channel1]","beatloop_activate"),1);
    ControlObject::set(ConfigKey("[Channel1]","beatloop_activate"),0); ProcessBuffer();
    ASSERT_DOUBLE_EQ(1,ControlObject::get(ConfigKey("[Channel1]","loop_enabled")));
    QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"),"activated",Q_ARG(int,2)); QTest::qWait(150);
    ASSERT_TRUE(strip(1)->isVisible());
    EXPECT_EQ(DeckLabStripRenderer::Mode::Compact, strip(1)->mode());
    EXPECT_TRUE(strip(1)->modesText().contains("LOOP 1/8"));
    EXPECT_EQ(QStringLiteral("1/8"), strip(1)->state().loopSize);
}

TEST_F(NativeDeckLabTest, DeckAndMasterFeedbackFollowRealControlsInBothLayouts) {
    for(int deck=1;deck<=4;++deck) loadGenerated(deck,126+2*deck);
    ProcessBuffer();
    auto* choice=panel->findChild<QComboBox*>("decklab-deck-layout"); ASSERT_NE(nullptr,choice);
    for(const auto& item:{"vu_meter_left","vu_meter_right","peak_indicator"})
        ASSERT_NE(nullptr,ControlObject::getControl(ConfigKey("[Main]",QString::fromLatin1(item))));
    ASSERT_EQ(ControlObject::getControl(ConfigKey("[Main]","vu_meter_left")),ControlObject::getControl(ConfigKey("[Master]","VuMeterL")));
    ASSERT_EQ(ControlObject::getControl(ConfigKey("[Main]","vu_meter_right")),ControlObject::getControl(ConfigKey("[Master]","VuMeterR")));
    ASSERT_EQ(ControlObject::getControl(ConfigKey("[Main]","peak_indicator")),ControlObject::getControl(ConfigKey("[Master]","PeakIndicator")));
    QTest::qWait(150);
    // The main output meter lives in the tab rail on every page.
    auto* mainMeter=panel->findChild<DeckLabLevelMeter*>("decklab-master-meter"); ASSERT_NE(nullptr,mainMeter);
    for(int side=0;side<2;++side) {
        const double level=ControlObject::get(ConfigKey("[Main]",side ? "vu_meter_right" : "vu_meter_left"));
        EXPECT_DOUBLE_EQ(std::clamp(level,0.0,1.0),mainMeter->level(side));
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
        QTest::qWait(150);
        int shown=0;
        for(int deck=1;deck<=4;++deck) {
            auto* readout=strip(deck); ASSERT_NE(nullptr,readout);
            // Lit states, not words: play lamp, headphone cue lamp.
            EXPECT_EQ(deck%2==1,readout->state().playing);
            EXPECT_EQ(deck%2==1,readout->state().pfl);
            EXPECT_EQ(deck%2==1,readout->modesText().contains("HP"));
            if(!readout->isVisible()) continue;
            ++shown;
            EXPECT_EQ(layout==2 ? DeckLabStripRenderer::Mode::Compact : DeckLabStripRenderer::Mode::Full,readout->mode());
            EXPECT_TRUE(readout->renderer().overflowingParts().isEmpty());
            auto* meter=panel->findChild<DeckLabLevelMeter*>(QStringLiteral("decklab-level-%1").arg(deck)); ASSERT_NE(nullptr,meter);
            EXPECT_TRUE(meter->isVisible()); EXPECT_NEAR(.1*deck,meter->level(0),1e-9);
            EXPECT_EQ(deck==1,meter->clipping());
        }
        EXPECT_EQ(layout==2 ? 4 : 2,shown);
        EXPECT_TRUE(mainMeter->isVisible()); EXPECT_DOUBLE_EQ(.25,mainMeter->level(0)); EXPECT_DOUBLE_EQ(.75,mainMeter->level(1));
        EXPECT_TRUE(mainMeter->clipping());
        EXPECT_GT(mainMeter->litSegments(1),mainMeter->litSegments(0));
        ControlObject::set(ConfigKey("[Main]","peak_indicator"),0);
        ControlObject::set(ConfigKey("[Channel1]","peak_indicator"),0); QTest::qWait(150);
        EXPECT_FALSE(mainMeter->clipping());
        EXPECT_FALSE(panel->findChild<DeckLabLevelMeter*>("decklab-level-1")->clipping());
        // Controller FX lamps open their read-only details without leaving Decks.
        auto* board=panel->findChild<QPushButton*>("decklab-performance-board-fx"); ASSERT_NE(nullptr,board);
        ASSERT_TRUE(board->isVisible()); EXPECT_EQ(44,board->height()); board->click(); QCoreApplication::processEvents();
        auto* details=panel->findChild<QDialog*>("decklab-controller-fx-info"); ASSERT_NE(nullptr,details);
        EXPECT_TRUE(details->isVisible()); EXPECT_EQ(0,panel->currentPage());
        details->findChild<QPushButton*>("decklab-controller-fx-close")->click(); QCoreApplication::processEvents();
        panel->findChild<QPushButton*>("decklab-tab-3")->click(); QCoreApplication::processEvents();
        EXPECT_TRUE(panel->findChild<QPushButton*>("decklab-tab-3")->isChecked());
        panel->selectPage(0); QCoreApplication::processEvents();
    }
    for(int deck=1;deck<=4;++deck) ControlObject::set(ConfigKey(QStringLiteral("[Channel%1]").arg(deck),"play"),0);
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
    auto* board=panel->findChild<DeckLabChipButton*>("decklab-performance-board-fx"); ASSERT_NE(nullptr,board);
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
    EXPECT_TRUE(ownership->text().contains("Lab FX controls are separate, on the Lab FX screen."));
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
    // Rail lamps: one per controller unit, lit while it is audibly wet.
    const auto lamps=[board] {
        QString lit; for(const auto& chip:board->chips()) lit+=chip.on ? QLatin1Char('1') : QLatin1Char('0'); return lit;
    };
    EXPECT_EQ(QStringLiteral("1010"),lamps());
    ControlObject::set(ConfigKey("[EffectRack1_EffectUnit4]","enabled"),1); const int enabledWrites=writes;
    QTest::qWait(150); EXPECT_EQ(enabledWrites,writes);
    EXPECT_EQ(QStringLiteral("1011"),lamps());
    EXPECT_TRUE(board->toolTip().contains("FX 4; decks 4;"));
    // A real MIDI step below one percent is still wet. Display rounding must
    // never turn it into a false DRY indication.
    ControlObject::set(ConfigKey("[EffectRack1_EffectUnit1]","mix"),1.0/127);
    ControlObject::set(ConfigKey("[EffectRack1_EffectUnit3]","mix"),1.0/127);
    ControlObject::set(ConfigKey("[EffectRack1_EffectUnit4]","mix"),.001); const int lowMixWrites=writes;
    QTest::qWait(150); EXPECT_EQ(lowMixWrites,writes);
    EXPECT_EQ(QStringLiteral("1011"),lamps());
    EXPECT_TRUE(board->toolTip().contains("<1%"));
    EXPECT_FALSE(board->toolTip().section('\n',0,0).contains("DRY"));
    ControlObject::set(ConfigKey("[EffectRack1_EffectUnit3]","mix"),0); const int dryWrites=writes;
    QTest::qWait(150); EXPECT_EQ(dryWrites,writes);
    EXPECT_EQ(QStringLiteral("1001"),lamps());
    ControlObject::set(ConfigKey("[EffectRack1_EffectUnit4]","mix"),0); const int fourthDryWrites=writes;
    QTest::qWait(150); EXPECT_EQ(fourthDryWrites,writes);
    EXPECT_EQ(QStringLiteral("1000"),lamps());
    // No unit names are ever cut short on the rail: the lamps carry numbers only.
    for(const auto& chip:board->chips()) EXPECT_EQ(1,chip.text.size());
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
    auto* board=panel->findChild<DeckLabChipButton*>("decklab-performance-board-fx"); ASSERT_NE(nullptr,board);
    QMetaObject::invokeMethod(choice,"activated",Q_ARG(int,2)); QTest::qWait(150);
    QString lit; for(const auto& chip:board->chips()) lit+=chip.on ? QLatin1Char('1') : QLatin1Char('0');
    EXPECT_EQ(QStringLiteral("1010"),lit);
    for(int unit:{1,3})
        EXPECT_TRUE(board->toolTip().contains(QStringLiteral("FX %1; decks %1; %2; 50%").arg(unit).arg(manifests[0]->name())));
    QMetaObject::invokeMethod(choice,"activated",Q_ARG(int,1)); QTest::qWait(150);
    EXPECT_TRUE(board->toolTip().section('\n',0,0).endsWith("50%"));
    EXPECT_FALSE(board->toolTip().section('\n',0,0).contains("DRY"));
    board->click(); QCoreApplication::processEvents();
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
    // Touch zoom (in the More panel) moves every deck together.
    panel->findChild<QPushButton*>("decklab-1-zoom_out")->click(); QTest::qWait(50);
    const double zoomed=ControlObject::get(ConfigKey("[Channel1]","waveform_zoom"));
    for(int deck=2;deck<=4;++deck) EXPECT_DOUBLE_EQ(zoomed,ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck),"waveform_zoom")));
}

TEST_F(NativeDeckLabTest, MasterLockMissingBeatGridAndSavedLoopIndicatorsUseRealState) {
    auto ungridded=m_pMixerDeck1->loadFakeTrack(false,0);
    m_pMixerDeck2->loadFakeTrack(false,130); ProcessBuffer(); QTest::qWait(150);
    ASSERT_FALSE(ungridded->getBeats());
    // No stored grid: the phase meter is dark rather than inventing beats.
    EXPECT_TRUE(strip(1)->state().loaded); EXPECT_EQ(-1,strip(1)->state().phase);
    m_pMixerDeck1->loadFakeTrack(false,128); ProcessBuffer();
    ControlObject::set(ConfigKey("[Channel1]","sync_leader"),1); ProcessBuffer(); QTest::qWait(150);
    ASSERT_GT(ControlObject::get(ConfigKey("[Channel1]","sync_leader")),0);
    EXPECT_TRUE(strip(1)->state().master);
    EXPECT_TRUE(strip(1)->state().sync);
    EXPECT_GE(strip(1)->state().phase,0);
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
    // The cue sets on touch-down, on deck 4 only; a context change before
    // the release closes the menu and the release adds nothing.
    QTest::mousePress(cue4,Qt::LeftButton);
    const double set4=ControlObject::get(ConfigKey("[Channel4]","hotcue_1_position"));
    EXPECT_GE(set4,0);
    ControlObject::set(ConfigKey("[DeckLabLeft]","current"),1); QCoreApplication::processEvents();
    QTest::mouseRelease(cue4,Qt::LeftButton);
    EXPECT_FALSE(menu4->isVisible()); EXPECT_DOUBLE_EQ(set4,ControlObject::get(ConfigKey("[Channel4]","hotcue_1_position")));
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
        panel->findChild<QPushButton*>("decklab-1-zoom_in")->click();
        panel->findChild<QPushButton*>("decklab-1-zoom_out")->click(); QTest::qWait(150);
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
    EXPECT_FALSE(play->isChecked());
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
    QCoreApplication::processEvents();
    // B-42: no hidden search and no source switch; the Match bar and the
    // breadcrumb show the filter, with a clear.
    auto* search = panel->findChild<WSearchLineEdit*>(); ASSERT_NE(nullptr, search);
    EXPECT_TRUE(search->currentText().isEmpty());
    EXPECT_TRUE(library->isVisible());
    auto* bar = panel->findChild<QWidget*>("decklab-match-bar"); ASSERT_NE(nullptr, bar);
    EXPECT_TRUE(bar->isVisible());
    auto* only = static_cast<DeckLabActionButton*>(panel->findChild<QPushButton*>("decklab-match-only"));
    ASSERT_NE(nullptr, only);
    EXPECT_EQ(DeckLabActionButton::Tone::Selected, only->tone());
    ASSERT_FALSE(crumbs().isEmpty());
    EXPECT_TRUE(crumbs().last().startsWith(QStringLiteral("Matches deck 4"))) << crumbs().join(" / ").toStdString();
    EXPECT_TRUE(panel->findChild<QPushButton*>("decklab-match-clear")->isVisible());
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
    const std::array<mixxx::track::io::key::ChromaticKey,4> keys{mixxx::track::io::key::A_MINOR,
            mixxx::track::io::key::C_MAJOR,mixxx::track::io::key::E_MINOR,mixxx::track::io::key::F_SHARP_MAJOR};
    for(int deck=1;deck<=4;++deck) loadGenerated(deck,124+2*deck,keys[deck-1]);
    ProcessBuffer();
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
        QList<QRect> readouts;
        for(int deck=1;deck<=4;++deck) {
            auto* readout=strip(deck); ASSERT_NE(nullptr,readout);
            if(!readout->isVisible()) continue;
            EXPECT_TRUE(readout->state().tempo.contains("%"));
            EXPECT_TRUE(readout->state().sync);
            EXPECT_FALSE(readout->state().range.isEmpty());
            if(index==2) {
                // Four compact readouts carry their own lamps.
                for(const auto& lamp:{"LOOP","KEY","QNT","SYNC"}) EXPECT_TRUE(readout->modesText().contains(lamp)) << lamp;
            } else {
                // Two-deck view lights the side's buttons instead.
                EXPECT_TRUE(panel->findChild<QPushButton*>(QStringLiteral("decklab-%1-quantize").arg(deck))->isChecked());
                EXPECT_TRUE(panel->findChild<QPushButton*>(QStringLiteral("decklab-%1-keylock").arg(deck))->isChecked());
                EXPECT_TRUE(panel->findChild<QPushButton*>(QStringLiteral("decklab-%1-beatloop_activate").arg(deck))->isChecked());
            }
            EXPECT_TRUE(readout->renderer().overflowingParts().isEmpty())
                    << deck << " layout " << index << " " << readout->renderer().overflowingParts().join(',').toStdString();
            const QRect area(readout->mapTo(panel.get(),QPoint()),readout->size());
            EXPECT_TRUE(panel->rect().contains(area));
            for(const auto& other:readouts) EXPECT_FALSE(other.intersects(area));
            readouts<<area;
        }
        EXPECT_EQ(index==2 ? 4 : 2,readouts.size());
    }
    capture(QStringLiteral("decks-fixture-four-modes.png"));
}

TEST_F(NativeDeckLabTest, NeedleSearchSeeksOnlyWhilePaused) {
    loadGenerated(1, 128); ProcessBuffer();
    ControlObject::set(ConfigKey("[Channel1]", "quantize"), 0); QTest::qWait(150);
    auto* overview = panel->findChild<QWidget*>("decklab-overview-1"); ASSERT_NE(nullptr, overview);
    ASSERT_TRUE(overview->isVisible());
    EXPECT_FALSE(overview->testAttribute(Qt::WA_TransparentForMouseEvents));
    EXPECT_FALSE(overview->acceptDrops());
    const auto position = [] { return ControlObject::get(ConfigKey("[Channel1]", "playposition")); };
    // Paused: touching the whole-track overview searches to that point.
    QTest::mouseClick(overview, Qt::LeftButton, Qt::NoModifier, QPoint(overview->width() * 3 / 4, overview->height() / 2));
    ProcessBuffer(); ProcessBuffer();
    EXPECT_NEAR(.75, position(), .01);
    // Dragging keeps searching under the finger.
    QTest::mousePress(overview, Qt::LeftButton, Qt::NoModifier, QPoint(overview->width() / 4, 20));
    const QPointF middle(overview->width() / 2.0, 20);
    QMouseEvent drag(QEvent::MouseMove, middle, overview->mapToGlobal(middle), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(overview, &drag);
    QTest::mouseRelease(overview, Qt::LeftButton, Qt::NoModifier, middle.toPoint());
    ProcessBuffer(); ProcessBuffer();
    EXPECT_NEAR(.5, position(), .01);
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "play")));
    // Playing: the overview is locked, so a stray touch never jumps a live track.
    ControlObject::set(ConfigKey("[Channel1]", "play"), 1); ProcessBuffer(); settle();
    const double live = position();
    QTest::mouseClick(overview, Qt::LeftButton, Qt::NoModifier, QPoint(overview->width() / 10, 20));
    ProcessBuffer(); ProcessBuffer();
    EXPECT_GE(position(), live);
    EXPECT_GT(position(), .4);
    ControlObject::set(ConfigKey("[Channel1]", "play"), 0); ProcessBuffer(); settle();
    // Off the Decks page the overview is hidden and nothing seeks either.
    panel->selectPage(1); QCoreApplication::processEvents();
    EXPECT_FALSE(overview->isVisible());
}

TEST_F(NativeDeckLabTest, BeatPhaseMeterCountsGridBeatsInFours) {
    // Beats counted from the grid's first marker, in groups of four. Mixxx has
    // no downbeat data, so this is a beat counter and never claims bars.
    EXPECT_EQ(0, deckLabBeatPhase(0, 0).segment);
    EXPECT_EQ(3, deckLabBeatPhase(3, .99).segment);
    EXPECT_EQ(0, deckLabBeatPhase(4, .01).segment);
    // The previous "BEAT 77.09" readout: index 76, 9% into the beat.
    EXPECT_EQ(0, deckLabBeatPhase(76, .09).segment);
    EXPECT_NEAR(.09, deckLabBeatPhase(76, .09).fraction, 1e-9);
    // Before the first marker the count continues backwards.
    EXPECT_EQ(3, deckLabBeatPhase(-1, .5).segment);
    // A phase at or past 1 carries into the following beat.
    EXPECT_EQ(2, deckLabBeatPhase(1, 1.25).segment);
    EXPECT_NEAR(.25, deckLabBeatPhase(1, 1.25).fraction, 1e-9);
    EXPECT_EQ(-1, deckLabBeatPhase(5, util_double_nan()).segment);
    // On a generated 128 BPM grid the lit segment follows the playhead.
    loadGenerated(1, 128); ProcessBuffer();
    ControlObject::set(ConfigKey("[Channel1]", "quantize"), 0); QTest::qWait(150);
    const double samples = ControlObject::get(ConfigKey("[Channel1]", "track_samples")); ASSERT_GT(samples, 0);
    const double beatFrames = 44100.0 * 60 / 128;
    int checked = 0;
    for (int beat : {0, 1, 2, 3, 4, 5, 6, 7, 9}) {
        const double frame = (beat + .5) * beatFrames;
        if (frame * 2 >= samples) break; // stay inside the generated track
        ControlObject::set(ConfigKey("[Channel1]", "playposition"), frame * 2 / samples);
        ProcessBuffer(); QTest::qWait(150);
        EXPECT_NEAR(frame * 2 / samples, ControlObject::get(ConfigKey("[Channel1]", "playposition")), .001);
        EXPECT_EQ(beat % 4, strip(1)->state().phase) << beat;
        ++checked;
    }
    EXPECT_GE(checked, 5);
    capture(QStringLiteral("decks-phase-meter.png"));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "play")));
}

TEST_F(NativeDeckLabTest, EndOfTrackWarningFlashesTheRemainingTime) {
    loadGenerated(1, 128, mixxx::track::io::key::D_MINOR); loadGenerated(2, 124, mixxx::track::io::key::F_MAJOR);
    ProcessBuffer();
    ControlObject::set(ConfigKey("[Channel1]", "quantize"), 0);
    ControlObject::set(ConfigKey("[Channel1]", "duration"), 240);
    ControlObject::set(ConfigKey("[Channel1]", "playposition"), .95); ProcessBuffer(); QTest::qWait(150);
    // Paused near the end: no warning yet.
    EXPECT_FALSE(strip(1)->state().endWarning);
    EXPECT_TRUE(strip(1)->state().remaining.startsWith(QString::fromUtf8("−0:1")));
    // Playing inside the configured warning time: the clock flashes red.
    ControlObject::set(ConfigKey("[Channel1]", "play"), 1); settle(); QTest::qWait(150);
    EXPECT_TRUE(strip(1)->state().endWarning);
    bool lit = false, dark = false;
    for (int step = 0; step < 12; ++step) {
        QTest::qWait(100);
        (strip(1)->renderer().flash() ? lit : dark) = true;
    }
    EXPECT_TRUE(lit); EXPECT_TRUE(dark);
    // The clock never dims: one phase is live-red figures on the ground, the
    // other a live-red box behind dark figures (at least 4.5:1 both ways).
    {
        const QColor red(0xff, 0x3d, 0x2e), ground(0x0d, 0x0f, 0x12);
        const QRect time = strip(1)->renderer().rect(DeckLabStripRenderer::Part::Time);
        const QPoint inBox(time.right() - 6, time.center().y());
        strip(1)->setFlash(false);
        EXPECT_EQ(red, strip(1)->grab().toImage().pixelColor(inBox)) << "red box behind the clock";
        capture(QStringLiteral("decks-end-warning-box.png"));
        strip(1)->setFlash(true);
        EXPECT_EQ(ground, strip(1)->grab().toImage().pixelColor(inBox)) << "red figures on the ground";
        bool redInk = false;
        const QImage figures = strip(1)->grab().toImage();
        for (int x = time.left(); x < time.right() && !redInk; ++x)
            for (int y = time.top(); y < time.bottom() && !redInk; ++y) redInk = figures.pixelColor(x, y) == red;
        EXPECT_TRUE(redInk);
    }
    capture(QStringLiteral("decks-end-warning.png"));
    // A running loop cannot run out, so it never warns.
    ControlObject::set(ConfigKey("[Channel1]", "loop_start_position"), 0);
    ControlObject::set(ConfigKey("[Channel1]", "loop_end_position"), 44100);
    ControlObject::set(ConfigKey("[Channel1]", "loop_enabled"), 1); settle(); QTest::qWait(150);
    EXPECT_FALSE(strip(1)->state().endWarning);
    ControlObject::set(ConfigKey("[Channel1]", "loop_enabled"), 0);
    ControlObject::set(ConfigKey("[Channel1]", "play"), 0); settle(); QTest::qWait(150);
    EXPECT_FALSE(strip(1)->state().endWarning);
}

TEST_F(NativeDeckLabTest, MasterSyncAndKeyLampsFollowRealState) {
    loadGenerated(1, 128, mixxx::track::io::key::A_MINOR); loadGenerated(2, 128, mixxx::track::io::key::C_MAJOR);
    ProcessBuffer(); QTest::qWait(150);
    EXPECT_FALSE(strip(1)->state().master); EXPECT_FALSE(strip(1)->state().sync);
    EXPECT_TRUE(strip(1)->state().tempoCentered);
    EXPECT_EQ(QStringLiteral("±8%"), strip(1)->state().range);
    ControlObject::set(ConfigKey("[Channel1]", "sync_leader"), 1);
    ControlObject::set(ConfigKey("[Channel2]", "sync_enabled"), 1); ProcessBuffer(); QTest::qWait(150);
    ASSERT_GT(ControlObject::get(ConfigKey("[Channel1]", "sync_leader")), 0);
    EXPECT_TRUE(strip(1)->state().master); EXPECT_TRUE(strip(2)->state().sync);
    EXPECT_FALSE(strip(2)->state().master);
    // Key hint against the master: A minor and C major (8A, 8B) mix; the
    // master itself shows no hint.
    EXPECT_EQ(0, strip(1)->state().keyMatch);
    EXPECT_EQ(1, strip(2)->state().keyMatch);
    EXPECT_NE(strip(1)->state().keyColor, QColor());
    capture(QStringLiteral("decks-master-sync.png"));
    ControlObject::set(ConfigKey("[Channel2]", "visual_key"), static_cast<double>(mixxx::track::io::key::F_SHARP_MAJOR));
    QTest::qWait(150);
    EXPECT_EQ(-1, strip(2)->state().keyMatch);
    ControlObject::set(ConfigKey("[Channel2]", "sync_enabled"), 0);
    ControlObject::set(ConfigKey("[Channel1]", "sync_enabled"), 0); ProcessBuffer(); QTest::qWait(150);
    EXPECT_FALSE(strip(1)->state().master); EXPECT_FALSE(strip(2)->state().sync);
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "play")));
}

TEST_F(NativeDeckLabTest, LayerFollowsDeliberateControllerTouchesWithoutFlapping) {
    for (int deck = 1; deck <= 4; ++deck) loadGenerated(deck, 120 + deck);
    ProcessBuffer();
    // Deck 3 starts during start-up; its later stop must not count as a touch.
    ControlObject::set(ConfigKey("[Channel3]", "play"), 1);
    QTest::qWait(1400);
    ASSERT_TRUE(strip(1)->isVisible()); ASSERT_TRUE(strip(2)->isVisible());
    // A deliberate controller touch on deck 3 (its channel fader) brings
    // deck 3 to the left side, and the layer control follows.
    ControlObject::set(ConfigKey("[Channel3]", "volume"), .5); settle();
    EXPECT_TRUE(strip(3)->isVisible()); EXPECT_FALSE(strip(1)->isVisible()); EXPECT_TRUE(strip(2)->isVisible());
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[DeckLabLeft]", "current")));
    EXPECT_TRUE(panel->findChild<QWidget*>("decklab-detail-3")->isVisible());
    // A mapping that writes both layers at once does not flip back and forth.
    ControlObject::set(ConfigKey("[Channel1]", "volume"), .6); settle();
    EXPECT_TRUE(strip(3)->isVisible());
    // A later deliberate touch on deck 1 returns it.
    QTest::qWait(450);
    ControlObject::set(ConfigKey("[Channel1]", "volume"), .7); settle();
    EXPECT_TRUE(strip(1)->isVisible()); EXPECT_FALSE(strip(3)->isVisible());
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[DeckLabLeft]", "current")));
    // Engine-originated changes never switch: a stop (as at the end of a
    // track), or a tempo move on a Sync follower.
    QTest::qWait(450);
    ControlObject::set(ConfigKey("[Channel3]", "play"), 0); settle();
    EXPECT_TRUE(strip(1)->isVisible());
    ControlObject::set(ConfigKey("[Channel4]", "sync_enabled"), 1); settle();
    ControlObject::set(ConfigKey("[Channel4]", "rate"), .05); settle();
    EXPECT_TRUE(strip(2)->isVisible()); EXPECT_FALSE(strip(4)->isVisible());
    ControlObject::set(ConfigKey("[Channel4]", "sync_enabled"), 0);
    // A jog touch is deliberate.
    QTest::qWait(450);
    ControlObject::set(ConfigKey("[Channel4]", "jog"), 1); settle();
    EXPECT_TRUE(strip(4)->isVisible()); EXPECT_FALSE(strip(2)->isVisible());
    ControlObject::set(ConfigKey("[Channel4]", "jog"), 0);
    // Four-deck view shows every deck: nothing to follow.
    QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"), "activated", Q_ARG(int, 2)); QTest::qWait(30);
    QTest::qWait(450);
    ControlObject::set(ConfigKey("[Channel2]", "volume"), .4); settle();
    for (int deck = 1; deck <= 4; ++deck) EXPECT_TRUE(strip(deck)->isVisible());
    for (int deck = 1; deck <= 4; ++deck) EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), "play")));
}

TEST_F(NativeDeckLabTest, DecksLayoutsScaleToLargerTouchscreens) {
    const std::array<mixxx::track::io::key::ChromaticKey,4> keys{mixxx::track::io::key::A_MINOR,
            mixxx::track::io::key::C_MAJOR,mixxx::track::io::key::E_MINOR,mixxx::track::io::key::G_MAJOR};
    for (int deck = 1; deck <= 4; ++deck) loadGenerated(deck, 120 + 3 * deck, keys[deck - 1]);
    ProcessBuffer(); QTest::qWait(150);
    for (const QSize size : {QSize(1024, 600), QSize(1280, 800), QSize(800, 480)}) {
        panel->resize(size); QTest::qWait(100);
        ASSERT_EQ(size, panel->size());
        for (int mode : {1, 2}) {
            QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"), "activated", Q_ARG(int, mode));
            QTest::qWait(100);
            for (auto* button : panel->findChildren<QPushButton*>()) {
                if (!button->isVisible()) continue;
                EXPECT_TRUE(panel->rect().contains(QRect(button->mapTo(panel.get(), QPoint()), button->size())))
                        << button->objectName().toStdString() << " " << size.width();
                EXPECT_GE(button->height(), 44) << button->objectName().toStdString();
            }
            for (int deck = 1; deck <= (mode == 2 ? 4 : 2); ++deck) {
                ASSERT_TRUE(strip(deck)->isVisible());
                EXPECT_TRUE(strip(deck)->renderer().overflowingParts().isEmpty()) << deck << " " << size.width();
                auto* detail = panel->findChild<QWidget*>(QStringLiteral("decklab-detail-%1").arg(deck));
                EXPECT_EQ(size.width() - 18, detail->width());
            }
            if (size == QSize(1024, 600))
                capture(QStringLiteral("decks-%1-1024x600.png").arg(mode == 2 ? QStringLiteral("four") : QStringLiteral("two")));
        }
    }
    EXPECT_EQ(QSize(800, 480), panel->size());
}

TEST_F(NativeDeckLabTest, LatchedRmxFollowsTempoChangesWithoutAnotherTouch) {
    m_pEffectsManager->setup(); m_pEffectsManager->ensureDeckLabEffects();
    m_pEngineMixer->prepareAudio(mixxx::audio::SampleRate(44100));
    m_pMixerDeck1->loadFakeTrack(false, 128); ProcessBuffer(); QCoreApplication::processEvents();
    panel->selectPage(3); panel->findChild<QPushButton*>("decklab-fx-on")->click();
    ControlObject::set(ConfigKey("[Channel1]", "bpm"), 140);
    QTest::qWait(150);
    EXPECT_DOUBLE_EQ(140, ControlObject::get(ConfigKey("[DeckLabEffectUnit1_Effect4]", "parameter11")));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "play")));
}

// r4-labfx: one page, six effects, all on the unchanged Lab FX engine (the
// three-band performance LV2): no tabs, no dropdowns.
TEST_F(NativeDeckLabTest, LabFxEffectTilesDriveTheExistingEngine) {
    m_pEffectsManager->setup(); m_pEffectsManager->ensureDeckLabEffects();
    m_pEngineMixer->prepareAudio(mixxx::audio::SampleRate(44100));
    m_pMixerDeck1->loadFakeTrack(false, 128); ProcessBuffer(); QCoreApplication::processEvents();
    panel->selectPage(3); QCoreApplication::processEvents();
    const auto param = [](int n) {
        return ControlObject::get(ConfigKey("[DeckLabEffectUnit1_Effect4]", QStringLiteral("parameter%1").arg(n)));
    };
    const auto find = [this](const QString& name) { return panel->findChild<QPushButton*>(name); };
    auto* on = find("decklab-fx-on"); ASSERT_NE(nullptr, on);
    auto* page = panel->findChild<QWidget*>("decklab-labfx"); ASSERT_NE(nullptr, page);
    EXPECT_EQ(nullptr, find("decklab-fx-view-0"));
    EXPECT_TRUE(page->findChildren<QComboBox*>().isEmpty());
    EXPECT_TRUE(page->findChildren<QSlider*>().isEmpty());
    // ECHO and REVERB: the band levers in modes 1 and 2, at the beat cell.
    for (int effect = 0; effect < 2; ++effect) {
        find(QStringLiteral("decklab-fx-effect-%1").arg(effect))->click();
        on->click();
        EXPECT_DOUBLE_EQ(effect + 1, param(4));
        for (int band = 1; band <= 3; ++band) EXPECT_DOUBLE_EQ(1, param(band));
        EXPECT_DOUBLE_EQ(0.5, param(12)) << "1/2 beat";
        find("decklab-fx-cut")->click();
    }
    // FILTER, DUCK, DRIVE: the isolator in modes 4, 5 and 3, never silent on ON.
    const std::array<std::pair<int, int>, 3> sweeps{{{2, 4}, {4, 5}, {5, 3}}};
    for (const auto& [tile, mode] : sweeps) {
        find(QStringLiteral("decklab-fx-effect-%1").arg(tile))->click();
        on->click();
        EXPECT_DOUBLE_EQ(mode, param(9)) << tile;
        EXPECT_NE(0, param(6)) << tile;
        find("decklab-fx-cut")->click();
    }
    // The band lamps choose what gets the effect.
    find("decklab-fx-effect-0")->click();
    find("decklab-fx-band-0")->click();
    on->click();
    EXPECT_DOUBLE_EQ(0, param(1));
    EXPECT_DOUBLE_EQ(1, param(2));
    find("decklab-fx-cut")->click();
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[DeckLabEffectUnit1]", "mix")));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "play")));
}

TEST_F(NativeDeckLabTest, RollPadsRequireAPlayingDeck) {
    m_pEffectsManager->setup(); m_pEffectsManager->ensureDeckLabEffects();
    m_pEngineMixer->prepareAudio(mixxx::audio::SampleRate(44100));
    m_pMixerDeck1->loadFakeTrack(false, 128); ProcessBuffer(); QCoreApplication::processEvents();
    panel->selectPage(3); QCoreApplication::processEvents();
    // r4-labfx: ROLL is an effect on the pad; its columns are the roll sizes.
    panel->findChild<QPushButton*>("decklab-fx-effect-3")->click();
    auto* pad = panel->findChild<QWidget*>("decklab-fx-xy"); ASSERT_NE(nullptr, pad);
    auto* cell = panel->findChild<QPushButton*>("decklab-fx-beat-2"); ASSERT_NE(nullptr, cell);
    EXPECT_EQ(QStringLiteral("1/4"), cell->text());
    const QPoint quarterColumn(pad->mapFromGlobal(cell->mapToGlobal(cell->rect().center())).x(), pad->height() / 2);
    const ConfigKey quarter("[Channel1]", "beatlooproll_0.25_activate");
    QTest::mousePress(pad, Qt::LeftButton, Qt::NoModifier, quarterColumn);
    EXPECT_DOUBLE_EQ(0, ControlObject::get(quarter)) << "a paused deck has nothing to roll";
    QTest::mouseRelease(pad, Qt::LeftButton, Qt::NoModifier, quarterColumn);
    ControlObject::set(ConfigKey("[Channel1]", "play"), 1); QCoreApplication::processEvents();
    QTest::mousePress(pad, Qt::LeftButton, Qt::NoModifier, quarterColumn);
    EXPECT_DOUBLE_EQ(1, ControlObject::get(quarter));
    QTest::mouseRelease(pad, Qt::LeftButton, Qt::NoModifier, quarterColumn);
    EXPECT_DOUBLE_EQ(0, ControlObject::get(quarter));
    ControlObject::set(ConfigKey("[Channel1]", "play"), 0); QCoreApplication::processEvents();
}

// qw-perf: one lamp language. Lit states are a #F2F4F7 fill with dark text
// (Play lights green), never a 1 px outline.
TEST_F(NativeDeckLabTest, OnStatesAreFilledLampsNotOutlines) {
    loadGenerated(1, 128); loadGenerated(3, 132); ProcessBuffer(); settle();
    const QColor lit(0xf2, 0xf4, 0xf7), panelFill(0x16, 0x19, 0x1e), green(0x2f, 0xd2, 0x7a);
    const auto fill = [](QPushButton* button) { return button->grab().toImage().pixelColor(2, button->height() / 2); };
    for (const char* item : {"quantize", "keylock", "loop_enabled"})
        ControlObject::set(ConfigKey(QStringLiteral("[Channel1]"), QString::fromLatin1(item)), 0);
    ControlObject::set(ConfigKey("[Channel1]", "beatloop_size"), 4); ProcessBuffer(); settle();
    for (const char* item : {"quantize", "keylock", "beatloop_activate"}) {
        auto* button = panel->findChild<QPushButton*>(QStringLiteral("decklab-1-") + QString::fromLatin1(item));
        ASSERT_NE(nullptr, button) << item;
        EXPECT_FALSE(button->isChecked()) << item;
        EXPECT_EQ(panelFill, fill(button)) << item;
        button->click(); QCoreApplication::processEvents(); ProcessBuffer(); settle();
        EXPECT_TRUE(button->isChecked()) << item;
        EXPECT_EQ(lit, fill(button)) << item << ": on is a filled lamp";
        EXPECT_EQ(44, button->height()) << item;
    }
    capture(QStringLiteral("decks-lit-lamps.png"));
    // Four-deck Play: green fill while playing.
    QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"), "activated", Q_ARG(int, 2)); QTest::qWait(150);
    auto* play = panel->findChild<QPushButton*>("decklab-card-3-play"); ASSERT_NE(nullptr, play);
    EXPECT_EQ(panelFill, fill(play));
    QTest::mousePress(play, Qt::LeftButton);
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel3]", "play"))) << "Play acts on touch-down";
    QTest::mouseRelease(play, Qt::LeftButton); settle();
    EXPECT_TRUE(play->isChecked()); EXPECT_EQ(green, fill(play));
    // A second touch pauses, again on touch-down; a press on an empty deck does nothing.
    QTest::mousePress(play, Qt::LeftButton);
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel3]", "play")));
    QTest::mouseRelease(play, Qt::LeftButton); settle();
    EXPECT_FALSE(play->isChecked());
    auto* emptyPlay = panel->findChild<QPushButton*>("decklab-card-2-play"); ASSERT_NE(nullptr, emptyPlay);
    EXPECT_FALSE(emptyPlay->isEnabled());
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel2]", "play")));
}

TEST_F(NativeDeckLabTest, SyncTapMatchesOnceAndHoldTogglesTheLock) {
    loadGenerated(1, 128); loadGenerated(2, 130); loadGenerated(3, 132); ProcessBuffer(); settle();
    auto* sync = panel->findChild<QPushButton*>("decklab-1-beatsync"); ASSERT_NE(nullptr, sync);
    const auto fill = [](QPushButton* button) { return button->grab().toImage().pixelColor(2, button->height() / 2); };
    const auto locked = [](int deck) { return ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), "sync_enabled")); };
    EXPECT_EQ(QStringLiteral("SYNC"), sync->text());
    EXPECT_TRUE(sync->accessibleDescription().contains("Hold"));
    // Tap: one-shot match, MATCHED on the readout, never a lock.
    sync->click(); settle();
    EXPECT_DOUBLE_EQ(0, locked(1));
    EXPECT_TRUE(sync->property("actionFeedback").toBool()); EXPECT_FALSE(sync->property("lit").toBool());
    EXPECT_TRUE(strip(1)->state().matched); EXPECT_FALSE(strip(1)->state().sync);
    capture(QStringLiteral("decks-sync-matched.png"));
    EXPECT_TRUE(QTest::qWaitFor([this] { settle(); return !strip(1)->state().matched; }, 2000));
    // Hold: the lock toggles at the hold time, with the finger still down,
    // and the release does not also match once.
    QTest::mousePress(sync, Qt::LeftButton);
    EXPECT_TRUE(QTest::qWaitFor([&locked] { return locked(1) > 0; }, 1500));
    QTest::mouseRelease(sync, Qt::LeftButton); settle();
    EXPECT_DOUBLE_EQ(1, locked(1));
    EXPECT_TRUE(sync->property("lit").toBool()); EXPECT_FALSE(sync->property("actionFeedback").toBool());
    EXPECT_EQ(QColor(0xf2, 0xf4, 0xf7), fill(sync)) << "locked SYNC is a filled lamp";
    EXPECT_TRUE(strip(1)->state().sync); EXPECT_FALSE(strip(1)->state().matched);
    EXPECT_TRUE(sync->accessibleName().contains("locked"));
    capture(QStringLiteral("decks-sync-locked.png"));
    // A tap while locked still only matches; it never unlocks.
    sync->click(); settle();
    EXPECT_DOUBLE_EQ(1, locked(1));
    // Hold again: unlocked.
    QTest::mousePress(sync, Qt::LeftButton); QTest::qWait(700);
    QTest::mouseRelease(sync, Qt::LeftButton); settle();
    EXPECT_DOUBLE_EQ(0, locked(1)); EXPECT_FALSE(sync->property("lit").toBool());
    // A layer change during the hold cancels it on both decks.
    QTest::mousePress(sync, Qt::LeftButton);
    ControlObject::set(ConfigKey("[DeckLabLeft]", "current"), 1); QCoreApplication::processEvents();
    QTest::qWait(700);
    QTest::mouseRelease(sync, Qt::LeftButton); settle();
    EXPECT_DOUBLE_EQ(0, locked(1)); EXPECT_DOUBLE_EQ(0, locked(3));
    ControlObject::set(ConfigKey("[DeckLabLeft]", "current"), 0); settle();
    // A press that slides off before the hold time is no lock either.
    QTest::mousePress(sync, Qt::LeftButton);
    sync->setDown(false);
    QTest::qWait(700);
    QTest::mouseRelease(sync, Qt::LeftButton); settle();
    EXPECT_DOUBLE_EQ(0, locked(1));
    // Four-deck cards: the same SYNC.
    QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"), "activated", Q_ARG(int, 2)); QTest::qWait(150);
    auto* card = panel->findChild<QPushButton*>("decklab-card-2-beatsync"); ASSERT_NE(nullptr, card);
    QTest::mousePress(card, Qt::LeftButton);
    EXPECT_TRUE(QTest::qWaitFor([&locked] { return locked(2) > 0; }, 1500));
    QTest::mouseRelease(card, Qt::LeftButton); settle();
    EXPECT_TRUE(card->property("lit").toBool()); EXPECT_TRUE(strip(2)->modesText().contains("SYNC"));
    ControlObject::set(ConfigKey("[Channel2]", "sync_enabled"), 0); settle();
    EXPECT_FALSE(card->property("lit").toBool());
    for (int deck = 1; deck <= 3; ++deck) EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), "play")));
}

TEST_F(NativeDeckLabTest, HotcuesSetWhilePlayingOnTouchDown) {
    loadGenerated(1, 128); ProcessBuffer();
    ControlObject::set(ConfigKey("[Channel1]", "quantize"), 0);
    ControlObject::set(ConfigKey("[Channel1]", "playposition"), .3); ProcessBuffer();
    ControlObject::set(ConfigKey("[Channel1]", "play"), 1); ProcessBuffer(); settle();
    auto* b = panel->findChild<QPushButton*>("decklab-hotcue-1-2"); ASSERT_NE(nullptr, b);
    EXPECT_TRUE(b->isEnabled()) << "an empty pad is live while the deck plays";
    EXPECT_EQ(QStringLiteral("B"), b->text());
    EXPECT_EQ(QStringLiteral("Set cue B here"), b->toolTip());
    QTest::mousePress(b, Qt::LeftButton); ProcessBuffer();
    const double position = ControlObject::get(ConfigKey("[Channel1]", "hotcue_2_position"));
    EXPECT_GE(position, 0) << "set on touch-down, while playing";
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel1]", "play")));
    QTest::mouseRelease(b, Qt::LeftButton); settle();
    EXPECT_DOUBLE_EQ(position, ControlObject::get(ConfigKey("[Channel1]", "hotcue_2_position")));
    EXPECT_TRUE(b->property("cueSet").toBool()); EXPECT_TRUE(b->toolTip().startsWith("Jump"));
    // The set pad now jumps on touch-down, still playing.
    for (int n = 0; n < 20; ++n) ProcessBuffer();
    QTest::mousePress(b, Qt::LeftButton); ProcessBuffer();
    const double jumped = m_pChannel1->getEngineBuffer()->getExactPlayPos().toEngineSamplePos();
    EXPECT_GE(jumped, position - 4); EXPECT_LT(jumped, position + 44100);
    QTest::mouseRelease(b, Qt::LeftButton);
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel1]", "play")));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "hotcue_2_activate")));
    // Without a track every pad is disabled.
    ControlObject::set(ConfigKey("[Channel1]", "play"), 0); ProcessBuffer();
    QTest::qWait(kUnreplaceDelay + 1);
    m_pMixerDeck1->slotEjectTrack(1); ProcessBuffer(); QTest::qWait(150);
    EXPECT_FALSE(b->isEnabled());
}

TEST_F(NativeDeckLabTest, XyPadAndHoldEffectsOutliveFourSecondsAndLostTouchesEndThem) {
    m_pEffectsManager->setup(); m_pEffectsManager->ensureDeckLabEffects();
    m_pEngineMixer->prepareAudio(mixxx::audio::SampleRate(44100));
    m_pMixerDeck1->loadFakeTrack(false, 128); ProcessBuffer(); QCoreApplication::processEvents();
    panel->selectPage(3);
    const auto mix = [] { return ControlObject::get(ConfigKey("[DeckLabEffectUnit1]", "mix")); };
    const auto on = [] { return ControlObject::get(ConfigKey("[DeckLabEffectUnit1_Effect4]", "enabled")); };
    const auto find = [this](const QString& name) { return panel->findChild<QPushButton*>(name); };
    auto* fx = panel->findChild<DeckLabLabFx*>("decklab-labfx"); ASSERT_NE(nullptr, fx);
    auto* xy = panel->findChild<QWidget*>("decklab-fx-xy"); ASSERT_NE(nullptr, xy); ASSERT_TRUE(xy->isVisible());
    // r4-labfx: ECHO on the pad, held well past the old 4 s cut-out.
    const QPoint point(xy->width() * 3 / 4, xy->height() / 4);
    QTest::mousePress(xy, Qt::LeftButton, Qt::NoModifier, point);
    EXPECT_DOUBLE_EQ(1, mix()); EXPECT_DOUBLE_EQ(1, on());
    QTest::qWait(4600);
    EXPECT_DOUBLE_EQ(1, mix()) << "no time limit while the finger is down";
    EXPECT_DOUBLE_EQ(1, on());
    EXPECT_EQ(DeckLabLabFx::Lamp::On, fx->lamp());
    capture(QStringLiteral("labfx-xy-held.png"));
    QTest::mouseRelease(xy, Qt::LeftButton, Qt::NoModifier, point);
    EXPECT_EQ(DeckLabLabFx::Lamp::Tail, fx->lamp()) << "lift: the echo tail rings out";
    find("decklab-fx-cut")->click();
    EXPECT_DOUBLE_EQ(0, mix());
    // FILTER: lift is straight back to dry.
    find("decklab-fx-effect-2")->click();
    QTest::mousePress(xy, Qt::LeftButton, Qt::NoModifier, point);
    EXPECT_DOUBLE_EQ(1, mix());
    QTest::mouseRelease(xy, Qt::LeftButton, Qt::NoModifier, point);
    EXPECT_DOUBLE_EQ(0, mix()) << "lift = dry";
    // Lost touch: the release never arrives, but the next touch lands
    // elsewhere. That ends the hold; no wall clock is involved.
    QTest::mousePress(xy, Qt::LeftButton, Qt::NoModifier, point);
    EXPECT_DOUBLE_EQ(1, mix());
    auto* elsewhere = panel->findChild<QWidget*>("decklab-fx-readout"); ASSERT_NE(nullptr, elsewhere);
    QTest::mousePress(elsewhere, Qt::LeftButton);
    EXPECT_DOUBLE_EQ(0, mix()) << "a touch elsewhere ends a hold whose release went astray";
    QTest::mouseRelease(elsewhere, Qt::LeftButton);
    QTest::mouseRelease(xy, Qt::LeftButton, Qt::NoModifier, point);
    EXPECT_DOUBLE_EQ(0, mix());
    // Rolls too: no time limit while held.
    ControlObject::set(ConfigKey("[Channel1]", "play"), 1); ProcessBuffer(); settle();
    find("decklab-fx-effect-3")->click();
    auto* cell = find("decklab-fx-beat-1"); ASSERT_NE(nullptr, cell);
    const QPoint eighth(xy->mapFromGlobal(cell->mapToGlobal(cell->rect().center())).x(), xy->height() / 2);
    const ConfigKey rolling("[Channel1]", "beatlooproll_0.125_activate");
    QTest::mousePress(xy, Qt::LeftButton, Qt::NoModifier, eighth);
    EXPECT_DOUBLE_EQ(1, ControlObject::get(rolling));
    QTest::qWait(4300);
    EXPECT_DOUBLE_EQ(1, ControlObject::get(rolling)) << "a roll is not cut at 4 s";
    QTest::mouseRelease(xy, Qt::LeftButton, Qt::NoModifier, eighth);
    EXPECT_DOUBLE_EQ(0, ControlObject::get(rolling));
    ControlObject::set(ConfigKey("[Channel1]", "play"), 0); ProcessBuffer();
    // Leaving the page keeps a latched effect playing (PF-X24); the rail's
    // Lab FX tab is lit meanwhile. CUT stops it.
    find("decklab-fx-effect-0")->click();
    find("decklab-fx-on")->click();
    EXPECT_TRUE(fx->isLatched());
    panel->selectPage(0); QTest::qWait(250);
    EXPECT_DOUBLE_EQ(1, mix()) << "a latched effect survives the page change";
    EXPECT_TRUE(find("decklab-tab-3")->property("live").toBool());
    panel->selectPage(3);
    find("decklab-fx-cut")->click();
    EXPECT_DOUBLE_EQ(0, mix());
}

TEST_F(NativeDeckLabTest, LabFxAndSamplerUseBarlowBrandColoursAndClearCopy) {
    m_pEffectsManager->setup(); m_pEffectsManager->ensureDeckLabEffects();
    m_pEngineMixer->prepareAudio(mixxx::audio::SampleRate(44100));
    m_pMixerDeck1->loadFakeTrack(false, 128); ProcessBuffer(); QCoreApplication::processEvents();
    panel->selectPage(3); QCoreApplication::processEvents();
    const QColor panelFill(0x16, 0x19, 0x1e), line(0x2c, 0x31, 0x3a), lit(0xf2, 0xf4, 0xf7);
    const auto find = [this](const char* name) { return panel->findChild<QPushButton*>(QString::fromLatin1(name)); };
    // r4-labfx: the controls say what they do ("Mute tail" muted the track).
    for (const auto& [name, text] : std::vector<std::pair<const char*, const char*>>{{"decklab-fx-release", "RELEASE"},
                 {"decklab-fx-echo-out", "ECHO OUT"}, {"decklab-fx-cut", "CUT"}, {"decklab-fx-on", "ON"},
                 {"decklab-fx-effect-0", "ECHO"}, {"decklab-fx-effect-4", "DUCK"}, {"decklab-fx-target-5", "MASTER"},
                 {"decklab-fx-target-6", "SMP"}, {"decklab-fx-beat-3", "1/2"}}) {
        ASSERT_NE(nullptr, find(name)) << name;
        EXPECT_EQ(QString::fromUtf8(text), find(name)->text()) << name;
    }
    // Painted in Barlow on the brand tokens: ON is an outline when dry and a
    // filled lamp when on.
    auto* labFx = panel->findChild<QWidget*>("decklab-labfx"); ASSERT_NE(nullptr, labFx);
    EXPECT_EQ(QStringLiteral("Barlow"), QFontInfo(labFx->font()).family());
    auto* on = find("decklab-fx-on");
    EXPECT_EQ(panelFill, on->grab().toImage().pixelColor(4, on->height() / 2));
    on->click(); QCoreApplication::processEvents();
    EXPECT_EQ(lit, on->grab().toImage().pixelColor(4, on->height() / 2)) << "ON is a filled lamp";
    capture(QStringLiteral("labfx-on.png"));
    find("decklab-fx-cut")->click();
    // The pad: brand panel, a line-colour centre for the FILTER sweep, no red.
    find("decklab-fx-effect-2")->click(); QCoreApplication::processEvents();
    auto* xy = panel->findChild<QWidget*>("decklab-fx-xy"); ASSERT_NE(nullptr, xy);
    const QImage pad = xy->grab().toImage();
    EXPECT_EQ(panelFill, pad.pixelColor(5, xy->height() / 3));
    EXPECT_EQ(line, pad.pixelColor(xy->width() / 2, xy->height() / 3));
    for (int y = 0; y < pad.height(); y += 3)
        for (int x = 0; x < pad.width(); x += 3) {
            const QColor c = pad.pixelColor(x, y);
            ASSERT_FALSE(c.red() > 150 && c.green() < 100 && c.blue() < 100) << "no red on the XY pad at " << x << "," << y;
        }
    capture(QStringLiteral("labfx-filter.png"));
    find("decklab-fx-effect-0")->click();
    // Sampler: Barlow pads on the brand panel, a "+" for empty pads, and
    // playing (checked) as a filled lamp.
    panel->selectPage(2); QCoreApplication::processEvents();
    auto* sample = find("decklab-sample-1"); ASSERT_NE(nullptr, sample);
    sample->ensurePolished();
    EXPECT_EQ(QStringLiteral("Barlow"), QFontInfo(sample->font()).family());
    EXPECT_EQ(QStringLiteral("Pad 1, empty: tap to load"), sample->accessibleName());
    EXPECT_TRUE(sample->text().section(QChar(10), 1).isEmpty());
    EXPECT_EQ(panelFill, sample->grab().toImage().pixelColor(3, sample->height() / 2));
    sample->setChecked(true);
    EXPECT_EQ(lit, sample->grab().toImage().pixelColor(3, sample->height() / 2));
    sample->setChecked(false);
    auto* stop = find("decklab-samples-stop"); ASSERT_NE(nullptr, stop); stop->ensurePolished();
    EXPECT_EQ(QStringLiteral("STOP ALL"), stop->text());
    EXPECT_EQ(QStringLiteral("Barlow"), QFontInfo(stop->font()).family());
    capture(QStringLiteral("sampler-brand.png"));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "play")));
}

TEST_F(NativeDeckLabTest, SamplerPadsFireOnTouchDownAndEmptyPadsOpenBrowseOnRelease) {
    // An empty pad never changes the page under a finger that is still down.
    panel->selectPage(2); QCoreApplication::processEvents();
    auto* empty = panel->findChild<QPushButton*>("decklab-sample-2"); ASSERT_NE(nullptr, empty);
    QTest::mousePress(empty, Qt::LeftButton);
    EXPECT_EQ(2, panel->currentPage());
    QTest::mouseRelease(empty, Qt::LeftButton);
    EXPECT_EQ(1, panel->currentPage()) << "the release opens Browse to load it";
    panel->selectPage(2);
    // A loaded pad plays on touch-down. Generated sampler controls only: no
    // audio, no files.
    std::vector<std::unique_ptr<ControlObject>> controls;
    for (const char* item : {"track_loaded", "play", "play_stutter", "stop", "pregain"})
        controls.push_back(std::make_unique<ControlObject>(ConfigKey(QStringLiteral("[Sampler1]"), QString::fromLatin1(item))));
    ControlObject::set(ConfigKey("[Sampler1]", "track_loaded"), 1);
    int triggers = 0;
    ControlProxy stutter(QStringLiteral("[Sampler1]"), QStringLiteral("play_stutter"));
    ASSERT_TRUE(stutter.connectValueChanged(panel.get(), [&triggers](double v) { if (v > 0) ++triggers; }, Qt::DirectConnection));
    WDeckLab surface(config(), nullptr, players.get(), m_pEffectsManager.get());
    surface.resize(800, 480); surface.show(); surface.selectPage(2); QCoreApplication::processEvents();
    auto* loaded = surface.findChild<QPushButton*>("decklab-sample-1"); ASSERT_NE(nullptr, loaded);
    QTest::mousePress(loaded, Qt::LeftButton);
    EXPECT_EQ(1, triggers) << "a loaded pad plays on touch-down";
    QTest::mouseRelease(loaded, Qt::LeftButton);
    EXPECT_EQ(1, triggers) << "the release does not trigger again";
    EXPECT_EQ(2, surface.currentPage());
    surface.hide();
}

TEST_F(NativeDeckLabTest, FourDeckBpmRoundsInsteadOfCutting) {
    EXPECT_EQ(QStringLiteral("128"), DeckLabStripRenderer::roundedBpm(QStringLiteral("127.96")));
    EXPECT_EQ(QStringLiteral("128"), DeckLabStripRenderer::roundedBpm(QStringLiteral("128.04")));
    EXPECT_EQ(QStringLiteral("100"), DeckLabStripRenderer::roundedBpm(QStringLiteral("99.70")));
    EXPECT_EQ(QStringLiteral("—"), DeckLabStripRenderer::roundedBpm(QStringLiteral("—")));
    // The whole BPM is still shown and never overflows.
    // A ten-minute clock leaves no room for decimals in a compact card.
    for (int deck = 1; deck <= 4; ++deck) loadGenerated(deck, 127.96);
    ProcessBuffer();
    for (int deck = 1; deck <= 4; ++deck) ControlObject::set(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), "duration"), 600);
    QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"), "activated", Q_ARG(int, 2)); QTest::qWait(150);
    for (int deck = 1; deck <= 4; ++deck) {
        EXPECT_EQ(DeckLabStripRenderer::Mode::Compact, strip(deck)->mode());
        EXPECT_TRUE(strip(deck)->renderer().overflowingParts().isEmpty())
                << deck << " " << strip(deck)->renderer().overflowingParts().join(',').toStdString();
    }
    capture(QStringLiteral("decks-four-rounded-bpm.png"));
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

namespace {
void pulseControl(const char* group, const char* item, double value = 1) {
    ControlObject::set(ConfigKey(group, item), value);
    if (value > 0) ControlObject::set(ConfigKey(group, item), 0);
    QCoreApplication::processEvents();
}
std::vector<double> deckSnapshot() {
    std::vector<double> values;
    for (int deck = 1; deck <= 4; ++deck) {
        for (const char* item : {"play", "rate", "quantize", "keylock", "pregain", "beatloop_size", "track_loaded"}) {
            values.push_back(ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), item)));
        }
    }
    return values;
}
} // namespace

// Generic mappings (FLX4, Inpulse and others) send MoveVertical, MoveFocus*,
// GoToItem, MoveLeft/Right and SelectTrackKnob. All must browse natively and
// none may become a key press on a deck, tempo or settings control.
TEST_F(NativeDeckLabTest, GenericControllerBrowseOpensBrowseWithoutTouchingDecks) {
    auto root = TreeItem::newRoot(source.get());
    auto* genres = root->appendChild("Genres"); genres->appendChild("House"); genres->appendChild("Techno");
    root->appendChild("Moods");
    source->model.setRootItem(std::move(root));
    m_pMixerDeck1->loadFakeTrack(false, 128); ProcessBuffer(); QCoreApplication::processEvents();
    const auto before = deckSnapshot();
    ASSERT_EQ(0, panel->currentPage());
    // An encoder turn on the Decks page opens Browse where it was left: the
    // source list.
    pulseControl("[Library]", "MoveVertical", -1);
    EXPECT_EQ(1, panel->currentPage());
    auto* home = browseList(); ASSERT_NE(nullptr, home);
    EXPECT_TRUE(home->isVisible());
    EXPECT_EQ(0, home->currentRow());
    EXPECT_EQ(0, source->activations);
    pulseControl("[Library]", "MoveVertical", 1);
    EXPECT_EQ(1, home->currentRow());
    pulseControl("[Library]", "MoveVertical", -1);
    EXPECT_EQ(0, home->currentRow());
    // The browse-knob press of those mappings (MoveFocusForward) enters.
    pulseControl("[Library]", "MoveFocusForward");
    EXPECT_EQ(1, source->activations);
    ASSERT_TRUE(sidebar->rootIndex().isValid());
    EXPECT_EQ(QStringLiteral("Genres"), sidebar->currentIndex().data().toString());
    // Turning highlights without opening anything.
    pulseControl("[Playlist]", "SelectTrackKnob", 1);
    EXPECT_EQ(QStringLiteral("Moods"), sidebar->currentIndex().data().toString());
    pulseControl("[Library]", "MoveUp");
    EXPECT_EQ(QStringLiteral("Genres"), sidebar->currentIndex().data().toString());
    EXPECT_EQ(1, source->activations);
    pulseControl("[Library]", "GoToItem");
    EXPECT_EQ(2, source->activations);
    EXPECT_EQ(QStringLiteral("Genres"), sidebar->rootIndex().data().toString());
    // MoveRight on a leaf playlist shows its tracks.
    pulseControl("[Library]", "MoveRight");
    EXPECT_EQ(3, source->activations);
    EXPECT_TRUE(library->isVisible());
    // In a track list the press previews and Right does nothing: the list stays.
    pulseControl("[Library]", "MoveFocusForward");
    pulseControl("[Library]", "MoveRight");
    EXPECT_TRUE(library->isVisible());
    EXPECT_EQ(3, source->activations);
    // MoveFocusBackward and MoveLeft go back one level each.
    pulseControl("[Library]", "MoveFocusBackward");
    EXPECT_TRUE(sidebar->isVisible());
    EXPECT_EQ(QStringLiteral("Genres"), sidebar->rootIndex().data().toString());
    pulseControl("[Library]", "MoveLeft");
    EXPECT_EQ(QStringLiteral("Genres"), sidebar->currentIndex().data().toString());
    EXPECT_EQ(QStringLiteral("USB library"), sidebar->rootIndex().data().toString());
    // A focused settings combo box never receives the turn.
    panel->selectPage(4);
    auto* range = panel->findChild<QComboBox*>("decklab-tempo-range-1"); ASSERT_NE(nullptr, range);
    const int rangeIndex = range->currentIndex();
    range->setFocus(); QCoreApplication::processEvents();
    pulseControl("[Playlist]", "SelectTrackKnob", 1);
    EXPECT_EQ(1, panel->currentPage());
    EXPECT_EQ(rangeIndex, range->currentIndex());
    EXPECT_EQ(QStringLiteral("Moods"), sidebar->currentIndex().data().toString());
    panel->selectPage(0);
    pulseControl("[Library]", "ScrollVertical", 1);
    EXPECT_EQ(1, panel->currentPage());
    QTest::qWait(50);
    EXPECT_EQ(before, deckSnapshot());
}

TEST_F(NativeDeckLabTest, PlayingDeckLoadShowsWhyAndKeepsTheDeck) {
    m_pMixerDeck2->loadFakeTrack(false, 128); ProcessBuffer();
    ControlObject::set(ConfigKey("[Channel2]", "play"), 1); ProcessBuffer(); QCoreApplication::processEvents();
    panel->openCollection(); QCoreApplication::processEvents();
    auto* load = panel->findChild<QPushButton*>("decklab-load-2"); ASSERT_NE(nullptr, load);
    auto* target = static_cast<DeckLabActionButton*>(load);
    EXPECT_FALSE(load->isEnabled());
    EXPECT_EQ(DeckLabActionButton::Tone::OnAir, target->tone());
    EXPECT_TRUE(target->detail().contains(QStringLiteral("air"), Qt::CaseInsensitive));
    auto* toast = panel->findChild<QLabel*>("decklab-toast"); ASSERT_NE(nullptr, toast);
    EXPECT_FALSE(toast->isVisible());
    // Tapping the ON AIR target explains instead of silently refusing.
    QTest::mouseClick(load, Qt::LeftButton);
    EXPECT_TRUE(toast->isVisible());
    EXPECT_EQ(QStringLiteral("Deck 2 is playing – pause it to load"), toast->text());
    const QString output = qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
    if (!output.isEmpty()) EXPECT_TRUE(panel->grab().save(QDir(output).filePath("browse-on-air-feedback.png")));
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel2]", "play")));
    EXPECT_TRUE(m_pMixerDeck2->getEngineDeck()->getEngineBuffer()->isTrackLoaded());
    // Controller LOAD: the track table reports Mixxx's own refusal, on any page.
    panel->selectPage(0); toast->hide();
    ControlObject::set(ConfigKey("[DeckLabNative]", "load_refused"), 2);
    ControlObject::set(ConfigKey("[DeckLabNative]", "load_refused"), 0);
    QCoreApplication::processEvents();
    EXPECT_TRUE(toast->isVisible());
    EXPECT_TRUE(toast->text().startsWith(QStringLiteral("Deck 2 is playing")));
    EXPECT_TRUE(panel->rect().contains(QRect(toast->mapTo(panel.get(), QPoint()), toast->size())));
    // A paused deck stays a normal (here: no selection yet) LOAD target.
    ControlObject::set(ConfigKey("[Channel2]", "play"), 0); ProcessBuffer(); QCoreApplication::processEvents();
    EXPECT_NE(DeckLabActionButton::Tone::OnAir, target->tone());
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel2]", "play")));
}

TEST_F(NativeDeckLabTest, CollectionOpensCategoriesAndBackReturnsToSources) {
    panel->selectPage(1); QCoreApplication::processEvents();
    const QPersistentModelIndex collectionRoot = model->index(1, 0);
    ASSERT_EQ(QStringLiteral("tracks"), collectionRoot.data(SidebarModel::IconNameRole).toString());
    // Mixxx's collection is "Library" on the source list.
    ASSERT_TRUE(selectRow(QStringLiteral("Library"))); QCoreApplication::processEvents();
    auto* card = static_cast<DeckLabTrackCard*>(panel->findChild<QWidget*>("decklab-track-card"));
    ASSERT_NE(nullptr, card);
    EXPECT_EQ(QStringLiteral("Library"), card->title());
    panel->browsePress(); QCoreApplication::processEvents();
    auto* list = panel->findChild<QListWidget*>("decklab-browse-categories"); ASSERT_NE(nullptr, list);
    ASSERT_TRUE(list->isVisible());
    EXPECT_FALSE(sidebar->isVisible());
    QStringList names;
    for (int row = 0; row < list->count(); ++row) names << list->item(row)->text();
    EXPECT_EQ((QStringList{"All Tracks", "Artist", "Album", "BPM", "Key"}), names);
    EXPECT_EQ(1, collection->activations);
    auto* crumb = static_cast<DeckLabBreadcrumb*>(panel->findChild<QWidget*>("decklab-breadcrumb"));
    ASSERT_NE(nullptr, crumb);
    EXPECT_EQ(QStringList{QStringLiteral("Library")}, crumb->segments());
    // A knob turn highlights a category without opening it.
    ControlObject::set(ConfigKey("[Playlist]", "SelectPlaylist"), 2); QCoreApplication::processEvents();
    EXPECT_EQ(2, list->currentRow());
    EXPECT_TRUE(list->isVisible());
    EXPECT_EQ(QStringLiteral("Album"), card->title());
    EXPECT_EQ(1, collection->activations);
    const QString output = qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
    if (!output.isEmpty()) EXPECT_TRUE(panel->grab().save(QDir(output).filePath("browse-categories-fixture.png")));
    panel->browseBack(); QCoreApplication::processEvents();
    // Back to the source list, on the source just left.
    EXPECT_TRUE(list->isVisible());
    EXPECT_FALSE(sidebar->isVisible());
    ASSERT_NE(nullptr, list->currentItem());
    EXPECT_EQ(QStringLiteral("Library"), list->currentItem()->text());
    EXPECT_EQ(QStringList{QStringLiteral("Sources")}, crumb->segments());
}

// 800x480 is the design target; 1024x600 and 1280x800 touch screens must not break.
TEST_F(NativeDeckLabTest, BrowseScalesCleanlyTo1024x600And1280x800) {
    panel->openCollection(); QCoreApplication::processEvents();
    const QString output = qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
    for (const QSize& size : {QSize(1024, 600), QSize(1280, 800), QSize(800, 480)}) {
        panel->resize(size); QTest::qWait(50);
        ASSERT_EQ(size, panel->size());
        auto* browse = panel->findChild<QWidget*>("decklab-browse"); ASSERT_NE(nullptr, browse);
        auto* pane = panel->findChild<QWidget*>("decklab-browse-pane"); ASSERT_NE(nullptr, pane);
        const QRect paneRect(pane->mapTo(panel.get(), QPoint()), pane->size());
        EXPECT_TRUE(panel->rect().contains(paneRect)) << size.width();
        EXPECT_GE(pane->width(), 236); EXPECT_LE(pane->width(), 340);
        const QRect tracksRect(library->mapTo(panel.get(), QPoint()), library->size());
        EXPECT_TRUE(library->isVisible());
        EXPECT_LE(tracksRect.right(), paneRect.left()) << size.width();
        EXPECT_GE(tracksRect.width(), 500) << size.width();
        for (auto* button : browse->findChildren<QPushButton*>()) {
            if (!button->isVisible()) continue;
            const QRect bounds(button->mapTo(panel.get(), QPoint()), button->size());
            EXPECT_TRUE(panel->rect().contains(bounds)) << button->objectName().toStdString() << " " << size.width();
            EXPECT_GE(button->height(), 44) << button->objectName().toStdString();
        }
        auto* card = panel->findChild<QWidget*>("decklab-track-card");
        EXPECT_TRUE(card->isVisible()); EXPECT_GE(card->height(), 96);
        if (!output.isEmpty()) {
            EXPECT_TRUE(panel->grab().save(QDir(output).filePath(
                    QStringLiteral("browse-%1x%2.png").arg(size.width()).arg(size.height()))));
        }
    }
}

// qw-library: C-01. A menu or dialog that opened and closed must never leave
// the browse knob dead. The kiosk has no window manager, so no focus signal
// may follow the close; the knob must work without a tap.
TEST_F(NativeDeckLabTest, BrowseKnobKeepsWorkingAfterMenusAndDialogsCloseWithoutATap) {
    panel->selectPage(1); QCoreApplication::processEvents();
    auto* list = browseList(); ASSERT_NE(nullptr, list);
    ASSERT_TRUE(list->isVisible());
    ASSERT_GE(list->count(), 3);
    list->setCurrentRow(0);
    const ConfigKey knob("[Library]", "MoveVertical");
    const ConfigKey focus("[Library]", "focused_widget");
    auto* tools = panel->findChild<QPushButton*>("decklab-browse-tools"); ASSERT_NE(nullptr, tools);
    // 1. A menu really is open: the knob belongs to it and Browse stays put.
    tools->click(); QCoreApplication::processEvents();
    QPointer<QMenu> menu = panel->findChild<QMenu*>("decklab-browse-tools-menu");
    ASSERT_TRUE(menu); ASSERT_TRUE(menu->isVisible());
    const int redirected = decklab::knobEventsRedirected();
    ControlObject::set(knob, 1); QCoreApplication::processEvents();
    EXPECT_EQ(0, list->currentRow());
    EXPECT_GT(decklab::knobEventsRedirected(), redirected);
    EXPECT_EQ(static_cast<double>(FocusWidget::ContextMenu), ControlObject::get(focus));
    // 2. It closes and nothing re-activates the window.
    {
        const QSignalBlocker noFocusSignals(qApp);
        menu->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    ASSERT_FALSE(menu);
    // 3. The next turn moves Browse. Scripts that read focused_widget first
    // (the Numark overlay skips 4 and up) see the knob free again.
    ControlObject::set(knob, 1); QCoreApplication::processEvents();
    EXPECT_EQ(1, list->currentRow());
    EXPECT_LT(ControlObject::get(focus), 4);
    // The Numark overlay's turn control takes the same route.
    ControlObject::set(ConfigKey("[Playlist]", "SelectPlaylist"), 1); QCoreApplication::processEvents();
    EXPECT_EQ(2, list->currentRow());
    ControlObject::set(ConfigKey("[Playlist]", "SelectPlaylist"), -2); QCoreApplication::processEvents();
    EXPECT_EQ(0, list->currentRow());
    // 4. A modal dialog and a message box follow the same rule.
    for (int kind = 0; kind < 2; ++kind) {
        QPointer<QDialog> dialog = kind == 0 ? new QDialog(panel.get())
                                             : new QMessageBox(QMessageBox::Information, QStringLiteral("Status"),
                                                       QStringLiteral("Generated message"), QMessageBox::Ok, panel.get());
        dialog->setWindowModality(Qt::ApplicationModal);
        dialog->show(); QCoreApplication::processEvents();
        ASSERT_EQ(dialog.data(), QApplication::activeModalWidget()) << kind;
        const int row = list->currentRow();
        const int activations = source->activations;
        ControlObject::set(knob, 1); QCoreApplication::processEvents();
        EXPECT_EQ(row, list->currentRow()) << kind;
        EXPECT_EQ(static_cast<double>(FocusWidget::Dialog), ControlObject::get(focus)) << kind;
        // A script's direct press goes to the dialog, never to Browse behind it.
        ControlObject::set(ConfigKey("[DeckLabNative]", "browse_press"), 1);
        ControlObject::set(ConfigKey("[DeckLabNative]", "browse_press"), 0);
        QCoreApplication::processEvents();
        EXPECT_EQ(activations, source->activations) << kind;
        EXPECT_EQ(1, panel->currentPage()) << kind;
        {
            const QSignalBlocker noFocusSignals(qApp);
            if (dialog) dialog->hide();
            delete dialog.data();
        }
        ControlObject::set(knob, 1); QCoreApplication::processEvents();
        EXPECT_EQ(row + 1, list->currentRow()) << kind;
        EXPECT_LT(ControlObject::get(focus), 4) << kind;
        ControlObject::set(knob, -1); QCoreApplication::processEvents();
    }
    // 5. The press opens the highlighted source again.
    list->setCurrentRow(0);
    ControlObject::set(ConfigKey("[DeckLabNative]", "browse_press"), 1);
    ControlObject::set(ConfigKey("[DeckLabNative]", "browse_press"), 0);
    QCoreApplication::processEvents();
    EXPECT_EQ(1, source->activations);
    EXPECT_TRUE(sidebar->rootIndex().isValid());
}

// qw-library: B-35 and B-36. Search stays in the source you are in and the
// breadcrumb says so.
TEST_F(NativeDeckLabTest, SearchStaysInTheSourceYouAreIn) {
    panel->selectPage(1); QCoreApplication::processEvents();
    ASSERT_TRUE(selectRow(QStringLiteral("Library")));
    panel->browsePress(); QCoreApplication::processEvents();
    ASSERT_EQ(QStringList{QStringLiteral("Library")}, crumbs());
    const int before = collection->activations;
    panel->findChild<QPushButton*>("decklab-search")->click(); QCoreApplication::processEvents();
    EXPECT_TRUE(library->isVisible());
    EXPECT_EQ(before + 1, collection->activations); // its own tracks, not a reset
    EXPECT_EQ(QStringLiteral("Library"), crumbs().value(0));
    auto* keyboard = panel->findChild<QWidget*>("decklab-keyboard"); ASSERT_NE(nullptr, keyboard);
    ASSERT_TRUE(keyboard->isVisible());
    for (auto* b : keyboard->findChildren<QPushButton*>()) if (b->text() == "Q") { b->click(); break; }
    QTest::qWait(800);
    ASSERT_GE(crumbs().size(), 2);
    EXPECT_EQ(QStringLiteral("Library"), crumbs().first());
    EXPECT_EQ(QString::fromUtf8("Search “q”"), crumbs().last());
    panel->findChild<QPushButton*>("decklab-search-done")->click();
    // On a stick, search its tracks.
    const QString stick = makeStick(QStringLiteral("3A1F-0C2D"), true);
    addRekordboxDevice(QStringLiteral("DJ USB"), stick, true);
    panel->browseBack(); panel->browseBack(); QCoreApplication::processEvents();
    ASSERT_EQ(QStringList{QStringLiteral("Sources")}, crumbs());
    ASSERT_TRUE(selectRow(QStringLiteral("USB stick (3A1F)"))) << listTexts().join(" / ").toStdString();
    panel->browsePress(); QCoreApplication::processEvents();
    ASSERT_EQ(QStringList{QStringLiteral("DJ USB")}, crumbs());
    const int stickBefore = rekordbox->activations;
    panel->findChild<QPushButton*>("decklab-search")->click(); QCoreApplication::processEvents();
    EXPECT_TRUE(library->isVisible());
    EXPECT_EQ(stickBefore + 1, rekordbox->activations);
    EXPECT_EQ(QStringLiteral("DJ USB"), crumbs().value(0));
    panel->findChild<QPushButton*>("decklab-search-done")->click();
}

// qw-library: B-01, B-03, B-05, U-03. The first screen says what is inside:
// sticks first by label, "Library", two-line rows, desktop roots under More.
TEST_F(NativeDeckLabTest, SourceListPutsSticksFirstAndSaysWhatIsInside) {
    const QString plain = makeStick(QStringLiteral("55AA-1234"), false);
    ASSERT_TRUE(QDir().mkpath(plain + QStringLiteral("/House")));
    ASSERT_TRUE(QDir().mkpath(plain + QStringLiteral("/System Volume Information")));
    { QFile track(plain + QStringLiteral("/Generated Track.mp3")); ASSERT_TRUE(track.open(QIODevice::WriteOnly)); }
    makeStick(QStringLiteral("3A1F-0C2D"), true);
    panel->selectPage(1); QCoreApplication::processEvents();
    auto* list = browseList(); ASSERT_NE(nullptr, list); ASSERT_TRUE(list->isVisible());
    EXPECT_EQ((QStringList{"USB stick (3A1F)", "USB stick (55AA)", "USB library", "Library", "More sources"}), listTexts());
    const auto subtitle = [list](int row) {
        return list->item(row)->data(DeckLabListDelegate::SubtitleRole).toString();
    };
    EXPECT_EQ(QStringLiteral("rekordbox · tap to read"), subtitle(0));
    EXPECT_EQ(QStringLiteral("Files · 1 folder · 1 track"), subtitle(1));
    EXPECT_EQ(QStringLiteral("1 item"), subtitle(2));
    EXPECT_FALSE(subtitle(3).isEmpty());
    EXPECT_EQ(QStringLiteral("Auto DJ · All USB exports · Computer"), subtitle(4));
    // One glyph per concept.
    EXPECT_EQ(static_cast<int>(decklab::Glyph::Usb), list->item(0)->data(DeckLabListDelegate::GlyphRole).toInt());
    EXPECT_EQ(static_cast<int>(decklab::Glyph::Collection), list->item(3)->data(DeckLabListDelegate::GlyphRole).toInt());
    EXPECT_EQ(static_cast<int>(decklab::Glyph::Sources), list->item(4)->data(DeckLabListDelegate::GlyphRole).toInt());
    // Two-line rows, and 16/15 px type at 800x480.
    EXPECT_GE(list->visualItemRect(list->item(0)).height(), 56);
    capture(QStringLiteral("browse-source-home.png"));
    // A plain stick opens its folder view: system folders hidden.
    list->setCurrentRow(1);
    panel->browsePress(); QCoreApplication::processEvents();
    EXPECT_EQ((QStringList{"Tracks in this folder", "House"}), listTexts());
    EXPECT_EQ(QStringList{QStringLiteral("USB stick (55AA)")}, crumbs());
    ASSERT_TRUE(selectRow(QStringLiteral("House")));
    panel->browsePress(); QCoreApplication::processEvents();
    EXPECT_EQ((QStringList{"USB stick (55AA)", "House"}), crumbs());
    EXPECT_EQ(QStringList{QStringLiteral("No music here")}, listTexts());
    panel->browseBack(); QCoreApplication::processEvents();
    // Its tracks come from Mixxx's file browser for that folder.
    mirrorFolder(plain);
    ASSERT_TRUE(selectRow(QStringLiteral("Tracks in this folder")));
    panel->browsePress(); QCoreApplication::processEvents();
    EXPECT_EQ(1, computer->activations);
    EXPECT_TRUE(library->isVisible());
    EXPECT_EQ((QStringList{"USB stick (55AA)", "Tracks"}), crumbs());
    panel->browseBack(); panel->browseBack(); QCoreApplication::processEvents();
    ASSERT_NE(nullptr, list->currentItem());
    EXPECT_EQ(QStringLiteral("USB stick (55AA)"), list->currentItem()->text());
    // "More sources" keeps the desktop roots reachable.
    ASSERT_TRUE(selectRow(QStringLiteral("More sources")));
    panel->browsePress(); QCoreApplication::processEvents();
    EXPECT_TRUE(sidebar->isVisible());
    EXPECT_EQ(5, model->rowCount(sidebar->rootIndex()));
    EXPECT_EQ((QStringList{"Sources", "More sources"}), crumbs());
    panel->browseBack(); QCoreApplication::processEvents();
    EXPECT_TRUE(list->isVisible());
    EXPECT_EQ(QStringLiteral("More sources"), list->currentItem()->text());
    // The rail's USB opens the stick (U-01): with two, the list on the first.
    auto* usb = panel->findChild<QPushButton*>("decklab-usb"); ASSERT_NE(nullptr, usb);
    EXPECT_TRUE(usb->isEnabled());
    panel->selectPage(0); usb->click(); QCoreApplication::processEvents();
    EXPECT_EQ(1, panel->currentPage());
    EXPECT_EQ(QStringLiteral("USB stick (3A1F)"), list->currentItem()->text());
    // With one stick, straight into it.
    QDir(media.filePath(QStringLiteral("decklab/3A1F-0C2D"))).removeRecursively();
    panel->selectPage(0); usb->click(); QCoreApplication::processEvents();
    EXPECT_EQ(1, panel->currentPage());
    EXPECT_EQ(QStringList{QStringLiteral("USB stick (55AA)")}, crumbs());
    EXPECT_EQ(QStringLiteral("Tracks in this folder"), list->item(0)->text());
}

// qw-library: B-10. An export that never finishes reading ends in an error
// with a way out instead of an eternal "Reading…".
TEST_F(NativeDeckLabTest, UnreadableRekordboxStickStopsReadingAndOffersItsFiles) {
    config()->setValue(ConfigKey("[DeckLab]", "RekordboxReadTimeoutMs"), 300);
    const QString stick = makeStick(QStringLiteral("3A1F-0C2D"), true);
    ASSERT_TRUE(QDir().mkpath(stick + QStringLiteral("/Contents")));
    addRekordboxDevice(QStringLiteral("DJ USB"), stick, false);
    panel->selectPage(1); QCoreApplication::processEvents();
    ASSERT_TRUE(selectRow(QStringLiteral("USB stick (3A1F)"))) << listTexts().join(" / ").toStdString();
    panel->browsePress(); QCoreApplication::processEvents();
    EXPECT_EQ(1, rekordbox->activations); // the read started
    EXPECT_EQ(QStringList{QStringLiteral("DJ USB")}, crumbs());
    auto* list = browseList();
    ASSERT_GE(list->count(), 1);
    EXPECT_EQ(QStringLiteral("Reading…"), list->item(0)->data(DeckLabListDelegate::CountRole).toString());
    QTest::qWait(900);
    EXPECT_EQ((QStringList{"Couldn't read this stick's rekordbox export", "Try again", "Browse its files"}), listTexts());
    EXPECT_EQ(1, list->currentRow());
    auto* card = static_cast<DeckLabTrackCard*>(panel->findChild<QWidget*>("decklab-track-card"));
    EXPECT_EQ(QStringLiteral("DJ USB"), card->title());
    capture(QStringLiteral("browse-usb-read-error.png"));
    // Try again reads it again.
    panel->browsePress(); QCoreApplication::processEvents();
    EXPECT_EQ(2, rekordbox->activations);
    EXPECT_EQ(QStringLiteral("Reading…"), list->item(0)->data(DeckLabListDelegate::CountRole).toString());
    QTest::qWait(900);
    ASSERT_TRUE(selectRow(QStringLiteral("Browse its files")));
    panel->browsePress(); QCoreApplication::processEvents();
    EXPECT_EQ(QStringList{QStringLiteral("Contents")}, listTexts());
    EXPECT_EQ(QStringList{QStringLiteral("DJ USB")}, crumbs());
    panel->browseBack(); QCoreApplication::processEvents();
    EXPECT_EQ(QStringList{QStringLiteral("Sources")}, crumbs());
    ASSERT_TRUE(selectRow(QStringLiteral("USB stick (3A1F)")));
    EXPECT_EQ(QStringLiteral("rekordbox · couldn't read the export"),
            list->currentItem()->data(DeckLabListDelegate::SubtitleRole).toString());
}

// qw-library: B-49..B-53. The Match bar shows the reference deck in its
// colour, the BPM window and direction, the key rule, Only and a clear.
TEST_F(NativeDeckLabTest, MatchBarShowsDeckWindowDirectionAndClears) {
    using Tone = DeckLabActionButton::Tone;
    panel->openCollection(); QCoreApplication::processEvents();
    auto* bar = panel->findChild<QWidget*>("decklab-match-bar"); ASSERT_NE(nullptr, bar);
    auto* summary = panel->findChild<DeckLabTwoLineLabel*>("decklab-match-summary"); ASSERT_NE(nullptr, summary);
    auto* chip = panel->findChild<DeckLabDeckChip*>("decklab-match-deck"); ASSERT_NE(nullptr, chip);
    auto* match = static_cast<DeckLabActionButton*>(panel->findChild<QPushButton*>("decklab-browse-match"));
    auto* only = static_cast<DeckLabActionButton*>(panel->findChild<QPushButton*>("decklab-match-only"));
    auto* faster = static_cast<DeckLabActionButton*>(panel->findChild<QPushButton*>("decklab-match-faster"));
    auto* both = static_cast<DeckLabActionButton*>(panel->findChild<QPushButton*>("decklab-match-both"));
    ASSERT_TRUE(match && only && faster && both);
    ASSERT_TRUE(bar->isVisible());
    // No deck loaded: neutral, never the match green (B-50).
    EXPECT_EQ(QStringLiteral("Load a deck to match"), summary->primary());
    EXPECT_FALSE(summary->active());
    EXPECT_EQ(0, chip->deck());
    EXPECT_EQ(Tone::Neutral, match->tone());
    EXPECT_EQ(QStringLiteral("Load a deck"), match->detail());
    EXPECT_FALSE(only->isEnabled());
    // A paused reference: deck 1 at 122 BPM in 8A.
    panel->selectPage(0);
    loadGenerated(1, 122, mixxx::track::io::key::A_MINOR); ProcessBuffer(); QTest::qWait(250);
    panel->openCollection(); QTest::qWait(250);
    ASSERT_TRUE(bar->isVisible());
    EXPECT_EQ(1, chip->deck());
    EXPECT_TRUE(chip->automatic());
    EXPECT_TRUE(chip->detail().contains(QLocale().toString(122.0, 'f', 1))) << chip->detail().toStdString();
    EXPECT_EQ(QString::fromUtf8("115–129 BPM"), summary->primary());
    EXPECT_TRUE(summary->active());
    EXPECT_EQ(Tone::Active, match->tone());
    EXPECT_EQ(Tone::Selected, both->tone());
    // Faster only: the window starts at the reference.
    faster->click(); QCoreApplication::processEvents();
    EXPECT_EQ(QString::fromUtf8("122–129 BPM"), summary->primary());
    EXPECT_EQ(Tone::Selected, faster->tone());
    EXPECT_EQ(Tone::Neutral, both->tone());
    EXPECT_EQ(decklab::TempoDirection::Faster, decklab::matchReference().direction);
    EXPECT_TRUE(decklab::tempoInWindow(124, decklab::matchReference().window()));
    EXPECT_FALSE(decklab::tempoInWindow(120, decklab::matchReference().window()));
    // Pin the deck: a tap leaves Auto.
    chip->click(); QTest::qWait(50);
    EXPECT_FALSE(chip->automatic());
    chip->click(); QTest::qWait(50);
    EXPECT_TRUE(chip->automatic());
    // Only: a visible filter on this list (B-42).
    only->click(); QCoreApplication::processEvents();
    EXPECT_EQ(Tone::Selected, only->tone());
    ASSERT_FALSE(crumbs().isEmpty());
    EXPECT_TRUE(crumbs().last().startsWith(QStringLiteral("Matches deck 1"))) << crumbs().join(" / ").toStdString();
    capture(QStringLiteral("browse-match-bar.png"));
    // The clear turns matching off: no bar, no filter, neutral header.
    panel->findChild<QPushButton*>("decklab-match-clear")->click(); QCoreApplication::processEvents();
    EXPECT_FALSE(bar->isVisible());
    EXPECT_EQ(QStringLiteral("Off"), match->detail());
    EXPECT_FALSE(crumbs().last().startsWith(QStringLiteral("Matches")));
    match->click(); QTest::qWait(50);
    EXPECT_TRUE(bar->isVisible());
    EXPECT_EQ(Tone::Neutral, only->tone());
    both->click(); QCoreApplication::processEvents();
}

// qw-library: B-29, B-30. LOAD targets carry the deck's identity and say
// what they would replace.
TEST_F(NativeDeckLabTest, LoadTargetsCarryDeckIdentityAndWhatTheyReplace) {
    loadGenerated(2, 128); ProcessBuffer(); QCoreApplication::processEvents();
    panel->openCollection(); QCoreApplication::processEvents();
    auto* load1 = static_cast<DeckLabActionButton*>(panel->findChild<QPushButton*>("decklab-load-1"));
    auto* load2 = static_cast<DeckLabActionButton*>(panel->findChild<QPushButton*>("decklab-load-2"));
    ASSERT_TRUE(load1 && load2);
    EXPECT_EQ(1, load1->deck());
    EXPECT_EQ(2, load2->deck());
    EXPECT_EQ(QStringLiteral("Empty"), load1->detail());
    EXPECT_EQ(QStringLiteral("Replaces Generated Tone 128"), load2->detail());
    // The deck colour bar shows even while the target is disabled.
    const QImage image = load1->grab().toImage();
    const QColor bar = image.pixelColor(3, image.height() / 2);
    const QColor cyan = deckLabDeckColor(1);
    EXPECT_LT(std::abs(bar.hue() - cyan.hue()), 12) << bar.name().toStdString();
    ControlObject::set(ConfigKey("[Channel2]", "play"), 1); ProcessBuffer(); QCoreApplication::processEvents();
    EXPECT_EQ(QStringLiteral("ON AIR"), load2->detail());
    EXPECT_EQ(DeckLabActionButton::Tone::OnAir, load2->tone());
    ControlObject::set(ConfigKey("[Channel2]", "play"), 0); ProcessBuffer(); QCoreApplication::processEvents();
    auto* choice = panel->findChild<QComboBox*>("decklab-deck-layout");
    QMetaObject::invokeMethod(choice, "activated", Q_ARG(int, 2)); QCoreApplication::processEvents();
    auto* load4 = static_cast<DeckLabActionButton*>(panel->findChild<QPushButton*>("decklab-load-4"));
    ASSERT_TRUE(load4->isVisible());
    EXPECT_EQ(4, load4->deck());
    EXPECT_EQ(QStringLiteral("Empty"), load4->detail());
    capture(QStringLiteral("browse-load-identity.png"));
}

TEST(DeckLabBrowseMatchTest, TempoWindowDirectionDeltaAndKeyWords) {
    using decklab::TempoDirection;
    const auto both = decklab::tempoWindow(122, TempoDirection::Both);
    EXPECT_NEAR(114.68, both.low, 1e-9);
    EXPECT_NEAR(129.32, both.high, 1e-9);
    EXPECT_EQ(QString::fromUtf8("115–129"), decklab::tempoWindowText(both));
    EXPECT_EQ(QString::fromUtf8("115–122"), decklab::tempoWindowText(decklab::tempoWindow(122, TempoDirection::Slower)));
    EXPECT_EQ(QString::fromUtf8("122–129"), decklab::tempoWindowText(decklab::tempoWindow(122, TempoDirection::Faster)));
    EXPECT_TRUE(decklab::tempoInWindow(127.2, decklab::tempoWindow(120, TempoDirection::Both)));
    EXPECT_FALSE(decklab::tempoInWindow(127.4, decklab::tempoWindow(120, TempoDirection::Both)));
    EXPECT_FALSE(decklab::tempoInWindow(118, decklab::tempoWindow(120, TempoDirection::Faster)));
    EXPECT_TRUE(decklab::tempoInWindow(118, decklab::tempoWindow(120, TempoDirection::Slower)));
    EXPECT_FALSE(decklab::tempoWindow(0, TempoDirection::Both).isValid());
    EXPECT_EQ(QString::fromUtf8("▲2.0"), decklab::bpmDeltaText(124, 122));
    EXPECT_EQ(QString::fromUtf8("▼2.0"), decklab::bpmDeltaText(120, 122));
    EXPECT_EQ(QString::fromUtf8("±0"), decklab::bpmDeltaText(122.01, 122));
    EXPECT_EQ(QString::fromUtf8("▲18"), decklab::bpmDeltaText(140, 122));
    EXPECT_TRUE(decklab::bpmDeltaText(0, 122).isEmpty());
    using namespace mixxx::track::io::key;
    EXPECT_EQ(QStringLiteral("Same key"), decklab::keyRelationText(A_MINOR, A_MINOR));
    EXPECT_EQ(QString::fromUtf8("Relative 8A→8B"), decklab::keyRelationText(C_MAJOR, A_MINOR));
    EXPECT_EQ(QStringLiteral("Key +1 (up)"), decklab::keyRelationText(E_MINOR, A_MINOR));
    EXPECT_EQ(QString::fromUtf8("Key −1 (down)"), decklab::keyRelationText(D_MINOR, A_MINOR));
    EXPECT_TRUE(decklab::keyRelationText(B_MINOR, A_MINOR).isEmpty());
    EXPECT_EQ((QStringList{"8A", "8B", "7A", "9A"}), decklab::compatibleLancelotKeys(A_MINOR));
    EXPECT_EQ((QStringList{"1A", "1B", "12A", "2A"}), decklab::compatibleLancelotKeys(G_SHARP_MINOR));
}

TEST(DeckLabBrowseMatchTest, MatchFilterIsOneQueryForKeyAndTempo) {
    decklab::MatchReference reference;
    reference.deck = 1;
    reference.key = mixxx::track::io::key::A_MINOR;
    reference.bpm = 122;
    EXPECT_EQ(QStringLiteral("bpm:114.68-129.32 key:8A | bpm:114.68-129.32 key:8B | "
                             "bpm:114.68-129.32 key:7A | bpm:114.68-129.32 key:9A"),
            decklab::matchFilterQuery(reference));
    reference.direction = decklab::TempoDirection::Faster;
    EXPECT_TRUE(decklab::matchFilterQuery(reference).startsWith(QStringLiteral("bpm:122.00-129.32 key:8A")));
    // A category filter or typed words stay required in every alternative.
    const QString query = decklab::matchFilterQuery(reference, QStringLiteral("artist:=\"Demo Artist B\""));
    EXPECT_EQ(4, query.count(QStringLiteral("artist:=\"Demo Artist B\" bpm:")));
    reference.key = mixxx::track::io::key::INVALID;
    EXPECT_EQ(QStringLiteral("bpm:122.00-129.32"), decklab::matchFilterQuery(reference));
    reference.deck = 0;
    EXPECT_TRUE(decklab::matchFilterQuery(reference).isEmpty());
}

TEST(DeckLabBrowseTest, UsbSticksAreListedByLabelWithExportsMarked) {
    QTemporaryDir media;
    const QString root = media.filePath(QStringLiteral("decklab"));
    ASSERT_TRUE(QDir().mkpath(root + QStringLiteral("/55AA-1234/Music")));
    ASSERT_TRUE(QDir().mkpath(root + QStringLiteral("/55AA-1234/.Trashes")));
    ASSERT_TRUE(QDir().mkpath(root + QStringLiteral("/55AA-1234/PIONEER")));
    ASSERT_TRUE(QDir().mkpath(root + QStringLiteral("/3A1F-0C2D/PIONEER/rekordbox")));
    { QFile pdb(root + QStringLiteral("/3A1F-0C2D/PIONEER/rekordbox/export.pdb")); ASSERT_TRUE(pdb.open(QIODevice::WriteOnly)); }
    { QFile track(root + QStringLiteral("/55AA-1234/Generated.flac")); ASSERT_TRUE(track.open(QIODevice::WriteOnly)); }
    { QFile text(root + QStringLiteral("/55AA-1234/notes.txt")); ASSERT_TRUE(text.open(QIODevice::WriteOnly)); }
    decklab::setUsbStickRootsForTesting({root});
    const auto sticks = decklab::findUsbSticks();
    decklab::setUsbStickRootsForTesting({});
#if defined(Q_OS_LINUX)
    ASSERT_EQ(2, sticks.size());
    EXPECT_EQ(QStringLiteral("USB stick (3A1F)"), sticks[0].label);
    EXPECT_TRUE(sticks[0].rekordbox);
    EXPECT_EQ(QStringLiteral("USB stick (55AA)"), sticks[1].label);
    EXPECT_FALSE(sticks[1].rekordbox);
#endif
    EXPECT_EQ(QStringList{QStringLiteral("Music")}, decklab::stickSubfolders(root + QStringLiteral("/55AA-1234")));
    EXPECT_EQ(QStringList{QStringLiteral("Generated.flac")}, decklab::stickAudioFiles(root + QStringLiteral("/55AA-1234")));
}

TEST(DeckLabBrowseMatchTest, LancelotWheelHarmonicMatchTable) {
    using namespace mixxx::track::io::key;
    const std::vector<std::pair<ChromaticKey, QString>> wheel{
            {C_MAJOR, "8B"}, {D_FLAT_MAJOR, "3B"}, {D_MAJOR, "10B"}, {E_FLAT_MAJOR, "5B"},
            {E_MAJOR, "12B"}, {F_MAJOR, "7B"}, {F_SHARP_MAJOR, "2B"}, {G_MAJOR, "9B"},
            {A_FLAT_MAJOR, "4B"}, {A_MAJOR, "11B"}, {B_FLAT_MAJOR, "6B"}, {B_MAJOR, "1B"},
            {C_MINOR, "5A"}, {C_SHARP_MINOR, "12A"}, {D_MINOR, "7A"}, {E_FLAT_MINOR, "2A"},
            {E_MINOR, "9A"}, {F_MINOR, "4A"}, {F_SHARP_MINOR, "11A"}, {G_MINOR, "6A"},
            {G_SHARP_MINOR, "1A"}, {A_MINOR, "8A"}, {B_FLAT_MINOR, "3A"}, {B_MINOR, "10A"}};
    for (const auto& [key, text] : wheel) {
        EXPECT_EQ(text, decklab::lancelotText(key)) << KeyUtils::keyDebugName(key).toStdString();
        // Mixxx's own Lancelot notation agrees with the wheel.
        EXPECT_EQ(text, KeyUtils::keyToString(key, KeyUtils::KeyNotation::Lancelot));
    }
    using decklab::HarmonicMatch;
    // 8A (A minor): same, relative 8B, neighbours 7A and 9A; nothing else.
    EXPECT_EQ(HarmonicMatch::Same, decklab::harmonicMatch(A_MINOR, A_MINOR));
    EXPECT_EQ(HarmonicMatch::Relative, decklab::harmonicMatch(C_MAJOR, A_MINOR));
    EXPECT_EQ(HarmonicMatch::Adjacent, decklab::harmonicMatch(D_MINOR, A_MINOR));
    EXPECT_EQ(HarmonicMatch::Adjacent, decklab::harmonicMatch(E_MINOR, A_MINOR));
    EXPECT_EQ(HarmonicMatch::None, decklab::harmonicMatch(B_MINOR, A_MINOR));   // 10A
    EXPECT_EQ(HarmonicMatch::None, decklab::harmonicMatch(F_MAJOR, A_MINOR));   // 7B diagonal
    EXPECT_EQ(HarmonicMatch::None, decklab::harmonicMatch(G_MAJOR, A_MINOR));   // 9B diagonal
    // The wheel wraps: 12A and 1A are neighbours.
    EXPECT_EQ(HarmonicMatch::Adjacent, decklab::harmonicMatch(G_SHARP_MINOR, C_SHARP_MINOR));
    EXPECT_EQ(HarmonicMatch::Adjacent, decklab::harmonicMatch(B_MAJOR, E_MAJOR));
    EXPECT_EQ(HarmonicMatch::None, decklab::harmonicMatch(INVALID, A_MINOR));
    EXPECT_EQ(HarmonicMatch::None, decklab::harmonicMatch(A_MINOR, INVALID));
    // Every key has exactly four highlighted partners, all of which Mixxx's
    // broader compatible-key search (~key:) also returns.
    for (int reference = 1; reference <= 24; ++reference) {
        const auto ref = static_cast<ChromaticKey>(reference);
        const auto broader = KeyUtils::getCompatibleKeys(ref);
        int matches = 0;
        for (int candidate = 1; candidate <= 24; ++candidate) {
            const auto key = static_cast<ChromaticKey>(candidate);
            if (decklab::harmonicMatch(key, ref) == HarmonicMatch::None) continue;
            ++matches;
            EXPECT_TRUE(broader.contains(key)) << reference << " " << candidate;
        }
        EXPECT_EQ(4, matches) << reference;
    }
    // Key text from any notation resolves to the same key.
    EXPECT_EQ(A_MINOR, decklab::keyFromText(QStringLiteral("8A")));
    EXPECT_EQ(A_MINOR, decklab::keyFromText(QStringLiteral("Am")));
    EXPECT_EQ(A_MINOR, decklab::keyFromText(QStringLiteral("1m")));
}

TEST(DeckLabBrowseMatchTest, TempoMatchesWithinSixPercentOnly) {
    EXPECT_TRUE(decklab::tempoMatches(124, 124));
    EXPECT_TRUE(decklab::tempoMatches(127.2, 120));
    EXPECT_TRUE(decklab::tempoMatches(112.8, 120));
    EXPECT_FALSE(decklab::tempoMatches(127.4, 120));
    EXPECT_FALSE(decklab::tempoMatches(112.6, 120));
    EXPECT_FALSE(decklab::tempoMatches(60, 120));   // half tempo is not highlighted
    EXPECT_FALSE(decklab::tempoMatches(240, 120));
    EXPECT_FALSE(decklab::tempoMatches(0, 120));
    EXPECT_FALSE(decklab::tempoMatches(124, 0));
    EXPECT_FALSE(decklab::tempoMatches(std::nan(""), 120));
    EXPECT_NEAR(2.5, decklab::tempoDifferencePercent(123, 120), 1e-9);
    // The reference only changes when it meaningfully differs (no repaint storms).
    decklab::MatchReference a; a.deck = 1; a.key = mixxx::track::io::key::A_MINOR; a.bpm = 124.001;
    decklab::MatchReference b = a; b.bpm = 124.003;
    EXPECT_TRUE(a.sameAs(b));
    b.bpm = 124.2;
    EXPECT_FALSE(a.sameAs(b));
}

TEST(DeckLabBrowseTest, BreadcrumbDropsMiddleSegmentsBeforeTheDeepestLevel) {
    const QStringList path{"Rekordbox", "USB DRIVE", "Folder", "Playlist 3"};
    const auto measure = [](const QString& text) { return static_cast<int>(text.size()); };
    const QString separator = QStringLiteral(" / ");
    // Fits: unchanged.
    EXPECT_EQ(path, decklab::elideBreadcrumb(path, 100, measure, separator));
    // Middle levels go first; the source and the deepest level stay.
    EXPECT_EQ((QStringList{"Rekordbox", decklab::ellipsis(), "Folder", "Playlist 3"}),
            decklab::elideBreadcrumb(path, 40, measure, separator));
    EXPECT_EQ((QStringList{"Rekordbox", decklab::ellipsis(), "Playlist 3"}),
            decklab::elideBreadcrumb(path, 30, measure, separator));
    EXPECT_EQ((QStringList{decklab::ellipsis(), "Playlist 3"}),
            decklab::elideBreadcrumb(path, 20, measure, separator));
    EXPECT_EQ(QStringList{"Playlist 3"}, decklab::elideBreadcrumb(path, 5, measure, separator));
    EXPECT_EQ(QStringList{"Sources"}, decklab::elideBreadcrumb({"Sources"}, 1, measure, separator));
    // The painted breadcrumb applies the same rule to its real width.
    DeckLabBreadcrumb crumb;
    crumb.resize(300, 44);
    crumb.setSegments({"Rekordbox", "USB DRIVE", "Playlists", "Folder 2", "Generated Playlist 12"});
    const QStringList shown = crumb.displayedSegments();
    ASSERT_GE(shown.size(), 2);
    EXPECT_EQ(QStringLiteral("Generated Playlist 12"), shown.last());
    EXPECT_TRUE(shown.contains(decklab::ellipsis()));
    EXPECT_EQ(crumb.segments().join(QStringLiteral(" › ")), crumb.toolTip());
}

TEST(DeckLabBrowseTest, CategoryValuesComeFromTheTrackModelWithNativeQueries) {
    QStandardItemModel model(4, 3);
    const std::array<QString, 4> artists{"Demo Artist B", "demo artist b", "Demo Artist A", ""};
    const std::array<double, 4> bpms{122.4, 121.6, 0, 128};
    const std::array<QString, 4> keys{"8A", "Am", "9B", ""};
    for (int row = 0; row < 4; ++row) {
        model.setData(model.index(row, 0), artists[row]);
        model.setData(model.index(row, 1), bpms[row], Qt::EditRole);
        model.setData(model.index(row, 2), keys[row]);
    }
    const auto artistValues = decklab::collectCategoryValues(model, 0, decklab::Category::Artist);
    ASSERT_EQ(3, artistValues.size());
    EXPECT_EQ(QStringLiteral("Demo Artist A"), artistValues[0].label);
    EXPECT_EQ(QStringLiteral("artist:=\"Demo Artist A\""), artistValues[0].query);
    EXPECT_EQ(2, artistValues[1].count);
    EXPECT_EQ(QStringLiteral("artist:\"\""), artistValues[2].query);
    const auto bpmValues = decklab::collectCategoryValues(model, 1, decklab::Category::Bpm);
    ASSERT_EQ(3, bpmValues.size());
    EXPECT_EQ(QStringLiteral("122"), bpmValues[0].label);
    EXPECT_EQ(2, bpmValues[0].count);
    EXPECT_EQ(QStringLiteral("bpm:121.5-122.49"), bpmValues[0].query);
    EXPECT_EQ(QStringLiteral("bpm:127.5-128.49"), bpmValues[1].query);
    EXPECT_EQ(QStringLiteral("bpm:\"\""), bpmValues[2].query);
    const auto keyValues = decklab::collectCategoryValues(model, 2, decklab::Category::Key);
    ASSERT_EQ(3, keyValues.size());
    EXPECT_EQ(QStringLiteral("key:8A"), keyValues[0].query);
    EXPECT_EQ(2, keyValues[0].count); // "8A" and "Am" are the same key
    EXPECT_TRUE(keyValues[0].accent.isValid());
    EXPECT_EQ(QStringLiteral("key:9B"), keyValues[1].query);
    EXPECT_EQ(QStringLiteral("key:\"\""), keyValues[2].query);
    // Quotes cannot be escaped by the parser; the longest quote-free part is used.
    EXPECT_EQ(QStringLiteral("album:\"Live at the Hall\""),
            decklab::exactTextQuery(QStringLiteral("album"), QStringLiteral("\"Live at the Hall\"")));
}

TEST(DeckLabTempoTest, JitterIsSmoothedButPitchChangesImmediately) {
    DeckLabTempoPresenter display;
    const double initial = display.update(true, 141.97, 1, 141.97, 0);
    for (int n = 1; n < 8; ++n) EXPECT_EQ(initial, display.update(true, n % 2 ? 141.93 : 141.97, 1, 141.93, n * 100));
    EXPECT_DOUBLE_EQ(144.81, display.update(true, 141.97, 1.02, 144.8, 800));
    EXPECT_EQ(130, display.update(true, 130, 1, 130, 900));
    EXPECT_EQ(0, display.update(false, 130, 1, 130, 1000));
}

namespace {
bool laidOut(QLayout* layout, const QWidget* widget) {
    if (!layout) return false;
    for (int index = 0; index < layout->count(); ++index) {
        QLayoutItem* item = layout->itemAt(index);
        if (item->widget() == widget || laidOut(item->layout(), widget)) return true;
    }
    return false;
}

// Counts the synthetic release that ends a touch scratch.
class ReleaseCounter final : public QObject {
  public:
    int releases = 0;
  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::MouseButtonRelease) ++releases;
        return QObject::eventFilter(watched, event);
    }
};
} // namespace

// The GL waveforms are native child windows: on a real display they cover
// any non-native widget drawn over them. The offscreen platform used here
// has no such stacking, so these tests check the structure that keeps every
// overlay visible (decklab/decklaboverlay.h).
class DeckLabOverlayTest : public NativeDeckLabTest {
  protected:
    void SetUp() override {
        NativeDeckLabTest::SetUp();
        panel->selectPage(0); settle();
    }
    QWidget* detail(int deck) const {
        return panel->findChild<QWidget*>(QStringLiteral("decklab-detail-%1").arg(deck));
    }
    // A native window in a deck's lane, parented exactly like the
    // QOpenGLWindow that WGLWidget embeds with createWindowContainer().
    QWindow* addWaveformWindow(int deck) {
        auto* window = new QWindow;
        auto* layout = new QVBoxLayout(detail(deck));
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(QWidget::createWindowContainer(window, detail(deck)));
        settle();
        return window;
    }
    // Every visible child of the surface that its layout does not place and
    // that crosses the waveform zone is an overlay over the waveform
    // windows. Each must hide them or be a native window stacked above them.
    int checkOverlays(const QWindow* waveformWindow) {
        auto* zone = panel->findChild<QWidget*>(QStringLiteral("decklab-wave-zone"));
        EXPECT_NE(nullptr, zone);
        if (!zone) return 0;
        const QRect zoneArea(zone->mapTo(panel.get(), QPoint()), zone->size());
        int overlays = 0;
        for (auto* child : panel->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
            if (child->isWindow() || !child->isVisible() || laidOut(panel->layout(), child)) continue;
            if (!QRect(child->mapTo(panel.get(), QPoint()), child->size()).intersects(zoneArea)) continue;
            ++overlays;
            const std::string name = child->objectName().toStdString() + " (" + child->metaObject()->className() + ")";
            switch (decklab::overlay::kind(child)) {
            case decklab::overlay::Kind::SuppressesWaveforms:
                EXPECT_TRUE(panel->waveformsSuppressed()) << name;
                EXPECT_FALSE(waveformWindow->isVisible()) << name;
                break;
            case decklab::overlay::Kind::AboveWaveforms:
                EXPECT_NE(WId(0), child->internalWinId()) << name;
                EXPECT_TRUE(child->testAttribute(Qt::WA_DontCreateNativeAncestors)) << name;
                if (child->windowHandle()) {
                    // X11 stacks siblings only: the same native parent as
                    // the waveform windows.
                    EXPECT_EQ(waveformWindow->parent(), child->windowHandle()->parent()) << name;
                    EXPECT_EQ(child->testAttribute(Qt::WA_TransparentForMouseEvents),
                            child->windowHandle()->flags().testFlag(Qt::WindowTransparentForInput))
                            << name;
                    EXPECT_TRUE(child->windowHandle()->flags().testFlag(Qt::WindowDoesNotAcceptFocus)) << name;
                } else {
                    ADD_FAILURE() << name << " has no native window";
                }
                EXPECT_GT(decklab::overlay::raiseCount(child), 0) << name;
                break;
            default:
                ADD_FAILURE() << name << " crosses the waveforms but neither hides them nor stays above them";
            }
        }
        return overlays;
    }
};

TEST_F(DeckLabOverlayTest, WaveformSuppressionCountsOwnersAndAlwaysRestores) {
    auto* first = detail(1);
    auto* second = detail(2);
    ASSERT_NE(nullptr, first); ASSERT_NE(nullptr, second);
    ASSERT_TRUE(first->isVisible()); ASSERT_TRUE(second->isVisible());
    auto* card = panel->findChild<QWidget*>(QStringLiteral("decklab-card-1"));
    auto* edge = panel->findChild<QWidget*>(QStringLiteral("decklab-lane-edge-1"));
    ASSERT_NE(nullptr, card); ASSERT_NE(nullptr, edge);
    const QRect lane = first->geometry(), cardArea = card->geometry(), edgeArea = edge->geometry();
    ReleaseCounter releases;
    first->installEventFilter(&releases);

    auto a = std::make_unique<QObject>();
    auto b = std::make_unique<QObject>();
    panel->setWaveformsSuppressed(a.get(), true);
    panel->setWaveformsSuppressed(a.get(), true); // one owner counts once
    EXPECT_TRUE(panel->waveformsSuppressed());
    EXPECT_FALSE(first->isVisible()); EXPECT_FALSE(second->isVisible());
    // A touch scratch held on a waveform that disappears is released.
    EXPECT_EQ(1, releases.releases);
    // The Decks page keeps its layout: nothing moves under the overlay.
    settle();
    EXPECT_EQ(lane, first->geometry());
    EXPECT_EQ(cardArea, card->geometry());
    EXPECT_EQ(edgeArea, edge->geometry());

    panel->setWaveformsSuppressed(b.get(), true);
    panel->setWaveformsSuppressed(a.get(), false);
    EXPECT_TRUE(panel->waveformsSuppressed()); // b still holds them
    EXPECT_FALSE(first->isVisible());
    b.reset(); // a destroyed owner releases
    EXPECT_FALSE(panel->waveformsSuppressed());
    EXPECT_TRUE(first->isVisible()); EXPECT_TRUE(second->isVisible());
    // Unknown, repeated and null owners change nothing.
    panel->setWaveformsSuppressed(a.get(), false);
    panel->setWaveformsSuppressed(nullptr, true);
    EXPECT_FALSE(panel->waveformsSuppressed());
    // Lanes outside this layout are never left hidden either.
    for (int deck = 1; deck <= 4; ++deck) EXPECT_FALSE(detail(deck)->isHidden()) << deck;
    settle();
    EXPECT_EQ(lane, first->geometry());
    // An owner destroyed without ever releasing.
    panel->setWaveformsSuppressed(a.get(), true);
    a.reset();
    EXPECT_FALSE(panel->waveformsSuppressed());
    EXPECT_TRUE(first->isVisible());
}

TEST_F(DeckLabOverlayTest, DeckSetupAndConfirmationsHideWaveformsOnlyWhileShown) {
    QWindow* waveform = addWaveformWindow(1);
    ASSERT_TRUE(waveform->isVisible());
    config()->setValue(ConfigKey("[DeckLab]", "first_run_done"), 1);
    DeckLabSystem::Services services;
    services.config = config();
    DeckLabSystem system(services, nullptr, QStringLiteral("/nonexistent/decklabd.sock"));
    panel->attachSystem(&system);
    auto* ui = panel->findChild<DeckLabSystemUi*>();
    ASSERT_NE(nullptr, ui);

    QPointer<DeckLabSetupWizard> wizard = ui->openFirstRun();
    ASSERT_FALSE(wizard.isNull());
    settle();
    EXPECT_EQ(decklab::overlay::Kind::SuppressesWaveforms, decklab::overlay::kind(wizard));
    EXPECT_TRUE(panel->waveformsSuppressed());
    EXPECT_FALSE(waveform->isVisible());
    EXPECT_FALSE(detail(1)->isVisible());
    auto* later = wizard->findChild<QPushButton*>(QStringLiteral("decklab-setup-close"));
    ASSERT_NE(nullptr, later);
    later->click();
    settle();
    EXPECT_FALSE(panel->waveformsSuppressed());
    EXPECT_TRUE(waveform->isVisible());
    EXPECT_TRUE(detail(1)->isVisible());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_TRUE(wizard.isNull());

    // A confirmation over the whole window, answered.
    QPointer<decklab::ui::Confirm> confirm = decklab::ui::Confirm::ask(panel->window(),
            QStringLiteral("Generated question?"), QString(), QStringLiteral("Yes"), [] {});
    settle();
    EXPECT_EQ(decklab::overlay::Kind::SuppressesWaveforms, decklab::overlay::kind(confirm));
    EXPECT_TRUE(panel->waveformsSuppressed());
    auto* cancel = confirm->findChild<QPushButton*>(QStringLiteral("decklab-confirm-cancel"));
    ASSERT_NE(nullptr, cancel);
    cancel->click();
    settle();
    EXPECT_FALSE(panel->waveformsSuppressed());
    EXPECT_TRUE(waveform->isVisible());

    // Destroyed while shown, with Deck Setup open as well.
    wizard = ui->openWizard(0);
    ASSERT_FALSE(wizard.isNull());
    auto* info = decklab::ui::Confirm::inform(panel.get(), QStringLiteral("Generated notice"), QString());
    settle();
    EXPECT_TRUE(panel->waveformsSuppressed());
    delete info;
    EXPECT_TRUE(panel->waveformsSuppressed()); // Deck Setup is still shown
    wizard->hide();
    EXPECT_FALSE(panel->waveformsSuppressed());
    EXPECT_TRUE(detail(1)->isVisible());
    wizard->show();
    EXPECT_TRUE(panel->waveformsSuppressed());
    // TearDown destroys the surface with Deck Setup still open.
}

TEST_F(DeckLabOverlayTest, EveryOverlayOverTheWaveformsHidesThemOrIsNativeAndRaised) {
    QWindow* waveform = addWaveformWindow(1);
    ASSERT_NE(nullptr, waveform->parent());
    config()->setValue(ConfigKey("[DeckLab]", "first_run_done"), 1);
    DeckLabSystem::Services services;
    services.config = config();
    DeckLabSystem system(services, nullptr, QStringLiteral("/nonexistent/decklabd.sock"));
    panel->attachSystem(&system);
    auto* ui = panel->findChild<DeckLabSystemUi*>();
    ASSERT_NE(nullptr, ui);

    // Every small overlay is classified, shown or not.
    for (const char* name : {"decklab-banner-audio", "decklab-banner-offer", "decklab-banner-thermal",
                 "decklab-banner-toast", "decklab-toast"}) {
        auto* overlay = panel->findChild<QWidget*>(QString::fromLatin1(name));
        ASSERT_NE(nullptr, overlay) << name;
        EXPECT_EQ(decklab::overlay::Kind::AboveWaveforms, decklab::overlay::kind(overlay)) << name;
    }
    // Making them native must make nothing else native: new native windows
    // are stacked above the waveform windows and would bury them.
    for (const char* name : {"decklab-rail", "decklab-decks", "decklab-wave-zone", "decklab-deck-row", "decklab-pages"}) {
        auto* widget = panel->findChild<QWidget*>(QString::fromLatin1(name));
        ASSERT_NE(nullptr, widget) << name;
        EXPECT_EQ(WId(0), widget->internalWinId()) << name;
    }
    EXPECT_EQ(WId(0), detail(1)->internalWinId());

    // The no-audio banner (after its start-up debounce), the overlay probe's
    // toast, warning banner and controller offer, and a load refusal.
    auto* audio = panel->findChild<QWidget*>(QStringLiteral("decklab-banner-audio"));
    EXPECT_TRUE(QTest::qWaitFor([&] { return audio->isVisible(); }, 3000));
    ui->showOverlayProbe();
    ControlObject::set(ConfigKey("[DeckLabNative]", "load_refused"), 2);
    ControlObject::set(ConfigKey("[DeckLabNative]", "load_refused"), 0);
    settle();
    auto* thermal = panel->findChild<QWidget*>(QStringLiteral("decklab-banner-thermal"));
    auto* toast = panel->findChild<QWidget*>(QStringLiteral("decklab-banner-toast"));
    auto* offer = panel->findChild<QPushButton*>(QStringLiteral("decklab-banner-offer"));
    auto* refusal = panel->findChild<QWidget*>(QStringLiteral("decklab-toast"));
    ASSERT_TRUE(thermal->isVisible()); ASSERT_TRUE(toast->isVisible());
    ASSERT_TRUE(offer->isVisible()); ASSERT_TRUE(refusal->isVisible());
    EXPECT_TRUE(thermal->accessibleName().startsWith(QStringLiteral("Overlay check")));
    EXPECT_FALSE(panel->waveformsSuppressed());
    EXPECT_TRUE(waveform->isVisible());
    EXPECT_GE(checkOverlays(waveform), 4);
    capture(QStringLiteral("overlay-banners-over-decks.png"));

    // A waveform window created later (another lane shown for the first
    // time) is stacked above the banners, so they are raised again.
    const int raised = decklab::overlay::raiseCount(thermal);
    QWindow* laterWaveform = addWaveformWindow(2);
    ASSERT_EQ(waveform->parent(), laterWaveform->parent());
    EXPECT_GT(decklab::overlay::raiseCount(thermal), raised);

    // The controller offer takes the tap and opens Deck Setup over Decks.
    QTest::mouseClick(offer, Qt::LeftButton);
    settle();
    QPointer<DeckLabSetupWizard> wizard = ui->wizard();
    ASSERT_FALSE(wizard.isNull());
    EXPECT_TRUE(wizard->isVisible());
    EXPECT_TRUE(panel->waveformsSuppressed());
    EXPECT_FALSE(waveform->isVisible()); EXPECT_FALSE(laterWaveform->isVisible());
    EXPECT_GE(checkOverlays(waveform), 2);
    capture(QStringLiteral("overlay-deck-setup-over-decks.png"));
    // A confirmation over Deck Setup.
    auto* confirm = decklab::ui::Confirm::ask(panel.get(), QStringLiteral("Generated question?"), QString(),
            QStringLiteral("Yes"), [] {});
    settle();
    EXPECT_GE(checkOverlays(waveform), 3);
    confirm->close();
    wizard->findChild<QPushButton*>(QStringLiteral("decklab-setup-close"))->click();
    settle();
    EXPECT_FALSE(panel->waveformsSuppressed());
    EXPECT_TRUE(waveform->isVisible()); EXPECT_TRUE(laterWaveform->isVisible());
}
