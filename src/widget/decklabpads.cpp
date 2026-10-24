// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabpads.h"

#include <QApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <cmath>

#include "moc_decklabpads.cpp"

namespace {
// Brand tokens. Lit is a #F2F4F7 fill with ground ink; live red is never
// used here (a pad is not a live state).
const QColor kGround(0x0d, 0x0f, 0x12);
const QColor kPanel(0x16, 0x19, 0x1e);
const QColor kRaise(0x20, 0x24, 0x2b);
const QColor kLine(0x2c, 0x31, 0x3a);
const QColor kMuted(0x9a, 0xa3, 0xaf);
const QColor kText(0xf2, 0xf4, 0xf7);
const QColor kSoft(0xc9, 0xce, 0xd6);
const QColor kOff(0x11, 0x14, 0x18);
const QColor kOffLine(0x1e, 0x22, 0x28);
const QColor kOffInk(0x4b, 0x52, 0x5c);
const QColor kDash(0x6b, 0x73, 0x80);

// Each pad carries its own sheet: a surface or skin sheet with button
// heights would otherwise squash the 44 px targets on polish. Buttons are
// 16 px SemiBold Barlow, as everywhere on the surface.
const QString kPadSheet = QStringLiteral(
        "QPushButton{min-height:44px;max-height:44px;padding:0px;margin:0px;border:0px;"
        "font-family:\"Barlow\";font-size:16px;font-weight:600;}");

void setVisibleIf(QWidget* widget, bool visible) {
    if (!widget || (widget->testAttribute(Qt::WA_WState_ExplicitShowHide) &&
                           widget->isHidden() == !visible)) {
        return;
    }
    widget->setVisible(visible);
}
void setEnabledIf(QWidget* widget, bool enabled) {
    if (widget && widget->testAttribute(Qt::WA_ForceDisabled) == enabled) {
        widget->setEnabled(enabled);
    }
}
void setTextIf(QAbstractButton* button, const QString& text) {
    if (button && button->text() != text) {
        button->setText(text);
    }
}
void setToolTipIf(QWidget* widget, const QString& tip) {
    if (widget && widget->toolTip() != tip) {
        widget->setToolTip(tip);
    }
}
void setAccessibleNameIf(QWidget* widget, const QString& name) {
    if (widget && widget->accessibleName() != name) {
        widget->setAccessibleName(name);
    }
}
void setAccessibleDescriptionIf(QWidget* widget, const QString& text) {
    if (widget && widget->accessibleDescription() != text) {
        widget->setAccessibleDescription(text);
    }
}
bool sameSize(double a, double b) {
    return std::abs(a - b) < 1e-6;
}
} // namespace

