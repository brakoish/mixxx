# Deck Lab next: Lab FX as one instrument (round 4, r4-labfx)

Branch `decklab/r4-labfx`, based on `decklab/round3` (5d4f4570f7). Source: `audit/performance.md`, move 3
("Lab FX as one instrument: Pioneer beat FX meets TE punch-in") and the PF-X findings it resolves. The DSP and the
effect engine are unchanged: this is UI over the existing Lab FX units (`[DeckLabEffectUnit1/2]`, slot 4, the
three-band performance LV2).

| Commit | Subject |
|---|---|
| d8fe23ea86 | Make Lab FX one instrument page (round 4, move 3) |
| (last) | This note |

## What changed

### Move 3: one page, no dropdowns

The five tabs (Bands / Sweep / Hold / XY / Samples) are gone. One page, built in the new
`src/widget/decklablabfx.{h,cpp}` (`DeckLabLabFx`), at 800×436 under the rail:

```
| LAB|FX  ECHO · 1/2 BEAT          [  ON   ]  [1][2][3][4][MASTER][SMP]  |
|         DECK 1 · 128.0 BPM       [LATCHED]                             |
| [ECHO  ] | ◀ |1/16| 1/8| 1/4|[1/2]| 3/4|  1 | ▶ |  FX BANDS            |
| [REVERB] |  ▲ DEPTH      columns = beat cells  |  [● HIGH  GETS FX ]  |
| [FILTER] |                 ( o )               |  [● MID   GETS FX ]  |
| [ ROLL ] |   HOLD TO PLAY · DOUBLE-TAP TO LATCH|  [○ LOW   DRY     ]  |
| [ DUCK ] |       LIFT: THE TAIL RINGS OUT      |                      |
| [DRIVE ] |                                     |                      |
| [RELEASE tail rings out][ECHO OUT track drops 1 bar][CUT dry now] | [1] ▶ title  ■□□□ −3:25 |
```

- **Pioneer beat-FX model.**
  - Effect select: six tiles with 24 px icons (fading bars, a wash, a slope, repeats, a pump, a clipped wave).
  - Beat ◀▶ with the value shown: six beat cells over the pad (lit cell, raise fill and a white underline) with ◀ and ▶
    at the ends, and the value in the header ("ECHO · 1/2 BEAT").
  - Channel select: 1–4 in deck colours (the target fills with its colour, like the badge; an empty deck keeps its
    colour bar and dims its number), MASTER (every deck and sampler) and SMP (the samplers).
  - A lit ON key: DRY (quiet outline), ON (white fill, with HELD or LATCHED), TAIL (white outline blinking on the beat).
- **DJM X-PAD style pad.** Y is depth (the bottom edge is a light touch, never silence; "DEPTH 65" is the default,
  half way). X depends on the effect:
  - ECHO, REVERB, ROLL: X is the beat. The pad's columns line up with the beat cells, so a slide changes the delay or
    roll size live, as on the DJM X-PAD.
  - FILTER, DUCK, DRIVE: X is the sweep, dry in the middle: LPF ◀ ● ▶ HPF, ON-BEAT ◀ ● ▶ OFF-BEAT, CUT ◀ ● ▶ DRIVE.
- **Band lamps** LOW / MID / HIGH (HIGH on top, like an EQ) choose which bands get the effect, like the DJM's FX
  FREQUENCY keys. Lamp style: white dot and "GETS FX", or a hollow dot and "DRY". While the effect sounds, the lit
  bands fill white.
- **Footer:** RELEASE (tail rings out), ECHO OUT (track drops 1 bar), CUT (dry now), each with its consequence printed
  under it. Then a readout of the target: badge, ▶/❚❚, title, four beat lamps in the deck colour and time left (live red
  at end of track). For SMP the footer is the link to the Sampler page.
- **TE instrument feel:**
  - every key acts on touch-down (`pressed`);
  - the pad is momentary by default and latches with a double-tap;
  - every control says what it does: the pad prints "LIFT: THE TAIL RINGS OUT" / "LIFT: BACK TO DRY" / "LIFT: THE
    TRACK PLAYS ON IN TIME", and while touched shows what the finger does ("1/2 BEAT · DEPTH 72", "HPF 64 · DEPTH 72").

**How each effect maps onto the unchanged engine:**

| Tile | DSP | X | Lift |
|---|---|---|---|
| ECHO | lever mode 1, lit bands send | beat (1/16–1) | tail rings out (release type 1) |
| REVERB | lever mode 2 | beat (1/16–1) | tail rings out |
| FILTER | isolator mode 4 | LPF ◀ ● ▶ HPF | back to dry |
| ROLL | Mixxx `beatlooproll_<size>_activate` (slot dry) | beat (1/16–2) | roll ends, slip carries on |
| DUCK | isolator mode 5 | ON-BEAT ◀ ● ▶ OFF-BEAT | back to dry |
| DRIVE | isolator mode 3 | CUT ◀ ● ▶ DRIVE | back to dry |

