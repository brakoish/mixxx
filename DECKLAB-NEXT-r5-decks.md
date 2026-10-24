# Deck Lab next: round 5, Decks (r5-decks)

Branch `decklab/r5-decks`, based on `decklab/round4` (d381cded9e, the build on the owner's Pi). The owner's picks #3
(key sync and key shift) and #5 (edit the beat grid on the waveform, audit move 6). The Decks page, its waveform
heights and its two- and four-deck layouts stay as round 4 left them.

| Commit | Subject |
|---|---|
| 4324cf0a7e | Decks: key sync and key shift, and GRID mode on the page |
| 64793b8871 | The first version of this note |
| aebc086c11 | Decks: review fixes (live safety, one knob owner, controller path, key lock, docking) and this note |
| (review 2) | Decks: second review (one sheet, knob owner changes, held press, transient-free key lock, saved ownership) |

## What changed

### Pick #3: key sync and key shift

- **The KEY readout shows the key and its deliberate shift: "8A +1".** At 15 px after the 24 px key, as `+1`, `−2`
  or `+0.5`; empty when nothing was shifted.
  - The shift is Mixxx's `[ChannelN],pitch` (from the track's key) while key lock holds the key, and `pitch_adjust`
    (the tweak over the tempo) while it does not. Riding the tempo fader with key lock off never reads as a shift,
    and round 4's ✓ stays.
  - The two-deck key box is 10 px wider (all the tempo can give and still outline OVER ±18.82% at 399 px); the
    compact card's too. When the key and shift still do not fit, a two-name Traditional key takes its second name
    ("D♯m/E♭m" reads "E♭m"), then the shift steps down to 14 px; a ✓ that no longer fits is dropped. All 24
    Traditional names fit with any shift (tested).
- **The KEY readout opens a small key sheet.** On touch-down while the deck is paused (checked against the live play
  control); on a playing deck only after a 400 ms hold that outlines the key while it counts (round 4's stray-thumb
  rule: a brush never swaps a live deck's pads). The hold ends on a slide-off, when the readout hides (page or layer
  change) and when the track changes: it must still mean the same deck, track and screen. A second touch closes the
  sheet. **MORE › KEY** (next to KEY LOCK, both layouts) opens it too. **One key sheet at a time:** opening one
  closes the others. It docks in the deck area, never over the lanes:
  - two decks: in place of the side's pads, beside SYNC / MORE (`DeckLabPadBlock::setPanel`):
    `[ MATCH TO DECK 2 · 9A ][KEY LOCK][DONE]` over `[‹ −1][MATCH][+1 ›][RESET]`;
  - four decks: in place of the card's overview and transport rows: `[‹ −1][MATCH][+1 ›]` over
    `[RESET][TO 2 · 9A][DONE]`. The short line names MATCH's deck, reads `MATCHED 2`, or says `NO KEY` / `NO DECK`.
