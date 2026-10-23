# Deck Lab next: round 4, Sampler (r4-sampler)

Branch `decklab/r4-sampler`, based on `decklab/round3` (5d4f4570f7). Source: `audit/performance.md`, section 3
move 4 ("Sampler as a 4×4 instrument with the decks in view") and the PF-S findings it resolves. No audio-engine or
effect-DSP changes.

## What changed

### Move 4: the Sampler as a 4×4 instrument

The page is a new surface in new files. `WDeckLab::buildSampler()` now only builds it and wires it in.

```
+------------------------------------------------------------------------------+
| rail (unchanged)                                                             |
+------------------------------------------------------------------------------+
| [1] > 128.0 #... MASTER -3:25 | [2] 126.0 ..#. -0:32          | DECKS >      |  44 px DECKS strip
+-----------------+-----------------+-----------------+-----------------+------+
| 1     124 BPM -> | 2    123 BPM (o)| 3               | 4               |QNT|SY|
| Signal Fire      | Glass Garden    |       +         |       +         |ED|FX |
|| ...      -0:06  || ...     -0:02  |                 |                 | LEVEL|
+-----------------+-----------------+-----------------+-----------------+  ||  |
|  ... rows 2-4 (169 x 94 px pads)                                      | BANK |
|                                                                       | STOP |
+-----------------------------------------------------------------------+------+
```

| Part | What it does |
|---|---|
| **Grid** | 4×4 pads, four wide like the EP-133 K.O. II and the DJS-1000, about 169×94 px at 800×480. Each pad shows its number, a two-line title (16 px SemiBold), a painted mode icon (one-shot →, loop ⟳, gate ▮), its BPM, a 4 px gain bar on the left edge, a "⋯" that teaches the hold, and its length (idle) or time left (playing). |
| **Playing** | A filled `#F2F4F7` lamp that **drains** from right to left: the lit part is the length still to play. Text is inked dark on the lamp and light on the drained part, so it stays readable. A 3 px top bar keeps saying "playing" until the end. Time left reads `−0:06` (18 px tabular figures). |
| **DECKS strip** | 44 px, under the rail. Per deck: the badge in its deck colour, the green ▶ while playing, the BPM (one decimal when two decks share it), the **four-beat lamp** (the current beat in the deck colour, dimmer while paused, as on the Decks phase meter), a **MASTER** chip ("M" in compact cells), the time left (live red inside the end warning), and the track position as a 2 px line in the deck colour. Decks 1 and 2 always, 3 and 4 while loaded. Tap anywhere to go back to Decks ("DECKS ›"). |
| **Side column** | 100 px: **QNT** and **SYNC**, **EDIT** and **FX** (2×2, 48×44 each), **LEVEL** (all pads), **BANK** (only with more than 16 samplers), **■ STOP ALL**. |
| **Edit sheet** | Docked over the half of the grid away from the pad (the pad and the finger stay in view), in brand colours: `PAD 7 · title`, **DONE**; **MODE** ONE-SHOT / LOOP / GATE; **GAIN** − value + (1 dB steps, −12 to +12, the pad's `pregain`); **STOP**; **CLEAR** (Mixxx's eject) at the far end from STOP. |

### Behaviour

