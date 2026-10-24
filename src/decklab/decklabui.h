// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QFont>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QString>
#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>
#include <functional>

class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QScrollArea;
class QVBoxLayout;
class QHBoxLayout;

// Touch-sized building blocks matching the Deck Lab surface: blue-black
// panels, Barlow type, white selection and 44 px+ targets.
namespace decklab::ui {

enum class Icon {
    Wifi,
    Bluetooth,
    Screen,
    Clock,
    Usb,
    Power,
    Info,
    Wrench,
    Controller,
    Speaker,
    Headphones,
    Check,
    Warning,
    Lock,
    Decks,
    Restart,
};

// Brand tokens. Every colour the system kit paints comes from this list.
extern const char* const kGround; // "#0d0f12": page background
extern const char* const kPanel;  // "#16191e": rows, cards, keys
extern const char* const kRaise;  // "#20242b": pressed and selected fills
extern const char* const kLine;   // "#2c313a": borders and separators
extern const char* const kDim;    // "#6b7380": disabled text (16 px and up)
extern const char* const kMuted;  // "#9aa3af": secondary text (15 px and up)
extern const char* const kText;   // "#f2f4f7": text, focus, primary fill
extern const char* const kLive;   // "#ff3d2e": recording, clip, no output only
extern const char* const kSelect; // "#f2f4f7": selection and focus (white, per brand)
extern const char* const kGo;     // "#2fd27a": working, connected, playing lamps (one green)

// r4-system: lamps, glyphs and type roles shared by the rail, Settings, the
// System pages and Deck Setup. Lamps look like lamps (an 8 px dot), never
// like buttons. Red is only for live things (recording, no sound output).
enum class Lamp {
    Off,         // hollow line ring: nothing there
    On,          // white: on, set, armed
    Go,          // green: working, connected, playing
    Caution,     // hatch with a white ring: look at me (no hue: amber is deck 2)
    Live,        // filled live red: recording
    LiveOutline, // live red ring: the room hears nothing (no sound output)
};
void paintLamp(QPainter& painter, const QPointF& center, double radius, Lamp lamp);
/// 45 degree 2 px stripes of text at 25 % on raise: caution without a hue.
void paintCautionHatch(QPainter& painter, const QRectF& rect, double radius = 0);

enum class Glyph {
    Back,
    Chevron,
    Gear,
    Check,
    Warning,
    Minus,
    Plus,
    Close,
    Lock,
};
/// One drawn glyph set (2 px stroke at 24, round caps) instead of font glyphs.
void paintGlyph(QPainter& painter, const QRectF& rect, Glyph glyph, const QColor& color);
/// Barlow Semi Condensed SemiBold with tabular figures: readouts, hero values.
QFont numberFont(const QFont& base, int pixels);
/// 14 px SemiBold caps with +0.06 em: labels, chips, lamps, step names.
QFont capsFont(const QFont& base, int pixels = 14);

/// The UI face (Barlow) at an on-device size. Painted widgets use this so
/// they never fall back to the system font.
QFont uiFont(const QFont& base, int pixels, QFont::Weight weight);

/// Draws a simple line icon in `rect`.
void paintIcon(QPainter& painter, const QRectF& rect, Icon icon, const QColor& color);
/// Signal strength bars (0-100) in `rect`.
void paintSignalBars(QPainter& painter, const QRectF& rect, int percent, const QColor& on, const QColor& off);

QPushButton* button(const QString& text, QWidget* parent, const QString& objectName = QString(), int height = 48);
/// White-filled call to action.
QPushButton* primaryButton(const QString& text, QWidget* parent, const QString& objectName = QString(), int height = 52);
QLabel* title(const QString& text, QWidget* parent);
QLabel* muted(const QString& text, QWidget* parent);
/// 14 px SemiBold section label, e.g. "NETWORKS".
QLabel* sectionLabel(const QString& text, QWidget* parent);
QLabel* body(const QString& text, QWidget* parent);
QVBoxLayout* vbox(QWidget* widget, int margin = 0, int spacing = 8);
QHBoxLayout* hbox(int spacing = 8);
/// A scrolling page body with kinetic touch scrolling that cannot turn a
/// swipe's release into a tap.
QScrollArea* scrollArea(QWidget* content, QWidget* parent, const QString& objectName);
/// Card-like frame (panel fill with a line border).
QFrame* card(QWidget* parent);
/// Applies the shared extra stylesheet to a top-level Deck Lab widget.
void applyStyle(QWidget* widget);
QString formatMegabytes(double megabytes);

/// A push button that keeps its touch height. The surface style sheet's
/// min-height would otherwise replace setFixedHeight() when it polishes.
class TouchButton : public QPushButton {
    Q_OBJECT
  public:
    TouchButton(const QString& text, QWidget* parent, int height);
    void setTouchHeight(int height);
    int touchHeight() const {
        return m_touchHeight;
    }

