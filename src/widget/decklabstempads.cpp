// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabstempads.h"

#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QStringList>
#include <QVBoxLayout>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "engine/channels/enginedeck.h"
#include "engine/enginebuffer.h"
#include "mixer/basetrackplayer.h"
#include "mixer/decklabstemplayback.h"
#include "mixer/playermanager.h"
#include "moc_decklabstempads.cpp"
#include "track/track.h"

namespace {
// Brand neutrals. The stem colour is only an accent: pads stay dark, and
// selection (the held solo) is white, never red.
const QColor kRaise(0x20, 0x24, 0x2b);
const QColor kPressed(0x2c, 0x31, 0x3a);
const QColor kLine(0x2c, 0x31, 0x3a);
const QColor kOff(0x11, 0x14, 0x18);
const QColor kOffLine(0x1e, 0x22, 0x28);
const QColor kText(0xf2, 0xf4, 0xf7);
const QColor kMuted(0x9a, 0xa3, 0xaf);
constexpr int kNameSize = 16; // buttons: 16 px SemiBold Barlow
constexpr int kBarWidth = 6;

void setVisibleIf(QWidget* widget, bool visible) {
    if (!widget || (widget->testAttribute(Qt::WA_WState_ExplicitShowHide) &&
                           widget->isHidden() == !visible)) {
        return;
    }
    widget->setVisible(visible);
}

// Stem colours come from the file. Very dark ones are lifted so the accent
// stays visible on the dark pad; greys fall back to the muted neutral.
QColor visibleAccent(QColor color) {
    if (!color.isValid()) {
        return kMuted;
    }
    if (color.lightnessF() < 0.45f) {
        color = color.hslHueF() < 0 ? kMuted
                                    : QColor::fromHslF(color.hslHueF(),
                                              color.hslSaturationF(),
                                              0.55f);
    }
    return color;
}
} // namespace

DeckLabStemPad::DeckLabStemPad(DeckLabStemPads* owner, int stem, QWidget* parent)
        : QAbstractButton(parent),
          m_owner(owner),
          m_stem(stem) {
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(DeckLabStemPads::kPadHeight);
    setMinimumWidth(DeckLabStemPads::kPadHeight);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setAccessibleDescription(tr("Touch to mute or unmute at once. Hold to hear only this stem while held. "
                                "Double-tap to bring back all stems."));
}

void DeckLabStemPad::setState(const DeckLabStemPadState& state) {
    if (state == m_state) {
        return;
    }
    m_state = state;
    const QString name = tr("Deck %1 stem %2, %3")
                                 .arg(m_owner ? m_owner->deck() : 0)
                                 .arg(state.name,
                                         state.solo        ? tr("solo")
                                                 : state.muted ? tr("muted")
                                                               : tr("playing"));
    if (accessibleName() != name) {
        setAccessibleName(name);
    }
    update();
}

QSize DeckLabStemPad::sizeHint() const {
    return QSize(88, DeckLabStemPads::kPadHeight);
}

QSize DeckLabStemPad::minimumSizeHint() const {
    return QSize(DeckLabStemPads::kPadHeight, DeckLabStemPads::kPadHeight);
}

