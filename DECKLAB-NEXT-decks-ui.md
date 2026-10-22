# Deck Lab next: Decks UI (decks-ui workstream)

Branch `decklab/decks-ui`. Target: a club-standard player screen on the 800x480 touch panel, smooth on a Pi 4.
Wider panels (1024x600, 1280x800) scale by layout stretch.

## Part A: refresh performance (commit "Refresh the Decks page from coalesced state changes instead of polling")

Before: a 100 ms timer ran `refreshDeck()` for all four decks on every page. Each pass rebuilt about 300
`group + item` control keys, made about 20 recursive `findChild` lookups, re-applied a style sheet (Qt repolish), re-set
word wrap, re-parsed an HTML label, and every `play`/`track_loaded` change ran a full relayout. VU meters were
style-sheet progress bars driven by engine-rate queued signals.

Now:

| Hot spot | Fix |
|---|---|
| Control lookups | `control()` uses a two-level `group -> item` cache (no key concatenation). Every per-deck control is resolved once into `DeckProxies` (`m_deck[4]`); `channel()` returns shared preformatted strings. Helper signatures are unchanged. |
| Polling | State controls (`bpm`, `local_bpm`, `rate_ratio`, `rateRange`, `sync_*`, `keylock`, `quantize`, `pfl`, `cue_point`, `loop_*`, `slip_enabled`, `duration`, hot cues x 24) mark a deck dirty. One queued `flushDirtyDecks()` refreshes each dirty deck once. `play`, `sync_leader` and `visual_key` mark all decks dirty, because the key hint compares decks. Title, artist, key and grid edits on the loaded track also mark the deck dirty. |
| Tick | `tickDecks()` only advances decks whose waveform is on screen and that play, or whose position moved while paused (cue jump, needle search, scratch). Nothing per deck runs on Sampler or Settings. |
| `findChild` | Transport and hot-cue buttons are cached per prefix. Each group keeps the last shown state and returns early when it is unchanged. |
| Unchanged writes | `setTextIf`, `setVisibleIf`, `setEnabledIf`, `setCheckedIf`, `setStyleSheetIf`, `setStylePropertyIf` (repolish only on change), `setFixedHeightIf`/`WidthIf`, accessible-name and tool-tip guards. `updateStatus()` is guarded too, because it runs on every waveform measurement. |
| Relayout on Play | `updateLayers()` computes a signature (deck count, waveform arrangement, page, displayed layers, attached widgets). It only relayouts when the signature changes. Otherwise it marks decks dirty. |
| VU meters | `DeckLabLevelMeter` is a painted segment meter, sampled every 40 ms. It repaints only when a lit segment or the clip lamp changes. There is no engine-rate signal traffic. |
| Controller FX state | Event driven: unit enable/mix/routing and slot enabled/loaded/loaded_effect. It connects lazily once the units exist. |

In Part B, painted readouts replaced the text labels. A playing deck repaints only the clock, beat-meter or tempo
rectangle of its readout. The test `IdleDecksRefreshWithoutStyleOrLayoutWork` proves this: it sees no `StyleChange`,
`LayoutRequest` or `Polish` event on any widget while idle, during dirty-but-unchanged control traffic, or during about
1.1 s of playback.

The Part A commit on its own keeps the old label UI. Two of its old-label assertions (headphone-mix and linked-zoom
captions) fail at that commit, because those captions only refreshed with controller FX units present. Part B deletes
those labels and their assertions. Merge the branch as a whole.

## Part B: Decks screen redesign

Layout at 800x480 (two decks, stacked):

```
rail 44   [mark] Decks | Browse | Sampler | Lab FX | Settings | USB | FX 1 2 3 4 | status | L/R meter
waves 214 [1] full-width waveform 782x106   (18 px deck tag: deck-color bar + number)
          [2] full-width waveform
deck 222  readout 86  title row / BPM tempo key clock / lamps + beat meter       x2 columns of 400
          overview 40 (needle search)
          hot cues A-H (44 px, 4 px gaps)
          ½ | LOOP n | ×2 | QNT | KEY LOCK | BEAT SYNC | MORE  (44 px, 4 px gaps)
```

