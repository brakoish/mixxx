# Deck Lab next: performance quick wins (qw-perf)

Branch `decklab/qw-perf`, based on `origin/decklab/round3` (3d16a26037). Source: the UI/UX audit of the
performance surfaces (`audit/performance.md`, section 5 "Quick wins" and the findings it cites). Scope: the Decks
deck widgets, stem pads, Lab FX and the Sampler. No audio-engine or effect-DSP changes.

| Commit | Subject |
|---|---|
| 0af8aa9734 | Make the performance surfaces fire on touch and light as filled lamps |
| (last) | This note |

## What changed, by audit finding

### Touch-down triggers (PF-S03, PF-D32, PF-D55, PF-D48)

| Control | Before | Now |
|---|---|---|
| Sampler pads (Sampler page and Lab FX "Samples") | play on release | a **loaded** pad plays (`play_stutter`) on touch-down. An **empty** pad still opens Browse on release, so the page never changes under a finger that is still down. |
| Hot cues A-H (side rows and four-deck MORE) | jump/set on release | jump (`hotcue_N_goto`) or set (`hotcue_N_set`) on touch-down. The release only refreshes the pad. |
| Stem pads | toggle on release | the mute flips on touch-down. Hold (300 ms) still solos; the release restores the state from **before the touch** (the touch-down toggle included). Double-tap (second touch within 400 ms) still unmutes every stem, and holding that second touch does not solo. |
| Four-deck Play | release | touch-down. Two-deck MORE › Play, the toggles (LOOP, QNT, KEY LOCK), loop sizes and the SYNC tap stay on release. |

Stale-tap guards now run at touch-down:

- **Hot cues.** The pads must show the deck being touched, a loaded track, and the same track object the readout was
  refreshed for (`m_tracks`). The touched cue must be set or empty both on screen and in the engine. Otherwise the
  pads refresh and the touch is ignored.
- **Stem pads.** A hold in another context (page, layer or layout change, a track load) never solos. A cancel never
  toggles back.
- **Destructive actions stay on release.** CLEAR (sampler hold menu) is a menu action, and sampler eject and loading
  remain release or menu driven.

Trade-off, by design: a press-fired action cannot be cancelled by sliding off. Holding a playing stem to solo it
mutes that stem for the 300 ms hold time before the solo starts (the audit's PF-D57 SOLO modifier would remove this).

### BEAT SYNC (PF-D43)

- Label **SYNC** (two-deck rows and four-deck cards).
- **Tap** = the same one-shot `beatsync_tempo` + `beatsync_phase` match as before, on release. The button shows its
  outlined acknowledgment for 1 s (was 600 ms) and the readout's SYNC lamp reads **MATCHED** for that second
  (`DeckLabStripState::matched`).
- **Hold 500 ms** = toggle the CDJ-style lock (`[ChannelN],sync_enabled`). It fires while the finger is still down,
  with the same guards as a tap (same deck, same interaction epoch, loaded, still pressed). The release then does not
  also match once. The button is **lit filled** while locked (`lit` property).
- Accessible description: "Tap to match tempo and beat once. Hold to turn sync lock on or off."

### On means filled (PF-D40-42, PF-X08, PF-S04)

One lamp language. A checked or lit control is a `#F2F4F7` fill with `#0D0F12` text. This covers LOOP, QNT,
KEY LOCK, SYNC lock, the beatgrid LOCK, the Lab FX band buttons, the "SAMPLER FX" toggle, and playing sampler pads.
Play lights **green** (`#2FD27A` fill). The one-shot SYNC acknowledgment stays an outline, so it never reads as the
lock.

Play/pause is a **fixed painted ▶❚❚ glyph** (`DeckLabPlayButton`). Barlow has no media glyphs. Its text stays
`PLAY` for accessibility fallback, and the spoken name is "Deck N play/pause, playing/paused".

### Hot cues while playing (PF-D33, PF-D34)

Every pad is enabled whenever a track is loaded. An empty pad sets a cue on touch-down while the deck plays, and
Mixxx snaps it to the beat when QNT is on. An empty pad is a dim letter in a 1 px dashed outline (no "+"), and is
disabled only without a track. Tooltip: "Set cue A here".

### XY pad, hold effects and rolls (PF-X15, PF-X11, PF-X12)

- **XY pad: no time limit.** The effect follows the finger until it lifts.
- **Hold effects and rolls (Lab FX and sampler rolls):** the 4 s cut-out is gone. Only an **8-bar backstop** at the
  deck's (or sample's) tempo remains (`holdBackstopMs`: 32 beats; the BPM is clamped to 60-240, so 8-32 s; 15 s at
  128 BPM).