The beat range is the DSP's: `beat_fraction` is 1/16–1 beat, so delays offer 1/16 · 1/8 · 1/4 · 1/2 · 3/4 · 1, not the
audit's 2 and 4. ROLL uses Mixxx's roll sizes, 1/16 to 2. ROLL runs the slot dry under the roll, so RELEASE and ECHO
OUT have the roll in their tail.

### Decisions the brief asked for

- **Latch gesture: double-tap.**
  - It needs one finger. Qt drives widgets from one touch point, so "hold and tap ON" can't work.
  - It never delays a release. A design that waits to see whether a second tap is coming would make every punch-out
    late.
  - Slide-to-latch is wrong on an XY pad: sliding *is* the parameter, so the latch gesture would change the sound.
  - It is the TE punch-in convention the audit asks for (PF-X08, PF-X10).
  - Cost: between the two taps the effect returns briefly. For ECHO and REVERB the tail covers it; FILTER, DUCK and
    DRIVE are dry for the gap.
  - Two glitch-free alternatives:
    - a double-tap on an effect tile chooses it and latches it on. The first tap only chooses, so a tile tap never
      sounds;
    - the ON key latches the selected effect at the last place it was played (the dotted ring on the pad).
- **Leaving the page (PF-X24):**
  - A latched effect keeps playing on every page. The tick keeps its heartbeat, tempo and phase, and the rail's Lab FX
    tab stays lit (it blinks with the beat during a tail).
  - A held effect is not cut by a page change. The pad does not end its hold when hidden. The hold ends when that
    finger lifts: the release reaches the window (verified with real `QWidgetWindow` delivery, see below), or the next
    touch lands anywhere.
  - A page change by touch can't happen while a finger holds the pad, for the single-pointer reason above. In practice
    this is a controller page switch.
- **Lost-touch safety (`endHolds`), with no wall clock (round 3):**
  - a press or release reaching any other widget ends a pad hold;
  - a release or TouchEnd reaching the window of a hidden page ends it;
  - TouchCancel ends it.
  - A latched effect only stops following the finger.
  - The DSP's 1.5 s heartbeat deadman still covers a frozen GUI. A GUI stall longer than that cuts to dry.
- **Forced exits ring out instead of cutting:** application deactivate and a hidden surface now RELEASE (tail rings
  out). CUT stays for the destructor, `releaseEffects()` callers, the tool launchers and an ejected target.
- **Switching target (PF-X02):** the old target rings out on its own unit, and the new one starts dry. Decks 1/3 and
  MASTER use unit 1; decks 2/4 and SMP use unit 2. So deck 1's tail rings while deck 2 plays.
- **Samples tab:** removed, not duplicated. SMP in the channel select routes Lab FX to the samplers. The SMP footer
  ("2 PLAYING · 5 LOADED · SAMPLER ›") and the empty-state button open the Sampler page.
- **Waveform lanes:** Lab FX now hides them, as Sampler does. This gives the pad 272 px instead of 132, and the Pi's GPU
  renders no waveforms on this page. The footer readout replaces their context. The layout still fits if the lanes come
  back (2-column tiles at 800×296, checked).
- **Controller:** the browse knob keeps its round-3 meaning on every page (a turn opens Browse), so a DJ can load from
  anywhere. Lab FX is a touch instrument. Its keys take no keyboard focus, like every Deck Lab button. Follow-up: a
  `[DeckLabFX]` control group for mappings.

### Findings resolved

