// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabkeysheet.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QVBoxLayout>
#include <algorithm>

#include "library/decklabknob.h"
#include "moc_decklabkeysheet.cpp"
#include "widget/decklabpads.h"

namespace {
// The info line: 15 px secondary text (two decks) or a 14 px label (the
// four-deck card), muted; the match green once MATCH has landed (#2FD27A,
// the readout's match colour).
QString infoSheet(int pixels, bool matched) {
    return QStringLiteral("QLabel{color:%1;background:transparent;font-family:\"Barlow\";font-size:%2px;"
                          "font-weight:600;padding:0px 2px;}")
            .arg(matched ? QStringLiteral("#2FD27A") : QStringLiteral("#9AA3AF"))
            .arg(pixels);
}
// Barlow has the single guillemets, not the triangles.
const QString kLeft = QString::fromUtf8("‹ ");
const QString kRight = QString::fromUtf8(" ›");
} // namespace

DeckLabKeySheet::DeckLabKeySheet(const QString& name, Shape shape, Host host, QWidget* parent)
        : QWidget(parent),
          m_shape(shape),
          m_host(std::move(host)) {
    setObjectName(name);
    setFocusPolicy(Qt::NoFocus);
    setFixedHeight(kHeight);
    const auto make = [this, &name](Action action, const QString& text) {
        auto* key = new DeckLabPadButton(this);
        key->setObjectName(QString(name + QLatin1Char('-') + actionName(action)));
        key->setText(text);
        // Touch-down, like every pad on the surface.
        connect(key, &QPushButton::pressed, this, [this, action] { pressed(action); });
        m_keys[static_cast<int>(action)] = key;
        return key;
    };
    auto* down = make(Action::Down, QString(kLeft + QString::fromUtf8("−1")));
    auto* match = make(Action::Match, tr("MATCH"));
    auto* up = make(Action::Up, QString(QStringLiteral("+1") + kRight));
    auto* reset = make(Action::Reset, tr("RESET"));
    auto* done = make(Action::Done, tr("DONE"));
    down->setAccessibleDescription(tr("Shift the key down a semitone from what plays now; key lock turns on"));
    up->setAccessibleDescription(tr("Shift the key up a semitone from what plays now; key lock turns on"));
    match->setAccessibleDescription(tr("Shift to the nearest key that mixes with the other deck; key lock turns on"));
    reset->setAccessibleDescription(tr("Back to the track's own key, and key lock as it was"));
    done->setAccessibleName(tr("Close the key sheet"));
    m_info = new QLabel(this);
    m_info->setObjectName(QString(name + QStringLiteral("-info")));
    m_info->setFixedHeight(kRowHeight);
    m_info->setMinimumWidth(0);
    m_info->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto* rows = new QVBoxLayout(this);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(kGap);
    auto* top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(kGap);
    auto* bottom = new QHBoxLayout;
    bottom->setContentsMargins(0, 0, 0, 0);
    bottom->setSpacing(kGap);
    rows->addLayout(top);
    rows->addLayout(bottom);
    if (shape == Shape::Pads) {
        // [ info ][KEY LOCK][DONE] over [‹ −1][MATCH][+1 ›][RESET].
        m_info->setStyleSheet(infoSheet(15, false));
        m_info->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        auto* lock = make(Action::KeyLock, tr("KEY LOCK"));
        lock->setAccessibleDescription(tr("Key lock: the key stays put when the tempo moves"));
        lock->setMinimumWidth(84);
        done->setMinimumWidth(64);
        top->addWidget(m_info, 1);
        top->addWidget(lock);
        top->addWidget(done);
        for (auto* key : {down, match, up, reset}) {
            bottom->addWidget(key, 1);
        }
    } else {
        // Four-deck cards: [‹ −1][MATCH][+1 ›] over [RESET][info][DONE]. The
        // short info names MATCH's deck or says why it is off; KEY LOCK
        // stays in MORE, and the compact readout above shows the key.
        m_info->setStyleSheet(infoSheet(14, false));
        m_info->setAlignment(Qt::AlignCenter);
        for (auto* key : {down, match, up}) {
            top->addWidget(key, 1);
        }
        reset->setMinimumWidth(52);
        done->setMinimumWidth(52);
        bottom->addWidget(reset);
        bottom->addWidget(m_info, 1);
        bottom->addWidget(done);
    }
    refresh();
}

