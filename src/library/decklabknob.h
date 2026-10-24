// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Deck Lab: who owns the controller's browse knob right now (audit C-01).
//
// Ownership is decided from what is actually open when the event arrives,
// never from a cached focus classification. The kiosk has no window manager
// to re-activate Mixxx after a menu, dialog or utility window closes, so the
// focus signals that refresh a cached state can be missing; a stale "menu" or
// "dialog" state then left the knob dead until the screen was tapped.
//
// Shared by LibraryControl (generic mappings) and the Browse page (scripts
// that write [DeckLabNative] browse_* directly), so both entry points use the
// same check, the same forwarding and the same log line.

#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QList>
#include <QMenu>
#include <QMetaObject>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QWidget>
#include <QtDebug>
#include <algorithm>
#include <cmath>
#include <functional>

namespace decklab {

enum class KnobAction {
    Move,  // turn: |steps| rows
    Press, // knob press: open, preview, or select in an open menu
    Enter, // move right: open only
    Back,  // back, or close an open menu or dialog
};

struct KnobOwner {
    QWidget* blocker = nullptr; // the menu or modal dialog that is really open
    bool popup = false;
    bool claimed = false; // r4-system: a Deck Lab page or sheet (claimKnob)
    bool browse() const {
        return blocker == nullptr;
    }
};

// r4-system: Deck Lab pages and sheets that the knob drives while they are
// shown (Settings, the rail status sheet, Deck Lab sheets, Deck Setup), like a
// CDJ's UTILITY screen. The most recent visible claim owns the knob after real
// popups and modal dialogs; it gets Up/Down for a turn, Return for a press and
// Escape for back. Claims are weak: a destroyed or hidden claimant never
// keeps the knob.
inline QList<QPointer<QWidget>>& knobClaims() {
    static QList<QPointer<QWidget>> claims;
    return claims;
}

// r5-decks: who owns the knob may have changed (a claim, a release, or a
// menu or dialog opening or closing, which the surface reports). Listeners
// run queued, on the GUI thread, once the change has settled, so a sheet can
// hide its knob cursor and a panel its knob lamps when the knob goes
// elsewhere, and show them again only after the knob comes back and is used.
inline QList<QPair<QPointer<QObject>, std::function<void()>>>& knobOwnerListeners() {
    static QList<QPair<QPointer<QObject>, std::function<void()>>> listeners;
    return listeners;
}

inline void onKnobOwnerChanged(QObject* receiver, std::function<void()> callback) {
    if (receiver && callback) {
        knobOwnerListeners().append({QPointer<QObject>(receiver), std::move(callback)});
    }
}

inline void noteKnobOwnerChanged() {
    auto& listeners = knobOwnerListeners();
    for (int index = static_cast<int>(listeners.size()) - 1; index >= 0; --index) {
        if (listeners.at(index).first.isNull()) {
            listeners.removeAt(index);
        }
    }
    for (const auto& [receiver, callback] : std::as_const(listeners)) {
        QMetaObject::invokeMethod(receiver.data(), callback, Qt::QueuedConnection);
    }
}

inline void releaseKnob(QWidget* widget) {
    auto& claims = knobClaims();
    bool changed = false;
    for (int index = static_cast<int>(claims.size()) - 1; index >= 0; --index) {
        if (claims.at(index).isNull() || claims.at(index).data() == widget) {
            changed |= claims.at(index).data() == widget && widget;
            claims.removeAt(index);
        }
    }
    if (changed) {
        noteKnobOwnerChanged();
    }
}

inline void claimKnob(QWidget* widget) {
    if (!widget) {
        return;
    }
    auto& claims = knobClaims();
    for (int index = static_cast<int>(claims.size()) - 1; index >= 0; --index) {
        if (claims.at(index).isNull() || claims.at(index).data() == widget) {
            claims.removeAt(index);
        }
    }
    claims.append(QPointer<QWidget>(widget));
    noteKnobOwnerChanged();
}

inline QWidget* knobClaimant() {
    const auto& claims = knobClaims();
    for (int index = static_cast<int>(claims.size()) - 1; index >= 0; --index) {
        QWidget* widget = claims.at(index).data();
        if (widget && widget->isVisible()) {
            return widget;
        }
    }
    return nullptr;
}

inline KnobOwner knobOwner() {
    KnobOwner owner;
    if (QWidget* popup = QApplication::activePopupWidget()) {
        owner.blocker = popup;
        owner.popup = true;
    } else if (QWidget* modal = QApplication::activeModalWidget()) {
        owner.blocker = modal;
    } else if (QWidget* claim = knobClaimant()) {
        owner.blocker = claim;
        owner.claimed = true;
    }
    return owner;
}

inline QString describeKnobOwner(const KnobOwner& owner) {
    if (!owner.blocker) {
        return QStringLiteral("Browse owns it");
    }
    const QString kind = QString::fromLatin1(owner.blocker->metaObject()->className());
    const QString name = owner.blocker->objectName();
    return QStringLiteral("%1 is open (%2%3)")
            .arg(owner.claimed ? QStringLiteral("a Deck Lab page")
                            : owner.popup ? QStringLiteral("a menu")
                                          : QStringLiteral("a dialog"),
                    kind,
                    name.isEmpty() ? QString() : QStringLiteral(" \"") + name + QLatin1Char('"'));
}

// Knob events that went to an open menu or dialog instead of Browse.
inline int& knobEventsRedirected() {
    static int count = 0;
    return count;
}

// r4-browse: called for every redirected knob event (the Browse knob legend
// says MENU and flashes). GUI thread only; empty when no surface listens.
inline std::function<void()>& knobRedirectedListener() {
    static std::function<void()> listener;
    return listener;
}

// "DeckLab knob: [Library] MoveVertical ignored by Browse because a menu is
// open (QMenu "x"); the menu gets it". Debug level (always in mixxx.log),
// at most twice a second for the same reason.
inline void logKnobRedirected(const char* control, const KnobOwner& owner) {
    ++knobEventsRedirected();
    if (knobRedirectedListener()) {
        knobRedirectedListener()();
    }
    static QElapsedTimer last;
    static QString lastReason;
    const QString reason = QString::fromLatin1(control) + describeKnobOwner(owner);
    if (last.isValid() && last.elapsed() < 500 && reason == lastReason) {
        return;
    }
    last.start();
    lastReason = reason;
    qDebug().noquote() << QStringLiteral("DeckLab knob: %1 ignored by Browse because %2; the %3 gets it")
                                  .arg(QString::fromLatin1(control),
                                          describeKnobOwner(owner),
                                          owner.claimed ? QStringLiteral("page")
                                                  : owner.popup ? QStringLiteral("menu")
                                                                : QStringLiteral("dialog"));
}

// The open menu or dialog gets the knob: turn = Up/Down (menus) or
// Tab/Backtab (dialogs), press = Return (menus) or Space (dialogs), back =
// Escape, which closes it.
inline void forwardKnob(const KnobOwner& owner, KnobAction action, double steps) {
    if (!owner.blocker) {
        return;
    }
    QWidget* target = owner.blocker;
    if (owner.claimed) {
        // r4-system: the Deck Lab page itself moves its focus ring.
        QPointer<QWidget> page(target);
        const auto key = [&page](int code) {
            if (!page) {
                return;
            }
            QKeyEvent press(QEvent::KeyPress, code, Qt::NoModifier);
            QApplication::sendEvent(page, &press);
            if (!page) {
                return;
            }
            QKeyEvent release(QEvent::KeyRelease, code, Qt::NoModifier);
            QApplication::sendEvent(page, &release);
        };
        if (action == KnobAction::Move) {
            if (!std::isfinite(steps) || steps == 0) {
                return;
            }
            const int count = std::clamp(static_cast<int>(std::lround(std::abs(steps))), 1, 50);
            for (int n = 0; n < count; ++n) {
                key(steps < 0 ? Qt::Key_Up : Qt::Key_Down);
            }
        } else {
            key(action == KnobAction::Back ? Qt::Key_Escape : Qt::Key_Return);
        }
        return;
    }
    if (!owner.popup || !qobject_cast<QMenu*>(target)) {
        if (QWidget* focus = target->focusWidget()) {
            target = focus;
        }
    }
    QPointer<QWidget> receiver(target);
    const auto send = [&receiver](int key) {
        if (!receiver) {
            return;
        }
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
        QApplication::sendEvent(receiver, &press);
        if (!receiver) {
            return;
        }
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
        QApplication::sendEvent(receiver, &release);
    };
    switch (action) {
    case KnobAction::Move: {
        if (!std::isfinite(steps) || steps == 0) {
            return;
        }
        const int count = std::clamp(static_cast<int>(std::lround(std::abs(steps))), 1, 50);
        const int key = owner.popup ? (steps < 0 ? Qt::Key_Up : Qt::Key_Down)
                                    : (steps < 0 ? Qt::Key_Backtab : Qt::Key_Tab);
        for (int n = 0; n < count; ++n) {
            send(key);
        }
        return;
    }
    case KnobAction::Press:
    case KnobAction::Enter:
        send(owner.popup ? Qt::Key_Return : Qt::Key_Space);
        return;
    case KnobAction::Back:
        send(Qt::Key_Escape);
        return;
    }
}

// For [DeckLabNative] browse_* writes: true when Browse owns the knob now;
// otherwise the event goes to the open menu or dialog (and is logged).
inline bool knobForBrowse(const char* control, KnobAction action, double steps) {
    const KnobOwner owner = knobOwner();
    if (owner.browse()) {
        return true;
    }
    logKnobRedirected(control, owner);
    forwardKnob(owner, action, steps);
    return false;
}

} // namespace decklab
