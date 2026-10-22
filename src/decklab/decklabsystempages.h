// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHash>
#include <QPointer>
#include <QWidget>
#include <functional>

class DeckLabSystem;
class QLabel;
class QStackedWidget;

namespace decklab::ui {
class Tile;
}

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
    DeckLabSystem* system() const {
        return m_system;
    }
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

  signals:
    void sectionChanged(Section section);
    void panelOpened(bool open);

  protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    void refreshTiles();

    QPointer<DeckLabSystem> m_system;
    QStackedWidget* m_stack;
    QLabel* m_serviceState;
    QHash<int, QWidget*> m_pages;
    QHash<int, decklab::ui::Tile*> m_tiles;
    Section m_current = Section::Home;
    bool m_embedded = false;
    std::function<bool(int)> m_legacyTool;
};