- **Lost-touch safety without a wall clock.** Only one contact drives the pointer, so `WDeckLab::eventFilter` treats a
  left press or release that reaches any other widget during a hold as the end of the holding contact. It then calls
  `endHolds()`: rolls stop and a hold effect or the XY pad returns dry. Latched effects keep playing.
- **Unchanged:** TouchCancel, application deactivate, hide and page change still release at once, and the DSP's 1.5 s
  heartbeat deadman still covers a frozen GUI.
- **Bug fixed on the way:** the end of a RELEASE tail used to call `releaseEffects()`, which also cut a roll that was
  being held at that moment. The tail-end path now keeps held rolls.

### Fonts and brand tokens (PF-X01, PF-S02)

- **New `kPerformStyle`**, set on the Lab FX page (`decklab-labfx`) and the Sampler page (`decklab-sampler`). A
  page's own sheet beats the surface sheet (`kStyle`), so these pages get Barlow 500 (600 on buttons) on every child,
  plus brand neutrals for buttons, the sub-tabs, dropdowns and their popups, sliders, and the Lab FX state lamp.
  `kStyle` itself is untouched (qw-system owns it).
- **New `kMenuStyle`** for the MORE menus and the sampler hold menu: panel `#16191E`, line `#2C313A`, raise `#20242B`.
- **Painted widgets:** `DeckLabXY` and `DeckLabPad` use brand colours and an explicit Barlow face. The XY pad has a
  crisp line-colour grid, 14 px axis labels LPF / HPF / DEPTH, "Hold to sweep", and a **white** touch ring with
  crosshairs (it was live red).
- None of my sheets name Open Sans.

### Copy (audit section 4, for these areas)

| Where | Old | New |
|---|---|---|
| Lab FX header | Release dry / Mute tail | **RELEASE** / **ECHO OUT** (accessible: "the tail rings out" / "the track drops for one bar under the tail") |
| Lab FX status | Lab FX ready · Dry / Lab FX active / Releasing tail | lamp **DRY** (outline) / **ON** (filled) / **TAIL** (white outline) |
| Lab FX status | Load a track / arm sample FX; Lab FX unavailable | "Load a track on deck N"; "Lab FX effect didn't load. Restart Deck Lab." |
| Lab FX tabs | Levers / Isolate / Perform / XY / Samples | Bands / Sweep / Hold / XY / Samples |
| Bands mode | Lever · Echo / Reverb / Solo | ECHO / REVERB / BAND SOLO |
| Sweep mode | "Echo · left cuts / right sends" … | "ECHO ‹ CUT · SEND ›", "FILTER ‹ LPF · HPF ›", "DUCK ‹ ON-BEAT · OFF-BEAT ›" … |
| Hold | Hold Echo/Reverb/Filter/Ducker; Roll 1/8…; Echo start / Bass cut / Reverb wash / Filter start | ECHO / REVERB / FILTER / DUCK; ROLL 1/8…; ECHO ALL / BASS KILL / WASH / HPF |
| Hold hint | Hold pads: 4 sec max · Presets open Lab FX · Release dry to recover | Hold = on while touched. Tap a preset to latch. |
| Footer | Depth; Pads A–D FX | DEPTH 65 (live value); SAMPLER FX |
| Samples tab | pads A–D, "Tap to load", "Hold roll A", "Play an analysed sample" | pads 1–4 with "+", ROLL · PAD 1, "Play pad 1 first (needs a BPM)" |
| Sampler | Tap to load (×16); Stop all samples; Hold a pad for Stop / Clear / Level; hold menu Stop / Clear pad / Sample level | "+" with the pad number (accessible "Pad 7, empty: tap to load"); STOP ALL; Hold a pad to edit; STOP / CLEAR / LEVEL |
| Transport | BEAT SYNC; four-deck Cue / Play·Pause / More | SYNC; CUE / ▶❚❚ / MORE |
| MORE | Hold Cue / Play / Beatgrid / Zoom out / Zoom in | CUE (hold) / ▶❚❚ / Edit grid / ZOOM − / ZOOM + |
| Hot cues | A+ ; "Set cue A at the current paused position" | A (dashed) ; "Set cue A here" |
| Readout, empty | No track loaded; Tap to browse and load a track; (four-deck) Tap to browse | Empty; Tap to browse · or press LOAD on the controller; Tap to load |
| Readout | phrase countdown "0.3"; grey × for an incompatible key | "0.3 BARS" (14 px caption, when it fits); nothing |
| Track info | DECK LAB · Deck 1 · TRACK INFORMATION; Title / Artist / File location; No track loaded | DECK 1 · TRACK; TITLE / ARTIST / FILE; Deck 1 is empty |
| Beatgrid | title, seven long buttons, GRID LOCK ON/OFF, six hints | DECK 1 · GRID · 128.00 BPM; ‹ GRID, BEAT HERE, GRID ›, BPM −, BPM +, ÷2, ×2 (full meaning kept as accessible names); LOCK (filled lamp); the audit's six short hints |

