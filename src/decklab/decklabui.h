// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QFont>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QString>
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
    static Confirm* build(QWidget* host,
            const QString& title,
            const QString& text,
            const QString& dismissText,
            const QString& confirmText,
            std::function<void()> onConfirm,
            bool destructive);
};

} // namespace decklab::ui