| ID | Now |
|---|---|
| PF-X01 | Every Lab FX control is painted in Barlow and Barlow Semi Condensed on the brand tokens. No stock widgets are left on the page. |
| PF-X02 | 1 2 3 4 in deck colours, 44 px; empty decks dim but keep their colour; switching rings out. |
| PF-X03 | The ON key is the state lamp: DRY / ON (HELD, LATCHED) / TAIL blinking on the beat. |
| PF-X04, PF-X05 (P0) | RELEASE "tail rings out", ECHO OUT "track drops 1 bar" (was "Mute tail"), and a new CUT "dry now". |
| PF-X06, PF-X07, PF-X09 | One page; the mode dropdowns became effect tiles; each sweep's meaning is printed on the pad. |
| PF-X08 | Band lamps act on touch-down, lit = white dot, a white fill while sounding. They are selectors (FX FREQUENCY), because the pad is the momentary part. |
| PF-X10 | No sliders and no CENTRE buttons. Lift returns the sweep. |
| PF-X11, PF-X12, PF-X15 (P0) | No time limit anywhere under a finger. ROLL is an effect type on the beat cells. |
| PF-X13, PF-X14 | Scenes and their developer hint are gone; double-tap, the tile double-tap or ON latch whatever is set up. |
| PF-X16 | Panel and line tokens, 14 px axis labels, a white ring with crosshairs, a dotted ring where ON will play, a live readout. |
| PF-X17 | The pad changes nothing elsewhere (no hidden mode switch). |
| PF-X18, PF-X19, PF-S14 | The Samples tab and its "A–D" pads and rolls are gone. SMP plus the Sampler link replace them, and ROLL on SMP rolls the playing analysed samplers. |
| PF-X20 | Beat cells with the current one lit. |
| PF-X21 | Depth is the pad's Y, shown as "DEPTH 72". |
| PF-X22 | SMP is a target ("SAMPLER FX" is gone). |
| PF-X23 | The lanes are gone from Lab FX; the footer reads the target deck. |
| PF-X24 | See "Leaving the page" above. |
| PF-X25 | Empty states on the pad: "Load a track on deck 3 to play Lab FX" with BROWSE, the samplers with SAMPLER ›, and "Lab FX effect didn't load. Restart Deck Lab." |
| PF-X26 | The brand Lab FX wordmark (`labfx-wordmark.svg` geometry, embedded) at 24 px cap height. |
| PF-X27 | Header: "ECHO · 1/2 BEAT" over "DECK 1 · 128.0 BPM". |
| PF-R11 | The rail tab is lit on every page while wet, and blinks with the beat during the tail (the existing `live` property). |

## Renders (before / after)

All renders are in `local/wt/audit/screens/r4-labfx/` and are local only (not in the repo).
- **Before:** `before-1-bands` … `before-5-samples`. A faithful mock of the round-3 page: the same widgets and the same
  `kPerformStyle`, with the 140 px lanes drawn above it.
- **After:** the real `decklablabfx.cpp`, linked with a generated fake engine:
  - `after-01-echo-dry`, `after-02-echo-held`, `after-03-echo-tail`
  - `after-04-filter-latched`, `after-05-roll-held`, `after-06-duck-master`
  - `after-07-deck3-empty`, `after-08-smp-empty`, `after-09-smp-loaded`
  - `after-10-unavailable`, `after-11-1024x600`, `after-12-800x296-with-lanes`
- **Checks:** `report.txt`. The rail in every render is a drawn mock.

## Verified locally (WSL, Qt 6.4.2, g++ 13)

- **Compile:** `decklablabfx.cpp` builds with `-Wall -Wextra -ffast-math` and no warnings.
- **Syntax check:** `-fsyntax-only -Wall -Wextra` with a fresh moc and the round's protobuf output, for `wdecklab.cpp`,
  `decklabsmoke.cpp`, `decklabtest.cpp` and `decklablabfxtest.cpp`. No new warnings. `decklabtest.cpp` keeps the base's
  nine `-Wdangling-else` warnings, and SPSCQueue keeps its third-party `-Winterference-size` warning.
- **`DeckLabLabFxTest`: 11/11 pass**, standalone with gtest and a fake engine.
  - Touch-down plays.
  - Lift returns per effect.
  - Double-tap latches, while one tile tap stays silent.
  - Pad columns are the beat cells, and ◀▶ step.
  - No time limit over 60 s held.
  - The page is hidden under latched and held effects; window-level and foreign touches end holds.
  - A target switch rings out on the old unit; MASTER and SMP routing.
  - ECHO OUT timing: type 2, then 3 after a bar, then off.
  - ROLL needs a playing deck and slides between sizes without a gap.
  - The GUI-stall deadman.
  - Empty states and the Sampler link (the page changes on release only).
  - ON lamp pixels, no live red outside the wordmark, and 44 px keys.
- **Harness: 43/43 checks pass**:
  - every key is 44×44 or larger, inside the page and 4 px or more from its neighbours, at 800×436, 1024×556 and
    800×296;
  - the engine writes for each state.
- **Real Qt delivery probe:** with `QTest` on the `QWindow`, a pad held while its `QStackedWidget` page switches away
  stays on. The finger's release then reaches `QWidgetWindow` and a widget, and the effect returns (TAIL).
- **Build scripts:** `build_pi.py --check-inputs` PASS after the lock refresh; `test_build_pi.py` 7 tests OK.

## Needs CI and the Pi

