// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <array>
#include <memory>
#include <vector>
#include <functional>

#include "decklab/decklabphraseview.h"
#include "preferences/usersettings.h"
#include "track/track_decl.h"
#include "widget/wbasewidget.h"
#include "widget/decklabdeckwidgets.h"
#include "widget/decklabtempopresenter.h"
#include <QWidget>

class ControlObject;
class ControlProxy;
class DeckLabLabFx; // r4-labfx: the Lab FX page (widget/decklablabfx.h)
class DeckLabLaneDecks; // r4-waveforms: the DECKS row under compact lanes (widget/decklablanes.h)
class DeckLabSampler;
class DeckLabPadBlock;
class DeckLabStemPads;
class DeckLabRail;
class DeckLabSettingsPage;
class DeckLabStatusZone;
class DeckLabSystem;
class DeckLabSystemUi;
class Library;
class PlayerManagerInterface;
class EffectsManager;
class WLibrary;
class WLibrarySidebar;
class WSearchLineEdit;
class QBoxLayout;
class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QProgressBar;
class QStackedWidget;
class QVBoxLayout;
class QGridLayout;
struct DeckLabBrowseState;
namespace decklab {
struct UsbStick;
struct MatchReference; // r4-browse
enum class SourceState; // r4-browse
} // namespace decklab

// A native appliance surface. All audio remains in Mixxx's existing engine.
// No window coordinates, injected keys, desktop focus polling or MIDI IPC.
class WDeckLab : public QWidget, public WBaseWidget {
    Q_OBJECT
  public:
    WDeckLab(UserSettingsPointer config, Library* library,
            PlayerManagerInterface* players, EffectsManager* effects, QWidget* parent = nullptr);
    ~WDeckLab() override;
    QWidget* waveformContainer() const { return m_waveforms; }
    QWidget* browserContainer() const { return m_browser; }
    void addWaveform(QWidget* widget, int deck);
    void addOverview(QWidget* widget, int deck);
    void bindBrowser(WSearchLineEdit* search, WLibrarySidebar* sidebar, WLibrary* library);
    void selectPage(int page);
    int currentPage() const { return m_page; }
    void browsePress();
    void browseBack();
    // r4-sampler: opens the Sampler page (Lab FX's SMP destination links
    // here); a sampler number also shows the bank that holds it.
    void showSampler(int sampler = 0);
    void openSource();
    void openCollection();
    // r5-polish (tests): the Match bar as if the list held `count` matches.
    void setMatchCountForTesting(int count);
    // r4-labfx: Lab FX out. Default: CUT (dry now); musical: RELEASE (the
    // tail rings out); mute: ECHO OUT (the track drops for a bar).
    void releaseEffects(bool musical = false, bool mute = false);
    // Ends a Lab FX pad hold whose release went astray; a latched effect
    // keeps playing.
    void endHolds();
    void setAudioOutputConfigured(bool configured);
    /// Deck Lab system integration (decklabd, Deck Setup, System pages).
    void attachSystem(DeckLabSystem* system);
    /// The GL waveforms are native child windows that cover any non-native
    /// widget drawn over them. A full-surface overlay (Deck Setup, the first
    /// run, a confirmation) hides them while it is visible: each owner counts
    /// once, destroying an owner releases it, and the waveforms return with a
    /// forced render when the last owner releases. The layout keeps their
    /// space. See decklab/decklaboverlay.h.
    void setWaveformsSuppressed(QObject* owner, bool suppressed);
    bool waveformsSuppressed() const {
        return !m_waveformSuppressors.isEmpty();
    }
    Library* library() const { return m_library; }

  signals:
    void advancedPreferencesRequested();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override; // r4-waveforms: compact lane height

