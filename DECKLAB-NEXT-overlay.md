# Deck Lab: overlays over the GL waveforms (workstream `overlay`)

Branch `decklab/overlay`, based on `decklab/round3`.

## The bug
On the Pi (Xorg kiosk, no window manager) the first-run / Deck Setup wizard over Decks showed a
black band exactly over the waveform zone (y 45 to 235 at 800x480), hiding its middle text and
controls.

Each scrolling waveform is a `WGLWidget` that embeds a `QOpenGLWindow` with
`QWidget::createWindowContainer()`. Qt parents that window to the **top-level** window, so on X11
it is a native child window. Ordinary child widgets are painted into the top-level window's
backing store, and every native child window covers them. So any child-widget overlay that
crosses the waveform zone is hidden behind the waveforms. CI's offscreen platform has no native
child windows, which is why the screenshot check never showed the bug.

## Fix
`src/decklab/decklaboverlay.{h,cpp}` adds two helpers. Every Deck Lab overlay that can cross the
waveform zone uses one of them.

### Full-surface overlays: hide the waveforms while visible
- `WDeckLab::setWaveformsSuppressed(QObject* owner, bool)` and `waveformsSuppressed()`.
  - Owner counting: each owner counts once. Releasing an unknown owner, releasing twice or
    passing a null owner changes nothing.
  - A destroyed owner is released automatically through `QObject::destroyed`.
  - When the last owner releases, the waveforms show again.
  - It hides the four waveform viewers registered by `addWaveform()`. Each viewer has
    `retainSizeWhenHidden`, so the Decks page does not reflow.
  - Hiding unmaps the GL windows. The render gate skips hidden widgets, and
    `WGLWidget::showEvent` plus the gate force a full render of the first frame after showing.
  - Before hiding, a viewer gets a synthetic mouse release. A touch scratch held at that moment
    therefore cannot leave the deck held, because a hidden viewer never sees the real release.
  - `~WDeckLab` disconnects its owners, because child overlays are destroyed after its members
    are gone.
- `decklab::overlay::suppressWaveformsWhileVisible(overlay)` connects an overlay's Show and Hide
  events to that API. It finds the surfaces from the nearest `WDeckLab` ancestor. For an overlay
  hosted by the window itself (a confirmation), it uses every `WDeckLab` in the window. With no
  surface (unit tests on a plain host) it does nothing.

### Small overlays: native windows stacked above the waveforms
`decklab::overlay::keepAboveWaveforms(overlay)` makes the overlay a native child window. Its native
parent is the top-level window, so it is an **X11 sibling** of the waveform windows, and it is
raised above them.
- `Qt::WA_DontCreateNativeAncestors` together with app-wide
  `Qt::AA_DontCreateNativeWidgetSiblings` are both required. With Qt defaults, one native child
  makes the surface, its siblings (rail, `decklab-decks`, pages) and every ancestor native. Those
  new windows are created above the waveform windows and bury them, and the overlay's native
  parent becomes the surface instead of the top-level window. I confirmed this with an offscreen
  Qt 6.4 probe (see Verification).
- The overlay is raised:
  - on every show;
  - on every move or resize;
  - from `raiseAboveWaveforms()`, called from `layoutBanners()` and `DeckLabToast::showMessage`;
  - after any new child window of the top-level window, such as a waveform window created when a
    deck lane first appears.

  The raise calls `QWindow::raise()` directly, because `QWidget::raise()` skips the window-system
  raise when the widget is already its parent's last child. On the offscreen platform only the
  window raise is skipped, because the platform has no stacking and warns.
- Qt moves a native child's window only when the widget itself moves, not when a non-native
  ancestor moves (for example a menu bar appearing, letterboxing or a skin layout pass). The
  helper watches the ancestors and resynchronises the window position.
- Informational overlays (`WA_TransparentForMouseEvents`) get `Qt::WindowTransparentForInput`,
  which is an empty X input shape. A touch on the thermal banner still reaches the waveform
  beneath, so scratching works there.
- Actionable banners take taps as before.
- None of these windows takes keyboard focus (`Qt::WindowDoesNotAcceptFocus`).
- Banners are painted opaque, since there is no compositor. The browse toast already was.
- Overlays stay QWidget children of their host, so `findChild()`, layouts, the smoke checks and
  `grab()` captures keep working. `grab()` renders native children, which I checked by reading a
  pixel.

### labwc / XWayland (fallback session)
Native child windows live inside Mixxx's XWayland top-level. Xwayland itself handles their
stacking, input shape and composition into that top-level's surface, so the behaviour matches the
Xorg kiosk. Suppression uses only show and hide.

## Audit of every overlay

| Overlay | Where | Treatment |
|---|---|---|
| `DeckLabSetupWizard`: Deck Setup and the first-run flow | `decklabsetupwizard.cpp` | suppress |
| `decklab::ui::Confirm`: confirmations on `window()` | `decklabui.cpp` | suppress |
| Banners: no audio, controller offer, thermal/undervoltage, and the toast (including "Looking for new tracks…") | `decklabsystemui.cpp` | native and raised |
| `DeckLabToast`: "Deck N is playing" / load unavailable | `decklabbrowse.cpp` | native and raised |

Already above the GL windows, because they are top-level windows:
- QMenu popups;
- the More, track information and beatgrid `QDialog`s;
- `QMessageBox`;
- combo box popups;
- tooltips.

Not over the waveform zone:
- `DeckLabPhraseBands`, inside the non-GL overview;
- the keyboards, which are in layouts.

The stems, FX and Sampler pages have no child-widget popups.

