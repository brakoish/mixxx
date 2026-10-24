// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QObject>
#include <QPointer>

class QFrame;
class QMenu;
class QPushButton;
class QWidget;

// r6-sync: the browse knob in a deck's MORE sheet. An open menu gets the
// knob as keys (library/decklabknob.h forwardKnob: a turn is Up or Down, a
// press is Return, back is Escape). The sheet is one widget action full of
// touch keys, so Qt's menu navigation had nothing to move to and the knob
// could only close it; MASTER and INST DOUBLES (decklabsync.h) need the knob.
// The first knob event shows the cursor and does nothing else; a turn moves
// it through the keys that can act, in reading order; a press clicks the
// key under it. Hold-only keys (CUE) are left to touch. The cursor is a 2 px
// ring around the key, drawn in the 4 px gap between keys, so it reads on a
// lit key too. Nothing runs while the menu is closed.
class DeckLabMoreKnob final : public QObject {
    Q_OBJECT
  public:
    DeckLabMoreKnob(QMenu* menu, QWidget* sheet);

    // The key under the cursor, null before the first knob event.
    QPushButton* cursorKey() const;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    QList<QPushButton*> keys() const;
    void showCursor();
    void move(int steps);

    QMenu* m_menu;
    QWidget* m_sheet;
    QFrame* m_ring;
    QPointer<QPushButton> m_cursor;
    bool m_used = false;
};
