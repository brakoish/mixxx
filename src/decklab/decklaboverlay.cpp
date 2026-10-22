// SPDX-License-Identifier: GPL-2.0-or-later
#include "decklab/decklaboverlay.h"

#include <QChildEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QList>
#include <QPointer>
#include <QTimer>
#include <QVariant>
#include <QWidget>
#include <QWindow>

#include "widget/wdecklab.h"

namespace decklab::overlay {
namespace {

constexpr char kKindProperty[] = "decklabOverlayKind";
constexpr char kRaisesProperty[] = "decklabOverlayRaises";

// The offscreen platform (CI) has no window stacking and warns on raise().
bool platformStacksWindows() {
    static const bool stacks = QGuiApplication::platformName() != QLatin1String("offscreen") &&
            !QGuiApplication::platformName().startsWith(QLatin1String("minimal"));
    return stacks;
}

QList<WDeckLab*> surfacesUnder(QWidget* overlay) {
    for (QWidget* widget = overlay->parentWidget(); widget; widget = widget->parentWidget()) {
        if (auto* surface = qobject_cast<WDeckLab*>(widget)) {
            return {surface};
        }
    }
    // Hosted by the window itself (confirmations): it covers every surface.
    QWidget* host = overlay->parentWidget();
    return host ? host->window()->findChildren<WDeckLab*>() : QList<WDeckLab*>();
}

// Owner of a full-surface overlay's suppression. It never calls a surface
// from its destructor: a surface releases destroyed owners itself, and may
// already be part-destroyed when its child overlays are deleted.
class WaveformSuppressor final : public QObject {
  public:
    explicit WaveformSuppressor(QWidget* overlay)
            : QObject(overlay),
              m_overlay(overlay) {
        overlay->installEventFilter(this);
        if (overlay->isVisible()) {
            setActive(true);
        }
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == m_overlay) {
            if (event->type() == QEvent::Show) {
                setActive(true);
            } else if (event->type() == QEvent::Hide) {
                setActive(false);
            }
        }
        return QObject::eventFilter(watched, event);
    }

  private:
    void setActive(bool active) {
        if (active) {
            for (WDeckLab* surface : surfacesUnder(m_overlay)) {
                if (!m_surfaces.contains(surface)) {
                    m_surfaces.append(surface);
                    surface->setWaveformsSuppressed(m_overlay, true);
                }
            }
            return;
        }
        for (const auto& surface : std::as_const(m_surfaces)) {
            if (surface) {
                surface->setWaveformsSuppressed(m_overlay, false);
            }
        }
        m_surfaces.clear();
    }

    QWidget* const m_overlay;
    QList<QPointer<WDeckLab>> m_surfaces;
};

// Keeps a native overlay a sibling of the waveform windows and above them.
class NativeOverlay final : public QObject {
  public:
    explicit NativeOverlay(QWidget* overlay)
            : QObject(overlay),
              m_overlay(overlay) {
        overlay->installEventFilter(this);
        watchAncestors();
        watchNativeParent();
        applyWindowFlags();
    }

    void raise() {
        m_overlay->raise();
        QWindow* window = m_overlay->windowHandle();
        if (window && platformStacksWindows()) {
            window->raise();
        }
        m_overlay->setProperty(kRaisesProperty, m_overlay->property(kRaisesProperty).toInt() + 1);
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        const QEvent::Type type = event->type();
        if (watched == m_overlay) {
            switch (type) {
            case QEvent::Show:
                // Before the window maps; X keeps the stacking across maps.
                watchNativeParent();
                applyWindowFlags();
                raise();
                break;
            case QEvent::Move:
            case QEvent::Resize:
                if (m_overlay->isVisible()) {
                    raise();
                }
                break;
            case QEvent::WinIdChange:
                watchNativeParent();
                applyWindowFlags();
                break;
            case QEvent::ParentChange:
                watchAncestors();
                watchNativeParent();
                applyWindowFlags();
                break;
            default:
                break;
            }
        } else if (watched == m_nativeParent) {
            // A waveform window created after this overlay (a deck lane
            // shown for the first time) is stacked on top of it.
            if (type == QEvent::ChildAdded) {
                QObject* child = static_cast<QChildEvent*>(event)->child();
                if (child && child->isWindowType() && child != m_overlay->windowHandle()) {
                    scheduleRaise();
                }
            }
        } else if (type == QEvent::ParentChange) {
            // Not while Qt dispatches this ancestor's event filters.
            scheduleRewatch();
        } else if (type == QEvent::Move) {
            syncNativeGeometry();
        }
        return QObject::eventFilter(watched, event);
    }

