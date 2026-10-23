// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QAbstractButton>
#include <QColor>
#include <QElapsedTimer>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <array>
#include <functional>

#include "preferences/usersettings.h"
#include "widget/decklabstemgestures.h"

class ControlProxy;
class PlayerManagerInterface;
class BaseTrackPlayer;

// Touch stem control for one deck (stems workstream).
//
// Engine state is Mixxx's own: [ChannelN],stem_count, and per stem
// [ChannelN_StemM],mute and [ChannelN_StemM],color. Names come from the
// loaded track's stem manifest. Nothing here keeps a second copy of any of
// it; pads repaint from the controls. Gesture rules: decklabstemgestures.h.

// What one pad shows.
struct DeckLabStemPadState {
    QString name;
    QColor color;
    bool muted = false;
    bool solo = false;    // held solo source
    bool pressed = false; // pointer down; becomes a solo if held
    bool operator==(const DeckLabStemPadState&) const = default;
};

class DeckLabStemPads;

// One painted stem pad. The pad stays dark; the stem's colour is a bar at its
// left edge (like a hot cue's badge), so a red stem never reads as live red.
// Muted: dim fill, hollow colour bar, muted name with a strike line.
class DeckLabStemPad final : public QAbstractButton {
    Q_OBJECT
  public:
    DeckLabStemPad(DeckLabStemPads* owner, int stem, QWidget* parent);
    void setState(const DeckLabStemPadState& state);
    const DeckLabStemPadState& state() const {
        return m_state;
    }
    int stem() const {
        return m_stem;
    }
    // The owner is going away: stop forwarding gestures to it.
    void detach() {
        m_owner = nullptr;
    }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    DeckLabStemPads* m_owner;
    int m_stem;
    DeckLabStemPadState m_state;
};

// The stem pads of one deck.
class DeckLabStemPads final : public QWidget {
    Q_OBJECT
  public:
    enum class Presentation {
        Row,  // a 48 px row in a two-deck card (4 px gap above)
        Menu, // a 44 px row inside the four-deck More menu
    };
    static constexpr int kRowHeight = 48;
    static constexpr int kPadHeight = 44;

    DeckLabStemPads(int deck,
            Presentation presentation,
            PlayerManagerInterface* players,
            std::function<quint64()> epoch,
            QWidget* parent = nullptr);
    ~DeckLabStemPads() override;

    int deck() const {
        return m_deck;
    }
    // A stem track is loaded and plays as stems.
    bool present() const {
        return m_present;
    }
    int stemCount() const {
        return m_gestures.count();
    }
    // Row presentation: the deck row reserves the stem row (some shown deck
    // has stems). Without stems on this deck the row stays empty.
    void setRowShown(bool shown);
    DeckLabStemPad* pad(int stem) const {
        return stem >= 0 && stem < DeckLabStemGestures::kStems ? m_pads[stem] : nullptr;
    }
    const DeckLabStemGestures& gestures() const {
        return m_gestures;
    }

    // Two-deck view needs the stem row when a displayed deck has stems.
    static bool rowWanted(const std::array<DeckLabStemPads*, 4>& pads,
            const std::array<int, 2>& displayedDecks);
    // Settings > Deck > Stem tracks: play as stems or as a normal track.
    // Applies to the next load. nullptr in builds without stem support.
    static QWidget* createPlaybackSetting(UserSettingsPointer config, QWidget* parent);

  signals:
    void presenceChanged(bool present);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    friend class DeckLabStemPad;
    void padPressed(int stem);
    void padReleased(int stem);
    void padHidden(int stem);
    void holdElapsed();
    void cancelGesture(bool trackReplaced);
    void trackChanging(bool replaced);
    void refresh();
    void refreshPads();
    void watchGestureEnd();
    DeckLabStemGestures::Context context() const;
    bool muted(int stem) const;
    void setMuted(int stem, bool muted);
    BaseTrackPlayer* player() const;
    bool playsAsStems() const;

    int m_deck;
    Presentation m_presentation;
    PlayerManagerInterface* m_players;
    std::function<quint64()> m_epoch;
    QString m_group;
    std::array<QString, DeckLabStemGestures::kStems> m_stemGroups;
    ControlProxy* m_count = nullptr;
    ControlProxy* m_loaded = nullptr;
    std::array<ControlProxy*, DeckLabStemGestures::kStems> m_mute{};
    std::array<ControlProxy*, DeckLabStemGestures::kStems> m_color{};
    std::array<DeckLabStemPad*, DeckLabStemGestures::kStems> m_pads{};
    std::array<QString, DeckLabStemGestures::kStems> m_names;
    DeckLabStemGestures m_gestures;
    QTimer m_holdTimer;
    QElapsedTimer m_clock;
    quint64 m_loadSerial = 0;
    bool m_present = false;
    bool m_rowShown = false;
    bool m_filtering = false;
};
