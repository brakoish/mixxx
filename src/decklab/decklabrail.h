// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QPointer>
#include <QPushButton>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <functional>

#include "decklab/decklabui.h"

// r4-system (Signature 2 + performance Move 5): the rail is a status bar that
// teaches. Tabs have fixed widths; a fixed status zone on the right holds a
// lamp per subsystem in signal-flow order (USB and controller in, network,
// recording, output), so nothing ever reflows mid-mix. Lamps look like
// lamps; red is only for live things (recording, no sound output). Notices
// slide into the rail for 4 s and collapse into their lamp; alerts hold the
// zone until fixed, and the whole zone is the FIX button. Nothing is drawn
// over the playhead, the Browse header or the waveforms: the rail is the only
// place system news appears. Tapping the zone opens the status sheet, one
// actionable line per subsystem.
namespace decklab::rail {

/// Everything the rail shows, gathered by the surface (audio, recording,
/// dropouts) and the system UI (service, USB, network, controller, heat).
struct Inputs {
    bool outputWorking = false;
    bool outputPending = false; // start-up: devices not opened yet, no alert
    bool recentDropouts = false;
    int dropouts = 0;
    bool recording = false;
    qint64 recordingSeconds = 0;
    bool serviceAttached = false; // a Deck Lab system service client exists
    bool serviceOnline = false;
    int usbDrives = 0;
    bool usbRekordbox = false;
    bool usbRecordTarget = false; // a writable drive is the recording target
    QString usbSummary;           // "DJ USB (read-only) · MIX SETS"
    QString recordTarget;         // label of the recording drive
    double recordHoursFree = -1;
    enum class Wifi {
        Unknown,
        Off,
        Disconnected,
        Connected,
    };
    Wifi wifi = Wifi::Unknown;
    QString ssid;
    int signal = -1;
    enum class Controller {
        None,
        Found,   // on USB, not working yet
        Working, // open with a mapping
    };
    Controller controller = Controller::None;
    QString controllerName;
    bool controllerNew = false; // an unknown controller waits for Deck Setup
    bool setupPending = false;  // r5-polish: first-run setup not finished
    QString soundSummary;       // "DDJ-FLX4 1-2 · phones 3-4"
    double temperature = -1;
    QString heat; // "", "warm" or "hot"
    bool undervoltage = false;
    bool operator==(const Inputs&) const = default;
};

enum class Slot {
    Controller,
    Network,
    Recording,
    Output,
};

struct SlotView {
    decklab::ui::Lamp lamp = decklab::ui::Lamp::Off;
    QString label;
    bool operator==(const SlotView&) const = default;
};

/// The lamp and label a slot shows for these inputs (pure, for tests).
SlotView slotView(Slot slot, const Inputs& inputs);
/// USB slot: lamp and label ("USB", "USB 2", "USB RB").
SlotView usbView(const Inputs& inputs);

enum class Alert {
    None,
    NoOutput, // live red: the room hears nothing
    LowPower, // caution: dropouts and SD card damage risk
};
Alert alertFor(const Inputs& inputs);
/// "12:34", or "1h02" from an hour on: fits the 44 px slot.
QString recordingTime(qint64 seconds);

struct Notice {
    QString text;
    decklab::ui::Lamp lamp = decklab::ui::Lamp::On;
    std::function<void()> action; // the whole notice is the button
    int milliseconds = 4000;
};

} // namespace decklab::rail

/// A page tab with a fixed width, its lamp (a playing Sampler, a wet Lab FX)
/// and, for Settings, a gear instead of the word (the CDJ's UTILITY button).
class DeckLabRailTab : public QPushButton {
    Q_OBJECT
  public:
    DeckLabRailTab(const QString& text, bool gear, QWidget* parent, const QString& objectName);
    /// Lamp dot at the top right; Off hides it.
    void setLamp(decklab::ui::Lamp lamp);
    decklab::ui::Lamp lamp() const {
        return m_lamp;
    }

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    bool m_gear;
    decklab::ui::Lamp m_lamp = decklab::ui::Lamp::Off;
};

/// The USB slot. A tap opens the stick in Browse; a long press opens the
/// status sheet (storage, eject). Shows drive presence, a rekordbox export
/// and the recording target.
class DeckLabUsbLamp : public QPushButton {
    Q_OBJECT
  public:
    explicit DeckLabUsbLamp(QWidget* parent);
    void setInputs(const decklab::rail::Inputs& inputs);
    decklab::rail::SlotView view() const {
        return m_view;
    }

  signals:
    void longPressed();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

  private:
    QTimer m_long;
    decklab::rail::SlotView m_view;
    bool m_recordTarget = false;
    bool m_recording = false;
    bool m_longFired = false;
};