  private:
    ControlProxy* control(const QString& group, const QString& item);
    double value(const QString& group, const QString& item);
    void set(const QString& group, const QString& item, double value);
    void pulse(const QString& group, const QString& item);
    void watch(const QString& group, const QString& item, std::function<void(double)> callback);
    void createControl(const QString& group, const QString& item, double initial = 0);
    QPushButton* button(const QString& text, std::function<void()> action,
            QWidget* parent = nullptr, const QString& name = {});
    QWidget* buildDecks();
    QWidget* buildTransport(int deck, bool followsLayer, QWidget* parent);
    // r4-decks: the pad block (decklabpads.h) for a side row or a card's MORE.
    DeckLabPadBlock* buildPads(int deck, bool followsLayer, QWidget* parent);
    void refreshPads(int deck, bool card);
    void showBeatgrid(int deck, bool followsLayer);
    void showTrackInfo(int actualDeck);
    void showControllerFxInfo();
    void refreshTransport(int deck, const QString& prefix);
    QWidget* buildBrowse();
    QWidget* buildSampler();
    void tickSampler(); // r4-sampler
    bool samplersGetLabFx() const; // round4 merge: Lab FX targets SMP
    QWidget* buildEffects();
    QWidget* buildSettings();
    void showSources();
    void showTracks();
    void loadDeck(int deck);
    void refreshDeck(int deck);
    void refreshLoadButtons();
    void refreshFx();
    void tickEffects();
    void tool(int id);
    void setKeyNotation(int notation);
    void updateStatus();
    void updateBreadcrumb();
    void updateLayers();
    // r4-waveforms: the compact lane zone's height for this surface (two
    // lanes and the DECKS row, decklab::lanes::compactZoneHeight).
    int compactLaneZoneHeight() const;
    void releaseTouchCue();
    void setSearchOpen(bool open);
    // Browse page internals (browse-ui workstream).
    void browseMove(int delta, bool listLevel);
    void browseEnter();
    void resetBrowseLevels();
    void enterCategories(const QModelIndex& source);
    void showCategories();
    void openCategory(int row);
    void openCategoryValue(int row);
    void activateBrowseSource();
    void clearBrowseFilter();
    void checkRekordboxImport();
    void syncSelectedTrack();
    void refreshBrowseCard();
    void refreshPreviewButton();
    void togglePreview();
    void stopPreview();
    void scheduleMatchRefresh();
    void refreshMatchReference();
    void showLoadRefused(int deck);
    void showLoadUnavailable(int slot);
    int matchReferenceDeck();
    QStringList browseSegments() const;
    // qw-library: source home, USB sticks, search scope, Match bar, knob focus.
    QWidget* buildMatchBar(QWidget* parent);
    void showHome(const QString& highlight = QString());
    void rebuildHome(const QString& highlight);
    void refreshHomeIfSticksChanged();
    QString homeRootSubtitle(const QModelIndex& root);
    QString stickSubtitle(const decklab::UsbStick& stick);
    int libraryTrackCount();
    void openHomeRow(int row);
    void openStick(const QString& path);
    void showFolder(const QString& path);
    void openFolderRow(int row);
    void openFolderTracks();
    QModelIndex sidebarIndexForPath(const QString& path);
    void retryRekordboxRead();
    QString currentSourceName() const;
    void openUsbInBrowse();
    void showOnlyMatches();
    void applyMatchFilter(bool force);
    void updateMatchBar();
    void scheduleMatchCount();
    void refreshMatchCount();
    void scheduleFocusReclaim();
    void reclaimFocus();
    // r4-browse: source home tiles, deck dock, knob legend, Mixes with deck N
    // (decklabbrowsehome/dock/legend/mix.h).
    bool browseTrackLevel() const;
    void applyBrowsePresentation();
    void refreshDock();
    void refreshDockFor(int deck);
    void scheduleKnobLegend();
    void updateKnobLegend();
    void flashKnobLegend();
    decklab::SourceState stickState(const decklab::UsbStick& stick);
    QString stickDetail(const decklab::UsbStick& stick);
    QString homeStateSignature(const QList<decklab::UsbStick>& sticks);
    void retryStickFromHome(const QString& path);
    decklab::MatchReference mixReference();
    void refreshMixCandidates();
    int mixSkipRow();
    void updateMixesRow();
    void showMixes();
    void fillMixes(bool keepCurrent);
    void syncMixSelection();
    int displayedDeck(int side) const;
    void setDisplayedDeck(int side, int deck);
    QString channel(int deck) const;