- **Mixxx's controls, Mixxx's results.**
  - MATCH: `sync_key` (the nearest key that mixes with the target's), from the track's own key.
  - −1 / +1: a semitone from what plays now. With key lock already on, Mixxx's `pitch_down` / `pitch_up`. With it
    off, key lock turns on at the heard pitch (Mixxx's `keylockMode` set to LockCurrentKey for that one write, then
    the DJ's mode back), then the pitch goes to heard ± 1. The engine only ever holds the heard pitch or the new one
    (tested with a recorder on `[ChannelN],pitch` at +8%, −8%, a pitch_adjust of +0.5, a sync follower and beyond
    ±5 st). No clamp: Mixxx's pitch takes values beyond ±6. A step lost to the engine's pending rate update is
    written again once, on the next refresh.
  - RESET: when the DJ owns key lock, `reset_key` (the track's key, key lock stays). When the sheet turned it on,
    the deck goes back to how it played: the pitch goes to the tempo's plus the pre-sheet `pitch_adjust` while still
    locked, then key lock comes off with `keyunlockMode` KeepLockedKey for that one write (the DJ's mode back), so
    it never passes through the track's key, under either unlock preference.
  - **Key lock:** MATCH and ±1 turn it on (a key set by hand stays put when the tempo moves). Mixxx saves key lock,
    so the sheet records that it turned it on, in `[DeckLab] KeySheetKeylockN` with the track and the pre-sheet
    adjust: RESET or the next track gives it back, and so does the next start if that track is gone (a skin rebuild
    with the track still loaded keeps it the sheet's). The DJ's own key lock is never turned off, and any off-on
    toggle in between (sheet, MORE or a controller) makes it the DJ's (all tested, the restart too).
  - MATCH's deck is Mixxx's `EngineSync::pickNonSyncSyncTarget` choice: the sync leader, else a playing synced deck,
    else a playing deck, else a loaded one (tested with several decks).
- **The result shows:** after MATCH the key and its shift turn the match green (#2FD27A) inside a green outline (a
  shape cue too), MATCH is lit and the sheet reads `MATCHED · DECK 2 3A`, naming the deck that was matched (not
  today's target). It ends when this deck's shift changes, the matched deck's pitch moves more than 0.1 st, its key
  stops mixing, or a track changes. The outline stays inside the readout's repaint region (tested by partial paint).
- Every key fires on touch-down with the pads' stale-touch guards (the shown deck, its track): a press that lands
  between a load and the refresh shifts nothing (tested). A layer, layout or page change, or a new track, closes the
  sheet.
- **Knob** (claims the browse knob while shown): the first event, turn or press, only shows the cursor, on DONE, so
  a blind press never shifts a key. Then turns walk the layout in reading order (two decks: KEY LOCK, DONE, −1,
  MATCH, +1, RESET), a press activates, back (Shift+press) closes. When the knob goes elsewhere (a MORE popup, a
  dialog, another claim) the cursor hides at once, and the first event after it comes back only shows it again. A
  press held on the knob (`browse_hold`) acts on its release, and a turn made while it is held moves and fires
  nothing (the overlay sends held turns eight times over).
- Arrows are the single guillemets ‹ ›, which Barlow has (the triangles fell back to another font).

### Pick #5: GRID mode, the beat grid edited on the waveform (audit move 6)

The `decklab-beatgrid-editor` dialog, which hid the waveform being aligned, is gone. **GRID mode** turns one deck's
readout, overview and pad area into the grid controls, like a club player's grid adjust mode; the deck's scrolling
waveform and its grid stay in view above:

```
 ▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬ deck colour ▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬
 [1] EDITING GRID  PAUSED                [LOCK][DONE]
 [ ‹ GRID ][ GRID › ][ ‹ BEAT ][ BEAT › ]
 [−0.1][−0.01]   128.00 BPM ▮▯▯▯   [+0.01][+0.1]
 [  ÷2  ][  ×2  ][  TAP  ][ DOWNBEAT ]
 KNOB  NUDGE · press for BPM · Shift+press DONE
```

- **Mixxx's beatgrid controls** (names from `src/engine/controls/bpmcontrol.cpp`):
  - ‹ GRID / GRID ›: `beats_translate_earlier` / `beats_translate_later` (10 ms);
  - ‹ BEAT / BEAT ›: a whole beat with Mixxx's `Beats::tryTranslateBeats(∓1)`, the call `beats_translate_half`
    makes with ½, so beat 1 of the phase meter moves;
  - −0.01 / +0.01: `beats_adjust_slower` / `beats_adjust_faster`; −0.1 / +0.1 are ten of them (each rounded by
    Mixxx to a nearby round BPM);
  - ÷2 / ×2: `beats_set_halve` / `beats_set_double` (÷2 only where Mixxx can halve the grid: asked once per grid);
  - TAP: `bpm_tap`, on touch-down;
  - DOWNBEAT: `beats_translate_curpos` (a beat at the playhead), then whole-beat moves so that beat is beat 1 of
    the count (Mixxx stores no downbeat; Deck Lab counts beats in fours from the grid's first marker). The readout's
    count shows beat 1 right after it (tested against the readout).
  - LOCK: `bpmlock` (the track's BPM lock); a locked grid refuses edits, Mixxx's rule.
- **Variable-tempo grids** (rekordbox imports are marker grids): ‹ BEAT ›, DOWNBEAT and (where it would fail) ÷2 are
  off, and the status line says "Variable-tempo grid: no BEAT moves. BPM edits flatten it."
- **The readout:** the grid BPM at the playhead (two decimals, 24 px) and the beat of four at the playhead.
- **Placement:** two decks, the deck's column of the deck row (readout, overview, pads); four decks, half the deck
  row: the half under the deck's lane column when the lanes are side by side (1 2 over 3 4), never under another
  deck's lane. The deck row keeps 244 / 230 px, so the waveforms keep their size; nothing floats.
- **The lanes zoom in** to Mixxx's closest zoom (`waveform_zoom` 1.0) while GRID mode is on, and the zoom from
  before comes back on leaving unless it was changed meanwhile. Moving GRID straight to another deck keeps the lanes
  zoomed in. The zoom control only: no GL reallocation. (The zoom is linked across decks, as round 3 made it.)
- **Entering:** MORE › GRID, or holding a **paused** deck's BPM readout for 600 ms (a hold that turns into a drag
  opens nothing). A playing deck enters from MORE only (a stray thumb on a live deck never opens it; MORE › GRID on
  a playing deck is tested). **Leaving:** DONE, Shift+press, a page change, a layer or
  layout change, or a new track on the deck. A context change is handled before the surface's relayout, so it costs
  one relayout, not two.
- **A playing deck can be edited** (DJs fix grids live), never silently: the header reads EDITING GRID filled in the
  deck colour, with PLAYING (play green) or PAUSED beside it.
- **Edits wait, and the status line says what to do,** while sync is on ("SYNC is on. DONE, then turn SYNC off."),
  while the same track plays or syncs on another deck ("Deck 2 plays or syncs it too. DONE, then stop it."),
  without a grid, or while the grid is locked. The checks run again at touch-down, ahead of the panel's refresh
  (tested with the engine change and the press in the same turn).
- **One knob owner at a time:** opening a key sheet leaves GRID mode; entering GRID mode closes the key sheets. The
  knob lamps and the KNOB legend show only while the panel really owns the knob, and follow every change of owner
  as it happens (claims, and menus or dialogs opening and closing, through `decklab::noteKnobOwnerChanged`).
- **Knob:** turn nudges the grid (one 10 ms step per detent), press switches to the BPM (±0.01) and back,
  Shift+press is DONE. A held press switches on its release, and a push-turn edits nothing.
- **Jog:** not taken over. Mixxx's jog still moves the playhead, which on a paused deck lands it on a transient for
  DOWNBEAT. Grid nudging from the jog needs a controller-script hook (follow-up).

### The owner's controller (Numark Mixtrack Pro 3)

Verified against the overlay on the appliance branch `decklab/round4-app` (`mixxx/controllers/DeckLab-Numark.js`,
commit f788b73; that this commit is what the Pi runs is for the coordinator to confirm). In native mode it forwards every turn as `[Playlist] SelectTrackKnob` (or `SelectPlaylist`
while the sidebar list is up), and on every page but Browse a press is `[DeckLabNative] browse_press` and
Shift+press is `browse_back`. `LibraryControl::nativeKnob` hands turns to the knob claim as Up/Down;
`browse_press` / `browse_back` reach it as Return / Escape through WDeckLab's watches. `browse_hold` reaches the
claimant as `Key_Select`, which the sheet and GRID ignore. Native tests drive exactly these controls and sequences,
including the held press (`browse_hold` 1, `browse_press`, `SelectTrackKnob` 8, `browse_hold` 0). **No appliance
change is needed.** (An older overlay gated turns on `[Tab] current == 1` and sent `browse_press` for Shift+press;
that is not what the Pi runs.)

### Code

- `src/widget/decklabkeysheet.{h,cpp}`: the key sheet (pure Qt, fake-host tested).
- `src/widget/decklabgridmode.{h,cpp}`: the GRID panel (pure Qt, fake-host tested).
- `src/widget/decklabdecktools.{h,cpp}`: the surface side: placement, the state each shows, Mixxx's controls, the
  `sync_key` target, the MATCH record, key-lock ownership and the context checks.
- `src/widget/decklabdeckwidgets.{h,cpp}`: `keyShift` / `keyMatched` in the readout state, the key box and its
  fallbacks, the matched look, the readout's touch zones (`keyTarget`, `bpmTarget`; signals `keyTouched`,
  `bpmHeld`; the live-deck hold), and a beat-count tolerance at a beat boundary (`deckLabBeatPhase`).
- `src/widget/decklabpads.{h,cpp}`: `setPanel` / `showPanel` (the docked sheet) and `DeckLabPadButton::armed()`.
- `src/library/decklabknob.h`: `onKnobOwnerChanged` / `noteKnobOwnerChanged` (queued listeners; claims and
  releases call it).

**Performance.** Everything is change-driven: `pitch`, `pitch_adjust`, `key` and `bpmlock` changes mark decks dirty
(coalesced; a pitch change also refreshes only a deck matched to it). With nothing open, the per-refresh hook is a
few comparisons; the readout state is read in place, never copied per tick. Open panels repaint only their changed
part (tested by paint region). The hold timers exist only while a zone is held. No timers, no per-frame work, nothing
on hidden pages.

## Renders

In `local/wt/audit/screens/r5-decks/`:

- `before/`: round 4's two-deck page and its MORE sheet with "Edit grid" (from `r4-decks/after`).
- `after/` (real surface from `NativeDeckLabTest`, 800×480, black lanes as in CI):
  - `decks-key-sheet.png`, `decks-key-sheet-matched.png`, `decks-four-key-sheet.png`;
  - `decks-grid-mode.png`, `decks-grid-mode-playing.png`, `decks-four-grid-mode.png`.
- `app/` (the real application smoke, generated demo tracks): `app-decks-key-sheet.png`,
  `app-decks-key-matched.png`, `app-decks-grid.png`, `app-decks-four-key-sheet.png`, `app-decks-four-grid.png`.

## Verified locally

- Qt 6.4 syntax checks in WSL (`g++ -std=c++20 -fsyntax-only -Wall -Wextra`, moc'd headers) for every changed C++
  file: no new warnings (decklabtest.cpp keeps round 4's 10 dangling-else warnings).
- A standalone offscreen build of the new widget tests and round 4's `DeckLabPadsTest`/`DeckLabReadoutTest`.
- The full local CI equivalent (`local-build/decklab-local.sh`, own slot): see the summary path in the final report.
- `build_pi.py --check-inputs`: PASS after the lock refresh. `test_build_pi.py`: OK.

## Tests

New suites (in the lock's `test_suites`), `src/test/decklabdecktoolstest.cpp`:

- `DeckLabKeySheetTest`: touch-down and stale guards; MATCH needs two keys, lit while matched, names the matched
  deck, says why when off; the knob (cursor first, on DONE; reading order; back; released when hidden); 44 px
  targets and the card's short line in both shapes; docking in the pad block.
- `DeckLabGridModeTest`: every key drives its control on touch-down; edits wait (locked, synced, shared, no grid,
  variable tempo) and say why; PLAYING in the deck colour; the knob; 44 px targets at 244 and 230 px; an unchanged
  state repaints nothing and a change only its part (paint regions); knob lamps and legend follow the real owner.
- `DeckLabKeyReadoutTest`: Mixxx semitones as text; key and shift fit every readout width in every notation; the
  matched look leaves nothing behind after a partial repaint; KEY on touch-down while paused, a live deck's brush
  does nothing and a hold opens; a held BPM opens GRID only while paused.

`NativeDeckLabTest`, real controls:

- `KeySheetMatchesShiftsAndResetsWithMixxxKeyControls`: at +8% with key lock off, no shift shows; +1 / −1 turn key
  lock on first and keep the heard pitch (±1); RESET restores the pitch and key lock off; MATCH on touch-down
  (Mixxx's `sync_key`, A minor to B♭ minor); the matched deck drifting ends MATCHED; the knob through
  SelectTrackKnob / SelectPlaylist / browse_press / browse_back; the toggle; layer change; a press between a load
  and the refresh shifts nothing; the next track gets key lock back; a live deck's brush and hold; MORE › KEY;
  four decks (card rows, short line, Mixxx picks deck 1, compatible result).
- `KeySheetFollowsMixxxsSyncKeyTargetWithSeveralDecks`: the first loaded deck, then a playing deck before a paused
  one, then the sync leader first; the result mixes with that deck.
- `KeySheetStepsKeepTheHeardPitchAndGiveKeyLockBack`: the pitch recorder at +8%, −8%, pitch_adjust +0.5, beyond
  ±5 st and a sync follower; RESET under KeepLockedKey; the DJ's own key lock kept through RESET and the next
  track; an off-on toggle hands it to the DJ; a restart (a fresh surface reading the saved record) gives it back.
- `KeySheetAndGridFollowTheKnobOwnerAsTheControllerSendsIt`: one sheet at a time; a MORE popup hides the cursor and
  the returning knob reveals first; the held press and push-turn through the controller's controls, for the sheet
  and GRID; GRID's lamps and legend follow a MORE popup with every deck paused.
- `ReadoutHoldsEndWithTheirContextAndMoreOpensGridOnAPlayingDeck`: a live KEY hold across a layer change or a
  track load opens nothing; MORE › GRID on a playing deck; a paused BPM hold that becomes a drag opens nothing.
- **Replaced** `BeatgridEditorUsesRealGridControlsAndRefusesUnsafeRelease` with
  `GridModeEditsTheRealGridBesideItsWaveformAndOwnsTheKnob`: the paused BPM hold enters; placement; every lane at
  zoom 1.0 from 4 and back, a zoom changed meanwhile kept, a switch to another deck keeps it; the knob claim; all
  keys; DOWNBEAT against the readout's beat count; the knob through the controller's controls; LOCK; the live
  press-time guards (sync and lock changed in the same turn); editing while playing; Shift+press, MORE › GRID,
  DONE; one knob owner with a key sheet; page, layer and track changes; the track-swap stale press; no GRID from a
  live deck's readout; four decks; side-by-side lanes docking under their own column.
- **Replaced** `SharedTrackGridEditingRequiresEveryAliasedDeckToBeQuiet` with
  `GridModeWaitsWhileTheSameTrackPlaysOrSyncsOnAnotherDeck`: the alias starts and the presses land in the same turn.
- **Updated on purpose:** `MoreAndDialogsKeepTouchGeometryAfterShownStylePolish` (GRID opens GRID mode),
  `FullDeckReadoutOpensTrackInformationAndRejectsStaleGestures` (track information from the title row; KEY opens
  the key sheet), `PadModesAndReadoutRowsFollowTheRealDecks` (the card MORE lists KEY). `build_pi.py` requires the
  three new native test names in place of the two replaced ones.

## Needs CI and the Pi

- CI: the ARM64 build (Qt 6.10 / GCC 15) and the suites above.
- The Pi:
  - MATCH and ±1 on real tracks while another deck plays (Rubber Band / SoundTouch CPU with key lock on);
  - GRID mode against a real scrolling waveform at `waveform_zoom` 1.0, and the knob's feel for 10 ms steps;
  - the 600 ms BPM hold and the 400 ms live KEY hold in a booth.

## Merge notes

**New files:** `src/widget/decklabkeysheet.{h,cpp}`, `decklabgridmode.{h,cpp}`, `decklabdecktools.{h,cpp}`,
`src/test/decklabdecktoolstest.cpp`, this note.

**`src/widget/wdecklab.cpp`**, every hook marked `// r5-decks`:
- `#include "widget/decklabdecktools.h"`.
- Constructor: the `DeckLabDeckTools` host block right before `m_deckControls = buildDecks();`.
- `buildTransport()`: `deckKeys` takes `(owner, menu)` and adds KEY after KEY LOCK (two call sites); "Edit grid"
  is labelled GRID with an accessible name.
- `showBeatgrid()`: the dialog body is replaced by one call to `enterGrid` (about 95 lines removed).
- `buildDecks()`: `attachSide` after `setSideColumn`.
- `ensureDeckSurfaces()`: `attachCard` after the card transport.
- `refreshDeck()`: `fillKey` before `fillDeckClock`.
- `refreshDeckClock()` and `applyDeck()`: `showState(deck)`.
- `updateLayers()`: `validate(); signature |= layoutSignature()` before the signature check, and `applyLayout()`
  after `setVisibleIf(m_deckRow, decks)`.
- Not touched: `kStyle`, Browse, History, the setup wizard, the controller list, the LaunchImage.

- `eventFilter()`: `decklab::noteKnobOwnerChanged()` beside `scheduleKnobLegend()` (menus and dialogs).
- The host block also passes `epoch`, `setting` and `setSetting` (`[DeckLab]` config).

**`src/widget/wdecklab.h`:** `class DeckLabDeckTools;`, `m_deckTools`, a comment on `showBeatgrid`.

**`src/library/decklabknob.h`:** the knob-owner listeners (round 4's file; additive).

**`src/widget/decklabsmoke.cpp`:** three includes, `captureDeckTools()` after `captureSampler()`, and a `decksDone`
stage after the Sampler stage (uncounted, like Browse and Sampler).

**Other:** `decklabdeckwidgets.{h,cpp}`, `decklabpads.{h,cpp}`, `CMakeLists.txt` (three sources after
`decklabpads.cpp`, the test after `decklabpadstest.cpp`), `tools/decklab/build_pi.py` and `test_build_pi.py`
(required native test names, fixture count 235), `tools/decklab/pi-build-lock.json` (suites
`DeckLabKeySheetTest.*`, `DeckLabGridModeTest.*`, `DeckLabKeyReadoutTest.*`; hashes for `CMakeLists.txt`,
`wdecklab.{h,cpp}`, `decklabtest.cpp`, `decklabsmoke.cpp`, `build_pi.py`, `test_build_pi.py`).

**Smoke count delta: +0 screens.** The five `app-decks-*.png` captures run after the Sampler captures and are not
counted (`screens` stays 48).

**Possible overlaps:**
- `decklab/r5-polish`, `decklab/r5-history`: `decklabsmoke.cpp` (separate function and stage), `decklabtest.cpp`
  (the grid and key tests sit between `FourthDeckCuePopup…` and `SideUtilities…`), `build_pi.py`'s required names,
  the lock's hashes (recompute at merge).
- Anything that looked for `decklab-beatgrid-editor`, `decklab-beatgrid-title` or `decklab-grid-close` must use
  `decklab-grid-mode` (`DeckLabGridPanel`, keys `decklab-grid-mode-<control>`).

## Follow-ups

- Jog in GRID mode: a controller-script hook (`[DeckLabNative]`) so a mapped jog nudges the grid instead of the
  playhead while GRID mode is on.
- Show downbeats (every fourth beat) on the scrolling waveform, so DOWNBEAT and ‹ BEAT › show on the lane itself and
  not only in the beat count (a waveform renderer change, owned by the waveform workstream).
- UNDO in GRID mode (`beats_undo_adjustment`), mainly for BPM edits that flatten a rekordbox grid.
- An in-panel SYNC lamp/key in GRID mode, if "DONE, then turn SYNC off" proves too slow in use.
- Track info as a docked sheet the same way (PF-D51).
