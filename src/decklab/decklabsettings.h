// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QPointer>
#include <QWidget>
#include <array>
#include <functional>

#include "decklab/decklabsystempages.h"
#include "preferences/usersettings.h"

class DeckLabSystemUi;
class QPushButton;
class QStackedWidget;

namespace decklab::ui {
class ActionRow;
class CategoryButton;
class ChoiceRow;
class InfoRow;
class KnobNavigator;
class StepperRow;
class SwitchRow;
} // namespace decklab::ui

/// r4-system (Signature 1): Settings as a utility list you drive with the
/// browse knob (Pioneer UTILITY). A category list with live values on the
/// left; rows with the value printed on the right; Unit (the System home and
/// pages) takes the full width with a breadcrumb header. Turn moves the focus
/// ring, press edits, turn changes, press sets, back goes up one level and,
/// from the category list, back to Decks. The surface hands in its controls
/// and actions through Hooks; no desktop combo boxes are shown (the old ones
/// stay as hidden models so existing callers keep working).
class DeckLabSettingsPage : public QWidget {
    Q_OBJECT
  public:
    enum Category {
        ControllerAudio = 0,
        Decks = 1,
        Headphones = 2,
        Display = 3,
        Library = 4,
        Unit = 5,
    };
    struct Hooks {
        std::function<double(const QString&, const QString&)> value;
        std::function<void(const QString&, const QString&, double)> set;
        std::function<void(const QString&, const QString&, std::function<void(double)>)> watch;
        std::function<void(int)> deckLayoutChosen; // [DeckLab] DeckLayout: 0 auto, 2, 4
        std::function<int()> autoDecks;            // what Auto shows now
        std::function<bool()> fourDecksShown;
        std::function<void(bool)> waveformLayoutChosen; // true: side by side
        std::function<bool()> sideBySide;
        std::function<int()> screenSleepMinutes; // [DeckLab] ScreenSleepMinutes
        std::function<void(int)> screenSleepChosen;
        std::function<void(int)> keyNotationChosen; // decklab::KeyFormat: 0 Lancelot, 1 Open Key, 2 Musical
        std::function<void(bool)> dimPlayedChosen;
        std::function<void(int)> tool;   // 2 Get music, 3 Sample Maker, 10 Storage
        std::function<void()> advanced; // the desktop preferences
        std::function<void()> leave;    // back from the category list
    };

    DeckLabSettingsPage(UserSettingsPointer config, DeckLabSystemUi* systemUi, Hooks hooks, QWidget* parent);
    /// Launchers that need the system service (enabled by the surface).
    QList<QPushButton*> toolButtons() const {
        return m_tools;
    }
    void setHealth(double fps, int dropped, int overloads);
    void showCategory(int category);
    int category() const {
        return m_category;
    }
    /// Unit, full width, at a System page.
    void showUnit(DeckLabSystemPage::Section section);
    DeckLabSystemPage* systemPage() const {
        return m_system;
    }
    decklab::ui::KnobNavigator* knob() const {
        return m_knob;
    }
    /// 0: the category list has the knob; 1: the open category does.
    int knobLevel() const {
        return m_level;
    }
    /// The deck the per-deck rows act on.
    int selectedDeck() const {
        return m_deck;
    }
    void refreshValues();

  protected:
    void showEvent(QShowEvent* event) override;

  private:
    QWidget* page(int category, const QString& heading, QWidget** content);
    void buildControllerAudio(QWidget* content);
    void buildDecks(QWidget* content);
    void buildHeadphones(QWidget* content);
    void buildDisplay(QWidget* content);
    void buildLibrary(QWidget* content);
    void selectDeck(int deck);
    void refreshDeckRows();
    void refreshLayoutRows();
    bool knobBack();
    void setLevel(int level);
    QString channel(int deck) const;

    UserSettingsPointer m_config;
    QPointer<DeckLabSystemUi> m_systemUi;
    Hooks m_hooks;
    QWidget* m_categories = nullptr;
    QStackedWidget* m_pages = nullptr;
    QList<decklab::ui::CategoryButton*> m_links;
    DeckLabSystemPage* m_system = nullptr;
    decklab::ui::KnobNavigator* m_knob = nullptr;
    int m_category = Decks;
    int m_lastCategory = Decks;
    int m_level = 0;
    int m_deck = 1;
    // Decks
    decklab::ui::ChoiceRow* m_layout = nullptr;
    decklab::ui::ChoiceRow* m_deckChoice = nullptr;
    decklab::ui::ChoiceRow* m_playStart = nullptr; // r6-playstart: [DeckLab] PlayStartAlign
    decklab::ui::SwitchRow* m_quantize = nullptr;
    decklab::ui::SwitchRow* m_keylock = nullptr;
    decklab::ui::SwitchRow* m_slip = nullptr;
    std::array<decklab::ui::ChoiceRow*, 4> m_ranges{};
    // Headphones
    std::array<decklab::ui::SwitchRow*, 4> m_cues{};
    decklab::ui::SwitchRow* m_split = nullptr;
    decklab::ui::StepperRow* m_mix = nullptr;
    // Display
    decklab::ui::ChoiceRow* m_waves = nullptr;
    // Library (r6-keys: the key format moved here, where keys are browsed)
    decklab::ui::ChoiceRow* m_keys = nullptr;
    // Controller & audio
    decklab::ui::InfoRow* m_health = nullptr;
    QList<QPushButton*> m_tools;
};
