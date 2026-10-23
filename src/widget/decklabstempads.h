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

// One painted stem pad. The stem's colour is only a 3 px underline (r4-decks,
// audit PF-D58: never a fill or a side bar), so a red stem never reads as
// live red and a cyan one never as deck 1. Playing: raised fill, white name.
// Muted: the darkest fill, a dimmed underline, a muted name with a strike.
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
    // r4-decks: both presentations sit on the top row of a pad block
    // (decklabpads.h) in STEM mode, which decides when they show.
    enum class Presentation {
        Row,  // the two-deck side pad block ("decklab-stems-N")
        Menu, // the pad block in the four-deck MORE sheet ("decklab-card-stems-N")
    };
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
    // The host pad block shows these pads (STEM mode on this deck). They are
    // visible only while shown and a stem track is present.
    void setRowShown(bool shown);
    // r4-decks: ALL brings every stem back at once (a held solo ends first).
    void restoreAll();
    // r4-decks: SOLO latch. While on, a touch plays only that stem (the
    // others mute) and stays so; it never toggles and never holds.
    void setSoloLatch(bool on);
    bool soloLatch() const {
        return m_soloLatch;
    }
    bool anyMuted() const;
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
    // Some stem's mute changed (touch, controller or a load reset).
    void mutesChanged();

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
    bool m_soloLatch = false;
    std::array<bool, DeckLabStemGestures::kStems> m_shownMutes{};
};