DeckLabPadButton::DeckLabPadButton(QWidget* parent)
        : QPushButton(parent) {
    setFocusPolicy(Qt::NoFocus);
    // Lay out on the painted rect: the 4 px grid is exact (a style's layout
    // margins must not eat the gap between the two pad rows).
    setAttribute(Qt::WA_LayoutUsesWidgetRect);
    setStyleSheet(kPadSheet);
    setFixedHeight(DeckLabPadBlock::kPadHeight);
    setMinimumWidth(DeckLabPadBlock::kPadHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void DeckLabPadButton::setKind(Kind kind) {
    if (kind != m_kind) {
        m_kind = kind;
        update();
    }
}

void DeckLabPadButton::setLit(bool lit) {
    if (lit != m_lit) {
        m_lit = lit;
        update();
    }
}

void DeckLabPadButton::setArmed(bool armed) {
    if (armed != m_armed) {
        m_armed = armed;
        update();
    }
}

void DeckLabPadButton::setIndicator(bool on) {
    if (on != m_indicator) {
        m_indicator = on;
        update();
    }
}

void DeckLabPadButton::setCue(bool set, const QColor& color) {
    const QString ink = color.name();
    if (property("cueSet").toBool() != set || property("cueColor").toString() != ink) {
        setProperty("cueSet", set);
        setProperty("cueColor", ink);
        update();
    }
}

void DeckLabPadButton::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const bool enabled = isEnabled();
    const bool lamp = m_lit && enabled;
    QColor fill = !enabled ? kOff : isDown() ? kRaise : kPanel;
    QColor line = !enabled ? kOffLine : kLine;
    if (lamp) {
        fill = kText;
        line = kText;
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(line, 1));
    painter.setBrush(fill);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    painter.setFont(font());
    if (m_kind == Kind::Cue) {
        // Round 3's cue look: a dark pad with the cue's colour badge (its
        // waveform marker colour), or a dim letter in a dashed outline.
        if (!property("cueSet").toBool()) {
            if (enabled) {
                painter.setPen(QPen(kDash, 1, Qt::DashLine));
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(QRectF(rect()).adjusted(3.5, 3.5, -3.5, -3.5), 2, 2);
            }
            painter.setPen(enabled ? kMuted : kOffInk);
            painter.drawText(rect(), Qt::AlignCenter, text());
            return;
        }
        painter.setRenderHint(QPainter::Antialiasing, false);
        const QColor color(property("cueColor").toString());
        const int badgeWidth = text().size() > 1 ? 36 : 26;
        const QRect badge((width() - badgeWidth) / 2, (height() - 26) / 2, badgeWidth, 26);
        painter.fillRect(badge, color);
        // A neutral rim also makes deliberately black/white imported cues visible.
        painter.setPen(QColor(0x88, 0x88, 0x88));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(badge.adjusted(0, 0, -1, -1));
        const auto linear = [](double c) {
            return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
        };
        const double luminance = 0.2126 * linear(color.redF()) +
                0.7152 * linear(color.greenF()) + 0.0722 * linear(color.blueF());
        painter.setPen(luminance > 0.179 ? Qt::black : Qt::white);
        painter.drawText(badge, Qt::AlignCenter, text());
        return;
    }
    if (m_armed && enabled && !lamp) {
        // The armed loop length: an inner outline, not a fill. Two whole
        // pixels (3 and 4 from the edge): a 1.5 px stroke on a whole-pixel
        // rect smeared into the pad's fill and read as a grey lamp.
        painter.setPen(QPen(kMuted, 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(rect()).adjusted(4, 4, -4, -4), 2, 2);
    }
    painter.setPen(!enabled ? kOffInk : lamp ? kGround : m_armed ? kText : kSoft);
    painter.drawText(rect(), Qt::AlignCenter, text());
    if (m_indicator && enabled) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(lamp ? kGround : kText);
        painter.drawEllipse(QRectF(width() - 11, 5, 6, 6));
    }
}

DeckLabPadBlock::DeckLabPadBlock(const QString& name, const QString& padPrefix, Host host, QWidget* parent)
        : QWidget(parent),
          m_name(name),
          m_host(std::move(host)) {
    setObjectName(name);
    setAttribute(Qt::WA_StyledBackground);
    setProperty("deckSideRow", true);
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(6);
    // The mode cluster: four keys in two columns, so every key keeps a
    // 44 px target inside the two pad rows.
    m_modeCluster = new QWidget(this);
    m_modeCluster->setObjectName(name + QStringLiteral("-modes"));
    auto* modes = new QGridLayout(m_modeCluster);
    modes->setContentsMargins(0, 0, 0, 0);
    modes->setSpacing(kGap);
    for (const Mode mode : {Mode::Cue, Mode::Loop, Mode::Roll, Mode::Stem}) {
        const int index = static_cast<int>(mode);
        auto* key = new DeckLabPadButton(m_modeCluster);
        key->setObjectName(name + QStringLiteral("-mode-") + modeName(mode).toLower());
        key->setText(modeName(mode));
        key->setFixedWidth(kModeKeyWidth);
        // Modes switch on touch-down, like a controller's mode buttons.
        connect(key, &QPushButton::pressed, this, [this, mode] { modePressed(mode); });
        modes->addWidget(key, index / 2, index % 2);
        m_modeKeys[index] = key;
    }
    m_modeKeys[static_cast<int>(Mode::Stem)]->setAccessibleDescription(
            tr("Stem pads: shown only for a stem track"));
    // Two rows of four. In STEM mode the top row's pads give way to the
    // deck's stem pads and the bottom row's to ALL and SOLO; hidden widgets
    // take no room, so the columns stay on the same 4 px grid.
    m_padArea = new QWidget(this);
    m_padArea->setObjectName(name + QStringLiteral("-area"));
    auto* rows = new QVBoxLayout(m_padArea);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(kGap);
    for (int line = 0; line < 2; ++line) {
        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(kGap);
        rows->addLayout(row);
        m_rows[line] = row;
    }
    for (int index = 0; index < 8; ++index) {
        auto* pad = new DeckLabPadButton(m_padArea);
        pad->setObjectName(padPrefix + QString::number(index + 1));
        // Everything fires on touch-down; the release only ends a roll.
        connect(pad, &QPushButton::pressed, this, [this, index] { padPressed(index); });
        connect(pad, &QPushButton::released, this, [this, index] { padReleased(index); });
        m_rows[index / 4]->addWidget(pad, 1);
        m_pads[index] = pad;
    }
    // STEM mode's second row: ALL brings every stem back at once (no
    // double-tap glitch), SOLO latches: while lit, a touch solos a stem.
    m_all = new DeckLabPadButton(m_padArea);
    m_all->setObjectName(name + QStringLiteral("-stem-all"));
    m_all->setText(tr("ALL"));
    m_all->setAccessibleDescription(tr("Bring every stem back"));
    connect(m_all, &QPushButton::pressed, this, &DeckLabPadBlock::allPressed);
    m_rows[1]->addWidget(m_all, 1);
    m_solo = new DeckLabPadButton(m_padArea);
    m_solo->setObjectName(name + QStringLiteral("-stem-solo"));
    m_solo->setText(tr("SOLO"));
    m_solo->setAccessibleDescription(tr("While lit, touching a stem plays only that stem; touch SOLO again to leave"));
    connect(m_solo, &QPushButton::pressed, this, &DeckLabPadBlock::soloPressed);
    m_rows[1]->addWidget(m_solo, 1);
    m_all->hide();
    m_solo->hide();
    outer->addWidget(m_modeCluster);
    outer->addWidget(m_padArea, 1);
    setFixedHeight(kBlockHeight);
    applyMode(true);
}

DeckLabPadBlock::~DeckLabPadBlock() {
    // A roll must never outlive its screen (skin reload, teardown).
    endHolds();
    if (m_filtering && qApp) {
        qApp->removeEventFilter(this);
    }
}

QString DeckLabPadBlock::modeName(Mode mode) {
    switch (mode) {
    case Mode::Cue:
        return tr("CUE");
    case Mode::Loop:
        return tr("LOOP");
    case Mode::Roll:
        return tr("ROLL");
    case Mode::Stem:
        return tr("STEM");
    }
    return QString();
}

QString DeckLabPadBlock::sizeText(double beats) {
    if (!(beats > 0)) {
        return QStringLiteral("—");
    }
    if (beats >= 1) {
        return QString::number(beats, 'g', 4);
    }
    const double reciprocal = 1 / beats;
    if (std::abs(reciprocal - std::round(reciprocal)) < 1e-6) {
        return QStringLiteral("1/%1").arg(static_cast<qint64>(std::round(reciprocal)));
    }
    return QString::number(beats, 'g', 3);
}

void DeckLabPadBlock::setOuterMargins(int left, int top, int right, int bottom) {
    layout()->setContentsMargins(left, top, right, bottom);
    setFixedHeight(kBlockHeight + top + bottom);
}

void DeckLabPadBlock::setSideColumn(QWidget* column) {
    if (!column || m_side) {
        return;
    }
    m_side = column;
    column->setParent(this);
    column->setFixedWidth(kSideWidth);
    static_cast<QHBoxLayout*>(layout())->addWidget(column);
}

void DeckLabPadBlock::setPanel(QWidget* panel) {
    if (!panel || m_panel) {
        return;
    }
    m_panel = panel;
    panel->setParent(this);
    panel->hide();
    static_cast<QHBoxLayout*>(layout())->insertWidget(0, panel, 1);
}

void DeckLabPadBlock::showPanel(bool shown) {
    shown = shown && m_panel;
    if (shown == m_panelShown) {
        return;
    }
    m_panelShown = shown;
    if (shown) {
        // Nothing stays held behind the sheet.
        endHolds();
        setSoloLatched(false);
    }
    m_modeCluster->setVisible(!shown);
    m_padArea->setVisible(!shown);
    if (m_panel) {
        m_panel->setVisible(shown);
    }
    if (!shown) {
        applyMode(true);
    }
}

void DeckLabPadBlock::addStems(const Stems& stems) {
    if (!stems.pads) {
        return;
    }
    m_stems.append(stems);
    stems.pads->setParent(m_padArea);
    m_rows[0]->addWidget(stems.pads, 1);
    if (stems.setShown) {
        stems.setShown(false);
    }
    applyMode(false);
}

void DeckLabPadBlock::stemsChanged() {
    applyMode(false);
}

const DeckLabPadBlock::Stems* DeckLabPadBlock::stemsFor(int deck) const {
    for (const auto& stems : m_stems) {
        if (stems.deck == deck) {
            return &stems;
        }
    }
    return nullptr;
}

bool DeckLabPadBlock::stemsPresent(int deck) const {
    const Stems* stems = stemsFor(deck);
    return stems && stems->present && stems->present();
}

DeckLabPadBlock::Mode DeckLabPadBlock::chosenMode(int deck) const {
    return deck >= 1 && deck <= 4 ? m_modes[deck - 1] : Mode::Cue;
}

DeckLabPadBlock::Mode DeckLabPadBlock::mode() const {
    const Mode chosen = chosenMode(m_state.deck);
    return chosen == Mode::Stem && !stemsPresent(m_state.deck) ? Mode::Cue : chosen;
}

void DeckLabPadBlock::setMode(Mode mode) {
    const int deck = m_state.deck;
    if (deck < 1 || deck > 4 || (mode == Mode::Stem && !stemsPresent(deck))) {
        return;
    }
    if (m_modes[deck - 1] == mode && m_applied == mode) {
        return;
    }
    endHolds();
    m_modes[deck - 1] = mode;
    applyMode(false);
    emit modeChanged(deck, mode);
}

void DeckLabPadBlock::setChosenMode(int deck, Mode mode) {
    if (deck < 1 || deck > 4 || m_modes[deck - 1] == mode) {
        return;
    }
    m_modes[deck - 1] = mode;
    if (deck == m_state.deck) {
        endHolds();
        applyMode(false);
    }
}

void DeckLabPadBlock::modePressed(Mode mode) {
    setMode(mode);
    refreshModeKeys();
}

void DeckLabPadBlock::applyMode(bool force) {
    const Mode now = mode();
    const bool changed = force || !m_appliedOnce || now != m_applied;
    if (changed) {
        if (m_applied == Mode::Stem && now != Mode::Stem) {
            setSoloLatched(false);
        }
        m_applied = now;
        m_appliedOnce = true;
        const bool stem = now == Mode::Stem;
        for (auto* pad : m_pads) {
            setVisibleIf(pad, !stem);
        }
        setVisibleIf(m_all, stem);
        setVisibleIf(m_solo, stem);
    }
    // Only the shown deck's stem pads, and only in STEM mode. Hiding a deck's
    // pads ends any solo held on them (decklabstempads.cpp).
    for (const auto& stems : std::as_const(m_stems)) {
        if (stems.setShown) {
            stems.setShown(now == Mode::Stem && stems.deck == m_state.deck);
        }
    }
    refreshModeKeys();
    refreshPads(changed);
}

void DeckLabPadBlock::setDeckState(const DeckLabPadDeckState& state) {
    if (state == m_state) {
        return;
    }
    const bool deckChanged = state.deck != m_state.deck;
    if (deckChanged || state.loaded != m_state.loaded) {
        // A held roll belongs to the deck and track it started on.
        endHolds();
    }
    if (deckChanged) {
        setSoloLatched(false);
    }
    m_state = state;
    if (deckChanged) {
        applyMode(true);
    } else {
        refreshModeKeys();
        refreshPads(false);
    }
}

void DeckLabPadBlock::refreshModeKeys() {
    const Mode now = mode();
    const int deck = m_state.deck;
    const bool stems = stemsPresent(deck);
    for (const Mode key : {Mode::Cue, Mode::Loop, Mode::Roll, Mode::Stem}) {
        auto* button = m_modeKeys[static_cast<int>(key)];
        const bool selected = key == now;
        button->setKind(DeckLabPadButton::Kind::Value);
        button->setLit(selected);
        bool indicator = false;
        if (key == Mode::Stem) {
            // STEM appears only for a stem track.
            setTextIf(button, stems ? modeName(key) : QString());
            setEnabledIf(button, stems);
            const Stems* deckStems = stemsFor(deck);
            indicator = stems && !selected && deckStems && deckStems->anyMuted && deckStems->anyMuted();
        } else if (key == Mode::Loop) {
            indicator = !selected && m_state.loaded && m_state.looping;
        }
        button->setIndicator(indicator);
        setAccessibleNameIf(button,
                key == Mode::Stem && !stems
                        ? tr("Deck %1 stem pads, not available for this track").arg(deck)
                        : tr("Deck %1 pads: %2%3").arg(deck).arg(modeName(key),
                                  selected    ? tr(", selected")
                                          : indicator ? (key == Mode::Loop ? tr(", a loop is running")
                                                                           : tr(", stems muted"))
                                                      : QString()));
    }
}

void DeckLabPadBlock::refreshPads(bool force) {
    Q_UNUSED(force);
    const Mode now = mode();
    if (now == Mode::Stem) {
        return;
    }
    const int deck = m_state.deck;
    const bool loaded = m_state.loaded;
    for (int index = 0; index < 8; ++index) {
        auto* pad = m_pads[index];
        // Every pad is live while a track is loaded.
        setEnabledIf(pad, loaded);
        QString name;
        QString help;
        if (now == Mode::Cue) {
            const qint64 cue = m_state.cues[index];
            const bool set = cue >= 0;
            const bool loop = set && (cue >> 32) != 0;
            const QString letter(QChar('A' + index));
            pad->setKind(DeckLabPadButton::Kind::Cue);
            pad->setLit(false);
            pad->setArmed(false);
            setTextIf(pad, letter + (loop ? QString(QChar(0x21bb)) : QString()));
            pad->setCue(set, set ? QColor::fromRgb(static_cast<QRgb>(cue & 0xffffff)) : QColor(0x55, 0x55, 0x55));
            name = tr("Deck %1 cue %2, %3").arg(deck).arg(letter, loop ? tr("saved loop") : set ? tr("set") : tr("empty"));
            help = loop ? tr("Jump to saved loop %1 start; preserves playback and does not enable looping").arg(letter)
                    : set ? tr("Jump to cue %1; preserves playback").arg(letter)
                          : tr("Set cue %1 here").arg(letter);
        } else {
            const bool loopMode = now == Mode::Loop;
            const double size = loopMode ? kLoopSizes[index] : kRollSizes[index];
            const QString text = sizeText(size);
            pad->setKind(DeckLabPadButton::Kind::Value);
            setTextIf(pad, text);
            if (loopMode) {
                const bool active = loaded && m_state.looping && sameSize(m_state.loopSize, size);
                pad->setLit(active);
                pad->setArmed(loaded && !m_state.looping && sameSize(m_state.loopSize, size));
                name = tr("Deck %1 loop %2 beats, %3").arg(deck).arg(text, active ? tr("on") : tr("off"));
                help = active ? tr("Touch to end the loop")
                              : tr("Touch to loop %1 beats here").arg(text);
            } else {
                const bool held = m_rollDeck == deck && m_rollPad == index;
                pad->setLit(held);
                pad->setArmed(false);
                name = tr("Deck %1 roll %2 beats%3").arg(deck).arg(text, held ? tr(", rolling") : QString());
                help = tr("Hold to roll %1 beats; the track carries on underneath and resumes on release").arg(text);
            }
        }
        setAccessibleNameIf(pad, name);
        setToolTipIf(pad, help);
        setAccessibleDescriptionIf(pad, help);
    }
}

bool DeckLabPadBlock::shownAndCurrent(int deck) const {
    // Round 3's stale-touch guards, at touch-down: the pads must show the
    // deck being touched (no layer change since the last refresh), a loaded
    // track, and the same track the screen was refreshed for.
    const int target = m_host.deck ? m_host.deck() : deck;
    return deck >= 1 && deck <= 4 && deck == target && m_state.loaded &&
            (!m_host.current || m_host.current(deck));
}

void DeckLabPadBlock::padPressed(int index) {
    const int deck = m_state.deck;
    const Mode now = mode();
    if (now == Mode::Stem || index < 0 || index >= 8) {
        return;
    }
    if (!shownAndCurrent(deck)) {
        if (m_host.acted) {
            m_host.acted(m_host.deck ? m_host.deck() : deck);
        }
        return;
    }
    const auto pulse = [this, deck](const QString& item) {
        if (m_host.write) {
            m_host.write(deck, item, 1);
            m_host.write(deck, item, 0);
        }
    };
    switch (now) {
    case Mode::Cue: {
        // The touched cue must be set (or empty) both on screen and in the
        // engine; otherwise the pad was stale and only refreshes.
        const QString base = QStringLiteral("hotcue_%1_").arg(index + 1);
        const double position = m_host.read ? m_host.read(deck, base + QStringLiteral("position")) : -1;
        if ((m_state.cues[index] >= 0) == (position >= 0)) {
            pulse(base + (position >= 0 ? QStringLiteral("goto") : QStringLiteral("set")));
        }
        break;
    }
    case Mode::Loop:
        // Toggle: a new length starts (or resizes) a loop, the lit one ends it.
        pulse(QStringLiteral("beatloop_%1_toggle").arg(kLoopSizes[index]));
        break;
    case Mode::Roll:
        endHolds();
        m_rollDeck = deck;
        m_rollSize = kRollSizes[index];
        m_rollPad = index;
        if (m_host.write) {
            m_host.write(deck, QStringLiteral("beatlooproll_%1_activate").arg(m_rollSize), 1);
        }
        watchHold(true);
        refreshPads(false);
        break;
    case Mode::Stem:
        break;
    }
    if (m_host.acted) {
        m_host.acted(deck);
    }
}

void DeckLabPadBlock::padReleased(int index) {
    if (m_rollDeck > 0 && m_rollPad == index) {
        endHolds();
    }
}

void DeckLabPadBlock::endHolds() {
    if (m_rollDeck > 0) {
        const int deck = m_rollDeck;
        const double size = m_rollSize;
        m_rollDeck = 0;
        m_rollPad = -1;
        if (m_host.write) {
            m_host.write(deck, QStringLiteral("beatlooproll_%1_activate").arg(size), 0);
        }
        refreshPads(false);
    }
    watchHold(false);
}

void DeckLabPadBlock::allPressed() {
    const int deck = m_state.deck;
    if (mode() != Mode::Stem || !shownAndCurrent(deck)) {
        return;
    }
    if (const Stems* stems = stemsFor(deck); stems && stems->restoreAll) {
        stems->restoreAll();
    }
    refreshModeKeys();
}

void DeckLabPadBlock::soloPressed() {
    if (mode() != Mode::Stem) {
        return;
    }
    setSoloLatched(!m_soloLatched);
}

void DeckLabPadBlock::setSoloLatched(bool on) {
    if (on) {
        if (const Stems* stems = stemsFor(m_state.deck); stems && stems->setSoloLatch) {
            stems->setSoloLatch(true);
        }
    } else {
        for (const auto& stems : std::as_const(m_stems)) {
            if (stems.setSoloLatch) {
                stems.setSoloLatch(false);
            }
        }
    }
    m_soloLatched = on;
    if (m_solo) {
        m_solo->setLit(on);
        setAccessibleNameIf(m_solo, on ? tr("Deck %1 stem solo, on").arg(m_state.deck)
                                       : tr("Deck %1 stem solo, off").arg(m_state.deck));
    }
}

void DeckLabPadBlock::watchHold(bool on) {
    const bool wanted = on && m_rollDeck > 0;
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

bool DeckLabPadBlock::eventFilter(QObject* watched, QEvent* event) {
    // Installed on the application only while a roll is held.
    if (m_rollDeck > 0) {
        switch (event->type()) {
        case QEvent::TouchCancel:
        case QEvent::ApplicationDeactivate:
            endHolds();
            break;
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
            // One contact drives the pointer: a press or release reaching any
            // other widget ends the holding contact (round 3's rule for held
            // Lab FX). Window-level copies of the pad's own events pass.
            if (watched->isWidgetType() && m_rollPad >= 0 && watched != m_pads[m_rollPad]) {
                endHolds();
            }
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void DeckLabPadBlock::hideEvent(QHideEvent* event) {
    // A page, layer or layout change hides the block: end a roll at once and
    // drop the SOLO latch, so nothing is left held behind another screen.
    endHolds();
    setSoloLatched(false);
    QWidget::hideEvent(event);
}
