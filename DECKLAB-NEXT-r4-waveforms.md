# Deck Lab next: round 4, waveform lanes on the compact pages (r4-waveforms)

Branch `decklab/r4-waveforms`, based on `decklab/round4` (b40c9dd716, then merged up to 0a720f744c). The owner: "I
think we need to see waveforms." After the round-4 merge only Decks showed the scrolling lanes. Scope, as narrowed
by the owner ("Browse doesn't need it maybe"): lanes come back on **Sampler and Lab FX**; Decks is unchanged; **Browse**
keeps r4-browse's deck dock, legend and list height with no lanes; Settings has none.

The model is the Pioneer all-in-ones: the XDJ-RX3 keeps both enlarged waveforms on top and works the pads or beat FX
in the panel below them; the XDJ-AZ shows scrolling waveforms for the two decks in control and overviews for all four.

## What changed

### Where lanes show: one rule, one place
`decklab::lanes::presentationFor(page)` (`src/widget/decklablanes.h`) returns Full (Decks), Compact (Sampler, Lab FX)
or None (Browse, Settings). `WDeckLab::selectPage()`, `updateLayers()`, `buildSampler()`, `buildEffects()` and
`resizeEvent()` all ask it. **Turning Browse lanes on later is one line**: add `case 1:` beside `case 2:`/`case 3:`.
The keyboard side is pre-wired (`setSearchOpen()` hides the lanes while the search keyboard is up on a compact Browse,
so it takes their room and never floats over them). What that switch would still need: r4-browse's "at least 5 list
rows at 800x480" budget (the zone is 166 px), and flipping r4-browse's tests that expect no lanes on Browse.

### The compact layout (800x480)
```
| rail 44                                                                  |
|1| lane, deck 1  (782x66, GL)                                    |      |  44..109
|2| lane, deck 2  (782x66, GL)                                    |      | 112..177
| [1] ▶ 128.0 ■□□□ MASTER ▌3.4 −3:25 | [2] ❚❚ 126.0 □□■□ ▌7.1 −5:02     | 178..209  DECKS row, 32 px
| page: Sampler 4x4 pads + side column, or the Lab FX instrument (800x270) | 210..479
```
- **Lane height**: `compactLaneHeight(pageArea) = clamp((pageArea − 270 − 32 − 2) / 2, 56, 96)`: **66 px** at 800x480,
  **96 px** at 1024x600 (page 330). 66 px is 12.6 mm on the 7" 133 ppi panel: full-height beat-grid lines, the
  waveform's shape and the live-red playhead read at arm's length. Decks lanes stay 95 px.
- The 270 px page floor is what the two pages need: Lab FX keeps two columns of 44 px+ effect tiles (it needs 256)
  and Sampler pads stay ≥ 56 px (they are 62).
- **Lanes keep the Decks arrangement**: stacked stays stacked, side by side stays side by side (one row of two
  381x134 lanes). The lane width never changes between pages.
- **Lanes are display only on compact pages**: `WDeckLab::eventFilter` drops press, double-click and wheel on a lane
  viewer when the page is not Decks (releases always pass, so a scratch begun on Decks still ends). Hands are on pads
  and FX keys there; scratching stays a Decks gesture. The 32 px row is also a buffer: a tap a little high lands on the
  display-only row, never on a lane.

