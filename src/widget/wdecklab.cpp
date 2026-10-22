// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/wdecklab.h"
#include "widget/decklabcombodelegate.h"
#include "widget/decklabdeckwidgets.h"

#include <QApplication>
#include <QBoxLayout>
#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QEvent>
#include <QGridLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLabel>
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
#include "effects/effectsmanager.h"
#include "effects/effectchain.h"
#include "effects/effectslot.h"
#include "library/basetracktablemodel.h"
#include "library/library.h"
#include "library/sidebarmodel.h"
#include "mixer/basetrackplayer.h"
#include "mixer/playermanager.h"
#include "track/keyutils.h"
#include "track/cueinfo.h"
#include "track/track.h"
#include "track/beats.h"
#include "waveform/waveformwidgetfactory.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarysidebar.h"
#include "widget/wsearchlineedit.h"
#include "widget/wtracktableview.h"
#include "moc_wdecklab.cpp"

namespace {
const QString kNative = QStringLiteral("[DeckLabNative]");
const QString kTab = QStringLiteral("[Tab]");
const QString kStyle = QStringLiteral(R"(
 WDeckLab {background:#101010;color:#eeeeee;font-family:"Open Sans",sans-serif;font-size:13px;}
 WDeckLab QWidget {color:#eeeeee;font-size:13px;}
 WDeckLab QPushButton {background:#202020;border:1px solid #444;border-radius:2px;padding:5px 9px;min-height:28px;}
 WDeckLab QPushButton:pressed {background:#383838;}
 WDeckLab QPushButton:checked {background:#303030;border-color:#d74448;}
 WDeckLab QPushButton[transport="true"] {border-radius:0;border-width:1px 1px 1px 0;}
 WDeckLab QPushButton[transport="true"][first="true"] {border-left-width:1px;}
 WDeckLab QPushButton[actionFeedback="true"] {border-color:#d74448;background:#303030;}
 WDeckLab QPushButton:disabled {color:#777;border-color:#303030;}
 WDeckLab QPushButton[rail="true"] {color:#aaa;border-width:0px 1px 1px 0px;border-radius:0px;font-weight:600;}
 WDeckLab QPushButton[rail="true"]:checked {color:#eee;background:#111;border-bottom:2px solid #d74448;}
 WDeckLab QLabel[muted="true"] {color:#aaa;font-size:12px;}
 WDeckLab QLabel[deck="true"] {font-size:15px;font-weight:600;}
 WDeckLab QLabel[numbers="true"] {font-size:20px;font-weight:600;}
 WDeckLab QLabel[deckIdentity="true"] {font-size:22px;font-weight:600;border-right:1px solid #555;}
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
    if (!std::isfinite(size) || size <= 0) return QStringLiteral("—");
    const double reciprocal = 1.0 / size;
    return size < 1 && std::isfinite(reciprocal) && reciprocal <= 1000000 &&
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
            painter.setPen(QColor(isEnabled() ? "#aaaaaa" : "#777777"));
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

WDeckLab::WDeckLab(UserSettingsPointer config, Library* library,
        PlayerManagerInterface* players, EffectsManager* effects, QWidget* parent)
        : QWidget(parent), WBaseWidget(this), m_config(config), m_library(library),
          m_players(players), m_effects(effects) {
    setObjectName(QStringLiteral("DeckLabNative"));
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
    m_root = column(this);
    auto* rail = row();
    const QStringList names = {tr("Decks"), tr("Browse"), tr("Sampler"), tr("FX"), tr("Settings")};
    const QStringList triggers = {"overview", "library", "samplers", "effects", "settings"};
    auto* navigation = new QButtonGroup(this);
    for (int page = 0; page < 5; ++page) {
        m_tabs[page] = button(names[page], [this, page] { selectPage(page); }, this,
                QStringLiteral("decklab-tab-%1").arg(page));
        m_tabs[page]->setProperty("rail", true);
        m_tabs[page]->setCheckable(true);
        navigation->addButton(m_tabs[page], page);
        m_tabs[page]->setFixedHeight(40);
        rail->addWidget(m_tabs[page], 1);
        createControl(kTab, triggers[page]);
        watch(kTab, triggers[page], [this, page](double v) { if (v > 0) selectPage(page); });
    }
    auto* usb = button(tr("USB"), [this] { tool(1); }, this, QStringLiteral("decklab-usb"));
    usb->setProperty("rail", true);
    usb->setFixedHeight(40);
    m_toolButtons.append(usb); usb->setEnabled(false);
    rail->addWidget(usb);
    m_root->addLayout(rail);
    m_waveforms = new QWidget(this);
    m_waveLayout = new QGridLayout(m_waveforms);
    m_waveLayout->setContentsMargins(0, 0, 0, 0); m_waveLayout->setSpacing(0);
    m_waveLayout->setRowStretch(0, 1); m_waveLayout->setRowStretch(1, 1);
    m_waveLayout->setColumnStretch(0, 1); m_waveLayout->setColumnStretch(1, 1);
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
    m_status = button(tr("Status"), [this] {
        QMessageBox::information(this, tr("Deck Lab status"), m_status->toolTip());
    }, this, QStringLiteral("decklab-status"));
    m_status->setProperty("rail", true); m_status->setFixedSize(80, 40);
    rail->addWidget(m_status);
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
        for (const auto& key : {"bpm", "local_bpm", "rate_ratio", "rateRange", "visual_key",
                     "sync_enabled", "sync_leader", "keylock", "quantize", "pfl",
                     "loop_enabled", "beatloop_size", "slip_enabled", "duration"}) {
            watch(channel(deck), QString::fromLatin1(key), [this, deck](double) { markDeckDirty(deck); });
        }
        for (int cue = 0; cue < 8; ++cue) {
            for (auto* proxy : {m_deck[deck - 1].cuePosition[cue], m_deck[deck - 1].cueType[cue],
                         m_deck[deck - 1].cueColor[cue]}) {
                proxy->connectValueChanged(this, [this, deck](double) { markDeckDirty(deck); }, Qt::QueuedConnection);
            }
        }
        if (m_players) {
            auto* player = m_players->getPlayer(channel(deck));
            if (player) {
                connect(player, &BaseTrackPlayer::newTrackLoaded, this, [this, deck](TrackPointer) {
                    m_tempos[deck - 1].at = -1;
                    m_shownPosition[deck - 1] = -1;
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
    for (const auto& item : {"headMix", "headSplit"}) {
        watch(QStringLiteral("[Master]"), QString::fromLatin1(item), [this](double) {
            m_fxSummaryDirty = true; markDeckDirty(0);
        });
    }
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
    const auto target = [this, deck, followsLayer] { return followsLayer ? displayedDeck(deck - 1) : deck; };
    const QString prefix = followsLayer ? QStringLiteral("decklab-%1-").arg(deck) : QStringLiteral("decklab-card-%1-").arg(deck);
    auto makeAction = [this, strip, target, prefix, followsLayer](const QString& label, const QString& item, bool checked = false) {
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
        b->setFixedHeight(followsLayer ? 44 : 48);
        b->setProperty("transport", true);
        b->setProperty("first", item == "cue_default" || item == "loop_halve" || item == "quantize" || item == "play");
        b->setStyleSheet(QStringLiteral("min-height:%1px;max-height:%1px;padding:1px 2px;font-size:12px;").arg(followsLayer ? 40 : 44));
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
    if (followsLayer) {
        auto* actions = row(strip);
        auto append = [actions](QPushButton* button, int width) {
            button->setFixedWidth(width); actions->addWidget(button);
        };
        append(makeAction(QString::fromUtf8("½"), QStringLiteral("loop_halve")), 44);
        auto* loop = makeAction(tr("LOOP\n4"), QStringLiteral("beatloop_activate"), true);
        loop->setStyleSheet(QStringLiteral("min-height:40px;max-height:40px;padding:1px 2px;font-size:13px;"));
        append(loop, 44);
        append(makeAction(QString::fromUtf8("×2"), QStringLiteral("loop_double")), 44);
        append(makeAction(tr("QNT\nOFF"), QStringLiteral("quantize"), true), 56);
        append(makeAction(tr("KEY LOCK\nOFF"), QStringLiteral("keylock"), true), 72);
        append(makeAction(tr("BEAT\nSYNC"), QStringLiteral("beatsync")), 72);
        auto* menu = new QMenu(strip); menu->setObjectName(prefix + QStringLiteral("more-menu"));
        auto* fallback = new QWidget(menu); auto* fallbackRows = column(fallback);
        auto* play = makeAction(tr("Play"), QStringLiteral("play"), true);
        auto* cue = makeAction(tr("Hold Cue"), QStringLiteral("cue_default"));
        // QSS min-height is the content box and is reapplied during popup
        // polish. Bound that box so the final border box stays touch-sized.
        const QString popupButtonStyle = QStringLiteral("min-height:40px;max-height:40px;padding:1px 2px;font-size:12px;");
        play->setStyleSheet(popupButtonStyle); cue->setStyleSheet(popupButtonStyle);
        play->setMinimumWidth(176); cue->setMinimumWidth(176);
        fallbackRows->addWidget(play); fallbackRows->addWidget(cue);
        auto* info = button(tr("Track info"), [this, target, menu] {
            menu->close();
            showTrackInfo(target());
        }, fallback, prefix + QStringLiteral("track-info"));
        info->setStyleSheet(popupButtonStyle);
        info->setFixedHeight(44); fallbackRows->addWidget(info);
        auto* grid = button(tr("Beatgrid"), [this, deck, menu] { menu->close(); showBeatgrid(deck, true); }, fallback, prefix + QStringLiteral("grid"));
        grid->setStyleSheet(popupButtonStyle);
        grid->setFixedHeight(44); fallbackRows->addWidget(grid);
        auto* widgetAction = new QWidgetAction(menu); widgetAction->setDefaultWidget(fallback); menu->addAction(widgetAction);
        connect(menu, &QMenu::aboutToShow, this, [this, target, prefix] { refreshTransport(target(), prefix); });
        connect(menu, &QMenu::aboutToHide, this, [this, play, cue] {
            releaseTouchCue(); play->setDown(false); cue->setDown(false);
        });
        watch(deck == 1 ? "[DeckLabLeft]" : "[DeckLabRight]", "current", [menu](double) { menu->close(); });
        watch(kNative, "layout_decks", [menu](double) { menu->close(); });
        watch(kTab, "current", [menu](double) { menu->close(); });
        for (int actual : {deck, deck + 2}) watch(channel(actual), "track_loaded", [menu](double) { menu->close(); });
        auto* more = button(tr("PLAY/CUE\nMORE"), [menu, strip] { menu->popup(strip->mapToGlobal(QPoint(0, strip->height()))); }, strip, prefix + QStringLiteral("more"));
        more->setFixedHeight(44); more->setStyleSheet(QStringLiteral("min-height:40px;max-height:40px;padding:1px 2px;font-size:11px;"));
        append(more, 68); m_transport[prefix].more = more;
        strip->setFixedHeight(44);
    } else {
        auto* actions = row(strip);
        actions->addWidget(makeAction(tr("Cue"), QStringLiteral("cue_default")), 1);
        actions->addWidget(makeAction(tr("Play"), QStringLiteral("play"), true), 1);
        actions->addWidget(makeAction(tr("BEAT\nSYNC"), QStringLiteral("beatsync")), 1);
        auto* menu = new QMenu(strip); menu->setObjectName(prefix + QStringLiteral("more-menu"));
        auto* content = new QWidget(menu); auto* rows = column(content);
        rows->addWidget(buildHotcues(deck, false, content));
        auto* grid = button(tr("Beatgrid"), [this, deck, menu] { menu->close(); showBeatgrid(deck, false); }, content);
        grid->setStyleSheet(QStringLiteral("min-height:40px;max-height:40px;padding:1px 2px;font-size:12px;"));
        grid->setFixedHeight(44); rows->addWidget(grid);
        auto* widgetAction = new QWidgetAction(menu); widgetAction->setDefaultWidget(content); menu->addAction(widgetAction);
        connect(menu, &QMenu::aboutToShow, this, [this, deck] { refreshHotcues(deck, QStringLiteral("decklab-card-hotcue-%1-").arg(deck)); });
        watch(kTab, "current", [menu](double) { menu->close(); });
        watch(kNative, "layout_decks", [menu](double) { menu->close(); });
        watch(channel(deck), "track_loaded", [menu](double) { menu->close(); });
        watch("[DeckLabLeft]", "current", [menu](double) { menu->close(); });
        watch("[DeckLabRight]", "current", [menu](double) { menu->close(); });
        auto* more = button(tr("More"), [menu, strip] { menu->popup(strip->mapToGlobal(QPoint(0, strip->height()))); }, strip, prefix + QStringLiteral("more"));
        more->setStyleSheet(QStringLiteral("min-height:44px;max-height:44px;padding:1px 2px;font-size:12px;"));
        more->setFixedHeight(48); actions->addWidget(more, 1); m_transport[prefix].more = more;
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
    dialog->setStyleSheet(QStringLiteral("QDialog{background:#101010;color:#eee;border:1px solid #555;} QLabel{color:#eee;} QTextBrowser{background:#111;color:#eee;border:1px solid #333;font-size:13px;} QPushButton{background:#202020;color:#eee;border:1px solid #555;min-height:38px;max-height:38px;padding:2px;}"));
    dialog->setWindowTitle(tr("Deck %1 track information").arg(actual));
    auto* content = column(dialog, 8); content->setSpacing(8);
    auto* heading = new QLabel(tr("DECK LAB · Deck %1 · TRACK INFORMATION").arg(actual), dialog);
    heading->setObjectName(QStringLiteral("decklab-track-info-heading")); heading->setFixedHeight(24);
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
    dialog->setStyleSheet(QStringLiteral("QDialog{background:#101010;color:#eee;border:1px solid #555;} QLabel{color:#eee;} QTextBrowser{background:#111;color:#eee;border:1px solid #333;font-size:13px;} QPushButton{background:#202020;color:#eee;border:1px solid #555;min-height:38px;max-height:38px;padding:2px;}"));
    auto* content = column(dialog, 8); content->setSpacing(8);
    auto* heading = new QLabel(tr("DECK LAB · CONTROLLER FX DETAILS"), dialog); heading->setFixedHeight(24); content->addWidget(heading);
    auto* ownership = new QLabel(tr("Read only · Adjust these effects on your controller or in advanced settings.\nRMX controls are separate, on the FX screen."), dialog);
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
                    std::isfinite(mix) ? QString::number(mix * 100, 'f', 0) + QLatin1Char('%') : tr("unavailable")).toHtmlEscaped();
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
    auto* strip = new QWidget(parent); auto* pads = row(strip);
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
        pad->setStyleSheet(QStringLiteral("min-height:40px;max-height:40px;padding:1px 2px;font-size:15px;font-weight:600;border-radius:0;border:1px solid #3c3c3c;"));
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
    strip->setFixedHeight(44); return strip;
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
    dialog->setStyleSheet(QStringLiteral("QDialog{background:#101010;color:#eee;border:1px solid #555;} QLabel{color:#eee;} QPushButton{background:#202020;color:#eee;border:1px solid #555;min-height:38px;max-height:38px;padding:2px;font-size:13px;} QPushButton:checked{border-color:#d74448;} QPushButton:disabled{color:#777;border-color:#333;}"));
    auto* content = column(dialog, 8); content->setSpacing(8);
    auto* title = new QLabel(dialog); title->setObjectName(QStringLiteral("decklab-beatgrid-title")); title->setFixedHeight(48);
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
                std::isfinite(bpm) && bpm > 0 ? QString::number(bpm, 'f', 2) : QStringLiteral("—"),
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
    for (int deck = 1; deck <= 2; ++deck) {
        m_sideUtilities[deck - 1] = buildTransport(deck, true, page);
        m_sideUtilities[deck - 1]->hide();
        m_sideHotcues[deck - 1] = buildHotcues(deck, true, page);
        m_sideHotcues[deck - 1]->hide();
    }
    // Parking parent only; each physical-side utility follows its visible deck.
    page->hide(); return page;
}

QWidget* WDeckLab::buildBrowse() {
    m_browser = new QWidget(this);
    auto* layout = column(m_browser);
    auto* heading = row();
    auto* back = button(tr("Back"), [this] { browseBack(); }, m_browser, "decklab-browse-back");
    back->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
    heading->addWidget(back);
    m_breadcrumb = new QLabel(tr("Sources"), m_browser);
    m_breadcrumb->setTextFormat(Qt::PlainText);
    m_breadcrumb->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_breadcrumb->setContentsMargins(9, 0, 4, 0);
    heading->addWidget(m_breadcrumb, 1);
    heading->addWidget(button(tr("Search"), [this] { setSearchOpen(!m_keyboard || m_keyboard->isHidden()); }, m_browser, "decklab-search"));
    auto* more = button(tr("Tools"), [this] {
        auto* menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->addAction(tr("All tracks"), this, &WDeckLab::openCollection);
        menu->addAction(tr("Preview selected track"), this, [this] { pulse("[PreviewDeck1]", "LoadSelectedTrackAndPlay"); });
        menu->addAction(tr("Stop preview"), this, [this] { pulse("[PreviewDeck1]", "stop"); });
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
                if (m_library) m_library->searchTracksInCollection(query);
                m_search->slotRestoreSearch(query);
                showTracks();
            }
        });
        match->setEnabled(value(channel(matchDeck), "track_loaded") && value(channel(matchDeck), "bpm") > 0);
        menu->addAction(tr("Clear filter"), this, [this] { if (m_search) m_search->slotClearSearch(); });
        menu->addAction(tr("Track details"), this, [this] { set("[Library]", "show_track_menu", 1); });
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
        menu->popup(mapToGlobal(QPoint(width() - 240, 80)));
    }, m_browser, "decklab-browse-tools");
    heading->addWidget(more);
    layout->addLayout(heading);
    m_searchPanel = new QWidget(m_browser); m_searchRow = row(m_searchPanel);
    layout->addWidget(m_searchPanel); m_searchPanel->hide();
    m_browseStack = new QStackedWidget(m_browser);
    layout->addWidget(m_browseStack, 1);
    for (int deck = 1; deck <= 4; ++deck) {
        auto pressed = std::make_shared<std::tuple<int, quint64, int>>(0, 0, 0);
        auto selection = std::make_shared<QPersistentModelIndex>();
        m_loadButtons[deck - 1] = button(tr("Load %1").arg(deck), [this, deck, pressed, selection] {
            auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
            const QPersistentModelIndex current = table ? table->currentIndex().siblingAtColumn(0) : QModelIndex();
            const int target = m_fourDeckLayout ? deck : (deck <= 2 ? displayedDeck(deck - 1) : deck);
            if (std::get<0>(*pressed) != target || std::get<1>(*pressed) != m_interactionEpoch ||
                    std::get<2>(*pressed) != m_sampleTarget || *selection != current) return;
            loadDeck(deck);
        }, m_browser,
                QStringLiteral("decklab-load-%1").arg(deck));
        connect(m_loadButtons[deck - 1], &QPushButton::pressed, this, [this, deck, pressed, selection] {
            *pressed = {m_fourDeckLayout ? deck : (deck <= 2 ? displayedDeck(deck - 1) : deck), m_interactionEpoch, m_sampleTarget};
            auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
            *selection = table ? table->currentIndex().siblingAtColumn(0) : QModelIndex();
        });
        m_loadButtons[deck - 1]->setFixedHeight(44);
        heading->addWidget(m_loadButtons[deck - 1]);
    }
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
    m_fxStatus = new QLabel(tr("RMX"), page);
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
    auto* holdHint = new QLabel(tr("Hold pads: 4 sec max · Presets open RMX · Release dry to recover"), perform);
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
    const QStringList names{tr("Deck"), tr("Levels / Cue"), tr("Display"), tr("Library"), tr("Device")};
    for (int index = 0; index < names.size(); ++index) {
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
            keys->addItems({tr("Camelot · 8A / 8B"), tr("Open Key · 8m / 8d"), tr("Musical · Am / C")});
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
                         qMakePair(tr("Record mix to USB"), 4), qMakePair(tr("Check Rekordbox USB"), 10)}) {
                auto* b = button(action.first, [this, id = action.second] { tool(id); }, content);
                b->setEnabled(false); m_toolButtons.append(b); rows->addWidget(b);
            }
        } else {
            rows->addWidget(new QLabel(tr("Controller audio is the normal main/cue route.\nChanges that interrupt audio are guarded by live playback state."), content));
            m_performanceStatus = new QLabel(content); rows->addWidget(m_performanceStatus);
            for (const auto& action : {qMakePair(tr("Controller & audio setup"), 5), qMakePair(tr("Wi-Fi"), 6),
                         qMakePair(tr("Advanced settings"), 8), qMakePair(tr("Desktop"), 9)}) {
                auto* b = button(action.first, [this, id = action.second] {
                    if (id == 8) { releaseEffects(); emit advancedPreferencesRequested(); } else tool(id);
                }, content);
                if (action.second != 8) { b->setEnabled(false); m_toolButtons.append(b); }
                rows->addWidget(b);
            }
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

void WDeckLab::addWaveform(QWidget* widget, int deck) {
    if (!widget || deck < 1 || deck > 4) return;
    if (!m_pairPerformance) {
        m_pairPerformance = new QWidget(m_waveforms);
        m_pairPerformance->setObjectName(QStringLiteral("decklab-performance-pair"));
        m_pairPerformance->setFixedHeight(220);
        auto* performance = row(m_pairPerformance);
        m_pairContexts = new QWidget(m_pairPerformance); column(m_pairContexts);
        m_pairContexts->setFixedWidth(112); performance->addWidget(m_pairContexts);
        m_pairDetails = new QWidget(m_pairPerformance);
        auto* detailGrid = new QGridLayout(m_pairDetails);
        detailGrid->setContentsMargins(0, 0, 0, 0); detailGrid->setSpacing(0);
        performance->addWidget(m_pairDetails, 1);
        auto* feedback = new QWidget(m_pairPerformance); auto* feedbackRows = column(feedback);
        feedback->setFixedWidth(112); performance->addWidget(feedback);
        auto* mainLevels = new QWidget(feedback); auto* levels = column(mainLevels, 2);
        mainLevels->setFixedHeight(48); feedbackRows->addWidget(mainLevels);
        m_masterLevelLabel = new QLabel(tr("MASTER"), mainLevels);
        m_masterLevelLabel->setObjectName(QStringLiteral("decklab-master-level-label"));
        m_masterLevelLabel->setProperty("muted", true); m_masterLevelLabel->setFixedHeight(16);
        m_masterLevelLabel->setAlignment(Qt::AlignCenter); levels->addWidget(m_masterLevelLabel);
        // One painted stereo meter, sampled by the meter tick.
        m_masterMeter = new DeckLabLevelMeter(Qt::Horizontal, 2, mainLevels);
        m_masterMeter->setObjectName(QStringLiteral("decklab-master-meter"));
        m_masterMeter->setAccessibleName(tr("Main output level"));
        m_masterMeter->setFixedHeight(21); levels->addWidget(m_masterMeter);
        m_masterMeter->setLevels(m_mainVu[0]->get(), m_mainVu[1]->get(), m_mainPeak->get() > 0);
        const auto showMainClip = [this](double v) {
            const bool clipping = v > 0;
            if (m_masterLevelLabel->property("clipping").toBool() == clipping) return;
            m_masterLevelLabel->setProperty("clipping", clipping);
            m_masterLevelLabel->setText(clipping ? tr("MASTER CLIP") : tr("MASTER"));
            m_masterLevelLabel->style()->unpolish(m_masterLevelLabel); m_masterLevelLabel->style()->polish(m_masterLevelLabel);
        };
        watch("[Main]", "peak_indicator", showMainClip);
        showMainClip(value("[Main]", "peak_indicator"));
        m_pairCue = new QLabel(feedback); m_pairCue->setProperty("muted", true);
        m_pairCue->setObjectName(QStringLiteral("decklab-headphone-context"));
        m_pairCue->setFixedHeight(20); m_pairCue->setAlignment(Qt::AlignCenter);
        m_pairCue->setStyleSheet(QStringLiteral("font-size:11px;")); feedbackRows->addWidget(m_pairCue);
        auto* boardFX = button(QString(), [this] { showControllerFxInfo(); }, feedback, QStringLiteral("decklab-performance-board-fx"));
        boardFX->setFixedHeight(44); boardFX->setProperty("transport", true);
        boardFX->setStyleSheet(QStringLiteral("padding:1px 2px;"));
        auto* boardLayout = column(boardFX, 1); feedbackRows->addWidget(boardFX);
        m_pairEffects = new QLabel(boardFX);
        m_pairEffects->setTextFormat(Qt::PlainText);
        m_pairEffects->setObjectName(QStringLiteral("decklab-performance-effects"));
        m_pairEffects->setProperty("muted", true); m_pairEffects->setStyleSheet(QStringLiteral("font-size:11px;"));
        m_pairEffects->setAttribute(Qt::WA_TransparentForMouseEvents); boardLayout->addWidget(m_pairEffects);
        boardFX->setAccessibleName(tr("Controller effects summary; read all unit and routing details"));
        auto* rmxFX = button(QString(), [this] { selectPage(3); }, feedback, QStringLiteral("decklab-performance-rmx-fx"));
        rmxFX->setFixedHeight(44); rmxFX->setProperty("transport", true);
        rmxFX->setStyleSheet(QStringLiteral("padding:1px 2px;"));
        auto* rmxLayout = column(rmxFX, 1); feedbackRows->addWidget(rmxFX);
        m_pairRMX = new QLabel(rmxFX); m_pairRMX->setProperty("muted", true);
        m_pairRMX->setObjectName(QStringLiteral("decklab-performance-rmx"));
        m_pairRMX->setAttribute(Qt::WA_TransparentForMouseEvents); rmxLayout->addWidget(m_pairRMX);
        rmxFX->setAccessibleName(tr("RMX effects summary; open FX"));
        m_pairZoom = new QLabel(feedback); m_pairZoom->setProperty("muted", true);
        m_pairZoom->setObjectName(QStringLiteral("decklab-performance-zoom"));
        m_pairZoom->setAlignment(Qt::AlignCenter); m_pairZoom->setFixedHeight(20);
        feedbackRows->addWidget(m_pairZoom);
        auto* zooms = row();
        for (const auto& zoom : {qMakePair(QString::fromUtf8("−"), QStringLiteral("waveform_zoom_up")),
                     qMakePair(QStringLiteral("+"), QStringLiteral("waveform_zoom_down"))}) {
            auto* b = button(zoom.first, [this, item = zoom.second] {
                for (int side = 0; side < 2; ++side) pulse(channel(displayedDeck(side)), item);
            }, feedback, QStringLiteral("decklab-%1").arg(zoom.second));
            b->setFixedHeight(44); b->setProperty("transport", true);
            b->setAccessibleName(zoom.second.endsWith("up") ? tr("Zoom both waveforms out") : tr("Zoom both waveforms in"));
            zooms->addWidget(b, 1);
        }
        feedbackRows->addLayout(zooms); m_pairPerformance->hide();
    }
    auto* frame = new QWidget(m_waveforms);
    auto* layout = column(frame);
    auto infoPress = std::make_shared<std::pair<quint64, TrackPointer>>();
    auto* header = button(QString(), [this, deck, infoPress] {
        auto* player = m_players ? m_players->getPlayer(channel(deck)) : nullptr;
        if (m_page == 0 && infoPress->first == m_interactionEpoch && player &&
                infoPress->second == player->getLoadedTrack()) {
            if (infoPress->second) showTrackInfo(deck);
            else selectPage(1);
        }
    }, frame, QStringLiteral("decklab-info-hit-%1").arg(deck));
    header->setStyleSheet(QStringLiteral("QPushButton{min-height:0;padding:0;border:0;background:transparent;} QPushButton:pressed{background:#181818;} QPushButton:focus{background:#181818;}"));
    header->setFixedHeight(m_page == 0 ? 44 : 24);
    header->setFocusPolicy(Qt::StrongFocus);
    connect(header, &QPushButton::pressed, this, [this, deck, infoPress] {
        auto* player = m_players ? m_players->getPlayer(channel(deck)) : nullptr;
        *infoPress = {m_interactionEpoch, player ? player->getLoadedTrack() : TrackPointer()};
    });
    header->setAccessibleName(tr("Deck %1 full track information").arg(deck));
    header->setAccessibleDescription(tr("Tap the deck readout to read its full title, artist and source"));
    header->installEventFilter(this);
    m_waveHeaders[deck - 1] = header;
    auto* labels = row(header);
    auto* identity = new QLabel(QString::number(deck), header);
    identity->setObjectName(QStringLiteral("decklab-wave-identity-%1").arg(deck));
    identity->setProperty("deckIdentity", true);
    identity->setFixedWidth(32);
    identity->setAlignment(Qt::AlignCenter);
    identity->setAccessibleName(tr("Deck %1").arg(deck));
    m_waveIdentities[deck - 1] = identity;
    labels->addWidget(identity);
    auto* content = new QWidget(header);
    auto* lines = column(content);
    auto* titleRow = new QWidget(content);
    titleRow->setObjectName(QStringLiteral("decklab-title-row-%1").arg(deck));
    auto* titleLayout = row(titleRow);
    labels->addWidget(content, 1);
    lines->addWidget(titleRow);
    m_titles[deck - 1] = new QLabel(titleRow);
    m_titles[deck - 1]->setObjectName(QStringLiteral("decklab-wave-title-%1").arg(deck));
    m_titles[deck - 1]->setTextFormat(Qt::PlainText);
    m_titles[deck - 1]->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_titles[deck - 1]->setProperty("deck", true);
    m_titles[deck - 1]->setContentsMargins(8, 0, 8, 0);
    m_titles[deck - 1]->setFixedHeight(m_page == 0 ? 20 : 24);
    titleLayout->addWidget(m_titles[deck - 1], 1);
    auto* infoAffordance = new QLabel(tr("INFO"), titleRow);
    infoAffordance->setObjectName(QStringLiteral("decklab-info-affordance-%1").arg(deck));
    infoAffordance->setStyleSheet(QStringLiteral("font-size:10px;color:#aaa;"));
    infoAffordance->setFixedWidth(30); infoAffordance->setAlignment(Qt::AlignCenter);
    m_infoAffordances[deck - 1] = infoAffordance; titleLayout->addWidget(infoAffordance);
    auto* state = new QLabel(titleRow);
    state->setObjectName(QStringLiteral("decklab-state-%1").arg(deck));
    state->setProperty("muted", true); state->setFixedWidth(70); state->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    state->setContentsMargins(0, 0, 8, 0);
    m_playStates[deck - 1] = state; titleLayout->addWidget(state);
    auto* meter = new DeckLabLevelMeter(Qt::Horizontal, 1, header); meter->setFixedSize(80,10);
    meter->setObjectName(QStringLiteral("decklab-level-%1").arg(deck));
    meter->setAccessibleName(tr("Deck %1 level").arg(deck)); titleLayout->addWidget(meter,0,Qt::AlignVCenter); m_meters[deck-1]=meter;
    watch(channel(deck),"peak_indicator",[this, deck](double v){
        if (auto* label = m_meterLabels[deck - 1]) {
            if (label->property("clipping").toBool() != (v > 0)) {
                label->setProperty("clipping", v > 0); label->setText(v > 0 ? tr("CLIP") : tr("LVL"));
                label->style()->unpolish(label); label->style()->polish(label);
            }
        }
    });
    m_waveNumbers[deck - 1] = new QLabel(header); m_waveNumbers[deck - 1]->setFixedWidth(176);
    m_waveNumbers[deck - 1]->setObjectName(QStringLiteral("decklab-wave-metrics-%1").arg(deck));
    m_waveNumbers[deck - 1]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_waveNumbers[deck - 1]->setContentsMargins(0, 0, 8, 0); titleLayout->addWidget(m_waveNumbers[deck - 1]);
    auto* metrics = new QWidget(content); metrics->setFixedHeight(28);
    m_deckMetrics[deck - 1] = metrics;
    auto* values = row(metrics); values->setContentsMargins(8, 0, 8, 0);
    auto metric = [metrics, values, deck](const QString& name, int width, bool primary) {
        auto* label = new QLabel(metrics); label->setObjectName(QStringLiteral("decklab-%1-%2").arg(name).arg(deck));
        label->setTextFormat(Qt::RichText); label->setFixedWidth(width);
        label->setProperty(primary ? "numbers" : "muted", true);
        values->addWidget(label); return label;
    };
    m_numbers[deck - 1] = metric("numbers", 170, true);
    m_keys[deck - 1] = metric("key", 130, true);
    m_remaining[deck - 1] = metric("remaining", 195, true);
    // The clock changes every second while playing; plain text avoids an HTML parse.
    m_remaining[deck - 1]->setTextFormat(Qt::PlainText);
    auto* modes = new QLabel(metrics); modes->setProperty("muted", true);
    modes->setObjectName(QStringLiteral("decklab-modes-%1").arg(deck)); m_modes[deck - 1] = modes;
    values->insertWidget(3, modes, 1);
    lines->addWidget(metrics);
    auto* secondary = new QWidget(content); auto* utility = row(secondary);
    utility->setContentsMargins(8, 0, 8, 0); secondary->setFixedHeight(18);
    m_deckSecondary[deck - 1] = secondary;
    m_pitch[deck - 1] = new QLabel(secondary); m_pitch[deck - 1]->setProperty("muted", true);
    m_pitch[deck - 1]->setObjectName(QStringLiteral("decklab-pitch-%1").arg(deck));
    utility->addWidget(m_pitch[deck - 1]);
    // Modes move into this row in the compact layout; no tiny extra controls.
    lines->addWidget(secondary);
    m_sources[deck - 1] = new QLabel(content);
    m_sources[deck - 1]->setTextFormat(Qt::PlainText);
    m_sources[deck - 1]->setObjectName(QStringLiteral("decklab-source-%1").arg(deck));
    m_sources[deck - 1]->setProperty("muted", true);
    m_sources[deck - 1]->setContentsMargins(8, 0, 8, 0);
    m_sources[deck - 1]->setFixedHeight(22); lines->addWidget(m_sources[deck - 1]);
    m_sources[deck - 1]->hide();
    // Width-dependent elision follows layout settling, not a polling timer.
    new DeckLabResizeWatcher(m_titles[deck - 1], [this, deck] { markDeckDirty(deck); });
    new DeckLabResizeWatcher(m_sources[deck - 1], [this, deck] { markDeckDirty(deck); });
    for (auto* child : header->findChildren<QWidget*>()) child->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* contextPanel = new QWidget(m_pairContexts); auto* contextRows = column(contextPanel);
    contextPanel->setFixedHeight(110); m_contextPanels[deck - 1] = contextPanel;
    m_waveContexts[deck - 1] = new QLabel(contextPanel);
    m_waveContexts[deck - 1]->setObjectName(QStringLiteral("decklab-wave-context-%1").arg(deck));
    m_waveContexts[deck - 1]->setTextFormat(Qt::PlainText);
    m_waveContexts[deck - 1]->setWordWrap(true);
    m_waveContexts[deck - 1]->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_waveContexts[deck - 1]->setContentsMargins(8, 0, 4, 0);
    m_waveContexts[deck - 1]->setFixedHeight(94); contextRows->addWidget(m_waveContexts[deck - 1]);
    auto* meterPanel = new QWidget(contextPanel); auto* meterLine = row(meterPanel);
    meterPanel->setFixedHeight(16); meterLine->setContentsMargins(8, 0, 4, 0);
    m_meterPanels[deck - 1] = meterPanel;
    auto* meterLabel = new QLabel(tr("LVL"), meterPanel); meterLabel->setProperty("muted", true);
    meterLabel->setObjectName(QStringLiteral("decklab-level-label-%1").arg(deck));
    meterLabel->setStyleSheet(QStringLiteral("font-size:11px;")); meterLabel->setFixedWidth(32);
    m_meterLabels[deck - 1] = meterLabel; meterLine->addWidget(meterLabel);
    contextRows->addWidget(meterPanel); contextPanel->hide();
    layout->addWidget(header);
    auto* body = new QWidget(frame); auto* bodyLayout = row(body);
    body->setObjectName(QStringLiteral("decklab-wave-body-%1").arg(deck));
    m_waveBodies[deck - 1] = body;
    auto* detail = new QWidget(body); auto* detailLayout = column(detail);
    detail->setObjectName(QStringLiteral("decklab-detail-column-%1").arg(deck));
    m_detailColumns[deck - 1] = detail;
    auto* separate = new QWidget(m_pairDetails); column(separate);
    separate->setObjectName(QStringLiteral("decklab-separate-wave-%1").arg(deck));
    m_separateWaves[deck - 1] = separate; separate->hide();
    widget->setMinimumHeight(16);
    widget->setObjectName(QStringLiteral("decklab-detail-%1").arg(deck));
    widget->setMaximumHeight(QWIDGETSIZE_MAX);
    detailLayout->addWidget(widget, 1);
    bodyLayout->addWidget(detail, 1); layout->addWidget(body, 1);
    m_cardTransports[deck - 1] = buildTransport(deck, false, frame);
    layout->addWidget(m_cardTransports[deck - 1]); m_cardTransports[deck - 1]->hide();
    m_deckWaveforms[deck - 1] = frame;
    watch(channel(deck), "waveform_zoom", [this, deck](double zoom) {
        m_fxSummaryDirty = true; markDeckDirty(0);
        if (!std::isfinite(zoom) || zoom <= 0 || zoom == m_linkedZoom ||
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
    m_waveLayout->addWidget(frame, deck - 1, 0, 1, 2);
    frame->setVisible(deck <= 2);
    refreshDeck(deck);
}

void WDeckLab::addOverview(QWidget* widget, int deck) {
    if (!widget || deck < 1 || deck > 4 || !m_deckWaveforms[deck - 1]) return;
    widget->setObjectName(QStringLiteral("decklab-overview-%1").arg(deck));
    widget->setFixedHeight(44);
    widget->setAttribute(Qt::WA_TransparentForMouseEvents);
    widget->setFocusPolicy(Qt::NoFocus); widget->setAcceptDrops(false);
    widget->setContextMenuPolicy(Qt::NoContextMenu);
    widget->setAccessibleName(tr("Deck %1 whole-track overview, read only").arg(deck));
    m_overviews[deck - 1] = widget;
    auto* layout = static_cast<QVBoxLayout*>(m_detailColumns[deck - 1]->layout());
    layout->addWidget(widget);
    widget->setVisible(m_page == 0);
    updateLayers();
}

void WDeckLab::bindBrowser(WSearchLineEdit* search, WLibrarySidebar* sidebar, WLibrary* library) {
    m_search = search; m_sidebar = sidebar; m_libraryWidget = library;
    search->setMinimumHeight(40); search->setMaximumHeight(40);
    search->setItemDelegate(new DeckLabComboDelegate(search));
    search->setMaxVisibleItems(6);
    search->setAccessibleName(tr("Search tracks"));
    m_searchRow->addWidget(search, 1);
    m_searchRow->addWidget(button(tr("Clear"), [search] { search->slotClearSearch(); }, m_browser));
    m_keyboard = new QWidget(this); m_keyboard->setObjectName("decklab-keyboard");
    auto* keys = column(m_keyboard); m_keyboard->setFixedHeight(200);
    const QStringList rows{"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    for (const QString& letters : rows) {
        auto* line = row(nullptr, 2);
        for (QChar letter : letters) {
            auto* key = button(QString(letter), [search, letter] { if (search->lineEdit()) search->lineEdit()->insert(QString(letter).toLower()); }, m_keyboard);
            key->setStyleSheet("min-height:0;padding:0;border-radius:0;"); key->setFixedHeight(40); line->addWidget(key, 1);
        }
        keys->addLayout(line);
    }
    auto* actions = row(nullptr, 2);
    for (const QString& mark : {QStringLiteral("-"), QStringLiteral("."), QStringLiteral("/")})
        actions->addWidget(button(mark, [search, mark] { if (search->lineEdit()) search->lineEdit()->insert(mark); }, m_keyboard));
    actions->addWidget(button(tr("Space"), [search] { if (search->lineEdit()) search->lineEdit()->insert(" "); }, m_keyboard), 1);
    auto* backspace = button(tr("Delete"), [search] { if (search->lineEdit()) search->lineEdit()->backspace(); }, m_keyboard);
    backspace->setAutoRepeat(true); backspace->setAutoRepeatDelay(400); backspace->setAutoRepeatInterval(100); actions->addWidget(backspace);
    actions->addWidget(button(tr("Done"), [this] { setSearchOpen(false); }, m_keyboard, "decklab-search-done"));
    keys->addLayout(actions); m_root->addWidget(m_keyboard); m_keyboard->hide();
    m_browseStack->addWidget(sidebar); m_browseStack->addWidget(library);
    sidebar->setIndentation(0);
    sidebar->setRootIsDecorated(false);
    sidebar->setItemsExpandable(false);
    sidebar->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    new DeckLabTouchScrollGuard(sidebar->viewport(), sidebar->viewport());
    sidebar->installEventFilter(this);
    sidebar->setEditTriggers(QAbstractItemView::NoEditTriggers);
    if (auto* model = qobject_cast<SidebarModel*>(sidebar->model()))
        disconnect(sidebar, &QTreeView::clicked, model, &SidebarModel::clicked);
    connect(sidebar, &QTreeView::clicked, this, [this](const QModelIndex& index) {
        if (index.isValid()) openSource();
    });
    connect(sidebar->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex&, const QModelIndex&) { updateBreadcrumb(); });
    connect(search, &WSearchLineEdit::search, this, [this](const QString&) { showTracks(); });
    if (m_library) connect(m_library, &Library::trackSelected, this, [this](TrackPointer) { refreshLoadButtons(); });
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
    // Decks gets larger targets without moving companion utility windows.
    for (auto* tab : m_tabs) {
        tab->ensurePolished();
        tab->setFixedHeight(40);
    }
    if (auto* usb = findChild<QPushButton*>(QStringLiteral("decklab-usb"))) {
        usb->ensurePolished();
        usb->setFixedHeight(40);
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
    if (!m_sidebar) return;
    m_browseStack->setCurrentWidget(m_sidebar);
    if (value("[Sidebar]", "sidebar_visible") != 1) set("[Sidebar]", "sidebar_visible", 1);
    set("[Library]", "focused_widget", 2);
    m_sidebar->setFocus(Qt::OtherFocusReason);
    if (!m_sidebar->currentIndex().isValid() && m_sidebar->model()->rowCount(m_sidebar->rootIndex())) {
        m_sidebar->setCurrentIndex(m_sidebar->model()->index(0, 0, m_sidebar->rootIndex()));
    }
    updateBreadcrumb(); refreshLoadButtons();
}
void WDeckLab::showTracks() {
    if (!m_libraryWidget) return;
    m_browseStack->setCurrentWidget(m_libraryWidget);
    if (value("[Sidebar]", "sidebar_visible")) set("[Sidebar]", "sidebar_visible", 0);
    set("[Library]", "focused_widget", 3);
    updateBreadcrumb(); refreshLoadButtons();
}
void WDeckLab::browsePress() {
    if (m_page != 1) { selectPage(1); return; }
    if (m_browseStack->currentIndex() != 0) { showSources(); return; }
    openSource();
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
    if (m_browseStack->currentIndex() != 0) { showSources(); return; }
    const QPersistentModelIndex previous(m_sidebar->rootIndex());
    if (previous.isValid()) {
        m_sidebar->setRootIndex(previous.parent());
        m_sidebar->setCurrentIndex(previous);
        showSources();
    } else selectPage(0);
}
void WDeckLab::updateBreadcrumb() {
    if (!m_sidebar) return;
    QStringList path;
    QModelIndex at = m_browseStack->currentWidget() == m_libraryWidget ? m_sidebar->currentIndex() : m_sidebar->rootIndex();
    while (at.isValid()) { path.prepend(at.data().toString()); at = at.parent(); }
    if (path.isEmpty()) path.append(tr("Sources"));
    const QString full = path.join(QStringLiteral("  /  "));
    m_breadcrumb->setText(m_breadcrumb->fontMetrics().elidedText(full, Qt::ElideRight, std::max(0, m_breadcrumb->width() - 14)));
    m_breadcrumb->setToolTip(full);
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
    if (m_page != 1 || !m_libraryWidget || m_browseStack->currentWidget() != m_libraryWidget ||
            value(channel(deck), "play")) return;
    set("[Library]", "focused_widget", 3);
    pulse(channel(deck), "LoadSelectedTrack");
    // Return happens on newTrackLoaded, never merely on a button press.
}
void WDeckLab::refreshLoadButtons() {
    if (!m_loadButtons[0]) return;
    auto* table = m_libraryWidget ? m_libraryWidget->getCurrentTrackTableView() : nullptr;
    const bool selected = table && table->selectionModel() && !table->selectionModel()->selectedRows().isEmpty();
    for (int deck = 1; deck <= 4; ++deck) {
        auto* load = m_loadButtons[deck - 1];
        load->setVisible(deck <= 2 || (m_fourDeckLayout && !m_sampleTarget));
        if (m_sampleTarget) {
            load->setText(deck == 1 ? tr("Load pad %1").arg(m_sampleTarget) : tr("Cancel"));
            load->setEnabled(deck == 2 || (m_libraryWidget && m_browseStack->currentWidget() == m_libraryWidget &&
                    selected && !value(QStringLiteral("[Sampler%1]").arg(m_sampleTarget), "play")));
            continue;
        }
        const int target = m_fourDeckLayout ? deck : (deck <= 2 ? displayedDeck(deck - 1) : deck);
        const bool playing = value(channel(target), "play");
        load->setText(playing ? tr("Deck %1 · Playing").arg(target) : tr("Load %1").arg(target));
        load->setEnabled(!playing && m_libraryWidget && m_browseStack->currentWidget() == m_libraryWidget && selected);
    }
}
void WDeckLab::refreshDeck(int deck) {
    if (deck < 1 || deck > 4) return;
    const auto& d = m_deck[deck - 1];
    auto* player = m_players ? m_players->getPlayer(channel(deck)) : nullptr;
    auto track = player ? player->getLoadedTrack() : TrackPointer();
    QString title = track ? track->getTitle() : tr("Choose a track in Browse");
    const bool loaded = d.loaded->get() > 0;
    const bool playing = d.play->get() > 0;
    const double bpm = m_tempos[deck - 1].update(loaded, d.localBpm->get(), d.rateRatio->get(), d.bpm->get(), m_clock.elapsed());
    const double duration = d.duration->get();
    const double rawPosition = d.position->get();
    const double position = std::isfinite(rawPosition) ? rawPosition : 0;
    m_shownPosition[deck - 1] = position;
    QString key = KeyUtils::keyToString(static_cast<mixxx::track::io::key::ChromaticKey>(int(d.key->get())));
    if (key.isEmpty()) key = QStringLiteral("—");
    if (m_titles[deck - 1]) {
        const bool compactTitle = m_page == 0 && m_fourDeckLayout;
        if (!track && compactTitle) title = tr("Browse tracks");
        setWordWrapIf(m_titles[deck - 1], compactTitle);
        // Fixed empty guidance fits its final card; do not elide against a
        // newly revealed upper deck's not-yet-settled width of zero.
        setTextIf(m_titles[deck - 1], track ? m_titles[deck - 1]->fontMetrics().elidedText(title, Qt::ElideRight,
                std::max(0, (m_titles[deck - 1]->width() - 18) * (compactTitle ? 2 : 1))) : title);
        setToolTipIf(m_titles[deck - 1], QStringLiteral("<span>%1</span>").arg(title.toHtmlEscaped()));
        if (m_waveNumbers[deck - 1]) {
            setVisibleIf(m_waveNumbers[deck - 1], m_page != 0);
            setTextIf(m_waveNumbers[deck - 1], track ? QStringLiteral("%1    %2 BPM").arg(key).arg(bpm, 0, 'f', 2) : QString());
        }
    }
    const int side = deck % 2 ? 0 : 1;
    if (m_numbers[deck - 1]) {
        const double remaining = duration * (1 - position);
        double pitch = (d.rateRatio->get() - 1) * 100;
        if (!std::isfinite(pitch) || std::abs(pitch) < .005) pitch = 0;
        const QString pitchText = (pitch >= 0 ? QStringLiteral("+") : QString()) + QString::number(pitch, 'f', 2) + QLatin1Char('%');
        const bool compact = m_page == 0 && m_fourDeckLayout;
        const bool paired = m_page == 0 && !m_fourDeckLayout;
        QWidget* content = m_deckMetrics[deck - 1]->parentWidget();
        QWidget* modeDestination = compact ? content : m_page == 0 ? m_deckSecondary[deck - 1] : m_deckMetrics[deck - 1];
        const bool presentationChanged = m_numbers[deck - 1]->minimumWidth() != (compact ? 108 : paired ? 125 : 150) ||
                m_modes[deck - 1]->parentWidget() != modeDestination ||
                m_deckMetrics[deck - 1]->isHidden() != (m_page != 0) ||
                m_deckSecondary[deck - 1]->isHidden() != (m_page != 0) ||
                m_sources[deck - 1]->isHidden() == paired;
        setEnabledIf(m_waveHeaders[deck - 1], m_page == 0);
        setAccessibleNameIf(m_waveHeaders[deck - 1], track ? tr("Deck %1 full track information").arg(deck) : tr("Deck %1 empty; open Browse").arg(deck));
        setAccessibleDescriptionIf(m_waveHeaders[deck - 1], track ? tr("Read the full title, artist and source") : tr("Choose a track in Browse; preserves the current browser location"));
        setTextIf(m_infoAffordances[deck - 1], track ? tr("INFO") : tr("BROWSE"));
        setFixedWidthIf(m_infoAffordances[deck - 1], track ? 30 : 46);
        setVisibleIf(m_infoAffordances[deck - 1], m_page == 0);
        setFocusPolicyIf(m_waveHeaders[deck - 1], m_page == 0 ? Qt::StrongFocus : Qt::NoFocus);
        setFixedHeightIf(m_waveHeaders[deck - 1], m_page == 0 ? (compact ? 128 : 88) : 24);
        setFixedWidthIf(m_waveIdentities[deck - 1], compact ? 20 : 32);
        setFixedHeightIf(m_titles[deck - 1], compact ? 44 : paired ? 20 : 24);
        setVisibleIf(m_deckMetrics[deck - 1], m_page == 0);
        setFixedHeightIf(m_deckMetrics[deck - 1], 28);
        setVisibleIf(m_playStates[deck - 1], false);
        setTextIf(m_playStates[deck - 1], loaded ? (playing ? tr("PLAYING") : tr("PAUSED")) : tr("EMPTY"));
        setTextIf(m_numbers[deck - 1], compact
                        ? (track ? QString::number(bpm, 'f', 2) : QStringLiteral("—"))
                        : track ? QStringLiteral("%1 <span style='font-size:11px;font-weight:400;color:#aaa'>BPM</span>").arg(bpm, 0, 'f', 2) : QStringLiteral("— <span style='font-size:11px;font-weight:400;color:#aaa'>BPM</span>"));
        setTextIf(m_keys[deck - 1], (compact ? QStringLiteral("%1") : QStringLiteral("<span style='font-size:11px;font-weight:400;color:#aaa'>KEY</span> %1")).arg(track ? key.toHtmlEscaped() : QStringLiteral("—")));
        setTextIf(m_remaining[deck - 1], track ? QStringLiteral("−") + timeText(remaining) : QStringLiteral("—"));
        setTextIf(m_pitch[deck - 1], loaded ? (compact ? pitchText : tr("PITCH %1").arg(pitchText)) : QString());
        QStringList modes;
        if (d.loop->get() > 0)
            modes << (compact ? tr("LOOP %1").arg(beatLoopSizeText(d.loopSize->get())) : tr("LOOP"));
        if (d.sync->get() > 0) modes << tr("SYNC LOCK");
        const bool leader = d.leader->get() > 0;
        const bool slip = d.slip->get() > 0;
        if (!compact && leader) modes << tr("MASTER");
        if (!compact && slip) modes << tr("SLIP");
        if (compact && d.keylock->get() > 0) modes << tr("KEYLOCK");
        if (compact && d.quantize->get() > 0) modes << tr("QNT");
        const bool pfl = d.pfl->get() > 0;
        QStringList compactModes;
        if (d.sync->get() > 0) compactModes << tr("SYNC");
        if (leader) compactModes << tr("MASTER");
        if (slip) compactModes << tr("SLIP");
        if (d.keylock->get() > 0) compactModes << tr("KEY");
        if (d.quantize->get() > 0) compactModes << tr("QNT");
        QString compactLoop;
        for (const auto& mode : modes) if (mode.startsWith(tr("LOOP"))) compactLoop = mode;
        setTextIf(m_modes[deck - 1], compact ? tr("HP %1 %2\n%3").arg(pfl ? tr("ON") : tr("OFF"), compactLoop, compactModes.join(QLatin1Char(' '))) : modes.join(QStringLiteral(" · ")));
        setToolTipIf(m_modes[deck - 1], modes.join(QStringLiteral(" · ")));
        auto* values = static_cast<QBoxLayout*>(m_deckMetrics[deck - 1]->layout());
        auto* utility = static_cast<QBoxLayout*>(m_deckSecondary[deck - 1]->layout());
        QWidget* destination = m_page == 0 ? m_deckSecondary[deck - 1] : m_deckMetrics[deck - 1];
        if (m_modes[deck - 1]->parentWidget() != modeDestination || m_pitch[deck - 1]->parentWidget() != destination ||
                m_remaining[deck - 1]->parentWidget() != (compact ? destination : m_deckMetrics[deck - 1])) {
            m_modes[deck - 1]->parentWidget()->layout()->removeWidget(m_modes[deck - 1]);
            m_pitch[deck - 1]->parentWidget()->layout()->removeWidget(m_pitch[deck - 1]);
            m_remaining[deck - 1]->parentWidget()->layout()->removeWidget(m_remaining[deck - 1]);
            m_modes[deck - 1]->setParent(modeDestination); m_pitch[deck - 1]->setParent(destination);
            m_remaining[deck - 1]->setParent(compact ? destination : m_deckMetrics[deck - 1]);
            if (compact) {
                utility->insertWidget(0, m_remaining[deck - 1]); utility->addWidget(m_pitch[deck - 1]);
                static_cast<QBoxLayout*>(content->layout())->insertWidget(3, m_modes[deck - 1]);
            }
            else if (m_page == 0) { values->insertWidget(2, m_remaining[deck - 1]); utility->insertWidget(0, m_pitch[deck - 1]); utility->addWidget(m_modes[deck - 1], 1); }
            else { values->insertWidget(2, m_remaining[deck - 1]); values->addWidget(m_modes[deck - 1], 1); values->addWidget(m_pitch[deck - 1]); }
            m_modes[deck - 1]->show(); m_pitch[deck - 1]->show();
            m_remaining[deck - 1]->show();
        }
        setFixedWidthIf(m_numbers[deck - 1], compact ? 108 : paired ? 125 : 150);
        setFixedWidthIf(m_keys[deck - 1], compact ? 56 : paired ? 85 : 105);
        setFixedWidthIf(m_remaining[deck - 1], compact ? 100 : paired ? 142 : 160);
        setFixedWidthIf(m_pitch[deck - 1], compact ? 64 : 96);
        setFixedHeightIf(m_deckSecondary[deck - 1], compact ? 28 : 18);
        setFixedHeightIf(m_modes[deck - 1], compact ? 28 : 18);
        setStyleSheetIf(m_modes[deck - 1], compact ? QStringLiteral("font-size:10px;padding-left:8px;") : QString());
        setVisibleIf(m_deckSecondary[deck - 1], m_page == 0);
        setVisibleIf(m_sources[deck - 1], paired);
        QString source = tr("SOURCE —");
        if (track) {
            const QString location = track->getLocation();
            if (m_sourceLocations[deck - 1] != location || m_sourceNames[deck - 1].isEmpty()) {
                m_sourceLocations[deck - 1] = location;
                const QStorageInfo volume(location);
                m_sourceNames[deck - 1] = volume.isValid() && volume.isReady()
                        ? (volume.rootPath() == QDir::rootPath() ? tr("LOCAL") :
                                   volume.displayName().isEmpty() ? volume.rootPath() : volume.displayName())
                        : tr("FILE");
            }
            source = m_sourceNames[deck - 1];
        }
        const QString sourceLine = loaded
                ? tr("%1 · ELAP %2")
                          .arg(source, timeText(duration * std::clamp(position, 0.0, 1.0)))
                : tr("EMPTY · Choose a track in Browse");
        setTextIf(m_sources[deck - 1], m_sources[deck - 1]->fontMetrics().elidedText(
                sourceLine, Qt::ElideRight, std::max(0, m_sources[deck - 1]->width() - 16)));
        setToolTipIf(m_sources[deck - 1], sourceLine);
        QString beatText = tr("BEAT —");
        if (track) {
            if (const auto beats = track->getBeats()) {
                const auto frame = mixxx::audio::FramePos::fromEngineSamplePos(position * d.samples->get());
                mixxx::audio::FramePos previous, next;
                if (frame.isValid() && beats->findPrevNextBeats(frame, &previous, &next, false) && next > previous) {
                    const auto iterator = beats->iteratorFrom(previous);
                    const double phase = (frame - previous) / (next - previous);
                    beatText = tr("BEAT %1").arg(double(iterator - beats->cfirstmarker()) + 1 + phase, 0, 'f', 2);
                }
            }
        }
        const QString stateText = loaded ? playing ? tr("PLAYING") : tr("PAUSED") : tr("EMPTY");
        // Plain text: this readout advances while playing and must never
        // re-parse HTML on the display tick.
        setTextIf(m_waveContexts[deck - 1], m_page == 0 && m_sideBySideWaveforms
                ? tr("D%1 · %2 · %3 · HP CUE %4").arg(deck).arg(stateText, beatText, pfl ? tr("ON") : tr("OFF"))
                : compact
                        ? tr("D%1 · %2\n%3").arg(deck).arg(stateText, beatText)
                        : tr("D%1 · %2\n%3\nHP CUE %4\n%5").arg(deck).arg(stateText, beatText, pfl ? tr("ON") : tr("OFF"),
                                  modes.join(QStringLiteral(" · "))));
        setToolTipIf(m_waveContexts[deck - 1], tr("Beat index and phase relative to the stored beat grid's first marker; no bar or phrase inference"));
        if (m_overviews[deck - 1]) setVisibleIf(m_overviews[deck - 1], m_page == 0);
        if (presentationChanged) {
            // Reparenting plus changed fixed columns must settle together,
            // including cards that were hidden in the previous presentation.
            values->invalidate(); utility->invalidate();
            auto* lines = m_deckMetrics[deck - 1]->parentWidget()->layout();
            lines->invalidate(); m_waveHeaders[deck - 1]->layout()->invalidate();
            m_waveHeaders[deck - 1]->layout()->activate(); lines->activate();
            values->activate(); utility->activate();
        }
    }
    if (displayedDeck(side) == deck) refreshTransport(deck, m_sidePrefixes[side]);
    refreshTransport(deck, m_cardPrefixes[deck - 1]);
}

void WDeckLab::refreshControllerFx() {
    // Event driven: unit, slot, routing, headphone and zoom changes mark this
    // dirty. Nothing here runs on the display tick.
    if (!m_pairEffects || !m_fxWatchersReady || m_page != 0) return;
    m_fxSummaryDirty = false;
    QStringList effects;
    QStringList activeStates;
    QStringList detailedEffects;
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
        const double mix = std::isfinite(rawMix) ? std::clamp(rawMix, 0.0, 1.0) : 0.0;
        const int mixPercent = int(mix * 100);
        const QString route = targets.isEmpty() ? QStringLiteral("—") : targets.join(QLatin1Char('/'));
        const bool active = effectOn && value(group, "enabled") > 0 && mix > 0 && !targets.isEmpty();
        const QString state = active ? (mixPercent > 0 ? tr("%1%").arg(mixPercent) : tr("<1%")) : tr("DRY");
        activeStates << (active ? tr("ON") : tr("DRY"));
        const QString prefix = tr("FX%1→%2 ").arg(unit).arg(route);
        const QString suffix = QLatin1Char(' ') + state;
        const int available = std::max(8, 100 - m_pairEffects->fontMetrics().horizontalAdvance(prefix + suffix));
        const QString shortName = m_pairEffects->fontMetrics().elidedText(name, Qt::ElideRight, available);
        if (unit <= 2) effects << prefix + shortName + suffix;
        detailedEffects << tr("FX %1; decks %2; %3; %4").arg(unit).arg(route, name, state);
    }
    setTextIf(m_pairEffects, m_fourDeckLayout
                    ? tr("FX 1 %1 · 2 %2\nFX 3 %3 · 4 %4").arg(activeStates[0], activeStates[1], activeStates[2], activeStates[3])
                    : effects.join(QLatin1Char('\n')));
    setToolTipIf(m_pairEffects, detailedEffects.join(QLatin1Char('\n')));
    const double headMix = value("[Master]", "headMix");
    const int mainMix = std::isfinite(headMix) ? int(std::clamp((headMix + 1) * 50, 0.0, 100.0)) : 0;
    const bool split = value("[Master]", "headSplit") > 0;
    setTextIf(m_pairCue, split ? tr("HP SPLIT") : mainMix == 0 ? tr("HP CUE") : mainMix == 100 ? tr("HP MAIN") : tr("HP %1% MAIN").arg(mainMix));
    setToolTipIf(m_pairCue, tr("Headphones: %1% toward main output; split cue %2. Per-deck HP CUE shows the selected cue feeds.").arg(mainMix).arg(split ? tr("on") : tr("off")));
    setTextIf(m_pairZoom, tr("LINKED ZOOM"));
    setToolTipIf(m_pairZoom, tr("All four decks share the same waveform zoom; current value %1")
                                  .arg(m_deck[0].zoom->get(), 0, 'g', 2));
}

void WDeckLab::connectControllerFx() {
    // Controller effect units may be created after this surface. Connect once
    // they exist; until then the summary simply waits.
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
    if (m_pairRMX) {
        const QString rmx = tr("RMX · D%1\n%2").arg(m_fxDeck).arg(m_fxReleasing ? tr("TAIL") : m_fxActive ? tr("ON") : tr("DRY"));
        if (rmx != m_shownRmx) { m_shownRmx = rmx; setTextIf(m_pairRMX, rmx); }
    }
    if (!isVisible() || !m_waveforms->isVisible()) return;
    // Only clocks advance on the tick: playing decks, or a paused deck whose
    // position was moved (cue, needle or scratch).
    for (int deck = 1; deck <= 4; ++deck) {
        if (!deckShown(deck)) continue;
        const auto& d = m_deck[deck - 1];
        if (d.loaded->get() <= 0) continue;
        const double rawPosition = d.position->get();
        const double position = std::isfinite(rawPosition) ? rawPosition : 0;
        if (d.play->get() > 0 || position != m_shownPosition[deck - 1]) refreshDeck(deck);
    }
}

void WDeckLab::tickMeters() {
    if (!isVisible() || !m_waveforms->isVisible()) return;
    for (int deck = 1; deck <= 4; ++deck) {
        auto* meter = m_meters[deck - 1];
        if (meter && meter->isVisible())
            meter->setLevels(m_deck[deck - 1].vu->get(), 0, m_deck[deck - 1].peak->get() > 0);
    }
    if (m_masterMeter && m_masterMeter->isVisible())
        m_masterMeter->setLevels(m_mainVu[0]->get(), m_mainVu[1]->get(), m_mainPeak->get() > 0);
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
        setTextIf(t.quantize, tr("QNT") + QLatin1Char('\n') + (state.quantize ? tr("ON") : tr("OFF")));
        setAccessibleNameIf(t.quantize, tr("Deck %1 Quantize %2").arg(deck).arg(state.quantize ? tr("on") : tr("off")));
    }
    if (t.keylock) {
        setCheckedIf(t.keylock, state.keylock);
        setTextIf(t.keylock, tr("KEY LOCK") + QLatin1Char('\n') + (state.keylock ? tr("ON") : tr("OFF")));
        setAccessibleNameIf(t.keylock, tr("Deck %1 %2").arg(deck).arg(t.keylock->text()));
    }
    if (t.sync) {
        setStylePropertyIf(t.sync, "actionFeedback", state.feedback);
        setAccessibleNameIf(t.sync, tr("Deck %1 %2").arg(deck).arg(t.sync->text()));
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
    m_fxStatus->setText(!ready ? tr("RMX processor unavailable") : !source ? tr("Load a track / arm sample FX") : m_fxReleasing ? tr("Releasing tail") : m_fxActive ? tr("RMX active") : tr("RMX ready · Dry"));
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
    if (m_toolsOnline && now - m_lastToolsReady > 2000) {
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
    if (m_page != 3 || now - m_lastFxTick > 550 || !isVisible()) { releaseEffects(); return; }
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
    m_outputConfigured = configured; updateStatus();
}
void WDeckLab::updateStatus() {
    int overloads = int(value("[App]", "audio_latency_overload_count"));
    int recording = int(value("[Recording]", "status"));
    QString text = recording == 2 ? tr("REC · Recording") : recording == 3 ? tr("Recording paused") : QString();
    if (!m_outputConfigured || value("[SoundManager]", "status") != 2) text += tr("   Audio disconnected · Settings → Device");
    if (!m_toolsOnline) text += tr("   System tools offline");
    if (overloads) text += tr("   Audio overloads: %1").arg(overloads);
    // Called for every waveform measurement too: unchanged status must not
    // relayout the rail.
    setTextIf(m_status, recording == 2 ? tr("REC") : recording == 3 ? tr("REC II") :
            (!m_outputConfigured || value("[SoundManager]", "status") != 2) ? tr("Audio !") :
            !m_toolsOnline ? tr("System !") : tr("Load !"));
    setToolTipIf(m_status, text.trimmed()); setAccessibleNameIf(m_status, text.trimmed());
    setVisibleIf(m_status, !text.isEmpty());
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
    if (open && m_library && m_browseStack->currentWidget() == m_sidebar) {
        m_library->searchTracksInCollection(QString()); showTracks();
    }
    m_keyboard->setVisible(open);
    m_searchPanel->setVisible(open || (m_search && !m_search->currentText().isEmpty()));
    if (m_page == 1) m_waveforms->setVisible(!open);
    if (open && m_search && m_search->lineEdit()) m_search->lineEdit()->setFocus(Qt::OtherFocusReason);
}

void WDeckLab::openCollection() {
    selectPage(1); setSearchOpen(false);
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
    if (signature == m_layoutSignature) {
        for (int deck = 1; deck <= 4; ++deck) markDeckDirty(deck);
        refreshLoadButtons();
        return;
    }
    m_layoutSignature = signature;
    m_fxSummaryDirty = true; markDeckDirty(0);
    m_deckControls->hide();
    const bool paired = m_page == 0;
    const bool separated = paired && m_sideBySideWaveforms;
    auto move = [](QWidget* widget, QWidget* parent, QBoxLayout* layout, int index) {
        if (!widget || !parent) return;
        if (widget->parentWidget() && widget->parentWidget()->layout())
            widget->parentWidget()->layout()->removeWidget(widget);
        if (widget->parentWidget() != parent) widget->setParent(parent);
        layout->insertWidget(index, widget);
    };
    auto* detailGrid = m_pairDetails ? static_cast<QGridLayout*>(m_pairDetails->layout()) : nullptr;
    if (detailGrid) {
        for (int index = 0; index < 4; ++index) {
            detailGrid->setRowStretch(index, 0); detailGrid->setColumnStretch(index, 0);
            if (m_separateWaves[index]) {
                detailGrid->removeWidget(m_separateWaves[index]);
                m_separateWaves[index]->setVisible(separated && (four || index + 1 == m_layers[0] || index + 1 == m_layers[1]));
            }
        }
        detailGrid->setColumnStretch(0, 1); detailGrid->setColumnStretch(1, separated ? 1 : 0);
        m_pairContexts->setVisible(paired && !separated);
    }
    if (m_pairPerformance) {
        m_waveLayout->removeWidget(m_pairPerformance);
        m_pairPerformance->setVisible(paired);
        for (int column = 0; column < 4; ++column)
            m_waveLayout->setColumnStretch(column, column < (four ? 4 : 2) ? 1 : 0);
        if (paired) m_waveLayout->addWidget(m_pairPerformance, 0, 0, 1, four ? 4 : 2);
    }
    for (int side = 0; side < 2; ++side) {
        auto* destination = m_deckWaveforms[m_layers[side] - 1];
        // Engine feedback during a held Cue must not hide its open menu's
        // ancestor. Hide only controls actually leaving this presentation.
        for (auto* strip : {m_sideUtilities[side], m_sideHotcues[side]})
            if (strip && (!paired || four || strip->parentWidget() != destination)) strip->hide();
    }
    for (auto* frame : m_deckWaveforms) if (frame) m_waveLayout->removeWidget(frame);
    for (int deck = 1; deck <= 4; ++deck) {
        auto* frame = m_deckWaveforms[deck - 1]; if (!frame) continue;
        const bool compact = m_page == 0 && four;
        const bool visible = compact || deck == m_layers[0] || deck == m_layers[1];
        auto* frameLayout = static_cast<QBoxLayout*>(frame->layout());
        frame->setMinimumHeight(0); frame->setMaximumHeight(QWIDGETSIZE_MAX);
        m_detailColumns[deck - 1]->setMinimumHeight(0);
        m_detailColumns[deck - 1]->setMaximumHeight(QWIDGETSIZE_MAX);
        m_waveBodies[deck - 1]->setVisible(!paired);
        if (m_contextPanels[deck - 1]) m_contextPanels[deck - 1]->hide();
        frame->layout()->setContentsMargins(0, 0, 0, 0);
        frame->setVisible(visible); m_cardTransports[deck - 1]->setVisible(compact);
        if (paired && visible) {
            const int side = compact ? deck - 1 : deck == m_layers[0] ? 0 : 1;
            frame->setFixedHeight(220);
            if (separated) {
                auto* separate = m_separateWaves[deck - 1];
                separate->setFixedHeight(compact ? 110 : 220);
                detailGrid->addWidget(separate, compact ? side / 2 : 0, side % 2);
                auto* separateRows = static_cast<QBoxLayout*>(separate->layout());
                move(m_contextPanels[deck - 1], separate, separateRows, 0);
                m_contextPanels[deck - 1]->setFixedHeight(44);
                m_waveContexts[deck - 1]->setFixedHeight(28);
                // Both dimensions fixed: a changing readout never relayouts.
                m_waveContexts[deck - 1]->setFixedWidth(344);
                move(m_detailColumns[deck - 1], separate, separateRows, 1);
                m_detailColumns[deck - 1]->setFixedHeight(compact ? 66 : 176);
                separate->show();
            } else {
                auto* detail = m_detailColumns[deck - 1];
                if (detail->parentWidget()->layout()) detail->parentWidget()->layout()->removeWidget(detail);
                if (detail->parentWidget() != m_pairDetails) detail->setParent(m_pairDetails);
                detailGrid->addWidget(detail, side, 0);
                detail->setFixedHeight(compact ? 55 : 110);
                m_contextPanels[deck - 1]->setFixedHeight(compact ? 55 : 110);
                m_waveContexts[deck - 1]->setFixedHeight(compact ? 39 : 94);
                m_waveContexts[deck - 1]->setFixedWidth(112);
                move(m_contextPanels[deck - 1], m_pairContexts,
                        static_cast<QBoxLayout*>(m_pairContexts->layout()), side);
            }
            m_detailColumns[deck - 1]->show();
            m_contextPanels[deck - 1]->show();
            m_meters[deck - 1]->setMinimumWidth(0); m_meters[deck - 1]->setMaximumWidth(QWIDGETSIZE_MAX);
            m_meters[deck - 1]->setFixedHeight(10);
            move(m_meters[deck - 1], m_meterPanels[deck - 1],
                    static_cast<QBoxLayout*>(m_meterPanels[deck - 1]->layout()), 1);
            m_meters[deck - 1]->show();
            if (m_overviews[deck - 1]) move(m_overviews[deck - 1], frame, frameLayout, 1);
            if (!compact) {
                move(m_sideHotcues[side], frame, frameLayout, 2);
                move(m_sideUtilities[side], frame, frameLayout, 3);
                if (m_sideHotcues[side]) m_sideHotcues[side]->show();
                if (m_sideUtilities[side]) m_sideUtilities[side]->show();
            }
        } else {
            m_meters[deck - 1]->setFixedSize(80, 10);
            auto* titleRow = m_titles[deck - 1]->parentWidget();
            move(m_meters[deck - 1], titleRow, static_cast<QBoxLayout*>(titleRow->layout()), 2);
            m_meters[deck - 1]->show();
            move(m_detailColumns[deck - 1], m_waveBodies[deck - 1],
                    static_cast<QBoxLayout*>(m_waveBodies[deck - 1]->layout()), 0);
            m_detailColumns[deck - 1]->setVisible(visible);
            if (m_overviews[deck - 1]) move(m_overviews[deck - 1], m_detailColumns[deck - 1],
                    static_cast<QBoxLayout*>(m_detailColumns[deck - 1]->layout()), 1);
        }
        if (visible) {
            if (compact) m_waveLayout->addWidget(frame, 1, deck - 1);
            else if (paired) m_waveLayout->addWidget(frame, 1, deck == m_layers[0] ? 0 : 1);
            else m_waveLayout->addWidget(frame, deck == m_layers[0] ? 0 : 1, 0, 1, 2);
        }
        refreshDeck(deck);
    }
    refreshLoadButtons();
}
