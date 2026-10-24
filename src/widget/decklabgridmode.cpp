// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabgridmode.h"

#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "library/decklabknob.h"
#include "moc_decklabgridmode.cpp"
#include "widget/decklabdeckwidgets.h"
#include "widget/decklabpads.h"

namespace {
// Brand tokens: ground, line, muted, text; play green for PLAYING. Live red
// is not used: an edit is not a live state, and the deck colour carries it.
const QColor kGround(0x0d, 0x0f, 0x12);
const QColor kLine(0x2c, 0x31, 0x3a);
const QColor kMuted(0x9a, 0xa3, 0xaf);
const QColor kText(0xf2, 0xf4, 0xf7);
const QColor kPlay(0x2f, 0xd2, 0x7a);

QFont uiFont(int pixels, QFont::Weight weight) {
    QFont font;
    font.setFamilies({QStringLiteral("Barlow")});
    font.setPixelSize(pixels);
    font.setWeight(weight);
    return font;
}

QFont numeralFont(int pixels) {
    QFont font;
    font.setFamilies({QStringLiteral("Barlow Semi Condensed"), QStringLiteral("Barlow")});
    font.setPixelSize(pixels);
    font.setWeight(QFont::DemiBold);
    return font;
}

QFont labelFont() {
    QFont font = uiFont(14, QFont::DemiBold);
    font.setLetterSpacing(QFont::PercentageSpacing, 106);
    return font;
}

// A transparent place in the layout that the panel paints over.
QWidget* spacer(QWidget* parent, const QString& name) {
    auto* widget = new QWidget(parent);
    widget->setObjectName(name);
    widget->setAttribute(Qt::WA_TransparentForMouseEvents);
    return widget;
}
} // namespace

DeckLabGridPanel::DeckLabGridPanel(const QString& name, Host host, QWidget* parent)
        : QWidget(parent),
          m_host(std::move(host)) {
    setObjectName(name);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumHeight(kMinimumHeight);
    const auto make = [this, &name](Action action, const QString& text, const QString& spoken) {
        auto* key = new DeckLabPadButton(this);
        key->setObjectName(name + QLatin1Char('-') + actionName(action));
        key->setText(text);
        key->setAccessibleName(spoken);
        // Touch-down, like every pad on the surface (TAP must be on time).
        connect(key, &QPushButton::pressed, this, [this, action] { pressed(action); });
        m_keys[static_cast<int>(action)] = key;
        return key;
    };
    // Barlow has the single guillemets, not the triangles.
    const QString left = QString::fromUtf8("‹ ");
    const QString right = QString::fromUtf8(" ›");
    auto* lock = make(Action::Lock, tr("LOCK"), tr("Grid lock"));
    auto* done = make(Action::Done, tr("DONE"), tr("Leave grid editing"));
    auto* earlier = make(Action::NudgeEarlier, QString(left + tr("GRID")), tr("Move the grid earlier"));
    auto* later = make(Action::NudgeLater, QString(tr("GRID") + right), tr("Move the grid later"));
    auto* beatEarlier = make(Action::BeatEarlier, QString(left + tr("BEAT")), tr("Move beat 1 a beat earlier"));
    auto* beatLater = make(Action::BeatLater, QString(tr("BEAT") + right), tr("Move beat 1 a beat later"));
    auto* downCoarse = make(Action::BpmDownCoarse, QString::fromUtf8("−0.1"), tr("Grid BPM down 0.1"));
    auto* downFine = make(Action::BpmDownFine, QString::fromUtf8("−0.01"), tr("Grid BPM down 0.01"));
    auto* upFine = make(Action::BpmUpFine, QStringLiteral("+0.01"), tr("Grid BPM up 0.01"));
    auto* upCoarse = make(Action::BpmUpCoarse, QStringLiteral("+0.1"), tr("Grid BPM up 0.1"));
    auto* halve = make(Action::Halve, QString::fromUtf8("÷2"), tr("Half grid BPM"));
    auto* twice = make(Action::Double, QString::fromUtf8("×2"), tr("Double grid BPM"));
    auto* tap = make(Action::Tap, tr("TAP"), tr("Tap the grid BPM"));
    auto* downbeat = make(Action::Downbeat, tr("DOWNBEAT"), tr("Put beat 1 at the playhead"));
    tap->setAccessibleDescription(tr("Tap four times or more on the beat"));
    downbeat->setAccessibleDescription(tr("A beat at the playhead, counted as beat 1"));
    lock->setMinimumWidth(64);
    done->setMinimumWidth(72);
    auto* rows = new QVBoxLayout(this);
    rows->setContentsMargins(kGap, kRule + kGap, kGap, kGap);
    rows->setSpacing(kGap);
    const auto line = [rows] {
        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(kGap);
        rows->addLayout(row);
        return row;
    };
    auto* header = line();
    m_title = spacer(this, name + QStringLiteral("-title"));
    m_title->setFixedHeight(kRowHeight);
    header->addWidget(m_title, 1);
    header->addWidget(lock);
    header->addWidget(done);
    auto* moves = line();
    for (auto* key : {earlier, later, beatEarlier, beatLater}) {
        moves->addWidget(key, 1);
    }
    auto* tempo = line();
    tempo->addWidget(downCoarse, 1);
    tempo->addWidget(downFine, 1);
    m_readout = spacer(this, name + QStringLiteral("-readout"));
    m_readout->setFixedHeight(kRowHeight);
    tempo->addWidget(m_readout, 2);
    tempo->addWidget(upFine, 1);
    tempo->addWidget(upCoarse, 1);
    auto* scale = line();
    for (auto* key : {halve, twice, tap, downbeat}) {
        scale->addWidget(key, 1);
    }
    m_statusLine = spacer(this, name + QStringLiteral("-status"));
    m_statusLine->setFixedHeight(kStatusHeight);
    rows->addWidget(m_statusLine);
    rows->addStretch(1);
    refresh();
}