### The DECKS row (`DeckLabLaneDecks`) replaces Sampler's DECKS strip
Decision: **one 32 px row under the lanes**, shared by Sampler and Lab FX, instead of the 44 px strip or two per-lane
headers. Two 30 px lane headers (round 3's one-line readouts) would cost 60 px and leave 36 px lanes; one row keeps
66 px lanes and 62 px pads, and it is where the RX3 puts its deck panels (left deck left, right deck right, under the
stacked waveforms). It keeps exactly what a lane cannot show:
- badge (filled in the deck colour, outlined when empty), ▶ green / ❚❚ muted;
- BPM (one decimal in wide cells, rounded in narrow ones), the **four-beat lamp** in the deck colour;
- **MASTER** (a filled chip, "M" in narrow cells);
- the rekordbox **phrase colour and "bars.beats" countdown** (the scrolling lane has no phrase colours);
- **time left**, live red in the end warning and blinking into a red box like the Decks clock;
- a 2 px **position line** in the deck colour (the slim overview);
- a deck-colour bar along the top of each cell whose deck has a lane, matching the lane's edge.

It drops the title (the lane's numbered colour edge says which deck) and the strip's "DECKS ›" tap target (a 32 px
row cannot be a 44 px target; the rail's Decks tab is the way back). It is display only, repaints only cells whose
whole-unit values changed (an unchanged tick costs comparisons), uses the existing 100 ms tick and does nothing while
hidden. Sampler's own strip stays in the widget (`DeckLabSampler::setDecksStripShown`, shown by default, so the
page still works on its own and r4-sampler's tests stand); the surface hides it when lanes show.

### Four decks: the XDJ-AZ rule
`decklab::lanes::choose()`:
- **Two scrolling lanes** go to the most relevant decks: **on air first**, then the controller's active layers (the
  Decks page's two-deck view), then loaded decks, then the lower number. Screen order follows the controller: the left
  side's deck (1 or 3) on top, the right side's (2 or 4) below.
- **The DECKS row names every deck**: all four (200 px cells, in deck order) in the four-deck layout; in the two-deck
  layout the two lane decks plus any other deck that is on air.
- So **a deck on air is never out of sight** (a lane or a row cell); an exhaustive test checks every combination of
  loaded/playing decks, layers and layouts.
- Why painted cells and not real `WOverview` strips for the others: the overviews belong to the Decks cards (one
  parent for life, with Decks' needle search on them), and four overview widgets repainting would cost more GUI time
  than a cell that repaints about once a second. The position line plus the phrase countdown give "where in the
  track" for every deck. Follow-up: a 6 px mini overview in each cell from the cached summary image.
- Lanes move only when the choice changes (play, load, layer), never on a tick; a lane keeps its parent and only
  changes grid cell (`updateLayers()` signature includes the choice).

### Sampler under the lanes (800x270)
- Pads **169x62** (were 169x94 under the 44 px strip). Under 84 px a pad keeps its title on the number's line and
  moves the BPM beside the time; the drain, lamps, gain bar, "⋯" and time are unchanged.
- **Edit sheet in two rows** so it fits two rows of pads: `STOP · PAD n title · GAIN − 0 dB + · DONE` /
  `MODE ONE-SHOT LOOP GATE · Knob: gain · Back: close · CLEAR`. STOP and CLEAR stay at opposite corners; it still
  docks over the half away from the pad, inside the page, under the lanes. Keys 44 px+.
- Fixed on the way (visible in the before renders): **pad rows 2-4 touched** (0 px gaps). The hidden sheet spanned
  grid rows 2-3 and made `QGridLayout` drop the spacing there; the sheet now joins the grid only while open. The
  sheet title was elided before the number's width was known and ran under GAIN; it is now laid out first.

### Lab FX under the lanes (800x270)
- r4-labfx's instrument layout as checked at 800x296, at 800x270: effect tiles in two columns (3 rows of 47 px),
  the X-PAD 524x106 (1024x600: 748x166), beat cells, ON, targets, RELEASE / ECHO OUT / CUT unchanged.
- **FX BANDS caption** steps aside when it would push the three lamps under 44 px (each lamp still says GETS FX/DRY).
- **Footer readout: kept, but it only names the target** (badge, ▶/❚❚, title, or the SMP link). Its beat lamps and
  clock are on the DECKS row right above, in the same deck colour, so they are dropped (and no longer repaint every
  second). The readout costs no height: it shares the footer row with RELEASE / ECHO OUT / CUT, which need it.
  `DeckLabLabFx::setLanesShown(bool)`.

## Pi 4 budget (GPU and GUI thread)
- **Per frame**: a compact page renders two lanes (Decks renders two, or four in the four-deck layout), smaller ones,
  through the unchanged render gate and `WaveformColumnCache`; lanes not chosen are hidden (unmapped, skipped by the
  gate). No new timer: the DECKS row runs on the existing 100 ms `tickDecks`, repaints a changed cell only, and is
  hidden (no work) on Decks, Browse and Settings. Net GUI saving on Sampler: `tickSampler()` no longer computes the
  strip's deck values nor a second `refreshDeckClock()` per deck (r4-sampler needed that because Decks' tick skipped
  clocks without lanes). Lab FX's footer no longer repaints for clock seconds.
- **Page switches, GL resources** (reasoned; GL cannot run offscreen):
  - Sampler ↔ Lab FX: identical lane geometry, **no resize event reaches a lane** (tested), so nothing is touched.
  - Decks ↔ Sampler/Lab FX: **height only** (95 → 66 px). The width (782, or 381 side by side) is the same, so
    `WaveformColumnCache::prepare()` keeps every chunk (it rebuilds on waveform, zoom/rate, DPR, width or stereo-split
    changes, not height: height is a shader uniform). What a height change costs, once per visible lane: an X
    ConfigureWindow, Mesa reallocating the window's back buffers, `WaveformWidget::resizeGL` (a matrix uniform per
    node) and `WaveformRenderMarkBase::onResize` re-rendering the cue mark images, then one forced full render.
  - The alternative, one lane height everywhere, needs 95 px compact lanes (zone 224, page 212: 48 px pads and Lab FX
    tiles under 44 px), so the height change at Decks ↔ compact is the deliberate trade. Round 3 paid the same class of
    cost (95 ↔ 39 px lanes off Decks), as does a deck-layout change today.
- `DECKLAB_RENDER_GATE` and `DECKLAB_WAVEFORM_CACHE` behaviour unchanged.

## Nothing floats over the lanes
- Sampler edit sheet: in the pad grid, inside the page, under the lanes (harness and unit test).
- Lab FX: every surface is a child of the page under the lanes (harness check: no page widget crosses the zone).
- Rail notices: inside the rail (y 0-43); the lanes start at 44. The status sheet, Settings' picker and confirmations
  already suppress the waveforms (`decklab/decklaboverlay.h`). Browse's toast and keyboard: Browse has no lanes.
- `DeckLabOverlayTest.CompactLanesOnSamplerAndLabFxHaveNothingOverThem` runs the structural overlay check on both pages
  with a native waveform window in the lane.

## Renders (local, not in the repo)
`local/wt/audit/screens/r4-waveforms/`, offscreen harness, real `decklabsampler.cpp`, `decklablabfx.cpp`,
`decklabdeckwidgets.cpp`, `decklablanes.cpp`, generated decks. The rail is a drawn mock; **the GL lanes are painted
stand-ins** at the real geometry and real edge style (synthetic waveform, beat grid with brighter bars, live-red
playhead), because GL cannot render offscreen.
- before (round4 code): `before-{sampler,labfx}-{800x480,1024x600}-{two,four}.png`, `before-labfx-800x480-two-echo-held.png`, `before-report.txt`.
- after: the same eight, plus `after-labfx-800x480-two-echo-held.png`, `after-sampler-800x480-edit-sheet-top-pad.png`,
  `after-sampler-800x480-edit-sheet-bottom-pad.png`, `after-{sampler,labfx}-800x480-two-side-by-side.png`,
  `after-labfx-800x480-four-smp.png`, `after-report.txt`.

## Verified locally (WSL, g++ 13, Qt 6.4, offscreen)
- **Syntax** (`-fsyntax-only -Wall -Wextra`, moc'd headers, now with `-DQT_USE_QSTRINGBUILDER -DQT_TABLET_SUPPORT`):
  `wdecklab.cpp`, `decklablanes.cpp`, `decklabsampler.cpp`, `decklablabfx.cpp`, `decklabsmoke.cpp`, `decklabtest.cpp`
  (its usual 9 `-Wdangling-else`), `decklablanestest.cpp`, `decklabsamplertest.cpp`, `decklablabfxtest.cpp`: no errors,
  no new warnings. Script: `~/r4wave-syntax.sh`.
- **New suite `DeckLabLanesTest`, 8/8 pass** (standalone gtest against the real code): one-place presentation,
  geometry (66/166/270, 96/226), lane choice (on air, layers, side order, third deck on air), the exhaustive "on air is
  never out of sight", the row's repaint economy (unchanged state, off-row deck, blink only on end-warning cells),
  lane bars and live red only at the end, Sampler at 800x270 (pads ≥ 56, 4 px row gaps, sheet fits with 44 px keys,
  STOP/CLEAR corners, elided title, hidden strip untouched by the tick), Lab FX at 800x270 (every key 44x44, inside,
  4 px apart; X-PAD ≥ 96 px; the compact footer paints no clock).
- **r4-sampler's `DeckLabSamplerTest` 11/11 and r4-labfx's `DeckLabLabFxTest` 11/11 still pass** with these changes.
- **Harness: 0 failures** (after): keys ≥ 44x44, inside the page, 4 px apart, no page widget over the lane zone,
  pads ≥ 56, the sheet docked under the lanes, at 800x480/1024x600, two/four decks, stacked/side by side.
  The before report shows the r4-sampler row-gap bug (FAIL "4 px between keys") that this branch fixes.
- `build_pi.py --check-inputs`: PASS. `test_build_pi.py`: 7 OK.
- **Not run**: the full app build and `NativeDeckLabTest` (the coordinator runs `local-build/decklab-local.sh` on this
  branch; I did not run it, as asked). No WSL packages installed.

## Needs CI / the local full build, and the Pi
- **`NativeDeckLabTest`**, new: `WaveformLanesShowOnDecksSamplerAndLabFxOnly` (pages 0/2/3 visible, 1/4 hidden;
  Decks 782x95, compact 782x66 at y 44 and 112, row at 178, page 800x270 at 210; no resize between Sampler and Lab FX;
  side by side kept, 134 px; 1024x600 gives 96 px lanes and a 330 px page), `FourDeckCompactLanesFollowTheLayersAndNameEveryDeck`,
  `CompactLanesTakeNoTouch`. Updated on purpose: `LongMetadataCannotHideTempoOrExpandSurface` (Lab FX shows the DECKS
  row; it also expected one-line readouts on Browse, which the round-4 merge left stale), and
  `SamplerIsAFourByFourInstrumentWithTheDecksInView` (pads ≥ 60 px, rows 4 px apart, the strip hidden, the DECKS row
  under the lanes follows deck 1). r4-browse's "no lanes on Browse" expectations are untouched.
- **`DeckLabOverlayTest`**, new: `CompactLanesOnSamplerAndLabFxHaveNothingOverThem`.
- **`DeckLabLanesTest.*`** added to the lock's `test_suites`.
- **Smoke**: `app-screen-02` (Sampler), `-03` and `-05..09` (Lab FX) and `app-sampler-01/02` now show the lanes and the
  DECKS row over the pages. **Smoke screen count: +0 screens** (no capture added or removed).
- **Pi**:
  - 66 px lanes at arm's length in a dark booth (beat grid, playhead);
  - the Decks ↔ Sampler switch: look for a stale or black first frame after the height change, time it with
    `DECKLAB_PERF_LOG=1`;
  - Sampler ↔ Lab FX should show no resize in the log;
  - GPU frame time with two compact lanes on Lab FX while the X-PAD is held;
  - the DECKS row beat lamps against the music at 10 Hz;
  - a stray touch on a compact lane does nothing.

## Merge notes
- **New files**: `src/widget/decklablanes.{h,cpp}` (rule, geometry, lane choice, `DeckLabLaneDecks`; no `Q_OBJECT`),
  `src/test/decklablanestest.cpp`, this note.
- **CMake**: `decklablanes.cpp` after `decklablabfx.cpp`; `decklablanestest.cpp` after `decklablabfxtest.cpp`.
- **`wdecklab.cpp`** (all marked `r4-waveforms`):
  - include;
  - the constructor adds `m_laneDecks` at `m_waveLayout` row 2;
  - `buildSampler()` and `buildEffects()`: one line each;
  - `tickSampler()`: its loop runs only while the strip shows, plus a comment;
  - **`selectPage()`**: the lane placement block now asks `presentationFor()`; it replaces round4's `page == 0` block;
  - `refreshDeckClock()` and `applyDeck()`: one line each;
  - `deckShown()` and `tickDecks()`: one line each;
  - `eventFilter()`: a block at the top (display-only compact lanes);
  - `setSearchOpen()`: three lines (pre-wired keyboard for a compact Browse);
  - `updateLayers()`: the lane choice and signature, the separated rule, `order`, the minis always hidden with the edge
    number always shown, and the row's choice and visibility;
  - `compactLaneZoneHeight()` and `resizeEvent()` after `updateLayers()`.
  - Not touched: `kStyle`, `kRailStyle`, Browse, Settings, the Decks builders.
- **`wdecklab.h`**: forward declaration, `resizeEvent`, `compactLaneZoneHeight()`, `m_laneDecks`.
- **`decklabsampler.{h,cpp}`** (r4-sampler's): `setDecksStripShown()`/`decksStripShown()`, `kCompactPadHeight` pad
  layout, the two-row sheet, the sheet in the grid only while open, the title elision order.
- **`decklablabfx.{h,cpp}`** (r4-labfx's): `setLanesShown()`, `DeckReadout::State::compact`, the band caption rule.
- **`decklabtest.cpp`**: include, the two updated tests, three new `NativeDeckLabTest` tests before
  `IndependentRmxDoesNotBorrowControllerChains`, one new `DeckLabOverlayTest` test before
  `EveryOverlayOverTheWaveformsHidesThemOrIsNativeAndRaised`.
- **Lock**: hashes refreshed for `CMakeLists.txt`, `decklabtest.cpp`, `wdecklab.cpp`, `wdecklab.h`; `DeckLabLanesTest.*`
  added. `decklabsmoke.cpp` and `build_pi.py` unchanged.

## Follow-ups
- **Browse lanes**, if the owner wants them: the one line in `presentationFor()`, then re-budget Browse's list rows
  (r4-browse's ≥ 5 rows at 800x480) and flip r4-browse's no-lanes test expectations.
- A 6 px mini overview (the cached summary image) inside each DECKS row cell, for the decks without a lane.
- r4-labfx's follow-up, now easy: a white "FX" lamp in the DECKS row cell of the deck Lab FX is wet on.
- `DeckLabMiniStrip` (round 3's one-line lane readout) is no longer shown anywhere; it can be removed once nothing
  (tests, smoke) looks for `decklab-mini-N`.
- If the 66 px lanes read small on the Pi, `kPageMinimum` is the one knob: 260 gives 71 px lanes and 60 px pads,
  with Lab FX's tiles and band lamps at exactly 44 px (no margin left).