## Tests
`DeckLabOverlayTest`, a new suite in `src/test/decklabtest.cpp` added to `test_suites`, with three
tests:

1. **WaveformSuppressionCountsOwnersAndAlwaysRestores** covers:
   - owner counting and duplicate owners;
   - releasing on destruction;
   - null and unknown owners;
   - no reflow of the Decks page;
   - the scratch release;
   - lanes outside the layout never left hidden.
2. **DeckSetupAndConfirmationsHideWaveformsOnlyWhileShown** covers the real first-run wizard
   ("Later"), Deck Setup, a window-hosted confirmation answered and one deleted while shown, with
   a waveform window standing in for the GL window. It ends with the surface destroyed while Deck
   Setup is open.
3. **EveryOverlayOverTheWaveformsHidesThemOrIsNativeAndRaised** is the structural test. Every
   visible child of the surface that its layout does not place, and that crosses the waveform
   zone, must be one of these:
   - a suppressor, with the waveforms hidden;
   - native, with the waveform windows' native parent (X11 siblings), raised, input-transparent
     exactly when informational, and not taking focus.

   It also checks that no other widget became native, that a waveform window created later
   re-raises the banners, and that the offer banner takes a tap and opens Deck Setup. It shows
   the real banners, the toast and the Browse refusal, then Deck Setup, then a confirmation. When
   `DECKLAB_SCREENSHOT_DIR` is set it saves `overlay-banners-over-decks.png` and
   `overlay-deck-setup-over-decks.png`.

A real `WGLWidget` cannot run offscreen. In its place the tests use a `QWindow` embedded with
`createWindowContainer()`, which Qt parents and shows exactly like the GL window.

## Verification
- An offscreen Qt 6.4 harness compiled the real `decklaboverlay.cpp` against a stub surface and
  passed 35 checks. It covered the X11 sibling parent, no cascade, raise after a new window,
  resync after an ancestor move, input flags, `grab()`, suppression and restore, and destruction
  orders.
- The modified sources passed a Qt 6.4 syntax check.
- CI run 39873539758 passed (see CI below).

## On-Pi verification (coordinator)
`DECKLAB_OVERLAY_PROBE=<s>` is a developer hook that does nothing unless set. `<s>` seconds after
Deck Lab starts, it shows a toast, the warning banner (thermal slot) and a tappable "New
controller: Overlay check · Set up" banner for 30 s. 30 s after that it opens the first-run flow.
That flow is only marked done if it is completed, so "Later" leaves it unset.

1. Start Deck Lab with the hook. Add `--env=DECKLAB_OVERLAY_PROBE=60` to the `flatpak run` line of
   the kiosk unit (a user drop-in), or run that line by hand once.
2. Within 60 s, set the two-deck layout on Decks, load two tracks and play both.
3. At about 60 s, banners appear at the top of the waveform zone. Take a screenshot:
   `DISPLAY=:0 XAUTHORITY=$HOME/.Xauthority scrot /tmp/overlay-1-banners.png`.
   Expected: all three pills are fully drawn above the moving waveforms. Optionally, touch the
   waveform under the warning banner: it scratches.
4. Tap the "New controller" banner. Deck Setup opens over Decks and audio keeps playing. Take
   `scrot /tmp/overlay-2-setup.png` and check that there is no black band and the middle text and
   controls are visible. Tap Close. The waveforms return at the current position with no stale
   frame. Take `scrot /tmp/overlay-3-back.png`.
5. At about 90 s, the first-run Welcome screen opens. Take `scrot /tmp/overlay-4-first-run.png`,
   tap Later, then take `scrot /tmp/overlay-5-back.png`.
6. Go to Browse with deck 1 playing and tap LOAD 1. The "Deck 1 is playing" toast covers the
   waveform strip. Take `scrot /tmp/overlay-6-browse-toast.png`.
7. Optional read-only check, if x11-utils is installed: `DISPLAY=:0 xwininfo -root -tree`. The
   44 px banner windows must be direct children of the Mixxx top-level, next to the waveform
   windows. xwininfo lists siblings top-most first.
8. Remove the hook. To check a real thermal warning without heat, run Mixxx against a fake
   decklabd that sends `thermal.changed {"level":"hot",...}`; the probe covers the same banner.
9. Repeat steps 3 to 6 once in the labwc fallback session. Under XWayland, scrot cannot capture
   the screen, so use `grim` if it is installed; otherwise check by eye.

## Known limits
- Offscreen CI cannot see X stacking. It can only check the structure that guarantees it; only
  the Pi steps above confirm the real stacking.
- If code ever calls `QWidget::raise()` on an ancestor of a waveform viewer, Qt
  (`QWindowContainer::parentWasRaised`) raises the GL windows above the banners until the next
  banner raise. Deck Lab never does this.
- Banner corners (radius 4) show the surface ground (#0D0F12) instead of the waveform beneath.
  That is the waveform background colour, so the corners don't stand out.

## Files
- New: `src/decklab/decklaboverlay.{h,cpp}`.
- Changed: `src/widget/wdecklab.{h,cpp}`, `src/decklab/decklabsetupwizard.cpp`,
  `src/decklab/decklabui.cpp`, `src/decklab/decklabsystemui.{h,cpp}`,
  `src/widget/decklabbrowse.cpp`, `src/test/decklabtest.cpp`, `CMakeLists.txt`,
  `tools/decklab/pi-build-lock.json` (suite added, hashes refreshed).

## CI
Run 39873539758 (commit fe6f50328e) passed: 360 native tests (357 on `round3`, plus the 3
new `DeckLabOverlayTest` tests), 0 failures, the offscreen smoke run with 43 screens and no
failures, and the input lock check.