- **Touch-down, kept from round 3.** A loaded pad fires on touch-down with `play_stutter`. An empty pad opens Browse on release, with the same stale-touch guard (the interaction epoch plus the page's own hides).
- **Modes.**
  - ONE-SHOT: every touch plays from the cue.
  - LOOP: the sampler's `repeat` is on. A touch starts it, a second touch stops it.
  - GATE: plays while held and stops on release. A gate whose release goes astray stops at the next touch anywhere on the page, on the next tick after a touch cancel, or when the page hides.
  - The mode is remembered per pad (`[DeckLabSampler] pad_N_mode`).
- **Hold 500 ms** on a loaded ONE-SHOT or LOOP pad opens its sheet. Touch-down already fired the pad, so the sample keeps playing and STOP is the first key on the sheet. For a **silent edit**, turn **EDIT** on: a tap then selects the pad instead of playing it. Holding a GATE pad plays it; use EDIT to edit it. DONE (or the knob's press or back) closes the sheet and turns EDIT off.
- **Destructive stays in the sheet.** CLEAR acts on release, is disabled while the pad plays, and closes the sheet.
- **QNT** sets `quantize` on every sampler and shows it lit when all samplers have it. Mixxx's sampler quantize starts a pad in phase with the master beat. It phase-aligns; it does not delay to the next beat.
- **SYNC** sets `sync_enabled` on loop pads (`[DeckLabSampler] sync_loops`).
- **LEVEL** sets every sampler's `volume`. It is a fader that moves with the finger from where it lands and never jumps. The value reads in dB.
- **FX** is the Lab FX sampler send (`m_sampleRouting`, the same state as Lab FX's SAMPLER FX key). Tapping it updates both keys. When Lab FX changes the send, the lamp follows on the next display tick.
- **Banks.** More than 16 samplers in `[App],num_samplers` (a controller mapping can ask for more) become banks of 16. **BANK A/B/…** cycles them, pads are numbered on from 17, and a bank's spare pads are dark. STOP ALL stops every bank.
- **Browse knob, only while the sheet is open:** turn = GAIN ±1 dB per step, press = done, back = close. With the sheet closed the knob keeps its global meaning (turn or press opens Browse).
- **Bug fixed on the way:** a ONE-SHOT pad that had **played to the end was dead.** The engine refuses `play` at the end of a track, so `play_stutter` did nothing until the pad was reloaded. A stopped pad is now rewound to its cue (`cue_gotoandstop`) on touch-down, before `play_stutter`, in the same event. Without a cue point the touch uses `start_play` instead. STOP and STOP ALL now go back to the cue (`cue_gotoandstop`) instead of pausing where the sample was, so the next touch starts from the top.

### Findings resolved

| ID | Status |
|---|---|
| PF-S01 grid 6/6/4 | **Done.** 4×4 plus the side column (QNT, SYNC, LEVEL, STOP ALL, and EDIT, FX, BANK). |
| PF-S02 font and colour | **Done.** Barlow and Barlow Semi Condensed tabular figures, brand tokens only. Empty pad: the number and a 24 px "+". |
| PF-S03 trigger timing | **Kept** (round 3: touch-down) and extended to every mode. **QNT** lamp added. |
| PF-S04 playing state | **Done.** Lit lamp, 3 px top bar, drain sweep, time left. |
| PF-S05 trigger modes | **Done.** ONE-SHOT, LOOP (touch again stops), GATE, in the sheet. |
| PF-S06 hold menu | **Done.** A real 500 ms long-press timer (no reliance on context-menu events), a docked brand sheet, GAIN in dB, MODE, CLEAR. The trigger itself is not cancelled, because touch-down fires first by design; EDIT gives the silent route. |
| PF-S07 STOP ALL | **Done.** Outlined "■ STOP ALL" in the side column, disabled when nothing plays. |
| PF-S08 hold hint | **Done.** "⋯" on loaded pads. "Hold a pad to edit" is in the pads' and EDIT's spoken descriptions. |
| PF-S09 deck context | **Done.** The DECKS strip. |
| PF-S10 tempo sync | **Done.** Each pad's BPM, and the SYNC lamp for loop pads. |
| PF-S12 level | **Mostly done.** The gain bar on each pad, GAIN in the sheet and on the knob. Hold-and-slide on the pad is not done (see follow-ups). |
| PF-S14 naming | **Done here.** Pads are numbered 1–16 (17+ in banks). `WDeckLab::showSampler(int)` is the link for Lab FX's SMP destination. |

Copy from audit section 4: "+" with the pad number, accessible "Pad 7, empty: tap to load"; "■ STOP ALL"; "Hold a pad
to edit"; sheet STOP / CLEAR / GAIN (dB); tooltip "Tap to load from Browse".

## Renders (800×480, before and after)

`local/wt/audit/screens/r4-sampler/` (outside the repo; generated demo names only):

- `before/sampler-empty.png`, `before/sampler-loaded.png`: round 3's real `DeckLabPad` and `kStyle`/`kRailStyle`/`kPerformStyle`, extracted from 5d4f4570f7, in round 3's 6/6/4 layout.
- `after/sampler-empty.png`, `after/sampler-playing.png` and `after/sampler-playing-later.png` (the drain), `after/sampler-edit-sheet.png` (top-half pad), `after/sampler-edit-sheet-bottom-pad.png`, `after/sampler-four-decks.png` (compact cells, end warning, a deck without a grid), `after/sampler-bank-b.png` (20 samplers configured: bank B, spare pads dark).
- `report.txt`: the pad size (169×94).

The rail in both is built from the real `decklabdeckwidgets.cpp`.

## Verified locally

- **Unit suite `DeckLabSamplerTest`** (new, `src/test/decklabsamplertest.cpp`, 11 tests): built in WSL against Qt 6.4 with gtest and run offscreen. **All 11 pass.** It drives the real page with generated samplers and covers:
  - the 4×4 geometry and 44 px keys;
  - touch-down fire, and Browse on release with the stale-touch guards;
  - drain width, time left and rate, and **paint counting: idle pads and unchanged strip cells never repaint on the tick**;
  - hold → sheet docking (both halves), CLEAR on release and only when stopped;
  - EDIT without playing, MODE, GAIN buttons and the knob, close;
  - LOOP second touch, GATE release, lost release, hide;
  - QNT, SYNC, the FX send and the LEVEL relative drag;
  - STOP ALL across banks;
  - banks 20/32 and spare pads;
  - the DECKS strip's cells, beat lamp, one-cell repaint and DECKS tap;
  - hide ending holds and the sheet.
- **Syntax and semantic compile** (`g++ -std=c++20 -fsyntax-only -Wall -Wextra -ffast-math`, Qt 6.4, fresh moc output) of `decklabsampler.cpp`, `decklabsamplercontrols.cpp`, `wdecklab.cpp`, `decklabsmoke.cpp`, `decklabsamplertest.cpp` and `decklabtest.cpp`. No new warnings. `decklabtest.cpp` keeps the base's 9 `-Wdangling-else` warnings.
- **Offscreen renders** above, from a harness that links the real `decklabsampler.cpp` and `decklabdeckwidgets.cpp`.
- `python tools/decklab/build_pi.py --check-inputs`: PASS. `test_build_pi.py`: 7 tests OK.
- WSL packages: nothing was installed. gmock is still missing; nothing here needs it.

## Needs CI (ARM64 Flatpak) and the Pi

- **`NativeDeckLabTest`**, updated:
  - `SixteenSamplerPadsRemainNearSquareAndFullyVisible` is replaced by `SamplerIsAFourByFourInstrumentWithTheDecksInView` (4×4, the strip under the rail following deck 1 on the tick, the DECKS tap, `showSampler()`).
  - Unchanged and expected to pass as they are: `EmptySamplerHasAnExplicitLoadAndCancelFlow`, `PagesFit800By480`, `LabFxAndSamplerUseBarlowBrandColoursAndClearCopy` (pixel at (3, h/2) is panel, then lit) and `SamplerPadsFireOnTouchDownAndEmptyPadsOpenBrowseOnRelease` (exactly one `play_stutter`).
- **`NativeDeckLabTest`**, four new tests with bare generated `[SamplerN]` controls (no engine, no audio):
  - `SamplerPadsLightAndDrainOnTheDisplayTick`;
  - `SamplerHoldOpensTheEditSheetAndTheKnobEditsGain`: hold, the knob via `[DeckLabNative] browse_move`, LOOP sets `repeat` and is remembered, CLEAR ejects on release, back closes;
  - `SamplerLoadReturnsToThePadsAndFxFollowsLabFx`;
  - `SamplerBanksFollowTheConfiguredSamplerCount`. It skips itself if a player manager owns `[App],num_samplers`.
- **`DeckLabSamplerTest.*`** added to `test_suites` in the lock.
- **Captures to look at:**
  - test captures: `sampler-grid.png`, `sampler-playing.png`, `sampler-edit-sheet.png`, `sampler-bank-b.png`, `sampler-brand.png`, `sampler-unit-*.png`, `screen-2.png`;
  - smoke: `app-screen-02.png` (Sampler tab), plus two new **uncounted** captures after the Browse ones: `app-sampler-01-pads.png` (demo tracks Signal Fire, Glass Garden and Spiral Steps loaded into pads 1, 2 and 5 with `[SamplerN],LoadSelectedTrack`, **never played**) and `app-sampler-02-edit.png` (pad 2's sheet).
- **On the Pi:**
  - Load real samples; check the drain and the beat lamp against the music at the 10 Hz tick.
  - Hold a pad on the real touchscreen (500 ms).
  - Turn the Mixtrack's browse knob with the sheet open.
  - Play a one-shot to its end, then touch it again: it must play (the dead-pad fix).
  - Check that the controller's own sampler pads light the screen pads.

## Merge notes

- **New files:**
  - `src/widget/decklabsampler.{h,cpp}`: the page and painted widgets, pure Qt, through `DeckLabSamplerEngine`.
  - `src/widget/decklabsamplercontrols.{h,cpp}`: the `[SamplerN]` binding.
  - `src/test/decklabsamplertest.cpp`.
  - This note.
- **`CMakeLists.txt`:** the two sources after `src/widget/decklabdeckwidgets.cpp`; the test after `src/test/decklabstemgesturestest.cpp`.
- **`src/widget/wdecklab.cpp`** (line numbers on this branch; every hook is marked `// r4-sampler`):
  - includes, lines 71–72;
  - **`buildSampler()` is replaced** (2071–2097; round 3's body is gone, the `return page; }` tail is kept), followed by the new `tickSampler()` and `showSampler()` (2099–2132), just before `buildEffects()` (2134), which is untouched;
  - one-line knob hooks at the top of `browsePress()` (2974), `browseMove()` (2993) and `browseBack()` (3043);
  - one line in `tickDecks()` (3930), before the waveform-visibility return.
  - Not touched: `kStyle`, `kPerformStyle`, `kMenuStyle`, `DeckLabPad` (Lab FX's Samples tab still uses it), the Decks and Lab FX builders, `refreshFx()` and `applyEffects()`.
- **`src/widget/wdecklab.h`:**
  - forward declaration `class DeckLabSampler;`;
  - public `showSampler(int sampler = 0)`;
  - private `tickSampler()`;
  - `m_samples` (16 `QPushButton*`, now unused) is replaced by `DeckLabSampler* m_sampler`.
- **`src/widget/decklabsmoke.cpp`:**
  - include;
  - new `captureSampler()` after `struct BrowseCapture`;
  - `samplerDone` next to `browseDone`;
  - one `else if (!*samplerDone)` block just before the final JSON branch.
- **`src/test/decklabtest.cpp`:**
  - include;
  - the grid test replaced (after `PagesFit800By480`);
  - helpers and four new tests before `FourDeckBpmRoundsInsteadOfCutting`.
- **Smoke screen count: +0 screens** (still 46). The two new PNGs are uncounted, like the Browse captures.
- **Lock:**
  - `tools/decklab/pi-build-lock.json` input hashes refreshed (CRLF→LF digest) for `CMakeLists.txt`, `src/test/decklabtest.cpp`, `src/widget/decklabsmoke.cpp`, `src/widget/wdecklab.cpp` and `src/widget/wdecklab.h`;
  - `DeckLabSamplerTest.*` added to `test_suites`.
- **For r4-labfx:**
  - Open the page with `WDeckLab::showSampler()`. `showSampler(n)` also shows the bank that holds sampler n.
  - The FX lamp writes `m_sampleRouting` and, if a button named `decklab-fx-samples` still exists, sets its checked state; then it calls `refreshFx()` and `applyEffects()` as SAMPLER FX did. If Lab FX renames or replaces that key, change that one line in `buildSampler()`. The lamp itself follows `m_sampleRouting` on the tick.
  - Returning to the asking page after a pad loads is now handled for **every** sampler by `DeckLabSamplerControls::sampleLoaded`, through `m_sampleTarget`/`m_sampleReturnPage`, so a Lab FX pad flow keeps working without the old per-slot watchers.
- **For the rail (move 5):** `DeckLabSampler::anyPlaying()` is there for a SAMPLER "▶" tab lamp. I did not touch the rail.
- **For r4-browse:** my only Browse-side lines are the three one-line hooks at the top of `browsePress`, `browseMove` and `browseBack`. They act only on page 2 with the sheet open.
- **Performance (Pi 4 GUI thread):**
  - No new periodic timer. The page runs from the existing 100 ms display tick (`tickDecks`) and only while it is shown.
  - The deck clocks advance there because Decks' own tick skips them while the waveforms are hidden.
  - Only playing pads whose drain moved a pixel (or whose seconds changed) and strip cells whose whole-unit values changed repaint. No allocation on an unchanged tick.
  - Engine watches are queued and deferred while the page is hidden.
  - The 500 ms hold timer is single-shot and runs only during a touch. The GAIN keys' auto-repeat runs only while held.

## Follow-ups

- **PF-S12 hold-and-slide** on a pad to set its gain (the sheet and the knob cover it for now).
- **"Next beat" triggers:** Mixxx's sampler quantize phase-aligns the start. A true wait-for-the-1 would need an engine-side scheduled start; a GUI timer would be too late on the Pi.
- A 25 Hz drain: move the `tickSampler()` call to the 40 ms meter tick if the 10 Hz steps look coarse on short samples. It is one line, but it is a budget decision.
- A per-pad colour or icon picker; per-pad VOLUME besides GAIN; sample BPM display options (file BPM vs synced BPM).
- Mapping the Mixtrack's sampler pad mode to the current bank (controller scripts address samplers by number today).
- A SAMPLER rail lamp while any pad plays (move 5's owner), using `anyPlaying()`.
