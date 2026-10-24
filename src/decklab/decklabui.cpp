// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklabui.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QBoxLayout>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QFrame>
#include <QKeyEvent>
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
#include <utility>

#include "decklab/decklaboverlay.h"
#include "library/decklabknob.h"
#include "moc_decklabui.cpp"
#include "widget/decklabcombodelegate.h"

namespace decklab::ui {

const char* const kGround = "#0d0f12";
const char* const kPanel = "#16191e";
const char* const kRaise = "#20242b";
const char* const kLine = "#2c313a";
const char* const kDim = "#6b7380";
const char* const kMuted = "#9aa3af";
const char* const kText = "#f2f4f7";
const char* const kLive = "#ff3d2e";
const char* const kSelect = "#f2f4f7";
const char* const kGo = "#2fd27a";

namespace {
// The system kit's own sheet (System pages, Deck Setup, the Setup panel and
// Confirm sheets). Its rules carry no WDeckLab ancestor, so a sheet hosted on
// the main window looks the same as one inside the surface, and they repeat
// every state they override: a widget's own sheet beats its parents' sheets
// whatever the specificity. Brand tokens only; Barlow everywhere.
const QString kExtraStyle = QStringLiteral(R"(
 QWidget {font-family:"Barlow";}
 QPushButton[touch="true"] {background:#16191E;color:#F2F4F7;border:1px solid #2C313A;border-radius:3px;
     padding:0px 12px;font-size:16px;font-weight:600;}
 QPushButton[touch="true"]:pressed {background:#20242B;border-color:#F2F4F7;}
 QPushButton[touch="true"]:checked {background:#20242B;border-color:#F2F4F7;}
 QPushButton[touch="true"]:disabled {background:#16191E;border-color:#20242B;color:#6B7380;}
 QComboBox[touch="true"] {background:#16191E;color:#F2F4F7;border:1px solid #2C313A;border-radius:3px;
     padding-left:12px;font-size:16px;font-weight:500;}
 QComboBox[touch="true"]:disabled {color:#6B7380;border-color:#20242B;}
 QComboBox QAbstractItemView {background:#16191E;color:#F2F4F7;border:1px solid #2C313A;
     selection-background-color:#20242B;selection-color:#F2F4F7;font-size:16px;font-weight:500;}
 QPushButton[primary="true"] {background:#F2F4F7;border:1px solid #F2F4F7;color:#0D0F12;font-weight:600;}
 QPushButton[primary="true"]:pressed {background:#20242B;border-color:#F2F4F7;color:#F2F4F7;}
 QPushButton[primary="true"]:disabled {background:#16191E;border-color:#2C313A;color:#6B7380;}
 QPushButton[quiet="true"] {background:transparent;border:0;color:#F2F4F7;font-weight:600;}
 QPushButton[quiet="true"]:pressed {background:#20242B;}
 QPushButton[quiet="true"]:disabled {background:transparent;color:#6B7380;}
 QPushButton[key="true"] {min-height:0;padding:0;border-radius:0;font-size:18px;font-weight:500;}
 QPushButton[key="true"]:checked {background:#20242B;border-color:#F2F4F7;}
 QLabel[role="title"] {font-size:18px;font-weight:600;color:#F2F4F7;}
 QLabel[role="hero"] {font-family:"Barlow Semi Condensed";font-size:36px;font-weight:600;color:#F2F4F7;}
 QLabel[role="muted"] {font-size:15px;font-weight:500;color:#9AA3AF;}
 QLabel[role="label"] {font-size:14px;font-weight:600;color:#F2F4F7;}
 QLabel[role="body"] {font-size:16px;font-weight:500;color:#F2F4F7;}
 QLabel[role="error"] {font-size:15px;font-weight:600;color:#F2F4F7;border:0;border-left:3px solid #F2F4F7;
     padding-left:8px;}
 QLabel[role="caution"] {font-size:16px;font-weight:600;color:#F2F4F7;background:#20242B;border:1px solid #2C313A;
     border-left:3px solid #F2F4F7;border-radius:3px;padding:6px 10px;}
 QLabel[role="value"] {font-size:16px;font-weight:600;color:#F2F4F7;}
 QFrame[card="true"] {background:#16191E;border:1px solid #2C313A;border-radius:4px;}
 QFrame[card="true"] QLabel {background:transparent;border:0;}
 QFrame[warning="true"] {background:#20242B;border:1px solid #2C313A;border-left:3px solid #F2F4F7;border-radius:4px;}
 QFrame[warning="true"] QLabel {background:transparent;border:0;color:#F2F4F7;font-size:15px;font-weight:500;}
 QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {height:0px;border:0;}
 QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {width:0px;border:0;}
 QLineEdit {background:#16191E;border:1px solid #2C313A;border-radius:3px;min-height:42px;padding:0 10px;
     font-size:16px;font-weight:500;color:#F2F4F7;selection-background-color:#F2F4F7;selection-color:#0D0F12;}
 QLineEdit:focus {border-color:#F2F4F7;}
 QSlider::groove:horizontal {height:10px;background:#2C313A;border-radius:3px;}
 QSlider::sub-page:horizontal {background:#F2F4F7;border-radius:3px;}
 QSlider::handle:horizontal {background:#F2F4F7;border:1px solid #2C313A;width:34px;margin:-16px 0;border-radius:3px;}
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

QFont uiFont(const QFont& base, int pixels, QFont::Weight weight) {
    QFont font = base;
    font.setFamilies({QStringLiteral("Barlow"), base.family()});
    font.setPixelSize(std::max(14, pixels));
    font.setWeight(weight);
    return font;
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
    // Pressed: raised fill and a text-coloured edge, visible in a dark booth.
    painter.setPen(QColor(isDown() ? kText : kLine));
    painter.setBrush(QColor(isDown() ? kRaise : kPanel));
    painter.drawRoundedRect(frame, 4, 4);
    if (m_alert) {
        // Caution, not live: a white edge (live red means the room hears it).
        painter.fillRect(QRectF(0, 0, 3, height()), QColor(kText));
    }
    const QRectF iconBox(16, (height() - 30) / 2.0, 30, 30);
    paintIcon(painter, iconBox, m_icon, QColor(isEnabled() ? kText : kDim));
    const QFont titleFont = uiFont(font(), 16, QFont::DemiBold);
    painter.setFont(titleFont);
    painter.setPen(QColor(isEnabled() ? kText : kDim));
    const int textLeft = 62;
    const int textWidth = std::max(0, width() - textLeft - 30);
    painter.drawText(QRect(textLeft, 12, textWidth, 26), Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(titleFont).elidedText(m_title, Qt::ElideRight, textWidth));
    const QFont subtitleFont = uiFont(font(), 15, QFont::Medium);
    painter.setFont(subtitleFont);
    painter.setPen(QColor(m_alert ? kText : kMuted));
    painter.drawText(QRect(textLeft, 40, textWidth, 22),
            Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(subtitleFont).elidedText(m_subtitle, Qt::ElideRight, textWidth));
    painter.setPen(QColor(kMuted));
    painter.setFont(uiFont(font(), 22, QFont::Medium));
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
    painter.fillRect(rect(), QColor(isDown() || m_selected ? kRaise : kPanel));
    painter.fillRect(QRect(0, height() - 1, width(), 1), QColor(kLine));
    if (isDown()) {
        // Pressed: a text-coloured inner edge, visible in a dark booth.
        painter.setPen(QColor(kText));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));
    }
    if (m_selected) {
        painter.fillRect(QRect(0, 0, 3, height()), QColor(kSelect));
    }
    int left = 14;
    if (m_hasIcon) {
        paintIcon(painter, QRectF(left, (height() - 24) / 2.0, 24, 24), m_icon, QColor(isEnabled() ? kText : kDim));
        left += 36;
    }
    int right = width() - 12;
    if (m_chevron) {
        painter.setFont(uiFont(font(), 22, QFont::Medium));
        painter.setPen(QColor(kMuted));
        painter.drawText(QRect(right - 16, 0, 16, height()), Qt::AlignCenter, QStringLiteral("›"));
        right -= 26;
    }
    if (m_signal >= 0) {
        paintSignalBars(painter, QRectF(right - 22, (height() - 16) / 2.0, 22, 16), m_signal,
                QColor(kText), QColor(kLine));
        right -= 32;
    }
    if (m_locked) {
        paintIcon(painter, QRectF(right - 16, (height() - 16) / 2.0, 16, 16), Icon::Lock, QColor(kMuted));
        right -= 26;
    }
    const QFont tagFont = uiFont(font(), 14, QFont::DemiBold);
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
        painter.setPen(QColor(strong ? kSelect : kLine));
        painter.setBrush(QColor(kRaise));
        painter.drawRoundedRect(pill, 3, 3);
        painter.setPen(QColor(kText));
        painter.drawText(pill, Qt::AlignCenter, tag);
        right -= tagWidth + 6;
    }
    const int textWidth = std::max(0, right - left - 6);
    const QFont titleFont = uiFont(font(), 16, QFont::DemiBold);
    painter.setFont(titleFont);
    painter.setPen(QColor(isEnabled() ? kText : kDim));
    const QFontMetrics titleMetrics(titleFont);
    if (m_subtitle.isEmpty()) {
        painter.drawText(QRect(left, 0, textWidth, height()), Qt::AlignLeft | Qt::AlignVCenter,
                titleMetrics.elidedText(m_title, Qt::ElideRight, textWidth));
        return;
    }
    painter.drawText(QRect(left, 6, textWidth, 24), Qt::AlignLeft | Qt::AlignVCenter,
            titleMetrics.elidedText(m_title, Qt::ElideRight, textWidth));
    const QFont subtitleFont = uiFont(font(), 15, QFont::Medium);
    painter.setFont(subtitleFont);
    painter.setPen(QColor(kMuted));
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
    // Rebuilding on Shift/symbols keeps every key a plain 44 px button.
    const auto oldKeys = m_keys->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    for (auto* child : oldKeys) {
        child->hide();
        child->deleteLater();
    }
    delete m_keys->layout();
    // 4 px gaps between keys (the touch rule), so neighbours aren't hit.
    auto* rows = vbox(m_keys, 0, 4);
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
        auto* row = hbox(4);
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
    auto* actions = hbox(4);
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
    // r4-system: the knob drives the sheet (turn, press; back dismisses), so
    // a turn never opens Browse behind it.
    m_knob = new KnobNavigator(this, [this] {
        close();
        return true;
    });
}

bool Confirm::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(watched, event);
}

void Confirm::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    // Scrim: ground at about 70 %.
    QColor scrim(kGround);
    scrim.setAlpha(175);
    painter.fillRect(rect(), scrim);
}

Confirm* Confirm::ask(QWidget* host,
        const QString& heading,
        const QString& text,
        const QString& confirmText,
        std::function<void()> onConfirm,
        bool destructive) {
    // Decided before the call: argument evaluation order is unspecified, and
    // the moved-from function would read as empty.
    const QString dismissText = onConfirm ? tr("Cancel") : tr("OK");
    return build(host, heading, text, dismissText, confirmText, std::move(onConfirm), destructive);
}

Confirm* Confirm::details(QWidget* host,
        const QString& heading,
        const QString& text,
        const QString& actionText,
        std::function<void()> onAction) {
    return build(host, heading, text, tr("Close"), actionText, std::move(onAction), false);
}

Confirm* Confirm::build(QWidget* host,
        const QString& heading,
        const QString& text,
        const QString& dismissText,
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
    auto* cancel = button(dismissText, panel, QStringLiteral("decklab-confirm-cancel"), 48);
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

// ------------------------------------------------------------ r4-system --

namespace {
// The 2 px white encoder focus ring, drawn on top of the focused control.
class FocusRing final : public QWidget {
  public:
    explicit FocusRing(QWidget* parent)
            : QWidget(parent) {
        setObjectName(QStringLiteral("decklab-knob-ring"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
        setProperty("knobSkip", true);
    }
    void setEditing(bool editing) {
        if (m_editing != editing) {
            m_editing = editing;
            update();
        }
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        // Editing: a heavier ring, so "turn changes the value" reads at a glance.
        const double width = m_editing ? 3.0 : 2.0;
        painter.setPen(QPen(QColor(kText), width));
        painter.setBrush(Qt::NoBrush);
        const double inset = width / 2.0;
        painter.drawRoundedRect(QRectF(rect()).adjusted(inset, inset, -inset, -inset), 4, 4);
    }

  private:
    bool m_editing = false;
};
} // namespace

void paintLamp(QPainter& painter, const QPointF& center, double radius, Lamp lamp) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    switch (lamp) {
    case Lamp::Off:
        painter.setPen(QPen(QColor(kDim), 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(center, radius - 0.75, radius - 0.75);
        break;
    case Lamp::On:
    case Lamp::Go:
    case Lamp::Live:
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(lamp == Lamp::On ? kText : lamp == Lamp::Go ? kGo : kLive));
        painter.drawEllipse(center, radius, radius);
        break;
    case Lamp::LiveOutline:
        painter.setPen(QPen(QColor(kLive), 2.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(center, radius - 1.0, radius - 1.0);
        break;
    case Lamp::Caution: {
        const QRectF box(center.x() - radius, center.y() - radius, radius * 2, radius * 2);
        QPainterPath dot;
        dot.addEllipse(box);
        painter.setClipPath(dot);
        paintCautionHatch(painter, box);
        painter.setClipping(false);
        painter.setPen(QPen(QColor(kText), 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(center, radius - 0.75, radius - 0.75);
        break;
    }
    }
    painter.restore();
}

void paintCautionHatch(QPainter& painter, const QRectF& rect, double radius) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath shape;
    shape.addRoundedRect(rect, radius, radius);
    painter.setClipPath(shape, Qt::IntersectClip);
    painter.fillRect(rect, QColor(kRaise));
    QColor stripe(kText);
    stripe.setAlphaF(0.25);
    painter.setPen(QPen(stripe, 2.0));
    for (double offset = -rect.height(); offset < rect.width(); offset += 6.0) {
        painter.drawLine(QPointF(rect.left() + offset, rect.bottom()),
                QPointF(rect.left() + offset + rect.height(), rect.top()));
    }
    painter.restore();
}

void paintGlyph(QPainter& painter, const QRectF& area, Glyph glyph, const QColor& color) {
    if (glyph == Glyph::Lock) {
        paintIcon(painter, area, Icon::Lock, color);
        return;
    }
    if (glyph == Glyph::Warning) {
        paintIcon(painter, area, Icon::Warning, color);
        return;
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    const double size = std::min(area.width(), area.height());
    const QRectF r(area.center().x() - size / 2, area.center().y() - size / 2, size, size);
    const double w = std::max(1.75, size / 12.0); // 2 px at 24
    const auto at = [&r](double x, double y) {
        return QPointF(r.left() + x * r.width(), r.top() + y * r.height());
    };
    QPainterPath path;
    switch (glyph) {
    case Glyph::Back:
        path.moveTo(at(0.62, 0.22));
        path.lineTo(at(0.36, 0.5));
        path.lineTo(at(0.62, 0.78));
        break;
    case Glyph::Chevron:
        path.moveTo(at(0.4, 0.22));
        path.lineTo(at(0.66, 0.5));
        path.lineTo(at(0.4, 0.78));
        break;
    case Glyph::Gear: {
        path.addEllipse(r.center(), size * 0.28, size * 0.28);
        path.addEllipse(r.center(), size * 0.1, size * 0.1);
        for (int tooth = 0; tooth < 8; ++tooth) {
            const double angle = tooth * M_PI / 4;
            path.moveTo(r.center() + QPointF(std::cos(angle), std::sin(angle)) * size * 0.3);
            path.lineTo(r.center() + QPointF(std::cos(angle), std::sin(angle)) * size * 0.43);
        }
        QPen pen(color, w);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
        painter.restore();
        return;
    }
    case Glyph::Check:
        path.moveTo(at(0.2, 0.52));
        path.lineTo(at(0.42, 0.74));
        path.lineTo(at(0.82, 0.3));
        break;
    case Glyph::Minus:
        path.moveTo(at(0.25, 0.5));
        path.lineTo(at(0.75, 0.5));
        break;
    case Glyph::Plus:
        path.moveTo(at(0.25, 0.5));
        path.lineTo(at(0.75, 0.5));
        path.moveTo(at(0.5, 0.25));
        path.lineTo(at(0.5, 0.75));
        break;
    case Glyph::Close:
        path.moveTo(at(0.28, 0.28));
        path.lineTo(at(0.72, 0.72));
        path.moveTo(at(0.72, 0.28));
        path.lineTo(at(0.28, 0.72));
        break;
    case Glyph::Warning:
    case Glyph::Lock:
        break;
    }
    strokePath(painter, path, color, w);
    painter.restore();
}

QFont numberFont(const QFont& base, int pixels) {
    QFont font = base;
    font.setFamilies({QStringLiteral("Barlow Semi Condensed"), QStringLiteral("Barlow")});
    font.setPixelSize(std::max(14, pixels));
    font.setWeight(QFont::DemiBold);
    return font;
}

QFont capsFont(const QFont& base, int pixels) {
    QFont font = uiFont(base, pixels, QFont::DemiBold);
    font.setCapitalization(QFont::AllUppercase);
    font.setLetterSpacing(QFont::PercentageSpacing, 106);
    return font;
}

KnobNavigator::KnobNavigator(QWidget* root, std::function<bool()> back)
        : QObject(root),
          m_root(root),
          m_back(std::move(back)) {
    root->installEventFilter(this);
    if (root->isVisible()) {
        decklab::claimKnob(root);
    }
    // r5-fix3-system: a menu, dialog, sheet or another page taking the knob
    // calls off a hold in progress and a first press of "press twice".
    decklab::onKnobOwnerChanged(this, [this] {
        if (decklab::knobOwner().blocker != m_root) {
            cancelHold();
            disarm(m_current);
        }
    });
}

KnobNavigator::~KnobNavigator() {
    decklab::releaseKnob(m_root);
}

void KnobNavigator::setScope(QWidget* scope) {
    if (m_scope.data() == scope) {
        return;
    }
    m_scope = scope;
    cancelHold();
    disarm(m_current);
    setEditing(false);
    if (m_ring) {
        m_ring->hide();
    }
}

QWidget* KnobNavigator::scope() const {
    return m_scope ? m_scope.data() : m_root;
}

QList<QWidget*> KnobNavigator::candidates() const {
    QWidget* area = scope();
    QList<QWidget*> list;
    const auto children = area->findChildren<QWidget*>();
    for (auto* widget : children) {
        if (!widget->isVisible() || !widget->isEnabled() || widget->property("knobSkip").toBool() ||
                widget->testAttribute(Qt::WA_TransparentForMouseEvents) || widget->width() <= 0 ||
                widget->height() <= 0) {
            continue;
        }
        if (!qobject_cast<QAbstractButton*>(widget) && !dynamic_cast<KnobTarget*>(widget)) {
            continue;
        }
        list.append(widget);
    }
    // Reading order: rows top to bottom (8 px bands), then left to right.
    std::stable_sort(list.begin(), list.end(), [area](QWidget* left, QWidget* right) {
        const QPoint a = left->mapTo(area, QPoint(0, 0));
        const QPoint b = right->mapTo(area, QPoint(0, 0));
        const int bandA = a.y() / 8;
        const int bandB = b.y() / 8;
        return bandA != bandB ? bandA < bandB : a.x() < b.x();
    });
    return list;
}

void KnobNavigator::focus(QWidget* control) {
    if (m_current.data() != control) {
        setEditing(false);
        // Moving the ring calls off the hold and "press again" on the old one.
        cancelHold();
        disarm(m_current);
    }
    m_current = control;
    if (!control) {
        if (m_ring) {
            m_ring->hide();
        }
        return;
    }
    for (QWidget* parent = control->parentWidget(); parent; parent = parent->parentWidget()) {
        if (auto* area = qobject_cast<QScrollArea*>(parent)) {
            area->ensureWidgetVisible(control, 8, 8);
            break;
        }
    }
    control->installEventFilter(this);
    placeRing();
    emit focusChanged(control);
}

void KnobNavigator::focusFirst() {
    const auto list = candidates();
    // The page's selected row (the checked category) first.
    for (auto* widget : list) {
        const auto* button = qobject_cast<QAbstractButton*>(widget);
        if (button && button->property("knobHome").toBool() && button->isChecked()) {
            focus(widget);
            return;
        }
    }
    focus(list.isEmpty() ? nullptr : list.first());
}

void KnobNavigator::move(int steps) {
    if (steps == 0) {
        return;
    }
    // Any turn calls off a hold in progress and a first press.
    m_releasePress.invalidate();
    cancelHold();
    disarm(m_current);
    if (m_editing && m_current) {
        if (auto* target = dynamic_cast<KnobTarget*>(m_current.data())) {
            target->knobTurn(steps);
            return;
        }
    }
    const auto list = candidates();
    if (list.isEmpty()) {
        focus(nullptr);
        return;
    }
    const int index = m_current ? static_cast<int>(list.indexOf(m_current.data())) : -1;
    if (index < 0) {
        focusFirst();
        return;
    }
    if (!m_ring || !m_ring->isVisible()) {
        // The first turn only shows where the ring is.
        focus(m_current);
        return;
    }
    focus(list.at(std::clamp(index + steps, 0, static_cast<int>(list.size()) - 1)));
}

void KnobNavigator::press() {
    // r5-fix3-system: on Browse the controller sends the press on release.
    // After a completed knob hold that press is the end of the same gesture.
    if (m_releasePress.isValid()) {
        const bool sameGesture = m_releasePress.elapsed() < 500;
        m_releasePress.invalidate();
        if (sameGesture) {
            return;
        }
    }
    if (m_holdTarget) {
        // Off Browse the press comes at press-down, during the hold.
        m_holdPressed = true;
        auto* held = qobject_cast<HoldButton*>(m_holdTarget.data());
        if (held && held == m_current.data() && held->holding()) {
            // Arms "press twice" only if the hold ends early.
            m_armOnRelease = true;
            return;
        }
    }
    if (!m_current || !m_current->isVisible() || !m_current->isEnabled() || !candidates().contains(m_current)) {
        focusFirst();
        return;
    }
    if (!m_ring || !m_ring->isVisible()) {
        // Never act on a control the DJ can't see is focused.
        focus(m_current);
        return;
    }
    if (auto* target = dynamic_cast<KnobTarget*>(m_current.data())) {
        if (m_editing) {
            target->knobCommit();
            setEditing(false);
            return;
        }
        setEditing(target->knobPress());
        return;
    }
    if (auto* button = qobject_cast<QAbstractButton*>(m_current.data())) {
        QPointer<KnobNavigator> self(this);
        button->click();
        if (self) {
            placeRing();
        }
    }
}

void KnobNavigator::back() {
    // Back calls off a hold or a first press before it goes up a level.
    m_releasePress.invalidate();
    if (auto* held = qobject_cast<HoldButton*>(m_current.data()); held && (held->armed() || held->holding())) {
        cancelHold();
        disarm(held);
        return;
    }
    cancelHold();
    if (m_editing && m_current) {
        if (auto* target = dynamic_cast<KnobTarget*>(m_current.data())) {
            target->knobCancel();
        }
        setEditing(false);
        return;
    }
    if (m_back) {
        m_back();
    }
}

void KnobNavigator::hold(bool down) {
    if (down) {
        cancelHold();
        m_releasePress.invalidate();
        QWidget* control = m_current.data();
        auto* target = dynamic_cast<KnobTarget*>(control);
        // r5-fix3-system: only the control the DJ can see is focused (ringed),
        // on screen, on the page that owns the knob, takes a hold.
        if (!target || !control->isVisible() || !control->isEnabled() || !m_ring || !m_ring->isVisible() ||
                decklab::knobOwner().blocker != m_root || control->visibleRegion().isEmpty() ||
                !candidates().contains(control)) {
            return;
        }
        m_holdTarget = control;
        if (auto* held = qobject_cast<HoldButton*>(control)) {
            m_holdConfirmed = connect(held, &HoldButton::confirmed, this, [this] { m_holdFired = true; });
            if (held->armed()) {
                // Armed by a first press: keep it, the press confirms.
                m_holdKeptArm = true;
                return;
            }
        }
        target->knobHold(true);
        return;
    }
    QPointer<QWidget> control = m_holdTarget;
    if (!control) {
        return;
    }
    const bool fired = m_holdFired;
    const bool pressed = m_holdPressed;
    const bool arm = m_armOnRelease;
    disconnect(m_holdConfirmed);
    m_holdTarget = nullptr;
    m_holdKeptArm = false;
    m_holdFired = false;
    m_holdPressed = false;
    m_armOnRelease = false;
    // The release goes to the control that took the hold, never another one.
    if (auto* target = dynamic_cast<KnobTarget*>(control.data())) {
        target->knobHold(false);
    }
    if (fired) {
        if (!pressed) {
            m_releasePress.start();
        }
        return;
    }
    auto* held = qobject_cast<HoldButton*>(control.data());
    if (held && arm && held->onScreen() && held == m_current.data()) {
        // A short hold with the press at press-down: the first press of
        // "press twice".
        held->knobPress();
    }
}

void KnobNavigator::cancelHold() {
    QPointer<QWidget> control = m_holdTarget;
    disconnect(m_holdConfirmed);
    m_holdTarget = nullptr;
    m_holdKeptArm = false;
    m_holdFired = false;
    m_holdPressed = false;
    m_armOnRelease = false;
    if (auto* held = qobject_cast<HoldButton*>(control.data())) {
        held->knobCancel();
    } else if (auto* target = dynamic_cast<KnobTarget*>(control.data())) {
        target->knobHold(false);
    }
}

void KnobNavigator::disarm(QWidget* control) {
    // Only hold buttons: other targets' cancel restores edited values.
    if (auto* held = qobject_cast<HoldButton*>(control); held && (held->armed() || held->holding())) {
        held->knobCancel();
    }
}

void KnobNavigator::clear() {
    cancelHold();
    disarm(m_current);
    setEditing(false);
    if (m_ring) {
        m_ring->hide();
    }
}

void KnobNavigator::setEditing(bool editing) {
    m_editing = editing;
    if (m_ring) {
        static_cast<FocusRing*>(m_ring.data())->setEditing(editing);
    }
}

void KnobNavigator::placeRing() {
    if (!m_current || !m_current->isVisible() || !m_current->parentWidget()) {
        if (m_ring) {
            m_ring->hide();
        }
        return;
    }
    QWidget* host = m_current->parentWidget();
    if (!m_ring || m_ring->parentWidget() != host) {
        if (m_ring) {
            m_ring->hide();
            m_ring->deleteLater();
        }
        m_ring = new FocusRing(host);
    }
    static_cast<FocusRing*>(m_ring.data())->setEditing(m_editing);
    m_ring->setGeometry(m_current->geometry());
    m_ring->raise();
    m_ring->show();
}

bool KnobNavigator::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_root) {
        switch (event->type()) {
        case QEvent::Show:
            decklab::claimKnob(m_root);
            break;
        case QEvent::Hide:
            decklab::releaseKnob(m_root);
            clear();
            break;
        case QEvent::KeyPress: {
            const auto* key = static_cast<QKeyEvent*>(event);
            switch (key->key()) {
            case Qt::Key_Up:
                move(-1);
                return true;
            case Qt::Key_Down:
                move(1);
                return true;
            case Qt::Key_Return:
            case Qt::Key_Enter:
                if (!key->isAutoRepeat()) {
                    press();
                }
                return true;
            case Qt::Key_Escape:
                back();
                return true;
            case Qt::Key_Select:
                if (!key->isAutoRepeat()) {
                    hold(true);
                }
                return true;
            default:
                break;
            }
            break;
        }
        case QEvent::KeyRelease: {
            const auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Select && !key->isAutoRepeat()) {
                hold(false);
                return true;
            }
            break;
        }
        default:
            break;
        }
    } else if (watched == m_current.data()) {
        // Follow the control while the ring is up. A Show never brings back a
        // cleared ring: the page opens without one, so the first turn shows
        // where it is instead of moving past the current row.
        if ((event->type() == QEvent::Move || event->type() == QEvent::Resize) && m_ring && !m_ring->isHidden()) {
            placeRing();
        } else if (event->type() == QEvent::Hide) {
            clear();
        } else if (event->type() == QEvent::EnabledChange && !m_current->isEnabled()) {
            // A row locking (a deck started) calls off its hold and first press.
            cancelHold();
            disarm(m_current);
        }
    }
    return QObject::eventFilter(watched, event);
}

HoldButton::HoldButton(const QString& text, QWidget* parent, const QString& objectName, int height)
        : TouchButton(text, parent, height) {
    setObjectName(objectName);
    setAccessibleName(text);
    setAccessibleDescription(tr("Hold for about a second to confirm"));
    // 25 fps sweep, only while a finger (or the knob) holds it.
    m_tick.setInterval(40);
    connect(&m_tick, &QTimer::timeout, this, [this] {
        if (m_since.elapsed() >= kHoldMs) {
            fire();
            return;
        }
        update();
    });
    m_disarm.setSingleShot(true);
    m_disarm.setInterval(3000);
    connect(&m_disarm, &QTimer::timeout, this, [this] {
        m_armed = false;
        update();
    });
}

void HoldButton::setRow(Icon icon, const QString& detail) {
    m_row = true;
    m_icon = icon;
    m_detail = detail;
    setTouchHeight(64);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    update();
}

void HoldButton::setDetail(const QString& detail) {
    if (m_detail != detail) {
        m_detail = detail;
        update();
    }
}

void HoldButton::setReason(const QString& reason) {
    if (m_reason == reason) {
        return;
    }
    m_reason = reason;
    if (!reason.isEmpty()) {
        stop();
        m_armed = false;
    }
    setEnabled(reason.isEmpty());
    setAccessibleDescription(reason.isEmpty() ? tr("Hold for about a second to confirm") : reason);
    update();
}

void HoldButton::setResult(const QString& result) {
    m_result = result;
    update();
}

double HoldButton::progress() const {
    return m_holding ? std::clamp(m_since.elapsed() / static_cast<double>(kHoldMs), 0.0, 1.0) : 0.0;
}

bool HoldButton::onScreen() const {
    return isVisible() && isEnabled() && window()->isVisible() && !visibleRegion().isEmpty();
}

bool HoldButton::begin() {
    // r5-fix3-system: never start on a button that isn't on screen.
    if (!isEnabled() || !isVisible()) {
        return false;
    }
    if (m_gate) {
        const QString refusal = m_gate();
        if (!refusal.isEmpty()) {
            emit refused(refusal);
            return false;
        }
    }
    m_armed = false;
    m_disarm.stop();
    m_result.clear();
    m_holding = true;
    m_since.start();
    m_tick.start();
    update();
    return true;
}

void HoldButton::stop() {
    // Let go early: snap back, nothing happens.
    m_holding = false;
    m_tick.stop();
    update();
}

void HoldButton::fire() {
    m_tick.stop();
    m_holding = false;
    m_armed = false;
    m_disarm.stop();
    update();
    // r5-fix3-system: a hold or a second press never completes on a button
    // the DJ can't see (another page, scrolled away, hidden meanwhile).
    if (!onScreen()) {
        return;
    }
    emit confirmed();
}

bool HoldButton::knobPress() {
    if (!isEnabled()) {
        return false;
    }
    if (m_gate) {
        const QString refusal = m_gate();
        if (!refusal.isEmpty()) {
            m_armed = false;
            emit refused(refusal);
            update();
            return false;
        }
    }
    if (m_armed) {
        // Second press within 3 s: the knob's version of holding.
        fire();
        return false;
    }
    m_armed = true;
    m_disarm.start();
    update();
    return false;
}

bool HoldButton::knobHold(bool down) {
    if (down) {
        return begin();
    }
    if (m_holding) {
        stop();
    }
    return true;
}

void HoldButton::knobCancel() {
    m_armed = false;
    m_disarm.stop();
    stop();
}

void HoldButton::mousePressEvent(QMouseEvent* event) {
    // Not a click: only a completed hold acts.
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    begin();
    event->accept();
}

void HoldButton::mouseReleaseEvent(QMouseEvent* event) {
    if (m_holding) {
        stop();
    }
    event->accept();
}

void HoldButton::mouseMoveEvent(QMouseEvent* event) {
    // Sliding off the button cancels, like letting go.
    if (m_holding && !rect().contains(event->position().toPoint())) {
        stop();
    }
    event->accept();
}

void HoldButton::hideEvent(QHideEvent* event) {
    m_armed = false;
    m_disarm.stop();
    stop();
    TouchButton::hideEvent(event);
}

void HoldButton::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const bool enabled = isEnabled();
    painter.setPen(QColor(m_armed || m_holding ? kText : kLine));
    painter.setBrush(QColor(m_holding ? kRaise : kPanel));
    painter.drawRoundedRect(frame, 4, 4);
    const double sweep = progress();
    QRectF filled = frame;
    filled.setWidth(frame.width() * sweep);
    if (sweep > 0) {
        // The white fill sweeps left to right like a playhead.
        QPainterPath clip;
        clip.addRoundedRect(frame, 4, 4);
        painter.save();
        painter.setClipPath(clip);
        painter.fillRect(filled, QColor(kText));
        painter.restore();
    }
    const auto content = [&](bool ink) {
        const QColor text = ink ? QColor(kGround) : QColor(enabled ? kText : kDim);
        const QColor second = ink ? QColor(kGround) : QColor(kMuted);
        if (!m_row) {
            painter.setFont(uiFont(font(), 16, QFont::DemiBold));
            painter.setPen(text);
            const QString label = m_armed ? tr("Press again") : m_result.isEmpty() ? QPushButton::text() : m_result;
            painter.drawText(rect().adjusted(8, 0, -8, 0),
                    Qt::AlignCenter,
                    QFontMetrics(painter.font()).elidedText(label, Qt::ElideRight, std::max(0, width() - 16)));
            return;
        }
        paintIcon(painter, QRectF(16, (height() - 24) / 2.0, 24, 24), m_icon, text);
        const int right = width() - 132;
        const QFont titleFont = capsFont(font(), 16);
        painter.setFont(titleFont);
        painter.setPen(text);
        painter.drawText(QRect(52, 8, std::max(0, right - 52), 26),
                Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(titleFont).elidedText(QPushButton::text(), Qt::ElideRight, std::max(0, right - 52)));
        const QString line = !m_result.isEmpty() ? m_result : !m_reason.isEmpty() ? m_reason : m_detail;
        const QFont lineFont = uiFont(font(), 15, QFont::Medium);
        painter.setFont(lineFont);
        // A reason (locked) prints in text colour: it is the thing to read.
        painter.setPen(!ink && (!m_reason.isEmpty() || !m_result.isEmpty()) ? QColor(kText) : second);
        painter.drawText(QRect(52, 34, std::max(0, width() - 68), 22),
                Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(lineFont).elidedText(line, Qt::ElideRight, std::max(0, width() - 68)));
        painter.setFont(capsFont(font(), 14));
        painter.setPen(ink ? QColor(kGround) : QColor(enabled ? (m_armed ? kText : kMuted) : kDim));
        painter.drawText(QRect(right, 8, 116, 26), Qt::AlignRight | Qt::AlignVCenter,
                m_armed ? tr("Press again") : tr("Hold"));
    };
    content(false);
    if (sweep > 0) {
        painter.save();
        painter.setClipRect(filled);
        content(true);
        painter.restore();
    }
}

GlyphButton::GlyphButton(Glyph glyph, const QString& accessibleName, QWidget* parent, const QString& objectName,
        int size)
        : TouchButton(QString(), parent, size),
          m_glyph(glyph) {
    setObjectName(objectName);
    setAccessibleName(accessibleName);
    setFixedWidth(size);
}

void GlyphButton::setGlyph(Glyph glyph) {
    if (m_glyph != glyph) {
        m_glyph = glyph;
        update();
    }
}

void GlyphButton::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QColor(isDown() ? kText : kLine));
    painter.setBrush(QColor(isDown() ? kRaise : kPanel));
    painter.drawRoundedRect(frame, 4, 4);
    const double size = std::min(24, std::min(width(), height()) - 12);
    paintGlyph(painter,
            QRectF((width() - size) / 2.0, (height() - size) / 2.0, size, size),
            m_glyph,
            QColor(isEnabled() ? kText : kDim));
}

} // namespace decklab::ui