Side by side gives two 381x214 lanes. Four decks give four 782x56 lanes stacked (2x2 side by side), with four compact
200x116 readouts, overviews and Cue/Play/Sync/More.

Design decisions:
- **Full-width waveforms.** The 224 px of side columns are gone. The waveforms are GL window containers, so overlays
  are impossible. Deck identity is an 18 px tag beside each lane: a deck-color bar with the deck number. Lanes and cards
  keep one parent for life: a layout change only moves them between grid cells, and never reparents a GL window.
- **Painted readout (`DeckLabDeckStrip`).** One tap target per deck: it opens full track information, or Browse for an
  empty deck. The existing stale-gesture guards are unchanged.
  - Title row: deck badge, then the state lamp (green PLAY triangle, PAUSE bars, orange CUE when paused on the main
    cue, dim stop square when empty), then title and muted artist on one elided line.
  - Value row: BPM at 30 px with smaller decimals; tempo % with the `rateRange` (±8%) and a green tempo-center lamp;
    key at 22 px in the configured library key colors (lightened for black) with a green check or grey cross against
    the master deck; remaining time at 30 px. Remaining time is real time at the current tempo. It flashes live red
    inside `[Waveform] EndOfTrackWarningTime` while playing and not looping, the same rule as Mixxx's `end_of_track`.
    Digits use fixed-width cells, so a running clock never shifts sideways. Big numbers shrink to fit instead of
    clipping.
  - Lamp row: MASTER (filled in deck color), SYNC (outlined in deck color) and HP (white), plus SLIP when on. The beat
    meter sits on the right.
  - A painted channel meter runs along the left edge.
  - Compact four-deck readouts show the same data. Lit lamps appear as space allows, in this order: MASTER, SYNC, HP,
    LOOP n, KEY, Q, SLIP.
  - Browse/FX get a one-line `DeckLabMiniStrip` showing badge, state, title, key, BPM and time.
- **Beat meter.** It shows `beat index mod 4`, where the index counts beats since the grid's first marker, plus the
  in-beat phase. Both come from the grid at the current position (the same quantity as `beat_distance`). Mixxx stores
  no downbeat, so this is a beat counter and never claims bars or phrases. The active segment uses the deck color,
  dimmer while paused, and the meter is dark without a grid. The math is `deckLabBeatPhase()` in
  `decklabdeckwidgets.h`. It uses `util_isfinite`, because the build uses `-ffast-math`.
- **Harmonic hint.** It compares against the sync leader on another deck. If there is none, it compares against another
  playing deck. The master itself shows no hint. It uses `KeyUtils::getCompatibleKeys`. Key wording is "key" or
  "Lancelot" only.
- **Needle search.** `DeckLabNeedleSearch` consumes every pointer event on the overview, so WOverview never sees drags,
  drops or menus. A touch or drag seeks `playposition` only while the deck is loaded and paused and Decks is shown. It
  is locked while the deck plays.
- **Controls.** Toggles light up instead of showing words: on means a raised fill with white text and a white outline.
  Play adds a green outline. One-shot BEAT SYNC flashes white. Every target is ≥ 44 px with 4 px gaps, and widths
  stretch on wider screens. Each button carries its own style sheet with paired min/max heights, so polish cannot shrink
  the 44 px border box (including in popups). Linked zoom (Zoom out / Zoom in) moved into the More panels.
- **Rail (44 px).**
  - The brand mark is graphite and white bars split by the red playhead.
  - Tabs show the active page in white with a 3 px white underline. The FX tab is now "Lab FX" and raises while Lab FX
    is wet or tailing.
  - USB and the controller FX unit lamps (`decklab-performance-board-fx`) follow. The lamps open the existing read-only
    details, so no unit names are truncated.
  - The status lamp shows `AUDIO OFF` (red) when output is not configured or not running, `OVERLOAD` (red) for 10 s
    after the overload counter rises, `REC` / `REC II`, and `SYSTEM` when tools are offline. It is hidden when all is
    well, and the tool tip keeps the full text.
  - The main L/R meter sits at the far right.