DeckLabGridPanel::~DeckLabGridPanel() {
    decklab::releaseKnob(this);
}

QString DeckLabGridPanel::actionName(Action action) {
    switch (action) {
    case Action::NudgeEarlier:
        return QStringLiteral("beats_translate_earlier");
    case Action::NudgeLater:
        return QStringLiteral("beats_translate_later");
    case Action::BeatEarlier:
        return QStringLiteral("beat-earlier");
    case Action::BeatLater:
        return QStringLiteral("beat-later");
    case Action::BpmDownCoarse:
        return QStringLiteral("bpm-down-coarse");
    case Action::BpmDownFine:
        return QStringLiteral("beats_adjust_slower");
    case Action::BpmUpFine:
        return QStringLiteral("beats_adjust_faster");
    case Action::BpmUpCoarse:
        return QStringLiteral("bpm-up-coarse");
    case Action::Halve:
        return QStringLiteral("beats_set_halve");
    case Action::Double:
        return QStringLiteral("beats_set_double");
    case Action::Tap:
        return QStringLiteral("bpm_tap");
    case Action::Downbeat:
        return QStringLiteral("downbeat");
    case Action::Lock:
        return QStringLiteral("bpmlock");
    case Action::Done:
        return QStringLiteral("done");
    case Action::Count:
        break;
    }
    return QString();
}

void DeckLabGridPanel::setState(const DeckLabGridState& state) {
    if (state == m_state) {
        return;
    }
    const DeckLabGridState previous = m_state;
    m_state = state;
    refresh();
    // Repaint only the painted parts that changed.
    if (previous.deck != state.deck || previous.loaded != state.loaded) {
        update();
        return;
    }
    if (previous.playing != state.playing) {
        update(titleRect());
    }
    if (previous.bpm != state.bpm || previous.beat != state.beat) {
        update(readoutRect());
    }
}

bool DeckLabGridPanel::editable() const {
    return m_state.loaded && m_state.hasBeats && !m_state.locked && !m_state.synced && m_state.sharedDeck == 0;
}

bool DeckLabGridPanel::usable(Action action) const {
    switch (action) {
    case Action::Done:
        return true;
    case Action::Lock:
        return m_state.loaded && m_state.sharedDeck == 0;
    case Action::BeatEarlier:
    case Action::BeatLater:
    case Action::Downbeat:
        return editable() && m_state.constantTempo;
    case Action::Halve:
        return editable() && m_state.canHalve;
    default:
        return editable();
    }
}

QString DeckLabGridPanel::playText() const {
    return !m_state.loaded ? QString() : m_state.playing ? tr("PLAYING") : tr("PAUSED");
}

