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
#include <QFontDatabase>
#include <QEvent>
#include <QGridLayout>
#include <QHeaderView>
#include <QItemSelectionModel> // r5-fix2-history
#include <QJsonObject> // r5-history
#include <QKeyEvent>
#include <QLineEdit>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QButtonGroup>
#include <QPushButton>
#include <QProgressBar>
#include <QWidgetAction>
#include <QScrollArea>
#include <QScroller>
#include <QSlider>
#include <QSet>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStorageInfo>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTextBrowser>
#include <QSqlQuery>
#include <algorithm>
#include <cmath>
#include <optional>
#include <tuple>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "decklab/decklabrail.h"
#include "decklab/decklabsettings.h"
#include "decklab/decklabdclient.h" // r5-history: is the clock synchronized?
#include "decklab/decklabsystem.h"  // r5-history
#include "decklab/decklabsystempages.h"
#include "decklab/decklabsystemui.h"
#include "decklab/decklabui.h"
#include "effects/effectsmanager.h"
#include "effects/effectchain.h"
#include "effects/effectslot.h"
#include "engine/decklabplaystart.h" // r6-playstart
#include "library/basetracktablemodel.h"
#include "library/decklabknob.h"
#include "library/library.h"
#include "library/sidebarmodel.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/trackmodel.h"
#include "library/playlisttablemodel.h"     // r5-history: the history table LOAD reads
#include "library/trackset/setlogfeature.h" // r5-history: "New set"
#include "mixer/basetrackplayer.h"
#include "mixer/playermanager.h"
#include "mixer/previewdeck.h"
#include "mixer/sampler.h"
#include "preferences/colorpalettesettings.h"
#include "decklab/decklabkeys.h" // r6-keys
#include "decklab/decklabanalysis.h"          // r7-browse: background analysis
#include "decklab/decklabdownloads.h"         // r7-browse: DOWNLOADS and SAMPLES
#include "decklab/decklabmedia.h"             // r7-library
#include "library/decklabfoldertablemodel.h"  // r7-browse
#include "track/keyutils.h"
#include "track/cueinfo.h"
#include "track/track.h"
#include "track/beats.h"
#include "waveform/waveformwidgetfactory.h"
#include "widget/decklabbrowse.h"
#include "widget/decklablabfx.h"
#include "widget/decklablanes.h" // r4-waveforms
#include "widget/decklabsampler.h"         // r4-sampler
#include "widget/decklabsamplercontrols.h" // r4-sampler
#include "widget/decklabpads.h" // r4-decks: pad modes
#include "widget/decklabdecktools.h" // r5-decks: key sheet and GRID mode
#include "widget/decklabsync.h" // r6-sync: SYNC, MASTER and INST DOUBLES
#include "widget/decklabmoreknob.h" // r6-sync: the knob in MORE
#include "widget/decklabcapture.h"  // r7-sampler: TO PAD
#include "engine/sync/decklabcdjsync.h" // r6-sync: the engine's CDJ BEAT SYNC rules
#include "widget/decklabbrowsedock.h"
#include "widget/decklabbrowsehome.h"
#include "widget/decklabbrowselegend.h"
#include "widget/decklabbrowsemix.h"
#include "widget/decklabhistory.h"     // r5-history
#include "widget/decklabhistoryview.h" // r5-history
#include "widget/decklabstempads.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarysidebar.h"
#include "widget/wsearchlineedit.h"
#include "widget/wtracktableview.h"
#include "moc_wdecklab.cpp"

