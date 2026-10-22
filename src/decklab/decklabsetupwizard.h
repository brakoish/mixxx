// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QPointer>
#include <QWidget>

#include "decklab/decklabsetupmodel.h"
#include "decklab/decklabsystem.h"

class DeckLabSystemPage;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

namespace decklab::ui {
class Keyboard;
class Row;
} // namespace decklab::ui

/// Deck Setup: "plug in any Mixxx-supported controller and play".
/// Controller (detect, suggest and enable a mapping in-process), Audio
/// (route Main/Headphones in-process, with test tones), Layout (two or four
/// decks), Done. The first-run flow adds Welcome, Wi-Fi (optional) and time
/// zone in front and sets [DeckLab] first_run_done when finished. Drawn over
/// the whole surface.
class DeckLabSetupWizard : public QWidget {
    Q_OBJECT
  public:
    enum class Step {
        Welcome,
        Wifi,
        Time,
        Controller,
        Audio,
        Layout,
        Done,
    };
    Q_ENUM(Step)

    DeckLabSetupWizard(DeckLabSystem* system, QWidget* host);
    /// Deck Setup only (Controller → Audio → Layout → Done).
    void start(Step step = Step::Controller, const QString& preferredKey = QString());
    /// Welcome → Wi-Fi → Time zone → Controller → Audio → Layout → Done.
    void startFirstRun();
    bool isFirstRun() const {
        return m_firstRun;
    }
    Step step() const {
        return m_step;
    }
    /// The selected controller's chosen mapping (tests/captures).
    QString chosenMapping() const {
        return m_mapping;
    }
    decklab::AudioPlan chosenAudio() const {
        return m_audio;
    }
    int chosenDecks() const {
        return m_decks;
    }
    void showMappingPicker();
    /// "Skip – set up later": straight to Done without a controller.
    void skipToDone();

  signals:
    void finished(bool completed);
    void layoutChosen(int decks);
    void playRequested();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

  private:
    int pageIndex(Step step) const;
    QString stepName(Step step) const;
    void setSequence(const QList<Step>& sequence);
    void goTo(Step step);
    void next();
    void back();
    void finish(bool completed);
    void setMessage(const QString& text, bool error = false);
    void showControllerPage();
    void buildWelcome(QWidget* page);
    void buildController(QWidget* page);
    void buildPicker(QWidget* page);
    void buildAudio(QWidget* page);
    void buildLayout(QWidget* page);
    void buildDone(QWidget* page);
    void refreshControllers();
    void selectController(int index);
    void renderMapping();
    void renderPicker();
    void enterAudio();
    void fillAudioCombos();
    void audioChoiceChanged();
    void testTone(bool headphones, int side);
    void renderLayout();
    void renderDone();
    const DeckLabSystem::ControllerInfo* selected() const;

    QPointer<DeckLabSystem> m_system;
    bool m_firstRun = false;
    bool m_skippedController = false;
    QList<Step> m_sequence;
    Step m_step = Step::Controller;
    QString m_preferredKey;
    QList<DeckLabSystem::ControllerInfo> m_controllers;
    int m_selected = -1;
    QString m_mapping;
    QString m_mappingName;
    int m_mappingScore = 0;
    bool m_mappingApplied = false;
    decklab::AudioPlan m_audio;
    QList<decklab::AudioDevice> m_audioDevices;
    bool m_audioChosen = false;
    QString m_audioForKey;
    int m_decks = 2;
    int m_suggestedDecks = 2;

    QLabel* m_heading;
    QHBoxLayout* m_stepRow;
    QList<QLabel*> m_stepLabels;
    QStackedWidget* m_pages;
    QWidget* m_footer;
    QLabel* m_message;
    QPushButton* m_back;
    QPushButton* m_next;
    QPushButton* m_close;
    // First-run pages
    DeckLabSystemPage* m_embedded;
    // Controller step
    QStackedWidget* m_controllerStack;
    QWidget* m_controllerList;
    QVBoxLayout* m_controllerLayout;
    QWidget* m_controllerEmpty;
    QWidget* m_controllerMain;
    QLabel* m_mappingTitle;
    QLabel* m_mappingReason;
    QPushButton* m_change;
    QPushButton* m_detect;
    QPushButton* m_restart;
    QPushButton* m_skipLater;
    // Picker
    QLineEdit* m_search;
    QWidget* m_pickerList;
    QVBoxLayout* m_pickerLayout;
    decklab::ui::Keyboard* m_keyboard;
    // Audio step
    QLabel* m_audioReason;
    QComboBox* m_main;
    QComboBox* m_phones;
    QLabel* m_audioWarning;
    QList<QPushButton*> m_testButtons;
    // Layout step
    QPushButton* m_two;
    QPushButton* m_four;
    QLabel* m_layoutReason;
    // Done step
    QLabel* m_doneTitle;
    QWidget* m_summary;
    QVBoxLayout* m_summaryLayout;
};