- **Brand tokens (provisional).**
  - Deck colors: 1 cyan `#35C8F0`, 2 amber `#F5A524`, 3 lime `#B5E61D`, 4 violet `#C77DFF`.
  - Neutrals: ground `#0D0F12`, panel `#16191E`, raise `#20242B`, line `#2C313A`, muted `#9AA3AF`, text `#F2F4F7`.
  - Live red `#FF3D2E` is used only for the waveform and overview playheads, the end-of-track warning, clip, REC and
    audio alarms.
  - Play green (`#2FD27A`) and cue orange (`#FF7A1A`) are kept from the brief as transport-state colors.
- **Type.** The bundled Open Sans is kept this round. Barlow / Barlow Semi Condensed would mean adding new font files to
  `res/fonts`; left for integration (see open issues). Sizes: secondary text ≥ 12 px, mid-mix readouts ≥ 14 px, BPM and
  time 30 px.
- **Layer follow.** In two-deck view, a deliberate controller touch on a deck brings it to its side and keeps
  `[DeckLabLeft/Right],current` in step. Deliberate touches are:
  - `play` 0→1
  - `cue_default` press
  - `scratch2_enable` press
  - `jog` / `wheel` movement
  - a `rate` change on a deck that is not a Sync follower
  - `volume`

  Ignored: falling `play` (end of track), Sync follower rate, the first 1.2 s after start-up (mapping initialisation),
  600 ms after a track load, and any touch while the shown deck on that side was touched in the last 400 ms (mappings
  that mirror both layers). A held touch Cue on that side also blocks it. Four-deck view does not follow.
- **Skin.**
  - Playheads are live red, `EndOfTrackColor` is live red, and the background is the ground color.
  - The simple-waveform `SignalColor` uses the deck color. The RGB band colors are unchanged.
  - Overview label size rises from 11 to 12 px.
  - The license is declared GPL-3.0-or-later, matching the bundled LICENSE text, with Pioneered credited in the
    manifest author.
