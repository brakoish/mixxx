// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDateTime>
#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <functional>

class DeckLabSystem;
class QLabel;
class QStackedWidget;

namespace decklab::ui {
class HoldButton;
}
class DeckLabLiveTile;

/// Settings → System: touch pages for Wi-Fi, Bluetooth, screen brightness,
/// date & time, storage, power, about and maintenance. Each page is one tap
/// from the System home tiles, so two taps from Settings.
class DeckLabSystemPage : public QWidget {
    Q_OBJECT
  public:
    enum class Section {
        Home,
        Wifi,
        Bluetooth,
        Screen,
        Time,
        Storage,
        Power,
        About,
        Maintenance,
    };
    Q_ENUM(Section)

    explicit DeckLabSystemPage(QWidget* parent);
    void setSystem(DeckLabSystem* system);
    DeckLabSystem* system() const;
    void showSection(Section section);
    Section currentSection() const {
        return m_current;
    }
    /// Old companion tools (6 = Wi-Fi, 9 = desktop) while decklabd is offline.
    void setLegacyTool(std::function<bool(int)> tool) {
        m_legacyTool = std::move(tool);
    }
    bool legacyTool(int id) const {
        return m_legacyTool && m_legacyTool(id);
    }
    static QString sectionName(Section section);
    /// Inside the first-run flow: no "‹ System" back buttons.
    void setEmbedded(bool embedded);
    bool isEmbedded() const {
        return m_embedded;
    }
    /// Called by sections when a full-height panel (password keyboard, time
    /// zone picker) opens or closes.
    void notifyPanel(bool open) {
        emit panelOpened(open);
    }
    /// r4-system: the home tile that opens `section` (knob focus on back).
    QWidget* tile(Section section) const;
    /// Restart app / Turn off from a tile or the Power page; refused while
    /// anything plays. Returns the refusal, empty when it started.
    QString power(const QString& action);
    /// "Stop all decks and samplers first." while anything plays.
    QString playingRefusal() const;

  signals:
    void sectionChanged(Section section);
    void panelOpened(bool open);
    /// r4-system: back from the home (to the Settings category list).
    void backRequested();

  protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    void refreshTiles();
    void refreshClock();
    void setBrightness(int value);

    QPointer<DeckLabSystem> m_system;
    QStackedWidget* m_stack;
    QLabel* m_serviceState;
    QHash<int, QWidget*> m_pages;
    // r4-system (Signature 3): the back panel of the unit, live tiles.
    QHash<int, DeckLabLiveTile*> m_tiles;
    QList<decklab::ui::HoldButton*> m_powerHolds;
    QTimer m_clockTimer;
    QTimer m_brightnessSend;
    QString m_zone;
    QDateTime m_now;
    qint64 m_nowAt = 0;
    int m_brightness = -1;
    // round4: a tap not yet confirmed by the service wins over a refresh.
    bool m_brightnessPending = false;
    quint32 m_brightnessEdit = 0;
    Section m_current = Section::Home;
    bool m_embedded = false;
    std::function<bool(int)> m_legacyTool;
};