void DeckLabStemPad::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const bool live = !m_state.muted;
    const QColor accent = visibleAccent(m_state.color);
    // Body: live pads are raised; muted pads drop to the dim fill.
    const qreal border = m_state.solo ? 2.0 : 1.0;
    const QRectF body = QRectF(rect()).adjusted(border / 2, border / 2, -border / 2, -border / 2);
    painter.setPen(QPen(m_state.solo ? kText : live ? kLine : kOffLine, border));
    painter.setBrush(m_state.pressed ? kPressed : live ? kRaise : kOff);
    painter.drawRoundedRect(body, 3, 3);
    // Colour bar: solid while the stem plays, hollow and dim when muted.
    const QRectF bar(5, 7, kBarWidth, height() - 14);
    if (live) {
        painter.fillRect(bar, accent);
    } else {
        QColor dim = accent;
        dim.setAlphaF(0.5f);
        painter.setPen(QPen(dim, 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(bar.adjusted(0.75, 0.75, -0.75, -0.75));
    }
    QFont font;
    font.setFamilies({QStringLiteral("Barlow")});
    font.setPixelSize(kNameSize);
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    const QFontMetrics metrics(font);
    const QRect text = rect().adjusted(5 + kBarWidth + 7, 0, -6, 0);
    const QString label = metrics.elidedText(
            m_state.name.toUpper(), Qt::ElideRight, std::max(0, text.width()));
    painter.setPen(live ? kText : kMuted);
    painter.drawText(text, Qt::AlignVCenter | Qt::AlignLeft, label);
    if (!live && !label.isEmpty()) {
        // Muted reads without colour: a strike line through the name.
        const int width = std::min(text.width(), metrics.horizontalAdvance(label));
        const qreal y = text.center().y() + 0.5;
        painter.setPen(QPen(kMuted, 2));
        painter.drawLine(QPointF(text.left() - 1, y), QPointF(text.left() + width + 1, y));
    }
}

void DeckLabStemPad::mousePressEvent(QMouseEvent* event) {
    if (m_owner && event->button() == Qt::LeftButton) {
        m_owner->padPressed(m_stem);
    }
    event->accept();
}

void DeckLabStemPad::mouseReleaseEvent(QMouseEvent* event) {
    if (m_owner && event->button() == Qt::LeftButton) {
        m_owner->padReleased(m_stem);
    }
    event->accept();
}

void DeckLabStemPad::mouseMoveEvent(QMouseEvent* event) {
    event->accept();
}

void DeckLabStemPad::hideEvent(QHideEvent* event) {
    if (m_owner) {
        m_owner->padHidden(m_stem);
    }
    QAbstractButton::hideEvent(event);
}

DeckLabStemPads::DeckLabStemPads(int deck,
        Presentation presentation,
        PlayerManagerInterface* players,
        std::function<quint64()> epoch,
        QWidget* parent)
        : QWidget(parent),
          m_deck(deck),
          m_presentation(presentation),
          m_players(players),
          m_epoch(std::move(epoch)),
          m_group(QStringLiteral("[Channel%1]").arg(deck)),
          m_gestures([this](int stem) { return muted(stem); },
                  [this](int stem, bool on) { setMuted(stem, on); }) {
    const bool row = presentation == Presentation::Row;
    const QString prefix = row ? QStringLiteral("decklab-stem-%1-").arg(deck)
                               : QStringLiteral("decklab-card-stem-%1-").arg(deck);
    setObjectName(row ? QStringLiteral("decklab-stems-%1").arg(deck)
                      : QStringLiteral("decklab-card-stems-%1").arg(deck));
    m_clock.start();
    auto* layout = new QHBoxLayout(this);
    // Like the hot-cue row: 4 px gap above, 4 px margins and gaps.
    if (row) {
        layout->setContentsMargins(4, 4, 4, 0);
    } else {
        layout->setContentsMargins(0, 0, 0, 0);
    }
    layout->setSpacing(4);
    for (int stem = 0; stem < DeckLabStemGestures::kStems; ++stem) {
        m_stemGroups[stem] = QStringLiteral("[Channel%1_Stem%2]").arg(deck).arg(stem + 1);
        auto* pad = new DeckLabStemPad(this, stem, this);
        pad->setObjectName(prefix + QString::number(stem + 1));
        layout->addWidget(pad, 1);
        m_pads[stem] = pad;
    }
    setFixedHeight(row ? kRowHeight : kPadHeight);
    m_count = new ControlProxy(m_group, QStringLiteral("stem_count"), this, ControlFlag::NoWarnIfMissing);
    m_loaded = new ControlProxy(m_group, QStringLiteral("track_loaded"), this, ControlFlag::NoWarnIfMissing);
    for (int stem = 0; stem < DeckLabStemGestures::kStems; ++stem) {
        m_mute[stem] = new ControlProxy(m_stemGroups[stem],
                QStringLiteral("mute"),
                this,
                ControlFlag::NoWarnIfMissing);
        m_color[stem] = new ControlProxy(m_stemGroups[stem],
                QStringLiteral("color"),
                this,
                ControlFlag::NoWarnIfMissing);
        // Mutes may also change from a controller or the engine (load reset).
        m_mute[stem]->connectValueChanged(
                this, [this](double) { refreshPads(); }, Qt::QueuedConnection);
        m_color[stem]->connectValueChanged(
                this, [this](double) { refreshPads(); }, Qt::QueuedConnection);
    }
    m_count->connectValueChanged(
            this, [this](double) { refresh(); }, Qt::QueuedConnection);
    m_loaded->connectValueChanged(
            this,
            [this](double) {
                ++m_loadSerial;
                refresh();
            },
            Qt::QueuedConnection);
    if (auto* deckPlayer = player()) {
        // A load in progress ends a gesture on the old track at once.
        connect(deckPlayer,
                &BaseTrackPlayer::loadingTrack,
                this,
                [this](TrackPointer, TrackPointer) { trackChanging(false); });
        connect(deckPlayer,
                &BaseTrackPlayer::newTrackLoaded,
                this,
                [this](TrackPointer) { trackChanging(true); });
        connect(deckPlayer,
                &BaseTrackPlayer::trackUnloaded,
                this,
                [this](TrackPointer) { trackChanging(true); });
    }
    m_holdTimer.setSingleShot(true);
    m_holdTimer.setInterval(static_cast<int>(DeckLabStemGestures::kHoldMs));
    connect(&m_holdTimer, &QTimer::timeout, this, &DeckLabStemPads::holdElapsed);
    setVisible(false);
    refresh();
}

DeckLabStemPads::~DeckLabStemPads() {
    // Never leave a stem soloed behind a destroyed surface (skin reload).
    m_holdTimer.stop();
    m_gestures.cancel(false);
    for (auto* pad : m_pads) {
        pad->detach();
    }
    if (m_filtering) {
        qApp->removeEventFilter(this);
    }
}

void DeckLabStemPads::setRowShown(bool shown) {
    m_rowShown = shown;
    if (m_presentation == Presentation::Row) {
        setVisibleIf(this, shown);
    }
}

bool DeckLabStemPads::rowWanted(const std::array<DeckLabStemPads*, 4>& pads,
        const std::array<int, 2>& displayedDecks) {
    for (int deck : displayedDecks) {
        if (deck >= 1 && deck <= 4 && pads[deck - 1] && pads[deck - 1]->present()) {
            return true;
        }
    }
    return false;
}

QWidget* DeckLabStemPads::createPlaybackSetting(UserSettingsPointer config, QWidget* parent) {
#ifdef __STEM__
    // One full settings row: caption on the left (top-aligned with the
    // choice, like the other rows), the choice and a one-line explanation on
    // the right. Touch screens show no tool tips.
    auto* setting = new QWidget(parent);
    setting->setObjectName(QStringLiteral("decklab-stem-playback-setting"));
    auto* line = new QHBoxLayout(setting);
    line->setContentsMargins(0, 0, 0, 0);
    line->setSpacing(12);
    auto* caption = new QLabel(tr("Stem tracks"), setting);
    caption->setObjectName(QStringLiteral("decklab-stem-playback-caption"));
    caption->setMinimumWidth(160);
    caption->setFixedHeight(kPadHeight);
    caption->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    line->addWidget(caption, 1, Qt::AlignTop);
    auto* choice = new QVBoxLayout();
    choice->setContentsMargins(0, 0, 0, 0);
    choice->setSpacing(4);
    auto* combo = new QComboBox(setting);
    combo->setObjectName(QStringLiteral("decklab-stem-playback"));
    combo->addItem(tr("Play as stems"), static_cast<int>(decklab::StemPlayback::Stems));
    combo->addItem(tr("Play as normal track"), static_cast<int>(decklab::StemPlayback::StereoMix));
    combo->setCurrentIndex(
            decklab::stemPlayback(config) == decklab::StemPlayback::StereoMix ? 1 : 0);
    combo->setAccessibleName(tr("Stem tracks"));
    // 44 px target (42 px plus the 1 px border).
    combo->setStyleSheet(QStringLiteral("QComboBox{min-height:42px;max-height:42px;}"));
    QObject::connect(combo,
            qOverload<int>(&QComboBox::activated),
            combo,
            [config, combo](int index) {
                config->setValue(decklab::stemPlaybackConfigKey(),
                        combo->itemData(index).toInt());
            });
    choice->addWidget(combo);
    auto* hint = new QLabel(tr("From the next load. As a normal track the stems play as one "
                               "stereo mix: no stem pads, lighter on the processor."),
            setting);
    hint->setObjectName(QStringLiteral("decklab-stem-playback-hint"));
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("font-size:15px;font-weight:500;color:#9AA3AF;"));
    combo->setAccessibleDescription(hint->text());
    choice->addWidget(hint);
    line->addLayout(choice, 1);
    return setting;
#else
    Q_UNUSED(config);
    Q_UNUSED(parent);
    return nullptr;
#endif
}