  protected:
    bool event(QEvent* event) override;

  private:
    int m_touchHeight;
};

/// Combo box with the same guaranteed touch height and 44 px popup rows.
QComboBox* touchCombo(QWidget* parent, const QString& objectName, int height = 48);

/// Large two-line tile with an icon, e.g. "Wi-Fi / Connected · Home".
class Tile : public TouchButton {
    Q_OBJECT
  public:
    Tile(Icon icon, const QString& title, QWidget* parent, const QString& objectName);
    void setSubtitle(const QString& subtitle);
    QString subtitle() const {
        return m_subtitle;
    }
    void setAlert(bool alert);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    Icon m_icon;
    QString m_title;
    QString m_subtitle;
    bool m_alert = false;
};

/// Painted list row for a choice: icon, two lines of text, optional tags and
/// a trailing chevron or signal bars. Taps behave like a button.
class Row : public TouchButton {
    Q_OBJECT
  public:
    Row(const QString& title, const QString& subtitle, QWidget* parent, const QString& objectName = QString());
    void setRowIcon(Icon icon);
    void setSignal(int percent); // -1 hides the bars
    void setTags(const QStringList& tags);
    void setLocked(bool locked);
    void setChevron(bool chevron);
    void setSelectedRow(bool selected);
    void setTexts(const QString& title, const QString& subtitle);
    /// Display-only row: full contrast, ignores touches.
    void setStatic(bool isStatic);
    QString titleText() const {
        return m_title;
    }
    QString subtitleText() const {
        return m_subtitle;
    }
    QStringList tags() const {
        return m_tags;
    }

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QString m_title;
    QString m_subtitle;
    QStringList m_tags;
    bool m_hasIcon = false;
    Icon m_icon = Icon::Info;
    int m_signal = -1;
    bool m_locked = false;
    bool m_chevron = false;
    bool m_selected = false;
};

/// Internal touch keyboard (same key rows as Browse search, 44 px keys),
/// with Shift and symbols for passwords. Types into its target line edit.
class Keyboard : public QWidget {
    Q_OBJECT
  public:
    explicit Keyboard(QWidget* parent);
    void setTarget(QLineEdit* target);
    QLineEdit* target() const {
        return m_target;
    }

  signals:
    void done();

  private:
    void rebuild();
    void type(const QString& text);

    QPointer<QLineEdit> m_target;
    QWidget* m_keys = nullptr;
    bool m_shift = false;
    bool m_symbols = false;
};

/// r4-system: a control the browse knob can drive once it is focused.
/// Press, turn, press is the CDJ UTILITY gesture: press to edit, turn to
/// change, press to set; back while editing restores the old value.
class KnobTarget {
  public:
    virtual ~KnobTarget() = default;
    /// The knob was pressed on this control. True: it now takes the turns.
    virtual bool knobPress() = 0;
    virtual void knobTurn(int steps) {
        Q_UNUSED(steps);
    }
    virtual void knobCommit() {
    }
    virtual void knobCancel() {
    }
    /// A held knob press from a script that reports the release.
    virtual bool knobHold(bool down) {
        Q_UNUSED(down);
        return false;
    }
};

/// r4-system: encoder focus for a page or sheet. A turn moves a 2 px white
/// ring over the visible controls of the scope in reading order, a press
/// activates the focused control (or starts editing a KnobTarget) and back
/// leaves editing or calls `back`. It claims the browse knob
/// (library/decklabknob.h) while `root` is visible, so a turn never opens
/// Browse behind a sheet and never turns a settings control by accident.
class KnobNavigator : public QObject {
    Q_OBJECT
  public:
    explicit KnobNavigator(QWidget* root, std::function<bool()> back = std::function<bool()>());
    ~KnobNavigator() override;
    void setBack(std::function<bool()> back) {
        m_back = std::move(back);
    }
    void setScope(QWidget* scope);
    QWidget* scope() const;
    QWidget* current() const {
        return m_current;
    }
    bool editing() const {
        return m_editing;
    }
    /// Controls a turn visits, in reading order.
    QList<QWidget*> candidates() const;
    void focus(QWidget* control);
    void focusFirst();
    void move(int steps);
    void press();
    void back();
    void hold(bool down);
    /// Hides the ring (touch took over, or the page changed).
    void clear();

