# Deck Lab: system quick wins (qw-system)

Branch `decklab/qw-system`, based on `origin/decklab/round3`. It implements the ten system quick wins from the
UI/UX audit (`audit/system.md`, section 5 and Appendix A): global typography, the system kit, the rail status
chip and Settings sizes. Decks, Lab FX, Sampler, Browse, `LibraryControl` and the banner/toast window behaviour
are untouched.

## What changed

1. **Barlow everywhere (G-01).** While a Deck Lab surface exists, Barlow Medium 16 px is the application font
   (`QApplication::setFont` right after `deckLabLoadFonts()`, only when the family loaded). The last surface to
   close restores the previous font, so other skins and later tests are unaffected. `kStyle` now names
   `font-family:"Barlow"` on `WDeckLab` and on the generic `WDeckLab QWidget` rule (the "Open Sans" fallback is
   gone), plus combo popups and menus. The kit sheet (`kExtraStyle`) starts with `QWidget {font-family:"Barlow"}`,
   and painted kit widgets (Tile, Row, Banner, LayoutChoice) take their fonts from `decklab::ui::uiFont()`,
   which always asks for Barlow. The clock and the Welcome title use a new `hero` role (36 px Barlow Semi
   Condensed SemiBold) instead of off-scale 40 and 28 px.
2. **About (Y-49).** "[REPO URL]" is replaced by a static card: **Source code**
   `https://github.com/brakoish/mixxx` and **Licence** "GNU GPL, version 2 or later". The URL is one constant,
   `kSourceCodeUrl`, in `decklabsystempages.cpp`.
3. **Status path (R-02, R-03).** The status text no longer says "Settings → Device". Lines are "No sound output.
   Tap Fix to choose one in Deck Setup.", "System service offline. DJing is unaffected.", "Audio dropouts: %1",
   "Recording your mix." / "Recording paused.". Tapping the zone opens a Deck Lab sheet (`Confirm`), not a
   `QMessageBox`; with no sound output its **Fix** button calls `DeckLabSystemUi::openWizard(1)` (Deck Setup at
   the Audio step), falling back to Settings → Setup.