namespace {
const QString kNative = QStringLiteral("[DeckLabNative]");
const QString kTab = QStringLiteral("[Tab]");
// Barlow is the application font while a Deck Lab surface exists.
int s_surfaces = 0;
std::optional<QFont> s_fontBefore;
// Surface sheet. Every child rule names Barlow itself: children with their
// own sheets (System pages, Deck Setup) and painted widgets otherwise fell
// back to the system face. QApplication's font is Barlow too (constructor).
// Settings (#decklab-settings): every control is a 44 px+ touch target
// (32 px min-height + 5 px padding + 1 px borders for buttons, 42 + 2 for
// combo boxes) and toggles that are on print as a white fill with ink text.
// Its category list is a list, not a second tab rail: 56 px rows, a raised
// fill and a white left edge on the selected one. r4-system: rows, switches,
// segments and the category list paint themselves (decklab/decklabutility.h);
// these rules keep any plain control in Settings at the same size.
const QString kStyle = QStringLiteral(R"(
 WDeckLab {background:#0D0F12;color:#F2F4F7;font-family:"Barlow";font-size:16px;font-weight:500;}
 WDeckLab QWidget {color:#F2F4F7;font-family:"Barlow";font-size:16px;font-weight:500;}
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
 WDeckLab QComboBox {background:#20242B;border:1px solid #2C313A;border-radius:2px;min-height:38px;padding-left:10px;}
 WDeckLab QComboBox QAbstractItemView {background:#16191E;color:#F2F4F7;selection-background-color:#20242B;
     selection-color:#F2F4F7;min-height:40px;font-family:"Barlow";font-size:16px;font-weight:500;}
 WDeckLab QComboBox QAbstractItemView::item {min-height:44px;}
 WDeckLab QSlider::groove:horizontal {height:8px;background:#2C313A;border-radius:2px;}
 WDeckLab QSlider::handle:horizontal {background:#F2F4F7;border:1px solid #2C313A;width:24px;margin:-12px 0;border-radius:2px;}
 WDeckLab QProgressBar {border:0;background:#2C313A;}
 WDeckLab QProgressBar::chunk {background:#F2F4F7;}
 WDeckLab QProgressBar[clipping="true"] {border:1px solid #FF3D2E;}
 WDeckLab QLabel[clipping="true"] {color:#F2F4F7;border:1px solid #FF3D2E;}
 WDeckLab QTreeView,WDeckLab QTableView {background:#0D0F12;alternate-background-color:#16191E;border:0;selection-background-color:#20242B;selection-color:#F2F4F7;}
 WDeckLab QTreeView::item {min-height:44px;padding-left:8px;border-bottom:1px solid #2C313A;}
 WDeckLab QHeaderView::section {background:#16191E;color:#9AA3AF;border:0;border-right:1px solid #2C313A;padding:5px;}
 WDeckLab QScrollBar:vertical {width:8px;background:#0D0F12;}
 WDeckLab QScrollBar::handle:vertical {background:#6B7380;min-height:24px;border-radius:2px;}
 WDeckLab QScrollArea, WDeckLab QScrollArea > QWidget, WDeckLab QScrollArea > QWidget > QWidget {background:#0D0F12;border:0;}
 WDeckLab QMenu {background:#16191E;border:1px solid #2C313A;font-family:"Barlow";font-size:16px;font-weight:500;}
 WDeckLab QMenu::item {padding:13px 20px;}
 WDeckLab QMenu::item:selected {background:#20242B;}
 WDeckLab QWidget#decklab-settings QPushButton {min-height:32px;}
 WDeckLab QWidget#decklab-settings QPushButton:checked {background:#F2F4F7;color:#0D0F12;border-color:#F2F4F7;}
 WDeckLab QWidget#decklab-settings QComboBox {min-height:42px;}
 WDeckLab QWidget#decklab-settings QLabel[label="true"] {font-size:14px;font-weight:600;color:#9AA3AF;}
 WDeckLab QWidget#decklab-settings-categories {background:#0D0F12;border-right:1px solid #2C313A;}
 WDeckLab QWidget#decklab-settings {background:#0D0F12;}
 WDeckLab QWidget#decklab-settings-categories QPushButton {background:#0D0F12;color:#9AA3AF;border:0px;
     border-left:3px solid #0D0F12;border-bottom:1px solid #2C313A;border-radius:0px;min-height:55px;max-height:55px;
     padding:0px 13px;text-align:left;font-weight:600;}
 WDeckLab QWidget#decklab-settings-categories QPushButton:pressed {background:#20242B;}
 WDeckLab QWidget#decklab-settings-categories QPushButton:checked {background:#20242B;color:#F2F4F7;
     border-color:#2C313A;border-left:3px solid #F2F4F7;}
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
 QPushButton[rail="true"]:disabled {color:#6B7380;}
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
// One lamp language: on = a #F2F4F7 fill with #0D0F12 text, readable from
// arm's length (LOOP, QNT, KEY LOCK, SYNC lock); Play lights green, as on
// club players. The one-shot SYNC acknowledgment stays an outline so it
// never reads as the lock.
// Text sizes follow the on-device scale: buttons 16 px SemiBold.
const QString kActionStyle = QStringLiteral(R"(
 QPushButton {background:#16191E;color:#C9CED6;border:1px solid #2C313A;border-radius:3px;
     min-height:42px;max-height:42px;padding:0px 2px;font-family:"Barlow";font-size:16px;font-weight:600;}
 QPushButton:pressed {background:#20242B;}
 QPushButton:checked {background:#F2F4F7;border-color:#F2F4F7;color:#0D0F12;}
 QPushButton[lamp="play"]:checked {background:#2FD27A;border-color:#2FD27A;color:#0D0F12;}
 QPushButton[lamp="cue"]:pressed {background:#20242B;border-color:#FF7A1A;}
 QPushButton[actionFeedback="true"] {background:#2C313A;border-color:#F2F4F7;color:#F2F4F7;}
 QPushButton[lit="true"] {background:#F2F4F7;border-color:#F2F4F7;color:#0D0F12;}
 QPushButton:disabled {background:#111418;color:#4B525C;border-color:#1E2228;}
 )");
// Popup menus that hold deck controls (MORE) or pad edits: brand panel and
// line instead of the stock greys.
const QString kMenuStyle = QStringLiteral(R"(
 QMenu {background:#16191E;color:#F2F4F7;border:1px solid #2C313A;padding:4px;
     font-family:"Barlow";font-size:16px;font-weight:600;}
 QMenu::item {padding:13px 20px;background:transparent;}
 QMenu::item:selected {background:#20242B;}
 QMenu::item:disabled {color:#9AA3AF;}
 QMenu::separator {height:1px;background:#2C313A;margin:4px 0px;}
 QSlider::groove:horizontal {height:8px;background:#2C313A;border-radius:2px;}
 QSlider::handle:horizontal {background:#F2F4F7;border:1px solid #9AA3AF;width:24px;margin:-12px 0px;border-radius:3px;}
 )");
// Lab FX and Sampler pages. Set on each page, so it takes precedence over
// the surface sheet for everything inside: Barlow 500/600 on every child
// (style sheet fonts do not pass down by themselves), brand neutrals only,
// and the same filled lamp for on states (bands, sampler pads, toggles).
const QString kPerformStyle = QStringLiteral(R"(
 QWidget {font-family:"Barlow";font-size:16px;font-weight:500;color:#F2F4F7;}
 QLabel[muted="true"] {color:#9AA3AF;font-size:15px;font-weight:500;}
 QLabel[caption="true"] {color:#9AA3AF;font-size:14px;font-weight:600;}
 QPushButton {background:#16191E;color:#F2F4F7;border:1px solid #2C313A;border-radius:3px;
     padding:5px 9px;min-height:28px;font-weight:600;}
 QPushButton:pressed {background:#20242B;}
 QPushButton:checked {background:#F2F4F7;border-color:#F2F4F7;color:#0D0F12;}
 QPushButton:disabled {background:#111418;color:#4B525C;border-color:#1E2228;}
 QPushButton[rail="true"] {background:#16191E;color:#9AA3AF;border:0px;border-right:1px solid #2C313A;
     border-bottom:1px solid #2C313A;border-radius:0px;font-weight:600;}
 QPushButton[rail="true"]:pressed {background:#20242B;}
 QPushButton[rail="true"]:checked {color:#F2F4F7;background:#16191E;border-bottom:3px solid #F2F4F7;}
 QPushButton[rail="true"]:disabled {color:#4B525C;background:#16191E;}
 QComboBox {background:#16191E;color:#F2F4F7;border:1px solid #2C313A;border-radius:3px;
     min-height:38px;padding-left:10px;font-weight:600;}
 QComboBox:disabled {color:#4B525C;border-color:#1E2228;}
 QComboBox QAbstractItemView {background:#16191E;color:#F2F4F7;border:1px solid #2C313A;
     selection-background-color:#20242B;selection-color:#F2F4F7;outline:0px;}
 QComboBox QAbstractItemView::item {min-height:44px;}
 QSlider::groove:horizontal {height:8px;background:#2C313A;border-radius:2px;}
 QSlider::handle:horizontal {background:#F2F4F7;border:1px solid #9AA3AF;width:24px;margin:-12px 0px;border-radius:3px;}
 QSlider::handle:horizontal:disabled {background:#4B525C;border-color:#2C313A;}
 QLabel#decklab-fx-status {border-radius:3px;padding:0px 10px;font-size:14px;font-weight:600;
     min-height:26px;max-height:26px;}
 QLabel#decklab-fx-status[lamp="dry"] {color:#9AA3AF;border:1px solid #2C313A;}
 QLabel#decklab-fx-status[lamp="on"] {color:#0D0F12;background:#F2F4F7;border:1px solid #F2F4F7;}
 QLabel#decklab-fx-status[lamp="tail"] {color:#F2F4F7;border:1px solid #F2F4F7;}
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
 QPushButton:checked {background:#F2F4F7;border-color:#F2F4F7;color:#0D0F12;}
 QPushButton:disabled {color:#4B525C;border-color:#1E2228;}
 )");
const QColor kFxLamp(0xf2, 0xf4, 0xf7);
// r6-sync: a SYNC press held this long is INST DOUBLES (on a stopped deck),
// not BEAT SYNC.
constexpr int kSyncHoldMs = 500;
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

// r4-decks: the hot cue pad look moved to DeckLabPadButton (decklabpads.cpp).

// Sampler pad: the pad number top left and the sample title (up to two
// lines) in Barlow; an empty pad shows a "+". Playing (checked) is the
// filled lamp, so the inks turn dark.
class DeckLabPad final : public QPushButton {
  public:
    using QPushButton::QPushButton;
  protected:
    void paintEvent(QPaintEvent*) override {
        QStyleOptionButton option; initStyleOption(&option); option.text.clear();
        QPainter painter(this); style()->drawControl(QStyle::CE_PushButton, &option, &painter, this);
        const bool lit = isChecked() && isEnabled();
        const QColor number = !isEnabled() ? QColor(0x4b, 0x52, 0x5c) : lit ? QColor(0x0d, 0x0f, 0x12) : QColor(0x9a, 0xa3, 0xaf);
        const QColor title = !isEnabled() ? QColor(0x4b, 0x52, 0x5c) : lit ? QColor(0x0d, 0x0f, 0x12) : QColor(0xf2, 0xf4, 0xf7);
        QFont face;
        face.setFamilies({QStringLiteral("Barlow")});
        face.setPixelSize(16); face.setWeight(QFont::DemiBold);
        painter.setFont(face);
        painter.setPen(number);
        painter.drawText(rect().adjusted(10, 6, -10, -6), Qt::AlignTop | Qt::AlignLeft, text().section(QChar(10), 0, 0));
        const QString name = text().section(QChar(10), 1);
        const QRect body = rect().adjusted(10, 26, -10, -8);
        if (name.isEmpty()) {
            QFont plus = face; plus.setPixelSize(24); plus.setWeight(QFont::Medium);
            painter.setFont(plus);
            painter.drawText(body, Qt::AlignCenter, QStringLiteral("+"));
            return;
        }
        painter.setPen(title);
        // Two lines at most, the second elided.
        const QFontMetrics metrics(face);
        QString first = name, second;
        if (metrics.horizontalAdvance(name) > body.width() && body.height() >= 2 * metrics.height()) {
            int cut = name.size();
            while (cut > 1 && metrics.horizontalAdvance(name.left(cut)) > body.width()) --cut;
            const int space = name.left(cut).lastIndexOf(QLatin1Char(' '));
            if (space > 0) cut = space;
            first = name.left(cut).trimmed();
            second = metrics.elidedText(name.mid(cut).trimmed(), Qt::ElideRight, std::max(0, body.width()));
        } else {
            first = metrics.elidedText(name, Qt::ElideRight, std::max(0, body.width()));
        }
        painter.drawText(body, Qt::AlignCenter, second.isEmpty() ? first : first + QChar(10) + second);
    }
};

// Play/pause with a fixed ▶❚❚ glyph (club players never swap the label);
// painted, since the bundled Barlow has no media glyphs. The lamp (green
// fill while playing) carries the state.
class DeckLabPlayButton final : public QPushButton {
  public:
    using QPushButton::QPushButton;
  protected:
    void paintEvent(QPaintEvent*) override {
        QStyleOptionButton option; initStyleOption(&option); option.text.clear();
        QPainter painter(this); style()->drawControl(QStyle::CE_PushButton, &option, &painter, this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor ink = !isEnabled() ? QColor(0x4b, 0x52, 0x5c) : isChecked() ? QColor(0x0d, 0x0f, 0x12) : QColor(0xf2, 0xf4, 0xf7);
        const double h = 14, triangle = 12, bar = 3.5, gap = 3, space = 5;
        const double total = triangle + space + bar + gap + bar;
        const QPointF origin((width() - total) / 2.0, (height() - h) / 2.0);
        QPainterPath play;
        play.moveTo(origin); play.lineTo(origin.x(), origin.y() + h); play.lineTo(origin.x() + triangle, origin.y() + h / 2); play.closeSubpath();
        painter.fillPath(play, ink);
        const double x = origin.x() + triangle + space;
        painter.fillRect(QRectF(x, origin.y(), bar, h), ink);
        painter.fillRect(QRectF(x + bar + gap, origin.y(), bar, h), ink);
    }
};
}

// r7-browse: what the folder table (DeckLabFolderTableModel) shows.
namespace decklab {
enum class FolderList {
    None,
    Tracks,       // single downloads, newest first
    Playlist,     // one playlist folder, in playlist order
    AllDownloads, // every download, newest first (search in DOWNLOADS)
    Samples,      // ~/Music/Deck Lab Samples, newest first
};
} // namespace decklab

// Browse page state (browse-ui). Lives here so other pages never see it.
struct DeckLabBrowseState {
    enum class List {
        Home,       // first screen: sticks first, then the library's sources
        Sources,    // the native sidebar at some level
        Categories,
        Values,
        Folder,     // a USB stick's folders (files view)
        Mixes,      // r4-browse: tracks that mix with the reference deck
        History,    // r5-history: the sets, then one set's tracks in play order
        Downloads,  // r7-browse: Get Music's downloads: Tracks, then playlist folders
    };
    List list = List::Home;
    List itemsList = List::Sources; // what the category/value list currently holds
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
    // qw-library: source home, USB sticks and read errors.
    QString homeKey;      // Home row to return to and highlight
    bool fromMore = false; // the full source list was opened from "More sources"
    QString stickPath;    // USB stick behind the current source or folder
    QString stickName;
    QString folderPath;   // Folder level: the folder listed
    QString pendingStick; // rekordbox stick waiting for its device row
    qint64 loadingSince = -1;
    int readTimeoutMs = 20000;
    bool loadFailed = false;
    QHash<QString, int> stickTracks; // tracks read per rekordbox stick
    QSet<QString> failedSticks;      // sticks whose export could not be read
    QString homeSignature;
    bool homeStale = false;      // r7 fix: a scan, a capture or a utility changed a tile
    bool downloadsStale = false; // r7 fix: a stick went while the DOWNLOADS list was off screen
    QTimer homeTimer;
    qint64 libraryCountAt = -100000;
    int libraryCount = -1;
    // qw-library: the Match bar.
    decklab::TempoDirection direction = decklab::TempoDirection::Both;
    int pinnedDeck = 0; // 0 = follow the playing deck
    bool matchOnly = false;
    QString matchApplied; // query applied for "Only"
    int matchCount = -1;
    QTimer matchCountTimer;
    QWidget* matchBar = nullptr;
    DeckLabDeckChip* matchDeck = nullptr;
    std::array<DeckLabActionButton*, 3> directions{};
    DeckLabTwoLineLabel* matchSummary = nullptr;
    DeckLabActionButton* matchOnlyButton = nullptr;
    DeckLabActionButton* matchClear = nullptr;
    bool reclaimQueued = false;
    // r4-browse: the page's frame, the deck dock (LOAD targets), the knob
    // legend, the source home's states and "Mixes with deck N".
    QWidget* pane = nullptr;
    QBoxLayout* paneLayout = nullptr;
    QBoxLayout* pageLayout = nullptr;
    DeckLabActionButton* back = nullptr;
    DeckLabDeckDock* dock = nullptr;
    DeckLabKnobLegend* legend = nullptr;
    bool legendQueued = false;
    QPoint pressPos;                      // last touch on the list (a tile's Try again)
    QHash<QString, qint64> readingSticks; // rekordbox sticks being read, since
    qint64 homeBackArmedAt = -100000;     // Back on the home: once more for Decks
    QTimer homeBackTimer;
    QTimer backHold;        // the header Back held: to the source home
    bool backHeld = false;
    QTimer backHoldControl; // a controller's Back held
    decklab::KnobAccelerator accelerator;
    QVector<decklab::MixCandidate> mixCandidates;
    QStringList mixLocations; // per candidate: the reference deck's own track is skipped
    const QAbstractItemModel* mixModel = nullptr;
    int mixModelRows = -1;
    QPersistentModelIndex mixSource;
    decklab::MatchReference mixShown; // the reference the Mixes list was built for
    QTimer mixSelectTimer;
    bool categoryOnMixes = false; // the category list was left from its Mixes row
    bool categoryReturn = false;  // ... or from the category in `category`
    // r5-history: the History view (decklabhistory.h): the sets, or the
    // tracks of one set in play order, and "New set" in the pane.
    int historySession = -1;            // -1: the sets; otherwise the set shown
    int historyKeepSession = -1;        // the set to highlight on the sets list
    bool historyFromCategories = false; // Back: the Library's categories, else the home
    QTimer historySelectTimer;
    decklab::ui::HoldButton* newSet = nullptr;
    decklab::history::Change historyPending; // listener changes, applied once, queued
    bool historyQueued = false;
    bool historyStale = false; // the History list changed while Browse was hidden
    QPointer<DeckLabSystem> historySystem; // decklabd: is the clock synchronized?
    qint64 clockAskedAt = -100000;
    // r5-fix-history: the knob on "New set": a full hold starts the set on
    // the gesture's release press (Shift's Back, a turn or another owner of
    // the knob cancel it); short presses count towards "press twice".
    bool newSetKnobHeld = false;       // browse_hold 1 on the row while Browse owns the knob
    bool newSetKnobDone = false;       // ... held long enough
    bool newSetConfirmOnPress = false; // released after a full hold: the press starts it
    qint64 newSetReleasedAt = -100000;
    QTimer newSetReleaseTimer;
    // r5-fix2-history: track_level follows the hidden table's selection; a
    // History or Mixes row that lost its table while Browse was hidden gets
    // it back on the next visit; the Mixes source is shown again once per swap.
    bool browseRowStale = false;
    bool mixReactivated = false;
    QPersistentModelIndex listClearedRow; // a track table's row a list level unselected
    QPointer<QItemSelectionModel> levelSelection;
    QList<QMetaObject::Connection> levelConnections;
    // r5-fix3-history: the Mixes cache is read again after any refill or
    // reorder of its model (a select(): the source shown again, a rescan).
    bool mixDirty = false;
    QPointer<QAbstractItemModel> mixWatched;
    QList<QMetaObject::Connection> mixModelConnections;
    // r7-browse: the DOWNLOADS and SAMPLES lists. Their tracks are library
    // rows in the folder table; a pad's Load opens on SAMPLES, with ALL
    // MUSIC beside the breadcrumb for the rest of Browse.
    decklab::FolderList folderList = decklab::FolderList::None;
    QString folderTitle;  // the playlist folder shown (its name)
    QString downloadsKey; // the DOWNLOADS row to return to
    DeckLabFolderTableModel* folderModel = nullptr;
    bool samplesForPad = false; // SAMPLES opened by a pad's Load
    bool homeOnReturn = false;  // ... left by the load: Browse comes back on the home
    DeckLabActionButton* allMusic = nullptr;
    // r7-recent: Recently Added. Its count is read with the All Tracks count
    // (from the same rows, no database read). It sorts the library's own
    // track table newest first; what it changed is kept here and given back
    // when the list is left: the column and order, Mixxx's saved sort
    // history and, if the table showed another list meanwhile, its saved
    // header. The library's saved sort never moves.
    int recentCount = -1;
    QPointer<WTrackTableView> recentTable;
    QPointer<QAbstractItemModel> recentModel;
    int recentSection = -1;
    Qt::SortOrder recentOrder = Qt::AscendingOrder;
    QString recentColumns;
    QString recentHeader;
};

WDeckLab::WDeckLab(UserSettingsPointer config, Library* library,
        PlayerManagerInterface* players, EffectsManager* effects, QWidget* parent)
        : QWidget(parent), WBaseWidget(this), m_config(config), m_library(library),
          m_players(players), m_effects(effects) {
    setObjectName(QStringLiteral("DeckLabNative"));
    // Barlow and Barlow Semi Condensed ship with the skin (OFL). Barlow
    // Medium 16 px becomes the application font, so menus, dialogs, combo
    // popups, keyboards and painted widgets never fall back to the system face.
    deckLabLoadFonts(m_config->getResourcePath());
    if (s_surfaces++ == 0) s_fontBefore = QApplication::font();
    // r7 fix: a track that cannot load (a stick pulled, a file gone) is said
    // on the touch screen; Mixxx's modal box would stop the set.
    BaseTrackPlayer::setLoadFailedDialogShown(false);
    if (QFontDatabase::families().contains(QStringLiteral("Barlow"))) {
        QFont face(QStringLiteral("Barlow"));
        face.setFamilies({QStringLiteral("Barlow")});
        face.setPixelSize(16);
        face.setWeight(QFont::Medium);
        QApplication::setFont(face);
    }
    setStyleSheet(kStyle);
    setAttribute(Qt::WA_StyledBackground);
    setMinimumSize(800, 480);
    m_clock.start();
    m_sideBySideWaveforms = m_config->getValue(ConfigKey("[DeckLab]", "WaveformLayout"), 0) == 1;
    // r6-playstart: PLAY starts exactly where the deck is (CDJ) unless the DJ
    // chose otherwise in Settings; live, and kept across a skin rebuild.
    ControlObject::set(decklab::playStartAlignControlKey(),
            static_cast<double>(decklab::nativePlayStartAlign(m_config)));
    // r6-keys: the saved key format for every surface and Mixxx's own tables;
    // a notation chosen in the desktop preferences reaches Deck Lab too.
    {
        const QString saved = m_config->getValueString(ConfigKey("[Key]", "KeyNotation"));
        const auto format = decklab::keyFormatFromConfig(saved);
        // The config says what is shown (a missing value is otherwise read as
        // Traditional by the desktop preferences, whose OK would apply it).
        if (saved != decklab::keyFormatConfigValue(format)) m_config->setValue(ConfigKey("[Key]", "KeyNotation"), decklab::keyFormatConfigValue(format));
        decklab::applyKeyFormat(format);
    }
    watch(QStringLiteral("[Library]"), QStringLiteral("key_notation"), [this](double) {
        // The notation now, not the queued value: two quick changes never
        // swap the format back and forth.
        decklab::KeyFormat format = decklab::keyFormat();
        if (!decklab::keyFormatFromNotation(static_cast<int>(value(QStringLiteral("[Library]"), QStringLiteral("key_notation"))), &format) ||
                format == decklab::keyFormat()) return;
        decklab::applyKeyFormat(format);
        if (auto* row = m_settings ? m_settings->findChild<QComboBox*>(QStringLiteral("decklab-key-format")) : nullptr) {
            row->setCurrentIndex(static_cast<int>(format));
            m_settings->refreshValues();
        }
        refreshKeyText();
    });
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
        m_shownPosition[deck - 1] = -1;
    }
    for (int side = 0; side < 2; ++side) {
        m_sidePrefixes[side] = QStringLiteral("decklab-%1-").arg(side + 1);
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
    // r4-system (Signature 2): the rail is a status bar that teaches. Fixed
    // tab widths, Settings as a gear at the far right (the CDJ's UTILITY),
    // and a fixed status zone in signal-flow order: USB, CTRL, WI-FI, REC,
    // OUT and the main meter. Nothing here ever changes width.
    const QStringList names = {tr("Decks"), tr("Browse"), tr("Sampler"), tr("Lab FX"), tr("Settings")};
    rail->addWidget(new DeckLabMark(QDir(m_config->getResourcePath())
                                            .filePath(QStringLiteral("skins/DeckLab/decklab-icon-24.png")),
            m_rail));
    const QStringList triggers = {"overview", "library", "samplers", "effects", "settings"};
    auto* navigation = new QButtonGroup(this);
    for (int page = 0; page < 5; ++page) {
        auto* tab = new DeckLabRailTab(names[page], page == 4, m_rail, QStringLiteral("decklab-tab-%1").arg(page));
        connect(tab, &QPushButton::clicked, this, [this, page] { selectPage(page); });
        m_tabs[page] = tab;
        m_tabs[page]->setProperty("rail", true);
        navigation->addButton(m_tabs[page], page);
        m_tabs[page]->setFixedHeight(44);
        if (page < 4) rail->addWidget(m_tabs[page]);
        createControl(kTab, triggers[page]);
        watch(kTab, triggers[page], [this, page](double v) { if (v > 0) selectPage(page); });
    }
    // Spare width (wider panels) goes here, never into the tabs.
    rail->addStretch(1);
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
    // r4-system: the status zone (decklab/decklabrail.h). The USB lamp keeps
    // the qw-library call: a tap opens the stick in Browse, with or without
    // the system service; a long press opens the status sheet.
    DeckLabRail::Hooks railHooks;
    railHooks.browseUsb = [this] { openUsbInBrowse(); };
    railHooks.deckSetup = [this](int step) {
        if (m_systemUi && m_systemUi->openWizard(step)) return;
        // No Deck Setup service: Settings › Controller & audio has the rows.
        selectPage(4);
        if (m_settings) m_settings->showCategory(DeckLabSettingsPage::ControllerAudio);
    };
    railHooks.system = [this](int section) {
        selectPage(4);
        if (m_settings) m_settings->showUnit(static_cast<DeckLabSystemPage::Section>(section));
    };
    railHooks.toggleRecording = [this] { set(QStringLiteral("[Recording]"), QStringLiteral("toggle_recording"), 1); };
    // r5-polish: the status sheet's Finish setup line opens the first-run flow.
    railHooks.finishSetup = [this] { if (m_systemUi) m_systemUi->openFirstRun(); };
    m_railUi = new DeckLabRail(this, m_rail, railHooks);
    rail->addWidget(m_railUi->usb());
    m_status = m_railUi->zone();
    rail->addWidget(m_status);
    m_railUi->setNoticeSpan(m_fxChips);
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
    m_tabs[4]->setFixedWidth(44);
    rail->addWidget(m_tabs[4]);
    {
        // Fixed tab widths for the session, from what the status zone needs.
        const int fixed = 48 + m_fxChips->sizeHint().width() + m_railUi->usb()->width() + m_status->width() + 21 + 44;
        const int tab = std::clamp((800 - fixed) / 4, 72, 96);
        for (int page = 0; page < 4; ++page) m_tabs[page]->setFixedWidth(tab);
        m_fxChips->setFixedWidth(m_fxChips->sizeHint().width());
    }
    // Sampler lamp: green while any sample plays (performance Move 5).
    for (int slot = 1; slot <= 16; ++slot) {
        watch(QStringLiteral("[Sampler%1]").arg(slot), QStringLiteral("play"), [this](double) {
            bool playing = false;
            for (int n = 1; n <= 16 && !playing; ++n) playing = value(QStringLiteral("[Sampler%1]").arg(n), "play") > 0;
            static_cast<DeckLabRailTab*>(m_tabs[2])->setLamp(playing ? decklab::ui::Lamp::Go : decklab::ui::Lamp::Off);
        });
    }
    // A held knob press, for scripts that report the release (hold to confirm).
    createControl(kNative, QStringLiteral("browse_hold"));
    watch(kNative, "browse_hold", [](double v) {
        if (QWidget* page = decklab::knobClaimant()) {
            QKeyEvent event(v > 0 ? QEvent::KeyPress : QEvent::KeyRelease, Qt::Key_Select, Qt::NoModifier);
            QApplication::sendEvent(page, &event);
        }
    });
    m_recordTick = new QTimer(this);
    m_recordTick->setInterval(1000);
    connect(m_recordTick, &QTimer::timeout, this, [this] { updateStatus(); });
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
    // r4-waveforms: under compact lanes (Sampler, Lab FX) a 32 px DECKS row
    // takes the readouts' place (widget/decklablanes.h).
    m_laneDecks = new DeckLabLaneDecks(m_waveforms);
    m_waveLayout->addWidget(m_laneDecks, 2, 0);
    m_laneDecks->hide();
    m_waveLayout->setRowStretch(0, 1); m_waveLayout->setRowStretch(1, 0);
    m_pages = new QStackedWidget(this);
    m_pages->setObjectName("decklab-pages");
    m_pages->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    m_pages->addWidget(new QWidget(m_pages));
    // r5-decks: the key sheet and GRID mode (widget/decklabdecktools.h).
    {
        DeckLabDeckTools::Host tools;
        tools.read = [this](int deck, const QString& item) { return value(channel(deck), item); };
        tools.write = [this](int deck, const QString& item, double v) { set(channel(deck), item, v); };
        tools.watch = [this](int deck, const QString& item, std::function<void(double)> changed) { watch(channel(deck), item, std::move(changed)); };
        tools.displayedDeck = [this](int side) { return displayedDeck(side); };
        tools.fourDecks = [this] { return m_fourDeckLayout; };
        tools.onDecks = [this] { return m_page == 0; };
        tools.sideBySide = [this] { return m_sideBySideWaveforms; };
        tools.track = [this](int deck) {
            auto* player = m_players ? m_players->getPlayer(channel(deck)) : nullptr;
            return player ? player->getLoadedTrack() : TrackPointer();
        };
        tools.shownTrack = [this](int deck) { return m_tracks[deck - 1]; };
        tools.refresh = [this](int deck) { markDeckDirty(deck); };
        tools.relayout = [this] { updateLayers(); };
        tools.overview = [this](int deck) { return m_overviews[deck - 1]; };
        tools.cardTransport = [this](int deck) { return m_cardTransports[deck - 1]; };
        tools.stripState = [this](int deck) -> const DeckLabStripState& { return m_stripStates[deck - 1]; };
        tools.epoch = [this] { return m_interactionEpoch; };
        tools.setting = [this](const QString& item) { return m_config->getValueString(ConfigKey(QStringLiteral("[DeckLab]"), item)); };
        tools.setSetting = [this](const QString& item, const QString& value) { m_config->set(ConfigKey(QStringLiteral("[DeckLab]"), item), ConfigValue(value)); };
        m_deckTools = new DeckLabDeckTools(tools, m_deckGrid, this);
    }
    // r6-sync: SYNC as on a CDJ-3000 (widget/decklabsync.h), and the engine's
    // half of it for every controller (engine/sync/decklabcdjsync.h).
    {
        DeckLabSync::Host host;
        host.read = [this](int deck, const QString& item) { return value(channel(deck), item); };
        host.write = [this](int deck, const QString& item, double v) { set(channel(deck), item, v); };
        m_sync = std::make_unique<DeckLabSync>(std::move(host));
        const ConfigKey cdjSync = decklab::cdjsync::controlKey();
        control(cdjSync.group, cdjSync.item)->set(1);
        // A mapping's SYNC hold: [DeckLabNative],inst_doubles = N (then 0) is
        // INST DOUBLES on deck N, with the touch rules (a stopped deck only).
        createControl(kNative, QStringLiteral("inst_doubles"));
        watch(kNative, QStringLiteral("inst_doubles"), [this](double v) {
            if (v >= 1 && v <= 4) m_sync->instantDoubles(static_cast<int>(v));
        });
    }
    // r7-sampler: TO PAD saves a deck's loop as a sample on a pad
    // (widget/decklabcapture.h).
    {
        DeckLabLoopCapture::Host host;
        host.read = [this](int deck, const QString& item) { return value(channel(deck), item); };
        host.track = [this](int deck) {
            auto* player = m_players ? m_players->getPlayer(channel(deck)) : nullptr;
            return player ? player->getLoadedTrack() : TrackPointer();
        };
        host.pads = [this] { return std::max(16, static_cast<int>(value(QStringLiteral("[App]"), QStringLiteral("num_samplers")))); };
        host.pad = [this](int sampler) {
            const QString group = QStringLiteral("[Sampler%1]").arg(sampler);
            DeckLabLoopCapture::Pad pad;
            pad.loaded = value(group, QStringLiteral("track_loaded")) > 0;
            pad.playing = pad.loaded && value(group, QStringLiteral("play")) > 0;
            auto* player = pad.loaded && m_players ? m_players->getPlayer(group) : nullptr;
            if (const TrackPointer track = player ? player->getLoadedTrack() : TrackPointer()) {
                pad.title = track->getTitle().trimmed();
                if (pad.title.isEmpty()) pad.title = QFileInfo(track->getLocation()).completeBaseName();
            }
            return pad;
        };
        // r7: a pad that is loading, or that a Load pad trip is filling, is
        // not empty: the same rule as PUT ON PAD (DeckLabSystem::emptyPad).
        host.emptyPad = [this] {
            return m_system ? m_system->emptyPad()
                            : DeckLabSystem::firstEmptyPad(m_players, DeckLabPadChooser::kPads, m_sampleTarget);
        };
        host.load = [this](int sampler, const decklab::capture::Result& result) {
            const QString group = QStringLiteral("[Sampler%1]").arg(sampler);
            auto* player = m_players ? m_players->getPlayer(group) : nullptr;
            if (!player || value(group, QStringLiteral("play")) > 0) return false;
            // r7: the pad plays it at once from a temporary track with the
            // loop's grid (whole beats over its length), locked so analysis
            // keeps it. No database work now: the deck is usually playing.
            // It joins the library, with that grid, once it is quiet.
            const TrackPointer track = decklab::capture::padTrack(result);
            // A captured loop is a LOOP pad (SYNC'd loop pads follow the master).
            if (m_sampler) m_sampler->setPadMode(sampler, DeckLabSamplerEngine::Mode::Loop);
            player->slotLoadTrack(track,
#ifdef __STEM__
                    mixxx::StemChannelSelection(),
#endif
                    false);
            if (m_captureLibrary) m_captureLibrary->add(result);
            return true;
        };
        host.tell = [this](int deck, const QString& text, bool error) {
            if (!m_browseState || !m_browseState->toast) return;
            // On Decks, just above the deck's pads (two decks) or its card's
            // keys (four), in the band under the readout.
            QRect anchor;
            if (m_page == 0) {
                QList<QWidget*> places;
                for (int side = 0; side < 2; ++side)
                    if (displayedDeck(side) == deck) places << m_sidePads[side];
                places << m_cardTransports[deck - 1];
                for (QWidget* place : std::as_const(places))
                    if (place && place->isVisible()) anchor = QRect(place->mapTo(this, QPoint()), place->size());
            }
            m_browseState->toast->showMessage(text, error ? 3200 : 2400, error, anchor);
        };
        host.sheetParent = [this]() -> QWidget* { return this; };
        m_capture = new DeckLabLoopCapture(std::move(host), this);
        if (m_library && m_library->trackCollectionManager()) {
            DeckLabCaptureLibrary::Hooks adds;
            adds.quiet = [this] { return backgroundSettled(); }; // r7 fix: a quiet spell
            adds.add = [this](const QString& path) {
                return m_library->trackCollectionManager()->getOrAddTrack(TrackRef::fromFilePath(path));
            };
            m_captureLibrary = new DeckLabCaptureLibrary(std::move(adds), this);
        }
        watch(kTab, QStringLiteral("current"), [this](double) { m_capture->cancelChoice(); });
        connect(m_capture, &DeckLabLoopCapture::finished, this, [this] { markHomeStale(); }); // r7 fix
    }
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
    // r4-system: the status zone, its sheet and the main meter are built with
    // the rail above (decklab/decklabrail.h).
    // Scripts write these directly; an open menu or dialog gets them instead.
    watch(kNative, "browse_press", [this](double v) {
        if (v > 0 && decklab::knobForBrowse("[DeckLabNative] browse_press", decklab::KnobAction::Press, 1)) browsePress();
    });
    watch(kNative, "browse_back", [this](double v) {
        // r4-browse (B-48): a Back held for 600 ms goes to the source home.
        if (m_browseState && v <= 0) m_browseState->backHoldControl.stop();
        if (v > 0 && decklab::knobForBrowse("[DeckLabNative] browse_back", decklab::KnobAction::Back, -1)) {
            browseBack();
            if (m_browseState && m_page == 1) m_browseState->backHoldControl.start();
        }
    });
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
        // r7-browse: the queued echo of a page change that was already
        // followed by another one is stale; following it would bounce the
        // two pages back and forth for good.
        if (v != value(kTab, QStringLiteral("current"))) return;
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
    if (m_players) {
        QList<BaseTrackPlayer*> players;
        for (int deck = 1; deck <= 4; ++deck) players << m_players->getPlayer(channel(deck));
        for (int sampler = 0; sampler < m_players->numberOfSamplers(); ++sampler) players << m_players->getSampler(sampler);
        for (int preview = 0; preview < m_players->numberOfPreviewDecks(); ++preview) players << m_players->getPreviewDeck(preview);
        for (auto* player : std::as_const(players)) {
            if (player) connect(player, &BaseTrackPlayer::trackLoadFailed, this, [this](TrackPointer, const QString&) { showLoadFailed(); });
        }
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
    // The last Deck Lab surface hands the application its own font back
    // (another skin, or the next test).
    if (--s_surfaces == 0 && s_fontBefore) {
        QApplication::setFont(*s_fontBefore);
        s_fontBefore.reset();
    }
    if (s_surfaces == 0) {
        BaseTrackPlayer::setLoadFailedDialogShown(true);
    }
    // QWidget closes focus after derived members have been destroyed. Search
    // emits on focus-out, so detach its callback and hide while our controls
    // and page state are still alive (including during a skin replacement).
    if (m_search) disconnect(m_search, nullptr, this, nullptr);
    if (m_sidebar && m_sidebar->selectionModel()) disconnect(m_sidebar->selectionModel(), nullptr, this, nullptr);
    if (m_browseState && m_browseState->items) disconnect(m_browseState->items, nullptr, this, nullptr);
    hide();
    // Overlays deleted with this surface would release their suppression
    // after our members are gone.
    for (const auto& connection : std::as_const(m_waveformSuppressors)) disconnect(connection);
    m_waveformSuppressors.clear();
    releaseTouchCue();
    releaseEffects();
    set(kNative, "active", 0);
    // r6-sync: without the native surface, Mixxx's own sync behaviour.
    const ConfigKey cdjSync = decklab::cdjsync::controlKey();
    control(cdjSync.group, cdjSync.item)->set(0);
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
    if (name.startsWith("decklab-sample-") && !name.startsWith("decklab-sample-roll-"))
        b = new DeckLabPad(text, parent ? parent : this);
    else if (name.startsWith("decklab-") && name.endsWith("-play")) b = new DeckLabPlayButton(text, parent ? parent : this);
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
        // Four-deck Play acts on touch-down, like a player's PLAY; the other
        // actions (toggles, loop sizes, SYNC) act on release.
        const bool onPress = item == QStringLiteral("play") && prefix.startsWith(QStringLiteral("decklab-card-"));
        // r6-sync: a SYNC press held past kSyncHoldMs that loaded INST
        // DOUBLES, or held on a playing deck, must not toggle sync on release.
        auto heldSync = std::make_shared<bool>(false);
        const auto perform = [this, prefix, item](int actual) {
            if (item == "play" || item == "quantize" || item == "keylock" || item == "slip_enabled")
                set(channel(actual), item, !value(channel(actual), item));
            else if (item == "loop_halve" || item == "loop_double" || item == "beatloop_activate")
                pulse(channel(actual), item);
            else if (item == "beatsync")
                m_sync->tap(actual); // r6-sync: BEAT SYNC on or off; the lamp follows sync_enabled
            refreshTransport(actual, prefix);
        };
        auto* b = button(label, [this, target, prefix, pressed, perform, onPress, heldSync] {
            // A click may have toggled a checkable button; never trust the
            // cached presentation after a touch.
            m_transport[prefix].shown = TransportState();
            if (onPress || *heldSync || pressed->second != m_interactionEpoch || pressed->first != target() ||
                    value(channel(target()), "track_loaded") <= 0) {
                // QPushButton toggles before clicked; cancel its visual toggle too.
                refreshTransport(target(), prefix);
                return;
            }
            perform(pressed->first);
        }, strip, prefix + item);
        // State is shown by lit buttons, never by ON/OFF words.
        b->setFixedHeight(44);
        b->setMinimumWidth(44);
        b->setStyleSheet(kActionStyle);
        if (item == "play") b->setProperty("lamp", QStringLiteral("play"));
        else if (item == "beatloop_activate") b->setProperty("lamp", QStringLiteral("loop"));
        else if (item == "cue_default") b->setProperty("lamp", QStringLiteral("cue"));
        connect(b, &QPushButton::pressed, this, [this, target, pressed, heldSync] {
            *pressed = {target(), m_interactionEpoch};
            *heldSync = false;
        });
        if (onPress) {
            connect(b, &QPushButton::pressed, this, [this, prefix, pressed, perform] {
                m_transport[prefix].shown = TransportState();
                if (value(channel(pressed->first), "track_loaded") > 0) perform(pressed->first);
                else refreshTransport(pressed->first, prefix);
            });
        }
        if (item == "beatsync") {
            auto* hold = new QTimer(b);
            hold->setSingleShot(true);
            hold->setInterval(kSyncHoldMs);
            connect(b, &QPushButton::pressed, hold, qOverload<>(&QTimer::start));
            connect(b, &QPushButton::released, hold, &QTimer::stop);
            connect(hold, &QTimer::timeout, this, [this, b, target, prefix, pressed, heldSync] {
                // Same stale-press guards as a tap: same deck, same epoch,
                // and the finger still on the button. r6-sync: the hold is
                // INST DOUBLES, which loads only onto a stopped deck (empty
                // or cued). On a playing deck the hold does nothing, its
                // release included; on a stopped deck with nothing to copy
                // the release is still a tap (DeckLabSync::hold).
                const int actual = pressed->first;
                if (!b->isDown() || pressed->second != m_interactionEpoch || actual != target()) return;
                *heldSync = m_sync->hold(actual);
                m_transport[prefix].shown = TransportState();
                refreshTransport(actual, prefix);
            });
        }
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
        else if (item == "slip_enabled") buttons.slip = b;
        return b;
    };
    // r4-decks: MORE is one sheet of labelled groups (audit PF-D44), the
    // same in both layouts: PLAY or PADS, DECK (QNT, KEY LOCK, SLIP), TRACK
    // (info, grid) and VIEW (zoom). Every key is a 44 px target.
    const auto addGroup = [](QWidget* owner, QBoxLayout* rows, const QString& title,
                                  const QList<QWidget*>& keys) {
        auto* caption = new QLabel(title, owner);
        caption->setObjectName(QStringLiteral("decklab-more-caption"));
        caption->setStyleSheet(QStringLiteral(
                "QLabel{color:#9AA3AF;font-family:\"Barlow\";font-size:14px;font-weight:600;padding:0px 2px;}"));
        caption->setFixedHeight(20);
        rows->addWidget(caption);
        if (keys.isEmpty()) return;
        auto* line = row(nullptr, 4);
        for (auto* key : keys) line->addWidget(key, 1);
        rows->addLayout(line);
    };
    // Linked waveform zoom lives with the other occasional deck tools.
    const auto zoomKeys = [this, prefix](QWidget* owner) {
        QList<QWidget*> keys;
        for (const auto& zoom : {qMakePair(tr("ZOOM −"), QStringLiteral("waveform_zoom_up")),
                     qMakePair(tr("ZOOM +"), QStringLiteral("waveform_zoom_down"))}) {
            auto* b = button(zoom.first, [this, item = zoom.second] {
                for (int side = 0; side < 2; ++side) pulse(channel(displayedDeck(side)), item);
            }, owner, prefix + (zoom.second.endsWith(QStringLiteral("up")) ? QStringLiteral("zoom_out") : QStringLiteral("zoom_in")));
            b->setStyleSheet(kActionStyle); b->setFixedHeight(44); b->setMinimumWidth(86);
            b->setAccessibleName(zoom.second.endsWith(QStringLiteral("up")) ? tr("Zoom all waveforms out") : tr("Zoom all waveforms in"));
            keys << b;
        }
        return keys;
    };
    // Track information from MORE: the readout tap is ignored while a deck
    // plays, so this is the way in mid-mix. Edit grid sits beside it.
    const auto trackKeys = [this, target, prefix, deck, followsLayer](QWidget* owner, QMenu* menu) {
        auto* info = button(tr("Track info"), [this, target, menu] {
            menu->close();
            showTrackInfo(target());
        }, owner, prefix + QStringLiteral("track-info"));
        info->setStyleSheet(kActionStyle); info->setFixedHeight(44);
        // r5-decks: GRID mode on the page (decklabgridmode.h), not a dialog.
        auto* grid = button(tr("GRID"), [this, deck, menu, followsLayer] { menu->close(); showBeatgrid(deck, followsLayer); },
                owner, prefix + QStringLiteral("grid"));
        grid->setStyleSheet(kActionStyle); grid->setFixedHeight(44);
        grid->setAccessibleName(tr("Edit the beat grid on the waveform"));
        // r7-sampler: TO PAD saves exactly the active loop as a sample on
        // the first empty sampler pad (rekordbox's Capture); lit by a loop.
        auto* toPad = button(tr("TO PAD"), [this, target, menu] { menu->close(); m_capture->capture(target()); },
                owner, prefix + QStringLiteral("to-pad"));
        toPad->setStyleSheet(kActionStyle); toPad->setFixedHeight(44);
        toPad->setAccessibleDescription(tr("Saves exactly this loop, up to 32 bars or 60 s, on the first empty sampler pad"));
        m_transport[prefix].capture = toPad;
        return QList<QWidget*>{info, grid, toPad};
    };
    // QNT, KEY LOCK and SLIP: set once and left, so they live in MORE; the
    // readout lights KEY and SLIP, and QNT OFF only when quantize is off.
    // r5-decks: KEY (the key sheet: MATCH, ±1, RESET) beside KEY LOCK.
    const auto deckKeys = [this, &makeAction, target, prefix](QWidget* owner, QMenu* menu) {
        auto* key = button(tr("KEY"), [this, target, menu] { menu->close(); m_deckTools->openKeySheet(target()); },
                owner, prefix + QStringLiteral("key-sheet"));
        key->setStyleSheet(kActionStyle); key->setFixedHeight(44); key->setMinimumWidth(44);
        key->setAccessibleName(tr("Key sheet: match or shift the key"));
        return QList<QWidget*>{makeAction(tr("QNT"), QStringLiteral("quantize"), true),
                makeAction(tr("KEY LOCK"), QStringLiteral("keylock"), true), key,
                makeAction(tr("SLIP"), QStringLiteral("slip_enabled"), true)};
    };
    // r6-sync: MASTER (take the tempo master, or hand it on) and INST DOUBLES
    // (the hold on SYNC, for the knob and for anyone who does not know it).
    const auto syncKeys = [this, target, prefix](QWidget* owner, QMenu* menu) {
        auto* master = button(tr("MASTER"), [this, target, prefix] {
            m_transport[prefix].shown = TransportState();
            m_sync->toggleMaster(target());
            refreshTransport(target(), prefix);
        }, owner, prefix + QStringLiteral("master"));
        auto* doubles = button(tr("INST DOUBLES"), [this, target, menu] {
            menu->close();
            m_sync->instantDoubles(target());
        }, owner, prefix + QStringLiteral("doubles"));
        for (auto* key : {master, doubles}) {
            key->setStyleSheet(kActionStyle); key->setFixedHeight(44); key->setMinimumWidth(44);
        }
        master->setCheckable(true);
        m_transport[prefix].master = master;
        m_transport[prefix].doubles = doubles;
        return QList<QWidget*>{master, doubles};
    };
    if (followsLayer) {
        // r4-decks: SYNC over MORE, the right-hand column of the side's pad
        // block. The loop row became LOOP mode on the pads (any length in
        // one touch; the lit one ends the loop).
        auto* actions = column(strip);
        actions->setSpacing(4);
        actions->addWidget(makeAction(tr("SYNC"), QStringLiteral("beatsync")));
        auto* menu = new QMenu(strip); menu->setObjectName(prefix + QStringLiteral("more-menu"));
        menu->setStyleSheet(kMenuStyle);
        auto* sheet = new QWidget(menu); auto* rows = column(sheet, 4);
        rows->setSpacing(4);
        auto* play = makeAction(tr("PLAY"), QStringLiteral("play"), true);
        auto* cue = makeAction(tr("CUE (hold)"), QStringLiteral("cue_default"));
        addGroup(sheet, rows, tr("PLAY"), {play, cue});
        addGroup(sheet, rows, tr("SYNC"), syncKeys(sheet, menu)); // r6-sync
        addGroup(sheet, rows, tr("DECK"), deckKeys(sheet, menu)); // r5-decks: + KEY
        addGroup(sheet, rows, tr("TRACK"), trackKeys(sheet, menu));
        addGroup(sheet, rows, tr("VIEW"), zoomKeys(sheet));
        sheet->setMinimumWidth(320);
        auto* widgetAction = new QWidgetAction(menu); widgetAction->setDefaultWidget(sheet); menu->addAction(widgetAction);
        new DeckLabMoreKnob(menu, sheet); // r6-sync: MASTER and INST DOUBLES by knob too
        connect(menu, &QMenu::aboutToShow, this, [this, target, prefix] { refreshTransport(target(), prefix); });
        connect(menu, &QMenu::aboutToHide, this, [this, play, cue] {
            releaseTouchCue(); play->setDown(false); cue->setDown(false);
        });
        watch(deck == 1 ? "[DeckLabLeft]" : "[DeckLabRight]", "current", [menu](double) { menu->close(); });
        watch(kNative, "layout_decks", [menu](double) { menu->close(); });
        watch(kTab, "current", [menu](double) { menu->close(); });
        for (int actual : {deck, deck + 2}) watch(channel(actual), "track_loaded", [menu](double) { menu->close(); });
        // The sheet opens above its own column, right-aligned with it.
        auto* more = button(tr("MORE"), [menu, strip] {
            const QSize size = menu->sizeHint();
            menu->popup(strip->mapToGlobal(QPoint(strip->width() - size.width(), -size.height())));
        }, strip, prefix + QStringLiteral("more"));
        more->setFixedHeight(44); more->setMinimumWidth(44); more->setStyleSheet(kActionStyle);
        actions->addWidget(more); m_transport[prefix].more = more;
        strip->setFixedHeight(DeckLabPadBlock::kBlockHeight);
    } else {
        auto* actions = row(strip, 4);
        actions->setContentsMargins(4, 4, 4, 0);
        actions->addWidget(makeAction(tr("CUE"), QStringLiteral("cue_default")), 1);
        actions->addWidget(makeAction(tr("PLAY"), QStringLiteral("play"), true), 1);
        actions->addWidget(makeAction(tr("SYNC"), QStringLiteral("beatsync")), 1);
        auto* menu = new QMenu(strip); menu->setObjectName(prefix + QStringLiteral("more-menu"));
        menu->setStyleSheet(kMenuStyle);
        auto* content = new QWidget(menu); auto* rows = column(content, 4);
        rows->setSpacing(4);
        // r4-decks: the same pad block as the two-deck rows (CUE / LOOP /
        // ROLL / STEM), for this card's deck. Stems are no longer second
        // class in four-deck view.
        addGroup(content, rows, tr("PADS"), {});
        rows->addWidget(buildPads(deck, false, content));
        addGroup(content, rows, tr("SYNC"), syncKeys(content, menu)); // r6-sync
        addGroup(content, rows, tr("DECK"), deckKeys(content, menu)); // r5-decks: + KEY
        addGroup(content, rows, tr("TRACK"), trackKeys(content, menu));
        addGroup(content, rows, tr("VIEW"), zoomKeys(content));
        content->setMinimumWidth(392);
        auto* widgetAction = new QWidgetAction(menu); widgetAction->setDefaultWidget(content); menu->addAction(widgetAction);
        new DeckLabMoreKnob(menu, content); // r6-sync: MASTER and INST DOUBLES by knob too
        connect(menu, &QMenu::aboutToShow, this, [this, deck] { refreshTransport(deck, m_cardPrefixes[deck - 1]); });
        watch(kTab, "current", [menu](double) { menu->close(); });
        watch(kNative, "layout_decks", [menu](double) { menu->close(); });
        watch(channel(deck), "track_loaded", [menu](double) { menu->close(); });
        watch("[DeckLabLeft]", "current", [menu](double) { menu->close(); });
        watch("[DeckLabRight]", "current", [menu](double) { menu->close(); });
        auto* more = button(tr("MORE"), [menu, strip] { menu->popup(strip->mapToGlobal(QPoint(0, strip->height()))); }, strip, prefix + QStringLiteral("more"));
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
    auto* heading = new QLabel(tr("DECK %1 · TRACK").arg(actual), dialog);
    heading->setObjectName(QStringLiteral("decklab-track-info-heading")); heading->setFixedHeight(28);
    heading->setProperty("heading", true);
    content->addWidget(heading);
    auto* text = new QTextBrowser(dialog); text->setReadOnly(true); text->setOpenExternalLinks(false);
    text->setObjectName(QStringLiteral("decklab-track-info-text"));
    text->setPlainText(track ? tr("TITLE\n%1\n\nARTIST\n%2\n\nFILE\n%3")
            .arg(track->getTitle(), track->getArtist(), track->getLocation()) : tr("Deck %1 is empty").arg(actual));
    content->addWidget(text, 1);
    auto* close = button(tr("Close"), [dialog] { dialog->close(); }, dialog, QStringLiteral("decklab-track-info-close"));
    close->setFixedHeight(44); content->addWidget(close);
    dialog->resize(760, 420); dialog->move(mapToGlobal(QPoint((width() - 760) / 2, (height() - 420) / 2)));
    dialog->setWindowModality(Qt::WindowModal); dialog->open();
}

namespace {
// r6-fxstrip: a controller unit's routes other than decks 1-4 (the main mix,
// headphones, samplers, aux, mics, buses) sound too. Named for the rail and
// the details sheet.
bool isDeckRoute(const QString& group) {
    return group.size() == 10 && group.startsWith(QLatin1String("[Channel")) &&
            group[8] >= QLatin1Char('1') && group[8] <= QLatin1Char('4');
}
QStringList offDeckRoutes(const EffectChainPointer& chain) {
    QStringList names;
    if (!chain) return names;
    for (const auto& input : chain->getActiveChannels()) {
        const QString& group = input.name();
        if (isDeckRoute(group)) continue;
        names << (group == QLatin1String("[Master]")    ? WDeckLab::tr("main")
                : group == QLatin1String("[Headphone]") ? WDeckLab::tr("headphones")
                                                        : QString(group).remove(QLatin1Char('[')).remove(QLatin1Char(']')));
    }
    names.sort();
    return names;
}
} // namespace

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
            QString routed = tr("Routed decks: %1").arg(routes.isEmpty() ? tr("none") : routes.join(QStringLiteral(", ")));
            const QStringList others = offDeckRoutes(chain); // r6-fxstrip
            if (!others.isEmpty()) routed += tr(" · also %1").arg(others.join(QStringLiteral(", ")));
            QString detail;
            bool slotOn = false;
            const auto& effectSlots = chain->getEffectSlots();
            for (int slot = 0; slot < effectSlots.size(); ++slot) {
                const auto effect = effectSlots[slot];
                const bool loaded = effect && effect->isLoaded() && effect->getManifest();
                const bool enabled = loaded && value(QStringLiteral("[EffectRack1_EffectUnit%1_Effect%2]").arg(unit).arg(slot + 1), "enabled") > 0;
                slotOn |= enabled;
                const QString state = loaded ? (enabled ? tr("ON") : tr("OFF")) : tr("EMPTY");
                detail += QStringLiteral("<br>") + tr("Slot %1: %2 [%3]").arg(slot + 1)
                        .arg(loaded ? effect->getManifest()->name() : tr("No effect"), state).toHtmlEscaped();
            }
            // r6-fxstrip: the full list keeps all four units; in the 2-deck
            // layout an idle unit 3 or 4 (no rail lamp) says why, on its
            // routing line, so the sheet gains no line.
            const bool wet = slotOn && value(group, "enabled") > 0 && util_isfinite(mix) && mix > 0 &&
                    (!routes.isEmpty() || !others.isEmpty());
            if (!m_fourDeckLayout && unit > 2 && !wet) routed += tr(" · not used in 2-deck layout");
            blocks << block + QStringLiteral("<br>") + routed.toHtmlEscaped() + detail;
        }
        // r6-fxstrip: 4 px cell spacing, so every line fits at 760x420 with
        // no scrolling (8 px left the last line a few pixels short).
        text->setHtml(QStringLiteral("<table width='100%' cellspacing='4'><tr><td width='50%' valign='top'>%1</td><td width='50%' valign='top'>%2</td></tr><tr><td valign='top'>%3</td><td valign='top'>%4</td></tr></table>")
                              .arg(blocks[0], blocks[1], blocks[2], blocks[3]));
    };
    auto* actions = row(nullptr, 8);
    auto* refresh = button(tr("Refresh"), update, dialog, QStringLiteral("decklab-controller-fx-refresh")); refresh->setFixedHeight(44); actions->addWidget(refresh, 1);
    auto* close = button(tr("Close"), [dialog] { dialog->close(); }, dialog, QStringLiteral("decklab-controller-fx-close")); close->setFixedHeight(44); actions->addWidget(close, 1); content->addLayout(actions);
    update(); dialog->resize(760, 420); dialog->move(mapToGlobal(QPoint((width() - 760) / 2, (height() - 420) / 2)));
    dialog->setWindowModality(Qt::WindowModal); dialog->open();
}

namespace {
// r4-decks: a deck's stem pads as the pad block's STEM row.
DeckLabPadBlock::Stems stemHooks(DeckLabStemPads* pads) {
    DeckLabPadBlock::Stems hooks;
    hooks.deck = pads->deck();
    hooks.pads = pads;
    hooks.present = [pads] { return pads->present(); };
    hooks.anyMuted = [pads] { return pads->anyMuted(); };
    hooks.restoreAll = [pads] { pads->restoreAll(); };
    hooks.setSoloLatch = [pads](bool on) { pads->setSoloLatch(on); };
    hooks.setShown = [pads](bool on) { pads->setRowShown(on); };
    return hooks;
}
} // namespace

// r4-decks (audit move 2): one 2x4 pad block with CUE / LOOP / ROLL / STEM
// modes (decklabpads.h). It replaces the hot cue row and the 48 px stem
// row. Side blocks follow the layer (decks 1/3 left, 2/4 right) and keep the
// round-3 pad names (decklab-hotcue-N-1..8); a card's block (four-deck MORE)
// is its own deck. Touch-down firing and the stale-touch guards are the
// block's; the surface supplies the context and the control writes.
DeckLabPadBlock* WDeckLab::buildPads(int deck, bool followsLayer, QWidget* parent) {
    const QString prefix = followsLayer ? m_sidePrefixes[deck - 1] : m_cardPrefixes[deck - 1];
    DeckLabPadBlock::Host host;
    host.deck = [this, deck, followsLayer] { return followsLayer ? displayedDeck(deck - 1) : deck; };
    host.epoch = [this] { return m_interactionEpoch; };
    host.current = [this](int actual) {
        if (actual < 1 || actual > 4) return false;
        auto* player = m_players ? m_players->getPlayer(channel(actual)) : nullptr;
        return m_deck[actual - 1].loaded->get() > 0 && player && player->getLoadedTrack() == m_tracks[actual - 1];
    };
    host.read = [this](int actual, const QString& item) { return value(channel(actual), item); };
    host.write = [this](int actual, const QString& item, double v) { set(channel(actual), item, v); };
    host.acted = [this, prefix](int actual) {
        if (actual < 1 || actual > 4) return;
        m_transport[prefix].shown = TransportState();
        refreshTransport(actual, prefix);
    };
    auto* block = new DeckLabPadBlock(
            followsLayer ? QStringLiteral("decklab-pads-%1").arg(deck) : QStringLiteral("decklab-card-pads-%1").arg(deck),
            followsLayer ? QStringLiteral("decklab-hotcue-%1-").arg(deck) : QStringLiteral("decklab-card-hotcue-%1-").arg(deck),
            host, parent);
    const QList<int> decks = followsLayer ? QList<int>{deck, deck + 2} : QList<int>{deck};
    for (int actual : decks) {
        auto* stems = new DeckLabStemPads(actual,
                followsLayer ? DeckLabStemPads::Presentation::Row : DeckLabStemPads::Presentation::Menu,
                m_players, [this] { return m_interactionEpoch; }, block);
        if (followsLayer) m_stemPads[actual - 1] = stems;
        block->addStems(stemHooks(stems));
        connect(stems, &DeckLabStemPads::presenceChanged, block, [block](bool) { block->stemsChanged(); });
        connect(stems, &DeckLabStemPads::mutesChanged, block, &DeckLabPadBlock::stemsChanged);
    }
    if (followsLayer) m_sidePads[deck - 1] = block;
    else m_cardPads[deck - 1] = block;
    // One mode choice per deck, whichever layout shows it.
    connect(block, &DeckLabPadBlock::modeChanged, this, [this](int actual, DeckLabPadBlock::Mode mode) {
        for (auto* other : m_sidePads) if (other) other->setChosenMode(actual, mode);
        for (auto* other : m_cardPads) if (other) other->setChosenMode(actual, mode);
    });
    return block;
}

// r4-decks: pushes one deck's pad state to its block. Change-driven (deck
// refresh, MORE opening, a touch); an unchanged state costs one comparison.
void WDeckLab::refreshPads(int deck, bool card) {
    if (deck < 1 || deck > 4) return;
    auto* block = card ? m_cardPads[deck - 1] : m_sidePads[deck % 2 ? 0 : 1];
    if (!block) return;
    const auto& d = m_deck[deck - 1];
    DeckLabPadDeckState state;
    state.deck = deck;
    state.loaded = d.loaded->get() > 0;
    state.playing = d.play->get() > 0;
    for (int cue = 0; cue < 8; ++cue) {
        const bool set = d.cuePosition[cue]->get() >= 0;
        const bool loop = set && int(d.cueType[cue]->get()) == int(mixxx::CueType::Loop);
        const int color = int(d.cueColor[cue]->get());
        state.cues[cue] = !set ? -1 : (qint64(loop) << 32) | (color >= 0 && color <= 0xffffff ? color : 0x555555);
    }
    state.looping = state.loaded && d.loop->get() > 0;
    state.loopSize = d.loopSize->get();
    block->setDeckState(state);
}

// r5-decks (audit move 6): the grid is edited on the page, beside the
// waveform being aligned (GRID mode, widget/decklabgridmode.h), instead of in
// the old decklab-beatgrid-editor dialog that covered it.
void WDeckLab::showBeatgrid(int deck, bool followsLayer) {
    m_deckTools->enterGrid(followsLayer ? displayedDeck(deck - 1) : deck);
}

QWidget* WDeckLab::buildDecks() {
    auto* page = new QWidget(this);
    // Physical-side rows live in the deck row for good; a layer change only
    // retargets them, it never reparents or relayouts them. r4-decks: one
    // pad block per side, with SYNC and MORE as its right-hand column, in
    // the 96 px the hot cue and transport rows took (waveforms keep their
    // height, and a stem track no longer takes a row from them).
    for (int deck = 1; deck <= 2; ++deck) {
        auto* pads = buildPads(deck, true, m_deckRow);
        pads->setOuterMargins(4, 4, 4, 0);
        m_sideUtilities[deck - 1] = buildTransport(deck, true, pads);
        pads->setSideColumn(m_sideUtilities[deck - 1]);
        m_deckTools->attachSide(deck - 1, pads); // r5-decks: the key sheet docks in place of the pads
        pads->hide();
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
    // One glyph per concept, never reused for another source.
    const QString icon = rootIconName(index);
    const bool root = !index.parent().isValid();
    if (icon == QStringLiteral("tracks")) return root ? decklab::Glyph::Collection : decklab::Glyph::Playlist;
    if (icon == QStringLiteral("crates")) return decklab::Glyph::Crate;
    if (icon == QStringLiteral("autodj")) return decklab::Glyph::AutoDj;
    if (icon == QStringLiteral("prepare")) return decklab::Glyph::Analyze;
    if (icon == QStringLiteral("history")) return decklab::Glyph::History;
    if (icon == QStringLiteral("recordings")) return decklab::Glyph::Mic;
    if (icon == QStringLiteral("computer")) return decklab::Glyph::Folder;
    if (icon == QStringLiteral("rekordbox") && root) return decklab::Glyph::Usb;
    if (isRekordboxDevice(index)) return decklab::Glyph::Usb;
    if (index.model() && index.model()->hasChildren(index)) return decklab::Glyph::Folder;
    return decklab::Glyph::Playlist;
}
// Desktop roots the appliance keeps under "More sources" (B-05). Sticks are
// listed one by one instead of the Rekordbox folder.
bool hiddenFromHome(const QString& icon) {
    return icon == QStringLiteral("autodj") || icon == QStringLiteral("prepare") ||
            icon == QStringLiteral("computer") || icon == QStringLiteral("rekordbox");
}
QString homeRootKey(const QModelIndex& root) {
    const QString icon = root.data(SidebarModel::IconNameRole).toString();
    return icon.isEmpty() ? QString(QStringLiteral("root::") + root.data().toString())
                          : QString(QStringLiteral("root:") + icon);
}
QString rekordboxDevicePath(const QModelIndex& device) {
    return device.data(SidebarModel::DataRole).toStringList().value(0);
}
// The rekordbox feature's row for the stick mounted at path, once it scanned.
QModelIndex rekordboxDeviceFor(const QAbstractItemModel* model, const QString& path) {
    const QModelIndex root = sidebarRoot(model, QStringLiteral("rekordbox"));
    if (!root.isValid()) return {};
    const QString wanted = QDir::cleanPath(path);
    for (int row = 0; row < model->rowCount(root); ++row) {
        const QModelIndex device = model->index(row, 0, root);
        if (QDir::cleanPath(rekordboxDevicePath(device)) == wanted) return device;
    }
    return {};
}
// many holds %1 for the (grouped) count.
QString countText(int count, const QString& one, const QString& many) {
    return count == 1 ? one : QString(many).arg(QLocale().toString(count));
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
    case decklab::Category::RecentlyAdded: return decklab::Glyph::Recent; // r7-recent
    }
    return decklab::Glyph::None;
}
// r7-recent: the model settings Mixxx keeps a table's sort in
// (BaseSqlTableModel, WTrackTableViewHeader).
const QString kColumnsSorting = QStringLiteral("ColumnsSorting");
const QString kHeaderState = QStringLiteral("header_state_pb");
// Sorts a track table's model without a select when it can (the search that
// follows reads it once) and moves the header's arrow without telling the
// table, so neither Mixxx's sort controls nor a second select follow.
void sortTrackTable(WTrackTableView* table, QAbstractItemModel* model, int column, Qt::SortOrder order, bool select) {
    if (!model || column < 0) return;
    auto* sql = qobject_cast<BaseSqlTableModel*>(model);
    if (sql && !select) sql->setSort(column, order);
    else model->sort(column, order);
    if (table && table->model() == model) {
        const QSignalBlocker blocker(table->horizontalHeader());
        table->horizontalHeader()->setSortIndicator(column, order);
    }
}
// Newest first on the table's model, keeping what it was (once per visit).
void sortNewestFirst(DeckLabBrowseState& b, WTrackTableView* table) {
    auto* model = table ? table->model() : nullptr;
    auto* tracks = dynamic_cast<TrackModel*>(model);
    if (!tracks) return;
    const int column = tracks->columnIndexFromSortColumnId(TrackModel::SortColumnId::DateTimeAdded);
    if (column < 0) return;
    if (!b.recentModel || b.recentModel != model) {
        b.recentTable = table;
        b.recentModel = model;
        b.recentSection = table->horizontalHeader()->sortIndicatorSection();
        b.recentOrder = table->horizontalHeader()->sortIndicatorOrder();
        b.recentColumns = tracks->getModelSetting(kColumnsSorting);
        b.recentHeader = tracks->getModelSetting(kHeaderState);
    }
    sortTrackTable(table, model, column, Qt::DescendingOrder, false);
}
// The sort Recently Added found, back on its model; select reads it at once
// (otherwise the search that follows does).
void restoreRecentSort(DeckLabBrowseState& b, bool select) {
    const QPointer<QAbstractItemModel> model = std::exchange(b.recentModel, nullptr);
    const QPointer<WTrackTableView> table = std::exchange(b.recentTable, nullptr);
    auto* tracks = dynamic_cast<TrackModel*>(model.data());
    if (!tracks) return;
    const bool shown = table && table->model() == model;
    int column = b.recentSection;
    Qt::SortOrder order = b.recentOrder;
    if (column < 0 || tracks->sortColumnIdFromColumnIndex(column) == TrackModel::SortColumnId::Invalid) {
        column = tracks->defaultSortColumn();
        order = tracks->defaultSortOrder();
    }
    sortTrackTable(shown ? table.data() : nullptr, model, column, order, select && shown);
    tracks->setModelSetting(kColumnsSorting, b.recentColumns);
    // The table saved its header (the newest-first arrow) when it moved on.
    if (!shown && !b.recentHeader.isEmpty()) tracks->setModelSetting(kHeaderState, b.recentHeader);
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
    // r4-browse: the list and the info pane on top, then the knob legend and
    // the deck dock (the LOAD targets) along the bottom.
    b.pageLayout = column(m_browser);
    auto* body = new QWidget(m_browser);
    body->setObjectName(QStringLiteral("decklab-browse-body"));
    b.pageLayout->addWidget(body, 1);
    auto* outer = row(body);
    auto* main = new QWidget(body);
    auto* layout = column(main);
    outer->addWidget(main, 1);

    // Header: back, where you are, and view tools. LOAD lives in the pane.
    auto* heading = row();
    auto* back = browseButton(m_browser, QString(), QStringLiteral("decklab-browse-back"), tr("Back"),
            decklab::Glyph::Back, 52);
    b.back = back;
    // r4-browse (B-48): a tap goes up one level; held 600 ms, to the source home.
    connect(back, &QPushButton::pressed, this, [this] {
        m_browseState->backHeld = false;
        m_browseState->backHold.start();
    });
    connect(back, &QPushButton::released, this, [this] { m_browseState->backHold.stop(); });
    connect(back, &QPushButton::clicked, this, [this] {
        if (!m_browseState->backHeld) browseBack();
        m_browseState->backHeld = false;
    });
    heading->addWidget(back);
    b.breadcrumb = new DeckLabBreadcrumb(m_browser);
    b.breadcrumb->setObjectName(QStringLiteral("decklab-breadcrumb"));
    b.breadcrumb->setSegments({tr("Sources")});
    heading->addWidget(b.breadcrumb, 1);
    // r7-browse: a pad's Load opens on SAMPLES; this is the way to the rest.
    b.allMusic = browseButton(m_browser, tr("All music"), QStringLiteral("decklab-browse-all-music"),
            tr("Load the pad from the library, a USB stick or a playlist"), decklab::Glyph::None, 128);
    b.allMusic->setDetail(tr("Other sources"));
    connect(b.allMusic, &QPushButton::clicked, this, [this] { showHome(QStringLiteral("root:tracks")); });
    b.allMusic->hide();
    heading->addWidget(b.allMusic);
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
        menu->setObjectName(QStringLiteral("decklab-browse-tools-menu"));
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->setStyleSheet(QStringLiteral("QMenu{font-size:16px;font-weight:500;}"));
        menu->addAction(tr("All tracks"), this, &WDeckLab::openCollection);
        menu->addAction(tr("Preview selected track"), this, [this] { togglePreview(); });
        menu->addAction(tr("Stop preview"), this, [this] { stopPreview(); });
        // Match Deck filters what you are looking at (B-42): the current list,
        // or the source you are inside; it never switches to another source.
        const int matchDeck = matchReferenceDeck();
        auto* match = menu->addAction(matchDeck ? tr("Match Deck %1").arg(matchDeck) : tr("Match Deck"),
                this, [this] { showOnlyMatches(); });
        match->setObjectName(QStringLiteral("decklab-tools-match"));
        const auto& state = *m_browseState;
        const bool hereIsAList = m_browseStack->currentIndex() != 0 || state.list == DeckLabList::Folder ||
                (state.source.isValid() && (state.list == DeckLabList::Categories || state.list == DeckLabList::Values ||
                                                   state.list == DeckLabList::Mixes));
        match->setEnabled(matchDeck > 0 && hereIsAList &&
                (value(channel(matchDeck), "bpm") > 0 || value(channel(matchDeck), "visual_key") > 0));
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
            // r6-fxstrip: the 2-deck layout lists decks 3 and 4 only when loaded.
            if (!m_fourDeckLayout && deck > 2 && value(channel(deck), "track_loaded") <= 0) continue;
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
                if (!browseLoadableNow()) return; // r5-fix2-history: only Browse's highlighted track
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
    layout->addWidget(buildMatchBar(main));
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
    // r4-browse: also paints the home's tiles and chips and the Mixes rows.
    b.items->setItemDelegate(new DeckLabBrowseDelegate(b.items));
    // Source rows are taller (two lines) than category rows.
    b.items->setUniformItemSizes(false);
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
        auto& state = *m_browseState;
        if (state.list == DeckLabList::History && !(index.flags() & Qt::ItemIsEnabled)) return; // r5-history: headers are labels
        state.items->setCurrentRow(index.row());
        // r4-browse: "Try again" inside the tile of a stick that could not be read.
        if (state.list == DeckLabList::Home &&
                static_cast<decklab::SourceState>(index.data(DeckLabListDelegate::StateRole).toInt()) ==
                        decklab::SourceState::Failed &&
                DeckLabBrowseDelegate::retryRect(state.items->visualRect(index)).contains(state.pressPos)) {
            retryStickFromHome(index.data(DeckLabListDelegate::KeyRole).toString().mid(4));
            return;
        }
        // r4-browse: a tap on a Mixes track highlights it; LOAD is on the dock.
        if (state.list == DeckLabList::Mixes) return;
        if (state.list == DeckLabList::History && historyTrackLevel()) return; // r5-history: same for history tracks
        browseEnter();
    });
    connect(b.items, &QListWidget::currentRowChanged, this, [this](int) {
        refreshBrowseCard();
        // r4-browse: a Mixes row is the highlighted track.
        if (m_browseState->list == DeckLabList::Mixes) syncMixSelection();
        if (m_browseState->list == DeckLabList::History) syncHistorySelection(); // r5-history
    });
    new DeckLabEventHook(b.items->viewport(), [this](QObject*, QEvent* event) {
        if (event->type() == QEvent::MouseButtonPress) m_browseState->pressPos = static_cast<QMouseEvent*>(event)->pos();
        // round4: the home's tile height follows the viewport height, but the
        // flow layout re-flows only when the width changes. Without this a
        // taller list left the tiles of one row overlapping the next.
        if (event->type() == QEvent::Resize && m_browseState->items->flow() == QListView::LeftToRight) {
            const auto* resize = static_cast<QResizeEvent*>(event);
            if (resize->oldSize().height() != resize->size().height()) m_browseState->items->doItemsLayout();
        }
        return false;
    });
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
    // r4-browse (move 3): the deck dock is the LOAD target. It replaces the
    // mini strips and these LOAD buttons; its tiles keep their names, text
    // and stale-tap guards.
    b.pane = pane;
    b.paneLayout = paneLayout;
    b.dock = new DeckLabDeckDock(m_browser);
    b.dock->setObjectName(QStringLiteral("decklab-deck-dock"));
    for (int deck = 1; deck <= 4; ++deck) {
        auto pressed = std::make_shared<std::tuple<int, quint64, int>>(0, 0, 0);
        auto selection = std::make_shared<QPersistentModelIndex>();
        auto* load = b.dock->tile(deck - 1);
        load->setObjectName(QStringLiteral("decklab-load-%1").arg(deck));
        load->setAccessibleName(tr("Load deck %1").arg(deck));
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
    }
    outer->addWidget(pane);
    // r4-browse (move 4): the knob legend, then the dock.
    b.legend = new DeckLabKnobLegend(m_browser);
    b.legend->setObjectName(QStringLiteral("decklab-knob-legend"));
    b.pageLayout->addWidget(b.legend);
    b.pageLayout->addWidget(b.dock);
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
    // A stick plugged in or pulled while the source list is shown.
    b.homeTimer.setInterval(1500);
    connect(&b.homeTimer, &QTimer::timeout, this, [this] { refreshHomeIfSticksChanged(); });
    b.matchCountTimer.setSingleShot(true);
    b.matchCountTimer.setInterval(150);
    connect(&b.matchCountTimer, &QTimer::timeout, this, [this] { refreshMatchCount(); });

    // Generic controller navigation, routed by LibraryControl (which checks
    // knob ownership). Scripts may write these directly, so check here too.
    // r5-fix2-history: a turn runs where LibraryControl sets it (the GUI
    // thread, at once), so it always lands before a press or LOAD the
    // controller sent after it; a script's own write from its thread queues.
    auto* moveTurn = new ControlProxy(kNative, QStringLiteral("browse_move"), this, ControlFlag::NoWarnIfMissing);
    moveTurn->connectValueChanged(this, [this](double v) {
        if (v != 0 && std::isfinite(v) &&
                decklab::knobForBrowse("[DeckLabNative] browse_move", decklab::KnobAction::Move, v)) {
            browseMove(int(std::clamp(v, -10000.0, 10000.0)), false);
        }
    }, Qt::AutoConnection);
    auto* selectTurn = new ControlProxy(kNative, QStringLiteral("browse_select"), this, ControlFlag::NoWarnIfMissing);
    selectTurn->connectValueChanged(this, [this](double v) {
        if (v != 0 && std::isfinite(v) &&
                decklab::knobForBrowse("[DeckLabNative] browse_select", decklab::KnobAction::Move, v)) {
            browseMove(int(std::clamp(v, -10000.0, 10000.0)), true);
        }
    }, Qt::AutoConnection);
    watch(kNative, "browse_enter", [this](double v) {
        if (v > 0 && decklab::knobForBrowse("[DeckLabNative] browse_enter", decklab::KnobAction::Enter, 1)) browseEnter();
    });
    watch(kNative, "load_refused", [this](double v) { if (v > 0) showLoadRefused(int(v)); });
    // Registered before the constructor's own browse_sources handler, which
    // then resets the sidebar root.
    watch(kNative, "browse_sources", [this](double v) { if (v > 0) resetBrowseLevels(); });
    watch(QStringLiteral("[PreviewDeck1]"), QStringLiteral("play"), [this](double) { refreshPreviewButton(); });
    watch(kTab, QStringLiteral("current"), [this](double v) { if (int(v) != 1) stopPreview(); });
    for (int deck = 1; deck <= 4; ++deck) {
        for (const auto& key : {"bpm", "visual_key", "sync_leader", "track_loaded", "play"}) {
            watch(channel(deck), QString::fromLatin1(key), [this](double) { scheduleMatchRefresh(); });
        }
    }
    // r4-browse: hold Back (touch or controller) for the source home; Back
    // twice on the home for Decks; a Mixes row's track once the knob settles.
    b.backHold.setSingleShot(true);
    b.backHold.setInterval(600);
    connect(&b.backHold, &QTimer::timeout, this, [this] {
        m_browseState->backHeld = true;
        if (m_page == 1) { setSearchOpen(false); showHome(QString()); }
    });
    b.backHoldControl.setSingleShot(true);
    b.backHoldControl.setInterval(600);
    connect(&b.backHoldControl, &QTimer::timeout, this, [this] {
        if (m_page == 1 && !(m_browseStack->currentIndex() == 0 && m_browseState->list == DeckLabList::Home)) {
            setSearchOpen(false);
            showHome(QString());
        }
    });
    b.homeBackTimer.setSingleShot(true);
    b.homeBackTimer.setInterval(2000);
    connect(&b.homeBackTimer, &QTimer::timeout, this, [this] {
        m_browseState->homeBackArmedAt = -100000;
        updateKnobLegend();
    });
    b.mixSelectTimer.setSingleShot(true);
    b.mixSelectTimer.setInterval(120);
    connect(&b.mixSelectTimer, &QTimer::timeout, this, [this] {
        auto& state = *m_browseState;
        if (state.list != DeckLabList::Mixes) return;
        auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
        auto* tracks = table ? dynamic_cast<TrackModel*>(table->model()) : nullptr;
        const QModelIndex source = mixTableRow(state.items->currentItem()); // r5-fix3-history: by its file
        state.selectedTrack = tracks && source.isValid() ? tracks->getTrack(source) : TrackPointer();
        refreshBrowseCard();
        refreshPreviewButton();
    });
    // A knob event that went to an open menu or dialog flashes the legend.
    decklab::knobRedirectedListener() = [surface = QPointer<WDeckLab>(this)] {
        if (surface) surface->flashKnobLegend();
    };
    refreshMatchReference();
    return m_browser;
}

QWidget* WDeckLab::buildMatchBar(QWidget* parent) {
    // The Match bar (B-49..B-53): reference deck, BPM direction and window,
    // key rule, how many tracks match, Only, and a clear. Shown over track
    // lists while matching is on.
    auto& b = *m_browseState;
    const int savedDirection = m_config->getValue(ConfigKey("[DeckLab]", "BrowseMatchDirection"), 1);
    b.direction = savedDirection == 0 ? decklab::TempoDirection::Slower
            : savedDirection == 2     ? decklab::TempoDirection::Faster
                                      : decklab::TempoDirection::Both;
    b.matchBar = new QWidget(parent);
    b.matchBar->setObjectName(QStringLiteral("decklab-match-bar"));
    b.matchBar->setAttribute(Qt::WA_StyledBackground);
    b.matchBar->setStyleSheet(QStringLiteral(
            "QWidget#decklab-match-bar{background:#0D0F12;border-bottom:1px solid #2C313A;}"));
    b.matchBar->setFixedHeight(46);
    auto* bar = row(b.matchBar, 4);
    bar->setContentsMargins(0, 1, 4, 1);
    b.matchDeck = new DeckLabDeckChip(b.matchBar);
    b.matchDeck->setObjectName(QStringLiteral("decklab-match-deck"));
    holdHeight(b.matchDeck, 44);
    b.matchDeck->setFixedWidth(128);
    connect(b.matchDeck, &QPushButton::clicked, this, [this] {
        // Auto, then each loaded deck in turn, then Auto again.
        auto& state = *m_browseState;
        int next = 0;
        for (int deck = state.pinnedDeck + 1; deck <= 4; ++deck) {
            if (value(channel(deck), "track_loaded") > 0) {
                next = deck;
                break;
            }
        }
        state.pinnedDeck = next;
        refreshMatchReference();
    });
    bar->addWidget(b.matchDeck);
    struct Segment {
        decklab::TempoDirection direction;
        decklab::Glyph glyph;
        QString name;
        QString accessible;
    };
    const std::array<Segment, 3> segments{
            Segment{decklab::TempoDirection::Slower, decklab::Glyph::ArrowDown,
                    QStringLiteral("decklab-match-slower"), tr("Slower tracks only")},
            Segment{decklab::TempoDirection::Both, decklab::Glyph::ArrowBoth,
                    QStringLiteral("decklab-match-both"), tr("Slower and faster tracks")},
            Segment{decklab::TempoDirection::Faster, decklab::Glyph::ArrowUp,
                    QStringLiteral("decklab-match-faster"), tr("Faster tracks only")}};
    for (int n = 0; n < 3; ++n) {
        const Segment& segment = segments[n];
        auto* button = browseButton(b.matchBar, QString(), segment.name, segment.accessible, segment.glyph, 44);
        connect(button, &QPushButton::clicked, this, [this, direction = segment.direction, n] {
            auto& state = *m_browseState;
            state.direction = direction;
            m_config->setValue(ConfigKey("[DeckLab]", "BrowseMatchDirection"), n);
            refreshMatchReference();
        });
        b.directions[n] = button;
        bar->addWidget(button);
    }
    b.matchSummary = new DeckLabTwoLineLabel(b.matchBar);
    b.matchSummary->setObjectName(QStringLiteral("decklab-match-summary"));
    bar->addWidget(b.matchSummary, 1);
    b.matchOnlyButton = browseButton(b.matchBar, tr("Only"), QStringLiteral("decklab-match-only"),
            tr("Show only matching tracks"), decklab::Glyph::None, 84);
    connect(b.matchOnlyButton, &QPushButton::clicked, this, [this] {
        auto& state = *m_browseState;
        state.matchOnly = !state.matchOnly;
        applyMatchFilter(true);
        updateMatchBar();
        updateBreadcrumb();
        scheduleMatchCount();
    });
    bar->addWidget(b.matchOnlyButton);
    b.matchClear = browseButton(b.matchBar, QString(), QStringLiteral("decklab-match-clear"),
            tr("Turn matching off"), decklab::Glyph::Close, 44);
    connect(b.matchClear, &QPushButton::clicked, this, [this] {
        auto& state = *m_browseState;
        state.matchOnly = false;
        applyMatchFilter(true);
        state.matchEnabled = false;
        m_config->setValue(ConfigKey("[DeckLab]", "BrowseMatch"), 0);
        refreshMatchReference();
        updateBreadcrumb();
    });
    bar->addWidget(b.matchClear);
    b.matchBar->hide();
    return b.matchBar;
}

QWidget* WDeckLab::buildSampler() {
    // r4-sampler: Move 4, the Sampler as a 4x4 instrument with the decks in
    // view (widget/decklabsampler.{h,cpp}; the [SamplerN] binding is
    // widget/decklabsamplercontrols.{h,cpp}). Wired here: loading an empty
    // pad from Browse and the return after the load, the DECKS strip, and
    // the FX lamp, which is the same Lab FX sampler send as SAMPLER FX.
    auto* samplers = new DeckLabSamplerControls(m_config, m_players);
    auto* page = new DeckLabSampler(samplers, this);
    samplers->setParent(page);
    m_sampler = page;
    page->setInteractionEpoch([this] { return m_interactionEpoch; });
    connect(page, &DeckLabSampler::loadRequested, this, [this](int sampler) {
        m_sampleEdit = m_sampler && m_sampler->editMode(); // r7-sampler: resumes back on the pads
        m_sampleTarget = sampler; m_sampleReturnPage = 2; selectPage(1);
        openSamples(true); // r7-browse: the samples first, ALL MUSIC for the rest
        refreshLoadButtons();
    });
    // r7 fix: a REPLACE trip's pad fired from the controller: LOAD PAD shows
    // it plays (and says so when tapped).
    for (int sampler = 1; sampler <= std::max(16, m_players ? m_players->numberOfSamplers() : 0); ++sampler) {
        watch(QStringLiteral("[Sampler%1]").arg(sampler), QStringLiteral("play"), [this, sampler](double) {
            if (m_sampleTarget == sampler) refreshLoadButtons();
        });
    }
    // Any page that sent Browse to load a pad gets it back once it loads.
    connect(samplers, &DeckLabSamplerControls::sampleLoaded, this, [this](int sampler) {
        if (m_sampleTarget == sampler) { m_sampleTarget = 0; selectPage(m_sampleReturnPage); }
    });
    connect(page, &DeckLabSampler::decksRequested, this, [this] { selectPage(0); });
    // round4 merge: Lab FX reaches the samplers through its SMP target (it
    // replaced the separate sampler send), so the FX lamp selects SMP and
    // turning it off hands Lab FX back to deck 1.
    connect(page, &DeckLabSampler::fxRoutingRequested, this, [this](bool on) {
        if (!m_labFx) return;
        if (on) m_labFx->selectTarget(DeckLabLabFx::kSampler);
        else if (m_labFx->target() == DeckLabLabFx::kSampler) m_labFx->selectTarget(1);
        m_sampler->setFxRouting(samplersGetLabFx());
    });
    page->setFxRouting(false);
    // r4-waveforms: under compact lanes the DECKS row replaces the strip.
    page->setDecksStripShown(decklab::lanes::presentationFor(2) != decklab::lanes::Presentation::Compact);
    return page;
}

// r4-sampler: the Sampler page's share of the display tick, only while it
// is shown; the page repaints only the pads that changed (playing pads
// drain). r4-waveforms: with the compact lanes shown, Decks' tick moves the
// deck clocks for the lanes' DECKS row; the page's own strip (and the deck
// clocks below) only run when the strip is shown.
void WDeckLab::tickSampler() {
    std::array<DeckLabSamplerDeck, 4> decks{};
    // r4-waveforms: with the lanes' DECKS row above, the strip is hidden and
    // Decks' tick moves the clocks; nothing to compute here.
    for (int deck = 1; deck <= 4 && m_sampler->decksStripShown(); ++deck) {
        const auto& d = m_deck[deck - 1];
        if (d.loaded->get() <= 0) continue;
        const double rawPosition = d.position->get();
        const double position = util_isfinite(rawPosition) ? rawPosition : 0;
        if (d.play->get() > 0 || position != m_shownPosition[deck - 1]) refreshDeckClock(deck);
        const auto& state = m_stripStates[deck - 1];
        auto& shown = decks[deck - 1];
        shown.loaded = state.loaded;
        shown.playing = state.playing;
        shown.master = state.master;
        shown.endWarning = state.endWarning;
        shown.phase = state.phase;
        shown.bpm = state.bpm.toDouble();
        shown.position = std::clamp(position, 0.0, 1.0);
        const double duration = d.duration->get();
        const double ratio = d.rateRatio->get();
        const double speed = util_isfinite(ratio) && ratio > 0 ? ratio : 1;
        shown.remaining = state.loaded && duration > 0 ? duration * (1 - shown.position) / speed : -1;
    }
    m_sampler->setFxRouting(samplersGetLabFx());
    m_sampler->tick(decks);
}

bool WDeckLab::samplersGetLabFx() const {
    return m_labFx && m_labFx->target() == DeckLabLabFx::kSampler;
}

void WDeckLab::showSampler(int sampler) {
    selectPage(2);
    if (m_sampler && sampler > 0) m_sampler->showPad(sampler);
}

QWidget* WDeckLab::buildEffects() {
    // r4-labfx: Lab FX is one instrument page (widget/decklablabfx.h). This
    // surface only lends it the engine, the deck readout and navigation.
    DeckLabLabFxHost host;
    host.value = [this](const QString& group, const QString& item) { return value(group, item); };
    host.set = [this](const QString& group, const QString& item, double v) { set(group, item, v); };
    host.setParameter = [this](const QString& group, const QString& item, double v) {
        control(group, item)->setParameter(v);
    };
    host.unitReady = [this, units = std::array<QString, 2>{QStringLiteral("[DeckLabEffectUnit1]"),
                                    QStringLiteral("[DeckLabEffectUnit2]")}](int unit) {
        auto chain = m_effects && unit >= 1 && unit <= 2 ? m_effects->getEffectChain(units[unit - 1]) : EffectChainPointer();
        return chain && chain->getEffectSlot(3) && chain->getEffectSlot(3)->isLoaded();
    };
    host.now = [this] { return m_clock.elapsed(); };
    host.deck = [this](int deck) {
        // The footer readout: title and play state from the deck readout,
        // the clock and beat lamp read fresh (Decks' clocks only tick there).
        DeckLabLabFxDeck info;
        if (deck < 1 || deck > 4) return info;
        const auto& d = m_deck[deck - 1];
        const auto& shown = m_stripStates[deck - 1];
        info.loaded = shown.loaded && d.loaded->get() > 0;
        if (!info.loaded) return info;
        info.playing = d.play->get() > 0;
        info.title = shown.title;
        const double rawPosition = d.position->get();
        const double position = std::clamp(util_isfinite(rawPosition) ? rawPosition : 0.0, 0.0, 1.0);
        const double ratio = d.rateRatio->get();
        const double speed = util_isfinite(ratio) && ratio > 0 ? ratio : 1;
        const double duration = d.duration->get();
        info.remaining = duration > 0 ? QStringLiteral("−") + timeText(duration * (1 - position) / speed) : QStringLiteral("—");
        info.endWarning = shown.endWarning;
        if (const auto beats = m_tracks[deck - 1] ? m_tracks[deck - 1]->getBeats() : mixxx::BeatsPointer()) {
            const auto frame = mixxx::audio::FramePos::fromEngineSamplePos(position * d.samples->get());
            mixxx::audio::FramePos previous, next;
            if (frame.isValid() && beats->findPrevNextBeats(frame, &previous, &next, false) && next > previous) {
                const long long index = beats->iteratorFrom(previous) - beats->cfirstmarker();
                info.phase = deckLabBeatPhase(index, (frame - previous) / (next - previous)).segment;
            }
        }
        return info;
    };
    host.showPage = [this](int page) { selectPage(page); };
    m_labFx = new DeckLabLabFx(std::move(host), this);
    // r4-waveforms: with lanes and their DECKS row above, the footer only
    // names the target.
    m_labFx->setLanesShown(decklab::lanes::presentationFor(3) == decklab::lanes::Presentation::Compact);
    m_labFx->setDeckLayout(m_fourDeckLayout ? 4 : 2); // r6-fxstrip: then updateLayers keeps it
    return m_labFx;
}

QWidget* WDeckLab::buildSettings() {
    // r4-system (Signature 1): Settings is a utility list driven by the knob
    // (decklab/decklabsettings.h). Deck Lab system integration (Deck Setup,
    // the Unit pages, tool routing, rail notices) hooks in through these calls.
    DeckLabSystemUi::Hooks hooks;
    hooks.showSystem = [this](DeckLabSystemPage::Section section) {
        selectPage(4);
        if (m_settings) m_settings->showUnit(section);
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
    hooks.notify = [this](const decklab::rail::Notice& notice) { if (m_railUi) m_railUi->notify(notice); };
    hooks.statusChanged = [this] { updateStatus(); };
    m_systemUi = new DeckLabSystemUi(this, hooks);
    connect(m_systemUi, &DeckLabSystemUi::toolsConnectedChanged, this, [this](bool connected) {
        const bool wasOnline = m_toolsOnline;
        m_toolsOnline = connected || m_clock.elapsed() - m_lastToolsReady <= 2000;
        for (auto* b : m_toolButtons) b->setEnabled(m_toolsOnline);
        if (wasOnline != m_toolsOnline) updateStatus();
    });
    DeckLabSettingsPage::Hooks settings;
    settings.value = [this](const QString& group, const QString& item) { return value(group, item); };
    settings.set = [this](const QString& group, const QString& item, double v) { set(group, item, v); };
    settings.watch = [this](const QString& group, const QString& item, std::function<void(double)> callback) {
        watch(group, item, std::move(callback));
    };
    settings.deckLayoutChosen = [this](int layout) {
        m_config->setValue(ConfigKey("[DeckLab]", "DeckLayout"), layout); updateLayers();
    };
    settings.autoDecks = [this] { return m_autoDeckLayout == 4 ? 4 : 2; };
    settings.fourDecksShown = [this] { return m_fourDeckLayout; };
    settings.sideBySide = [this] { return m_sideBySideWaveforms; };
    settings.waveformLayoutChosen = [this](bool sideBySide) {
        if (sideBySide == m_sideBySideWaveforms) return;
        for (auto* menu : findChildren<QMenu*>())
            if (menu->objectName().endsWith(QStringLiteral("more-menu"))) menu->close();
        releaseTouchCue(); ++m_interactionEpoch;
        m_sideBySideWaveforms = sideBySide;
        m_config->setValue(ConfigKey("[DeckLab]", "WaveformLayout"), sideBySide ? 1 : 0);
        updateLayers();
    };
    settings.keyNotationChosen = [this](int notation) { setKeyNotation(notation); };
    settings.dimPlayedChosen = [this](bool dim) {
        m_config->setValue(ConfigKey("[Library]", "ApplyPlayedTrackColor"), dim);
        BaseTrackTableModel::setApplyPlayedTrackColor(dim);
        decklab::history::Store::instance().setDimPlayed(dim); // r5-history: the history's marks dim too
        if (m_libraryWidget) m_libraryWidget->update();
    };
    settings.tool = [this](int id) { tool(id); };
    settings.advanced = [this] { releaseEffects(); emit advancedPreferencesRequested(); };
    settings.leave = [this] { selectPage(0); };
    m_settings = new DeckLabSettingsPage(m_config, m_systemUi, settings, this);
    m_toolButtons.append(m_settings->toolButtons());
    return m_settings;
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
            // A stray thumb on a playing deck's readout must never cover
            // the decks: Track info opens from a paused readout, or from
            // MORE at any time.
            if (infoPress->second) {
                if (m_deck[deck - 1].play->get() <= 0) showTrackInfo(deck);
            } else {
                selectPage(1);
            }
        }
    });
    strip->installEventFilter(this);
    cardRows->addWidget(strip);
    m_strips[deck - 1] = strip;
    m_meters[deck - 1] = strip->meter();
    m_cardTransports[deck - 1] = buildTransport(deck, false, card);
    cardRows->addWidget(m_cardTransports[deck - 1]);
    m_cardTransports[deck - 1]->hide();
    m_deckTools->attachCard(deck, card, strip); // r5-decks: KEY touch, BPM hold, the card's key sheet
    // r4-decks: stem pads are STEM mode in the side pad blocks (buildPads),
    // not a row of their own: no layout change when a stem track loads.
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
    // Suppression hides the viewer without reflowing the Decks page.
    QSizePolicy policy = widget->sizePolicy();
    policy.setRetainSizeWhenHidden(true);
    widget->setSizePolicy(policy);
    static_cast<QBoxLayout*>(body->layout())->addWidget(widget, 1);
    m_detailWaveforms[deck - 1] = widget;
    widget->setVisible(!waveformsSuppressed());
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

void WDeckLab::setWaveformsSuppressed(QObject* owner, bool suppressed) {
    if (!owner) return;
    const bool before = waveformsSuppressed();
    if (suppressed) {
        if (m_waveformSuppressors.contains(owner)) return;
        // Only the address is used: the owner is already gone when this runs.
        m_waveformSuppressors.insert(owner, connect(owner, &QObject::destroyed, this,
                [this, owner] { setWaveformsSuppressed(owner, false); }));
    } else {
        const auto found = m_waveformSuppressors.find(owner);
        if (found == m_waveformSuppressors.end()) return;
        disconnect(found.value());
        m_waveformSuppressors.erase(found);
    }
    if (before != waveformsSuppressed()) applyWaveformSuppression();
}

void WDeckLab::applyWaveformSuppression() {
    const bool suppressed = waveformsSuppressed();
    for (const auto& viewer : m_detailWaveforms) {
        if (!viewer) continue;
        if (suppressed && viewer->isVisible()) {
            // A hidden viewer never sees the release of a touch scratch that
            // is still held: end it now so the deck cannot stay held.
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(), QPointF(),
                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(viewer, &release);
        }
        // Showing maps the GL window again; WGLWidget::showEvent and the
        // render gate force a full render of its first frame.
        viewer->setVisible(!suppressed);
    }
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
    // usb-first: rekordbox phrase bands along the bottom edge; hidden while
    // the loaded track has no phrase data.
    m_phraseBands[deck - 1] = new DeckLabPhraseBands(widget);
    updateLayers();
    markDeckDirty(deck);
}

void WDeckLab::bindBrowser(WSearchLineEdit* search, WLibrarySidebar* sidebar, WLibrary* library) {
    m_search = search; m_sidebar = sidebar; m_libraryWidget = library;
    // r7: a stick plugged in mid-set re-reads the track list only while
    // Browse shows it; otherwise when Browse shows it again (showTracks).
    decklab::MediaPresence::instance().setOnScreen([self = QPointer<WDeckLab>(this)] {
        return !self || self->trackListOnScreen();
    });
    // r7 fix: a stick ejected or pulled while the DOWNLOADS list is open
    // leaves it at once (one folder read); off screen, when it shows again.
    connect(&decklab::MediaPresence::instance(), &decklab::MediaPresence::changed, this, [this] {
        if (!m_browseState || m_browseState->list != DeckLabList::Downloads ||
                m_browseState->itemsList != DeckLabList::Downloads) return;
        if (m_page == 1 && m_browseStack && m_browseStack->currentIndex() == 0) {
            showDownloads(m_browseState->downloadsKey);
        } else {
            m_browseState->downloadsStale = true;
        }
    });
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
        // Typing replaces a category value filter but stays in the source
        // (B-35); the breadcrumb says "Search “…” · n" (B-36).
        auto& state = *m_browseState;
        state.filter.clear();
        state.valueLabel.clear();
        restoreRecentSort(state, true); // r7-recent: so does Recently Added's order
        if (state.matchOnly) applyMatchFilter(true);
        showTracks();
        // Counts are final once the model has applied the query.
        QTimer::singleShot(0, this, [this] { updateBreadcrumb(); scheduleMatchCount(); });
    });
    if (m_library) {
        connect(m_library, &Library::trackSelected, this, [this](TrackPointer track) {
            // r4-browse: a Mixes list sets its own track (the table is hidden).
            if (m_browseState->list == DeckLabList::Mixes && m_browseStack->currentIndex() == 0) return;
            if (m_browseState->list == DeckLabList::History && m_browseStack->currentIndex() == 0) return; // r5-history
            m_browseState->selectedTrack = track;
            if (m_browseStack->currentIndex() != 0) refreshBrowseCard();
            refreshLoadButtons();
        });
        b.card->enableCovers();
    }
    if (m_library) m_library->setRowHeight(44);
    initHistory(); // r5-history
    initBackgroundAnalysis(); // r7-browse
    showSources();
}

void WDeckLab::selectPage(int page) {
    if (page < 0 || page > 4 || page == m_page) return;
    // r4-labfx: leaving Lab FX keeps a latched effect playing, and a held
    // one until its finger lifts (PF-X24); the rail tab shows it.
    setSearchOpen(false);
    // round5: the status sheet covers the page area but not the rail, so a tab
    // tap would put a page (and its knob claim) under it; close it first.
    if (m_railUi && m_railUi->sheet() && m_railUi->sheet()->isVisible()) m_railUi->sheet()->hide();
    // r7-sampler: a Load pad trip the Sampler page started with EDIT on
    // (REPLACE or an empty pad) comes back to the pads with EDIT still on,
    // loaded or cancelled; leaving Browse for any other page ends it.
    const bool resumeEdit = m_page == 1 && page == 2 && m_sampleEdit;
    if (m_page == 1 && page != 1) {
        // r7-browse: a pad's SAMPLES list, left by the load or Cancel: Browse
        // comes back on the home, never on samples.
        if (m_browseState && m_browseState->samplesForPad && folderTableShown()) {
            m_browseState->homeOnReturn = true;
        }
        m_sampleTarget = 0;
        m_sampleEdit = false;
    }
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
    if (resumeEdit && m_sampler) m_sampler->setEditMode(true); // r7-sampler
    // Transport has a fixed place below the Decks waveforms. Keeping it out
    // of the expanding page stack avoids sibling-page minimum sizes pushing
    // the essential controls below a small display.
    m_pages->setVisible(page != 0);
    m_deckControls->hide();
    m_root->removeWidget(m_waveforms);
    // r4-waveforms: which page shows the lanes, and how, is decided in one
    // place (decklab::lanes::presentationFor): Decks full, Sampler and Lab
    // FX two compact lanes over a DECKS row, Browse (its deck dock is the
    // LOAD target) and Settings none. Compact pages share one height, so a
    // Sampler/Lab FX switch never resizes the GL lanes.
    const auto lanes = decklab::lanes::presentationFor(page);
    if (lanes == decklab::lanes::Presentation::Full) {
        m_waveforms->setMinimumHeight(0); m_waveforms->setMaximumHeight(QWIDGETSIZE_MAX);
        m_root->insertWidget(1, m_waveforms, 1);
    } else {
        m_pages->setMinimumHeight(0); m_pages->setMaximumHeight(QWIDGETSIZE_MAX);
        if (lanes == decklab::lanes::Presentation::Compact) m_waveforms->setFixedHeight(compactLaneZoneHeight());
        m_root->insertWidget(1, m_waveforms);
    }
    m_waveforms->setVisible(lanes != decklab::lanes::Presentation::None);
    m_root->setStretchFactor(m_pages, page == 0 ? 0 : 1);
    if (page == 1) {
        if (m_browseState && std::exchange(m_browseState->homeOnReturn, false)) { m_hasBrowsed = true; showHome(QStringLiteral("samples")); }
        else if (!m_hasBrowsed) { m_hasBrowsed = true; showSources(); }
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
    if (b.list == DeckLabList::Home) {
        if (b.itemsList != DeckLabList::Home) rebuildHome(b.homeKey);
        else if (m_page == 1) refreshHomeIfSticksChanged();
    }
    if (b.list == DeckLabList::Downloads && b.itemsList == DeckLabList::Downloads && m_page == 1 &&
            std::exchange(b.downloadsStale, false)) {
        showDownloads(b.downloadsKey); // r7 fix: a stick went meanwhile
        return;
    }
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
        if (b.items->currentRow() < 0 && b.items->count()) {
            // r4-browse: never a disabled row (the home's "plug in a stick" tile).
            b.items->setCurrentRow(decklab::stepToEnabledRow(b.items->count(), -1, 1, [&b](int row) {
                return b.items->item(row)->flags().testFlag(Qt::ItemIsEnabled);
            }));
        }
    }
    if (b.list == DeckLabList::Home && m_page == 1) {
        if (!b.homeTimer.isActive()) b.homeTimer.start();
    } else {
        b.homeTimer.stop();
    }
    applyBrowsePresentation(); // r4-browse
    updateBreadcrumb(); refreshLoadButtons(); refreshBrowseCard(); updateMatchBar();
}
void WDeckLab::showTracks() {
    if (!m_libraryWidget) return;
    m_browseStack->setCurrentWidget(m_libraryWidget);
    if (value("[Sidebar]", "sidebar_visible")) set("[Sidebar]", "sidebar_visible", 0);
    set("[Library]", "focused_widget", 3);
    if (m_browseState) m_browseState->homeTimer.stop();
    restoreTableSelection(); // r5-fix2-history
    syncSelectedTrack();
    if (m_page == 1) decklab::MediaPresence::instance().shownOnScreen(); // r7: a stale list reads now
    applyBrowsePresentation(); // r4-browse
    updateBreadcrumb(); refreshLoadButtons(); refreshBrowseCard(); updateMatchBar(); scheduleMatchCount();
}
bool WDeckLab::trackListOnScreen() const {
    return m_page == 1 && m_browseStack && m_libraryWidget && m_browseStack->currentWidget() == m_libraryWidget;
}
void WDeckLab::browsePress() {
    if (m_page == 2 && m_sampler && m_sampler->knob(decklab::KnobAction::Press, 1)) return; // r4-sampler: pad sheet
    if (m_page != 1) { selectPage(1); return; }
    if (consumeNewSetRelease()) return; // r5-fix-history: a full knob hold's release press
    // In a track list (r4-browse: or Mixes) the press previews the highlighted track.
    if (browseTrackLevel()) { togglePreview(); return; }
    browseEnter();
}
void WDeckLab::browseEnter() {
    if (m_page != 1) { selectPage(1); return; }
    if (!m_sidebar || m_browseStack->currentIndex() != 0) return;
    auto& b = *m_browseState;
    switch (b.list) {
    case DeckLabList::Home: openHomeRow(b.items->currentRow()); return;
    case DeckLabList::Categories: openCategory(b.items->currentRow()); return;
    case DeckLabList::Values: openCategoryValue(b.items->currentRow()); return;
    case DeckLabList::Folder: openFolderRow(b.items->currentRow()); return;
    case DeckLabList::Sources: openSource(); return;
    case DeckLabList::Mixes: return; // r4-browse: rows are tracks; the press previews
    case DeckLabList::History: openHistoryRow(b.items->currentRow()); return; // r5-history
    case DeckLabList::Downloads: openDownloadsRow(b.items->currentRow()); return; // r7-browse
    }
}
void WDeckLab::browseMove(int delta, bool listLevel) {
    if (m_page == 2 && m_sampler && m_sampler->knob(decklab::KnobAction::Move, delta)) return; // r4-sampler: pad sheet
    if (delta != 0) cancelNewSetKnob(); // r5-fix-history: a turn never confirms New set
    if (delta == 0 || !m_sidebar) return;
    auto& b = *m_browseState;
    // A turn on any other page opens Browse where it was left. It never
    // becomes a key press on a deck, tempo or settings control.
    if (m_page != 1) selectPage(1);
    if (listLevel && m_browseStack->currentIndex() != 0) showSources();
    if (m_browseStack->currentIndex() != 0) {
        // r4-browse (C-03): sustained fast turns move further in track lists.
        delta = b.accelerator.steps(delta, m_clock.elapsed());
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
    if (b.list == DeckLabList::History) { historyMove(delta); return; } // r5-history
    const int count = b.items->count();
    if (!count) return;
    int rowIndex = 0;
    if (b.list == DeckLabList::Home) {
        // r4-browse: tiles in reading order, then the "Also" chips; the
        // "plug in a stick" tile is never highlighted.
        rowIndex = decklab::stepToEnabledRow(count, b.items->currentRow(), delta, [&b](int row) {
            return b.items->item(row)->flags().testFlag(Qt::ItemIsEnabled);
        });
        if (rowIndex < 0) return;
    } else {
        if (b.list == DeckLabList::Values || b.list == DeckLabList::Mixes) delta = b.accelerator.steps(delta, m_clock.elapsed());
        rowIndex = std::clamp(b.items->currentRow() + delta, 0, count - 1);
    }
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
    if (m_page == 2 && m_sampler && m_sampler->knob(decklab::KnobAction::Back, -1)) return; // r4-sampler: pad sheet
    cancelNewSetKnob(); // r5-fix-history: nor does Back (Shift+press)
    if (m_keyboard && m_keyboard->isVisible()) { setSearchOpen(false); return; }
    if (m_page != 1) { selectPage(1); return; }
    if (!m_sidebar) return;
    auto& b = *m_browseState;
    if (m_browseStack->currentIndex() != 0) {
        // r7-browse: the tracks of DOWNLOADS go back to its list, SAMPLES to the home.
        if (folderTableShown()) {
            if (b.list == DeckLabList::Downloads) showDownloads(b.downloadsKey);
            else showHome(QStringLiteral("samples"));
            return;
        }
        // Leaving a category's tracks restores the whole source.
        clearBrowseFilter();
        b.fromCategories = false;
        // r4-browse: the categories come back fresh ("Mixes with deck N" is
        // live), on the category that was opened.
        if (b.list == DeckLabList::Categories && b.itemsList == DeckLabList::Categories && !b.loadFailed) showCategories();
        else showSources();
        return;
    }
    switch (b.list) {
    case DeckLabList::Home: {
        // r4-browse (B-48): the top level. Back does nothing but say so (the
        // legend flashes); a second Back within 2 s goes to Decks, for
        // controllers without a page button.
        if (m_clock.elapsed() - b.homeBackArmedAt < 2000) {
            b.homeBackArmedAt = -100000;
            b.homeBackTimer.stop();
            selectPage(0);
            return;
        }
        b.homeBackArmedAt = m_clock.elapsed();
        b.homeBackTimer.start();
        flashKnobLegend();
        return;
    }
    case DeckLabList::Mixes:
        showCategories();
        return;
    case DeckLabList::History: // r5-history
        historyBack();
        return;
    case DeckLabList::Downloads: // r7-browse
        showHome(QStringLiteral("downloads"));
        return;
    case DeckLabList::Values:
        showCategories();
        return;
    case DeckLabList::Folder:
        if (b.stickPath.isEmpty() || QDir::cleanPath(b.folderPath) == QDir::cleanPath(b.stickPath)) showHome(b.homeKey);
        else showFolder(QFileInfo(b.folderPath).absolutePath());
        return;
    case DeckLabList::Categories: {
        if (!b.fromMore) { showHome(b.homeKey); return; }
        // Opened from "More sources": back to that list, on this source.
        const QPersistentModelIndex source = b.source;
        resetBrowseLevels();
        b.fromMore = true;
        b.list = DeckLabList::Sources;
        if (source.isValid()) {
            m_sidebar->setRootIndex(source.parent());
            m_sidebar->setCurrentIndex(source);
        }
        showSources();
        return;
    }
    case DeckLabList::Sources:
        break;
    }
    if (b.returnRoot.isValid() && m_sidebar->rootIndex() == b.returnRoot && b.source.isValid()) {
        b.returnRoot = QPersistentModelIndex();
        activateBrowseSource();
        showCategories();
        return;
    }
    const QPersistentModelIndex previous(m_sidebar->rootIndex());
    if (!previous.isValid()) { showHome(b.fromMore ? QStringLiteral("more") : b.homeKey); return; }
    if (!previous.parent().isValid() && !b.fromMore) { showHome(b.homeKey); return; }
    m_sidebar->setRootIndex(previous.parent());
    m_sidebar->setCurrentIndex(previous);
    showSources();
}
QStringList WDeckLab::browseSegments() const {
    const auto& b = *m_browseState;
    // Source names as a DJ knows them: Mixxx's collection is "Library", a
    // stick is its label (no "Rekordbox" level in between).
    const auto pathTo = [](QModelIndex at) {
        QStringList path;
        while (at.isValid()) {
            const QModelIndex parent = at.parent();
            if (!parent.isValid()) {
                const QString icon = at.data(SidebarModel::IconNameRole).toString();
                if (icon == QStringLiteral("tracks")) path.prepend(tr("Library"));
                else if (icon != QStringLiteral("rekordbox") || path.isEmpty()) path.prepend(at.data().toString());
            } else {
                path.prepend(at.data().toString());
            }
            at = parent;
        }
        return path;
    };
    const auto folderPath = [&b] {
        QStringList path{b.stickName.isEmpty() ? tr("USB stick") : b.stickName};
        const QString relative = QDir(b.stickPath).relativeFilePath(b.folderPath);
        if (!b.stickPath.isEmpty() && !relative.isEmpty() && relative != QStringLiteral(".") && !relative.startsWith(QStringLiteral(".."))) {
            path += relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        }
        return path;
    };
    const bool tracks = m_browseStack->currentIndex() != 0;
    if (tracks) {
        const QString typed = m_search ? m_search->currentText().trimmed() : QString();
        QStringList path;
        if (folderTableShown()) { // r7-browse
            if (b.folderList == decklab::FolderList::Samples) path << tr("Samples");
            else path << tr("Downloads");
            if (b.folderList == decklab::FolderList::Playlist) path << b.folderTitle;
            if (b.folderList == decklab::FolderList::Tracks && typed.isEmpty()) path << tr("Tracks");
        } else if (b.list == DeckLabList::Folder) {
            path = folderPath();
            if (typed.isEmpty() && !b.matchOnly) path << tr("Tracks");
        } else if (b.source.isValid() && b.fromCategories) {
            path = pathTo(b.source);
            const bool filtered = !b.filter.isEmpty();
            if (filtered) {
                path << decklab::categoryName(b.category);
                if (!b.valueLabel.isEmpty()) path << b.valueLabel; // r7-recent: Recently Added has none
            } else if (typed.isEmpty() && !b.matchOnly) path << decklab::categoryName(decklab::Category::AllTracks);
        } else {
            path = pathTo(m_sidebar->currentIndex());
        }
        if (path.isEmpty()) path << tr("Library");
        auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
        const int count = table && table->model() ? table->model()->rowCount() : -1;
        if (!typed.isEmpty()) {
            path << (count >= 0 && !b.matchOnly ? tr("Search “%1” · %2").arg(typed, QLocale().toString(count))
                                                : tr("Search “%1”").arg(typed));
        }
        if (b.matchOnly) {
            const int deck = decklab::matchReference().deck;
            const QString matches = deck > 0 ? tr("Matches deck %1").arg(deck) : tr("Matches");
            path << (count >= 0 ? matches + QStringLiteral(" · ") + QLocale().toString(count) : matches);
        }
        return path;
    }
    switch (b.list) {
    case DeckLabList::Home:
        return {tr("Sources")};
    case DeckLabList::Folder:
        return folderPath();
    case DeckLabList::Categories:
        return b.source.isValid() ? pathTo(b.source) : QStringList{b.stickName.isEmpty() ? tr("USB stick") : b.stickName};
    case DeckLabList::Values: {
        QStringList path = pathTo(b.source);
        path << decklab::categoryName(b.category);
        return path;
    }
    case DeckLabList::Mixes: {
        // r4-browse: "DJ USB › Mixes with deck 1 · 37".
        QStringList path = b.source.isValid() ? pathTo(b.source) : QStringList{b.stickName.isEmpty() ? tr("USB stick") : b.stickName};
        const int count = b.items->count() == 1 && !(b.items->item(0)->flags() & Qt::ItemIsEnabled) ? 0 : b.items->count();
        path << decklab::mixesTitle(b.mixShown.deck) + QStringLiteral(" · ") + QLocale().toString(count);
        return path;
    }
    case DeckLabList::History: // r5-history
        return historySegments();
    case DeckLabList::Downloads: // r7-browse
        return {tr("Downloads")};
    case DeckLabList::Sources:
        break;
    }
    if (!m_sidebar->rootIndex().isValid()) return {tr("Sources"), tr("More sources")};
    QStringList path = pathTo(m_sidebar->rootIndex());
    if (b.rekordbox && b.returnRoot.isValid() && m_sidebar->rootIndex() == b.returnRoot) {
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
        if (!browseTrackLevel()) return; // r4-browse: Mixes too
        if (value(group, "play")) { // r7 fix: say it instead of ignoring the tap
            if (m_browseState->toast) m_browseState->toast->showMessage(tr("Pad %1 is playing. Stop it first.").arg(m_sampleTarget), 2400, true);
            return;
        }
        if (listRowStale() && !browseLoadableNow()) { // r5-fix2-history: exactly the highlighted row, settled now
            if (m_browseState->toast) m_browseState->toast->showMessage(tr("This track can't be loaded right now"), 2000);
            return;
        }
        if (selectedFileGone()) return; // r7 fix
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
    if (m_page != 1 || !m_libraryWidget || !browseTrackLevel()) return; // r4-browse: Mixes too
    if (listRowStale() && !browseLoadableNow()) { // r5-fix2-history: exactly the highlighted row, settled now
        if (m_browseState->toast) m_browseState->toast->showMessage(tr("This track can't be loaded right now"), 2000);
        return;
    }
    if (selectedFileGone()) return; // r7 fix: the deck keeps its track
    set("[Library]", "focused_widget", 3);
    pulse(channel(deck), "LoadSelectedTrack");
    // Return happens on newTrackLoaded, never merely on a button press.
}
void WDeckLab::refreshLoadButtons() {
    if (!m_loadButtons[0]) return;
    using Tone = DeckLabActionButton::Tone;
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    const bool tracks = browseTrackLevel(); // r4-browse: a track list or Mixes
    const bool loadable = table && table->selectionModel() && !table->selectionModel()->selectedRows().isEmpty() &&
            browseRowLoadable(); // r5-fix-history: the table row is the highlighted row
    setTrackLevel(tracks && loadable); // r5-fix-history: controller LOAD on list levels
    // r5-fix3-history: a History or Mixes track row still waiting for its
    // table is offered too: the tap settles it, then loads exactly that row.
    const bool selected = loadable || listTrackRow();
    // r4-browse (move 3): the dock shows two tiles, four in the four-deck layout.
    if (m_browseState && m_browseState->dock) m_browseState->dock->setShownCount(m_fourDeckLayout && !m_sampleTarget ? 4 : 2);
    for (int deck = 1; deck <= 4; ++deck) {
        auto* load = static_cast<DeckLabDockTile*>(m_loadButtons[deck - 1]);
        load->setLoadReady(false);
        if (m_sampleTarget) {
            const bool padPlaying = value(QStringLiteral("[Sampler%1]").arg(m_sampleTarget), "play") > 0;
            const bool ready = tracks && selected && !padPlaying;
            load->setDeck(0);
            load->setText(deck == 1 ? tr("Load pad %1").arg(m_sampleTarget) : tr("Cancel"));
            load->setDetail(deck == 1 ? (padPlaying ? tr("Playing") : tr("Sampler")) : tr("Back to pads"));
            load->setTone(deck == 1 && ready ? Tone::Ready : Tone::Neutral);
            load->setEnabled(deck == 2 || ready);
            load->setAccessibleName(load->text());
            continue;
        }
        const int target = m_fourDeckLayout ? deck : (deck <= 2 ? displayedDeck(deck - 1) : deck);
        const bool playing = value(channel(target), "play") > 0;
        const bool loaded = value(channel(target), "track_loaded") > 0;
        // Deck identity (B-29) and what a load would replace (B-30).
        QString title;
        if (loaded && !playing) {
            auto* player = m_players ? m_players->getPlayer(channel(target)) : nullptr;
            if (const TrackPointer track = player ? player->getLoadedTrack() : TrackPointer()) {
                title = track->getTitle().trimmed();
                if (title.isEmpty()) title = QFileInfo(track->getLocation()).completeBaseName();
            }
        }
        load->setDeck(target);
        load->setText(tr("Load %1").arg(target));
        load->setDetail(playing ? tr("ON AIR") : loaded ? (title.isEmpty() ? tr("Loaded") : tr("Replaces %1").arg(title)) : tr("Empty"));
        load->setTone(playing ? Tone::OnAir : tracks && selected ? Tone::Ready : Tone::Neutral);
        load->setEnabled(!playing && tracks && selected);
        load->setLoadReady(!playing && tracks && selected);
        load->setAccessibleName(playing ? tr("Deck %1 is on air and cannot be loaded").arg(target)
                : title.isEmpty() ? tr("Load deck %1").arg(target)
                                  : tr("Load deck %1, replaces %2").arg(target).arg(title));
    }
    refreshDock(); // r4-browse
    refreshPreviewButton();
    scheduleMatchRefresh();
}
void WDeckLab::resetBrowseLevels() {
    auto& b = *m_browseState;
    clearBrowseFilter();
    b.list = DeckLabList::Home;
    b.source = QPersistentModelIndex();
    b.returnRoot = QPersistentModelIndex();
    b.fromCategories = false;
    b.fromMore = false;
    b.rekordbox = false;
    b.pendingStick.clear();
    b.loadFailed = false;
    b.loadingSince = -1;
    b.loadingTimer.stop();
    b.folderList = decklab::FolderList::None; // r7-browse
    b.samplesForPad = false;
}
void WDeckLab::clearBrowseFilter() {
    auto& b = *m_browseState;
    // Category value filters and "Only" matches are both filters on the
    // current list; leaving it drops them.
    const bool filtered = !b.filter.isEmpty() || !b.matchApplied.isEmpty();
    b.filter.clear();
    b.matchOnly = false;
    b.matchApplied.clear();
    // r7-recent: the search below reads the sort given back.
    restoreRecentSort(b, !(filtered && m_libraryWidget));
    if (filtered && m_libraryWidget) m_libraryWidget->search(QString());
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
    b.categoryReturn = false; // r4-browse
    b.categoryOnMixes = false;
    b.trackCount = -1;
    b.pendingStick.clear();
    b.loadFailed = false;
    if (b.rekordbox) {
        b.stickPath = rekordboxDevicePath(source);
        b.stickName = source.data().toString();
    }
    // A source opens unfiltered: drop typed or earlier category searches.
    if (m_search) m_search->slotRestoreSearch(QString());
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    auto* tracks = table ? dynamic_cast<TrackModel*>(table->model()) : nullptr;
    const bool ready = !b.rekordbox || rekordboxImported(source);
    if (ready && tracks && !tracks->currentSearch().isEmpty()) m_libraryWidget->search(QString());
    if (ready && table && table->model()) b.trackCount = table->model()->rowCount();
    if (ready && b.rekordbox && b.trackCount >= 0) b.stickTracks.insert(b.stickPath, b.trackCount);
    // r7-recent: Recently Added counts from the same rows. A rekordbox
    // export has no date added.
    b.recentCount = -1;
    if (ready && !b.rekordbox && tracks) {
        b.recentCount = decklab::countAddedSince(*table->model(),
                tracks->columnIndexFromSortColumnId(TrackModel::SortColumnId::DateTimeAdded),
                decklab::recentlyAddedSince(QDate::currentDate()));
    }
    b.loadingSince = ready ? -1 : m_clock.elapsed();
    showCategories();
}
void WDeckLab::showCategories() {
    auto& b = *m_browseState;
    using decklab::Category;
    const bool ready = !b.rekordbox ||
            (b.pendingStick.isEmpty() && b.source.isValid() && rekordboxImported(b.source));
    b.list = DeckLabList::Categories;
    b.itemsList = DeckLabList::Categories;
    if (b.rekordbox && !ready && b.loadFailed) {
        // B-10: the export could not be read in time. Say so and offer the
        // stick's files; a late read still opens the categories.
        const QSignalBlocker blocker(b.items);
        b.items->clear();
        auto* error = new QListWidgetItem(tr("Couldn't read this stick's rekordbox export"), b.items);
        error->setData(DeckLabListDelegate::GlyphRole, static_cast<int>(decklab::Glyph::Warning));
        error->setData(DeckLabListDelegate::KeyRole, QStringLiteral("error"));
        auto* retry = new QListWidgetItem(tr("Try again"), b.items);
        retry->setData(DeckLabListDelegate::GlyphRole, static_cast<int>(decklab::Glyph::Retry));
        retry->setData(DeckLabListDelegate::KeyRole, QStringLiteral("retry"));
        retry->setData(DeckLabListDelegate::ChevronRole, true);
        auto* files = new QListWidgetItem(tr("Browse its files"), b.items);
        files->setData(DeckLabListDelegate::GlyphRole, static_cast<int>(decklab::Glyph::Folder));
        files->setData(DeckLabListDelegate::KeyRole, QStringLiteral("files"));
        files->setData(DeckLabListDelegate::ChevronRole, true);
        if (b.stickPath.isEmpty()) files->setFlags(files->flags() & ~Qt::ItemIsEnabled);
        b.items->setCurrentRow(1);
        showSources();
        return;
    }
    const QAbstractItemModel* sources = m_sidebar ? m_sidebar->model() : nullptr;
    const QString loading = tr("Reading…");
    QList<std::tuple<Category, bool, QString>> entries;
    entries.append(std::make_tuple(Category::AllTracks, ready, !ready ? loading : b.trackCount >= 0 ? QString::number(b.trackCount) : QString()));
    // r7-recent: the library's tracks of the last 30 days; no row without any.
    if (!b.rekordbox && b.recentCount > 0) {
        entries.append(std::make_tuple(Category::RecentlyAdded, ready, ready ? QString::number(b.recentCount) : loading));
    }
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
        // r4-browse (move 5): a live "Mixes with deck N" row first, while a
        // deck is loaded and the source's tracks are known.
        bool mixes = false;
        if (ready) {
            refreshMixCandidates();
            if (mixReference().active()) {
                auto* item = new QListWidgetItem(decklab::mixesTitle(mixReference().deck), b.items);
                item->setData(DeckLabListDelegate::KeyRole, QStringLiteral("mixes"));
                item->setData(DeckLabListDelegate::ChevronRole, true);
                mixes = true;
            }
        }
        for (const auto& [category, enabled, detail] : entries) {
            auto* item = new QListWidgetItem(decklab::categoryName(category), b.items);
            item->setData(DeckLabListDelegate::CategoryRole, static_cast<int>(category));
            item->setData(DeckLabListDelegate::GlyphRole, static_cast<int>(categoryGlyph(category)));
            item->setData(DeckLabListDelegate::ChevronRole, true);
            item->setData(DeckLabListDelegate::CountRole, detail);
            if (!enabled) item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        }
        int current = std::clamp(b.categoryRow, 0, std::max(0, b.items->count() - 1));
        if (b.categoryReturn && !b.categoryOnMixes) {
            // Back on the category that was opened, wherever the Mixes row is.
            for (int row = mixes ? 1 : 0; row < b.items->count(); ++row) {
                if (b.items->item(row)->data(DeckLabListDelegate::CategoryRole).toInt() == static_cast<int>(b.category)) {
                    current = row;
                    break;
                }
            }
        } else if (b.categoryOnMixes || !b.categoryReturn) {
            current = 0;
        }
        b.items->setCurrentRow(current);
    }
    updateMixesRow();
    if (ready) {
        b.loadingTimer.stop();
    } else {
        if (b.loadingSince < 0) b.loadingSince = m_clock.elapsed();
        b.loadingTimer.start();
    }
    showSources();
}
void WDeckLab::openCategory(int rowIndex) {
    auto& b = *m_browseState;
    auto* item = b.items->item(rowIndex);
    if (!item || !(item->flags() & Qt::ItemIsEnabled) || !m_sidebar) return;
    const QString action = item->data(DeckLabListDelegate::KeyRole).toString();
    if (action == QStringLiteral("retry")) { retryRekordboxRead(); return; }
    if (action == QStringLiteral("files")) { if (!b.stickPath.isEmpty()) showFolder(b.stickPath); return; }
    if (action == QStringLiteral("mixes")) { showMixes(); return; } // r4-browse
    if (!action.isEmpty()) return;
    using decklab::Category;
    const auto category = static_cast<Category>(item->data(DeckLabListDelegate::CategoryRole).toInt());
    b.categoryRow = rowIndex;
    b.category = category;
    b.categoryReturn = true;
    b.categoryOnMixes = false;
    if (category == Category::AllTracks) {
        activateBrowseSource();
        clearBrowseFilter();
        b.fromCategories = true;
        showTracks();
        return;
    }
    if (category == Category::RecentlyAdded) {
        // r7-recent: the library's tracks with a date added filter the
        // breadcrumb names (the search box stays free for typing), newest first.
        if (!m_libraryWidget) return;
        activateBrowseSource();
        if (m_search) m_search->slotRestoreSearch(QString());
        clearBrowseFilter();
        const QString query = decklab::recentlyAddedQuery(QDate::currentDate());
        sortNewestFirst(b, m_libraryWidget->getCurrentTrackTableView());
        m_libraryWidget->search(query);
        b.filter = query;
        b.valueLabel.clear();
        b.fromCategories = true;
        showTracks();
        return;
    }
    if (category == Category::History && !b.rekordbox) { showHistory(true); return; } // r5-history
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
    if (!b.rekordbox) { b.loadingTimer.stop(); return; }
    if (!b.pendingStick.isEmpty()) {
        // The rekordbox feature lists a stick only after its scan.
        const QModelIndex device = m_sidebar ? rekordboxDeviceFor(m_sidebar->model(), b.pendingStick) : QModelIndex();
        if (device.isValid()) {
            b.pendingStick.clear();
            b.source = device;
            b.stickName = device.data().toString();
            if (auto* model = qobject_cast<SidebarModel*>(m_sidebar->model())) {
                model->doubleClicked(device);
                model->clicked(device); // starts reading its export
            }
        }
    }
    if (b.pendingStick.isEmpty() && !b.source.isValid()) { b.loadingTimer.stop(); return; }
    const bool imported = b.pendingStick.isEmpty() && rekordboxImported(b.source);
    if (!imported) {
        // B-10: a damaged export never finishes; stop saying "Reading…"
        // after 20 s ([DeckLab] RekordboxReadTimeoutMs).
        b.readTimeoutMs = std::max(100, m_config->getValue(ConfigKey("[DeckLab]", "RekordboxReadTimeoutMs"), 20000));
        if (!b.loadFailed && b.loadingSince >= 0 && m_clock.elapsed() - b.loadingSince >= b.readTimeoutMs) {
            b.loadFailed = true;
            if (!b.stickPath.isEmpty()) b.failedSticks.insert(b.stickPath);
            b.readingSticks.remove(b.stickPath); // r4-browse
            qWarning() << "DeckLab Browse: no rekordbox export read from" << b.stickName << "after"
                       << b.readTimeoutMs << "ms";
            if (m_page == 1 && b.list == DeckLabList::Categories && m_browseStack->currentIndex() == 0) showCategories();
        }
        return; // keep checking: a slow stick may still finish
    }
    // Keep polling (one data() call) until Browse is visible again; never
    // move focus or Library state while another page is shown.
    if (m_page != 1) return;
    b.loadingTimer.stop();
    b.loadFailed = false;
    b.failedSticks.remove(b.stickPath);
    b.readingSticks.remove(b.stickPath); // r4-browse
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    if (table && table->model()) b.trackCount = table->model()->rowCount();
    if (!b.stickPath.isEmpty() && b.trackCount >= 0) b.stickTracks.insert(b.stickPath, b.trackCount);
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
    if (b.list == DeckLabList::History && m_browseStack->currentIndex() == 0) { refreshHistoryCard(); return; } // r5-history
    if (browseTrackLevel()) { // r4-browse: Mixes too
        // r6-keys: a track Mixxx has no key for shows the key its row stores
        // (a rekordbox export's "8A", a USB file's tag), never "—".
        QString stored;
        if (b.selectedTrack && b.selectedTrack->getKey() == mixxx::track::io::key::INVALID) {
            if (b.list == DeckLabList::Mixes && m_browseStack->currentIndex() == 0) {
                if (const auto* item = b.items->currentItem()) stored = item->data(DeckLabListDelegate::MixKeyRole).toString();
            } else if (auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr) {
                auto* tracks = dynamic_cast<TrackModel*>(table->model());
                const int column = tracks ? tracks->columnIndexFromSortColumnId(TrackModel::SortColumnId::Key) : -1;
                const QModelIndexList rows = table->selectionModel() ? table->selectionModel()->selectedRows() : QModelIndexList();
                if (column >= 0 && rows.size() == 1) stored = rows.first().sibling(rows.first().row(), column).data().toString();
            }
        }
        b.card->showTrack(b.selectedTrack, stored);
        return;
    }
    const QString openHint = tr("Turn or swipe to choose · press or tap to open");
    if (b.list == DeckLabList::Home || b.list == DeckLabList::Folder) {
        const QListWidgetItem* item = b.items->currentItem();
        if (!item) {
            b.card->showItem(decklab::Glyph::Usb, tr("Sources"), tr("Plug in a USB stick to play its music"), QString());
            return;
        }
        const QString key = item->data(DeckLabListDelegate::KeyRole).toString();
        const auto glyph = static_cast<decklab::Glyph>(item->data(DeckLabListDelegate::GlyphRole).toInt());
        QString subtitle = item->data(DeckLabListDelegate::SubtitleRole).toString();
        QString hint = openHint;
        if (b.list == DeckLabList::Folder) {
            const QString count = item->data(DeckLabListDelegate::CountRole).toString();
            subtitle = key == QStringLiteral("tracks") ? countText(count.toInt(), tr("1 track"), tr("%1 tracks"))
                                                       : (b.stickName.isEmpty() ? tr("Folder") : b.stickName);
            if (key == QStringLiteral("tracks")) hint = tr("Press or tap to show them");
        } else if (key.startsWith(QStringLiteral("usb:"))) {
            hint = subtitle.startsWith(tr("rekordbox")) ? tr("Opens its playlists and tracks") : tr("Opens its folders");
        } else if (key == QStringLiteral("more")) {
            hint = tr("Everything else this unit can browse");
        } else if (key == QStringLiteral("root:tracks")) {
            hint = tr("By artist, album, BPM and key");
        } else if (key == QStringLiteral("downloads")) {
            hint = tr("Newest first; a Spotify playlist is a folder"); // r7-browse
        } else if (key == QStringLiteral("samples")) {
            hint = tr("Load them into the sampler pads");
        }
        b.card->showItem(glyph, item->text(), subtitle, hint);
        return;
    }
    if (b.list == DeckLabList::Downloads) { // r7-browse
        const QListWidgetItem* item = b.items->currentItem();
        if (!item) return;
        const bool open = item->flags().testFlag(Qt::ItemIsEnabled);
        const QString count = item->data(DeckLabListDelegate::CountRole).toString();
        b.card->showItem(static_cast<decklab::Glyph>(item->data(DeckLabListDelegate::GlyphRole).toInt()), item->text(),
                count.isEmpty() ? item->data(DeckLabListDelegate::SubtitleRole).toString()
                                : countText(count.toInt(), tr("1 track"), tr("%1 tracks")),
                open ? tr("Press or tap to show them") : QString());
        return;
    }
    if (b.list != DeckLabList::Sources) {
        const QListWidgetItem* item = b.items->currentItem();
        if (!item) return;
        const QString source = currentSourceName();
        if (b.list == DeckLabList::Categories && b.loadFailed) {
            b.card->showItem(decklab::Glyph::Warning, source, tr("Couldn't read this stick's rekordbox export."),
                    tr("Try again, or browse its files."));
            return;
        }
        if (b.list == DeckLabList::Categories && item->data(DeckLabListDelegate::KeyRole).toString() == QStringLiteral("mixes")) {
            // r4-browse (move 5): what "Mixes with deck N" holds, in the deck colour.
            const auto reference = mixReference();
            const auto& deck = m_stripStates[std::clamp(reference.deck, 1, 4) - 1];
            QStringList about;
            if (!deck.title.isEmpty()) about << deck.title;
            const QString key = decklab::formatKey(reference.key); // r6-keys
            if (reference.key != mixxx::track::io::key::INVALID && !key.isEmpty()) about << key;
            if (reference.bpm > 0) about << QLocale().toString(reference.bpm, 'f', 1);
            if (deck.playing) about << tr("on air");
            const QString count = item->data(DeckLabListDelegate::CountRole).toString();
            const auto window = reference.window();
            b.card->showItem(decklab::Glyph::Mixes, item->text(), about.join(QStringLiteral(" · ")),
                    window.isValid() ? tr("%1 tracks in key and within %2 BPM").arg(count, decklab::tempoWindowText(window))
                                     : tr("%1 tracks in key").arg(count),
                    deckLabDeckColor(reference.deck));
            return;
        }
        if (b.list == DeckLabList::Categories) {
            const auto category = static_cast<decklab::Category>(item->data(DeckLabListDelegate::CategoryRole).toInt());
            const QString count = item->data(DeckLabListDelegate::CountRole).toString();
            const bool enabled = item->flags() & Qt::ItemIsEnabled;
            QString about = !enabled || count.isEmpty() || category != decklab::Category::AllTracks ? source
                    : count == QStringLiteral("1") ? tr("%1 · 1 track").arg(source) : tr("%1 · %2 tracks").arg(source, count);
            if (category == decklab::Category::RecentlyAdded && enabled && !count.isEmpty()) { // r7-recent
                const QString days = QString::number(decklab::kRecentlyAddedDays);
                // The card is narrow: the days first, then the count.
                about = count == QStringLiteral("1") ? tr("Last %1 days · 1 track").arg(days)
                                                     : tr("Last %1 days · %2 tracks").arg(days, count);
            }
            b.card->showItem(categoryGlyph(category), item->text(), about,
                    enabled ? openHint : tr("Reading %1…").arg(source));
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
        b.card->showItem(decklab::Glyph::Sources, tr("Sources"), tr("Library, playlists and USB sticks"), openHint);
        return;
    }
    QString subtitle;
    const QString icon = rootIconName(index);
    const int children = index.model()->rowCount(index);
    if (isCollectionSource(index)) subtitle = tr("Whole library, by category");
    else if (isRekordboxDevice(index)) subtitle = rekordboxImported(index) ? tr("USB stick · rekordbox export") : tr("USB stick · rekordbox · tap to read");
    else if (!index.parent().isValid() && icon == QStringLiteral("rekordbox")) subtitle = tr("USB sticks with a rekordbox export");
    else if (children == 1) subtitle = tr("1 item");
    else if (children > 0) subtitle = tr("%1 items").arg(children);
    else if (icon == QStringLiteral("history")) subtitle = tr("Played in earlier sessions");
    else if (icon == QStringLiteral("crates")) subtitle = tr("Crate");
    else subtitle = tr("Track list");
    b.card->showItem(sourceGlyph(index), isCollectionSource(index) ? tr("Library") : index.data().toString(), subtitle, openHint);
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
    const bool tracks = browseTrackLevel(); // r4-browse: Mixes too
    const bool same = playing && previewing && previewing == b.selectedTrack;
    b.preview->setText(same ? tr("Stop") : tr("Preview"));
    b.preview->setGlyph(same ? decklab::Glyph::Stop : decklab::Glyph::Play);
    b.preview->setTone(same ? Tone::Active : Tone::Neutral);
    b.preview->setDetail(playing ? (same ? tr("Playing in headphones") : tr("Another track playing")) : tr("Headphones"));
    b.preview->setEnabled(available && ((tracks && (b.selectedTrack || listTrackRow())) || playing)); // r5-fix3-history
}
void WDeckLab::togglePreview() {
    const ConfigKey load(QStringLiteral("[PreviewDeck1]"), QStringLiteral("LoadSelectedTrackAndPlay"));
    if (!ControlObject::exists(load)) return;
    const bool tracks = browseTrackLevel(); // r4-browse: Mixes too
    // r5-fix2-history: a History or Mixes row still waiting for its table
    // gets it now (a press shows the knob has settled); a track row that
    // cannot load yet never just stops the running preview.
    if (tracks && listRowStale() && !browseLoadableNow()) {
        if (!listTrackRow()) stopPreview();
        return;
    }
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    const bool selected = table && table->selectionModel() && !table->selectionModel()->selectedRows().isEmpty();
    // r7 fix: a file whose stick is out is said, never loaded (the same
    // track playing still pauses).
    auto* previewDeck = m_players && m_players->numberOfPreviewDecks() > 0 ? m_players->getPreviewDeck(0) : nullptr;
    const bool same = value(load.group, QStringLiteral("play")) > 0 && previewDeck && m_browseState->selectedTrack &&
            previewDeck->getLoadedTrack() == m_browseState->selectedTrack;
    if (tracks && selected && !same && selectedFileGone()) return;
    // Same track: Mixxx toggles play/pause; another track: load and play it.
    if (tracks && selected) pulse(load.group, load.item);
    else stopPreview();
}
bool WDeckLab::selectedFileGone() {
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    auto* tracks = table ? dynamic_cast<TrackModel*>(table->model()) : nullptr;
    const QModelIndexList rows = table && table->selectionModel() ? table->selectionModel()->selectedRows() : QModelIndexList();
    if (!tracks || rows.size() != 1) return false;
    // One stat: on a stick that is in it answers from the cache, on one
    // that is out at once.
    const QString location = tracks->getTrackLocation(rows.first());
    if (location.isEmpty() || QFileInfo::exists(location)) return false;
    if (m_browseState && m_browseState->toast) {
        m_browseState->toast->showMessage(decklab::mediaRootOf(location).isEmpty()
                        ? tr("That file is gone.")
                        : tr("That track's USB stick is out."),
                2600, true);
    }
    return true;
}
void WDeckLab::showLoadFailed() {
    if (m_browseState && m_browseState->toast) {
        m_browseState->toast->showMessage(tr("Couldn't load that track. Is its USB stick in?"), 3200, true);
    }
}
void WDeckLab::stopPreview() {
    const ConfigKey play(QStringLiteral("[PreviewDeck1]"), QStringLiteral("play"));
    if (ControlObject::exists(play) && value(play.group, play.item) > 0) set(play.group, play.item, 0);
}
void WDeckLab::scheduleMatchRefresh() {
    if (m_browseState && !m_browseState->matchTimer.isActive()) m_browseState->matchTimer.start();
}
int WDeckLab::matchReferenceDeck() {
    // A deck pinned on the Match bar while it stays loaded; otherwise the same
    // rule as before: the playing (sync leader) deck, else a loaded displayed deck.
    if (m_browseState && m_browseState->pinnedDeck > 0 &&
            value(channel(m_browseState->pinnedDeck), "track_loaded") > 0) {
        return m_browseState->pinnedDeck;
    }
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
    reference.direction = b.direction;
    reference.deck = matchReferenceDeck();
    reference.automatic = !(b.pinnedDeck > 0 && reference.deck == b.pinnedDeck);
    if (reference.deck) {
        reference.key = KeyUtils::keyFromNumericValue(value(channel(reference.deck), "visual_key"));
        reference.bpm = value(channel(reference.deck), "bpm");
    }
    if (b.match) {
        // Green only when there really is a deck to match (B-50).
        QString detail = tr("Off");
        auto tone = DeckLabActionButton::Tone::Neutral;
        if (b.matchEnabled && !reference.deck) {
            detail = tr("Load a deck");
        } else if (b.matchEnabled) {
            QStringList parts{QString::number(reference.deck)};
            const QString key = decklab::formatKey(reference.key); // r6-keys
            if (reference.key != mixxx::track::io::key::INVALID && !key.isEmpty()) parts << key;
            if (reference.bpm > 0) parts << QLocale().toString(reference.bpm, 'f', 1);
            detail = parts.join(QStringLiteral(" · "));
            tone = DeckLabActionButton::Tone::Active;
        }
        b.match->setDetail(detail);
        b.match->setTone(tone);
        b.match->setAccessibleName(!b.matchEnabled ? tr("Matching is off")
                        : reference.deck         ? tr("Match tracks to deck %1: key and tempo").arg(reference.deck)
                                                 : tr("Matching is on; load a deck to match"));
    }
    if (!reference.sameAs(decklab::matchReference())) {
        decklab::setMatchReference(reference);
        // Repaint only: rows evaluate the new reference while painting.
        if (auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr) {
            if (table->isVisible()) table->viewport()->update();
        }
        if (b.card) b.card->refreshMatch();
        if (b.matchOnly) applyMatchFilter(false);
        scheduleMatchCount();
    }
    updateMatchBar();
    // r4-browse (move 5): "Mixes with deck N" is live. Only on Browse; the
    // next visit refreshes it (selectPage → refreshLoadButtons → here).
    if (m_page == 1 && m_browseStack && m_browseStack->currentIndex() == 0 && b.items) {
        const auto mixes = mixReference();
        if (b.list == DeckLabList::Mixes && b.itemsList == DeckLabList::Mixes) {
            if (!mixes.active()) {
                showCategories();
            } else if (!mixes.sameAs(b.mixShown)) {
                fillMixes(true);
                syncMixSelection();
                updateBreadcrumb();
            }
        } else if (b.list == DeckLabList::Categories && b.itemsList == DeckLabList::Categories && !b.loadFailed) {
            const bool shown = b.items->count() && b.items->item(0)->data(DeckLabListDelegate::KeyRole).toString() == QStringLiteral("mixes");
            // The same rule as showCategories(): only once the source is read.
            const bool ready = !b.rekordbox || (b.pendingStick.isEmpty() && b.source.isValid() && rekordboxImported(b.source));
            if (shown != (ready && mixes.active())) showCategories();
            else if (shown && !mixes.sameAs(b.mixShown)) updateMixesRow(); // pitch drift: quarter-BPM steps
        }
    }
}
void WDeckLab::showLoadRefused(int deck) {
    if (!m_browseState || !m_browseState->toast) return;
    // r4-browse (B-33): on Browse the deck's own dock tile refuses in place,
    // and the message sits right above it.
    QRect anchor;
    if (m_page == 1) {
        for (auto* button : m_loadButtons) {
            auto* tile = static_cast<DeckLabDockTile*>(button);
            if (!tile || tile->deck() != deck || !tile->isVisible()) continue;
            tile->showRefusal();
            anchor = QRect(tile->mapTo(this, QPoint()), tile->size());
        }
    }
    m_browseState->toast->showMessage(tr("Deck %1 is on air. Pause it first.").arg(deck), 2600, true, anchor);
}
void WDeckLab::showLoadUnavailable(int slot) {
    if (m_sampleTarget || !m_browseState->toast) return;
    if (!m_fourDeckLayout && slot > 2) return;
    const int target = m_fourDeckLayout ? slot : displayedDeck(slot - 1);
    if (value(channel(target), "play") > 0) { showLoadRefused(target); return; }
    // r4-browse: above the tile that was tapped.
    QWidget* tile = m_loadButtons[slot - 1];
    const QRect anchor = tile && tile->isVisible() ? QRect(tile->mapTo(this, QPoint()), tile->size()) : QRect();
    m_browseState->toast->showMessage(listTrackRow()        ? tr("This track can't be loaded right now") // r5-fix3-history
                    : browseTrackLevel() ? tr("Pick a track first")
                                         : tr("Open a list, then pick a track"),
            2000, false, anchor);
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
    // usb-first: phrases ride on Track objects read from a rekordbox export.
    m_phraseMaps[deck - 1] = decklab::trackPhrases(track.get());
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
        state.key = decklab::formatKey(key); // r6-keys
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
    m_deckTools->fillKey(deck, state); // r5-decks: "8A +1" and the MATCH result
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
    // r4-decks: one decimal, as a CDJ shows (the second one flickered).
    state.bpm = state.loaded && bpm > 0 ? QString::number(bpm, 'f', 1) : QStringLiteral("—");
    const double ratio = d.rateRatio->get();
    double pitch = (ratio - 1) * 100;
    if (!util_isfinite(pitch) || std::abs(pitch) < .005) pitch = 0;
    state.tempoCentered = state.loaded && pitch == 0;
    // r4-decks: Sync can take the tempo beyond the fader range; then the
    // next touch of the tempo fader would jump. The readout says OVER.
    const double rangeValue = d.rateRange->get();
    state.overRange = state.loaded && util_isfinite(ratio) && util_isfinite(rangeValue) && rangeValue > 0 &&
            std::abs(ratio - 1) > rangeValue + 5e-5;
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
    // Beat position from the stored grid: beats counted in fours from the
    // first marker, and how far into the beat.
    const auto beatAt = [this](int which, double where) {
        DeckLabBeatPhase phase;
        const auto& track = m_tracks[which - 1];
        if (!track) return phase;
        if (const auto beats = track->getBeats()) {
            const auto frame = mixxx::audio::FramePos::fromEngineSamplePos(where * m_deck[which - 1].samples->get());
            mixxx::audio::FramePos previous, next;
            if (frame.isValid() && beats->findPrevNextBeats(frame, &previous, &next, false) && next > previous) {
                const long long index = beats->iteratorFrom(previous) - beats->cfirstmarker();
                phase = deckLabBeatPhase(index, (frame - previous) / (next - previous));
            }
        }
        return phase;
    };
    state.phase = -1;
    state.phaseStep = -1;
    state.referenceDeck = 0;
    state.referencePhase = -1;
    state.referenceStep = -1;
    state.referencePlaying = false;
    if (state.loaded && samples > 0) {
        const auto own = beatAt(deck, position);
        state.phase = own.segment;
        state.phaseStep = deckLabBeatStep(own);
        // r4-decks: the phase meter compares with the tempo master (else
        // another playing deck), read at this same moment, never a tick old.
        if (const int reference = referenceDeck(deck); reference > 0 && m_deck[reference - 1].loaded->get() > 0) {
            const double raw = m_deck[reference - 1].position->get();
            const auto other = beatAt(reference, util_isfinite(raw) ? raw : 0);
            state.referenceDeck = reference;
            state.referencePhase = other.segment;
            state.referenceStep = deckLabBeatStep(other);
            state.referencePlaying = m_deck[reference - 1].play->get() > 0;
        }
    }
    // usb-first: current rekordbox phrase and its countdown, and the overview
    // bands' playhead. Both stay empty without phrase data.
    const auto& phrases = state.loaded ? m_phraseMaps[deck - 1] : decklab::PhraseMapPointer();
    state.phrase = deckLabPhraseReadout(phrases.get(), std::clamp(position, 0.0, 1.0) * duration);
    if (auto* bands = m_phraseBands[deck - 1].data()) bands->setPlayback(phrases, duration, position);
}

void WDeckLab::refreshDeckClock(int deck) {
    DeckLabStripState state = m_stripStates[deck - 1];
    fillDeckClock(deck, state);
    if (state == m_stripStates[deck - 1]) return;
    m_stripStates[deck - 1] = state;
    if (m_strips[deck - 1]) m_strips[deck - 1]->setState(state);
    m_deckTools->showState(deck); // r5-decks: the open key sheet / GRID panel
    if (m_minis[deck - 1]) m_minis[deck - 1]->setState(state);
    refreshDockFor(deck); // r4-browse
    if (!m_laneDecks->isHidden()) m_laneDecks->setDeck(deck, state, m_shownPosition[deck - 1]); // r4-waveforms
}

void WDeckLab::applyDeck(int deck) {
    const auto& state = m_stripStates[deck - 1];
    if (auto* strip = m_strips[deck - 1]) {
        strip->setState(state);
        setAccessibleNameIf(strip, state.loaded ? tr("Deck %1 full track information").arg(deck)
                                                : tr("Deck %1 empty; open Browse").arg(deck));
        setAccessibleDescriptionIf(strip, !state.loaded ? tr("Choose a track in Browse; preserves the current browser location")
                        : state.playing ? tr("Playing: pause the deck, or use MORE, to read the full title, artist and source")
                                        : tr("Read the full title, artist and source"));
    }
    if (auto* mini = m_minis[deck - 1]) mini->setState(state);
    m_deckTools->showState(deck); // r5-decks: the open key sheet / GRID panel
    refreshDockFor(deck); // r4-browse
    if (!m_laneDecks->isHidden()) m_laneDecks->setDeck(deck, state, m_shownPosition[deck - 1]); // r4-waveforms
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
        // r6-fxstrip: a route to the main mix, headphones, a sampler... is
        // wet too.
        const QStringList others = offDeckRoutes(chain);
        // A real controller step below one percent is still wet: never DRY.
        const bool on = effectOn && value(group, "enabled") > 0 && mix > 0 && (!targets.isEmpty() || !others.isEmpty());
        const QString state = on ? (mixPercent > 0 ? tr("%1%").arg(mixPercent) : tr("<1%")) : tr("DRY");
        // r6-fxstrip: the 2-deck layout shows units 1 and 2 (a two-deck
        // controller's FX sections), 3 and 4 only while wet: nothing sounding
        // is hidden. A lamp reads the deck its unit feeds (after a layer flip
        // the left lamp reads 3), else the unit. The width stays as built.
        if (!m_fourDeckLayout && unit > 2 && !on) continue;
        QStringList places;
        if (!targets.isEmpty()) places << (targets.size() == 1 ? tr("deck %1") : tr("decks %1")).arg(route);
        places << others;
        const QString where = places.isEmpty() ? tr("FX unit %1 on no deck").arg(unit)
                                               : tr("FX unit %1 on %2").arg(unit).arg(places.join(QStringLiteral(", ")));
        chips.append(DeckLabChipButton::Chip{targets.size() == 1 ? route : QString::number(unit), kFxLamp, on});
        if (on) active << where;
        details << tr("%1; %2; %3").arg(where, name, state);
    }
    m_fxChips->setChips(chips);
    setToolTipIf(m_fxChips, details.join(QLatin1Char('\n')));
    setAccessibleDescriptionIf(m_fxChips, active.isEmpty() ? tr("No controller effect unit is wet")
                                                           : tr("Wet: %1").arg(active.join(QStringLiteral(", "))));
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
        // r6-fxstrip: the other routes (main, headphones, samplers...) too.
        for (const auto& input : m_effects->registeredInputChannels())
            if (!isDeckRoute(input.name())) watch(group, QStringLiteral("group_%1_enable").arg(input.name()), dirty);
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
    if ((m_page == 0 || m_page == 1) && m_fourDeckLayout) return true; // r4-browse: the dock shows all four
    if (m_laneDecks && !m_laneDecks->isHidden()) return m_laneDecks->shows(deck); // r4-waveforms: the DECKS row
    return deck == m_layers[0] || deck == m_layers[1];
}

void WDeckLab::tickDecks() {
    const qint64 now = m_clock.elapsed();
    connectControllerFx();
    // r4-labfx: the Lab FX tab is lit while an effect plays (on any page)
    // and blinks with the beat while its tail rings out.
    if (m_tabs[3]) setStylePropertyIf(m_tabs[3], "live", m_labFx && m_labFx->tabLit());
    if (m_overloadUntil && now >= m_overloadUntil) {
        m_overloadUntil = 0;
        updateStatus();
    }
    if (m_sampler && m_page == 2 && isVisible()) tickSampler(); // r4-sampler
    // r4-browse: on Browse the dock's clocks tick instead of the lanes'.
    if (!isVisible() || (!m_waveforms->isVisible() && m_page != 1)) return;
    // Only clocks advance on the tick: playing decks, or a paused deck whose
    // position was moved (cue, needle search or scratch). r4-decks: while
    // any deck plays, a paused deck's phase meter follows the reference's
    // beat (cueing a drop on the one); unchanged parts never repaint.
    const bool flash = (now / 400) % 2 == 0;
    if (!m_laneDecks->isHidden()) m_laneDecks->setFlash(flash); // r4-waveforms: only end-warning cells repaint
    bool anyPlaying = false;
    for (const auto& d : m_deck) anyPlaying |= d.play->get() > 0 && d.loaded->get() > 0;
    for (int deck = 1; deck <= 4; ++deck) {
        if (!deckShown(deck)) continue;
        const auto& d = m_deck[deck - 1];
        if (d.loaded->get() <= 0) continue;
        const double rawPosition = d.position->get();
        const double position = util_isfinite(rawPosition) ? rawPosition : 0;
        if (d.play->get() > 0 || position != m_shownPosition[deck - 1] || anyPlaying) refreshDeckClock(deck);
        if (m_stripStates[deck - 1].endWarning) {
            if (m_strips[deck - 1]) m_strips[deck - 1]->setFlash(flash);
            if (m_minis[deck - 1]) m_minis[deck - 1]->setFlash(flash);
            if (m_page == 1) {
                for (auto* tile : m_loadButtons) {
                    if (tile && static_cast<DeckLabDockTile*>(tile)->deck() == deck) static_cast<DeckLabDockTile*>(tile)->setFlash(flash);
                }
            }
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
    refreshPads(deck, card); // r4-decks: the pad block of this side or card
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
    state.slip = d.slip->get() > 0;
    state.synced = state.loaded && d.sync->get() > 0;
    // r6-sync: MASTER and what INST DOUBLES would copy (a few control reads).
    state.master = state.loaded && d.leader->get() > 0;
    state.canMaster = m_sync->canMaster(deck);
    state.doubles = m_sync->doublesSource(deck);
    state.size = d.loopSize->get();
    // Cached buttons and an unchanged state: no lookups and no widget writes.
    if (state == t.shown) return;
    t.shown = state;
    if (t.more)
        setAccessibleNameIf(t.more, (card ? tr("Deck %1 more: pads, master, instant doubles, quantize, key lock, slip, track information, grid and zoom")
                                          : tr("Deck %1 more: play, cue, master, instant doubles, quantize, key lock, slip, track information, grid and zoom")).arg(deck));
    for (auto* b : {t.play, t.cue, t.halve, t.loop, t.twice, t.quantize, t.keylock, t.slip}) setEnabledIf(b, state.loaded);
    // r6-sync: an empty deck's SYNC still holds for INST DOUBLES.
    setEnabledIf(t.sync, state.loaded || state.doubles > 0);
    if (t.play) {
        // A fixed play/pause glyph; the green lamp says it is playing.
        setCheckedIf(t.play, state.playing);
        setAccessibleNameIf(t.play, tr("Deck %1 play/pause, %2").arg(deck).arg(state.playing ? tr("playing") : tr("paused")));
    }
    if (t.cue) setAccessibleNameIf(t.cue, tr("Deck %1 %2").arg(deck).arg(t.cue->text()));
    if (t.loop) {
        const QString sizeText = beatLoopSizeText(state.size);
        setCheckedIf(t.loop, state.looping); setTextIf(t.loop, tr("LOOP\n%1").arg(sizeText));
        setAccessibleNameIf(t.loop, tr("Deck %1 loop %2 beats, %3").arg(deck).arg(sizeText, state.looping ? tr("on") : tr("off")));
        setAccessibleDescriptionIf(t.loop, tr("Toggle the selected beat loop; half and double change its beat length"));
    }
    if (t.capture) { // r7-sampler: TO PAD needs an active loop
        setEnabledIf(t.capture, state.loaded && state.looping);
        setAccessibleNameIf(t.capture, state.loaded && state.looping ? tr("Save deck %1's loop to a sampler pad").arg(deck)
                                                                     : tr("Save a loop to a pad: set a loop on deck %1 first").arg(deck));
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
    if (t.slip) {
        setCheckedIf(t.slip, state.slip);
        setAccessibleNameIf(t.slip, tr("Deck %1 slip %2").arg(deck).arg(state.slip ? tr("on") : tr("off")));
    }
    if (t.sync) {
        // r6-sync: BEAT SYNC. Tap: on or off, lit filled while on. Hold on a
        // stopped deck: INST DOUBLES.
        setStylePropertyIf(t.sync, "lit", state.synced);
        setAccessibleNameIf(t.sync, tr("Deck %1 sync, %2").arg(deck).arg(state.synced
                        ? (state.master ? tr("on, tempo master") : tr("on")) : tr("off")));
        setAccessibleDescriptionIf(t.sync, state.doubles > 0
                        ? tr("Tap to turn beat sync on or off. Hold for instant doubles of deck %1.").arg(state.doubles)
                        : tr("Tap to turn beat sync on or off. Hold on a stopped deck for instant doubles."));
    }
    if (t.master) {
        setCheckedIf(t.master, state.master);
        setEnabledIf(t.master, state.canMaster);
        setAccessibleNameIf(t.master, state.master ? tr("Deck %1 is the tempo master; hand it on").arg(deck)
                        : state.canMaster ? tr("Make deck %1 the tempo master").arg(deck)
                                          : tr("Deck %1 can be the tempo master while it plays").arg(deck));
    }
    if (t.doubles) {
        setEnabledIf(t.doubles, state.doubles > 0);
        setAccessibleNameIf(t.doubles, state.doubles > 0
                        ? tr("Instant doubles: deck %1's track on deck %2, same position").arg(state.doubles).arg(deck)
                        : state.playing ? tr("Instant doubles: deck %1 is playing").arg(deck)
                                        : tr("Instant doubles: no tempo master or playing deck to copy"));
    }
}

// r4-labfx: the Lab FX state lives in DeckLabLabFx; these keep the surface's
// entry points (deck and page changes, the 100 ms tick, release requests).
void WDeckLab::refreshFx() {
    if (m_labFx) m_labFx->refresh();
}
void WDeckLab::endHolds() {
    if (m_labFx) m_labFx->endHolds();
}
void WDeckLab::releaseEffects(bool musical, bool mute) {
    if (!m_labFx) return;
    if (mute) m_labFx->echoOut();
    else if (musical) m_labFx->release();
    else m_labFx->cut();
}
void WDeckLab::tickEffects() {
    qint64 now = m_clock.elapsed();
    // The legacy companion heartbeat lapses; decklabd keeps tools online.
    if (m_toolsOnline && now - m_lastToolsReady > 2000 && !(m_systemUi && m_systemUi->toolsConnected())) {
        m_toolsOnline = false; for (auto* b : m_toolButtons) b->setEnabled(false); updateStatus();
    }
    tickDecks();
    if (m_page == 1) updateBreadcrumb();
    // Heartbeat, tempo and beat phase for a wet unit, on every page.
    if (m_labFx) m_labFx->tick();
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
    // r6-keys: 0 Lancelot, 1 Open Key, 2 Musical ([Key] KeyNotation keeps
    // Mixxx's names). decklab/decklabkeys.h formats every key on screen.
    const auto format = static_cast<decklab::KeyFormat>(std::clamp(index, 0, 2));
    m_config->setValue(ConfigKey("[Key]", "KeyNotation"), decklab::keyFormatConfigValue(format));
    decklab::applyKeyFormat(format);
    refreshKeyText();
}
// r6-keys: everything that shows a key takes the new format now. Rows and
// cards format at paint time, so they only repaint; texts built ahead (the
// Match bar, the Mixes row, key values, a key's crumb) are rebuilt.
void WDeckLab::refreshKeyText() {
    for (int deck = 1; deck <= 4; ++deck) refreshDeck(deck);
    if (!m_browseState) return;
    auto& b = *m_browseState;
    if (b.items && b.list == DeckLabList::Values && b.category == decklab::Category::Key) decklab::relabelKeyValues(*b.items);
    if (b.filter.startsWith(QStringLiteral("key:")) && b.category == decklab::Category::Key) {
        const auto key = decklab::parseKey(b.filter.mid(4));
        if (key != mixxx::track::io::key::INVALID) b.valueLabel = decklab::formatKey(key);
        updateBreadcrumb();
    }
    refreshMatchReference();
    updateMatchBar();
    updateMixesRow();
    if (b.items) b.items->viewport()->update();
    if (b.card && b.card->isVisible()) refreshBrowseCard();
    // Mixxx's tables repaint on [Library],key_notation; one sorted by key
    // sorts again round the new wheel.
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    auto* tracks = table ? dynamic_cast<TrackModel*>(table->model()) : nullptr;
    if (tracks && table->horizontalHeader()->sortIndicatorSection() ==
                    tracks->columnIndexFromSortColumnId(TrackModel::SortColumnId::Key)) {
        table->sortByColumn(table->horizontalHeader()->sortIndicatorSection(), table->horizontalHeader()->sortIndicatorOrder());
    }
}
void WDeckLab::setAudioOutputConfigured(bool configured) {
    m_outputConfigured = configured;
    if (m_systemUi) m_systemUi->setAudioOutputConfigured(configured);
    updateStatus();
}
void WDeckLab::attachSystem(DeckLabSystem* system) {
    m_system = system;
    if (m_systemUi) m_systemUi->attach(system);
    historyAttachSystem(system); // r5-history
    if (system) {
        // r7: PUT ON PAD leaves the pad a Load pad trip is filling alone.
        const QPointer<WDeckLab> self(this);
        system->setPadTarget([self] { return self ? self->m_sampleTarget : 0; });
        // r7: a stick being ejected is not held by the background analysis.
        system->setDriveRelease([self](const QString& mount) {
            return self && self->m_analysis && self->m_analysis->letGo(mount);
        });
        // r7 fix: Get Music or Sample Maker closed: the DOWNLOADS and
        // SAMPLES tiles count their folders again.
        connect(system->utilities(), &DeckLabUtilities::closed, this, [this] { markHomeStale(); });
    }
}
void WDeckLab::updateStatus() {
    int overloads = int(value("[App]", "audio_latency_overload_count"));
    // [Recording] status: 2 recording, 3 recording into the next split file.
    const int recordingStatus = int(value("[Recording]", "status"));
    const bool recording = recordingStatus == 2 || recordingStatus == 3;
    const bool audioDown = !m_outputConfigured || value("[SoundManager]", "status") != 2;
    // DROPOUTS lights for ten seconds after the counter rises, so it reports
    // fresh dropouts instead of staying lit for the rest of the session.
    if (overloads > m_lastOverloads) m_overloadUntil = m_clock.elapsed() + 10000;
    m_lastOverloads = overloads;
    const bool recentOverload = m_overloadUntil != 0 && m_clock.elapsed() < m_overloadUntil;
    // r4-system: the rail's inputs. The system half (service, USB, Wi-Fi,
    // controller, heat) is cached by the system UI and changes only on events.
    decklab::rail::Inputs inputs;
    if (recording && m_recordingSince < 0) m_recordingSince = m_clock.elapsed();
    if (!recording) m_recordingSince = -1;
    if (recording != m_recordTick->isActive()) { if (recording) m_recordTick->start(); else m_recordTick->stop(); }
    inputs.recording = recording;
    inputs.recordingSeconds = recording ? (m_clock.elapsed() - m_recordingSince) / 1000 : 0;
    inputs.recentDropouts = recentOverload;
    inputs.dropouts = overloads;
    // Start-up opens the sound devices after the skin: don't flash NO OUTPUT.
    inputs.outputWorking = !audioDown;
    if (audioDown && m_clock.elapsed() < 1500) {
        inputs.outputPending = true;
        QTimer::singleShot(1600 - int(m_clock.elapsed()), this, [this] { updateStatus(); });
    }
    if (m_systemUi) m_systemUi->fillRailInputs(inputs);
    if (!inputs.serviceOnline) {
        // Without the system service the USB lamp still knows the sticks.
        if (m_usbScannedAt < 0 || m_clock.elapsed() - m_usbScannedAt > 5000) {
            m_usbScannedAt = m_clock.elapsed();
            m_usbSticksSeen = int(decklab::findUsbSticks().size());
        }
        inputs.usbDrives = m_usbSticksSeen;
    }
    // One plain line per state, for the tool tip and accessibility.
    QStringList lines;
    if (recording) lines.append(tr("Recording your mix."));
    if (audioDown && !inputs.outputPending) lines.append(tr("No sound output. Tap Fix to choose one in Deck Setup."));
    if (!m_toolsOnline) lines.append(tr("System service offline. DJing is unaffected."));
    if (overloads) lines.append(tr("Audio dropouts: %1").arg(overloads));
    const QString text = lines.join(QLatin1Char('\n'));
    // Called for every waveform measurement too: unchanged status must not
    // repaint or relayout. The zone keeps its width whatever is lit.
    if (m_railUi) m_railUi->setInputs(inputs);
    setToolTipIf(m_status, text);
    // The gear's lamp is the unit's health (hot, low power, service offline).
    if (auto* gear = static_cast<DeckLabRailTab*>(m_tabs[4]))
        gear->setLamp(inputs.undervoltage || inputs.heat == QStringLiteral("hot") ? decklab::ui::Lamp::Caution
                        : inputs.serviceAttached && !inputs.serviceOnline           ? decklab::ui::Lamp::Caution
                                                                                     : decklab::ui::Lamp::Off);
    if (m_settings) m_settings->setHealth(m_measuredFps, m_droppedFrames, overloads);
}
bool WDeckLab::eventFilter(QObject* watched, QEvent* event) {
    // r4-waveforms: compact lanes (Sampler, Lab FX) are display only: a
    // press or wheel there never starts a scratch or a zoom while the hands
    // are on the pads. Releases always pass, so a scratch begun on Decks ends.
    if (m_page != 0 && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick ||
                event->type() == QEvent::Wheel) && watched->isWidgetType()) {
        for (const auto& viewer : m_detailWaveforms)
            if (viewer && (watched == viewer || viewer->isAncestorOf(static_cast<QWidget*>(watched)))) {
                // Still a touch somewhere else for a Lab FX pad hold.
                if (m_labFx && event->type() == QEvent::MouseButtonPress) m_labFx->noteTouch(watched, event);
                return true;
            }
    }
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
            event->type() == QEvent::TouchCancel) {
        ++m_interactionEpoch; releaseTouchCue();
        // r4-labfx: a cancelled touch only ends a pad hold; leaving the
        // application lets Lab FX ring out instead of cutting it (PF-X24).
        if (event->type() == QEvent::TouchCancel) endHolds(); else releaseEffects(true);
    }
    // qw-library (C-01): the kiosk has no window manager to re-activate Mixxx
    // after a menu, dialog or utility window closes. Do it here.
    if ((event->type() == QEvent::Hide || event->type() == QEvent::Show) && watched != this && watched->isWidgetType()) {
        auto* widget = static_cast<QWidget*>(watched);
        const Qt::WindowType type = widget->windowType();
        if (widget->isWindow() && (type == Qt::Popup || type == Qt::Dialog || type == Qt::Sheet || type == Qt::Tool)) {
            if (event->type() == QEvent::Hide) scheduleFocusReclaim();
            scheduleKnobLegend(); // r4-browse: the legend says MENU while it owns the knob
            decklab::noteKnobOwnerChanged(); // r5-decks: key sheet cursor and GRID lamps follow the owner
        }
    } else if (event->type() == QEvent::ApplicationStateChange && watched == qApp &&
            QGuiApplication::applicationState() == Qt::ApplicationActive) {
        scheduleFocusReclaim();
    }
    // r4-labfx: lost-touch safety for a Lab FX pad hold, instead of a time
    // limit (DeckLabLabFx::noteTouch).
    if (m_labFx && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease ||
            event->type() == QEvent::TouchEnd)) m_labFx->noteTouch(watched, event);
    return QWidget::eventFilter(watched, event);
}
// r4-labfx: a hidden surface lets Lab FX ring out (the tick keeps running).
void WDeckLab::hideEvent(QHideEvent* event) { ++m_interactionEpoch; releaseTouchCue(); releaseEffects(true); QWidget::hideEvent(event); }

void WDeckLab::releaseTouchCue() {
    if (!m_touchCue.isEmpty()) { set(m_touchCue, "cue_default", 0); m_touchCue.clear(); }
}

void WDeckLab::setSearchOpen(bool open) {
    if (!m_keyboard || !m_searchPanel) return;
    if (open && m_browseState && m_browseStack->currentIndex() == 0) {
        // Search where you are (B-35): the stick, folder or source you are
        // in; the whole library only from the source list itself.
        auto& b = *m_browseState;
        const bool inSource = b.source.isValid() &&
                (b.list == DeckLabList::Categories || b.list == DeckLabList::Values || b.list == DeckLabList::Mixes ||
                        (b.list == DeckLabList::Sources && b.returnRoot.isValid() && m_sidebar &&
                                m_sidebar->rootIndex() == b.returnRoot));
        if (b.list == DeckLabList::Folder) {
            openFolderTracks();
        } else if (b.list == DeckLabList::Downloads) {
            // r7-browse: search every download, newest first.
            showFolderTracks(decklab::FolderList::AllDownloads,
                    decklab::allDownloads(currentDownloadFolders()), QStringLiteral("downloads:all"), QString());
        } else if (inSource && b.rekordbox && !rekordboxImported(b.source)) {
            if (b.toast) b.toast->showMessage(tr("Still reading %1…").arg(currentSourceName()), 2000);
            return;
        } else if (inSource) {
            clearBrowseFilter();
            activateBrowseSource();
            b.category = decklab::Category::AllTracks;
            b.fromCategories = true;
            showTracks();
        } else if (b.list == DeckLabList::Categories && !b.pendingStick.isEmpty()) {
            if (b.toast) b.toast->showMessage(tr("Still reading %1…").arg(currentSourceName()), 2000);
            return;
        } else if (m_library) {
            resetBrowseLevels();
            m_library->searchTracksInCollection(QString()); showTracks();
        }
    }
    m_keyboard->setVisible(open);
    m_searchPanel->setVisible(open || (m_search && !m_search->currentText().isEmpty()));
    // r4-waveforms: Browse shows no lanes today. Should presentationFor()
    // give it compact lanes, the keyboard takes their room while it is up
    // (it never floats over them).
    if (m_page == 1 && decklab::lanes::presentationFor(1) == decklab::lanes::Presentation::Compact)
        setVisibleIf(m_waveforms, !open);
    // The keyboard leaves little height: keep Preview and LOAD, drop the card.
    if (m_browseState && m_browseState->card) m_browseState->card->setVisible(!open);
    // r4-browse: the deck dock moves into the pane above the keyboard (the
    // list keeps its rows), and the knob legend steps aside.
    if (m_browseState && m_browseState->dock) {
        auto& b = *m_browseState;
        if (open && b.dock->arrangement() != DeckLabDeckDock::Arrangement::Pane) {
            b.paneLayout->addWidget(b.dock);
            b.dock->setArrangement(DeckLabDeckDock::Arrangement::Pane);
        } else if (!open && b.dock->arrangement() != DeckLabDeckDock::Arrangement::Bar) {
            b.pageLayout->addWidget(b.dock);
            b.dock->setArrangement(DeckLabDeckDock::Arrangement::Bar);
        }
        // round4: the LOAD targets never vanish. Re-parenting hides the dock
        // until the next event-loop pass, and the pane it moves into is
        // hidden on the source home, so show both now.
        setVisibleIf(b.pane, open || !(m_browseStack->currentIndex() == 0 && b.list == DeckLabList::Home));
        b.dock->show();
        setVisibleIf(b.legend, !open);
    }
    if (open && m_search && m_search->lineEdit()) m_search->lineEdit()->setFocus(Qt::OtherFocusReason);
    updateMatchBar();
    if (m_browseState) updateBreadcrumb();
}

// qw-library: the source home (B-01..B-06, U-03). Sticks first by label, then
// the library's own sources in sidebar order, desktop roots under "More".
void WDeckLab::showHome(const QString& highlight) {
    auto& b = *m_browseState;
    resetBrowseLevels();
    if (!highlight.isEmpty()) b.homeKey = highlight;
    if (m_sidebar) m_sidebar->setRootIndex(QModelIndex());
    rebuildHome(b.homeKey);
    showSources();
}
namespace {
QString stickSignature(const QList<decklab::UsbStick>& sticks) {
    QStringList parts;
    for (const auto& stick : sticks) parts << stick.path + (stick.rekordbox ? QStringLiteral("*") : QString());
    return parts.join(QLatin1Char('|'));
}
} // namespace
void WDeckLab::rebuildHome(const QString& highlight) {
    auto& b = *m_browseState;
    if (!m_sidebar || !b.items) return;
    // r4-browse (move 1): a 2x2 grid of source tiles, then the "Also" chips.
    // Sticks come first (a "plug one in" tile when none is in), then the
    // library's sources in sidebar order, then More sources.
    const auto sticks = decklab::findUsbSticks();
    b.homeSignature = homeStateSignature(sticks);
    b.homeStale = false;
    QVector<decklab::SourceEntry> stickEntries;
    for (const auto& stick : sticks) {
        decklab::SourceEntry entry;
        entry.key = QStringLiteral("usb:") + stick.path;
        entry.name = stick.label;
        entry.state = stickState(stick); // first: a late read clears its failure
        entry.line1 = stickSubtitle(stick);
        entry.line2 = stickDetail(stick);
        entry.glyph = decklab::Glyph::Usb;
        entry.readOnly = QStorageInfo(stick.path).isReadOnly();
        stickEntries.append(entry);
    }
    const QAbstractItemModel* model = m_sidebar->model();
    QVector<decklab::SourceEntry> sources;
    QStringList hidden;
    for (int row = 0; model && row < model->rowCount(); ++row) {
        const QModelIndex root = model->index(row, 0);
        const QString icon = root.data(SidebarModel::IconNameRole).toString();
        if (hiddenFromHome(icon)) {
            hidden << (icon == QStringLiteral("autodj") ? tr("Auto DJ")
                            : icon == QStringLiteral("prepare") ? tr("Analyze")
                            : icon == QStringLiteral("computer") ? tr("Computer")
                                                                 : tr("All USB exports"));
            continue;
        }
        decklab::SourceEntry entry;
        entry.key = homeRootKey(root);
        entry.name = icon == QStringLiteral("tracks") ? tr("Library") : root.data().toString();
        entry.line1 = homeRootSubtitle(root);
        const int children = model->rowCount(root);
        if (icon == QStringLiteral("tracks")) {
            entry.line2 = tr("By artist, album, BPM and key");
        } else {
            // What is inside, by name: the first few playlists or crates.
            QStringList names;
            for (int child = 0; child < std::min(3, children); ++child) names << model->index(child, 0, root).data().toString();
            entry.line2 = names.join(QStringLiteral(" · "));
        }
        if (children > 0) entry.count = QLocale().toString(children);
        entry.glyph = sourceGlyph(root);
        if (icon == QStringLiteral("history")) decorateHistoryEntry(entry); // r5-history
        sources.append(entry);
    }
    decklab::SourceEntry more;
    if (!hidden.isEmpty()) {
        more.key = QStringLiteral("more");
        more.name = tr("More sources");
        more.line1 = hidden.join(QStringLiteral(" · "));
        more.line2 = tr("Everything else this unit can browse");
        more.glyph = decklab::Glyph::Sources;
    }
    // r7-browse: DOWNLOADS (Get Music's folders on plugged-in sticks and the
    // Pi card) and SAMPLES follow the Library, once their folders exist.
    QVector<decklab::SourceEntry> extras;
    QStringList stickPaths;
    for (const auto& stick : sticks) stickPaths << stick.path;
    const QStringList downloads = decklab::downloadFolders(stickPaths);
    if (!downloads.isEmpty()) extras.append(downloadsEntry(downloads));
    if (QFileInfo(decklab::samplesFolder()).isDir()) extras.append(samplesEntry());
    const decklab::HomeLayout layout = decklab::arrangeHome(stickEntries, sources, more, extras);
    QString keep = highlight;
    if (keep.isEmpty() && b.itemsList == DeckLabList::Home && b.items->currentItem()) {
        keep = b.items->currentItem()->data(DeckLabListDelegate::KeyRole).toString();
    }
    {
        const QSignalBlocker blocker(b.items);
        b.items->clear();
        int current = -1;
        const auto add = [&](const decklab::SourceEntry& entry, int tile, bool lead) {
            auto* item = new QListWidgetItem(entry.name, b.items);
            item->setData(DeckLabListDelegate::KeyRole, entry.key);
            item->setData(DeckLabListDelegate::SubtitleRole, entry.line1);
            item->setData(DeckLabListDelegate::DetailRole, entry.line2);
            item->setData(DeckLabListDelegate::GlyphRole, static_cast<int>(entry.glyph));
            item->setData(DeckLabListDelegate::StateRole, static_cast<int>(entry.state));
            item->setData(DeckLabListDelegate::ReadOnlyRole, entry.readOnly);
            item->setData(DeckLabListDelegate::TileRole, tile);
            item->setData(DeckLabListDelegate::AlsoLeadRole, lead);
            item->setData(DeckLabListDelegate::ChevronRole, true);
            if (tile == 2 && !entry.count.isEmpty()) item->setData(DeckLabListDelegate::CountRole, entry.count);
            QStringList spoken{entry.name, entry.line1};
            if (!decklab::sourceStateText(entry.state).isEmpty()) spoken << decklab::sourceStateText(entry.state);
            if (entry.readOnly) spoken << tr("read-only");
            item->setData(Qt::AccessibleTextRole, spoken.join(QStringLiteral(", ")));
            if (!entry.enabled) item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
            if (entry.enabled && entry.key == keep) current = b.items->count() - 1;
        };
        for (const auto& entry : layout.tiles) add(entry, 1, false);
        for (int n = 0; n < layout.also.size(); ++n) add(layout.also[n], 2, n == 0);
        if (current < 0) {
            current = decklab::stepToEnabledRow(b.items->count(), -1, 1, [&b](int row) {
                return b.items->item(row)->flags().testFlag(Qt::ItemIsEnabled);
            });
        }
        b.items->setCurrentRow(current);
    }
    b.itemsList = DeckLabList::Home;
}
void WDeckLab::refreshHomeIfSticksChanged() {
    auto& b = *m_browseState;
    if (m_page != 1 || b.list != DeckLabList::Home || m_browseStack->currentIndex() != 0) {
        b.homeTimer.stop();
        return;
    }
    // r4-browse: a stick read that finished, or that never will (B-10), while
    // the home is shown: its tile changes state.
    const qint64 now = m_clock.elapsed();
    const int timeout = std::max(100, m_config->getValue(ConfigKey("[DeckLab]", "RekordboxReadTimeoutMs"), 20000));
    for (auto it = b.readingSticks.begin(); it != b.readingSticks.end();) {
        const QModelIndex device = rekordboxDeviceFor(m_sidebar->model(), it.key());
        if (device.isValid() && rekordboxImported(device)) {
            it = b.readingSticks.erase(it);
        } else if (now - it.value() >= timeout) {
            b.failedSticks.insert(it.key());
            it = b.readingSticks.erase(it);
        } else {
            ++it;
        }
    }
    if (!b.homeStale && homeStateSignature(decklab::findUsbSticks()) == b.homeSignature) return;
    rebuildHome(QString());
    refreshBrowseCard();
}
void WDeckLab::markHomeStale() {
    if (!m_browseState) return;
    m_browseState->homeStale = true;
    m_browseState->libraryCountAt = -100000; // the Library tile counts again
    if (m_page == 1 && m_browseState->list == DeckLabList::Home && m_browseState->itemsList == DeckLabList::Home &&
            m_browseStack && m_browseStack->currentIndex() == 0) {
        refreshHomeIfSticksChanged();
    }
}
QString WDeckLab::homeRootSubtitle(const QModelIndex& root) {
    const QString icon = root.data(SidebarModel::IconNameRole).toString();
    const int children = root.model() ? root.model()->rowCount(root) : 0;
    if (icon == QStringLiteral("tracks")) {
        const int count = libraryTrackCount();
        return count >= 0 ? countText(count, tr("1 track on this unit"), tr("%1 tracks on this unit"))
                          : tr("All your music, by artist, BPM and key");
    }
    if (icon == QStringLiteral("playlist")) {
        return children ? countText(children, tr("1 playlist"), tr("%1 playlists")) : tr("No playlists yet");
    }
    if (icon == QStringLiteral("crates")) {
        return children ? countText(children, tr("1 crate"), tr("%1 crates")) : tr("No crates yet");
    }
    if (icon == QStringLiteral("history")) return tr("Played in earlier sessions");
    if (icon == QStringLiteral("recordings")) return tr("Mixes recorded on this unit");
    return children ? countText(children, tr("1 item"), tr("%1 items")) : tr("Tracks");
}
QString WDeckLab::stickSubtitle(const decklab::UsbStick& stick) {
    auto& b = *m_browseState;
    if (stick.rekordbox) {
        if (b.failedSticks.contains(stick.path)) return tr("rekordbox · couldn't read the export");
        const QModelIndex device = m_sidebar ? rekordboxDeviceFor(m_sidebar->model(), stick.path) : QModelIndex();
        if (device.isValid() && rekordboxImported(device)) {
            QStringList parts{tr("rekordbox")};
            const int tracks = b.stickTracks.value(stick.path, -1);
            if (tracks >= 0) parts << countText(tracks, tr("1 track"), tr("%1 tracks"));
            const int playlists = m_sidebar->model()->rowCount(device);
            if (playlists > 0) parts << countText(playlists, tr("1 playlist"), tr("%1 playlists"));
            return parts.join(QStringLiteral(" · "));
        }
        return tr("rekordbox · tap to read");
    }
    // A plain stick: what is at its top level (one directory read).
    const int folders = static_cast<int>(decklab::stickSubfolders(stick.path).size());
    const int files = static_cast<int>(decklab::stickAudioFiles(stick.path).size());
    QStringList parts{tr("Files")};
    if (folders) parts << countText(folders, tr("1 folder"), tr("%1 folders"));
    if (files) parts << countText(files, tr("1 track"), tr("%1 tracks"));
    if (!folders && !files) parts << tr("no music found");
    return parts.join(QStringLiteral(" · "));
}
int WDeckLab::libraryTrackCount() {
    auto& b = *m_browseState;
    if (!m_library || !m_library->trackCollectionManager()) return -1;
    if (m_clock.elapsed() - b.libraryCountAt < 10000) return b.libraryCount;
    b.libraryCountAt = m_clock.elapsed();
    // r7-library: the tracks Tracks lists (not missing, not on a USB stick
    // that is out). r7 fix: nor samples, which the song lists leave out.
    b.libraryCount = decklab::shownSongCount(m_library->trackCollectionManager()->internalCollection()->database());
    return b.libraryCount;
}
void WDeckLab::openHomeRow(int rowIndex) {
    auto& b = *m_browseState;
    auto* item = b.items->item(rowIndex);
    if (!item || !(item->flags() & Qt::ItemIsEnabled) || !m_sidebar) return;
    const QString key = item->data(DeckLabListDelegate::KeyRole).toString();
    b.homeKey = key;
    if (key == QStringLiteral("root:history")) { showHistory(false); return; } // r5-history
    if (key == QStringLiteral("downloads")) { showDownloads(QString()); return; } // r7-browse
    if (key == QStringLiteral("samples")) { openSamples(false); return; }
    if (key.startsWith(QStringLiteral("usb:"))) {
        openStick(key.mid(4));
        return;
    }
    if (key == QStringLiteral("more")) {
        b.fromMore = true;
        b.list = DeckLabList::Sources;
        m_sidebar->setRootIndex(QModelIndex());
        m_sidebar->setCurrentIndex(m_sidebar->model()->index(0, 0));
        showSources();
        return;
    }
    QModelIndex root;
    for (int row = 0; row < m_sidebar->model()->rowCount(); ++row) {
        const QModelIndex candidate = m_sidebar->model()->index(row, 0);
        if (homeRootKey(candidate) == key) { root = candidate; break; }
    }
    if (!root.isValid()) { rebuildHome(QString()); showSources(); return; }
    b.fromMore = false;
    b.list = DeckLabList::Sources;
    m_sidebar->setRootIndex(QModelIndex());
    m_sidebar->setCurrentIndex(root);
    openSource();
}
void WDeckLab::openStick(const QString& path) {
    auto& b = *m_browseState;
    decklab::UsbStick stick;
    bool found = false;
    for (const auto& candidate : decklab::findUsbSticks()) {
        if (candidate.path == path) { stick = candidate; found = true; }
    }
    if (!found) {
        if (b.toast) b.toast->showMessage(tr("That USB stick is not plugged in any more"), 2600);
        showHome(QString());
        return;
    }
    resetBrowseLevels();
    b.homeKey = QStringLiteral("usb:") + path;
    b.stickPath = path;
    b.stickName = stick.label;
    if (!stick.rekordbox) { showFolder(path); return; }
    auto* model = qobject_cast<SidebarModel*>(m_sidebar->model());
    const QModelIndex device = rekordboxDeviceFor(m_sidebar->model(), path);
    // r4-browse: opening a stick that isn't read yet starts a new read.
    if (!(device.isValid() && rekordboxImported(device))) {
        b.failedSticks.remove(path);
        b.readingSticks.insert(path, m_clock.elapsed());
    }
    if (device.isValid()) {
        b.list = DeckLabList::Sources;
        m_sidebar->setRootIndex(QModelIndex());
        m_sidebar->setCurrentIndex(device);
        openSource(); // its categories; starts reading the export
        return;
    }
    const QModelIndex root = sidebarRoot(m_sidebar->model(), QStringLiteral("rekordbox"));
    if (!model || !root.isValid()) { showFolder(path); return; }
    // The rekordbox feature lists sticks after a scan: start one, and read
    // this stick as soon as its row appears (checkRekordboxImport).
    model->clicked(root);
    b.rekordbox = true;
    b.pendingStick = path;
    b.loadingSince = m_clock.elapsed();
    b.trackCount = -1;
    b.categoryRow = 0;
    showCategories();
}
void WDeckLab::showFolder(const QString& path) {
    auto& b = *m_browseState;
    clearBrowseFilter();
    b.loadingTimer.stop();
    b.list = DeckLabList::Folder;
    b.folderPath = QDir::cleanPath(path);
    b.source = QPersistentModelIndex();
    b.returnRoot = QPersistentModelIndex();
    b.rekordbox = false;
    b.fromCategories = false;
    b.pendingStick.clear();
    const QStringList files = decklab::stickAudioFiles(b.folderPath);
    const QStringList folders = decklab::stickSubfolders(b.folderPath);
    {
        const QSignalBlocker blocker(b.items);
        b.items->clear();
        if (!files.isEmpty()) {
            auto* item = new QListWidgetItem(tr("Tracks in this folder"), b.items);
            item->setData(DeckLabListDelegate::KeyRole, QStringLiteral("tracks"));
            item->setData(DeckLabListDelegate::GlyphRole, static_cast<int>(decklab::Glyph::Tracks));
            item->setData(DeckLabListDelegate::CountRole, QString::number(files.size()));
            item->setData(DeckLabListDelegate::ChevronRole, true);
        }
        for (const QString& folder : folders) {
            auto* item = new QListWidgetItem(folder, b.items);
            item->setData(DeckLabListDelegate::KeyRole, QString(QStringLiteral("folder:") + folder));
            item->setData(DeckLabListDelegate::GlyphRole, static_cast<int>(decklab::Glyph::Folder));
            item->setData(DeckLabListDelegate::ChevronRole, true);
        }
        if (!b.items->count()) {
            auto* item = new QListWidgetItem(tr("No music here"), b.items);
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        }
        b.items->setCurrentRow(0);
    }
    b.itemsList = DeckLabList::Folder;
    showSources();
}
void WDeckLab::openFolderRow(int rowIndex) {
    auto& b = *m_browseState;
    auto* item = b.items->item(rowIndex);
    if (!item || !(item->flags() & Qt::ItemIsEnabled)) return;
    const QString key = item->data(DeckLabListDelegate::KeyRole).toString();
    if (key == QStringLiteral("tracks")) openFolderTracks();
    else if (key.startsWith(QStringLiteral("folder:"))) showFolder(QDir(b.folderPath).filePath(key.mid(7)));
}
void WDeckLab::openFolderTracks() {
    auto& b = *m_browseState;
    auto* model = m_sidebar ? qobject_cast<SidebarModel*>(m_sidebar->model()) : nullptr;
    const QModelIndex folder = sidebarIndexForPath(b.folderPath);
    if (!model || !folder.isValid()) {
        if (b.toast) b.toast->showMessage(tr("This folder can't be opened"), 2400);
        return;
    }
    model->clicked(folder); // Mixxx's file browser lists the folder's tracks
    showTracks();
}
QModelIndex WDeckLab::sidebarIndexForPath(const QString& path) {
    // Walks Mixxx's file browser ("Computer") down to path, listing each
    // level on the way (one directory read per level).
    auto* model = m_sidebar ? qobject_cast<SidebarModel*>(m_sidebar->model()) : nullptr;
    if (!model) return {};
    QModelIndex parent = sidebarRoot(model, QStringLiteral("computer"));
    if (!parent.isValid()) return {};
    QString target = QDir::cleanPath(path);
    if (!target.endsWith(QLatin1Char('/'))) target += QLatin1Char('/');
    for (int depth = 0; depth < 48; ++depth) {
        QModelIndex best;
        int bestLength = 0;
        for (int row = 0; row < model->rowCount(parent); ++row) {
            const QModelIndex child = model->index(row, 0, parent);
            QString data = child.data(SidebarModel::DataRole).toString();
            if (data.isEmpty() || data.startsWith(QStringLiteral("::"))) continue;
            if (!data.endsWith(QLatin1Char('/'))) data += QLatin1Char('/');
            if (target.startsWith(data) && data.size() > bestLength) {
                best = child;
                bestLength = static_cast<int>(data.size());
            }
        }
        if (!best.isValid()) return {};
        if (bestLength == target.size()) return best;
        model->doubleClicked(best); // lists its subfolders
        parent = best;
    }
    return {};
}
void WDeckLab::retryRekordboxRead() {
    auto& b = *m_browseState;
    b.loadFailed = false;
    b.failedSticks.remove(b.stickPath);
    b.loadingSince = m_clock.elapsed();
    if (!b.stickPath.isEmpty()) b.readingSticks.insert(b.stickPath, b.loadingSince); // r4-browse
    if (b.source.isValid()) {
        activateBrowseSource(); // reads the export again
    } else if (!b.stickPath.isEmpty()) {
        b.pendingStick = b.stickPath;
        const QModelIndex root = m_sidebar ? sidebarRoot(m_sidebar->model(), QStringLiteral("rekordbox")) : QModelIndex();
        if (auto* model = m_sidebar ? qobject_cast<SidebarModel*>(m_sidebar->model()) : nullptr; model && root.isValid()) {
            model->clicked(root);
        }
    }
    showCategories();
}
QString WDeckLab::currentSourceName() const {
    const auto& b = *m_browseState;
    if (b.source.isValid()) return isCollectionSource(b.source) ? tr("Library") : b.source.data().toString();
    return b.stickName.isEmpty() ? tr("USB stick") : b.stickName;
}
void WDeckLab::openUsbInBrowse() {
    // U-01: the rail's USB opens the stick in Browse; with several sticks,
    // the source list with the first one highlighted.
    selectPage(1);
    setSearchOpen(false);
    if (!m_browseState || !m_sidebar) return;
    const auto sticks = decklab::findUsbSticks();
    // r4-browse (move 1): the source home with the stick highlighted, so it
    // says what is on the stick before you open it; a press opens it.
    showHome(sticks.isEmpty() ? QString() : QStringLiteral("usb:") + sticks.first().path);
    if (sticks.isEmpty() && m_browseState->toast) {
        m_browseState->toast->showMessage(tr("No USB stick. Plug one in to play its music."), 2600);
    }
}
void WDeckLab::showOnlyMatches() {
    // B-42: Tools › Match Deck filters what you are looking at and says so on
    // the Match bar and in the breadcrumb, with a visible way out.
    auto& b = *m_browseState;
    if (m_browseStack->currentIndex() == 0) {
        if (b.list == DeckLabList::Folder) {
            openFolderTracks();
        } else if (b.source.isValid() && (b.list == DeckLabList::Categories || b.list == DeckLabList::Values ||
                           b.list == DeckLabList::Mixes)) {
            clearBrowseFilter();
            activateBrowseSource();
            b.category = decklab::Category::AllTracks;
            b.fromCategories = true;
            showTracks();
        }
        if (m_browseStack->currentIndex() == 0) {
            if (b.toast) b.toast->showMessage(tr("Open a list, then pick Match Deck"), 2000);
            return;
        }
    }
    if (!b.matchEnabled) {
        b.matchEnabled = true;
        m_config->setValue(ConfigKey("[DeckLab]", "BrowseMatch"), 1);
    }
    b.matchOnly = true;
    refreshMatchReference();
    applyMatchFilter(true);
    updateMatchBar();
    updateBreadcrumb();
}
void WDeckLab::applyMatchFilter(bool force) {
    if (!m_browseState || !m_libraryWidget) return;
    auto& b = *m_browseState;
    const QString typed = m_search ? m_search->currentText().trimmed() : QString();
    // The list's own filter (a category value or typed words) stays required.
    const QString extra = !b.filter.isEmpty() ? b.filter : (typed.contains(QLatin1Char('|')) ? QString() : typed);
    QString query;
    if (b.matchOnly) {
        decklab::MatchReference reference = decklab::matchReference();
        // Pitch drift re-filters only when the window moves by a quarter BPM.
        reference.bpm = std::round(reference.bpm * 4.0) / 4.0;
        query = decklab::matchFilterQuery(reference, extra);
    }
    if (!force && query == b.matchApplied) return;
    const bool wasApplied = !b.matchApplied.isEmpty();
    b.matchApplied = query;
    if (!query.isEmpty()) m_libraryWidget->search(query);
    else if (wasApplied || force) m_libraryWidget->search(extra);
    scheduleMatchCount();
}
void WDeckLab::updateMatchBar() {
    if (!m_browseState || !m_browseState->matchBar) return;
    auto& b = *m_browseState;
    const bool tracks = m_browseStack && m_libraryWidget && m_browseStack->currentWidget() == m_libraryWidget;
    const bool keyboard = m_keyboard && m_keyboard->isVisible();
    // r7-browse: a pad's SAMPLES list has ALL MUSIC instead of MATCH, and no Match bar.
    const bool padSamples = m_sampleTarget && b.folderList == decklab::FolderList::Samples && folderTableShown();
    const bool shown = b.matchEnabled && tracks && (!keyboard || b.matchOnly) && !padSamples;
    setVisibleIf(b.matchBar, shown);
    // r5-history: re-lay the column now; a layout request taken while Browse
    // was hidden left the bar's 46 px reserved over the list (History, home).
    if (QLayout* column = b.matchBar->parentWidget() ? b.matchBar->parentWidget()->layout() : nullptr) column->activate();
    if (!shown) return;
    using Tone = DeckLabActionButton::Tone;
    const auto& reference = decklab::matchReference();
    const bool active = reference.active();
    QStringList detail;
    const QString key = decklab::formatKey(reference.key); // r6-keys
    if (active && reference.key != mixxx::track::io::key::INVALID && !key.isEmpty()) detail << key;
    if (active && reference.bpm > 0) detail << QLocale().toString(reference.bpm, 'f', 1);
    b.matchDeck->setReference(active ? reference.deck : 0, reference.automatic,
            active ? detail.join(QStringLiteral(" · ")) : tr("No deck"));
    const std::array<decklab::TempoDirection, 3> order{decklab::TempoDirection::Slower,
            decklab::TempoDirection::Both, decklab::TempoDirection::Faster};
    for (int n = 0; n < 3; ++n) {
        b.directions[n]->setTone(b.direction == order[n] ? Tone::Selected : Tone::Neutral);
        b.directions[n]->setEnabled(active && reference.bpm > 0);
    }
    if (!active) {
        // r5-polish: short enough for the summary at 800 px ("…to mat…" was cut).
        b.matchSummary->setLines(tr("Load a deck first"), QString(), false);
    } else {
        const auto window = reference.window();
        b.matchSummary->setLines(window.isValid() ? tr("%1 BPM").arg(decklab::tempoWindowText(window)) : tr("Any tempo"),
                reference.key != mixxx::track::io::key::INVALID ? tr("Key: same ±1 rel") : tr("Key: any"), true);
    }
    b.matchOnlyButton->setEnabled(active);
    b.matchOnlyButton->setTone(b.matchOnly ? Tone::Selected : Tone::Neutral);
    // r5-polish: the bare count when "1,234 tracks" would not fit.
    b.matchOnlyButton->setDetail(b.matchCount >= 0 ? countText(b.matchCount, tr("1 track"), tr("%1 tracks")) : QString(),
            b.matchCount >= 0 ? QLocale().toString(b.matchCount) : QString());
    b.matchOnlyButton->setAccessibleName(b.matchOnly ? tr("Showing only matching tracks; tap to show all")
                                                     : tr("Show only matching tracks"));
}
// r5-polish (tests): a count the fixtures' empty track table can't produce.
void WDeckLab::setMatchCountForTesting(int count) {
    if (!m_browseState) return;
    m_browseState->matchCount = count;
    updateMatchBar();
}
void WDeckLab::scheduleMatchCount() {
    if (m_browseState && !m_browseState->matchCountTimer.isActive()) m_browseState->matchCountTimer.start();
}
void WDeckLab::refreshMatchCount() {
    if (!m_browseState) return;
    auto& b = *m_browseState;
    int count = -1;
    const auto& reference = decklab::matchReference();
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    auto* tracks = table ? dynamic_cast<TrackModel*>(table->model()) : nullptr;
    const bool shown = m_libraryWidget && m_browseStack->currentWidget() == m_libraryWidget;
    if (reference.active() && shown && tracks && table->model()) {
        const QAbstractItemModel* rows = table->model();
        if (b.matchOnly && !b.matchApplied.isEmpty()) {
            count = rows->rowCount();
        } else if (rows->rowCount() <= 4000) {
            // Same rule as the filter: compatible key and inside the window.
            const int keyColumn = tracks->columnIndexFromSortColumnId(TrackModel::SortColumnId::Key);
            const int bpmColumn = tracks->columnIndexFromSortColumnId(TrackModel::SortColumnId::Bpm);
            const auto window = reference.window();
            const bool anyKey = reference.key == mixxx::track::io::key::INVALID;
            count = 0;
            for (int row = 0; row < rows->rowCount(); ++row) {
                bool tempo = !window.isValid();
                if (!tempo && bpmColumn >= 0) {
                    bool ok = false;
                    const double bpm = rows->index(row, bpmColumn).data(Qt::EditRole).toDouble(&ok);
                    tempo = ok && decklab::tempoInWindow(bpm, window);
                }
                const bool key = anyKey || (keyColumn >= 0 &&
                        decklab::harmonicMatch(decklab::keyFromText(rows->index(row, keyColumn).data().toString()),
                                reference.key) != decklab::HarmonicMatch::None);
                if (tempo && key) ++count;
            }
        }
    }
    if (count == b.matchCount) return;
    b.matchCount = count;
    updateMatchBar();
    updateBreadcrumb();
}
void WDeckLab::scheduleFocusReclaim() {
    if (!m_browseState || m_browseState->reclaimQueued) return;
    m_browseState->reclaimQueued = true;
    QTimer::singleShot(0, this, [this] { reclaimFocus(); });
}
void WDeckLab::reclaimFocus() {
    if (!m_browseState) return;
    m_browseState->reclaimQueued = false;
    updateKnobLegend(); // r4-browse
    if (!isVisible() || QApplication::activePopupWidget() || QApplication::activeModalWidget()) return;
    QWidget* top = window();
    if (QApplication::activeWindow() != top) {
        top->activateWindow();
        qDebug() << "DeckLab knob: main window re-activated after a menu or dialog closed";
    }
    if (m_page != 1 || (m_keyboard && m_keyboard->isVisible())) return;
    // Keep a visible focused control; otherwise the visible list takes focus.
    QWidget* focus = QApplication::focusWidget();
    if (focus && focus->window() == top && focus->isVisible()) return;
    if (m_browseStack->currentIndex() != 0) {
        if (QWidget* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr) table->setFocus(Qt::OtherFocusReason);
    } else if (m_browseState->list == DeckLabList::Sources) {
        if (m_sidebar) m_sidebar->setFocus(Qt::OtherFocusReason);
    } else if (m_browseState->items) {
        m_browseState->items->setFocus(Qt::OtherFocusReason);
    }
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
    // A held touch belongs to its actual deck, not the side.
    if (m_touchCue == channel(m_layers[side])) releaseTouchCue();
    ++m_interactionEpoch;
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
    // r4-waveforms: compact pages give their two lanes to the most relevant
    // decks (on air, then the controller's layers, then loaded) and name the
    // others on the DECKS row; a change of that choice relayouts.
    const bool compact = decklab::lanes::presentationFor(m_page) == decklab::lanes::Presentation::Compact;
    decklab::lanes::Choice laneChoice;
    if (compact) {
        std::array<decklab::lanes::Deck, 4> facts{};
        for (int deck = 1; deck <= 4; ++deck)
            facts[deck - 1] = {m_deck[deck - 1].loaded->get() > 0, m_deck[deck - 1].play->get() > 0};
        laneChoice = decklab::lanes::choose(facts, {m_layers[0], m_layers[1]}, four);
        signature |= quint64(laneChoice.lanes[0]) << 19 | quint64(laneChoice.lanes[1]) << 22 |
                quint64(laneChoice.rowCount) << 25;
        for (int index = 0; index < laneChoice.rowCount; ++index)
            signature |= quint64(laneChoice.row[index]) << (28 + 3 * index);
    }
    // r4-decks: stems are a pad mode (decklabpads.h), so a stem track no
    // longer changes the layout (round 3 took a 48 px row from the waveforms).
    m_deckTools->validate(); signature |= m_deckTools->layoutSignature(); // r5-decks: GRID mode (one relayout)
    if (signature == m_layoutSignature) {
        for (int deck = 1; deck <= 4; ++deck) markDeckDirty(deck);
        refreshLoadButtons();
        return;
    }
    m_layoutSignature = signature;
    m_fxSummaryDirty = true; markDeckDirty(0);
    if (m_labFx) m_labFx->setDeckLayout(four ? 4 : 2); // r6-fxstrip
    m_deckControls->hide();
    const bool decks = m_page == 0;
    // r4-waveforms: compact lanes keep the Decks arrangement, so a lane's
    // width (and its waveform column cache) never changes between pages.
    const bool separated = (decks || compact) && m_sideBySideWaveforms;
    // Waveform zone: stacked full-width lanes, or side by side (two columns).
    // Lanes keep their parent; they only move between grid cells.
    for (int index = 0; index < 4; ++index) {
        m_zoneGrid->setRowStretch(index, 0);
        m_zoneGrid->setColumnStretch(index, 0);
        if (m_deckWaveforms[index]) m_zoneGrid->removeWidget(m_deckWaveforms[index]);
    }
    const std::array<int, 4> order = decks && four ? std::array<int, 4>{1, 2, 3, 4}
            : compact ? std::array<int, 4>{laneChoice.lanes[0], laneChoice.lanes[1], 0, 0}
                      : std::array<int, 4>{m_layers[0], m_layers[1], 0, 0};
    const int count = decks && four ? 4 : 2;
    const int columns = separated ? 2 : 1;
    for (int deck = 1; deck <= 4; ++deck) {
        auto* lane = m_deckWaveforms[deck - 1];
        if (!lane) continue;
        int slot = -1;
        for (int index = 0; index < count; ++index) if (order[index] == deck) slot = index;
        // r4-waveforms: the DECKS row replaces the one-line readouts, so the
        // lane's edge keeps its number on every page.
        setVisibleIf(m_minis[deck - 1], false);
        if (auto* tag = lane->findChild<QLabel*>(QStringLiteral("decklab-lane-edge-%1").arg(deck)))
            setTextIf(tag, QString::number(deck));
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
        if (m_sidePads[side]) m_deckGrid->removeWidget(m_sidePads[side]);
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
        if (shown && m_sidePads[side]) m_deckGrid->addWidget(m_sidePads[side], 1, side);
        setVisibleIf(m_sidePads[side], shown);
        setVisibleIf(m_sideUtilities[side], shown);
    }
    // Readout + overview + transport (four decks), or + the side's pad block
    // (r4-decks: 4 px gap and two 44 px rows, the height the hot cue and
    // transport rows had).
    setFixedHeightIf(m_deckRow, four
                    ? DeckLabStripRenderer::preferredHeight(DeckLabStripRenderer::Mode::Compact) + kOverviewHeight + 48
                    : DeckLabStripRenderer::preferredHeight(DeckLabStripRenderer::Mode::Full) + kOverviewHeight +
                            4 + DeckLabPadBlock::kBlockHeight);
    setVisibleIf(m_deckRow, decks);
    m_deckTools->applyLayout(); // r5-decks: GRID mode's place; stale sheets close
    // r4-waveforms: the DECKS row under compact lanes; refreshDeck() below
    // gives it every deck's state.
    if (compact) m_laneDecks->setChoice(laneChoice);
    setVisibleIf(m_laneDecks, compact);
    for (int deck = 1; deck <= 4; ++deck) refreshDeck(deck);
    refreshLoadButtons();
}

// r4-waveforms: compact lanes (Sampler, Lab FX) share one height, from the
// room under the rail (800x480: two 66 px lanes and the 32 px DECKS row).
int WDeckLab::compactLaneZoneHeight() const {
    return decklab::lanes::compactZoneHeight(height() - (m_rail ? m_rail->height() : 44));
}

void WDeckLab::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (decklab::lanes::presentationFor(m_page) == decklab::lanes::Presentation::Compact) {
        setFixedHeightIf(m_waveforms, compactLaneZoneHeight());
    }
}

// r4-browse: the round-4 Browse signature moves (audit library.md section 3):
// the source home's tiles (move 1), the deck dock (move 3), the knob legend
// (move 4) and "Mixes with deck N" (move 5). Widgets and pure logic live in
// decklabbrowsehome/dock/legend/mix.*; this is the surface's side.
bool WDeckLab::browseTrackLevel() const {
    if (!m_browseStack || !m_browseState) return false;
    if (m_libraryWidget && m_browseStack->currentWidget() == m_libraryWidget) return true;
    if (m_browseState->list == DeckLabList::History) return historyTrackLevel(); // r5-history
    return m_browseStack->currentIndex() == 0 && m_browseState->list == DeckLabList::Mixes;
}
void WDeckLab::applyBrowsePresentation() {
    if (!m_browseState || !m_browseStack) return;
    auto& b = *m_browseState;
    const bool home = m_browseStack->currentIndex() == 0 && b.list == DeckLabList::Home;
    // The home's tiles take the whole width: they say what is inside, so the
    // info pane, Back (nowhere to go) and MATCH (nothing to match) step aside.
    // round4: ... except the pane while the keyboard is up: it holds the dock.
    setVisibleIf(b.pane, !home || (m_keyboard && !m_keyboard->isHidden()));
    setVisibleIf(b.back, !home);
    // r7-browse: a pad's SAMPLES list puts ALL MUSIC where MATCH sits.
    const bool padSamples = m_sampleTarget && b.folderList == decklab::FolderList::Samples && folderTableShown();
    setVisibleIf(b.allMusic, padSamples);
    setVisibleIf(b.match, !home && !padSamples);
    // Tiles flow left to right in reading order; every other level is a list.
    const bool tiles = home && b.itemsList == DeckLabList::Home;
    if (b.items && (b.items->flow() == QListView::LeftToRight) != tiles) {
        b.items->setFlow(tiles ? QListView::LeftToRight : QListView::TopToBottom);
        b.items->setWrapping(tiles);
        b.items->setResizeMode(tiles ? QListView::Adjust : QListView::Fixed);
        b.items->setSpacing(tiles ? DeckLabBrowseDelegate::kSpacing : 0);
        // The flow layout reserves room for a scroll bar unless it is off.
        b.items->setVerticalScrollBarPolicy(tiles ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
    }
    // Preview only where a track can be highlighted (B-31).
    setVisibleIf(b.preview, browseTrackLevel());
    applyHistoryPresentation(); // r5-history
    updateKnobLegend();
}
void WDeckLab::refreshDock() {
    if (!m_browseState || !m_browseState->dock) return;
    for (int slot = 0; slot < 4; ++slot) {
        auto* tile = m_browseState->dock->tile(slot);
        const int deck = tile->deck();
        if (deck >= 1 && deck <= 4) tile->setDeckState(decklab::dockDeckFrom(m_stripStates[deck - 1]));
    }
}
void WDeckLab::refreshDockFor(int deck) {
    // Only while Browse shows the dock; the next visit refreshes it all.
    if (m_page != 1 || !m_browseState || !m_browseState->dock || deck < 1 || deck > 4) return;
    for (int slot = 0; slot < 4; ++slot) {
        auto* tile = m_browseState->dock->tile(slot);
        if (tile->deck() == deck) tile->setDeckState(decklab::dockDeckFrom(m_stripStates[deck - 1]));
    }
}
void WDeckLab::scheduleKnobLegend() {
    if (!m_browseState || m_browseState->legendQueued) return;
    m_browseState->legendQueued = true;
    QTimer::singleShot(0, this, [this] {
        if (!m_browseState) return;
        m_browseState->legendQueued = false;
        updateKnobLegend();
    });
}
void WDeckLab::updateKnobLegend() {
    if (!m_browseState || !m_browseState->legend || !m_browseStack) return;
    auto& b = *m_browseState;
    using decklab::LegendLevel;
    LegendLevel level = LegendLevel::List;
    if (!decklab::knobOwner().browse()) {
        level = LegendLevel::Menu; // C-05: a menu or dialog really owns the knob
    } else if (browseTrackLevel()) {
        level = LegendLevel::Tracks; // C-02: the press previews
    } else if (m_browseStack->currentIndex() == 0 && b.list == DeckLabList::Home) {
        level = m_clock.elapsed() - b.homeBackArmedAt < 2000 ? LegendLevel::HomeBackArmed : LegendLevel::Home;
    }
    b.legend->setLevel(level);
}
void WDeckLab::flashKnobLegend() {
    if (!m_browseState || !m_browseState->legend) return;
    updateKnobLegend();
    m_browseState->legend->flash();
}
decklab::SourceState WDeckLab::stickState(const decklab::UsbStick& stick) {
    auto& b = *m_browseState;
    using decklab::SourceState;
    if (!stick.rekordbox) return SourceState::None;
    const QModelIndex device = m_sidebar ? rekordboxDeviceFor(m_sidebar->model(), stick.path) : QModelIndex();
    if (device.isValid() && rekordboxImported(device)) {
        b.failedSticks.remove(stick.path); // a read that finished late
        return SourceState::Ready;
    }
    if (b.failedSticks.contains(stick.path)) return SourceState::Failed;
    if (b.readingSticks.contains(stick.path)) return SourceState::Reading;
    return SourceState::NotRead;
}
QString WDeckLab::stickDetail(const decklab::UsbStick& stick) {
    if (stick.rekordbox) {
        if (m_browseState->failedSticks.contains(stick.path)) return tr("Try again, or open it for its files");
        return decklab::exportDateText(stick.path);
    }
    const QStorageInfo storage(stick.path);
    return decklab::capacityText(storage.bytesTotal(), storage.bytesAvailable());
}
QString WDeckLab::homeStateSignature(const QList<decklab::UsbStick>& sticks) {
    QString signature = stickSignature(sticks);
    QStringList stickPaths;
    for (const auto& stick : sticks) {
        stickPaths << stick.path;
        if (stick.rekordbox) signature += QLatin1Char('|') + QString::number(static_cast<int>(stickState(stick)));
    }
    // r7 fix: the DOWNLOADS and SAMPLES tiles appear with their folders
    // (Get Music's first download, the first sample): a few stats.
    signature += QLatin1Char('|') + decklab::downloadFolders(stickPaths).join(QLatin1Char(':'));
    signature += QFileInfo(decklab::samplesFolder()).isDir() ? QStringLiteral("|S") : QStringLiteral("|-");
    return signature;
}
void WDeckLab::retryStickFromHome(const QString& path) {
    m_browseState->failedSticks.remove(path);
    openStick(path);
}
decklab::MatchReference WDeckLab::mixReference() {
    // The Match bar's reference deck, direction and window, even while its
    // highlight is switched off.
    return decklab::mixesReference(decklab::matchReference());
}
void WDeckLab::refreshMixCandidates() {
    auto& b = *m_browseState;
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    auto* tracks = table ? dynamic_cast<TrackModel*>(table->model()) : nullptr;
    QAbstractItemModel* model = table ? table->model() : nullptr;
    if (!tracks || !model) {
        b.mixCandidates.clear();
        b.mixLocations.clear();
        b.mixModel = nullptr;
        b.mixModelRows = -1;
        return;
    }
    // r5-fix2-history: a table Mixxx swapped in (a stick read finishing) is
    // never this source's list: the cache stays until the source shows again.
    if (mixTableForeign()) return;
    // Only the source's whole list counts: a filtered list keeps the cache
    // of the same source, and forgets that of another one.
    if (!tracks->currentSearch().isEmpty()) {
        if (b.mixSource != b.source) {
            b.mixCandidates.clear();
            b.mixLocations.clear();
            b.mixModel = nullptr;
            b.mixModelRows = -1;
        }
        return;
    }
    const int rows = model->rowCount();
    if (b.mixModel == model && b.mixModelRows == rows && b.mixSource == b.source && !b.mixDirty) return;
    if (b.mixWatched.data() != model) {
        // r5-fix3-history: a select() on the model (the source shown again, a
        // rescan, a sort) moves or refills its rows: read them again then.
        for (const auto& connection : std::as_const(b.mixModelConnections)) disconnect(connection);
        b.mixModelConnections.clear();
        b.mixWatched = model;
        const auto dirty = [this] {
            if (m_browseState) m_browseState->mixDirty = true;
        };
        b.mixModelConnections << connect(model, &QAbstractItemModel::modelReset, this, dirty)
                              << connect(model, &QAbstractItemModel::rowsInserted, this, dirty)
                              << connect(model, &QAbstractItemModel::rowsRemoved, this, dirty)
                              << connect(model, &QAbstractItemModel::rowsMoved, this, dirty)
                              << connect(model, &QAbstractItemModel::layoutChanged, this, dirty);
    }
    b.mixDirty = false;
    b.mixModel = model;
    b.mixModelRows = rows;
    b.mixSource = b.source;
    // Read once per source (O(rows), no database); recounts are arithmetic.
    b.mixCandidates = decklab::collectMixCandidates(*model,
            tracks->columnIndexFromSortColumnId(TrackModel::SortColumnId::Key),
            tracks->columnIndexFromSortColumnId(TrackModel::SortColumnId::Bpm), 20000);
    b.mixLocations.clear();
    b.mixLocations.reserve(b.mixCandidates.size());
    for (const auto& candidate : std::as_const(b.mixCandidates)) {
        b.mixLocations << tracks->getTrackLocation(model->index(candidate.row, 0));
    }
}
int WDeckLab::mixSkipRow() {
    // The reference deck's own track never mixes with itself.
    auto& b = *m_browseState;
    const int deck = decklab::matchReference().deck;
    const TrackPointer& track = deck >= 1 && deck <= 4 ? m_tracks[deck - 1] : TrackPointer();
    const int at = track ? static_cast<int>(b.mixLocations.indexOf(track->getLocation())) : -1;
    return at >= 0 && at < b.mixCandidates.size() ? b.mixCandidates[at].row : -1;
}
void WDeckLab::updateMixesRow() {
    auto& b = *m_browseState;
    if (b.itemsList != DeckLabList::Categories || !b.items || !b.items->count()) return;
    auto* item = b.items->item(0);
    if (item->data(DeckLabListDelegate::KeyRole).toString() != QStringLiteral("mixes")) return;
    const auto reference = mixReference();
    b.mixShown = reference;
    const int count = decklab::countMixes(b.mixCandidates, reference, mixSkipRow());
    QStringList facts;
    const QString key = decklab::formatKey(reference.key); // r6-keys
    if (reference.key != mixxx::track::io::key::INVALID && !key.isEmpty()) facts << key;
    if (reference.bpm > 0) facts << QLocale().toString(reference.bpm, 'f', 1);
    const auto window = reference.window();
    if (window.isValid()) facts << tr("%1 BPM").arg(decklab::tempoWindowText(window));
    const QString title = decklab::mixesTitle(reference.deck);
    item->setText(title);
    item->setData(DeckLabListDelegate::SubtitleRole, facts.join(QStringLiteral(" · ")));
    item->setData(DeckLabListDelegate::CountRole, QLocale().toString(count));
    item->setData(DeckLabListDelegate::AccentRole, deckLabDeckColor(reference.deck));
    item->setData(Qt::AccessibleTextRole, tr("%1: %2 tracks").arg(title, QLocale().toString(count)));
    if (b.list == DeckLabList::Categories && b.items->currentRow() == 0) refreshBrowseCard();
}
void WDeckLab::showMixes() {
    auto& b = *m_browseState;
    if (!mixReference().active()) return;
    // The whole source: no category filter, no Only, no typed words (row
    // numbers of the candidates refer to the unfiltered list).
    clearBrowseFilter();
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    auto* tracks = table ? dynamic_cast<TrackModel*>(table->model()) : nullptr;
    if (tracks && !tracks->currentSearch().isEmpty()) {
        if (m_search) m_search->slotRestoreSearch(QString());
        m_libraryWidget->search(QString());
    }
    b.categoryOnMixes = true;
    b.categoryReturn = true;
    b.categoryRow = 0;
    b.list = DeckLabList::Mixes;
    b.mixReactivated = false; // r5-fix2-history
    fillMixes(false);
    showSources();
    syncMixSelection();
}
void WDeckLab::fillMixes(bool keepCurrent) {
    auto& b = *m_browseState;
    // r5-fix2-history: the source's own table first; while another one is
    // shown (Browse hidden, say) the list stays as it is.
    if (!ensureMixTable() && b.itemsList == DeckLabList::Mixes && b.items->count()) return;
    refreshMixCandidates();
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    auto* tracks = table ? dynamic_cast<TrackModel*>(table->model()) : nullptr;
    const QAbstractItemModel* model = table ? table->model() : nullptr;
    const auto reference = mixReference();
    b.mixShown = reference;
    // r5-fix3-history: the highlighted row is kept by its file (its index is
    // gone after a select() on the source model).
    QString keep;
    if (keepCurrent && b.itemsList == DeckLabList::Mixes && b.items->currentItem()) {
        keep = b.items->currentItem()->data(decklab::history::LocationRole).toString();
    }
    const QSignalBlocker blocker(b.items);
    b.items->clear();
    // round5: a kept highlight whose file is gone leaves no highlight, so a
    // press never loads a track the DJ did not pick (row 0).
    int current = keep.isEmpty() ? 0 : -1;
    if (tracks && model && b.mixModel == model && b.mixModelRows == model->rowCount()) {
        const int titleColumn = tracks->columnIndexFromSortColumnId(TrackModel::SortColumnId::Title);
        const int artistColumn = tracks->columnIndexFromSortColumnId(TrackModel::SortColumnId::Artist);
        const int keyColumn = tracks->columnIndexFromSortColumnId(TrackModel::SortColumnId::Key); // r6-keys
        for (const auto& mix : decklab::rankMixes(b.mixCandidates, reference, mixSkipRow())) {
            const QModelIndex index = model->index(mix.row, 0);
            if (!index.isValid()) continue;
            QString title = titleColumn >= 0 ? model->index(mix.row, titleColumn).data().toString().trimmed() : QString();
            if (title.isEmpty()) title = QFileInfo(tracks->getTrackLocation(index)).completeBaseName();
            const QString relation = decklab::mixRelationText(mix.key, mix.bpm, reference);
            // r6-keys: a key that is not one (no reference key ranks by tempo
            // only) shows as its row stores it, never as "—".
            const QString key = mix.key != mixxx::track::io::key::INVALID ? decklab::formatKey(mix.key)
                    : keyColumn >= 0 ? model->index(mix.row, keyColumn).data().toString().trimmed() : QString();
            auto* item = new QListWidgetItem(title, b.items);
            item->setData(DeckLabListDelegate::MixRole, true);
            item->setData(DeckLabListDelegate::MixKeyRole, key);
            item->setData(DeckLabListDelegate::MixBpmRole, mix.bpm);
            item->setData(DeckLabListDelegate::MixDeltaRole, decklab::bpmDeltaText(mix.bpm, reference.bpm));
            item->setData(DeckLabListDelegate::MixRelationRole, relation);
            item->setData(DeckLabListDelegate::MixMatchRole, static_cast<int>(decklab::harmonicMatch(mix.key, reference.key)));
            item->setData(DeckLabListDelegate::MixIndexRole, QVariant::fromValue(QPersistentModelIndex(index)));
            item->setData(decklab::history::LocationRole, tracks->getTrackLocation(index)); // r5-history: its played mark
            if (artistColumn >= 0) item->setData(DeckLabListDelegate::MixArtistRole, model->index(mix.row, artistColumn).data().toString().trimmed());
            item->setData(Qt::AccessibleTextRole, QString(title + QStringLiteral(", ") + relation));
            if (!keep.isEmpty() && item->data(decklab::history::LocationRole).toString() == keep) current = b.items->count() - 1;
        }
    }
    if (!b.items->count()) {
        auto* item = new QListWidgetItem(tr("Nothing here mixes with deck %1 yet").arg(reference.deck), b.items);
        item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        current = -1;
    }
    b.items->setCurrentRow(current);
    b.itemsList = DeckLabList::Mixes;
}
void WDeckLab::syncMixSelection(bool pressed) {
    // The highlighted Mixes row is the track LOAD and Preview act on: select
    // it in the (hidden) track table at once, read its Track object once the
    // knob settles (never for every row a fast knob passes).
    auto& b = *m_browseState;
    if (b.list == DeckLabList::Mixes && b.itemsList == DeckLabList::Mixes) {
        // r5-fix2-history: rows of the source's own table: shown again when
        // Mixxx swapped another one in, and ranked again if the reference
        // moved meanwhile. r5-fix3-history: a row is its track's file; when
        // its table row went (any select() on the source model: the source
        // shown again, a rescan, a sort) the rows are read again, keeping the
        // highlighted file. A press does this on any page.
        const bool foreign = mixTableForeign();
        if (!foreign || ensureMixTable(pressed)) {
            const QListWidgetItem* current = b.items->currentItem();
            const bool lost = current && current->data(DeckLabListDelegate::MixRole).toBool() &&
                    !mixTableRow(current).isValid();
            if (lost || (foreign && !mixReference().sameAs(b.mixShown))) {
                fillMixes(true);
                updateBreadcrumb();
            }
        }
    }
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    const QListWidgetItem* item = b.items->currentItem();
    const QModelIndex source = b.list == DeckLabList::Mixes ? mixTableRow(item) : QModelIndex();
    if (table && table->selectionModel()) {
        if (source.isValid()) {
            table->selectionModel()->setCurrentIndex(source,
                    QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        } else {
            table->selectionModel()->clearSelection();
        }
    }
    if (!source.isValid()) b.selectedTrack.reset();
    b.mixSelectTimer.start();
    refreshLoadButtons();
    refreshBrowseCard();
}

// r5-history: track history and "played" marks (decklabhistory.h holds the
// data, Mixxx's own history playlists; decklabhistoryview.h the rows and the
// mark). The History view is a Browse list level: the sets, then one set's
// tracks in play order. A track row is the highlighted track: it is selected
// in the (hidden) table that shows its Mixxx history playlist, matched by
// position and library id, so LOAD, Preview and every guard use Mixxx's own
// LoadSelectedTrack, as for Mixes.
void WDeckLab::initHistory() {
    auto& b = *m_browseState;
    auto& store = decklab::history::Store::instance();
    if (m_library && m_library->trackCollectionManager()) {
        const QPointer<Library> library(m_library);
        store.setBackend(decklab::history::makeMixxxBackend(m_library->trackCollectionManager(), m_config,
                                 [library] {
                                     // SetlogFeature's "Finish current and start new".
                                     auto* setlog = library ? library->findChild<SetlogFeature*>() : nullptr;
                                     if (!setlog) return false;
                                     setlog->slotGetNewPlaylist();
                                     return true;
                                 }),
                this);
    }
    store.setDimPlayed(m_config->getValue(ConfigKey("[Library]", "ApplyPlayedTrackColor"), true));
    store.listen(this, [this](const decklab::history::Change& change) { historyChanged(change); });
    // History rows paint next to the home's tiles and the Mixes rows.
    auto* previous = b.items->itemDelegate();
    b.items->setItemDelegate(new DeckLabHistoryDelegate(b.items));
    delete previous;
    // "New set": hold to confirm (the System kit), on the History levels only.
    b.newSet = new decklab::ui::HoldButton(tr("Hold for a new set"), b.pane, QStringLiteral("decklab-history-new-set"), 44);
    b.newSet->setGate([] {
        return decklab::history::Store::instance().currentCount() > 0 ? QString() : tr("This set is still empty");
    });
    connect(b.newSet, &decklab::ui::HoldButton::refused, this, [this](const QString& reason) {
        if (m_browseState->toast) m_browseState->toast->showMessage(reason, 2400);
    });
    connect(b.newSet, &decklab::ui::HoldButton::confirmed, this, [this] {
        auto& state = *m_browseState;
        if (state.newSetKnobHeld) {
            // A knob hold: the set starts on the release press, so Shift+press
            // (Back) or a turn can still call it off.
            state.newSetKnobDone = true;
            state.newSet->setResult(tr("Let go to start"));
            refreshNewSetRow();
            refreshHistoryCard();
            return;
        }
        startNewHistorySet();
    });
    b.paneLayout->insertWidget(b.paneLayout->indexOf(b.preview), b.newSet);
    b.newSet->hide();
    // From the knob, on the sets' "New set" row: a controller that reports
    // the held press (browse_hold) holds it and lets go; press twice works
    // with every controller.
    watch(kNative, "browse_hold", [this](double v) { historyKnobHold(v > 0); });
    // r5-fix2-history: a menu, sheet or dialog taking the knob calls off a
    // first press ("press twice") and a hold in progress.
    decklab::onKnobOwnerChanged(this, [this] {
        if (!decklab::knobOwner().browse()) cancelNewSetKnob();
    });
    b.newSetReleaseTimer.setSingleShot(true);
    b.newSetReleaseTimer.setInterval(500);
    connect(&b.newSetReleaseTimer, &QTimer::timeout, this, [this] {
        // No release press came (a turn): nothing starts.
        cancelNewSetKnob();
    });
    // Controller LOAD on a Browse list level (History, Mixes): 1 exactly
    // when the table row LOAD reads is the highlighted track row.
    createControl(kNative, QStringLiteral("track_level"));
    // r5-fix2-history: and LibraryControl asks again at load time.
    decklab::nativeLoadGuard() = [surface = QPointer<WDeckLab>(this)] {
        return surface && surface->browseLoadableNow();
    };
    // A table shown under a highlighted row (a rekordbox read finishing, for
    // example) never becomes the one LOAD or Preview acts on. r5-fix2-history:
    // at once (after the table took the model), then the row comes back queued.
    if (m_library) {
        connect(m_library, &Library::showTrackModel, this, [this] { historyTableShown(); });
        connect(m_library, &Library::switchToView, this, [this] { updateTrackLevel(); }, Qt::QueuedConnection);
    }
    // The card reads the highlighted track's Track object once the knob
    // settles, never for every row a fast knob passes.
    b.historySelectTimer.setSingleShot(true);
    b.historySelectTimer.setInterval(120);
    connect(&b.historySelectTimer, &QTimer::timeout, this, [this] { settleHistoryRow(); });
}
void WDeckLab::settleHistoryRow(bool pressed) {
    auto& state = *m_browseState;
    if (!historyTrackLevel()) return;
    // r5-fix-history: the row's own playlist and row, once the knob settles
    // (switching Mixxx's table is SQL and a model reset: never per step).
    // r5-fix2-history: and never while Browse is hidden; the next visit does.
    // r5-fix3-history: except for a deliberate load, on any page.
    const QListWidgetItem* item = state.items->currentItem();
    if (item && static_cast<decklab::history::RowKind>(item->data(decklab::history::RowRole).toInt()) ==
                    decklab::history::RowKind::Track) {
        if (m_page == 1 || pressed) {
            showHistoryPlaylist(item->data(decklab::history::PlaylistRole).toInt());
            selectHistoryRow();
        } else if (!browseRowLoadable()) {
            state.browseRowStale = true;
        }
    }
    if (!browseRowLoadable()) {
        state.selectedTrack.reset();
        refreshLoadButtons();
        refreshBrowseCard();
        return;
    }
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    auto* tracks = table ? dynamic_cast<TrackModel*>(table->model()) : nullptr;
    const QModelIndexList rows = table && table->selectionModel() ? table->selectionModel()->selectedRows() : QModelIndexList();
    state.selectedTrack = tracks && rows.size() == 1 ? tracks->getTrack(rows.first()) : TrackPointer();
    refreshLoadButtons();
    refreshBrowseCard();
    refreshPreviewButton();
}
void WDeckLab::historyAttachSystem(DeckLabSystem* system) {
    if (m_browseState) m_browseState->historySystem = system;
}
void WDeckLab::askHistoryClock() {
    // A Pi 4 has no real-time clock: ask decklabd whether the clock is
    // synchronized (at most once a minute, only while Browse is shown).
    auto& b = *m_browseState;
    DeckLabdClient* client = b.historySystem ? b.historySystem->client() : nullptr;
    if (!client || !client->isReady() || m_clock.elapsed() - b.clockAskedAt < 60000) return;
    b.clockAskedAt = m_clock.elapsed();
    const QPointer<WDeckLab> self(this);
    client->call(QStringLiteral("time.status"), QJsonObject(), [self](const DeckLabdReply& reply) {
        if (!self || !reply.ok) return;
        const bool synced = reply.result.value(QStringLiteral("synced")).toBool() ||
                reply.result.value(QStringLiteral("rtc")).toBool();
        decklab::history::Store::instance().setClockState(
                synced ? decklab::history::ClockState::Synced : decklab::history::ClockState::Unsynced);
    });
}
bool WDeckLab::historyTrackLevel() const {
    return m_browseState && m_browseStack && m_browseStack->currentIndex() == 0 &&
            m_browseState->list == DeckLabList::History && m_browseState->historySession >= 0;
}
void WDeckLab::showHistory(bool fromCategories) {
    auto& b = *m_browseState;
    auto& store = decklab::history::Store::instance();
    clearBrowseFilter();
    store.ensureFresh(); // r7: a stick change mid-set is read now
    b.list = DeckLabList::History;
    b.historyFromCategories = fromCategories;
    b.historySession = -1;
    b.historyKeepSession = store.currentId();
    fillHistory(false);
    showSources();
}
void WDeckLab::fillHistory(bool keepCurrent) {
    auto& b = *m_browseState;
    using namespace decklab::history;
    auto& store = Store::instance();
    const bool trusted = store.clockTrusted();
    const QListWidgetItem* item = keepCurrent && b.itemsList == DeckLabList::History ? b.items->currentItem() : nullptr;
    const QSignalBlocker blocker(b.items);
    int row = -1;
    if (b.historySession < 0) {
        const int keep = item && item->data(SessionRole).isValid() ? item->data(SessionRole).toInt() : b.historyKeepSession;
        row = fillSessions(b.items, store.sessions(), QDate::currentDate(), keep, trusted);
    } else {
        const Session* session = store.session(b.historySession);
        const bool track = item && item->data(PositionRole).isValid();
        row = fillTracks(b.items, store.entries(b.historySession), session && session->current,
                track ? item->data(PositionRole).toInt() : -1, trusted, track ? item->data(PlaylistRole).toInt() : -1);
    }
    b.items->setCurrentRow(row);
    if (row >= 0) b.items->scrollToItem(b.items->item(row));
    b.itemsList = DeckLabList::History;
    b.historyStale = false;
}
void WDeckLab::openHistoryRow(int rowIndex) {
    auto& b = *m_browseState;
    using namespace decklab::history;
    const QListWidgetItem* item = b.items->item(rowIndex);
    if (!item || !(item->flags() & Qt::ItemIsEnabled)) return;
    const auto kind = static_cast<RowKind>(item->data(RowRole).toInt());
    if (kind == RowKind::NewSet) {
        // The knob's hold to confirm: this press arms, the next one starts it.
        if (b.newSet) b.newSet->knobPress();
        refreshNewSetRow();
        refreshHistoryCard();
        QTimer::singleShot(decklab::ui::HoldButton::kHoldMs + 2000, this, [this] {
            refreshNewSetRow();
            if (m_browseState->list == DeckLabList::History) refreshHistoryCard();
        });
        return;
    }
    if (kind != RowKind::Session) return; // a track: the press previews it (browsePress)
    b.historyKeepSession = item->data(SessionRole).toInt();
    b.historySession = b.historyKeepSession;
    fillHistory(false);
    showSources();
    syncHistorySelection();
}
void WDeckLab::refreshNewSetRow() {
    auto& b = *m_browseState;
    using namespace decklab::history;
    if (b.itemsList != DeckLabList::History || b.historySession >= 0) return;
    const bool empty = Store::instance().currentCount() <= 0;
    for (int row = 0; row < b.items->count(); ++row) {
        QListWidgetItem* item = b.items->item(row);
        if (static_cast<RowKind>(item->data(RowRole).toInt()) != RowKind::NewSet) continue;
        item->setFlags(empty ? Qt::NoItemFlags : Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        item->setData(DetailRole, empty ? tr("This set is still empty")
                        : b.newSetKnobDone              ? tr("Let go to end this set and start another")
                        : b.newSet && b.newSet->armed() ? tr("Press again to end this set and start another")
                                                         : tr("Hold the knob and let go, or press it twice"));
        return;
    }
}
void WDeckLab::showHistoryPlaylist(int playlistId) {
    // A history track row's own Mixxx playlist in the (hidden) track table:
    // the rows LOAD and Preview read. Nothing to do when it is shown.
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    if (const auto* shown = table ? dynamic_cast<PlaylistTableModel*>(table->model()) : nullptr;
            shown && shown->getPlaylist() == playlistId) {
        return;
    }
    auto* model = m_sidebar ? qobject_cast<SidebarModel*>(m_sidebar->model()) : nullptr;
    const QModelIndex root = model ? sidebarRoot(model, QStringLiteral("history")) : QModelIndex();
    QModelIndex found;
    for (int row = 0; root.isValid() && !found.isValid() && row < model->rowCount(root); ++row) {
        const QModelIndex child = model->index(row, 0, root);
        if (child.data(SidebarModel::DataRole).toInt() == playlistId) found = child;
        for (int leaf = 0; !found.isValid() && leaf < model->rowCount(child); ++leaf) {
            const QModelIndex year = model->index(leaf, 0, child); // sets grouped under a year
            if (year.data(SidebarModel::DataRole).toInt() == playlistId) found = year;
        }
    }
    if (!found.isValid()) return;
    if (m_search) m_search->slotRestoreSearch(QString());
    model->clicked(found);
    table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    auto* tracks = table ? dynamic_cast<TrackModel*>(table->model()) : nullptr;
    if (tracks && !tracks->currentSearch().isEmpty()) m_libraryWidget->search(QString());
}
void WDeckLab::historyMove(int delta) {
    auto& b = *m_browseState;
    const int count = b.items->count();
    if (!count) return;
    // Date headers and empty rows are never highlighted; a set's tracks
    // accelerate like every other track list (C-03).
    if (historyTrackLevel()) delta = b.accelerator.steps(delta, m_clock.elapsed());
    const int row = decklab::stepToEnabledRow(count, b.items->currentRow(), delta, [&b](int at) {
        return b.items->item(at)->flags().testFlag(Qt::ItemIsEnabled);
    });
    if (row < 0) return;
    b.items->setCurrentRow(row);
    b.items->scrollToItem(b.items->item(row));
}
void WDeckLab::historyBack() {
    auto& b = *m_browseState;
    if (b.historySession >= 0) {
        // Up to the sets, on the set that was open.
        b.historyKeepSession = b.historySession;
        b.historySession = -1;
        fillHistory(false);
        showSources();
        syncHistorySelection();
        return;
    }
    if (b.newSet) b.newSet->knobCancel();
    if (b.historyFromCategories && b.source.isValid()) {
        activateBrowseSource(); // the Library's own view again (Mixes reads it)
        showCategories();
        return;
    }
    showHome(b.homeKey);
}
void WDeckLab::syncHistorySelection(bool restartSettle) {
    // The highlighted history track is the one LOAD and Preview act on:
    // select its row in its playlist's (hidden) table; nothing else is.
    auto& b = *m_browseState;
    using namespace decklab::history;
    if (b.list != DeckLabList::History) return;
    const QListWidgetItem* item = b.items->currentItem();
    const auto kind = item ? static_cast<RowKind>(item->data(RowRole).toInt()) : RowKind::None;
    if (kind != RowKind::NewSet) cancelNewSetKnob(); // r5-fix-history: the highlight left "New set"
    const bool track = historyTrackLevel() && kind == RowKind::Track;
    const int trackId = track ? item->data(TrackIdRole).toInt() : -1;
    // The card follows the highlight at once: the row's own facts until the
    // Track object is read, never the track highlighted before.
    if (!b.selectedTrack || b.selectedTrack->getId().toVariant().toInt() != trackId) b.selectedTrack.reset();
    // r5-fix-history: select at once only when the row's playlist is already
    // in the table; switching it waits for the knob to settle (the timer).
    const int row = track ? selectHistoryRow() : -1;
    if (!track) {
        auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
        if (table && table->selectionModel()) table->selectionModel()->clearSelection();
    }
    if (row < 0) b.selectedTrack.reset();
    // r5-fix2-history: a knob step restarts the settle; a resync never
    // postpones one already running.
    if (track && (restartSettle || !b.historySelectTimer.isActive())) b.historySelectTimer.start();
    refreshLoadButtons();
    refreshBrowseCard();
}
int WDeckLab::selectHistoryRow() {
    // r5-fix-history: the highlighted history track's row in the table when
    // the table shows its playlist (by position and library id); otherwise
    // nothing is selected, so LOAD and Preview can act on nothing else.
    auto& b = *m_browseState;
    using namespace decklab::history;
    const QListWidgetItem* item = b.items->currentItem();
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    auto* tracks = table ? dynamic_cast<TrackModel*>(table->model()) : nullptr;
    const auto* playlist = table ? dynamic_cast<PlaylistTableModel*>(table->model()) : nullptr;
    int row = -1;
    if (item && tracks && playlist && playlist->getPlaylist() == item->data(PlaylistRole).toInt()) {
        const QAbstractItemModel& model = *table->model();
        row = modelRowFor(model, tracks->columnIndexFromSortColumnId(TrackModel::SortColumnId::Position),
                item->data(PositionRole).toInt(), item->data(TrackIdRole).toInt(),
                [tracks, &model](int at) { return tracks->getTrackId(model.index(at, 0)).toVariant().toInt(); });
    }
    if (table && table->selectionModel()) {
        if (row >= 0) {
            table->selectionModel()->setCurrentIndex(table->model()->index(row, 0),
                    QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        } else {
            table->selectionModel()->clearSelection();
        }
    }
    return row;
}
bool WDeckLab::browseRowLoadable() {
    // r5-fix-history: LOAD and Preview read the hidden table's selected row.
    // On History and Mixes it must be the highlighted row's own track: the
    // set's playlist with the row's library id, or the Mixes row's index.
    if (!m_browseState || !browseTrackLevel()) return false;
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    const QModelIndexList rows = table && table->selectionModel() ? table->selectionModel()->selectedRows() : QModelIndexList();
    if (m_browseStack->currentIndex() != 0) return !rows.isEmpty(); // a track table: its own highlight
    if (rows.size() != 1) return false;
    const auto& b = *m_browseState;
    const QListWidgetItem* item = b.items->currentItem();
    if (!item) return false;
    if (b.list == DeckLabList::History) {
        const auto* playlist = dynamic_cast<PlaylistTableModel*>(table->model());
        return playlist && playlist->getPlaylist() == item->data(decklab::history::PlaylistRole).toInt() &&
                playlist->getTrackId(rows.first()).toVariant().toInt() == item->data(decklab::history::TrackIdRole).toInt();
    }
    if (b.list == DeckLabList::Mixes) {
        const QModelIndex source = mixTableRow(item); // r5-fix3-history: by the row's file
        return source.isValid() && source.row() == rows.first().row();
    }
    return false;
}
bool WDeckLab::listRowStale() {
    // A History or Mixes row whose table row is not (yet) its own track.
    return m_browseStack && m_browseStack->currentIndex() == 0 && !browseRowLoadable();
}
void WDeckLab::resyncBrowseRow() {
    if (!m_browseState) return;
    if (m_browseState->list == DeckLabList::History) syncHistorySelection(false);
    else if (m_browseState->list == DeckLabList::Mixes) syncMixSelection();
}
void WDeckLab::setTrackLevel(bool loadable) {
    // r5-fix-history: [DeckLabNative] track_level tells a controller's LOAD
    // (the appliance overlay) that the highlighted list row is a loadable track.
    // Read through the key: the Browse refresh runs before the control exists.
    static const ConfigKey key(kNative, QStringLiteral("track_level"));
    const double level = loadable ? 1 : 0;
    if (ControlObject::exists(key) && ControlObject::get(key) != level) ControlObject::set(key, level);
}
void WDeckLab::historyTableChanged() {
    // r5-fix-history: Mixxx showed another model in the hidden table (a
    // rekordbox read finishing, say). Under a History or Mixes row, LOAD and
    // Preview refuse until the row's own table and row are back.
    if (!m_browseState || !m_browseStack || m_browseStack->currentIndex() != 0) return;
    auto& b = *m_browseState;
    if (b.list != DeckLabList::History && b.list != DeckLabList::Mixes) return;
    // r5-fix2-history: nothing to do when the row is its own track again (the
    // settle's own switch); nothing switches while Browse is hidden.
    if (browseRowLoadable()) return;
    if (m_page != 1) {
        b.browseRowStale = true;
        return;
    }
    resyncBrowseRow();
}
void WDeckLab::historyTableShown() {
    // r5-fix2-history: Mixxx just showed a model in the hidden table. At once:
    // a selection that is not the highlighted track goes (a list level never
    // keeps one; a History or Mixes row keeps only its own), and track_level
    // with it, so a LOAD already on its way finds nothing; then the row comes
    // back, queued.
    if (!m_browseState || !m_browseStack) return;
    hookTrackLevel();
    if (m_browseStack->currentIndex() == 0) {
        const auto list = m_browseState->list;
        if (!browseTrackLevel()) clearTableSelection(true);
        else if (!browseRowLoadable()) clearTableSelection(false);
        if (list == DeckLabList::History || list == DeckLabList::Mixes) {
            QMetaObject::invokeMethod(this, [this] { historyTableChanged(); }, Qt::QueuedConnection);
        }
    }
    updateTrackLevel();
}
void WDeckLab::flushBrowseRow() {
    // r5-fix2-history: a deliberate press (LOAD, Preview, a controller's
    // LOAD) shows the knob has settled: a History or Mixes row still waiting
    // for its table gets it now, once (never per knob step).
    // r5-fix3-history: on any page, as a CDJ loads from every screen: one
    // table switch at press time.
    if (!m_browseState || !m_browseStack || m_browseStack->currentIndex() != 0) return;
    if (browseRowLoadable()) return;
    auto& b = *m_browseState;
    if (b.list == DeckLabList::History && historyTrackLevel()) {
        b.historySelectTimer.stop();
        settleHistoryRow(true);
    } else if (b.list == DeckLabList::Mixes) {
        syncMixSelection(true);
    }
    if (browseRowLoadable()) b.browseRowStale = false;
}
bool WDeckLab::browseLoadableNow() {
    // r5-fix2-history: may LOAD take the table's selected row now? Exactly
    // when Browse highlights a loadable track and that row is it.
    flushBrowseRow();
    const bool loadable = browseTrackLevel() && browseRowLoadable();
    setTrackLevel(loadable);
    return loadable;
}
bool WDeckLab::listTrackRow() const {
    // A History or Mixes track row is highlighted (loadable or not yet).
    if (!m_browseState || !m_browseStack || m_browseStack->currentIndex() != 0) return false;
    const QListWidgetItem* item = m_browseState->items->currentItem();
    if (!item) return false;
    if (m_browseState->list == DeckLabList::History) {
        return historyTrackLevel() && static_cast<decklab::history::RowKind>(item->data(decklab::history::RowRole).toInt()) ==
                decklab::history::RowKind::Track;
    }
    return m_browseState->list == DeckLabList::Mixes && item->data(DeckLabListDelegate::MixRole).toBool() &&
            !item->data(decklab::history::LocationRole).toString().isEmpty(); // r5-fix3-history: its file
}
void WDeckLab::updateTrackLevel() {
    // r5-fix2-history: [DeckLabNative] track_level is 1 exactly while Browse
    // highlights a loadable track and the hidden table's selected row is it,
    // on every page (a controller's LOAD, empty-deck PLAY and empty pad read it).
    if (!m_browseState) return;
    hookTrackLevel();
    setTrackLevel(browseTrackLevel() && browseRowLoadable());
}
void WDeckLab::hookTrackLevel() {
    // The hidden table's selection (and its model's resets and removals)
    // keeps track_level exact between Deck Lab's own refreshes; followed
    // again whenever the table gets another model.
    auto& b = *m_browseState;
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    QItemSelectionModel* selection = table ? table->selectionModel() : nullptr;
    if (selection == b.levelSelection.data()) return;
    for (const auto& connection : std::as_const(b.levelConnections)) disconnect(connection);
    b.levelConnections.clear();
    b.levelSelection = selection;
    if (!selection) return;
    const auto update = [this] { updateTrackLevel(); };
    b.levelConnections << connect(selection, &QItemSelectionModel::selectionChanged, this, update);
    if (const QAbstractItemModel* model = selection->model()) {
        b.levelConnections << connect(model, &QAbstractItemModel::modelReset, this, update)
                           << connect(model, &QAbstractItemModel::rowsRemoved, this, update)
                           << connect(model, &QAbstractItemModel::layoutChanged, this, update);
    }
}
void WDeckLab::clearTableSelection(bool remember) {
    // r5-fix2-history: no selection under a level that highlights no track.
    // A track table's row is remembered: it comes back with its table.
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    if (!table || !table->selectionModel()) return;
    const QModelIndexList rows = table->selectionModel()->selectedRows();
    if (rows.isEmpty()) return;
    if (remember) m_browseState->listClearedRow = QPersistentModelIndex(rows.first());
    table->selectionModel()->clearSelection();
}
void WDeckLab::restoreTableSelection() {
    // r5-fix2-history: the DJ's place in a track table, unselected while a
    // list level was shown, as when the selection was simply kept.
    if (!m_browseState) return;
    const QPersistentModelIndex row = std::exchange(m_browseState->listClearedRow, QPersistentModelIndex());
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    if (!table || !table->selectionModel() || !row.isValid() || row.model() != table->model() ||
            !table->selectionModel()->selectedRows().isEmpty()) {
        return;
    }
    table->selectionModel()->select(QModelIndex(row), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
}
bool WDeckLab::mixTableForeign() const {
    // r5-fix2-history: the hidden table shows another model than the one the
    // Mixes rows (and their cache) were read from, for the same source.
    const auto& b = *m_browseState;
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    return table && table->model() && b.mixModel && table->model() != b.mixModel && b.source.isValid() &&
            b.mixSource == b.source;
}
bool WDeckLab::ensureMixTable(bool pressed) {
    // r5-fix2-history: show the Mixes source again when another table was
    // swapped in: on Browse only, once per swap. True when its table shows.
    // r5-fix3-history: a deliberate load does it on any page, every time.
    auto& b = *m_browseState;
    if (!mixTableForeign()) {
        b.mixReactivated = false;
        return true;
    }
    if (m_page != 1 && !pressed) {
        b.browseRowStale = true;
        return false;
    }
    if (b.mixReactivated && !pressed) return false;
    b.mixReactivated = true;
    activateBrowseSource();
    if (mixTableForeign()) return false;
    b.mixReactivated = false;
    return true;
}
QModelIndex WDeckLab::mixTableRow(const QListWidgetItem* item) const {
    // r5-fix3-history: a Mixes row is its track's file (LocationRole); the
    // index kept with it is only a shortcut into the source's table, checked
    // against that file. Never a row of another model; invalid when the index
    // went (a select() on the model: syncMixSelection reads the rows again).
    if (!item || !item->data(DeckLabListDelegate::MixRole).toBool()) return {};
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    const auto* tracks = table ? dynamic_cast<const TrackModel*>(table->model()) : nullptr;
    const QString location = item->data(decklab::history::LocationRole).toString();
    if (!tracks || location.isEmpty()) return {};
    const QPersistentModelIndex cached = item->data(DeckLabListDelegate::MixIndexRole).value<QPersistentModelIndex>();
    if (!cached.isValid() || cached.model() != table->model() || tracks->getTrackLocation(cached) != location) return {};
    return QModelIndex(cached);
}
bool WDeckLab::onNewSetRow() const {
    if (!m_browseState || !m_browseStack || m_browseStack->currentIndex() != 0 ||
            m_browseState->list != DeckLabList::History || m_browseState->historySession >= 0) {
        return false;
    }
    const QListWidgetItem* item = m_browseState->items->currentItem();
    return item && (item->flags() & Qt::ItemIsEnabled) &&
            static_cast<decklab::history::RowKind>(item->data(decklab::history::RowRole).toInt()) ==
            decklab::history::RowKind::NewSet;
}
void WDeckLab::historyKnobHold(bool down) {
    // r5-fix-history: browse_hold from a controller that reports the held
    // press (Numark, SB2). Only a hold Browse owns, on "New set", counts.
    auto& b = *m_browseState;
    if (!b.newSet) return;
    if (!down) {
        if (!b.newSetKnobHeld) return;
        b.newSetKnobHeld = false;
        b.newSet->knobHold(false); // a sweep still running snaps back
        // The gesture's release press follows: after a full hold it starts
        // the set; after a short one it is a press of "press twice".
        b.newSetConfirmOnPress = b.newSetKnobDone;
        b.newSetKnobDone = false;
        b.newSetReleasedAt = m_clock.elapsed();
        b.newSetReleaseTimer.start();
        return;
    }
    b.newSetKnobDone = false;
    b.newSetConfirmOnPress = false;
    b.newSetReleasedAt = -100000;
    if (m_page != 1 || !onNewSetRow() || !decklab::knobOwner().browse() ||
            decklab::history::Store::instance().currentCount() <= 0) {
        return; // a menu, dialog, sheet or the wizard owns this hold, or nothing to end
    }
    b.newSetKnobHeld = true;
    // Armed by a first press: this press completes "press twice" on release.
    if (!b.newSet->armed()) b.newSet->knobHold(true);
}
bool WDeckLab::consumeNewSetRelease() {
    // r5-fix-history: the press that ends a knob hold on "New set". After a
    // full hold it starts the set and goes no further (it never opens the
    // new set); after a short hold it acts as a press on the row.
    auto& b = *m_browseState;
    if (b.newSetReleasedAt < 0) return false;
    const bool confirm = b.newSetConfirmOnPress && m_clock.elapsed() - b.newSetReleasedAt < 500 && onNewSetRow();
    b.newSetReleasedAt = -100000;
    b.newSetConfirmOnPress = false;
    b.newSetReleaseTimer.stop();
    if (!confirm) return false;
    startNewHistorySet();
    return true;
}
void WDeckLab::cancelNewSetKnob() {
    // r5-fix-history: a turn, Back, the highlight leaving the row or the end
    // of History calls a knob hold (or a first press) off.
    if (!m_browseState || !m_browseState->newSet) return;
    auto& b = *m_browseState;
    const bool pending = b.newSetKnobHeld || b.newSetKnobDone || b.newSetConfirmOnPress || b.newSetReleasedAt >= 0;
    if (!pending && !b.newSet->armed() && !(b.newSetKnobHeld && b.newSet->holding())) return;
    b.newSetKnobHeld = false;
    b.newSetKnobDone = false;
    b.newSetConfirmOnPress = false;
    b.newSetReleasedAt = -100000;
    b.newSetReleaseTimer.stop();
    b.newSet->knobCancel();
    if (b.newSet->result() == tr("Let go to start")) b.newSet->setResult(QString());
    refreshNewSetRow();
}
QStringList WDeckLab::historySegments() const {
    const auto& b = *m_browseState;
    using namespace decklab::history;
    const auto& store = Store::instance();
    QStringList path;
    if (b.historyFromCategories) path << tr("Library");
    path << tr("History");
    const Session* session = b.historySession >= 0 ? store.session(b.historySession) : nullptr;
    if (session) {
        // "This set · 14", "Yesterday 21:04 · 37" ("Set 12 · 37" without a clock).
        const QDateTime start = session->first.isValid() ? session->first : session->created;
        const QString name = session->current || !store.clockTrusted()
                ? sessionTitle(*session, store.clockTrusted())
                : QStringLiteral("%1 %2").arg(dayLabel(start.date(), QDate::currentDate()), clockText(start));
        path << QStringLiteral("%1 · %2").arg(name, QLocale().toString(session->count));
    }
    return path;
}
void WDeckLab::refreshHistoryCard() {
    auto& b = *m_browseState;
    using namespace decklab::history;
    const auto& store = Store::instance();
    const bool trusted = store.clockTrusted();
    const QListWidgetItem* item = b.items->currentItem();
    const auto kind = item ? static_cast<RowKind>(item->data(RowRole).toInt()) : RowKind::None;
    const QString joins = tr("A track joins when it plays on a deck the room hears");
    if (historyTrackLevel()) {
        if (b.selectedTrack) {
            b.card->showTrack(b.selectedTrack, kind == RowKind::Track ? item->data(KeyRole).toString() : QString()); // r6-keys
            return;
        }
        if (kind != RowKind::Track) {
            const Session* session = store.session(b.historySession);
            b.card->showItem(decklab::Glyph::History, session ? sessionTitle(*session, trusted) : tr("History"),
                    session ? sessionDetail(*session, trusted) : QString(), joins);
            return;
        }
        // Until the Track object is read (or without a table): what the row says.
        QStringList facts;
        if (!item->data(ArtistRole).toString().isEmpty()) facts << item->data(ArtistRole).toString();
        if (!item->data(KeyRole).toString().isEmpty()) facts << decklab::displayKey(item->data(KeyRole).toString()); // r6-keys
        if (item->data(BpmRole).toDouble() > 0) facts << QLocale().toString(item->data(BpmRole).toDouble(), 'f', 1);
        const int deck = item->data(DeckRole).toInt();
        const QString when = trusted ? tr("Played %1").arg(item->data(TimeRole).toString())
                                     : tr("Track %1 of the set").arg(b.items->currentRow() + 1);
        b.card->showItem(decklab::Glyph::History, item->text(), facts.join(QStringLiteral(" · ")),
                deck > 0 ? tr("%1 on deck %2").arg(when).arg(deck) : when,
                deck > 0 ? deckLabDeckColor(deck) : QColor());
        return;
    }
    if (kind == RowKind::NewSet) {
        const bool armed = b.newSet && b.newSet->armed();
        b.card->showItem(decklab::Glyph::Retry, item->text(),
                b.newSetKnobDone ? tr("Let go to start it")
                        : armed  ? tr("Press again to start it")
                                 : tr("Ends this set. It stays in History."),
                tr("Hold the knob and let go, or press it twice"));
        return;
    }
    if (kind != RowKind::Session || !(item->flags() & Qt::ItemIsEnabled)) {
        // Not a set: say what the history holds, from the store.
        const Session* current = store.session(store.currentId());
        if (current) {
            b.card->showItem(decklab::Glyph::History, sessionTitle(*current, trusted), sessionDetail(*current, trusted),
                    current->count > 0 ? tr("Press or tap to open") : joins);
        } else {
            b.card->showItem(decklab::Glyph::History, tr("History"), tr("Nothing played yet"), joins);
        }
        return;
    }
    b.card->showItem(decklab::Glyph::History, item->text(), item->data(DetailRole).toString(),
            tr("Press or tap to open"));
}
void WDeckLab::applyHistoryPresentation() {
    auto& b = *m_browseState;
    // Read once, when Browse is first shown (never at start-up).
    if (m_page == 1) {
        decklab::history::Store::instance().ensureLoaded();
        askHistoryClock();
    }
    const bool history = m_browseStack && m_browseStack->currentIndex() == 0 && b.list == DeckLabList::History;
    if (history && m_page == 1 && b.historyStale) {
        // Tracks that played while another page was shown join now.
        fillHistory(true);
        QTimer::singleShot(0, this, [this] {
            syncHistorySelection();
            updateBreadcrumb();
        });
    }
    // r5-fix2-history: LOAD's table selection exists only under a highlighted
    // track. Home, Sources, Categories, Values, Folder and the sets list
    // unselect it (its track table gets the row back), and a History or Mixes
    // row whose table changed while Browse was hidden gets it back now.
    if (m_browseStack && m_browseStack->currentIndex() == 0) {
        if (!browseTrackLevel()) {
            clearTableSelection(true);
            b.browseRowStale = false;
        } else if (m_page == 1 && b.browseRowStale) {
            b.browseRowStale = false;
            if (!browseRowLoadable()) resyncBrowseRow();
        }
    }
    updateTrackLevel();
    if (!b.newSet) return;
    if (!history) {
        cancelNewSetKnob(); // r5-fix-history
        if (!b.newSet->result().isEmpty()) b.newSet->setResult(QString());
    }
    setVisibleIf(b.newSet, history);
}
void WDeckLab::decorateHistoryEntry(decklab::SourceEntry& entry) {
    // The home's History chip counts this set's tracks ("History 14").
    const auto& store = decklab::history::Store::instance();
    if (!store.loaded()) {
        entry.count.clear();
        return;
    }
    const int count = store.currentCount();
    const int earlier = std::max(0, static_cast<int>(store.sessions().size()) - 1);
    entry.count = count > 0 ? QLocale().toString(count) : QString();
    entry.line1 = count > 0 ? tr("This set · %1").arg(countText(count, tr("1 track"), tr("%1 tracks")))
                            : tr("Nothing played in this set yet");
    entry.line2 = earlier > 0 ? countText(earlier, tr("1 earlier set"), tr("%1 earlier sets")) : QString();
}
void WDeckLab::updateHomeHistoryTile() {
    // The home's History tile or chip, in place: one item, not the home.
    auto& b = *m_browseState;
    if (b.itemsList != DeckLabList::Home || !b.items) return;
    for (int row = 0; row < b.items->count(); ++row) {
        QListWidgetItem* item = b.items->item(row);
        if (item->data(DeckLabListDelegate::KeyRole).toString() != QStringLiteral("root:history")) continue;
        decklab::SourceEntry entry;
        entry.line1 = item->data(DeckLabListDelegate::SubtitleRole).toString();
        entry.line2 = item->data(DeckLabListDelegate::DetailRole).toString();
        decorateHistoryEntry(entry);
        const bool chip = item->data(DeckLabListDelegate::TileRole).toInt() == 2;
        const QString count = chip ? entry.count : QString();
        const bool resized = count != item->data(DeckLabListDelegate::CountRole).toString();
        item->setData(DeckLabListDelegate::SubtitleRole, entry.line1);
        item->setData(DeckLabListDelegate::DetailRole, entry.line2);
        item->setData(DeckLabListDelegate::CountRole, count);
        item->setData(Qt::AccessibleTextRole, QString(item->text() + QStringLiteral(", ") + entry.line1));
        if (resized && b.items->isVisible()) b.items->doItemsLayout(); // a chip's width follows its count
        return;
    }
}
void WDeckLab::startNewHistorySet() {
    auto& b = *m_browseState;
    auto& store = decklab::history::Store::instance();
    if (!store.startNewSet()) {
        if (b.toast) {
            b.toast->showMessage(store.currentCount() == 0 ? tr("This set is still empty") : tr("Couldn't start a new set"), 2400);
        }
        return;
    }
    if (b.newSet) b.newSet->setResult(tr("New set started"));
    // r5-fix3-history: a start that could not be written stays for this
    // session; say so (a restart may join the two sets).
    if (b.toast) {
        b.toast->showMessage(store.startsSaved()
                        ? tr("New set started. The last one stays in History.")
                        : tr("New set started, but it couldn't be saved: a restart may join it to the last set."),
                2600);
    }
    // The sets, on the new one, the last set listed below it: the store's
    // change refills the list once (queued).
    if (b.list == DeckLabList::History) {
        b.historySession = -1;
        b.historyKeepSession = store.currentId();
        const QSignalBlocker blocker(b.items);
        b.items->setCurrentRow(-1); // the refill lands on the new set
    }
}
void WDeckLab::historyChanged(const decklab::history::Change& change) {
    // Coalesced: one queued pass however many changes arrive together.
    auto& b = *m_browseState;
    b.historyPending.locations.unite(change.locations);
    b.historyPending.history = b.historyPending.history || change.history;
    b.historyPending.reloaded = b.historyPending.reloaded || change.reloaded;
    b.historyPending.added += change.added;
    if (b.historyQueued) return;
    b.historyQueued = true;
    QTimer::singleShot(0, this, [this] { flushHistoryChange(); });
}
void WDeckLab::flushHistoryChange() {
    auto& b = *m_browseState;
    using namespace decklab::history;
    b.historyQueued = false;
    const Change change = std::exchange(b.historyPending, Change());
    const bool shown = m_page == 1;
    // Only rows whose mark changed, and only while Browse is on screen.
    if (shown) repaintPlayed(change.locations);
    if (!change.history) return;
    updateHomeHistoryTile();
    if (!m_browseStack || m_browseStack->currentIndex() != 0 || b.list != DeckLabList::History ||
            b.itemsList != DeckLabList::History) {
        return;
    }
    if (!shown) {
        b.historyStale = true; // no work on a hidden page: the next visit refills
        return;
    }
    auto& store = Store::instance();
    const Session* current = store.session(store.currentId());
    const bool trusted = store.clockTrusted();
    if (change.reloaded || b.historyStale || !current) {
        fillHistory(true);
        syncHistorySelection();
        updateBreadcrumb();
        return;
    }
    // A track joined this set: change only what shows it.
    if (b.historySession < 0) {
        for (int row = 0; row < b.items->count(); ++row) {
            QListWidgetItem* item = b.items->item(row);
            if (static_cast<RowKind>(item->data(RowRole).toInt()) == RowKind::Session &&
                    item->data(SessionRole).toInt() == current->id) {
                updateSessionItem(item, *current, trusted);
                break;
            }
        }
        refreshNewSetRow();
        refreshBrowseCard();
    } else if (b.historySession == current->id) {
        const QVector<Entry> entries = store.entries(current->id);
        int tracks = 0;
        for (int row = 0; row < b.items->count(); ++row) {
            tracks += static_cast<RowKind>(b.items->item(row)->data(RowRole).toInt()) == RowKind::Track;
        }
        if (tracks == 0 || tracks > entries.size()) {
            fillHistory(true); // the "Nothing played yet" row goes
            syncHistorySelection();
        } else {
            const bool onLatest = b.items->currentRow() == b.items->count() - 1;
            for (int index = tracks; index < entries.size(); ++index) {
                appendTrack(b.items, entries.mid(0, index + 1), true, trusted);
            }
            // The highlight follows the newest track only from the latest one.
            if (onLatest && b.items->count() > 0) {
                b.items->setCurrentRow(b.items->count() - 1);
                b.items->scrollToItem(b.items->item(b.items->count() - 1));
            }
        }
    }
    updateBreadcrumb();
}
void WDeckLab::repaintPlayed(const QSet<QString>& locations) {
    if (locations.isEmpty() || !m_browseState || !m_browseStack) return;
    auto& b = *m_browseState;
    // The track table: its visible rows, and only those whose mark changed.
    auto* table = m_libraryWidget && m_browseStack->currentWidget() == m_libraryWidget
            ? m_libraryWidget->getCurrentTrackTableView()
            : nullptr;
    auto* tracks = table ? dynamic_cast<TrackModel*>(table->model()) : nullptr;
    if (tracks && table->model()->rowCount() > 0) {
        const int rows = table->model()->rowCount();
        const int first = std::max(0, table->rowAt(0));
        int last = table->rowAt(table->viewport()->height() - 1);
        if (last < 0) last = rows - 1;
        for (int row = first; row <= last && row < rows; ++row) {
            if (locations.contains(tracks->getTrackLocation(table->model()->index(row, 0)))) {
                table->viewport()->update(QRect(0, table->rowViewportPosition(row), table->viewport()->width(), table->rowHeight(row)));
            }
        }
    }
    // The Browse list: Mixes and History rows carry their file.
    if (m_browseStack->currentIndex() == 0 && b.items->isVisible()) {
        for (int row = 0; row < b.items->count(); ++row) {
            const QListWidgetItem* item = b.items->item(row);
            if (locations.contains(item->data(decklab::history::LocationRole).toString())) {
                b.items->viewport()->update(b.items->visualItemRect(item));
            }
        }
    }
}

// r7-browse: the DOWNLOADS and SAMPLES tiles (decklab/decklabdownloads.h),
// their lists in the folder table (library/decklabfoldertablemodel.h: library
// rows in a given order), a pad's Load on SAMPLES, and the background
// analysis of new downloads (decklab/decklabanalysis.h).
QStringList WDeckLab::currentDownloadFolders() const {
    QStringList sticks;
    for (const auto& stick : decklab::findUsbSticks()) sticks << stick.path;
    return decklab::downloadFolders(sticks);
}
QString WDeckLab::downloadsDetail() const {
    const QString progress = m_analysis ? m_analysis->progressText() : QString();
    return progress.isEmpty() ? tr("Newest first") : progress;
}
decklab::SourceEntry WDeckLab::downloadsEntry(const QStringList& folders) const {
    decklab::SourceEntry entry;
    entry.key = QStringLiteral("downloads");
    entry.name = tr("Downloads");
    entry.glyph = decklab::Glyph::Download;
    // r7: counted from names alone: a stick plugged in mid-set shows its
    // tile without a stat of every download on it.
    const decklab::DownloadCounts counts = decklab::countDownloads(folders);
    const int tracks = counts.tracks;
    const int playlists = counts.playlists;
    QStringList parts;
    if (tracks) parts << countText(tracks, tr("1 track"), tr("%1 tracks"));
    if (playlists) parts << countText(playlists, tr("1 playlist"), tr("%1 playlists"));
    entry.line1 = parts.isEmpty() ? tr("Nothing downloaded yet") : parts.join(QStringLiteral(" · "));
    entry.line2 = downloadsDetail();
    if (tracks) entry.count = QLocale().toString(tracks);
    return entry;
}
decklab::SourceEntry WDeckLab::samplesEntry() const {
    decklab::SourceEntry entry;
    entry.key = QStringLiteral("samples");
    entry.name = tr("Samples");
    entry.glyph = decklab::Glyph::Sample;
    const int count = static_cast<int>(decklab::audioFilesNewestFirst(decklab::samplesFolder()).size());
    entry.line1 = count ? countText(count, tr("1 sample"), tr("%1 samples")) : tr("No samples yet");
    entry.line2 = tr("For the sampler pads");
    if (count) entry.count = QLocale().toString(count);
    return entry;
}
void WDeckLab::refreshDownloadsTile() {
    // The analysis moved on: only the DOWNLOADS tile's second line changes.
    if (!m_browseState || !m_browseState->items || m_browseState->itemsList != DeckLabList::Home) return;
    auto& b = *m_browseState;
    const QString detail = downloadsDetail();
    for (int row = 0; row < b.items->count(); ++row) {
        auto* item = b.items->item(row);
        if (item->data(DeckLabListDelegate::KeyRole).toString() == QStringLiteral("downloads") &&
                item->data(DeckLabListDelegate::DetailRole).toString() != detail) {
            item->setData(DeckLabListDelegate::DetailRole, detail);
        }
    }
}
void WDeckLab::showDownloads(const QString& highlight) {
    if (!m_browseState || !m_sidebar) return;
    auto& b = *m_browseState;
    resetBrowseLevels();
    b.homeKey = QStringLiteral("downloads");
    b.list = DeckLabList::Downloads;
    b.downloadsKey = highlight;
    b.downloadsStale = false;
    // Single downloads (one row, newest first), then each Spotify playlist
    // as a folder, newest first. A stick that is not plugged in adds nothing.
    const auto entries = decklab::listDownloads(currentDownloadFolders());
    int singles = 0;
    for (const auto& entry : entries) singles += entry.folder ? 0 : 1;
    const auto saved = [this](const QDateTime& when) {
        const QDate day = when.date();
        const QDate today = QDate::currentDate();
        if (day == today) return tr("saved today");
        if (day == today.addDays(-1)) return tr("saved yesterday");
        return tr("saved %1").arg(QLocale().toString(day,
                day.year() == today.year() ? QStringLiteral("d MMM") : QStringLiteral("d MMM yyyy")));
    };
    {
        const QSignalBlocker blocker(b.items);
        b.items->clear();
        int current = 0;
        if (singles) {
            auto* item = new QListWidgetItem(tr("Tracks"), b.items);
            item->setData(DeckLabListDelegate::KeyRole, QStringLiteral("tracks"));
            item->setData(DeckLabListDelegate::GlyphRole, static_cast<int>(decklab::Glyph::Tracks));
            item->setData(DeckLabListDelegate::CountRole, QString::number(singles));
            item->setData(DeckLabListDelegate::SubtitleRole, tr("Single downloads, newest first"));
            item->setData(DeckLabListDelegate::ChevronRole, true);
        }
        for (const auto& entry : entries) {
            if (!entry.folder) continue;
            const QString key = QStringLiteral("folder:") + entry.path;
            auto* item = new QListWidgetItem(entry.name, b.items);
            item->setData(DeckLabListDelegate::KeyRole, key);
            item->setData(DeckLabListDelegate::GlyphRole, static_cast<int>(decklab::Glyph::Playlist));
            item->setData(DeckLabListDelegate::CountRole, QString::number(entry.tracks));
            item->setData(DeckLabListDelegate::SubtitleRole, tr("Playlist · %1").arg(saved(entry.arrived)));
            item->setData(DeckLabListDelegate::ChevronRole, true);
            if (key == highlight) current = b.items->count() - 1;
        }
        if (!b.items->count()) {
            auto* item = new QListWidgetItem(tr("Nothing downloaded yet"), b.items);
            item->setData(DeckLabListDelegate::SubtitleRole, tr("Get Music saves here, on a USB stick or the Pi card"));
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        }
        b.items->setCurrentRow(current);
    }
    b.itemsList = DeckLabList::Downloads;
    showSources();
}
void WDeckLab::openDownloadsRow(int rowIndex) {
    auto& b = *m_browseState;
    auto* item = b.items->item(rowIndex);
    if (!item || !(item->flags() & Qt::ItemIsEnabled)) return;
    const QString key = item->data(DeckLabListDelegate::KeyRole).toString();
    b.downloadsKey = key;
    if (key == QStringLiteral("tracks")) {
        showFolderTracks(decklab::FolderList::Tracks, decklab::newestDownloads(currentDownloadFolders()),
                QStringLiteral("downloads:tracks"), tr("Tracks"));
    } else if (key.startsWith(QStringLiteral("folder:"))) {
        const QString folder = key.mid(7);
        if (!QFileInfo(folder).isDir()) {
            // r7 fix: its stick went: say so, and the list loses it.
            if (b.toast) b.toast->showMessage(tr("That USB stick is out."), 2600, true);
            showDownloads(QString());
            return;
        }
        showFolderTracks(decklab::FolderList::Playlist, decklab::playlistOrder(folder),
                QStringLiteral("downloads:") + folder, QFileInfo(folder).fileName());
    }
}
int WDeckLab::showFolderTracks(decklab::FolderList list,
        const QStringList& locations,
        const QString& key,
        const QString& title) {
    auto& b = *m_browseState;
    clearBrowseFilter();
    b.fromCategories = false;
    b.folderList = list;
    b.folderTitle = title;
    int rows = -1;
    if (m_library && m_library->trackCollectionManager()) {
        // The library's own rows, in this order: BPM, key and analysis come
        // from the library, and LOAD, pads and Preview work as anywhere.
        if (!b.folderModel) b.folderModel = new DeckLabFolderTableModel(this, m_library->trackCollectionManager());
        rows = b.folderModel->setLocations(locations, key);
        m_library->slotShowTrackModel(b.folderModel);
    }
    showTracks();
    // r7 fix: what the list leaves out, truthfully. Files join the library
    // with the next library scan (or, a TO PAD sample, once it is quiet).
    const int files = static_cast<int>(locations.size());
    if (rows >= 0 && rows < files && b.toast) {
        const int waiting = files - rows;
        if (list == decklab::FolderList::Samples) {
            b.toast->showMessage(waiting == 1 ? tr("1 new sample joins this list when nothing plays.")
                                              : tr("%1 new samples join this list when nothing plays.").arg(waiting),
                    2600);
        } else if (rows == 0 && !QFileInfo::exists(locations.first())) {
            b.toast->showMessage(tr("That USB stick is out."), 2600, true);
        } else {
            b.toast->showMessage(waiting == 1 ? tr("1 track joins this list when nothing plays.")
                                              : tr("%1 tracks join this list when nothing plays.").arg(waiting),
                    2600);
        }
    } else if (rows == 0 && b.toast) {
        b.toast->showMessage(list == decklab::FolderList::Samples
                        ? tr("No samples yet. Sample Maker and loop capture save them here.")
                        : tr("Nothing in this list."),
                2600);
    }
    return rows;
}
void WDeckLab::openSamples(bool forPad) {
    if (!m_browseState || !m_sidebar) return;
    auto& b = *m_browseState;
    resetBrowseLevels();
    b.homeKey = QStringLiteral("samples");
    const QStringList files = decklab::audioFilesNewestFirst(decklab::samplesFolder());
    if (forPad && m_library && m_library->trackCollectionManager()) {
        // r7 fix: TO PAD's samples join the library once it is quiet (never
        // a library write mid-set); the toasts say how many still wait. A
        // pad's Load opens SAMPLES only when it lists a sample; otherwise
        // the home, to pick from the Library.
        if (m_captureLibrary) m_captureLibrary->flush();
        if (!b.folderModel) b.folderModel = new DeckLabFolderTableModel(this, m_library->trackCollectionManager());
        if (b.folderModel->setLocations(files, QStringLiteral("samples")) == 0) {
            showHome(QStringLiteral("samples"));
            if (b.toast) {
                b.toast->showMessage(files.isEmpty()
                                ? tr("No samples yet. Pick any track for pad %1.").arg(m_sampleTarget)
                                : tr("New samples join the list when nothing plays. Pick any track for pad %1.")
                                          .arg(m_sampleTarget),
                        3200);
            }
            return;
        }
    }
    showFolderTracks(decklab::FolderList::Samples, files, QStringLiteral("samples"), tr("Samples"));
    b.samplesForPad = forPad;
    applyBrowsePresentation();
}
bool WDeckLab::folderTableShown() const {
    if (!m_browseState || !m_browseStack || m_browseStack->currentIndex() == 0) return false;
    const auto& b = *m_browseState;
    if (b.folderList == decklab::FolderList::None) return false;
    // Another table swapped in (a stick read finishing) is not the list.
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    return !b.folderModel || !table || table->model() == b.folderModel;
}
bool WDeckLab::backgroundSettled() {
    return m_system ? m_system->settled() : backgroundQuiet();
}
decklab::Activity WDeckLab::backgroundActivity() {
    // One rule for every background job (decklab/decklabquiet.h); the
    // system also knows the open utility.
    if (m_system) return m_system->activity();
    decklab::Activity activity;
    for (int deck = 1; deck <= 4 && !activity.playing; ++deck) {
        activity.playing = value(channel(deck), QStringLiteral("play")) > 0;
    }
    const int samplers = std::max(16, m_players ? m_players->numberOfSamplers() : 0);
    for (int sampler = 1; sampler <= samplers && !activity.playing; ++sampler) {
        activity.playing = value(QStringLiteral("[Sampler%1]").arg(sampler), QStringLiteral("play")) > 0;
    }
    const int previews = std::max(1, m_players ? m_players->numberOfPreviewDecks() : 0);
    for (int preview = 1; preview <= previews && !activity.playing; ++preview) {
        activity.playing = value(QStringLiteral("[PreviewDeck%1]").arg(preview), QStringLiteral("play")) > 0;
    }
    // [Recording] status: 2 recording, 3 recording into the next split file.
    const int recording = int(value(QStringLiteral("[Recording]"), QStringLiteral("status")));
    activity.recording = recording == 2 || recording == 3;
    activity.scanning = m_libraryScanning ||
            (m_library && m_library->trackCollectionManager() &&
                    m_library->trackCollectionManager()->isLibraryScanRunning());
    return activity;
}
bool WDeckLab::loadedOnAPlayer(TrackId trackId) const {
    if (!m_players || !trackId.isValid()) return false;
    QStringList groups;
    for (int deck = 0; deck < m_players->numberOfDecks(); ++deck) groups << PlayerManager::groupForDeck(deck);
    for (int sampler = 0; sampler < m_players->numberOfSamplers(); ++sampler) groups << PlayerManager::groupForSampler(sampler);
    for (int preview = 0; preview < m_players->numberOfPreviewDecks(); ++preview) groups << PlayerManager::groupForPreviewDeck(preview);
    for (const QString& group : std::as_const(groups)) {
        // A player that is loading already holds its new track.
        auto* player = m_players->getPlayer(group);
        const TrackPointer track = player ? player->getLoadedTrack() : TrackPointer();
        if (track && track->getId() == trackId) return true;
    }
    return false;
}
void WDeckLab::initBackgroundAnalysis() {
    if (m_analysis) return;
    m_analysis = new DeckLabBackgroundAnalysis(this);
    DeckLabBackgroundAnalysis::Hooks hooks;
    hooks.busy = [this] { return !backgroundQuiet(); };
    hooks.loaded = [this](TrackId trackId) { return loadedOnAPlayer(trackId); };
    if (m_library && m_library->trackCollectionManager()) {
        hooks.candidates = [this] {
            m_analysisLocations.clear();
            return DeckLabBackgroundAnalysis::tracksToAnalyze(
                    m_library->trackCollectionManager()->internalCollection()->database(),
                    decklab::allDownloads(currentDownloadFolders()),
                    &m_analysisLocations);
        };
        // r7: a track on a USB stick is let go of, not paused, so the stick
        // can be ejected mid-set.
        hooks.location = [this](TrackId trackId) { return m_analysisLocations.value(trackId); };
    }
    m_analysis->setHooks(hooks);
    if (m_library) m_analysis->useLibrary(m_library, m_config);
    // The moment it is not quiet the analyzer pauses; a quiet spell goes on.
    const auto changed = [this](double) {
        if (m_analysis) m_analysis->playbackChanged();
    };
    for (int deck = 1; deck <= 4; ++deck) watch(channel(deck), QStringLiteral("play"), changed);
    const int samplers = std::max(16, m_players ? m_players->numberOfSamplers() : 0);
    for (int sampler = 1; sampler <= samplers; ++sampler) watch(QStringLiteral("[Sampler%1]").arg(sampler), QStringLiteral("play"), changed);
    const int previews = std::max(1, m_players ? m_players->numberOfPreviewDecks() : 0);
    for (int preview = 1; preview <= previews; ++preview) watch(QStringLiteral("[PreviewDeck%1]").arg(preview), QStringLiteral("play"), changed);
    watch(QStringLiteral("[Recording]"), QStringLiteral("status"), changed);
    connect(m_analysis, &DeckLabBackgroundAnalysis::progressChanged, this, [this] { refreshDownloadsTile(); });
    if (m_library && m_library->trackCollectionManager()) {
        auto* collection = m_library->trackCollectionManager();
        // A library scan holds its write transaction until it ends: the
        // analyzer waits. The quiet rule reads the scanner's own state, so
        // it already counts the scan here (also the start-up scan, which
        // began before this was connected); the signals only say when to
        // decide again.
        m_libraryScanning = collection->isLibraryScanRunning();
        connect(collection, &TrackCollectionManager::libraryScanStarted, this, [this] {
            m_libraryScanning = true;
            if (m_analysis) m_analysis->playbackChanged();
        }, Qt::QueuedConnection);
        // New downloads arrive with a library scan (after Get Music closes).
        connect(collection, &TrackCollectionManager::libraryScanFinished, this, [this] {
            m_libraryScanning = false;
            if (m_analysis) {
                m_analysis->playbackChanged();
                m_analysis->wake();
            }
            markHomeStale(); // r7 fix: new downloads and samples, the Library count
        }, Qt::QueuedConnection);
    }
    m_analysis->wake();
}
