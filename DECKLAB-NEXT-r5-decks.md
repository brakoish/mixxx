# Deck Lab next: round 5, Decks (r5-decks)

Branch `decklab/r5-decks`, based on `decklab/round4` (d381cded9e, the build on the owner's Pi). The owner's picks #3
(key sync and key shift) and #5 (edit the beat grid on the waveform, audit move 6). The Decks page, its waveform
heights and its two- and four-deck layouts stay as round 4 left them.

## What changed

### Pick #3: key sync and key shift

- **The KEY readout shows the key and its shift: "8A +1".** The shift is Mixxx's own `[ChannelN],pitch` (semitones
  from the track's key), shown as `+1`, `−2` or `+0.5` at 15 px after the 24 px key. Empty when unshifted.
  - The two-deck key box is 10 px wider (all the tempo can give and still outline OVER ±18.82% at 399 px); the
    compact card's is 10 px wider too. A ✓ that no longer fits beside a shifted key is dropped, never squeezed.
- **Touching the KEY readout opens a small key sheet, on touch-down.** It docks in the deck area, never over the
  waveforms (native GL windows):
  - two decks: in place of the side's pads, beside SYNC / MORE (`DeckLabPadBlock::setPanel`);
    `[ MATCH TO DECK 2 · 9A ][KEY LOCK][DONE]` over `[◀ −1][MATCH][+1 ▶][RESET]`;
  - four decks: in place of the card's overview and transport rows; `[◀ −1][MATCH][+1 ▶]` over `[RESET][DONE]`.
  - A second touch on KEY closes it, like a toggle. **MORE › KEY** (next to KEY LOCK, both layouts) opens it too.
- **Mixxx's controls, Mixxx's results.**
  - MATCH: `sync_key` (the nearest key that mixes with the target's: same, relative, a fourth or a fifth).
  - ◀ −1 / +1 ▶: `pitch_down` / `pitch_up` (the pitch knob's semitone steps).
  - RESET: `reset_key`.
  - MATCH and ±1 turn key lock on first, so a key set by hand stays put when the tempo moves (KEY LOCK is a lamp in
    the two-deck sheet and stays in MORE).
  - The sheet names the deck MATCH will follow as Mixxx's `EngineSync::pickNonSyncSyncTarget` picks it: the sync
    leader, else a playing synced deck, else a playing deck, else a loaded one.
- **The result shows:** after MATCH the key and its shift turn the match green (#2FD27A) inside a green outline (a
  shape cue too, not only colour), MATCH is lit and the sheet reads `MATCHED · DECK 2 3A`. It holds while the key
  still matches that deck with the shift MATCH set; any other shift, a new track or a key change there ends it.
- Every key fires on touch-down with the pads' stale-touch guards (the shown deck, its track). A layer, layout or page
  change, or a new track, closes the sheet.
- **Knob:** while the sheet is shown it claims the browse knob (`decklab::claimKnob`): turn moves between the keys
  (a cursor appears once the knob is used), press activates (MATCH by default), Shift+press closes.

### Pick #5: GRID mode, the beat grid edited on the waveform (audit move 6)

The `decklab-beatgrid-editor` dialog, which hid the waveform being aligned, is gone. **GRID mode** turns one deck's
readout, overview and pad area into the grid controls, like a club player's grid adjust mode; the deck's scrolling
waveform and its grid stay in view above:

```
 ▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬ deck colour ▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬
 [1] EDITING GRID  PAUSED                [LOCK][DONE]
 [ ◀ GRID ][ GRID ▶ ][ ◀ BEAT ][ BEAT ▶ ]
 [−0.1][−0.01]   128.00 BPM ▮▯▯▯   [+0.01][+0.1]
 [  ÷2  ][  ×2  ][  TAP  ][ DOWNBEAT ]
 KNOB  NUDGE · press for BPM · Shift+press DONE
```

- **Mixxx's beatgrid controls** (names from `src/engine/controls/bpmcontrol.cpp`):
  - ◀ GRID / GRID ▶: `beats_translate_earlier` / `beats_translate_later` (10 ms);
  - ◀ BEAT / BEAT ▶: a whole beat with Mixxx's `Beats::tryTranslateBeats(∓1)`, the call `beats_translate_half`
    makes with ½ (constant-tempo grids only, Mixxx's rule), so beat 1 of the phase meter moves;
  - −0.01 / +0.01: `beats_adjust_slower` / `beats_adjust_faster`; −0.1 / +0.1 are ten of them (each rounded by
    Mixxx to a nearby round BPM);
  - ÷2 / ×2: `beats_set_halve` / `beats_set_double`;
  - TAP: `bpm_tap`, on touch-down;
  - DOWNBEAT: `beats_translate_curpos` (a beat at the playhead), then whole-beat moves so that beat is beat 1 of
    the count (Mixxx stores no downbeat; Deck Lab counts beats in fours from the grid's first marker);
  - LOCK: `bpmlock` (the track's BPM lock); a locked grid refuses edits, Mixxx's rule.
- **The readout:** the grid BPM at the playhead (two decimals, 24 px) and the beat of four at the playhead.
- **Placement:** two decks, the deck's column of the deck row (readout, overview, pads); four decks, the half of the
  deck row that holds the deck. The deck row keeps 244 / 230 px, so the waveforms keep their size; nothing floats.
- **The lanes zoom in** to Mixxx's closest zoom (`waveform_zoom` 1.0) while GRID mode is on, and the zoom comes back
  on leaving unless it was changed meanwhile. That is the zoom control only: no GL reallocation. (The zoom is linked
  across decks, as round 3 made it, so every lane zooms together.)
- **Entering:** MORE › GRID (was "Edit grid") or holding the BPM readout for 600 ms (the press is cancelled, so its
  release never opens track information). **Leaving:** DONE, Shift+press, a page change, a layer or layout change,
  or a new track on the deck.
- **A playing deck can be edited** (DJs fix grids live), never silently: the header always reads EDITING GRID filled
  in the deck colour, with PLAYING (play green) or PAUSED beside it.
- **Edits wait, and the status line says why,** while sync is on or the deck is the tempo master (other decks would
  move), while the same track plays or syncs on another deck, without a grid, or while the grid is locked. The
  checks run again at touch-down (round 3's press-time guards).
- **Knob:** GRID mode claims the browse knob: turn nudges the grid (`beats_translate_earlier/later`, one step per
  detent), press switches to the BPM (±0.01) and back, Shift+press is DONE. The keys the knob stands for carry its
  lamp (an inner outline).
- **Jog:** not taken over. Mixxx's jog still moves the playhead, which on a paused deck lands it on a transient for
  DOWNBEAT. Grid nudging from the jog needs a controller-script hook (follow-up).

### Code

- `src/widget/decklabkeysheet.{h,cpp}`: the key sheet (pure Qt, fake-host tested).
- `src/widget/decklabgridmode.{h,cpp}`: the GRID panel (pure Qt, fake-host tested).
- `src/widget/decklabdecktools.{h,cpp}`: the surface side: placement, the state each shows, Mixxx's controls, the
  `sync_key` target, the MATCH record and the context checks.
- `src/widget/decklabdeckwidgets.{h,cpp}`: `keyShift` / `keyMatched` in the readout state, the key box, the matched
  look, and the readout's two touch zones (`keyTarget`, `bpmTarget`, signals `keyTouched`, `bpmHeld`).
- `src/widget/decklabpads.{h,cpp}`: `setPanel` / `showPanel` (the docked sheet) and `DeckLabPadButton::armed()`.

**Performance.** Everything is change-driven: `pitch`, `key` and `bpmlock` changes mark decks dirty (coalesced, as
round 4 does). With nothing open, the per-refresh hook is three comparisons; the readout state is read in place,
never copied per tick. Open panels repaint only what changed (tested). The BPM hold timer exists only while the BPM
is held. No timers, no per-frame work, nothing on hidden pages.

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
  file: no new warnings.
- A standalone offscreen build of the new widget tests (15/15) and round 4's `DeckLabPadsTest`/`DeckLabReadoutTest`.
- The full local CI equivalent (`local-build/decklab-local.sh`, own slot): see the summary path in the final report.
- `build_pi.py --check-inputs`: PASS after the lock refresh. `test_build_pi.py`: OK.

## Tests

New suites (added to the lock's `test_suites`), `src/test/decklabdecktoolstest.cpp`:

- `DeckLabKeySheetTest`: touch-down and stale guards; MATCH needs two keys, lit while matched, says why when off;
  the knob (move, press, back, released when hidden); 44 px targets in both shapes; docking in the pad block (a held
  roll ends, SYNC/MORE stay, the pads return in their mode).
- `DeckLabGridModeTest`: every key drives its control on touch-down; edits wait (locked, synced, shared, no grid)
  and say why; PLAYING in the deck colour; the knob (nudge, BPM, back); 44 px targets at 244 and 230 px; an unchanged
  state repaints nothing.
- `DeckLabKeyReadoutTest`: Mixxx semitones as text; key and shift fit every readout width; the matched look and its
  repaint region; KEY on touch-down, a held BPM opens GRID, a short BPM tap is still the readout's tap.

`NativeDeckLabTest`, real controls:

- New `KeySheetMatchesShiftsAndResetsWithMixxxKeyControls`: KEY touch-down docks the sheet (SYNC/MORE stay, nothing
  over the lanes, deck row 244); MATCH turns key lock on and Mixxx's `sync_key` takes A minor to B♭ minor (+1),
  the readout reads the key, "+1" and matched; ±1 and RESET; KEY LOCK; the knob's press (MATCH) and back; the
  toggle; layer change and a new track close it; MORE › KEY beside KEY LOCK; four decks (card rows, Mixxx picks deck
  1, compatible result, DONE restores the overview and transport).
- **Replaced** `BeatgridEditorUsesRealGridControlsAndRefusesUnsafeRelease` with
  `GridModeEditsTheRealGridBesideItsWaveformAndOwnsTheKnob`: the BPM hold enters (no track info, no dialog), the
  placement (deck 2 and the lanes stay, deck row 244, no focus frame handed on), the zoom in and back, the knob
  claim; GRID ◀▶ on touch-down, DOWNBEAT (beat 1 at the playhead), whole-beat moves, ÷2/×2, ±0.01/±0.1, TAP
  (`bpm_tap` on touch-down), the knob (nudge, BPM), LOCK (and the press-time guard), sync waiting, editing while
  playing; Shift+press, MORE › GRID and DONE; a page change, a layer change and a new track end it; four decks.
- **Replaced** `SharedTrackGridEditingRequiresEveryAliasedDeckToBeQuiet` with
  `GridModeWaitsWhileTheSameTrackPlaysOrSyncsOnAnotherDeck` (the same three aliases and forced presses).
- **Updated on purpose:**
  - `MoreAndDialogsKeepTouchGeometryAfterShownStylePolish`: GRID opens GRID mode; its keys keep 44 px after polish.
  - `FullDeckReadoutOpensTrackInformationAndRejectsStaleGestures`: track information opens from the title row; a
    touch on KEY opens the key sheet instead (the readout's centre is the KEY zone at 399 px).
  - `PadModesAndReadoutRowsFollowTheRealDecks`: the card MORE also lists KEY.

## Needs CI and the Pi

- CI: the ARM64 build (Qt 6.10 / GCC 15) and the suites above. Captures: `decks-key-sheet*.png`,
  `decks-four-key-sheet.png`, `decks-grid-mode*.png`, `decks-four-grid-mode.png` and the smoke's `app-decks-*.png`.
- The Pi:
  - MATCH and ±1 on real tracks while another deck plays (Rubber Band / SoundTouch CPU with key lock on);
  - GRID mode against a real scrolling waveform at `waveform_zoom` 1.0, and the knob's feel for 10 ms steps;
  - the 600 ms BPM hold in a booth.

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
- `updateLayers()`: `signature |= layoutSignature()` and `applyLayout()` after `setVisibleIf(m_deckRow, decks)`.
- Not touched: `kStyle`, Browse, History, the setup wizard, the controller list, the LaunchImage.

**`src/widget/wdecklab.h`:** `class DeckLabDeckTools;`, `m_deckTools`, a comment on `showBeatgrid`.

**`src/widget/decklabsmoke.cpp`:** three includes, `captureDeckTools()` after `captureSampler()`, and a `decksDone`
stage after the Sampler stage (uncounted, like Browse and Sampler).

**Other:** `decklabdeckwidgets.{h,cpp}`, `decklabpads.{h,cpp}`, `CMakeLists.txt` (three sources after
`decklabpads.cpp`, the test after `decklabpadstest.cpp`), `tools/decklab/pi-build-lock.json` (suites
`DeckLabKeySheetTest.*`, `DeckLabGridModeTest.*`, `DeckLabKeyReadoutTest.*`; hashes for `CMakeLists.txt`,
`wdecklab.{h,cpp}`, `decklabtest.cpp`, `decklabsmoke.cpp`).

**Smoke count delta: +0 screens.** The five `app-decks-*.png` captures run after the Sampler captures and are not
counted (`screens` stays 48).

**Possible overlaps:**
- `decklab/r5-polish`, `decklab/r5-history`: `decklabsmoke.cpp` (separate function and stage), `decklabtest.cpp`
  (the replaced grid tests sit between `FourthDeckCuePopup…` and `SideUtilities…`), the lock's hashes (recompute
  at merge).
- Anything that looked for `decklab-beatgrid-editor`, `decklab-beatgrid-title` or `decklab-grid-close` must use
  `decklab-grid-mode` (`DeckLabGridPanel`, keys `decklab-grid-mode-<control>`).

## Follow-ups

- Jog in GRID mode: a controller-script hook (`[DeckLabNative]`) so a mapped jog nudges the grid instead of the
  playhead while GRID mode is on.
- Show downbeats (every fourth beat) on the scrolling waveform, so DOWNBEAT and ◀ BEAT ▶ show on the lane itself
  and not only in the beat count (a waveform renderer change, owned by the waveform workstream).
- Undo in GRID mode (`beats_undo_adjustment`), one level, if DJs ask.
- Track info as a docked sheet the same way (PF-D51).
