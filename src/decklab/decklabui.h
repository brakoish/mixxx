// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QString>
#include <QWidget>
#include <functional>

class QFrame;
class QLabel;
class QLineEdit;
class QScrollArea;
class QVBoxLayout;
class QHBoxLayout;

// Touch-sized building blocks matching the Deck Lab surface: matte black and
// graphite panels, white type, red selection and 44 px+ targets.
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
};

extern const char* const kAccent; // "#d74448"

/// Draws a simple line icon in `rect`.
void paintIcon(QPainter& painter, const QRectF& rect, Icon icon, const QColor& color);
/// Signal strength bars (0-100) in `rect`.
void paintSignalBars(QPainter& painter, const QRectF& rect, int percent, const QColor& on, const QColor& off);

QPushButton* button(const QString& text, QWidget* parent, const QString& objectName = QString(), int height = 48);
/// Red-bordered call to action.
QPushButton* primaryButton(const QString& text, QWidget* parent, const QString& objectName = QString(), int height = 52);
QLabel* title(const QString& text, QWidget* parent);
QLabel* muted(const QString& text, QWidget* parent);
QLabel* body(const QString& text, QWidget* parent);
QVBoxLayout* vbox(QWidget* widget, int margin = 0, int spacing = 8);
QHBoxLayout* hbox(int spacing = 8);
/// A scrolling page body with kinetic touch scrolling that cannot turn a
/// swipe's release into a tap.
QScrollArea* scrollArea(QWidget* content, QWidget* parent, const QString& objectName);
/// Card-like frame (#181818 with a subtle border).
QFrame* card(QWidget* parent);
/// Applies the shared extra stylesheet to a top-level Deck Lab widget.
void applyStyle(QWidget* widget);
QString formatMegabytes(double megabytes);

/// Large two-line tile with an icon, e.g. "Wi-Fi / Connected · Home".
class Tile : public QPushButton {
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
class Row : public QPushButton {
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

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

  private:
    explicit Confirm(QWidget* host);
};

} // namespace decklab::ui
