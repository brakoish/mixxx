// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <functional>

#include "decklab/decklabrail.h"
#include "decklab/decklabsystempages.h"

class DeckLabSetupWizard;
class DeckLabSystem;
class QPushButton;
class QWidget;

namespace decklab::ui {
class ActionRow;
}

/// Glue between the native surface (WDeckLab) and Deck Lab system
/// integration: the Settings → Unit and Controller & audio pages, native
/// tool routing and the Deck Setup wizard. r4-system: system news goes to
/// the rail (notices and lamps), never over the page: no banners.
class DeckLabSystemUi : public QObject {
    Q_OBJECT
  public:
    struct Hooks {
        std::function<void(DeckLabSystemPage::Section)> showSystem;
        std::function<void()> showDecks;
        std::function<void(int)> applyLayout;
        std::function<bool(int)> legacyTool;
        std::function<void()> beforeTool;
        /// r4-system: a notice for the rail (slides in, collapses to its lamp).
        std::function<void(const decklab::rail::Notice&)> notify;
        /// r4-system: something the rail shows changed (service, USB, Wi-Fi,
        /// controllers, heat, power).
        std::function<void()> statusChanged;
    };

    DeckLabSystemUi(QWidget* surface, Hooks hooks);
    ~DeckLabSystemUi() override;

    DeckLabSystemPage* createSystemPage(QWidget* parent);
    QWidget* createSetupPanel(QWidget* parent);
    void attach(DeckLabSystem* system);
    DeckLabSystem* system() const;
    /// decklabd connected (native tools available).
    bool toolsConnected() const;
    /// Routes a tool id natively. False means "use the legacy companion".
    bool handleTool(int id);
    DeckLabSetupWizard* openWizard(int step = 0, const QString& controllerKey = QString());
    /// Welcome → Wi-Fi → time zone → Deck Setup; sets first_run_done.
    DeckLabSetupWizard* openFirstRun();
    /// r5-polish: setup not finished yet (Settings and the status sheet
    /// offer Finish setup until it is).
    bool setupPending() const;
    DeckLabSetupWizard* wizard() const;
    void setAudioOutputConfigured(bool configured);
    /// Overlay and rail check for a real display (DECKLAB_OVERLAY_PROBE=<s>
    /// runs it <s> seconds after attach, then opens the first-run flow 30 s
    /// later): a rail notice and a tappable controller offer. Changes no
    /// setting.
    void showOverlayProbe();
    /// r4-system: the system half of the rail's inputs (service, USB, Wi-Fi,
    /// controller, heat, power, sound summary). Cached; cheap to call.
    void fillRailInputs(decklab::rail::Inputs& inputs) const;
    /// Controller & audio at a glance: READY, NO SOUND or SET UP.
    void readiness(QString* value, decklab::ui::Lamp* lamp) const;
    /// The unknown controller offered for Deck Setup, if any.
    QString offeredController() const {
        return m_offerKey;
    }

  signals:
    void toolsConnectedChanged(bool connected);

  private:
    void updateSetupPanel();
    void refreshRail();
    void showToast(const QString& text, int milliseconds = 4000);
    void offerController(const QString& key, const QString& name);
    void launchApp(const QString& app, const QString& name);
    void notify(const QString& text, decklab::ui::Lamp lamp, std::function<void()> action = {}, int milliseconds = 4000);

    QPointer<QWidget> m_surface;
    Hooks m_hooks;
    QPointer<DeckLabSystem> m_system;
    QPointer<DeckLabSystemPage> m_systemPage;
    QPointer<DeckLabSetupWizard> m_wizard;
    QPointer<decklab::ui::ActionRow> m_setupButton;
    QPointer<decklab::ui::ActionRow> m_finishSetup;
    QPointer<decklab::ui::ActionRow> m_setupController;
    QPointer<decklab::ui::ActionRow> m_setupAudio;
    QTimer m_offerTimer;
    QString m_offerKey;
    QString m_offerName;
    QJsonObject m_wifi;
    // The system half of the rail, recomputed only when decklabd says so.
    decklab::rail::Inputs m_rail;
    QString m_lastHeat;
    bool m_outputConfigured = false;
};