  signals:
    void focusChanged(QWidget* control);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void placeRing();
    void setEditing(bool editing);
    // r5-fix3-system: a knob hold belongs to the one control that took it.
    void cancelHold();
    void disarm(QWidget* control);

    QWidget* m_root;
    QPointer<QWidget> m_scope;
    QPointer<QWidget> m_current;
    QPointer<QWidget> m_ring;
    std::function<bool()> m_back;
    bool m_editing = false;
    // r5-fix3-system: the held knob press (browse_hold) in progress. Only the
    // visible, ringed control of the page that owns the knob takes it; a
    // turn, a focus move, the control hiding or the knob going elsewhere
    // cancels it, and the release goes to the same control.
    QPointer<QWidget> m_holdTarget;
    QMetaObject::Connection m_holdConfirmed;
    bool m_holdKeptArm = false; // the hold started on an armed button: the press confirms
    bool m_holdFired = false;
    bool m_holdPressed = false;  // a press arrived during the hold (press at press-down)
    bool m_armOnRelease = false; // that press arms "press twice" once the hold ends early
    QElapsedTimer m_releasePress; // after a completed hold: the release press is not a new press
};

/// r4-system (Signature 4): hold-to-confirm for disruptive actions (restart,
/// turn off, forget, eject). Hold 1.2 s while a white fill sweeps left to
/// right like a playhead; let go early and it snaps back. From the knob: a
/// held press when the script reports the release, otherwise press, then
/// press again within 3 s. The result shows in place. `confirmed` is the only
/// signal that acts: `clicked` never fires.
class HoldButton : public TouchButton, public KnobTarget {
    Q_OBJECT
  public:
    static constexpr int kHoldMs = 1200;
    HoldButton(const QString& text, QWidget* parent, const QString& objectName = QString(), int height = 44);
    /// Row anatomy (Power page): icon, title, detail, HOLD at the right.
    void setRow(Icon icon, const QString& detail);
    void setDetail(const QString& detail);
    /// Locked: dimmed, with the reason printed instead of the detail.
    void setReason(const QString& reason);
    QString reason() const {
        return m_reason;
    }
    /// Shown in place of the detail after the action fired.
    void setResult(const QString& result);
    QString result() const {
        return m_result;
    }
    /// Checked when a hold starts: a non-empty refusal stops it (refused()).
    void setGate(std::function<QString()> gate) {
        m_gate = std::move(gate);
    }
    double progress() const;
    bool holding() const {
        return m_holding;
    }
    /// r5-fix3-system: shown and not scrolled or covered out of sight. A hold
    /// never completes on a button that isn't.
    bool onScreen() const;
    bool armed() const {
        return m_armed;
    }
    bool knobPress() override;
    bool knobHold(bool down) override;
    void knobCancel() override;

  signals:
    void confirmed();
    void refused(const QString& reason);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    bool begin();
    void stop();
    void fire();

    QTimer m_tick;
    QTimer m_disarm;
    QElapsedTimer m_since;
    std::function<QString()> m_gate;
    QString m_detail;
    QString m_reason;
    QString m_result;
    Icon m_icon = Icon::Power;
    bool m_row = false;
    bool m_holding = false;
    bool m_armed = false;
};

/// r4-system: a 44 x 44 button with a drawn glyph (back, plus, minus, close).
class GlyphButton : public TouchButton {
    Q_OBJECT
  public:
    GlyphButton(Glyph glyph, const QString& accessibleName, QWidget* parent, const QString& objectName,
            int size = 44);
    void setGlyph(Glyph glyph);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    Glyph m_glyph;
};

/// Modal-looking but non-blocking confirmation drawn over `host`.
class Confirm : public QWidget {
    Q_OBJECT
  public:
    static Confirm* ask(QWidget* host,
            const QString& title,
            const QString& text,
            const QString& confirmText,
            std::function<void()> onConfirm,
            bool destructive = true);
    /// Information only (single OK button).
    static Confirm* inform(QWidget* host, const QString& title, const QString& text);
    /// Details sheet: "Close" plus one optional explicit action. Nothing
    /// happens unless the action button itself is tapped.
    static Confirm* details(QWidget* host,
            const QString& title,
            const QString& text,
            const QString& actionText,
            std::function<void()> onAction);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

  private:
    explicit Confirm(QWidget* host);
    KnobNavigator* m_knob = nullptr;
    static Confirm* build(QWidget* host,
            const QString& title,
            const QString& text,
            const QString& dismissText,
            const QString& confirmText,
            std::function<void()> onConfirm,
            bool destructive);
};

} // namespace decklab::ui
