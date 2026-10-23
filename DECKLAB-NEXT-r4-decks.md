# Deck Lab next: round 4, Decks (r4-decks)

Branch `decklab/r4-decks`, based on `decklab/round3` (5d4f4570f7). Source: `audit/performance.md`, section 3,
moves 1 and 2 (move 6 is written up as a design below). The owner likes the Decks page, so this round refines it:
the layout, the waveform heights, the two- and four-deck modes and round 3's behaviour all stay.

| Commit | Subject |
|---|---|
| 54884c1bda | Decks: a one-glance deck readout and pad modes (round 4, moves 1 and 2) |
| (last) | This note |

## What changed

### Move 1: a deck readout a CDJ user reads in one glance

The two-deck readout keeps its 104 px and now reads in four rows:

1. **Identity:** badge, play state, title · artist.
2. **Values at 36/24 px:** BPM, tempo %, key with a ✓ only, and the clock. The BPM now shows one decimal (PF-D12).
3. **Flat lamps:** MASTER, SYNC and a headphone glyph (PF-D21, PF-D22).
   - Off: a dark dot and a #9AA3AF label, with no outline, so it no longer reads as a button.
   - Lit: MASTER fills with the deck colour, SYNC is outlined in it, and headphone cue fills white.
   - The SYNC slot is wide enough for MATCHED, so nothing moves during round 3's one-second MATCHED.
   - After the lamps comes the tempo range. When the tempo is beyond the fader range, it becomes an outlined white
     **OVER ±8%** chip and the tempo value is outlined white too (PF-D14).
4. **Phrase and phase:**
   - Left: the lit exceptions **LOOP n, SLIP, KEY** and **QNT OFF** (PF-D41, D42, D49), then the rekordbox phrase
     chip with "8.3 BARS".
   - Right: a **two-row phase meter** (PF-D24, D25):
     - the top row is the reference deck's beat, in its colour, labelled with its number;
     - the bottom row is this deck's beat;
     - the reference is the tempo master, or else another playing deck;
     - the lit beat fills in eighths, so two decks in phase light the same cell to the same edge, and a drift of an
       eighth of a beat shows;
     - on the master, or when nothing else is playing, the top row stays dark.
   - The phrase never replaces the meter any more.

Also:
- **Empty badge** (PF-D09): outlined in the full deck colour instead of a darkened fill.
- **Empty decks** (PF-D10): no transport glyph.
- **CUE** (PF-D10): an outlined orange chip.
- **Phrase chips** (PF-D27): outlined in the phrase colour, with white text.
- **Tempo-centre dot** (PF-D16): removed, because the muted 0.00% already says it.

**Four-deck cards** (compact readout) get:
- the same phase meter, as two thin rows;
- OVER on the tempo and as a lamp;
- QNT OFF only when quantize is off (PF-D49);
- the headphone glyph.

**Kept from round 3, unchanged:**
- the end-of-track clock (red figures alternating with a red box);
- MATCHED;
- SYNC hold-to-lock;
- four-deck BPM rounding;
- readout taps ignored while the deck plays.

**How it runs.** Everything is change-driven at the existing 100 ms tick:
- the phase meter repaints only its own rectangle (tested);
- the reference's beat is read at the same moment as this deck's, never a tick late;
- while any deck plays, a paused deck's meter follows the reference's beat, so a drop can be cued on the one.

### Move 2: pad modes

New `src/widget/decklabpads.{h,cpp}`: one **2×4 pad block per deck**, with a CUE / LOOP / ROLL / STEM cluster and
SYNC / MORE in a column on the right. It replaces the hot cue row, the transport row and round 3's separate 48 px stem
row:

```
 [CUE ][LOOP]  [ A ][ B ][ C ][ D ]  [SYNC]
 [ROLL][STEM]  [ E ][ F ][ G ][ H ]  [MORE]
```

**Layout**
- The block fits in the 96 px that the hot cue row and the transport row took. The deck row stays at 244 px, so the
  waveforms keep their 192 px at 800×480.
- A stem track no longer takes 48 px from the waveforms (they used to drop to 144 px). That is the "about 44 px back"
  from the brief.
- The audit's wireframe kept the transport row as well. That would have shrunk every normal track's waveforms by
  48 px, against the owner's "keep the waveform sizes".
- The mode keys form a 2×2 cluster, because four stacked keys in 92 px would be 22 px targets.

**The modes**
- **CUE:** hot cues A–H with round 3's look, touch-down firing and stale-touch guards.
  - Guards: the pads must show the touched deck and its current track, and the cue must be set or empty both on
    screen and in the engine.
  - The pads keep the names `decklab-hotcue-N-1..8`.
