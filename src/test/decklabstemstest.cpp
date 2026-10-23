// SPDX-License-Identifier: GPL-2.0-or-later
// Deck Lab stem pads on the real Decks surface with real decks. Most tests
// fake what the engine reports for a stem file ([ChannelN],stem_count and
// [ChannelN_StemM],color on a generated track); the rest load Mixxx's own
// synthetic stem fixture (sine tones, src/test/stems), so no copyrighted
// audio is involved.
#ifdef __STEM__

#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFontInfo>
#include <QImage>
#include <QLabel>
#include <QMap>
#include <QMenu>
#include <QPushButton>
#include <QTest>
#include <QTouchEvent>
#include <QUrl>
#include <algorithm>
#include <cmath>

#include "control/controlobject.h"
#include "effects/effectsmanager.h"
#include "engine/channels/enginedeck.h"
#include "engine/enginebuffer.h"
#include "library/librarycontrol.h"
#include "library/libraryfeature.h"
#include "library/sidebarmodel.h"
#include "library/treeitem.h"
#include "mixer/decklabstemplayback.h"
#include "mixer/playermanager.h"
#include "sources/soundsourceffmpeg.h"
#include "sources/soundsourceproxy.h"
#include "test/signalpathtest.h"
#include "track/track.h"
#include "util/samplebuffer.h"
#include "waveform/waveformwidgetfactory.h"
#include "widget/decklabdeckwidgets.h"
#include "widget/decklabstempads.h"
#include "widget/wdecklab.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarysidebar.h"
#include "widget/wsearchlineedit.h"

namespace {

// Mixxx's synthetic stem fixture: four sine stems named Drums, Bass, Synths
// and Vox (#fd4a4a, #ffff00, #00e8e8, #ad65ff), AAC like NI STEMS files.
const QString kStemFixture = QStringLiteral("stems/sin_AAC_256kbps_VBR.stem.mp4");
const std::array<int, 4> kFixtureColors{0xfd4a4a, 0xffff00, 0x00e8e8, 0xad65ff};
const QStringList kFixtureNames{QStringLiteral("Drums"),
        QStringLiteral("Bass"),
        QStringLiteral("Synths"),
        QStringLiteral("Vox")};

void settle() {
    for (int pass = 0; pass < 3; ++pass) {
        QCoreApplication::processEvents();
    }
}

QString stemGroup(int deck, int stem) {
    return QStringLiteral("[Channel%1_Stem%2]").arg(deck).arg(stem);
}

bool closeTo(const QColor& a, const QColor& b, int tolerance = 10) {
    return std::abs(a.red() - b.red()) <= tolerance && std::abs(a.green() - b.green()) <= tolerance &&
            std::abs(a.blue() - b.blue()) <= tolerance;
}

// Longest horizontal run of pixels close to a color, over a few rows.
int longestRun(const QImage& image, int top, int bottom, const QColor& color) {
    int longest = 0;
    for (int y = std::max(0, top); y <= std::min(image.height() - 1, bottom); ++y) {
        int run = 0;
        for (int x = 0; x < image.width(); ++x) {
            run = closeTo(image.pixelColor(x, y), color, 12) ? run + 1 : 0;
            longest = std::max(longest, run);
        }
    }
    return longest;
}

class StemTestPlayers final : public PlayerManagerInterface {
  public:
    std::array<BaseTrackPlayer*, 4> decks{};
    BaseTrackPlayer* getPlayer(const QString& group) const override {
        for (int n = 0; n < 4; ++n) {
            if (group == QStringLiteral("[Channel%1]").arg(n + 1)) {
                return decks[n];
            }
        }
        return nullptr;
    }
    BaseTrackPlayer* getPlayer(const ChannelHandle&) const override {
        return nullptr;
    }
    BaseTrackPlayer* getDeckBase(int n) const override {
        return n >= 0 && n < 4 ? decks[n] : nullptr;
    }
    int numberOfDecks() const override {
        return 4;
    }
    PreviewDeck* getPreviewDeck(int) const override {
        return nullptr;
    }
    int numberOfPreviewDecks() const override {
        return 0;
    }
    Sampler* getSampler(int) const override {
        return nullptr;
    }
    int numberOfSamplers() const override {
        return 0;
    }
};

class StemTestSource final : public LibraryFeature {
  public:
    StemTestSource(UserSettingsPointer config, const QString& icon, const QString& name)
            : LibraryFeature(nullptr, config, icon),
              name(name) {
        model.setRootItem(TreeItem::newRoot(this));
    }
    QVariant title() override {
        return name;
    }
    TreeItemModel* sidebarModel() const override {
        return const_cast<TreeItemModel*>(&model);
    }
    void activate() override {
    }
    void activateChild(const QModelIndex&) override {
    }
    TreeItemModel model;
    QString name;
};

} // namespace

