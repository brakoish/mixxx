# Deck Lab next: stems (stems workstream)

Branch `decklab/stems`, based on `origin/decklab/getmusic-plus` (4f5786df38). Goal: touch stem control on the Decks
screen for stem files (NI STEMS, `*.stem.mp4`), which this Mixxx 2.6 fork already decodes and mixes.

| Commit | Subject |
|---|---|
| 98ff069963 | Add touch stem pads to the Decks screen |
| b141649761 | Wait for the caching reader in the stem audio tests |
| 67ab30a36a | Time only engine calls in the stem cost probe and use the fdk-aac mirror |
| (last) | This note |

## What Mixxx already does (no engine state was added)

| Item | Where | Notes |
|---|---|---|
| `[ChannelN],stem_count` | `EngineDeck::slotTrackLoaded` | Read-only. Stems in the loaded track's manifest, 0 otherwise. Main decks only. |
| `[ChannelN_StemM],mute` | `EngineDeck` | Push button (PowerWindow for MIDI only); `set()` writes the value directly. |
| `[ChannelN_StemM],volume` | `EngineDeck` | Potmeter, default 1. Not used by the pads. |
| `[ChannelN_StemM],color` | `BaseTrackPlayerImpl::slotTrackLoaded` | Read-only, manifest colour as `0xRRGGBB`, -1 without a stem track. |
| Stem names | `Track::getStemInfo()` | From the `moov/udta/stem` JSON manifest (`StemInfoImporter`); Mixxx substitutes "Stem #n" for a missing name. Imported when the audio source opens, so it is set by the time the track is loaded. |
| `[QuickEffectRack1_[ChannelN_StemM]]` | `EffectsManager::addStem` | One quick-effect chain per stem, processed post-fader inside the stem mix. |
| Load reset | `EngineDeck::slotTrackLoaded` | Every load unmutes all stems and sets volume 1, unless `[Mixer Profile],stem_auto_reset` is off. |
| Stem selection on load | `BaseTrackPlayer::slotLoadTrack(track, stemMask, play)` | Empty mask: stem mixing. `StemChannel::All`: one stereo signal summed from the four stems ("Load pre-mixed stereo track" in Mixxx's track menu). One flag: that single stem. |

How a stem file plays:

- **Decode.** `SoundSourceSTEM` never decodes the file's mastered mixdown stream (stream 0; see the comment in
  `SoundSourceSTEM::tryOpen`: no analysis data matches it). It always opens and decodes the **four stem streams**, in
  both modes. In stem mode it interleaves them into 8 channels; with a stem mask it sums them into stereo in the reader.
- **Engine.** `EngineDeck::process` only takes the stem path (`processStem`) when the engine buffer holds more than 2
  channels, i.e. a stem file loaded for stem mixing. Normal tracks, and stem files loaded as a stereo mix, take the
  unchanged stereo path. In stem mode the time-stretcher (linear, SoundTouch or Rubber Band) runs on 8 channels;
  Rubber Band splits them into stereo tasks on `RubberBandWorkerPool`. Each stem then runs through its quick effect,
  gets its mute/volume gain (ramped, no clicks), and the four are mixed to stereo.
- **Reader cache.** A main deck's `CachingReader` allocates 80 chunks of 8192 frames for 8 channels at start-up
  (about 21 MB virtual per deck) whether or not stems are used. A stereo track touches a quarter of each chunk
  (about 5 MB resident per deck); a stem track touches all of it.

## Features

