// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/wdecklab.h"

#include <QApplication>
#include <QBoxLayout>
#include <QComboBox>
#include <QDir>
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
#include <QSlider>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOptionButton>
#include <algorithm>
#include <cmath>

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
#include "track/track.h"
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
    for (const auto& name : {"active", "browse_press", "browse_back", "browse_sources", "tool_request", "tool_serial", "tools_ready"}) {
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
    auto* brand = new QLabel(QStringLiteral("DECK LAB"), this);
    brand->setProperty("muted", true);
    brand->setContentsMargins(10, 0, 10, 0);
    rail->addWidget(brand);
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
    m_waveLayout = column(m_waveforms);
    m_pages = new QStackedWidget(this);
    m_pages->setObjectName("decklab-pages");
    m_pages->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    m_pages->addWidget(new QWidget(m_pages));
    m_deckControls = buildDecks();
    m_pages->addWidget(buildBrowse());
    m_pages->addWidget(buildSampler());
    m_pages->addWidget(buildEffects());
    m_pages->addWidget(buildSettings());
    m_root->addWidget(m_waveforms, 1);
    m_root->addWidget(m_pages);
    m_root->addWidget(m_deckControls);
    m_status = new QLabel(this);
    m_status->setObjectName("decklab-status");
    m_status->setProperty("muted", true);
    m_status->setContentsMargins(9, 0, 9, 0);
    m_status->setFixedHeight(22);
    m_root->addWidget(m_status);
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
                refreshDeck(deck); refreshLoadButtons(); refreshFx();
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
    auto* b = name.startsWith("decklab-sample-") ? new DeckLabPad(text, parent ? parent : this) : new QPushButton(text, parent ? parent : this);
    b->setObjectName(name);
    b->setFocusPolicy(Qt::NoFocus);
    b->setAccessibleName(text);
    connect(b, &QPushButton::clicked, this, std::move(action));
    return b;
}