4. **Read-only drive (Y-35).** "Use for recordings" on a read-only drive first asks "Make <label> writable?":
   "Deck Lab will write recordings to <label>. Your rekordbox export isn't changed." (or "Nothing else on it is
   changed." without an export). With a rekordbox export it recommends another drive, naming a writable one when
   there is one ("MIX SETS is ready to use"), and still allows it. Every version ends "Eject the drive before you
   unplug it." Writable drives go straight through, as before.
5. **Rail reflow (R-01).** `m_status` is always shown with a fixed width: the widest real combination (REC II +
   DROPOUTS) measured with the chip font at start-up, so translations fit too. An empty zone is disabled and
   blank. Lamps lighting or going out never move the tabs.
6. **44 px Settings (R-13, S-02, S-05).** The Settings root is `#decklab-settings`. Its buttons are 44 px
   (32 + padding + borders), combo boxes 44 px (42 + borders) and Advanced 44 px. Toggles that are on print as a
   white fill with ink text. The category list (`#decklab-settings-categories`) is a list: 52 px rows,
   left-aligned, a raised fill and a 3 px white left edge on the selected row, and a line divider (R-12). The
   "DECK n" headings are 14 px SemiBold caps labels.
7. **Token sweep (G-03, G-04, G-05, G-06).** Appendix A is applied to `decklabui.cpp`, `decklabsystemui.cpp`,
   `decklabsetupwizard.cpp`, `decklabsystempages.cpp` and `kStyle`. `decklab::ui` now exports the tokens
   (`kGround`, `kPanel`, `kRaise`, `kLine`, `kDim`, `kMuted`, `kText`, `kLive`); `kAccent` (#d74448) is deleted.
   #d74448 and #e5484d are gone from these files. Live #FF3D2E stays only for clip borders, the REC lamp, the
   About playhead and the no-output banner. Caution (heat banner, Bluetooth latency card, audio advice) is a
   raised fill with a white edge, never amber. Errors are text-coloured SemiBold with a white left rule (no pinks).
   The pressed state on rows and tiles adds a text-coloured edge (G-15). The keyboard now has 4 px gaps (Y-18).
   Banners are opaque (A-02). The chosen layout in Deck Setup draws its decks in deck colours (F-23).
8. **Bluetooth (Y-22).** A row tap opens a details sheet (state, latency note for audio devices, address) with
   **Close** and the row's action. Only that button or the row's own button acts. New `Confirm::details()`.
9. **Safe test tones (F-19).** Tones start at **−24 dBFS** (`decklab::kTestToneDbfs`, amplitude 0.063, was 0.25 =
   −12 dBFS) with 50 ms fades. The Audio step always shows "Turn MASTER and PHONES down before you test."
10. **Rail chips (R-04 to R-06).** AUDIO OFF → **NO AUDIO**, OVERLOAD → **DROPOUTS**, SYSTEM → **SYSTEM OFF**,
    all white inverted chips (#F2F4F7 with ink text). REC / REC II are live red #FF3D2E. The control logic is
    unchanged (DROPOUTS for 10 s after a new dropout; SYSTEM OFF only when nothing else is lit).

The kit sheet no longer depends on a `WDeckLab` ancestor (G-12), so `Confirm` sheets that the System pages host
on the main window now get the kit's buttons, card and type. It repeats every state it overrides, because a
widget's own sheet beats its parents' sheets whatever the specificity.

## Tests and checks

- `DeckLabSystemTest`: new `SystemKitUsesBarlowAndAboutNamesTheRealSource`,
  `UseForRecordingsAsksBeforeMakingAReadOnlyDriveWritable`, `BluetoothRowTapOpensDetailsAndOnlyTheButtonActs`
  and `TestTonesStartQuietBehindALevelsWarning`. Drives are generated under the test's temporary folder.
- `NativeDeckLabTest.InternalClockCannotClaimAConfiguredAudioOutput` now expects "No sound output", the NO AUDIO
  chip in white, and an unchanged status-zone and Settings-tab geometry when the lamps change.
- Smoke check: the touch-target rule now covers `decklab-settings` (category buttons, toggles, combo boxes) and
  Deck Lab sheets on Settings steps 10-14 and every system step. A new rule fails if any visible label, button,
  combo box or line edit in Settings, the System pages, Deck Setup or a sheet resolves to a face other than
  Barlow. Three captures were added (the smoke count goes from 43 to 46; `build_pi.py` expects 46).
- `pi-build-lock.json` refreshed for the locked inputs this touches; `build_pi.py --check-inputs` passes.

| Screen | Content |
|---|---|
| app-screen-43 | Rail status sheet with no sound output (Fix) |
| app-screen-44 | "Make DJ USB writable?" on the read-only rekordbox stick |
| app-screen-45 | Bluetooth device details |

### Verification status

**CI could not run.** Run 39876489616 was cancelled by GitHub mid-build ("repository invocation blocked", the
day's ARM64 quota); further dispatches return "Actions has been disabled for this repository". On the
coordinator's instruction no further runs were started. Everything below was checked on this machine.

Verified locally:

- `build_pi.py --check-inputs`: PASS after refreshing the lock. `test_build_pi.py`: 7 tests OK.
- Syntax-only compile of every changed C++ file (`g++ -std=c++20 -fsyntax-only` against Qt 6.4 in WSL, real moc
  and protobuf output): no errors. `decklabtest.cpp` was not compiled because gmock isn't installed there; its
  change is one test body.
- An offscreen harness compiled the real `decklabui.cpp`, `decklabdeckwidgets.cpp` and the real `kStyle`,
  `kRailStyle` and `statusChips()` text from `wdecklab.cpp` into a Settings/System mock-up, before and after
  this branch (local tool, not committed). Before: category buttons 39 px, toggles and combo boxes 40 px, every
  label and button DejaVu Sans, a sheet on the main window unstyled (the audit's G-01, R-13 and G-12). After:
  39/39 checks pass: categories 52 px, toggles and combo boxes 44 px, Barlow on the rail, Settings, the kit,
  line edits and sheets hosted on the main window, a 162 px status zone that keeps the tabs still with no lamp
  and with REC II + DROPOUTS, and the Settings tab at 75 px. It also caught a real bug, fixed here: a sheet's
  dismiss button read "OK" instead of "Cancel" (argument evaluation order around `std::move`).
- Renders (local only, not published): `local/wt/audit/screens/qw-system-local/before/*.png` and `after/*.png`
  (`settings-deck`, `system-home`, `status-sheet`, `writable-sheet`, `bluetooth-details`, `storage-keyboard`,
  plus `report.txt`). Compare with the real-app captures in `local/wt/audit/screens/ci-round2/app-screen-10.png`
  and `app-screen-21.png`.

Still needs CI:

- The full ARM64 build, `DeckLabSystemTest.*` (four new tests) and `NativeDeckLabTest.*` (the changed status test
  and the existing Settings swipe, fit and type-scale tests under the new sheet).
- The smoke run: 46 screens, the new Barlow and `decklab-settings` touch-target rules, and the three sheet captures
  (app-screen-43 to 45). Then a look at the real captures of Settings (app-screen-10 to 14), the System pages
  (21 to 31) and the first run (32 to 39) to confirm Barlow and the tokens in the full app, including Deck Setup,
  which the harness can't build.

## Merge notes

`wdecklab.cpp` regions touched (line numbers on this branch):

- includes: `<QFontDatabase>`, `<optional>`, `decklab/decklabui.h`;
- anonymous namespace, lines 74-131: `s_surfaces` / `s_fontBefore`, the `kStyle` header comment and `kStyle`
  itself (font rules, token sweep, the `#decklab-settings` rules at the end);
- anonymous namespace, lines 420-437: new `statusChips()` before `beatLoopSizeText()`;
- constructor, lines 525-536: application font after `deckLabLoadFonts()`;
- constructor, lines 693-724: the `m_status` block (reserved width, Deck Lab status sheet with Fix);
- destructor, lines 853-858: font restore;
- `buildSettings()`: object names for the page and the category list (1909-1914), the "DECK n" label property
  (1997-1999), the Advanced button sheet (2127-2128);
- `updateStatus()`: the text, chips, tooltip, accessible name and enabled state (3446-3462). Overload timing,
  the performance line and everything else in the function are unchanged apart from "Audio dropouts" wording.

Other notes:

- `decklabsmoke.cpp` and `build_pi.py` change the screen count to 46. If another branch adds smoke screens,
  add the counts and keep the three sheet captures last (`kSheetFirst`); they grab the main window cropped to the
  surface because the System pages host sheets on the window.
- `decklabtest.cpp`: only `InternalClockCannotClaimAConfiguredAudioOutput` changed.
- Overlay work: the status sheet is a `Confirm` hosted on the surface. If native waveform windows cover in-surface
  overlays on the Pi (F-01), `Confirm` needs the same raise/native treatment as banners and toasts.
- `DeckLabXY` (Lab FX) still paints its touch ring in #d74448; that is the Lab FX owner's to change (the audit
  suggests text #F2F4F7).
- The About URL names the fork's GitHub owner, as requested. PLAN.md asks for no hardcoded identity, so a fork
  that publishes its own builds changes `kSourceCodeUrl`.
- The chip asked for as "SYSTEM OFF stays" said "SYSTEM" before; it now reads SYSTEM OFF, consistent with the
  other two.
- Not done (outside the quick wins): the full status zone and sheet (Signature 2), switch and segmented
  controls, the encoder focus model, the Archivo wordmark, the copy deck beyond the strings above.