- **Stem pads** (`src/widget/decklabstempads.{h,cpp}`, new). For a deck that plays a stem file as stems, four pads show
  the stem's name (upper case, 16 px SemiBold Barlow, elided) and its colour from the file.
  - **Tap** toggles that stem's mute, on release (like the hot-cue pads).
  - **Hold** (300 ms) solos the stem while held: the other stems mute. **Release** restores the state from before the hold.
  - **Double-tap** (second tap on the same pad within 400 ms) unmutes every stem.
  - Pads stay dark. The stem colour is a 6 px bar at the left edge, like a hot cue's colour badge, so a red stem
    (NI's default drums colour is `#FD4A4A`) never reads as live red. **Muted:** dim fill (`#111418`), hollow and dimmed
    colour bar, muted name (`#9AA3AF`) with a 2 px strike line. **Held solo:** 2 px white outline (selection is white).
  - Mutes changed by a controller or by the engine (load reset) show at once: pads repaint from the controls.
  - Accessible name per pad ("Deck 1 stem Drums, muted") and a description of the three gestures.
- **Two-deck view.** A 48 px row (4 px gap, 44 px pads, 4 px gaps) under the overview of each card, only while a
  shown deck has stems. The row is taken from the waveforms (two stacked lanes go from 95 to 71 px at 800x480). Hot
  cues and the deck tool row do not move. A shown deck without stems keeps an empty, inert band so both cards stay
  aligned. With no stem track on a shown deck the layout is exactly today's (tested geometry equality).
- **Four-deck view.** The compact cards keep their layout: a 48 px row would cut the four 50 px waveform lanes to
  38 px. Stems are offered in each deck's **More** menu instead (a 44 px pad row under the hot cues, only for a stem
  track). Muted stems also show on the deck's waveform: Mixxx's stem waveform renderer draws a muted stem flat.
- **Settings > Deck > Stem tracks**: "Play as stems" (default) or "Play as normal track", as one settings row with a
  short explanation under the choice (touch screens show no tool tips). Stored as `[DeckLab],StemPlayback` (0/1). It
  applies to the next load on the four main decks and maps onto Mixxx's own stereo load (`StemChannel::All`). An explicit request (Mixxx's
  single-stem or pre-mixed menu items) is kept as requested. A stem file loaded as a normal track still reports its
  stems in `stem_count`, but mutes do nothing, so the pads check how the engine buffer was loaded
  (`EngineBuffer::getStemMask()`) and stay hidden.
- **Not done (optional item):** a per-stem quick-effect filter strip. The controls exist
  (`[QuickEffectRack1_[ChannelN_StemM]],super1`), but there is no room for a strip in either view, and putting a sweep
  on the pads (drag) would make tap/hold/sweep ambiguous on a 44 px target. Suggested follow-up: a momentary
  "STEM FX" mode in the More menu, where each pad becomes a horizontal sweep that springs back to neutral on release.

## Gesture safety

The rules live in `src/widget/decklabstemgestures.h` (header only, QtCore only), separate from widgets and controls:

- Every press is tied to a context: WDeckLab's `m_interactionEpoch` (page, layer and layout changes, touch cancel,
  deactivation) and the deck's load serial (bumped on `loadingTrack`, `newTrackLoaded`, `trackUnloaded` and
  `track_loaded` changes). A tap whose release lands in another context, or off the pad, writes nothing. This is the
  hot-cue rule.
- A **solo always ends**: on release (even after an epoch change), when the pad or row hides (page change, layer
  change, layout change, More menu closing, the row going away), on `TouchCancel` or `ApplicationDeactivate` (an
  application event filter installed only while a pad is down), when a load starts or finishes, and in the destructor.
- Restoring writes back only stems whose mute still holds the value the solo wrote; a mute changed meanwhile by a
  controller is kept.
- A new track mid-hold: Mixxx resets stems on load, so the solo is only undone if the deck kept the solo state
  (`stem_auto_reset` off; the deck then keeps the mutes from before the hold, as Mixxx intends). The new track never
  inherits the solo.

## Pi 4 cost

Measured by `DeckLabStemsTest.StemDecodeAndMixCostProbe` in CI run 39862978901 on the GitHub ARM64 runner (the same
aarch64 build and runtime as the Pi), with Mixxx's 5 s synthetic AAC stem fixture. All 150 timed buffers of every
engine case were audible. The Pi 4 column assumes a Cortex-A72 at 1.5 GHz is about 4 times slower per core than the
runner (range 3 to 5); it is an estimate, not a Pi measurement.