- **CI (ARM64 Flatpak):**
  - `DeckLabLabFxTest.*` (added to the lock's `test_suites`).
  - The updated `NativeDeckLabTest` cases:
    - `ApplicationDeactivateReleasesRmxWithoutChangingControllerFx`: now a tail, then off within 4 s;
    - `LongMetadataCannotHideTempoOrExpandSurface`: minis on Browse, the Lab FX footer;
    - `LatchedRmxFollowsTempoChangesWithoutAnotherTouch`;
    - `LabFxEffectTilesDriveTheExistingEngine`: replaces `TransitionStartersApplyOwnedRmxAndOpenItsEditor`;
    - `RollPadsRequireAPlayingDeck`;
    - `XyPadAndHoldEffectsOutliveFourSecondsAndLostTouchesEndThem`;
    - the Lab FX half of `LabFxAndSamplerUseBarlowBrandColoursAndClearCopy`.
  - The smoke run (46 screens), then a look at `app-screen-05` to `09`.
- **Pi, by ear and hand:**
  - X-PAD slides on ECHO and REVERB: the DSP smooths delay time over 40 ms, which should glide, not click.
  - The double-tap window (350 ms) on the real panel.
  - DUCK on MASTER pumping in time.
  - Lab FX tab blinking during a tail, with no measurable GUI cost.
  - That the page renders no waveforms.

## Merge notes

- **New files:**
  - `src/widget/decklablabfx.h`, `src/widget/decklablabfx.cpp`: the page and its state (`DeckLabLabFx`, host struct
    `DeckLabLabFxHost`);
  - `src/test/decklablabfxtest.cpp`.
- **CMake:**
  - `src/widget/decklablabfx.cpp` after `src/widget/decklabstempads.cpp`;
  - `src/test/decklablabfxtest.cpp` after `src/test/decklabvsynctest.cpp`.
- **`wdecklab.cpp` regions** (all marked `r4-labfx`):
  - Include `widget/decklablabfx.h`.
  - **Removed:** `holdBackstopMs` and the `DeckLabXY` class. Both were used only by Lab FX.
  - **`buildEffects()`:** the body is replaced. It fills the host and returns `new DeckLabLabFx`.
  - **`selectPage()`, two hooks:**
    - leaving page 3 no longer calls `releaseEffects()`;
    - `m_waveforms->setVisible(page == 0 || page == 1)` (was `|| page == 3`).
  - **`tickDecks()`, one line:** the tab's `live` property comes from `m_labFx->tabLit()`.
  - **Lab FX functions:** `fxGroup`, `fxSlot`, `refreshFx`, `applyEffects`, `endHolds`, `releaseEffects` and
    `tickEffects`'s Lab FX part become thin forwards. `applyEffects`, `fxGroup` and `fxSlot` are deleted.
    `tickEffects` keeps its tools/`tickDecks`/breadcrumb lines.
  - **`eventFilter()`, two hooks:**
    - TouchCancel → `endHolds()`; ApplicationDeactivate → `releaseEffects(true)`;
    - the lost-touch block calls `m_labFx->noteTouch()`.
  - **`hideEvent()`:** `releaseEffects(true)`.
  - **Untouched:** `kStyle`, `kRailStyle` and `kPerformStyle`. Its `decklab-fx-status` rules are now unused; Sampler
    still uses the sheet. The tool and Settings call sites of `releaseEffects()` are also untouched.
- **`wdecklab.h`:**
  - forward declaration `DeckLabLabFx`;
  - `applyEffects`, `fxGroup` and `fxSlot` removed;
  - the Lab FX members removed (`m_levers` … `m_sampleRouting`, `m_updating`, `m_fxSample`, the held-roll and hold
    fields);
  - `DeckLabLabFx* m_labFx` added.
  - `m_sampleTarget` and `m_sampleReturnPage` stay; they are Sampler's. Possible line-adjacent conflicts there with
    r4-sampler.
- **`decklabsmoke.cpp`:** the new `labFxSmokeStep()` drives steps 5–9. **Smoke screen count: +0 screens** (still five Lab
  FX captures).
- **`decklabtest.cpp`:**
  - the Lab FX tests listed above;
  - the Sampler half of `LabFxAndSamplerUseBarlowBrandColoursAndClearCopy` is unchanged, but r4-sampler may edit the
    lines right after mine.
- **Lock:**
  - refreshed `CMakeLists.txt`, `src/test/decklabtest.cpp`, `src/widget/decklabsmoke.cpp`, `src/widget/wdecklab.cpp`
    and `src/widget/wdecklab.h` (CRLF→LF digests);
  - added `DeckLabLabFxTest.*` to `test_suites`.
  - Recompute these digests after merging the other areas.

## Follow-ups

- A `[DeckLabFX]` control group (effect, beat ◀▶, target, ON, hold, x/y) so controller mappings can play Lab FX.
- Decks owner (move 1/2): a white "LAB FX" chip on the target deck's lane while an effect plays (PF-X23 on Decks).
- Browse returns to Decks after a load. When BROWSE came from the Lab FX empty state, returning to Lab FX would be nicer.
- The audit's 300 ms touch trail on the pad (PF-X16) and a beat-quantised start for strip changes (PF-X20) are not
  done.
- Lever mode 3 (BAND SOLO) and the isolator's echo/reverb CUT/SEND modes are no longer exposed. The DSP still has them.