DeckLabKeySheet::~DeckLabKeySheet() {
    decklab::releaseKnob(this);
}

QString DeckLabKeySheet::actionName(Action action) {
    switch (action) {
    case Action::Down:
        return QStringLiteral("down");
    case Action::Match:
        return QStringLiteral("match");
    case Action::Up:
        return QStringLiteral("up");
    case Action::Reset:
        return QStringLiteral("reset");
    case Action::KeyLock:
        return QStringLiteral("keylock");
    case Action::Done:
        return QStringLiteral("done");
    }
    return QString();
}

QList<DeckLabKeySheet::Action> DeckLabKeySheet::knobOrder() const {
    // Reading order of the layout: left to right, top row first.
    if (m_shape == Shape::Pads) {
        return {Action::KeyLock, Action::Done, Action::Down, Action::Match, Action::Up, Action::Reset};
    }
    return {Action::Down, Action::Match, Action::Up, Action::Reset, Action::Done};
}

void DeckLabKeySheet::setState(const DeckLabKeySheetState& state) {
    if (state == m_state) {
        return;
    }
    m_state = state;
    refresh();
}

bool DeckLabKeySheet::usable(Action action) const {
    switch (action) {
    case Action::Match:
        return m_state.loaded && m_state.hasKey && m_state.target > 0 && !m_state.targetKey.isEmpty();
    case Action::Down:
    case Action::Up:
    case Action::Reset:
    case Action::KeyLock:
        return m_state.loaded;
    case Action::Done:
        return true;
    }
    return false;
}

QString DeckLabKeySheet::infoText() const {
    if (!m_state.loaded) {
        return tr("Deck %1 is empty").arg(m_state.deck);
    }
    if (m_state.matched && m_state.matchedDeck > 0) {
        return tr("MATCHED · DECK %1 %2").arg(m_state.matchedDeck).arg(m_state.matchedKey);
    }
    if (!m_state.hasKey) {
        return tr("This track has no key");
    }
    if (m_state.target <= 0) {
        return tr("No other deck to match");
    }
    if (m_state.targetKey.isEmpty()) {
        return tr("Deck %1 has no key").arg(m_state.target);
    }
    return tr("MATCH TO DECK %1 · %2").arg(m_state.target).arg(m_state.targetKey);
}

QString DeckLabKeySheet::shortInfoText() const {
    if (!m_state.loaded) {
        return QString();
    }
    if (m_state.matched && m_state.matchedDeck > 0) {
        return tr("MATCHED %1").arg(m_state.matchedDeck);
    }
    if (!m_state.hasKey || (m_state.target > 0 && m_state.targetKey.isEmpty())) {
        return tr("NO KEY");
    }
    if (m_state.target <= 0) {
        return tr("NO DECK");
    }
    return tr("TO %1 · %2").arg(m_state.target).arg(m_state.targetKey);
}

