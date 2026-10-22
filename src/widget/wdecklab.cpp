// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/wdecklab.h"
#include "widget/decklabcombodelegate.h"
#include "widget/decklabdeckwidgets.h"

#include <QApplication>
#include <QBoxLayout>
#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QFileInfo>
#include <QEvent>
#include <QGridLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QMouseEvent>
#include <QButtonGroup>
#include <QPushButton>
#include <QProgressBar>
#include <QWidgetAction>
#include <QScrollArea>
#include <QScroller>
#include <QSlider>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStorageInfo>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTextBrowser>
#include <algorithm>
#include <cmath>
#include <tuple>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "decklab/decklabsystempages.h"
#include "decklab/decklabsystemui.h"
#include "effects/effectsmanager.h"
#include "effects/effectchain.h"
#include "effects/effectslot.h"
#include "library/basetracktablemodel.h"
#include "library/library.h"
#include "library/sidebarmodel.h"
#include "library/trackmodel.h"
#include "mixer/basetrackplayer.h"
#include "mixer/playermanager.h"
#include "mixer/previewdeck.h"
#include "preferences/colorpalettesettings.h"
#include "track/keyutils.h"
#include "track/cueinfo.h"
#include "track/track.h"
#include "track/beats.h"
#include "waveform/waveformwidgetfactory.h"
#include "widget/decklabbrowse.h"
#include "widget/decklabstempads.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarysidebar.h"
#include "widget/wsearchlineedit.h"
#include "widget/wtracktableview.h"
#include "moc_wdecklab.cpp"

