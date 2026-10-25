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
#include <QFrame>
#include <QHeaderView> // r6-keys
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
#include "decklab/decklabfakeservice.h"
#include "decklab/decklabkeys.h" // r6-keys
#include "decklab/decklaboverlay.h"
#include "decklab/decklabsetupwizard.h"
#include "decklab/decklabsystem.h"
#include "decklab/decklabsystemui.h"
#include "decklab/decklabusbname.h"
#include "decklab/decklabrail.h"
#include "decklab/decklabsettings.h"
#include "decklab/decklabutility.h"
#include "library/decklabknob.h"
#include "decklab/decklabui.h"
#include "effects/effectchain.h"
#include "effects/effectslot.h"
#include "engine/decklabplaystart.h" // r6-playstart
#include "library/decklabknob.h"
#include "library/libraryfeature.h"
#include "library/librarycontrol.h"
#include "library/library_prefs.h" // r6-keys
#include "library/proxytrackmodel.h" // r6-keys
#include "library/rekordbox/rekordboxfeature.h"
#include "library/sidebarmodel.h"
#include "library/treeitem.h"
#include "test/decklabgeneratedfolder.h" // r6-keys
#include "test/decklabpaintedtext.h" // r6-keys
#include "test/signalpathtest.h"
#include "mixer/playermanager.h"
#include "track/beats.h"
#include "track/keyutils.h"
#include "waveform/waveformwidgetfactory.h"
#include "widget/decklabbrowse.h"
#include "widget/decklabbrowsedock.h"
#include "widget/decklabbrowsehome.h"
#include "widget/decklabbrowselegend.h"
#include "widget/decklabbrowsemix.h"
#include "widget/decklabdeckwidgets.h"
#include "widget/decklabdecktools.h" // r5-decks
#include "widget/decklabgridmode.h"
#include "widget/decklabkeysheet.h"
#include "widget/decklabmoreknob.h" // r6-sync
#include "widget/decklabcapture.h"  // r7-sampler: TO PAD
#include "sources/soundsourceproxy.h" // r7-sampler
#include "widget/decklabpads.h"
#include "widget/decklabhistory.h"     // r5-history
#include "widget/decklabhistoryview.h" // r5-history
#include "widget/decklablabfx.h"
#include "widget/decklablanes.h"
#include "widget/decklabtempopresenter.h"
#include "widget/decklabsampler.h"
#include "widget/wdecklab.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarysidebar.h"
#include "widget/wsearchlineedit.h"
#include "widget/wtracktableview.h" // r6-keys
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
    QHash<QString, BaseTrackPlayer*> others; // r7-sampler: a sampler stand-in
    BaseTrackPlayer* getPlayer(const QString& group) const override {
        for(int n=0;n<4;++n)if(group==QStringLiteral("[Channel%1]").arg(n+1))return decks[n];
        return others.value(group);
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
        decklab::usbNameBook().clear(); // r5-polish: stick names start fresh per test
        navigation = std::make_unique<LibraryControl>(nullptr);
        soundStatus = std::make_unique<ControlObject>(ConfigKey("[SoundManager]", "status"));
        // RecordingManager's status exists before the skin in the app.
        recordingStatus = std::make_unique<ControlObject>(ConfigKey("[Recording]", "status"));
        // r6-keys: Library makes [Library],key_notation before the skin in the app.
        keyNotation = std::make_unique<ControlObject>(mixxx::library::prefs::kKeyNotationConfigKey);
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
        panel.reset();
        generatedRows.reset(); generatedFolder.reset(); keyNotation.reset(); // r6-keys: after the table
        soundStatus.reset(); recordingStatus.reset(); players.reset(); navigation.reset(); model.reset(); source.reset();
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
    std::unique_ptr<ControlObject> recordingStatus;
    std::unique_ptr<ControlObject> keyNotation; // r6-keys
    std::unique_ptr<DeckLabGeneratedFolder> generatedFolder; // r6-keys
    std::unique_ptr<ProxyTrackModel> generatedRows;          // r6-keys
    std::unique_ptr<Deck> fourthDeck;
    WLibrarySidebar* sidebar = nullptr;
    WLibrary* library = nullptr;
    // r6-keys: a Browse track table (Mixxx's WTrackTableView with Deck Lab's
    // cells) over generated rows whose keys are stored text, as a USB folder
    // or a rekordbox export hands them over: the library's current view.
    WTrackTableView* installGeneratedTable(const QStringList& keys) {
        generatedFolder = std::make_unique<DeckLabGeneratedFolder>(keys);
        generatedRows = std::make_unique<ProxyTrackModel>(generatedFolder.get());
        auto* table = new WTrackTableView(library, config(), nullptr, 1.0);
        library->registerView(QStringLiteral("decklab-generated-table"), table);
        table->loadTrackModel(generatedRows.get(), false);
        library->switchToView(QStringLiteral("decklab-generated-table"));
        return table;
    }
    // r6-keys: Browse's track level from the Decks page (USB library ›
    // Genres › House shows the library's current view).
    void openTrackLevel() {
        panel->browsePress(); panel->browsePress();
        const QPersistentModelIndex genres = model->index(0, 0, sidebar->rootIndex());
        sidebar->selectIndex(genres); QCoreApplication::processEvents();
        QTest::mouseClick(sidebar->viewport(), Qt::LeftButton, Qt::NoModifier, sidebar->visualRect(genres).center());
        QCoreApplication::processEvents();
        panel->browsePress(); QCoreApplication::processEvents(); settle();
    }
    // r6-keys: what a table's own KEY cell paints for each row, in row order.
    static QString paintedKeys(QTableView* table, int column) {
        QStringList keys;
        QAbstractItemDelegate* cell = table->itemDelegateForColumn(column);
        for (int row = 0; cell && row < table->model()->rowCount(); ++row) {
            QStyleOptionViewItem option;
            option.initFrom(table);
            option.widget = table;
            option.rect = QRect(0, 0, 120, 44); // wide: no text is cut
            option.font = table->font();
            option.state = QStyle::State_Enabled;
            keys << DeckLabPaintedText::of(*cell, option, table->model()->index(row, column)).join(QLatin1Char('|'));
        }
        return keys.join(QLatin1Char(' '));
    }
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
    // r4-browse: the deck dock (the LOAD targets) moves into the pane above
    // the keyboard; the knob legend steps aside.
    auto* dock = panel->findChild<QWidget*>("decklab-deck-dock"); ASSERT_NE(nullptr, dock);
    auto* pane = panel->findChild<QWidget*>("decklab-browse-pane"); ASSERT_NE(nullptr, pane);
    auto* legend = panel->findChild<QWidget*>("decklab-knob-legend"); ASSERT_NE(nullptr, legend);
    EXPECT_TRUE(dock->isVisible()); EXPECT_TRUE(pane->isAncestorOf(dock)); EXPECT_FALSE(legend->isVisible());
    for (auto* b : keyboard->findChildren<QPushButton*>()) if (b->text() == "Q") { b->click(); break; }
    auto* search = panel->findChild<WSearchLineEdit*>(); ASSERT_NE(nullptr, search);
    EXPECT_EQ(QStringLiteral("q"), search->lineEdit()->text());
    panel->findChild<QPushButton*>("decklab-search-done")->click();
    EXPECT_FALSE(keyboard->isVisible());
    // Browse keeps the dock along the bottom; the waveforms return on Decks.
    EXPECT_TRUE(dock->isVisible()); EXPECT_FALSE(pane->isAncestorOf(dock)); EXPECT_TRUE(legend->isVisible());
    EXPECT_FALSE(panel->waveformContainer()->isVisible());
    panel->selectPage(0); QCoreApplication::processEvents();
    EXPECT_TRUE(panel->waveformContainer()->isVisible());
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
    // r4-system: a switch row that prints ON/OFF inside the switch.
    auto* split = panel->findChild<decklab::ui::SwitchRow*>("decklab-head-split"); ASSERT_NE(nullptr, split);
    EXPECT_EQ(QStringLiteral("Split cue"), split->text());
    ControlObject::set(ConfigKey("[Master]", "headSplit"), 0); QCoreApplication::processEvents();
    EXPECT_EQ(QStringLiteral("OFF"), split->valueText()); EXPECT_FALSE(split->isChecked());
    split->click(); QCoreApplication::processEvents();
    EXPECT_EQ(QStringLiteral("ON"), split->valueText()); EXPECT_TRUE(split->isChecked());
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Master]", "headSplit")));
    ControlObject::set(ConfigKey("[Master]", "headSplit"), 0); QCoreApplication::processEvents();
    EXPECT_EQ(QStringLiteral("OFF"), split->valueText());
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

TEST_F(NativeDeckLabTest, SettingsTouchSwipeScrollsWithoutTogglingQuantize) {
    panel->selectPage(4); QTest::qWait(50);
    for (int category = 0; category < 5; ++category) {
        auto* scroll = panel->findChild<QScrollArea*>(QStringLiteral("decklab-settings-scroll-%1").arg(category));
        ASSERT_NE(nullptr, scroll);
        EXPECT_TRUE(QScroller::hasScroller(scroll->viewport()));
        EXPECT_NE(static_cast<Qt::GestureType>(0), QScroller::grabbedGesture(scroll->viewport()));
    }
    // r4-system: one set of per-deck rows, driven by a deck selector.
    panel->findChild<QPushButton*>("decklab-settings-category-1")->click(); QTest::qWait(50);
    auto* scroll = panel->findChild<QScrollArea*>("decklab-settings-scroll-1");
    auto* viewport = scroll->viewport();
    auto* range = panel->findChild<QWidget*>("decklab-settings-tempo-range-1"); ASSERT_NE(nullptr, range);
    auto* quantize = panel->findChild<decklab::ui::SwitchRow*>("decklab-settings-quantize"); ASSERT_NE(nullptr, quantize);
    const auto fullyVisible = [viewport](QWidget* widget) {
        return viewport->rect().contains(QRect(widget->mapTo(viewport, QPoint()), widget->size()));
    };
    const double before = ControlObject::get(ConfigKey("[Channel1]", "quantize"));
    const QPoint start = quantize->mapTo(viewport, quantize->rect().center());
    swipeTouch(viewport, start, QPoint(start.x(), 10), 400);
    for (int attempt = 0; attempt < 8 && !fullyVisible(range); ++attempt)
        swipeTouch(viewport, QPoint(viewport->width() / 2, viewport->height() - 30), QPoint(viewport->width() / 2, 30));
    EXPECT_TRUE(fullyVisible(range));
    // A swipe that starts on a switch never flips it.
    EXPECT_DOUBLE_EQ(before, ControlObject::get(ConfigKey("[Channel1]", "quantize")));
    if (!fullyVisible(quantize)) scroll->ensureWidgetVisible(quantize, 0, 0);
    tapTouch(viewport, quantize->mapTo(viewport, quantize->rect().center()));
    EXPECT_DOUBLE_EQ(before > 0 ? 0 : 1, ControlObject::get(ConfigKey("[Channel1]", "quantize")));
    QTest::mouseClick(quantize, Qt::LeftButton);
    EXPECT_DOUBLE_EQ(before, ControlObject::get(ConfigKey("[Channel1]", "quantize")));
    // Deck 2 on the selector: the same rows act on deck 2.
    auto* decks = panel->findChild<QComboBox*>("decklab-settings-deck-model"); ASSERT_NE(nullptr, decks);
    QMetaObject::invokeMethod(decks, "activated", Q_ARG(int, 1)); QCoreApplication::processEvents();
    const double deckTwo = ControlObject::get(ConfigKey("[Channel2]", "quantize"));
    EXPECT_EQ(deckTwo > 0, quantize->isChecked());
    QTest::mouseClick(quantize, Qt::LeftButton);
    EXPECT_DOUBLE_EQ(deckTwo > 0 ? 0 : 1, ControlObject::get(ConfigKey("[Channel2]", "quantize")));
    EXPECT_DOUBLE_EQ(before, ControlObject::get(ConfigKey("[Channel1]", "quantize")));
}

namespace {
// r4-system: a controller write as a mapping makes it (press, then release).
void r4Pulse(const char* group, const char* item, double value = 1) {
    ControlObject::set(ConfigKey(group, item), value);
    if (value > 0) ControlObject::set(ConfigKey(group, item), 0);
    QCoreApplication::processEvents();
}
} // namespace

namespace {
// r5-fix3-system: the Numark overlay's knob in native Deck Lab
// (DeckLab-Numark.js, appliance round5-app). Off Browse: browse_hold 1 and
// the press at press-down, browse_hold 0 at release. On Browse: browse_hold
// 1 at press-down; at release browse_hold 0, then the press. Shift+press is
// browse_back alone. Turns are [Playlist] SelectTrackKnob.
void numarkDownOffBrowse() {
    ControlObject::set(ConfigKey("[DeckLabNative]", "browse_hold"), 1);
    r4Pulse("[DeckLabNative]", "browse_press");
}
void numarkUpOffBrowse() {
    ControlObject::set(ConfigKey("[DeckLabNative]", "browse_hold"), 0);
    QCoreApplication::processEvents();
}
void numarkDownOnBrowse() {
    ControlObject::set(ConfigKey("[DeckLabNative]", "browse_hold"), 1);
    QCoreApplication::processEvents();
}
void numarkUpOnBrowse() {
    ControlObject::set(ConfigKey("[DeckLabNative]", "browse_hold"), 0);
    r4Pulse("[DeckLabNative]", "browse_press");
}
void numarkShiftPress() {
    r4Pulse("[DeckLabNative]", "browse_back");
}
void numarkTurn(int steps) {
    r4Pulse("[Playlist]", "SelectTrackKnob", steps);
}
} // namespace

TEST_F(NativeDeckLabTest, NumarkKnobConfirmsHoldButtonsOnlyWhenSeenAndMeant) {
    // r5-fix3-system: "Press again" confirms with the real controller
    // sequence, a held knob confirms after 1.2 s, and Turn off never fires
    // unless it is on screen, focused, owned by the knob and meant.
    config()->setValue(ConfigKey("[DeckLab]", "first_run_done"), 1);
    QTemporaryDir socketDir;
    DeckLabFakeService fake;
    ASSERT_TRUE(fake.listen(socketDir.filePath(QStringLiteral("decklabd.sock"))));
    DeckLabSystem::Services services;
    services.config = config();
    DeckLabSystem system(services, nullptr, socketDir.filePath(QStringLiteral("decklabd.sock")));
    panel->attachSystem(&system);
    system.start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return system.isReady(); }, 3000));
    auto* settings = panel->findChild<DeckLabSettingsPage*>(); ASSERT_NE(nullptr, settings);
    panel->selectPage(4);
    settings->showUnit(DeckLabSystemPage::Section::Power);
    auto* off = panel->findChild<decklab::ui::HoldButton*>(QStringLiteral("decklab-power-shutdown"));
    auto* reboot = panel->findChild<decklab::ui::HoldButton*>(QStringLiteral("decklab-power-reboot"));
    ASSERT_NE(nullptr, off); ASSERT_NE(nullptr, reboot);
    ASSERT_TRUE(QTest::qWaitFor([&] { return off->isVisible() && off->isEnabled(); }, 3000));
    int fired = 0;
    QObject::connect(off, &decklab::ui::HoldButton::confirmed, [&fired] { ++fired; });
    const auto powerCalls = [&fake] { return static_cast<int>(fake.calls.count(QStringLiteral("system.power"))); };
    auto* knob = settings->knob();
    EXPECT_EQ(settings, decklab::knobClaimant());

    // 1. Press twice, off Browse: the first press arms ("Press again") and
    // waits, the second one confirms.
    knob->focus(off);
    numarkDownOffBrowse(); QTest::qWait(150); numarkUpOffBrowse();
    EXPECT_TRUE(off->armed()); EXPECT_EQ(0, fired);
    QTest::qWait(1500);
    EXPECT_TRUE(off->armed()); EXPECT_EQ(0, fired);
    numarkDownOffBrowse(); QTest::qWait(100); numarkUpOffBrowse();
    EXPECT_EQ(1, fired);
    EXPECT_FALSE(off->armed());
    EXPECT_TRUE(QTest::qWaitFor([&] { return powerCalls() == 1; }, 2000));

    // 2. A turn or Shift+press (Back) calls the first press off.
    numarkDownOffBrowse(); numarkUpOffBrowse();
    ASSERT_TRUE(off->armed());
    numarkTurn(-1);
    EXPECT_EQ(reboot, knob->current());
    EXPECT_FALSE(off->armed());
    numarkTurn(1);
    EXPECT_EQ(off, knob->current());
    numarkDownOffBrowse(); numarkUpOffBrowse();
    EXPECT_TRUE(off->armed()); EXPECT_EQ(1, fired);
    numarkShiftPress();
    EXPECT_FALSE(off->armed());
    EXPECT_EQ(DeckLabSystemPage::Section::Power, settings->systemPage()->currentSection());
    EXPECT_EQ(1, fired);

    // 3. Holding the knob, off Browse: 1.2 s confirms; a short hold only arms.
    numarkDownOffBrowse();
    EXPECT_TRUE(off->holding());
    QTest::qWait(1500);
    EXPECT_EQ(2, fired);
    numarkUpOffBrowse();
    EXPECT_FALSE(off->armed()); EXPECT_EQ(2, fired);
    numarkDownOffBrowse(); QTest::qWait(400); numarkUpOffBrowse();
    EXPECT_TRUE(off->armed());
    QTest::qWait(1500);
    EXPECT_EQ(2, fired);
    numarkShiftPress();
    EXPECT_FALSE(off->armed());

    // 4. On Browse (a sheet over Browse) the press comes at release: after a
    // full hold it is the end of the same gesture, never a new first press.
    numarkDownOnBrowse(); QTest::qWait(1500);
    EXPECT_EQ(3, fired);
    numarkUpOnBrowse();
    EXPECT_FALSE(off->armed()); EXPECT_EQ(3, fired);
    numarkDownOnBrowse(); QTest::qWait(100); numarkUpOnBrowse();
    EXPECT_TRUE(off->armed()); EXPECT_EQ(3, fired);
    numarkDownOnBrowse(); QTest::qWait(100); numarkUpOnBrowse();
    EXPECT_EQ(4, fired);
    EXPECT_TRUE(QTest::qWaitFor([&] { return powerCalls() == 4; }, 2000));

    // 5. Never off screen: a touch takes Unit back to its home while the knob
    // ring was on Turn off. A press and hold then acts on nothing hidden.
    knob->focus(off);
    settings->systemPage()->showSection(DeckLabSystemPage::Section::Home);
    QCoreApplication::processEvents();
    ASSERT_FALSE(off->isVisible());
    numarkDownOffBrowse(); QTest::qWait(1500); numarkUpOffBrowse();
    numarkDownOnBrowse(); QTest::qWait(1500); numarkUpOnBrowse();
    EXPECT_FALSE(off->holding()); EXPECT_FALSE(off->armed());
    EXPECT_EQ(4, fired);
    // A hold in progress stops when its button leaves the screen.
    settings->showUnit(DeckLabSystemPage::Section::Power);
    ASSERT_TRUE(QTest::qWaitFor([&] { return off->isVisible() && off->isEnabled(); }, 2000));
    knob->focus(off);
    numarkDownOffBrowse();
    ASSERT_TRUE(off->holding());
    settings->systemPage()->showSection(DeckLabSystemPage::Section::Home);
    QTest::qWait(1500);
    numarkUpOffBrowse();
    EXPECT_EQ(4, fired);

    // 6. A turn mid-hold, or the knob going to a sheet mid-hold, calls it off.
    settings->showUnit(DeckLabSystemPage::Section::Power);
    ASSERT_TRUE(QTest::qWaitFor([&] { return off->isVisible() && off->isEnabled(); }, 2000));
    knob->focus(off);
    numarkDownOffBrowse();
    ASSERT_TRUE(off->holding());
    numarkTurn(1);
    EXPECT_FALSE(off->holding());
    QTest::qWait(1500);
    numarkUpOffBrowse();
    EXPECT_EQ(4, fired); EXPECT_FALSE(off->armed());
    knob->focus(off);
    numarkDownOffBrowse();
    ASSERT_TRUE(off->holding());
    QPointer<decklab::ui::Confirm> sheet =
            decklab::ui::Confirm::inform(panel.get(), QStringLiteral("Generated notice"), QString());
    QTest::qWait(1500);
    numarkUpOffBrowse();
    EXPECT_FALSE(off->holding()); EXPECT_FALSE(off->armed());
    EXPECT_EQ(4, fired);
    if (sheet) sheet->close();
    QTest::qWait(300);
    EXPECT_EQ(4, powerCalls());
}

TEST_F(NativeDeckLabTest, SettingsIsAUtilityListTheKnobDrives) {
    // r4-system (Signature 1): turn moves, press opens or edits, back goes up
    // one level; from the category list back returns to Decks.
    panel->selectPage(4); QTest::qWait(50);
    auto* settings = panel->findChild<DeckLabSettingsPage*>(); ASSERT_NE(nullptr, settings);
    EXPECT_EQ(settings, decklab::knobClaimant());
    EXPECT_EQ(DeckLabSettingsPage::Decks, settings->category());
    EXPECT_EQ(0, settings->knobLevel());
    r4Pulse("[Playlist]", "SelectTrackKnob", 1);
    EXPECT_EQ(4, panel->currentPage());
    EXPECT_EQ(panel->findChild<QWidget*>("decklab-settings-category-1"), settings->knob()->current());
    r4Pulse("[Playlist]", "SelectTrackKnob", 1); // Headphones
    r4Pulse("[Library]", "GoToItem");             // press opens it
    EXPECT_EQ(DeckLabSettingsPage::Headphones, settings->category());
    EXPECT_EQ(1, settings->knobLevel());
    // Turns visit Headphones' rows; a press on a switch flips its control.
    auto* split = panel->findChild<decklab::ui::SwitchRow*>("decklab-head-split"); ASSERT_NE(nullptr, split);
    ControlObject::set(ConfigKey("[Master]", "headSplit"), 0); QCoreApplication::processEvents();
    settings->knob()->focus(split);
    r4Pulse("[Library]", "GoToItem");
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Master]", "headSplit")));
    // The mix: press to edit, turn to change, back to restore.
    auto* mix = panel->findChild<decklab::ui::StepperRow*>("decklab-head-mix"); ASSERT_NE(nullptr, mix);
    const double mixBefore = ControlObject::get(ConfigKey("[Master]", "headMix"));
    settings->knob()->focus(mix);
    r4Pulse("[Library]", "GoToItem");
    EXPECT_TRUE(settings->knob()->editing());
    r4Pulse("[Playlist]", "SelectTrackKnob", 2);
    EXPECT_NE(mixBefore, ControlObject::get(ConfigKey("[Master]", "headMix")));
    r4Pulse("[Library]", "MoveFocusBackward"); // back: restores
    EXPECT_FALSE(settings->knob()->editing());
    EXPECT_NEAR(mixBefore, ControlObject::get(ConfigKey("[Master]", "headMix")), 1e-9);
    r4Pulse("[Library]", "MoveFocusBackward"); // up to the list
    EXPECT_EQ(0, settings->knobLevel());
    r4Pulse("[Library]", "MoveFocusBackward"); // back to Decks
    EXPECT_EQ(0, panel->currentPage());
    EXPECT_NE(settings, decklab::knobClaimant());
    ControlObject::set(ConfigKey("[Master]", "headSplit"), 0);
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
    auto* status = panel->findChild<DeckLabStatusZone*>("decklab-status"); ASSERT_NE(nullptr, status);
    auto* settingsTab = panel->findChild<QPushButton*>("decklab-tab-4"); ASSERT_NE(nullptr, settingsTab);
    QTest::qWait(1700); // start-up opens the sound devices first: no alert flash
    ControlObject::set(ConfigKey("[SoundManager]", "status"), 2); QCoreApplication::processEvents();
    EXPECT_TRUE(status->toolTip().contains("No sound output"));
    // The status never points at a page that doesn't exist; Fix opens Deck Setup.
    EXPECT_FALSE(status->toolTip().contains(QString::fromUtf8("Settings →")));
    // r4-system: no sound output holds the zone (live red outline: the room
    // hears nothing), and the whole zone is the FIX button.
    EXPECT_EQ(decklab::rail::Alert::NoOutput, status->alert());
    EXPECT_EQ(QString::fromUtf8("NO OUTPUT · FIX"), status->alertText());
    const QRect zone = status->geometry();
    const QRect tab = settingsTab->geometry();
    panel->setAudioOutputConfigured(true); QCoreApplication::processEvents();
    EXPECT_FALSE(status->toolTip().contains("No sound output"));
    EXPECT_EQ(decklab::rail::Alert::None, status->alert());
    ASSERT_EQ(4, status->slotViews().size());
    EXPECT_EQ(QStringLiteral("OUT"), status->slotViews().last().label);
    EXPECT_EQ(decklab::ui::Lamp::Go, status->slotViews().last().lamp);
    // Red only for live: nothing is live red while nothing records.
    for (const auto& view : status->slotViews()) EXPECT_NE(decklab::ui::Lamp::Live, view.lamp) << qPrintable(view.label);
    // A fixed status zone: lamps lighting or going out never move the tabs.
    EXPECT_FALSE(status->isHidden());
    EXPECT_EQ(zone, status->geometry());
    EXPECT_EQ(tab, settingsTab->geometry());
    ControlObject::set(ConfigKey("[SoundManager]", "status"), 0); QCoreApplication::processEvents();
    EXPECT_TRUE(status->toolTip().contains("No sound output"));
    EXPECT_EQ(decklab::rail::Alert::NoOutput, status->alert());
    EXPECT_EQ(zone, status->geometry());
}

TEST_F(NativeDeckLabTest, RailKeepsFixedTabsAndTheUsbLampOpensTheStick) {
    // r4-system (Signature 2): fixed tab widths, Settings as a gear at the far
    // right, and the status zone in the rail, never over a page.
    std::array<QRect, 5> tabs;
    for (int page = 0; page < 5; ++page) {
        auto* tab = panel->findChild<QPushButton*>(QStringLiteral("decklab-tab-%1").arg(page)); ASSERT_NE(nullptr, tab);
        tabs[page] = tab->geometry();
        EXPECT_EQ(44, tab->height());
    }
    EXPECT_EQ(tabs[0].width(), tabs[3].width());
    EXPECT_EQ(44, tabs[4].width());
    EXPECT_EQ(panel->width() - 1, tabs[4].right());
    auto* usb = panel->findChild<DeckLabUsbLamp*>("decklab-usb"); ASSERT_NE(nullptr, usb);
    auto* zone = panel->findChild<DeckLabStatusZone*>("decklab-status"); ASSERT_NE(nullptr, zone);
    EXPECT_LT(usb->geometry().right(), zone->geometry().left() + 1);
    ControlObject::set(ConfigKey("[Recording]", "status"), 2); QCoreApplication::processEvents();
    EXPECT_EQ(decklab::ui::Lamp::Live, zone->slotViews().at(2).lamp);
    for (int page = 0; page < 5; ++page)
        EXPECT_EQ(tabs[page], panel->findChild<QPushButton*>(QStringLiteral("decklab-tab-%1").arg(page))->geometry());
    ControlObject::set(ConfigKey("[Recording]", "status"), 0); QCoreApplication::processEvents();
    // The status sheet: one line per subsystem, over the page below the rail.
    auto* rail = panel->findChild<DeckLabRail*>(); ASSERT_NE(nullptr, rail);
    auto* sheet = rail->openSheet(); ASSERT_NE(nullptr, sheet);
    QCoreApplication::processEvents();
    EXPECT_TRUE(sheet->isVisible());
    EXPECT_EQ(44, sheet->geometry().top());
    EXPECT_EQ(6, sheet->lines().size());
    EXPECT_EQ(sheet, decklab::knobClaimant());
    r4Pulse("[Library]", "MoveFocusBackward");
    EXPECT_FALSE(sheet->isVisible());
    EXPECT_EQ(0, panel->currentPage());
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

// r4-sampler (move 4): sixteen pads in a 4x4 grid, four wide like the EP-133
// and the DJS-1000; the side column keeps 44 px keys. r4-waveforms: the decks
// stay in view as two compact lanes over the DECKS row, which replaces the
// page's own strip.
TEST_F(NativeDeckLabTest, SamplerIsAFourByFourInstrumentWithTheDecksInView) {
    loadGenerated(1, 128); ProcessBuffer();
    panel->selectPage(2); QCoreApplication::processEvents();
    QSet<int> columns, rows;
    std::array<QRect, 16> bounds{};
    for (int slot = 1; slot <= 16; ++slot) {
        auto* pad = panel->findChild<QPushButton*>(QStringLiteral("decklab-sample-%1").arg(slot));
        ASSERT_NE(nullptr, pad); EXPECT_TRUE(pad->isVisible());
        EXPECT_GE(pad->width(), 160); EXPECT_GE(pad->height(), 60) << "r4-waveforms: 62 px pads under the lanes";
        bounds[slot - 1] = QRect(pad->mapTo(panel.get(), QPoint()), pad->size());
        EXPECT_TRUE(panel->rect().contains(bounds[slot - 1]));
        columns.insert(bounds[slot - 1].x()); rows.insert(bounds[slot - 1].y());
    }
    EXPECT_EQ(4, columns.size()); EXPECT_EQ(4, rows.size());
    // Row-major: pads 1-4 are the top row, left to right.
    for (int slot = 1; slot <= 16; ++slot) {
        EXPECT_EQ(bounds[(slot - 1) % 4].x(), bounds[slot - 1].x()) << slot;
        EXPECT_EQ(bounds[4 * ((slot - 1) / 4)].y(), bounds[slot - 1].y()) << slot;
    }
    EXPECT_LT(bounds[0].x(), bounds[1].x()); EXPECT_LT(bounds[0].y(), bounds[4].y());
    // Rows keep 4 px between them (a hidden edit sheet used to swallow them).
    for (int row = 1; row < 4; ++row) EXPECT_GE(bounds[4 * row].y() - bounds[4 * (row - 1)].bottom(), 5) << row;
    auto* strip = panel->findChild<DeckLabSamplerDecks*>(QStringLiteral("decklab-sampler-decks"));
    ASSERT_NE(nullptr, strip); EXPECT_FALSE(strip->isVisible()) << "the lanes' DECKS row replaces the strip";
    auto* row = dynamic_cast<DeckLabLaneDecks*>(panel->findChild<QWidget*>(QStringLiteral("decklab-lane-decks")));
    ASSERT_NE(nullptr, row); EXPECT_TRUE(row->isVisible());
    EXPECT_EQ(32, row->height()); EXPECT_EQ(44 + 134, row->mapTo(panel.get(), QPoint()).y()) << "under the two lanes";
    EXPECT_LE(row->mapTo(panel.get(), QPoint()).y() + row->height(), bounds[0].y()) << "the pads start under the row";
    QTest::qWait(250); // the display tick
    EXPECT_TRUE(row->state(1).loaded) << "the DECKS row follows the decks";
    EXPECT_TRUE(row->state(1).bpm.contains("128"));
    for (const char* name : {"decklab-sampler-qnt", "decklab-sampler-sync", "decklab-sampler-edit", "decklab-sampler-fx",
                 "decklab-samples-stop"}) {
        auto* key = panel->findChild<QPushButton*>(QString::fromLatin1(name)); ASSERT_NE(nullptr, key) << name;
        EXPECT_TRUE(key->isVisible()) << name; EXPECT_GE(key->height(), 44) << name; EXPECT_GE(key->width(), 44) << name;
    }
    EXPECT_FALSE(panel->findChild<QPushButton*>("decklab-sampler-bank")->isVisible()) << "16 samplers: one bank";
    EXPECT_FALSE(panel->findChild<QPushButton*>("decklab-samples-stop")->isEnabled()) << "nothing plays";
    capture(QStringLiteral("sampler-grid.png"));
    panel->selectPage(0); EXPECT_EQ(0, panel->currentPage());
    // Lab FX's SMP destination (and anyone else) opens the page this way.
    panel->showSampler(); EXPECT_EQ(2, panel->currentPage());
}

// r4-waveforms: the waveform lanes are on Decks (full), Sampler and Lab FX
// (two compact lanes over the 32 px DECKS row), never on Browse (its deck dock
// is the LOAD target) or Settings. Compact pages share one geometry, so a
// Sampler/Lab FX switch never resizes a GL lane, and a lane keeps its width
// on every page (its waveform column cache stays valid).
TEST_F(NativeDeckLabTest, WaveformLanesShowOnDecksSamplerAndLabFxOnly) {
    using decklab::lanes::Presentation;
    EXPECT_EQ(Presentation::Full, decklab::lanes::presentationFor(0));
    EXPECT_EQ(Presentation::None, decklab::lanes::presentationFor(1));
    EXPECT_EQ(Presentation::Compact, decklab::lanes::presentationFor(2));
    EXPECT_EQ(Presentation::Compact, decklab::lanes::presentationFor(3));
    EXPECT_EQ(Presentation::None, decklab::lanes::presentationFor(4));
    loadGenerated(1, 128); loadGenerated(2, 126); ProcessBuffer(); QTest::qWait(150);
    auto* lanes = panel->waveformContainer();
    auto* pages = panel->findChild<QWidget*>(QStringLiteral("decklab-pages")); ASSERT_NE(nullptr, pages);
    auto* row = dynamic_cast<DeckLabLaneDecks*>(panel->findChild<QWidget*>(QStringLiteral("decklab-lane-decks"))); ASSERT_NE(nullptr, row);
    const auto detail = [this](int deck) { return panel->findChild<QWidget*>(QStringLiteral("decklab-detail-%1").arg(deck)); };
    const auto area = [this](QWidget* widget) { return QRect(widget->mapTo(panel.get(), QPoint()), widget->size()); };
    for (int page = 0; page < 5; ++page) {
        panel->selectPage(page); settle();
        EXPECT_EQ(QSize(800, 480), panel->size()) << page;
        const bool compact = decklab::lanes::presentationFor(page) == Presentation::Compact;
        EXPECT_EQ(page == 0 || compact, lanes->isVisible()) << page;
        EXPECT_EQ(compact, row->isVisible()) << page;
        if (page == 0) {
            EXPECT_EQ(436, lanes->height());
            EXPECT_EQ(QRect(18, 44, 782, 95), area(detail(1)));
            EXPECT_EQ(QRect(18, 141, 782, 95), area(detail(2)));
        } else if (compact) {
            // Two 66 px lanes and the DECKS row under the rail; the page
            // gets the 270 px below them.
            EXPECT_EQ(166, lanes->height()) << page;
            EXPECT_EQ(QRect(18, 44, 782, 66), area(detail(1))) << page;
            EXPECT_EQ(QRect(18, 112, 782, 66), area(detail(2))) << page;
            EXPECT_FALSE(detail(3)->isVisible()) << page;
            EXPECT_EQ(QRect(0, 178, 800, 32), area(row)) << page;
            EXPECT_EQ(QRect(0, 210, 800, 270), area(pages)) << page;
            EXPECT_TRUE(row->shows(1) && row->shows(2)) << page;
            EXPECT_TRUE(row->state(2).bpm.contains("126")) << page;
            // The lane's edge keeps its number (the one-line readouts are gone).
            auto* edge = panel->findChild<QLabel*>(QStringLiteral("decklab-lane-edge-1"));
            ASSERT_NE(nullptr, edge); EXPECT_EQ(QStringLiteral("1"), edge->text());
            EXPECT_FALSE(panel->findChild<QWidget*>(QStringLiteral("decklab-mini-1"))->isVisible());
        }
        if (page == 2) capture(QStringLiteral("lanes-sampler.png"));
        if (page == 3) capture(QStringLiteral("lanes-labfx.png"));
    }
    // Sampler to Lab FX and back: no resize reaches a lane.
    struct ResizeCounter final : QObject {
        int resizes = 0;
        bool eventFilter(QObject* watched, QEvent* event) override {
            if (event->type() == QEvent::Resize) ++resizes;
            return QObject::eventFilter(watched, event);
        }
    } counter;
    panel->selectPage(2); settle();
    detail(1)->installEventFilter(&counter); detail(2)->installEventFilter(&counter);
    panel->selectPage(3); settle(); panel->selectPage(2); settle();
    EXPECT_EQ(0, counter.resizes) << "compact pages share the lanes' geometry";
    // Decks to Sampler changes the height only.
    panel->selectPage(0); settle();
    EXPECT_EQ(782, detail(1)->width());
    panel->selectPage(2); settle();
    EXPECT_EQ(782, detail(1)->width());
    detail(1)->removeEventFilter(&counter); detail(2)->removeEventFilter(&counter);
    // Side by side on Decks stays side by side under the compact pages.
    auto* choice = panel->findChild<QComboBox*>(QStringLiteral("decklab-waveform-layout")); ASSERT_NE(nullptr, choice);
    choice->setCurrentIndex(1); QMetaObject::invokeMethod(choice, "activated", Q_ARG(int, 1)); settle();
    panel->selectPage(0); settle();
    const QSize sideBySide = detail(1)->size();
    panel->selectPage(3); settle();
    EXPECT_EQ(sideBySide.width(), detail(1)->width());
    EXPECT_EQ(area(detail(1)).top(), area(detail(2)).top()) << "one row of two lanes";
    EXPECT_EQ(134, detail(1)->height());
    EXPECT_EQ(QRect(0, 178, 800, 32), area(row));
    choice->setCurrentIndex(0); QMetaObject::invokeMethod(choice, "activated", Q_ARG(int, 0)); settle();
    // 1024x600: taller compact lanes (96 px); the page keeps 330 px.
    panel->resize(1024, 600); settle();
    EXPECT_EQ(226, lanes->height());
    EXPECT_EQ(96, detail(1)->height());
    EXPECT_EQ(330, pages->height());
    panel->resize(800, 480); settle();
    EXPECT_EQ(166, lanes->height());
    EXPECT_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "play")));
}

// r4-waveforms, four decks (the XDJ-AZ model): two scrolling lanes for the
// two most relevant decks, the DECKS row for all four.
TEST_F(NativeDeckLabTest, FourDeckCompactLanesFollowTheLayersAndNameEveryDeck) {
    QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"), "activated", Q_ARG(int, 2));
    for (int deck = 1; deck <= 3; ++deck) loadGenerated(deck, 120 + deck);
    ProcessBuffer(); QTest::qWait(150);
    panel->selectPage(3); settle();
    auto* row = dynamic_cast<DeckLabLaneDecks*>(panel->findChild<QWidget*>(QStringLiteral("decklab-lane-decks"))); ASSERT_NE(nullptr, row);
    ASSERT_TRUE(row->isVisible());
    const auto detail = [this](int deck) { return panel->findChild<QWidget*>(QStringLiteral("decklab-detail-%1").arg(deck)); };
    EXPECT_EQ(4, row->choice().rowCount);
    for (int deck = 1; deck <= 4; ++deck) {
        EXPECT_TRUE(row->shows(deck)) << deck;
        EXPECT_NEAR(200, row->cellRect(deck).width(), 1) << deck;
    }
    // Nothing on air: the controller's layers (decks 1 and 2) keep the lanes.
    EXPECT_EQ((std::array<int, 2>{1, 2}), row->choice().lanes);
    EXPECT_TRUE(detail(1)->isVisible()); EXPECT_TRUE(detail(2)->isVisible());
    EXPECT_FALSE(detail(3)->isVisible()); EXPECT_FALSE(detail(4)->isVisible());
    EXPECT_EQ(66, detail(1)->height());
    EXPECT_TRUE(row->state(3).loaded);
    EXPECT_TRUE(row->state(3).bpm.contains("123"));
    capture(QStringLiteral("lanes-labfx-four.png"));
    panel->selectPage(0); settle();
    for (int deck = 1; deck <= 4; ++deck) EXPECT_TRUE(detail(deck)->isVisible()) << deck;
    EXPECT_FALSE(row->isVisible());
}

// r4-waveforms: compact lanes are display only. A press there never reaches
// the viewer (no scratch while the hands are on the pads); on Decks it does.
TEST_F(NativeDeckLabTest, CompactLanesTakeNoTouch) {
    loadGenerated(1, 128); ProcessBuffer(); QTest::qWait(150);
    auto* lane = panel->findChild<QWidget*>(QStringLiteral("decklab-detail-1")); ASSERT_NE(nullptr, lane);
    struct PressCounter final : QObject {
        int presses = 0;
        bool eventFilter(QObject* watched, QEvent* event) override {
            if (event->type() == QEvent::MouseButtonPress) ++presses;
            return QObject::eventFilter(watched, event);
        }
    } counter;
    lane->installEventFilter(&counter);
    panel->selectPage(0); settle();
    QTest::mousePress(lane, Qt::LeftButton); QTest::mouseRelease(lane, Qt::LeftButton);
    EXPECT_EQ(1, counter.presses) << "Decks: the lane takes the touch (scratch)";
    for (int page : {2, 3}) {
        panel->selectPage(page); settle();
        ASSERT_TRUE(lane->isVisible()) << page;
        QTest::mousePress(lane, Qt::LeftButton); QTest::mouseRelease(lane, Qt::LeftButton);
        EXPECT_EQ(1, counter.presses) << page;
    }
    lane->removeEventFilter(&counter);
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
    // r4-browse: Browse has the deck dock, no lanes and no one-line readouts.
    panel->selectPage(1); QTest::qWait(150);
    EXPECT_FALSE(mini->isVisible()); EXPECT_EQ(QSize(800,480),panel->size());
    // round4: there the dock tile carries the tempo, and the long title stays literal.
    auto* tile = static_cast<DeckLabDockTile*>(panel->findChild<QPushButton*>("decklab-load-1")); ASSERT_NE(nullptr, tile);
    EXPECT_TRUE(tile->isVisible());
    EXPECT_TRUE(tile->deckState().bpm.contains("128"));
    EXPECT_TRUE(tile->deckState().title.endsWith("<b>Title</b>"));
    // r4-waveforms: Lab FX has compact lanes over the 32 px DECKS row (the
    // one-line readouts are gone); its footer names the target.
    panel->selectPage(3); QTest::qWait(150);
    EXPECT_FALSE(mini->isVisible());
    auto* row = dynamic_cast<DeckLabLaneDecks*>(panel->findChild<QWidget*>("decklab-lane-decks")); ASSERT_NE(nullptr, row);
    EXPECT_TRUE(row->isVisible()); EXPECT_EQ(32, row->height());
    EXPECT_TRUE(row->state(1).bpm.contains("128"));
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
    // r4-decks: QNT lives in MORE; the pad mode keys are on the surface.
    for (const char* name : {"decklab-tab-0", "decklab-usb", "decklab-pads-1-mode-loop", "decklab-1-more", "decklab-hotcue-1-1"}) {
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
            "decklab-pads-1-mode-cue", "decklab-1-more", "decklab-2-beatsync", "decklab-hotcue-1-1", "decklab-hotcue-2-8"};
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
            // Browse: no cards; r4-browse: the deck dock instead of waveform
            // lanes and one-line readouts (Lab FX keeps those).
            EXPECT_FALSE(strip(1)->isVisible());
            EXPECT_FALSE(panel->findChild<QWidget*>("decklab-mini-1")->isVisible());
            EXPECT_TRUE(panel->findChild<QWidget*>("decklab-load-1")->isVisible());
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
    // r6-sync: SYNC is a CDJ BEAT SYNC: a tap turns it on (lit, SYNC on the
    // readout) and the next tap off. No one-shot acknowledgment.
    EXPECT_EQ(QStringLiteral("SYNC"), sync->text()); EXPECT_FALSE(sync->isCheckable());
    sync->click(); QCoreApplication::processEvents();
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel1]", "sync_enabled")));
    EXPECT_TRUE(sync->property("lit").toBool()); EXPECT_FALSE(sync->property("actionFeedback").toBool());
    settle(); EXPECT_TRUE(strip(1)->state().sync);
    sync->click(); QCoreApplication::processEvents();
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "sync_enabled")));
    EXPECT_FALSE(sync->property("lit").toBool());
    settle(); EXPECT_FALSE(strip(1)->state().sync);
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

// r6-sync: a SYNC tap acts on the deck it started on, or not at all. (The
// name is the one tools/decklab/build_pi.py requires; there is no one-shot
// acknowledgment any more, the tap itself must not transfer.)
TEST_F(NativeDeckLabTest, SyncAcknowledgmentCannotTransferToAnotherLayer) {
    m_pMixerDeck1->loadFakeTrack(false, 128);
    m_pMixerDeck2->loadFakeTrack(false, 130);
    m_pMixerDeck3->loadFakeTrack(false, 132); fourthDeck->loadFakeTrack(false, 134); ProcessBuffer(); settle();
    auto* left = panel->findChild<QPushButton*>("decklab-1-beatsync");
    auto* right = panel->findChild<QPushButton*>("decklab-2-beatsync");
    ASSERT_NE(nullptr, left); ASSERT_NE(nullptr, right);
    const auto synced = [](int deck) { return ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), "sync_enabled")); };
    left->click(); QCoreApplication::processEvents(); ASSERT_DOUBLE_EQ(1, synced(1));
    left->click(); QCoreApplication::processEvents(); ASSERT_DOUBLE_EQ(0, synced(1));
    for (auto* button : {left, right}) {
        const char* side = button == left ? "[DeckLabLeft]" : "[DeckLabRight]";
        // Pressed on the lower deck, released once the side shows the upper
        // one: neither deck changes.
        QTest::mousePress(button, Qt::LeftButton);
        ControlObject::set(ConfigKey(side, "current"), 1); QCoreApplication::processEvents();
        QTest::mouseRelease(button, Qt::LeftButton); settle();
        for (int deck = 1; deck <= 4; ++deck) EXPECT_DOUBLE_EQ(0, synced(deck)) << side << " deck " << deck;
        EXPECT_FALSE(button->property("lit").toBool());
        ControlObject::set(ConfigKey(side, "current"), 0); settle();
    }
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
    // r4-decks: one pad block per side (4 px gap, two 44 px rows) takes the
    // 96 px of the hot cue and transport rows; SYNC and MORE are its column.
    auto* pads = panel->findChild<QWidget*>("decklab-pads-1"); ASSERT_NE(nullptr, pads);
    EXPECT_NEAR(400, pads->width(), 1); EXPECT_EQ(96, pads->height());
    EXPECT_EQ(384, pads->mapTo(panel.get(), QPoint()).y());
    auto* utilities = panel->findChild<QPushButton*>("decklab-1-more")->parentWidget();
    EXPECT_EQ(pads, utilities->parentWidget());
    EXPECT_EQ(436, panel->findChild<QPushButton*>("decklab-1-more")->mapTo(panel.get(), QPoint()).y());
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

// r6-sync: SYNC is a CDJ-3000 BEAT SYNC (owner decision). A tap locks the
// deck to the tempo master's tempo and beat until the next tap; the deck on
// air becomes the master and keeps its tempo; only the master's fader moves
// the tempo; after sync is off the deck keeps the tempo it had. (The name
// is the one tools/decklab/build_pi.py requires: the match is no longer
// once but a lock until the next tap.)
TEST_F(NativeDeckLabTest, TouchBeatSyncMatchesTempoAndPhaseOnceWithoutStopping) {
    m_pMixerDeck1->loadFakeTrack(false, 128); m_pMixerDeck2->loadFakeTrack(false, 130); ProcessBuffer();
    const auto get = [](int deck, const char* item) { return ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), item)); };
    const auto put = [](int deck, const char* item, double v) { ControlObject::set(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), item), v); };
    ASSERT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[DeckLab]", "cdj_sync"))) << "the native surface turns the CDJ rules on";
    put(1, "quantize", 1); put(2, "quantize", 1);
    ASSERT_DOUBLE_EQ(1, ControlObject::get(decklab::playStartAlignControlKey())) << "r6-playstart: PLAY starts exactly, so the decks start out of phase";
    put(1, "playposition", .11); put(2, "playposition", .21); ProcessBuffer();
    put(1, "play", 1); put(2, "play", 1); ProcessBuffer(); ProcessBuffer();
    ASSERT_GT(std::abs(get(1, "beat_distance") - get(2, "beat_distance")), .01);
    auto* sync = panel->findChild<QPushButton*>("decklab-1-beatsync"); ASSERT_NE(nullptr, sync);
    settle(); sync->click();
    for (int n = 0; n < 8; ++n) ProcessBuffer();
    settle();
    // Deck 2, on air, became the master at its own tempo; deck 1 follows it.
    EXPECT_GT(get(2, "sync_leader"), 0); EXPECT_DOUBLE_EQ(0, get(1, "sync_leader"));
    EXPECT_DOUBLE_EQ(1, get(1, "sync_enabled"));
    EXPECT_NEAR(130, get(2, "bpm"), 1e-6);
    EXPECT_NEAR(130, get(1, "bpm"), 1e-6);
    EXPECT_NEAR(get(1, "beat_distance"), get(2, "beat_distance"), .001);
    EXPECT_TRUE(sync->property("lit").toBool());
    EXPECT_TRUE(strip(1)->state().sync); EXPECT_FALSE(strip(1)->state().master);
    EXPECT_TRUE(strip(2)->state().master);
    EXPECT_DOUBLE_EQ(1, get(1, "play")); EXPECT_DOUBLE_EQ(1, get(2, "play"));
    // A lock, not a one-shot match: the master's tempo carries deck 1 along.
    put(2, "rate_ratio", 1.02);
    for (int n = 0; n < 8; ++n) ProcessBuffer();
    EXPECT_NEAR(132.6, get(2, "bpm"), 1e-6);
    EXPECT_NEAR(132.6, get(1, "bpm"), 1e-6);
    // Deck 1 is not the master: its tempo fader does nothing, for anyone.
    put(1, "rate", .5);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    EXPECT_NEAR(132.6, get(1, "bpm"), 1e-6);
    EXPECT_NEAR(132.6, get(2, "bpm"), 1e-6);
    EXPECT_NEAR(132.6 / 128, get(1, "rate_ratio"), 1e-6) << "the fader went back to the synced tempo";
    // Tap off: the lamp goes out and deck 1 keeps the tempo it had.
    sync->click();
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    settle();
    EXPECT_DOUBLE_EQ(0, get(1, "sync_enabled"));
    EXPECT_FALSE(sync->property("lit").toBool()); EXPECT_FALSE(strip(1)->state().sync);
    EXPECT_NEAR(132.6, get(1, "bpm"), 1e-6);
    // Its own fader works again, and the master no longer moves it.
    put(1, "rate", 0);
    put(2, "rate_ratio", 1);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    EXPECT_NEAR(128, get(1, "bpm"), 1e-6);
    EXPECT_NEAR(130, get(2, "bpm"), 1e-6);
}

// r6-sync: the engine half of the CDJ rules, for every controller: a
// mapping's beatsync is the BEAT SYNC button, a synced deck that is not the
// master ignores its tempo fader, and nothing turns on without a tempo.
// Without the native surface ([DeckLab] cdj_sync 0) Mixxx's behaviour stays.
TEST_F(NativeDeckLabTest, ControllerBeatSyncTogglesAndOnlyTheMasterFaderMovesTheTempo) {
    m_pMixerDeck1->loadFakeTrack(false, 124); m_pMixerDeck2->loadFakeTrack(false, 128);
    auto unanalysed = m_pMixerDeck3->loadFakeTrack(false, 0); ProcessBuffer(); settle();
    const auto get = [](int deck, const char* item) { return ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), item)); };
    const auto put = [](int deck, const char* item, double v) { ControlObject::set(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), item), v); };
    // A controller presses SYNC on deck 1 (stopped, cued) while deck 2 plays.
    put(2, "play", 1); ProcessBuffer(); ProcessBuffer();
    put(1, "beatsync", 1); put(1, "beatsync", 0);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    settle();
    EXPECT_DOUBLE_EQ(1, get(1, "sync_enabled")) << "the lamp a controller binds to sync_enabled stays lit";
    EXPECT_GT(get(2, "sync_leader"), 0) << "the deck on air is the master";
    EXPECT_NEAR(128, get(1, "bpm"), 1e-6);
    EXPECT_NEAR(128, get(2, "bpm"), 1e-6) << "the master kept its tempo";
    EXPECT_TRUE(strip(1)->state().sync); EXPECT_TRUE(strip(2)->state().master);
    // The cued deck's tempo fader never retunes the master on air.
    put(1, "rate", -1);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    EXPECT_NEAR(128, get(2, "bpm"), 1e-6);
    EXPECT_NEAR(128, get(1, "bpm"), 1e-6);
    // The master's tempo buttons move both (a 4 % step, as Mixxx's own
    // engine tests set it).
    const double step = RateControl::getPermanentRateChangeCoarseAmount();
    RateControl::setPermanentRateChangeCoarseAmount(4);
    put(2, "rate_perm_up", 1); put(2, "rate_perm_up", 0);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    const double moved = get(2, "bpm");
    EXPECT_GT(std::abs(moved - 128), 1);
    EXPECT_NEAR(moved, get(1, "bpm"), 1e-6);
    // rate_perm_* on the follower does nothing either.
    put(1, "rate_perm_down", 1); put(1, "rate_perm_down", 0);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    RateControl::setPermanentRateChangeCoarseAmount(step);
    EXPECT_NEAR(moved, get(1, "bpm"), 1e-6);
    EXPECT_NEAR(moved, get(2, "bpm"), 1e-6);
    // Nor does a tempo written as BPM on the follower (a mapping's bpm, as a
    // Shift+TAP sends it, and bpm_up / bpm_down_small; tempo_tap writes bpm
    // the same way): the master on air keeps its tempo, the follower's BPM
    // comes back.
    put(1, "bpm", 140);
    put(1, "bpm_up", 1); put(1, "bpm_up", 0);
    put(1, "bpm_down_small", 1); put(1, "bpm_down_small", 0);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    EXPECT_NEAR(moved, get(1, "bpm"), 1e-6);
    EXPECT_NEAR(moved, get(2, "bpm"), 1e-6) << "a follower's BPM never retunes the master";
    // The master's BPM moves both.
    put(2, "bpm", 131);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    EXPECT_NEAR(131, get(2, "bpm"), 1e-6);
    EXPECT_NEAR(131, get(1, "bpm"), 1e-6);
    // The next press turns it off; the deck keeps the tempo.
    put(1, "beatsync", 1); put(1, "beatsync", 0);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    EXPECT_DOUBLE_EQ(0, get(1, "sync_enabled"));
    EXPECT_NEAR(131, get(1, "bpm"), 1e-6);
    // Off sync, the deck's own BPM controls work again.
    put(1, "bpm", 125);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    EXPECT_NEAR(125, get(1, "bpm"), 1e-6);
    EXPECT_NEAR(131, get(2, "bpm"), 1e-6);
    // No tempo yet (not analysed): BEAT SYNC stays off, so a later analysis
    // can never retune the deck by itself.
    ASSERT_FALSE(unanalysed->getBeats());
    put(3, "beatsync", 1); put(3, "beatsync", 0);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    EXPECT_DOUBLE_EQ(0, get(3, "sync_enabled"));
    // Mixxx's own behaviour without the native rules: beatsync is a one-shot
    // match, and any synced deck's fader retunes the group.
    put(2, "sync_enabled", 0); put(2, "play", 0); ProcessBuffer();
    ControlObject::set(ConfigKey("[DeckLab]", "cdj_sync"), 0);
    put(1, "beatsync", 1); put(1, "beatsync", 0);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    EXPECT_DOUBLE_EQ(0, get(1, "sync_enabled")) << "stock beatsync: momentary";
    ControlObject::set(ConfigKey("[DeckLab]", "cdj_sync"), 1);
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
        // r4-decks: the side rows are SYNC, MORE and the pad block.
        for(const auto& suffix:{"more","beatsync"}) {
            auto* action=panel->findChild<QPushButton*>(QStringLiteral("decklab-%1-%2").arg(side).arg(QString::fromLatin1(suffix)));
            ASSERT_NE(nullptr,action); EXPECT_FALSE(action->isVisible()) << action->objectName().toStdString();
        }
        EXPECT_FALSE(panel->findChild<QWidget*>(QStringLiteral("decklab-pads-%1").arg(side))->isVisible());
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
        const QString pads=QStringLiteral("decklab-pads-%1-mode-").arg(side);
        for(const QString& name:QStringList{QStringLiteral("decklab-%1-more").arg(side),QStringLiteral("decklab-%1-beatsync").arg(side),
                    pads+QStringLiteral("cue"),pads+QStringLiteral("loop"),pads+QStringLiteral("roll")}) {
            auto* action=panel->findChild<QPushButton*>(name);
            ASSERT_NE(nullptr,action) << name.toStdString(); EXPECT_TRUE(action->isVisible());
            EXPECT_GE(action->height(),44); EXPECT_GE(action->width(),44);
            EXPECT_TRUE(panel->rect().contains(QRect(action->mapTo(panel.get(),QPoint()),action->size())));
        }
        // Adjacent touch targets keep at least a 4 px gap.
        auto* sync=panel->findChild<QPushButton*>(QStringLiteral("decklab-%1-beatsync").arg(side));
        auto* lastPad=panel->findChild<QPushButton*>(QStringLiteral("decklab-hotcue-%1-4").arg(side));
        EXPECT_GE(sync->mapTo(panel.get(),QPoint()).x()-(lastPad->mapTo(panel.get(),QPoint()).x()+lastPad->width()),4);
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
    // r5-decks: GRID (was Edit grid) follows the side's layer; the loop above
    // left it on deck 3. It opens GRID mode on the page, not a dialog.
    ControlObject::set(ConfigKey("[DeckLabLeft]","current"),0); QTest::qWait(150);
    more->click(); QCoreApplication::processEvents();
    auto* grid=panel->findChild<QPushButton*>("decklab-1-grid"); ASSERT_NE(nullptr,grid);
    grid->click(); QCoreApplication::processEvents(); QTest::qWait(20);
    EXPECT_EQ(nullptr,panel->findChild<QDialog*>("decklab-beatgrid-editor"));
    auto* mode=panel->findChild<DeckLabGridPanel*>("decklab-grid-mode"); ASSERT_NE(nullptr,mode); ASSERT_TRUE(mode->isVisible());
    EXPECT_EQ(1,mode->state().deck);
    checkButtons(mode);
    // A lit lamp keeps the same physical target size after polish.
    auto* lock=mode->key(DeckLabGridPanel::Action::Lock); ASSERT_NE(nullptr,lock);
    lock->click(); QCoreApplication::processEvents(); checkButtons(mode);
    lock->click(); QCoreApplication::processEvents();
    mode->key(DeckLabGridPanel::Action::Done)->click(); QCoreApplication::processEvents();
    EXPECT_FALSE(mode->isVisible());
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
    // The readout is one target; nothing painted inside takes the touch.
    // r5-decks: except its KEY readout (the key sheet, tested in
    // KeySheetMatchesShiftsAndResetsWithMixxxKeyControls) and a held BPM
    // (GRID mode): the title row is what opens track information.
    for(auto* child:header->findChildren<QWidget*>()) EXPECT_TRUE(child->testAttribute(Qt::WA_TransparentForMouseEvents));
    const QPoint title(header->width()/3,10);
    ASSERT_FALSE(strip(1)->renderer().keyTarget().contains(title));
    ASSERT_FALSE(strip(1)->renderer().bpmTarget().contains(title));
    QTest::mouseClick(header,Qt::LeftButton,Qt::NoModifier,strip(1)->renderer().keyTarget().center());
    QCoreApplication::processEvents(); QTest::qWait(20);
    EXPECT_EQ(nullptr,panel->findChild<QDialog*>("decklab-track-info")) << "KEY opens the key sheet instead";
    QTest::mouseClick(header,Qt::LeftButton,Qt::NoModifier,strip(1)->renderer().keyTarget().center()); // closes it
    QTest::mouseClick(header,Qt::LeftButton,Qt::NoModifier,title);
    QCoreApplication::processEvents(); QTest::qWait(20);
    auto* dialog=panel->findChild<QDialog*>("decklab-track-info"); ASSERT_NE(nullptr,dialog);
    EXPECT_TRUE(dialog->findChild<QTextBrowser*>("decklab-track-info-text")->toPlainText().contains(track->getTitle()));
    dialog->findChild<QPushButton*>("decklab-track-info-close")->click(); QTest::qWait(20);
    for(auto key:{Qt::Key_Return,Qt::Key_Space}) {
        header->setFocus(); QTest::keyClick(header,key); QTest::qWait(20);
        dialog=panel->findChild<QDialog*>("decklab-track-info"); ASSERT_NE(nullptr,dialog); ASSERT_TRUE(dialog->isVisible());
        dialog->findChild<QPushButton*>("decklab-track-info-close")->click(); QTest::qWait(20);
    }
    QTest::mousePress(header,Qt::LeftButton,Qt::NoModifier,title);
    ControlObject::set(ConfigKey("[DeckLabLeft]","current"),1); QCoreApplication::processEvents();
    QTest::mouseRelease(header,Qt::LeftButton,Qt::NoModifier,title); QTest::qWait(20);
    EXPECT_EQ(nullptr,panel->findChild<QDialog*>("decklab-track-info"));
    ControlObject::set(ConfigKey("[DeckLabLeft]","current"),0); QTest::qWait(150);
    QTest::mousePress(header,Qt::LeftButton,Qt::NoModifier,title);
    m_pMixerDeck1->loadFakeTrack(false,140); ProcessBuffer(); QCoreApplication::processEvents();
    QTest::mouseRelease(header,Qt::LeftButton,Qt::NoModifier,title); QTest::qWait(20);
    EXPECT_EQ(nullptr,panel->findChild<QDialog*>("decklab-track-info"));
    // While the deck plays, a stray tap on its readout never covers the
    // decks; Track info stays one tap away in MORE.
    QTest::qWait(150);
    ControlObject::set(ConfigKey("[Channel1]","play"),1); ProcessBuffer(); settle();
    EXPECT_TRUE(header->accessibleDescription().contains("MORE"));
    QTest::mouseClick(header,Qt::LeftButton,Qt::NoModifier,title); QTest::qWait(20);
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
    // r6-fxstrip: the full list keeps all four units; in the 2-deck layout an
    // idle unit 3 or 4 says so on its routing line. Wet unit 3 is not marked.
    EXPECT_EQ(1,detail.count(QStringLiteral("not used in 2-deck layout")));
    EXPECT_TRUE(detail.contains(QStringLiteral("Routed decks: 4 · not used in 2-deck layout"))) << detail.toStdString();
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
    // r6-fxstrip: every line of all four units fits; the sheet never scrolls.
    QCoreApplication::processEvents();
    EXPECT_EQ(0,text->verticalScrollBar()->maximum())
            << text->document()->size().height() << " px of text in " << text->viewport()->height() << " px";
    const QString output=qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
    if(!output.isEmpty()) EXPECT_TRUE(dialog->grab().save(QDir(output).filePath("controller-fx-details.png")));
    close->click(); QCoreApplication::processEvents(); EXPECT_EQ(externalWrites,writes);
    QTest::qWait(150);
    // Rail lamps, lit while a unit is audibly wet. r6-fxstrip: the 2-deck
    // layout shows units 1 and 2, and 3 or 4 only while wet.
    const auto lamps=[board] {
        QString lit; for(const auto& chip:board->chips()) lit+=chip.on ? QLatin1Char('1') : QLatin1Char('0'); return lit;
    };
    const int railWidth=board->width();
    EXPECT_EQ(QStringLiteral("101"),lamps());
    ControlObject::set(ConfigKey("[EffectRack1_EffectUnit4]","enabled"),1); const int enabledWrites=writes;
    QTest::qWait(150); EXPECT_EQ(enabledWrites,writes);
    EXPECT_EQ(QStringLiteral("1011"),lamps());
    EXPECT_TRUE(board->toolTip().contains("FX unit 4 on deck 4;"));
    EXPECT_TRUE(board->accessibleDescription().contains("FX unit 4 on deck 4"));
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
    EXPECT_EQ(QStringLiteral("101"),lamps()) << "units 1, 2 and wet unit 4";
    EXPECT_EQ(QStringLiteral("4"),board->chips().last().text);
    ControlObject::set(ConfigKey("[EffectRack1_EffectUnit4]","mix"),0); const int fourthDryWrites=writes;
    QTest::qWait(150); EXPECT_EQ(fourthDryWrites,writes);
    EXPECT_EQ(QStringLiteral("10"),lamps());
    EXPECT_FALSE(board->toolTip().contains("FX unit 3")) << "idle units 3 and 4 have no lamp";
    // No unit names are ever cut short on the rail: the lamps carry numbers only.
    for(const auto& chip:board->chips()) EXPECT_EQ(1,chip.text.size());
    // Fewer lamps never reflow the rail.
    EXPECT_EQ(railWidth,board->width());
    // r6-fxstrip: both idle (the usual two-deck case), units 3 and 4 are
    // still listed, each marked on its routing line; still no scrolling.
    board->click(); QCoreApplication::processEvents(); QTest::qWait(20);
    QDialog* again=nullptr;
    for(auto* candidate:panel->findChildren<QDialog*>("decklab-controller-fx-info")) if(candidate->isVisible()) again=candidate;
    ASSERT_NE(nullptr,again);
    auto* againText=again->findChild<QTextBrowser*>("decklab-controller-fx-text"); ASSERT_NE(nullptr,againText);
    QCoreApplication::processEvents();
    const QString idle=againText->toPlainText();
    EXPECT_EQ(2,idle.count(QStringLiteral("not used in 2-deck layout"))) << idle.toStdString();
    for(int unit:{3,4}) EXPECT_TRUE(idle.contains(QStringLiteral("Routed decks: %1 · not used in 2-deck layout").arg(unit)));
    EXPECT_TRUE(idle.contains(QStringLiteral("Slot 3: %1 [ON]").arg(manifests[2]->name())));
    EXPECT_EQ(0,againText->verticalScrollBar()->maximum())
            << againText->document()->size().height() << " px of text in " << againText->viewport()->height() << " px";
    if(!output.isEmpty()) { EXPECT_TRUE(again->grab().save(QDir(output).filePath("r6-fxstrip-controller-fx-idle.png"))); }
    again->findChild<QPushButton*>("decklab-controller-fx-close")->click(); QCoreApplication::processEvents();
    EXPECT_EQ(fourthDryWrites,writes);
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
    const auto lamps=[board] {
        QString lit; for(const auto& chip:board->chips()) lit+=chip.on ? QLatin1Char('1') : QLatin1Char('0'); return lit;
    };
    const int railWidth=board->width();
    // r6-fxstrip: four decks, four lamps; two decks, units 1 and 2 plus wet unit 3.
    QMetaObject::invokeMethod(choice,"activated",Q_ARG(int,2)); QTest::qWait(150);
    EXPECT_EQ(QStringLiteral("1010"),lamps());
    for(int unit:{1,3})
        EXPECT_TRUE(board->toolTip().contains(QStringLiteral("FX unit %1 on deck %1; %2; 50%").arg(unit).arg(manifests[0]->name())));
    QMetaObject::invokeMethod(choice,"activated",Q_ARG(int,1)); QTest::qWait(150);
    EXPECT_EQ(QStringLiteral("101"),lamps());
    EXPECT_EQ(railWidth,board->width());
    EXPECT_TRUE(board->toolTip().section('\n',0,0).endsWith("50%"));
    EXPECT_FALSE(board->toolTip().section('\n',0,0).contains("DRY"));
    board->click(); QCoreApplication::processEvents();
    auto* dialog=panel->findChild<QDialog*>("decklab-controller-fx-info"); ASSERT_NE(nullptr,dialog);
    auto* text=dialog->findChild<QTextBrowser*>("decklab-controller-fx-text"); ASSERT_NE(nullptr,text);
    EXPECT_TRUE(text->toPlainText().contains(QStringLiteral("Slot 4: %1 [ON]").arg(manifests[0]->name())));
    // r6-fxstrip: enabled but with no effect on, unit 4 is idle (no lamp);
    // the sheet still lists it and says why. Wet unit 3 is not marked.
    EXPECT_EQ(1,text->toPlainText().count(QStringLiteral("not used in 2-deck layout")));
    EXPECT_TRUE(text->toPlainText().section(QStringLiteral("Unit 4 · Enabled · Mix 50%"),1).startsWith(QStringLiteral("\nRouted decks: 4 · not used in 2-deck layout\n")))
            << text->toPlainText().toStdString();
    dialog->findChild<QPushButton*>("decklab-controller-fx-refresh")->click(); QTest::qWait(150);
    EXPECT_EQ(0,writes);
    for(size_t index=0;index<keys.size();++index) EXPECT_DOUBLE_EQ(snapshot[index],ControlObject::get(keys[index]));
}

TEST_F(NativeDeckLabTest, TwoDeckLayoutFxSurfacesFollowTheLayoutAndTheRouting) {
    // r6-fxstrip: in the 2-deck layout the rail shows controller FX units 1
    // and 2, each named by the deck it feeds; Lab FX offers decks 1 and 2,
    // MASTER and SMP, and 3 or 4 only while in use, in slots that never move.
    m_pEffectsManager->setup(); m_pEffectsManager->ensureDeckLabEffects();
    m_pEngineMixer->prepareAudio(mixxx::audio::SampleRate(44100));
    m_pMixerDeck1->loadFakeTrack(false, 128); m_pMixerDeck2->loadFakeTrack(false, 126);
    ProcessBuffer(); QCoreApplication::processEvents();
    const auto route=[](int unit,int deck) {
        return ConfigKey(QStringLiteral("[EffectRack1_EffectUnit%1]").arg(unit),QStringLiteral("group_[Channel%1]_enable").arg(deck));
    };
    for(int unit=1;unit<=4;++unit)
        for(int deck=1;deck<=4;++deck) ControlObject::set(route(unit,deck),unit<=2 && deck==unit ? 1 : 0);
    QTest::qWait(150);
    auto* board=panel->findChild<DeckLabChipButton*>("decklab-performance-board-fx"); ASSERT_NE(nullptr,board);
    const auto lamps=[board] {
        QStringList texts; for(const auto& chip:board->chips()) texts<<chip.text; return texts.join(QLatin1Char(' '));
    };
    const int railWidth=board->width();
    EXPECT_EQ(QStringLiteral("1 2"),lamps());
    // A layer flip on the controller (the Numark's Shift+Tap) moves unit 1
    // to deck 3: its lamp reads 3.
    ControlObject::set(route(1,1),0); ControlObject::set(route(1,3),1); QTest::qWait(150);
    EXPECT_EQ(QStringLiteral("3 2"),lamps());
    EXPECT_TRUE(board->toolTip().contains(QStringLiteral("FX unit 1 on deck 3;"))) << board->toolTip().toStdString();
    EXPECT_TRUE(board->toolTip().contains(QStringLiteral("FX unit 2 on deck 2;")));
    EXPECT_EQ(railWidth,board->width());
    // Lab FX: 1, 2, MASTER and SMP; the slots of 3 and 4 stay reserved.
    panel->selectPage(3); QCoreApplication::processEvents();
    auto* fx=panel->findChild<DeckLabLabFx*>("decklab-labfx"); ASSERT_NE(nullptr,fx);
    const auto key=[this](int n) { return panel->findChild<QPushButton*>(QStringLiteral("decklab-fx-target-%1").arg(n)); };
    auto* onKey=panel->findChild<QPushButton*>("decklab-fx-on"); ASSERT_NE(nullptr,onKey);
    EXPECT_EQ(2,fx->deckLayout());
    for(int n:{1,2,5,6}) EXPECT_TRUE(key(n)->isVisible()) << n;
    for(int n:{3,4}) EXPECT_FALSE(key(n)->isVisible()) << n;
    const QRect master=key(5)->geometry(), on=onKey->geometry(), third=key(3)->geometry();
    capture(QStringLiteral("r6-fxstrip-labfx-two-deck.png"));
    auto* choice=panel->findChild<QComboBox*>("decklab-deck-layout"); ASSERT_NE(nullptr,choice);
    QMetaObject::invokeMethod(choice,"activated",Q_ARG(int,2)); QTest::qWait(150);
    EXPECT_EQ(4,fx->deckLayout());
    for(int n=1;n<=6;++n) EXPECT_TRUE(key(n)->isVisible()) << n;
    EXPECT_EQ(QStringLiteral("3 2 3 4"),lamps()) << "four decks: four lamps";
    EXPECT_EQ(master,key(5)->geometry()); EXPECT_EQ(on,onKey->geometry()); EXPECT_EQ(third,key(3)->geometry());
    capture(QStringLiteral("r6-fxstrip-labfx-four-deck.png"));
    QMetaObject::invokeMethod(choice,"activated",Q_ARG(int,1)); QTest::qWait(150);
    EXPECT_EQ(QStringLiteral("3 2"),lamps());
    EXPECT_FALSE(key(3)->isVisible());
    // A loaded deck 3 gets its key back, in its own slot.
    m_pMixerDeck3->loadFakeTrack(false, 124); ProcessBuffer(); QTest::qWait(50); settle();
    EXPECT_TRUE(key(3)->isVisible()); EXPECT_FALSE(key(4)->isVisible());
    EXPECT_EQ(QStringLiteral("3"),key(3)->text());
    EXPECT_EQ(third,key(3)->geometry()); EXPECT_EQ(master,key(5)->geometry()); EXPECT_EQ(on,onKey->geometry());
    capture(QStringLiteral("r6-fxstrip-labfx-deck3-loaded.png"));
    // ECHO latched on deck 1, then the left side flips to deck 3. Nothing is
    // retargeted: after the lift, the next touch plays deck 1 again and deck
    // 3 never gets the effect.
    const ConfigKey onDeck1("[DeckLabEffectUnit1]","group_[Channel1]_enable"), onDeck3("[DeckLabEffectUnit1]","group_[Channel3]_enable");
    ASSERT_EQ(1,fx->target());
    onKey->click(); ASSERT_TRUE(fx->isLatched());
    auto* laneRow=dynamic_cast<DeckLabLaneDecks*>(panel->findChild<QWidget*>("decklab-lane-decks")); ASSERT_NE(nullptr,laneRow);
    const auto lane=[this](int deck) { return panel->findChild<QWidget*>(QStringLiteral("decklab-detail-%1").arg(deck)); };
    ASSERT_EQ(1,laneRow->choice().lanes[0]); ASSERT_TRUE(lane(1)->isVisible()); ASSERT_FALSE(lane(3)->isVisible());
    ControlObject::set(ConfigKey("[DeckLabLeft]","current"),1); QTest::qWait(50); settle();
    // The flip really happened: the left side's lane is deck 3's now.
    EXPECT_EQ(3,laneRow->choice().lanes[0]);
    EXPECT_TRUE(lane(3)->isVisible()); EXPECT_FALSE(lane(1)->isVisible());
    EXPECT_EQ(1,fx->target()); EXPECT_TRUE(fx->isLatched());
    EXPECT_DOUBLE_EQ(1,ControlObject::get(onDeck1)); EXPECT_DOUBLE_EQ(0,ControlObject::get(onDeck3));
    onKey->click();
    EXPECT_EQ(DeckLabLabFx::Lamp::Tail,fx->lamp()) << "lift: deck 1's echo tail rings out";
    auto* xy=panel->findChild<QWidget*>("decklab-fx-xy"); ASSERT_NE(nullptr,xy);
    const QPoint point(xy->width()*3/4,xy->height()/4);
    QTest::mousePress(xy,Qt::LeftButton,Qt::NoModifier,point);
    EXPECT_TRUE(fx->isOn()); EXPECT_EQ(1,fx->target());
    EXPECT_DOUBLE_EQ(1,ControlObject::get(onDeck1)) << "the touch plays the deck the DJ selected";
    EXPECT_DOUBLE_EQ(0,ControlObject::get(onDeck3));
    QTest::mouseRelease(xy,Qt::LeftButton,Qt::NoModifier,point);
    panel->findChild<QPushButton*>("decklab-fx-cut")->click();
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[DeckLabEffectUnit1]","mix")));
    ControlObject::set(ConfigKey("[DeckLabLeft]","current"),0); QCoreApplication::processEvents();
}

TEST_F(NativeDeckLabTest, TwoDeckLayoutKeepsAUnitWetOffTheDecksOnTheRail) {
    // r6-fxstrip: a unit 3 or 4 that is wet only on the main mix (or the
    // headphones, a sampler...) is sounding: the 2-deck rail keeps its lamp,
    // lit, and the details sheet does not call it unused.
    m_pEffectsManager->setup();
    const auto manifests=m_pEffectsManager->getBackendManager()->getManifestsForBackend(EffectBackendType::BuiltIn);
    ASSERT_GT(manifests.size(),0);
    const auto unitKey=[](int unit,const QString& item) {
        return ConfigKey(QStringLiteral("[EffectRack1_EffectUnit%1]").arg(unit),item);
    };
    const QString main=QStringLiteral("group_[Master]_enable"), phones=QStringLiteral("group_[Headphone]_enable");
    for(int unit=1;unit<=4;++unit) {
        for(int deck=1;deck<=4;++deck)
            ControlObject::set(unitKey(unit,QStringLiteral("group_[Channel%1]_enable").arg(deck)),unit<=2 && deck==unit ? 1 : 0);
        for(const QString& other:{main,phones}) {
            ASSERT_NE(nullptr,ControlObject::getControl(unitKey(unit,other))) << other.toStdString();
            ControlObject::set(unitKey(unit,other),0);
        }
    }
    auto chain=m_pEffectsManager->getEffectChain(QStringLiteral("[EffectRack1_EffectUnit3]")); ASSERT_FALSE(chain.isNull());
    chain->getEffectSlot(0)->loadEffectWithDefaults(manifests[0]);
    for(int slot=1;slot<chain->getEffectSlots().size();++slot) chain->getEffectSlot(slot)->loadEffectWithDefaults(EffectManifestPointer());
    ControlObject::set(ConfigKey("[EffectRack1_EffectUnit3_Effect1]","enabled"),1);
    ControlObject::set(unitKey(3,"enabled"),1); ControlObject::set(unitKey(3,"mix"),.5);
    QTest::qWait(150);
    auto* board=panel->findChild<DeckLabChipButton*>("decklab-performance-board-fx"); ASSERT_NE(nullptr,board);
    const auto lamps=[board] {
        QStringList texts; for(const auto& chip:board->chips()) texts<<chip.text; return texts.join(QLatin1Char(' '));
    };
    const int railWidth=board->width();
    EXPECT_EQ(QStringLiteral("1 2"),lamps()) << "armed but routed nowhere: silent, no lamp";
    // Routed to the main mix only: wet. Its lamp shows, lit, by unit number.
    ControlObject::set(unitKey(3,main),1); QTest::qWait(150);
    EXPECT_EQ(QStringLiteral("1 2 3"),lamps());
    ASSERT_EQ(3,board->chips().size()); EXPECT_TRUE(board->chips().last().on);
    EXPECT_TRUE(board->toolTip().contains(QStringLiteral("FX unit 3 on main; %1; 50%").arg(manifests[0]->name())))
            << board->toolTip().toStdString();
    EXPECT_TRUE(board->accessibleDescription().contains(QStringLiteral("FX unit 3 on main")));
    EXPECT_EQ(railWidth,board->width());
    board->click(); QCoreApplication::processEvents();
    auto* dialog=panel->findChild<QDialog*>("decklab-controller-fx-info"); ASSERT_NE(nullptr,dialog);
    auto* text=dialog->findChild<QTextBrowser*>("decklab-controller-fx-text"); ASSERT_NE(nullptr,text);
    const QString sheet=text->toPlainText();
    EXPECT_TRUE(sheet.contains(QStringLiteral("Routed decks: none · also main\n"))) << sheet.toStdString();
    EXPECT_FALSE(sheet.section(QStringLiteral("Unit 3 ·"),1).section(QStringLiteral("Unit 4 ·"),0,0).contains(QStringLiteral("not used")));
    dialog->findChild<QPushButton*>("decklab-controller-fx-close")->click(); QCoreApplication::processEvents();
    // The headphones count too; with no route left the lamp goes.
    ControlObject::set(unitKey(3,main),0); ControlObject::set(unitKey(3,phones),1); QTest::qWait(150);
    EXPECT_EQ(QStringLiteral("1 2 3"),lamps());
    EXPECT_TRUE(board->toolTip().contains(QStringLiteral("FX unit 3 on headphones;"))) << board->toolTip().toStdString();
    ControlObject::set(unitKey(3,phones),0); QTest::qWait(150);
    EXPECT_EQ(QStringLiteral("1 2"),lamps());
    EXPECT_EQ(railWidth,board->width());
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
    // r6-sync: SYNC on deck 2 follows the master, lit while on.
    auto* sync=panel->findChild<QPushButton*>("decklab-2-beatsync"); ASSERT_NE(nullptr,sync); sync->click(); QCoreApplication::processEvents();
    EXPECT_TRUE(sync->property("lit").toBool()); EXPECT_FALSE(sync->isCheckable());
    EXPECT_DOUBLE_EQ(1,ControlObject::get(ConfigKey("[Channel2]","sync_enabled")));
    EXPECT_GT(ControlObject::get(ConfigKey("[Channel1]","sync_leader")),0);
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

// r5-decks (audit move 6): GRID mode replaces the decklab-beatgrid-editor
// dialog. These tests replace BeatgridEditorUsesRealGridControlsAndRefuses-
// UnsafeRelease and SharedTrackGridEditingRequiresEveryAliasedDeckToBeQuiet:
// the same real grid controls and stale-context guards, on the page, beside
// the waveform; a playing deck may now be edited (flagged EDITING GRID).
TEST_F(NativeDeckLabTest, GridModeEditsTheRealGridBesideItsWaveformAndOwnsTheKnob) {
    auto track=m_pMixerDeck1->loadFakeTrack(false,128); track->setTitle("Grid target");
    loadGenerated(2,126,mixxx::track::io::key::E_MINOR); ProcessBuffer();
    // bpmlock requests toggle regardless of the requested value; establish
    // fixture state through the track and verify the native control follows.
    track->setBpmLocked(false); QTest::qWait(150);
    ASSERT_FALSE(track->isBpmLocked());
    ASSERT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel1]","bpmlock")));
    // The lanes start zoomed out (linked across the decks).
    const auto zoomOf=[](int deck) { return ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck),"waveform_zoom")); };
    ControlObject::set(ConfigKey("[Channel1]","waveform_zoom"),4); QTest::qWait(20); settle();
    for(int deck=1;deck<=4;++deck) ASSERT_DOUBLE_EQ(4,zoomOf(deck)) << deck;
    // Holding a paused deck's BPM readout opens GRID mode; the release opens nothing.
    const QPoint bpm=strip(1)->renderer().bpmTarget().center();
    ASSERT_FALSE(bpm.isNull());
    QTest::mousePress(strip(1),Qt::LeftButton,Qt::NoModifier,bpm);
    QTest::qWait(DeckLabDeckStrip::kGridHoldMs+150);
    auto* grid=panel->findChild<DeckLabGridPanel*>("decklab-grid-mode"); ASSERT_NE(nullptr,grid);
    ASSERT_TRUE(grid->isVisible());
    QTest::mouseRelease(strip(1),Qt::LeftButton,Qt::NoModifier,bpm); QTest::qWait(20);
    EXPECT_EQ(nullptr,panel->findChild<QDialog*>("decklab-track-info"));
    EXPECT_EQ(nullptr,panel->findChild<QDialog*>("decklab-beatgrid-editor")) << "no dialog any more";
    // It takes deck 1's readout, overview and pads; deck 2 and the lanes stay.
    EXPECT_FALSE(strip(1)->isVisible());
    EXPECT_FALSE(panel->findChild<QWidget*>("decklab-pads-1")->isVisible());
    EXPECT_TRUE(strip(2)->isVisible());
    EXPECT_FALSE(strip(2)->hasFocus()) << "no focus frame handed to the other deck";
    EXPECT_TRUE(panel->findChild<QWidget*>("decklab-pads-2")->isVisible());
    auto* zone=panel->findChild<QWidget*>("decklab-wave-zone"); ASSERT_NE(nullptr,zone);
    auto* row=panel->findChild<QWidget*>("decklab-deck-row"); ASSERT_NE(nullptr,row);
    const QRect gridRect(grid->mapTo(panel.get(),QPoint()),grid->size());
    EXPECT_FALSE(gridRect.intersects(QRect(zone->mapTo(panel.get(),QPoint()),zone->size()))) << "nothing over the lanes";
    EXPECT_TRUE(QRect(row->mapTo(panel.get(),QPoint()),row->size()).contains(gridRect));
    EXPECT_LE(gridRect.width(),panel->width()/2+1);
    EXPECT_EQ(244,row->height()) << "the waveforms keep their height";
    EXPECT_EQ(1,grid->state().deck); EXPECT_EQ(QStringLiteral("128.00"),grid->state().bpm);
    EXPECT_EQ(QStringLiteral("PAUSED"),grid->playText());
    // Every lane zooms in to Mixxx's closest zoom while editing.
    settle();
    for(int deck=1;deck<=4;++deck) EXPECT_DOUBLE_EQ(DeckLabDeckTools::kGridZoom,zoomOf(deck)) << deck;
    // The browse knob belongs to GRID mode while it is on.
    EXPECT_EQ(grid,decklab::knobClaimant());
    EXPECT_TRUE(grid->ownsKnob());
    capture(QStringLiteral("decks-grid-mode.png"));
    const auto key=[grid](DeckLabGridPanel::Action action) { return grid->key(action); };
    using A=DeckLabGridPanel::Action;
    // GRID › on touch-down: Mixxx's beats_translate_later.
    const auto reference=mixxx::audio::FramePos(44100);
    const auto closest=[&track,&reference] { return track->getBeats()->findClosestBeat(reference).toEngineSamplePos(); };
    double oldBeat=closest();
    QTest::mousePress(key(A::NudgeLater),Qt::LeftButton); ProcessBuffer();
    EXPECT_GT(closest(),oldBeat) << "on touch-down";
    QTest::mouseRelease(key(A::NudgeLater),Qt::LeftButton);
    oldBeat=closest();
    QTest::mouseClick(key(A::NudgeEarlier),Qt::LeftButton); ProcessBuffer();
    EXPECT_LT(closest(),oldBeat);
    // DOWNBEAT: a beat at the playhead, and the beat count shows it as beat 1.
    ControlObject::set(ConfigKey("[Channel1]","playposition"),.317); ProcessBuffer(); settle();
    const auto position=m_pChannel1->getEngineBuffer()->getExactPlayPos();
    QTest::mouseClick(key(A::Downbeat),Qt::LeftButton); ProcessBuffer(); settle();
    EXPECT_NEAR(position.toEngineSamplePos(),track->getBeats()->findClosestBeat(position).toEngineSamplePos(),2);
    EXPECT_EQ(0,grid->state().beat) << "the readout's count: beat 1";
    EXPECT_EQ(0,strip(1)->state().phase);
    // ‹ BEAT / BEAT › slip the count a whole beat, the beats stay put.
    const double here=track->getBeats()->findClosestBeat(position).toEngineSamplePos();
    QTest::mouseClick(key(A::BeatLater),Qt::LeftButton); ProcessBuffer(); settle();
    EXPECT_NEAR(here,track->getBeats()->findClosestBeat(position).toEngineSamplePos(),2);
    EXPECT_EQ(3,grid->state().beat);
    QTest::mouseClick(key(A::BeatEarlier),Qt::LeftButton); QTest::mouseClick(key(A::BeatEarlier),Qt::LeftButton);
    ProcessBuffer(); settle();
    EXPECT_EQ(1,grid->state().beat);
    QTest::mouseClick(key(A::BeatLater),Qt::LeftButton); ProcessBuffer(); settle();
    EXPECT_EQ(0,grid->state().beat);
    // ÷2 / ×2 and the BPM steps (Mixxx's 0.01 adjust).
    QTest::mouseClick(key(A::Halve),Qt::LeftButton); ProcessBuffer(); EXPECT_NEAR(64,track->getBpm(),.001);
    QTest::mouseClick(key(A::Double),Qt::LeftButton); ProcessBuffer(); EXPECT_NEAR(128,track->getBpm(),.001);
    QTest::mouseClick(key(A::BpmUpFine),Qt::LeftButton); ProcessBuffer(); EXPECT_NEAR(128.01,track->getBpm(),.001);
    // ±0.1 is ten of Mixxx's 0.01 steps, each rounded to a nearby round BPM.
    QTest::mouseClick(key(A::BpmUpCoarse),Qt::LeftButton); ProcessBuffer(); EXPECT_NEAR(128.11,track->getBpm(),.006);
    QTest::mouseClick(key(A::BpmDownCoarse),Qt::LeftButton); ProcessBuffer(); EXPECT_NEAR(128.01,track->getBpm(),.006);
    QTest::mouseClick(key(A::BpmDownFine),Qt::LeftButton); ProcessBuffer(); EXPECT_NEAR(128,track->getBpm(),.001);
    settle(); EXPECT_EQ(QStringLiteral("128.00"),grid->state().bpm) << "the readout follows";
    // TAP drives bpm_tap on touch-down.
    int taps=0;
    ControlProxy tap("[Channel1]","bpm_tap");
    tap.connectValueChanged(grid,[&taps](double v) { taps+=v>0; },Qt::DirectConnection);
    QTest::mousePress(key(A::Tap),Qt::LeftButton); EXPECT_EQ(1,taps); QTest::mouseRelease(key(A::Tap),Qt::LeftButton);
    // The knob as the owner's controller sends it (the Numark overlay in
    // native mode: SelectTrackKnob / SelectPlaylist for a turn, browse_press
    // for a press, browse_back for Shift+press), through LibraryControl.
    oldBeat=closest();
    r4Pulse("[Playlist]","SelectTrackKnob",1); ProcessBuffer();
    EXPECT_GT(closest(),oldBeat) << "a turn nudges the grid";
    oldBeat=closest();
    r4Pulse("[Playlist]","SelectPlaylist",-1); ProcessBuffer();
    EXPECT_LT(closest(),oldBeat) << "SelectPlaylist turns the same way";
    r4Pulse("[DeckLabNative]","browse_press"); settle();
    EXPECT_EQ(DeckLabGridPanel::Knob::Bpm,grid->knob()) << "a press switches to the BPM";
    r4Pulse("[Playlist]","SelectTrackKnob",-1); ProcessBuffer();
    EXPECT_NEAR(127.99,track->getBpm(),.001);
    r4Pulse("[Playlist]","SelectTrackKnob",1); ProcessBuffer();
    EXPECT_NEAR(128,track->getBpm(),.001);
    // LOCK is the track's BPM lock; a locked grid refuses every edit.
    QTest::mouseClick(key(A::Lock),Qt::LeftButton); settle();
    ASSERT_TRUE(track->isBpmLocked()); EXPECT_TRUE(key(A::Lock)->lit());
    EXPECT_FALSE(key(A::Double)->isEnabled()); EXPECT_TRUE(grid->status().contains("locked"));
    QTest::mouseClick(key(A::Lock),Qt::LeftButton); settle();
    ASSERT_FALSE(track->isBpmLocked()); EXPECT_TRUE(key(A::Double)->isEnabled());
    // The live press-time guards: the engine changes and the press lands in
    // the same turn, before the panel can refresh (watches are queued).
    ControlObject::set(ConfigKey("[Channel1]","sync_enabled"),1);
    ASSERT_TRUE(key(A::Double)->isEnabled()) << "the panel has not refreshed yet";
    QTest::mouseClick(key(A::Double),Qt::LeftButton); ProcessBuffer();
    EXPECT_NEAR(128,track->getBpm(),.001) << "sync on: refused at touch-down";
    settle(); EXPECT_FALSE(key(A::NudgeLater)->isEnabled()); EXPECT_TRUE(grid->status().contains("SYNC"));
    ControlObject::set(ConfigKey("[Channel1]","sync_enabled"),0); ProcessBuffer(); settle();
    ASSERT_TRUE(key(A::NudgeLater)->isEnabled());
    track->setBpmLocked(true);
    ASSERT_TRUE(key(A::Halve)->isEnabled()) << "the panel has not refreshed yet";
    QTest::mouseClick(key(A::Halve),Qt::LeftButton); ProcessBuffer();
    EXPECT_NEAR(128,track->getBpm(),.001) << "locked: refused at touch-down";
    track->setBpmLocked(false); settle();
    // A playing deck can be edited, but says so in the deck colour.
    ControlObject::set(ConfigKey("[Channel1]","play"),1); ProcessBuffer(); settle();
    EXPECT_EQ(QStringLiteral("PLAYING"),grid->playText());
    EXPECT_TRUE(key(A::NudgeLater)->isEnabled());
    oldBeat=closest();
    QTest::mouseClick(key(A::NudgeLater),Qt::LeftButton); ProcessBuffer();
    EXPECT_GT(closest(),oldBeat);
    capture(QStringLiteral("decks-grid-mode-playing.png"));
    ControlObject::set(ConfigKey("[Channel1]","play"),0); ProcessBuffer(); settle();
    // Shift+press (browse_back) leaves GRID mode: the deck and the zoom come back.
    r4Pulse("[DeckLabNative]","browse_back"); settle(); QTest::qWait(20); settle();
    EXPECT_FALSE(grid->isVisible());
    EXPECT_TRUE(strip(1)->isVisible()); EXPECT_TRUE(panel->findChild<QWidget*>("decklab-pads-1")->isVisible());
    EXPECT_NE(grid,decklab::knobClaimant());
    for(int deck=1;deck<=4;++deck) EXPECT_DOUBLE_EQ(4,zoomOf(deck)) << deck << ": the zoom comes back";
    // MORE › GRID; a zoom changed meanwhile stays; DONE leaves.
    panel->findChild<QPushButton*>("decklab-1-more")->click(); QCoreApplication::processEvents();
    panel->findChild<QPushButton*>("decklab-1-grid")->click(); settle();
    ASSERT_TRUE(grid->isVisible()); EXPECT_EQ(1,grid->state().deck);
    EXPECT_DOUBLE_EQ(DeckLabDeckTools::kGridZoom,zoomOf(1));
    ControlObject::set(ConfigKey("[Channel2]","waveform_zoom"),6); QTest::qWait(20); settle();
    QTest::mouseClick(key(A::Done),Qt::LeftButton); settle();
    EXPECT_FALSE(grid->isVisible()); EXPECT_TRUE(strip(1)->isVisible());
    EXPECT_DOUBLE_EQ(6,zoomOf(1)) << "a zoom changed during GRID is kept";
    // Moving GRID straight to another deck keeps the lanes zoomed in, and
    // the zoom from before comes back at the end.
    auto* tools=panel->findChild<DeckLabDeckTools*>("decklab-deck-tools"); ASSERT_NE(nullptr,tools);
    tools->enterGrid(1); settle();
    tools->enterGrid(2); settle();
    ASSERT_EQ(2,tools->gridDeck());
    for(int deck=1;deck<=4;++deck) EXPECT_DOUBLE_EQ(DeckLabDeckTools::kGridZoom,zoomOf(deck)) << deck;
    EXPECT_TRUE(strip(1)->isVisible()) << "deck 1 back, deck 2 under GRID";
    EXPECT_FALSE(strip(2)->isVisible());
    tools->leaveGrid(); settle();
    for(int deck=1;deck<=4;++deck) EXPECT_DOUBLE_EQ(6,zoomOf(deck)) << deck;
    // One knob owner: a key sheet opened during GRID mode leaves it, and
    // GRID mode closes the sheets.
    tools->enterGrid(1); settle(); ASSERT_EQ(1,tools->gridDeck());
    QTest::mouseClick(strip(2),Qt::LeftButton,Qt::NoModifier,strip(2)->renderer().keyTarget().center()); settle();
    auto* sheet=panel->findChild<DeckLabKeySheet*>("decklab-key-2"); ASSERT_NE(nullptr,sheet);
    EXPECT_TRUE(sheet->isVisible());
    EXPECT_EQ(0,tools->gridDeck()) << "GRID mode gave way";
    EXPECT_EQ(sheet,decklab::knobClaimant());
    const double pitchTwo=ControlObject::get(ConfigKey("[Channel2]","pitch"));
    r4Pulse("[DeckLabNative]","browse_press"); settle();
    EXPECT_DOUBLE_EQ(pitchTwo,ControlObject::get(ConfigKey("[Channel2]","pitch"))) << "a blind press shifts nothing";
    tools->enterGrid(1); settle();
    EXPECT_FALSE(sheet->isVisible()) << "GRID mode closes the sheets";
    EXPECT_EQ(grid,decklab::knobClaimant()); EXPECT_TRUE(grid->ownsKnob());
    // Leaving the page, a layer change or a new track ends GRID mode.
    panel->selectPage(1); settle(); EXPECT_EQ(0,tools->gridDeck());
    panel->selectPage(0); settle(); EXPECT_TRUE(strip(1)->isVisible()); EXPECT_FALSE(grid->isVisible());
    tools->enterGrid(1); settle(); ASSERT_EQ(1,tools->gridDeck());
    ControlObject::set(ConfigKey("[DeckLabLeft]","current"),1); QTest::qWait(150); settle();
    EXPECT_EQ(0,tools->gridDeck()); EXPECT_FALSE(grid->isVisible());
    ControlObject::set(ConfigKey("[DeckLabLeft]","current"),0); QTest::qWait(150); settle();
    // The track-swap stale-touch guard: a press that lands between a load
    // and the screen's refresh never edits the new track.
    tools->enterGrid(1); settle(); ASSERT_EQ(1,tools->gridDeck());
    auto* later=key(A::NudgeLater);
    auto replacement=m_pMixerDeck1->loadFakeTrack(false,144);
    const double newBeat=replacement->getBeats()->findClosestBeat(reference).toEngineSamplePos();
    QTest::mousePress(later,Qt::LeftButton); QTest::mouseRelease(later,Qt::LeftButton);
    EXPECT_DOUBLE_EQ(newBeat,replacement->getBeats()->findClosestBeat(reference).toEngineSamplePos());
    ProcessBuffer(); QTest::qWait(150); settle();
    EXPECT_EQ(0,tools->gridDeck()); EXPECT_FALSE(grid->isVisible()); EXPECT_TRUE(strip(1)->isVisible());
    EXPECT_NEAR(144,replacement->getBpm(),.001);
    // A playing deck's BPM readout never opens GRID (MORE › GRID does).
    ControlObject::set(ConfigKey("[Channel1]","play"),1); ProcessBuffer(); settle();
    QTest::mousePress(strip(1),Qt::LeftButton,Qt::NoModifier,strip(1)->renderer().bpmTarget().center());
    QTest::qWait(DeckLabDeckStrip::kGridHoldMs+150);
    QTest::mouseRelease(strip(1),Qt::LeftButton,Qt::NoModifier,strip(1)->renderer().bpmTarget().center());
    EXPECT_EQ(0,tools->gridDeck()) << "a stray thumb on a live deck";
    ControlObject::set(ConfigKey("[Channel1]","play"),0); ProcessBuffer(); settle();
    // Four decks: the half of the deck row that holds the deck.
    loadGenerated(3,122,mixxx::track::io::key::C_MAJOR); loadGenerated(4,124,mixxx::track::io::key::G_MAJOR); ProcessBuffer();
    QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"),"activated",Q_ARG(int,2)); QTest::qWait(150); settle();
    panel->findChild<QPushButton*>("decklab-card-2-more")->click(); QCoreApplication::processEvents();
    panel->findChild<QPushButton*>("decklab-card-2-grid")->click(); settle();
    ASSERT_TRUE(grid->isVisible()); EXPECT_EQ(2,grid->state().deck);
    EXPECT_FALSE(panel->findChild<QWidget*>("decklab-card-1")->isVisible());
    EXPECT_FALSE(panel->findChild<QWidget*>("decklab-card-2")->isVisible());
    EXPECT_TRUE(panel->findChild<QWidget*>("decklab-card-3")->isVisible());
    EXPECT_TRUE(panel->findChild<QWidget*>("decklab-card-4")->isVisible());
    const QRect fourRect(grid->mapTo(panel.get(),QPoint()),grid->size());
    EXPECT_FALSE(fourRect.intersects(QRect(zone->mapTo(panel.get(),QPoint()),zone->size())));
    EXPECT_EQ(138+44+48,row->height());
    EXPECT_GE(fourRect.width(),panel->width()/2-4);
    capture(QStringLiteral("decks-four-grid-mode.png"));
    QTest::mouseClick(key(A::Done),Qt::LeftButton); settle();
    EXPECT_TRUE(panel->findChild<QWidget*>("decklab-card-1")->isVisible());
    EXPECT_TRUE(panel->findChild<QWidget*>("decklab-card-2")->isVisible());
    // Lanes side by side (1 2 over 3 4): GRID docks under its own lane's
    // column, never under another deck's lane.
    auto* waves=panel->findChild<QComboBox*>("decklab-waveform-layout"); ASSERT_NE(nullptr,waves);
    waves->setCurrentIndex(1); QMetaObject::invokeMethod(waves,"activated",Q_ARG(int,1)); QTest::qWait(150); settle();
    for(int deck:{1,2,3,4}) {
        tools->enterGrid(deck); settle();
        ASSERT_EQ(deck,tools->gridDeck());
        auto* lane=panel->findChild<QWidget*>(QStringLiteral("decklab-lane-%1").arg(deck)); ASSERT_NE(nullptr,lane);
        const QRect laneRect(lane->mapTo(panel.get(),QPoint()),lane->size());
        const QRect dock(grid->mapTo(panel.get(),QPoint()),grid->size());
        EXPECT_GE(dock.right(),laneRect.center().x()) << deck;
        EXPECT_LE(dock.left(),laneRect.center().x()) << deck << ": under its own lane";
        for(int other=1;other<=4;++other) {
            if(other==deck) continue;
            auto* otherLane=panel->findChild<QWidget*>(QStringLiteral("decklab-lane-%1").arg(other));
            const QRect otherRect(otherLane->mapTo(panel.get(),QPoint()),otherLane->size());
            if(otherRect.center().x()!=laneRect.center().x()) {
                EXPECT_FALSE(dock.contains(QPoint(otherRect.center().x(),dock.center().y()))) << deck << " " << other;
            }
        }
    }
    tools->leaveGrid(); settle();
    waves->setCurrentIndex(0); QMetaObject::invokeMethod(waves,"activated",Q_ARG(int,0)); QTest::qWait(150);
    for(int deck=1;deck<=4;++deck) EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck),"play")));
}

TEST_F(NativeDeckLabTest, GridModeWaitsWhileTheSameTrackPlaysOrSyncsOnAnotherDeck) {
    auto shared=Track::newTemporary(getTestDir().filePath(QStringLiteral("sine-30.wav")));
    loadTrack(m_pMixerDeck1.get(),shared); loadTrack(m_pMixerDeck2.get(),shared);
    ASSERT_EQ(shared,m_pMixerDeck1->getLoadedTrack()); ASSERT_EQ(shared,m_pMixerDeck2->getLoadedTrack());
    ASSERT_TRUE(shared->trySetBpm(128)); ProcessBuffer();
    shared->setBpmLocked(false); QTest::qWait(150);
    ASSERT_FALSE(shared->isBpmLocked());
    panel->findChild<QPushButton*>("decklab-1-more")->click(); QCoreApplication::processEvents();
    panel->findChild<QPushButton*>("decklab-1-grid")->click(); settle();
    auto* grid=panel->findChild<DeckLabGridPanel*>("decklab-grid-mode"); ASSERT_NE(nullptr,grid); ASSERT_TRUE(grid->isVisible());
    using A=DeckLabGridPanel::Action;
    auto* half=grid->key(A::Halve); auto* later=grid->key(A::NudgeLater); auto* lock=grid->key(A::Lock);
    ASSERT_TRUE(half->isEnabled()); ASSERT_TRUE(later->isEnabled()); ASSERT_TRUE(lock->isEnabled());
    const auto reference=mixxx::audio::FramePos(44100);
    for(int busy=0;busy<3;++busy) {
        const QString control=busy==0 ? QStringLiteral("play") : busy==1 ? QStringLiteral("sync_enabled") : QStringLiteral("sync_leader");
        const double beat=shared->getBeats()->findClosestBeat(reference).toEngineSamplePos();
        // The live guard: deck 2 starts and the presses land in the same
        // turn, before the panel knows (it still offers the keys).
        ControlObject::set(ConfigKey("[Channel2]",control),1);
        ASSERT_TRUE(half->isEnabled());
        for(auto* b:{half,later,lock}) QTest::mouseClick(b,Qt::LeftButton);
        ProcessBuffer();
        EXPECT_NEAR(128,shared->getBpm(),.001) << control.toStdString();
        EXPECT_DOUBLE_EQ(beat,shared->getBeats()->findClosestBeat(reference).toEngineSamplePos());
        EXPECT_FALSE(shared->isBpmLocked());
        // Refreshed: the keys are off and the status says what to do.
        QTest::qWait(150); settle();
        ASSERT_GT(ControlObject::get(ConfigKey("[Channel2]",control)),0);
        EXPECT_EQ(2,grid->state().sharedDeck) << control.toStdString();
        EXPECT_FALSE(half->isEnabled()); EXPECT_FALSE(later->isEnabled()); EXPECT_FALSE(lock->isEnabled());
        EXPECT_TRUE(grid->status().contains("Deck 2")) << grid->status().toStdString();
        EXPECT_TRUE(grid->status().contains("DONE")) << "its controls are under the panel";
        ControlObject::set(ConfigKey("[Channel2]","play"),0);
        ControlObject::set(ConfigKey("[Channel2]","sync_enabled"),0); ProcessBuffer(); QTest::qWait(150); settle();
    }
    ASSERT_TRUE(half->isEnabled()); QTest::mouseClick(half,Qt::LeftButton); ProcessBuffer();
    EXPECT_NEAR(64,shared->getBpm(),.001);
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel1]","play")));
    EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey("[Channel2]","play")));
    QTest::mouseClick(grid->key(A::Done),Qt::LeftButton); settle();
}

// r5-decks (owner's pick #3): one touch matches this deck's key to the other
// deck with Mixxx's sync_key; ±1 and RESET are Mixxx's pitch steps. The
// sheet docks in the deck area (never over the lanes) in both layouts.
TEST_F(NativeDeckLabTest, KeySheetMatchesShiftsAndResetsWithMixxxKeyControls) {
    loadGenerated(1,128,mixxx::track::io::key::A_MINOR);
    loadGenerated(2,126,mixxx::track::io::key::B_FLAT_MINOR); ProcessBuffer(); QTest::qWait(150); settle();
    // The waveform tick copies the engine key into visual_key, as the app's
    // VisualsManager does at the render rate.
    const auto visualTick=[] {
        for(int deck=1;deck<=4;++deck) {
            const QString group=QStringLiteral("[Channel%1]").arg(deck);
            ControlObject::set(ConfigKey(group,"visual_key"),ControlObject::get(ConfigKey(group,"key")));
        }
        settle();
    };
    const auto pitch=[](int deck) { return ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck),"pitch")); };
    const auto keylock=[](int deck) { return ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck),"keylock")); };
    const QString aMinor=KeyUtils::keyToString(mixxx::track::io::key::A_MINOR);
    const QString bFlatMinor=KeyUtils::keyToString(mixxx::track::io::key::B_FLAT_MINOR);
    ASSERT_EQ(aMinor,strip(1)->state().key); EXPECT_TRUE(strip(1)->state().keyShift.isEmpty());
    // Deck 1 rides +8% with key lock off: its pitch follows the tempo, and
    // the readout shows no key shift (none was made) and keeps its ✓.
    ControlObject::set(ConfigKey("[Channel1]","keylock"),0);
    ControlObject::set(ConfigKey("[Channel1]","rateRange"),0.08);
    ControlObject::set(ConfigKey("[Channel1]","rate"),1); ProcessBuffer(); settle();
    const double heard=pitch(1);
    ASSERT_GT(std::abs(heard),1.0) << "the tempo moved the pitch";
    visualTick();
    EXPECT_TRUE(strip(1)->state().keyShift.isEmpty()) << "riding the tempo is not a key shift";
    // Touch-down on a paused deck's KEY readout docks the sheet in place of the pads.
    auto* sheet=panel->findChild<DeckLabKeySheet*>("decklab-key-1"); ASSERT_NE(nullptr,sheet);
    EXPECT_FALSE(sheet->isVisible());
    const QPoint keyAt=strip(1)->renderer().keyTarget().center();
    QTest::mousePress(strip(1),Qt::LeftButton,Qt::NoModifier,keyAt);
    EXPECT_TRUE(sheet->isVisible()) << "on touch-down";
    QTest::mouseRelease(strip(1),Qt::LeftButton,Qt::NoModifier,keyAt); QTest::qWait(20);
    EXPECT_EQ(nullptr,panel->findChild<QDialog*>("decklab-track-info"));
    EXPECT_FALSE(panel->findChild<QWidget*>("decklab-pads-1-area")->isVisible());
    EXPECT_TRUE(panel->findChild<QPushButton*>("decklab-1-beatsync")->isVisible()) << "SYNC and MORE stay";
    auto* zone=panel->findChild<QWidget*>("decklab-wave-zone"); ASSERT_NE(nullptr,zone);
    const QRect sheetRect(sheet->mapTo(panel.get(),QPoint()),sheet->size());
    EXPECT_FALSE(sheetRect.intersects(QRect(zone->mapTo(panel.get(),QPoint()),zone->size()))) << "nothing over the lanes";
    EXPECT_EQ(244,panel->findChild<QWidget*>("decklab-deck-row")->height());
    EXPECT_EQ(2,sheet->state().target);
    EXPECT_EQ(QString(QStringLiteral("MATCH TO DECK 2 · ")+bFlatMinor),sheet->info()->text());
    EXPECT_EQ(sheet,decklab::knobClaimant()) << "the knob drives the sheet";
    capture(QStringLiteral("decks-key-sheet.png"));
    using K=DeckLabKeySheet::Action;
    // +1 at +8% with key lock off: key lock turns on first and the deck
    // lands one semitone above what it played, not above the track's key.
    QTest::mousePress(sheet->key(K::Up),Qt::LeftButton);
    EXPECT_DOUBLE_EQ(1,keylock(1)) << "key lock first, on touch-down";
    EXPECT_NEAR(heard+1,pitch(1),.02) << "heard pitch + 1";
    QTest::mouseRelease(sheet->key(K::Up),Qt::LeftButton);
    QTest::mouseClick(sheet->key(K::Down),Qt::LeftButton);
    EXPECT_NEAR(heard,pitch(1),.02) << "and back down a semitone";
    // RESET: the track's own key, and key lock off again (the sheet turned
    // it on; Mixxx would keep it forever).
    QTest::mouseClick(sheet->key(K::Reset),Qt::LeftButton); visualTick();
    EXPECT_DOUBLE_EQ(0,keylock(1));
    EXPECT_NEAR(heard,pitch(1),.02) << "the deck plays as it did before the sheet";
    EXPECT_TRUE(strip(1)->state().keyShift.isEmpty());
    // −1 with key lock off: the same rule, downwards.
    QTest::mousePress(sheet->key(K::Down),Qt::LeftButton);
    EXPECT_DOUBLE_EQ(1,keylock(1)); EXPECT_NEAR(heard-1,pitch(1),.02);
    QTest::mouseRelease(sheet->key(K::Down),Qt::LeftButton);
    QTest::mouseClick(sheet->key(K::Reset),Qt::LeftButton); settle();
    EXPECT_DOUBLE_EQ(0,keylock(1));
    // MATCH on touch-down: key lock on, then Mixxx's sync_key from the
    // track's key (A minor is a semitone under B flat minor), whatever the tempo.
    QTest::mousePress(sheet->key(K::Match),Qt::LeftButton);
    EXPECT_DOUBLE_EQ(1,keylock(1));
    EXPECT_NEAR(1,pitch(1),.01);
    EXPECT_EQ(static_cast<double>(mixxx::track::io::key::B_FLAT_MINOR),ControlObject::get(ConfigKey("[Channel1]","key")));
    QTest::mouseRelease(sheet->key(K::Match),Qt::LeftButton);
    visualTick();
    EXPECT_EQ(bFlatMinor,strip(1)->state().key);
    EXPECT_EQ(QStringLiteral("+1"),strip(1)->state().keyShift) << "the readout shows the key and its shift";
    EXPECT_TRUE(strip(1)->state().keyMatched) << "the key turns the match green";
    EXPECT_TRUE(sheet->key(K::Match)->lit());
    EXPECT_EQ(QString(QStringLiteral("MATCHED · DECK 2 ")+bFlatMinor),sheet->info()->text());
    capture(QStringLiteral("decks-key-sheet-matched.png"));
    // The matched deck drifts more than a tenth of a semitone: no longer MATCHED.
    ControlObject::set(ConfigKey("[Channel2]","pitch"),.3); settle(); visualTick();
    EXPECT_FALSE(strip(1)->state().keyMatched) << "a drifting target ends the match";
    ControlObject::set(ConfigKey("[Channel2]","pitch"),0); settle();
    // The knob as the owner's controller sends it: the first press only
    // shows the cursor (on DONE), turns walk the layout, a press acts,
    // Shift+press (browse_back) closes.
    r4Pulse("[DeckLabNative]","browse_press"); settle(); visualTick();
    EXPECT_TRUE(sheet->knobCursorShown()); EXPECT_EQ(K::Done,sheet->knobFocus());
    EXPECT_NEAR(1,pitch(1),.01) << "a blind press shifts nothing";
    r4Pulse("[Playlist]","SelectTrackKnob",2);
    EXPECT_EQ(K::Match,sheet->knobFocus());
    r4Pulse("[Playlist]","SelectPlaylist",-1);
    EXPECT_EQ(K::Down,sheet->knobFocus()) << "SelectPlaylist turns the same way";
    r4Pulse("[Playlist]","SelectTrackKnob",1);
    r4Pulse("[DeckLabNative]","browse_press"); settle(); visualTick();
    EXPECT_TRUE(strip(1)->state().keyMatched) << "the knob's press on MATCH";
    r4Pulse("[DeckLabNative]","browse_back"); settle();
    EXPECT_FALSE(sheet->isVisible()); EXPECT_TRUE(panel->findChild<QWidget*>("decklab-pads-1-area")->isVisible());
    // A second touch on KEY closes the sheet; the readout keeps the shift.
    QTest::mouseClick(strip(1),Qt::LeftButton,Qt::NoModifier,keyAt); ASSERT_TRUE(sheet->isVisible());
    QTest::mouseClick(strip(1),Qt::LeftButton,Qt::NoModifier,keyAt); EXPECT_FALSE(sheet->isVisible());
    EXPECT_EQ(QStringLiteral("+1"),strip(1)->state().keyShift);
    // Stale guards: a layer change closes the sheet; a press that lands
    // between a track load and the refresh shifts nothing.
    QTest::mouseClick(strip(1),Qt::LeftButton,Qt::NoModifier,keyAt); ASSERT_TRUE(sheet->isVisible());
    ControlObject::set(ConfigKey("[DeckLabLeft]","current"),1); QTest::qWait(150); settle();
    EXPECT_FALSE(sheet->isVisible());
    ControlObject::set(ConfigKey("[DeckLabLeft]","current"),0); QTest::qWait(150); settle();
    // MORE › KEY, beside KEY LOCK, opens it too.
    panel->findChild<QPushButton*>("decklab-1-more")->click(); QCoreApplication::processEvents();
    auto* moreKey=panel->findChild<QPushButton*>("decklab-1-key-sheet"); ASSERT_NE(nullptr,moreKey);
    EXPECT_TRUE(moreKey->isVisible()); EXPECT_EQ(44,moreKey->height());
    auto* moreLock=panel->findChild<QPushButton*>("decklab-1-keylock"); ASSERT_NE(nullptr,moreLock);
    EXPECT_EQ(moreLock->y(),moreKey->y()) << "next to KEY LOCK";
    moreKey->click(); settle();
    ASSERT_TRUE(sheet->isVisible());
    ASSERT_DOUBLE_EQ(1,keylock(1)) << "still on from MATCH, the sheet's";
    auto* up=sheet->key(K::Up);
    auto replacement=deckPlayer(1)->loadFakeTrack(false,130);
    const double pitchBefore=pitch(1); // after Mixxx's own load handling
    QTest::mousePress(up,Qt::LeftButton); QTest::mouseRelease(up,Qt::LeftButton);
    EXPECT_NEAR(pitchBefore,pitch(1),.01) << "the press landed before the refresh: nothing";
    ProcessBuffer(); QTest::qWait(150); settle();
    EXPECT_FALSE(sheet->isVisible()) << "a new track closes the sheet";
    EXPECT_FALSE(strip(1)->state().keyMatched);
    EXPECT_DOUBLE_EQ(0,keylock(1)) << "the next track gets the deck's key lock back";
    replacement->setKey(mixxx::track::io::key::C_MAJOR,mixxx::track::io::key::USER);
    ControlObject::set(ConfigKey("[Channel1]","rate"),0); ProcessBuffer(); settle(); visualTick();
    // A playing deck's KEY readout: a brush does nothing; a short hold opens.
    ControlObject::set(ConfigKey("[Channel1]","play"),1); ProcessBuffer(); settle();
    QTest::mouseClick(strip(1),Qt::LeftButton,Qt::NoModifier,keyAt); QTest::qWait(DeckLabDeckStrip::kLiveKeyHoldMs+100);
    EXPECT_FALSE(sheet->isVisible()) << "a stray thumb never swaps a live deck's pads";
    EXPECT_TRUE(panel->findChild<QWidget*>("decklab-pads-1-area")->isVisible());
    QTest::mousePress(strip(1),Qt::LeftButton,Qt::NoModifier,keyAt);
    EXPECT_FALSE(sheet->isVisible()) << "not on touch-down";
    QTest::qWait(DeckLabDeckStrip::kLiveKeyHoldMs+100);
    EXPECT_TRUE(sheet->isVisible()) << "a deliberate hold";
    QTest::mouseRelease(strip(1),Qt::LeftButton,Qt::NoModifier,keyAt);
    QTest::mouseClick(sheet->key(K::Done),Qt::LeftButton);
    ControlObject::set(ConfigKey("[Channel1]","play"),0); ProcessBuffer(); settle();
    // Four decks: the card's overview and transport rows give way, and a
    // short line names MATCH's deck.
    QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"),"activated",Q_ARG(int,2)); QTest::qWait(150); settle();
    auto* card=panel->findChild<DeckLabKeySheet*>("decklab-card-key-2"); ASSERT_NE(nullptr,card);
    const QPoint cardKey=strip(2)->renderer().keyTarget().center();
    ASSERT_FALSE(cardKey.isNull());
    QTest::mousePress(strip(2),Qt::LeftButton,Qt::NoModifier,cardKey);
    EXPECT_TRUE(card->isVisible());
    QTest::mouseRelease(strip(2),Qt::LeftButton,Qt::NoModifier,cardKey);
    EXPECT_FALSE(panel->findChild<QWidget*>("decklab-overview-2")->isVisible());
    EXPECT_FALSE(panel->findChild<QWidget*>("decklab-card-2-more")->isVisible());
    EXPECT_EQ(138+44+48,panel->findChild<QWidget*>("decklab-deck-row")->height());
    const QRect cardRect(card->mapTo(panel.get(),QPoint()),card->size());
    EXPECT_FALSE(cardRect.intersects(QRect(zone->mapTo(panel.get(),QPoint()),zone->size())));
    EXPECT_TRUE(QRect(strip(2)->parentWidget()->mapTo(panel.get(),QPoint()),strip(2)->parentWidget()->size()).contains(cardRect));
    for(auto action:{K::Down,K::Match,K::Up,K::Reset,K::Done}) {
        EXPECT_EQ(44,card->key(action)->height()); EXPECT_GE(card->key(action)->width(),44);
    }
    EXPECT_TRUE(card->info()->isVisible());
    EXPECT_TRUE(card->info()->text().startsWith(QStringLiteral("TO 1"))) << card->info()->text().toStdString();
    QTest::mouseClick(card->key(K::Match),Qt::LeftButton); visualTick();
    // C major on deck 1 now: deck 2 matches it (Mixxx picks the target).
    EXPECT_EQ(1,card->state().target);
    EXPECT_TRUE(strip(2)->state().keyMatched);
    EXPECT_EQ(QStringLiteral("MATCHED 1"),card->info()->text());
    EXPECT_TRUE(KeyUtils::getCompatibleKeys(KeyUtils::keyFromNumericValue(ControlObject::get(ConfigKey("[Channel1]","key"))))
                    .contains(KeyUtils::keyFromNumericValue(ControlObject::get(ConfigKey("[Channel2]","key")))));
    capture(QStringLiteral("decks-four-key-sheet.png"));
    QTest::mouseClick(card->key(K::Done),Qt::LeftButton); settle();
    EXPECT_FALSE(card->isVisible());
    EXPECT_TRUE(panel->findChild<QWidget*>("decklab-overview-2")->isVisible());
    EXPECT_TRUE(panel->findChild<QWidget*>("decklab-card-2-more")->isVisible());
    for(int deck=1;deck<=4;++deck) EXPECT_DOUBLE_EQ(0,ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck),"play")));
}

// r5-decks: the sheet's MATCH deck is Mixxx's sync_key target
// (EngineSync::pickNonSyncSyncTarget) with several candidates: the sync
// leader, else a playing deck, else a loaded one; the result mixes with it.
TEST_F(NativeDeckLabTest, KeySheetFollowsMixxxsSyncKeyTargetWithSeveralDecks) {
    loadGenerated(1,124,mixxx::track::io::key::E_MINOR);
    loadGenerated(2,126,mixxx::track::io::key::C_MAJOR);
    loadGenerated(3,128,mixxx::track::io::key::F_SHARP_MINOR); ProcessBuffer(); QTest::qWait(150); settle();
    auto* tools=panel->findChild<DeckLabDeckTools*>("decklab-deck-tools"); ASSERT_NE(nullptr,tools);
    EXPECT_EQ(1,tools->keySyncTarget(2)) << "nothing plays: the first loaded deck";
    ControlObject::set(ConfigKey("[Channel3]","play"),1); ProcessBuffer(); settle();
    EXPECT_EQ(3,tools->keySyncTarget(2)) << "a playing deck before a paused one";
    tools->openKeySheet(2); settle();
    auto* sheet=panel->findChild<DeckLabKeySheet*>("decklab-key-2"); ASSERT_NE(nullptr,sheet); ASSERT_TRUE(sheet->isVisible());
    EXPECT_EQ(3,sheet->state().target);
    const auto compatible=[](int deck,int with) {
        return KeyUtils::getCompatibleKeys(KeyUtils::keyFromNumericValue(ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(with),"key"))))
                .contains(KeyUtils::keyFromNumericValue(ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck),"key"))));
    };
    QTest::mouseClick(sheet->key(DeckLabKeySheet::Action::Match),Qt::LeftButton); settle();
    EXPECT_TRUE(compatible(2,3)) << "Mixxx matched the playing deck";
    EXPECT_EQ(3,sheet->state().matchedDeck);
    QTest::mouseClick(sheet->key(DeckLabKeySheet::Action::Reset),Qt::LeftButton); settle();
    ControlObject::set(ConfigKey("[Channel1]","sync_leader"),1); ProcessBuffer(); settle();
    if(ControlObject::get(ConfigKey("[Channel1]","sync_leader"))>0) {
        EXPECT_EQ(1,tools->keySyncTarget(2)) << "the sync leader first";
        EXPECT_EQ(1,sheet->state().target);
        QTest::mouseClick(sheet->key(DeckLabKeySheet::Action::Match),Qt::LeftButton); settle();
        EXPECT_TRUE(compatible(2,1)) << "Mixxx matched the leader";
        EXPECT_EQ(1,sheet->state().matchedDeck);
    }
    ControlObject::set(ConfigKey("[Channel1]","sync_leader"),0);
    ControlObject::set(ConfigKey("[Channel3]","play"),0); ProcessBuffer(); settle();
    QTest::mouseClick(sheet->key(DeckLabKeySheet::Action::Done),Qt::LeftButton);
}

// r5-decks: −1 / +1 keep the heard pitch in every state Mixxx can be in
// (the engine never holds the track's own key in between: a recorder on
// [Channel1],pitch sees only the heard pitch and the new one), and key lock
// goes back only where the sheet turned it on, also after a restart.
TEST_F(NativeDeckLabTest, KeySheetStepsKeepTheHeardPitchAndGiveKeyLockBack) {
    loadGenerated(1,126,mixxx::track::io::key::A_MINOR);
    loadGenerated(2,128,mixxx::track::io::key::B_FLAT_MINOR); ProcessBuffer(); QTest::qWait(150); settle();
    const auto get=[](int deck,const char* item) { return ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck),item)); };
    const auto put=[](int deck,const char* item,double v) { ControlObject::set(ConfigKey(QStringLiteral("[Channel%1]").arg(deck),item),v); };
    auto* tools=panel->findChild<DeckLabDeckTools*>("decklab-deck-tools"); ASSERT_NE(nullptr,tools);
    tools->openKeySheet(1); settle();
    auto* sheet=panel->findChild<DeckLabKeySheet*>("decklab-key-1"); ASSERT_NE(nullptr,sheet); ASSERT_TRUE(sheet->isVisible());
    using K=DeckLabKeySheet::Action;
    QList<double> seen;
    ControlProxy recorder("[Channel1]","pitch");
    recorder.connectValueChanged(panel.get(),[&seen](double v) { seen << v; },Qt::DirectConnection);
    // Presses +1 (or −1) with key lock off and checks every pitch the
    // engine could have played on the way, then RESET.
    const auto step=[&](K action,const char* what) {
        ASSERT_DOUBLE_EQ(0,get(1,"keylock")) << what;
        const double heard=get(1,"pitch");
        const double target=heard+(action==K::Up ? 1 : -1);
        const double mode=get(1,"keylockMode");
        seen.clear();
        QTest::mousePress(sheet->key(action),Qt::LeftButton);
        EXPECT_DOUBLE_EQ(1,get(1,"keylock")) << what << ": key lock first";
        EXPECT_NEAR(target,get(1,"pitch"),.02) << what << ": the heard pitch, one semitone on";
        for(double v:std::as_const(seen)) {
            EXPECT_TRUE(std::abs(v-heard)<.02 || std::abs(v-target)<.02) << what << ": the engine held " << v
                    << " between " << heard << " and " << target;
        }
        EXPECT_DOUBLE_EQ(mode,get(1,"keylockMode")) << what << ": the DJ's key lock mode comes back";
        QTest::mouseRelease(sheet->key(action),Qt::LeftButton); settle();
        seen.clear();
        QTest::mouseClick(sheet->key(K::Reset),Qt::LeftButton); settle();
        EXPECT_DOUBLE_EQ(0,get(1,"keylock")) << what << ": RESET gives key lock back";
        EXPECT_NEAR(heard,get(1,"pitch"),.02) << what << ": and the deck plays as before";
        for(double v:std::as_const(seen)) {
            EXPECT_TRUE(std::abs(v-heard)<.02 || std::abs(v-target)<.02) << what << ": RESET passed through " << v;
        }
    };
    put(1,"keylock",0); put(1,"rateRange",.08); settle();
    put(1,"rate",1); ProcessBuffer(); settle();
    ASSERT_GT(std::abs(get(1,"pitch")),1.0);
    step(K::Up,"+8%");
    put(1,"rate",-1); ProcessBuffer(); settle();
    ASSERT_GT(std::abs(get(1,"pitch")),1.0);
    step(K::Up,"−8%");
    step(K::Down,"−8% down");
    // pitch_adjust +0.5 at rate 0: heard 0.5, +1 gives 1.5, RESET gives the
    // 0.5 adjust back.
    put(1,"rate",0); ProcessBuffer(); settle();
    put(1,"pitch_adjust",.5); settle();
    ASSERT_NEAR(.5,get(1,"pitch"),.02);
    step(K::Up,"adjust +0.5");
    EXPECT_NEAR(.5,get(1,"pitch_adjust"),.02) << "the pre-sheet adjust is back";
    put(1,"pitch_adjust",0); settle();
    // |heard| beyond 5: no clamp shortens or turns the step round.
    put(1,"rateRange",.5); put(1,"rate",1); ProcessBuffer(); settle();
    ASSERT_GT(std::abs(get(1,"pitch")),5.0);
    step(K::Up,"wide range");
    put(1,"rate",0); put(1,"rateRange",.08); ProcessBuffer(); settle();
    // A sync follower (126 following 128).
    put(2,"sync_leader",1); ProcessBuffer();
    put(1,"sync_enabled",1); put(1,"play",1); put(2,"play",1);
    for(int pass=0;pass<5;++pass) ProcessBuffer();
    settle();
    put(1,"play",0); put(2,"play",0); ProcessBuffer(); settle();
    ASSERT_GT(std::abs(get(1,"rate_ratio")-1),.005) << "deck 1 follows deck 2";
    step(K::Up,"sync follower");
    put(1,"sync_enabled",0); put(2,"sync_leader",0); put(2,"sync_enabled",0); ProcessBuffer();
    put(1,"rate",0); ProcessBuffer(); settle();
    // KeepLockedKey (Mixxx Preferences > Decks): RESET still gives back
    // the pitch the tempo set, not the track's key.
    put(1,"keyunlockMode",1); put(1,"rate",1); ProcessBuffer(); settle();
    step(K::Up,"KeepLockedKey");
    EXPECT_NEAR(0,get(1,"pitch_adjust"),.02);
    put(1,"keyunlockMode",0); put(1,"rate",0); ProcessBuffer(); settle();
    // The DJ's own key lock is never turned off: not by RESET, not by the
    // next track.
    put(1,"keylock",1); settle();
    QTest::mouseClick(sheet->key(K::Match),Qt::LeftButton); settle();
    QTest::mouseClick(sheet->key(K::Up),Qt::LeftButton); settle();
    EXPECT_FALSE(tools->keylockBySheet(1));
    QTest::mouseClick(sheet->key(K::Reset),Qt::LeftButton); settle();
    EXPECT_DOUBLE_EQ(1,get(1,"keylock")) << "the DJ's key lock stays";
    EXPECT_NEAR(0,get(1,"pitch"),.01) << "RESET: the track's key";
    deckPlayer(1)->loadFakeTrack(false,124)->setKey(mixxx::track::io::key::A_MINOR,mixxx::track::io::key::USER);
    ProcessBuffer(); QTest::qWait(150); settle();
    EXPECT_DOUBLE_EQ(1,get(1,"keylock")) << "and stays with the next track";
    // A manual toggle in between hands key lock back to the DJ.
    put(1,"keylock",0); settle();
    tools->openKeySheet(1); settle(); ASSERT_TRUE(sheet->isVisible());
    QTest::mouseClick(sheet->key(K::Match),Qt::LeftButton); settle();
    ASSERT_TRUE(tools->keylockBySheet(1));
    put(1,"keylock",0); settle(); // a controller's KEY LOCK, off...
    put(1,"keylock",1); settle(); // ...and on again
    EXPECT_FALSE(tools->keylockBySheet(1)) << "the DJ's now";
    QTest::mouseClick(sheet->key(K::Reset),Qt::LeftButton); settle();
    EXPECT_DOUBLE_EQ(1,get(1,"keylock"));
    deckPlayer(1)->loadFakeTrack(false,125)->setKey(mixxx::track::io::key::A_MINOR,mixxx::track::io::key::USER);
    ProcessBuffer(); QTest::qWait(150); settle();
    EXPECT_DOUBLE_EQ(1,get(1,"keylock")) << "still the DJ's with the next track";
    // A crash restart (no teardown ran) with the track gone: the saved
    // record is stale; it goes, and key lock stays (it may be the DJ's).
    put(1,"keylock",0); settle();
    tools->openKeySheet(1); settle(); ASSERT_TRUE(sheet->isVisible());
    QTest::mouseClick(sheet->key(K::Match),Qt::LeftButton); settle();
    ASSERT_DOUBLE_EQ(1,get(1,"keylock"));
    EXPECT_EQ(QStringLiteral("1"),config()->getValueString(ConfigKey("[DeckLab]","KeySheetKeylock1"))) << "saved";
    {
        DeckLabDeckTools::Host host;
        host.read=[](int deck,const QString& item) { return ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck),item)); };
        host.write=[](int deck,const QString& item,double v) { ControlObject::set(ConfigKey(QStringLiteral("[Channel%1]").arg(deck),item),v); };
        host.track=[this](int deck) { return deckPlayer(deck)->getLoadedTrack(); };
        host.setting=[this](const QString& item) { return config()->getValueString(ConfigKey("[DeckLab]",item)); };
        host.setSetting=[this](const QString& item,const QString& value) { config()->set(ConfigKey("[DeckLab]",item),ConfigValue(value)); };
        DeckLabDeckTools restarted(host,nullptr);
        EXPECT_DOUBLE_EQ(1,get(1,"keylock")) << "a stale record never turns key lock off";
        EXPECT_EQ(QStringLiteral("0"),config()->getValueString(ConfigKey("[DeckLab]","KeySheetKeylock1"))) << "it is dropped";
        EXPECT_FALSE(restarted.keylockBySheet(1));
    }
    settle();
    for(int deck=1;deck<=4;++deck) EXPECT_DOUBLE_EQ(0,get(deck,"play"));
}

// r5-decks: a skin rebuild keeps a playing deck's pitch and the key lock
// the sheet turned on (the audio runs on; the next surface takes it over),
// and only a real quit hands key lock back, the pitch restored as RESET
// does, so a build without the sheet never inherits it. On the next start a
// stale record goes: key lock off, or a record of this build for another
// track, leave key lock alone (it may be the DJ's); a record from an
// earlier round-5 build, which never handed back at quit, is kept to its
// promise and handed back.
TEST_F(NativeDeckLabTest, KeySheetKeepsKeyLockThroughASkinRebuildAndHandsItBackAtQuit) {
    auto track=Track::newTemporary(getTestDir().filePath(QStringLiteral("sine-30.wav")));
    loadTrack(m_pMixerDeck1.get(),track); ASSERT_TRUE(track->trySetBpm(126));
    track->setKey(mixxx::track::io::key::A_MINOR,mixxx::track::io::key::USER);
    ASSERT_FALSE(track->getLocation().isEmpty());
    loadGenerated(2,128,mixxx::track::io::key::B_FLAT_MINOR); ProcessBuffer(); QTest::qWait(150); settle();
    const auto get=[](int deck,const char* item) { return ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck),item)); };
    const auto put=[](int deck,const char* item,double v) { ControlObject::set(ConfigKey(QStringLiteral("[Channel%1]").arg(deck),item),v); };
    const auto record=[this](const char* item) { return config()->getValueString(ConfigKey("[DeckLab]",QString::fromLatin1(item))); };
    const auto setRecord=[this](const char* item,const QString& value) { config()->set(ConfigKey("[DeckLab]",QString::fromLatin1(item)),ConfigValue(value)); };
    // Deck 1 plays at +8% with key lock off; +1 from the sheet.
    put(1,"keylock",0); put(1,"rateRange",.08); put(1,"rate",1); put(1,"play",1); ProcessBuffer(); settle();
    const double heard=get(1,"pitch");
    ASSERT_GT(std::abs(heard),1.0);
    auto* tools=panel->findChild<DeckLabDeckTools*>("decklab-deck-tools"); ASSERT_NE(nullptr,tools);
    tools->openKeySheet(1); settle();
    auto* sheet=panel->findChild<DeckLabKeySheet*>("decklab-key-1"); ASSERT_NE(nullptr,sheet); ASSERT_TRUE(sheet->isVisible());
    QTest::mouseClick(sheet->key(DeckLabKeySheet::Action::Up),Qt::LeftButton); settle();
    ASSERT_DOUBLE_EQ(1,get(1,"keylock")); ASSERT_NEAR(heard+1,get(1,"pitch"),.02);
    ASSERT_EQ(QStringLiteral("1"),record("KeySheetKeylock1"));
    EXPECT_EQ(QStringLiteral("2"),record("KeySheetKeylockVersion1"));
    // A skin rebuild: the surface goes while the deck plays on. Its pitch and
    // key lock stay, and so does the record.
    QList<double> seen;
    ControlProxy recorder("[Channel1]","pitch");
    recorder.connectValueChanged(&recorder,[&seen](double v) { seen << v; },Qt::DirectConnection);
    panel.reset();
    EXPECT_TRUE(seen.isEmpty()) << "a rebuild never moves a playing deck's pitch";
    EXPECT_DOUBLE_EQ(1,get(1,"keylock"));
    EXPECT_NEAR(heard+1,get(1,"pitch"),.02);
    EXPECT_EQ(QStringLiteral("1"),record("KeySheetKeylock1"));
    // The rebuilt surface takes the ownership over: same track, key lock on.
    panel=std::make_unique<WDeckLab>(config(),nullptr,players.get(),m_pEffectsManager.get());
    QCoreApplication::processEvents();
    tools=panel->findChild<DeckLabDeckTools*>("decklab-deck-tools"); ASSERT_NE(nullptr,tools);
    EXPECT_TRUE(tools->keylockBySheet(1)) << "still the sheet's after the rebuild";
    EXPECT_DOUBLE_EQ(1,get(1,"keylock"));
    EXPECT_TRUE(seen.isEmpty());
    // The real quit (QCoreApplication::aboutToQuit): key lock back, the deck
    // at the pitch it played before the sheet, with no snap, the record gone.
    tools->applicationQuitting();
    EXPECT_DOUBLE_EQ(0,get(1,"keylock")) << "handed back at quit";
    EXPECT_NEAR(heard,get(1,"pitch"),.02);
    for(double v:std::as_const(seen)) {
        EXPECT_TRUE(std::abs(v-heard)<.02 || std::abs(v-heard-1)<.02) << "no snap on the way: " << v;
    }
    EXPECT_EQ(QStringLiteral("0"),record("KeySheetKeylock1"));
    put(1,"play",0); put(1,"rate",0); ProcessBuffer();
    DeckLabDeckTools::Host host;
    host.read=[](int deck,const QString& item) { return ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck),item)); };
    host.write=[](int deck,const QString& item,double v) { ControlObject::set(ConfigKey(QStringLiteral("[Channel%1]").arg(deck),item),v); };
    host.track=[this](int deck) { return deckPlayer(deck)->getLoadedTrack(); };
    host.setting=[this](const QString& item) { return config()->getValueString(ConfigKey("[DeckLab]",item)); };
    host.setSetting=[this](const QString& item,const QString& value) { config()->set(ConfigKey("[DeckLab]",item),ConfigValue(value)); };
    // Stale: key lock off. Dropped.
    setRecord("KeySheetKeylock1",QStringLiteral("1")); setRecord("KeySheetKeylockTrack1",QStringLiteral("gone.wav"));
    setRecord("KeySheetKeylockVersion1",QStringLiteral("2"));
    put(1,"keylock",0);
    {
        DeckLabDeckTools next(host,nullptr);
        EXPECT_EQ(QStringLiteral("0"),record("KeySheetKeylock1"));
        EXPECT_DOUBLE_EQ(0,get(1,"keylock"));
        EXPECT_FALSE(next.keylockBySheet(1));
    }
    // Stale, this build's, another track, key lock on: dropped, key lock left on.
    setRecord("KeySheetKeylock1",QStringLiteral("1")); setRecord("KeySheetKeylockTrack1",QStringLiteral("gone.wav"));
    setRecord("KeySheetKeylockVersion1",QStringLiteral("2"));
    put(1,"keylock",1);
    {
        DeckLabDeckTools next(host,nullptr);
        EXPECT_EQ(QStringLiteral("0"),record("KeySheetKeylock1"));
        EXPECT_DOUBLE_EQ(1,get(1,"keylock")) << "it may be the DJ's now";
        EXPECT_FALSE(next.keylockBySheet(1));
    }
    // Stale, from an earlier round-5 build (no version; it never handed back
    // at quit), another track, key lock on: handed back, as it promised.
    setRecord("KeySheetKeylock1",QStringLiteral("1")); setRecord("KeySheetKeylockTrack1",QStringLiteral("gone.wav"));
    setRecord("KeySheetKeylockVersion1",QString()); setRecord("KeySheetKeylockAdjust1",QStringLiteral("0"));
    put(1,"keylock",1);
    {
        DeckLabDeckTools next(host,nullptr);
        EXPECT_DOUBLE_EQ(0,get(1,"keylock")) << "an earlier build's key lock goes back";
        EXPECT_EQ(QStringLiteral("0"),record("KeySheetKeylock1"));
        EXPECT_FALSE(next.keylockBySheet(1));
    }
    for(int deck=1;deck<=4;++deck) EXPECT_DOUBLE_EQ(0,get(deck,"play"));
}

// r5-decks: one knob owner at a time, as the owner's Numark sends it. One
// key sheet at a time; a sheet whose knob went elsewhere (another sheet, a
// MORE popup) shows its cursor again before acting; a held press acts on
// release and a push-turn does nothing; GRID's lamps follow the owner.
TEST_F(NativeDeckLabTest, KeySheetAndGridFollowTheKnobOwnerAsTheControllerSendsIt) {
    loadGenerated(1,128,mixxx::track::io::key::A_MINOR);
    loadGenerated(2,126,mixxx::track::io::key::B_FLAT_MINOR); ProcessBuffer(); QTest::qWait(150); settle();
    const auto pitch=[](int deck) { return ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck),"pitch")); };
    auto* tools=panel->findChild<DeckLabDeckTools*>("decklab-deck-tools"); ASSERT_NE(nullptr,tools);
    using K=DeckLabKeySheet::Action;
    tools->openKeySheet(1); settle();
    auto* one=panel->findChild<DeckLabKeySheet*>("decklab-key-1"); ASSERT_NE(nullptr,one);
    auto* two=panel->findChild<DeckLabKeySheet*>("decklab-key-2"); ASSERT_NE(nullptr,two);
    ASSERT_TRUE(one->isVisible());
    r4Pulse("[DeckLabNative]","browse_press"); settle();            // reveal
    r4Pulse("[Playlist]","SelectTrackKnob",3); settle();             // DONE → +1
    ASSERT_EQ(K::Up,one->knobFocus());
    // Deck 2's sheet: deck 1's closes, one sheet at a time.
    tools->openKeySheet(2); settle();
    EXPECT_TRUE(two->isVisible()); EXPECT_FALSE(one->isVisible()) << "one key sheet at a time";
    EXPECT_EQ(two,decklab::knobClaimant());
    r4Pulse("[DeckLabNative]","browse_back"); settle();
    EXPECT_FALSE(two->isVisible());
    EXPECT_FALSE(one->isVisible()) << "no sheet left to act on deck 1";
    EXPECT_NE(one,decklab::knobClaimant());
    EXPECT_NEAR(0,pitch(1),.01);
    // A MORE popup takes the knob: back on the sheet, the next press only
    // shows the cursor again.
    tools->openKeySheet(1); settle(); ASSERT_TRUE(one->isVisible());
    r4Pulse("[DeckLabNative]","browse_press"); settle();
    r4Pulse("[Playlist]","SelectTrackKnob",3); settle();
    ASSERT_EQ(K::Up,one->knobFocus()); ASSERT_TRUE(one->knobCursorShown());
    panel->findChild<QPushButton*>("decklab-2-more")->click(); settle();
    auto* menu=panel->findChild<QMenu*>("decklab-2-more-menu"); ASSERT_NE(nullptr,menu); ASSERT_TRUE(menu->isVisible());
    EXPECT_FALSE(one->knobCursorShown()) << "the cursor hides while the menu has the knob";
    menu->close(); settle();
    r4Pulse("[DeckLabNative]","browse_press"); settle();
    EXPECT_NEAR(0,pitch(1),.01) << "a returning knob reveals first";
    EXPECT_EQ(K::Done,one->knobFocus());
    // The overlay's held press: browse_hold, the press at once, a turn eight
    // times over, the release. Nothing moves and nothing fires.
    r4Pulse("[Playlist]","SelectTrackKnob",3); settle();
    ASSERT_EQ(K::Up,one->knobFocus());
    ControlObject::set(ConfigKey("[DeckLabNative]","browse_hold"),1);
    r4Pulse("[DeckLabNative]","browse_press");
    r4Pulse("[Playlist]","SelectTrackKnob",8);
    ControlObject::set(ConfigKey("[DeckLabNative]","browse_hold"),0); settle();
    EXPECT_NEAR(0,pitch(1),.01) << "a push-turn fires nothing";
    EXPECT_EQ(K::Up,one->knobFocus()) << "and moves nothing";
    // Held and released without a turn: +1 on the release.
    ControlObject::set(ConfigKey("[DeckLabNative]","browse_hold"),1);
    r4Pulse("[DeckLabNative]","browse_press"); settle();
    EXPECT_NEAR(0,pitch(1),.01) << "not at press-down";
    ControlObject::set(ConfigKey("[DeckLabNative]","browse_hold"),0); settle();
    EXPECT_NEAR(1,pitch(1),.01) << "on the release";
    QTest::mouseClick(one->key(K::Reset),Qt::LeftButton);
    QTest::mouseClick(one->key(K::Done),Qt::LeftButton); settle();
    // GRID mode with every deck paused: the lamps and legend follow a MORE
    // popup opening and closing, with no deck refresh in between.
    tools->enterGrid(1); settle();
    auto* grid=panel->findChild<DeckLabGridPanel*>("decklab-grid-mode"); ASSERT_NE(nullptr,grid);
    ASSERT_TRUE(grid->ownsKnob());
    EXPECT_TRUE(grid->key(DeckLabGridPanel::Action::NudgeLater)->armed());
    EXPECT_TRUE(grid->status().startsWith(QStringLiteral("KNOB")));
    panel->findChild<QPushButton*>("decklab-2-more")->click(); settle();
    ASSERT_TRUE(menu->isVisible());
    EXPECT_FALSE(grid->key(DeckLabGridPanel::Action::NudgeLater)->armed()) << "the menu has the knob";
    EXPECT_FALSE(grid->status().startsWith(QStringLiteral("KNOB")));
    menu->close(); settle();
    EXPECT_TRUE(grid->key(DeckLabGridPanel::Action::NudgeLater)->armed()) << "back with GRID";
    EXPECT_TRUE(grid->status().startsWith(QStringLiteral("KNOB")));
    // GRID's push-turn edits nothing; a held press switches on release.
    auto track=deckPlayer(1)->getLoadedTrack(); ASSERT_TRUE(track);
    const auto reference=mixxx::audio::FramePos(44100);
    const double beat=track->getBeats()->findClosestBeat(reference).toEngineSamplePos();
    const double bpm=track->getBpm();
    ControlObject::set(ConfigKey("[DeckLabNative]","browse_hold"),1);
    r4Pulse("[DeckLabNative]","browse_press");
    r4Pulse("[Playlist]","SelectTrackKnob",8);
    ControlObject::set(ConfigKey("[DeckLabNative]","browse_hold"),0); settle(); ProcessBuffer();
    EXPECT_EQ(DeckLabGridPanel::Knob::Nudge,grid->knob());
    EXPECT_DOUBLE_EQ(beat,track->getBeats()->findClosestBeat(reference).toEngineSamplePos());
    EXPECT_NEAR(bpm,track->getBpm(),.001);
    ControlObject::set(ConfigKey("[DeckLabNative]","browse_hold"),1);
    r4Pulse("[DeckLabNative]","browse_press");
    ControlObject::set(ConfigKey("[DeckLabNative]","browse_hold"),0); settle();
    EXPECT_EQ(DeckLabGridPanel::Knob::Bpm,grid->knob());
    tools->leaveGrid(); settle();
}

// r5-decks: holds on the readout end with their context: a live deck's KEY
// hold through a layer change or a track load opens nothing, a paused
// deck's BPM hold that turns into a drag opens nothing, and MORE › GRID is
// the deliberate way into GRID mode on a playing deck.
TEST_F(NativeDeckLabTest, ReadoutHoldsEndWithTheirContextAndMoreOpensGridOnAPlayingDeck) {
    loadGenerated(1,128,mixxx::track::io::key::A_MINOR);
    loadGenerated(2,126,mixxx::track::io::key::B_FLAT_MINOR);
    loadGenerated(3,124,mixxx::track::io::key::C_MAJOR); ProcessBuffer(); QTest::qWait(150); settle();
    auto* sheet=panel->findChild<DeckLabKeySheet*>("decklab-key-1"); ASSERT_NE(nullptr,sheet);
    auto* tools=panel->findChild<DeckLabDeckTools*>("decklab-deck-tools"); ASSERT_NE(nullptr,tools);
    ControlObject::set(ConfigKey("[Channel1]","play"),1); ProcessBuffer(); settle();
    const QPoint keyAt=strip(1)->renderer().keyTarget().center();
    // A layer flip mid-hold (and back before the hold would end).
    QTest::mousePress(strip(1),Qt::LeftButton,Qt::NoModifier,keyAt);
    ControlObject::set(ConfigKey("[DeckLabLeft]","current"),1); QCoreApplication::processEvents(); settle();
    ControlObject::set(ConfigKey("[DeckLabLeft]","current"),0); QCoreApplication::processEvents(); settle();
    QTest::qWait(DeckLabDeckStrip::kLiveKeyHoldMs+100); settle();
    EXPECT_FALSE(sheet->isVisible()) << "a hold across a layer change opens nothing";
    QTest::mouseRelease(strip(1),Qt::LeftButton,Qt::NoModifier,keyAt);
    // A track load mid-hold.
    QTest::mousePress(strip(1),Qt::LeftButton,Qt::NoModifier,keyAt);
    auto replacement=deckPlayer(1)->loadFakeTrack(false,132);
    ASSERT_TRUE(replacement);
    QTest::qWait(DeckLabDeckStrip::kLiveKeyHoldMs+100); ProcessBuffer(); settle();
    EXPECT_FALSE(sheet->isVisible()) << "a hold across a track load opens nothing";
    QTest::mouseRelease(strip(1),Qt::LeftButton,Qt::NoModifier,keyAt);
    // MORE › GRID on the playing deck: EDITING GRID · PLAYING.
    ControlObject::set(ConfigKey("[Channel1]","play"),1); ProcessBuffer(); QTest::qWait(150); settle();
    ASSERT_GT(ControlObject::get(ConfigKey("[Channel1]","play")),0);
    panel->findChild<QPushButton*>("decklab-1-more")->click(); QCoreApplication::processEvents();
    panel->findChild<QPushButton*>("decklab-1-grid")->click(); settle();
    auto* grid=panel->findChild<DeckLabGridPanel*>("decklab-grid-mode"); ASSERT_NE(nullptr,grid);
    ASSERT_TRUE(grid->isVisible()); EXPECT_EQ(1,tools->gridDeck());
    EXPECT_EQ(QStringLiteral("PLAYING"),grid->playText());
    QTest::mouseClick(grid->key(DeckLabGridPanel::Action::Done),Qt::LeftButton); settle();
    ControlObject::set(ConfigKey("[Channel1]","play"),0); ProcessBuffer(); settle();
    // A paused deck's BPM hold that turns into a drag.
    const QPoint bpm=strip(2)->renderer().bpmTarget().center();
    QTest::mousePress(strip(2),Qt::LeftButton,Qt::NoModifier,bpm);
    QTest::mouseMove(strip(2),bpm+QPoint(40,0));
    QTest::qWait(DeckLabDeckStrip::kGridHoldMs+150);
    QTest::mouseRelease(strip(2),Qt::LeftButton,Qt::NoModifier,bpm+QPoint(40,0)); settle();
    EXPECT_EQ(0,tools->gridDeck()) << "a drag is not a hold";
    for(int deck=1;deck<=4;++deck) ControlObject::set(ConfigKey(QStringLiteral("[Channel%1]").arg(deck),"play"),0);
    ProcessBuffer();
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
    // r4-decks: loops are LOOP mode on the pads (any length in one touch;
    // the lit length ends the loop). QNT and KEY LOCK live in MORE.
    const auto pad = [this](int index) {
        return panel->findChild<QPushButton*>(QStringLiteral("decklab-hotcue-1-%1").arg(index));
    };
    auto* loopMode = panel->findChild<QPushButton*>("decklab-pads-1-mode-loop"); ASSERT_NE(nullptr, loopMode);
    QTest::mousePress(loopMode, Qt::LeftButton); QTest::mouseRelease(loopMode, Qt::LeftButton); settle();
    // The generated track lasts 5 s (441000 samples at 44.1 kHz): 8 beats fit
    // at 128 BPM (3.75 s), 16 would not, so Mixxx refuses those.
    auto* quarter = pad(1); auto* half = pad(2); auto* four = pad(5); auto* longest = pad(6);
    auto* quantize = find("quantize"); auto* keylock = find("keylock"); auto* play = find("play");
    ASSERT_NE(nullptr, quarter); ASSERT_NE(nullptr, half); ASSERT_NE(nullptr, four); ASSERT_NE(nullptr, longest);
    ASSERT_NE(nullptr, quantize); ASSERT_NE(nullptr, keylock); ASSERT_NE(nullptr, play);
    for (auto* action : {quarter, half, four, longest, quantize, keylock, play}) {
        EXPECT_EQ(44, action->height()); EXPECT_TRUE(action->isEnabled());
    }
    for (auto* length : {quarter, half, four, longest}) {
        EXPECT_TRUE(length->isVisible()); EXPECT_GE(length->width(), 44);
    }
    EXPECT_EQ(QStringLiteral("1/4"), quarter->text()); EXPECT_EQ(QStringLiteral("1/2"), half->text());
    EXPECT_EQ(QStringLiteral("4"), four->text()); EXPECT_EQ(QStringLiteral("8"), longest->text());
    EXPECT_EQ(QStringLiteral("32"), pad(8)->text());
    const auto lit = [](QPushButton* button) {
        return button->grab().toImage().pixelColor(2, button->height() / 2) == QColor(0xf2, 0xf4, 0xf7);
    };
    const auto loopOn = [] { return ControlObject::get(ConfigKey("[Channel1]", "loop_enabled")); };
    const auto loopSize = [] { return ControlObject::get(ConfigKey("[Channel1]", "beatloop_size")); };
    EXPECT_FALSE(lit(four));
    four->click(); QCoreApplication::processEvents(); ProcessBuffer(); QTest::qWait(150);
    EXPECT_DOUBLE_EQ(1, loopOn()); EXPECT_DOUBLE_EQ(4, loopSize());
    EXPECT_TRUE(lit(four)); EXPECT_TRUE(four->accessibleName().contains("on"));
    // Another length while looping: the loop takes it at once.
    half->click(); QCoreApplication::processEvents(); ProcessBuffer(); QTest::qWait(150);
    EXPECT_DOUBLE_EQ(1, loopOn()); EXPECT_DOUBLE_EQ(.5, loopSize());
    EXPECT_TRUE(lit(half)); EXPECT_FALSE(lit(four));
    // The lit length ends the loop; the length stays armed.
    half->click(); QCoreApplication::processEvents(); ProcessBuffer(); QTest::qWait(150);
    EXPECT_DOUBLE_EQ(0, loopOn()); EXPECT_DOUBLE_EQ(.5, loopSize()); EXPECT_FALSE(lit(half));
    for (auto* length : {quarter, longest}) {
        length->click(); ProcessBuffer(); QTest::qWait(150);
        ASSERT_DOUBLE_EQ(1, loopOn());
        const double size = loopSize();
        length->click(); ProcessBuffer(); QTest::qWait(150);
        EXPECT_DOUBLE_EQ(0, loopOn());
        EXPECT_DOUBLE_EQ(size, loopSize());
        panel->findChild<QPushButton*>("decklab-1-zoom_in")->click();
        panel->findChild<QPushButton*>("decklab-1-zoom_out")->click(); QTest::qWait(150);
        EXPECT_DOUBLE_EQ(size, loopSize());
        EXPECT_DOUBLE_EQ(0, loopOn());
    }
    quantize->click(); keylock->click(); QCoreApplication::processEvents();
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel1]", "quantize")));
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel1]", "keylock")));
    EXPECT_TRUE(quantize->isChecked()); EXPECT_TRUE(keylock->isChecked());
    // Touch-down acts on the deck the pads show; MORE's toggles act on
    // release. A layer change in between never reaches deck 3.
    for (auto* button : {four, quantize, keylock, play}) {
        ControlObject::set(ConfigKey("[DeckLabLeft]", "current"), 0); QCoreApplication::processEvents(); settle();
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
    // Each deck keeps its own pad mode: deck 3 shows CUE, deck 1 LOOP.
    EXPECT_EQ(QStringLiteral("E"), four->text());
    ControlObject::set(ConfigKey("[DeckLabLeft]", "current"), 0); QCoreApplication::processEvents(); settle();
    EXPECT_EQ(QStringLiteral("4"), four->text());
    ControlObject::set(ConfigKey("[DeckLabLeft]", "current"), 1); QCoreApplication::processEvents();
    QTest::qWait(kUnreplaceDelay + 1);
    m_pMixerDeck3->slotEjectTrack(1); ProcessBuffer(); QTest::qWait(150);
    EXPECT_FALSE(four->isEnabled()); EXPECT_FALSE(quantize->isEnabled()); EXPECT_FALSE(keylock->isEnabled());
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
    // r6-fxstrip: the 2-deck layout lists decks 3 and 4 only when loaded.
    EXPECT_NE(nullptr, menu->findChild<QAction*>("decklab-unload-1"));
    EXPECT_NE(nullptr, menu->findChild<QAction*>("decklab-unload-2"));
    EXPECT_EQ(nullptr, menu->findChild<QAction*>("decklab-unload-3"));
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
                // Two-deck view: QNT and KEY LOCK light in the side's MORE;
                // the readout lights LOOP and KEY (r4-decks).
                EXPECT_TRUE(panel->findChild<QPushButton*>(QStringLiteral("decklab-%1-quantize").arg(deck))->isChecked());
                EXPECT_TRUE(panel->findChild<QPushButton*>(QStringLiteral("decklab-%1-keylock").arg(deck))->isChecked());
                for(const auto& lamp:{"LOOP","KEY","SYNC"}) EXPECT_TRUE(readout->modesText().contains(lamp)) << lamp;
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
    // r4-decks: QNT and KEY LOCK in MORE, and a LOOP pad (4 beats).
    auto* loopMode = panel->findChild<QPushButton*>("decklab-pads-1-mode-loop"); ASSERT_NE(nullptr, loopMode);
    QTest::mousePress(loopMode, Qt::LeftButton); QTest::mouseRelease(loopMode, Qt::LeftButton); settle();
    EXPECT_EQ(lit, fill(loopMode)) << "the current pad mode is a filled lamp";
    for (const char* item : {"decklab-1-quantize", "decklab-1-keylock", "decklab-hotcue-1-5"}) {
        auto* button = panel->findChild<QPushButton*>(QString::fromLatin1(item));
        ASSERT_NE(nullptr, button) << item;
        // MORE's toggles are checkable; a pad's lamp is painted from state.
        EXPECT_FALSE(button->isChecked()) << item;
        EXPECT_EQ(panelFill, fill(button)) << item;
        button->click(); QCoreApplication::processEvents(); ProcessBuffer(); settle();
        if (button->isCheckable()) {
            EXPECT_TRUE(button->isChecked()) << item;
        }
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

// r6-sync: SYNC is a CDJ BEAT SYNC: a tap turns it on, the lamp stays lit
// until the next tap. A hold is INST DOUBLES; one that loads nothing on a
// stopped deck (here nothing plays and there is no master) is still a tap on
// release, so a slow press is never a dead button.
TEST_F(NativeDeckLabTest, SyncTapTogglesBeatSyncAndAHoldThatLoadsNothingIsATap) {
    loadGenerated(1, 128); loadGenerated(2, 130); loadGenerated(3, 132); ProcessBuffer(); settle();
    auto* sync = panel->findChild<QPushButton*>("decklab-1-beatsync"); ASSERT_NE(nullptr, sync);
    const auto fill = [](QPushButton* button) { return button->grab().toImage().pixelColor(2, button->height() / 2); };
    const auto locked = [](int deck) { return ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), "sync_enabled")); };
    EXPECT_EQ(QStringLiteral("SYNC"), sync->text());
    EXPECT_TRUE(sync->accessibleDescription().contains("Hold"));
    // Tap: BEAT SYNC on, a filled lamp, SYNC on the readout.
    sync->click(); settle();
    EXPECT_DOUBLE_EQ(1, locked(1));
    EXPECT_TRUE(sync->property("lit").toBool()); EXPECT_FALSE(sync->property("actionFeedback").toBool());
    EXPECT_EQ(QColor(0xf2, 0xf4, 0xf7), fill(sync)) << "SYNC on is a filled lamp";
    EXPECT_TRUE(strip(1)->state().sync);
    EXPECT_TRUE(sync->accessibleName().contains("on"));
    capture(QStringLiteral("decks-sync-on.png"));
    // It stays on: nothing times out.
    QTest::qWait(1200); settle();
    EXPECT_DOUBLE_EQ(1, locked(1)); EXPECT_TRUE(sync->property("lit").toBool()); EXPECT_TRUE(strip(1)->state().sync);
    // Tap again: off.
    sync->click(); settle();
    EXPECT_DOUBLE_EQ(0, locked(1)); EXPECT_FALSE(sync->property("lit").toBool()); EXPECT_FALSE(strip(1)->state().sync);
    // A hold with nothing to copy (no master, nothing plays) loads nothing,
    // so its release is a tap: on, and the next hold off again.
    QTest::mousePress(sync, Qt::LeftButton); QTest::qWait(700);
    EXPECT_DOUBLE_EQ(0, locked(1)) << "nothing happens while held";
    QTest::mouseRelease(sync, Qt::LeftButton); settle();
    EXPECT_DOUBLE_EQ(1, locked(1)); EXPECT_TRUE(sync->property("lit").toBool());
    EXPECT_EQ(QStringLiteral("Generated Tone 128"), strip(1)->state().title) << "no track was replaced";
    QTest::mousePress(sync, Qt::LeftButton); QTest::qWait(700);
    QTest::mouseRelease(sync, Qt::LeftButton); settle();
    EXPECT_DOUBLE_EQ(0, locked(1)); EXPECT_FALSE(sync->property("lit").toBool());
    // A press that slides off is no tap.
    QTest::mousePress(sync, Qt::LeftButton);
    sync->setDown(false);
    QTest::mouseRelease(sync, Qt::LeftButton); settle();
    EXPECT_DOUBLE_EQ(0, locked(1));
    // Four-deck cards: the same SYNC.
    QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"), "activated", Q_ARG(int, 2)); QTest::qWait(150);
    auto* card = panel->findChild<QPushButton*>("decklab-card-2-beatsync"); ASSERT_NE(nullptr, card);
    card->click(); settle();
    EXPECT_DOUBLE_EQ(1, locked(2));
    EXPECT_TRUE(card->property("lit").toBool()); EXPECT_TRUE(strip(2)->modesText().contains("SYNC"));
    card->click(); settle();
    EXPECT_DOUBLE_EQ(0, locked(2)); EXPECT_FALSE(card->property("lit").toBool());
    for (int deck = 1; deck <= 3; ++deck) EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), "play")));
}

// r6-sync: INST DOUBLES (SYNC held, or INST DOUBLES in MORE) loads the tempo
// master's track, else a playing deck's, onto a stopped deck at the same
// position, playing if it plays. Never onto a playing deck, where a SYNC
// hold does nothing at all (owner rule).
TEST_F(NativeDeckLabTest, SyncHoldIsInstantDoublesOnlyOntoAStoppedDeck) {
    auto track = Track::newTemporary(getTestDir().filePath(QStringLiteral("sine-30.wav")));
    loadTrack(m_pMixerDeck1.get(), track); ASSERT_TRUE(track->trySetBpm(128));
    const auto get = [](int deck, const char* item) { return ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), item)); };
    const auto put = [](int deck, const char* item, double v) { ControlObject::set(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), item), v); };
    put(1, "quantize", 0); put(2, "quantize", 0);
    put(1, "playposition", .3); ProcessBuffer();
    auto* sync2 = panel->findChild<QPushButton*>("decklab-2-beatsync"); ASSERT_NE(nullptr, sync2);
    settle(); QTest::qWait(150); settle();
    // Nothing plays and deck 1 is no master: the empty deck has nothing to copy.
    EXPECT_FALSE(sync2->isEnabled());
    put(1, "play", 1); ProcessBuffer(); settle(); QTest::qWait(150); settle();
    // Deck 1 plays: deck 2's SYNC (empty) is live for the hold; a tap does nothing.
    ASSERT_TRUE(sync2->isEnabled());
    EXPECT_TRUE(sync2->accessibleDescription().contains("deck 1")) << sync2->accessibleDescription().toStdString();
    sync2->click(); settle();
    EXPECT_DOUBLE_EQ(0, get(2, "track_loaded")); EXPECT_DOUBLE_EQ(0, get(2, "sync_enabled"));
    // Hold: deck 1's track on deck 2, at deck 1's position, playing.
    QTest::mousePress(sync2, Qt::LeftButton);
    EXPECT_TRUE(QTest::qWaitFor([&] {
        ProcessBuffer();
        return m_pChannel2->getEngineBuffer()->getLoadedTrack() == track;
    }, 3000));
    QTest::mouseRelease(sync2, Qt::LeftButton);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    settle();
    EXPECT_DOUBLE_EQ(0, get(2, "sync_enabled")) << "the release is no tap";
    EXPECT_DOUBLE_EQ(1, get(2, "play")) << "Mixxx's clone plays with its source";
    EXPECT_NEAR(get(1, "playposition"), get(2, "playposition"), .01);
    // Deck 2 plays now: a hold there does nothing at all. It never replaces
    // the track, and its release is no tap, so BEAT SYNC stays off and the
    // deck on air keeps its tempo. MORE's INST DOUBLES is off.
    auto other = Track::newTemporary(getTestDir().filePath(QStringLiteral("sine-30.wav")));
    loadTrack(m_pMixerDeck3.get(), other); ASSERT_TRUE(other->trySetBpm(126));
    settle(); QTest::qWait(150); settle(); // the player finishes its load (cue recall) first
    put(3, "playposition", .6); ProcessBuffer();
    put(3, "play", 1); ProcessBuffer(); ProcessBuffer();
    put(3, "sync_leader", 1);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    settle(); QTest::qWait(150); settle();
    const double onAir = get(2, "bpm");
    ASSERT_GT(std::abs(onAir - get(3, "bpm")), 1);
    QTest::mousePress(sync2, Qt::LeftButton); QTest::qWait(700);
    QTest::mouseRelease(sync2, Qt::LeftButton);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    settle();
    EXPECT_EQ(track, m_pMixerDeck2->getLoadedTrack()) << "a playing deck's track is never replaced";
    EXPECT_DOUBLE_EQ(1, get(2, "play"));
    EXPECT_DOUBLE_EQ(0, get(2, "sync_enabled")) << "a hold on a playing deck is no tap";
    EXPECT_NEAR(onAir, get(2, "bpm"), 1e-6) << "the deck on air keeps its tempo";
    EXPECT_FALSE(sync2->property("lit").toBool());
    panel->findChild<QPushButton*>("decklab-2-more")->click(); settle();
    auto* doubles = panel->findChild<QPushButton*>("decklab-2-doubles"); ASSERT_NE(nullptr, doubles);
    EXPECT_FALSE(doubles->isEnabled());
    EXPECT_TRUE(doubles->accessibleName().contains("playing")) << doubles->accessibleName().toStdString();
    panel->findChild<QMenu*>("decklab-2-more-menu")->close(); settle();
    // Stopped, deck 2 offers the playing tempo master (deck 3, hidden on its
    // layer) before deck 1, which only plays.
    ASSERT_GT(get(3, "sync_leader"), 0); ASSERT_DOUBLE_EQ(1, get(3, "play"));
    put(2, "play", 0); ProcessBuffer(); settle(); QTest::qWait(150); settle();
    EXPECT_TRUE(sync2->accessibleDescription().contains("deck 3")) << sync2->accessibleDescription().toStdString();
    panel->findChild<QPushButton*>("decklab-2-more")->click(); settle();
    ASSERT_TRUE(doubles->isEnabled());
    doubles->click();
    EXPECT_TRUE(QTest::qWaitFor([&] {
        ProcessBuffer();
        // The engine's track: the previous one stays loaded until the new one is.
        return m_pChannel2->getEngineBuffer()->getLoadedTrack() == other;
    }, 3000));
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    EXPECT_DOUBLE_EQ(1, get(2, "play"));
    EXPECT_NEAR(get(3, "playposition"), get(2, "playposition"), .01);
    // A mapping's hold ([DeckLabNative] inst_doubles = deck): the same rules.
    const auto controllerHold = [](int deck) {
        ControlObject::set(ConfigKey("[DeckLabNative]", "inst_doubles"), deck);
        ControlObject::set(ConfigKey("[DeckLabNative]", "inst_doubles"), 0);
        settle();
    };
    controllerHold(1);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    EXPECT_EQ(track, m_pChannel1->getEngineBuffer()->getLoadedTrack()) << "deck 1 plays: never replaced";
    put(1, "play", 0); ProcessBuffer(); settle();
    controllerHold(1);
    EXPECT_TRUE(QTest::qWaitFor([&] {
        ProcessBuffer();
        return m_pChannel1->getEngineBuffer()->getLoadedTrack() == other;
    }, 3000)) << "stopped: the master's track (deck 3)";
    for (int deck = 1; deck <= 3; ++deck) { put(deck, "play", 0); put(deck, "sync_enabled", 0); }
    ProcessBuffer();
}

// r6-sync: MASTER in MORE takes the tempo master for a playing deck and
// hands it on from the master; pausing the master hands it to the deck that
// plays, as on a CDJ. The readout shows MASTER on exactly one deck.
TEST_F(NativeDeckLabTest, MasterInMoreTakesAndHandsOnTheTempoMaster) {
    loadGenerated(1, 128); loadGenerated(2, 130); ProcessBuffer(); settle();
    const auto get = [](int deck, const char* item) { return ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), item)); };
    const auto put = [](int deck, const char* item, double v) { ControlObject::set(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), item), v); };
    const auto openMore = [this](int side) {
        panel->findChild<QPushButton*>(QStringLiteral("decklab-%1-more").arg(side))->click(); settle();
        auto* menu = panel->findChild<QMenu*>(QStringLiteral("decklab-%1-more-menu").arg(side));
        EXPECT_TRUE(menu && menu->isVisible());
        return menu;
    };
    auto* master1 = panel->findChild<QPushButton*>("decklab-1-master"); ASSERT_NE(nullptr, master1);
    auto* master2 = panel->findChild<QPushButton*>("decklab-2-master"); ASSERT_NE(nullptr, master2);
    EXPECT_EQ(QStringLiteral("MASTER"), master1->text());
    EXPECT_EQ(QStringLiteral("INST DOUBLES"), panel->findChild<QPushButton*>("decklab-1-doubles")->text());
    // Stopped: no deck can take the master yet.
    auto* menu = openMore(2); ASSERT_NE(nullptr, menu);
    EXPECT_FALSE(master2->isEnabled()); EXPECT_FALSE(master2->isChecked());
    EXPECT_TRUE(master2->accessibleName().contains("while it plays"));
    EXPECT_EQ(44, master2->height());
    menu->close(); settle();
    put(1, "play", 1); put(2, "play", 1); ProcessBuffer(); ProcessBuffer();
    // SYNC on deck 1: deck 2, on air, is the master.
    panel->findChild<QPushButton*>("decklab-1-beatsync")->click();
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    settle();
    ASSERT_GT(get(2, "sync_leader"), 0);
    EXPECT_TRUE(strip(2)->state().master); EXPECT_FALSE(strip(1)->state().master);
    capture(QStringLiteral("decks-sync-follows-master.png"));
    // MASTER on deck 1: deck 1 leads, deck 2 follows at deck 1's tempo.
    menu = openMore(1); ASSERT_NE(nullptr, menu);
    ASSERT_TRUE(master1->isEnabled()); EXPECT_FALSE(master1->isChecked());
    master1->click();
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    settle();
    EXPECT_GT(get(1, "sync_leader"), 0); EXPECT_DOUBLE_EQ(0, get(2, "sync_leader"));
    EXPECT_DOUBLE_EQ(1, get(2, "sync_enabled"));
    EXPECT_TRUE(master1->isChecked()) << "MASTER is lit on the master";
    EXPECT_TRUE(master1->accessibleName().contains("is the tempo master"));
    EXPECT_TRUE(strip(1)->state().master); EXPECT_FALSE(strip(2)->state().master);
    EXPECT_TRUE(strip(2)->state().sync);
    EXPECT_NEAR(get(1, "bpm"), get(2, "bpm"), 1e-6);
    capture(QStringLiteral("decks-sync-master-deck-1.png"));
    const QString output = qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
    if (!output.isEmpty()) EXPECT_TRUE(menu->grab().save(QDir(output).filePath(QStringLiteral("decks-more-sync-master.png"))));
    // MASTER again on the master hands it on, to deck 2, still synced.
    master1->click();
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    settle();
    EXPECT_GT(get(2, "sync_leader"), 0); EXPECT_DOUBLE_EQ(0, get(1, "sync_leader"));
    EXPECT_DOUBLE_EQ(1, get(1, "sync_enabled"));
    EXPECT_FALSE(master1->isChecked());
    menu->close(); settle();
    // Pausing the master hands it to the deck that plays.
    put(2, "play", 0);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    settle();
    EXPECT_GT(get(1, "sync_leader"), 0); EXPECT_DOUBLE_EQ(0, get(2, "sync_leader"));
    EXPECT_TRUE(strip(1)->state().master); EXPECT_FALSE(strip(2)->state().master);
    int masters = 0;
    for (int deck = 1; deck <= 4; ++deck) masters += get(deck, "sync_leader") > 0 ? 1 : 0;
    EXPECT_EQ(1, masters) << "exactly one tempo master";
    // Four-deck cards: the same keys.
    QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"), "activated", Q_ARG(int, 2)); QTest::qWait(150);
    EXPECT_NE(nullptr, panel->findChild<QPushButton*>("decklab-card-1-master"));
    EXPECT_NE(nullptr, panel->findChild<QPushButton*>("decklab-card-1-doubles"));
    settle(); QTest::qWait(150); settle();
    EXPECT_TRUE(strip(1)->modesText().contains("MASTER")); EXPECT_TRUE(strip(2)->modesText().contains("SYNC"));
    EXPECT_TRUE(panel->findChild<QPushButton*>("decklab-card-1-beatsync")->property("lit").toBool());
    capture(QStringLiteral("decks-four-sync-master.png"));
    for (int deck = 1; deck <= 2; ++deck) { put(deck, "play", 0); put(deck, "sync_enabled", 0); }
    ProcessBuffer();
}

// r6-sync: SYNC on the deck on air while the only other playing deck is in
// the headphones (its channel fader down): the deck on air becomes the tempo
// master at its own tempo, with no seek. It is never matched to a deck nobody
// hears. The headphone deck's SYNC then follows the deck on air.
TEST_F(NativeDeckLabTest, SyncOnTheDeckOnAirNeverMatchesADeckNobodyHears) {
    m_pMixerDeck1->loadFakeTrack(false, 124); m_pMixerDeck2->loadFakeTrack(false, 128); ProcessBuffer();
    settle(); ProcessBuffer(); // the loads finish (cue recall) before the positions are set
    const auto get = [](int deck, const char* item) { return ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), item)); };
    const auto put = [](int deck, const char* item, double v) { ControlObject::set(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), item), v); };
    // Quantize goes on once both play: a quantized start would snap deck 2
    // to deck 1's phase.
    put(1, "quantize", 0); put(2, "quantize", 0);
    put(1, "playposition", .11); put(2, "playposition", .26); ProcessBuffer();
    put(2, "volume", 0); // cued in the headphones
    put(1, "play", 1); put(2, "play", 1);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    put(1, "quantize", 1); put(2, "quantize", 1);
    settle();
    ASSERT_NEAR(.11, get(1, "playposition"), .02); ASSERT_NEAR(.26, get(2, "playposition"), .02);
    ASSERT_GT(std::abs(get(1, "beat_distance") - get(2, "beat_distance")), .05);
    // The engine's own position, updated every buffer (playposition is only
    // published a few times a second).
    const auto exact = [this] { return m_pChannel1->getEngineBuffer()->getExactPlayPos().value(); };
    const double start = exact();
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    const double step = (exact() - start) / 4;
    ASSERT_GT(step, 0);
    const double tapped = exact();
    auto* sync1 = panel->findChild<QPushButton*>("decklab-1-beatsync"); ASSERT_NE(nullptr, sync1);
    sync1->click();
    for (int n = 0; n < 8; ++n) ProcessBuffer();
    settle();
    EXPECT_GT(get(1, "sync_leader"), 0) << "the deck on air is the master";
    EXPECT_DOUBLE_EQ(1, get(1, "sync_enabled"));
    EXPECT_DOUBLE_EQ(0, get(2, "sync_enabled")) << "the headphone deck is not synced";
    EXPECT_NEAR(124, get(1, "bpm"), 1e-6) << "the deck on air keeps its tempo";
    EXPECT_NEAR(128, get(2, "bpm"), 1e-6);
    EXPECT_NEAR(tapped + 8 * step, exact(), step / 2) << "no phase seek on air";
    EXPECT_TRUE(sync1->property("lit").toBool()); EXPECT_TRUE(strip(1)->state().master);
    // The headphone deck's SYNC: it follows the deck on air, which stays the
    // master however quiet the headphone deck is.
    panel->findChild<QPushButton*>("decklab-2-beatsync")->click();
    for (int n = 0; n < 8; ++n) ProcessBuffer();
    settle();
    EXPECT_GT(get(1, "sync_leader"), 0); EXPECT_DOUBLE_EQ(0, get(2, "sync_leader"));
    EXPECT_DOUBLE_EQ(1, get(2, "sync_enabled"));
    EXPECT_NEAR(124, get(1, "bpm"), 1e-6);
    EXPECT_NEAR(124, get(2, "bpm"), 1e-6);
    EXPECT_NEAR(get(1, "beat_distance"), get(2, "beat_distance"), .001);
    for (int deck = 1; deck <= 2; ++deck) { put(deck, "play", 0); put(deck, "sync_enabled", 0); put(deck, "volume", 1); }
    ProcessBuffer();
}

// r6-sync: as on a CDJ the tempo master moves only on a pause, a new track or
// MASTER. Closing its channel fader (a cut) leaves it the master, so its
// tempo fader still leads and the synced deck's still does nothing; another
// deck's SYNC follows it too. A pause hands the master on.
TEST_F(NativeDeckLabTest, TempoMasterStaysWhileItPlaysWhenItsFaderCloses) {
    m_pMixerDeck1->loadFakeTrack(false, 124); m_pMixerDeck2->loadFakeTrack(false, 128);
    m_pMixerDeck3->loadFakeTrack(false, 126); ProcessBuffer();
    const auto get = [](int deck, const char* item) { return ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), item)); };
    const auto put = [](int deck, const char* item, double v) { ControlObject::set(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), item), v); };
    for (int deck = 1; deck <= 3; ++deck) put(deck, "quantize", 0);
    put(1, "play", 1); put(2, "play", 1); ProcessBuffer(); ProcessBuffer();
    // SYNC on deck 1: deck 2, on air, is the master; deck 1 follows it.
    put(1, "beatsync", 1); put(1, "beatsync", 0);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    settle();
    ASSERT_GT(get(2, "sync_leader"), 0); ASSERT_DOUBLE_EQ(1, get(1, "sync_enabled"));
    ASSERT_NEAR(128, get(1, "bpm"), 1e-6);
    // A cut: the master's channel fader closes while both play.
    put(2, "volume", 0);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    settle();
    EXPECT_GT(get(2, "sync_leader"), 0) << "the master stays the master";
    EXPECT_DOUBLE_EQ(0, get(1, "sync_leader"));
    EXPECT_TRUE(strip(2)->state().master); EXPECT_FALSE(strip(1)->state().master);
    // Deck 1, on air, still follows: its fader does nothing, the master's
    // moves both.
    put(1, "rate", .5);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    EXPECT_NEAR(128, get(1, "bpm"), 1e-6);
    EXPECT_NEAR(128, get(2, "bpm"), 1e-6);
    put(2, "rate", .25);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    const double moved = get(2, "bpm");
    EXPECT_GT(std::abs(moved - 128), 1);
    EXPECT_NEAR(moved, get(1, "bpm"), 1e-6);
    // Another deck on air syncs: it follows the master too.
    put(3, "play", 1); ProcessBuffer(); ProcessBuffer();
    put(3, "beatsync", 1); put(3, "beatsync", 0);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    settle();
    EXPECT_GT(get(2, "sync_leader"), 0); EXPECT_DOUBLE_EQ(0, get(3, "sync_leader"));
    EXPECT_DOUBLE_EQ(1, get(3, "sync_enabled"));
    EXPECT_NEAR(moved, get(3, "bpm"), 1e-6);
    // The fader back up changes nothing.
    put(2, "volume", 1);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    EXPECT_GT(get(2, "sync_leader"), 0);
    // A pause hands the master to a deck that plays, at the same tempo.
    put(2, "play", 0);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    settle();
    EXPECT_DOUBLE_EQ(0, get(2, "sync_leader"));
    EXPECT_GT(get(1, "sync_leader"), 0);
    int masters = 0;
    for (int deck = 1; deck <= 4; ++deck) masters += get(deck, "sync_leader") > 0 ? 1 : 0;
    EXPECT_EQ(1, masters) << "exactly one tempo master";
    EXPECT_NEAR(moved, get(1, "bpm"), 1e-6);
    EXPECT_NEAR(moved, get(3, "bpm"), 1e-6);
    for (int deck = 1; deck <= 3; ++deck) { put(deck, "play", 0); put(deck, "sync_enabled", 0); put(deck, "volume", 1); }
    ProcessBuffer();
}

// r6-sync: the browse knob drives a MORE sheet. A press before any turn
// closes it, as it always did; the first turn shows the cursor on MASTER and
// does nothing else; turns move it through the keys that can act, in reading
// order, never onto PLAY, CUE or the pads (touch only: a stray press must not
// stop a deck on air or jump it to a cue); a press clicks the key under it;
// back closes.
TEST_F(NativeDeckLabTest, KnobReachesMasterInTheMoreSheet) {
    loadGenerated(1, 128); loadGenerated(2, 130); ProcessBuffer(); settle();
    const auto get = [](int deck, const char* item) { return ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), item)); };
    const auto put = [](int deck, const char* item, double v) { ControlObject::set(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), item), v); };
    const auto touchOnly = [](QPushButton* key) {
        const QString name = key->objectName();
        return name.endsWith(QStringLiteral("-play")) || name.endsWith(QStringLiteral("cue_default")) ||
                qobject_cast<DeckLabPadButton*>(key) != nullptr;
    };
    put(1, "quantize", 0); put(2, "quantize", 0);
    put(1, "play", 1); put(2, "play", 1); ProcessBuffer(); ProcessBuffer();
    panel->findChild<QPushButton*>("decklab-1-beatsync")->click();
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    settle();
    ASSERT_GT(get(2, "sync_leader"), 0);
    auto* more = panel->findChild<QPushButton*>("decklab-1-more"); ASSERT_NE(nullptr, more);
    more->click(); settle();
    auto* menu = panel->findChild<QMenu*>("decklab-1-more-menu"); ASSERT_NE(nullptr, menu); ASSERT_TRUE(menu->isVisible());
    auto* knob = menu->findChild<DeckLabMoreKnob*>(); ASSERT_NE(nullptr, knob);
    auto* ring = menu->findChild<QFrame*>("decklab-more-knob-cursor"); ASSERT_NE(nullptr, ring);
    auto* master = panel->findChild<QPushButton*>("decklab-1-master"); ASSERT_NE(nullptr, master);
    EXPECT_EQ(nullptr, knob->cursorKey()); EXPECT_FALSE(ring->isVisible());
    // A press before any turn (from habit, or the first of a double press)
    // closes MORE and acts on no key.
    r4Pulse("[DeckLabNative]", "browse_press");
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    settle();
    EXPECT_FALSE(menu->isVisible());
    EXPECT_DOUBLE_EQ(1, get(1, "play")); EXPECT_DOUBLE_EQ(1, get(2, "play"));
    EXPECT_DOUBLE_EQ(0, get(1, "sync_leader")); EXPECT_GT(get(2, "sync_leader"), 0);
    more->click(); settle(); ASSERT_TRUE(menu->isVisible());
    EXPECT_EQ(nullptr, knob->cursorKey());
    // The first turn only shows the cursor: on MASTER, as PLAY and CUE above
    // it are touch only.
    r4Pulse("[Playlist]", "SelectTrackKnob", 1); settle();
    ASSERT_EQ(master, knob->cursorKey()); EXPECT_TRUE(ring->isVisible());
    EXPECT_DOUBLE_EQ(0, get(1, "sync_leader")) << "a reveal acts on nothing";
    EXPECT_TRUE(menu->isVisible());
    const QRect key(master->mapTo(ring->parentWidget(), QPoint()), master->size());
    EXPECT_TRUE(ring->geometry().contains(key)) << "the ring is around MASTER";
    const QString output = qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
    if (!output.isEmpty()) EXPECT_TRUE(menu->grab().save(QDir(output).filePath(QStringLiteral("decks-more-knob-master.png"))));
    // Down to the last key and back: settings keys only, and never past
    // MASTER to PLAY.
    QStringList reached;
    for (int n = 0; n < 16; ++n) {
        ASSERT_NE(nullptr, knob->cursorKey());
        EXPECT_FALSE(touchOnly(knob->cursorKey())) << knob->cursorKey()->objectName().toStdString();
        reached << knob->cursorKey()->objectName();
        r4Pulse("[Playlist]", "SelectTrackKnob", 1); settle();
    }
    EXPECT_TRUE(reached.contains(QStringLiteral("decklab-1-zoom_in"))) << reached.join(' ').toStdString();
    for (int n = 0; n < 16; ++n) { r4Pulse("[Playlist]", "SelectTrackKnob", -1); settle(); }
    ASSERT_EQ(master, knob->cursorKey());
    // Press: deck 1 takes the master; the sheet stays open on MASTER.
    r4Pulse("[DeckLabNative]", "browse_press");
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    settle();
    EXPECT_GT(get(1, "sync_leader"), 0); EXPECT_DOUBLE_EQ(0, get(2, "sync_leader"));
    EXPECT_TRUE(menu->isVisible()); EXPECT_EQ(master, knob->cursorKey());
    EXPECT_TRUE(master->isChecked());
    // Back closes the sheet; the next opening starts over.
    r4Pulse("[DeckLabNative]", "browse_back"); settle();
    EXPECT_FALSE(menu->isVisible());
    more->click(); settle();
    ASSERT_TRUE(menu->isVisible());
    EXPECT_EQ(nullptr, knob->cursorKey()); EXPECT_FALSE(ring->isVisible());
    menu->close(); settle();
    // Four-deck card: the pads (hot cues, loops, rolls, stems) sit above
    // MASTER, and the knob never reaches them.
    QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"), "activated", Q_ARG(int, 2)); QTest::qWait(150);
    settle();
    panel->findChild<QPushButton*>("decklab-card-1-more")->click(); settle();
    auto* card = panel->findChild<QMenu*>("decklab-card-1-more-menu"); ASSERT_NE(nullptr, card); ASSERT_TRUE(card->isVisible());
    auto* cardKnob = card->findChild<DeckLabMoreKnob*>(); ASSERT_NE(nullptr, cardKnob);
    auto* pad = card->findChild<DeckLabPadButton*>(); ASSERT_NE(nullptr, pad); EXPECT_TRUE(pad->isVisible());
    r4Pulse("[Playlist]", "SelectTrackKnob", 1); settle();
    EXPECT_EQ(panel->findChild<QPushButton*>("decklab-card-1-master"), cardKnob->cursorKey()) << "the reveal skips the pads";
    for (int n = 0; n < 16; ++n) {
        ASSERT_NE(nullptr, cardKnob->cursorKey());
        EXPECT_FALSE(touchOnly(cardKnob->cursorKey())) << cardKnob->cursorKey()->objectName().toStdString();
        r4Pulse("[Playlist]", "SelectTrackKnob", n < 8 ? 1 : -1); settle();
    }
    card->close(); settle();
    EXPECT_DOUBLE_EQ(1, get(1, "play")); EXPECT_DOUBLE_EQ(1, get(2, "play"));
    for (int deck = 1; deck <= 2; ++deck) { put(deck, "play", 0); put(deck, "sync_enabled", 0); }
    ProcessBuffer();
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

// r4-decks (audit moves 1 and 2): pad modes and the readout's rows on the
// real surface and engine: rolls held and always ended, STEM only for stem
// tracks, OVER beyond the fader range, the phase meter's reference deck,
// one-decimal BPM, SLIP in MORE, and the same pad block in four-deck MORE.
TEST_F(NativeDeckLabTest, PadModesAndReadoutRowsFollowTheRealDecks) {
    loadGenerated(1, 128, mixxx::track::io::key::A_MINOR);
    loadGenerated(2, 100, mixxx::track::io::key::C_MAJOR); ProcessBuffer();
    ControlObject::set(ConfigKey("[Channel1]", "quantize"), 0); settle(); QTest::qWait(150);
    // The pads took the hot cue and transport rows: the waveforms keep
    // their height, stem track or not.
    EXPECT_EQ(244, panel->findChild<QWidget*>("decklab-deck-row")->height());
    EXPECT_EQ(QStringLiteral("128.0"), strip(1)->state().bpm) << "one decimal, as on a CDJ";
    // STEM appears only for a stem track.
    auto* stem = panel->findChild<QPushButton*>("decklab-pads-1-mode-stem"); ASSERT_NE(nullptr, stem);
    EXPECT_FALSE(stem->isEnabled()); EXPECT_TRUE(stem->text().isEmpty());
    // ROLL: on while held, off on release, and off when the page changes.
    auto* roll = panel->findChild<QPushButton*>("decklab-pads-1-mode-roll"); ASSERT_NE(nullptr, roll);
    QTest::mousePress(roll, Qt::LeftButton); QTest::mouseRelease(roll, Qt::LeftButton); settle();
    auto* quarter = panel->findChild<QPushButton*>("decklab-hotcue-1-3"); ASSERT_NE(nullptr, quarter);
    EXPECT_EQ(QStringLiteral("1/4"), quarter->text());
    const ConfigKey rolling("[Channel1]", "beatlooproll_0.25_activate");
    ControlObject::set(ConfigKey("[Channel1]", "play"), 1); ProcessBuffer(); settle();
    QTest::mousePress(quarter, Qt::LeftButton); ProcessBuffer();
    EXPECT_DOUBLE_EQ(1, ControlObject::get(rolling)) << "the roll starts on touch-down";
    capture(QStringLiteral("decks-pads-roll.png"));
    QTest::mouseRelease(quarter, Qt::LeftButton); ProcessBuffer();
    EXPECT_DOUBLE_EQ(0, ControlObject::get(rolling));
    QTest::mousePress(quarter, Qt::LeftButton); ProcessBuffer();
    ASSERT_DOUBLE_EQ(1, ControlObject::get(rolling));
    panel->selectPage(1); settle();
    EXPECT_DOUBLE_EQ(0, ControlObject::get(rolling)) << "a page change ends a held roll";
    QTest::mouseRelease(quarter, Qt::LeftButton); ProcessBuffer();
    EXPECT_DOUBLE_EQ(0, ControlObject::get(rolling));
    panel->selectPage(0); settle();
    // The mode is the deck's own: ROLL is still there.
    EXPECT_EQ(QStringLiteral("1/4"), quarter->text());
    ControlObject::set(ConfigKey("[Channel1]", "play"), 0); ProcessBuffer(); settle();
    // Sync takes deck 2 from 100 to 128 BPM, far beyond its ±8% fader: OVER.
    ControlObject::set(ConfigKey("[Channel1]", "sync_leader"), 1);
    ControlObject::set(ConfigKey("[Channel2]", "sync_enabled"), 1); ProcessBuffer(); settle(); QTest::qWait(150);
    ASSERT_GT(std::abs(ControlObject::get(ConfigKey("[Channel2]", "rate_ratio")) - 1), .08);
    EXPECT_TRUE(strip(2)->state().overRange);
    EXPECT_FALSE(strip(1)->state().overRange);
    // The phase meter compares the follower with the master; the master has
    // nothing to compare with.
    EXPECT_EQ(1, strip(2)->state().referenceDeck);
    EXPECT_EQ(0, strip(1)->state().referenceDeck);
    EXPECT_GE(strip(2)->state().referencePhase, 0);
    EXPECT_TRUE(strip(2)->renderer().overflowingParts().isEmpty());
    capture(QStringLiteral("decks-sync-over-range.png"));
    ControlObject::set(ConfigKey("[Channel2]", "sync_enabled"), 0);
    ControlObject::set(ConfigKey("[Channel2]", "rate_ratio"), 1); ProcessBuffer(); settle(); QTest::qWait(150);
    EXPECT_FALSE(strip(2)->state().overRange);
    // SLIP in MORE, with its lamp on the readout.
    auto* slip = panel->findChild<QPushButton*>("decklab-1-slip_enabled"); ASSERT_NE(nullptr, slip);
    EXPECT_EQ(44, slip->height());
    slip->click(); QCoreApplication::processEvents(); settle();
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel1]", "slip_enabled")));
    EXPECT_TRUE(slip->isChecked()); EXPECT_TRUE(strip(1)->state().slip);
    slip->click(); QCoreApplication::processEvents(); settle();
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "slip_enabled")));
    // LOOP mode: one touch loops 4 beats, the LOOP key lamp shows it from CUE.
    auto* loopMode = panel->findChild<QPushButton*>("decklab-pads-1-mode-loop"); ASSERT_NE(nullptr, loopMode);
    QTest::mousePress(loopMode, Qt::LeftButton); QTest::mouseRelease(loopMode, Qt::LeftButton); settle();
    panel->findChild<QPushButton*>("decklab-hotcue-1-5")->click(); ProcessBuffer(); settle(); QTest::qWait(150);
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel1]", "loop_enabled")));
    capture(QStringLiteral("decks-pads-loop.png"));
    auto* cueMode = panel->findChild<QPushButton*>("decklab-pads-1-mode-cue"); ASSERT_NE(nullptr, cueMode);
    QTest::mousePress(cueMode, Qt::LeftButton); QTest::mouseRelease(cueMode, Qt::LeftButton); settle();
    EXPECT_EQ(QStringLiteral("E"), panel->findChild<QPushButton*>("decklab-hotcue-1-5")->text());
    EXPECT_TRUE(loopMode->accessibleName().contains("loop is running"));
    ControlObject::set(ConfigKey("[Channel1]", "loop_enabled"), 0); settle();
    // Four decks: each card's MORE holds the same pad block, QNT, KEY LOCK,
    // KEY (r5-decks) and SLIP, Track info and GRID.
    QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"), "activated", Q_ARG(int, 2)); QTest::qWait(150);
    EXPECT_EQ(138 + 44 + 48, panel->findChild<QWidget*>("decklab-deck-row")->height());
    panel->findChild<QPushButton*>("decklab-card-1-more")->click(); QCoreApplication::processEvents();
    auto* menu = panel->findChild<QMenu*>("decklab-card-1-more-menu"); ASSERT_NE(nullptr, menu); ASSERT_TRUE(menu->isVisible());
    auto* block = panel->findChild<QWidget*>("decklab-card-pads-1"); ASSERT_NE(nullptr, block);
    EXPECT_TRUE(block->isVisible());
    for (const char* name : {"decklab-card-pads-1-mode-cue", "decklab-card-pads-1-mode-loop", "decklab-card-pads-1-mode-roll",
                 "decklab-card-hotcue-1-1", "decklab-card-hotcue-1-8", "decklab-card-1-quantize", "decklab-card-1-keylock",
                 "decklab-card-1-slip_enabled", "decklab-card-1-track-info", "decklab-card-1-grid",
                 "decklab-card-1-key-sheet"}) {
        auto* key = panel->findChild<QPushButton*>(QString::fromLatin1(name)); ASSERT_NE(nullptr, key) << name;
        EXPECT_TRUE(key->isVisible()) << name; EXPECT_EQ(44, key->height()) << name; EXPECT_GE(key->width(), 44) << name;
        EXPECT_TRUE(menu->rect().contains(QRect(key->mapTo(menu, QPoint()), key->size()))) << name;
    }
    const QString output = qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
    if (!output.isEmpty()) {
        EXPECT_TRUE(menu->grab().save(QDir(output).filePath("decks-four-more-pads.png")));
    }
    menu->close(); QCoreApplication::processEvents();
    for (int deck = 1; deck <= 2; ++deck) EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey(QStringLiteral("[Channel%1]").arg(deck), "play")));
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
    // elsewhere. That ends the hold; no wall clock is involved. (Wait out
    // the double-tap window first: a second touch on the same spot within
    // it latches, by design.)
    QTest::qWait(DeckLabLabFx::kDoubleTapMs + 50);
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

namespace {
// Generated sampler controls: bare ControlObjects, no engine, no audio, no
// files. They exist before the surface, as Mixxx's samplers do.
std::vector<std::unique_ptr<ControlObject>> makeSamplerControls(int first, int last) {
    std::vector<std::unique_ptr<ControlObject>> controls;
    for (int sampler = first; sampler <= last; ++sampler) {
        const QString group = QStringLiteral("[Sampler%1]").arg(sampler);
        for (const char* item : {"track_loaded", "play", "playposition", "duration", "rate_ratio", "bpm", "pregain",
                     "volume", "repeat", "quantize", "sync_enabled", "cue_point", "play_stutter", "cue_gotoandstop",
                     "start_play", "start_stop", "eject", "stop"}) {
            const ConfigKey key(group, QString::fromLatin1(item));
            if (!ControlObject::exists(key)) controls.push_back(std::make_unique<ControlObject>(key));
        }
        for (const char* item : {"rate_ratio", "pregain", "volume"}) ControlObject::set(ConfigKey(group, QString::fromLatin1(item)), 1);
    }
    return controls;
}
// Counts presses of a push control (its 1 of a 1-then-0 pulse).
struct PulseCounter {
    PulseCounter(const QString& group, const QString& item, QObject* context) : proxy(group, item) {
        proxy.connectValueChanged(context, [this](double v) { if (v > 0) ++count; }, Qt::DirectConnection);
    }
    ControlProxy proxy;
    int count = 0;
};
} // namespace

// r4-sampler: a playing pad is a lit lamp that drains on the existing
// display tick; nothing plays audio here.
TEST_F(NativeDeckLabTest, SamplerPadsLightAndDrainOnTheDisplayTick) {
    auto controls = makeSamplerControls(1, 2);
    ControlObject::set(ConfigKey("[Sampler1]", "track_loaded"), 1);
    ControlObject::set(ConfigKey("[Sampler1]", "duration"), 8);
    ControlObject::set(ConfigKey("[Sampler1]", "bpm"), 128);
    WDeckLab surface(config(), nullptr, players.get(), m_pEffectsManager.get());
    surface.resize(800, 480); surface.show(); surface.selectPage(2); QCoreApplication::processEvents();
    auto* pad = surface.findChild<DeckLabSamplerPad*>("decklab-sample-1"); ASSERT_NE(nullptr, pad);
    EXPECT_FALSE(pad->isChecked()); EXPECT_EQ(8, pad->seconds()) << "idle: the sample's length";
    ControlObject::set(ConfigKey("[Sampler1]", "playposition"), .25);
    ControlObject::set(ConfigKey("[Sampler1]", "play"), 1);
    QTest::qWait(250);
    EXPECT_TRUE(pad->isChecked()) << "playing is the filled lamp";
    EXPECT_NEAR(.75 * pad->width(), pad->litWidth(), 1.0) << "the lit part is the length still to play";
    EXPECT_EQ(6, pad->seconds());
    EXPECT_TRUE(pad->accessibleName().endsWith(QStringLiteral(", playing")));
    const QImage image = pad->grab().toImage();
    EXPECT_EQ(QColor(0xf2, 0xf4, 0xf7), image.pixelColor(pad->width() / 2, pad->height() - 3));
    EXPECT_EQ(QColor(0x16, 0x19, 0x1e), image.pixelColor(pad->width() - 8, pad->height() - 3)) << "drained";
    const QString output = qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
    if (!output.isEmpty()) {
        EXPECT_TRUE(surface.grab().save(QDir(output).filePath("sampler-playing.png")));
    }
    ControlObject::set(ConfigKey("[Sampler1]", "playposition"), .5); QTest::qWait(250);
    EXPECT_EQ(4, pad->seconds());
    // STOP ALL takes every playing pad back to its cue.
    PulseCounter stop(QStringLiteral("[Sampler1]"), QStringLiteral("cue_gotoandstop"), &surface);
    auto* stopAll = surface.findChild<QPushButton*>("decklab-samples-stop"); ASSERT_NE(nullptr, stopAll);
    EXPECT_TRUE(stopAll->isEnabled());
    stopAll->click();
    EXPECT_EQ(1, stop.count);
    ControlObject::set(ConfigKey("[Sampler1]", "play"), 0); QTest::qWait(250);
    EXPECT_FALSE(pad->isChecked()); EXPECT_FALSE(stopAll->isEnabled());
    surface.hide();
}

// r4-sampler: hold a pad for the docked edit sheet; the knob edits GAIN
// there and never leaves the page; CLEAR acts on release.
TEST_F(NativeDeckLabTest, SamplerHoldOpensTheEditSheetAndTheKnobEditsGain) {
    // One surface, as on the unit: the fixture's would take the knob turn
    // below as "open Browse" and, through [DeckLabTab] current, drag this
    // surface to Browse with it.
    panel.reset();
    auto controls = makeSamplerControls(1, 1);
    ControlObject::set(ConfigKey("[Sampler1]", "track_loaded"), 1);
    ControlObject::set(ConfigKey("[Sampler1]", "duration"), 4);
    WDeckLab surface(config(), nullptr, players.get(), m_pEffectsManager.get());
    surface.resize(800, 480); surface.show(); surface.selectPage(2); QCoreApplication::processEvents();
    PulseCounter stutter(QStringLiteral("[Sampler1]"), QStringLiteral("play_stutter"), &surface);
    PulseCounter eject(QStringLiteral("[Sampler1]"), QStringLiteral("eject"), &surface);
    auto* pad = surface.findChild<QPushButton*>("decklab-sample-1"); ASSERT_NE(nullptr, pad);
    auto* sheet = surface.findChild<QWidget*>("decklab-sampler-sheet"); ASSERT_NE(nullptr, sheet);
    QTest::mousePress(pad, Qt::LeftButton);
    EXPECT_EQ(1, stutter.count) << "the touch still plays the pad at once";
    QTest::qWait(DeckLabSampler::kHoldMs + 150);
    EXPECT_TRUE(sheet->isVisible()) << "a held pad opens its sheet";
    QTest::mouseRelease(pad, Qt::LeftButton);
    EXPECT_EQ(1, stutter.count);
    EXPECT_TRUE(surface.rect().contains(QRect(sheet->mapTo(&surface, QPoint()), sheet->size())));
    const QString output = qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
    if (!output.isEmpty()) {
        EXPECT_TRUE(surface.grab().save(QDir(output).filePath("sampler-edit-sheet.png")));
    }
    // The knob turns GAIN in 1 dB steps and stays on the page.
    ControlObject::set(ConfigKey("[DeckLabNative]", "browse_move"), 2);
    ControlObject::set(ConfigKey("[DeckLabNative]", "browse_move"), 0);
    settle();
    EXPECT_EQ(2, surface.currentPage());
    EXPECT_NEAR(std::pow(10.0, 2 / 20.0), ControlObject::get(ConfigKey("[Sampler1]", "pregain")), 1e-6);
    // MODE LOOP: the sampler repeats, and the pad remembers it.
    surface.findChild<QPushButton*>("decklab-sampler-sheet-mode-1")->click();
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Sampler1]", "repeat")));
    EXPECT_EQ(1, config()->getValue(ConfigKey("[DeckLabSampler]", "pad_1_mode"), 0));
    // CLEAR (eject) is destructive: on release, never on touch-down.
    auto* clear = surface.findChild<QPushButton*>("decklab-sampler-sheet-clear"); ASSERT_NE(nullptr, clear);
    ASSERT_TRUE(clear->isEnabled());
    QTest::mousePress(clear, Qt::LeftButton); EXPECT_EQ(0, eject.count);
    QTest::mouseRelease(clear, Qt::LeftButton); EXPECT_EQ(1, eject.count);
    EXPECT_FALSE(sheet->isVisible());
    // Back closes the sheet without leaving; the next back opens Browse.
    surface.findChild<DeckLabSampler*>("decklab-sampler")->editPad(1);
    EXPECT_TRUE(sheet->isVisible());
    surface.browseBack(); settle();
    EXPECT_FALSE(sheet->isVisible()); EXPECT_EQ(2, surface.currentPage());
    surface.browseBack(); settle();
    EXPECT_EQ(1, surface.currentPage());
    config()->setValue(ConfigKey("[DeckLabSampler]", "pad_1_mode"), 0);
    surface.hide();
}

// r4-sampler: an empty pad loads from Browse and comes back; the FX lamp is
// Lab FX's sampler send, whichever page changes it.
TEST_F(NativeDeckLabTest, SamplerLoadReturnsToThePadsAndFxFollowsLabFx) {
    m_pEffectsManager->setup(); m_pEffectsManager->ensureDeckLabEffects();
    m_pEngineMixer->prepareAudio(mixxx::audio::SampleRate(44100));
    auto controls = makeSamplerControls(1, 2);
    WDeckLab surface(config(), nullptr, players.get(), m_pEffectsManager.get());
    surface.resize(800, 480); surface.show(); surface.selectPage(2); QCoreApplication::processEvents();
    surface.findChild<QPushButton*>("decklab-sample-2")->click();
    EXPECT_EQ(1, surface.currentPage());
    auto* load = surface.findChild<QPushButton*>("decklab-load-1"); ASSERT_NE(nullptr, load);
    EXPECT_EQ(QStringLiteral("Load pad 2"), load->text());
    ControlObject::set(ConfigKey("[Sampler2]", "track_loaded"), 1); settle();
    EXPECT_EQ(2, surface.currentPage()) << "the loaded pad brings Browse back to the pads";
    auto* fx = surface.findChild<QPushButton*>("decklab-sampler-fx"); ASSERT_NE(nullptr, fx);
    EXPECT_FALSE(fx->isChecked());
    fx->click();
    EXPECT_TRUE(fx->isChecked());
    // Lab FX's own SAMPLER FX key, while it has one (r4-labfx may replace it).
    if (auto* labFx = surface.findChild<QPushButton*>("decklab-fx-samples"); labFx && labFx->isEnabled()) {
        EXPECT_TRUE(labFx->isChecked()) << "one send, two lamps";
        labFx->click();
        QTest::qWait(250);
        EXPECT_FALSE(fx->isChecked()) << "the lamp follows Lab FX on the display tick";
    }
    surface.hide();
}

// r4-sampler: more samplers than one bank (a controller mapping can ask for
// more) become banks of sixteen, numbered on from 17.
TEST_F(NativeDeckLabTest, SamplerBanksFollowTheConfiguredSamplerCount) {
    // The signal-path fixture owns [App],num_samplers, as PlayerManager does
    // in the app; a mapping asking for 32 samplers sets it. (Skipping when it
    // exists meant this never ran.)
    const ConfigKey count("[App]", "num_samplers");
    std::unique_ptr<ControlObject> counter;
    if (!ControlObject::exists(count)) counter = std::make_unique<ControlObject>(count);
    ControlObject::set(count, 32);
    auto controls = makeSamplerControls(1, 32);
    WDeckLab surface(config(), nullptr, players.get(), m_pEffectsManager.get());
    surface.resize(800, 480); surface.show(); surface.selectPage(2); QCoreApplication::processEvents();
    auto* bank = surface.findChild<QPushButton*>("decklab-sampler-bank"); ASSERT_NE(nullptr, bank);
    EXPECT_TRUE(bank->isVisible()); EXPECT_GE(bank->height(), 44);
    EXPECT_EQ(QStringLiteral("BANK A"), bank->text());
    bank->click();
    EXPECT_EQ(QStringLiteral("BANK B"), bank->text());
    auto* first = surface.findChild<QPushButton*>("decklab-sample-1");
    EXPECT_EQ(QStringLiteral("Pad 17, empty: tap to load"), first->accessibleName());
    const QString output = qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
    if (!output.isEmpty()) {
        EXPECT_TRUE(surface.grab().save(QDir(output).filePath("sampler-bank-b.png")));
    }
    first->click();
    EXPECT_EQ(1, surface.currentPage());
    EXPECT_EQ(QStringLiteral("Load pad 17"), surface.findChild<QPushButton*>("decklab-load-1")->text());
    surface.hide();
}

// r7-sampler: REPLACE in a pad's sheet sends Browse to load that pad; the new
// sample brings Browse back to the pads. A Load pad trip started with EDIT on
// (REPLACE or an empty pad) comes back with EDIT still on, loaded or
// cancelled; going anywhere else from Browse ends EDIT.
TEST_F(NativeDeckLabTest, SamplerReplaceLoadsThatPadFromBrowse) {
    // One surface, as on the unit: the fixture's follows [Tab] page changes
    // a beat late and would drag this one back to Browse.
    panel.reset();
    auto controls = makeSamplerControls(1, 3);
    ControlObject::set(ConfigKey("[Sampler3]", "track_loaded"), 1);
    ControlObject::set(ConfigKey("[Sampler3]", "duration"), 4);
    WDeckLab surface(config(), nullptr, players.get(), m_pEffectsManager.get());
    surface.resize(800, 480); surface.show(); surface.selectPage(2); QCoreApplication::processEvents();
    auto* page = surface.findChild<DeckLabSampler*>("decklab-sampler"); ASSERT_NE(nullptr, page);
    PulseCounter stutter(QStringLiteral("[Sampler3]"), QStringLiteral("play_stutter"), &surface);
    surface.findChild<QPushButton*>("decklab-sampler-edit")->click();
    ASSERT_TRUE(page->editMode());
    QTest::mouseClick(surface.findChild<QPushButton*>("decklab-sample-3"), Qt::LeftButton);
    EXPECT_EQ(0, stutter.count) << "EDIT: the tap edits, never plays";
    ASSERT_EQ(3, page->sheetSampler());
    auto* replace = surface.findChild<QPushButton*>("decklab-sampler-sheet-replace"); ASSERT_NE(nullptr, replace);
    EXPECT_TRUE(replace->isVisible()); EXPECT_GE(replace->height(), 44);
    // Never while the pad plays.
    ControlObject::set(ConfigKey("[Sampler3]", "play"), 1); QTest::qWait(250);
    EXPECT_FALSE(replace->isEnabled());
    replace->click();
    EXPECT_EQ(2, surface.currentPage());
    ControlObject::set(ConfigKey("[Sampler3]", "play"), 0); QTest::qWait(250);
    ASSERT_TRUE(replace->isEnabled());
    QTest::mousePress(replace, Qt::LeftButton);
    EXPECT_EQ(2, surface.currentPage()) << "on release";
    QTest::mouseRelease(replace, Qt::LeftButton); settle(); // [Tab] current follows, queued
    EXPECT_EQ(1, surface.currentPage()) << "Browse, loading pad 3";
    EXPECT_EQ(QStringLiteral("Load pad 3"), surface.findChild<QPushButton*>("decklab-load-1")->text());
    // Browse loads the new sample into pad 3: back to the pads, EDIT still on.
    ControlObject::set(ConfigKey("[Sampler3]", "track_loaded"), 0); settle();
    ControlObject::set(ConfigKey("[Sampler3]", "track_loaded"), 1); settle();
    EXPECT_EQ(2, surface.currentPage()) << "the replaced pad brings Browse back to the pads";
    auto* edit = surface.findChild<QPushButton*>("decklab-sampler-edit");
    EXPECT_TRUE(page->editMode()) << "EDIT survives the Load pad trip";
    EXPECT_TRUE(edit->isChecked());
    EXPECT_FALSE(page->sheetOpen());
    auto* pad3 = surface.findChild<DeckLabSamplerPad*>("decklab-sample-3");
    EXPECT_TRUE(pad3->state().editing);
    // The next tap still edits, pad to pad.
    QTest::mouseClick(pad3, Qt::LeftButton);
    EXPECT_EQ(0, stutter.count);
    EXPECT_EQ(3, page->sheetSampler());
    surface.findChild<QPushButton*>("decklab-sampler-sheet-done")->click();
    // An empty pad in EDIT, then Cancel (back to pads): EDIT still on.
    QTest::mouseClick(surface.findChild<QPushButton*>("decklab-sample-1"), Qt::LeftButton); settle();
    ASSERT_EQ(1, surface.currentPage());
    auto* cancel = surface.findChild<QPushButton*>("decklab-load-2"); ASSERT_NE(nullptr, cancel);
    EXPECT_EQ(QStringLiteral("Cancel"), cancel->text());
    cancel->click(); settle();
    EXPECT_EQ(2, surface.currentPage());
    EXPECT_TRUE(page->editMode()) << "a cancelled load keeps EDIT on too";
    EXPECT_TRUE(edit->isChecked());
    // An empty pad in EDIT, then Decks: EDIT ends, and stays off back on the pads.
    QTest::mouseClick(surface.findChild<QPushButton*>("decklab-sample-1"), Qt::LeftButton); settle();
    ASSERT_EQ(1, surface.currentPage());
    surface.findChild<QPushButton*>("decklab-tab-0")->click(); settle();
    EXPECT_EQ(0, surface.currentPage());
    surface.findChild<QPushButton*>("decklab-tab-2")->click(); settle();
    EXPECT_EQ(2, surface.currentPage());
    EXPECT_FALSE(page->editMode()) << "another page ends EDIT";
    EXPECT_FALSE(edit->isChecked());
    // EDIT off when the trip starts: it comes back off.
    QTest::mouseClick(surface.findChild<QPushButton*>("decklab-sample-1"), Qt::LeftButton); settle();
    ASSERT_EQ(1, surface.currentPage());
    surface.findChild<QPushButton*>("decklab-load-2")->click(); settle();
    EXPECT_EQ(2, surface.currentPage());
    EXPECT_FALSE(page->editMode());
    // EDIT on, then Lab FX straight from the pads: EDIT ends.
    edit->click();
    ASSERT_TRUE(page->editMode());
    surface.findChild<QPushButton*>("decklab-tab-3")->click(); settle();
    surface.findChild<QPushButton*>("decklab-tab-2")->click(); settle();
    EXPECT_FALSE(page->editMode());
    surface.hide();
}

// r7-sampler: TO PAD in a deck's MORE sheet saves exactly the deck's loop,
// while it plays, as a WAV in the samples folder and loads it onto the first
// empty pad as a LOOP pad with the track's tempo. A real decoder and a real
// player stand in for sampler 3; nothing plays audio.
TEST_F(NativeDeckLabTest, ToPadSavesTheDeckLoopOnTheFirstEmptyPad) {
    panel.reset();
    ASSERT_TRUE(SoundSourceProxy::isFileTypeSupported(QStringLiteral("wav")) || SoundSourceProxy::registerProviders());
    QTemporaryDir samples;
    decklab::capture::setSamplesFolderForTesting(samples.filePath(QStringLiteral("Deck Lab Samples")));
    // Pads 1 and 2 are taken; sampler 3 is a real (empty) player.
    auto controls = makeSamplerControls(1, 2);
    ControlObject::set(ConfigKey("[Sampler1]", "track_loaded"), 1);
    ControlObject::set(ConfigKey("[Sampler2]", "track_loaded"), 1);
    auto sampler = std::make_unique<Deck>(nullptr, config(), m_pEngineMixer.get(), m_pEffectsManager.get(),
            EngineChannel::CENTER, m_pEngineMixer->registerChannelGroup(QStringLiteral("[Sampler3]")));
    players->others.insert(QStringLiteral("[Sampler3]"), sampler.get());
    WDeckLab surface(config(), nullptr, players.get(), m_pEffectsManager.get());
    for (int deck = 1; deck <= 4; ++deck) {
        auto* slot = new QWidget(&surface); slot->setStyleSheet("background:#000");
        surface.addWaveform(slot, deck);
        auto* overview = new QWidget(&surface); overview->setStyleSheet("background:#181818");
        surface.addOverview(overview, deck);
    }
    surface.resize(800, 480); surface.show(); surface.selectPage(0); QCoreApplication::processEvents();
    // A generated sine on deck 1 at 120 BPM.
    auto track = Track::newTemporary(getTestDir().filePath(QStringLiteral("sine-30.wav")));
    track->setTitle(QStringLiteral("Generated Sine"));
    track->setAudioProperties(mixxx::audio::ChannelCount::mono(), mixxx::audio::SampleRate(44100),
            mixxx::audio::Bitrate(), mixxx::Duration::fromSeconds(30));
    ASSERT_TRUE(track->trySetBpm(120));
    loadTrack(m_pMixerDeck1.get(), track);
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    settle();
    auto* toPad = surface.findChild<QPushButton*>("decklab-1-to-pad"); ASSERT_NE(nullptr, toPad);
    EXPECT_EQ(QStringLiteral("TO PAD"), toPad->text());
    EXPECT_EQ(44, toPad->height());
    EXPECT_NE(nullptr, surface.findChild<QPushButton*>("decklab-card-1-to-pad")) << "four decks: in the card's MORE";
    EXPECT_FALSE(toPad->isEnabled()) << "no loop yet";
    // A four-beat loop, and the deck plays (DJs capture live).
    ControlObject::set(ConfigKey("[Channel1]", "beatloop_4_activate"), 1);
    ControlObject::set(ConfigKey("[Channel1]", "beatloop_4_activate"), 0);
    ControlObject::set(ConfigKey("[Channel1]", "play"), 1);
    ProcessBuffer(); settle();
    ASSERT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel1]", "loop_enabled")));
    ASSERT_TRUE(QTest::qWaitFor([&] { return toPad->isEnabled(); }, 2000)) << "a loop lights TO PAD";
    const double loopFrames = (ControlObject::get(ConfigKey("[Channel1]", "loop_end_position")) -
                                      ControlObject::get(ConfigKey("[Channel1]", "loop_start_position"))) / 2;
    EXPECT_NEAR(2 * 44100, loopFrames, 2) << "four beats at 120 BPM";
    const QString output = qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
    if (!output.isEmpty()) {
        surface.findChild<QPushButton*>("decklab-1-more")->click(); settle();
        auto* menu = surface.findChild<QMenu*>("decklab-1-more-menu");
        if (menu && menu->isVisible()) EXPECT_TRUE(menu->grab().save(QDir(output).filePath("decks-more-to-pad.png")));
        if (menu) menu->close();
        settle();
    }
    auto* capture = surface.findChild<DeckLabLoopCapture*>("decklab-loop-capture"); ASSERT_NE(nullptr, capture);
    int padUsed = -1;
    decklab::capture::Result saved;
    QObject::connect(capture, &DeckLabLoopCapture::finished, &surface,
            [&](int, int pad, const decklab::capture::Result& result) { padUsed = pad; saved = result; });
    toPad->click();
    ASSERT_TRUE(QTest::qWaitFor([&] { ProcessBuffer(); return padUsed >= 0; }, 10000));
    EXPECT_EQ(3, padUsed) << "the first empty pad";
    EXPECT_EQ(samples.filePath(QStringLiteral("Deck Lab Samples/Generated Sine - Loop 1 bar.wav")), saved.path);
    EXPECT_TRUE(QFile::exists(saved.path));
    EXPECT_NEAR(loopFrames, saved.frames, 1) << "exactly the loop";
    EXPECT_EQ(44100, saved.sampleRate);
    EXPECT_NEAR(120.0, saved.bpm, 0.1);
    auto* toast = surface.findChild<QLabel*>("decklab-toast"); ASSERT_NE(nullptr, toast);
    EXPECT_TRUE(toast->isVisible());
    EXPECT_EQ(QStringLiteral("Saved to pad 3"), toast->text());
    if (!output.isEmpty()) EXPECT_TRUE(surface.grab().save(QDir(output).filePath("decks-to-pad-saved.png")));
    // Pad 3 holds the loop as a LOOP pad with the deck's tempo, grid locked.
    ASSERT_TRUE(QTest::qWaitFor([&] {
        ProcessBuffer();
        return ControlObject::get(ConfigKey("[Sampler3]", "track_loaded")) > 0;
    }, 5000));
    for (int n = 0; n < 4; ++n) ProcessBuffer();
    settle();
    const TrackPointer loaded = sampler->getLoadedTrack(); ASSERT_TRUE(loaded);
    EXPECT_EQ(saved.path, loaded->getLocation());
    EXPECT_EQ(QStringLiteral("Generated Sine - Loop 1 bar"), loaded->getTitle()) << "the whole name, not split at the dash";
    EXPECT_TRUE(loaded->isBpmLocked());
    EXPECT_NEAR(120.0, loaded->getBpm(), 0.1);
    EXPECT_EQ(1, config()->getValue(ConfigKey("[DeckLabSampler]", "pad_3_mode"), 0)) << "a captured loop is a LOOP pad";
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Sampler3]", "repeat")));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Sampler3]", "play"))) << "loaded, never played";
    surface.selectPage(2); QTest::qWait(250);
    auto* pad3 = surface.findChild<DeckLabSamplerPad*>("decklab-sample-3"); ASSERT_NE(nullptr, pad3);
    EXPECT_TRUE(pad3->state().loaded);
    EXPECT_EQ(DeckLabSamplerEngine::Mode::Loop, pad3->state().mode);
    if (!output.isEmpty()) EXPECT_TRUE(surface.grab().save(QDir(output).filePath("sampler-captured-loop.png")));
    ControlObject::set(ConfigKey("[Channel1]", "play"), 0); ProcessBuffer();
    config()->setValue(ConfigKey("[DeckLabSampler]", "pad_3_mode"), 0);
    decklab::capture::setSamplesFolderForTesting(QString());
    surface.hide();
    players->others.clear();
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
    // r4-browse: the source home's first tile asks for a USB stick while none
    // is in and is never highlighted; the knob starts on the next one.
    EXPECT_FALSE(home->item(0)->flags() & Qt::ItemIsEnabled);
    const int first = 1;
    EXPECT_EQ(first, home->currentRow());
    EXPECT_EQ(0, source->activations);
    pulseControl("[Library]", "MoveVertical", 1);
    EXPECT_EQ(first + 1, home->currentRow());
    pulseControl("[Library]", "MoveVertical", -1);
    EXPECT_EQ(first, home->currentRow());
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
    // A focused settings combo box never receives the turn. r4-system: on
    // Settings the knob drives Settings' own focus ring (CDJ UTILITY).
    panel->selectPage(4);
    auto* range = panel->findChild<QComboBox*>("decklab-tempo-range-1"); ASSERT_NE(nullptr, range);
    const int rangeIndex = range->currentIndex();
    range->setFocus(); QCoreApplication::processEvents();
    pulseControl("[Playlist]", "SelectTrackKnob", 1);
    EXPECT_EQ(4, panel->currentPage());
    EXPECT_EQ(rangeIndex, range->currentIndex());
    auto* settingsPage = panel->findChild<DeckLabSettingsPage*>(); ASSERT_NE(nullptr, settingsPage);
    EXPECT_NE(nullptr, settingsPage->knob()->current());
    EXPECT_EQ(QStringLiteral("Genres"), sidebar->currentIndex().data().toString());
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
    // Tapping the ON AIR target explains instead of silently refusing:
    // r4-browse (B-33) in the dock tile itself and just above it.
    QTest::mouseClick(load, Qt::LeftButton);
    EXPECT_TRUE(toast->isVisible());
    EXPECT_EQ(QStringLiteral("Deck 2 is on air. Pause it first."), toast->text());
    EXPECT_TRUE(static_cast<DeckLabDockTile*>(load)->refusing());
    EXPECT_LE(toast->geometry().bottom(), load->mapTo(panel.get(), QPoint()).y());
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
    EXPECT_TRUE(toast->text().startsWith(QStringLiteral("Deck 2 is on air")));
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
    ASSERT_GE(list->count(), 4);
    // r4-browse: row 0 is the home's "plug in a USB stick" tile (none here).
    const int first = 1;
    list->setCurrentRow(first);
    const ConfigKey knob("[Library]", "MoveVertical");
    const ConfigKey focus("[Library]", "focused_widget");
    auto* tools = panel->findChild<QPushButton*>("decklab-browse-tools"); ASSERT_NE(nullptr, tools);
    // 1. A menu really is open: the knob belongs to it and Browse stays put.
    tools->click(); QCoreApplication::processEvents();
    QPointer<QMenu> menu = panel->findChild<QMenu*>("decklab-browse-tools-menu");
    ASSERT_TRUE(menu); ASSERT_TRUE(menu->isVisible());
    const int redirected = decklab::knobEventsRedirected();
    ControlObject::set(knob, 1); QCoreApplication::processEvents();
    EXPECT_EQ(first, list->currentRow());
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
    EXPECT_EQ(first + 1, list->currentRow());
    EXPECT_LT(ControlObject::get(focus), 4);
    // The Numark overlay's turn control takes the same route.
    ControlObject::set(ConfigKey("[Playlist]", "SelectPlaylist"), 1); QCoreApplication::processEvents();
    EXPECT_EQ(first + 2, list->currentRow());
    ControlObject::set(ConfigKey("[Playlist]", "SelectPlaylist"), -2); QCoreApplication::processEvents();
    EXPECT_EQ(first, list->currentRow());
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
    list->setCurrentRow(first);
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
    ASSERT_TRUE(selectRow(QStringLiteral("DJ USB"))) << listTexts().join(" / ").toStdString();
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
    EXPECT_EQ((QStringList{"DJ USB", "USB stick", "USB library", "Library", "More sources"}), listTexts());
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
    EXPECT_EQ(QStringList{QStringLiteral("USB stick")}, crumbs());
    ASSERT_TRUE(selectRow(QStringLiteral("House")));
    panel->browsePress(); QCoreApplication::processEvents();
    EXPECT_EQ((QStringList{"USB stick", "House"}), crumbs());
    EXPECT_EQ(QStringList{QStringLiteral("No music here")}, listTexts());
    panel->browseBack(); QCoreApplication::processEvents();
    // Its tracks come from Mixxx's file browser for that folder.
    mirrorFolder(plain);
    ASSERT_TRUE(selectRow(QStringLiteral("Tracks in this folder")));
    panel->browsePress(); QCoreApplication::processEvents();
    EXPECT_EQ(1, computer->activations);
    EXPECT_TRUE(library->isVisible());
    EXPECT_EQ((QStringList{"USB stick", "Tracks"}), crumbs());
    panel->browseBack(); panel->browseBack(); QCoreApplication::processEvents();
    ASSERT_NE(nullptr, list->currentItem());
    EXPECT_EQ(QStringLiteral("USB stick"), list->currentItem()->text());
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
    EXPECT_EQ(QStringLiteral("DJ USB"), list->currentItem()->text());
    // r4-browse (move 1): with one stick, the source home with that stick
    // highlighted, so its tile says what is on it; a press opens it.
    QDir(media.filePath(QStringLiteral("decklab/3A1F-0C2D"))).removeRecursively();
    panel->selectPage(0); usb->click(); QCoreApplication::processEvents();
    EXPECT_EQ(1, panel->currentPage());
    EXPECT_EQ(QStringList{QStringLiteral("Sources")}, crumbs());
    ASSERT_NE(nullptr, list->currentItem());
    EXPECT_EQ(QStringLiteral("USB stick"), list->currentItem()->text());
    panel->browsePress(); QCoreApplication::processEvents();
    EXPECT_EQ(QStringList{QStringLiteral("USB stick")}, crumbs());
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
    ASSERT_TRUE(selectRow(QStringLiteral("DJ USB"))) << listTexts().join(" / ").toStdString();
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
    ASSERT_TRUE(selectRow(QStringLiteral("DJ USB")));
    EXPECT_EQ(QStringLiteral("rekordbox · couldn't read the export"),
            list->currentItem()->data(DeckLabListDelegate::SubtitleRole).toString());
}

// qw-library: B-49..B-53. The Match bar shows the reference deck in its
// colour, the BPM window and direction, the key rule, Only and a clear.
TEST_F(NativeDeckLabTest, MatchBarShowsDeckWindowDirectionAndClears) {
    using Tone = DeckLabActionButton::Tone;
    panel->openCollection(); QCoreApplication::processEvents();
    auto* bar = panel->findChild<QWidget*>("decklab-match-bar"); ASSERT_NE(nullptr, bar);
    auto* summary = dynamic_cast<DeckLabTwoLineLabel*>(panel->findChild<QWidget*>("decklab-match-summary")); ASSERT_NE(nullptr, summary);
    auto* chip = dynamic_cast<DeckLabDeckChip*>(panel->findChild<QPushButton*>("decklab-match-deck")); ASSERT_NE(nullptr, chip);
    auto* match = static_cast<DeckLabActionButton*>(panel->findChild<QPushButton*>("decklab-browse-match"));
    auto* only = static_cast<DeckLabActionButton*>(panel->findChild<QPushButton*>("decklab-match-only"));
    auto* faster = static_cast<DeckLabActionButton*>(panel->findChild<QPushButton*>("decklab-match-faster"));
    auto* both = static_cast<DeckLabActionButton*>(panel->findChild<QPushButton*>("decklab-match-both"));
    ASSERT_TRUE(match && only && faster && both);
    ASSERT_TRUE(bar->isVisible());
    // No deck loaded: neutral, never the match green (B-50). r5-polish:
    // nothing in the bar is cut off at 800x480 ("Load a deck to mat...").
    EXPECT_EQ(QStringLiteral("Load a deck first"), summary->primary());
    EXPECT_FALSE(summary->active());
    EXPECT_EQ(800, panel->width());
    EXPECT_TRUE(summary->textFits()) << summary->width();
    EXPECT_TRUE(chip->textFits());
    EXPECT_TRUE(only->textFits());
    capture(QStringLiteral("browse-match-bar-empty.png"));
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
    EXPECT_TRUE(summary->textFits()) << summary->secondary().toStdString();
    EXPECT_TRUE(chip->textFits()) << chip->detail().toStdString();
    EXPECT_TRUE(only->textFits()) << only->detail().toStdString();
    // A big count falls back to the number alone rather than "123456 tr...",
    // through the bar's own update; a small one keeps its unit.
    panel->setMatchCountForTesting(123456);
    EXPECT_TRUE(only->textFits()) << only->detail().toStdString();
    EXPECT_EQ(QLocale().toString(123456), only->shownDetail());
    panel->setMatchCountForTesting(3);
    EXPECT_EQ(QStringLiteral("3 tracks"), only->shownDetail());
    EXPECT_TRUE(only->textFits());
    panel->setMatchCountForTesting(-1);
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

// r4-browse (move 1, B-01..B-06, B-48): the source home is a 2x2 grid of
// tiles in reading order and an "Also" row; each tile says what is inside
// and its state; Back on the home stays, twice goes to Decks.
TEST_F(NativeDeckLabTest, SourceHomeIsTilesInReadingOrderWithStates) {
    panel->selectPage(1); QCoreApplication::processEvents();
    auto* list = browseList(); ASSERT_NE(nullptr, list); ASSERT_TRUE(list->isVisible());
    // No stick in: the first tile asks for one and is never highlighted.
    EXPECT_EQ((QStringList{"No USB stick", "USB library", "Library", "More sources"}), listTexts());
    EXPECT_FALSE(list->item(0)->flags() & Qt::ItemIsEnabled);
    EXPECT_EQ(static_cast<int>(decklab::SourceState::Missing), list->item(0)->data(DeckLabListDelegate::StateRole).toInt());
    EXPECT_EQ(1, list->currentRow());
    for (int row = 0; row < 4; ++row) EXPECT_EQ(1, list->item(row)->data(DeckLabListDelegate::TileRole).toInt()) << row;
    // The tiles take the width (the info pane, Back and MATCH step aside), two per row.
    EXPECT_FALSE(panel->findChild<QWidget*>("decklab-browse-pane")->isVisible());
    EXPECT_FALSE(panel->findChild<QWidget*>("decklab-browse-back")->isVisible());
    EXPECT_FALSE(panel->findChild<QWidget*>("decklab-browse-match")->isVisible());
    const QRect a = list->visualItemRect(list->item(0));
    const QRect b = list->visualItemRect(list->item(1));
    const QRect c = list->visualItemRect(list->item(2));
    EXPECT_EQ(a.top(), b.top()); EXPECT_GT(b.left(), a.right()); EXPECT_GT(c.top(), a.bottom());
    EXPECT_GE(a.width(), 380); EXPECT_GE(a.height(), 96);
    EXPECT_TRUE(list->viewport()->rect().contains(list->visualItemRect(list->item(3))));
    // The knob walks them in reading order and skips the empty one.
    ControlObject::set(ConfigKey("[Playlist]", "SelectPlaylist"), -1); QCoreApplication::processEvents();
    EXPECT_EQ(1, list->currentRow());
    ControlObject::set(ConfigKey("[Playlist]", "SelectPlaylist"), 2); QCoreApplication::processEvents();
    EXPECT_EQ(3, list->currentRow());
    // Sticks come first; the fifth source becomes the first "Also" chip.
    makeStick(QStringLiteral("3A1F-0C2D"), true);
    makeStick(QStringLiteral("55AA-1234"), false);
    ControlObject::set(ConfigKey("[DeckLabNative]", "browse_sources"), 1);
    ControlObject::set(ConfigKey("[DeckLabNative]", "browse_sources"), 0);
    QCoreApplication::processEvents();
    EXPECT_EQ((QStringList{"DJ USB", "USB stick", "USB library", "Library", "More sources"}), listTexts());
    EXPECT_EQ(2, list->item(4)->data(DeckLabListDelegate::TileRole).toInt());
    EXPECT_TRUE(list->item(4)->data(DeckLabListDelegate::AlsoLeadRole).toBool());
    EXPECT_EQ(static_cast<int>(decklab::SourceState::NotRead), list->item(0)->data(DeckLabListDelegate::StateRole).toInt());
    EXPECT_EQ(static_cast<int>(decklab::SourceState::None), list->item(1)->data(DeckLabListDelegate::StateRole).toInt());
    EXPECT_FALSE(list->item(1)->data(DeckLabListDelegate::DetailRole).toString().isEmpty()); // its size
    capture(QStringLiteral("browse-source-home-tiles.png"));
    // Opening the rekordbox stick starts a read; its tile says so on the home.
    ASSERT_TRUE(selectRow(QStringLiteral("DJ USB")));
    panel->browsePress(); QCoreApplication::processEvents();
    panel->browseBack(); QCoreApplication::processEvents();
    EXPECT_EQ(QStringList{QStringLiteral("Sources")}, crumbs());
    EXPECT_EQ(static_cast<int>(decklab::SourceState::Reading), list->item(0)->data(DeckLabListDelegate::StateRole).toInt());
    // Holding Back anywhere returns to the home (touch: 600 ms on the header Back).
    ASSERT_TRUE(selectRow(QStringLiteral("USB stick")));
    panel->browsePress(); QCoreApplication::processEvents();
    ASSERT_EQ(QStringList{QStringLiteral("USB stick")}, crumbs());
    auto* back = panel->findChild<QPushButton*>("decklab-browse-back"); ASSERT_NE(nullptr, back);
    ASSERT_TRUE(back->isVisible());
    QTest::mousePress(back, Qt::LeftButton); QTest::qWait(750); QTest::mouseRelease(back, Qt::LeftButton);
    QCoreApplication::processEvents();
    EXPECT_EQ(QStringList{QStringLiteral("Sources")}, crumbs());
    EXPECT_EQ(1, panel->currentPage());
    // Back on the home has nowhere to go: it stays; a second Back goes to Decks.
    panel->browseBack(); QCoreApplication::processEvents();
    EXPECT_EQ(1, panel->currentPage());
    panel->browseBack(); QCoreApplication::processEvents();
    EXPECT_EQ(0, panel->currentPage());
}

// r4-browse (move 3, B-25, B-29..B-31, B-56..B-59): the deck dock replaces
// the strip under Browse and the pane's LOAD buttons; an on-air tile refuses
// in place.
TEST_F(NativeDeckLabTest, DeckDockIsTheLoadTargetAndRefusesOnAirInPlace) {
    loadGenerated(1, 122, mixxx::track::io::key::A_MINOR); loadGenerated(2, 128); ProcessBuffer();
    ControlObject::set(ConfigKey("[Channel1]", "play"), 1); ProcessBuffer(); QTest::qWait(150);
    panel->openCollection(); QCoreApplication::processEvents(); settle();
    auto* dock = panel->findChild<QWidget*>("decklab-deck-dock"); ASSERT_NE(nullptr, dock);
    ASSERT_TRUE(dock->isVisible());
    EXPECT_FALSE(panel->waveformContainer()->isVisible());
    auto* tile1 = static_cast<DeckLabDockTile*>(panel->findChild<QPushButton*>("decklab-load-1"));
    auto* tile2 = static_cast<DeckLabDockTile*>(panel->findChild<QPushButton*>("decklab-load-2"));
    ASSERT_TRUE(tile1 && tile2);
    EXPECT_TRUE(dock->isAncestorOf(tile1));
    const QRect r1(tile1->mapTo(panel.get(), QPoint()), tile1->size());
    EXPECT_EQ(64, tile1->height()); EXPECT_EQ(panel->height() - 1, r1.bottom()); EXPECT_GE(tile1->width(), 390);
    EXPECT_TRUE(tile2->isVisible()); EXPECT_FALSE(panel->findChild<QPushButton*>("decklab-load-3")->isVisible());
    // Each tile shows its deck: state word, title, key and BPM, in white.
    EXPECT_EQ(decklab::DockWord::OnAir, decklab::dockWord(tile1->deckState()));
    EXPECT_EQ(QStringLiteral("Generated Tone 122"), tile1->deckState().title);
    EXPECT_EQ(QLocale().toString(122.0, 'f', 1), tile1->deckState().bpm);
    EXPECT_FALSE(tile1->deckState().key.isEmpty());
    EXPECT_NE(decklab::DockWord::Empty, decklab::dockWord(tile2->deckState()));
    EXPECT_EQ(QStringLiteral("Generated Tone 128"), tile2->deckState().title);
    // The info pane keeps the card and Preview only.
    auto* pane = panel->findChild<QWidget*>("decklab-browse-pane"); ASSERT_NE(nullptr, pane);
    EXPECT_FALSE(pane->isAncestorOf(tile2));
    // The fixture has no track table: nothing is highlighted, so no LOAD.
    EXPECT_FALSE(tile2->loadReady());
    // A tap on the on-air tile refuses in place and says so just above it.
    auto* toast = panel->findChild<QLabel*>("decklab-toast"); ASSERT_NE(nullptr, toast);
    QTest::mouseClick(tile1, Qt::LeftButton);
    EXPECT_TRUE(tile1->refusing());
    EXPECT_TRUE(toast->isVisible());
    EXPECT_EQ(QStringLiteral("Deck 1 is on air. Pause it first."), toast->text());
    const QRect shown(toast->mapTo(panel.get(), QPoint()), toast->size());
    EXPECT_LE(shown.bottom(), r1.top());
    EXPECT_TRUE(shown.left() < r1.right() && shown.right() > r1.left());
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel1]", "play")));
    // Four decks: four 200 px tiles, decks 3 and 4 always visible.
    QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"), "activated", Q_ARG(int, 2));
    QCoreApplication::processEvents(); settle();
    auto* tile4 = static_cast<DeckLabDockTile*>(panel->findChild<QPushButton*>("decklab-load-4"));
    ASSERT_TRUE(tile4->isVisible());
    EXPECT_GE(tile4->width(), 190); EXPECT_LE(tile4->width(), 200);
    EXPECT_EQ(4, tile4->deck());
    EXPECT_EQ(decklab::DockWord::Empty, decklab::dockWord(tile4->deckState()));
    capture(QStringLiteral("browse-deck-dock.png"));
    ControlObject::set(ConfigKey("[Channel1]", "play"), 0); ProcessBuffer();
}

// r4-browse (move 4, C-02, C-05): the knob legend says what TURN, PRESS and
// SHIFT+PRESS do here, and MENU while a menu owns the knob.
TEST_F(NativeDeckLabTest, KnobLegendFollowsTheLevelAndSaysMenu) {
    using decklab::LegendLevel;
    panel->selectPage(1); QCoreApplication::processEvents();
    auto* legend = dynamic_cast<DeckLabKnobLegend*>(panel->findChild<QWidget*>("decklab-knob-legend")); ASSERT_NE(nullptr, legend);
    EXPECT_TRUE(legend->isVisible()); EXPECT_EQ(24, legend->height());
    EXPECT_EQ(LegendLevel::Home, legend->level());
    // Back with nowhere to go: the legend flashes and says Back again goes to Decks.
    panel->browseBack(); QCoreApplication::processEvents();
    EXPECT_EQ(1, panel->currentPage());
    EXPECT_TRUE(legend->flashing());
    EXPECT_EQ(LegendLevel::HomeBackArmed, legend->level());
    ASSERT_TRUE(selectRow(QStringLiteral("Library")));
    panel->browsePress(); QCoreApplication::processEvents();
    EXPECT_EQ(LegendLevel::List, legend->level());
    // In a track list the press previews (C-02).
    panel->openCollection(); QCoreApplication::processEvents();
    EXPECT_EQ(LegendLevel::Tracks, legend->level());
    // A menu that really owns the knob: MENU, and a redirected turn flashes it (C-05).
    QTest::qWait(200);
    panel->findChild<QPushButton*>("decklab-browse-tools")->click(); QCoreApplication::processEvents();
    QPointer<QMenu> menu = panel->findChild<QMenu*>("decklab-browse-tools-menu");
    ASSERT_TRUE(menu); ASSERT_TRUE(menu->isVisible());
    QCoreApplication::processEvents();
    EXPECT_EQ(LegendLevel::Menu, legend->level());
    ControlObject::set(ConfigKey("[Library]", "MoveVertical"), 1); QCoreApplication::processEvents();
    EXPECT_TRUE(legend->flashing());
    menu->close(); QCoreApplication::processEvents(); QCoreApplication::processEvents();
    EXPECT_EQ(LegendLevel::Tracks, legend->level());
}

// r4-browse (move 5, B-11): "Mixes with deck N" leads a source's categories,
// live in the deck colour, and opens its own list.
TEST_F(NativeDeckLabTest, MixesWithDeckRowLeadsTheCategoriesAndOpensItsList) {
    loadGenerated(1, 122, mixxx::track::io::key::A_MINOR); ProcessBuffer(); QTest::qWait(250);
    panel->selectPage(1); QCoreApplication::processEvents();
    ASSERT_TRUE(selectRow(QStringLiteral("Library")));
    panel->browsePress(); QCoreApplication::processEvents();
    auto* list = browseList(); ASSERT_NE(nullptr, list);
    ASSERT_GE(list->count(), 2);
    EXPECT_EQ(decklab::mixesTitle(1), list->item(0)->text());
    EXPECT_EQ(QStringLiteral("mixes"), list->item(0)->data(DeckLabListDelegate::KeyRole).toString());
    EXPECT_EQ(deckLabDeckColor(1), list->item(0)->data(DeckLabListDelegate::AccentRole).value<QColor>());
    EXPECT_TRUE(list->item(0)->data(DeckLabListDelegate::SubtitleRole).toString().contains(QLocale().toString(122.0, 'f', 1)))
            << list->item(0)->data(DeckLabListDelegate::SubtitleRole).toString().toStdString();
    EXPECT_EQ(QStringLiteral("All Tracks"), list->item(1)->text());
    // A source opens on what mixes with the reference deck.
    EXPECT_EQ(0, list->currentRow());
    auto* card = static_cast<DeckLabTrackCard*>(panel->findChild<QWidget*>("decklab-track-card"));
    ASSERT_NE(nullptr, card);
    EXPECT_EQ(decklab::mixesTitle(1), card->title());
    // Its list: the fixture has no track table, so nothing mixes yet.
    panel->browsePress(); QCoreApplication::processEvents();
    EXPECT_EQ((QStringList{QStringLiteral("Library"), decklab::mixesTitle(1) + QStringLiteral(" · 0")}), crumbs());
    auto* legend = dynamic_cast<DeckLabKnobLegend*>(panel->findChild<QWidget*>("decklab-knob-legend")); ASSERT_NE(nullptr, legend);
    EXPECT_EQ(decklab::LegendLevel::Tracks, legend->level());
    panel->browseBack(); QCoreApplication::processEvents();
    EXPECT_EQ(QStringList{QStringLiteral("Library")}, crumbs());
    EXPECT_EQ(0, list->currentRow());
    // Back on All Tracks after opening it.
    ASSERT_TRUE(selectRow(QStringLiteral("All Tracks")));
    panel->browsePress(); QCoreApplication::processEvents();
    panel->browseBack(); QCoreApplication::processEvents();
    ASSERT_NE(nullptr, list->currentItem());
    EXPECT_EQ(QStringLiteral("All Tracks"), list->currentItem()->text());
}

// r5-history: a generated history (no database): the history playlists and
// tracks a test sets up, and what Mixxx's setlog does when a track becomes
// audible.
namespace {
class GeneratedSets final : public decklab::history::Backend {
  public:
    QVector<decklab::history::Playlist> lists; // newest first
    QHash<int, QVector<decklab::history::Entry>> tracks;
    QVector<int> starts;
    int newSets = 0;
    QVector<decklab::history::Playlist> playlists() override {
        for (auto& list : lists) {
            const auto rows = tracks.value(list.id);
            list.count = static_cast<int>(rows.size());
            list.first = rows.isEmpty() ? QDateTime() : rows.first().played;
            list.last = rows.isEmpty() ? QDateTime() : rows.last().played;
        }
        return lists;
    }
    QVector<decklab::history::Entry> entries(int playlistId) override {
        return tracks.value(playlistId);
    }
    QVector<decklab::history::PlayedRow> playedRows() override {
        QVector<decklab::history::PlayedRow> rows;
        for (auto it = tracks.cbegin(); it != tracks.cend(); ++it) {
            for (const auto& entry : it.value()) rows.append({it.key(), entry.trackId, entry.location});
        }
        return rows;
    }
    int startNewSet() override {
        ++newSets;
        decklab::history::Playlist list;
        list.id = lists.isEmpty() ? 1 : lists.first().id + 1;
        list.created = QDateTime::currentDateTime();
        lists.prepend(list);
        return list.id;
    }
    QVector<int> setStarts() override {
        return starts;
    }
    bool saveSetStarts(const QVector<int>& value) override {
        starts = value;
        return true;
    }
    TestSource* source = nullptr; // r5-fix3-history: the History source on the sidebar
    decklab::history::Entry add(int playlistId, int deck, double bpm, const QDateTime& when) {
        decklab::history::Entry entry;
        entry.playlistId = playlistId;
        entry.position = static_cast<int>(tracks.value(playlistId).size()) + 1;
        entry.played = when;
        entry.deck = deck;
        entry.trackId = static_cast<int>(bpm);
        entry.title = QStringLiteral("Generated Tone %1").arg(bpm, 0, 'f', 0);
        entry.artist = QStringLiteral("Deck Lab Fixture");
        entry.bpm = bpm;
        entry.key = mixxx::track::io::key::A_MINOR;
        entry.location = QStringLiteral("/generated/history/%1.wav").arg(bpm, 0, 'f', 0);
        tracks[playlistId].append(entry);
        return entry;
    }
    // Deck N became the audible deck: the setlog appends its track.
    decklab::history::Entry play(int deck, double bpm) {
        const auto entry = add(lists.first().id, deck, bpm, QDateTime::currentDateTime());
        decklab::history::Store::instance().addPlayed(lists.first().id, entry);
        return entry;
    }
};
// Last night's history playlist (three tracks, older than any set start),
// then this run's (two, on decks 1 and 2), and a History source on the
// sidebar (Mixxx's setlog feature stand-in).
GeneratedSets* installGeneratedSets(SidebarModel* sidebar, UserSettingsPointer config, QObject* owner) {
    // Generated stand-in, kept for the process (the per-test sidebar model holds it).
    auto* source = new TestSource(config, QStringLiteral("history"), QStringLiteral("History"), false);
    sidebar->addLibraryFeature(source);
    auto sets = std::make_unique<GeneratedSets>();
    sets->source = source;
    decklab::history::Playlist lastNight;
    lastNight.id = 4;
    lastNight.created = QDateTime(QDate::currentDate().addDays(-1), QTime(21, 0));
    decklab::history::Playlist tonight;
    tonight.id = 5;
    tonight.created = QDateTime(QDate::currentDate(), QTime(0, 0));
    sets->lists = {tonight, lastNight};
    for (int n = 0; n < 3; ++n) sets->add(4, n % 2 + 1, 118 + n, lastNight.created.addSecs(n * 400));
    sets->add(5, 1, 122, QDateTime(QDate::currentDate(), QTime(0, 5)));
    sets->add(5, 2, 124, QDateTime(QDate::currentDate(), QTime(0, 10)));
    GeneratedSets* raw = sets.get();
    decklab::history::Store::instance().setBackend(std::move(sets), owner);
    return raw;
}
int historyKind(QListWidget* list, int row) {
    return list->item(row)->data(decklab::history::RowRole).toInt();
}
} // namespace

// r5-history: History is a first-class Browse view: the home's tile counts
// this set; the sets list puts this set and "New set" first and earlier sets
// under their day; the knob and taps skip the date headers; this set opens
// in play order with time, deck and BPM; the card follows the highlight at
// once; a track that plays joins the open list in place (the highlight
// follows the newest only from the latest row), on a hidden page at the next
// visit. Without the set's own table nothing is offered to LOAD: the smoke
// checks the real table (decklabsmoke.cpp, "history-set").
TEST_F(NativeDeckLabTest, HistoryIsAFirstClassBrowseViewInPlayOrder) {
    using decklab::history::RowKind;
    GeneratedSets* sets = installGeneratedSets(model.get(), config(), panel.get());
    panel->selectPage(1); QCoreApplication::processEvents(); settle();
    auto* list = browseList(); ASSERT_NE(nullptr, list);
    // The History source joined the sidebar after the home was built: the
    // home lists it once it is built again (Library, then Back).
    ASSERT_TRUE(selectRow(QStringLiteral("Library")));
    panel->browsePress(); panel->browseBack(); QCoreApplication::processEvents();
    // The home's History tile says what this set holds (read on the first visit).
    ASSERT_TRUE(selectRow(QStringLiteral("History")));
    EXPECT_EQ(QStringLiteral("This set · 2 tracks"), list->currentItem()->data(DeckLabListDelegate::SubtitleRole).toString());
    EXPECT_EQ(QStringLiteral("1 earlier set"), list->currentItem()->data(DeckLabListDelegate::DetailRole).toString());
    // The first run starts a set at this run's history playlist.
    EXPECT_EQ(QVector<int>{5}, sets->starts);
    panel->browsePress(); QCoreApplication::processEvents();
    EXPECT_EQ(QStringList{QStringLiteral("History")}, crumbs());
    ASSERT_EQ(4, list->count());
    EXPECT_EQ(static_cast<int>(RowKind::Session), historyKind(list, 0));
    EXPECT_EQ(QStringLiteral("This set"), list->item(0)->text());
    EXPECT_EQ(static_cast<int>(RowKind::NewSet), historyKind(list, 1));
    EXPECT_TRUE(list->item(1)->flags() & Qt::ItemIsEnabled);
    EXPECT_EQ(static_cast<int>(RowKind::Header), historyKind(list, 2));
    EXPECT_EQ(QStringLiteral("Yesterday"), list->item(2)->text());
    EXPECT_FALSE(list->item(2)->flags() & Qt::ItemIsEnabled);
    EXPECT_EQ(0, list->currentRow());
    auto* card = static_cast<DeckLabTrackCard*>(panel->findChild<QWidget*>("decklab-track-card"));
    ASSERT_NE(nullptr, card);
    EXPECT_EQ(QStringLiteral("This set"), card->title());
    auto* legend = dynamic_cast<DeckLabKnobLegend*>(panel->findChild<QWidget*>("decklab-knob-legend"));
    ASSERT_NE(nullptr, legend);
    EXPECT_EQ(decklab::LegendLevel::List, legend->level());
    capture(QStringLiteral("browse-history-sets.png"));
    // The knob and taps never land on a date header.
    pulseControl("[Library]", "MoveVertical", 1);
    EXPECT_EQ(1, list->currentRow());
    pulseControl("[Library]", "MoveVertical", 1);
    EXPECT_EQ(3, list->currentRow());
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualItemRect(list->item(2)).center());
    QCoreApplication::processEvents();
    EXPECT_EQ(3, list->currentRow());
    EXPECT_FALSE(card->title().isEmpty());
    pulseControl("[Library]", "MoveVertical", -1);
    pulseControl("[Library]", "MoveVertical", -1);
    EXPECT_EQ(0, list->currentRow());
    // This set, in play order, opening on its latest track.
    panel->browsePress(); QCoreApplication::processEvents();
    EXPECT_EQ((QStringList{QStringLiteral("History"), QStringLiteral("This set · 2")}), crumbs());
    ASSERT_EQ(2, list->count());
    EXPECT_EQ(QStringLiteral("Generated Tone 122"), list->item(0)->text());
    EXPECT_EQ(QStringLiteral("Generated Tone 124"), list->item(1)->text());
    EXPECT_EQ(1, list->item(0)->data(decklab::history::DeckRole).toInt());
    EXPECT_EQ(2, list->item(1)->data(decklab::history::DeckRole).toInt());
    EXPECT_EQ(5, list->item(1)->data(decklab::history::PlaylistRole).toInt());
    EXPECT_EQ(124, list->item(1)->data(decklab::history::TrackIdRole).toInt());
    EXPECT_DOUBLE_EQ(124, list->item(1)->data(decklab::history::BpmRole).toDouble());
    EXPECT_EQ(QStringLiteral("00:10"), list->item(1)->data(decklab::history::TimeRole).toString());
    EXPECT_EQ(1, list->currentRow());
    EXPECT_EQ(decklab::LegendLevel::Tracks, legend->level());
    EXPECT_TRUE(panel->findChild<QPushButton*>("decklab-preview")->isVisible());
    // The card follows the highlight at once (the row's facts, no table here).
    EXPECT_EQ(QStringLiteral("Generated Tone 124"), card->title());
    pulseControl("[Library]", "MoveVertical", -1);
    EXPECT_EQ(QStringLiteral("Generated Tone 122"), card->title());
    pulseControl("[Library]", "MoveVertical", 1);
    capture(QStringLiteral("browse-history-set.png"));
    // No set table in this fixture. r5-fix3-history: the highlighted track
    // is offered to LOAD (a tap settles the row first); with no table to
    // settle into, a tap loads nothing (never another track) and says so,
    // never "Pick a track first"; track_level stays 0.
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[DeckLabNative]", "track_level")));
    std::unique_ptr<ControlObject> missing2;
    if (!ControlObject::getControl(ConfigKey("[Channel2]", "LoadSelectedTrack"))) {
        missing2 = std::make_unique<ControlObject>(ConfigKey("[Channel2]", "LoadSelectedTrack"));
    }
    ControlProxy second("[Channel2]", "LoadSelectedTrack");
    int requests = 0;
    ASSERT_TRUE(second.connectValueChanged(panel.get(), [&requests](double v) { if (v > 0) ++requests; }, Qt::DirectConnection));
    auto* load2 = static_cast<DeckLabDockTile*>(panel->findChild<QPushButton*>("decklab-load-2")); ASSERT_NE(nullptr, load2);
    EXPECT_TRUE(load2->loadReady());
    QTest::mouseClick(load2, Qt::LeftButton); QCoreApplication::processEvents();
    EXPECT_EQ(0, requests);
    auto* unavailable = panel->findChild<QLabel*>("decklab-toast"); ASSERT_NE(nullptr, unavailable);
    EXPECT_EQ(QStringLiteral("This track can't be loaded right now"), unavailable->text());
    // Deck 3 becomes audible while the list is open: the set grows in place,
    // the highlight (on the latest) follows, the track is marked.
    QListWidgetItem* before = list->item(0);
    const auto joined = sets->play(3, 126);
    QCoreApplication::processEvents(); settle();
    ASSERT_EQ(3, list->count());
    EXPECT_EQ(before, list->item(0)); // appended, not rebuilt
    EXPECT_EQ(2, list->currentRow());
    EXPECT_EQ(3, list->item(2)->data(decklab::history::DeckRole).toInt());
    EXPECT_EQ(QStringLiteral("This set · 3"), crumbs().value(1));
    EXPECT_EQ(decklab::history::Played::Session, decklab::history::Store::instance().played(joined.location));
    EXPECT_EQ(decklab::history::Played::Earlier,
            decklab::history::Store::instance().played(sets->tracks.value(4).first().location));
    // Not on the latest row: the highlight stays.
    pulseControl("[Library]", "MoveVertical", -1);
    EXPECT_EQ(1, list->currentRow());
    sets->play(1, 127);
    QCoreApplication::processEvents(); settle();
    ASSERT_EQ(4, list->count());
    EXPECT_EQ(1, list->currentRow());
    // A track that plays while Decks is shown joins on the next visit: no
    // work on a hidden page.
    panel->selectPage(0); QCoreApplication::processEvents();
    sets->play(4, 128);
    QCoreApplication::processEvents(); settle();
    EXPECT_EQ(4, list->count());
    panel->selectPage(1); QCoreApplication::processEvents(); settle();
    EXPECT_EQ(5, list->count());
    EXPECT_EQ(1, list->currentRow());
    EXPECT_EQ(QStringLiteral("This set · 5"), crumbs().value(1));
    // Back: the sets, on this set; Back again: the home, its tile in place.
    panel->browseBack(); QCoreApplication::processEvents();
    EXPECT_EQ(QStringList{QStringLiteral("History")}, crumbs());
    EXPECT_EQ(0, list->currentRow());
    panel->browseBack(); QCoreApplication::processEvents();
    EXPECT_EQ(QStringList{QStringLiteral("Sources")}, crumbs());
    ASSERT_NE(nullptr, list->currentItem());
    EXPECT_EQ(QStringLiteral("History"), list->currentItem()->text());
    EXPECT_EQ(QStringLiteral("This set · 5 tracks"), list->currentItem()->data(DeckLabListDelegate::SubtitleRole).toString());
    QListWidgetItem* tile = list->currentItem();
    sets->play(1, 129);
    QCoreApplication::processEvents(); settle();
    EXPECT_EQ(tile, list->currentItem()); // updated in place, not rebuilt
    EXPECT_EQ(QStringLiteral("This set · 6 tracks"), tile->data(DeckLabListDelegate::SubtitleRole).toString());
}

// r5-history: "New set" ends the set from the knob (press, then press again;
// or a held press when the controller reports browse_hold) and by touch (a
// hold, the System kit). The last set stays listed with a quieter mark, the
// new start is kept in the settings, and an empty set refuses another.
TEST_F(NativeDeckLabTest, NewSetHoldStartsAnotherHistorySetAndKeepsTheLastOne) {
    using decklab::history::Played;
    using decklab::history::RowKind;
    GeneratedSets* sets = installGeneratedSets(model.get(), config(), panel.get());
    auto& store = decklab::history::Store::instance();
    panel->selectPage(1); QCoreApplication::processEvents(); settle();
    auto* list = browseList(); ASSERT_NE(nullptr, list);
    ASSERT_TRUE(selectRow(QStringLiteral("Library")));
    panel->browsePress(); panel->browseBack(); QCoreApplication::processEvents();
    ASSERT_TRUE(selectRow(QStringLiteral("History")));
    panel->browsePress(); QCoreApplication::processEvents();
    auto* newSet = panel->findChild<decklab::ui::HoldButton*>("decklab-history-new-set");
    ASSERT_NE(nullptr, newSet);
    ASSERT_TRUE(newSet->isVisible());
    EXPECT_GE(newSet->height(), 44);
    const QString played = sets->tracks.value(5).first().location;
    EXPECT_EQ(Played::Session, store.played(played));
    // The knob: to "New set", press (armed), press again (started).
    pulseControl("[Library]", "MoveVertical", 1);
    ASSERT_EQ(static_cast<int>(RowKind::NewSet), historyKind(list, list->currentRow()));
    panel->browsePress(); QCoreApplication::processEvents();
    EXPECT_TRUE(newSet->armed());
    EXPECT_EQ(0, sets->newSets);
    panel->browsePress(); QCoreApplication::processEvents(); settle();
    EXPECT_EQ(1, sets->newSets);
    EXPECT_EQ((QVector<int>{5, 6}), sets->starts);
    EXPECT_EQ(Played::Earlier, store.played(played));
    ASSERT_GE(list->count(), 5);
    EXPECT_EQ(QStringLiteral("This set"), list->item(0)->text());
    EXPECT_EQ(QStringLiteral("Nothing played yet"), list->item(0)->data(decklab::history::DetailRole).toString());
    EXPECT_FALSE(list->item(1)->flags() & Qt::ItemIsEnabled); // nothing to end yet
    EXPECT_EQ(static_cast<int>(RowKind::Header), historyKind(list, 2));
    EXPECT_EQ(QStringLiteral("Today"), list->item(2)->text());
    EXPECT_EQ(5, list->item(3)->data(decklab::history::SessionRole).toInt());
    EXPECT_EQ(0, list->currentRow());
    // Touch: a tap is not a hold; a 1.2 s hold is.
    sets->play(1, 131);
    QCoreApplication::processEvents(); settle();
    EXPECT_TRUE(list->item(1)->flags() & Qt::ItemIsEnabled);
    QTest::mouseClick(newSet, Qt::LeftButton); QCoreApplication::processEvents();
    EXPECT_EQ(1, sets->newSets);
    QTest::mousePress(newSet, Qt::LeftButton);
    QTest::qWait(decklab::ui::HoldButton::kHoldMs + 250);
    QTest::mouseRelease(newSet, Qt::LeftButton);
    QCoreApplication::processEvents(); settle();
    EXPECT_EQ(2, sets->newSets);
    EXPECT_EQ(QStringLiteral("New set started"), newSet->result());
    EXPECT_EQ(0, list->currentRow()); // on the new set
    capture(QStringLiteral("browse-history-new-set.png"));
    // A controller that reports the held press: hold it on the row.
    sets->play(2, 132);
    QCoreApplication::processEvents(); settle();
    list->setCurrentRow(1); QCoreApplication::processEvents();
    ControlObject::set(ConfigKey("[DeckLabNative]", "browse_hold"), 1); QCoreApplication::processEvents();
    QTest::qWait(decklab::ui::HoldButton::kHoldMs + 250);
    EXPECT_EQ(2, sets->newSets); // r5-fix-history: the release press starts it
    ControlObject::set(ConfigKey("[DeckLabNative]", "browse_hold"), 0); QCoreApplication::processEvents();
    ControlObject::set(ConfigKey("[DeckLabNative]", "browse_press"), 1);
    ControlObject::set(ConfigKey("[DeckLabNative]", "browse_press"), 0); QCoreApplication::processEvents(); settle();
    EXPECT_EQ(3, sets->newSets);
    // An empty set does not start another: the hold is refused and says why.
    auto* toast = panel->findChild<QLabel*>("decklab-toast"); ASSERT_NE(nullptr, toast);
    QTest::mousePress(newSet, Qt::LeftButton); QCoreApplication::processEvents();
    QTest::mouseRelease(newSet, Qt::LeftButton); QCoreApplication::processEvents();
    EXPECT_EQ(3, sets->newSets);
    EXPECT_TRUE(toast->isVisible());
    EXPECT_EQ(QStringLiteral("This set is still empty"), toast->text());
    // Only on the History levels.
    panel->browseBack(); QCoreApplication::processEvents();
    EXPECT_FALSE(newSet->isVisible());
}

// r5-fix3-history: the overlay forwards LOAD, empty-deck PLAY and empty pads
// as they are ([ChannelN] LoadSelectedTrack, LoadSelectedTrackAndPlay,
// [SamplerN] LoadSelectedTrack): LibraryControl's load-time check decides. On
// a highlighted History track still waiting for its table it settles the row
// at the press, on any page (Browse, Decks, Lab FX), then loads exactly that
// row; on the sets list it settles nothing and loads nothing.
TEST_F(NativeDeckLabTest, OverlayLoadsSettleTheHighlightedRowOnEveryPage) {
    // LibraryControl's own load controls, as PlayerManager's counts make them
    // (the fixture's decks are counted already; one sampler here).
    ASSERT_FALSE(ControlObject::exists(ConfigKey("[Sampler1]", "LoadSelectedTrack")));
    ControlObject::set(ConfigKey("[App]", "num_samplers"), 1);
    QCoreApplication::processEvents();
    for (const char* group : {"[Channel1]", "[Channel2]", "[Sampler1]"}) {
        ASSERT_TRUE(ControlObject::exists(ConfigKey(group, "LoadSelectedTrack"))) << group;
    }
    GeneratedSets* sets = installGeneratedSets(model.get(), config(), panel.get());
    // The History source lists this run's playlist: settling a row of it
    // shows up as the source activating that playlist.
    sets->source->model.getRootItem()->appendChild(QStringLiteral("Tonight"), 5);
    panel->selectPage(1); QCoreApplication::processEvents(); settle();
    ASSERT_TRUE(selectRow(QStringLiteral("Library")));
    panel->browsePress(); panel->browseBack(); QCoreApplication::processEvents();
    ASSERT_TRUE(selectRow(QStringLiteral("History")));
    panel->browsePress(); QCoreApplication::processEvents();
    ASSERT_EQ(QStringList{QStringLiteral("History")}, crumbs());
    const auto real = decklab::nativeLoadGuard();
    ASSERT_TRUE(real);
    int asked = 0;
    decklab::nativeLoadGuard() = [&asked, real] {
        ++asked;
        return real();
    };
    const auto press = [](const char* group, const char* item) {
        ControlObject::set(ConfigKey(group, item), 1);
        ControlObject::set(ConfigKey(group, item), 0);
    };
    // The sets list: asked, refused, nothing settled.
    int activations = sets->source->activations;
    press("[Channel1]", "LoadSelectedTrack");
    EXPECT_EQ(1, asked);
    EXPECT_EQ(activations, sets->source->activations);
    // This set, on its latest track (waiting for its table here).
    panel->browsePress(); QCoreApplication::processEvents();
    ASSERT_EQ(2u, static_cast<unsigned>(crumbs().size()));
    auto* load1 = static_cast<DeckLabDockTile*>(panel->findChild<QPushButton*>("decklab-load-1")); ASSERT_NE(nullptr, load1);
    EXPECT_TRUE(load1->loadReady());
    activations = sets->source->activations;
    press("[Channel1]", "LoadSelectedTrack");
    press("[Channel2]", "LoadSelectedTrackAndPlay");
    press("[Sampler1]", "LoadSelectedTrack");
    EXPECT_EQ(4, asked);
    EXPECT_EQ(activations + 3, sets->source->activations); // each press settles the row first
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel2]", "play"))); // no table: nothing loads or plays
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[DeckLabNative]", "track_level")));
    // Off Browse: Decks, then Lab FX. The row is settled there too.
    panel->selectPage(0); QCoreApplication::processEvents();
    activations = sets->source->activations;
    press("[Channel1]", "LoadSelectedTrack");
    panel->selectPage(3); QCoreApplication::processEvents();
    press("[Sampler1]", "LoadSelectedTrack");
    EXPECT_EQ(6, asked);
    EXPECT_EQ(activations + 2, sets->source->activations);
    decklab::nativeLoadGuard() = real;
}

// r5-fix-history: the controllers' real sequences on History's "New set" row.
// The Numark and SB2 overlays send browse_hold 1 on press and 0 on release,
// then a release press unless the knob turned; a held turn arrives eight
// times over; Shift+press sends Back (Numark on release, SB2 at once).
// r5-fix2-history: the Numark latches Shift as the knob goes down (no
// browse_hold then); a sheet taking the knob calls an armed press off; a turn
// LibraryControl sets lands at once, before any later press.
TEST_F(NativeDeckLabTest, NewSetKnobFollowsTheControllersRealSequences) {
    GeneratedSets* sets = installGeneratedSets(model.get(), config(), panel.get());
    panel->selectPage(1); QCoreApplication::processEvents(); settle();
    auto* list = browseList(); ASSERT_NE(nullptr, list);
    ASSERT_TRUE(selectRow(QStringLiteral("Library")));
    panel->browsePress(); panel->browseBack(); QCoreApplication::processEvents();
    const auto openHistory = [this] {
        ASSERT_TRUE(selectRow(QStringLiteral("History")));
        panel->browsePress(); QCoreApplication::processEvents();
        ASSERT_EQ(QStringList{QStringLiteral("History")}, crumbs());
    };
    openHistory();
    auto* newSet = panel->findChild<decklab::ui::HoldButton*>("decklab-history-new-set");
    ASSERT_NE(nullptr, newSet);
    const auto toNewSet = [list] {
        list->setCurrentRow(1); QCoreApplication::processEvents();
        ASSERT_EQ(static_cast<int>(decklab::history::RowKind::NewSet),
                list->item(1)->data(decklab::history::RowRole).toInt());
    };
    const auto hold = [](double v) {
        ControlObject::set(ConfigKey("[DeckLabNative]", "browse_hold"), v);
        QCoreApplication::processEvents(); QCoreApplication::processEvents();
    };
    const auto knob = [](const char* item) {
        ControlObject::set(ConfigKey("[DeckLabNative]", item), 1);
        ControlObject::set(ConfigKey("[DeckLabNative]", item), 0);
        QCoreApplication::processEvents(); settle();
    };
    const int full = decklab::ui::HoldButton::kHoldMs + 250;
    // The sets list is not a track: a controller's LOAD stays blocked, and
    // LibraryControl's load-time check says so too.
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[DeckLabNative]", "track_level")));
    ASSERT_TRUE(decklab::nativeLoadGuard());
    EXPECT_FALSE(decklab::nativeLoadGuard()());
    // A push-turn from "New set": held, turned (x8), let go with no press.
    toNewSet();
    hold(1);
    pulseControl("[Playlist]", "SelectPlaylist", 8);
    hold(0);
    QTest::qWait(full + 100); QCoreApplication::processEvents();
    EXPECT_EQ(0, sets->newSets);
    EXPECT_NE(1, list->currentRow());
    EXPECT_FALSE(newSet->holding());
    // Numark Shift+press held past the hold: Back on release, never New set.
    toNewSet();
    hold(1);
    QTest::qWait(full);
    hold(0);
    knob("browse_back");
    EXPECT_EQ(0, sets->newSets);
    EXPECT_EQ(QStringList{QStringLiteral("Sources")}, crumbs());
    // The Numark with Shift latched at press-down: no browse_hold, Back on
    // release (even when Shift lifted first). After a first press armed New
    // set, that Back calls it off and never completes it.
    openHistory();
    toNewSet();
    hold(1);
    hold(0);
    knob("browse_press");
    EXPECT_TRUE(newSet->armed());
    hold(0);
    knob("browse_back");
    EXPECT_EQ(0, sets->newSets);
    EXPECT_FALSE(newSet->armed());
    EXPECT_EQ(QStringList{QStringLiteral("Sources")}, crumbs());
    // Nor anything on the home: no selection, no controller LOAD.
    EXPECT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[DeckLabNative]", "track_level")));
    EXPECT_FALSE(decklab::nativeLoadGuard()());
    // SB2 Shift+press: Back at once while held.
    openHistory();
    toNewSet();
    hold(1);
    knob("browse_back");
    QTest::qWait(full);
    hold(0);
    EXPECT_EQ(0, sets->newSets);
    // A hold that a menu over History owns never confirms New set behind it.
    openHistory();
    toNewSet();
    QTest::qWait(200);
    panel->findChild<QPushButton*>("decklab-browse-tools")->click(); QCoreApplication::processEvents();
    QPointer<QMenu> menu = panel->findChild<QMenu*>("decklab-browse-tools-menu");
    ASSERT_TRUE(menu); ASSERT_TRUE(menu->isVisible());
    hold(1);
    QTest::qWait(full);
    hold(0);
    menu->close(); QCoreApplication::processEvents(); QCoreApplication::processEvents();
    QTest::qWait(600);
    EXPECT_EQ(0, sets->newSets);
    EXPECT_FALSE(newSet->holding());
    // An armed "press twice" ends when a sheet takes the knob: open and close
    // the status sheet, and the next press arms again, never ends the set.
    toNewSet();
    hold(1);
    hold(0);
    knob("browse_press");
    EXPECT_TRUE(newSet->armed());
    auto* rail = panel->findChild<DeckLabRail*>(); ASSERT_NE(nullptr, rail);
    QPointer<DeckLabStatusSheet> sheet = rail->openSheet(); ASSERT_TRUE(sheet);
    QCoreApplication::processEvents(); QCoreApplication::processEvents();
    EXPECT_FALSE(newSet->armed());
    sheet->hide();
    QCoreApplication::processEvents(); QCoreApplication::processEvents();
    hold(1);
    hold(0);
    knob("browse_press");
    EXPECT_EQ(0, sets->newSets);
    EXPECT_EQ(QStringList{QStringLiteral("History")}, crumbs());
    // A turn LibraryControl sets runs at once (no event loop in between), so a
    // press the controller sent after it acts on the row the turn reached.
    EXPECT_TRUE(newSet->armed());
    ControlObject::set(ConfigKey("[DeckLabNative]", "browse_select"), -1);
    ControlObject::set(ConfigKey("[DeckLabNative]", "browse_select"), 0);
    EXPECT_EQ(0, list->currentRow());
    EXPECT_FALSE(newSet->armed());
    // The Numark's "press twice": (hold 1, hold 0, press) twice.
    toNewSet();
    for (int press = 0; press < 2; ++press) {
        hold(1);
        hold(0);
        knob("browse_press");
    }
    EXPECT_EQ(1, sets->newSets);
    EXPECT_EQ(QStringList{QStringLiteral("History")}, crumbs()); // the new set is not opened
    EXPECT_EQ(0, list->currentRow());
    // A full hold, let go: the release press starts the set and stays on the sets.
    sets->play(1, 140);
    QCoreApplication::processEvents(); settle();
    toNewSet();
    hold(1);
    QTest::qWait(full);
    QCoreApplication::processEvents();
    EXPECT_EQ(1, sets->newSets); // not before letting go
    EXPECT_EQ(QStringLiteral("Let go to start"), newSet->result());
    hold(0);
    knob("browse_press");
    EXPECT_EQ(2, sets->newSets);
    EXPECT_EQ(QStringList{QStringLiteral("History")}, crumbs());
    EXPECT_EQ(0, list->currentRow());
    // A full hold, then a turn before letting go: nothing.
    sets->play(2, 141);
    QCoreApplication::processEvents(); settle();
    toNewSet();
    hold(1);
    QTest::qWait(full);
    pulseControl("[Library]", "MoveVertical", 1);
    hold(0);
    QTest::qWait(600); QCoreApplication::processEvents();
    EXPECT_EQ(2, sets->newSets);
    EXPECT_TRUE(newSet->result().isEmpty());
}

// r4-browse: the pure parts of the round-4 Browse moves.
TEST(DeckLabBrowseTest, HomeArrangesSticksFirstIntoFourTilesAndAnAlsoRow) {
    using decklab::SourceEntry;
    using decklab::SourceState;
    const auto source = [](const QString& key) {
        SourceEntry entry;
        entry.key = key;
        entry.name = key;
        return entry;
    };
    const auto keys = [](const QVector<SourceEntry>& entries) {
        QStringList out;
        for (const auto& entry : entries) out << entry.key;
        return out;
    };
    const SourceEntry more = source(QStringLiteral("more"));
    const QVector<SourceEntry> roots{source(QStringLiteral("root:tracks")), source(QStringLiteral("root:playlist")),
            source(QStringLiteral("root:crates")), source(QStringLiteral("root:recordings")),
            source(QStringLiteral("root:history"))};
    // No stick: a "plug one in" tile leads and cannot be opened.
    auto layout = decklab::arrangeHome({}, roots, more);
    EXPECT_EQ((QStringList{"nostick", "root:tracks", "root:playlist", "root:crates"}), keys(layout.tiles));
    EXPECT_EQ((QStringList{"root:recordings", "root:history", "more"}), keys(layout.also));
    EXPECT_FALSE(layout.tiles[0].enabled);
    EXPECT_EQ(SourceState::Missing, layout.tiles[0].state);
    // Sticks first, in the order found; More stays last.
    layout = decklab::arrangeHome({source(QStringLiteral("usb:/a")), source(QStringLiteral("usb:/b"))}, roots, more);
    EXPECT_EQ((QStringList{"usb:/a", "usb:/b", "root:tracks", "root:playlist"}), keys(layout.tiles));
    EXPECT_EQ((QStringList{"root:crates", "root:recordings", "root:history", "more"}), keys(layout.also));
    // Few sources: all of them are tiles.
    layout = decklab::arrangeHome({source(QStringLiteral("usb:/a"))}, {source(QStringLiteral("root:tracks"))}, more);
    EXPECT_EQ((QStringList{"usb:/a", "root:tracks", "more"}), keys(layout.tiles));
    EXPECT_TRUE(layout.also.isEmpty());
    // State words; nothing for states that say nothing.
    EXPECT_EQ(QString::fromUtf8("Reading…"), decklab::sourceStateText(SourceState::Reading));
    EXPECT_EQ(QStringLiteral("Ready"), decklab::sourceStateText(SourceState::Ready));
    EXPECT_EQ(QStringLiteral("Couldn't read"), decklab::sourceStateText(SourceState::Failed));
    EXPECT_TRUE(decklab::sourceStateText(SourceState::None).isEmpty());
    EXPECT_TRUE(decklab::capacityText(29800000000LL, 12100000000LL).contains(QStringLiteral("GB")));
    EXPECT_TRUE(decklab::capacityText(0, 0).isEmpty());
    // The knob skips disabled rows and stops at both ends.
    const auto enabled = [](int row) { return row != 0 && row != 3; };
    EXPECT_EQ(1, decklab::stepToEnabledRow(6, -1, 1, enabled));
    EXPECT_EQ(2, decklab::stepToEnabledRow(6, 1, 1, enabled));
    EXPECT_EQ(4, decklab::stepToEnabledRow(6, 2, 1, enabled));
    EXPECT_EQ(5, decklab::stepToEnabledRow(6, 4, 9, enabled));
    EXPECT_EQ(1, decklab::stepToEnabledRow(6, 2, -5, enabled));
    // The export date of a generated rekordbox export.
    QTemporaryDir stick;
    EXPECT_TRUE(decklab::exportDateText(stick.path()).isEmpty());
    ASSERT_TRUE(QDir().mkpath(stick.filePath(QStringLiteral("PIONEER/rekordbox"))));
    QFile pdb(stick.filePath(QStringLiteral("PIONEER/rekordbox/export.pdb")));
    ASSERT_TRUE(pdb.open(QIODevice::WriteOnly));
    pdb.close();
    EXPECT_TRUE(decklab::exportDateText(stick.path()).startsWith(QStringLiteral("exported ")));
    // Tiles fill two columns and two rows over the chip row.
    const QSize tile = DeckLabBrowseDelegate::tileSize(QSize(800, 304));
    EXPECT_LE(2 * tile.width() + 4 * DeckLabBrowseDelegate::kSpacing, 800);
    EXPECT_GE(tile.width(), 390);
    EXPECT_LE(2 * tile.height() + DeckLabBrowseDelegate::kChipHeight + 6 * DeckLabBrowseDelegate::kSpacing, 304);
    const QRect retry = DeckLabBrowseDelegate::retryRect(QRect(QPoint(4, 4), tile));
    EXPECT_TRUE(QRect(QPoint(4, 4), tile).contains(retry));
    EXPECT_GE(retry.height(), 44);
}

TEST(DeckLabBrowseMatchTest, MixesRankByKeyThenTempoAndSayTheRelation) {
    using namespace decklab;
    decklab::MatchReference reference;
    reference.deck = 1;
    reference.key = mixxx::track::io::key::A_MINOR; // 8A
    reference.bpm = 122.0;
    reference.direction = TempoDirection::Both;
    QStandardItemModel model(0, 2);
    const QList<QPair<QString, double>> rows{{"7A", 123.0}, {"8A", 125.0}, {"8B", 122.5}, {"8A", 121.0},
            {"3A", 122.0}, {"8A", 140.0}, {"9A", 116.0}, {"8A", 122.0}};
    for (const auto& [key, bpm] : rows) {
        auto* tempo = new QStandardItem;
        tempo->setData(bpm, Qt::EditRole);
        model.appendRow({new QStandardItem(key), tempo});
    }
    const auto candidates = collectMixCandidates(model, 0, 1);
    ASSERT_EQ(8, candidates.size());
    // Compatible key AND inside the window (114.7-129.3): rows 0, 1, 2, 3, 6, 7.
    EXPECT_EQ(6, countMixes(candidates, reference));
    // The reference deck's own track (row 7) never mixes with itself.
    EXPECT_EQ(5, countMixes(candidates, reference, 7));
    QList<int> order;
    for (const auto& mix : rankMixes(candidates, reference, 7)) order << mix.row;
    // Same key first by BPM distance, then relative and +1/-1 by distance.
    EXPECT_EQ((QList<int>{3, 1, 2, 0, 6}), order);
    // The Match bar's direction applies: faster only.
    reference.direction = TempoDirection::Faster;
    EXPECT_EQ(3, countMixes(candidates, reference, 7));
    reference.direction = TempoDirection::Both;
    // The relation in words.
    const QLocale locale;
    EXPECT_EQ(QString::fromUtf8("same key, ▼%1 BPM").arg(locale.toString(1.0, 'f', 0)),
            mixRelationText(keyFromText(QStringLiteral("8A")), 121.0, reference));
    EXPECT_EQ(QString::fromUtf8("+1 key, ▲%1 BPM").arg(locale.toString(2.0, 'f', 0)),
            mixRelationText(keyFromText(QStringLiteral("9A")), 124.0, reference));
    EXPECT_EQ(QString::fromUtf8("relative, ▲%1 BPM").arg(locale.toString(0.5, 'f', 1)),
            mixRelationText(keyFromText(QStringLiteral("8B")), 122.5, reference));
    EXPECT_EQ(QString::fromUtf8("−1 key, same BPM"), mixRelationText(keyFromText(QStringLiteral("7A")), 122.0, reference));
    EXPECT_EQ(QStringLiteral("Mixes with deck 1"), mixesTitle(1));
    // Pitch drift moves the list's reference in quarter-BPM steps.
    MatchReference drift = reference;
    drift.enabled = false;
    drift.bpm = 122.1;
    EXPECT_DOUBLE_EQ(122.0, mixesReference(drift).bpm);
    EXPECT_TRUE(mixesReference(drift).enabled);
}

TEST(DeckLabBrowseTest, KnobLegendVerbsFollowTheLevelAndFastTurnsAccelerate) {
    using decklab::LegendLevel;
    const auto verbs = [](LegendLevel level) {
        QStringList out;
        for (const auto& item : decklab::legendItems(level)) out << item.key + QLatin1Char(':') + item.verb;
        return out;
    };
    EXPECT_EQ((QStringList{"Turn:Choose", "Press:Open", QString::fromUtf8("Shift+Press ×2:Decks")}), verbs(LegendLevel::Home));
    EXPECT_EQ((QStringList{"Turn:Choose", "Press:Open", "Shift+Press:Back"}), verbs(LegendLevel::List));
    EXPECT_EQ((QStringList{"Turn:Choose", "Press:Preview", "Shift+Press:Back"}), verbs(LegendLevel::Tracks));
    EXPECT_EQ(QStringLiteral("Menu:"), verbs(LegendLevel::Menu).value(0));
    EXPECT_EQ(QStringLiteral("Tap a deck to load"), decklab::legendHint(LegendLevel::Tracks));
    DeckLabKnobLegend legend;
    legend.setLevel(LegendLevel::List);
    EXPECT_EQ(24, legend.height());
    EXPECT_TRUE(legend.accessibleName().contains(QStringLiteral("Back")));
    legend.flash();
    EXPECT_TRUE(legend.flashing());
    QTest::qWait(300);
    EXPECT_FALSE(legend.flashing());
    // C-03: slow turns move one row a detent; a sustained fast run moves more.
    decklab::KnobAccelerator knob;
    EXPECT_EQ(1, knob.steps(1, 0));
    EXPECT_EQ(1, knob.steps(1, 100));
    EXPECT_EQ(1, knob.steps(1, 130));
    EXPECT_EQ(4, knob.steps(1, 160));
    EXPECT_EQ(8, knob.steps(1, 170));
    EXPECT_EQ(-1, knob.steps(-1, 180));
    EXPECT_EQ(1, knob.steps(1, 400));
}

TEST(DeckLabBrowseTest, DockTilesSayTheirDeckStateAndWhatALoadReplaces) {
    DeckLabStripState state;
    state.deck = 2;
    state.loaded = true;
    state.cue = true;
    state.title = QStringLiteral("Sweet Talk");
    state.key = QStringLiteral("7A");
    state.bpm = QStringLiteral("128.00");
    state.remaining = QString::fromUtf8("−5:10");
    auto deck = decklab::dockDeckFrom(state);
    EXPECT_EQ(decklab::DockWord::Cued, decklab::dockWord(deck));
    EXPECT_EQ(QLocale().toString(128.0, 'f', 1), deck.bpm);
    EXPECT_EQ(QStringLiteral("replaces Sweet Talk"), decklab::dockLoadLine(deck));
    state.cue = false;
    EXPECT_EQ(decklab::DockWord::Paused, decklab::dockWord(decklab::dockDeckFrom(state)));
    state.playing = true;
    EXPECT_EQ(decklab::DockWord::OnAir, decklab::dockWord(decklab::dockDeckFrom(state)));
    state = DeckLabStripState();
    state.key = QStringLiteral("—");
    deck = decklab::dockDeckFrom(state);
    EXPECT_EQ(decklab::DockWord::Empty, decklab::dockWord(deck));
    EXPECT_TRUE(deck.key.isEmpty());
    EXPECT_EQ(QStringLiteral("empty deck"), decklab::dockLoadLine(deck));
    // Two tiles of ~400 px, four of ~200 px, 52 px tiles in the pane.
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* dock = new DeckLabDeckDock(&host);
    layout->addWidget(dock);
    host.resize(800, 68);
    host.show();
    QCoreApplication::processEvents();
    EXPECT_EQ(68, dock->height());
    EXPECT_TRUE(dock->tile(1)->isVisible());
    EXPECT_FALSE(dock->tile(2)->isVisible());
    EXPECT_GE(dock->tile(0)->width(), 390);
    dock->setShownCount(4);
    QCoreApplication::processEvents();
    EXPECT_TRUE(dock->tile(3)->isVisible());
    EXPECT_GE(dock->tile(3)->width(), 190);
    EXPECT_EQ(64, dock->tile(3)->height());
    dock->setArrangement(DeckLabDeckDock::Arrangement::Pane);
    QCoreApplication::processEvents();
    EXPECT_EQ(52, dock->tile(0)->height());
    dock->setArrangement(DeckLabDeckDock::Arrangement::Bar);
    dock->setShownCount(2);
    QCoreApplication::processEvents();
    // The deck colour bar stays on a disabled tile; a refusal is temporary.
    auto* tile = dock->tile(0);
    tile->setEnabled(false);
    const QImage image = tile->grab().toImage();
    const QColor bar = image.pixelColor(3, image.height() / 2);
    EXPECT_LT(std::abs(bar.hue() - deckLabDeckColor(1).hue()), 12) << bar.name().toStdString();
    tile->showRefusal(100);
    EXPECT_TRUE(tile->refusing());
    QTest::qWait(250);
    EXPECT_FALSE(tile->refusing());
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
    EXPECT_EQ(QStringLiteral("DJ USB"), sticks[0].label);
    EXPECT_TRUE(sticks[0].rekordbox);
    EXPECT_EQ(QStringLiteral("USB stick"), sticks[1].label);
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

// r4-waveforms: on the compact pages nothing floats over the lanes: every
// Sampler and Lab FX surface (the edit sheet included) sits in the page under
// the DECKS row, and the rail's notices stay in the rail.
TEST_F(DeckLabOverlayTest, CompactLanesOnSamplerAndLabFxHaveNothingOverThem) {
    loadGenerated(1, 128); ProcessBuffer(); QTest::qWait(150);
    QWindow* waveform = addWaveformWindow(1);
    const auto area = [this](QWidget* widget) { return QRect(widget->mapTo(panel.get(), QPoint()), widget->size()); };
    auto* zone = panel->findChild<QWidget*>(QStringLiteral("decklab-wave-zone")); ASSERT_NE(nullptr, zone);
    auto* rail = panel->findChild<QWidget*>(QStringLiteral("decklab-rail")); ASSERT_NE(nullptr, rail);
    auto* pages = panel->findChild<QStackedWidget*>(QStringLiteral("decklab-pages")); ASSERT_NE(nullptr, pages);
    for (int page : {2, 3}) {
        panel->selectPage(page); settle();
        ASSERT_TRUE(waveform->isVisible()) << page;
        EXPECT_EQ(0, checkOverlays(waveform)) << page;
        EXPECT_FALSE(area(rail).intersects(area(zone))) << "rail notices live in the rail";
        // Every visible widget of the page sits under the lanes.
        for (auto* child : pages->currentWidget()->findChildren<QWidget*>()) {
            if (child->isVisible()) EXPECT_FALSE(area(child).intersects(area(zone))) << page << " " << child->objectName().toStdString();
        }
    }
    // The edit sheet is part of the page (DeckLabLanesTest docks it under
    // the lanes at 800x270).
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

    // Every small overlay is classified, shown or not. r4-system: system
    // banners are gone; their news goes to the rail, never over the page.
    for (const char* name : {"decklab-banner-audio", "decklab-banner-offer", "decklab-banner-thermal",
                 "decklab-banner-toast"}) {
        EXPECT_EQ(nullptr, panel->findChild<QWidget*>(QString::fromLatin1(name))) << name;
    }
    for (const char* name : {"decklab-toast"}) {
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

    // No sound output holds the rail's status zone (after the start-up
    // grace); the probe's notice and controller offer slide into the rail;
    // a load refusal is the one toast over the waveforms.
    auto* zone = panel->findChild<DeckLabStatusZone*>(QStringLiteral("decklab-status"));
    ASSERT_NE(nullptr, zone);
    EXPECT_TRUE(QTest::qWaitFor([&] { return zone->alert() == decklab::rail::Alert::NoOutput; }, 3000));
    ui->showOverlayProbe();
    ControlObject::set(ConfigKey("[DeckLabNative]", "load_refused"), 2);
    ControlObject::set(ConfigKey("[DeckLabNative]", "load_refused"), 0);
    settle();
    auto* notice = panel->findChild<DeckLabRailNotice*>(QStringLiteral("decklab-rail-notice"));
    auto* rail = panel->findChild<QWidget*>(QStringLiteral("decklab-rail"));
    auto* refusal = panel->findChild<QWidget*>(QStringLiteral("decklab-toast"));
    ASSERT_NE(nullptr, notice); ASSERT_NE(nullptr, rail);
    ASSERT_TRUE(notice->isVisible()); ASSERT_TRUE(refusal->isVisible());
    EXPECT_TRUE(notice->noticeText().startsWith(QStringLiteral("Overlay check")));
    EXPECT_EQ(rail, notice->parentWidget());
    EXPECT_EQ(WId(0), notice->internalWinId());
    EXPECT_FALSE(QRect(notice->mapTo(panel.get(), QPoint()), notice->size())
                    .intersects(QRect(detail(1)->mapTo(panel.get(), QPoint()), detail(1)->size())));
    EXPECT_FALSE(ui->offeredController().isEmpty());
    EXPECT_FALSE(panel->waveformsSuppressed());
    EXPECT_TRUE(waveform->isVisible());
    EXPECT_GE(checkOverlays(waveform), 1);
    capture(QStringLiteral("overlay-banners-over-decks.png"));

    // A waveform window created later (another lane shown for the first
    // time) is stacked above the toast, so it is raised again.
    const int raised = decklab::overlay::raiseCount(refusal);
    QWindow* laterWaveform = addWaveformWindow(2);
    ASSERT_EQ(waveform->parent(), laterWaveform->parent());
    EXPECT_GT(decklab::overlay::raiseCount(refusal), raised);

    // The controller offer is the next notice; the whole notice is the button.
    notice->dismiss();
    settle();
    ASSERT_TRUE(notice->isVisible());
    EXPECT_TRUE(notice->noticeText().contains(QStringLiteral("Overlay check")));
    QTest::mouseClick(notice, Qt::LeftButton);
    settle();
    QPointer<DeckLabSetupWizard> wizard = ui->wizard();
    ASSERT_FALSE(wizard.isNull());
    EXPECT_TRUE(wizard->isVisible());
    EXPECT_TRUE(panel->waveformsSuppressed());
    EXPECT_FALSE(waveform->isVisible()); EXPECT_FALSE(laterWaveform->isVisible());
    EXPECT_GE(checkOverlays(waveform), 1);
    capture(QStringLiteral("overlay-deck-setup-over-decks.png"));
    // A confirmation over Deck Setup.
    auto* confirm = decklab::ui::Confirm::ask(panel.get(), QStringLiteral("Generated question?"), QString(),
            QStringLiteral("Yes"), [] {});
    settle();
    EXPECT_GE(checkOverlays(waveform), 2);
    confirm->close();
    wizard->findChild<QPushButton*>(QStringLiteral("decklab-setup-close"))->click();
    settle();
    EXPECT_FALSE(panel->waveformsSuppressed());
    EXPECT_TRUE(waveform->isVisible()); EXPECT_TRUE(laterWaveform->isVisible());
}

// r5-polish: an unlabeled rekordbox stick is "DJ USB" in the source list and
// the breadcrumb, never its UUID. RekordboxFeature's own device scan and
// merge fill the tree Browse reads; a device is its mount path, so a swapped
// stick with the same name gets its own row, and a stick already listed is
// never renamed or dropped when another one arrives.
TEST_F(NativeDeckLabTest, RekordboxSticksKeepTheirRowAndNameWhenSticksAreSwapped) {
    const auto rows = [this] {
        QList<QPair<QString, QString>> result; // (label, path)
        const QModelIndex root;
        for (int row = 0; row < rekordbox->model.rowCount(root); ++row) {
            const QModelIndex index = rekordbox->model.index(row, 0, root);
            result.append({index.data().toString(),
                    index.data(TreeItemModel::kDataRole).toStringList().value(0)});
        }
        return result;
    };
    const auto scan = [this] {
        const QList<TreeItem*> found = RekordboxFeature::findDevices();
        std::vector<std::unique_ptr<TreeItem>> devices(found.cbegin(), found.cend());
        QStringList removed;
        RekordboxFeature::mergeDevices(&rekordbox->model, std::move(devices), [&removed](TreeItem* item) {
            removed.append(item->getData().toList().value(0).toString());
        });
        return removed;
    };
    const auto openTile = [this](const QString& name) {
        panel->selectPage(1);
        ControlObject::set(ConfigKey("[DeckLabNative]", "browse_sources"), 1);
        ControlObject::set(ConfigKey("[DeckLabNative]", "browse_sources"), 0);
        QCoreApplication::processEvents();
        if (!selectRow(name)) return false;
        panel->browsePress(); QCoreApplication::processEvents();
        return true;
    };
    // One unlabeled stick: "DJ USB" everywhere.
    const QString first = makeStick(QStringLiteral("5D2C-81F0"), true);
    EXPECT_TRUE(scan().isEmpty());
    ASSERT_EQ(1, rows().size());
    EXPECT_EQ(QStringLiteral("DJ USB"), rows().first().first);
    EXPECT_EQ(QDir::cleanPath(first), QDir::cleanPath(rows().first().second));
    ASSERT_TRUE(openTile(QStringLiteral("DJ USB"))) << listTexts().join(" / ").toStdString();
    EXPECT_EQ(QStringList{QStringLiteral("DJ USB")}, crumbs());
    EXPECT_EQ(1, rekordbox->activations); // its export is read
    capture(QStringLiteral("browse-usb-unlabeled-breadcrumb.png"));
    panel->browseBack(); QCoreApplication::processEvents();
    // Swapped for another unlabeled stick: the row follows the new stick.
    ASSERT_TRUE(QDir(first).removeRecursively());
    const QString second = makeStick(QStringLiteral("9E41-07B3"), true);
    EXPECT_EQ(QStringList{QDir::cleanPath(first)}, scan());
    ASSERT_EQ(1, rows().size());
    EXPECT_EQ(QStringLiteral("DJ USB"), rows().first().first);
    EXPECT_EQ(QDir::cleanPath(second), QDir::cleanPath(rows().first().second));
    ASSERT_TRUE(openTile(QStringLiteral("DJ USB"))) << listTexts().join(" / ").toStdString();
    EXPECT_EQ(QStringList{QStringLiteral("DJ USB")}, crumbs());
    EXPECT_EQ(2, rekordbox->activations); // the new stick is read
    panel->browseBack(); QCoreApplication::processEvents();
    // The first comes back while the second is in: the second keeps its row
    // and name, the newcomer is "DJ USB 2" (tile, row and breadcrumb).
    makeStick(QStringLiteral("5D2C-81F0"), true);
    EXPECT_TRUE(scan().isEmpty());
    ASSERT_EQ(2, rows().size());
    QHash<QString, QString> byPath;
    for (const auto& row : rows()) byPath.insert(QDir::cleanPath(row.second), row.first);
    EXPECT_EQ(QStringLiteral("DJ USB"), byPath.value(QDir::cleanPath(second)));
    EXPECT_EQ(QStringLiteral("DJ USB 2"), byPath.value(QDir::cleanPath(first)));
    ASSERT_TRUE(openTile(QStringLiteral("DJ USB 2"))) << listTexts().join(" / ").toStdString();
    EXPECT_EQ(QStringList{QStringLiteral("DJ USB 2")}, crumbs());
    for (const QString& segment : crumbs()) EXPECT_FALSE(segment.contains(QStringLiteral("5D2C"))) << segment.toStdString();
}

// r5-polish: after "Set up later", Settings and the status sheet offer
// Finish setup until setup is done.
TEST_F(NativeDeckLabTest, SettingsAndStatusSheetOfferFinishSetupAfterSetUpLater) {
    config()->setValue(ConfigKey("[DeckLab]", "first_run_deferred"), 1);
    DeckLabSystem::Services services;
    services.config = config();
    DeckLabSystem system(services, nullptr, QStringLiteral("/nonexistent/decklabd.sock"));
    panel->attachSystem(&system);
    auto* ui = panel->findChild<DeckLabSystemUi*>(); ASSERT_NE(nullptr, ui);
    EXPECT_TRUE(ui->setupPending());
    panel->selectPage(4); QCoreApplication::processEvents();
    auto* settings = panel->findChild<DeckLabSettingsPage*>(); ASSERT_NE(nullptr, settings);
    settings->showCategory(DeckLabSettingsPage::ControllerAudio); QTest::qWait(30);
    auto* finish = panel->findChild<QPushButton*>(QStringLiteral("decklab-finish-setup")); ASSERT_NE(nullptr, finish);
    EXPECT_TRUE(finish->isVisible());
    capture(QStringLiteral("settings-finish-setup.png"));
    // Not opened by itself at start.
    QTest::qWait(1700);
    EXPECT_EQ(nullptr, ui->wizard());
    // The status sheet's first line.
    auto* rail = panel->findChild<DeckLabRail*>(); ASSERT_NE(nullptr, rail);
    panel->selectPage(0); QCoreApplication::processEvents();
    auto* sheet = rail->openSheet(); ASSERT_NE(nullptr, sheet); QTest::qWait(30);
    ASSERT_FALSE(sheet->lines().isEmpty());
    EXPECT_TRUE(sheet->lines().first().startsWith(QStringLiteral("Setup: "))) << sheet->lines().first().toStdString();
    capture(QStringLiteral("status-sheet-finish-setup.png"));
    auto* sheetFinish = sheet->findChild<QPushButton*>(QStringLiteral("decklab-status-finish-setup")); ASSERT_NE(nullptr, sheetFinish);
    sheetFinish->click(); QCoreApplication::processEvents();
    QPointer<DeckLabSetupWizard> wizard = ui->wizard(); ASSERT_FALSE(wizard.isNull());
    EXPECT_TRUE(wizard->isFirstRun());
    wizard->skipToDone();
    wizard->findChild<QPushButton*>(QStringLiteral("decklab-setup-next"))->click(); QCoreApplication::processEvents();
    EXPECT_TRUE(system.firstRunDone());
    EXPECT_FALSE(ui->setupPending());
    EXPECT_FALSE(finish->isVisibleTo(settings));
}

// r5-polish: a MIDI + vendor HID controller is one Deck Setup row with the
// MIDI badge (fake enumeration and decklabd report; generated USB location).
TEST_F(NativeDeckLabTest, DeckSetupListsAMidiAndHidControllerOnce) {
    config()->setValue(ConfigKey("[DeckLab]", "first_run_done"), 1);
    QTemporaryDir socketDir;
    DeckLabFakeService fake;
    fake.devices = QJsonObject{{"controllers", QJsonArray{QJsonObject{{"key", "15e4:0035"}, {"vid", "15e4"}, {"pid", "0035"},
            {"name", "Numark MixTrack Pro 3"}, {"midi_ports", QJsonArray{"MixTrack Pro 3 MIDI 1"}}, {"hid", true},
            {"usb_port", "1-1.2"}}}}, {"audio_cards", QJsonArray{}}};
    ASSERT_TRUE(fake.listen(socketDir.filePath(QStringLiteral("decklabd.sock"))));
    DeckLabSystem::Services services;
    services.config = config();
    DeckLabSystem system(services, nullptr, socketDir.filePath(QStringLiteral("decklabd.sock")));
    const QString location = QStringLiteral("usb-0000:01:00.0-1.2");
    decklab::ControllerEndpoint midi;
    midi.name = QStringLiteral("MixTrack Pro 3 MIDI 1");
    midi.open = midi.enabled = true;
    midi.mapping = QDir(config()->getResourcePath()).filePath(QStringLiteral("controllers/Numark-Mixtrack-3.midi.xml"));
    midi.usbPath = location;
    decklab::ControllerEndpoint hid;
    hid.name = QStringLiteral("MixTrack Pro 3 _3");
    hid.hid = true;
    hid.product = QStringLiteral("MixTrack Pro 3");
    hid.usagePage = 0xFFA0;
    hid.interfaceNumber = 3;
    hid.vendorId = 0x15e4;
    hid.productId = 0x0035;
    hid.usbPath = location;
    QStringList opened;
    DeckLabSystem::ControllerHooks hooks;
    hooks.endpoints = [midi, hid] { return QList<decklab::ControllerEndpoint>{midi, hid}; };
    hooks.open = [&opened](const QString& name, const QString&, QString*) { opened.append(name); return true; };
    hooks.disable = [&opened](const decklab::ControllerEndpoint& endpoint) { opened.append(QStringLiteral("disabled ") + endpoint.name); };
    system.setControllerHooksForTesting(hooks);
    system.start();
    ASSERT_TRUE(QTest::qWaitFor([&] { return system.isReady() && !system.devices().isEmpty(); }, 3000));
    panel->attachSystem(&system);
    auto* ui = panel->findChild<DeckLabSystemUi*>(); ASSERT_NE(nullptr, ui);
    QPointer<DeckLabSetupWizard> wizard = ui->openWizard(0); ASSERT_FALSE(wizard.isNull());
    QTest::qWait(50);
    QList<decklab::ui::Row*> rows;
    for (auto* row : wizard->findChildren<decklab::ui::Row*>(QStringLiteral("decklab-setup-controller")))
        if (row->isVisibleTo(wizard)) rows.append(row);
    ASSERT_EQ(1, rows.size());
    EXPECT_EQ(QStringLiteral("Numark MixTrack Pro 3"), rows.first()->titleText());
    EXPECT_EQ(QStringList{QStringLiteral("MIDI")}, rows.first()->tags());
    capture(QStringLiteral("setup-controller-midi-hid.png"));
    // The status sheet and Settings name it once, working.
    auto* rail = panel->findChild<DeckLabRail*>(); ASSERT_NE(nullptr, rail);
    EXPECT_TRUE(QTest::qWaitFor([rail] { return rail->inputs().controller == decklab::rail::Inputs::Controller::Working; }, 2000));
    EXPECT_EQ(QStringLiteral("Numark MixTrack Pro 3"), rail->inputs().controllerName);
    auto* state = static_cast<decklab::ui::ActionRow*>(panel->findChild<QPushButton*>(QStringLiteral("decklab-setup-controller-state")));
    ASSERT_NE(nullptr, state);
    EXPECT_FALSE(state->value().contains(QStringLiteral("_3"))) << state->value().toStdString();
    EXPECT_FALSE(state->value().contains(QStringLiteral(","))) << state->value().toStdString();
    EXPECT_TRUE(opened.isEmpty()) << opened.join(QStringLiteral(", ")).toStdString();
    wizard->hide();
}

// r6-playstart: Deck Lab's PLAY starts exactly where the deck is (CDJ) unless
// it syncs; Mixxx's "start on the other deck's beat" is a Settings choice.
TEST_F(NativeDeckLabTest, NativeStartMakesPlayExactOnceAndKeepsTheChoice) {
    const ConfigKey saved = decklab::playStartAlignConfigKey();
    const ConfigKey live = decklab::playStartAlignControlKey();
    // The fixture's fresh profile had no choice: the surface wrote the CDJ start.
    EXPECT_EQ(1, config()->getValue(saved, -1));
    EXPECT_DOUBLE_EQ(1, ControlObject::get(live));
    // A rebuild (or the next start) keeps a saved choice, either one.
    config()->setValue(saved, 0);
    panel.reset();
    EXPECT_DOUBLE_EQ(1, ControlObject::get(live)) << "leaving the surface changes nothing";
    panel = std::make_unique<WDeckLab>(config(), nullptr, players.get(), m_pEffectsManager.get());
    QCoreApplication::processEvents();
    EXPECT_EQ(0, config()->getValue(saved, -1));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(live));
    EXPECT_EQ(decklab::PlayStartAlign::Quantize, decklab::nativePlayStartAlign(config()));
}

TEST_F(NativeDeckLabTest, SettingsPlayStartsRowSwitchesLiveByTouchAndKnob) {
    const ConfigKey saved = decklab::playStartAlignConfigKey();
    const ConfigKey live = decklab::playStartAlignControlKey();
    panel->selectPage(4); QTest::qWait(50);
    auto* settings = panel->findChild<DeckLabSettingsPage*>(); ASSERT_NE(nullptr, settings);
    panel->findChild<QPushButton*>("decklab-settings-category-1")->click(); QTest::qWait(50);
    auto* row = panel->findChild<decklab::ui::ChoiceRow*>("decklab-settings-play-start"); ASSERT_NE(nullptr, row);
    ASSERT_TRUE(row->isVisible());
    EXPECT_EQ(QStringLiteral("PLAY starts"), row->label());
    ASSERT_EQ(2, row->model()->count());
    EXPECT_EQ(QStringLiteral("Exactly where it is (CDJ)"), row->model()->itemText(0));
    EXPECT_EQ(1, row->model()->itemData(0).toInt());
    EXPECT_EQ(QStringLiteral("On the other deck's beat (Mixxx)"), row->model()->itemText(1));
    EXPECT_EQ(0, row->model()->itemData(1).toInt());
    EXPECT_EQ(0, row->currentIndex()) << "the native default: exactly where it is";
    // The reason line tells what still lines decks up: SYNC, touch or
    // controller, which follows the master's beat whatever Quantize is set to.
    EXPECT_TRUE(row->hint().startsWith(QStringLiteral("SYNC on before PLAY")));
    EXPECT_TRUE(row->hint().contains(QStringLiteral("Quantize or not")));
    capture(QStringLiteral("settings-decks-play-start.png"));

    // A tap on the second segment: saved and live, no restart.
    const QRect area = row->rect();
    QTest::mouseClick(row, Qt::LeftButton, Qt::NoModifier, QPoint(area.right() - 30, area.center().y()));
    EXPECT_EQ(1, row->currentIndex());
    EXPECT_EQ(0, config()->getValue(saved, -1));
    EXPECT_DOUBLE_EQ(0, ControlObject::get(live));

    // The knob: press edits, turn changes, press sets.
    settings->knob()->focus(row);
    r4Pulse("[Library]", "GoToItem");
    EXPECT_TRUE(settings->knob()->editing());
    r4Pulse("[Playlist]", "SelectTrackKnob", -1);
    r4Pulse("[Library]", "GoToItem");
    EXPECT_FALSE(settings->knob()->editing());
    EXPECT_EQ(0, row->currentIndex());
    EXPECT_EQ(1, config()->getValue(saved, -1));
    EXPECT_DOUBLE_EQ(1, ControlObject::get(live));

    // Another writer of the control (a mapping, a script) moves the row too.
    ControlObject::set(live, 0); settle();
    EXPECT_EQ(1, row->currentIndex());
    ControlObject::set(live, 1); settle();
    EXPECT_EQ(0, row->currentIndex());

    // Fits the 600 px of Settings at 800x480: two segments, and the label and
    // reason line in full beside them (row anatomy of decklabutility.cpp).
    EXPECT_TRUE(row->segmented());
    EXPECT_EQ(64, row->height());
    EXPECT_LE(row->mapTo(panel.get(), QPoint(row->width(), 0)).x(), 800);
    const QFontMetrics caps(decklab::ui::capsFont(row->font(), 14));
    int segments = 0;
    for (int index = 0; index < row->model()->count(); ++index) {
        segments += std::max(64, caps.horizontalAdvance(row->segmentText(index)) + 28) + (index ? 4 : 0);
    }
    const int text = row->width() - 16 - segments - 16 - 12;
    EXPECT_LE(QFontMetrics(decklab::ui::uiFont(row->font(), 16, QFont::Medium)).horizontalAdvance(row->label()), text);
    EXPECT_LE(QFontMetrics(decklab::ui::uiFont(row->font(), 15, QFont::Medium)).horizontalAdvance(row->hint()), text)
            << text;
}

TEST_F(NativeDeckLabTest, TouchPlayStartsExactlyWhileAnotherDeckPlaysAndSyncTapLinesItUp) {
    // Quantize stays on (Mixxx's default): sync lock and quantized hot cues,
    // loops and beat jumps need it.
    m_pMixerDeck1->loadFakeTrack(false, 128); m_pMixerDeck2->loadFakeTrack(false, 130); ProcessBuffer();
    ASSERT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel1]", "quantize")));
    ASSERT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel2]", "quantize")));
    ControlObject::set(ConfigKey("[Channel2]", "playposition"), .21);
    ControlObject::set(ConfigKey("[Channel1]", "play"), 1); ProcessBuffer(); ProcessBuffer(); settle();
    const double before = ControlObject::get(ConfigKey("[Channel2]", "beat_distance"));
    auto* play = panel->findChild<QPushButton*>("decklab-2-play"); ASSERT_NE(nullptr, play);
    play->click(); settle();
    ASSERT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel2]", "play")));
    ProcessBuffer();
    // One buffer (512 frames) of a 130 BPM beat later: never seeked.
    EXPECT_NEAR(before + 512 / (44100 * 60.0 / 130), ControlObject::get(ConfigKey("[Channel2]", "beat_distance")), 1e-9);
    EXPECT_GT(std::abs(ControlObject::get(ConfigKey("[Channel1]", "beat_distance")) -
            ControlObject::get(ConfigKey("[Channel2]", "beat_distance"))), .01);

    // Cue it, SYNC, PLAY: the tap on the paused deck locks it to the master
    // (r6-sync: a CDJ BEAT SYNC stays on) and PLAY starts in phase.
    play->click(); settle(); ProcessBuffer();
    ASSERT_DOUBLE_EQ(0, ControlObject::get(ConfigKey("[Channel2]", "play")));
    ControlObject::set(ConfigKey("[Channel2]", "playposition"), .21); ProcessBuffer(); settle();
    panel->findChild<QPushButton*>("decklab-2-beatsync")->click(); settle(); ProcessBuffer();
    EXPECT_NEAR(128, ControlObject::get(ConfigKey("[Channel2]", "bpm")), 1e-6);
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel2]", "sync_enabled")));
    play->click(); settle(); ProcessBuffer();
    EXPECT_DOUBLE_EQ(1, ControlObject::get(ConfigKey("[Channel2]", "play")));
    EXPECT_NEAR(ControlObject::get(ConfigKey("[Channel1]", "beat_distance")),
            ControlObject::get(ConfigKey("[Channel2]", "beat_distance")), 1e-6);
}

TEST_F(NativeDeckLabTest, TouchSyncTapLinesUpPlayAfterACueHoldAndWithoutQuantize) {
    // r6-playstart: SYNC on the paused deck, then CUE held to listen and let
    // go. The preview starts in phase, and PLAY after it still does: a
    // preview back to pause leaves the tap for the next start.
    m_pMixerDeck1->loadFakeTrack(false, 128); m_pMixerDeck2->loadFakeTrack(false, 130); ProcessBuffer();
    const auto get = [](const char* group, const char* item) { return ControlObject::get(ConfigKey(group, item)); };
    ASSERT_DOUBLE_EQ(1, get("[Channel2]", "quantize"));
    ControlObject::set(ConfigKey("[Channel2]", "playposition"), .21);
    ControlObject::set(ConfigKey("[Channel1]", "play"), 1); ProcessBuffer(); ProcessBuffer(); settle();
    auto* cue = panel->findChild<QPushButton*>("decklab-2-cue_default"); ASSERT_NE(nullptr, cue);
    auto* play = panel->findChild<QPushButton*>("decklab-2-play"); ASSERT_NE(nullptr, play);
    auto* sync = panel->findChild<QPushButton*>("decklab-2-beatsync"); ASSERT_NE(nullptr, sync);
    // CUE on the paused deck sets the cue where it waits.
    QTest::mousePress(cue, Qt::LeftButton); QTest::mouseRelease(cue, Qt::LeftButton); settle(); ProcessBuffer();
    ASSERT_DOUBLE_EQ(0, get("[Channel2]", "play"));
    ASSERT_NE(-1, get("[Channel2]", "cue_point"));
    sync->click(); settle(); ProcessBuffer();
    ASSERT_NEAR(128, get("[Channel2]", "bpm"), 1e-6);
    QTest::mousePress(cue, Qt::LeftButton); settle(); ProcessBuffer();
    ASSERT_DOUBLE_EQ(1, get("[Channel2]", "play"));
    EXPECT_NEAR(get("[Channel1]", "beat_distance"), get("[Channel2]", "beat_distance"), 1e-6);
    QTest::mouseRelease(cue, Qt::LeftButton); settle(); ProcessBuffer();
    ASSERT_DOUBLE_EQ(0, get("[Channel2]", "play"));
    play->click(); settle(); ProcessBuffer();
    ASSERT_DOUBLE_EQ(1, get("[Channel2]", "play"));
    EXPECT_NEAR(get("[Channel1]", "beat_distance"), get("[Channel2]", "beat_distance"), 1e-6);

    // r6-sync: SYNC is a CDJ lock, so deck 2 is still synced. Like a CDJ's
    // BEAT SYNC it follows the master's beat with Quantize off too: PLAY
    // starts in phase again (Mixxx's own sync lock needs Quantize for that).
    EXPECT_DOUBLE_EQ(1, get("[Channel2]", "sync_enabled"));
    play->click(); settle(); ProcessBuffer();
    ASSERT_DOUBLE_EQ(0, get("[Channel2]", "play"));
    ControlObject::set(ConfigKey("[Channel2]", "quantize"), 0);
    ControlObject::set(ConfigKey("[Channel2]", "playposition"), .21); ProcessBuffer(); settle();
    play->click(); settle(); ProcessBuffer();
    ASSERT_DOUBLE_EQ(1, get("[Channel2]", "play"));
    EXPECT_NEAR(get("[Channel1]", "beat_distance"), get("[Channel2]", "beat_distance"), 1e-6);

    // A tap turns SYNC off and keeps the tempo; PLAY then starts exactly
    // where the deck waits.
    play->click(); settle(); ProcessBuffer();
    ASSERT_DOUBLE_EQ(0, get("[Channel2]", "play"));
    sync->click(); settle(); ProcessBuffer();
    EXPECT_DOUBLE_EQ(0, get("[Channel2]", "sync_enabled"));
    EXPECT_NEAR(128, get("[Channel2]", "bpm"), 1e-6);
    ControlObject::set(ConfigKey("[Channel2]", "playposition"), .21); ProcessBuffer(); settle();
    const double before = get("[Channel2]", "beat_distance");
    play->click(); settle(); ProcessBuffer();
    ASSERT_DOUBLE_EQ(1, get("[Channel2]", "play"));
    EXPECT_NEAR(before + 512 / (44100 * 60.0 / 128), get("[Channel2]", "beat_distance"), 1e-6);

    // The next tap turns SYNC on, still with Quantize off: PLAY starts in
    // phase. SYNC never touches the deck's Quantize, which still governs hot
    // cues, loops and beat jumps.
    play->click(); settle(); ProcessBuffer();
    ASSERT_DOUBLE_EQ(0, get("[Channel2]", "play"));
    ControlObject::set(ConfigKey("[Channel2]", "playposition"), .21); ProcessBuffer(); settle();
    sync->click(); settle(); ProcessBuffer();
    EXPECT_DOUBLE_EQ(1, get("[Channel2]", "sync_enabled"));
    EXPECT_DOUBLE_EQ(0, get("[Channel2]", "quantize"));
    play->click(); settle(); ProcessBuffer();
    ASSERT_DOUBLE_EQ(1, get("[Channel2]", "play"));
    EXPECT_NEAR(get("[Channel1]", "beat_distance"), get("[Channel2]", "beat_distance"), 1e-6);
    play->click(); settle(); ProcessBuffer();
    sync->click(); settle(); ProcessBuffer();
    ControlObject::set(ConfigKey("[Channel1]", "play"), 0); ProcessBuffer();
}

// r6-keys: the Key format setting (Settings › Library) reaches every key on
// screen at once, whatever the key's source: Browse rows whose keys are only
// stored text (a rekordbox export's "Am", a USB file's "F#m", "Ebm" or Open
// Key "1m"), a library key id, the Match button, the decks. Rows repaint;
// text that is not a key stays as stored (cut, never clipped). Mixxx's own
// tables and [Key] KeyNotation follow with Mixxx's values.
TEST_F(NativeDeckLabTest, KeyFormatSettingReachesEveryKeyLive) {
    using namespace mixxx::track::io::key;
    loadGenerated(1, 122, A_MINOR);
    loadGenerated(2, 124, C_MAJOR); ProcessBuffer(); QTest::qWait(250);
    GeneratedSets* sets = installGeneratedSets(model.get(), config(), panel.get());
    const std::array<std::pair<const char*, ChromaticKey>, 6> stored{{{"Am", INVALID}, {"F#m", INVALID},
            {"1m", INVALID}, {"Ebm", INVALID}, {"Unknown", INVALID}, {"", C_MAJOR}}};
    for (int n = 0; n < 6; ++n) {
        sets->add(5, 1 + n % 2, 126 + n, QDateTime(QDate::currentDate(), QTime(0, 15 + n)));
        sets->tracks[5].last().key = stored[n].second;
        sets->tracks[5].last().keyText = QString::fromLatin1(stored[n].first);
    }
    panel->selectPage(1); QCoreApplication::processEvents(); settle();
    auto* list = browseList(); ASSERT_NE(nullptr, list);
    ASSERT_TRUE(selectRow(QStringLiteral("Library")));
    panel->browsePress(); panel->browseBack(); QCoreApplication::processEvents();
    ASSERT_TRUE(selectRow(QStringLiteral("History")));
    panel->browsePress(); QCoreApplication::processEvents();
    panel->browsePress(); QCoreApplication::processEvents(); settle();
    ASSERT_EQ(8, list->count());
    // What each of the six rows paints in its key slot, through the list's
    // own delegate: the text that is not the time, deck, title, artist or BPM.
    const auto keysShown = [list] {
        QStringList keys;
        for (int n = 0; n < 6; ++n) {
            const QString title = QStringLiteral("Generated Tone %1").arg(126 + n);
            QListWidgetItem* item = nullptr;
            for (int row = 0; row < list->count(); ++row) {
                if (list->item(row)->text() == title) item = list->item(row);
            }
            if (!item) {
                keys << QStringLiteral("(missing)");
                continue;
            }
            QStyleOptionViewItem option;
            option.initFrom(list);
            option.widget = list;
            option.rect = list->visualItemRect(item);
            option.font = list->font();
            option.state = QStyle::State_Enabled;
            const QModelIndex index = list->model()->index(list->row(item), 0);
            QString key = QStringLiteral("(none)");
            for (const QString& text : DeckLabPaintedText::of(*list->itemDelegate(), option, index)) {
                if (!text.contains(QLatin1Char(':')) && !text.startsWith(QLatin1Char('#')) &&
                        text != QStringLiteral("1") && text != QStringLiteral("2") &&
                        !text.startsWith(QStringLiteral("Generated")) && !text.startsWith(QStringLiteral("Deck Lab")) &&
                        !text.contains(QLatin1Char('.')) && !text.contains(QLatin1Char(','))) {
                    key = text;
                }
            }
            keys << key;
        }
        return keys;
    };
    auto* match = static_cast<DeckLabActionButton*>(panel->findChild<QPushButton*>("decklab-browse-match"));
    ASSERT_NE(nullptr, match);
    // The default, Lancelot: the rekordbox and tag texts too.
    const QString unknownCut = QFontMetrics(decklab::numberFont(list->font(), 16))
                                       .elidedText(QStringLiteral("Unknown"), Qt::ElideRight, 42);
    EXPECT_EQ((QStringList{"8A", "11A", "8A", "2A", unknownCut, "8B"}), keysShown());
    EXPECT_NE(QStringLiteral("known"), unknownCut);
    EXPECT_EQ(QStringLiteral("8A"), strip(1)->state().key);
    EXPECT_EQ(QStringLiteral("8B"), strip(2)->state().key);
    EXPECT_TRUE(match->detail().contains(QStringLiteral("8A"))) << match->detail().toStdString();
    capture(QStringLiteral("r6-keys-browse-lancelot.png"));
    // Musical, chosen on the Settings row: the shown list repaints at once.
    QComboBox* format = panel->findChild<QComboBox*>(QStringLiteral("decklab-key-format"));
    ASSERT_NE(nullptr, format);
    EXPECT_EQ(QStringLiteral("Lancelot (8A)"), format->itemText(0));
    EXPECT_EQ(QStringLiteral("Open Key (1m)"), format->itemText(1));
    EXPECT_EQ(QStringLiteral("Musical (Am)"), format->itemText(2));
    struct PaintCounter : QObject {
        int paints = 0;
        bool eventFilter(QObject*, QEvent* event) override {
            if (event->type() == QEvent::Paint) ++paints;
            return false;
        }
    } counter;
    list->viewport()->installEventFilter(&counter);
    QMetaObject::invokeMethod(format, "activated", Q_ARG(int, 2)); settle(); QTest::qWait(30);
    EXPECT_GT(counter.paints, 0) << "the shown rows repaint in the new format";
    list->viewport()->removeEventFilter(&counter);
    EXPECT_EQ((QStringList{"Am", QString::fromUtf8("F♯m"), "Am", QString::fromUtf8("E♭m"), unknownCut, "C"}), keysShown());
    EXPECT_EQ(QStringLiteral("Am"), strip(1)->state().key);
    EXPECT_EQ(QStringLiteral("C"), strip(2)->state().key);
    EXPECT_TRUE(match->detail().contains(QStringLiteral("Am"))) << match->detail().toStdString();
    EXPECT_EQ(QStringLiteral("Traditional"), config()->getValueString(ConfigKey("[Key]", "KeyNotation")));
    EXPECT_EQ(QStringLiteral("Am"), KeyUtils::keyToString(A_MINOR)); // Mixxx's own tables
    capture(QStringLiteral("r6-keys-browse-musical.png"));
    // Open Key.
    QMetaObject::invokeMethod(format, "activated", Q_ARG(int, 1)); settle();
    EXPECT_EQ((QStringList{"1m", "4m", "1m", "7m", unknownCut, "1d"}), keysShown());
    EXPECT_EQ(QStringLiteral("1m"), strip(1)->state().key);
    EXPECT_EQ(QStringLiteral("OpenKey"), config()->getValueString(ConfigKey("[Key]", "KeyNotation")));
    capture(QStringLiteral("r6-keys-browse-openkey.png"));
    // Back to Lancelot.
    QMetaObject::invokeMethod(format, "activated", Q_ARG(int, 0)); settle();
    EXPECT_EQ(QStringLiteral("Lancelot"), config()->getValueString(ConfigKey("[Key]", "KeyNotation")));
    EXPECT_EQ(QStringLiteral("8A"), strip(1)->state().key);
    EXPECT_EQ(decklab::KeyFormat::Lancelot, decklab::keyFormat());
    // The row leads Library, says what it covers and keeps its case
    // (segments would print "AM"); the category says "Keys · Lancelot".
    panel->selectPage(4); QCoreApplication::processEvents();
    panel->findChild<QPushButton*>(QStringLiteral("decklab-settings-category-4"))->click(); QTest::qWait(50);
    auto* row = panel->findChild<decklab::ui::ChoiceRow*>(QStringLiteral("decklab-settings-keys"));
    ASSERT_NE(nullptr, row);
    EXPECT_TRUE(row->isVisible());
    EXPECT_TRUE(panel->findChild<QWidget*>(QStringLiteral("decklab-settings-page-4"))->isAncestorOf(row));
    EXPECT_FALSE(row->segmented());
    EXPECT_EQ(QStringLiteral("Lancelot (8A)"), row->segmentText(row->currentIndex()));
    EXPECT_EQ(QString::fromUtf8("Keys · Lancelot"),
            static_cast<decklab::ui::CategoryButton*>(
                    panel->findChild<QPushButton*>(QStringLiteral("decklab-settings-category-4")))
                    ->value());
    capture(QStringLiteral("r6-keys-settings-library.png"));
    panel->findChild<QPushButton*>(QStringLiteral("decklab-settings-category-3"))->click(); QTest::qWait(50);
    capture(QStringLiteral("r6-keys-settings-display.png"));
}

// r6-keys: a notation chosen in the desktop preferences (Mixxx's
// [Library],key_notation) reaches Deck Lab too, its Settings row included;
// quick changes settle on the last one; the config always says what is shown.
TEST_F(NativeDeckLabTest, KeyFormatFollowsTheDesktopPreferences) {
    using namespace mixxx::track::io::key;
    // From the start the config says what is shown: a missing value would
    // read as Traditional in the desktop preferences, and their OK apply it.
    EXPECT_EQ(QStringLiteral("Lancelot"), config()->getValueString(ConfigKey("[Key]", "KeyNotation")));
    EXPECT_EQ(static_cast<double>(KeyUtils::KeyNotation::Lancelot),
            ControlObject::get(mixxx::library::prefs::kKeyNotationConfigKey));
    loadGenerated(1, 122, A_MINOR); ProcessBuffer(); QTest::qWait(250); settle();
    EXPECT_EQ(QStringLiteral("8A"), strip(1)->state().key);
    ControlObject::set(mixxx::library::prefs::kKeyNotationConfigKey,
            static_cast<double>(KeyUtils::KeyNotation::Traditional));
    settle(); QTest::qWait(30);
    EXPECT_EQ(decklab::KeyFormat::Musical, decklab::keyFormat());
    EXPECT_EQ(QStringLiteral("Am"), strip(1)->state().key);
    auto* format = panel->findChild<QComboBox*>(QStringLiteral("decklab-key-format"));
    ASSERT_NE(nullptr, format);
    EXPECT_EQ(2, format->currentIndex());
    // Custom (the desktop's own names) is not one of ours: nothing changes.
    ControlObject::set(mixxx::library::prefs::kKeyNotationConfigKey, static_cast<double>(KeyUtils::KeyNotation::Custom));
    settle();
    EXPECT_EQ(decklab::KeyFormat::Musical, decklab::keyFormat());
    // Two changes before the queued notifications run (two knob commits in
    // one busy pass, or the desktop and Deck Lab at once) settle on the last
    // one and stay: no notification is acted on with its old value.
    ControlProxy notation(mixxx::library::prefs::kKeyNotationConfigKey);
    int changes = 0;
    notation.connectValueChanged(&notation, [&changes](double) { ++changes; });
    QMetaObject::invokeMethod(format, "activated", Q_ARG(int, 0));
    QMetaObject::invokeMethod(format, "activated", Q_ARG(int, 1));
    changes = 0;
    for (int pass = 0; pass < 6; ++pass) {
        QCoreApplication::processEvents();
        EXPECT_EQ(decklab::KeyFormat::OpenKey, decklab::keyFormat()) << pass;
    }
    EXPECT_EQ(0, changes) << "the notation swapped back and forth";
    EXPECT_EQ(QStringLiteral("1m"), strip(1)->state().key);
    ControlObject::set(mixxx::library::prefs::kKeyNotationConfigKey,
            static_cast<double>(KeyUtils::KeyNotation::Traditional));
    ControlObject::set(mixxx::library::prefs::kKeyNotationConfigKey,
            static_cast<double>(KeyUtils::KeyNotation::Lancelot));
    changes = 0;
    for (int pass = 0; pass < 6; ++pass) {
        QCoreApplication::processEvents();
        EXPECT_EQ(decklab::KeyFormat::Lancelot, decklab::keyFormat()) << pass;
    }
    EXPECT_EQ(0, changes) << "the notation swapped back and forth";
    EXPECT_EQ(QStringLiteral("8A"), strip(1)->state().key);
    EXPECT_EQ(0, format->currentIndex());
    QMetaObject::invokeMethod(format, "activated", Q_ARG(int, 0)); settle();
    EXPECT_EQ(static_cast<double>(KeyUtils::KeyNotation::Lancelot),
            ControlObject::get(mixxx::library::prefs::kKeyNotationConfigKey));
    // At start-up a value Deck Lab does not show is written as what it
    // shows: the desktop's combined notations as the closest format, Custom
    // and junk as Lancelot.
    const auto restart = [this](const QString& saved) {
        panel.reset();
        config()->setValue(ConfigKey("[Key]", "KeyNotation"), saved);
        panel = std::make_unique<WDeckLab>(config(), nullptr, players.get(), m_pEffectsManager.get());
        for (int deck = 1; deck <= 4; ++deck) {
            panel->addWaveform(new QWidget(panel.get()), deck);
            panel->addOverview(new QWidget(panel.get()), deck);
        }
        panel->resize(800, 480); panel->show(); QCoreApplication::processEvents();
        return config()->getValueString(ConfigKey("[Key]", "KeyNotation"));
    };
    EXPECT_EQ(QStringLiteral("OpenKey"), restart(QStringLiteral("OpenKey/Traditional")));
    EXPECT_EQ(decklab::KeyFormat::OpenKey, decklab::keyFormat());
    EXPECT_EQ(static_cast<double>(KeyUtils::KeyNotation::OpenKey),
            ControlObject::get(mixxx::library::prefs::kKeyNotationConfigKey));
    EXPECT_EQ(QStringLiteral("Traditional"), restart(QStringLiteral("Traditional")));
    EXPECT_EQ(decklab::KeyFormat::Musical, decklab::keyFormat());
    EXPECT_EQ(QStringLiteral("Lancelot"), restart(QStringLiteral("Custom")));
    EXPECT_EQ(decklab::KeyFormat::Lancelot, decklab::keyFormat());
    EXPECT_EQ(QStringLiteral("Lancelot"), restart(QStringLiteral("Lancelot/Traditional")));
    // The Settings row, built after, shows the format on screen.
    config()->setValue(ConfigKey("[Key]", "KeyNotation"), QStringLiteral("OpenKey/Traditional"));
    ControlObject::set(mixxx::library::prefs::kKeyNotationConfigKey,
            static_cast<double>(KeyUtils::KeyNotation::OpenKeyAndTraditional));
    settle();
    EXPECT_EQ(decklab::KeyFormat::OpenKey, decklab::keyFormat());
    panel->selectPage(4); QCoreApplication::processEvents();
    auto* row = panel->findChild<QComboBox*>(QStringLiteral("decklab-key-format"));
    ASSERT_NE(nullptr, row);
    EXPECT_EQ(1, row->currentIndex());
    panel.reset();
}

// r6-keys: Browse's track table, the surface the owner reported (the
// library, a rekordbox USB, a USB folder: Mixxx's WTrackTableView with Deck
// Lab's KEY cell). Keys stored as text ("Am", "F#m", Open Key "1d", "Ebm",
// "12B", "4A") read in the chosen format; a format change repaints the rows
// and a table sorted by key goes round the new wheel, with the same column and
// order; text that is not a key stays as stored.
TEST_F(NativeDeckLabTest, KeyFormatReachesTheLibraryTableLive) {
    WTrackTableView* table = installGeneratedTable({QStringLiteral("Am"), QStringLiteral("F#m"), QStringLiteral("1d"),
            QStringLiteral("Ebm"), QStringLiteral("Unknown"), QStringLiteral("12B"), QStringLiteral("4A")});
    openTrackLevel();
    ASSERT_TRUE(library->isVisible());
    ASSERT_TRUE(table->isVisible());
    const int keyColumn = 1;
    ASSERT_NE(nullptr, table->itemDelegateForColumn(keyColumn));
    EXPECT_TRUE(table->itemDelegateForColumn(keyColumn)->property("decklabKey").toBool());
    const auto stored = [table] {
        QStringList keys;
        for (int row = 0; row < table->model()->rowCount(); ++row) {
            keys << table->model()->index(row, 1).data().toString();
        }
        return keys.join(QLatin1Char(' '));
    };
    // Lancelot, sorted by KEY: round the Lancelot wheel, keyless first.
    table->sortByColumn(keyColumn, Qt::AscendingOrder); settle();
    EXPECT_EQ(QStringLiteral("Unknown Ebm 4A Am 1d F#m 12B"), stored());
    EXPECT_EQ(QStringLiteral("Unknown 2A 4A 8A 8B 11A 12B"), paintedKeys(table, keyColumn));
    capture(QStringLiteral("r6-keys-table-lancelot.png"));
    // Open Key on the Settings row: the rows repaint and go round the Open
    // Key wheel, though the column and order are the same.
    struct PaintCounter : QObject {
        int paints = 0;
        bool eventFilter(QObject*, QEvent* event) override {
            if (event->type() == QEvent::Paint) ++paints;
            return false;
        }
    } counter;
    table->viewport()->installEventFilter(&counter);
    QComboBox* format = panel->findChild<QComboBox*>(QStringLiteral("decklab-key-format"));
    ASSERT_NE(nullptr, format);
    QMetaObject::invokeMethod(format, "activated", Q_ARG(int, 1)); settle(); QTest::qWait(30);
    EXPECT_GT(counter.paints, 0);
    EXPECT_EQ(keyColumn, table->horizontalHeader()->sortIndicatorSection());
    EXPECT_EQ(Qt::AscendingOrder, table->horizontalHeader()->sortIndicatorOrder());
    EXPECT_EQ(QStringLiteral("Unknown 1d Am F#m 12B Ebm 4A"), stored());
    EXPECT_EQ(QStringLiteral("Unknown 1d 1m 4m 5d 7m 9m"), paintedKeys(table, keyColumn));
    // Musical from the desktop preferences: names, in Open Key order (as Mixxx).
    ControlObject::set(mixxx::library::prefs::kKeyNotationConfigKey,
            static_cast<double>(KeyUtils::KeyNotation::Traditional));
    settle(); QTest::qWait(30);
    EXPECT_EQ(QStringLiteral("Unknown 1d Am F#m 12B Ebm 4A"), stored());
    EXPECT_EQ(QString::fromUtf8("Unknown C Am F♯m E E♭m Fm"), paintedKeys(table, keyColumn));
    capture(QStringLiteral("r6-keys-table-musical.png"));
    // Not sorted by key: the rows only repaint ([Library],key_notation reaches
    // the table itself).
    table->sortByColumn(0, Qt::AscendingOrder); settle(); QTest::qWait(30);
    counter.paints = 0;
    QMetaObject::invokeMethod(format, "activated", Q_ARG(int, 0)); settle(); QTest::qWait(30);
    EXPECT_GT(counter.paints, 0) << "the shown rows repaint in the new format";
    table->viewport()->removeEventFilter(&counter);
    EXPECT_EQ(QStringLiteral("Am F#m 1d Ebm Unknown 12B 4A"), stored());
    EXPECT_EQ(QStringLiteral("8A 11A 8B 2A Unknown 12B 4A"), paintedKeys(table, keyColumn));
}

// r6-keys: in Mixes, a row whose stored key text is not a key ("Unknown")
// shows that text, never "—" (a keyless reference deck ranks by tempo only).
TEST_F(NativeDeckLabTest, KeyTextThatIsNotAKeyShowsAsStoredInMixes) {
    loadGenerated(1, 122); ProcessBuffer(); QTest::qWait(250); settle();
    installGeneratedTable({QStringLiteral("Am"), QStringLiteral("Unknown"), QStringLiteral("12B")});
    panel->selectPage(1); QCoreApplication::processEvents(); settle();
    ASSERT_TRUE(selectRow(QStringLiteral("Library")));
    panel->browsePress(); QCoreApplication::processEvents(); settle();
    auto* list = browseList(); ASSERT_NE(nullptr, list);
    ASSERT_GE(list->count(), 1);
    ASSERT_EQ(decklab::mixesTitle(1), list->item(0)->text());
    list->setCurrentRow(0);
    panel->browsePress(); QCoreApplication::processEvents(); settle();
    QMap<QString, QString> keys;
    for (int row = 0; row < list->count(); ++row) {
        if (list->item(row)->data(DeckLabListDelegate::MixRole).toBool()) {
            keys.insert(list->item(row)->text(), list->item(row)->data(DeckLabListDelegate::MixKeyRole).toString());
        }
    }
    EXPECT_EQ(QStringLiteral("8A"), keys.value(QStringLiteral("Generated Tone 0")));
    EXPECT_EQ(QStringLiteral("Unknown"), keys.value(QStringLiteral("Generated Tone 1")));
    EXPECT_EQ(QStringLiteral("12B"), keys.value(QStringLiteral("Generated Tone 2")));
}