Decode (caching reader thread, bursty, 8192-frame chunks read ahead; CPU per second of audio):

| Source | CI, x realtime | CI, CPU | Pi 4 estimate |
|---|---|---|---|
| One AAC stream (the file's mixdown; like a normal AAC track) | 1425x | 0.07 % of a core | about 0.3 % |
| Four stems, stem mode | 319x | 0.31 % | about 1.3 % |
| Four stems summed, "normal track" mode | 330x | 0.30 % | about 1.2 % |

Engine (audio thread, one deck at +4 % tempo; time per 512-frame buffer of 11.6 ms with the idle engine, 12.8 us,
subtracted; the share of the buffer period is the same at the Pi's 1024 frames):

| Deck | Keylock off (linear) | Keylock on (Rubber Band, default) |
|---|---|---|
| Stem file as stems | 40.4 us, 0.35 % (Pi about 1.4 %) | 138.6 us, 1.2 % (Pi about 4.8 %) |
| Stem file as normal track, or a normal track | 18.2 us, 0.16 % (Pi about 0.6 %) | 33.5 us, 0.29 % (Pi about 1.2 %) |
| Stems vs stereo | 2.2x | 4.1x |

With keylock on, Rubber Band splits the 8 channels into stereo tasks on `RubberBandWorkerPool`; the figure is the
engine thread's wall time, which includes waiting for those tasks, so total CPU across cores is somewhat higher.

What this means on a Pi 4:

- Decode is cheap: about 1.3 % of one core per playing stem deck, four times a normal track, on the reader thread.
  Analysis decodes four streams once per track (about 4.5 times a normal track's analysis decode).
- The engine is where stems cost: about 4 times a normal deck with keylock. Four stem decks with keylock would use
  about 19 % of the audio period on the engine thread (about 5 % for four normal decks); with keylock off, about 6 %.
  Two stem decks fit comfortably next to today's baseline (GUI 54 %, audio FIFO about 7 % of a core).
- Memory: each main deck's reader cache is allocated for 8 channels already (about 21 MB virtual). A stem track
  touches all of it (about 21 MB resident, against about 5 MB for a stereo track), so up to about 64 MB more with
  four stem decks. This matters on 2 GB boards.
- GPU: for a stem track Mixxx's waveform draws four stems in two layers instead of one signal. Not measured here;
  pi4-perf should check it with `DECKLAB_PERF_LOG=1` on a stem track.
- "Play as normal track" brings the engine back to normal-track cost and removes the stem effect chains. It does not
  reduce decode (still four streams) or the analysis cost.

## Tests

All in the native CI suite (`tools/decklab/pi-build-lock.json` `test_suites`):

- `DeckLabStemGesturesTest.*` (`src/test/decklabstemgesturestest.cpp`, 8 tests): the gesture rules against fake mute
  controls, no engine or widgets. Tap on release, double-tap restores all, slow taps and taps on two pads stay
  toggles, hold solos and release restores (also released off the pad), a hold never starts a double tap, stale
  epoch / stale load / off-pad / cancelled taps write nothing, no solo after a context change, a solo ends on release
  even after an epoch change and on cancel, track loaded mid-hold with and without `stem_auto_reset`, mutes changed
  elsewhere during a solo are kept. These also build and run standalone (QtCore + gtest).
- `DeckLabStemsTest.*` (`src/test/decklabstemstest.cpp`, 7 tests) on the real `WDeckLab` with real decks (stem
  channel handles registered as `PlayerManager` does):
  - `PadsAppearOnlyForStemTracksAndOtherwiseKeepTheLayoutExactly`: fake stem controls (`stem_count`, `color` on a
    generated track). No pads and an identical geometry snapshot of every visible `decklab-*` widget without stems,
    and with stems only on a deck that is not shown. With stems: deck row 292 px, pads 44 px and 4 px under the
    overview, the two lanes 48 px shorter, hot cues and deck tools unmoved, the other card's band empty and aligned.
    Loading a normal track restores the exact original geometry.
  - `TapHoldAndDoubleTapDriveTheRealStemMutes`: the real `[Channel1_StemM],mute` controls; nothing on press; muted
    pads checked by pixels (hollow bar, dimmer fill, strike line) so the state is not colour alone; a controller mute
    shows; hold/release; double tap; the other deck untouched.
  - `StaleGesturesNeverLeaveAStemSoloedOrMuted`: page change mid-hold, layout change before the hold time, layer
    change mid-hold, `TouchCancel`, `ApplicationDeactivate`, a load mid-hold (auto reset on and off).
  - `FourDeckViewOffersStemsInTheMoreMenu`: four-deck geometry unchanged by stems, pads in the deck's More menu
    (44 px, inside the menu), closing the menu mid-hold ends the solo, a deck without stems offers none, back to two
    decks shows the row with the mute kept.
  - `StemFileShowsManifestNamesAndColoursAndMutesTheEngine`: loads Mixxx's synthetic stem fixture
    (`src/test/stems/sin_AAC_256kbps_VBR.stem.mp4`, sine tones; no copyrighted audio). Names Drums/Bass/Synths/Vox and
    manifest colours on the pads; the deck plays; tapping all four pads silences the main mix; a double tap brings it
    back. Captures the screens below.
  - `StemTracksCanPlayAsANormalTrack`: the Settings choice (44 px, caption aligned, 15 px explanation) writes
    `[DeckLab],StemPlayback`; the next load has 2 engine channels, mask `All`, no pads and still plays; switching back
    loads 8 channels with pads. An explicit stem selection is kept.
  - `StemDecodeAndMixCostProbe`: records decode and engine timings as XML properties (never asserted).
- `*StemFixture.*` (upstream `src/test/stemtest.cpp`, 10 tests): proves the pinned KDE 6.10 runtime's FFmpeg decodes
  AAC and ALAC stem files and reads the manifest on ARM64. Upstream's `StemControlFixture` engine tests are
  `DISABLED_` upstream, so they are not in the filter; `DeckLabStemsTest` covers stem mute in the engine instead.

A stem fixture could be loaded in tests, so no file is generated at test time: the upstream fixture is already a
synthetic sine-tone stem file in the public repository. The full application smoke check is unchanged (43 screens);
it has no media, so the stem captures come from the native tests.

Screens (artifact `decklab-mixxx-aarch64-<sha>/`): `stems-two-deck.png` (deck 1 stems with Vox muted, deck 2 a
normal track), `stems-two-deck-solo.png` (Drums held solo on deck 2), `stems-two-deck-1024x600.png`,
`stems-four-deck.png` (unchanged compact cards), `stems-four-deck-more-menu.png`, `stems-settings-deck.png`.

CI (`decklab-pi.yml`):
- 39853382658: failed before compiling; `wtaymans.fedorapeople.org` (fdk-aac-free source) timed out.
- 39853945247: built; 327/329 passed. Both failures were my real-file audio checks reading silence because the caching
  reader fills on its own thread, plus a load wait that did not block. Fixed in b141649761.
- 39858735094 (head b141649761): green. 329 passed, 0 failed, 4 disabled upstream; 67 native UI regressions,
  smoke 43 screens with no failures. Screenshots reviewed.
- 39862978901 (head 67ab30a36a): green. 329 passed, 0 failed, 4 disabled upstream; smoke 43 screens with no
  failures; the cost figures above. Includes the fdk-aac-free mirror.

## Notes for merging (exact hooks)

New files (no other workstream touches them): `src/widget/decklabstempads.{h,cpp}`,
`src/widget/decklabstemgestures.h`, `src/mixer/decklabstemplayback.h`, `src/test/decklabstemgesturestest.cpp`,
`src/test/decklabstemstest.cpp`, this note.

Hooks in shared files, all additive:

- `src/widget/wdecklab.h`: `class DeckLabStemPads;` forward declaration and one member after `m_minis`:
  `std::array<DeckLabStemPads*, 4> m_stemPads{};`.
- `src/widget/wdecklab.cpp` (decks-ui and system-setup regions):
  - `#include "widget/decklabstempads.h"`.
  - `ensureDeckSurfaces()`: after `m_cardTransports[deck - 1]->hide();`, create the `Row` pads with `card` as parent,
    add them to `cardRows`, and connect `presenceChanged` to `updateLayers()` (3 statements, before `card->hide()`).
  - `updateLayers()`: after the waveform/overview signature loop, `const bool stemRow = m_page == 0 && !four &&
    DeckLabStemPads::rowWanted(m_stemPads, m_layers);` and `signature |= quint64(stemRow) << 19;` (bit 19 was free).
    Before the deck row height, `for (auto* pads : m_stemPads) if (pads) pads->setRowShown(stemRow);`, and the deck
    row height adds `(stemRow ? DeckLabStemPads::kRowHeight : 0)`.
  - `buildTransport()`, compact (`!followsLayer`) More menu: one statement after `buildHotcues(deck, false, content)`
    adds the `Menu` pads.
  - `buildSettings()`, Deck page: one line after the "Deck layout" row adds `createPlaybackSetting()` as a full row.
- `src/mixer/basetrackplayer.cpp` (no workstream owns it): include `mixer/decklabstemplayback.h` and, in
  `slotLoadTrack()` after the sandbox check, `stemMask = decklab::stemLoadSelection(m_pConfig, stemMask);` for primary
  decks under `#ifdef __STEM__`.
- `CMakeLists.txt`: `src/widget/decklabstempads.cpp` after `decklabdeckwidgets.cpp`; the two test files after
  `decklabsystemtest.cpp`.
- `tools/decklab/pi-build-lock.json`: suites `DeckLabStemGesturesTest.*`, `DeckLabStemsTest.*`, `*StemFixture.*`;
  hashes for `CMakeLists.txt`, `wdecklab.{h,cpp}` and `fdk-aac-free.yaml`. Expect conflicts; regenerate after merging.
- `packaging/flatpak/modules/fdk-aac-free.yaml`: the Fedora lookaside mirror, identical to pi4-perf's 94564c026f (only
  this file was taken), so it merges cleanly with pi4-perf.
- Not touched: `decklabdeckwidgets.*` and the deck strip (usb-first's phrase hook), waveform renderers (pi4-perf),
  `decklabsmoke.cpp`, `build_pi.py`.

Behaviour other workstreams should know:

- The two-deck deck row is 292 px instead of 244 px while a shown deck plays a stem file. Anything that assumes 244 px
  should read the height of `decklab-deck-row`. The readout (where usb-first adds phrase info) does not change size.
- `DeckLabStemPad` is a `QAbstractButton`, not a `QPushButton`, so the `QPushButton` geometry sweeps in the tests and
  the skin's `QPushButton` rules do not apply to it; its 44 px height is fixed in code and tested.
- Pads are event driven (control notifications and player signals). Nothing runs per frame or per tick, and hidden
  pads do no work; `IdleDecksRefreshWithoutStyleOrLayoutWork` still passes.

## Follow-ups

- Four-deck view: a stem lamp on the compact card (for example on More) when a stem is muted, so the state shows
  without opening the menu. Needs a decks-ui change to the card or the compact readout.
- The optional per-stem quick-effect sweep ("STEM FX" mode in More, springs back on release).
- "Normal track" still decodes four stems. Decoding only the file's mixdown stream would cut decode to one stream,
  but Mixxx's analysis (gain, grid, waveform) is made on the stem sum and the NI mixdown is mastered (limiter and
  compressor), so it needs level matching first.
- Measure on the Pi with real NI files: `DECKLAB_PERF_LOG=1`, `top -H` for the engine, reader and Rubber Band pool
  threads, two and four stem decks, keylock on and off.