bool DeckLabStemPads::eventFilter(QObject* watched, QEvent* event) {
    // Installed on the application only while a pad is down: a touch
    // cancel or losing the screen ends the gesture like a release.
    if (event->type() == QEvent::TouchCancel || event->type() == QEvent::ApplicationDeactivate) {
        cancelGesture(false);
    }
    return QWidget::eventFilter(watched, event);
}

void DeckLabStemPads::hideEvent(QHideEvent* event) {
    cancelGesture(false);
    QWidget::hideEvent(event);
}

void DeckLabStemPads::padPressed(int stem) {
    // The mute flips on touch-down; holding on turns the touch into a solo.
    if (m_gestures.press(stem, context(), m_clock.elapsed()) == DeckLabStemGestures::Result::Ignored) {
        return;
    }
    m_holdTimer.start();
    watchGestureEnd();
    refreshPads();
}

void DeckLabStemPads::padReleased(int stem) {
    if (m_gestures.pressedStem() != stem) {
        return;
    }
    m_holdTimer.stop();
    m_gestures.release(context());
    watchGestureEnd();
    refreshPads();
}

void DeckLabStemPads::padHidden(int stem) {
    if (m_gestures.pressedStem() == stem) {
        cancelGesture(false);
    }
}

void DeckLabStemPads::holdElapsed() {
    const int stem = m_gestures.pressedStem();
    if (stem < 0) {
        return;
    }
    if (!m_pads[stem] || !m_pads[stem]->isVisible()) {
        cancelGesture(false);
        return;
    }
    m_gestures.hold(context());
    refreshPads();
}