class DeckLabStemsTest : public BaseSignalPathTest {
  protected:
    void SetUp() override {
        BaseSignalPathTest::SetUp();
        config()->setValue(ConfigKey("[DeckLab]", "DeckLayout"), 2);
        config()->setValue(ConfigKey("[DeckLab]", "WaveformLayout"), 0);
        fourthDeck = std::make_unique<Deck>(nullptr,
                config(),
                m_pEngineMixer.get(),
                m_pEffectsManager.get(),
                EngineChannel::CENTER,
                m_pEngineMixer->registerChannelGroup(QStringLiteral("[Channel4]")));
        addDeck(fourthDeck->getEngineDeck());
        // As PlayerManager does for every main deck: the engine mixes each
        // stem through its own channel handle and quick effect.
        for (int deck = 1; deck <= 4; ++deck) {
            for (int stem = 1; stem <= 4; ++stem) {
                const auto handle = m_pEngineMixer->registerChannelGroup(stemGroup(deck, stem));
                deckPlayer(deck)->getEngineDeck()->addStemHandle(handle);
                m_pEffectsManager->addStem(handle);
            }
        }
        WaveformWidgetFactory::createInstance()->setConfig(config());
        collection = std::make_unique<StemTestSource>(
                config(), QStringLiteral("tracks"), QStringLiteral("Collection"));
        model = std::make_unique<SidebarModel>();
        model->addLibraryFeature(collection.get());
        navigation = std::make_unique<LibraryControl>(nullptr);
        soundStatus = std::make_unique<ControlObject>(ConfigKey("[SoundManager]", "status"));
        players = std::make_unique<StemTestPlayers>();
        players->decks = {m_pMixerDeck1.get(), m_pMixerDeck2.get(), m_pMixerDeck3.get(), fourthDeck.get()};
        panel = std::make_unique<WDeckLab>(config(), nullptr, players.get(), m_pEffectsManager.get());
        auto* sidebar = new WLibrarySidebar(panel.get());
        sidebar->setModel(model.get());
        auto* library = new WLibrary(panel.get());
        auto* search = new WSearchLineEdit(panel.get(), config());
        panel->bindBrowser(search, sidebar, library);
        navigation->bindSearchboxWidget(search);
        navigation->bindSidebarWidget(sidebar);
        navigation->bindLibraryWidget(library, nullptr);
        // Geometry slots stand in for the GL waveform viewers.
        for (int deck = 1; deck <= 4; ++deck) {
            auto* slot = new QWidget(panel.get());
            slot->setStyleSheet("background:#000");
            panel->addWaveform(slot, deck);
            auto* overview = new QWidget(panel.get());
            overview->setStyleSheet("background:#181818");
            panel->addOverview(overview, deck);
        }
        panel->resize(800, 480);
        panel->show();
        QCoreApplication::processEvents();
    }
    void TearDown() override {
        panel.reset();
        soundStatus.reset();
        players.reset();
        navigation.reset();
        model.reset();
        collection.reset();
        fourthDeck.reset();
        WaveformWidgetFactory::destroy();
        BaseSignalPathTest::TearDown();
    }