    // Decks page refresh. State controls mark a deck dirty and one queued
    // flush refreshes it; the display tick only advances playing clocks.
    struct DeckProxies {
        ControlProxy* play = nullptr;
        ControlProxy* loaded = nullptr;
        ControlProxy* bpm = nullptr;
        ControlProxy* localBpm = nullptr;
        ControlProxy* rateRatio = nullptr;
        ControlProxy* rateRange = nullptr;
        ControlProxy* duration = nullptr;
        ControlProxy* position = nullptr;
        ControlProxy* samples = nullptr;
        ControlProxy* key = nullptr;
        ControlProxy* sync = nullptr;
        ControlProxy* leader = nullptr;
        ControlProxy* keylock = nullptr;
        ControlProxy* quantize = nullptr;
        ControlProxy* pfl = nullptr;
        ControlProxy* loop = nullptr;
        ControlProxy* loopSize = nullptr;
        ControlProxy* slip = nullptr;
        ControlProxy* vu = nullptr;
        ControlProxy* peak = nullptr;
        ControlProxy* zoom = nullptr;
        ControlProxy* cuePoint = nullptr;
        ControlProxy* rate = nullptr;
        std::array<ControlProxy*, 8> cuePosition{};
        std::array<ControlProxy*, 8> cueType{};
        std::array<ControlProxy*, 8> cueColor{};
    };
    struct TransportState {
        int deck = -1;
        bool loaded = false;
        bool playing = false;
        bool looping = false;
        bool quantize = false;
        bool keylock = false;
        bool slip = false; // r4-decks: SLIP in MORE
        bool synced = false; // sync lock: the SYNC button is lit
        bool feedback = false;
        double size = -1;
        bool operator==(const TransportState&) const = default;
    };
    struct TransportButtons {
        QPushButton* play = nullptr;
        QPushButton* cue = nullptr;
        QPushButton* sync = nullptr;
        QPushButton* halve = nullptr;
        QPushButton* loop = nullptr;
        QPushButton* twice = nullptr;
        QPushButton* quantize = nullptr;
        QPushButton* keylock = nullptr;
        QPushButton* slip = nullptr;
        QPushButton* more = nullptr;
        TransportState shown;
    };
    void markDeckDirty(int deck);
    void flushDirtyDecks();
    void tickDecks();
    void tickMeters();
    void refreshControllerFx();
    void connectControllerFx();
    bool deckShown(int deck) const;
    void ensureDeckSurfaces(int deck);
    void fillDeckClock(int deck, DeckLabStripState& state);
    void refreshDeckClock(int deck);
    void applyDeck(int deck);
    void applyWaveformSuppression();
    int referenceDeck(int deck) const;
    void noteDeckTouched(int deck);

