// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QTimer>
#include <array>
#include <memory>
#include <vector>
#include <functional>

#include "preferences/usersettings.h"
#include "widget/wbasewidget.h"
#include "widget/decklabtempopresenter.h"
#include <QWidget>

class ControlObject;
class ControlProxy;
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
    void setAudioOutputConfigured(bool configured);

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
    int displayedDeck(int side) const;
    void setDisplayedDeck(int side, int deck);
    QString fxGroup() const;
    QString fxSlot() const;
    QString channel(int deck) const;

    UserSettingsPointer m_config;
    Library* m_library;
    PlayerManagerInterface* m_players;
    EffectsManager* m_effects;
    std::vector<std::unique_ptr<ControlObject>> m_ownedControls;
    QHash<QString, ControlProxy*> m_controls;
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
    QPushButton* m_status;
    std::array<QPushButton*, 5> m_tabs{};
    std::array<QLabel*, 4> m_titles{};
    std::array<QLabel*, 4> m_waveNumbers{};
    std::array<QProgressBar*, 4> m_meters{};
    std::array<QLabel*, 4> m_numbers{};
    std::array<QLabel*, 4> m_keys{};
    std::array<QLabel*, 4> m_remaining{};
    std::array<QLabel*, 4> m_pitch{};
    std::array<QLabel*, 4> m_playStates{};
    std::array<QWidget*, 4> m_waveHeaders{};
    std::array<QLabel*, 4> m_infoAffordances{};
    std::array<QWidget*, 4> m_deckMetrics{};
    std::array<QWidget*, 4> m_deckSecondary{};
    std::array<QLabel*, 4> m_modes{};
    std::array<QWidget*, 4> m_cardTransports{};
    std::array<QWidget*, 4> m_waveBodies{};
    std::array<QWidget*, 4> m_detailColumns{};
    std::array<QWidget*, 2> m_sideUtilities{};
    std::array<QWidget*, 2> m_sideHotcues{};
    QHash<QString, std::array<QPushButton*, 8>> m_hotcueButtons;
    QWidget* m_pairPerformance = nullptr;
    QWidget* m_pairContexts = nullptr;
    QWidget* m_pairDetails = nullptr;
    std::array<QWidget*, 4> m_separateWaves{};
    bool m_sideBySideWaveforms = false;
    QLabel* m_pairEffects = nullptr;
    QLabel* m_pairRMX = nullptr;
    QLabel* m_pairCue = nullptr;
    QLabel* m_masterLevelLabel = nullptr;
    std::array<QProgressBar*, 2> m_masterMeters{};
    QLabel* m_pairZoom = nullptr;
    double m_linkedZoom = 0;
    bool m_zoomPending = false;
    std::array<QLabel*, 4> m_waveContexts{};
    std::array<QLabel*, 4> m_waveIdentities{};
    std::array<QWidget*, 4> m_contextPanels{};
    std::array<QWidget*, 4> m_meterPanels{};
    std::array<QLabel*, 4> m_meterLabels{};
    std::array<QLabel*, 4> m_sources{};
    std::array<QString, 4> m_sourceLocations{};
    std::array<QString, 4> m_sourceNames{};
    std::array<QWidget*, 4> m_overviews{};
    std::array<qint64, 4> m_syncFeedbackUntil{};
    std::array<QPushButton*, 4> m_loadButtons{};
    std::array<QPushButton*, 16> m_samples{};
    std::array<QWidget*, 4> m_deckWaveforms{};
    std::array<QPushButton*, 3> m_levers{};
    std::array<QSlider*, 3> m_isolates{};
    QComboBox* m_leverMode;
    QComboBox* m_isoMode;
    QComboBox* m_fxTime;
    QSlider* m_depth;
    QLabel* m_fxStatus;
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
};