QString DeckLabGridPanel::status() const {
    if (!m_state.loaded) {
        return tr("Deck %1 is empty").arg(m_state.deck);
    }
    if (m_state.sharedDeck > 0) {
        return tr("Deck %1 plays or syncs it too. DONE, then stop it.").arg(m_state.sharedDeck);
    }
    if (m_state.synced) {
        return tr("SYNC is on. DONE, then turn SYNC off.");
    }
    if (!m_state.hasBeats) {
        return tr("No beat grid yet. Analyse the track first.");
    }
    if (m_state.locked) {
        return tr("Grid locked. Touch LOCK to edit.");
    }
    if (!m_state.constantTempo) {
        return tr("Variable-tempo grid: no BEAT moves. BPM edits flatten it.");
    }
    if (!m_ownsKnob) {
        return tr("Touch the keys · the knob is in use elsewhere");
    }
    return m_knob == Knob::Nudge ? tr("KNOB  NUDGE · press for BPM · Shift+press DONE")
                                 : tr("KNOB  BPM ±0.01 · press for NUDGE · Shift+press DONE");
}

bool DeckLabGridPanel::ownsKnob() const {
    const decklab::KnobOwner owner = decklab::knobOwner();
    return isVisible() && owner.claimed && owner.blocker == this;
}

void DeckLabGridPanel::refreshKnob() {
    if (ownsKnob() != m_ownsKnob) {
        refresh();
    }
}

void DeckLabGridPanel::setKnob(Knob knob) {
    if (knob == m_knob) {
        return;
    }
    m_knob = knob;
    refresh();
}

void DeckLabGridPanel::refresh() {
    m_ownsKnob = ownsKnob();
    for (int index = 0; index < static_cast<int>(Action::Count); ++index) {
        auto* key = m_keys[index];
        const bool enabled = usable(static_cast<Action>(index));
        if (key && key->isEnabled() != enabled) {
            key->setEnabled(enabled);
        }
    }
    auto* lock = key(Action::Lock);
    lock->setLit(m_state.loaded && m_state.locked);
    const QString lockName = m_state.locked ? tr("Deck %1 grid lock, locked").arg(m_state.deck)
                                            : tr("Deck %1 grid lock, unlocked").arg(m_state.deck);
    if (lock->accessibleName() != lockName) {
        lock->setAccessibleName(lockName);
    }
    // The knob's function is a lamp on the keys it stands for, only while
    // the knob is really this panel's.
    const bool knob = m_ownsKnob && editable();
    key(Action::NudgeEarlier)->setArmed(knob && m_knob == Knob::Nudge);
    key(Action::NudgeLater)->setArmed(knob && m_knob == Knob::Nudge);
    key(Action::BpmDownFine)->setArmed(knob && m_knob == Knob::Bpm);
    key(Action::BpmUpFine)->setArmed(knob && m_knob == Knob::Bpm);
    const QString text = status();
    if (text != m_shownStatus) {
        m_shownStatus = text;
        update(statusRect());
    }
    const QString description = tr("Deck %1 grid editing, %2. %3").arg(m_state.deck).arg(playText().toLower(), text);
    if (accessibleDescription() != description) {
        setAccessibleDescription(description);
    }
}

void DeckLabGridPanel::pressed(Action action) {
    const int deck = m_state.deck;
    if (action == Action::Done) {
        if (m_host.act) {
            m_host.act(deck, action);
        }
        return;
    }
    // Stale-touch guards at touch-down: the deck still holds the track GRID
    // mode was opened for, and the edit is possible right now.
    if (deck < 1 || deck > 4 || !m_state.loaded || (m_host.current && !m_host.current(deck)) || !usable(action)) {
        return;
    }
    if (m_host.act) {
        m_host.act(deck, action);
    }
}

void DeckLabGridPanel::knobTurn(int steps) {
    if (steps == 0) {
        return;
    }
    const Action action = m_knob == Knob::Nudge ? (steps < 0 ? Action::NudgeEarlier : Action::NudgeLater)
                                                : (steps < 0 ? Action::BpmDownFine : Action::BpmUpFine);
    for (int count = std::min(std::abs(steps), 50); count > 0; --count) {
        pressed(action);
    }
}

bool DeckLabGridPanel::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::Show:
        // The browse knob adjusts the grid while GRID mode is shown.
        decklab::claimKnob(this);
        QWidget::event(event);
        refresh();
        return true;
    case QEvent::Hide:
        decklab::releaseKnob(this);
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

void DeckLabGridPanel::keyPressEvent(QKeyEvent* event) {
    // The knob claim sends Up/Down for a turn, Return for a press and Escape
    // for back (decklab::forwardKnob).
    switch (event->key()) {
    case Qt::Key_Up:
    case Qt::Key_Left:
        knobTurn(-1);
        event->accept();
        return;
    case Qt::Key_Down:
    case Qt::Key_Right:
        knobTurn(1);
        event->accept();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        setKnob(m_knob == Knob::Nudge ? Knob::Bpm : Knob::Nudge);
        event->accept();
        return;
    case Qt::Key_Escape:
        pressed(Action::Done);
        event->accept();
        return;
    default:
        QWidget::keyPressEvent(event);
    }
}