namespace {
const QString kNative = QStringLiteral("[DeckLabNative]");
const QString kTab = QStringLiteral("[Tab]");
const QString kStyle = QStringLiteral(R"(
 WDeckLab {background:#0D0F12;color:#F2F4F7;font-family:"Barlow","Open Sans",sans-serif;font-size:16px;font-weight:500;}
 WDeckLab QWidget {color:#F2F4F7;font-size:16px;}
 WDeckLab QPushButton {background:#16191E;border:1px solid #2C313A;border-radius:2px;padding:5px 9px;min-height:28px;font-weight:600;}
 WDeckLab QPushButton:pressed {background:#20242B;}
 WDeckLab QPushButton:checked {background:#20242B;border-color:#F2F4F7;}
 WDeckLab QPushButton[transport="true"] {border-radius:0;border-width:1px 1px 1px 0;}
 WDeckLab QPushButton[transport="true"][first="true"] {border-left-width:1px;}
 WDeckLab QPushButton[actionFeedback="true"] {border-color:#F2F4F7;background:#20242B;}
 WDeckLab QPushButton:disabled {color:#6B7380;border-color:#20242B;}
 WDeckLab QPushButton[rail="true"] {color:#9AA3AF;border-width:0px 1px 1px 0px;border-radius:0px;font-weight:600;}
 WDeckLab QPushButton[rail="true"]:checked {color:#F2F4F7;background:#16191E;border-bottom:3px solid #F2F4F7;}
 WDeckLab QLabel[muted="true"] {color:#9AA3AF;font-size:15px;}
 WDeckLab QLabel[deck="true"] {font-size:18px;font-weight:600;}
 WDeckLab QLabel[numbers="true"] {font-size:24px;font-weight:600;}
 WDeckLab QLabel[deckIdentity="true"] {font-size:24px;font-weight:600;border-right:1px solid #2C313A;}
 WDeckLab QComboBox {background:#222;border:1px solid #484848;border-radius:2px;min-height:38px;padding-left:10px;}
 WDeckLab QComboBox QAbstractItemView {background:#181818;selection-background-color:#333;min-height:40px;}
 WDeckLab QComboBox QAbstractItemView::item {min-height:40px;}
 WDeckLab QSlider::groove:horizontal {height:8px;background:#444;border-radius:2px;}
 WDeckLab QSlider::handle:horizontal {background:#eee;border:1px solid #777;width:24px;margin:-12px 0;border-radius:2px;}
 WDeckLab QProgressBar {border:0;background:#303030;}
 WDeckLab QProgressBar::chunk {background:#ddd;}
 WDeckLab QProgressBar[clipping="true"] {border:1px solid #d74448;}
 WDeckLab QLabel[clipping="true"] {color:#fff;border:1px solid #d74448;}
 WDeckLab QTreeView,WDeckLab QTableView {background:#111;alternate-background-color:#191919;border:0;selection-background-color:#333;selection-color:white;}
 WDeckLab QTreeView::item {min-height:44px;padding-left:8px;border-bottom:1px solid #282828;}
 WDeckLab QHeaderView::section {background:#222;color:#aaa;border:0;border-right:1px solid #444;padding:5px;}
 WDeckLab QScrollBar:vertical {width:10px;background:#111;}
 WDeckLab QScrollBar::handle:vertical {background:#666;min-height:24px;}
 WDeckLab QScrollArea, WDeckLab QScrollArea > QWidget, WDeckLab QScrollArea > QWidget > QWidget {background:#101010;border:0;}
 WDeckLab QMenu {background:#191919;border:1px solid #666;}
 WDeckLab QMenu::item {padding:13px 20px;}
 WDeckLab QMenu::item:selected {background:#333;}
 )");
// Brand tokens: ground #0D0F12, panel #16191E, raise #20242B, line #2C313A,
// muted #9AA3AF, text #F2F4F7. Selection is white or a raised fill; live red
// #FF3D2E is reserved for playhead, recording, clip and end-of-track.
// Tab rail: 44 px targets, the active page in white with a 3 px white
// underline. min/max heights pin the polished border box to 44 px.
const QString kRailStyle = QStringLiteral(R"(
 QWidget#decklab-rail {background:#16191E;}
 QPushButton[rail="true"] {background:#16191E;color:#9AA3AF;border:0px;border-right:1px solid #2C313A;
     border-bottom:1px solid #2C313A;border-radius:0px;min-height:43px;max-height:43px;padding:0px 6px;
     font-family:"Barlow";font-size:16px;font-weight:600;}
 QPushButton[rail="true"]:pressed {background:#20242B;}
 QPushButton[rail="true"]:checked {color:#F2F4F7;background:#16191E;border-bottom:3px solid #F2F4F7;}
 QPushButton[rail="true"]:disabled {color:#4B525C;}
 QPushButton[rail="true"][live="true"] {background:#20242B;color:#F2F4F7;}
 )");
// Decks page surfaces: ground with 1-2 px line gaps between waveforms/cards.
const QString kDeckStyle = QStringLiteral(R"(
 QWidget#decklab-wave-zone {background:#0D0F12;}
 QWidget#decklab-deck-row {background:#2C313A;}
 QWidget[deckCard="true"] {background:#0D0F12;}
 QWidget[deckSideRow="true"] {background:#0D0F12;}
 )");
// Lit touch controls. Each button carries its own sheet so the same rules
// also apply inside popups, and min/max heights keep a 44 px border box.
// On = raised fill with white text and outline; Play adds its green lamp.
// Text sizes follow the on-device scale: buttons 16 px SemiBold.
const QString kActionStyle = QStringLiteral(R"(
 QPushButton {background:#16191E;color:#C9CED6;border:1px solid #2C313A;border-radius:3px;
     min-height:42px;max-height:42px;padding:0px 2px;font-family:"Barlow";font-size:16px;font-weight:600;}
 QPushButton:pressed {background:#20242B;}
 QPushButton:checked {background:#20242B;border-color:#F2F4F7;color:#F2F4F7;}
 QPushButton[lamp="play"]:checked {background:#20242B;border-color:#2FD27A;color:#F2F4F7;}
 QPushButton[lamp="cue"]:pressed {background:#20242B;border-color:#FF7A1A;}
 QPushButton[actionFeedback="true"] {background:#2C313A;border-color:#F2F4F7;color:#F2F4F7;}
 QPushButton:disabled {background:#111418;color:#4B525C;border-color:#1E2228;}
 )");
const QString kPadStyle = QStringLiteral(R"(
 QPushButton {background:#16191E;color:#9AA3AF;border:1px solid #2C313A;border-radius:3px;
     min-height:42px;max-height:42px;padding:0px;font-family:"Barlow";font-size:16px;font-weight:600;}
 QPushButton:pressed {background:#20242B;}
 QPushButton:disabled {background:#111418;color:#4B525C;border-color:#1E2228;}
 )");
// Deck dialogs (track information, beatgrid, controller FX details): body
// 16 px, headings 18 px SemiBold, 44 px buttons.
const QString kDialogStyle = QStringLiteral(R"(
 QDialog {background:#0D0F12;color:#F2F4F7;border:1px solid #2C313A;}
 QDialog QWidget {font-family:"Barlow";font-size:16px;font-weight:500;}
 QLabel {color:#F2F4F7;}
 QLabel[heading="true"] {font-size:18px;font-weight:600;}
 QTextBrowser {background:#16191E;color:#F2F4F7;border:1px solid #2C313A;}
 QPushButton {background:#16191E;color:#F2F4F7;border:1px solid #2C313A;border-radius:3px;
     min-height:42px;max-height:42px;padding:0px 8px;font-weight:600;}
 QPushButton:pressed {background:#20242B;}
 QPushButton:checked {background:#20242B;border-color:#F2F4F7;}
 QPushButton:disabled {color:#4B525C;border-color:#1E2228;}
 )");
const QColor kFxLamp(0xf2, 0xf4, 0xf7);
// Layer follow ignores controller traffic right after start-up and right
// after a track load (mapping initialisation and load-time engine resets).
constexpr qint64 kFollowStartupGraceMs = 1200;
// Whole-track overview height on the Decks page (room for 14 px cue labels).
constexpr int kOverviewHeight = 44;
constexpr qint64 kFollowLoadGraceMs = 600;
// Both layers of one side written together (a mapping that mirrors decks)
// never flips the screen: the shown deck must have been idle this long.
constexpr qint64 kFollowHoldMs = 400;
QVBoxLayout* column(QWidget* widget, int margin = 0) {
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(margin, margin, margin, margin);
    layout->setSpacing(0);
    return layout;
}
QHBoxLayout* row(QWidget* widget = nullptr, int spacing = 0) {
    auto* layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(spacing);
    return layout;
}
// Guarded presentation writes. Several Qt setters repolish, relayout or post
// accessibility events even for an unchanged value, so every Decks refresh
// goes through these and is a visual no-op when nothing changed.
void setTextIf(QLabel* label, const QString& text) {
    if (label && label->text() != text) label->setText(text);
}
void setTextIf(QAbstractButton* button, const QString& text) {
    if (button && button->text() != text) button->setText(text);
}
void setToolTipIf(QWidget* widget, const QString& tip) {
    if (widget && widget->toolTip() != tip) widget->setToolTip(tip);
}
void setAccessibleNameIf(QWidget* widget, const QString& name) {
    if (widget && widget->accessibleName() != name) widget->setAccessibleName(name);
}
void setAccessibleDescriptionIf(QWidget* widget, const QString& text) {
    if (widget && widget->accessibleDescription() != text) widget->setAccessibleDescription(text);
}
void setVisibleIf(QWidget* widget, bool visible) {
    // Mirrors QWidget's own early return, including widgets never shown yet.
    if (!widget || (widget->testAttribute(Qt::WA_WState_ExplicitShowHide) &&
                           widget->isHidden() == !visible)) return;
    widget->setVisible(visible);
}
void setEnabledIf(QWidget* widget, bool enabled) {
    if (widget && widget->testAttribute(Qt::WA_ForceDisabled) == enabled) widget->setEnabled(enabled);
}
void setCheckedIf(QAbstractButton* button, bool checked) {
    if (button && button->isChecked() != checked) button->setChecked(checked);
}
void setStyleSheetIf(QWidget* widget, const QString& style) {
    if (widget && widget->styleSheet() != style) widget->setStyleSheet(style);
}
// Deck readouts are painted QPushButtons. Any surface or skin sheet with a
// QPushButton min-height would override setFixedHeight() on polish, so each
// readout carries its own sheet with paired min/max heights.
QString deckStripSheet(int height) {
    return QStringLiteral("QPushButton{min-height:%1px;max-height:%1px;padding:0px;margin:0px;border:0px;}")
            .arg(height);
}
void setFixedHeightIf(QWidget* widget, int height) {
    if (widget && (widget->minimumHeight() != height || widget->maximumHeight() != height))
        widget->setFixedHeight(height);
}
void setFixedWidthIf(QWidget* widget, int width) {
    if (widget && (widget->minimumWidth() != width || widget->maximumWidth() != width))
        widget->setFixedWidth(width);
}
void setFocusPolicyIf(QWidget* widget, Qt::FocusPolicy policy) {
    if (widget && widget->focusPolicy() != policy) widget->setFocusPolicy(policy);
}
void setWordWrapIf(QLabel* label, bool wrap) {
    if (label && label->wordWrap() != wrap) label->setWordWrap(wrap);
}
// Dynamic style properties only take effect after a repolish; never repolish
// for a property that did not change.
void setStylePropertyIf(QWidget* widget, const char* name, bool value) {
    if (!widget || widget->property(name).toBool() == value) return;
    widget->setProperty(name, value);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}
// Calls back when a watched widget's size changes, so width-dependent elision
// is refreshed by layout settling rather than by polling.
class DeckLabResizeWatcher final : public QObject {
  public:
    DeckLabResizeWatcher(QWidget* target, std::function<void()> changed)
            : QObject(target), m_changed(std::move(changed)) {
        target->installEventFilter(this);
    }
  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Resize) m_changed();
        return QObject::eventFilter(watched, event);
    }
  private:
    std::function<void()> m_changed;
};
// Needle search on a whole-track overview. Every pointer event is consumed
// here, so the overview never receives drags, drops, hover or menus; a
// touch or drag seeks only while allowed() holds (deck loaded and paused).
class DeckLabNeedleSearch final : public QObject {
  public:
    DeckLabNeedleSearch(QWidget* target, std::function<bool()> allowed, std::function<void(double)> seek)
            : QObject(target), m_allowed(std::move(allowed)), m_seek(std::move(seek)) {
        target->installEventFilter(this);
    }
  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        auto* target = static_cast<QWidget*>(watched);
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            m_dragging = mouse->button() == Qt::LeftButton && m_allowed();
            if (m_dragging) seekTo(target, mouse->position().x());
            return true;
        }
        case QEvent::MouseMove: {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (m_dragging && (mouse->buttons() & Qt::LeftButton)) {
                if (m_allowed()) seekTo(target, mouse->position().x());
                else m_dragging = false;
            }
            return true;
        }
        case QEvent::MouseButtonRelease:
            m_dragging = false;
            return true;
        case QEvent::Wheel:
        case QEvent::ContextMenu:
            return true;
        case QEvent::Hide:
            m_dragging = false;
            break;
        default:
            break;
        }
        return QObject::eventFilter(watched, event);
    }
  private:
    void seekTo(QWidget* target, double x) {
        m_seek(std::clamp(x / std::max(1, target->width()), 0.0, 1.0));
    }
    std::function<bool()> m_allowed;
    std::function<void(double)> m_seek;
    bool m_dragging = false;
};
// Qt can synthesize a button release after a recognized touch scroll. Keep
// that release from becoming a command on the control beneath the swipe.
class DeckLabTouchScrollGuard final : public QObject {
  public:
    DeckLabTouchScrollGuard(QWidget* viewport, QWidget* content)
            : QObject(viewport), m_scroller(QScroller::scroller(viewport)) {
        QScroller::grabGesture(viewport, QScroller::TouchGesture);
        connect(m_scroller, &QScroller::stateChanged, this, [this](QScroller::State state) {
            if (state == QScroller::Dragging) m_touchDragged = true;
        });
        viewport->installEventFilter(this);
        if (content != viewport) content->installEventFilter(this);
        for (auto* child : content->findChildren<QWidget*>()) child->installEventFilter(this);
    }
  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->source() != Qt::MouseEventNotSynthesized) {
                const auto state = m_scroller->state();
                const bool press = event->type() == QEvent::MouseButtonPress;
                if (press && (state == QScroller::Pressed || state == QScroller::Inactive))
                    m_touchDragged = false;
                const bool suppress = m_touchDragged || state == QScroller::Dragging || state == QScroller::Scrolling;
                // A slow drag may already be Inactive at release. Retain the
                // recognized drag until that synthesized release is consumed.
                if (!press) m_touchDragged = false;
                if (suppress) {
                    if (auto* button = qobject_cast<QAbstractButton*>(watched)) button->setDown(false);
                    if (!press) {
                        if (auto* viewport = qobject_cast<QWidget*>(watched);
                                viewport && qobject_cast<QAbstractItemView*>(viewport->parentWidget())) {
                            // Item views must receive a release to clear their
                            // native press/drag state. Cancel outside every row
                            // so the swipe cannot click an index. This internal
                            // nonsynthesized event bypasses this filter.
                            const QPoint outside(-10, -10);
                            QMouseEvent cancel(QEvent::MouseButtonRelease, outside,
                                    viewport->mapToGlobal(outside), mouse->button(),
                                    mouse->buttons(), mouse->modifiers());
                            QCoreApplication::sendEvent(viewport, &cancel);
                        }
                    }
                    return true;
                }
            }
        }
        return QObject::eventFilter(watched, event);
    }
  private:
    QScroller* m_scroller;
    bool m_touchDragged = false;
};
// Mouse events also receive Qt's synthesized primary touchscreen contact.
// TouchCancel and application deactivation are handled by the owning surface.
class DeckLabXY final : public QWidget {
  public:
    DeckLabXY(std::function<void(double, double, bool)> send, QWidget* parent)
            : QWidget(parent), m_send(std::move(send)) { setMinimumHeight(70); setAccessibleName("Hold XY filter, release returns dry"); }
  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.fillRect(rect(), QColor("#181818"));
        p.setPen(QColor("#444")); p.drawRect(rect().adjusted(0, 0, -1, -1));
        p.drawLine(width()/2, 0, width()/2, height()); p.drawLine(0, height()/2, width(), height()/2);
        p.setPen(QColor("#aaa")); p.drawText(rect().adjusted(10, 7, -10, -7), Qt::AlignTop | Qt::AlignHCenter, tr("Hold to filter · up = depth · release = dry"));
        if (m_down) { p.setPen(QPen(QColor("#d74448"), 2)); p.drawEllipse(m_point, 10, 10); }
    }
    void mousePressEvent(QMouseEvent* e) override { if (e->button() == Qt::LeftButton) { m_down = true; send(e); } }
    void mouseMoveEvent(QMouseEvent* e) override { if (m_down) send(e); }
    void mouseReleaseEvent(QMouseEvent*) override { if (m_down) { m_down = false; m_send(0, 0, false); update(); } }
    void hideEvent(QHideEvent* e) override { if (m_down) { m_down = false; m_send(0, 0, false); } QWidget::hideEvent(e); }
  private:
    void send(QMouseEvent* e) { m_point = e->pos(); m_send(std::clamp(m_point.x() / double(std::max(1, width())), 0.0, 1.0),
            1 - std::clamp(m_point.y() / double(std::max(1, height())), 0.0, 1.0), true); update(); }
    std::function<void(double, double, bool)> m_send;
    bool m_down = false;
    QPoint m_point;
};
QString timeText(double seconds) {
    seconds = std::max(0.0, seconds);
    return QStringLiteral("%1:%2").arg(int(seconds) / 60).arg(int(seconds) % 60, 2, 10, QLatin1Char('0'));
}
QString beatLoopSizeText(double size) {
    if (!util_isfinite(size) || size <= 0) return QStringLiteral("—");
    const double reciprocal = 1.0 / size;
    return size < 1 && util_isfinite(reciprocal) && reciprocal <= 1000000 &&
                    std::abs(reciprocal - std::round(reciprocal)) < 1e-6
            ? QStringLiteral("1/%1").arg(qint64(std::round(reciprocal)))
            : QString::number(size, 'g', 3);
}

// Keep the full touch target dark; the cue's saved color belongs to its letter
// badge and waveform marker, not to the whole instrument panel.
class DeckLabHotcuePad final : public QPushButton {
  public:
    using QPushButton::QPushButton;
  protected:
    void paintEvent(QPaintEvent*) override {
        QStyleOptionButton option; initStyleOption(&option); option.text.clear();
        QPainter painter(this); style()->drawControl(QStyle::CE_PushButton, &option, &painter, this);
        if (!property("cueSet").toBool()) {
            painter.setPen(QColor(isEnabled() ? "#9aa3af" : "#4b525c"));
            painter.drawText(rect(), Qt::AlignCenter, text());
            return;
        }
        const QColor color(property("cueColor").toString());
        const int badgeWidth = text().size() > 1 ? 36 : 26;
        const QRect badge((width() - badgeWidth) / 2, (height() - 26) / 2, badgeWidth, 26);
        painter.fillRect(badge, color);
        // A neutral rim also makes deliberately black/white imported cues visible.
        painter.setPen(QColor("#888888")); painter.drawRect(badge.adjusted(0, 0, -1, -1));
        const auto linear = [](double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
        const double luminance = 0.2126 * linear(color.redF()) +
                0.7152 * linear(color.greenF()) + 0.0722 * linear(color.blueF());
        painter.setPen(luminance > 0.179 ? Qt::black : Qt::white);
        painter.drawText(badge, Qt::AlignCenter, text());
    }
};

class DeckLabPad final : public QPushButton {
  public:
    using QPushButton::QPushButton;
  protected:
    void paintEvent(QPaintEvent*) override {
        QStyleOptionButton option; initStyleOption(&option); option.text.clear();
        QPainter painter(this); style()->drawControl(QStyle::CE_PushButton, &option, &painter, this);
        painter.setPen(QColor("#aaa")); painter.drawText(rect().adjusted(10, 6, -10, -6), Qt::AlignTop | Qt::AlignLeft, text().section(QChar(10), 0, 0));
        painter.setPen(QColor("#eee"));
        painter.drawText(rect().adjusted(10, 24, -10, -8), Qt::AlignCenter,
                fontMetrics().elidedText(text().section(QChar(10), 1), Qt::ElideRight, std::max(0, width() - 20)));
    }
};
}

// Browse page state (browse-ui). Lives here so other pages never see it.
struct DeckLabBrowseState {
    enum class List {
        Sources,
        Categories,
        Values,
    };
    List list = List::Sources;
    QPersistentModelIndex source; // Collection root or Rekordbox USB device
    bool rekordbox = false;
    int trackCount = -1;
    decklab::Category category = decklab::Category::AllTracks;
    QString valueLabel;
    QString filter; // Category query currently applied to the track view
    bool fromCategories = false;
    QPersistentModelIndex returnRoot; // Sidebar folder opened from a category
    int categoryRow = 0;
    TrackPointer selectedTrack;
    bool matchEnabled = true;
    QTimer matchTimer;
    QTimer loadingTimer;
    QStackedWidget* listStack = nullptr;
    QListWidget* items = nullptr;
    DeckLabBreadcrumb* breadcrumb = nullptr;
    DeckLabTrackCard* card = nullptr;
    DeckLabActionButton* preview = nullptr;
    DeckLabActionButton* match = nullptr;
    DeckLabToast* toast = nullptr;
};

WDeckLab::WDeckLab(UserSettingsPointer config, Library* library,
        PlayerManagerInterface* players, EffectsManager* effects, QWidget* parent)
        : QWidget(parent), WBaseWidget(this), m_config(config), m_library(library),
          m_players(players), m_effects(effects) {
    setObjectName(QStringLiteral("DeckLabNative"));
    // Barlow and Barlow Semi Condensed ship with the skin (OFL).
    deckLabLoadFonts(m_config->getResourcePath());
    setStyleSheet(kStyle);
    setAttribute(Qt::WA_StyledBackground);
    setMinimumSize(800, 480);
    m_clock.start();
    m_sideBySideWaveforms = m_config->getValue(ConfigKey("[DeckLab]", "WaveformLayout"), 0) == 1;
    for (const auto& name : {"active", "browse_press", "browse_back", "browse_sources", "tool_request", "tool_serial", "tools_ready", "layout_decks"}) {
        createControl(kNative, QString::fromLatin1(name));
    }
    m_toolSerial = static_cast<unsigned int>(value(kNative, "tool_serial"));
    // A QA profile must not allow the host helper to edit the normal profile.
    QString profilePath = QDir::cleanPath(m_config->getSettingsPath());
    if (profilePath == QDir::homePath() + "/.mixxx")
        profilePath = QDir::homePath() + "/.var/app/org.mixxx.Mixxx/.mixxx";
    quint32 profileTag = 2166136261u;
    for (unsigned char byte : profilePath.toUtf8()) { profileTag ^= byte; profileTag *= 16777619u; }
    createControl(kNative, "profile_tag", profileTag);
    set(kNative, "profile_tag", profileTag);
    createControl(kTab, QStringLiteral("current"));
    createControl(QStringLiteral("[Sidebar]"), QStringLiteral("sidebar_visible"), 1);
    createControl(QStringLiteral("[DeckLabLeft]"), QStringLiteral("current"));
    createControl(QStringLiteral("[DeckLabRight]"), QStringLiteral("current"));
    // Resolve every per-deck control once; refreshes read cached proxies and
    // never build group/item strings on the hot path.
    for (int deck = 1; deck <= 4; ++deck) {
        const QString group = channel(deck);
        auto& d = m_deck[deck - 1];
        d.play = control(group, QStringLiteral("play"));
        d.loaded = control(group, QStringLiteral("track_loaded"));
        d.bpm = control(group, QStringLiteral("bpm"));
        d.localBpm = control(group, QStringLiteral("local_bpm"));
        d.rateRatio = control(group, QStringLiteral("rate_ratio"));
        d.rateRange = control(group, QStringLiteral("rateRange"));
        d.duration = control(group, QStringLiteral("duration"));
        d.position = control(group, QStringLiteral("playposition"));
        d.samples = control(group, QStringLiteral("track_samples"));
        d.key = control(group, QStringLiteral("visual_key"));
        d.sync = control(group, QStringLiteral("sync_enabled"));
        d.leader = control(group, QStringLiteral("sync_leader"));
        d.keylock = control(group, QStringLiteral("keylock"));
        d.quantize = control(group, QStringLiteral("quantize"));
        d.pfl = control(group, QStringLiteral("pfl"));
        d.loop = control(group, QStringLiteral("loop_enabled"));
        d.loopSize = control(group, QStringLiteral("beatloop_size"));
        d.slip = control(group, QStringLiteral("slip_enabled"));
        d.vu = control(group, QStringLiteral("vu_meter"));
        d.peak = control(group, QStringLiteral("peak_indicator"));
        d.zoom = control(group, QStringLiteral("waveform_zoom"));
        d.cuePoint = control(group, QStringLiteral("cue_point"));
        d.rate = control(group, QStringLiteral("rate"));
        for (int cue = 1; cue <= 8; ++cue) {
            const QString base = QStringLiteral("hotcue_%1_").arg(cue);
            d.cuePosition[cue - 1] = control(group, base + QStringLiteral("position"));
            d.cueType[cue - 1] = control(group, base + QStringLiteral("type"));
            d.cueColor[cue - 1] = control(group, base + QStringLiteral("color"));
        }
        m_cardPrefixes[deck - 1] = QStringLiteral("decklab-card-%1-").arg(deck);
        m_cardHotcuePrefixes[deck - 1] = QStringLiteral("decklab-card-hotcue-%1-").arg(deck);
        m_shownPosition[deck - 1] = -1;
    }
    for (int side = 0; side < 2; ++side) {
        m_sidePrefixes[side] = QStringLiteral("decklab-%1-").arg(side + 1);
        m_sideHotcuePrefixes[side] = QStringLiteral("decklab-hotcue-%1-").arg(side + 1);
    }
    m_mainVu = {control(QStringLiteral("[Main]"), QStringLiteral("vu_meter_left")),
            control(QStringLiteral("[Main]"), QStringLiteral("vu_meter_right"))};
    m_mainPeak = control(QStringLiteral("[Main]"), QStringLiteral("peak_indicator"));
    // Key text uses the configured library key colors, lifted where needed so
    // every key stays readable on the black panel.
    const ColorPalette keyPalette = ColorPaletteSettings(m_config).getConfigKeyColorPalette();
    for (int key = 1; key <= 24; ++key) {
        QColor color = KeyUtils::keyToColor(static_cast<mixxx::track::io::key::ChromaticKey>(key), keyPalette);
        if (!color.isValid()) continue;
        if (color.lightnessF() < 0.6f)
            color = color.hslHueF() < 0 ? QColor(0xd0, 0xd0, 0xd0) : QColor::fromHslF(color.hslHueF(), color.hslSaturationF(), 0.62f);
        m_keyColors[key] = color;
    }
    m_endWarningSeconds = m_config->getValue(ConfigKey("[Waveform]", "EndOfTrackWarningTime"), 30);
    m_root = column(this);
    m_rail = new QWidget(this);
    m_rail->setObjectName(QStringLiteral("decklab-rail"));
    m_rail->setAttribute(Qt::WA_StyledBackground);
    m_rail->setStyleSheet(kRailStyle);
    m_rail->setFixedHeight(44);
    auto* rail = row(m_rail);
    const QStringList names = {tr("Decks"), tr("Browse"), tr("Sampler"), tr("Lab FX"), tr("Settings")};
    rail->addWidget(new DeckLabMark(QDir(m_config->getResourcePath())
                                            .filePath(QStringLiteral("skins/DeckLab/decklab-icon-24.png")),
            m_rail));
    const QStringList triggers = {"overview", "library", "samplers", "effects", "settings"};
    auto* navigation = new QButtonGroup(this);
    for (int page = 0; page < 5; ++page) {
        m_tabs[page] = button(names[page], [this, page] { selectPage(page); }, this,
                QStringLiteral("decklab-tab-%1").arg(page));
        m_tabs[page]->setProperty("rail", true);
        m_tabs[page]->setCheckable(true);
        navigation->addButton(m_tabs[page], page);
        m_tabs[page]->setFixedHeight(44);
        rail->addWidget(m_tabs[page], 1);
        createControl(kTab, triggers[page]);
        watch(kTab, triggers[page], [this, page](double v) { if (v > 0) selectPage(page); });
    }
    auto* usb = button(tr("USB"), [this] { tool(1); }, this, QStringLiteral("decklab-usb"));
    usb->setProperty("rail", true);
    usb->setFixedHeight(44);
    m_toolButtons.append(usb); usb->setEnabled(false);
    rail->addWidget(usb);
    // Controller effect units as lamps; tapping opens the full read-only list.
    m_fxChips = new DeckLabChipButton(m_rail);
    m_fxChips->setObjectName(QStringLiteral("decklab-performance-board-fx"));
    m_fxChips->setProperty("rail", true);
    m_fxChips->setFocusPolicy(Qt::NoFocus);
    m_fxChips->setCaption(tr("FX"));
    m_fxChips->setChips({{QStringLiteral("1"), kFxLamp, false}, {QStringLiteral("2"), kFxLamp, false},
            {QStringLiteral("3"), kFxLamp, false}, {QStringLiteral("4"), kFxLamp, false}});
    m_fxChips->setFixedHeight(44);
    m_fxChips->setAccessibleName(tr("Controller effects; read all unit and routing details"));
    connect(m_fxChips, &QPushButton::clicked, this, [this] { showControllerFxInfo(); });
    rail->addWidget(m_fxChips);
    m_root->addWidget(m_rail);
    m_waveforms = new QWidget(this);
    m_waveforms->setObjectName(QStringLiteral("decklab-decks"));
    m_waveforms->setStyleSheet(kDeckStyle);
    m_waveLayout = new QGridLayout(m_waveforms);
    m_waveLayout->setContentsMargins(0, 0, 0, 0); m_waveLayout->setSpacing(0);
    // Full-width waveforms on top; the per-deck readouts and touch controls
    // below. Each surface keeps one parent for its lifetime: presentations
    // only move it within these grids, never reparent a GL waveform.
    m_waveZone = new QWidget(m_waveforms);
    m_waveZone->setObjectName(QStringLiteral("decklab-wave-zone"));
    m_waveZone->setAttribute(Qt::WA_StyledBackground);
    m_zoneGrid = new QGridLayout(m_waveZone);
    m_zoneGrid->setContentsMargins(0, 0, 0, 0); m_zoneGrid->setSpacing(2);
    m_deckRow = new QWidget(m_waveforms);
    m_deckRow->setObjectName(QStringLiteral("decklab-deck-row"));
    m_deckRow->setAttribute(Qt::WA_StyledBackground);
    m_deckGrid = new QGridLayout(m_deckRow);
    m_deckGrid->setContentsMargins(0, 0, 0, 0); m_deckGrid->setHorizontalSpacing(1); m_deckGrid->setVerticalSpacing(0);
    m_waveLayout->addWidget(m_waveZone, 0, 0);
    m_waveLayout->addWidget(m_deckRow, 1, 0);
    m_waveLayout->setRowStretch(0, 1); m_waveLayout->setRowStretch(1, 0);
    m_pages = new QStackedWidget(this);
    m_pages->setObjectName("decklab-pages");
    m_pages->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    m_pages->addWidget(new QWidget(m_pages));
    m_deckControls = buildDecks();
    m_pages->addWidget(buildBrowse());
    m_pages->addWidget(buildSampler());
    m_pages->addWidget(buildEffects());
    m_pages->addWidget(buildSettings());
    for (auto* combo : findChildren<QComboBox*>()) {
        combo->setItemDelegate(new DeckLabComboDelegate(combo));
        combo->setMaxVisibleItems(6);
    }
    m_root->addWidget(m_waveforms, 1);
    m_root->addWidget(m_pages);
    m_deckControls->hide();
    // Unambiguous lamps: AUDIO OFF, OVERLOAD (recent dropouts), REC, SYSTEM.
    m_status = new DeckLabChipButton(m_rail);
    m_status->setObjectName(QStringLiteral("decklab-status"));
    m_status->setProperty("rail", true);
    m_status->setFocusPolicy(Qt::NoFocus);
    m_status->setFixedHeight(44);
    connect(m_status, &QPushButton::clicked, this, [this] {
        QMessageBox::information(this, tr("Deck Lab status"), m_status->toolTip());
    });
    rail->addWidget(m_status);
    auto* mainLevel = new QWidget(m_rail);
    auto* mainLevelBox = row(mainLevel);
    mainLevelBox->setContentsMargins(6, 6, 6, 6);
    m_masterMeter = new DeckLabLevelMeter(Qt::Vertical, 2, mainLevel);
    m_masterMeter->setObjectName(QStringLiteral("decklab-master-meter"));
    m_masterMeter->setAccessibleName(tr("Main output level"));
    m_masterMeter->setFixedWidth(9);
    m_masterMeter->setLevels(m_mainVu[0]->get(), m_mainVu[1]->get(), m_mainPeak->get() > 0);
    mainLevelBox->addWidget(m_masterMeter);
    rail->addWidget(mainLevel);
    watch(kNative, "browse_press", [this](double v) { if (v > 0) browsePress(); });
    watch(kNative, "browse_back", [this](double v) { if (v > 0) browseBack(); });
    watch(kNative, "browse_sources", [this](double v) {
        if (v <= 0 || !m_sidebar) return;
        selectPage(1); setSearchOpen(false);
        m_sidebar->setRootIndex(QModelIndex());
        m_sidebar->setCurrentIndex(QModelIndex());
        showSources();
    });
    watch(kNative, "tools_ready", [this](double v) {
        if (v > 0) { const bool wasOnline = m_toolsOnline; m_lastToolsReady = m_clock.elapsed(); m_toolsOnline = true; for (auto* b : m_toolButtons) b->setEnabled(true); if (!wasOnline) updateStatus(); }
    });
    watch(kNative, "layout_decks", [this](double v) {
        if (v != 2 && v != 4) return;
        m_autoDeckLayout = int(v); updateLayers();
    });
    watch(kTab, "current", [this](double v) {
        int page = int(v);
        selectPage(page == 5 ? 4 : page == 4 ? 1 : page);
    });
    watch("[Sidebar]", "sidebar_visible", [this](double v) {
        if (m_page == 1) { if (v > 0) showSources(); else showTracks(); }
    });
    for (int deck = 1; deck <= 4; ++deck) {
        for (const auto& key : {"play", "track_loaded"}) {
            watch(channel(deck), QString::fromLatin1(key), [this, deck](double) {
                // Relayout only happens when the layout actually changes;
                // otherwise this just queues one coalesced deck refresh.
                updateLayers(); markDeckDirty(deck); refreshLoadButtons(); refreshFx();
            });
        }
        // Deck state shown on the Decks page. Each change only marks the deck
        // dirty; one queued flush refreshes it however many values changed.
        for (const auto& key : {"bpm", "local_bpm", "rate_ratio", "rateRange",
                     "sync_enabled", "keylock", "quantize", "pfl", "cue_point",
                     "loop_enabled", "beatloop_size", "slip_enabled", "duration"}) {
            watch(channel(deck), QString::fromLatin1(key), [this, deck](double) { markDeckDirty(deck); });
        }
        for (int cue = 0; cue < 8; ++cue) {
            for (auto* proxy : {m_deck[deck - 1].cuePosition[cue], m_deck[deck - 1].cueType[cue],
                         m_deck[deck - 1].cueColor[cue]}) {
                proxy->connectValueChanged(this, [this, deck](double) { markDeckDirty(deck); }, Qt::QueuedConnection);
            }
        }
        // Layer follow: deliberate controller touches on a deck bring it to
        // its side. Only controls the engine never writes count (and play only
        // when it starts), so end-of-track stops or Sync never flip a layer.
        m_lastTouch[deck - 1] = -100000;
        m_lastLoad[deck - 1] = -100000;
        m_lastPlay[deck - 1] = m_deck[deck - 1].play->get() > 0;
        watch(channel(deck), QStringLiteral("play"), [this, deck](double v) {
            const bool started = v > 0 && !m_lastPlay[deck - 1];
            m_lastPlay[deck - 1] = v > 0;
            if (started) noteDeckTouched(deck);
        });
        for (const auto& key : {"cue_default", "scratch2_enable"}) {
            watch(channel(deck), QString::fromLatin1(key), [this, deck](double v) { if (v > 0) noteDeckTouched(deck); });
        }
        for (const auto& key : {"jog", "wheel"}) {
            watch(channel(deck), QString::fromLatin1(key), [this, deck](double v) { if (v != 0) noteDeckTouched(deck); });
        }
        watch(channel(deck), QStringLiteral("volume"), [this, deck](double) { noteDeckTouched(deck); });
        watch(channel(deck), QStringLiteral("rate"), [this, deck](double) {
            // Sync moves a follower's rate itself; only a free deck's tempo
            // fader is a deliberate touch.
            if (m_deck[deck - 1].sync->get() <= 0) noteDeckTouched(deck);
        });
        // Key compatibility compares against the master or another playing deck.
        for (const auto& key : {"play", "sync_leader", "visual_key"}) {
            watch(channel(deck), QString::fromLatin1(key), [this](double) {
                for (int other = 1; other <= 4; ++other) markDeckDirty(other);
            });
        }
        if (m_players) {
            auto* player = m_players->getPlayer(channel(deck));
            if (player) {
                connect(player, &BaseTrackPlayer::newTrackLoaded, this, [this, deck](TrackPointer) {
                    m_tempos[deck - 1].at = -1;
                    m_shownPosition[deck - 1] = -1;
                    m_lastLoad[deck - 1] = m_clock.elapsed();
                    refreshDeck(deck);
                    if (m_page == 1) {
                        setDisplayedDeck((deck - 1) % 2, deck);
                        set(deck % 2 ? "[DeckLabLeft]" : "[DeckLabRight]", "current", deck > 2 ? 1 : 0);
                        updateLayers(); selectPage(0);
                    }
                });
            }
        }
        refreshDeck(deck);
    }
    for (int side = 0; side < 2; ++side) {
        watch(side == 0 ? "[DeckLabLeft]" : "[DeckLabRight]", "current", [this, side](double v) {
            setDisplayedDeck(side, side + 1 + (v > 0 ? 2 : 0)); updateLayers();
        });
    }
    connect(WaveformWidgetFactory::instance(), &WaveformWidgetFactory::waveformMeasured,
            this, [this](float fps, int dropped) { m_measuredFps = fps; m_droppedFrames = dropped; updateStatus(); });
    watch("[App]", "audio_latency_overload_count", [this](double) { updateStatus(); });
    watch("[Recording]", "status", [this](double) { updateStatus(); });
    watch("[SoundManager]", "status", [this](double) { updateStatus(); });
    connect(&m_fxTimer, &QTimer::timeout, this, &WDeckLab::tickEffects);
    m_fxTimer.start(100);
    // Meters are sampled at a fixed display rate instead of following each
    // engine-rate value signal; unchanged segments never repaint.
    connect(&m_meterTimer, &QTimer::timeout, this, &WDeckLab::tickMeters);
    m_meterTimer.start(40);
    qApp->installEventFilter(this);
    set(kNative, "active", 1);
    selectPage(0);
    updateStatus();
}

WDeckLab::~WDeckLab() {
    m_fxTimer.stop();
    m_meterTimer.stop();
    qApp->removeEventFilter(this);
    // QWidget closes focus after derived members have been destroyed. Search
    // emits on focus-out, so detach its callback and hide while our controls
    // and page state are still alive (including during a skin replacement).
    if (m_search) disconnect(m_search, nullptr, this, nullptr);
    if (m_sidebar && m_sidebar->selectionModel()) disconnect(m_sidebar->selectionModel(), nullptr, this, nullptr);
    if (m_browseState && m_browseState->items) disconnect(m_browseState->items, nullptr, this, nullptr);
    hide();
    releaseTouchCue();
    releaseEffects();
    set(kNative, "active", 0);
}

QString WDeckLab::channel(int deck) const {
    // Shared, preformatted group names: refreshes copy an implicitly shared
    // string instead of formatting a new one for every control lookup.
    static const std::array<QString, 4> groups{QStringLiteral("[Channel1]"),
            QStringLiteral("[Channel2]"), QStringLiteral("[Channel3]"), QStringLiteral("[Channel4]")};
    return deck >= 1 && deck <= 4 ? groups[deck - 1] : QStringLiteral("[Channel%1]").arg(deck);
}

void WDeckLab::createControl(const QString& group, const QString& item, double initial) {
    if (!ControlObject::getControl(ConfigKey(group, item), ControlFlag::NoWarnIfMissing)) {
        auto object = std::make_unique<ControlObject>(ConfigKey(group, item));
        object->set(initial);
        m_ownedControls.emplace_back(std::move(object));
    }
}

ControlProxy* WDeckLab::control(const QString& group, const QString& item) {
    // Two-level lookup: no concatenated key is allocated per call.
    auto& items = m_controls[group];
    const auto found = items.constFind(item);
    if (found != items.cend()) return found.value();
    auto* proxy = new ControlProxy(group, item, this, ControlFlag::NoWarnIfMissing);
    items.insert(item, proxy);
    return proxy;
}
double WDeckLab::value(const QString& group, const QString& item) { return control(group, item)->get(); }
void WDeckLab::set(const QString& group, const QString& item, double v) {
    // A ControlProxy filters notifications originating from itself. Native
    // widgets also watch these values, so use the shared control write path
    // to deliver queued feedback for both touch and controller changes.
    ControlObject::set(ConfigKey(group, item), v);
}
void WDeckLab::pulse(const QString& group, const QString& item) { set(group, item, 1); set(group, item, 0); }
void WDeckLab::watch(const QString& group, const QString& item, std::function<void(double)> callback) {
    control(group, item)->connectValueChanged(this, std::move(callback), Qt::QueuedConnection);
}
QPushButton* WDeckLab::button(const QString& text, std::function<void()> action,
        QWidget* parent, const QString& name) {
    QPushButton* b;
    if (name.startsWith("decklab-hotcue-") || name.startsWith("decklab-card-hotcue-"))
        b = new DeckLabHotcuePad(text, parent ? parent : this);
    else if (name.startsWith("decklab-sample-")) b = new DeckLabPad(text, parent ? parent : this);
    else b = new QPushButton(text, parent ? parent : this);
    b->setObjectName(name);
    b->setFocusPolicy(Qt::NoFocus);
    b->setAccessibleName(text);
    connect(b, &QPushButton::clicked, this, std::move(action));
    return b;
}

QWidget* WDeckLab::buildTransport(int deck, bool followsLayer, QWidget* parent) {
    auto* strip = new QWidget(parent);
    strip->setAttribute(Qt::WA_StyledBackground);
    strip->setProperty("deckSideRow", true);
    const auto target = [this, deck, followsLayer] { return followsLayer ? displayedDeck(deck - 1) : deck; };
    const QString prefix = followsLayer ? QStringLiteral("decklab-%1-").arg(deck) : QStringLiteral("decklab-card-%1-").arg(deck);
    auto makeAction = [this, strip, target, prefix](const QString& label, const QString& item, bool checked = false) {
        auto pressed = std::make_shared<std::pair<int, quint64>>(0, 0);
        auto* b = button(label, [this, target, prefix, pressed, item] {
            // A click may have toggled a checkable button; never trust the
            // cached presentation after a touch.
            m_transport[prefix].shown = TransportState();
            if (pressed->second != m_interactionEpoch || pressed->first != target() ||
                    value(channel(target()), "track_loaded") <= 0) {
                // QPushButton toggles before clicked; cancel its visual toggle too.
                refreshTransport(target(), prefix);
                return;
            }
            const int actual = pressed->first;
            if (item == "play" || item == "quantize" || item == "keylock")
                set(channel(actual), item, !value(channel(actual), item));
            else if (item == "loop_halve" || item == "loop_double" || item == "beatloop_activate")
                pulse(channel(actual), item);
            else if (item == "beatsync") {
                // Explicit tempo + phase once, independent of Quantize; no lock.
                pulse(channel(actual), QStringLiteral("beatsync_tempo"));
                pulse(channel(actual), QStringLiteral("beatsync_phase"));
                m_syncFeedbackUntil[actual - 1] = m_clock.elapsed() + 600;
            }
            refreshTransport(actual, prefix);
        }, strip, prefix + item);
        // State is shown by lit buttons, never by ON/OFF words.
        b->setFixedHeight(44);
        b->setMinimumWidth(44);
        b->setStyleSheet(kActionStyle);
        if (item == "play") b->setProperty("lamp", QStringLiteral("play"));
        else if (item == "beatloop_activate") b->setProperty("lamp", QStringLiteral("loop"));
        else if (item == "cue_default") b->setProperty("lamp", QStringLiteral("cue"));
        connect(b, &QPushButton::pressed, this, [this, target, pressed] {
            *pressed = {target(), m_interactionEpoch};
        });
        if (item == "cue_default") {
            connect(b, &QPushButton::pressed, this, [this, target] {
                if (value(channel(target()), "track_loaded") <= 0) return;
                releaseTouchCue(); m_touchCue = channel(target()); set(m_touchCue, "cue_default", 1);
            });
            connect(b, &QPushButton::released, this, &WDeckLab::releaseTouchCue);
        }
        b->setCheckable(checked);
        auto& buttons = m_transport[prefix];
        if (item == "play") buttons.play = b;
        else if (item == "cue_default") buttons.cue = b;
        else if (item == "beatsync") buttons.sync = b;
        else if (item == "loop_halve") buttons.halve = b;
        else if (item == "beatloop_activate") buttons.loop = b;
        else if (item == "loop_double") buttons.twice = b;
        else if (item == "quantize") buttons.quantize = b;
        else if (item == "keylock") buttons.keylock = b;
        return b;
    };
    // Linked waveform zoom lives with the other occasional deck tools.
    const auto addZoom = [this, prefix](QWidget* owner, QBoxLayout* rows) {
        auto* zooms = row(nullptr, 4);
        for (const auto& zoom : {qMakePair(tr("Zoom out"), QStringLiteral("waveform_zoom_up")),
                     qMakePair(tr("Zoom in"), QStringLiteral("waveform_zoom_down"))}) {
            auto* b = button(zoom.first, [this, item = zoom.second] {
                for (int side = 0; side < 2; ++side) pulse(channel(displayedDeck(side)), item);
            }, owner, prefix + (zoom.second.endsWith(QStringLiteral("up")) ? QStringLiteral("zoom_out") : QStringLiteral("zoom_in")));
            b->setStyleSheet(kActionStyle); b->setFixedHeight(44); b->setMinimumWidth(86);
            b->setAccessibleName(zoom.second.endsWith(QStringLiteral("up")) ? tr("Zoom all waveforms out") : tr("Zoom all waveforms in"));
            zooms->addWidget(b, 1);
        }
        rows->addLayout(zooms);
    };
    if (followsLayer) {
        // One row per side: loop size, loop, quantize, key lock, one-shot
        // Beat Sync and More. 4 px gaps; stretch keeps proportions on wider
        // screens.
        auto* actions = row(strip, 4);
        actions->setContentsMargins(4, 4, 4, 0);
        auto append = [actions](QPushButton* button, int stretch) {
            actions->addWidget(button, stretch);
        };
        append(makeAction(QString::fromUtf8("½"), QStringLiteral("loop_halve")), 44);
        append(makeAction(tr("LOOP\n4"), QStringLiteral("beatloop_activate"), true), 56);
        append(makeAction(QString::fromUtf8("×2"), QStringLiteral("loop_double")), 44);
        append(makeAction(tr("QNT"), QStringLiteral("quantize"), true), 52);
        append(makeAction(tr("KEY\nLOCK"), QStringLiteral("keylock"), true), 60);
        append(makeAction(tr("BEAT\nSYNC"), QStringLiteral("beatsync")), 64);
        auto* menu = new QMenu(strip); menu->setObjectName(prefix + QStringLiteral("more-menu"));
        auto* fallback = new QWidget(menu); auto* fallbackRows = column(fallback);
        fallbackRows->setSpacing(4);
        auto* play = makeAction(tr("Play"), QStringLiteral("play"), true);
        auto* cue = makeAction(tr("Hold Cue"), QStringLiteral("cue_default"));
        play->setMinimumWidth(176); cue->setMinimumWidth(176);
        fallbackRows->addWidget(play); fallbackRows->addWidget(cue);
        auto* info = button(tr("Track info"), [this, target, menu] {
            menu->close();
            showTrackInfo(target());
        }, fallback, prefix + QStringLiteral("track-info"));
        info->setStyleSheet(kActionStyle);
        info->setFixedHeight(44); fallbackRows->addWidget(info);
        auto* grid = button(tr("Beatgrid"), [this, deck, menu] { menu->close(); showBeatgrid(deck, true); }, fallback, prefix + QStringLiteral("grid"));
        grid->setStyleSheet(kActionStyle);
        grid->setFixedHeight(44); fallbackRows->addWidget(grid);
        addZoom(fallback, fallbackRows);
        auto* widgetAction = new QWidgetAction(menu); widgetAction->setDefaultWidget(fallback); menu->addAction(widgetAction);
        connect(menu, &QMenu::aboutToShow, this, [this, target, prefix] { refreshTransport(target(), prefix); });
        connect(menu, &QMenu::aboutToHide, this, [this, play, cue] {
            releaseTouchCue(); play->setDown(false); cue->setDown(false);
        });
        watch(deck == 1 ? "[DeckLabLeft]" : "[DeckLabRight]", "current", [menu](double) { menu->close(); });
        watch(kNative, "layout_decks", [menu](double) { menu->close(); });
        watch(kTab, "current", [menu](double) { menu->close(); });
        for (int actual : {deck, deck + 2}) watch(channel(actual), "track_loaded", [menu](double) { menu->close(); });
        auto* more = button(tr("MORE"), [menu, strip] { menu->popup(strip->mapToGlobal(QPoint(0, strip->height()))); }, strip, prefix + QStringLiteral("more"));
        more->setFixedHeight(44); more->setMinimumWidth(44); more->setStyleSheet(kActionStyle);
        append(more, 52); m_transport[prefix].more = more;
        strip->setFixedHeight(48);
    } else {
        auto* actions = row(strip, 4);
        actions->setContentsMargins(4, 4, 4, 0);
        actions->addWidget(makeAction(tr("Cue"), QStringLiteral("cue_default")), 1);
        actions->addWidget(makeAction(tr("Play"), QStringLiteral("play"), true), 1);
        actions->addWidget(makeAction(tr("BEAT\nSYNC"), QStringLiteral("beatsync")), 1);
        auto* menu = new QMenu(strip); menu->setObjectName(prefix + QStringLiteral("more-menu"));
        auto* content = new QWidget(menu); auto* rows = column(content);
        rows->setSpacing(4);
        rows->addWidget(buildHotcues(deck, false, content));
        // Stem pads for a stem track (stems workstream); hidden otherwise.
        rows->addWidget(new DeckLabStemPads(deck, DeckLabStemPads::Presentation::Menu, m_players,
                [this] { return m_interactionEpoch; }, content));
        auto* grid = button(tr("Beatgrid"), [this, deck, menu] { menu->close(); showBeatgrid(deck, false); }, content);
        grid->setStyleSheet(kActionStyle);
        grid->setFixedHeight(44); rows->addWidget(grid);
        addZoom(content, rows);
        auto* widgetAction = new QWidgetAction(menu); widgetAction->setDefaultWidget(content); menu->addAction(widgetAction);
        connect(menu, &QMenu::aboutToShow, this, [this, deck] { refreshHotcues(deck, m_cardHotcuePrefixes[deck - 1]); });
        watch(kTab, "current", [menu](double) { menu->close(); });
        watch(kNative, "layout_decks", [menu](double) { menu->close(); });
        watch(channel(deck), "track_loaded", [menu](double) { menu->close(); });
        watch("[DeckLabLeft]", "current", [menu](double) { menu->close(); });
        watch("[DeckLabRight]", "current", [menu](double) { menu->close(); });
        auto* more = button(tr("More"), [menu, strip] { menu->popup(strip->mapToGlobal(QPoint(0, strip->height()))); }, strip, prefix + QStringLiteral("more"));
        more->setStyleSheet(kActionStyle);
        more->setFixedHeight(44); more->setMinimumWidth(44); actions->addWidget(more, 1); m_transport[prefix].more = more;
        strip->setFixedHeight(48);
    }
    return strip;
}

void WDeckLab::showTrackInfo(int actual) {
    auto* player = m_players ? m_players->getPlayer(channel(actual)) : nullptr;
    const auto track = player ? player->getLoadedTrack() : TrackPointer();
    auto* dialog = new QDialog(this, Qt::Dialog | Qt::FramelessWindowHint);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("decklab-track-info"));
    dialog->setStyleSheet(kDialogStyle);
    dialog->setWindowTitle(tr("Deck %1 track information").arg(actual));
    auto* content = column(dialog, 8); content->setSpacing(8);
    auto* heading = new QLabel(tr("DECK LAB · Deck %1 · TRACK INFORMATION").arg(actual), dialog);
    heading->setObjectName(QStringLiteral("decklab-track-info-heading")); heading->setFixedHeight(28);
    heading->setProperty("heading", true);
    content->addWidget(heading);
    auto* text = new QTextBrowser(dialog); text->setReadOnly(true); text->setOpenExternalLinks(false);
    text->setObjectName(QStringLiteral("decklab-track-info-text"));
    text->setPlainText(track ? tr("Title\n%1\n\nArtist\n%2\n\nFile location\n%3")
            .arg(track->getTitle(), track->getArtist(), track->getLocation()) : tr("No track loaded"));
    content->addWidget(text, 1);
    auto* close = button(tr("Close"), [dialog] { dialog->close(); }, dialog, QStringLiteral("decklab-track-info-close"));
    close->setFixedHeight(44); content->addWidget(close);
    dialog->resize(760, 420); dialog->move(mapToGlobal(QPoint((width() - 760) / 2, (height() - 420) / 2)));
    dialog->setWindowModality(Qt::WindowModal); dialog->open();
}

void WDeckLab::showControllerFxInfo() {
    auto* dialog = new QDialog(this, Qt::Dialog | Qt::FramelessWindowHint);
    dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setObjectName(QStringLiteral("decklab-controller-fx-info"));
    dialog->setWindowTitle(tr("Controller effects details"));
    dialog->setStyleSheet(kDialogStyle);
    auto* content = column(dialog, 8); content->setSpacing(8);
    auto* heading = new QLabel(tr("DECK LAB · CONTROLLER FX DETAILS"), dialog); heading->setFixedHeight(28);
    heading->setProperty("heading", true); content->addWidget(heading);
    auto* ownership = new QLabel(tr("Read only · Adjust these effects on your controller or in advanced settings.\nLab FX controls are separate, on the Lab FX screen."), dialog);
    ownership->setObjectName(QStringLiteral("decklab-controller-fx-ownership")); ownership->setWordWrap(true); content->addWidget(ownership);
    auto* text = new QTextBrowser(dialog); text->setReadOnly(true); text->setOpenExternalLinks(false);
    text->setObjectName(QStringLiteral("decklab-controller-fx-text")); content->addWidget(text, 1);
    text->setTextInteractionFlags(Qt::NoTextInteraction);
    QScroller::grabGesture(text->viewport(), QScroller::TouchGesture);
    const auto update = [this, text] {
        QStringList blocks;
        for (int unit = 1; unit <= 4; ++unit) {
            const QString group = QStringLiteral("[EffectRack1_EffectUnit%1]").arg(unit);
            const auto chain = m_effects ? m_effects->getEffectChain(group) : EffectChainPointer();
            if (!chain) { blocks << tr("Unit %1 · unavailable").arg(unit).toHtmlEscaped(); continue; }
            const double mix = value(group, "mix");
            QString block = tr("Unit %1 · %2 · Mix %3").arg(unit).arg(value(group, "enabled") > 0 ? tr("Enabled") : tr("Bypassed"),
                    util_isfinite(mix) ? QString::number(mix * 100, 'f', 0) + QLatin1Char('%') : tr("unavailable")).toHtmlEscaped();
            QStringList routes;
            for (int deck = 1; deck <= 4; ++deck)
                if (value(group, QStringLiteral("group_[Channel%1]_enable").arg(deck)) > 0) routes << QString::number(deck);
            block += QStringLiteral("<br>") + tr("Routed decks: %1").arg(routes.isEmpty() ? tr("none") : routes.join(QStringLiteral(", "))).toHtmlEscaped();
            const auto& effectSlots = chain->getEffectSlots();
            for (int slot = 0; slot < effectSlots.size(); ++slot) {
                const auto effect = effectSlots[slot];
                const bool loaded = effect && effect->isLoaded() && effect->getManifest();
                const QString state = loaded ? (value(QStringLiteral("[EffectRack1_EffectUnit%1_Effect%2]").arg(unit).arg(slot + 1), "enabled") > 0 ? tr("ON") : tr("OFF")) : tr("EMPTY");
                block += QStringLiteral("<br>") + tr("Slot %1: %2 [%3]").arg(slot + 1)
                        .arg(loaded ? effect->getManifest()->name() : tr("No effect"), state).toHtmlEscaped();
            }
            blocks << block;
        }
        text->setHtml(QStringLiteral("<table width='100%' cellspacing='8'><tr><td width='50%' valign='top'>%1</td><td width='50%' valign='top'>%2</td></tr><tr><td valign='top'>%3</td><td valign='top'>%4</td></tr></table>")
                              .arg(blocks[0], blocks[1], blocks[2], blocks[3]));
    };
    auto* actions = row(nullptr, 8);
    auto* refresh = button(tr("Refresh"), update, dialog, QStringLiteral("decklab-controller-fx-refresh")); refresh->setFixedHeight(44); actions->addWidget(refresh, 1);
    auto* close = button(tr("Close"), [dialog] { dialog->close(); }, dialog, QStringLiteral("decklab-controller-fx-close")); close->setFixedHeight(44); actions->addWidget(close, 1); content->addLayout(actions);
    update(); dialog->resize(760, 420); dialog->move(mapToGlobal(QPoint((width() - 760) / 2, (height() - 420) / 2)));
    dialog->setWindowModality(Qt::WindowModal); dialog->open();
}

QWidget* WDeckLab::buildHotcues(int deck, bool followsLayer, QWidget* parent) {
    auto* strip = new QWidget(parent); auto* pads = row(strip, 4);
    strip->setAttribute(Qt::WA_StyledBackground);
    strip->setProperty("deckSideRow", true);
    // Side rows sit 4 px below the overview; popup pads need no margin.
    if (followsLayer) pads->setContentsMargins(4, 4, 4, 0);
    const auto target = [this, deck, followsLayer] { return followsLayer ? displayedDeck(deck - 1) : deck; };
    const QString prefix = followsLayer ? QStringLiteral("decklab-hotcue-%1-").arg(deck) : QStringLiteral("decklab-card-hotcue-%1-").arg(deck);
    std::array<QPushButton*, 8> buttons{};
    struct Press { int deck = 0; quint64 epoch = 0; TrackPointer track; double position = -1; bool playing = false; };
    for (int cue = 1; cue <= 8; ++cue) {
        auto pressed = std::make_shared<Press>();
        auto* pad = button(QString(QChar('A' + cue - 1)), [this, target, prefix, pressed, cue] {
            const int actual = target(); const QString group = channel(actual);
            m_hotcueButtons[prefix].shown = HotcueState();
            auto* player = m_players ? m_players->getPlayer(group) : nullptr;
            const QString base = QStringLiteral("hotcue_%1_").arg(cue);
            const double position = value(group, base + QStringLiteral("position"));
            if (pressed->deck != actual || pressed->epoch != m_interactionEpoch || !player ||
                    pressed->track != player->getLoadedTrack() || value(group, "track_loaded") <= 0 ||
                    pressed->position != position || pressed->playing != (value(group, "play") > 0)) {
                refreshHotcues(actual, prefix); return;
            }
            if (position >= 0) pulse(group, base + QStringLiteral("goto"));
            else if (!pressed->playing) pulse(group, base + QStringLiteral("set"));
            refreshHotcues(actual, prefix);
        }, strip, prefix + QString::number(cue));
        pad->setFixedHeight(44); pad->setMinimumWidth(44); pad->setFocusPolicy(Qt::NoFocus);
        pad->setStyleSheet(kPadStyle);
        connect(pad, &QPushButton::pressed, this, [this, target, pressed, cue] {
            pressed->deck = target(); pressed->epoch = m_interactionEpoch;
            auto* player = m_players ? m_players->getPlayer(channel(pressed->deck)) : nullptr;
            pressed->track = player ? player->getLoadedTrack() : TrackPointer();
            pressed->position = value(channel(pressed->deck), QStringLiteral("hotcue_%1_position").arg(cue));
            pressed->playing = value(channel(pressed->deck), "play") > 0;
        });
        pads->addWidget(pad, 1); buttons[cue - 1] = pad;
    }
    HotcuePads registered;
    registered.pads = buttons;
    m_hotcueButtons.insert(prefix, registered);
    strip->setFixedHeight(followsLayer ? 48 : 44); return strip;
}

void WDeckLab::refreshHotcues(int deck, const QString& prefix) {
    const auto found = m_hotcueButtons.find(prefix);
    if (found == m_hotcueButtons.end() || deck < 1 || deck > 4) return;
    const auto& d = m_deck[deck - 1];
    HotcueState state;
    state.deck = deck;
    state.loaded = d.loaded->get() > 0;
    state.playing = d.play->get() > 0;
    for (int cue = 0; cue < 8; ++cue) {
        const bool set = d.cuePosition[cue]->get() >= 0;
        const bool loop = set && int(d.cueType[cue]->get()) == int(mixxx::CueType::Loop);
        const int color = int(d.cueColor[cue]->get());
        state.cues[cue] = !set ? -1 : (qint64(loop) << 32) | (color >= 0 && color <= 0xffffff ? color : 0x555555);
    }
    // Unchanged cue state: no text, property, tooltip or accessibility writes.
    if (state == found.value().shown) return;
    found.value().shown = state;
    for (int cue = 1; cue <= 8; ++cue) {
        auto* pad = found.value().pads[cue - 1]; if (!pad) continue;
        const qint64 cueState = state.cues[cue - 1];
        const bool set = cueState >= 0;
        const bool loop = set && (cueState >> 32) != 0;
        const QString letter(QChar('A' + cue - 1));
        const bool enabled = state.loaded && (set || !state.playing);
        const QString label = letter + (loop ? QString(QChar(0x21bb)) : set ? QString() : QStringLiteral("+"));
        setEnabledIf(pad, enabled);
        setTextIf(pad, label);
        const QString ink = set ? QColor::fromRgb(int(cueState & 0xffffff)).name() : QStringLiteral("#555555");
        if (pad->property("cueSet").toBool() != set || pad->property("cueColor").toString() != ink) {
            pad->setProperty("cueSet", set); pad->setProperty("cueColor", ink); pad->update();
        }
        setAccessibleNameIf(pad, tr("Deck %1 cue %2, %3").arg(deck).arg(letter, loop ? tr("saved loop") : set ? tr("set") : tr("empty")));
        const QString help = loop ? tr("Jump to saved loop %1 start; preserves playback and does not enable looping").arg(letter) :
                set ? tr("Jump to cue %1; preserves playback").arg(letter) : tr("Set cue %1 at the current paused position").arg(letter);
        setToolTipIf(pad, help);
        setAccessibleDescriptionIf(pad, help);
    }
}

void WDeckLab::showBeatgrid(int deck, bool followsLayer) {
    const int actual = followsLayer ? displayedDeck(deck - 1) : deck;
    const QString group = channel(actual);
    auto* player = m_players ? m_players->getPlayer(group) : nullptr;
    const TrackPointer track = player ? player->getLoadedTrack() : TrackPointer();
    if (!track) return;
    const quint64 epoch = m_interactionEpoch;
    auto* dialog = new QDialog(this, Qt::Dialog | Qt::FramelessWindowHint); dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("decklab-beatgrid-editor"));
    dialog->setWindowTitle(tr("Deck %1 beatgrid").arg(actual));
    dialog->setStyleSheet(kDialogStyle);
    auto* content = column(dialog, 8); content->setSpacing(8);
    auto* title = new QLabel(dialog); title->setObjectName(QStringLiteral("decklab-beatgrid-title")); title->setFixedHeight(48);
    title->setProperty("heading", true);
    content->addWidget(title);
    auto* hint = new QLabel(tr("Pause deck, release Sync lock and unlock grid to edit."), dialog); hint->setFixedHeight(32); content->addWidget(hint);
    const auto current = [this, deck, followsLayer, actual, track, epoch] {
        auto* player = m_players ? m_players->getPlayer(channel(actual)) : nullptr;
        return epoch == m_interactionEpoch && (!followsLayer || displayedDeck(deck - 1) == actual) &&
                player && player->getLoadedTrack() == track && value(channel(actual), "track_loaded") > 0;
    };
    const auto quietShared = [this, track] {
        for (int deck = 1; deck <= 4; ++deck) {
            auto* player = m_players ? m_players->getPlayer(channel(deck)) : nullptr;
            if (player && player->getLoadedTrack() == track &&
                    (value(channel(deck), "play") > 0 || value(channel(deck), "sync_enabled") > 0 ||
                            value(channel(deck), "sync_leader") > 0)) return false;
        }
        return true;
    };
    auto update = std::make_shared<std::function<void()>>();
    QList<QPushButton*> edits;
    auto make = [this, dialog, current, quietShared, group, track, update, &edits](const QString& text, const QString& item, bool lock) {
        auto pressed = std::make_shared<quint64>(0);
        auto* b = button(text, [this, dialog, current, quietShared, group, track, update, pressed, item, lock] {
            if (!current()) { dialog->close(); return; }
            if (*pressed == m_interactionEpoch && quietShared() && !value(group, "play") &&
                    (lock || (!value(group, "bpmlock") && !value(group, "sync_enabled") &&
                                     !value(group, "sync_leader") && track->getBeats()))) {
                if (lock) set(group, item, !value(group, item));
                else pulse(group, item);
            }
            if (*update) (*update)();
        }, dialog, QStringLiteral("decklab-grid-%1").arg(item));
        b->setFixedHeight(44); b->setCheckable(lock);
        connect(b, &QPushButton::pressed, this, [this, pressed] { *pressed = m_interactionEpoch; });
        if (!lock) edits.append(b);
        return b;
    };
    auto* shifts = row();
    shifts->addWidget(make(tr("Grid earlier"), QStringLiteral("beats_translate_earlier"), false), 1);
    shifts->addWidget(make(tr("Align beat to playhead"), QStringLiteral("beats_translate_curpos"), false), 1);
    shifts->addWidget(make(tr("Grid later"), QStringLiteral("beats_translate_later"), false), 1); content->addLayout(shifts);
    auto* tempo = row();
    tempo->addWidget(make(tr("Grid BPM slower"), QStringLiteral("beats_adjust_slower"), false), 1);
    tempo->addWidget(make(tr("Grid BPM faster"), QStringLiteral("beats_adjust_faster"), false), 1); content->addLayout(tempo);
    auto* scale = row();
    scale->addWidget(make(tr("Half grid BPM"), QStringLiteral("beats_set_halve"), false), 1);
    scale->addWidget(make(tr("Double grid BPM"), QStringLiteral("beats_set_double"), false), 1); content->addLayout(scale);
    auto* footer = row();
    auto* lock = make(tr("GRID LOCK"), QStringLiteral("bpmlock"), true); footer->addWidget(lock, 1);
    auto* close = button(tr("Close"), [dialog] { dialog->close(); }, dialog, QStringLiteral("decklab-grid-close")); close->setFixedHeight(44); footer->addWidget(close, 1); content->addLayout(footer);
    *update = [this, dialog, current, quietShared, group, track, actual, title, hint, lock, edits] {
        if (!current()) { dialog->close(); return; }
        const bool paused = !value(group, "play"); const bool locked = value(group, "bpmlock") > 0;
        const bool synced = value(group, "sync_enabled") > 0 || value(group, "sync_leader") > 0;
        const bool sharedSafe = quietShared();
        const bool beats = bool(track->getBeats());
        const double bpm = value(group, "local_bpm");
        title->setText(tr("DECK LAB · Deck %1 · BEATGRID · Local BPM %2\n%3").arg(actual).arg(
                util_isfinite(bpm) && bpm > 0 ? QString::number(bpm, 'f', 2) : QStringLiteral("—"),
                title->fontMetrics().elidedText(track->getTitle(), Qt::ElideRight, 660)));
        hint->setText(!sharedSafe ? tr("Stop playback and release Sync on every deck using this track before editing.") : !paused ? tr("Pause deck, release Sync lock and unlock grid to edit.") : !beats ? tr("No stored beat grid. Editing is unavailable.") : synced ? tr("Release Sync lock before editing; a master grid can affect other decks.") : locked ? tr("Grid is locked. Unlock while paused to edit.") : tr("Paused · grid unlocked. Changes affect this track's beat grid."));
        lock->setEnabled(paused && sharedSafe); lock->setChecked(locked); lock->setText(locked ? tr("GRID LOCK ON") : tr("GRID LOCK OFF"));
        for (auto* b : edits) b->setEnabled(paused && sharedSafe && !locked && !synced && beats);
    };
    auto* timer = new QTimer(dialog); connect(timer, &QTimer::timeout, dialog, [update] { if (*update) (*update)(); });
    timer->start(100); (*update)();
    dialog->resize(760, 420); dialog->move(mapToGlobal(QPoint((width() - 760) / 2, (height() - 420) / 2)));
    dialog->setWindowModality(Qt::WindowModal); dialog->open();
}

QWidget* WDeckLab::buildDecks() {
    auto* page = new QWidget(this);
    // Physical-side rows live in the deck row for good; a layer change only
    // retargets them, it never reparents or relayouts them.
    for (int deck = 1; deck <= 2; ++deck) {
        m_sideHotcues[deck - 1] = buildHotcues(deck, true, m_deckRow);
        m_sideHotcues[deck - 1]->hide();
        m_sideUtilities[deck - 1] = buildTransport(deck, true, m_deckRow);
        m_sideUtilities[deck - 1]->hide();
    }
    page->hide(); return page;
}

namespace {
using DeckLabList = DeckLabBrowseState::List;

// Generic QObject event hook; keeps key and press handling out of eventFilter().
class DeckLabEventHook final : public QObject {
  public:
    DeckLabEventHook(QObject* watched, std::function<bool(QObject*, QEvent*)> handler)
            : QObject(watched), m_handler(std::move(handler)) {
        watched->installEventFilter(this);
    }
  protected:
    bool eventFilter(QObject* watched, QEvent* event) override { return m_handler(watched, event); }
  private:
    std::function<bool(QObject*, QEvent*)> m_handler;
};

QString rootIconName(QModelIndex index) {
    while (index.parent().isValid()) index = index.parent();
    return index.data(SidebarModel::IconNameRole).toString();
}
bool isCollectionSource(const QModelIndex& index) {
    return index.isValid() && !index.parent().isValid() && rootIconName(index) == QStringLiteral("tracks");
}
bool isRekordboxDevice(const QModelIndex& index) {
    return index.isValid() && index.parent().isValid() && !index.parent().parent().isValid() &&
            rootIconName(index) == QStringLiteral("rekordbox");
}
bool rekordboxImported(const QModelIndex& device) {
    const QStringList data = device.data(SidebarModel::DataRole).toStringList();
    return data.size() < 2 || data.at(1) != QStringLiteral("::isRecordboxDevice::");
}
QModelIndex sidebarRoot(const QAbstractItemModel* model, const QString& icon) {
    if (!model) return {};
    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex index = model->index(row, 0);
        if (index.data(SidebarModel::IconNameRole).toString() == icon) return index;
    }
    return {};
}
decklab::Glyph sourceGlyph(const QModelIndex& index) {
    const QString icon = rootIconName(index);
    if (icon == QStringLiteral("tracks")) return decklab::Glyph::Collection;
    if (icon == QStringLiteral("crates")) return decklab::Glyph::Crate;
    if (icon == QStringLiteral("autodj")) return decklab::Glyph::Playlist;
    if (icon == QStringLiteral("history")) return decklab::Glyph::History;
    if (icon == QStringLiteral("recordings")) return decklab::Glyph::Album;
    if (icon == QStringLiteral("computer")) return decklab::Glyph::Folder;
    if (icon == QStringLiteral("rekordbox") && !index.parent().isValid()) return decklab::Glyph::Usb;
    if (isRekordboxDevice(index)) return decklab::Glyph::Usb;
    if (index.model() && index.model()->hasChildren(index)) return decklab::Glyph::Folder;
    return decklab::Glyph::Playlist;
}
decklab::Glyph categoryGlyph(decklab::Category category) {
    switch (category) {
    case decklab::Category::AllTracks: return decklab::Glyph::Tracks;
    case decklab::Category::Playlists: return decklab::Glyph::Playlist;
    case decklab::Category::Crates: return decklab::Glyph::Crate;
    case decklab::Category::History: return decklab::Glyph::History;
    case decklab::Category::Artist: return decklab::Glyph::Artist;
    case decklab::Category::Album: return decklab::Glyph::Album;
    case decklab::Category::Bpm: return decklab::Glyph::Bpm;
    case decklab::Category::Key: return decklab::Glyph::Key;
    }
    return decklab::Glyph::None;
}
// The surface style sheet's QPushButton min-height wins over setFixedHeight()
// in the real application. Pin the height in the button's own style sheet too
// (1 px border, no vertical padding) so touch targets keep their size.
void holdHeight(QPushButton* button, int height, const QString& extra = QString()) {
    button->setFixedHeight(height);
    button->setStyleSheet(QStringLiteral("min-height:%1px;max-height:%1px;padding-top:0px;padding-bottom:0px;%2")
                                  .arg(height - 2)
                                  .arg(extra));
}

DeckLabActionButton* browseButton(QWidget* parent, const QString& text, const QString& name,
        const QString& accessible, decklab::Glyph glyph, int width) {
    auto* b = new DeckLabActionButton(text, parent);
    b->setObjectName(name);
    b->setAccessibleName(accessible);
    b->setGlyph(glyph);
    holdHeight(b, 44);
    if (width > 0) b->setFixedWidth(width);
    return b;
}
} // namespace

QWidget* WDeckLab::buildBrowse() {
    m_browseState = std::make_unique<DeckLabBrowseState>();
    auto& b = *m_browseState;
    b.matchEnabled = m_config->getValue(ConfigKey("[DeckLab]", "BrowseMatch"), 1) != 0;
    for (const auto& name : {"browse_move", "browse_select", "browse_enter", "load_refused"}) {
        createControl(kNative, QString::fromLatin1(name));
    }
    m_breadcrumb = nullptr; // Replaced by the painted, middle-eliding breadcrumb.
    m_browser = new QWidget(this);
    m_browser->setObjectName(QStringLiteral("decklab-browse"));
    auto* outer = row(m_browser);
    auto* main = new QWidget(m_browser);
    auto* layout = column(main);
    outer->addWidget(main, 1);

    // Header: back, where you are, and view tools. LOAD lives in the pane.
    auto* heading = row();
    auto* back = browseButton(m_browser, QString(), QStringLiteral("decklab-browse-back"), tr("Back"),
            decklab::Glyph::Back, 52);
    connect(back, &QPushButton::clicked, this, [this] { browseBack(); });
    heading->addWidget(back);
    b.breadcrumb = new DeckLabBreadcrumb(m_browser);
    b.breadcrumb->setObjectName(QStringLiteral("decklab-breadcrumb"));
    b.breadcrumb->setSegments({tr("Sources")});
    heading->addWidget(b.breadcrumb, 1);
    b.match = browseButton(m_browser, tr("Match"), QStringLiteral("decklab-browse-match"),
            tr("Highlight tracks that mix with the playing deck"), decklab::Glyph::None, 128);
    connect(b.match, &QPushButton::clicked, this, [this] {
        // Toggle; the button's tone and state line show on/off and the deck.
        auto& state = *m_browseState;
        state.matchEnabled = !state.matchEnabled;
        m_config->setValue(ConfigKey("[DeckLab]", "BrowseMatch"), state.matchEnabled ? 1 : 0);
        refreshMatchReference();
    });
    heading->addWidget(b.match);
    auto* search = browseButton(m_browser, QString(), QStringLiteral("decklab-search"), tr("Search"),
            decklab::Glyph::Search, 52);
    connect(search, &QPushButton::clicked, this, [this] { setSearchOpen(!m_keyboard || m_keyboard->isHidden()); });
    heading->addWidget(search);
    auto* tools = browseButton(m_browser, QString(), QStringLiteral("decklab-browse-tools"), tr("Tools"),
            decklab::Glyph::More, 52);
    connect(tools, &QPushButton::clicked, this, [this, tools] {
        auto* menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->setStyleSheet(QStringLiteral("QMenu{font-size:16px;font-weight:500;}"));
        menu->addAction(tr("All tracks"), this, &WDeckLab::openCollection);
        menu->addAction(tr("Preview selected track"), this, [this] { togglePreview(); });
        menu->addAction(tr("Stop preview"), this, [this] { stopPreview(); });
        int matchDeck = displayedDeck(0);
        bool foundPlaying = false;
        for (int deck = 1; deck <= 4; ++deck) {
            if (!value(channel(deck), "track_loaded")) continue;
            if (value(channel(deck), "play") && (!foundPlaying || value(channel(deck), "sync_leader") > 0)) {
                matchDeck = deck; foundPlaying = true;
            } else if (!foundPlaying && !value(channel(matchDeck), "track_loaded")) matchDeck = deck;
        }
        auto* match = menu->addAction(tr("Match Deck %1").arg(matchDeck), this, [this, deck = matchDeck] {
            const double bpm = value(channel(deck), "bpm");
            if (bpm > 0 && m_search) {
                const QString key = KeyUtils::keyToString(KeyUtils::keyFromNumericValue(value(channel(deck), "visual_key")), KeyUtils::KeyNotation::Lancelot);
                QString query = QStringLiteral("~bpm:%1").arg(bpm, 0, 'f', 2);
                if (!key.isEmpty()) query += QStringLiteral(" ~key:%1").arg(key);
                resetBrowseLevels();
                if (m_library) m_library->searchTracksInCollection(query);
                m_search->slotRestoreSearch(query);
                showTracks();
            }
        });
        match->setEnabled(value(channel(matchDeck), "track_loaded") && value(channel(matchDeck), "bpm") > 0);
        menu->addAction(tr("Clear filter"), this, [this] {
            clearBrowseFilter(); m_browseState->fromCategories = false;
            if (m_search) m_search->slotClearSearch();
        });
        menu->addAction(tr("Track details"), this, [this] { set("[Library]", "show_track_menu", 1); });
        // Deck Lab's Key/BPM/Title/Artist order is a default; Mixxx's column
        // chooser changes it per view (touch screens have no right click).
        auto* table = m_libraryWidget && m_browseStack->currentWidget() == m_libraryWidget
                ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
        auto* columns = menu->addAction(tr("Choose columns…"), this, [table, tools] {
            if (table) table->showDeckLabColumnMenu(tools->mapToGlobal(QPoint(tools->width() - 260, tools->height())));
        });
        columns->setEnabled(table != nullptr);
        auto* resetColumns = menu->addAction(tr("Default columns"), this, [table] { if (table) table->resetDeckLabColumns(); });
        resetColumns->setEnabled(table != nullptr);
        auto* unload = menu->addMenu(tr("Unload stopped deck"));
        for (int deck = 1; deck <= 4; ++deck) {
            auto* action = unload->addAction(tr("Deck %1").arg(deck), this, [this, deck] {
                if (value(channel(deck), "track_loaded") && !value(channel(deck), "play")) pulse(channel(deck), "eject");
            });
            action->setObjectName(QStringLiteral("decklab-unload-%1").arg(deck));
            action->setEnabled(value(channel(deck), "track_loaded") && !value(channel(deck), "play"));
        }
        auto* loadSamples = menu->addMenu(tr("Load into sampler"));
        for (int slot = 1; slot <= 16; ++slot) {
            auto* action = loadSamples->addAction(tr("Pad %1").arg(slot), this, [this, slot] {
                const QString group = QStringLiteral("[Sampler%1]").arg(slot);
                if (value(group, "play") || !m_libraryWidget) return;
                if (value(group, "track_loaded") && QMessageBox::question(this, tr("Replace sample"),
                        tr("Replace the sample on pad %1?").arg(slot), QMessageBox::Yes | QMessageBox::Cancel,
                        QMessageBox::Cancel) != QMessageBox::Yes) return;
                if (auto* table = m_libraryWidget->getCurrentTrackTableView()) {
                    table->loadSelectedTrackToGroup(group,
#ifdef __STEM__
                            mixxx::StemChannelSelection(),
#endif
                            false);
                }
            });
            action->setEnabled(!value(QStringLiteral("[Sampler%1]").arg(slot), "play"));
        }
        menu->popup(tools->mapToGlobal(QPoint(tools->width() - 260, tools->height())));
    });
    heading->addWidget(tools);
    layout->addLayout(heading);
    m_searchPanel = new QWidget(main); m_searchRow = row(m_searchPanel);
    layout->addWidget(m_searchPanel); m_searchPanel->hide();
    m_browseStack = new QStackedWidget(main);
    layout->addWidget(m_browseStack, 1);

    // List level: the native sidebar (added by bindBrowser) or the category/
    // value list. Index 0 of the Browse stack in both cases.
    b.listStack = new QStackedWidget(m_browseStack);
    b.listStack->setObjectName(QStringLiteral("decklab-browse-lists"));
    m_browseStack->addWidget(b.listStack);
    b.items = new QListWidget(b.listStack);
    b.items->setObjectName(QStringLiteral("decklab-browse-categories"));
    b.items->setStyleSheet(QStringLiteral("QListWidget{background:#0D0F12;border:0;outline:0;}"));
    b.items->setItemDelegate(new DeckLabListDelegate(b.items));
    b.items->setUniformItemSizes(true);
    b.items->setFrameShape(QFrame::NoFrame);
    b.items->setEditTriggers(QAbstractItemView::NoEditTriggers);
    b.items->setSelectionMode(QAbstractItemView::SingleSelection);
    b.items->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    b.items->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    b.items->setMouseTracking(false);
    b.items->viewport()->setAttribute(Qt::WA_Hover, false);
    b.listStack->addWidget(b.items);
    new DeckLabTouchScrollGuard(b.items->viewport(), b.items->viewport());
    connect(b.items, &QListWidget::clicked, this, [this](const QModelIndex& index) {
        if (!index.isValid()) return;
        m_browseState->items->setCurrentRow(index.row());
        browseEnter();
    });
    connect(b.items, &QListWidget::currentRowChanged, this, [this](int) { refreshBrowseCard(); });
    new DeckLabEventHook(b.items, [this](QObject*, QEvent* event) {
        if (event->type() != QEvent::KeyPress) return false;
        auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
        case Qt::Key_Up: browseMove(-std::max(1, key->count()), true); return true;
        case Qt::Key_Down: browseMove(std::max(1, key->count()), true); return true;
        case Qt::Key_Return: case Qt::Key_Enter: case Qt::Key_Right: browseEnter(); return true;
        case Qt::Key_Left: case Qt::Key_Escape: case Qt::Key_Backspace: browseBack(); return true;
        default: return false;
        }
    });

    // Details pane: the highlighted item, one-tap Preview, and the protected
    // LOAD targets showing each deck's state.
    auto* pane = new QWidget(m_browser);
    pane->setObjectName(QStringLiteral("decklab-browse-pane"));
    pane->setAttribute(Qt::WA_StyledBackground);
    pane->setStyleSheet(QStringLiteral("QWidget#decklab-browse-pane{background:#0D0F12;border-left:1px solid #2C313A;}"));
    pane->setFixedWidth(236);
    // Browse-scoped brand styling: no arrow stubs on the thin touch scroll
    // bars, 14 px SemiBold column headers, white-on-raise selection.
    m_browser->setStyleSheet(QStringLiteral(
            "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0px;border:0;background:transparent;}"
            "QHeaderView{font-size:14px;font-weight:600;}"
            "QHeaderView::section{background:#16191E;color:#F2F4F7;border:0;border-right:1px solid #2C313A;"
            "border-bottom:1px solid #2C313A;padding:4px 8px;}"
            "QTableView{background:#0D0F12;alternate-background-color:#121519;"
            "selection-background-color:#20242B;selection-color:#F2F4F7;}"));
    auto* paneLayout = column(pane);
    paneLayout->setContentsMargins(6, 6, 6, 6);
    paneLayout->setSpacing(6);
    paneLayout->addStretch(0);
    b.card = new DeckLabTrackCard(pane);
    b.card->setObjectName(QStringLiteral("decklab-track-card"));
    paneLayout->addWidget(b.card, 1);
    b.preview = browseButton(pane, tr("Preview"), QStringLiteral("decklab-preview"),
            tr("Preview the highlighted track in headphones"), decklab::Glyph::Play, 0);
    connect(b.preview, &QPushButton::clicked, this, [this] { togglePreview(); });
    paneLayout->addWidget(b.preview);
    auto* loads = new QGridLayout;
    loads->setContentsMargins(0, 0, 0, 0);
    loads->setSpacing(6);
    for (int deck = 1; deck <= 4; ++deck) {
        auto pressed = std::make_shared<std::tuple<int, quint64, int>>(0, 0, 0);
        auto selection = std::make_shared<QPersistentModelIndex>();
        auto* load = new DeckLabActionButton(tr("Load %1").arg(deck), pane);
        load->setObjectName(QStringLiteral("decklab-load-%1").arg(deck));
        load->setAccessibleName(tr("Load deck %1").arg(deck));
        holdHeight(load, 52);
        m_loadButtons[deck - 1] = load;
        connect(load, &QPushButton::clicked, this, [this, deck, pressed, selection] {
            auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
            const QPersistentModelIndex current = table ? table->currentIndex().siblingAtColumn(0) : QModelIndex();
            const int target = m_fourDeckLayout ? deck : (deck <= 2 ? displayedDeck(deck - 1) : deck);
            if (std::get<0>(*pressed) != target || std::get<1>(*pressed) != m_interactionEpoch ||
                    std::get<2>(*pressed) != m_sampleTarget || *selection != current) return;
            loadDeck(deck);
        });
        connect(load, &QPushButton::pressed, this, [this, deck, pressed, selection] {
            *pressed = {m_fourDeckLayout ? deck : (deck <= 2 ? displayedDeck(deck - 1) : deck), m_interactionEpoch, m_sampleTarget};
            auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
            *selection = table ? table->currentIndex().siblingAtColumn(0) : QModelIndex();
        });
        // A disabled target still answers a tap: say why it cannot load.
        new DeckLabEventHook(load, [this, deck](QObject* watched, QEvent* event) {
            if (event->type() == QEvent::MouseButtonPress && !static_cast<QWidget*>(watched)->isEnabled()) {
                showLoadUnavailable(deck);
            }
            return false;
        });
        loads->addWidget(load, (deck - 1) / 2, (deck - 1) % 2);
    }
    paneLayout->addLayout(loads);
    outer->addWidget(pane);
    // 800 px is the design target; wider screens give the pane a little more.
    new DeckLabEventHook(m_browser, [pane](QObject* watched, QEvent* event) {
        if (event->type() == QEvent::Resize) {
            const int width = std::clamp(static_cast<QWidget*>(watched)->width() * 3 / 10, 236, 340);
            if (pane->minimumWidth() != width) pane->setFixedWidth(width);
        }
        return false;
    });

    b.toast = new DeckLabToast(this);
    b.matchTimer.setSingleShot(true);
    b.matchTimer.setInterval(150);
    connect(&b.matchTimer, &QTimer::timeout, this, [this] { refreshMatchReference(); });
    b.loadingTimer.setInterval(300);
    connect(&b.loadingTimer, &QTimer::timeout, this, [this] { checkRekordboxImport(); });

    // Generic controller navigation, routed by LibraryControl.
    watch(kNative, "browse_move", [this](double v) {
        if (v != 0 && std::isfinite(v)) browseMove(int(std::clamp(v, -10000.0, 10000.0)), false);
    });
    watch(kNative, "browse_select", [this](double v) {
        if (v != 0 && std::isfinite(v)) browseMove(int(std::clamp(v, -10000.0, 10000.0)), true);
    });
    watch(kNative, "browse_enter", [this](double v) { if (v > 0) browseEnter(); });
    watch(kNative, "load_refused", [this](double v) { if (v > 0) showLoadRefused(int(v)); });
    // Registered before the constructor's own browse_sources handler, which
    // then resets the sidebar root.
    watch(kNative, "browse_sources", [this](double v) { if (v > 0) resetBrowseLevels(); });
    watch(QStringLiteral("[PreviewDeck1]"), QStringLiteral("play"), [this](double) { refreshPreviewButton(); });
    watch(kTab, QStringLiteral("current"), [this](double v) { if (int(v) != 1) stopPreview(); });
    for (int deck = 1; deck <= 4; ++deck) {
        for (const auto& key : {"bpm", "visual_key", "sync_leader"}) {
            watch(channel(deck), QString::fromLatin1(key), [this](double) { scheduleMatchRefresh(); });
        }
    }
    refreshMatchReference();
    return m_browser;
}

QWidget* WDeckLab::buildSampler() {
    auto* page = new QWidget(this);
    auto* layout = column(page, 4);
    auto* grid = new QGridLayout;
    grid->setSpacing(3);
    for (int slot = 1; slot <= 16; ++slot) {
        QString group = QStringLiteral("[Sampler%1]").arg(slot);
        auto* pad = button(tr("%1\nEmpty").arg(slot), [this, group, slot] {
            if (value(group, "track_loaded")) pulse(group, "play_stutter");
            else { m_sampleTarget = slot; m_sampleReturnPage = 2; selectPage(1); refreshLoadButtons(); }
        }, page, QStringLiteral("decklab-sample-%1").arg(slot));
        pad->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
        pad->setCheckable(true);
        connect(pad, &QPushButton::clicked, this, [this, group, pad] { pad->setChecked(value(group, "play") > 0); });
        pad->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(pad, &QWidget::customContextMenuRequested, this, [this, group, pad](const QPoint& point) {
            auto* menu = new QMenu(this); menu->setAttribute(Qt::WA_DeleteOnClose);
            menu->addAction(tr("Stop"), this, [this, group] { pulse(group, "stop"); });
            auto* clear = menu->addAction(tr("Clear pad"), this, [this, group] { if (!value(group, "play")) pulse(group, "eject"); });
            clear->setEnabled(!value(group, "play"));
            auto* level = new QSlider(Qt::Horizontal, menu); level->setMinimumSize(240,44); level->setRange(0,100);
            level->setAccessibleName(tr("Sample level")); level->setValue(int(control(group,"pregain")->getParameter()*100));
            connect(level, &QSlider::valueChanged, this, [this,group](int v) { control(group,"pregain")->setParameter(v/100.0); });
            auto* action = new QWidgetAction(menu); action->setDefaultWidget(level); menu->addSeparator(); menu->addAction(tr("Sample level"))->setEnabled(false); menu->addAction(action);
            menu->popup(pad->mapToGlobal(point));
        });
        m_samples[slot - 1] = pad;
        grid->addWidget(pad, (slot - 1) / 6, (slot - 1) % 6);
        watch(group, "play", [pad](double v) { pad->setChecked(v > 0); });
        auto refresh = [this, group, slot, pad](double loaded) {
            auto* player = m_players ? m_players->getPlayer(group) : nullptr;
            auto track = player ? player->getLoadedTrack() : TrackPointer();
            pad->setText(QStringLiteral("%1\n%2").arg(slot).arg(track ? track->getTitle() : tr("Tap to load")));
            pad->setAccessibleName(tr("Pad %1, %2").arg(slot).arg(track ? track->getTitle() : tr("Empty, tap to load")));
            pad->setToolTip(track ? track->getTitle() : tr("Browse → Tools → Load into sampler"));
            if (loaded && m_sampleTarget == slot) { m_sampleTarget = 0; selectPage(m_sampleReturnPage); }
        };
        watch(group, "track_loaded", refresh);
        if (m_players) if (auto* player = m_players->getPlayer(group))
            connect(player, &BaseTrackPlayer::newTrackLoaded, this, [refresh](TrackPointer) { refresh(1); });
        refresh(0);
    }
    auto* tools = new QWidget(page); auto* toolsLayout = column(tools, 5);
    auto* stop = button(tr("Stop all samples"), [this] {
        for (int slot = 1; slot <= 16; ++slot) pulse(QStringLiteral("[Sampler%1]").arg(slot), "stop");
    }, tools, "decklab-samples-stop");
    stop->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    toolsLayout->addWidget(stop, 1);
    auto* hint = new QLabel(tr("Hold a pad for Stop / Clear / Level"), tools);
    hint->setProperty("muted", true); hint->setAlignment(Qt::AlignCenter); toolsLayout->addWidget(hint);
    grid->addWidget(tools, 2, 4, 1, 2);
    for (int col = 0; col < 6; ++col) grid->setColumnStretch(col, 1);
    for (int row = 0; row < 3; ++row) grid->setRowStretch(row, 1);
    layout->addLayout(grid, 1);
    return page;
}

QWidget* WDeckLab::buildEffects() {
    auto* page = new QWidget(this);
    auto* layout = column(page, 5);
    auto* heading = row(nullptr, 3);
    auto* target = new QComboBox(page);
    target->setObjectName("decklab-fx-target");
    target->addItems({tr("Deck 1"), tr("Deck 2")});
    connect(target, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        releaseEffects(); m_fxDeck = index + 1; refreshFx();
    });
    heading->addWidget(target);
    m_fxStatus = new QLabel(tr("LAB FX"), page);
    m_fxStatus->setProperty("muted", true);
    heading->addWidget(m_fxStatus, 1);
    heading->addWidget(button(tr("Release dry"), [this] { releaseEffects(true); }, page, "decklab-fx-release"));
    heading->addWidget(button(tr("Mute tail"), [this] { releaseEffects(true, true); }, page, "decklab-fx-mute"));
    layout->addLayout(heading);
    m_fxBody = new QWidget(page);
    auto* body = column(m_fxBody);
    auto* tabs = row();
    auto* views = new QStackedWidget(m_fxBody);
    auto* selection = new QButtonGroup(views);
    for (int n = 0; n < 5; ++n) {
        const QStringList names{tr("Levers"), tr("Isolate"), tr("Perform"), tr("XY"), tr("Samples")};
        auto* tab = button(names[n], [this, views, n] {
            if(m_holdFx)releaseEffects(true);
            if(!m_heldRoll.isEmpty()){set(channel(m_fxDeck),m_heldRoll,0);m_heldRoll.clear();}
            if(!m_heldSamplerRoll.isEmpty()){set(m_heldSamplerGroup,m_heldSamplerRoll,0);m_heldSamplerRoll.clear();}
            views->setCurrentIndex(n);
        }, page,
                QStringLiteral("decklab-fx-view-%1").arg(n));
        tab->setCheckable(true); tab->setChecked(n == 0); tab->setProperty("rail", true);
        selection->addButton(tab); tabs->addWidget(tab, 1);
    }
    layout->addLayout(tabs);
    views->setObjectName("decklab-fx-pages");
    const QStringList bands{tr("LOW"), tr("MID"), tr("HIGH")};
    auto* lever = new QWidget(views); auto* leverLayout = column(lever);
    m_leverMode = new QComboBox(lever);
    m_leverMode->addItems({tr("Lever · Echo"), tr("Lever · Reverb"), tr("Lever · Solo")});
    leverLayout->addWidget(m_leverMode);
    auto* pads = row(nullptr, 3);
    for (int n = 0; n < 3; ++n) {
        m_levers[n] = button(bands[n], [this] { applyEffects(); }, lever,
                QStringLiteral("decklab-fx-band-%1").arg(n));
        m_levers[n]->setCheckable(true);
        m_levers[n]->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        pads->addWidget(m_levers[n], 1);
    }
    leverLayout->addLayout(pads, 1); views->addWidget(lever);
    auto* isolate = new QWidget(views); auto* isolateLayout = column(isolate);
    m_isoMode = new QComboBox(isolate);
    m_isoMode->addItems({tr("Echo · left cuts / right sends"), tr("Reverb · left cuts / right sends"),
            tr("Drive · left cuts / right drives"), tr("Filter · left low-pass / right high-pass"),
            tr("Ducker · left on-beat / right off-beat")});
    isolateLayout->addWidget(m_isoMode);
    auto* channels = row(nullptr, 12);
    for (int n = 0; n < 3; ++n) {
        auto* band = new QWidget(isolate); auto* controls = column(band);
        auto* label = new QLabel(bands[n], band); label->setAlignment(Qt::AlignCenter); controls->addWidget(label);
        m_isolates[n] = new QSlider(Qt::Horizontal, band);
        m_isolates[n]->setRange(-100, 100); m_isolates[n]->setMinimumHeight(44);
        m_isolates[n]->setAccessibleName(tr("%1 isolate, centre is dry").arg(bands[n]));
        connect(m_isolates[n], &QSlider::valueChanged, this, [this](int) { if (!m_updating) applyEffects(); });
        controls->addWidget(m_isolates[n], 1);
        controls->addWidget(button(tr("Centre"), [this, n] { m_isolates[n]->setValue(0); }, band));
        channels->addWidget(band, 1);
    }
    isolateLayout->addLayout(channels, 1); views->addWidget(isolate);
    auto* perform = new QWidget(views); auto* performLayout = new QGridLayout(perform);
    performLayout->setContentsMargins(0, 0, 0, 0); performLayout->setSpacing(3);
    const QStringList holdNames{tr("Hold Echo"), tr("Hold Reverb"), tr("Hold Filter"), tr("Hold Ducker")};
    for (int n = 0; n < 4; ++n) {
        auto* hold = button(holdNames[n], [] {}, perform, QStringLiteral("decklab-hold-%1").arg(n));
        hold->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        connect(hold, &QPushButton::pressed, this, [this, n] {
            releaseEffects(); m_updating = true;
            if (n < 2) { m_leverMode->setCurrentIndex(n); for (auto* pad : m_levers) pad->setChecked(true); }
            else { m_isoMode->setCurrentIndex(n == 2 ? 3 : 4); for (auto* fader : m_isolates) fader->setValue(70); }
            m_updating = false; applyEffects(); m_holdFx = true; m_holdStarted = m_clock.elapsed();
        });
        connect(hold, &QPushButton::released, this, [this] { m_holdFx = false; releaseEffects(true); });
        performLayout->addWidget(hold, 0, n);
    }
    const std::array<double, 4> fractions{.125, .25, .5, 1};
    const QStringList rollNames{tr("Roll 1/8"), tr("Roll 1/4"), tr("Roll 1/2"), tr("Roll 1")};
    for (int n = 0; n < 4; ++n) {
        auto* hold = button(rollNames[n], [] {}, perform, QStringLiteral("decklab-roll-%1").arg(n));
        hold->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        connect(hold, &QPushButton::pressed, this, [this, fraction = fractions[n]] {
            if (!value(channel(m_fxDeck), "play")) return;
            if (!m_heldRoll.isEmpty()) set(channel(m_fxDeck), m_heldRoll, 0);
            m_heldRoll = QStringLiteral("beatlooproll_%1_activate").arg(fraction);
            m_holdStarted = m_clock.elapsed(); set(channel(m_fxDeck), m_heldRoll, 1);
        });
        connect(hold, &QPushButton::released, this, [this] {
            if (!m_heldRoll.isEmpty()) set(channel(m_fxDeck), m_heldRoll, 0);
            m_heldRoll.clear();
        });
        performLayout->addWidget(hold, 1, n);
    }
    const QStringList starters{tr("Echo start"), tr("Bass cut"), tr("Reverb wash"), tr("Filter start")};
    for (int n = 0; n < starters.size(); ++n) {
        auto* starter = button(starters[n], [this, n] {
            releaseEffects(); m_updating = true;
            m_leverMode->setCurrentIndex(n == 2 ? 1 : 0);
            m_isoMode->setCurrentIndex(n == 3 ? 3 : 0);
            for (int band = 0; band < 3; ++band) {
                m_levers[band]->setChecked(n == 0 || (n == 2 && band > 0));
                m_isolates[band]->setValue(n == 1 && band == 0 ? -90 : n == 3 ? 70 : 0);
            }
            m_fxTime->setCurrentIndex(2); m_depth->setValue(70); m_updating = false;
            findChild<QPushButton*>(QStringLiteral("decklab-fx-view-%1").arg(n == 1 || n == 3 ? 1 : 0))->click();
            applyEffects();
        }, perform, QStringLiteral("decklab-transition-%1").arg(n));
        performLayout->addWidget(starter, 2, n);
    }
    auto* holdHint = new QLabel(tr("Hold pads: 4 sec max · Presets open Lab FX · Release dry to recover"), perform);
    holdHint->setProperty("muted", true); performLayout->addWidget(holdHint, 3, 0, 1, 4);
    views->addWidget(perform);
    auto* xy = new DeckLabXY([this](double x, double y, bool on) {
        if (!on) { m_holdFx = false; releaseEffects(); return; }
        m_updating = true; m_isoMode->setCurrentIndex(3);
        for (auto* pad : m_levers) pad->setChecked(false);
        for (auto* fader : m_isolates) fader->setValue(int((x * 2 - 1) * 100));
        m_depth->setValue(int(y * 100)); m_updating = false;
        applyEffects(); if (!m_holdFx) m_holdStarted = m_clock.elapsed(); m_holdFx = true;
    }, views);
    views->addWidget(xy);
    auto* sampler = new QWidget(views); auto* sampleLayout = column(sampler);
    auto* triggers = row(nullptr, 3);
    for (int slot = 1; slot <= 4; ++slot) {
        const QString group = QStringLiteral("[Sampler%1]").arg(slot);
        auto* pad = new DeckLabPad(QStringLiteral("%1\nTap to load").arg(QChar('A'+slot-1)), sampler);
        pad->setFocusPolicy(Qt::NoFocus); pad->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
        pad->setCheckable(true); pad->setObjectName(QStringLiteral("decklab-fx-sample-%1").arg(slot));
        connect(pad, &QPushButton::clicked, this, [this,group,slot,pad] {
            m_fxSample=slot;
            if (m_sampleRollHint) m_sampleRollHint->setText(tr("Hold roll %1").arg(QChar('A'+slot-1)));
            if (value(group,"track_loaded")) pulse(group,"play_stutter");
            else { m_sampleTarget=slot; m_sampleReturnPage=3; selectPage(1); refreshLoadButtons(); }
            pad->setChecked(value(group,"play")>0);
        });
        auto refresh = [this,group,slot,pad](double) {
            auto* player=m_players?m_players->getPlayer(group):nullptr;
            const auto track=player?player->getLoadedTrack():TrackPointer();
            pad->setText(QStringLiteral("%1\n%2").arg(QChar('A'+slot-1)).arg(track?track->getTitle():tr("Tap to load")));
            pad->setAccessibleName(tr("Sample %1, %2").arg(QChar('A'+slot-1)).arg(track?track->getTitle():tr("Empty")));
            refreshFx();
            if(m_fxActive&&m_sampleRouting)applyEffects();
        };
        watch(group,"play",[this,pad](double v){pad->setChecked(v>0);refreshFx();}); watch(group,"track_loaded",refresh);
        watch(group,"local_bpm",[this](double){refreshFx();});
        if(m_players) if(auto* player=m_players->getPlayer(group)) connect(player,&BaseTrackPlayer::newTrackLoaded,this,[refresh](TrackPointer){refresh(1);});
        QTimer::singleShot(0,pad,[refresh]{refresh(0);});
        triggers->addWidget(pad,1);
    }
    sampleLayout->addLayout(triggers,1);
    auto* sampleRolls=row(nullptr,3); m_sampleRollHint=new QLabel(tr("Hold roll A"),sampler); m_sampleRollHint->setProperty("muted",true); sampleRolls->addWidget(m_sampleRollHint);
    for(int n=0;n<4;++n) {
        auto* hold=button(rollNames[n],[]{},sampler,QStringLiteral("decklab-sample-roll-%1").arg(n));
        connect(hold,&QPushButton::pressed,this,[this,fraction=fractions[n]] {
            const QString group=QStringLiteral("[Sampler%1]").arg(m_fxSample);
            if(!value(group,"play")||value(group,"local_bpm")<=0) {m_sampleRollHint->setText(tr("Play an analysed sample"));return;}
            if(!m_heldSamplerRoll.isEmpty())set(m_heldSamplerGroup,m_heldSamplerRoll,0);
            m_heldSamplerGroup=group; m_heldSamplerRoll=QStringLiteral("beatlooproll_%1_activate").arg(fraction); m_holdStarted=m_clock.elapsed(); set(group,m_heldSamplerRoll,1);
        });
        connect(hold,&QPushButton::released,this,[this]{if(!m_heldSamplerRoll.isEmpty())set(m_heldSamplerGroup,m_heldSamplerRoll,0);m_heldSamplerRoll.clear();});
        sampleRolls->addWidget(hold,1);
    }
    sampleLayout->addLayout(sampleRolls); views->addWidget(sampler); body->addWidget(views, 1);
    auto* timeDepth = row(nullptr, 9);
    m_fxTime = new QComboBox(m_fxBody);
    m_fxTime->addItems({tr("1/8 beat"), tr("1/4 beat"), tr("1/2 beat"), tr("1 beat")}); m_fxTime->setCurrentIndex(2);
    timeDepth->addWidget(m_fxTime); timeDepth->addWidget(new QLabel(tr("Depth"), m_fxBody));
    m_depth = new QSlider(Qt::Horizontal, m_fxBody); m_depth->setRange(0, 100); m_depth->setValue(65); m_depth->setMinimumHeight(44);
    timeDepth->addWidget(m_depth, 1);
    auto* samples = button(tr("Pads A–D FX"), [this] { m_sampleRouting = !m_sampleRouting; refreshFx(); if (m_fxActive) applyEffects(); }, m_fxBody);
    samples->setCheckable(true); timeDepth->addWidget(samples); body->addLayout(timeDepth);
    for (auto* combo : {m_leverMode, m_isoMode, m_fxTime}) connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) { if (!m_updating && m_fxActive) applyEffects(); });
    connect(m_depth, &QSlider::valueChanged, this, [this](int) { if (!m_updating && m_fxActive) applyEffects(); });
    layout->addWidget(m_fxBody, 1); return page;
}

QWidget* WDeckLab::buildSettings() {
    auto* page = new QWidget(this);
    auto* layout = row(page);
    auto* categories = new QWidget(page);
    categories->setFixedWidth(150);
    auto* links = column(categories);
    auto* pages = new QStackedWidget(page);
    pages->setObjectName("decklab-settings-pages");
    auto* categorySelection = new QButtonGroup(pages);
    // Deck Lab system integration: Setup (Deck Setup) and System pages, native
    // tool routing, banners. It only hooks the surface through these calls.
    DeckLabSystemUi::Hooks hooks;
    hooks.showSystem = [this, pages](DeckLabSystemPage::Section section) {
        selectPage(4); pages->setCurrentIndex(5);
        if (auto* system = pages->findChild<DeckLabSystemPage*>(QStringLiteral("decklab-system-page"))) system->showSection(section);
    };
    hooks.showDecks = [this] { selectPage(0); };
    hooks.applyLayout = [this](int decks) {
        const int layout = decks == 4 ? 4 : 2;
        m_config->setValue(ConfigKey("[DeckLab]", "DeckLayout"), layout);
        if (auto* combo = findChild<QComboBox*>("decklab-deck-layout")) combo->setCurrentIndex(std::max(0, combo->findData(layout)));
        updateLayers();
    };
    hooks.legacyTool = [this](int id) {
        if (m_clock.elapsed() - m_lastToolsReady > 2000) return false;
        releaseEffects(); if (id != 1) selectPage(4);
        set(kNative, "tool_request", id); set(kNative, "tool_serial", ++m_toolSerial);
        return true;
    };
    hooks.beforeTool = [this] { releaseEffects(); };
    m_systemUi = new DeckLabSystemUi(this, hooks);
    connect(m_systemUi, &DeckLabSystemUi::toolsConnectedChanged, this, [this](bool connected) {
        const bool wasOnline = m_toolsOnline;
        m_toolsOnline = connected || m_clock.elapsed() - m_lastToolsReady <= 2000;
        for (auto* b : m_toolButtons) b->setEnabled(m_toolsOnline);
        if (wasOnline != m_toolsOnline) updateStatus();
    });
    const QStringList names{tr("Deck"), tr("Levels / Cue"), tr("Display"), tr("Library"), tr("Setup"), tr("System")};
    for (int index = 0; index < names.size(); ++index) {
        if (index == 5) {
            // Wi-Fi, Bluetooth, screen, time, storage, power, about, maintenance.
            pages->addWidget(m_systemUi->createSystemPage(pages));
            auto* link = button(names[index], [pages, index] { pages->setCurrentIndex(index); }, categories,
                    QStringLiteral("decklab-settings-category-%1").arg(index));
            link->setCheckable(true); categorySelection->addButton(link, index); link->setProperty("rail", true);
            connect(pages, &QStackedWidget::currentChanged, link, [link, index](int page) { link->setChecked(page == index); });
            link->setFixedHeight(52); links->addWidget(link);
            continue;
        }
        auto* scroll = new QScrollArea(pages);
        scroll->setObjectName(QStringLiteral("decklab-settings-scroll-%1").arg(index));
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* content = new QWidget(scroll);
        auto* rows = column(content, 10);
        rows->setSpacing(7);
        auto addRow = [content, rows](const QString& label, QWidget* setting) {
            auto* line = row(nullptr, 12);
            auto* caption = new QLabel(label, content);
            caption->setMinimumWidth(160);
            line->addWidget(caption, 1); line->addWidget(setting, 1);
            rows->addLayout(line);
        };
        auto toggle = [this, content](const QString& label, const QString& group, const QString& item) {
            auto* b = button(label, [this, group, item] { set(group, item, !value(group, item)); }, content);
            b->setCheckable(true); b->setChecked(value(group, item) > 0);
            const bool onOff = label == tr("On");
            if (onOff) b->setText(b->isChecked() ? tr("On") : tr("Off"));
            watch(group, item, [b, onOff](double v) {
                b->setChecked(v > 0);
                if (onOff) b->setText(v > 0 ? tr("On") : tr("Off"));
            });
            return b;
        };
        if (index == 0) {
            auto* layout = new QComboBox(content); layout->setObjectName("decklab-deck-layout");
            layout->addItem(tr("Auto · connected controller"), 0);
            layout->addItem(tr("Two decks"), 2); layout->addItem(tr("Four decks"), 4);
            layout->setCurrentIndex(std::max(0, layout->findData(m_config->getValue(ConfigKey("[DeckLab]", "DeckLayout"), 0))));
            connect(layout, qOverload<int>(&QComboBox::activated), this, [this, layout](int n) {
                m_config->setValue(ConfigKey("[DeckLab]", "DeckLayout"), layout->itemData(n).toInt()); updateLayers();
            });
            addRow(tr("Deck layout"), layout);
            // Stems workstream: play stem files as stems or as a normal track.
            if (auto* stems = DeckLabStemPads::createPlaybackSetting(m_config, content)) addRow(tr("Stem tracks"), stems);
            for (int deck = 1; deck <= 4; ++deck) {
                auto* title = new QLabel(tr("DECK %1").arg(deck), content);
                title->setProperty("muted", true); rows->addWidget(title);
                auto* switches = row(nullptr, 4);
                switches->addWidget(toggle(tr("Quantize"), channel(deck), "quantize"));
                switches->addWidget(toggle(tr("Slip"), channel(deck), "slip_enabled"));
                switches->addWidget(toggle(tr("Key lock"), channel(deck), "keylock"));
                rows->addLayout(switches);
                auto* range = new QComboBox(content);
                range->setObjectName(QStringLiteral("decklab-tempo-range-%1").arg(deck));
                range->setAccessibleName(tr("Deck %1 tempo range").arg(deck));
                const std::array<double, 3> ranges{0.08, 0.16, 0.50};
                for (double amount : ranges) range->addItem(QStringLiteral("±%1%").arg(amount * 100), amount);
                auto showRange = [range](double v) {
                    if (!std::isfinite(v) || v <= 0) return;
                    const QSignalBlocker blocker(range);
                    int index = -1;
                    for (int n = 0; n < 3; ++n) if (std::abs(range->itemData(n).toDouble() - v) < 1e-6) index = n;
                    if (range->count() > 3) range->removeItem(3);
                    if (index < 0) {
                        range->addItem(tr("±%1% · current").arg(v * 100, 0, 'g', 4), v);
                        index = 3;
                    }
                    range->setCurrentIndex(index);
                };
                showRange(value(channel(deck), "rateRange"));
                watch(channel(deck), "rateRange", showRange);
                auto enableRange = [this, deck, range] {
                    range->setEnabled(!value(channel(deck), "play") && !value(channel(deck), "sync_enabled"));
                    range->setToolTip(range->isEnabled() ? tr("Tempo slider range") : tr("Stop this deck and release Sync to change range"));
                };
                watch(channel(deck), "play", [enableRange](double) { enableRange(); });
                watch(channel(deck), "sync_enabled", [enableRange](double) { enableRange(); });
                connect(range, qOverload<int>(&QComboBox::activated), this, [this, deck, range](int n) {
                    const double amount = range->itemData(n).toDouble();
                    if (std::isfinite(amount) && amount > 0 && !value(channel(deck), "play") && !value(channel(deck), "sync_enabled"))
                        set(channel(deck), "rateRange", amount);
                });
                enableRange(); addRow(tr("Tempo range"), range);
            }
        } else if (index == 1) {
            for (int deck = 1; deck <= 4; ++deck) addRow(tr("Deck %1 headphones").arg(deck), toggle(tr("Cue"), channel(deck), "pfl"));
            auto* split = toggle(tr("On"), "[Master]", "headSplit");
            split->setObjectName("decklab-head-split"); split->setAccessibleName(tr("Split cue"));
            addRow(tr("Split cue"), split);
            auto* mix = new QSlider(Qt::Horizontal, content);
            mix->setMinimumHeight(44); mix->setRange(0, 100);
            mix->setValue(int((value("[Master]", "headMix") + 1) * 50));
            connect(mix, &QSlider::valueChanged, this, [this](int v) { set("[Master]", "headMix", v / 50.0 - 1); });
            watch("[Master]", "headMix", [mix](double v) { if (!mix->isSliderDown()) { const QSignalBlocker blocker(mix); mix->setValue(int((v + 1) * 50)); } });
            addRow(tr("Cue ← blend → Main"), mix);
        } else if (index == 2) {
            auto* keys = new QComboBox(content);
            keys->addItems({tr("Lancelot · 8A / 8B"), tr("Open Key · 8m / 8d"), tr("Musical · Am / C")});
            QString notation = m_config->getValueString(ConfigKey("[Key]", "KeyNotation"));
            keys->setCurrentIndex(notation == "OpenKey" ? 1 : notation == "Traditional" ? 2 : 0);
            connect(keys, qOverload<int>(&QComboBox::activated), this, &WDeckLab::setKeyNotation);
            addRow(tr("Key format"), keys);
            auto* waveLayout = new QComboBox(content);
            waveLayout->setObjectName(QStringLiteral("decklab-waveform-layout"));
            waveLayout->addItem(tr("Stacked"), 0);
            waveLayout->addItem(tr("Side by side"), 1);
            waveLayout->setCurrentIndex(m_sideBySideWaveforms ? 1 : 0);
            connect(waveLayout, qOverload<int>(&QComboBox::activated), this, [this, waveLayout](int n) {
                const bool sideBySide = waveLayout->itemData(n).toInt() == 1;
                if (sideBySide == m_sideBySideWaveforms) return;
                for (auto* menu : findChildren<QMenu*>())
                    if (menu->objectName().endsWith(QStringLiteral("more-menu"))) menu->close();
                releaseTouchCue(); ++m_interactionEpoch;
                m_sideBySideWaveforms = sideBySide;
                m_config->setValue(ConfigKey("[DeckLab]", "WaveformLayout"), sideBySide ? 1 : 0);
                updateLayers();
            });
            addRow(tr("Decks waveform layout"), waveLayout);
            auto* fps = new QComboBox(content);
            fps->addItems({tr("30 FPS · lower draw cost"), tr("60 FPS · smoother motion")});
            fps->setCurrentIndex(WaveformWidgetFactory::instance()->getFrameRate() > 30 ? 1 : 0);
            connect(fps, qOverload<int>(&QComboBox::activated), this, [](int n) {
                WaveformWidgetFactory::instance()->setFrameRate(n ? 60 : 30);
            });
            addRow(tr("Waveform frame rate"), fps);
            auto* zoom = new QSlider(Qt::Horizontal, content);
            zoom->setMinimumHeight(44); zoom->setRange(1, 10);
            zoom->setValue(int(value(channel(1), "waveform_zoom")));
            connect(zoom, &QSlider::valueChanged, this, [this](int v) {
                for (int deck = 1; deck <= 2; ++deck) set(channel(deck), "waveform_zoom", v);
            });
            addRow(tr("Waveform zoom"), zoom);
            auto* grid = new QSlider(Qt::Horizontal, content);
            grid->setRange(0, 100); grid->setMinimumHeight(44);
            grid->setValue(WaveformWidgetFactory::instance()->getBeatGridAlpha());
            connect(grid, &QSlider::valueChanged, this, [](int v) { WaveformWidgetFactory::instance()->setDisplayBeatGridAlpha(v); });
            addRow(tr("Beatgrid brightness"), grid);
            auto* appearance = new QComboBox(content);
            auto* factory = WaveformWidgetFactory::instance();
            const auto& types = factory->getAvailableTypes();
            for (int n = 0; n < types.size(); ++n) {
                if (types[n].getType() == WaveformWidgetType::Empty) continue;
                appearance->addItem(types[n].getDisplayName(), n);
                if (types[n].getType() == factory->getType()) appearance->setCurrentIndex(appearance->count() - 1);
            }
            connect(appearance, qOverload<int>(&QComboBox::activated), this, [appearance, factory](int n) { factory->setWidgetTypeFromHandle(appearance->itemData(n).toInt()); });
            addRow(tr("Waveform colours"), appearance);
        } else if (index == 3) {
            auto* played = button(tr("Dim played tracks"), [this] {
                bool next = !m_config->getValue(ConfigKey("[Library]", "ApplyPlayedTrackColor"), true);
                m_config->setValue(ConfigKey("[Library]", "ApplyPlayedTrackColor"), next);
                BaseTrackTableModel::setApplyPlayedTrackColor(next);
                if (m_libraryWidget) m_libraryWidget->update();
            }, content);
            played->setCheckable(true);
            played->setChecked(m_config->getValue(ConfigKey("[Library]", "ApplyPlayedTrackColor"), true));
            rows->addWidget(played);
            for (const auto& action : {qMakePair(tr("Get music"), 2), qMakePair(tr("Sample Maker"), 3),
                         qMakePair(tr("Record mix to USB"), 4), qMakePair(tr("Check USB drives"), 10)}) {
                auto* b = button(action.first, [this, id = action.second] { tool(id); }, content,
                        action.second == 2 ? QStringLiteral("decklab-tool-get-music")
                        : action.second == 3 ? QStringLiteral("decklab-tool-sample-maker")
                                             : QStringLiteral("decklab-tool-%1").arg(action.second));
                b->setEnabled(false); m_toolButtons.append(b); rows->addWidget(b);
            }
        } else {
            // Setup: Deck Setup (controller, audio, layout) plus diagnostics.
            rows->addWidget(m_systemUi->createSetupPanel(content));
            m_performanceStatus = new QLabel(content); rows->addWidget(m_performanceStatus);
            // Small-screen type scale: secondary 15 px Medium, buttons 16 px.
            m_performanceStatus->setStyleSheet(QStringLiteral("font-size:15px;font-weight:500;color:#9aa3af;"));
            auto* advanced = button(tr("Advanced settings"), [this] {
                releaseEffects(); emit advancedPreferencesRequested();
            }, content, QStringLiteral("decklab-advanced-settings"));
            advanced->setStyleSheet(QStringLiteral("font-size:16px;font-weight:500;min-height:34px;"));
            rows->addWidget(advanced);
        }
        rows->addStretch(); scroll->setWidget(content);
        new DeckLabTouchScrollGuard(scroll->viewport(), content);
        pages->addWidget(scroll);
        auto* link = button(names[index], [pages, index] { pages->setCurrentIndex(index); }, categories,
                QStringLiteral("decklab-settings-category-%1").arg(index));
        link->setCheckable(true); categorySelection->addButton(link, index);
        link->setChecked(index == 0); link->setProperty("rail", true);
        connect(pages, &QStackedWidget::currentChanged, link, [link, index](int page) { link->setChecked(page == index); });
        link->setFixedHeight(52); links->addWidget(link);
    }
    links->addStretch(); layout->addWidget(categories); layout->addWidget(pages, 1);
    return page;
}

void WDeckLab::ensureDeckSurfaces(int deck) {
    if (m_deckWaveforms[deck - 1]) return;
    // Lane: a full-width waveform with a thin deck-color edge. Off Decks a
    // one-line readout sits above it.
    auto* lane = new QWidget(m_waveZone);
    lane->setObjectName(QStringLiteral("decklab-lane-%1").arg(deck));
    auto* laneRows = column(lane);
    auto* mini = new DeckLabMiniStrip(lane);
    mini->setObjectName(QStringLiteral("decklab-mini-%1").arg(deck));
    mini->setFixedHeight(DeckLabStripRenderer::preferredHeight(DeckLabStripRenderer::Mode::Mini));
    laneRows->addWidget(mini);
    m_minis[deck - 1] = mini;
    auto* body = new QWidget(lane);
    body->setObjectName(QStringLiteral("decklab-lane-body-%1").arg(deck));
    auto* bodyRow = row(body);
    // Deck identity beside the waveform: a deck-color bar with the number.
    auto* edge = new QLabel(QString::number(deck), body);
    edge->setObjectName(QStringLiteral("decklab-lane-edge-%1").arg(deck));
    edge->setAttribute(Qt::WA_StyledBackground);
    edge->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    edge->setStyleSheet(QStringLiteral("background:#0D0F12;color:%1;border-left:3px solid %1;"
                                       "font-family:\"Barlow Semi Condensed\";font-size:16px;font-weight:700;padding-top:4px;")
                                .arg(deckLabDeckColor(deck).name()));
    edge->setFixedWidth(18);
    bodyRow->addWidget(edge);
    laneRows->addWidget(body, 1);
    lane->hide();
    m_deckWaveforms[deck - 1] = lane;
    // Card: the deck readout (a tap target for full track information) and
    // the whole-track overview; the four-deck layout adds direct transport.
    auto* card = new QWidget(m_deckRow);
    card->setObjectName(QStringLiteral("decklab-card-%1").arg(deck));
    card->setAttribute(Qt::WA_StyledBackground);
    card->setProperty("deckCard", true);
    auto* cardRows = column(card);
    auto* strip = new DeckLabDeckStrip(deck, card);
    strip->setObjectName(QStringLiteral("decklab-info-hit-%1").arg(deck));
    strip->setProperty("deckStrip", true);
    strip->setFocusPolicy(Qt::StrongFocus);
    strip->setFixedHeight(DeckLabStripRenderer::preferredHeight(DeckLabStripRenderer::Mode::Full));
    strip->setStyleSheet(deckStripSheet(DeckLabStripRenderer::preferredHeight(DeckLabStripRenderer::Mode::Full)));
    strip->setAccessibleName(tr("Deck %1 full track information").arg(deck));
    strip->setAccessibleDescription(tr("Tap the deck readout to read its full title, artist and source"));
    auto infoPress = std::make_shared<std::pair<quint64, TrackPointer>>();
    connect(strip, &QPushButton::pressed, this, [this, deck, infoPress] {
        auto* player = m_players ? m_players->getPlayer(channel(deck)) : nullptr;
        *infoPress = {m_interactionEpoch, player ? player->getLoadedTrack() : TrackPointer()};
    });
    connect(strip, &QPushButton::clicked, this, [this, deck, infoPress] {
        auto* player = m_players ? m_players->getPlayer(channel(deck)) : nullptr;
        if (m_page == 0 && infoPress->first == m_interactionEpoch && player &&
                infoPress->second == player->getLoadedTrack()) {
            if (infoPress->second) showTrackInfo(deck);
            else selectPage(1);
        }
    });
    strip->installEventFilter(this);
    cardRows->addWidget(strip);
    m_strips[deck - 1] = strip;
    m_meters[deck - 1] = strip->meter();
    m_cardTransports[deck - 1] = buildTransport(deck, false, card);
    cardRows->addWidget(m_cardTransports[deck - 1]);
    m_cardTransports[deck - 1]->hide();
    // Stem pads (stems workstream): a row under the overview in two-deck
    // view while this deck plays a stem file; see updateLayers().
    m_stemPads[deck - 1] = new DeckLabStemPads(deck, DeckLabStemPads::Presentation::Row, m_players,
            [this] { return m_interactionEpoch; }, card);
    cardRows->addWidget(m_stemPads[deck - 1]);
    connect(m_stemPads[deck - 1], &DeckLabStemPads::presenceChanged, this, [this](bool) { updateLayers(); });
    card->hide();
    m_cards[deck - 1] = card;
}

void WDeckLab::addWaveform(QWidget* widget, int deck) {
    if (!widget || deck < 1 || deck > 4) return;
    ensureDeckSurfaces(deck);
    auto* body = m_deckWaveforms[deck - 1]->findChild<QWidget*>(QStringLiteral("decklab-lane-body-%1").arg(deck));
    widget->setParent(body);
    widget->setMinimumHeight(16);
    widget->setObjectName(QStringLiteral("decklab-detail-%1").arg(deck));
    widget->setMaximumHeight(QWIDGETSIZE_MAX);
    static_cast<QBoxLayout*>(body->layout())->addWidget(widget, 1);
    widget->show();
    watch(channel(deck), "waveform_zoom", [this, deck](double zoom) {
        if (!util_isfinite(zoom) || zoom <= 0 || zoom == m_linkedZoom ||
                zoom != value(channel(deck), "waveform_zoom")) return;
        m_linkedZoom = zoom;
        if (m_zoomPending) return;
        m_zoomPending = true;
        QTimer::singleShot(0, this, [this] {
            m_zoomPending = false;
            for (int target = 1; target <= 4; ++target)
                if (value(channel(target), "waveform_zoom") != m_linkedZoom)
                    set(channel(target), "waveform_zoom", m_linkedZoom);
        });
    });
    if (deck == 1) m_linkedZoom = value(channel(deck), "waveform_zoom");
    else if (m_linkedZoom > 0) set(channel(deck), "waveform_zoom", m_linkedZoom);
    updateLayers();
    refreshDeck(deck);
}

void WDeckLab::addOverview(QWidget* widget, int deck) {
    if (!widget || deck < 1 || deck > 4) return;
    ensureDeckSurfaces(deck);
    widget->setObjectName(QStringLiteral("decklab-overview-%1").arg(deck));
    widget->setFixedHeight(kOverviewHeight);
    widget->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    widget->setFocusPolicy(Qt::NoFocus); widget->setAcceptDrops(false);
    widget->setContextMenuPolicy(Qt::NoContextMenu);
    widget->setAccessibleName(tr("Deck %1 whole-track overview; touch to search while paused").arg(deck));
    // Needle search: touching the overview seeks a paused deck. It is locked
    // while the deck plays, so a stray touch can never jump a live track.
    new DeckLabNeedleSearch(widget,
            [this, deck] {
                const auto& d = m_deck[deck - 1];
                return m_page == 0 && d.loaded->get() > 0 && d.play->get() <= 0;
            },
            [this, deck](double fraction) {
                set(channel(deck), QStringLiteral("playposition"), fraction);
                markDeckDirty(deck);
            });
    widget->setParent(m_cards[deck - 1]);
    static_cast<QBoxLayout*>(m_cards[deck - 1]->layout())->insertWidget(1, widget);
    widget->show();
    m_overviews[deck - 1] = widget;
    updateLayers();
}

void WDeckLab::bindBrowser(WSearchLineEdit* search, WLibrarySidebar* sidebar, WLibrary* library) {
    m_search = search; m_sidebar = sidebar; m_libraryWidget = library;
    search->setMinimumHeight(40); search->setMaximumHeight(40);
    search->setItemDelegate(new DeckLabComboDelegate(search));
    search->setMaxVisibleItems(6);
    search->setAccessibleName(tr("Search tracks"));
    m_searchRow->addWidget(search, 1);
    auto* clear = button(tr("Clear"), [search] { search->slotClearSearch(); }, m_browser);
    holdHeight(clear, 44);
    m_searchRow->addWidget(clear);
    m_keyboard = new QWidget(this); m_keyboard->setObjectName("decklab-keyboard");
    m_keyboard->setStyleSheet(QStringLiteral("QPushButton{font-size:16px;font-weight:600;}"));
    m_searchPanel->setStyleSheet(QStringLiteral(
            "QPushButton{font-size:16px;font-weight:600;} QComboBox,QLineEdit{font-size:16px;font-weight:500;}"));
    // Five rows of 44 px touch targets.
    auto* keys = column(m_keyboard); m_keyboard->setFixedHeight(220);
    const QStringList rows{"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    for (const QString& letters : rows) {
        auto* line = row(nullptr, 2);
        for (QChar letter : letters) {
            auto* key = button(QString(letter), [search, letter] { if (search->lineEdit()) search->lineEdit()->insert(QString(letter).toLower()); }, m_keyboard);
            holdHeight(key, 44, QStringLiteral("padding:0px;border-radius:0px;")); line->addWidget(key, 1);
        }
        keys->addLayout(line);
    }
    auto* actions = row(nullptr, 2);
    for (const QString& mark : {QStringLiteral("-"), QStringLiteral("."), QStringLiteral("/")}) {
        auto* key = button(mark, [search, mark] { if (search->lineEdit()) search->lineEdit()->insert(mark); }, m_keyboard);
        holdHeight(key, 44); key->setMinimumWidth(56); actions->addWidget(key);
    }
    auto* space = button(tr("Space"), [search] { if (search->lineEdit()) search->lineEdit()->insert(" "); }, m_keyboard);
    holdHeight(space, 44); actions->addWidget(space, 1);
    auto* backspace = button(tr("Delete"), [search] { if (search->lineEdit()) search->lineEdit()->backspace(); }, m_keyboard);
    backspace->setAutoRepeat(true); backspace->setAutoRepeatDelay(400); backspace->setAutoRepeatInterval(100);
    holdHeight(backspace, 44); actions->addWidget(backspace);
    auto* done = button(tr("Done"), [this] { setSearchOpen(false); }, m_keyboard, "decklab-search-done");
    holdHeight(done, 44); done->setMinimumWidth(88); actions->addWidget(done);
    keys->addLayout(actions); m_root->addWidget(m_keyboard); m_keyboard->hide();
    auto& b = *m_browseState;
    b.listStack->insertWidget(0, sidebar); b.listStack->setCurrentWidget(sidebar);
    m_browseStack->addWidget(library);
    sidebar->setIndentation(0);
    sidebar->setRootIsDecorated(false);
    sidebar->setItemsExpandable(false);
    sidebar->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    auto* sourceRows = new DeckLabListDelegate(sidebar);
    sourceRows->setGlyphProvider(sourceGlyph);
    sidebar->setItemDelegate(sourceRows);
    new DeckLabTouchScrollGuard(sidebar->viewport(), sidebar->viewport());
    sidebar->installEventFilter(this);
    sidebar->setEditTriggers(QAbstractItemView::NoEditTriggers);
    if (auto* model = qobject_cast<SidebarModel*>(sidebar->model()))
        disconnect(sidebar, &QTreeView::clicked, model, &SidebarModel::clicked);
    connect(sidebar, &QTreeView::clicked, this, [this](const QModelIndex& index) {
        if (index.isValid()) openSource();
    });
    connect(sidebar->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex&, const QModelIndex&) { updateBreadcrumb(); refreshBrowseCard(); });
    connect(search, &WSearchLineEdit::search, this, [this](const QString&) {
        // Typing replaces a category filter; the breadcrumb must not claim it.
        m_browseState->filter.clear(); m_browseState->fromCategories = false;
        showTracks();
    });
    if (m_library) {
        connect(m_library, &Library::trackSelected, this, [this](TrackPointer track) {
            m_browseState->selectedTrack = track;
            if (m_browseStack->currentIndex() != 0) refreshBrowseCard();
            refreshLoadButtons();
        });
        b.card->enableCovers();
    }
    if (m_library) m_library->setRowHeight(44);
    showSources();
}

void WDeckLab::selectPage(int page) {
    if (page < 0 || page > 4 || page == m_page) return;
    if (m_page == 3) releaseEffects();
    setSearchOpen(false);
    if (m_page == 1 && page != 1) m_sampleTarget = 0;
    releaseTouchCue(); ++m_interactionEpoch;
    m_page = page;
    // 44 px rail targets on every page.
    for (auto* tab : m_tabs) {
        tab->ensurePolished();
        setFixedHeightIf(tab, 44);
    }
    if (auto* usb = findChild<QPushButton*>(QStringLiteral("decklab-usb"))) {
        usb->ensurePolished();
        setFixedHeightIf(usb, 44);
    }
    // Keep the existing mapping's public page numbers: 4 = tools, 5 = settings.
    set(kTab, "current", page == 4 ? 5 : page);
    for (int n = 0; n < 5; ++n) m_tabs[n]->setChecked(n == page);
    m_pages->setCurrentIndex(page);
    // Transport has a fixed place below the Decks waveforms. Keeping it out
    // of the expanding page stack avoids sibling-page minimum sizes pushing
    // the essential controls below a small display.
    m_pages->setVisible(page != 0);
    m_deckControls->hide();
    m_root->removeWidget(m_waveforms);
    if (page == 0) {
        m_waveforms->setMinimumHeight(0); m_waveforms->setMaximumHeight(QWIDGETSIZE_MAX);
        m_root->insertWidget(1, m_waveforms, 1);
    } else {
        m_pages->setMinimumHeight(0); m_pages->setMaximumHeight(QWIDGETSIZE_MAX);
        m_waveforms->setFixedHeight(page == 1 ? 104 : 140);
        m_root->insertWidget(page == 1 ? 2 : 1, m_waveforms);
    }
    m_waveforms->setVisible(page == 0 || page == 1 || page == 3);
    m_root->setStretchFactor(m_pages, page == 0 ? 0 : 1);
    if (page == 1) {
        if (!m_hasBrowsed) { m_hasBrowsed = true; showSources(); }
        else if (m_browseStack->currentIndex() == 0) showSources(); else showTracks();
    }
    updateLayers();
    refreshLoadButtons(); refreshFx(); updateStatus();
}

void WDeckLab::showSources() {
    if (!m_sidebar || !m_browseState) return;
    auto& b = *m_browseState;
    // Captured before any widget switch can move focus to the sidebar.
    const QPersistentModelIndex highlighted(m_sidebar->currentIndex());
    m_browseStack->setCurrentWidget(b.listStack);
    if (value("[Sidebar]", "sidebar_visible") != 1) set("[Sidebar]", "sidebar_visible", 1);
    if (b.list == DeckLabList::Sources) {
        m_sidebar->setFocusProxy(nullptr);
        b.listStack->setCurrentWidget(m_sidebar);
        set("[Library]", "focused_widget", 2);
        if (m_page == 1) m_sidebar->setFocus(Qt::OtherFocusReason);
        // Keep the highlighted source (for example after Back from categories).
        if (highlighted.isValid() && highlighted.parent() == m_sidebar->rootIndex() &&
                m_sidebar->currentIndex() != highlighted) {
            m_sidebar->setCurrentIndex(highlighted);
        }
        if (!m_sidebar->currentIndex().isValid() && m_sidebar->model()->rowCount(m_sidebar->rootIndex())) {
            m_sidebar->setCurrentIndex(m_sidebar->model()->index(0, 0, m_sidebar->rootIndex()));
        }
    } else {
        b.listStack->setCurrentWidget(b.items);
        // Focus requests for the sidebar (focused_widget 2) reach this list.
        m_sidebar->setFocusProxy(b.items);
        set("[Library]", "focused_widget", 2);
        if (m_page == 1) b.items->setFocus(Qt::OtherFocusReason);
        if (b.items->currentRow() < 0 && b.items->count()) b.items->setCurrentRow(0);
    }
    updateBreadcrumb(); refreshLoadButtons(); refreshBrowseCard();
}
void WDeckLab::showTracks() {
    if (!m_libraryWidget) return;
    m_browseStack->setCurrentWidget(m_libraryWidget);
    if (value("[Sidebar]", "sidebar_visible")) set("[Sidebar]", "sidebar_visible", 0);
    set("[Library]", "focused_widget", 3);
    syncSelectedTrack();
    updateBreadcrumb(); refreshLoadButtons(); refreshBrowseCard();
}
void WDeckLab::browsePress() {
    if (m_page != 1) { selectPage(1); return; }
    // In a track list the press previews the highlighted track.
    if (m_browseStack->currentIndex() != 0) { togglePreview(); return; }
    browseEnter();
}
void WDeckLab::browseEnter() {
    if (m_page != 1) { selectPage(1); return; }
    if (!m_sidebar || m_browseStack->currentIndex() != 0) return;
    auto& b = *m_browseState;
    if (b.list == DeckLabList::Categories) openCategory(b.items->currentRow());
    else if (b.list == DeckLabList::Values) openCategoryValue(b.items->currentRow());
    else openSource();
}
void WDeckLab::browseMove(int delta, bool listLevel) {
    if (delta == 0 || !m_sidebar) return;
    auto& b = *m_browseState;
    // A turn on any other page opens Browse where it was left. It never
    // becomes a key press on a deck, tempo or settings control.
    if (m_page != 1) selectPage(1);
    if (listLevel && m_browseStack->currentIndex() != 0) showSources();
    if (m_browseStack->currentIndex() != 0) {
        if (auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr) table->moveSelection(delta);
        return;
    }
    if (b.list == DeckLabList::Sources) {
        // Selection never activates or imports the items a fast knob passes.
        const QModelIndex root = m_sidebar->rootIndex();
        const int count = m_sidebar->model()->rowCount(root);
        if (!count) return;
        const QModelIndex current = m_sidebar->currentIndex();
        const int from = current.isValid() && current.parent() == root ? current.row() : -1;
        const QModelIndex index = m_sidebar->model()->index(std::clamp(from + delta, 0, count - 1), 0, root);
        m_sidebar->setCurrentIndex(index);
        m_sidebar->scrollTo(index);
        return;
    }
    const int count = b.items->count();
    if (!count) return;
    const int rowIndex = std::clamp(b.items->currentRow() + delta, 0, count - 1);
    b.items->setCurrentRow(rowIndex);
    b.items->scrollToItem(b.items->item(rowIndex));
}
void WDeckLab::openSource() {
    if (!m_sidebar) return;
    const QPersistentModelIndex index(m_sidebar->currentIndex());
    if (!index.isValid()) return;
    auto* model = qobject_cast<SidebarModel*>(m_sidebar->model());
    if (!model) return;
    // The model owns source activation, including asynchronous Rekordbox import.
    model->doubleClicked(index); // Populate lazy USB/filesystem children before deciding.
    if (!index.isValid()) return;
    model->clicked(index);
    if (!index.isValid()) return;
    // The collection and rekordbox USB exports open on categories first.
    if (isCollectionSource(index) || isRekordboxDevice(index)) { enterCategories(index); return; }
    if ((!model->hasTrackTable(index) && model->hasChildren(index)) || (!index.parent().isValid() && !model->hasTrackTable(index))) {
        m_sidebar->setRootIndex(index);
        const QModelIndex child = model->index(0, 0, index);
        if (child.isValid()) m_sidebar->setCurrentIndex(child);
        showSources();
    } else showTracks();
}
void WDeckLab::browseBack() {
    if (m_keyboard && m_keyboard->isVisible()) { setSearchOpen(false); return; }
    if (m_page != 1) { selectPage(1); return; }
    if (!m_sidebar) return;
    auto& b = *m_browseState;
    if (m_browseStack->currentIndex() != 0) {
        // Leaving a category's tracks restores the whole source.
        clearBrowseFilter();
        b.fromCategories = false;
        showSources();
        return;
    }
    if (b.list == DeckLabList::Values) { showCategories(); return; }
    if (b.list == DeckLabList::Categories) {
        const QPersistentModelIndex source = b.source;
        resetBrowseLevels();
        if (source.isValid()) {
            m_sidebar->setRootIndex(source.parent());
            m_sidebar->setCurrentIndex(source);
        }
        showSources();
        return;
    }
    if (b.returnRoot.isValid() && m_sidebar->rootIndex() == b.returnRoot && b.source.isValid()) {
        b.returnRoot = QPersistentModelIndex();
        activateBrowseSource();
        showCategories();
        return;
    }
    const QPersistentModelIndex previous(m_sidebar->rootIndex());
    if (previous.isValid()) {
        m_sidebar->setRootIndex(previous.parent());
        m_sidebar->setCurrentIndex(previous);
        showSources();
    } else selectPage(0);
}
QStringList WDeckLab::browseSegments() const {
    const auto& b = *m_browseState;
    const auto pathTo = [](QModelIndex at) {
        QStringList path;
        while (at.isValid()) { path.prepend(at.data().toString()); at = at.parent(); }
        return path;
    };
    const bool tracks = m_browseStack->currentIndex() != 0;
    if (b.source.isValid() && ((!tracks && b.list != DeckLabList::Sources) || (tracks && b.fromCategories))) {
        QStringList path = pathTo(b.source);
        const bool filtered = tracks && !b.filter.isEmpty();
        if ((!tracks && b.list == DeckLabList::Values) || filtered) path << decklab::categoryName(b.category);
        if (tracks) path << (filtered ? b.valueLabel : decklab::categoryName(decklab::Category::AllTracks));
        return path;
    }
    QStringList path = pathTo(tracks ? m_sidebar->currentIndex() : m_sidebar->rootIndex());
    if (b.rekordbox && !tracks && b.returnRoot.isValid() && m_sidebar->rootIndex() == b.returnRoot) {
        path << decklab::categoryName(decklab::Category::Playlists);
    }
    if (path.isEmpty()) path.append(tr("Sources"));
    return path;
}
void WDeckLab::updateBreadcrumb() {
    if (!m_sidebar || !m_browseState || !m_browseState->breadcrumb) return;
    m_browseState->breadcrumb->setSegments(browseSegments());
}
void WDeckLab::loadDeck(int deck) {
    if (m_sampleTarget) {
        if (deck == 2) { m_sampleTarget = 0; selectPage(m_sampleReturnPage); return; }
        const QString group = QStringLiteral("[Sampler%1]").arg(m_sampleTarget);
        if (m_browseStack->currentWidget() != m_libraryWidget || value(group, "play")) return;
        if (auto* table = m_libraryWidget->getCurrentTrackTableView()) table->loadSelectedTrackToGroup(group,
#ifdef __STEM__
                mixxx::StemChannelSelection(),
#endif
                false);
        return;
    }
    if (!m_fourDeckLayout && deck > 2) return;
    deck = m_fourDeckLayout ? deck : displayedDeck(deck - 1);
    // ON AIR is never replaced from the touch screen; say so instead of ignoring the tap.
    if (value(channel(deck), "play")) { showLoadRefused(deck); return; }
    if (m_page != 1 || !m_libraryWidget || m_browseStack->currentWidget() != m_libraryWidget) return;
    set("[Library]", "focused_widget", 3);
    pulse(channel(deck), "LoadSelectedTrack");
    // Return happens on newTrackLoaded, never merely on a button press.
}
void WDeckLab::refreshLoadButtons() {
    if (!m_loadButtons[0]) return;
    using Tone = DeckLabActionButton::Tone;
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    const bool tracks = m_libraryWidget && m_browseStack->currentWidget() == m_libraryWidget;
    const bool selected = table && table->selectionModel() && !table->selectionModel()->selectedRows().isEmpty();
    for (int deck = 1; deck <= 4; ++deck) {
        auto* load = static_cast<DeckLabActionButton*>(m_loadButtons[deck - 1]);
        load->setVisible(deck <= 2 || (m_fourDeckLayout && !m_sampleTarget));
        if (m_sampleTarget) {
            const bool ready = tracks && selected && !value(QStringLiteral("[Sampler%1]").arg(m_sampleTarget), "play");
            load->setText(deck == 1 ? tr("Load pad %1").arg(m_sampleTarget) : tr("Cancel"));
            load->setDetail(deck == 1 ? tr("Sampler") : tr("Back to pads"));
            load->setTone(deck == 1 && ready ? Tone::Ready : Tone::Neutral);
            load->setEnabled(deck == 2 || ready);
            load->setAccessibleName(load->text());
            continue;
        }
        const int target = m_fourDeckLayout ? deck : (deck <= 2 ? displayedDeck(deck - 1) : deck);
        const bool playing = value(channel(target), "play") > 0;
        const bool loaded = value(channel(target), "track_loaded") > 0;
        load->setText(tr("Load %1").arg(target));
        load->setDetail(playing ? tr("On air") : loaded ? tr("Paused") : tr("Empty deck"));
        load->setTone(playing ? Tone::OnAir : tracks && selected ? Tone::Ready : Tone::Neutral);
        load->setEnabled(!playing && tracks && selected);
        load->setAccessibleName(playing ? tr("Deck %1 is on air and cannot be loaded").arg(target)
                                        : tr("Load deck %1").arg(target));
    }
    refreshPreviewButton();
    scheduleMatchRefresh();
}
void WDeckLab::resetBrowseLevels() {
    auto& b = *m_browseState;
    clearBrowseFilter();
    b.list = DeckLabList::Sources;
    b.source = QPersistentModelIndex();
    b.returnRoot = QPersistentModelIndex();
    b.fromCategories = false;
    b.rekordbox = false;
    b.loadingTimer.stop();
}
void WDeckLab::clearBrowseFilter() {
    auto& b = *m_browseState;
    if (b.filter.isEmpty()) return;
    b.filter.clear();
    if (m_libraryWidget) m_libraryWidget->search(QString());
}
void WDeckLab::activateBrowseSource() {
    auto& b = *m_browseState;
    auto* model = m_sidebar ? qobject_cast<SidebarModel*>(m_sidebar->model()) : nullptr;
    if (model && b.source.isValid()) model->clicked(b.source);
}
void WDeckLab::enterCategories(const QModelIndex& source) {
    auto& b = *m_browseState;
    b.source = source;
    b.rekordbox = isRekordboxDevice(source);
    b.filter.clear();
    b.fromCategories = false;
    b.returnRoot = QPersistentModelIndex();
    b.categoryRow = 0;
    b.trackCount = -1;
    // A source opens unfiltered: drop typed or earlier category searches.
    if (m_search) m_search->slotRestoreSearch(QString());
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    auto* tracks = table ? dynamic_cast<TrackModel*>(table->model()) : nullptr;
    const bool ready = !b.rekordbox || rekordboxImported(source);
    if (ready && tracks && !tracks->currentSearch().isEmpty()) m_libraryWidget->search(QString());
    if (ready && table && table->model()) b.trackCount = table->model()->rowCount();
    showCategories();
}
void WDeckLab::showCategories() {
    auto& b = *m_browseState;
    using decklab::Category;
    const bool ready = !b.rekordbox || rekordboxImported(b.source);
    const QAbstractItemModel* sources = m_sidebar ? m_sidebar->model() : nullptr;
    const QString loading = tr("Loading…");
    QList<std::tuple<Category, bool, QString>> entries;
    entries.append(std::make_tuple(Category::AllTracks, ready, !ready ? loading : b.trackCount >= 0 ? QString::number(b.trackCount) : QString()));
    if (b.rekordbox) {
        const int playlists = ready && sources && b.source.isValid() ? sources->rowCount(b.source) : 0;
        if (!ready || playlists) entries.append(std::make_tuple(Category::Playlists, playlists > 0, ready ? QString::number(playlists) : loading));
    } else {
        if (sidebarRoot(sources, QStringLiteral("playlist")).isValid()) entries.append(std::make_tuple(Category::Playlists, true, QString()));
        if (sidebarRoot(sources, QStringLiteral("crates")).isValid()) entries.append(std::make_tuple(Category::Crates, true, QString()));
    }
    for (Category category : {Category::Artist, Category::Album, Category::Bpm, Category::Key}) {
        entries.append(std::make_tuple(category, ready, ready ? QString() : loading));
    }
    if (!b.rekordbox && sidebarRoot(sources, QStringLiteral("history")).isValid()) {
        entries.append(std::make_tuple(Category::History, true, QString()));
    }
    {
        const QSignalBlocker blocker(b.items);
        b.items->clear();
        for (const auto& [category, enabled, detail] : entries) {
            auto* item = new QListWidgetItem(decklab::categoryName(category), b.items);
            item->setData(DeckLabListDelegate::CategoryRole, static_cast<int>(category));
            item->setData(DeckLabListDelegate::GlyphRole, static_cast<int>(categoryGlyph(category)));
            item->setData(DeckLabListDelegate::ChevronRole, true);
            item->setData(DeckLabListDelegate::CountRole, detail);
            if (!enabled) item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        }
        b.items->setCurrentRow(std::clamp(b.categoryRow, 0, std::max(0, b.items->count() - 1)));
    }
    b.list = DeckLabList::Categories;
    if (ready) b.loadingTimer.stop(); else b.loadingTimer.start();
    showSources();
}
void WDeckLab::openCategory(int rowIndex) {
    auto& b = *m_browseState;
    auto* item = b.items->item(rowIndex);
    if (!item || !(item->flags() & Qt::ItemIsEnabled) || !m_sidebar) return;
    using decklab::Category;
    const auto category = static_cast<Category>(item->data(DeckLabListDelegate::CategoryRole).toInt());
    b.categoryRow = rowIndex;
    b.category = category;
    if (category == Category::AllTracks) {
        activateBrowseSource();
        clearBrowseFilter();
        b.fromCategories = true;
        showTracks();
        return;
    }
    if (category == Category::Playlists || category == Category::Crates || category == Category::History) {
        if (b.rekordbox) {
            if (!b.source.isValid() || !m_sidebar->model()->rowCount(b.source)) return;
            b.list = DeckLabList::Sources;
            b.returnRoot = b.source;
            m_sidebar->setRootIndex(b.source);
            m_sidebar->setCurrentIndex(m_sidebar->model()->index(0, 0, b.source));
            showSources();
            return;
        }
        const QModelIndex root = sidebarRoot(m_sidebar->model(),
                category == Category::Playlists ? QStringLiteral("playlist")
                        : category == Category::Crates ? QStringLiteral("crates") : QStringLiteral("history"));
        if (!root.isValid()) return;
        b.list = DeckLabList::Sources;
        m_sidebar->setRootIndex(QModelIndex());
        m_sidebar->setCurrentIndex(root);
        openSource();
        b.returnRoot = m_sidebar->rootIndex().isValid() ? QPersistentModelIndex(m_sidebar->rootIndex()) : QPersistentModelIndex(root);
        updateBreadcrumb();
        return;
    }
    // Artist, Album, BPM and Key: distinct values from the source's own model.
    activateBrowseSource();
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    auto* tracks = table ? dynamic_cast<TrackModel*>(table->model()) : nullptr;
    if (!tracks) return;
    if (!tracks->currentSearch().isEmpty()) m_libraryWidget->search(QString());
    const TrackModel::SortColumnId field = category == Category::Artist ? TrackModel::SortColumnId::Artist
            : category == Category::Album ? TrackModel::SortColumnId::Album
            : category == Category::Bpm ? TrackModel::SortColumnId::Bpm : TrackModel::SortColumnId::Key;
    const auto values = decklab::collectCategoryValues(*table->model(), tracks->columnIndexFromSortColumnId(field), category);
    {
        const QSignalBlocker blocker(b.items);
        b.items->clear();
        for (const auto& entry : values) {
            auto* item = new QListWidgetItem(entry.label, b.items);
            item->setData(DeckLabListDelegate::CountRole, QString::number(entry.count));
            item->setData(DeckLabListDelegate::QueryRole, entry.query);
            item->setData(DeckLabListDelegate::ChevronRole, true);
            if (entry.accent.isValid()) item->setData(DeckLabListDelegate::AccentRole, entry.accent);
        }
        if (values.isEmpty()) {
            auto* item = new QListWidgetItem(tr("No tracks"), b.items);
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        }
        b.items->setCurrentRow(0);
    }
    b.list = DeckLabList::Values;
    showSources();
}
void WDeckLab::openCategoryValue(int rowIndex) {
    auto& b = *m_browseState;
    auto* item = b.items->item(rowIndex);
    if (!item || !(item->flags() & Qt::ItemIsEnabled) || !m_libraryWidget) return;
    const QString query = item->data(DeckLabListDelegate::QueryRole).toString();
    if (query.isEmpty()) return;
    b.valueLabel = item->text();
    activateBrowseSource();
    // The breadcrumb names the filter; keep the search box free for typing.
    if (m_search) m_search->slotRestoreSearch(QString());
    m_libraryWidget->search(query);
    b.filter = query;
    b.fromCategories = true;
    showTracks();
}
void WDeckLab::checkRekordboxImport() {
    auto& b = *m_browseState;
    if (!b.rekordbox || !b.source.isValid()) { b.loadingTimer.stop(); return; }
    // Keep polling (one data() call) until Browse is visible again; never
    // move focus or Library state while another page is shown.
    if (!rekordboxImported(b.source) || m_page != 1) return;
    b.loadingTimer.stop();
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    if (table && table->model()) b.trackCount = table->model()->rowCount();
    if (b.list == DeckLabList::Categories && m_browseStack->currentIndex() == 0) showCategories();
}
void WDeckLab::syncSelectedTrack() {
    auto& b = *m_browseState;
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    auto* tracks = table ? dynamic_cast<TrackModel*>(table->model()) : nullptr;
    const QModelIndexList rows = table && table->selectionModel() ? table->selectionModel()->selectedRows() : QModelIndexList();
    if (!tracks || rows.size() != 1) { b.selectedTrack.reset(); return; }
    if (b.selectedTrack && tracks->getTrackLocation(rows.first()) == b.selectedTrack->getLocation()) return;
    b.selectedTrack = tracks->getTrack(rows.first());
}
void WDeckLab::refreshBrowseCard() {
    if (!m_browseState) return;
    auto& b = *m_browseState;
    if (!b.card || !m_browseStack) return;
    if (m_browseStack->currentIndex() != 0) { b.card->showTrack(b.selectedTrack); return; }
    const QString openHint = tr("Turn or swipe to choose · press or tap to open");
    if (b.list != DeckLabList::Sources) {
        const QListWidgetItem* item = b.items->currentItem();
        if (!item) return;
        const QString source = b.source.isValid() ? b.source.data().toString() : QString();
        if (b.list == DeckLabList::Categories) {
            const auto category = static_cast<decklab::Category>(item->data(DeckLabListDelegate::CategoryRole).toInt());
            const QString count = item->data(DeckLabListDelegate::CountRole).toString();
            b.card->showItem(categoryGlyph(category), item->text(),
                    count.isEmpty() || category != decklab::Category::AllTracks ? source
                            : count == QStringLiteral("1") ? tr("%1 · 1 track").arg(source) : tr("%1 · %2 tracks").arg(source, count),
                    (item->flags() & Qt::ItemIsEnabled) ? openHint : tr("Reading the USB export…"));
        } else {
            const QString count = item->data(DeckLabListDelegate::CountRole).toString();
            b.card->showItem(categoryGlyph(b.category), item->text(),
                    count == QStringLiteral("1") ? tr("%1 · 1 track").arg(decklab::categoryName(b.category))
                                                 : tr("%1 · %2 tracks").arg(decklab::categoryName(b.category), count),
                    tr("Press or tap to show these tracks"),
                    item->data(DeckLabListDelegate::AccentRole).value<QColor>());
        }
        return;
    }
    const QModelIndex index = m_sidebar ? m_sidebar->currentIndex() : QModelIndex();
    if (!index.isValid()) {
        b.card->showItem(decklab::Glyph::Folder, tr("Sources"), tr("Library, playlists and USB drives"), openHint);
        return;
    }
    QString subtitle;
    const QString icon = rootIconName(index);
    const int children = index.model()->rowCount(index);
    if (isCollectionSource(index)) subtitle = tr("Whole library, by category");
    else if (isRekordboxDevice(index)) subtitle = rekordboxImported(index) ? tr("USB drive · rekordbox export") : tr("USB drive · reads its rekordbox export when opened");
    else if (!index.parent().isValid() && icon == QStringLiteral("rekordbox")) subtitle = tr("USB drives with a rekordbox export");
    else if (children == 1) subtitle = tr("1 item");
    else if (children > 0) subtitle = tr("%1 items").arg(children);
    else if (icon == QStringLiteral("history")) subtitle = tr("Played in an earlier session");
    else if (icon == QStringLiteral("crates")) subtitle = tr("Crate");
    else subtitle = tr("Track list");
    b.card->showItem(sourceGlyph(index), index.data().toString(), subtitle, openHint);
}
void WDeckLab::refreshPreviewButton() {
    if (!m_browseState) return;
    auto& b = *m_browseState;
    if (!b.preview) return;
    using Tone = DeckLabActionButton::Tone;
    const bool available = ControlObject::exists(ConfigKey(QStringLiteral("[PreviewDeck1]"), QStringLiteral("LoadSelectedTrackAndPlay")));
    const bool playing = available && value(QStringLiteral("[PreviewDeck1]"), QStringLiteral("play")) > 0;
    auto* previewDeck = m_players && m_players->numberOfPreviewDecks() > 0 ? m_players->getPreviewDeck(0) : nullptr;
    const TrackPointer previewing = previewDeck ? previewDeck->getLoadedTrack() : TrackPointer();
    const bool tracks = m_browseStack && m_browseStack->currentIndex() != 0;
    const bool same = playing && previewing && previewing == b.selectedTrack;
    b.preview->setText(same ? tr("Stop") : tr("Preview"));
    b.preview->setGlyph(same ? decklab::Glyph::Stop : decklab::Glyph::Play);
    b.preview->setTone(same ? Tone::Active : Tone::Neutral);
    b.preview->setDetail(playing ? (same ? tr("Playing in headphones") : tr("Another track playing")) : tr("Headphones"));
    b.preview->setEnabled(available && ((tracks && b.selectedTrack) || playing));
}
void WDeckLab::togglePreview() {
    const ConfigKey load(QStringLiteral("[PreviewDeck1]"), QStringLiteral("LoadSelectedTrackAndPlay"));
    if (!ControlObject::exists(load)) return;
    const bool tracks = m_browseStack->currentIndex() != 0;
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    const bool selected = table && table->selectionModel() && !table->selectionModel()->selectedRows().isEmpty();
    // Same track: Mixxx toggles play/pause; another track: load and play it.
    if (tracks && selected) pulse(load.group, load.item);
    else stopPreview();
}
void WDeckLab::stopPreview() {
    const ConfigKey play(QStringLiteral("[PreviewDeck1]"), QStringLiteral("play"));
    if (ControlObject::exists(play) && value(play.group, play.item) > 0) set(play.group, play.item, 0);
}
void WDeckLab::scheduleMatchRefresh() {
    if (m_browseState && !m_browseState->matchTimer.isActive()) m_browseState->matchTimer.start();
}
int WDeckLab::matchReferenceDeck() {
    // Same rule as Tools > Match Deck: the playing (sync leader) deck, else a loaded displayed deck.
    int matchDeck = displayedDeck(0);
    bool foundPlaying = false;
    for (int deck = 1; deck <= 4; ++deck) {
        if (!value(channel(deck), "track_loaded")) continue;
        if (value(channel(deck), "play") && (!foundPlaying || value(channel(deck), "sync_leader") > 0)) {
            matchDeck = deck; foundPlaying = true;
        } else if (!foundPlaying && !value(channel(matchDeck), "track_loaded")) matchDeck = deck;
    }
    return value(channel(matchDeck), "track_loaded") > 0 ? matchDeck : 0;
}
void WDeckLab::refreshMatchReference() {
    if (!m_browseState) return;
    auto& b = *m_browseState;
    decklab::MatchReference reference;
    reference.enabled = b.matchEnabled;
    reference.deck = matchReferenceDeck();
    if (reference.deck) {
        reference.key = KeyUtils::keyFromNumericValue(value(channel(reference.deck), "visual_key"));
        reference.bpm = value(channel(reference.deck), "bpm");
    }
    if (b.match) {
        QString detail = tr("Off");
        if (b.matchEnabled) {
            detail = tr("No deck");
            if (reference.deck) {
                QStringList parts{tr("D%1", "Short deck label, e.g. D2").arg(reference.deck)};
                const QString key = KeyUtils::keyToString(reference.key);
                if (reference.key != mixxx::track::io::key::INVALID && !key.isEmpty()) parts << key;
                if (reference.bpm > 0) parts << QString::number(reference.bpm, 'f', 0);
                detail = parts.join(QStringLiteral(" · "));
            }
        }
        b.match->setDetail(detail);
        b.match->setTone(b.matchEnabled ? DeckLabActionButton::Tone::Active : DeckLabActionButton::Tone::Neutral);
        b.match->setAccessibleName(b.matchEnabled ? tr("Harmonic match highlighting on, %1").arg(detail)
                                                  : tr("Harmonic match highlighting off"));
    }
    if (reference.sameAs(decklab::matchReference())) return;
    decklab::setMatchReference(reference);
    // Repaint only: rows evaluate the new reference while painting.
    if (auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr) {
        if (table->isVisible()) table->viewport()->update();
    }
    if (b.card) b.card->refreshMatch();
}
void WDeckLab::showLoadRefused(int deck) {
    if (m_browseState && m_browseState->toast) {
        m_browseState->toast->showMessage(tr("Deck %1 is playing – pause it to load").arg(deck), 2600, true);
    }
}
void WDeckLab::showLoadUnavailable(int slot) {
    if (m_sampleTarget || !m_browseState->toast) return;
    if (!m_fourDeckLayout && slot > 2) return;
    const int target = m_fourDeckLayout ? slot : displayedDeck(slot - 1);
    if (value(channel(target), "play") > 0) { showLoadRefused(target); return; }
    m_browseState->toast->showMessage(m_browseStack->currentIndex() == 0
            ? tr("Open a playlist or category, then choose a track")
            : tr("Choose a track first"), 2000);
}
void WDeckLab::refreshDeck(int deck) {
    if (deck < 1 || deck > 4) return;
    const auto& d = m_deck[deck - 1];
    auto* player = m_players ? m_players->getPlayer(channel(deck)) : nullptr;
    const TrackPointer track = player ? player->getLoadedTrack() : TrackPointer();
    if (track != m_tracks[deck - 1]) {
        // Metadata, key and grid edits on the loaded track refresh this deck.
        for (const auto& connection : std::as_const(m_trackConnections[deck - 1])) disconnect(connection);
        m_trackConnections[deck - 1].clear();
        if (track) {
            const auto dirty = [this, deck] { markDeckDirty(deck); };
            m_trackConnections[deck - 1] << connect(track.get(), &Track::titleChanged, this, dirty)
                                         << connect(track.get(), &Track::artistChanged, this, dirty)
                                         << connect(track.get(), &Track::keyChanged, this, dirty)
                                         << connect(track.get(), &Track::beatsUpdated, this, dirty);
        }
        m_tracks[deck - 1] = track;
    }
    DeckLabStripState state;
    state.deck = deck;
    state.loaded = track && d.loaded->get() > 0;
    if (track) {
        state.title = track->getTitle();
        state.artist = track->getArtist();
        if (state.title.isEmpty()) state.title = QFileInfo(track->getLocation()).completeBaseName();
    }
    const auto key = KeyUtils::keyFromNumericValue(d.key->get());
    if (state.loaded && key != mixxx::track::io::key::INVALID) {
        state.key = KeyUtils::keyToString(key);
        state.keyColor = m_keyColors[int(key)];
        // Harmonic hint against the tempo master (or another playing deck);
        // none when this deck is the reference itself.
        if (const int reference = referenceDeck(deck)) {
            const auto referenceKey = KeyUtils::keyFromNumericValue(m_deck[reference - 1].key->get());
            if (referenceKey != mixxx::track::io::key::INVALID)
                state.keyMatch = KeyUtils::getCompatibleKeys(referenceKey).contains(key) ? 1 : -1;
        }
    } else {
        state.key = QStringLiteral("—");
    }
    const double range = d.rateRange->get();
    state.range = util_isfinite(range) && range > 0
            ? QStringLiteral("±%1%").arg(QString::number(range * 100, 'g', 3)) : QString();
    state.master = state.loaded && d.leader->get() > 0;
    state.sync = state.loaded && d.sync->get() > 0;
    state.pfl = d.pfl->get() > 0;
    state.slip = state.loaded && d.slip->get() > 0;
    state.keylock = state.loaded && d.keylock->get() > 0;
    state.quantize = state.loaded && d.quantize->get() > 0;
    state.looping = state.loaded && d.loop->get() > 0;
    state.loopSize = beatLoopSizeText(d.loopSize->get());
    fillDeckClock(deck, state);
    m_stripStates[deck - 1] = state;
    applyDeck(deck);
}

void WDeckLab::fillDeckClock(int deck, DeckLabStripState& state) {
    // Values that move while a deck plays: transport, tempo, clock, phase.
    const auto& d = m_deck[deck - 1];
    const double rawPosition = d.position->get();
    const double position = util_isfinite(rawPosition) ? rawPosition : 0;
    m_shownPosition[deck - 1] = position;
    const double samples = d.samples->get();
    state.playing = state.loaded && d.play->get() > 0;
    const double cuePoint = d.cuePoint->get();
    state.cue = state.loaded && !state.playing && cuePoint >= 0 && samples > 0 &&
            std::abs(position * samples - cuePoint) < 2048;
    const double bpm = m_tempos[deck - 1].update(state.loaded, d.localBpm->get(),
            d.rateRatio->get(), d.bpm->get(), m_clock.elapsed());
    state.bpm = state.loaded && bpm > 0 ? QString::number(bpm, 'f', 2) : QStringLiteral("—");
    const double ratio = d.rateRatio->get();
    double pitch = (ratio - 1) * 100;
    if (!util_isfinite(pitch) || std::abs(pitch) < .005) pitch = 0;
    state.tempoCentered = state.loaded && pitch == 0;
    state.tempo = !state.loaded ? QStringLiteral("—")
            : (pitch > 0 ? QStringLiteral("+") : pitch < 0 ? QStringLiteral("−") : QString()) +
                    QString::number(std::abs(pitch), 'f', 2) + QLatin1Char('%');
    // Remaining real time at the current tempo, as the end warning uses.
    const double duration = d.duration->get();
    const double speed = util_isfinite(ratio) && ratio > 0 ? ratio : 1;
    const double remaining = duration * (1 - std::clamp(position, 0.0, 1.0)) / speed;
    state.remaining = state.loaded && duration > 0 ? QStringLiteral("−") + timeText(remaining) : QStringLiteral("—");
    auto* factory = WaveformWidgetFactory::instance();
    const int warning = factory ? factory->getEndOfTrackWarningTime() : m_endWarningSeconds;
    state.endWarning = state.playing && d.loop->get() <= 0 && duration / speed > warning && remaining <= warning;
    state.phase = -1;
    if (state.loaded && m_tracks[deck - 1]) {
        if (const auto beats = m_tracks[deck - 1]->getBeats()) {
            const auto frame = mixxx::audio::FramePos::fromEngineSamplePos(position * samples);
            mixxx::audio::FramePos previous, next;
            if (frame.isValid() && beats->findPrevNextBeats(frame, &previous, &next, false) && next > previous) {
                const long long index = beats->iteratorFrom(previous) - beats->cfirstmarker();
                state.phase = deckLabBeatPhase(index, (frame - previous) / (next - previous)).segment;
            }
        }
    }
}

void WDeckLab::refreshDeckClock(int deck) {
    DeckLabStripState state = m_stripStates[deck - 1];
    fillDeckClock(deck, state);
    if (state == m_stripStates[deck - 1]) return;
    m_stripStates[deck - 1] = state;
    if (m_strips[deck - 1]) m_strips[deck - 1]->setState(state);
    if (m_minis[deck - 1]) m_minis[deck - 1]->setState(state);
}

void WDeckLab::applyDeck(int deck) {
    const auto& state = m_stripStates[deck - 1];
    if (auto* strip = m_strips[deck - 1]) {
        strip->setState(state);
        setAccessibleNameIf(strip, state.loaded ? tr("Deck %1 full track information").arg(deck)
                                                : tr("Deck %1 empty; open Browse").arg(deck));
        setAccessibleDescriptionIf(strip, state.loaded ? tr("Read the full title, artist and source")
                                                       : tr("Choose a track in Browse; preserves the current browser location"));
    }
    if (auto* mini = m_minis[deck - 1]) mini->setState(state);
    const int side = deck % 2 ? 0 : 1;
    if (displayedDeck(side) == deck) refreshTransport(deck, m_sidePrefixes[side]);
    refreshTransport(deck, m_cardPrefixes[deck - 1]);
}

int WDeckLab::referenceDeck(int deck) const {
    // The tempo master when another deck holds it; none when this deck is
    // the master; otherwise another playing deck.
    for (int other = 1; other <= 4; ++other)
        if (other != deck && m_deck[other - 1].leader->get() > 0 && m_deck[other - 1].loaded->get() > 0) return other;
    if (m_deck[deck - 1].leader->get() > 0) return 0;
    for (int other = 1; other <= 4; ++other)
        if (other != deck && m_deck[other - 1].play->get() > 0 && m_deck[other - 1].loaded->get() > 0) return other;
    return 0;
}

void WDeckLab::refreshControllerFx() {
    // Event driven: unit, slot and routing changes mark this dirty. Nothing
    // here runs on the display tick.
    if (!m_fxChips || !m_fxWatchersReady) return;
    m_fxSummaryDirty = false;
    QList<DeckLabChipButton::Chip> chips;
    QStringList details;
    QStringList active;
    for (int unit = 1; unit <= 4; ++unit) {
        const QString group = QStringLiteral("[EffectRack1_EffectUnit%1]").arg(unit);
        auto chain = m_effects ? m_effects->getEffectChain(group) : EffectChainPointer();
        QString name = tr("Empty");
        bool effectOn = false;
        int loadedEffects = 0;
        for (int slot = 0; chain && slot < chain->getEffectSlots().size(); ++slot) {
            const auto effect = chain->getEffectSlots().at(slot);
            if (effect && effect->isLoaded() && effect->getManifest()) {
                const bool enabled = value(QStringLiteral("[EffectRack1_EffectUnit%1_Effect%2]").arg(unit).arg(slot + 1), "enabled") > 0;
                if (loadedEffects == 0 || (enabled && !effectOn)) name = effect->getManifest()->name();
                effectOn |= enabled;
                ++loadedEffects;
            }
        }
        if (loadedEffects > 1) name += tr(" +%1").arg(loadedEffects - 1);
        QStringList targets;
        for (int target = 1; target <= 4; ++target)
            if (value(group, QStringLiteral("group_[Channel%1]_enable").arg(target)) > 0)
                targets << QString::number(target);
        const double rawMix = value(group, "mix");
        const double mix = util_isfinite(rawMix) ? std::clamp(rawMix, 0.0, 1.0) : 0.0;
        const int mixPercent = int(mix * 100);
        const QString route = targets.isEmpty() ? QStringLiteral("—") : targets.join(QLatin1Char('/'));
        // A real controller step below one percent is still wet: never DRY.
        const bool on = effectOn && value(group, "enabled") > 0 && mix > 0 && !targets.isEmpty();
        const QString state = on ? (mixPercent > 0 ? tr("%1%").arg(mixPercent) : tr("<1%")) : tr("DRY");
        chips.append(DeckLabChipButton::Chip{QString::number(unit), kFxLamp, on});
        if (on) active << QString::number(unit);
        details << tr("FX %1; decks %2; %3; %4").arg(unit).arg(route, name, state);
    }
    m_fxChips->setChips(chips);
    setToolTipIf(m_fxChips, details.join(QLatin1Char('\n')));
    setAccessibleDescriptionIf(m_fxChips, active.isEmpty() ? tr("No controller effect unit is wet")
                                                           : tr("Wet controller effect units: %1").arg(active.join(QStringLiteral(", "))));
}

void WDeckLab::connectControllerFx() {
    // Controller effect units may be created after this surface. Connect once
    // they exist; until then the lamps simply stay dark.
    if (m_fxWatchersReady || !m_effects ||
            !m_effects->getEffectChain(QStringLiteral("[EffectRack1_EffectUnit1]"))) return;
    m_fxWatchersReady = true;
    const auto dirty = [this](double) { m_fxSummaryDirty = true; markDeckDirty(0); };
    for (int unit = 1; unit <= 4; ++unit) {
        const QString group = QStringLiteral("[EffectRack1_EffectUnit%1]").arg(unit);
        watch(group, QStringLiteral("enabled"), dirty);
        watch(group, QStringLiteral("mix"), dirty);
        for (int deck = 1; deck <= 4; ++deck) watch(group, QStringLiteral("group_[Channel%1]_enable").arg(deck), dirty);
        for (int slot = 1; slot <= 4; ++slot) {
            const QString effect = QStringLiteral("[EffectRack1_EffectUnit%1_Effect%2]").arg(unit).arg(slot);
            for (const auto& item : {"enabled", "loaded", "loaded_effect"}) watch(effect, QString::fromLatin1(item), dirty);
        }
    }
    m_fxSummaryDirty = true; markDeckDirty(0);
}

void WDeckLab::markDeckDirty(int deck) {
    if (deck >= 1 && deck <= 4) m_deckDirty[deck - 1] = true;
    if (m_flushQueued) return;
    m_flushQueued = true;
    QTimer::singleShot(0, this, &WDeckLab::flushDirtyDecks);
}

void WDeckLab::flushDirtyDecks() {
    m_flushQueued = false;
    for (int deck = 1; deck <= 4; ++deck) {
        if (!m_deckDirty[deck - 1]) continue;
        m_deckDirty[deck - 1] = false;
        refreshDeck(deck);
    }
    if (m_fxSummaryDirty) refreshControllerFx();
}

bool WDeckLab::deckShown(int deck) const {
    if (m_page == 0 && m_fourDeckLayout) return true;
    return deck == m_layers[0] || deck == m_layers[1];
}

void WDeckLab::tickDecks() {
    const qint64 now = m_clock.elapsed();
    for (int deck = 1; deck <= 4; ++deck) {
        // An expired one-shot Sync acknowledgment repaints exactly once.
        if (m_syncFeedbackUntil[deck - 1] && now >= m_syncFeedbackUntil[deck - 1]) {
            m_syncFeedbackUntil[deck - 1] = 0;
            markDeckDirty(deck);
        }
    }
    connectControllerFx();
    // Lab FX raises its tab while it is wet or tailing.
    if (m_tabs[3]) setStylePropertyIf(m_tabs[3], "live", m_fxActive || m_fxReleasing);
    if (m_overloadUntil && now >= m_overloadUntil) {
        m_overloadUntil = 0;
        updateStatus();
    }
    if (!isVisible() || !m_waveforms->isVisible()) return;
    // Only clocks advance on the tick: playing decks, or a paused deck whose
    // position was moved (cue, needle search or scratch).
    const bool flash = (now / 400) % 2 == 0;
    for (int deck = 1; deck <= 4; ++deck) {
        if (!deckShown(deck)) continue;
        const auto& d = m_deck[deck - 1];
        if (d.loaded->get() <= 0) continue;
        const double rawPosition = d.position->get();
        const double position = util_isfinite(rawPosition) ? rawPosition : 0;
        if (d.play->get() > 0 || position != m_shownPosition[deck - 1]) refreshDeckClock(deck);
        if (m_stripStates[deck - 1].endWarning) {
            if (m_strips[deck - 1]) m_strips[deck - 1]->setFlash(flash);
            if (m_minis[deck - 1]) m_minis[deck - 1]->setFlash(flash);
        }
    }
}

void WDeckLab::tickMeters() {
    if (!isVisible()) return;
    if (m_waveforms->isVisible()) {
        for (int deck = 1; deck <= 4; ++deck) {
            auto* meter = m_meters[deck - 1];
            if (meter && meter->isVisible())
                meter->setLevels(m_deck[deck - 1].vu->get(), 0, m_deck[deck - 1].peak->get() > 0);
        }
    }
    if (m_masterMeter && m_masterMeter->isVisible())
        m_masterMeter->setLevels(m_mainVu[0]->get(), m_mainVu[1]->get(), m_mainPeak->get() > 0);
}

void WDeckLab::noteDeckTouched(int deck) {
    // Two-deck view follows a controller's deck layer. Only deliberate
    // touches arrive here; the guards below keep start-up, load-time and
    // mirrored writes from flipping the screen back and forth.
    if (deck < 1 || deck > 4) return;
    const qint64 now = m_clock.elapsed();
    m_lastTouch[deck - 1] = now;
    if (m_fourDeckLayout || now < kFollowStartupGraceMs || now - m_lastLoad[deck - 1] < kFollowLoadGraceMs) return;
    const int side = (deck - 1) % 2;
    const int shown = m_layers[side];
    if (shown == deck) return;
    if (now - m_lastTouch[shown - 1] < kFollowHoldMs) return;
    // A held touch Cue owns its deck; never retarget the side under it.
    if (m_touchCue == channel(shown)) return;
    setDisplayedDeck(side, deck);
    // Keep the layer controls truthful for mappings that read them.
    set(side == 0 ? QStringLiteral("[DeckLabLeft]") : QStringLiteral("[DeckLabRight]"),
            QStringLiteral("current"), deck > 2 ? 1 : 0);
    updateLayers();
}

void WDeckLab::refreshTransport(int deck, const QString& prefix) {
    if (deck < 1 || deck > 4) return;
    const bool card = prefix.startsWith(QStringLiteral("decklab-card-"));
    refreshHotcues(deck, card ? m_cardHotcuePrefixes[deck - 1] : m_sideHotcuePrefixes[deck % 2 ? 0 : 1]);
    const auto found = m_transport.find(prefix);
    if (found == m_transport.end()) return;
    auto& t = found.value();
    const auto& d = m_deck[deck - 1];
    TransportState state;
    state.deck = deck;
    state.loaded = d.loaded->get() > 0;
    state.playing = d.play->get() > 0;
    state.looping = d.loop->get() > 0;
    state.quantize = d.quantize->get() > 0;
    state.keylock = d.keylock->get() > 0;
    state.feedback = m_clock.elapsed() < m_syncFeedbackUntil[deck - 1];
    state.size = d.loopSize->get();
    // Cached buttons and an unchanged state: no lookups and no widget writes.
    if (state == t.shown) return;
    t.shown = state;
    if (t.more)
        setAccessibleNameIf(t.more, (card ? tr("Deck %1 more: hot cues and beatgrid")
                                          : tr("Deck %1 more: transport, track information and beatgrid")).arg(deck));
    for (auto* b : {t.play, t.cue, t.sync, t.halve, t.loop, t.twice, t.quantize, t.keylock}) setEnabledIf(b, state.loaded);
    if (t.play) {
        setCheckedIf(t.play, state.playing); setTextIf(t.play, state.playing ? tr("Pause") : tr("Play"));
        setAccessibleNameIf(t.play, tr("Deck %1 %2").arg(deck).arg(t.play->text()));
    }
    if (t.cue) setAccessibleNameIf(t.cue, tr("Deck %1 %2").arg(deck).arg(t.cue->text()));
    if (t.loop) {
        const QString sizeText = beatLoopSizeText(state.size);
        setCheckedIf(t.loop, state.looping); setTextIf(t.loop, tr("LOOP\n%1").arg(sizeText));
        setAccessibleNameIf(t.loop, tr("Deck %1 loop %2 beats, %3").arg(deck).arg(sizeText, state.looping ? tr("on") : tr("off")));
        setAccessibleDescriptionIf(t.loop, tr("Toggle the selected beat loop; half and double change its beat length"));
    }
    if (t.halve) setAccessibleNameIf(t.halve, tr("Deck %1 halve loop length").arg(deck));
    if (t.twice) setAccessibleNameIf(t.twice, tr("Deck %1 double loop length").arg(deck));
    if (t.quantize) {
        setCheckedIf(t.quantize, state.quantize);
        setAccessibleNameIf(t.quantize, tr("Deck %1 Quantize %2").arg(deck).arg(state.quantize ? tr("on") : tr("off")));
    }
    if (t.keylock) {
        setCheckedIf(t.keylock, state.keylock);
        setAccessibleNameIf(t.keylock, tr("Deck %1 key lock %2").arg(deck).arg(state.keylock ? tr("on") : tr("off")));
    }
    if (t.sync) {
        setStylePropertyIf(t.sync, "actionFeedback", state.feedback);
        setAccessibleNameIf(t.sync, tr("Deck %1 beat sync").arg(deck));
        setAccessibleDescriptionIf(t.sync, tr("Match tempo and beat once; does not enable sync lock"));
    }
}

QString WDeckLab::fxGroup() const { return QStringLiteral("[DeckLabEffectUnit%1]").arg((m_fxDeck - 1) % 2 + 1); }
QString WDeckLab::fxSlot() const { return QStringLiteral("[DeckLabEffectUnit%1_Effect4]").arg((m_fxDeck - 1) % 2 + 1); }
void WDeckLab::refreshFx() {
    auto* target = findChild<QComboBox*>("decklab-fx-target");
    if (target && target->count() == 2 && (value(channel(3), "track_loaded") || value(channel(4), "track_loaded")))
        target->addItems({tr("Deck 3"), tr("Deck 4")});
    auto chain = m_effects ? m_effects->getEffectChain(fxGroup()) : EffectChainPointer();
    const bool ready = chain && chain->getEffectSlot(3) && chain->getEffectSlot(3)->isLoaded();
    const bool loaded = value(channel(m_fxDeck), "track_loaded") > 0;
    bool samplesLoaded=false; for(int slot=1;slot<=4;++slot) samplesLoaded|=value(QStringLiteral("[Sampler%1]").arg(slot),"track_loaded")>0;
    const bool source=loaded||(m_sampleRouting&&samplesLoaded);
    m_fxBody->setEnabled(ready);
    for(int n=0;n<3;++n) {m_levers[n]->setEnabled(source);m_isolates[n]->setEnabled(source);}
    for(int n=0;n<4;++n) if(auto* hold=findChild<QPushButton*>(QStringLiteral("decklab-hold-%1").arg(n))) hold->setEnabled(source);
    for(int n=0;n<4;++n) if(auto* starter=findChild<QPushButton*>(QStringLiteral("decklab-transition-%1").arg(n))) starter->setEnabled(source);
    const QString sampleGroup = QStringLiteral("[Sampler%1]").arg(m_fxSample);
    for (int n = 0; n < 4; ++n) {
        if (auto* roll = findChild<QPushButton*>(QStringLiteral("decklab-roll-%1").arg(n)))
            roll->setEnabled(loaded && value(channel(m_fxDeck), "play"));
        if (auto* roll = findChild<QPushButton*>(QStringLiteral("decklab-sample-roll-%1").arg(n)))
            roll->setEnabled(value(sampleGroup, "play") && value(sampleGroup, "local_bpm") > 0);
    }
    m_fxStatus->setText(!ready ? tr("Lab FX unavailable") : !source ? tr("Load a track / arm sample FX") : m_fxReleasing ? tr("Releasing tail") : m_fxActive ? tr("Lab FX active") : tr("Lab FX ready · Dry"));
    if (m_fxActive && (!ready || !source)) releaseEffects();
}
void WDeckLab::applyEffects() {
    if (m_updating || m_page != 3 || !m_fxBody->isEnabled()) return;
    bool active = false;
    for (int band = 0; band < 3; ++band) active |= m_levers[band]->isChecked() || m_isolates[band]->value() != 0;
    if (!active) { releaseEffects(); return; }
    m_fxReleasing = false; m_fxReturning = false;
    const QString group = fxGroup(), slot = fxSlot();
    // Arm before routing/enabling. A freeze immediately after the first press
    // must not leave an unprotected effect running with the default heartbeat 0.
    control(slot,"parameter15")->setParameter((++m_heartbeat % 65535 + 1) / 65535.0);
    for (int sampler = 1; sampler <= 16; ++sampler) set(group, QStringLiteral("group_[Sampler%1]_enable").arg(sampler), m_sampleRouting && sampler<=4 && value(QStringLiteral("[Sampler%1]").arg(sampler),"track_loaded"));
    for (int deck = 1; deck <= 4; ++deck) set(group, QStringLiteral("group_[Channel%1]_enable").arg(deck), deck == m_fxDeck);
    for (int band = 0; band < 3; ++band) {
        control(slot, QStringLiteral("parameter%1").arg(band + 1))->setParameter(m_levers[band]->isChecked() ? 1 : 0);
        control(slot, QStringLiteral("parameter%1").arg(band + 6))->setParameter((m_isolates[band]->value() / 100.0 + 1) / 2);
    }
    const std::array<int, 3> leverModes{1, 2, 3};
    const std::array<int, 5> isoModes{1, 2, 3, 4, 5};
    control(slot, "parameter4")->setParameter(leverModes[m_leverMode->currentIndex()] / 3.0);
    control(slot, "parameter9")->setParameter(isoModes[m_isoMode->currentIndex()] / 5.0);
    control(slot, "parameter5")->setParameter(m_depth->value() / 100.0);
    control(slot, "parameter10")->setParameter(m_depth->value() / 100.0);
    const double rawBpm = value(channel(m_fxDeck), "bpm");
    double bpm = rawBpm > 0 ? std::clamp(rawBpm, 40.0, 240.0) : 120;
    control(slot, "parameter11")->setParameter((bpm - 40) / 200);
    const std::array<double, 4> times{.125, .25, .5, 1};
    control(slot, "parameter12")->setParameter((times[m_fxTime->currentIndex()] - .0625) / .9375);
    control(slot, "parameter13")->setParameter(0);
    set(slot, "enabled", 1);
    control(group, "mix")->setParameter(1);
    m_fxActive = true;
    m_lastFxTick = m_clock.elapsed();
    refreshFx();
}
void WDeckLab::releaseEffects(bool musical, bool mute) {
    if(!m_heldSamplerRoll.isEmpty())set(m_heldSamplerGroup,m_heldSamplerRoll,0);
    m_heldSamplerRoll.clear();
    if (!m_heldRoll.isEmpty()) set(channel(m_fxDeck), m_heldRoll, 0);
    m_heldRoll.clear();
    if (!m_levers[0]) return;
    m_holdFx = false;
    m_updating = true;
    for (int band = 0; band < 3; ++band) { m_levers[band]->setChecked(false); m_isolates[band]->setValue(0); }
    m_updating = false;
    if (musical && m_fxActive && m_page == 3) {
        for (int band = 0; band < 3; ++band) {
            control(fxSlot(), QStringLiteral("parameter%1").arg(band + 1))->setParameter(0);
            control(fxSlot(), QStringLiteral("parameter%1").arg(band + 6))->setParameter(.5);
        }
        control(fxSlot(), "parameter4")->setParameter(0);
        control(fxSlot(), "parameter9")->setParameter(0);
        control(fxSlot(), "parameter13")->setParameter(mute ? 2.0 / 3 : 1.0 / 3);
        m_fxReleasing = true; m_fxMute = mute; m_fxReturning = false;
        double bpm = value(channel(m_fxDeck), "bpm");
        m_releaseAt = m_clock.elapsed() + qint64(std::clamp(240000.0 / (bpm > 0 ? bpm : 120), 2000.0, 6000.0));
    } else {
        for (int unit = 1; unit <= 2; ++unit) {
            QString group = QStringLiteral("[DeckLabEffectUnit%1]").arg(unit);
            if (!m_effects || !m_effects->getEffectChain(group)) continue;
            set(QStringLiteral("[DeckLabEffectUnit%1_Effect4]").arg(unit), "enabled", 0);
            control(group, "mix")->setParameter(0);
            for (int sampler = 1; sampler <= 16; ++sampler) set(group, QStringLiteral("group_[Sampler%1]_enable").arg(sampler), 0);
            for (int deck = 1; deck <= 4; ++deck) set(group, QStringLiteral("group_[Channel%1]_enable").arg(deck), 0);
        }
        m_fxActive = false; m_fxReleasing = false; m_fxReturning = false;
    }
    refreshFx();
}
void WDeckLab::tickEffects() {
    qint64 now = m_clock.elapsed();
    // The legacy companion heartbeat lapses; decklabd keeps tools online.
    if (m_toolsOnline && now - m_lastToolsReady > 2000 && !(m_systemUi && m_systemUi->toolsConnected())) {
        m_toolsOnline = false; for (auto* b : m_toolButtons) b->setEnabled(false); updateStatus();
    }
    tickDecks();
    if (m_page == 1) updateBreadcrumb();
    if (m_holdFx && now - m_holdStarted >= 4000) { m_holdFx = false; releaseEffects(); }
    if(!m_heldSamplerRoll.isEmpty()&&now-m_holdStarted>=4000){set(m_heldSamplerGroup,m_heldSamplerRoll,0);m_heldSamplerRoll.clear();}
    if (!m_heldRoll.isEmpty() && now - m_holdStarted >= 4000) {
        set(channel(m_fxDeck), m_heldRoll, 0); m_heldRoll.clear();
    }
    if (!m_fxActive) return;
    // Matches the DSP deadman (1.5 s): a GUI stall during a track load must
    // not snap Lab FX dry. Page change and hide still release at once.
    if (m_page != 3 || now - m_lastFxTick > 1500 || !isVisible()) { releaseEffects(); return; }
    m_lastFxTick = now;
    control(fxSlot(), "parameter15")->setParameter((++m_heartbeat % 65535 + 1) / 65535.0);
    // A latched delay follows pitch/Sync changes without needing another touch.
    const double rawBpm = value(channel(m_fxDeck), "bpm");
    const double bpm = rawBpm > 0 ? std::clamp(rawBpm, 40.0, 240.0) : 120;
    control(fxSlot(), "parameter11")->setParameter((bpm - 40) / 200);
    double phase = value(channel(m_fxDeck), "beat_distance");
    control(fxSlot(), "parameter14")->setParameter(std::isfinite(phase) && phase >= 0 && phase <= 1 ? (phase + 1) / 2 : 0);
    if (m_fxReleasing && now >= m_releaseAt) {
        if (m_fxMute && !m_fxReturning) {
            control(fxSlot(), "parameter13")->setParameter(1);
            m_fxReturning = true;
            double bpm = value(channel(m_fxDeck), "bpm");
            m_releaseAt = now + qint64(60000 / (bpm > 0 ? bpm : 120)) + 100;
        } else releaseEffects();
    }
}
void WDeckLab::tool(int id) {
    // decklabd (apps.launch, usb.*, native System pages) first; the legacy
    // companion SysEx bridge remains the fallback while both exist.
    if (m_systemUi && m_systemUi->handleTool(id)) return;
    if (!m_toolsOnline || m_clock.elapsed() - m_lastToolsReady > 2000) return;
    releaseEffects();
    if (id != 1) selectPage(4);
    // The host utility companion observes this explicit, bounded action only.
    // It owns OS integration; the native surface never gets a shell command API.
    set(kNative, "tool_request", id);
    set(kNative, "tool_serial", ++m_toolSerial);
}
void WDeckLab::setKeyNotation(int index) {
    const std::array<KeyUtils::KeyNotation, 3> formats{KeyUtils::KeyNotation::Lancelot, KeyUtils::KeyNotation::OpenKey, KeyUtils::KeyNotation::Traditional};
    const QStringList names{"Lancelot", "OpenKey", "Traditional"};
    QMap<mixxx::track::io::key::ChromaticKey, QString> map;
    for (int key = 1; key <= 24; ++key) {
        auto chromatic = static_cast<mixxx::track::io::key::ChromaticKey>(key);
        map.insert(chromatic, KeyUtils::keyToString(chromatic, formats[index]));
    }
    m_config->setValue(ConfigKey("[Key]", "KeyNotation"), names[index]);
    KeyUtils::setNotation(map);
    set("[Library]", "key_notation", static_cast<double>(formats[index]));
    for (int deck = 1; deck <= 4; ++deck) refreshDeck(deck);
}
void WDeckLab::setAudioOutputConfigured(bool configured) {
    m_outputConfigured = configured;
    if (m_systemUi) m_systemUi->setAudioOutputConfigured(configured);
    updateStatus();
}
void WDeckLab::attachSystem(DeckLabSystem* system) {
    if (m_systemUi) m_systemUi->attach(system);
}
void WDeckLab::updateStatus() {
    int overloads = int(value("[App]", "audio_latency_overload_count"));
    int recording = int(value("[Recording]", "status"));
    const bool audioDown = !m_outputConfigured || value("[SoundManager]", "status") != 2;
    // OVERLOAD lights for ten seconds after the counter rises, so it reports
    // fresh dropouts instead of staying lit for the rest of the session.
    if (overloads > m_lastOverloads) m_overloadUntil = m_clock.elapsed() + 10000;
    m_lastOverloads = overloads;
    const bool recentOverload = m_overloadUntil != 0 && m_clock.elapsed() < m_overloadUntil;
    QString text = recording == 2 ? tr("REC · Recording") : recording == 3 ? tr("Recording paused") : QString();
    if (audioDown) text += tr("   Audio disconnected · Settings → Device");
    if (!m_toolsOnline) text += tr("   System tools offline");
    if (overloads) text += tr("   Audio overloads: %1").arg(overloads);
    const QColor alarm(0xe5, 0x48, 0x4d);
    const QColor caution(0xf5, 0xa6, 0x23);
    QList<DeckLabChipButton::Chip> chips;
    if (recording == 2 || recording == 3)
        chips.append(DeckLabChipButton::Chip{recording == 2 ? tr("REC") : tr("REC II"), alarm, true});
    if (audioDown) chips.append(DeckLabChipButton::Chip{tr("AUDIO OFF"), alarm, true});
    else if (recentOverload) chips.append(DeckLabChipButton::Chip{tr("OVERLOAD"), caution, true});
    if (!m_toolsOnline && chips.isEmpty()) chips.append(DeckLabChipButton::Chip{tr("SYSTEM"), caution, true});
    // Called for every waveform measurement too: unchanged status must not
    // relayout the rail.
    m_status->setChips(chips);
    setToolTipIf(m_status, text.trimmed()); setAccessibleNameIf(m_status, text.trimmed());
    setVisibleIf(m_status, !chips.isEmpty());
    if (m_performanceStatus) setTextIf(m_performanceStatus, tr("Waveforms: %1 FPS · %2 dropped\nAudio overload count: %3")
            .arg(m_measuredFps, 0, 'f', 1).arg(m_droppedFrames).arg(overloads));
}
bool WDeckLab::eventFilter(QObject* watched, QEvent* event) {
    if (watched->objectName().startsWith(QStringLiteral("decklab-info-hit-")) && m_page == 0 &&
            event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            if (!key->isAutoRepeat()) if (auto* header = qobject_cast<QPushButton*>(watched)) header->click();
            return true;
        }
    }
    if (watched == m_sidebar && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Up || key->key() == Qt::Key_Down) {
            set("[Playlist]", "SelectPlaylist", key->key() == Qt::Key_Up ? -key->count() : key->count()); return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter || key->key() == Qt::Key_Right) { openSource(); return true; }
        if (key->key() == Qt::Key_Left || key->key() == Qt::Key_Escape) { browseBack(); return true; }
    }
    if ((event->type() == QEvent::ApplicationDeactivate) ||
            event->type() == QEvent::TouchCancel) { ++m_interactionEpoch; releaseTouchCue(); releaseEffects(); }
    return QWidget::eventFilter(watched, event);
}
void WDeckLab::hideEvent(QHideEvent* event) { ++m_interactionEpoch; releaseTouchCue(); releaseEffects(); QWidget::hideEvent(event); }

void WDeckLab::releaseTouchCue() {
    if (!m_touchCue.isEmpty()) { set(m_touchCue, "cue_default", 0); m_touchCue.clear(); }
}

void WDeckLab::setSearchOpen(bool open) {
    if (!m_keyboard || !m_searchPanel) return;
    if (open && m_library && m_browseStack->currentIndex() == 0) {
        // Searching from a list searches the whole collection.
        resetBrowseLevels();
        m_library->searchTracksInCollection(QString()); showTracks();
    }
    m_keyboard->setVisible(open);
    m_searchPanel->setVisible(open || (m_search && !m_search->currentText().isEmpty()));
    // The keyboard leaves little height: keep Preview and LOAD, drop the card.
    if (m_browseState && m_browseState->card) m_browseState->card->setVisible(!open);
    if (m_page == 1) m_waveforms->setVisible(!open);
    if (open && m_search && m_search->lineEdit()) m_search->lineEdit()->setFocus(Qt::OtherFocusReason);
}

void WDeckLab::openCollection() {
    selectPage(1); setSearchOpen(false);
    resetBrowseLevels();
    if (m_library) m_library->searchTracksInCollection(QString());
    showTracks();
}

int WDeckLab::displayedDeck(int side) const { return m_layers[side]; }
void WDeckLab::setDisplayedDeck(int side, int deck) {
    if (m_layers[side] == deck) return;
    // A held touch or acknowledgment belongs to its actual deck, not the side.
    if (m_touchCue == channel(m_layers[side])) releaseTouchCue();
    ++m_interactionEpoch;
    m_syncFeedbackUntil[m_layers[side] - 1] = 0;
    m_layers[side] = deck;
}
void WDeckLab::updateLayers() {
    const int mode = m_config->getValue(ConfigKey("[DeckLab]", "DeckLayout"), 0);
    bool upper = false; for (int deck = 3; deck <= 4; ++deck) upper |= value(channel(deck), "track_loaded") > 0 || value(channel(deck), "play") > 0;
    // Software layers on a two-channel controller do not imply a four-deck UI.
    // Retain an existing four-deck session across disconnect while upper decks
    // are loaded, rather than concealing its working decks unexpectedly.
    const bool four = mode == 4 || (mode == 0 && (m_autoDeckLayout == 4 || (m_fourDeckLayout && upper)));
    if (four != m_fourDeckLayout) {
        for (auto* menu : findChildren<QMenu*>()) {
            if (menu->objectName().endsWith(QStringLiteral("more-menu"))) {
                menu->close();
            }
        }
        releaseTouchCue();
        ++m_interactionEpoch;
        m_fourDeckLayout = four;
    }
    // Play and track_loaded changes call this. Only a real change in the
    // presentation (page, deck count, waveform arrangement, displayed layers or
    // attached widgets) relayouts; otherwise just refresh the deck state.
    quint64 signature = quint64(four) | quint64(m_sideBySideWaveforms) << 1 |
            quint64(m_page + 1) << 2 | quint64(m_layers[0]) << 5 | quint64(m_layers[1]) << 8;
    for (int index = 0; index < 4; ++index) {
        if (m_deckWaveforms[index]) signature |= quint64(1) << (11 + index);
        if (m_overviews[index]) signature |= quint64(1) << (15 + index);
    }
    // Stems workstream: two-deck view adds a 48 px stem row (taken from the
    // waveforms) while a shown deck plays a stem file. Four-deck view keeps
    // its layout and offers stems in each deck's More menu.
    const bool stemRow = m_page == 0 && !four && DeckLabStemPads::rowWanted(m_stemPads, m_layers);
    signature |= quint64(stemRow) << 19;
    if (signature == m_layoutSignature) {
        for (int deck = 1; deck <= 4; ++deck) markDeckDirty(deck);
        refreshLoadButtons();
        return;
    }
    m_layoutSignature = signature;
    m_fxSummaryDirty = true; markDeckDirty(0);
    m_deckControls->hide();
    const bool decks = m_page == 0;
    const bool separated = decks && m_sideBySideWaveforms;
    // Waveform zone: stacked full-width lanes, or side by side (two columns).
    // Lanes keep their parent; they only move between grid cells.
    for (int index = 0; index < 4; ++index) {
        m_zoneGrid->setRowStretch(index, 0);
        m_zoneGrid->setColumnStretch(index, 0);
        if (m_deckWaveforms[index]) m_zoneGrid->removeWidget(m_deckWaveforms[index]);
    }
    const std::array<int, 4> order = decks && four ? std::array<int, 4>{1, 2, 3, 4}
                                                   : std::array<int, 4>{m_layers[0], m_layers[1], 0, 0};
    const int count = decks && four ? 4 : 2;
    const int columns = separated ? 2 : 1;
    for (int deck = 1; deck <= 4; ++deck) {
        auto* lane = m_deckWaveforms[deck - 1];
        if (!lane) continue;
        int slot = -1;
        for (int index = 0; index < count; ++index) if (order[index] == deck) slot = index;
        setVisibleIf(m_minis[deck - 1], !decks);
        // Off Decks the one-line readout carries the number; the tag keeps
        // only its color bar.
        if (auto* tag = lane->findChild<QLabel*>(QStringLiteral("decklab-lane-edge-%1").arg(deck)))
            setTextIf(tag, decks ? QString::number(deck) : QString());
        if (slot < 0) { setVisibleIf(lane, false); continue; }
        m_zoneGrid->addWidget(lane, slot / columns, slot % columns);
        m_zoneGrid->setRowStretch(slot / columns, 1);
        m_zoneGrid->setColumnStretch(slot % columns, 1);
        setVisibleIf(lane, true);
    }
    // Deck row: two readout cards with their side's cue pads and controls,
    // or four compact cards with direct transport.
    for (auto* card : m_cards) if (card) m_deckGrid->removeWidget(card);
    for (int side = 0; side < 2; ++side) {
        if (m_sideHotcues[side]) m_deckGrid->removeWidget(m_sideHotcues[side]);
        if (m_sideUtilities[side]) m_deckGrid->removeWidget(m_sideUtilities[side]);
    }
    for (int index = 0; index < 4; ++index) m_deckGrid->setColumnStretch(index, 0);
    for (int deck = 1; deck <= 4; ++deck) {
        auto* card = m_cards[deck - 1];
        if (!card) continue;
        const bool shown = decks && (four || deck == m_layers[0] || deck == m_layers[1]);
        if (auto* strip = m_strips[deck - 1]) {
            strip->setMode(four ? DeckLabStripRenderer::Mode::Compact : DeckLabStripRenderer::Mode::Full);
            const int stripHeight = DeckLabStripRenderer::preferredHeight(four
                            ? DeckLabStripRenderer::Mode::Compact
                            : DeckLabStripRenderer::Mode::Full);
            setStyleSheetIf(strip, deckStripSheet(stripHeight));
            setFixedHeightIf(strip, stripHeight);
            setEnabledIf(strip, decks);
            setFocusPolicyIf(strip, decks ? Qt::StrongFocus : Qt::NoFocus);
        }
        setVisibleIf(m_cardTransports[deck - 1], four);
        if (shown) {
            const int column = four ? deck - 1 : (deck == m_layers[0] ? 0 : 1);
            m_deckGrid->addWidget(card, 0, column);
            m_deckGrid->setColumnStretch(column, 1);
        }
        setVisibleIf(card, shown);
    }
    for (int side = 0; side < 2; ++side) {
        // Engine feedback during a held Cue must not hide its open menu's
        // ancestor: these rows only hide when the presentation drops them.
        const bool shown = decks && !four;
        if (shown) {
            if (m_sideHotcues[side]) m_deckGrid->addWidget(m_sideHotcues[side], 1, side);
            if (m_sideUtilities[side]) m_deckGrid->addWidget(m_sideUtilities[side], 2, side);
        }
        setVisibleIf(m_sideHotcues[side], shown);
        setVisibleIf(m_sideUtilities[side], shown);
    }
    // Readout + overview + transport (four decks) or + hotcues + utilities.
    for (auto* pads : m_stemPads) if (pads) pads->setRowShown(stemRow);
    setFixedHeightIf(m_deckRow, (four
                    ? DeckLabStripRenderer::preferredHeight(DeckLabStripRenderer::Mode::Compact) + kOverviewHeight + 48
                    : DeckLabStripRenderer::preferredHeight(DeckLabStripRenderer::Mode::Full) + kOverviewHeight + 48 + 48) +
                    (stemRow ? DeckLabStemPads::kRowHeight : 0));
    setVisibleIf(m_deckRow, decks);
    for (int deck = 1; deck <= 4; ++deck) refreshDeck(deck);
    refreshLoadButtons();
}
