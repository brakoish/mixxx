// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <functional>

#include "decklab/decklabsystempages.h"

class ControlProxy;
class DeckLabSetupWizard;
class DeckLabSystem;
class QLabel;
class QPushButton;
class QWidget;

/// Glue between the native surface (WDeckLab) and Deck Lab system
/// integration: the Settings → System and Setup pages, native tool routing,
/// the Deck Setup wizard and the touch-through status banners.
class DeckLabSystemUi : public QObject {
    Q_OBJECT
  public:
    struct Hooks {
        std::function<void(DeckLabSystemPage::Section)> showSystem;
        std::function<void()> showDecks;
        std::function<void(int)> applyLayout;
        std::function<bool(int)> legacyTool;
        std::function<void()> beforeTool;
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
    DeckLabSetupWizard* wizard() const;
    void setAudioOutputConfigured(bool configured);

  signals:
    void toolsConnectedChanged(bool connected);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void layoutBanners();
    void updateAudioBanner();
    void updateSetupPanel();
    void showToast(const QString& text, int milliseconds = 5000);
    void offerController(const QString& key, const QString& name);
    void launchApp(const QString& app, const QString& name);

    QPointer<QWidget> m_surface;
    Hooks m_hooks;
    QPointer<DeckLabSystem> m_system;
    QPointer<DeckLabSystemPage> m_systemPage;
    QPointer<DeckLabSetupWizard> m_wizard;
    QPointer<QPushButton> m_setupButton;
    QPointer<QLabel> m_setupController;
    QPointer<QLabel> m_setupAudio;
    QPushButton* m_audioBanner;
    QPushButton* m_offerBanner;
    QPushButton* m_thermalBanner;
    QPushButton* m_toast;
    ControlProxy* m_tab;
    ControlProxy* m_soundStatus;
    QTimer m_toastTimer;
    QTimer m_offerTimer;
    QTimer m_audioDebounce;
    QString m_offerKey;
    bool m_outputConfigured = false;
};