/// The fixed status zone: CTRL, NET, REC and OUT lamps, or an alert that
/// holds the zone (NO OUTPUT · FIX, LOW POWER). Never changes width.
class DeckLabStatusZone : public QPushButton {
    Q_OBJECT
  public:
    static constexpr int kSlotWidth = 44;
    explicit DeckLabStatusZone(QWidget* parent);
    void setInputs(const decklab::rail::Inputs& inputs);
    QList<decklab::rail::SlotView> slotViews() const {
        return m_slots;
    }
    decklab::rail::Alert alert() const {
        return m_alert;
    }
    QString alertText() const;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QList<decklab::rail::SlotView> m_slots;
    decklab::rail::Alert m_alert = decklab::rail::Alert::None;
    bool m_recording = false;
};

/// A notice sliding into the rail (over the controller FX lamps, USB and the
/// zone, never over a page) for its time, then collapsing into its lamp.
class DeckLabRailNotice : public QPushButton {
    Q_OBJECT
  public:
    explicit DeckLabRailNotice(QWidget* parent);
    void showNotice(const decklab::rail::Notice& notice);
    QString noticeText() const {
        return m_current.text;
    }
    bool active() const {
        return !m_current.text.isEmpty();
    }
    /// Runs the notice's action (a tap); false when it has none.
    bool trigger();
    void dismiss();

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    void next();
    void step();

    QList<decklab::rail::Notice> m_queue;
    decklab::rail::Notice m_current;
    QTimer m_animation;
    QTimer m_hold;
    int m_phase = 0; // 0-5 sliding in, 5 shown, 6-9 collapsing
};

/// The status sheet: one line per subsystem with its lamp, what it is doing
/// and one action (Deck Setup, hold to record, Storage, Wi-Fi). It covers the
/// page below the rail, hides the GL waveforms while shown and is driven by
/// the knob; back or the close glyph dismisses it.
class DeckLabStatusSheet : public QWidget {
    Q_OBJECT
  public:
    struct Actions {
        std::function<void(int step)> deckSetup; // 0 controller, 1 sound
        std::function<void(int section)> system; // DeckLabSystemPage::Section
        std::function<void()> toggleRecording;
        std::function<void()> browseUsb;
        std::function<void()> finishSetup; // r5-polish: the first-run flow
    };
    DeckLabStatusSheet(QWidget* surface, Actions actions);
    void refresh(const decklab::rail::Inputs& inputs);
    /// The line texts, for tests and accessibility.
    QStringList lines() const;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

  private:
    struct Line;
    Line* addLine(const QString& key, const QString& name);
    void place();

    Actions m_actions;
    QWidget* m_panel = nullptr;
    QList<Line*> m_lines;
    QPushButton* m_soundAction = nullptr;
    decklab::ui::HoldButton* m_record = nullptr;
    QPushButton* m_storage = nullptr;
    QPushButton* m_wifi = nullptr;
    QPushButton* m_controller = nullptr;
    QWidget* m_setupLine = nullptr;
    decklab::ui::KnobNavigator* m_knob = nullptr;
};

/// Owns the rail's status widgets and routes taps and notices.
class DeckLabRail : public QObject {
    Q_OBJECT
  public:
    struct Hooks {
        std::function<void()> browseUsb;          // the stick in Browse
        std::function<void(int step)> deckSetup;  // 0 controller, 1 sound
        std::function<void(int section)> system;  // a System page
        std::function<void()> toggleRecording;    // [Recording] toggle_recording
        std::function<void()> finishSetup;        // r5-polish: the first-run flow
    };
    DeckLabRail(QWidget* surface, QWidget* rail, Hooks hooks);
    DeckLabUsbLamp* usb() const {
        return m_usb;
    }
    DeckLabStatusZone* zone() const {
        return m_zone;
    }
    DeckLabRailNotice* noticeStrip() const {
        return m_notice;
    }
    /// The area notices slide over: from `from`'s left edge to the zone's right.
    void setNoticeSpan(QWidget* from);
    void setInputs(const decklab::rail::Inputs& inputs);
    const decklab::rail::Inputs& inputs() const {
        return m_inputs;
    }
    void notify(const decklab::rail::Notice& notice);
    DeckLabStatusSheet* openSheet();
    DeckLabStatusSheet* sheet() const {
        return m_sheet;
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void placeNotice();
    void zoneTapped();

    QPointer<QWidget> m_surface;
    QPointer<QWidget> m_rail;
    Hooks m_hooks;
    DeckLabUsbLamp* m_usb;
    DeckLabStatusZone* m_zone;
    DeckLabRailNotice* m_notice;
    QPointer<QWidget> m_noticeFrom;
    QPointer<DeckLabStatusSheet> m_sheet;
    decklab::rail::Inputs m_inputs;
};
