// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/wdecklab.h"
#include "widget/decklabcombodelegate.h"

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
#include "track/keyutils.h"
#include "track/cueinfo.h"
#include "track/track.h"
#include "track/beats.h"
#include "waveform/waveformwidgetfactory.h"
#include "widget/decklabbrowse.h"
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
                updateLayers(); refreshLoadButtons(); refreshFx();
            });
        }
        if (m_players) {
            auto* player = m_players->getPlayer(channel(deck));
            if (player) {
                connect(player, &BaseTrackPlayer::newTrackLoaded, this, [this, deck](TrackPointer) {
                    m_tempos[deck - 1].at = -1;
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
    qApp->installEventFilter(this);
    set(kNative, "active", 1);
    selectPage(0);
    updateStatus();
}

WDeckLab::~WDeckLab() {
    m_fxTimer.stop();
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

QString WDeckLab::channel(int deck) const { return QStringLiteral("[Channel%1]").arg(deck); }

void WDeckLab::createControl(const QString& group, const QString& item, double initial) {
    if (!ControlObject::getControl(ConfigKey(group, item), ControlFlag::NoWarnIfMissing)) {
        auto object = std::make_unique<ControlObject>(ConfigKey(group, item));
        object->set(initial);
        m_ownedControls.emplace_back(std::move(object));
    }
}

ControlProxy* WDeckLab::control(const QString& group, const QString& item) {
    const QString key = group + QLatin1Char(',') + item;
    if (!m_controls.contains(key)) m_controls.insert(key, new ControlProxy(group, item, this, ControlFlag::NoWarnIfMissing));
    return m_controls.value(key);
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
        append(more, 68);
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
        more->setFixedHeight(48); actions->addWidget(more, 1);
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
        connect(pad, &QPushButton::pressed, this, [this, target, pressed, cue] {
            pressed->deck = target(); pressed->epoch = m_interactionEpoch;
            auto* player = m_players ? m_players->getPlayer(channel(pressed->deck)) : nullptr;
            pressed->track = player ? player->getLoadedTrack() : TrackPointer();
            pressed->position = value(channel(pressed->deck), QStringLiteral("hotcue_%1_position").arg(cue));
            pressed->playing = value(channel(pressed->deck), "play") > 0;
        });
        pads->addWidget(pad, 1); buttons[cue - 1] = pad;
    }
    m_hotcueButtons.insert(prefix, buttons);
    strip->setFixedHeight(44); return strip;
}

void WDeckLab::refreshHotcues(int deck, const QString& prefix) {
    const auto found = m_hotcueButtons.constFind(prefix);
    if (found == m_hotcueButtons.cend()) return;
    const QString group = channel(deck);
    const bool loaded = value(group, "track_loaded") > 0;
    const bool playing = value(group, "play") > 0;
    for (int cue = 1; cue <= 8; ++cue) {
        auto* pad = found.value()[cue - 1]; if (!pad) continue;
        const QString base = QStringLiteral("hotcue_%1_").arg(cue);
        const bool set = value(group, base + QStringLiteral("position")) >= 0;
        const bool loop = set && int(value(group, base + QStringLiteral("type"))) == int(mixxx::CueType::Loop);
        const QString letter(QChar('A' + cue - 1));
        const bool enabled = loaded && (set || !playing);
        const QString label = letter + (loop ? QString(QChar(0x21bb)) : set ? QString() : QStringLiteral("+"));
        if (pad->isEnabled() != enabled) pad->setEnabled(enabled);
        if (pad->text() != label) pad->setText(label);
        const int color = int(value(group, base + QStringLiteral("color")));
        const QString ink = set && color >= 0 && color <= 0xffffff ? QColor::fromRgb(color).name() : QStringLiteral("#555555");
        if (pad->property("cueSet").toBool() != set || pad->property("cueColor").toString() != ink) {
            pad->setProperty("cueSet", set); pad->setProperty("cueColor", ink); pad->update();
        }
        const QString style = QStringLiteral("min-height:40px;max-height:40px;padding:1px 2px;font-size:15px;font-weight:600;border-radius:0;border:1px solid #3c3c3c;");
        if (pad->styleSheet() != style) pad->setStyleSheet(style);
        const QString name = tr("Deck %1 cue %2, %3").arg(deck).arg(letter, loop ? tr("saved loop") : set ? tr("set") : tr("empty"));
        if (pad->accessibleName() != name) pad->setAccessibleName(name);
        const QString help = loop ? tr("Jump to saved loop %1 start; preserves playback and does not enable looping").arg(letter) :
                set ? tr("Jump to cue %1; preserves playback").arg(letter) : tr("Set cue %1 at the current paused position").arg(letter);
        if (pad->toolTip() != help) pad->setToolTip(help);
        if (pad->accessibleDescription() != help) pad->setAccessibleDescription(help);
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
        for (int side = 0; side < 2; ++side) {
            auto* meter = new QProgressBar(mainLevels); meter->setRange(0, 100);
            meter->setTextVisible(false); meter->setFixedHeight(10);
            const double initial = value("[Main]", side ? "vu_meter_right" : "vu_meter_left");
            meter->setValue(std::isfinite(initial) ? int(std::clamp(initial, 0.0, 1.0) * 100) : 0);
            meter->setObjectName(QStringLiteral("decklab-master-meter-%1").arg(side));
            meter->setAccessibleName(side ? tr("Main output right level") : tr("Main output left level"));
            levels->addWidget(meter); m_masterMeters[side] = meter;
            watch("[Main]", side ? "vu_meter_right" : "vu_meter_left", [meter](double v) {
                meter->setValue(std::isfinite(v) ? int(std::clamp(v, 0.0, 1.0) * 100) : 0);
            });
        }
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
    auto* meter = new QProgressBar(header); meter->setRange(0,100); meter->setValue(0); meter->setTextVisible(false); meter->setFixedSize(80,10);
    meter->setObjectName(QStringLiteral("decklab-level-%1").arg(deck));
    meter->setAccessibleName(tr("Deck %1 level").arg(deck)); titleLayout->addWidget(meter,0,Qt::AlignVCenter); m_meters[deck-1]=meter;
    watch(channel(deck),"vu_meter",[meter](double v){meter->setValue(std::isfinite(v)?int(std::clamp(v,0.0,1.0)*100):0);});
    watch(channel(deck),"peak_indicator",[this, deck, meter](double v){if(meter->property("clipping").toBool()!=(v>0)){meter->setProperty("clipping",v>0);meter->style()->unpolish(meter);meter->style()->polish(meter);}
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
    for (auto* child : header->findChildren<QWidget*>()) child->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* contextPanel = new QWidget(m_pairContexts); auto* contextRows = column(contextPanel);
    contextPanel->setFixedHeight(110); m_contextPanels[deck - 1] = contextPanel;
    m_waveContexts[deck - 1] = new QLabel(contextPanel);
    m_waveContexts[deck - 1]->setObjectName(QStringLiteral("decklab-wave-context-%1").arg(deck));
    m_waveContexts[deck - 1]->setTextFormat(Qt::RichText);
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
    auto* player = m_players ? m_players->getPlayer(channel(deck)) : nullptr;
    auto track = player ? player->getLoadedTrack() : TrackPointer();
    QString title = track ? track->getTitle() : tr("Choose a track in Browse");
    const double raw = value(channel(deck), "bpm");
    const double bpm = m_tempos[deck - 1].update(value(channel(deck), "track_loaded"),
            value(channel(deck), "local_bpm"), value(channel(deck), "rate_ratio"), raw, m_clock.elapsed());
    const double duration = value(channel(deck), "duration");
    QString key = KeyUtils::keyToString(static_cast<mixxx::track::io::key::ChromaticKey>(int(value(channel(deck), "visual_key"))));
    if (key.isEmpty()) key = QStringLiteral("—");
    if (m_titles[deck - 1]) {
        const bool compactTitle = m_page == 0 && m_fourDeckLayout;
        if (!track && compactTitle) title = tr("Browse tracks");
        m_titles[deck - 1]->setWordWrap(compactTitle);
        // Fixed empty guidance fits its final card; do not elide against a
        // newly revealed upper deck's not-yet-settled width of zero.
        m_titles[deck - 1]->setText(track ? m_titles[deck - 1]->fontMetrics().elidedText(title, Qt::ElideRight,
                std::max(0, (m_titles[deck - 1]->width() - 18) * (compactTitle ? 2 : 1))) : title);
        m_titles[deck - 1]->setToolTip(QStringLiteral("<span>%1</span>").arg(title.toHtmlEscaped()));
        if (m_waveNumbers[deck - 1]) {
            m_waveNumbers[deck - 1]->setVisible(m_page != 0);
            m_waveNumbers[deck - 1]->setText(track ? QStringLiteral("%1    %2 BPM").arg(key).arg(bpm, 0, 'f', 2) : QString());
        }
    }
    const int side = deck % 2 ? 0 : 1;
    if (m_numbers[deck - 1]) {
        double remaining = duration * (1 - value(channel(deck), "playposition"));
        const bool loaded = value(channel(deck), "track_loaded") > 0;
        const bool playing = value(channel(deck), "play") > 0;
        double pitch = (value(channel(deck), "rate_ratio") - 1) * 100;
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
        m_waveHeaders[deck - 1]->setEnabled(m_page == 0);
        m_waveHeaders[deck - 1]->setAccessibleName(track ? tr("Deck %1 full track information").arg(deck) : tr("Deck %1 empty; open Browse").arg(deck));
        m_waveHeaders[deck - 1]->setAccessibleDescription(track ? tr("Read the full title, artist and source") : tr("Choose a track in Browse; preserves the current browser location"));
        m_infoAffordances[deck - 1]->setText(track ? tr("INFO") : tr("BROWSE"));
        m_infoAffordances[deck - 1]->setFixedWidth(track ? 30 : 46);
        m_infoAffordances[deck - 1]->setVisible(m_page == 0);
        m_waveHeaders[deck - 1]->setFocusPolicy(m_page == 0 ? Qt::StrongFocus : Qt::NoFocus);
        m_waveHeaders[deck - 1]->setFixedHeight(m_page == 0 ? (compact ? 128 : 88) : 24);
        m_waveIdentities[deck - 1]->setFixedWidth(compact ? 20 : 32);
        m_titles[deck - 1]->setFixedHeight(compact ? 44 : paired ? 20 : 24);
        m_deckMetrics[deck - 1]->setVisible(m_page == 0);
        m_deckMetrics[deck - 1]->setFixedHeight(28);
        m_playStates[deck - 1]->setVisible(false);
        m_playStates[deck - 1]->setText(loaded ? (playing ? tr("PLAYING") : tr("PAUSED")) : tr("EMPTY"));
        m_numbers[deck - 1]->setText(compact
                        ? (track ? QString::number(bpm, 'f', 2) : QStringLiteral("—"))
                        : track ? QStringLiteral("%1 <span style='font-size:11px;font-weight:400;color:#aaa'>BPM</span>").arg(bpm, 0, 'f', 2) : QStringLiteral("— <span style='font-size:11px;font-weight:400;color:#aaa'>BPM</span>"));
        m_keys[deck - 1]->setText((compact ? QStringLiteral("%1") : QStringLiteral("<span style='font-size:11px;font-weight:400;color:#aaa'>KEY</span> %1")).arg(track ? key.toHtmlEscaped() : QStringLiteral("—")));
        m_remaining[deck - 1]->setText(track ? QStringLiteral("−") + timeText(remaining) : QStringLiteral("—"));
        m_pitch[deck - 1]->setText(loaded ? (compact ? pitchText : tr("PITCH %1").arg(pitchText)) : QString());
        QStringList modes;
        if (value(channel(deck), "loop_enabled") > 0)
            modes << (compact ? tr("LOOP %1").arg(beatLoopSizeText(value(channel(deck), "beatloop_size"))) : tr("LOOP"));
        if (value(channel(deck), "sync_enabled") > 0) modes << tr("SYNC LOCK");
        const bool leader = value(channel(deck), "sync_leader") > 0;
        const bool slip = value(channel(deck), "slip_enabled") > 0;
        if (!compact && leader) modes << tr("MASTER");
        if (!compact && slip) modes << tr("SLIP");
        if (compact && value(channel(deck), "keylock") > 0) modes << tr("KEYLOCK");
        if (compact && value(channel(deck), "quantize") > 0) modes << tr("QNT");
        const bool pfl = value(channel(deck), "pfl") > 0;
        QStringList compactModes;
        if (value(channel(deck), "sync_enabled") > 0) compactModes << tr("SYNC");
        if (leader) compactModes << tr("MASTER");
        if (slip) compactModes << tr("SLIP");
        if (value(channel(deck), "keylock") > 0) compactModes << tr("KEY");
        if (value(channel(deck), "quantize") > 0) compactModes << tr("QNT");
        QString compactLoop;
        for (const auto& mode : modes) if (mode.startsWith(tr("LOOP"))) compactLoop = mode;
        m_modes[deck - 1]->setText(compact ? tr("HP %1 %2\n%3").arg(pfl ? tr("ON") : tr("OFF"), compactLoop, compactModes.join(QLatin1Char(' '))) : modes.join(QStringLiteral(" · ")));
        m_modes[deck - 1]->setToolTip(modes.join(QStringLiteral(" · ")));
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
        m_numbers[deck - 1]->setFixedWidth(compact ? 108 : paired ? 125 : 150);
        m_keys[deck - 1]->setFixedWidth(compact ? 56 : paired ? 85 : 105);
        m_remaining[deck - 1]->setFixedWidth(compact ? 100 : paired ? 142 : 160);
        m_pitch[deck - 1]->setFixedWidth(compact ? 64 : 96);
        m_deckSecondary[deck - 1]->setFixedHeight(compact ? 28 : 18);
        m_modes[deck - 1]->setFixedHeight(compact ? 28 : 18);
        m_modes[deck - 1]->setStyleSheet(compact ? QStringLiteral("font-size:10px;padding-left:8px;") : QString());
        m_deckSecondary[deck - 1]->setVisible(m_page == 0);
        m_sources[deck - 1]->setVisible(paired);
        const double rawPosition = value(channel(deck), "playposition");
        const double position = std::isfinite(rawPosition) ? rawPosition : 0;
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
        m_sources[deck - 1]->setText(m_sources[deck - 1]->fontMetrics().elidedText(
                sourceLine, Qt::ElideRight, std::max(0, m_sources[deck - 1]->width() - 16)));
        m_sources[deck - 1]->setToolTip(sourceLine);
        QString beatText = tr("BEAT —");
        if (track && track->getBeats()) {
            const auto beats = track->getBeats();
            const auto frame = mixxx::audio::FramePos::fromEngineSamplePos(
                    position * value(channel(deck), "track_samples"));
            mixxx::audio::FramePos previous, next;
            if (frame.isValid() && beats->findPrevNextBeats(frame, &previous, &next, false) && next > previous) {
                const auto iterator = beats->iteratorFrom(previous);
                const double phase = (frame - previous) / (next - previous);
                beatText = tr("BEAT %1").arg(double(iterator - beats->cfirstmarker()) + 1 + phase, 0, 'f', 2);
            }
        }
        const QString stateText = loaded ? playing ? tr("PLAYING") : tr("PAUSED") : tr("EMPTY");
        m_waveContexts[deck - 1]->setText(m_page == 0 && m_sideBySideWaveforms
                ? tr("<b>D%1 · %2</b> · <span style='font-size:11px'>%3 · HP CUE %4</span>")
                          .arg(deck).arg(loaded ? (playing ? tr("PLAYING") : tr("PAUSED")) : tr("EMPTY"), beatText,
                                  value(channel(deck), "pfl") > 0 ? tr("ON") : tr("OFF"))
                : compact
                        ? QStringLiteral("<b style='font-size:12px'>D%1 · %2</b><br><span style='font-size:11px'>%3</span>")
                                  .arg(deck).arg(stateText, beatText.toHtmlEscaped())
                        : QStringLiteral("<b style='font-size:14px'>D%1 · %2</b><br><span style='font-size:12px'>%3<br>HP CUE %4</span><br><span style='font-size:11px;color:#aaa'>%5</span>")
                                  .arg(deck).arg(stateText, beatText.toHtmlEscaped(), pfl ? tr("ON") : tr("OFF"),
                                          modes.join(QStringLiteral(" · ")).toHtmlEscaped()));
        m_waveContexts[deck - 1]->setToolTip(tr("Beat index and phase relative to the stored beat grid's first marker; no bar or phrase inference"));
        if (m_overviews[deck - 1]) m_overviews[deck - 1]->setVisible(m_page == 0);
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
    if (displayedDeck(side) == deck) {
        refreshTransport(deck, QStringLiteral("decklab-%1-").arg(side + 1));
    }
    refreshTransport(deck, QStringLiteral("decklab-card-%1-").arg(deck));
    if (m_pairPerformance && m_pairPerformance->isVisible() && deck == (m_fourDeckLayout ? 4 : displayedDeck(1))) {
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
        m_pairEffects->setText(m_fourDeckLayout
                        ? tr("FX 1 %1 · 2 %2\nFX 3 %3 · 4 %4").arg(activeStates[0], activeStates[1], activeStates[2], activeStates[3])
                        : effects.join(QLatin1Char('\n')));
        m_pairEffects->setToolTip(detailedEffects.join(QLatin1Char('\n')));
        m_pairRMX->setText(tr("RMX · D%1\n%2").arg(m_fxDeck).arg(m_fxReleasing ? tr("TAIL") : m_fxActive ? tr("ON") : tr("DRY")));
        const double headMix = value("[Master]", "headMix");
        const int mainMix = std::isfinite(headMix) ? int(std::clamp((headMix + 1) * 50, 0.0, 100.0)) : 0;
        const bool split = value("[Master]", "headSplit") > 0;
        m_pairCue->setText(split ? tr("HP SPLIT") : mainMix == 0 ? tr("HP CUE") : mainMix == 100 ? tr("HP MAIN") : tr("HP %1% MAIN").arg(mainMix));
        m_pairCue->setToolTip(tr("Headphones: %1% toward main output; split cue %2. Per-deck HP CUE shows the selected cue feeds.").arg(mainMix).arg(split ? tr("on") : tr("off")));
        m_pairZoom->setText(tr("LINKED ZOOM"));
        m_pairZoom->setToolTip(tr("All four decks share the same waveform zoom; current value %1")
                                      .arg(value(channel(1), "waveform_zoom"), 0, 'g', 2));
    }
}

void WDeckLab::refreshTransport(int deck, const QString& prefix) {
    const bool loaded = value(channel(deck), "track_loaded") > 0;
    const bool playing = value(channel(deck), "play") > 0;
    if (auto* more = findChild<QPushButton*>(prefix + QStringLiteral("more")))
        more->setAccessibleName((prefix.startsWith(QStringLiteral("decklab-card-"))
                ? tr("Deck %1 more: hot cues and beatgrid")
                : tr("Deck %1 more: transport, track information and beatgrid")).arg(deck));
    if (prefix.startsWith(QStringLiteral("decklab-card-")))
        refreshHotcues(deck, QStringLiteral("decklab-card-hotcue-%1-").arg(deck));
    else
        refreshHotcues(deck, QStringLiteral("decklab-hotcue-%1-").arg(deck % 2 ? 1 : 2));
    for (const auto& action : {"play", "cue_default", "beatsync", "loop_halve", "beatloop_activate", "loop_double", "quantize", "keylock"}) {
        auto* b = findChild<QPushButton*>(prefix + QString::fromLatin1(action)); if (!b) continue;
        b->setEnabled(loaded);
        if (QString::fromLatin1(action) == "play") {
            b->setChecked(playing); b->setText(playing ? tr("Pause") : tr("Play"));
        } else if (QString::fromLatin1(action) == "beatloop_activate") {
            const bool looping = value(channel(deck), "loop_enabled") > 0;
            const QString sizeText = beatLoopSizeText(value(channel(deck), "beatloop_size"));
            b->setChecked(looping); b->setText(tr("LOOP\n%1").arg(sizeText));
            b->setAccessibleName(tr("Deck %1 loop %2 beats, %3").arg(deck).arg(sizeText, looping ? tr("on") : tr("off")));
            b->setAccessibleDescription(tr("Toggle the selected beat loop; half and double change its beat length"));
            continue;
        } else if (QString::fromLatin1(action) == "quantize" || QString::fromLatin1(action) == "keylock") {
            const bool on = value(channel(deck), QString::fromLatin1(action)) > 0;
            b->setChecked(on);
            b->setText((QString::fromLatin1(action) == "quantize" ? tr("QNT") : tr("KEY LOCK")) + QLatin1Char('\n') + (on ? tr("ON") : tr("OFF")));
        }
        b->setAccessibleName(tr("Deck %1 %2").arg(deck).arg(b->text()));
        if (QString::fromLatin1(action) == "loop_halve") b->setAccessibleName(tr("Deck %1 halve loop length").arg(deck));
        if (QString::fromLatin1(action) == "loop_double") b->setAccessibleName(tr("Deck %1 double loop length").arg(deck));
        if (QString::fromLatin1(action) == "quantize") b->setAccessibleName(tr("Deck %1 Quantize %2").arg(deck).arg(b->isChecked() ? tr("on") : tr("off")));
        if (QString::fromLatin1(action) == "beatsync") {
            const bool feedback = m_clock.elapsed() < m_syncFeedbackUntil[deck - 1];
            if (b->property("actionFeedback").toBool() != feedback) {
                b->setProperty("actionFeedback", feedback); b->style()->unpolish(b); b->style()->polish(b);
            }
            b->setAccessibleDescription(tr("Match tempo and beat once; does not enable sync lock"));
        }
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
    for (int deck = 1; deck <= 4; ++deck) refreshDeck(deck);
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
    m_status->setText(recording == 2 ? tr("REC") : recording == 3 ? tr("REC II") :
            (!m_outputConfigured || value("[SoundManager]", "status") != 2) ? tr("Audio !") :
            !m_toolsOnline ? tr("System !") : tr("Load !"));
    m_status->setToolTip(text.trimmed()); m_status->setAccessibleName(text.trimmed());
    m_status->setVisible(!text.isEmpty());
    if (m_performanceStatus) m_performanceStatus->setText(tr("Waveforms: %1 FPS · %2 dropped\nAudio overload count: %3")
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