void DeckLabStemPads::cancelGesture(bool trackReplaced) {
    m_holdTimer.stop();
    if (m_gestures.active()) {
        m_gestures.cancel(trackReplaced);
        refreshPads();
    }
    watchGestureEnd();
}

void DeckLabStemPads::trackChanging(bool replaced) {
    ++m_loadSerial;
    cancelGesture(replaced);
    refresh();
}

void DeckLabStemPads::watchGestureEnd() {
    const bool wanted = m_gestures.active();
    if (wanted == m_filtering || !qApp) {
        return;
    }
    if (wanted) {
        qApp->installEventFilter(this);
    } else {
        qApp->removeEventFilter(this);
    }
    m_filtering = wanted;
}

DeckLabStemGestures::Context DeckLabStemPads::context() const {
    return {m_epoch ? m_epoch() : 0, m_loadSerial};
}

bool DeckLabStemPads::muted(int stem) const {
    return stem >= 0 && stem < DeckLabStemGestures::kStems && m_mute[stem]->get() > 0;
}

void DeckLabStemPads::setMuted(int stem, bool on) {
    if (stem < 0 || stem >= DeckLabStemGestures::kStems || !m_mute[stem]->valid()) {
        return;
    }
    // The shared write path notifies every proxy, including other surfaces
    // showing this deck (the More menu, controller feedback).
    ControlObject::set(ConfigKey(m_stemGroups[stem], QStringLiteral("mute")), on ? 1.0 : 0.0);
}

BaseTrackPlayer* DeckLabStemPads::player() const {
    return m_players ? m_players->getPlayer(m_group) : nullptr;
}

bool DeckLabStemPads::playsAsStems() const {
#ifdef __STEM__
    // A stem file loaded as one stereo mix (Settings > Deck > Stem tracks,
    // or Mixxx's pre-mixed load) still reports its stems in stem_count, but
    // its stem mutes do nothing. The engine buffer knows how it was loaded.
    const auto* impl = qobject_cast<BaseTrackPlayerImpl*>(player());
    if (!impl || !impl->getEngineDeck()) {
        return true;
    }
    return !impl->getEngineDeck()->getEngineBuffer()->getStemMask();
#else
    return false;
#endif
}

void DeckLabStemPads::refresh() {
    int count = 0;
#ifdef __STEM__
    if (m_count->valid() && m_loaded->get() > 0 && playsAsStems()) {
        count = std::clamp(static_cast<int>(m_count->get()), 0, DeckLabStemGestures::kStems);
    }
#endif
    const bool present = count > 0;
    // Names come from the track's stem manifest (Mixxx already substitutes
    // "Stem #n" for a missing one); generic names otherwise.
    QStringList labels;
#ifdef __STEM__
    if (present) {
        if (auto* deckPlayer = player()) {
            if (const auto track = deckPlayer->getLoadedTrack()) {
                for (const auto& info : track->getStemInfo()) {
                    labels.append(info.getLabel().trimmed());
                }
            }
        }
    }
#endif
    for (int stem = 0; stem < DeckLabStemGestures::kStems; ++stem) {
        const QString label = stem < labels.size() ? labels.at(stem) : QString();
        m_names[stem] = label.isEmpty() ? tr("Stem %1").arg(stem + 1) : label;
    }
    m_gestures.setCount(count);
    const bool changed = present != m_present;
    m_present = present;
    setVisibleIf(this, m_presentation == Presentation::Row ? m_rowShown : present);
    for (int stem = 0; stem < DeckLabStemGestures::kStems; ++stem) {
        setVisibleIf(m_pads[stem], stem < count);
    }
    refreshPads();
    if (changed) {
        emit presenceChanged(present);
    }
}

void DeckLabStemPads::refreshPads() {
    for (int stem = 0; stem < DeckLabStemGestures::kStems; ++stem) {
        DeckLabStemPadState state;
        state.name = m_names[stem];
        const double code = m_color[stem]->get();
        // [ChannelN_StemM],color is the manifest colour as 0xRRGGBB, -1 if none.
        state.color = code >= 0 && code <= 0xffffff
                ? QColor::fromRgb(static_cast<QRgb>(static_cast<int>(code)))
                : QColor();
        state.muted = muted(stem);
        state.solo = m_gestures.soloStem() == stem;
        state.pressed = m_gestures.pressedStem() == stem && !state.solo;
        m_pads[stem]->setState(state);
    }
}