- **Distribution rules.** All user strings go through `tr()`, or `QCoreApplication::translate("DeckLabDeckStrip", …)`
  in the painter. No brand names appear as labels. Fixtures and screenshots use generated tracks only ("Generated Tone
  128" / "Deck Lab Fixture").

## Verification

Tests in `src/test/decklabtest.cpp` (all `NativeDeckLabTest`; every required name is kept):
- `IdleDecksRefreshWithoutStyleOrLayoutWork`: no style, layout or polish events while idle, for dirty-but-unchanged
  traffic, or while playing.
- `NeedleSearchSeeksOnlyWhilePaused`: tap and drag seek while paused; locked while playing; hidden off Decks.
- `BeatPhaseMeterCountsGridBeatsInFours`: pure math (wrap, negative index, carry, NaN) plus a generated 128 BPM grid.
- `EndOfTrackWarningFlashesTheRemainingTime`: no warning while paused or looping; while playing, both flash phases
  appear.
- `MasterSyncAndKeyLampsFollowRealState`: master/sync lamps, tempo-center lamp, range text, key match/clash.
- `LayerFollowsDeliberateControllerTouchesWithoutFlapping`: a fader touch switches the layer, mirrored writes do not
  flap, end-of-track stops and Sync-follower rate are ignored, jog switches, and four-deck view does not follow.
- `DecksLayoutsScaleToLargerTouchscreens`: 1024x600, 1280x800 and 800x480 in two- and four-deck views. No clipped
  buttons, all targets ≥ 44 px, readouts fit.
- The existing geometry, gesture-guard, hot-cue, beatgrid, FX and transport tests were moved to the new surfaces.

CI (`gh run view <id> --repo brakoish/mixxx`):
- 39797321948, Part A only: built. 258/260 tests passed; the two old-label assertions noted above failed.
- 39802760470: failed the ancestry check before building, because the workflow fetched only 50 commits. Fixed by
  `fetch-depth: 0`.
- 39803064323, Part B snapshot pushed as `decklab/decks-ui-ci1`: built. Two test fixes followed: the NaN check under
  fast-math, and a beat beyond the generated track.
- See the final section for the last runs.

Screenshots (artifact `decklab-mixxx-aarch64-<sha>/`; generated tracks only):
- Before (Part A artifact, old layout): `decks-fixture-playing.png`, `decks-fixture-four.png`, `screen-0.png`.
- After, test fixtures:
  - `decks-fixture-playing.png`, `decks-fixture-four.png`, `decks-fixture-four-modes.png`
  - `decks-end-warning.png`, `decks-master-sync.png`, `decks-phase-meter.png`
  - `decks-two-1024x600.png`, `decks-four-1024x600.png`
  - `hotcue-badges-stacked.png`, `hotcue-badges-side.png`, `screen-0.png` … `screen-4.png`
- After, real application smoke check (empty decks, real waveform viewers): `app-screen-17.png` (four decks),
  `app-screen-18.png` (auto), `app-screen-19.png` / `app-screen-20.png` (side by side), `app-decks-1024x600.png`.

## Open issues
- Barlow / Barlow Semi Condensed are not bundled yet. Add the OFL TTFs under `res/fonts` and change the readout's font
  family in `DeckLabStripRenderer::setFamily()`. Digits already use fixed cells.
- Cover artwork is not shown. `CoverArtCache::requestUncachedCover(strip, track, 44)` per load would be cheap, but the
  400 px readout has no room without squeezing the title.
- The RGB waveform band colors are unchanged. Only the simple/filtered renderers take the deck tint. A deck-tinted
  RGB scheme needs review on real tracks on the Pi.
- Strings outside my code still say "Camelot" (the Settings key-format combo) and "RMX" (FX page). They are left for
  the integration rename pass. The tests no longer depend on the word "Camelot".
- `DECKLAB.md` still lists the skin as GPL-2.0-or-later. I do not own that file.
- `decklab/decks-ui-ci1` is a one-commit CI snapshot, pushed before the fetch-depth fix. It can be deleted.

## Notes for merging
- **Header.** `wdecklab.h` drops the old per-deck label members (`m_titles`, `m_numbers`, `m_waveContexts`,
  `m_pair*` …). It adds `DeckProxies`/transport/hot-cue state structs, the card/strip/lane members and new private
  functions. `m_status` is now a `DeckLabChipButton*`. `m_controls` changed type (two-level hash). It includes
  `widget/decklabdeckwidgets.h` and `track/track_decl.h`.
- **New files.** `src/widget/decklabdeckwidgets.{h,cpp}`, added to `CMakeLists.txt` after `decklabsmoke.cpp`.
- **Constructor.** The rail is a `QWidget` (`decklab-rail`) with its own style sheet. The USB button lines are unchanged
  apart from height 44. The tools-online handshake lines (constructor `tools_ready` watcher and the first `tickEffects`
  lines) are byte-identical.
- **`tickEffects`.** The deck line is now `tickDecks()`. At the coordinator's request, the Lab FX release threshold
  changed from 550 to 1500 ms. Everything else is untouched.
- **`selectPage`.** Only rail heights (44) changed; the waveform placement and Browse lines are intact.
- **Removed from Decks.** The controller FX summary label, RMX summary button, headphone-mix readout, LINKED ZOOM label
  and zoom buttons are gone. Their roles moved to the rail lamps, the Lab FX tab and the More panels.
- **`decklabsmoke.cpp`.** Adds one extra capture (`app-decks-1024x600.png`) inside step 18. The screen count stays 21.
- **`.github/workflows/decklab-pi.yml`.** `fetch-depth: 0`, the same text as perf-engine's commit `375429e8b1`.
- **`tools/decklab/pi-build-lock.json`.** Hashes are refreshed per commit. Expect conflicts; regenerate after merging.
- **Deck state refresh is queued.** A test that sets a control and checks the UI after a single
  `QCoreApplication::processEvents()` needs a second pass (`settle()` in the test file).
