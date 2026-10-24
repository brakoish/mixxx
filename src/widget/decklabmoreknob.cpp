// SPDX-License-Identifier: GPL-2.0-or-later
#include "widget/decklabmoreknob.h"

#include <QEvent>
#include <QFrame>
#include <QKeyEvent>
#include <QMenu>
#include <QPushButton>
#include <algorithm>

#include "moc_decklabmoreknob.cpp"
#include "widget/decklabpads.h"

DeckLabMoreKnob::DeckLabMoreKnob(QMenu* menu, QWidget* sheet)
        : QObject(menu),
          m_menu(menu),
          m_sheet(sheet),
          m_ring(new QFrame(sheet)) {
    m_ring->setObjectName(QStringLiteral("decklab-more-knob-cursor"));
    m_ring->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_ring->setStyleSheet(QStringLiteral(
            "QFrame{background:transparent;border:2px solid #F2F4F7;border-radius:5px;}"));
    m_ring->hide();
    m_menu->installEventFilter(this);
}

QPushButton* DeckLabMoreKnob::cursorKey() const {
    return m_used ? m_cursor.data() : nullptr;
}

bool DeckLabMoreKnob::touchOnly(QPushButton* key) const {
    // Transport and performance keys act on a deck that may be on air: PLAY,
    // CUE (a held key) and the pad block (hot cues, loops, rolls, stems and
    // their mode keys). A press one detent off, or a bounce, must never stop
    // a deck or jump it to a cue.
    const QString name = key->objectName();
    if (name.endsWith(QStringLiteral("-play")) || name.endsWith(QStringLiteral("cue_default"))) {
        return true;
    }
    for (QWidget* widget = key; widget && widget != m_sheet; widget = widget->parentWidget()) {
        if (qobject_cast<DeckLabPadBlock*>(widget)) {
            return true;
        }
    }
    return false;
}

QList<QPushButton*> DeckLabMoreKnob::keys() const {
    QList<QPushButton*> usable;
    for (auto* key : m_sheet->findChildren<QPushButton*>()) {
        if (key->isVisible() && key->isEnabled() && !touchOnly(key)) {
            usable << key;
        }
    }
    // Reading order: rows top to bottom, then left to right.
    std::sort(usable.begin(), usable.end(), [this](QPushButton* a, QPushButton* b) {
        const QPoint pa = a->mapTo(m_sheet, QPoint());
        const QPoint pb = b->mapTo(m_sheet, QPoint());
        return pa.y() != pb.y() ? pa.y() < pb.y() : pa.x() < pb.x();
    });
    return usable;
}

void DeckLabMoreKnob::showCursor() {
    QPushButton* key = m_used ? m_cursor.data() : nullptr;
    if (!key || !key->isVisible()) {
        m_ring->hide();
        return;
    }
    const QRect area(key->mapTo(m_sheet, QPoint()), key->size());
    m_ring->setGeometry(area.adjusted(-3, -3, 3, 3));
    m_ring->raise();
    m_ring->show();
}

void DeckLabMoreKnob::move(int steps) {
    const QList<QPushButton*> usable = keys();
    if (usable.isEmpty()) {
        m_cursor = nullptr;
        showCursor();
        return;
    }
    int index = static_cast<int>(usable.indexOf(m_cursor.data()));
    if (index < 0) {
        index = 0;
    } else {
        index = std::clamp(index + steps, 0, static_cast<int>(usable.size()) - 1);
    }
    m_cursor = usable.at(index);
    showCursor();
}

bool DeckLabMoreKnob::eventFilter(QObject* watched, QEvent* event) {
    if (watched != m_menu) {
        return false;
    }
    if (event->type() == QEvent::Show || event->type() == QEvent::Hide) {
        // Every opening starts over: the first turn reveals.
        m_used = false;
        m_cursor = nullptr;
        m_ring->hide();
        return false;
    }
    if (event->type() != QEvent::KeyPress) {
        return false;
    }
    const int key = static_cast<QKeyEvent*>(event)->key();
    const bool turn = key == Qt::Key_Up || key == Qt::Key_Down;
    const bool press = key == Qt::Key_Return || key == Qt::Key_Enter;
    if (!turn && !press) {
        return false; // Escape closes the menu, as before.
    }
    if (!m_used && press) {
        // Before any turn a press closes MORE, as it always did: a press
        // from habit, or a double press, acts on no key.
        m_menu->close();
        return true;
    }
    if (!m_used || !m_cursor || !keys().contains(m_cursor.data())) {
        // Reveal (or the key under the cursor went away): show where the
        // cursor is and do nothing else.
        m_used = true;
        m_cursor = nullptr;
        move(0);
        return true;
    }
    if (turn) {
        move(key == Qt::Key_Up ? -1 : 1);
        return true;
    }
    QPointer<QPushButton> target = m_cursor;
    target->click();
    if (target && m_menu->isVisible()) {
        showCursor();
    }
    return true;
}