- **LOOP:** lengths 1/4, 1/2, 1, 2 / 4, 8, 16, 32.
  - One touch loops that length (`beatloop_X_toggle`).
  - The active length is lit (filled), and touching it again ends the loop.
  - The armed length (`beatloop_size`) is outlined.
  - This replaces ½ / LOOP 4 / ×2.
- **ROLL:** 1/16 … 8 (`beatlooproll_X_activate`), on only while held.
  - A roll always ends: on release, hide (page or layout change), a layer change, a load, touch cancel, deactivation,
    a press elsewhere (round 3's one-contact rule) or destruction.
  - The end is written to the deck and size it started on.
- **STEM:** shown **only for stem tracks**. Otherwise the STEM key is a blank, disabled cell.
  - Round 3's `DeckLabStemPads` and its gestures are reused unchanged: touch-down mute, hold to solo, double-tap to
    bring every stem back, and every stale-gesture guard.
  - The stem pads sit on the top row. The bottom row has **ALL** (bring every stem back, with no double-tap glitch,
    PF-D56) and a latching **SOLO** (PF-D57): while it is lit, a touch plays only that stem, on touch-down, with no
    hold.
  - The stem colour is now only a 3 px underline (PF-D58/59).
  - Names try Barlow 16, then Barlow Semi Condensed 16, then 14 px, so "DRUMS", "SYNTHS" and "VOCALS" fit the 52 px
    pads.

**Behaviour shared by all modes**
- Everything fires on touch-down. Lit means filled.
- The LOOP key shows a small lamp while a loop runs in another mode, and the STEM key shows one while a stem is muted.
  This was the stems note's follow-up.
- Each deck remembers its mode, in both layouts. On a normal track STEM falls back to CUE, and the choice returns with
  the deck's next stem track.

**MORE**
- MORE is now one grouped sheet in both layouts (PF-D44): PLAY (▶❚❚, CUE (hold)), DECK (QNT, KEY LOCK, **SLIP**,
  PF-D23), TRACK (Track info, Edit grid) and VIEW (ZOOM −/+).
- **QNT and KEY LOCK moved into MORE.** The readout lights KEY, and QNT OFF only when quantize is off (audit move 2).
- **The four-deck MORE** holds the same pad block for its deck, in place of the hot cue row and the stem row, so stems
  are no longer second-class in four-deck view (PF-D38, D61).

**Performance**
- The surface pushes one plain state per deck refresh (change-driven, never per tick).
- An unchanged state costs one comparison and writes nothing (tested), and a change repaints only the pads whose look
  changed.
- The block is pure Qt: the surface supplies reads, writes and the context through `Host`, so it renders offscreen
  and is unit-tested.

Waveforms are untouched, and nothing floats over them: MORE is a `QMenu`, a top-level popup.

## Before and after renders

All in `local/wt/audit/screens/r4-decks/`.

**Before**
- `before/readouts-before.png`: the base readouts, from the round-3 harness.
- `before/*-ci*.png`: the last real-app CI captures for reference (two-deck, four-deck, stems, phrase, master/sync).

**After (offscreen harness)**
- `after/decks-two.png`: master with phrase; follower synced OVER the range, looping.
- `after/decks-two-stems.png`: STEM mode, vocals muted.
- `after/decks-two-roll.png`
- `after/decks-two-empty.png`
- `after/decks-two-more.png`
- `after/decks-two-1024x600.png`
- `after/decks-four.png`
- `after/decks-four-more-stems.png`: four-deck MORE, STEM mode.
- `after/readouts-after-2x.png`: nine readout states and four compact cards.

The after renders come from an offscreen harness (local, not committed). It links the real
`decklabdeckwidgets.cpp`, `decklabpads.cpp` and the real `DeckLabStemPad` painting, with the real `kStyle`,
`kRailStyle`, `kDeckStyle`, `kActionStyle` and `kMenuStyle` text, on an 800×480 page mock with generated data.
Waveforms are black lanes, as in CI.

## Verified locally

- **Syntax and semantic compile, Qt 6.4 in WSL** (`g++ -std=c++20 -fsyntax-only -Wall -Wextra`, moc'd headers):
  - `wdecklab.cpp`, `decklabpads.cpp`, `decklabdeckwidgets.cpp` and `decklabstempads.cpp`: no warnings;
  - `decklabpadstest.cpp`, `decklabstemstest.cpp` and `decklabusbfirsttest.cpp`: no warnings;
  - `decklabtest.cpp`: compiled without `__LILV__`, because lilv isn't installed. It shows the same 9
    `-Wdangling-else` warnings as the base.
- **New unit tests built and run in WSL (gtest, offscreen Qt): 11/11 pass.**
  - `DeckLabPadsTest`, against a fake host:
    - CUE fires on touch-down with the guards: stale layer, other track, screen/engine mismatch, and disabled without
      a track;
    - LOOP toggles and lights the active length, and the LOOP lamp shows from CUE;
    - ROLL is on while held and ends on release and on each of 5 astray paths, plus destruction;
    - modes are remembered per deck and STEM is offered only for stem tracks, with fallback and return;
    - ALL and SOLO drive the stem hooks, and the latch drops on mode change and hide;
    - 44 px targets, 16 px type and 4 px gaps at the 399 px card width;
    - no repaint, layout or polish work for an unchanged state.
  - `DeckLabReadoutTest`:
    - four rows fit 104 px at 399, 400, 512 and 640 px, and the phrase never overlaps the phase meter;
    - the phase meter shows the reference and own colours and repaints only its rectangle, with a dark top row on the
      master;
    - OVER outlines the tempo and dirties tempo and lamps;
    - off lamps have no outline and MASTER fills the deck colour;
    - QNT OFF;
    - the empty badge is outlined in its deck colour.
- **Harness: 499/499 checks.**
  - Every visible button in every scene is at least 44×44 and inside the page.
  - No readout overflows: two-deck, four-deck, 1024×600, OVER, a 120-minute clock and 300 BPM.
  - The deck row is 244 px.
  - A held roll writes `beatlooproll_0.25_activate=1`, and `=0` when its screen goes away.
- `build_pi.py --check-inputs`: PASS after the lock refresh. `test_build_pi.py`: 7 tests OK.

## Needs CI and the Pi

**CI**
- The ARM64 build, and in the native suite:
  - `NativeDeckLabTest.*`: one new test, `PadModesAndReadoutRowsFollowTheRealDecks` (real rolls, page change mid-roll,
    OVER from a real 100 to 128 BPM sync, phase reference, SLIP, LOOP lamp, four-deck MORE). Updated tests:
    - `DeckTextFollowsTheOnDeviceScale`
    - `DeckIdentityAndLabelledReadoutsFitWithoutStealingWaveformSpace`
    - `AutoLayoutShowsFourRealDecksAndRetainsLoadedUpperDecks`
    - `SideUtilitiesOperateRealLoopsAndModesWithoutRetargetingHeldActions` (rewritten for LOOP pads)
    - `LayoutRoundTripsKeepModeAndPitchReadoutsInsideEachDeck`
    - `OnStatesAreFilledLampsNotOutlines`
  - `DeckLabStemsTest.*`: all stem tests now choose STEM mode first. The layout test now asserts that a stem track
    changes nothing until STEM is chosen. The look test checks the underline.
  - `DeckLabPhraseScreenTest`: also asserts that the phase meter stays beside the phrase.
  - `DeckLabPadsTest.*` and `DeckLabReadoutTest.*` were added to `test_suites`.
- Captures to look at:
  - existing: `decks-fixture-playing`, `decks-fixture-four`, `decks-lit-lamps`, `decks-sync-*`,
    `stems-two-deck(-solo, -1024x600)`, `stems-four-deck-more-menu`, `phrase-decks-*`;
  - new: `decks-pads-roll.png`, `decks-pads-loop.png`, `decks-sync-over-range.png`, `decks-four-more-pads.png`.

**The Pi**
- Feel and reach of 52 px pads in a booth.
- Rolls on real tracks (Mixxx slip roll).
- The phase meter against the master at 10 Hz.
- CPU: nothing new runs per tick except the reference's beat lookup, a binary search, once per shown deck.

## Merge notes

**New files**
- `src/widget/decklabpads.{h,cpp}`
- `src/test/decklabpadstest.cpp`
- this note

**`src/widget/wdecklab.cpp`**, every change marked `r4-decks`:
- `#include "widget/decklabpads.h"`.
- Removed: `kPadStyle`, `DeckLabHotcuePad` and its branch in `button()`, `buildHotcues()` and `refreshHotcues()`
  (they move into the block).
- Constructor: the two `*HotcuePrefixes` lines are removed.
- `buildTransport()`: one `slip_enabled` line in `perform` and in the button registry. The MORE builders are rewritten
  (`addGroup`, `zoomKeys`, `trackKeys`, `deckKeys`). The two-deck branch is now the SYNC/MORE column; the four-deck
  branch's MORE holds `buildPads()`.
- New `buildPads()`, `stemHooks()` and `refreshPads()` where `buildHotcues`/`refreshHotcues` were.
- `buildDecks()`: side pad blocks.
- `ensureDeckSurfaces()`: the card's stem row is removed.
- `fillDeckClock()`: one-decimal BPM, `overRange`, and the `beatAt` reference phase.
- `tickDecks()`: `anyPlaying`.
- `refreshTransport()`: `refreshPads` and `slip`.
- `updateLayers()`: the stem-row signature bit and `kRowHeight` are gone, side blocks sit at grid row 1, and the deck
  row is `104 + 44 + 4 + 92`.
- **Not touched:** `kStyle`, `kRailStyle`, the Browse, Lab FX, Sampler, Settings and System code, and the rail.

**`src/widget/wdecklab.h`**
- `class DeckLabPadBlock;` forward declaration.
- `buildPads`/`refreshPads` replace `buildHotcues`/`refreshHotcues`.
- `TransportState::slip` and `TransportButtons::slip`.
- `HotcueState`, `HotcuePads`, `m_hotcueButtons` and the two prefix arrays are removed.
- `m_sideHotcues` became `m_sidePads`, plus a new `m_cardPads`.

**Other files**
- `decklabdeckwidgets.{h,cpp}`: readout state fields (`overRange`, `phaseStep`, `reference*`), `deckLabBeatStep`, the
  layout, lamps and phase meter. Rail chip classes are untouched.
- `decklabstempads.{h,cpp}`:
  - Both presentations are now shown by their host block. `kRowHeight` and the Row margins are gone.
  - New `restoreAll()`, `setSoloLatch()`, `anyMuted()` and the `mutesChanged` signal.
  - New look.
  - `decklabstemgestures.h` is unchanged.
- `CMakeLists.txt`: `decklabpads.cpp` after `decklabdeckwidgets.cpp`, and `decklabpadstest.cpp` after
  `decklabstemstest.cpp`.
- `tools/decklab/pi-build-lock.json`: suites `DeckLabReadoutTest.*` and `DeckLabPadsTest.*`; hashes refreshed for
  `CMakeLists.txt`, `wdecklab.{h,cpp}` and `decklabtest.cpp`.

**Smoke count delta: +0 screens.** The app smoke has no media, so its Decks screens (0, 17–20) simply show the new
block; the new captures come from the native tests.

**Possible overlaps**
- **Overlay:** none; no new overlay.
- **System/rail:** none.
- **Browse:**
  - `refreshLoadButtons` is untouched.
  - Anything that looked for `decklab-N-loop_halve`, `-beatloop_activate` or `-loop_double` must use the LOOP pads.
  - `decklab-N-quantize` and `-keylock` still exist, inside MORE.

## Follow-ups

### Move 6: edit the beat grid on the waveform, not in a dialog (designed, not built)

It was left for a round where CI can verify it. It touches the GL waveform's input and zoom, which neither the harness
nor the unit tests can exercise.

**The sheet**
- "Edit grid" in MORE docks a 190 px sheet inside the deck row: the readout row stays, and the overview and pad block
  give way. It is never a floating window, so the waveforms stay visible above it.
- The edited deck is zoomed to 8 beats (`waveform_zoom` for that deck only, restored on DONE).

**Rows**
1. Header: `DECK 1 · GRID · 128.00 BPM`, the status (`Paused · unlocked`), a LOCK lamp (`bpmlock`) and DONE.
2. One row of 44 px keys: ◀ GRID, BEAT HERE, GRID ▶, BPM −, BPM +, ÷2, ×2, TAP. These use today's dialog controls,
   plus `bpm_tap`.
3. A hint: "Drag the waveform to slide the grid · changes save to this track".

**Interaction**
- Dragging the edited deck's waveform while the sheet is open becomes `beats_translate_*` steps, one per 4 px of
  travel. This needs a hook in `WWaveformViewer`'s touch handling, owned by pi4-perf/overlay.
- Keep `showBeatgrid`'s guards: paused, not synced, the shared track quiet, and the dialog closing on any context
  change. They become the sheet's.
- Track info gets the same kind of sheet from the right edge (PF-D51).

### Others
- **PF-D15:** RANGE ±6/±8/±16/WIDE in MORE's DECK group, and "Widen" offered from OVER.
- **PF-D19:** a 44 px tap target on the clock to toggle elapsed and remaining time.
- **PF-D30:** a Deck Lab hot cue palette that avoids the deck colours and red (`skin.xml` `<Mark>`).
- **PF-D36:** fill a saved-loop cue pad while that loop is active.
- **PF-D61:** name unnamed stems DRUMS/BASS/OTHER/VOCALS (NI order). This is still deferred because of the mislabel
  risk.
- Controller: the pads are the touch twin of the controller's pads; the browse knob is not mapped to them. MORE is a
  `QMenu` and gets the knob through round 3's `decklabknob` menu handling.