QRect DeckLabGridPanel::titleRect() const {
    return m_title ? m_title->geometry() : QRect();
}

QRect DeckLabGridPanel::readoutRect() const {
    return m_readout ? m_readout->geometry() : QRect();
}

QRect DeckLabGridPanel::statusRect() const {
    return m_statusLine ? m_statusLine->geometry() : QRect();
}

void DeckLabGridPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    update();
}

void DeckLabGridPanel::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    painter.fillRect(event->rect(), kGround);
    const QColor deck = deckLabDeckColor(m_state.deck);
    // The deck colour along the top ties the controls to the deck's lane.
    painter.fillRect(QRect(0, 0, width(), kRule), deck);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    const QRect title = titleRect();
    if (title.intersects(event->rect())) {
        // [N] EDITING GRID (filled in the deck colour), then PLAYING / PAUSED.
        const QRect badge(title.left(), title.top() + (title.height() - 28) / 2, 28, 28);
        QPainterPath box;
        box.addRoundedRect(QRectF(badge).adjusted(0.75, 0.75, -0.75, -0.75), 3, 3);
        painter.setPen(QPen(deck, 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(box);
        painter.setFont(numeralFont(18));
        painter.setPen(deck);
        painter.drawText(badge, Qt::AlignCenter, QString::number(m_state.deck));
        const QFont chipFont = uiFont(16, QFont::DemiBold);
        const QString words = tr("EDITING GRID");
        const int chipWidth = QFontMetrics(chipFont).horizontalAdvance(words) + 16;
        const QRect chip(badge.right() + 7, badge.top(), chipWidth, 28);
        QPainterPath fill;
        fill.addRoundedRect(QRectF(chip), 3, 3);
        painter.fillPath(fill, deck);
        painter.setFont(chipFont);
        painter.setPen(kGround);
        painter.drawText(chip, Qt::AlignCenter, words);
        const QString state = playText();
        if (!state.isEmpty()) {
            painter.setFont(labelFont());
            painter.setPen(m_state.playing ? kPlay : kMuted);
            const QRect word(chip.right() + 9, title.top(), std::max(0, title.right() - chip.right() - 9), title.height());
            painter.drawText(word, Qt::AlignVCenter | Qt::AlignLeft, state);
        }
    }
    const QRect readout = readoutRect();
    if (readout.intersects(event->rect())) {
        // The grid BPM at readout size, and the beat of four under it.
        const QFont number = numeralFont(24);
        const QFont unit = labelFont();
        const QString bpm = m_state.loaded && !m_state.bpm.isEmpty() ? m_state.bpm : QString::fromUtf8("—");
        const int numberWidth = QFontMetrics(number).horizontalAdvance(bpm);
        const QString bpmWord = tr("BPM");
        const int unitWidth = QFontMetrics(unit).horizontalAdvance(bpmWord);
        const int left = readout.left() + std::max(0, (readout.width() - numberWidth - 4 - unitWidth) / 2);
        const int baseline = readout.top() + 26;
        painter.setFont(number);
        painter.setPen(kText);
        painter.drawText(QPoint(left, baseline), bpm);
        painter.setFont(unit);
        painter.setPen(kMuted);
        painter.drawText(QPoint(left + numberWidth + 4, baseline), bpmWord);
        const int cell = std::max(8, std::min(20, (readout.width() - 3 * 4) / 4));
        const int cellsLeft = readout.left() + (readout.width() - (4 * cell + 3 * 4)) / 2;
        for (int beat = 0; beat < 4; ++beat) {
            const QRect area(cellsLeft + beat * (cell + 4), readout.top() + 33, cell, 6);
            painter.fillRect(area, beat == m_state.beat && m_state.loaded ? deck : kLine);
        }
    }
    const QRect status = statusRect();
    if (status.intersects(event->rect())) {
        painter.setFont(uiFont(15, QFont::DemiBold));
        const bool legend = editable();
        painter.setPen(legend ? kMuted : kText);
        const QString text = QFontMetrics(painter.font()).elidedText(m_shownStatus, Qt::ElideRight, status.width());
        painter.drawText(status, Qt::AlignVCenter | Qt::AlignLeft, text);
    }
}