    Deck* deckPlayer(int deck) const {
        const std::array<Deck*, 4> all{
                m_pMixerDeck1.get(), m_pMixerDeck2.get(), m_pMixerDeck3.get(), fourthDeck.get()};
        return all[deck - 1];
    }
    DeckLabStemPads* rowPads(int deck) const {
        return panel->findChild<DeckLabStemPads*>(QStringLiteral("decklab-stems-%1").arg(deck));
    }
    DeckLabStemPads* menuPads(int deck) const {
        return panel->findChild<DeckLabStemPads*>(QStringLiteral("decklab-card-stems-%1").arg(deck));
    }
    DeckLabStemPad* pad(int deck, int stem) const {
        return panel->findChild<DeckLabStemPad*>(QStringLiteral("decklab-stem-%1-%2").arg(deck).arg(stem));
    }
    DeckLabStemPad* menuPad(int deck, int stem) const {
        return panel->findChild<DeckLabStemPad*>(
                QStringLiteral("decklab-card-stem-%1-%2").arg(deck).arg(stem));
    }
    QWidget* deckRow() const {
        return panel->findChild<QWidget*>(QStringLiteral("decklab-deck-row"));
    }
    double mute(int deck, int stem) const {
        return ControlObject::get(ConfigKey(stemGroup(deck, stem), QStringLiteral("mute")));
    }
    std::array<double, 4> mutes(int deck) const {
        return {mute(deck, 1), mute(deck, 2), mute(deck, 3), mute(deck, 4)};
    }
    void setMute(int deck, int stem, double value) {
        ControlObject::set(ConfigKey(stemGroup(deck, stem), QStringLiteral("mute")), value);
    }
    // Generated, non-stem track (no file, synthetic names).
    TrackPointer loadGenerated(int deck, double bpm) {
        auto track = deckPlayer(deck)->loadFakeTrack(false, bpm);
        track->setTitle(QStringLiteral("Generated Tone %1").arg(bpm, 0, 'f', 0));
        track->setArtist(QStringLiteral("Deck Lab Fixture"));
        settle();
        return track;
    }
    // What the engine reports for a four-stem file, on the loaded track.
    void fakeStems(int deck) {
        ControlObject::getControl(ConfigKey(QStringLiteral("[Channel%1]").arg(deck),
                                          QStringLiteral("stem_count")))
                ->forceSet(4);
        for (int stem = 1; stem <= 4; ++stem) {
            ControlObject::getControl(ConfigKey(stemGroup(deck, stem), QStringLiteral("color")))
                    ->forceSet(kFixtureColors[stem - 1]);
        }
        settle();
    }
    // Loads the synthetic stem file through the deck, as the library does.
    TrackPointer loadStemFile(int deck) {
        TrackPointer track = Track::newTemporary(getTestDir().filePath(kStemFixture));
        track->setTitle(QStringLiteral("Generated Stems %1").arg(deck));
        track->setArtist(QStringLiteral("Deck Lab Fixture"));
        TrackPointer loaded;
        const auto connection = QObject::connect(deckPlayer(deck),
                &BaseTrackPlayer::newTrackLoaded,
                [&loaded](TrackPointer newTrack) { loaded = newTrack; });
        loadTrack(deckPlayer(deck), track);
        QElapsedTimer waited;
        waited.start();
        while (loaded != track && waited.elapsed() < 10000) {
            QTest::qWait(5);
        }
        QObject::disconnect(connection);
        EXPECT_EQ(track, loaded) << "timed out loading the stem fixture";
        settle();
        return track;
    }
    void press(QWidget* widget) {
        QTest::mousePress(widget, Qt::LeftButton, {}, widget->rect().center());
    }
    void release(QWidget* widget) {
        QTest::mouseRelease(widget, Qt::LeftButton, {}, widget->rect().center());
        settle();
    }
    void tap(QWidget* widget) {
        press(widget);
        release(widget);
    }
    void hold(QWidget* widget) {
        press(widget);
        QTest::qWait(static_cast<int>(DeckLabStemGestures::kHoldMs) + 150);
        settle();
    }
    // Waits out the double-tap window so the next tap is a plain toggle.
    void pause() {
        QTest::qWait(static_cast<int>(DeckLabStemGestures::kDoubleTapMs) + 80);
    }
    void selectLayout(int index) {
        QMetaObject::invokeMethod(panel->findChild<QComboBox*>("decklab-deck-layout"),
                "activated",
                Q_ARG(int, index));
        QTest::qWait(100);
        settle();
    }
    // Geometry of every visible Deck Lab surface outside the stem pads.
    QMap<QString, QRect> layoutSnapshot() const {
        QMap<QString, QRect> shot;
        for (auto* widget : panel->findChildren<QWidget*>()) {
            const QString name = widget->objectName();
            if (!widget->isVisible() || !name.startsWith(QStringLiteral("decklab-")) ||
                    name.startsWith(QStringLiteral("decklab-stem")) ||
                    name.startsWith(QStringLiteral("decklab-card-stem"))) {
                continue;
            }
            shot.insert(name, QRect(widget->mapTo(panel.get(), QPoint()), widget->size()));
        }
        return shot;
    }
    double mainRms() const {
        const auto buffer = m_pEngineMixer->getMainBuffer();
        double sum = 0;
        for (const CSAMPLE sample : buffer) {
            sum += double(sample) * sample;
        }
        return buffer.empty() ? 0 : std::sqrt(sum / buffer.size());
    }
    // Plays deck 1 from the start until audio arrives. The caching reader
    // decodes on its own thread, so the first buffers after a load or seek
    // can be silent; the engine keeps asking, as it does on the device.
    double rmsWhenFlowing() {
        m_pChannel1->getEngineBuffer()->queueNewPlaypos(
                mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
        double rms = 0;
        QElapsedTimer waited;
        waited.start();
        for (int n = 0; waited.elapsed() < 5000; ++n) {
            ProcessBuffer();
            rms = mainRms();
            if (n >= 4 && rms > 1e-3) {
                break;
            }
            QTest::qWait(2);
        }
        return rms;
    }
    // RMS after a few buffers from the start, once the start is cached.
    double rmsAfter(int buffers) {
        m_pChannel1->getEngineBuffer()->queueNewPlaypos(
                mixxx::audio::FramePos{0}, EngineBuffer::SEEK_STANDARD);
        for (int n = 0; n < buffers; ++n) {
            ProcessBuffer();
        }
        return mainRms();
    }
    void capture(const QString& name, QWidget* widget = nullptr) {
        const QString output = qEnvironmentVariable("DECKLAB_SCREENSHOT_DIR");
        if (!output.isEmpty()) {
            EXPECT_TRUE((widget ? widget : panel.get())->grab().save(QDir(output).filePath(name)))
                    << name.toStdString();
        }
    }
    // Touch targets of the visible pads: 44 px, inside the panel, 16 px names.
    void expectTouchPads(int deck, QWidget* within) {
        for (int stem = 1; stem <= 4; ++stem) {
            auto* stemPad = within == panel.get() ? pad(deck, stem) : menuPad(deck, stem);
            ASSERT_NE(nullptr, stemPad);
            EXPECT_TRUE(stemPad->isVisible()) << deck << " " << stem;
            EXPECT_EQ(44, stemPad->height());
            EXPECT_GE(stemPad->width(), 44);
            EXPECT_TRUE(within->rect().contains(QRect(stemPad->mapTo(within, QPoint()), stemPad->size())))
                    << deck << " " << stem;
        }
    }

    std::unique_ptr<StemTestSource> collection;
    std::unique_ptr<SidebarModel> model;
    std::unique_ptr<WDeckLab> panel;
    std::unique_ptr<StemTestPlayers> players;
    std::unique_ptr<LibraryControl> navigation;
    std::unique_ptr<ControlObject> soundStatus;
    std::unique_ptr<Deck> fourthDeck;
};

TEST_F(DeckLabStemsTest, PadsAppearOnlyForStemTracksAndOtherwiseKeepTheLayoutExactly) {
    loadGenerated(1, 124);
    loadGenerated(2, 126);
    ProcessBuffer();
    QTest::qWait(50);
    const auto today = layoutSnapshot();
    ASSERT_TRUE(today.contains(QStringLiteral("decklab-detail-1")));
    EXPECT_EQ(104 + 44 + 48 + 48, deckRow()->height());
    for (int deck = 1; deck <= 4; ++deck) {
        ASSERT_NE(nullptr, rowPads(deck));
        EXPECT_FALSE(rowPads(deck)->present()) << deck;
        EXPECT_FALSE(rowPads(deck)->isVisible()) << deck;
    }
    // Stems on a deck that is not shown change nothing.
    loadGenerated(3, 128);
    fakeStems(3);
    EXPECT_TRUE(rowPads(3)->present());
    EXPECT_EQ(today, layoutSnapshot());

    // Deck 1 holds a stem track: a 48 px stem row under its overview. The
    // waveforms give up the height; hot cues and deck tools stay put.
    fakeStems(1);
    QTest::qWait(50);
    ASSERT_TRUE(rowPads(1)->present());
    EXPECT_EQ(104 + 44 + 48 + 48 + 48, deckRow()->height());
    EXPECT_TRUE(rowPads(1)->isVisible());
    expectTouchPads(1, panel.get());
    const auto withStems = layoutSnapshot();
    for (const auto* fixed : {"decklab-hotcue-1-1", "decklab-hotcue-2-8", "decklab-1-more", "decklab-2-more"}) {
        EXPECT_EQ(today.value(QString::fromLatin1(fixed)), withStems.value(QString::fromLatin1(fixed))) << fixed;
    }
    const QRect overview = withStems.value(QStringLiteral("decklab-overview-1"));
    EXPECT_EQ(overview.bottom() + 1 + 4, pad(1, 1)->mapTo(panel.get(), QPoint()).y())
            << "4 px below the overview, like the hot-cue row";
    const int waves = today.value(QStringLiteral("decklab-lane-1")).height() +
            today.value(QStringLiteral("decklab-lane-2")).height();
    EXPECT_EQ(waves - 48,
            withStems.value(QStringLiteral("decklab-lane-1")).height() +
                    withStems.value(QStringLiteral("decklab-lane-2")).height());
    for (int stem = 1; stem <= 4; ++stem) {
        // No manifest on a generated track: generic names, colours from the controls.
        EXPECT_EQ(QStringLiteral("Stem %1").arg(stem), pad(1, stem)->state().name);
        EXPECT_EQ(QColor::fromRgb(QRgb(kFixtureColors[stem - 1])), pad(1, stem)->state().color);
        EXPECT_FALSE(pad(1, stem)->state().muted);
    }
    // Deck 2 has no stems: its half of the row stays empty and inert.
    EXPECT_TRUE(rowPads(2)->isVisible());
    for (int stem = 1; stem <= 4; ++stem) {
        EXPECT_FALSE(pad(2, stem)->isVisible());
    }
    EXPECT_EQ(withStems.value(QStringLiteral("decklab-card-1")).height(),
            withStems.value(QStringLiteral("decklab-card-2")).height());
    EXPECT_EQ(withStems.value(QStringLiteral("decklab-overview-2")),
            today.value(QStringLiteral("decklab-overview-2")).translated(0, -48));

    // A normal track on deck 1 again: exactly today's layout.
    loadGenerated(1, 130);
    QTest::qWait(50);
    EXPECT_FALSE(rowPads(1)->present());
    EXPECT_EQ(0, ControlObject::get(ConfigKey("[Channel1]", "stem_count")));
    EXPECT_EQ(today, layoutSnapshot());
}

TEST_F(DeckLabStemsTest, TapHoldAndDoubleTapDriveTheRealStemMutes) {
    loadGenerated(1, 124);
    loadGenerated(2, 126);
    fakeStems(1);
    ASSERT_TRUE(pad(1, 2)->isVisible());
    // A touch mutes on touch-down, so a drop lands on the beat; the release
    // adds nothing.
    press(pad(1, 2));
    settle();
    EXPECT_EQ(1, mute(1, 2)) << "the mute lands on touch-down";
    EXPECT_TRUE(pad(1, 2)->state().pressed);
    EXPECT_TRUE(pad(1, 2)->state().muted);
    release(pad(1, 2));
    EXPECT_EQ((std::array<double, 4>{0, 1, 0, 0}), mutes(1));
    EXPECT_TRUE(pad(1, 2)->state().muted);
    // Muted reads without colour: hollow bar (no stem colour inside it), dim
    // fill and a strike line through the name.
    const QImage muted = pad(1, 2)->grab().toImage();
    const QImage live = pad(1, 1)->grab().toImage();
    const QColor bass = QColor::fromRgb(QRgb(kFixtureColors[1]));
    const QColor drums = QColor::fromRgb(QRgb(kFixtureColors[0]));
    EXPECT_TRUE(closeTo(drums, live.pixelColor(8, live.height() / 2))) << "live: solid colour bar";
    EXPECT_FALSE(closeTo(bass, muted.pixelColor(8, muted.height() / 2))) << "muted: hollow bar";
    EXPECT_LT(muted.pixelColor(muted.width() - 3, 4).lightness(),
            live.pixelColor(live.width() - 3, 4).lightness())
            << "muted: dimmer fill";
    EXPECT_GE(longestRun(muted, muted.height() / 2 - 3, muted.height() / 2 + 2, QColor(0x9a, 0xa3, 0xaf)), 20)
            << "muted: strike line";
    EXPECT_LT(longestRun(live, live.height() / 2 - 3, live.height() / 2 + 2, QColor(0x9a, 0xa3, 0xaf)), 20);
    pause();
    tap(pad(1, 2));
    EXPECT_EQ((std::array<double, 4>{0, 0, 0, 0}), mutes(1));
    // A controller mute shows on the pad.
    setMute(1, 4, 1);
    settle();
    EXPECT_TRUE(pad(1, 4)->state().muted);
    // Hold solos while held; release restores the state before the hold.
    hold(pad(1, 1));
    EXPECT_EQ((std::array<double, 4>{0, 1, 1, 1}), mutes(1));
    EXPECT_TRUE(pad(1, 1)->state().solo);
    release(pad(1, 1));
    EXPECT_EQ((std::array<double, 4>{0, 0, 0, 1}), mutes(1));
    EXPECT_FALSE(pad(1, 1)->state().solo);
    // Double tap restores every stem.
    pause();
    tap(pad(1, 3));
    EXPECT_EQ((std::array<double, 4>{0, 0, 1, 1}), mutes(1));
    tap(pad(1, 3));
    EXPECT_EQ((std::array<double, 4>{0, 0, 0, 0}), mutes(1));
    // Deck 2 was never touched.
    EXPECT_EQ((std::array<double, 4>{0, 0, 0, 0}), mutes(2));
}

TEST_F(DeckLabStemsTest, StaleGesturesNeverLeaveAStemSoloedOrMuted) {
    loadGenerated(1, 124);
    loadGenerated(2, 126);
    loadGenerated(3, 128);
    fakeStems(1);
    // A page change mid-hold ends the solo at once; the late release is spent.
    hold(pad(1, 1));
    ASSERT_EQ((std::array<double, 4>{0, 1, 1, 1}), mutes(1));
    panel->selectPage(1);
    settle();
    EXPECT_EQ((std::array<double, 4>{0, 0, 0, 0}), mutes(1));
    QTest::mouseRelease(panel.get(), Qt::LeftButton, {}, QPoint(400, 300));
    settle();
    EXPECT_EQ((std::array<double, 4>{0, 0, 0, 0}), mutes(1));
    panel->selectPage(0);
    settle();
    // A layout change before the hold time: the touch-down mute stands (it
    // already happened), but no solo follows and the release adds nothing.
    press(pad(1, 2));
    selectLayout(2);
    QTest::qWait(450);
    release(pad(1, 2));
    EXPECT_EQ((std::array<double, 4>{0, 1, 0, 0}), mutes(1));
    selectLayout(1);
    ASSERT_TRUE(pad(1, 2)->isVisible());
    setMute(1, 2, 0);
    settle();
    // The layer on the left changes to deck 3 mid-hold.
    hold(pad(1, 3));
    ASSERT_EQ((std::array<double, 4>{1, 1, 0, 1}), mutes(1));
    ControlObject::set(ConfigKey("[DeckLabLeft]", "current"), 1);
    settle();
    EXPECT_FALSE(pad(1, 3)->isVisible());
    EXPECT_EQ((std::array<double, 4>{0, 0, 0, 0}), mutes(1));
    ControlObject::set(ConfigKey("[DeckLabLeft]", "current"), 0);
    settle();
    ASSERT_TRUE(pad(1, 3)->isVisible());
    // The touch is cancelled mid-hold.
    hold(pad(1, 4));
    ASSERT_EQ((std::array<double, 4>{1, 1, 1, 0}), mutes(1));
    {
        static auto* device = QTest::createTouchDevice();
        QTouchEvent cancel(QEvent::TouchCancel, device);
        QCoreApplication::sendEvent(pad(1, 4), &cancel);
    }
    settle();
    EXPECT_EQ((std::array<double, 4>{0, 0, 0, 0}), mutes(1));
    release(pad(1, 4));
    EXPECT_EQ((std::array<double, 4>{0, 0, 0, 0}), mutes(1)) << "the release after a cancel is spent";
    // The application loses the screen mid-hold.
    hold(pad(1, 2));
    ASSERT_EQ((std::array<double, 4>{1, 0, 1, 1}), mutes(1));
    {
        QEvent deactivate(QEvent::ApplicationDeactivate);
        QCoreApplication::sendEvent(qApp, &deactivate);
    }
    settle();
    EXPECT_EQ((std::array<double, 4>{0, 0, 0, 0}), mutes(1));
    release(pad(1, 2));
    // A new track mid-hold. Mixxx resets stems on load, so nothing returns.
    pause();
    tap(pad(1, 4));
    ASSERT_EQ((std::array<double, 4>{0, 0, 0, 1}), mutes(1));
    hold(pad(1, 1));
    ASSERT_EQ((std::array<double, 4>{0, 1, 1, 1}), mutes(1));
    loadGenerated(1, 130);
    release(pad(1, 1));
    EXPECT_EQ((std::array<double, 4>{0, 0, 0, 0}), mutes(1));
    EXPECT_FALSE(rowPads(1)->present());
    // With stem_auto_reset off the deck keeps its stem state across loads,
    // so the state from before the hold comes back, not the solo.
    config()->setValue(ConfigKey("[Mixer Profile]", "stem_auto_reset"), false);
    fakeStems(1);
    tap(pad(1, 4));
    ASSERT_EQ((std::array<double, 4>{0, 0, 0, 1}), mutes(1));
    hold(pad(1, 1));
    ASSERT_EQ((std::array<double, 4>{0, 1, 1, 1}), mutes(1));
    loadGenerated(1, 132);
    release(pad(1, 1));
    EXPECT_EQ((std::array<double, 4>{0, 0, 0, 1}), mutes(1));
    config()->setValue(ConfigKey("[Mixer Profile]", "stem_auto_reset"), true);
}

TEST_F(DeckLabStemsTest, FourDeckViewOffersStemsInTheMoreMenu) {
    for (int deck = 1; deck <= 4; ++deck) {
        loadGenerated(deck, 120 + 2 * deck);
    }
    selectLayout(2);
    const auto fourDecks = layoutSnapshot();
    const int rowHeight = deckRow()->height();
    EXPECT_EQ(138 + 44 + 48, rowHeight);
    fakeStems(2);
    QTest::qWait(50);
    // The compact cards keep their layout: stems live in More.
    EXPECT_EQ(fourDecks, layoutSnapshot());
    EXPECT_EQ(rowHeight, deckRow()->height());
    EXPECT_FALSE(rowPads(2)->isVisible());
    auto* more = panel->findChild<QPushButton*>("decklab-card-2-more");
    ASSERT_NE(nullptr, more);
    more->click();
    settle();
    auto* menu = panel->findChild<QMenu*>("decklab-card-2-more-menu");
    ASSERT_NE(nullptr, menu);
    ASSERT_TRUE(menu->isVisible());
    ASSERT_NE(nullptr, menuPads(2));
    EXPECT_TRUE(menuPads(2)->isVisible());
    expectTouchPads(2, menu);
    EXPECT_GE(menu->width(), 352);
    tap(menuPad(2, 4));
    EXPECT_EQ((std::array<double, 4>{0, 0, 0, 1}), mutes(2));
    EXPECT_TRUE(menuPad(2, 4)->state().muted);
    capture(QStringLiteral("stems-four-deck-more-menu.png"), menu);
    // Closing the menu mid-hold ends the solo.
    hold(menuPad(2, 1));
    ASSERT_EQ((std::array<double, 4>{0, 1, 1, 1}), mutes(2));
    menu->close();
    settle();
    EXPECT_EQ((std::array<double, 4>{0, 0, 0, 1}), mutes(2));
    // A deck without stems offers none.
    panel->findChild<QPushButton*>("decklab-card-1-more")->click();
    settle();
    auto* menu1 = panel->findChild<QMenu*>("decklab-card-1-more-menu");
    ASSERT_TRUE(menu1->isVisible());
    EXPECT_FALSE(menuPads(1)->isVisible());
    menu1->close();
    settle();
    capture(QStringLiteral("stems-four-deck.png"));
    // Back to two decks: deck 2 shows its row, with the mute still shown.
    selectLayout(1);
    EXPECT_TRUE(rowPads(2)->isVisible());
    EXPECT_TRUE(pad(2, 4)->state().muted);
    EXPECT_EQ(104 + 44 + 48 + 48 + 48, deckRow()->height());
}

TEST_F(DeckLabStemsTest, StemFileShowsManifestNamesAndColoursAndMutesTheEngine) {
    loadStemFile(1);
    ASSERT_EQ(4, ControlObject::get(ConfigKey("[Channel1]", "stem_count")));
    ASSERT_EQ(mixxx::audio::ChannelCount::stem(), m_pChannel1->getEngineBuffer()->getChannelCount());
    loadGenerated(2, 126);
    QTest::qWait(50);
    ASSERT_TRUE(rowPads(1)->present());
    expectTouchPads(1, panel.get());
    for (int stem = 1; stem <= 4; ++stem) {
        EXPECT_EQ(kFixtureNames[stem - 1], pad(1, stem)->state().name);
        EXPECT_EQ(QColor::fromRgb(QRgb(kFixtureColors[stem - 1])), pad(1, stem)->state().color);
        EXPECT_TRUE(pad(1, stem)->accessibleName().contains(kFixtureNames[stem - 1]));
    }
    // The pads drive the engine's own stem mix: all four muted is silence,
    // a double tap brings the mix back.
    ControlObject::set(ConfigKey("[Channel1]", "play"), 1);
    const double playing = rmsWhenFlowing();
    ASSERT_GT(playing, 1e-3) << "the stem mix plays";
    for (int stem = 1; stem <= 4; ++stem) {
        tap(pad(1, stem));
    }
    EXPECT_EQ((std::array<double, 4>{1, 1, 1, 1}), mutes(1));
    EXPECT_LT(rmsAfter(6), 1e-5) << "all four stems muted";
    tap(pad(1, 1));
    tap(pad(1, 1));
    EXPECT_EQ((std::array<double, 4>{0, 0, 0, 0}), mutes(1));
    EXPECT_GT(rmsAfter(6), 1e-3) << "a double tap brings the mix back";
    ControlObject::set(ConfigKey("[Channel1]", "play"), 0);
    ProcessBuffer();
    settle();
    // Screens: deck 1 with Vox muted next to a normal track, then both decks
    // with stems while Drums on deck 2 is held solo.
    pause();
    tap(pad(1, 4));
    capture(QStringLiteral("stems-two-deck.png"));
    loadStemFile(2);
    QTest::qWait(50);
    hold(pad(2, 1));
    EXPECT_TRUE(pad(2, 1)->state().solo);
    capture(QStringLiteral("stems-two-deck-solo.png"));
    release(pad(2, 1));
    EXPECT_EQ((std::array<double, 4>{0, 0, 0, 0}), mutes(2));
    // Larger touch panels scale the row; side-by-side waveforms keep it.
    panel->resize(1024, 600);
    QTest::qWait(100);
    expectTouchPads(1, panel.get());
    expectTouchPads(2, panel.get());
    capture(QStringLiteral("stems-two-deck-1024x600.png"));
    panel->resize(800, 480);
    QTest::qWait(100);
    EXPECT_EQ(QSize(800, 480), panel->size());
}

TEST_F(DeckLabStemsTest, StemTracksCanPlayAsANormalTrack) {
    panel->selectPage(4);
    settle();
    auto* combo = panel->findChild<QComboBox*>("decklab-stem-playback");
    ASSERT_NE(nullptr, combo);
    ASSERT_EQ(2, combo->count());
    EXPECT_EQ(0, combo->currentIndex()) << "stems by default";
    EXPECT_TRUE(combo->isVisible());
    EXPECT_GE(combo->height(), 44);
    auto* hint = panel->findChild<QLabel*>("decklab-stem-playback-hint");
    ASSERT_NE(nullptr, hint);
    EXPECT_GE(QFontInfo(hint->font()).pixelSize(), 14);
    auto* caption = panel->findChild<QLabel*>("decklab-stem-playback-caption");
    ASSERT_NE(nullptr, caption);
    EXPECT_EQ(caption->mapTo(panel.get(), QPoint()).y() + caption->height() / 2,
            combo->mapTo(panel.get(), QPoint()).y() + combo->height() / 2)
            << "the caption lines up with the choice, not the hint";
    capture(QStringLiteral("stems-settings-deck.png"));
    QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, 1));
    EXPECT_EQ(decklab::StemPlayback::StereoMix, decklab::stemPlayback(config()));
    panel->selectPage(0);
    settle();
    // The next load plays the stem file as one stereo mix: Mixxx still
    // reports its stems, but there is nothing to mute, so no pads.
    loadStemFile(1);
    QTest::qWait(50);
    EXPECT_EQ(4, ControlObject::get(ConfigKey("[Channel1]", "stem_count")));
    EXPECT_EQ(mixxx::audio::ChannelCount::stereo(), m_pChannel1->getEngineBuffer()->getChannelCount());
    EXPECT_EQ(mixxx::StemChannelSelection(mixxx::StemChannel::All),
            m_pChannel1->getEngineBuffer()->getStemMask());
    EXPECT_FALSE(rowPads(1)->present());
    EXPECT_FALSE(rowPads(1)->isVisible());
    EXPECT_EQ(104 + 44 + 48 + 48, deckRow()->height());
    ControlObject::set(ConfigKey("[Channel1]", "play"), 1);
    EXPECT_GT(rmsWhenFlowing(), 1e-3) << "the stereo mix plays";
    ControlObject::set(ConfigKey("[Channel1]", "play"), 0);
    ProcessBuffer();
    // An explicit request (Mixxx's single-stem load) is kept as asked.
    EXPECT_EQ(mixxx::StemChannelSelection(mixxx::StemChannel::Second),
            decklab::stemLoadSelection(config(), mixxx::StemChannel::Second));
    // Back to stems for the next load.
    panel->selectPage(4);
    settle();
    QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, 0));
    panel->selectPage(0);
    settle();
    loadStemFile(1);
    QTest::qWait(50);
    EXPECT_EQ(mixxx::audio::ChannelCount::stem(), m_pChannel1->getEngineBuffer()->getChannelCount());
    EXPECT_TRUE(rowPads(1)->present());
    EXPECT_TRUE(rowPads(1)->isVisible());
}