  private:
    // Qt moves a native child's window when the child itself moves, not when
    // a non-native ancestor does (a menu bar appearing, letterboxing, a
    // skin layout pass). Watch the ancestors and resynchronise.
    void watchAncestors() {
        for (const auto& ancestor : std::as_const(m_ancestors)) {
            if (ancestor) {
                ancestor->removeEventFilter(this);
            }
        }
        m_ancestors.clear();
        for (QWidget* ancestor = m_overlay->parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
            ancestor->installEventFilter(this);
            m_ancestors.append(ancestor);
        }
    }

    void watchNativeParent() {
        QWindow* window = m_overlay->windowHandle();
        QWindow* parent = window ? window->parent() : nullptr;
        if (parent == m_nativeParent) {
            return;
        }
        if (m_nativeParent) {
            m_nativeParent->removeEventFilter(this);
        }
        m_nativeParent = parent;
        if (parent) {
            parent->installEventFilter(this);
        }
    }

    // Informational overlays let touches through to the waveform beneath
    // (an empty input shape); none takes the keyboard focus from the deck
    // surface when tapped.
    void applyWindowFlags() {
        QWindow* window = m_overlay->windowHandle();
        if (!window) {
            return;
        }
        Qt::WindowFlags flags = window->flags() | Qt::WindowDoesNotAcceptFocus;
        flags.setFlag(Qt::WindowTransparentForInput, m_overlay->testAttribute(Qt::WA_TransparentForMouseEvents));
        if (flags != window->flags()) {
            window->setFlags(flags);
        }
    }

    void syncNativeGeometry() {
        QWindow* window = m_overlay->windowHandle();
        QWidget* nativeParent = m_overlay->nativeParentWidget();
        // A hidden overlay is placed from its widget geometry when shown.
        if (!window || !nativeParent || !m_overlay->isVisible()) {
            return;
        }
        if (window->position() == m_overlay->mapTo(nativeParent, QPoint())) {
            return;
        }
        // Moving the widget is the public way to make Qt place its window
        // from the current widget geometry.
        const QPoint position = m_overlay->pos();
        m_overlay->move(position + QPoint(1, 0));
        m_overlay->move(position);
    }

    void scheduleRewatch() {
        if (m_rewatchQueued) {
            return;
        }
        m_rewatchQueued = true;
        QTimer::singleShot(0, this, [this] {
            m_rewatchQueued = false;
            watchAncestors();
            watchNativeParent();
            syncNativeGeometry();
        });
    }

    void scheduleRaise() {
        if (m_raiseQueued) {
            return;
        }
        m_raiseQueued = true;
        // The new window is created and shown right after it is parented.
        QTimer::singleShot(0, this, [this] {
            m_raiseQueued = false;
            if (m_overlay->isVisible()) {
                raise();
            }
        });
    }

    QWidget* const m_overlay;
    QList<QPointer<QWidget>> m_ancestors;
    QPointer<QWindow> m_nativeParent;
    bool m_raiseQueued = false;
    bool m_rewatchQueued = false;
};

} // namespace

void suppressWaveformsWhileVisible(QWidget* overlay) {
    if (!overlay || kind(overlay) != Kind::None) {
        return;
    }
    overlay->setProperty(kKindProperty, static_cast<int>(Kind::SuppressesWaveforms));
    new WaveformSuppressor(overlay);
}

void keepAboveWaveforms(QWidget* overlay) {
    if (!overlay || kind(overlay) != Kind::None) {
        return;
    }
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    overlay->setProperty(kKindProperty, static_cast<int>(Kind::AboveWaveforms));
    // The native parent must be the top-level window, which also parents
    // every waveform window: only siblings can be stacked above them.
    overlay->setAttribute(Qt::WA_DontCreateNativeAncestors);
    overlay->setAttribute(Qt::WA_NativeWindow);
    new NativeOverlay(overlay);
}

void raiseAboveWaveforms(QWidget* overlay) {
    if (!overlay) {
        return;
    }
    for (QObject* child : overlay->children()) {
        if (auto* native = dynamic_cast<NativeOverlay*>(child)) {
            native->raise();
            return;
        }
    }
    overlay->raise();
}

Kind kind(const QWidget* overlay) {
    return overlay ? static_cast<Kind>(overlay->property(kKindProperty).toInt()) : Kind::None;
}

int raiseCount(const QWidget* overlay) {
    return overlay ? overlay->property(kRaisesProperty).toInt() : 0;
}

} // namespace decklab::overlay
