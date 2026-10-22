// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabui.h"

#include <QAbstractItemView>
#include <QBoxLayout>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QScroller>
#include <QTimer>
#include <algorithm>
#include <cmath>

#include "decklab/decklaboverlay.h"
#include "moc_decklabui.cpp"
#include "widget/decklabcombodelegate.h"

namespace decklab::ui {

const char* const kAccent = "#d74448";
const char* const kSelect = "#f2f4f7";

namespace {
const QString kExtraStyle = QStringLiteral(R"(
 WDeckLab QPushButton[touch="true"] {font-size:16px;font-weight:500;}
 WDeckLab QComboBox[touch="true"] {font-size:16px;font-weight:500;}
 WDeckLab QPushButton[primary="true"] {background:#f2f4f7;border:1px solid #f2f4f7;color:#0d0f12;font-weight:600;}
 WDeckLab QPushButton[primary="true"]:pressed {background:#c9ced6;}
 WDeckLab QPushButton[primary="true"]:disabled {background:#1c1f24;border-color:#2c313a;color:#6f6f6f;}
 WDeckLab QPushButton[quiet="true"] {background:transparent;border:0;color:#c9ced6;font-weight:600;}
 WDeckLab QPushButton[quiet="true"]:pressed {color:#ffffff;}
 WDeckLab QPushButton[key="true"] {min-height:0;padding:0;border-radius:0;font-size:18px;font-weight:500;}
 WDeckLab QPushButton[key="true"]:checked {background:#20242b;border-color:#f2f4f7;}
 WDeckLab QLabel[role="title"] {font-size:18px;font-weight:600;color:#f2f4f7;}
 WDeckLab QLabel[role="muted"] {font-size:15px;font-weight:500;color:#9aa3af;}
 WDeckLab QLabel[role="label"] {font-size:14px;font-weight:600;color:#f2f4f7;}
 WDeckLab QLabel[role="body"] {font-size:16px;font-weight:500;color:#e6e9ee;}
 WDeckLab QLabel[role="error"] {font-size:15px;font-weight:600;color:#ffb4ae;}
 WDeckLab QLabel[role="value"] {font-size:16px;font-weight:600;color:#f2f4f7;}
 WDeckLab QFrame[card="true"] {background:#171717;border:1px solid #2c2c2c;border-radius:3px;}
 WDeckLab QFrame[card="true"] QLabel {background:transparent;border:0;}
 WDeckLab QFrame[warning="true"] {background:#2a2312;border:1px solid #a07a22;border-radius:3px;}
 WDeckLab QFrame[warning="true"] QLabel {background:transparent;border:0;color:#ffe2a8;font-size:15px;font-weight:500;}
 WDeckLab QScrollBar::add-line:vertical, WDeckLab QScrollBar::sub-line:vertical {height:0px;border:0;}
 WDeckLab QScrollBar::add-line:horizontal, WDeckLab QScrollBar::sub-line:horizontal {width:0px;border:0;}
 WDeckLab QLineEdit {background:#141414;border:1px solid #555555;border-radius:2px;min-height:42px;padding:0 10px;font-size:16px;color:#ffffff;selection-background-color:#4a5160;}
 WDeckLab QLineEdit:focus {border-color:#f2f4f7;}
 WDeckLab QSlider::groove:horizontal {height:10px;background:#333333;border-radius:3px;}
 WDeckLab QSlider::sub-page:horizontal {background:#f2f4f7;border-radius:3px;}
 WDeckLab QSlider::handle:horizontal {background:#eeeeee;border:1px solid #777777;width:34px;margin:-16px 0;border-radius:3px;}
 )");

// Same anti-mis-tap rule as the Settings pages: Qt can synthesize a release
// after a recognized touch scroll; never let it become a command.
class TouchScrollGuard final : public QObject {
  public:
    TouchScrollGuard(QWidget* viewport, QWidget* content)
            : QObject(viewport),
              m_scroller(QScroller::scroller(viewport)),
              m_content(content) {
        QScroller::grabGesture(viewport, QScroller::TouchGesture);
        connect(m_scroller, &QScroller::stateChanged, this, [this](QScroller::State state) {
            if (state == QScroller::Dragging) {
                m_dragged = true;
            }
        });
        viewport->installEventFilter(this);
        watchAll();
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == m_content && event->type() == QEvent::ChildAdded) {
            // Rows are rebuilt as data arrives; guard them too.
            QTimer::singleShot(0, this, [this] { watchAll(); });
        }
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->source() != Qt::MouseEventNotSynthesized) {
                const auto state = m_scroller->state();
                const bool press = event->type() == QEvent::MouseButtonPress;
                if (press && (state == QScroller::Pressed || state == QScroller::Inactive)) {
                    m_dragged = false;
                }
                const bool suppress = m_dragged || state == QScroller::Dragging ||
                        state == QScroller::Scrolling;
                if (!press) {
                    m_dragged = false;
                }
                if (suppress) {
                    if (auto* pushButton = qobject_cast<QAbstractButton*>(watched)) {
                        pushButton->setDown(false);
                    }
                    return true;
                }
            }
        }
        return QObject::eventFilter(watched, event);
    }

  private:
    void watchAll() {
        if (!m_content) {
            return;
        }
        m_content->installEventFilter(this);
        const auto children = m_content->findChildren<QWidget*>();
        for (auto* child : children) {
            child->installEventFilter(this);
        }
    }
    QScroller* m_scroller;
    QPointer<QWidget> m_content;
    bool m_dragged = false;
};

void strokePath(QPainter& painter, const QPainterPath& path, const QColor& color, double width) {
    QPen pen(color, width);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
}
} // namespace

void applyStyle(QWidget* widget) {
    widget->setStyleSheet(kExtraStyle);
}

void paintIcon(QPainter& painter, const QRectF& area, Icon icon, const QColor& color) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    const double size = std::min(area.width(), area.height());
    const QRectF r(area.center().x() - size / 2, area.center().y() - size / 2, size, size);
    const double w = std::max(1.6, size / 13.0);
    const auto at = [&r](double x, double y) {
        return QPointF(r.left() + x * r.width(), r.top() + y * r.height());
    };
    QPainterPath path;
    switch (icon) {
    case Icon::Wifi: {
        const QPointF base = at(0.5, 0.82);
        for (double radius : {0.18, 0.34, 0.5}) {
            const double px = radius * size;
            QRectF arc(base.x() - px, base.y() - px, px * 2, px * 2);
            path.arcMoveTo(arc, 45);
            path.arcTo(arc, 45, 90);
        }
        strokePath(painter, path, color, w);
        painter.setBrush(color);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(base, w * 0.9, w * 0.9);
        break;
    }
    case Icon::Bluetooth:
        path.moveTo(at(0.3, 0.32));
        path.lineTo(at(0.7, 0.68));
        path.lineTo(at(0.5, 0.86));
        path.lineTo(at(0.5, 0.14));
        path.lineTo(at(0.7, 0.32));
        path.lineTo(at(0.3, 0.68));
        strokePath(painter, path, color, w);
        break;
    case Icon::Screen: {
        path.addEllipse(r.center(), size * 0.17, size * 0.17);
        for (int ray = 0; ray < 8; ++ray) {
            const double angle = ray * M_PI / 4;
            const QPointF inner(r.center().x() + std::cos(angle) * size * 0.28,
                    r.center().y() + std::sin(angle) * size * 0.28);
            const QPointF outer(r.center().x() + std::cos(angle) * size * 0.42,
                    r.center().y() + std::sin(angle) * size * 0.42);
            path.moveTo(inner);
            path.lineTo(outer);
        }
        strokePath(painter, path, color, w);
        break;
    }
    case Icon::Clock:
        path.addEllipse(r.center(), size * 0.38, size * 0.38);
        path.moveTo(r.center());
        path.lineTo(at(0.5, 0.26));
        path.moveTo(r.center());
        path.lineTo(at(0.68, 0.58));
        strokePath(painter, path, color, w);
        break;
    case Icon::Usb:
        path.addRoundedRect(QRectF(at(0.3, 0.38), at(0.7, 0.9)), w, w);
        path.moveTo(at(0.38, 0.38));
        path.lineTo(at(0.38, 0.12));
        path.lineTo(at(0.62, 0.12));
        path.lineTo(at(0.62, 0.38));
        path.moveTo(at(0.45, 0.22));
        path.lineTo(at(0.46, 0.22));
        path.moveTo(at(0.54, 0.22));
        path.lineTo(at(0.55, 0.22));
        strokePath(painter, path, color, w);
        break;
    case Icon::Power: {
        QRectF arc(at(0.18, 0.2), at(0.82, 0.84));
        path.arcMoveTo(arc, 60);
        path.arcTo(arc, 60, -300);
        path.moveTo(at(0.5, 0.1));
        path.lineTo(at(0.5, 0.48));
        strokePath(painter, path, color, w);
        break;
    }
    case Icon::Info:
        path.addEllipse(r.center(), size * 0.4, size * 0.4);
        path.moveTo(at(0.5, 0.46));
        path.lineTo(at(0.5, 0.72));
        path.moveTo(at(0.5, 0.3));
        path.lineTo(at(0.5, 0.31));
        strokePath(painter, path, color, w);
        break;
    case Icon::Wrench:
        path.moveTo(at(0.2, 0.8));
        path.lineTo(at(0.56, 0.44));
        path.addEllipse(at(0.66, 0.34), size * 0.15, size * 0.15);
        strokePath(painter, path, color, w * 1.3);
        break;
    case Icon::Controller:
        path.addRoundedRect(QRectF(at(0.06, 0.24), at(0.94, 0.76)), w * 1.5, w * 1.5);
        path.addEllipse(at(0.28, 0.5), size * 0.12, size * 0.12);
        path.addEllipse(at(0.72, 0.5), size * 0.12, size * 0.12);
        path.moveTo(at(0.5, 0.34));
        path.lineTo(at(0.5, 0.66));
        strokePath(painter, path, color, w);
        break;
    case Icon::Speaker: {
        path.moveTo(at(0.14, 0.38));
        path.lineTo(at(0.3, 0.38));
        path.lineTo(at(0.5, 0.2));
        path.lineTo(at(0.5, 0.8));
        path.lineTo(at(0.3, 0.62));
        path.lineTo(at(0.14, 0.62));
        path.closeSubpath();
        QRectF arc(at(0.42, 0.3), at(0.72, 0.7));
        path.arcMoveTo(arc, -50);
        path.arcTo(arc, -50, 100);
        QRectF outer(at(0.4, 0.16), at(0.88, 0.84));
        path.arcMoveTo(outer, -50);
        path.arcTo(outer, -50, 100);
        strokePath(painter, path, color, w);
        break;
    }
    case Icon::Headphones: {
        QRectF arc(at(0.16, 0.16), at(0.84, 0.84));
        path.arcMoveTo(arc, 180);
        path.arcTo(arc, 180, -180);
        path.addRoundedRect(QRectF(at(0.12, 0.52), at(0.3, 0.84)), w, w);
        path.addRoundedRect(QRectF(at(0.7, 0.52), at(0.88, 0.84)), w, w);
        strokePath(painter, path, color, w);
        break;
    }
    case Icon::Check:
        path.moveTo(at(0.2, 0.52));
        path.lineTo(at(0.42, 0.74));
        path.lineTo(at(0.82, 0.3));
        strokePath(painter, path, color, w * 1.3);
        break;
    case Icon::Warning:
        path.moveTo(at(0.5, 0.14));
        path.lineTo(at(0.9, 0.84));
        path.lineTo(at(0.1, 0.84));
        path.closeSubpath();
        path.moveTo(at(0.5, 0.4));
        path.lineTo(at(0.5, 0.62));
        path.moveTo(at(0.5, 0.73));
        path.lineTo(at(0.5, 0.74));
        strokePath(painter, path, color, w);
        break;
    case Icon::Lock: {
        path.addRoundedRect(QRectF(at(0.22, 0.46), at(0.78, 0.86)), w, w);
        QRectF arc(at(0.32, 0.16), at(0.68, 0.64));
        path.arcMoveTo(arc, 0);
        path.arcTo(arc, 0, 180);
        strokePath(painter, path, color, w);
        break;
    }
    case Icon::Restart: {
        QRectF arc(at(0.18, 0.18), at(0.82, 0.82));
        path.arcMoveTo(arc, 90);
        path.arcTo(arc, 90, -300);
        const QPointF tip = at(0.5, 0.18);
        path.moveTo(tip);
        path.lineTo(tip + QPointF(-size * 0.14, -size * 0.1));
        path.moveTo(tip);
        path.lineTo(tip + QPointF(-size * 0.12, size * 0.13));
        strokePath(painter, path, color, w);
        break;
    }
    case Icon::Decks:
        path.addRoundedRect(QRectF(at(0.06, 0.22), at(0.46, 0.78)), w, w);
        path.addRoundedRect(QRectF(at(0.54, 0.22), at(0.94, 0.78)), w, w);
        path.addEllipse(at(0.26, 0.5), size * 0.1, size * 0.1);
        path.addEllipse(at(0.74, 0.5), size * 0.1, size * 0.1);
        strokePath(painter, path, color, w);
        break;
    }
    painter.restore();
}

void paintSignalBars(QPainter& painter, const QRectF& rect, int percent, const QColor& on, const QColor& off) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    const int lit = percent >= 75 ? 4 : percent >= 50 ? 3 : percent >= 25 ? 2 : percent > 0 ? 1 : 0;
    const double gap = rect.width() * 0.08;
    const double barWidth = (rect.width() - 3 * gap) / 4;
    for (int bar = 0; bar < 4; ++bar) {
        const double height = rect.height() * (bar + 1) / 4.0;
        const QRectF shape(rect.left() + bar * (barWidth + gap), rect.bottom() - height, barWidth, height);
        painter.setBrush(bar < lit ? on : off);
        painter.drawRoundedRect(shape, 1, 1);
    }
    painter.restore();
}

TouchButton::TouchButton(const QString& text, QWidget* parent, int height)
        : QPushButton(text, parent),
          m_touchHeight(height) {
    setProperty("touch", true);
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setFixedHeight(height);
}

void TouchButton::setTouchHeight(int height) {
    m_touchHeight = height;
    setFixedHeight(height);
}

bool TouchButton::event(QEvent* event) {
    const bool handled = QPushButton::event(event);
    // Style sheets apply min-height while polishing: restore the target.
    if (m_touchHeight > 0 && (event->type() == QEvent::Polish || event->type() == QEvent::StyleChange)) {
        setMinimumHeight(m_touchHeight);
        setMaximumHeight(m_touchHeight);
    }
    return handled;
}

namespace {
class TouchComboBox : public QComboBox {
  public:
    TouchComboBox(QWidget* parent, int height)
            : QComboBox(parent),
              m_touchHeight(height) {
        setFixedHeight(height);
    }

  protected:
    bool event(QEvent* event) override {
        const bool handled = QComboBox::event(event);
        if (event->type() == QEvent::Polish || event->type() == QEvent::StyleChange) {
            setMinimumHeight(m_touchHeight);
            setMaximumHeight(m_touchHeight);
        }
        return handled;
    }

  private:
    int m_touchHeight;
};
} // namespace

QComboBox* touchCombo(QWidget* parent, const QString& objectName, int height) {
    auto* combo = new TouchComboBox(parent, height);
    combo->setObjectName(objectName);
    combo->setProperty("touch", true);
    combo->setFocusPolicy(Qt::NoFocus);
    combo->setItemDelegate(new DeckLabComboDelegate(combo));
    combo->setMaxVisibleItems(6);
    return combo;
}

QPushButton* button(const QString& text, QWidget* parent, const QString& objectName, int height) {
    auto* result = new TouchButton(text, parent, height);
    result->setObjectName(objectName);
    result->setAccessibleName(text);
    return result;
}

QPushButton* primaryButton(const QString& text, QWidget* parent, const QString& objectName, int height) {
    auto* result = button(text, parent, objectName, height);
    result->setProperty("primary", true);
    return result;
}

namespace {
QLabel* labelWithRole(const QString& text, QWidget* parent, const char* role) {
    auto* label = new QLabel(text, parent);
    label->setProperty("role", QString::fromLatin1(role));
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    return label;
}
} // namespace

QLabel* title(const QString& text, QWidget* parent) {
    auto* label = labelWithRole(text, parent, "title");
    label->setWordWrap(false);
    return label;
}
QLabel* muted(const QString& text, QWidget* parent) {
    return labelWithRole(text, parent, "muted");
}
QLabel* sectionLabel(const QString& text, QWidget* parent) {
    auto* label = labelWithRole(text, parent, "label");
    label->setWordWrap(false);
    return label;
}
QLabel* body(const QString& text, QWidget* parent) {
    return labelWithRole(text, parent, "body");
}

QVBoxLayout* vbox(QWidget* widget, int margin, int spacing) {
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(margin, margin, margin, margin);
    layout->setSpacing(spacing);
    return layout;
}

QHBoxLayout* hbox(int spacing) {
    auto* layout = new QHBoxLayout();
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(spacing);
    return layout;
}

QScrollArea* scrollArea(QWidget* content, QWidget* parent, const QString& objectName) {
    auto* scroll = new QScrollArea(parent);
    scroll->setObjectName(objectName);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(content);
    new TouchScrollGuard(scroll->viewport(), content);
    return scroll;
}

QFrame* card(QWidget* parent) {
    auto* frame = new QFrame(parent);
    frame->setProperty("card", true);
    return frame;
}

QString formatMegabytes(double megabytes) {
    if (megabytes < 0) {
        return QStringLiteral("—");
    }
    if (megabytes < 1024) {
        return QCoreApplication::translate("DeckLabUi", "%1 MB").arg(std::round(megabytes));
    }
    return QCoreApplication::translate("DeckLabUi", "%1 GB").arg(megabytes / 1024.0, 0, 'f', megabytes < 10240 ? 1 : 0);
}

Tile::Tile(Icon icon, const QString& title, QWidget* parent, const QString& objectName)
        : TouchButton(title, parent, 76),
          m_icon(icon),
          m_title(title) {
    setObjectName(objectName);
    setAccessibleName(title);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void Tile::setSubtitle(const QString& subtitle) {
    if (m_subtitle == subtitle) {
        return;
    }
    m_subtitle = subtitle;
    setAccessibleDescription(subtitle);
    update();
}

void Tile::setAlert(bool alert) {
    m_alert = alert;
    update();
}

void Tile::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QColor(isDown() ? "#5a5a5a" : "#303030"));
    painter.setBrush(QColor(isDown() ? "#262626" : "#191919"));
    painter.drawRoundedRect(frame, 3, 3);
    if (m_alert) {
        painter.fillRect(QRectF(0, 0, 3, height()), QColor(kAccent));
    }
    const QRectF iconBox(16, (height() - 30) / 2.0, 30, 30);
    paintIcon(painter, iconBox, m_icon, isEnabled() ? QColor("#f0f0f0") : QColor("#666666"));
    QFont titleFont = font();
    titleFont.setPixelSize(16);
    titleFont.setWeight(QFont::DemiBold);
    painter.setFont(titleFont);
    painter.setPen(isEnabled() ? QColor("#f2f4f7") : QColor("#777777"));
    const int textLeft = 62;
    const int textWidth = std::max(0, width() - textLeft - 30);
    painter.drawText(QRect(textLeft, 12, textWidth, 26), Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(titleFont).elidedText(m_title, Qt::ElideRight, textWidth));
    QFont subtitleFont = font();
    subtitleFont.setPixelSize(15);
    subtitleFont.setWeight(QFont::Medium);
    painter.setFont(subtitleFont);
    painter.setPen(m_alert ? QColor("#ffb4ae") : QColor("#9aa3af"));
    painter.drawText(QRect(textLeft, 40, textWidth, 22),
            Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(subtitleFont).elidedText(m_subtitle, Qt::ElideRight, textWidth));
    painter.setPen(QColor("#6a6a6a"));
    QFont chevron = font();
    chevron.setPixelSize(22);
    painter.setFont(chevron);
    painter.drawText(QRect(width() - 30, 0, 20, height()), Qt::AlignCenter, QStringLiteral("›"));
}

Row::Row(const QString& title, const QString& subtitle, QWidget* parent, const QString& objectName)
        : TouchButton(title, parent, subtitle.isEmpty() ? 48 : 56),
          m_title(title),
          m_subtitle(subtitle) {
    setObjectName(objectName);
    setAccessibleName(title);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void Row::setStatic(bool isStatic) {
    setAttribute(Qt::WA_TransparentForMouseEvents, isStatic);
}

void Row::setRowIcon(Icon icon) {
    m_icon = icon;
    m_hasIcon = true;
    update();
}
void Row::setSignal(int percent) {
    m_signal = percent;
    update();
}
void Row::setTags(const QStringList& tags) {
    m_tags = tags;
    update();
}
void Row::setLocked(bool locked) {
    m_locked = locked;
    update();
}
void Row::setChevron(bool chevron) {
    m_chevron = chevron;
    update();
}
void Row::setSelectedRow(bool selected) {
    m_selected = selected;
    update();
}
void Row::setTexts(const QString& title, const QString& subtitle) {
    m_title = title;
    m_subtitle = subtitle;
    setText(title);
    setAccessibleName(title);
    setTouchHeight(subtitle.isEmpty() ? 48 : 56);
    update();
}

void Row::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(isDown() ? "#272727" : m_selected ? "#20242b" : "#171717"));
    painter.fillRect(QRect(0, height() - 1, width(), 1), QColor("#2a2a2a"));
    if (m_selected) {
        painter.fillRect(QRect(0, 0, 3, height()), QColor(kSelect));
    }
    int left = 14;
    if (m_hasIcon) {
        paintIcon(painter, QRectF(left, (height() - 24) / 2.0, 24, 24), m_icon, QColor("#e6e6e6"));
        left += 36;
    }
    int right = width() - 12;
    if (m_chevron) {
        QFont chevron = font();
        chevron.setPixelSize(22);
        painter.setFont(chevron);
        painter.setPen(QColor("#6a6a6a"));
        painter.drawText(QRect(right - 16, 0, 16, height()), Qt::AlignCenter, QStringLiteral("›"));
        right -= 26;
    }
    if (m_signal >= 0) {
        paintSignalBars(painter, QRectF(right - 22, (height() - 16) / 2.0, 22, 16), m_signal,
                QColor("#f0f0f0"), QColor("#3d3d3d"));
        right -= 32;
    }
    if (m_locked) {
        paintIcon(painter, QRectF(right - 16, (height() - 16) / 2.0, 16, 16), Icon::Lock, QColor("#9c9c9c"));
        right -= 26;
    }
    QFont tagFont = font();
    tagFont.setPixelSize(14);
    tagFont.setWeight(QFont::DemiBold);
    const QFontMetrics tagMetrics(tagFont);
    painter.setFont(tagFont);
    for (int index = m_tags.size() - 1; index >= 0; --index) {
        const QString tag = m_tags.at(index);
        const int tagWidth = tagMetrics.horizontalAdvance(tag) + 16;
        if (right - tagWidth < left + 80) {
            break;
        }
        const QRectF pill(right - tagWidth, (height() - 24) / 2.0, tagWidth, 24);
        const bool strong = index == 0;
        painter.setPen(QColor(strong ? kSelect : "#3f3f3f"));
        painter.setBrush(QColor(strong ? "#20242b" : "#222222"));
        painter.drawRoundedRect(pill, 3, 3);
        painter.setPen(QColor(strong ? "#ffffff" : "#c8c8c8"));
        painter.drawText(pill, Qt::AlignCenter, tag);
        right -= tagWidth + 6;
    }
    const int textWidth = std::max(0, right - left - 6);
    QFont titleFont = font();
    titleFont.setPixelSize(16);
    titleFont.setWeight(QFont::DemiBold);
    painter.setFont(titleFont);
    painter.setPen(isEnabled() ? QColor("#f2f4f7") : QColor("#777777"));
    const QFontMetrics titleMetrics(titleFont);
    if (m_subtitle.isEmpty()) {
        painter.drawText(QRect(left, 0, textWidth, height()), Qt::AlignLeft | Qt::AlignVCenter,
                titleMetrics.elidedText(m_title, Qt::ElideRight, textWidth));
        return;
    }
    painter.drawText(QRect(left, 6, textWidth, 24), Qt::AlignLeft | Qt::AlignVCenter,
            titleMetrics.elidedText(m_title, Qt::ElideRight, textWidth));
    QFont subtitleFont = font();
    subtitleFont.setPixelSize(15);
    subtitleFont.setWeight(QFont::Medium);
    painter.setFont(subtitleFont);
    painter.setPen(QColor("#9aa3af"));
    painter.drawText(QRect(left, 30, textWidth, 22), Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(subtitleFont).elidedText(m_subtitle, Qt::ElideRight, textWidth));
}

Keyboard::Keyboard(QWidget* parent)
        : QWidget(parent) {
    setObjectName(QStringLiteral("decklab-system-keyboard"));
    auto* layout = vbox(this, 0, 2);
    m_keys = new QWidget(this);
    layout->addWidget(m_keys);
    rebuild();
}

void Keyboard::setTarget(QLineEdit* target) {
    m_target = target;
}

void Keyboard::type(const QString& text) {
    if (m_target) {
        m_target->insert(text);
    }
}

void Keyboard::rebuild() {
    // Rebuilding on Shift/symbols keeps every key a plain 40 px button.
    const auto oldKeys = m_keys->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    for (auto* child : oldKeys) {
        child->hide();
        child->deleteLater();
    }
    delete m_keys->layout();
    auto* rows = vbox(m_keys, 0, 2);
    QStringList letters;
    if (m_symbols) {
        letters = {QStringLiteral("1234567890"),
                QStringLiteral("!@#$%^&*()"),
                QStringLiteral("-_=+;:'\"/?"),
                QStringLiteral(",.<>[]{}|~")};
    } else {
        letters = {QStringLiteral("1234567890"),
                QStringLiteral("qwertyuiop"),
                QStringLiteral("asdfghjkl"),
                QStringLiteral("zxcvbnm")};
    }
    for (const QString& line : std::as_const(letters)) {
        auto* row = hbox(2);
        for (QChar letter : line) {
            const QString text = m_shift && !m_symbols ? QString(letter).toUpper() : QString(letter);
            auto* key = button(text, m_keys, QString(), 44);
            key->setProperty("key", true);
            key->setAccessibleName(text);
            connect(key, &QPushButton::clicked, this, [this, text] { type(text); });
            row->addWidget(key, 1);
        }
        rows->addLayout(row);
    }
    auto* actions = hbox(2);
    auto* shift = button(QStringLiteral("⇧"), m_keys, QStringLiteral("decklab-key-shift"), 44);
    shift->setProperty("key", true);
    shift->setAccessibleName(tr("Shift"));
    shift->setCheckable(true);
    shift->setChecked(m_shift);
    shift->setEnabled(!m_symbols);
    connect(shift, &QPushButton::clicked, this, [this] {
        m_shift = !m_shift;
        QTimer::singleShot(0, this, [this] { rebuild(); });
    });
    actions->addWidget(shift, 1);
    auto* symbols = button(m_symbols ? QStringLiteral("abc") : QStringLiteral("#+="), m_keys,
            QStringLiteral("decklab-key-symbols"), 44);
    symbols->setProperty("key", true);
    symbols->setAccessibleName(tr("Symbols"));
    connect(symbols, &QPushButton::clicked, this, [this] {
        m_symbols = !m_symbols;
        QTimer::singleShot(0, this, [this] { rebuild(); });
    });
    actions->addWidget(symbols, 1);
    auto* space = button(tr("Space"), m_keys, QString(), 44);
    space->setProperty("key", true);
    connect(space, &QPushButton::clicked, this, [this] { type(QStringLiteral(" ")); });
    actions->addWidget(space, 4);
    auto* erase = button(tr("Delete"), m_keys, QStringLiteral("decklab-key-delete"), 44);
    erase->setProperty("key", true);
    erase->setAutoRepeat(true);
    erase->setAutoRepeatDelay(400);
    erase->setAutoRepeatInterval(100);
    connect(erase, &QPushButton::clicked, this, [this] {
        if (m_target) {
            m_target->backspace();
        }
    });
    actions->addWidget(erase, 2);
    auto* finished = button(tr("Done"), m_keys, QStringLiteral("decklab-key-done"), 44);
    finished->setProperty("key", true);
    connect(finished, &QPushButton::clicked, this, &Keyboard::done);
    actions->addWidget(finished, 2);
    rows->addLayout(actions);
    // Polish new keys with the surface style immediately.
    m_keys->ensurePolished();
}

Confirm::Confirm(QWidget* host)
        : QWidget(host) {
    setObjectName(QStringLiteral("decklab-confirm"));
    setAttribute(Qt::WA_DeleteOnClose);
    setGeometry(host->rect());
    host->installEventFilter(this);
    // Covers the whole host: hide the native GL waveforms while shown.
    decklab::overlay::suppressWaveformsWhileVisible(this);
}

bool Confirm::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(watched, event);
}

void Confirm::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0, 0, 0, 175));
}

Confirm* Confirm::ask(QWidget* host,
        const QString& heading,
        const QString& text,
        const QString& confirmText,
        std::function<void()> onConfirm,
        bool destructive) {
    auto* overlay = new Confirm(host);
    applyStyle(overlay);
    auto* outer = vbox(overlay, 0, 0);
    outer->addStretch(1);
    auto* line = hbox(0);
    line->addStretch(1);
    auto* panel = card(overlay);
    panel->setFixedWidth(std::min(540, std::max(320, host->width() - 80)));
    auto* layout = vbox(panel, 20, 12);
    layout->addWidget(title(heading, panel));
    if (!text.isEmpty()) {
        layout->addWidget(body(text, panel));
    }
    auto* buttons = hbox(10);
    auto* cancel = button(onConfirm ? tr("Cancel") : tr("OK"), panel, QStringLiteral("decklab-confirm-cancel"), 48);
    buttons->addWidget(cancel, 1);
    connect(cancel, &QPushButton::clicked, overlay, &QWidget::close);
    if (onConfirm) {
        auto* accept = primaryButton(confirmText, panel, QStringLiteral("decklab-confirm-accept"), 48);
        accept->setProperty("destructive", destructive);
        buttons->addWidget(accept, 1);
        connect(accept, &QPushButton::clicked, overlay, [overlay, onConfirm] {
            overlay->close();
            onConfirm();
        });
    }
    layout->addLayout(buttons);
    line->addWidget(panel);
    line->addStretch(1);
    outer->addLayout(line);
    outer->addStretch(1);
    overlay->show();
    overlay->raise();
    return overlay;
}

Confirm* Confirm::inform(QWidget* host, const QString& heading, const QString& text) {
    return ask(host, heading, text, QString(), std::function<void()>(), false);
}

} // namespace decklab::ui