// Records what stems cost, for the Pi 4 estimate in DECKLAB-NEXT-stems.md.
// Timing is never asserted (shared CI runners); the numbers land in the
// test XML as properties and in the log.
TEST_F(DeckLabStemsTest, StemDecodeAndMixCostProbe) {
    const QString path = getTestDir().filePath(kStemFixture);
    // Decode, as the caching reader does on its worker thread.
    const auto decodeSeconds = [](mixxx::AudioSource& source) {
        mixxx::SampleBuffer buffer(8192 * source.getSignalInfo().getChannelCount());
        const auto range = source.frameIndexRange();
        QElapsedTimer timer;
        timer.start();
        for (SINT start = range.start(); start < range.end(); start += 8192) {
            const auto chunk = mixxx::IndexRange::between(start, std::min(range.end(), start + 8192));
            source.readSampleFrames(mixxx::WritableSampleFrames(chunk,
                    mixxx::SampleBuffer::WritableSlice(buffer.data(),
                            source.getSignalInfo().frames2samples(chunk.length()))));
        }
        return timer.nsecsElapsed() / 1e9;
    };
    const auto best = [&decodeSeconds](const std::function<mixxx::AudioSourcePointer()>& open) {
        double seconds = 1e9;
        for (int pass = 0; pass < 3; ++pass) {
            auto source = open();
            if (!source) {
                return -1.0;
            }
            seconds = std::min(seconds, decodeSeconds(*source));
        }
        return seconds;
    };
    TrackPointer track = Track::newTemporary(path);
    mixxx::AudioSource::OpenParams stems;
    stems.setChannelCount(mixxx::audio::ChannelCount::stem());
    mixxx::AudioSource::OpenParams stereo;
    stereo.setChannelCount(mixxx::audio::ChannelCount::stereo());
    stereo.setStemMask(mixxx::StemChannel::All);
    mixxx::AudioSource::OpenParams mixdown;
    mixdown.setChannelCount(mixxx::audio::ChannelCount::stereo());
    const double stemDecode = best([&] { return SoundSourceProxy(track).openAudioSource(stems); });
    const double sumDecode = best([&] { return SoundSourceProxy(track).openAudioSource(stereo); });
    // The file's own mixdown stream alone: what one ordinary AAC track costs.
    const double mixdownDecode = best([&] {
        auto source = std::make_shared<mixxx::SoundSourceFFmpeg>(QUrl::fromLocalFile(path), 0);
        return source->open(mixxx::AudioSource::OpenMode::Strict, mixdown) ==
                        mixxx::AudioSource::OpenResult::Succeeded
                ? mixxx::AudioSourcePointer(source)
                : mixxx::AudioSourcePointer();
    });
    ASSERT_GT(stemDecode, 0);
    ASSERT_GT(sumDecode, 0);
    ASSERT_GT(mixdownDecode, 0);
    auto probe = SoundSourceProxy(track).openAudioSource(stems);
    ASSERT_TRUE(probe);
    const double seconds = probe->getSignalInfo().frames2secs(probe->frameLength());
    RecordProperty("audio_seconds", QString::number(seconds, 'f', 2).toStdString());
    RecordProperty("decode_realtime_mixdown_stream", QString::number(seconds / mixdownDecode, 'f', 1).toStdString());
    RecordProperty("decode_realtime_four_stems", QString::number(seconds / stemDecode, 'f', 1).toStdString());
    RecordProperty("decode_realtime_stereo_sum", QString::number(seconds / sumDecode, 'f', 1).toStdString());

    // Engine: one deck playing the file at +4 % tempo, stems or stereo mix,
    // keylock off (linear) and on (Rubber Band, Mixxx's default engine).
    // Only the engine call is timed. Buffers are paced so the reader thread
    // keeps up, and the count of audible buffers shows that audio flowed.
    constexpr int kBuffers = 150;
    struct Timing {
        double micros = 0;
        int audible = 0;
    };
    const auto timeBuffers = [this](bool playing) {
        Timing timing;
        qint64 nanos = 0;
        for (int n = 0; n < kBuffers; ++n) {
            QElapsedTimer timer;
            timer.start();
            ProcessBuffer();
            nanos += timer.nsecsElapsed();
            timing.audible += playing && mainRms() > 1e-3 ? 1 : 0;
            QTest::qWait(1);
        }
        timing.micros = nanos / 1e3 / kBuffers;
        return timing;
    };
    const auto engineTiming = [this, &timeBuffers](bool keylock) {
        ControlObject::set(ConfigKey("[Channel1]", "keylock"), keylock ? 1 : 0);
        ControlObject::set(ConfigKey("[Channel1]", "rate"), 0.5); // +4 % at �8 %
        ControlObject::set(ConfigKey("[Channel1]", "play"), 1);
        EXPECT_GT(rmsWhenFlowing(), 1e-3);
        const Timing timing = timeBuffers(true);
        ControlObject::set(ConfigKey("[Channel1]", "play"), 0);
        ProcessBuffer();
        return timing;
    };
    const double idle = timeBuffers(false).micros;
    loadStemFile(1);
    ASSERT_EQ(mixxx::audio::ChannelCount::stem(), m_pChannel1->getEngineBuffer()->getChannelCount());
    const Timing stemsLinear = engineTiming(false);
    const Timing stemsKeylock = engineTiming(true);
    config()->setValue(decklab::stemPlaybackConfigKey(), 1);
    loadStemFile(1);
    ASSERT_EQ(mixxx::audio::ChannelCount::stereo(), m_pChannel1->getEngineBuffer()->getChannelCount());
    const Timing stereoLinear = engineTiming(false);
    const Timing stereoKeylock = engineTiming(true);
    const double bufferMicros = 1e6 * (kProcessBufferSize / 2) / 44100.0;
    const auto micros = [](double value) { return QString::number(value, 'f', 1).toStdString(); };
    const auto audible = [](const Timing& timing) {
        return QStringLiteral("%1/%2").arg(timing.audible).arg(kBuffers).toStdString();
    };
    RecordProperty("engine_buffer_us", QString::number(bufferMicros, 'f', 0).toStdString());
    RecordProperty("engine_idle_us", micros(idle));
    RecordProperty("engine_stems_linear_us", micros(stemsLinear.micros));
    RecordProperty("engine_stems_linear_audible", audible(stemsLinear));
    RecordProperty("engine_stems_keylock_us", micros(stemsKeylock.micros));
    RecordProperty("engine_stems_keylock_audible", audible(stemsKeylock));
    RecordProperty("engine_stereo_linear_us", micros(stereoLinear.micros));
    RecordProperty("engine_stereo_linear_audible", audible(stereoLinear));
    RecordProperty("engine_stereo_keylock_us", micros(stereoKeylock.micros));
    RecordProperty("engine_stereo_keylock_audible", audible(stereoKeylock));
    EXPECT_GT(stemsLinear.micros, 0);
    EXPECT_GT(stereoKeylock.micros, 0);
    config()->setValue(decklab::stemPlaybackConfigKey(), 0);
}

#endif // __STEM__
