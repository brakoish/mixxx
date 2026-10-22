// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

class QWidget;

/// Overlays drawn over the Deck Lab surface while GL waveforms are on screen.
///
/// Each scrolling waveform is a QOpenGLWindow embedded with
/// QWidget::createWindowContainer(), so on X11 (and XWayland) it is a native
/// child window of the top-level window. An ordinary (non-native) child
/// widget is painted into the top-level window's own backing store, and every
/// native child window covers it: on a real display an overlay crossing the
/// waveform zone shows a black or stale band where the waveforms are. The
/// offscreen platform used by CI has no native child windows, so captures
/// never show it.
///
/// Every Deck Lab overlay that can cross the waveform zone uses one of these:
///  - suppressWaveformsWhileVisible() for overlays covering the surface (Deck
///    Setup and the first-run flow, confirmations). While the overlay is
///    visible the surface hides its waveform viewers
///    (WDeckLab::setWaveformsSuppressed); they return when it hides or is
///    destroyed.
///  - keepAboveWaveforms() for small overlays that must not hide the waveforms
///    mid-set (banners, toasts). The overlay becomes a native child window
///    whose native parent is the top-level window, so it is an X11 sibling of
///    the waveform windows, and it is raised above them after every show,
///    geometry change and new waveform window. It stays a QWidget child of
///    its host, so layouts, findChild() and grab() work as before.
namespace decklab::overlay {

enum class Kind {
    None,
    SuppressesWaveforms,
    AboveWaveforms,
};

/// Hides the waveforms of the Deck Lab surfaces under `overlay` while it is
/// visible: the nearest WDeckLab ancestor, or every surface in the window for
/// an overlay hosted by the window itself. Without a surface it does nothing.
void suppressWaveformsWhileVisible(QWidget* overlay);

/// Makes `overlay` a native window stacked above the waveform windows.
/// Overlays with Qt::WA_TransparentForMouseEvents also pass touches through
/// at the window-system level (an empty input shape), so the waveform under
/// an informational banner can still be scratched.
///
/// Sets Qt::AA_DontCreateNativeWidgetSiblings for the application: by
/// default Qt would turn every sibling and ancestor of a native widget into a
/// native window, created above the waveform windows, burying them.
void keepAboveWaveforms(QWidget* overlay);

/// Raises an overlay among its siblings and, if native, its window above the
/// waveform windows. QWidget::raise() alone skips the window-system raise
/// when the widget is already its parent's last child.
void raiseAboveWaveforms(QWidget* overlay);

Kind kind(const QWidget* overlay);

/// How often keepAboveWaveforms() raised `overlay` (tests and diagnostics).
int raiseCount(const QWidget* overlay);

} // namespace decklab::overlay
