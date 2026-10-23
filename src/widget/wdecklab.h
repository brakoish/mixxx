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
class DeckLabStemPads;
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
    void openSource();
    void openCollection();
    void releaseEffects(bool musical = false, bool mute = false);
    // Ends touch holds (hold effects, rolls, the XY pad) whose release went
    // astray; latched effects keep playing.
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
    QWidget* buildHotcues(int deck, bool followsLayer, QWidget* parent);
    void refreshHotcues(int deck, const QString& prefix);
    void showBeatgrid(int deck, bool followsLayer);
    void showTrackInfo(int actualDeck);
    void showControllerFxInfo();
    void refreshTransport(int deck, const QString& prefix);
    QWidget* buildBrowse();
    QWidget* buildSampler();
    QWidget* buildEffects();
    QWidget* buildSettings();
    void showSources();
    void showTracks();
    void loadDeck(int deck);
    void refreshDeck(int deck);
    void refreshLoadButtons();
    void refreshFx();
    void applyEffects();
    void tickEffects();
    void tool(int id);
    void setKeyNotation(int notation);
    void updateStatus();
    void updateBreadcrumb();
    void updateLayers();
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
    QString fxGroup() const;
    QString fxSlot() const;
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
        QPushButton* more = nullptr;
        TransportState shown;
    };
    struct HotcueState {
        int deck = -1;
        bool loaded = false;
        bool playing = false;
        std::array<qint64, 8> cues{};
        bool operator==(const HotcueState&) const = default;
    };
    struct HotcuePads {
        std::array<QPushButton*, 8> pads{};
        HotcueState shown;
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
    std::array<QString, 2> m_sideHotcuePrefixes;
    std::array<QString, 4> m_cardHotcuePrefixes;
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
    // Stem pads per deck card (stems workstream; decklabstempads.h).
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
    DeckLabChipButton* m_status;
    std::array<QPushButton*, 5> m_tabs{};
    std::array<DeckLabLevelMeter*, 4> m_meters{};
    std::array<QWidget*, 4> m_cardTransports{};
    std::array<QWidget*, 2> m_sideUtilities{};
    std::array<QWidget*, 2> m_sideHotcues{};
    QHash<QString, HotcuePads> m_hotcueButtons;
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
    std::array<QPushButton*, 16> m_samples{};
    std::array<QWidget*, 4> m_deckWaveforms{};
    // The waveform viewers (native GL windows) and the overlays hiding them.
    std::array<QPointer<QWidget>, 4> m_detailWaveforms{};
    QHash<QObject*, QMetaObject::Connection> m_waveformSuppressors;
    std::array<QPushButton*, 3> m_levers{};
    std::array<QSlider*, 3> m_isolates{};
    QComboBox* m_leverMode;
    QComboBox* m_isoMode;
    QComboBox* m_fxTime;
    QSlider* m_depth;
    QLabel* m_fxStatus;
    QLabel* m_fxMessage = nullptr;
    QWidget* m_fxBody;
    int m_page = -1;
    bool m_hasBrowsed = false;
    bool m_updating = false;
    int m_fxDeck = 1;
    int m_sampleTarget = 0;
    int m_sampleReturnPage = 2;
    int m_fxSample = 1;
    QString m_heldSamplerGroup;
    QString m_heldSamplerRoll;
    QLabel* m_sampleRollHint = nullptr;
    bool m_fxActive = false;
    bool m_fxReleasing = false;
    bool m_fxMute = false;
    bool m_fxReturning = false;
    qint64 m_releaseAt = 0;
    qint64 m_holdStarted = 0;
    // The control whose touch keeps the current hold alive (hold effect,
    // roll or XY pad), and its backstop in ms (0: none, the XY pad).
    QPointer<QWidget> m_holdSource;
    qint64 m_holdLimit = 0;
    QString m_heldRoll;
    QString m_touchCue;
    QElapsedTimer m_clock;
    QTimer m_fxTimer;
    qint64 m_lastFxTick = 0;
    unsigned int m_heartbeat = 0;
    unsigned int m_toolSerial = 0;
    qint64 m_lastToolsReady = -10000;
    bool m_toolsOnline = false;
    bool m_outputConfigured = false;
    QList<QPushButton*> m_toolButtons;
    DeckLabSystemUi* m_systemUi = nullptr;
    std::array<DeckLabTempoPresenter, 4> m_tempos{};
    std::array<int, 2> m_layers{1, 2};
    int m_autoDeckLayout = 2;
    bool m_fourDeckLayout = false;
    quint64 m_interactionEpoch = 0;
    double m_measuredFps = 0;
    int m_droppedFrames = 0;
    QLabel* m_performanceStatus = nullptr;
    bool m_holdFx = false;
    bool m_sampleRouting = false;
    std::unique_ptr<DeckLabBrowseState> m_browseState;
};