### Readout and clock (PF-D20, PF-D08, PF-D13)

- **End-of-track clock:** alternates live-red figures on the ground with a live-red box behind dark figures. Both
  phases measure about 5.4:1, and the dimmed `#8C2219` phase is gone.
- **Readout tap:** opens Track info only while the deck is paused. A tap on a playing deck's readout does nothing, and
  neither does Return/Enter. Track info is in MORE at all times, now in the four-deck card's MORE too
  (`decklab-card-N-track-info`). The accessible description says so while playing.
- **Compact (four-deck) BPM:** when the decimals do not fit, the card shows the **rounded** whole BPM: 127.96 reads
  128, not 127 (`DeckLabStripRenderer::roundedBpm`).

## Tests

Native (`NativeDeckLabTest`, all in the locked suite):

- **New tests:**
  - `OnStatesAreFilledLampsNotOutlines`: QNT, KEY LOCK and LOOP pixel-check `#F2F4F7` when on. Four-deck Play acts on
    touch-down and is green when playing.
  - `SyncTapMatchesOnceAndHoldTogglesTheLock`: tap matches only (MATCHED). The hold locks with the finger still
    down, the button is filled, and a tap while locked keeps the lock. Holding again unlocks. A layer change or a
    slide-off cancels the hold. The four-deck card works the same.
  - `HotcuesSetWhilePlayingOnTouchDown`: an empty pad is live while playing, sets on press, then jumps on press.
  - `XyPadAndHoldEffectsOutliveFourSecondsAndLostTouchesEndThem`: the XY pad stays wet 4.6 s with the finger down.
    A touch elsewhere ends an astray hold. A hold effect and a roll both last past 4 s, and RELEASE shows TAIL.
  - `LabFxAndSamplerUseBarlowBrandColoursAndClearCopy`: checks Barlow 600 and 16 px on the buttons and Barlow on the
    labels, the brand fills (dropdown, bands, lamp, XY grid, no red), the copy, and the sampler pad and STOP ALL look.
  - `SamplerPadsFireOnTouchDownAndEmptyPadsOpenBrowseOnRelease`
  - `FourDeckBpmRoundsInsteadOfCutting`
- **Updated tests:**
  - the readout test (no Track info from a playing deck, both layouts; MORE and four-deck MORE do open it);
  - the hot cue tests (press-to-fire semantics, enabled while playing, "A" label);
  - the end-of-track test (red box vs red figures pixels);
  - the SYNC, Play and four-deck label checks, and the Track info and beatgrid headings.
- Stem gestures: `decklabstemgesturestest.cpp` is rewritten for touch-down muting, and `decklabstemstest.cpp` is
  updated where a press now acts immediately.

Captures added: `decks-lit-lamps.png`, `decks-sync-matched.png`, `decks-sync-locked.png`,
`decks-end-warning-box.png`, `decks-four-rounded-bpm.png`, `labfx-band-lit.png`, `labfx-xy.png`,
`labfx-xy-held.png`, `sampler-brand.png`.

The lock was refreshed with `resolve_lock.py --refresh`, and `build_pi.py --check-inputs` passes.

### Verification status

**Not yet verified in CI.** Run 39876574054 (dispatched on 0af8aa9734) was cancelled mid-compile. GitHub was
blocking invocations on brakoish/mixxx ("repository invocation blocked"), and the re-run is still stuck in the queue.
Per the coordinator, no further runs were dispatched.

Verified locally:

- `build_pi.py --check-inputs`: PASS. `test_build_pi.py`: 7 tests OK.
- **Syntax and semantic compile against Qt 6.4 in WSL** (`g++ -std=c++20 -fsyntax-only -Wall -Wextra -ffast-math`,
  moc'd headers). Covered: `wdecklab.cpp`, `decklabdeckwidgets.cpp`, `decklabstempads.cpp`, `decklabtest.cpp`,
  `decklabstemstest.cpp` and `decklabstemgesturestest.cpp`. No new warnings; the 9 `-Wdangling-else` warnings in
  `decklabtest.cpp` are the same as on the base.
- `DeckLabStemGesturesTest`: all 8 tests built and **passed** in WSL. The suite is header only, plus gtest.
- **Readout render check.** `decklabdeckwidgets.cpp` was linked into a small offscreen harness with generated
  values. It confirmed, with no overflow reported:
  - the end-of-track red figures and the red-box phase;
  - MATCHED in the lamp row and "8.3 BARS";
  - the empty-deck copy;
  - compact cards reading **128** for 127.96. The 800 px four-deck card has no room for decimals even with a short
    clock, so it used to read 127.

**Still needs CI** (the ARM64 Flatpak build):

- `NativeDeckLabTest`, all new and updated tests, including the two 4.3-4.6 s hold waits;
- `DeckLabStemsTest`;
- the native app smoke (43 screens);
- a look at the captures: `screen-0/2/3`, `labfx-*`, `sampler-brand`, `decks-*`.

## Merge notes (regions touched)

- `src/widget/wdecklab.cpp`
  - **Sheets.** `kActionStyle` (`:checked` filled, Play green, `[lit]`) and `kDialogStyle` (`:checked` filled) are
    modified. `kMenuStyle`, `kPerformStyle`, `holdBackstopMs` and the `kSync*` constants are new. **`kStyle` and
    `kRailStyle` are not touched.**
  - **Painted widgets.** `DeckLabXY`, `DeckLabHotcuePad` (empty look) and `DeckLabPad` change; `DeckLabPlayButton` is
    new. `button()` gets one branch for `*-play` names, and `decklab-sample-roll-*` are now plain buttons (they were
    accidentally `DeckLabPad`s).
  - **Deck functions.** `buildTransport` is rewritten. Also changed: `showTrackInfo` and `showBeatgrid` (copy),
    `buildHotcues` / `refreshHotcues`, `buildSampler`, `buildEffects`, and the readout click plus the description in
    `ensureDeckSurfaces` / `applyDeck`.
  - **Refresh and effects.** One line in `refreshDeck` (`matched`). Also changed: `refreshTransport` (play label,
    sync lit), `refreshFx` (lamp and message), the new `endHolds`, `releaseEffects` (clears the hold source), and
    `tickEffects` (backstop, and the tail end keeps rolls). `tickEffects` heartbeat behaviour is unchanged.
  - **`eventFilter`.** One block is added after the TouchCancel line.
- `src/widget/wdecklab.h`: `endHolds()`, `TransportState::synced`, `m_fxMessage`, `m_holdSource`, `m_holdLimit`.
- `src/widget/decklabdeckwidgets.{h,cpp}`: `DeckLabStripState::matched`, `roundedBpm`, lamps, values, phrase
  caption, empty copy. Rail chip classes are untouched.
- `src/widget/decklabstemgestures.h` (API: `press(stem, context, now)` returns a `Result`; `release(context)`) and
  `decklabstempads.{h,cpp}`.
- **Possible overlap with qw-system:** it owns `kStyle` and the global font. If it adds a global Barlow rule, mine
  still apply; the page sheets only add brand values.
- **Possible overlap with the overlay agent:** none. Banners are not touched.
- **Possible overlap with qw-library:** none. `refreshLoadButtons` ("Load pad N") is unchanged, and pads are
  numbered 1–16 everywhere.

## Not done (left for the bigger moves)

- **Controller FX details dialog copy (PF-X28/29).** Ownership with the rail chips is unclear, so only its sheet's
  checked state changed.
- **Hot cue pad tint (PF-D35), LOOP "4 BEAT" (PF-D39) and the readout lamps restyle (PF-D21).**
- **Lab FX deck buttons and beat strip (PF-X02/X20), fade back to dry over one beat (PF-X17), and stems DRUMS / BASS
  names for unnamed stems (PF-D61).** Mislabel risk for unnamed stems.
- **Hold-to-solo glitch.** Holding a playing stem to solo it still drops that stem for the 300 ms hold time; the
  latching SOLO key (PF-D57) is the fix.