void DeckLabKeySheet::refresh() {
    const int deck = m_state.deck;
    for (const Action action : knobOrder()) {
        auto* key = m_keys[static_cast<int>(action)];
        const bool enabled = usable(action);
        if (key && key->isEnabled() != enabled) {
            key->setEnabled(enabled);
        }
    }
    if (auto* lock = key(Action::KeyLock)) {
        lock->setLit(m_state.loaded && m_state.keylock);
        const QString name = m_state.keylock ? tr("Deck %1 key lock, on").arg(deck) : tr("Deck %1 key lock, off").arg(deck);
        if (lock->accessibleName() != name) {
            lock->setAccessibleName(name);
        }
    }
    // MATCH is lit while the key it set still matches: the readout's key is
    // green and outlined at the same time.
    const bool matched = m_state.loaded && m_state.matched && m_state.matchedDeck > 0;
    if (auto* match = key(Action::Match)) {
        match->setLit(matched);
        const QString name = matched
                ? tr("Deck %1 key matched to deck %2 (%3)").arg(deck).arg(m_state.matchedDeck).arg(m_state.matchedKey)
                : !usable(Action::Match)
                ? tr("Deck %1 key match, unavailable: %2").arg(deck).arg(infoText())
                : tr("Match deck %1's key to deck %2 (%3)").arg(deck).arg(m_state.target).arg(m_state.targetKey);
        if (match->accessibleName() != name) {
            match->setAccessibleName(name);
        }
    }
    for (const auto& [action, words] :
            {std::pair{Action::Down, tr("Deck %1 key down a semitone")},
                    std::pair{Action::Up, tr("Deck %1 key up a semitone")},
                    std::pair{Action::Reset, tr("Deck %1 key back to the track's key")}}) {
        const QString name = words.arg(deck);
        if (key(action)->accessibleName() != name) {
            key(action)->setAccessibleName(name);
        }
    }
    const QString text = m_shape == Shape::Pads ? infoText() : shortInfoText();
    if (m_info->text() != text) {
        m_info->setText(text);
        m_info->setAccessibleName(infoText());
    }
    if (matched != m_infoMatched) {
        m_infoMatched = matched;
        m_info->setStyleSheet(infoSheet(m_shape == Shape::Pads ? 15 : 14, matched));
    }
    if (!usable(m_focus)) {
        m_focus = Action::Done;
    }
    showFocus();
}

void DeckLabKeySheet::pressed(Action action) {
    const int deck = m_state.deck;
    if (action == Action::Done) {
        if (m_host.act) {
            m_host.act(deck, action);
        }
        return;
    }
    // The pads' stale-touch guards, at touch-down: the sheet shows the deck
    // being touched (no layer change since its refresh) and its track.
    const int shown = m_host.deck ? m_host.deck() : deck;
    if (deck < 1 || deck > 4 || deck != shown || !m_state.loaded ||
            (m_host.current && !m_host.current(deck)) || !usable(action)) {
        return;
    }
    if (m_host.act) {
        m_host.act(deck, action);
    }
}

bool DeckLabKeySheet::revealCursor() {
    if (m_knobUsed) {
        return false;
    }
    // The first knob event shows where the cursor is (DONE, the safe key)
    // and does nothing else.
    m_knobUsed = true;
    m_focus = Action::Done;
    showFocus();
    return true;
}

void DeckLabKeySheet::moveFocus(int steps) {
    // Only keys that can act take the cursor, in reading order.
    QList<Action> keys;
    for (const Action action : knobOrder()) {
        if (m_keys[static_cast<int>(action)] && usable(action)) {
            keys << action;
        }
    }
    if (keys.isEmpty()) {
        return;
    }
    int index = std::max(0, static_cast<int>(keys.indexOf(m_focus)));
    index = std::clamp(index + steps, 0, static_cast<int>(keys.size()) - 1);
    m_focus = keys.at(index);
    showFocus();
}

void DeckLabKeySheet::showFocus() {
    for (const Action action : knobOrder()) {
        if (auto* key = m_keys[static_cast<int>(action)]) {
            key->setArmed(m_knobUsed && action == m_focus);
        }
    }
}

bool DeckLabKeySheet::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::Show:
        // The browse knob drives the sheet while it is shown.
        m_knobUsed = false;
        m_focus = Action::Done;
        refresh();
        decklab::claimKnob(this);
        break;
    case QEvent::Hide:
        decklab::releaseKnob(this);
        break;
    default:
        break;
    }
    return QWidget::event(event);
}

void DeckLabKeySheet::keyPressEvent(QKeyEvent* event) {
    // The knob claim sends Up/Down for a turn, Return for a press and Escape
    // for back (decklab::forwardKnob).
    switch (event->key()) {
    case Qt::Key_Up:
    case Qt::Key_Left:
    case Qt::Key_Backtab:
        if (!revealCursor()) {
            moveFocus(-1);
        }
        event->accept();
        return;
    case Qt::Key_Down:
    case Qt::Key_Right:
    case Qt::Key_Tab:
        if (!revealCursor()) {
            moveFocus(1);
        }
        event->accept();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        if (!revealCursor()) {
            pressed(m_focus);
        }
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