QWidget* WDeckLab::buildDecks() {
    auto* page = new QWidget(this);
    auto* layout = row(page);
    for (int deck = 1; deck <= 2; ++deck) {
        const int side = deck - 1;
        auto* strip = new QWidget(page);
        strip->setMinimumWidth(0);
        auto* contents = row(strip);
        contents->setContentsMargins(2, 2, 2, 2);
        m_deckIdentifiers[side] = new QLabel(QString::number(deck), strip);
        m_deckIdentifiers[side]->setObjectName(QStringLiteral("decklab-identity-%1").arg(deck));
        m_deckIdentifiers[side]->setProperty("deckIdentity", true);
        m_deckIdentifiers[side]->setFixedWidth(32);
        m_deckIdentifiers[side]->setAlignment(Qt::AlignCenter);
        contents->addWidget(m_deckIdentifiers[side]);
        auto* actions = row(nullptr, 0);
        for (const auto& pair : {qMakePair(tr("Cue"), QStringLiteral("cue_default")),
                     qMakePair(tr("Play"), QStringLiteral("play")),
                     qMakePair(tr("Beat Sync"), QStringLiteral("beatsync"))}) {
            auto* b = button(pair.first, [this, side, item = pair.second] {
                const int deck = displayedDeck(side);
                if (item == "play") set(channel(deck), item, !value(channel(deck), item));
                else if (item != "cue_default") {
                    // Mixxx's general beatsync phase-seeks only with Quantize.
                    // This action explicitly promises tempo AND phase once.
                    pulse(channel(deck), QStringLiteral("beatsync_tempo"));
                    pulse(channel(deck), QStringLiteral("beatsync_phase"));
                    m_syncFeedbackUntil[side] = m_clock.elapsed() + 600;
                    refreshDeck(deck);
                }
            }, strip, QStringLiteral("decklab-%1-%2").arg(deck).arg(pair.second));
            b->setFixedHeight(48);
            b->setProperty("transport", true);
            b->setProperty("first", pair.second == "cue_default");
            if (pair.second == "cue_default") {
                connect(b, &QPushButton::pressed, this, [this, side] { releaseTouchCue(); m_touchCue = channel(displayedDeck(side)); set(m_touchCue, "cue_default", 1); });
                connect(b, &QPushButton::released, this, &WDeckLab::releaseTouchCue);
            }
            if (pair.second == "play") {
                b->setCheckable(true);
                for (int layer : {deck, deck + 2}) watch(channel(layer), "play", [this, side, b](double) {
                    const bool playing = value(channel(displayedDeck(side)), "play") > 0;
                    b->setChecked(playing);
                    b->setText(playing ? tr("Pause") : tr("Play"));
                });
            }
            actions->addWidget(b);
        }
        contents->addLayout(actions, 1);
        layout->addWidget(strip, 1);
    }
    page->setFixedHeight(52);
    return page;
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
        const int matchDeck = value(channel(2), "play") && (!value(channel(1), "play") || value(channel(2), "sync_mode") == 2) ? 2 : 1;
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
    for (int deck = 1; deck <= 2; ++deck) {
        m_loadButtons[deck - 1] = button(tr("Load %1").arg(deck), [this, deck] { loadDeck(deck); }, m_browser,
                QStringLiteral("decklab-load-%1").arg(deck));
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
            for (int deck = 1; deck <= 2; ++deck) {
                auto* title = new QLabel(tr("DECK %1").arg(deck), content);
                title->setProperty("muted", true); rows->addWidget(title);
                auto* switches = row(nullptr, 4);
                switches->addWidget(toggle(tr("Quantize"), channel(deck), "quantize"));
                switches->addWidget(toggle(tr("Slip"), channel(deck), "slip_enabled"));
                switches->addWidget(toggle(tr("Master tempo"), channel(deck), "keylock"));
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
            for (int deck = 1; deck <= 2; ++deck) addRow(tr("Deck %1 headphones").arg(deck), toggle(tr("Cue"), channel(deck), "pfl"));
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
        rows->addStretch(); scroll->setWidget(content); pages->addWidget(scroll);
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
    auto* frame = new QWidget(m_waveforms);
    auto* layout = column(frame);
    auto* header = new QWidget(frame); header->setFixedHeight(m_page == 0 ? 48 : 24);
    m_waveHeaders[deck - 1] = header;
    auto* labels = row(header);
    auto* identity = new QLabel(QString::number(deck), header);
    identity->setObjectName(QStringLiteral("decklab-wave-identity-%1").arg(deck));
    identity->setProperty("deckIdentity", true);
    identity->setFixedWidth(32);
    identity->setAlignment(Qt::AlignCenter);
    identity->setAccessibleName(tr("Deck %1").arg(deck));
    labels->addWidget(identity);
    auto* content = new QWidget(header);
    auto* lines = column(content);
    auto* titleRow = new QWidget(content);
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
    auto* state = new QLabel(titleRow);
    state->setObjectName(QStringLiteral("decklab-state-%1").arg(deck));
    state->setProperty("muted", true); state->setFixedWidth(70); state->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    state->setContentsMargins(0, 0, 8, 0);
    m_playStates[deck - 1] = state; titleLayout->addWidget(state);
    auto* meter = new QProgressBar(header); meter->setRange(0,100); meter->setValue(0); meter->setTextVisible(false); meter->setFixedSize(60,6);
    meter->setAccessibleName(tr("Deck %1 level").arg(deck)); titleLayout->addWidget(meter,0,Qt::AlignVCenter); m_meters[deck-1]=meter;
    watch(channel(deck),"vu_meter",[meter](double v){meter->setValue(std::isfinite(v)?int(std::clamp(v,0.0,1.0)*100):0);});
    watch(channel(deck),"peak_indicator",[meter](double v){if(meter->property("clipping").toBool()!=(v>0)){meter->setProperty("clipping",v>0);meter->style()->unpolish(meter);meter->style()->polish(meter);}});
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
    values->addStretch(1);
    m_pitch[deck - 1] = metric("pitch", 150, false);
    m_pitch[deck - 1]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    lines->addWidget(metrics);
    layout->addWidget(header);
    widget->setMinimumHeight(16);
    widget->setMaximumHeight(QWIDGETSIZE_MAX);
    layout->addWidget(widget, 1);
    m_deckWaveforms[deck - 1] = frame;
    m_waveLayout->addWidget(frame, 1);
    frame->setVisible(deck <= 2);
    refreshDeck(deck);
}

void WDeckLab::bindBrowser(WSearchLineEdit* search, WLibrarySidebar* sidebar, WLibrary* library) {
    m_search = search; m_sidebar = sidebar; m_libraryWidget = library;
    search->setMinimumHeight(40); search->setMaximumHeight(40);
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
    releaseTouchCue();
    m_page = page;
    // Decks gets larger targets without moving companion utility windows.
    for (auto* tab : m_tabs) {
        tab->ensurePolished();
        tab->setFixedHeight(page == 0 ? 44 : 40);
    }
    if (auto* usb = findChild<QPushButton*>(QStringLiteral("decklab-usb"))) {
        usb->ensurePolished();
        usb->setFixedHeight(page == 0 ? 44 : 40);
    }
    // Keep the existing mapping's public page numbers: 4 = tools, 5 = settings.
    set(kTab, "current", page == 4 ? 5 : page);
    for (int n = 0; n < 5; ++n) m_tabs[n]->setChecked(n == page);
    m_pages->setCurrentIndex(page);
    // Transport has a fixed place below the Decks waveforms. Keeping it out
    // of the expanding page stack avoids sibling-page minimum sizes pushing
    // the essential controls below a small display.
    m_pages->setVisible(page != 0);
    m_deckControls->setVisible(page == 0);
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
    for (int deck = 1; deck <= 4; ++deck) refreshDeck(deck);
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
    deck = displayedDeck(deck - 1);
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
    for (int deck = 1; deck <= 2; ++deck) {
        auto* load = m_loadButtons[deck - 1];
        if (m_sampleTarget) {
            load->setText(deck == 1 ? tr("Load pad %1").arg(m_sampleTarget) : tr("Cancel"));
            load->setEnabled(deck == 2 || (m_libraryWidget && m_browseStack->currentWidget() == m_libraryWidget &&
                    selected && !value(QStringLiteral("[Sampler%1]").arg(m_sampleTarget), "play")));
            continue;
        }
        const int target = displayedDeck(deck - 1);
        const bool playing = value(channel(target), "play");
        load->setText(playing ? tr("Deck %1 · Playing").arg(target) : tr("Load %1").arg(target));
        load->setEnabled(!playing && m_libraryWidget && m_browseStack->currentWidget() == m_libraryWidget && selected);
    }
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
        m_titles[deck - 1]->setText(m_titles[deck - 1]->fontMetrics().elidedText(title, Qt::ElideRight,
                std::max(0, m_titles[deck - 1]->width() - 18)));
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
        m_waveHeaders[deck - 1]->setFixedHeight(m_page == 0 ? 48 : 24);
        m_titles[deck - 1]->setFixedHeight(m_page == 0 ? 20 : 24);
        m_deckMetrics[deck - 1]->setVisible(m_page == 0);
        m_playStates[deck - 1]->setVisible(m_page == 0);
        m_playStates[deck - 1]->setText(loaded ? (playing ? tr("PLAYING") : tr("PAUSED")) : tr("EMPTY"));
        m_numbers[deck - 1]->setText(track ? QStringLiteral("%1 <span style='font-size:11px;font-weight:400;color:#aaa'>BPM</span>").arg(bpm, 0, 'f', 2) : QStringLiteral("— <span style='font-size:11px;font-weight:400;color:#aaa'>BPM</span>"));
        m_keys[deck - 1]->setText(QStringLiteral("<span style='font-size:11px;font-weight:400;color:#aaa'>KEY</span> %1").arg(track ? key.toHtmlEscaped() : QStringLiteral("—")));
        m_remaining[deck - 1]->setText(QStringLiteral("<span style='font-size:11px;font-weight:400;color:#aaa'>REM</span> %1").arg(track ? QStringLiteral("−") + timeText(remaining) : QStringLiteral("—")));
        m_pitch[deck - 1]->setText(loaded ? tr("PITCH %1").arg(pitchText) : QString());
    }
    if (displayedDeck(side) == deck) {
        const bool loaded = value(channel(deck), "track_loaded") > 0;
        const bool playing = value(channel(deck), "play") > 0;
        m_deckIdentifiers[side]->setText(QString::number(deck));
        m_deckIdentifiers[side]->setAccessibleName(tr("Deck %1").arg(deck));
        for (const auto& action : {"play", "cue_default", "beatsync"}) {
            auto* button = findChild<QPushButton*>(QStringLiteral("decklab-%1-%2").arg(side + 1).arg(QString::fromLatin1(action)));
            if (!button) continue;
            button->setEnabled(loaded);
            if (QString::fromLatin1(action) == "play") {
                button->setChecked(playing);
                button->setText(playing ? tr("Pause") : tr("Play"));
                button->setAccessibleName(tr("Deck %1 %2").arg(deck).arg(playing ? tr("Pause") : tr("Play")));
            } else if (QString::fromLatin1(action) == "beatsync") {
                const bool feedback = m_clock.elapsed() < m_syncFeedbackUntil[side];
                if (button->property("actionFeedback").toBool() != feedback) {
                    button->setProperty("actionFeedback", feedback);
                    button->style()->unpolish(button); button->style()->polish(button);
                }
                button->setAccessibleDescription(tr("Match tempo and beat once; does not enable sync lock"));
            }
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
    m_status->setText(text);
    m_status->setVisible(!text.isEmpty());
    if (m_performanceStatus) m_performanceStatus->setText(tr("Waveforms: %1 FPS · %2 dropped\nAudio overload count: %3")
            .arg(m_measuredFps, 0, 'f', 1).arg(m_droppedFrames).arg(overloads));
}
bool WDeckLab::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_sidebar && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Up || key->key() == Qt::Key_Down) {
            set("[Playlist]", "SelectPlaylist", key->key() == Qt::Key_Up ? -key->count() : key->count()); return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter || key->key() == Qt::Key_Right) { openSource(); return true; }
        if (key->key() == Qt::Key_Left || key->key() == Qt::Key_Escape) { browseBack(); return true; }
    }
    if ((event->type() == QEvent::ApplicationDeactivate) ||
            event->type() == QEvent::TouchCancel) { releaseTouchCue(); releaseEffects(); }
    return QWidget::eventFilter(watched, event);
}
void WDeckLab::hideEvent(QHideEvent* event) { releaseTouchCue(); releaseEffects(); QWidget::hideEvent(event); }

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
    m_syncFeedbackUntil[side] = 0;
    m_layers[side] = deck;
}
void WDeckLab::updateLayers() {
    for (auto* frame : m_deckWaveforms) if (frame) m_waveLayout->removeWidget(frame);
    for (int deck = 1; deck <= 4; ++deck) {
        if (m_deckWaveforms[deck - 1]) m_deckWaveforms[deck - 1]->setVisible(deck == m_layers[0] || deck == m_layers[1]);
        refreshDeck(deck);
    }
    refreshLoadButtons();
    for (int side = 0; side < 2; ++side) {
        if (auto* play = findChild<QPushButton*>(QStringLiteral("decklab-%1-play").arg(side + 1)))
            play->setChecked(value(channel(displayedDeck(side)), "play") > 0);
        if (auto* frame = m_deckWaveforms[m_layers[side] - 1]) m_waveLayout->addWidget(frame, 1);
    }
}