    UserSettingsPointer m_config;
    Library* m_library;
    PlayerManagerInterface* m_players;
    EffectsManager* m_effects;
    std::vector<std::unique_ptr<ControlObject>> m_ownedControls;
    QHash<QString, QHash<QString, ControlProxy*>> m_controls;
    std::array<DeckProxies, 4> m_deck{};
    std::array<bool, 4> m_deckDirty{};
    std::array<double, 4> m_shownPosition{};
    bool m_flushQueued = false;
    bool m_fxSummaryDirty = false;
    bool m_fxWatchersReady = false;
    quint64 m_layoutSignature = 0;
    QHash<QString, TransportButtons> m_transport;
    std::array<QString, 2> m_sidePrefixes;
    std::array<QString, 4> m_cardPrefixes;
    QTimer m_meterTimer;
    DeckLabLevelMeter* m_masterMeter = nullptr;
    std::array<ControlProxy*, 2> m_mainVu{};
    ControlProxy* m_mainPeak = nullptr;
    QWidget* m_rail = nullptr;
    QWidget* m_waveZone = nullptr;
    QGridLayout* m_zoneGrid = nullptr;
    QWidget* m_deckRow = nullptr;
    QGridLayout* m_deckGrid = nullptr;
    DeckLabChipButton* m_fxChips = nullptr;
    std::array<QWidget*, 4> m_cards{};
    std::array<DeckLabDeckStrip*, 4> m_strips{};
    std::array<DeckLabMiniStrip*, 4> m_minis{};
    DeckLabLaneDecks* m_laneDecks = nullptr; // r4-waveforms
    // Stem pads per deck (stems workstream; decklabstempads.h). r4-decks:
    // they live in the side pad blocks (decks 1/3 left, 2/4 right).
    std::array<DeckLabStemPads*, 4> m_stemPads{};
    std::array<DeckLabStripState, 4> m_stripStates{};
    std::array<TrackPointer, 4> m_tracks{};
    std::array<QList<QMetaObject::Connection>, 4> m_trackConnections{};
    std::array<QColor, 25> m_keyColors{};
    std::array<qint64, 4> m_lastTouch{};
    std::array<qint64, 4> m_lastLoad{};
    std::array<bool, 4> m_lastPlay{};
    int m_endWarningSeconds = 30;
    int m_lastOverloads = 0;
    qint64 m_overloadUntil = 0;
    QVBoxLayout* m_root;
    QStackedWidget* m_pages;
    QWidget* m_deckControls;
    QWidget* m_waveforms;
    QGridLayout* m_waveLayout;
    QWidget* m_browser;
    QStackedWidget* m_browseStack;
    WLibrarySidebar* m_sidebar = nullptr;
    WLibrary* m_libraryWidget = nullptr;
    WSearchLineEdit* m_search = nullptr;
    QBoxLayout* m_searchRow;
    QWidget* m_searchPanel = nullptr;
    QWidget* m_keyboard = nullptr;
    QLabel* m_breadcrumb;
    DeckLabStatusZone* m_status = nullptr; // r4-system: the rail status zone
    std::array<QPushButton*, 5> m_tabs{};
    std::array<DeckLabLevelMeter*, 4> m_meters{};
    std::array<QWidget*, 4> m_cardTransports{};
    // r4-decks: SYNC and MORE, the right column of each side pad block.
    std::array<QWidget*, 2> m_sideUtilities{};
    // r4-decks: pad blocks (decklabpads.h): one per side in two-deck view
    // (follows the layer), one per card's MORE in four-deck view.
    std::array<DeckLabPadBlock*, 2> m_sidePads{};
    std::array<DeckLabPadBlock*, 4> m_cardPads{};
    bool m_sideBySideWaveforms = false;
    double m_linkedZoom = 0;
    bool m_zoomPending = false;
    std::array<QWidget*, 4> m_overviews{};
    // usb-first: phrase data of tracks read from a rekordbox export, and the
    // phrase bands drawn along each overview. Empty for every other track.
    std::array<decklab::PhraseMapPointer, 4> m_phraseMaps{};
    std::array<QPointer<DeckLabPhraseBands>, 4> m_phraseBands{};
    std::array<qint64, 4> m_syncFeedbackUntil{};
    std::array<QPushButton*, 4> m_loadButtons{};
    DeckLabSampler* m_sampler = nullptr; // r4-sampler: the Sampler page
    std::array<QWidget*, 4> m_deckWaveforms{};
    // The waveform viewers (native GL windows) and the overlays hiding them.
    std::array<QPointer<QWidget>, 4> m_detailWaveforms{};
    QHash<QObject*, QMetaObject::Connection> m_waveformSuppressors;
    // r4-labfx: the whole Lab FX page and its state.
    DeckLabLabFx* m_labFx = nullptr;
    int m_page = -1;
    bool m_hasBrowsed = false;
    int m_sampleTarget = 0;
    int m_sampleReturnPage = 2;
    QString m_touchCue;
    QElapsedTimer m_clock;
    QTimer m_fxTimer;
    unsigned int m_toolSerial = 0;
    qint64 m_lastToolsReady = -10000;
    bool m_toolsOnline = false;
    bool m_outputConfigured = false;
    QList<QPushButton*> m_toolButtons;
    DeckLabSystemUi* m_systemUi = nullptr;
    // r4-system: rail status (decklabrail.h) and Settings (decklabsettings.h).
    DeckLabRail* m_railUi = nullptr;
    DeckLabSettingsPage* m_settings = nullptr;
    QTimer* m_recordTick = nullptr;
    qint64 m_recordingSince = -1;
    qint64 m_usbScannedAt = -1;
    int m_usbSticksSeen = 0;
    std::array<DeckLabTempoPresenter, 4> m_tempos{};
    std::array<int, 2> m_layers{1, 2};
    int m_autoDeckLayout = 2;
    bool m_fourDeckLayout = false;
    quint64 m_interactionEpoch = 0;
    double m_measuredFps = 0;
    int m_droppedFrames = 0;
    QLabel* m_performanceStatus = nullptr;
    std::unique_ptr<DeckLabBrowseState> m_browseState;
};
