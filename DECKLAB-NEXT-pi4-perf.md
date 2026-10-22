# Deck Lab pi4-perf workstream (2026-10-22)

Branch `decklab/pi4-perf`, based on `origin/decklab/getmusic-plus` (4f5786df38). That base already had the
render-on-demand gate, event-driven Deck Lab refresh, audio-thread pinning and analyzer idle priority. This round
targets what is left on a Raspberry Pi 4 with 2 to 4 **playing** decks.

The profile on the Pi uses `WaveformType 12`, `use_hardware_acceleration 3`, `waveform_options 0`, `FrameRate 60` and
`VSync 0`. That is `allshader::WaveformRendererRGB` (RGB, no high-detail texture, no stereo split) driven by the timer
loop. Read-only check of the running build on the Pi (labwc + Xwayland, decks idle): GUI thread 1.4% of a core,
engine 3.2%, controller thread 2.0%.

## Commits

| Commit | Subject |
|---|---|
| 36c7239c8a | Build the RGB waveform geometry once per chunk instead of every frame |
| 94564c026f | Reuse the marks-on-screen list and mirror the fdk-aac-free tarball |
| 8ba1bdd128 | Cache the Simple, Filtered and RGB Stacked waveforms too |
| (last) | Add the pi4-perf hand-off note with the CI record |

## Environment variables (all optional)

| Variable | Effect |
|---|---|
| `DECKLAB_WAVEFORM_CACHE=0` | **Kill switch.** Uses the upstream renderers that rebuild their geometry every frame. Default: cached. |
| `DECKLAB_PERF_LOG=1` | Adds per-renderer CPU time, cache counters and frame pacing to the existing 10 s log (section 5). |
| `DECKLAB_MIDI_BRIDGE=0` | Does not open the legacy SysEx state bridge mapping at startup (section 4). Unset keeps it. |

## 1. Waveform geometry built once, not every frame

Files: `src/waveform/renderers/waveformcolumncache.{h,cpp}` (new, no OpenGL),
`src/waveform/renderers/allshader/waveformrenderercached.{h,cpp}` (new), `src/waveform/widgets/allshader/waveformwidget.cpp`.

Upstream rebuilt every visible column on the CPU each frame: for 800 px, 800 max-scans over the waveform data, colour
and height maths and 4,800 vertices (96 KB), which the GL backend then uploaded again from client memory.

Upstream already snaps the scroll position to whole pixels, and its column `pos` samples around visual frame
`(qRound(first / increment) + pos) * increment`. So a column's content depends only on its global index `k`, for a
given zoom, tempo, width, DPR and waveform. The new renderer:

- keeps columns in **128-column chunks** in a ring of `ceil(width / 128) + 3` slots inside one vertex buffer (12-byte
  vertices: x, edge sign, raw low/mid/high/all maxima);
- per frame computes `k0` exactly like upstream and draws the visible columns with a **scroll offset uniform**
  (`-k0` px). At DPR 1 the GPU sees bit-identical vertex positions (integers and halves in float);
- applies **gains, colours and height in the vertex shader** from uniforms, in upstream's operation order. EQ knobs,
  kills, deck gain, visual gains, theme colours and waveform height rebuild nothing;
- builds a chunk only when it scrolls into view, prefetching at most one chunk ahead per frame and only in frames that
  built nothing else. At the default zoom a playing deck builds one chunk about every 0.9 s (9 KB upload);
- rebuilds everything only on a zoom, tempo (`rate_ratio`), width, DPR, stereo-split or waveform-object change. Chunks
  built while the track was still being analysed (touching data at or beyond `getCompletion()`) are rebuilt once more
  data arrives. An ejected track's waveform is released;
- draws the axis line first and the columns over it, as upstream's single draw did, with the same blending, then
  unbinds its buffer (the other rendergraph nodes draw from client memory).

Covered types (every type the Settings "Waveform colours" list offers except HSV): **RGB** (incl. stereo split and the
slip waveform), **Simple**, **Filtered** and **RGB Stacked** (three band planes drawn low, mid, high, like upstream).
HSV and the high-detail (textured) option keep the upstream renderers.

Scratching, needle search and hotcue jumps work at any speed: they only change `k0`. A jump builds the newly visible
chunks in that frame (at most `ceil(width / 128) + 1`).

**Identical output.** `DeckLabWaveformCacheTest` (new, in `test_suites`) runs the upstream renderer and the cached
renderer side by side and rasterises what each submits: the upstream vertex array, and the cached vertex buffer with
the vertex shader applied on the CPU in the same order. It covers widths 800, 640, 392 and 383 (half-pixel play
marker), zooms 1 to 10, track start and end (columns outside the data), stereo split, the slip renderer, Simple,
Filtered and Stacked, 600 frames of playback, 120 frames of back-and-forth scratching, a hotcue jump, EQ and kills, deck
and visual gains, theme colour, height, zoom, tempo, width and track changes, and a track still being analysed.
Tolerance: 0.2% of pixels may differ (float contraction). A local harness with verbatim copies of the three upstream
column loops (x86-64, the build's `-ffast-math`) found **0 differing pixels in every case and in 3,000 consecutive
frames**. On a GPU: `GpuPixelCompareWhenOpenGLIsAvailable` renders both renderers through real rendergraph engines into
FBOs when a GL context exists (a desktop, or the Pi with `QT_QPA_PLATFORM=xcb`); the CI runner has no GL, so there it
only records `gpu_pixel_compare=unavailable`. A local Mesa llvmpipe run (desktop GL 4.5 compatibility) of the exact
shader sources, buffer layout and draw ranges gave **0 differing pixels for RGB, Simple and Filtered** at three
positions with non-unity EQ gains.

## 2. Frame pacing

Measured facts:

- The panel runs at **60.029 Hz** (`wlr-randr` on the Pi). The timer loop runs at exactly 60.000 Hz with swap
  interval 0 (`VSync 0`). The two drift past each other once every **~34 s**: one display frame repeats a waveform
  position and the next jumps two, a visible hitch in a fast-scrolling waveform. Under Xorg without a compositor,
  swap interval 0 also tears.
- `VSync 5` (PLL) switches waveforms to swap interval 1 and locks the render tick to the measured vblank
  (`frameSwapped` of the 3x3 shared GL window, which it repaints every frame). That removes both effects, at the cost
  of one extra tiny swap per frame. The earlier team saw PLL/VSync crashes on Wayland; the kiosk is now Xorg, where
  it has not been tried.

This branch adds the `DECKLAB_PERF pacing` line (section 5). Proposed Deck Lab default, pending the A/B in "Measure on
the Pi": **`[Waveform] VSync 5` with `FrameRate 60`** in the appliance profile if PLL is stable on Xorg and its p95/max
intervals are no worse; otherwise keep `VSync 0`. It is a profile value (appliance repo, `profile_settings.py`), not a
new UI option.

Adaptive rates were considered and **not** added: the render gate already renders paused, unchanged decks only on
change plus a 2 Hz safety refresh, and a playing or scratched deck must render every frame to scroll smoothly. Capping
changed-but-paused decks at 30 Hz would slow needle search and knob feedback for almost no saving, now that gains and
colours cost only uniforms.

## 3. One GL window for the wave zone (design and estimate, not implemented)

Today each scrolling waveform is a `QOpenGLWindow` child with its own context. Per frame and playing deck the GUI
thread makes that context current twice (render slot, swap slot), releases it twice and swaps once. With four decks
that is 8 context switches, 8 releases and 4 `glXSwapBuffers` per frame.

Design:

1. A `WWaveZone` GL widget owns one native `QOpenGLWindow` covering the wave zone; the `WWaveformViewer`s stay as
   plain layout and input widgets without native windows.
2. Each deck keeps its allshader renderer stack and rendergraph engine but renders into its **own FBO texture**
   (viewport sized), so the render gate still skips unchanged decks: a skipped deck reuses its texture.
3. Each frame the zone draws the FBOs as textured quads at the viewer rectangles and swaps **once**.
4. Input: the zone window hit-tests events and forwards them to the viewer under the point (touch scratch must stay
   instant). Drag and drop and tooltips go to the same viewer.
5. Geometry: the zone follows the viewers on layout changes (2/4 decks, side by side) and requests a frame.

Expected saving on the Pi with four playing decks: 6 of 8 context switches and 3 of 4 swaps per frame, and three fewer
native windows for the overlay fix to stack Qt widgets above. Estimate: **3 to 5 days** including Pi testing (new
widget and event routing, factory holders per viewport, a hook in `addWaveform` owned by decks-ui, and the overlay
interplay). Too big to do safely in this round while the overlay work is in flight.

A cheaper first step, to measure first: stop calling `doneCurrent()` after each widget in the factory's render and
swap loops (`makeCurrentIfNeeded()` already switches directly). That halves the GLX calls without touching windows.

## 4. Smaller wins

- **VU meters and overview.** Deck Lab draws its own `DeckLabLevelMeter` (decks-ui), sampled every 40 ms and repainted
  only when a lit segment changes. `WOverview` repaints only when the cursor moves a whole pixel (2 to 3 times a second
  while playing, see perf-engine). The skin has no GL VU meters or spinnies. Nothing to gain here.
- **Per-frame allocations.** The allshader mark renderer allocated a new on-screen mark list every frame. It now
  refills one list that it swaps with the renderer's. The cached waveform renderer allocates nothing per frame (fixed
  ring, reused vectors). The perf-log timing vectors are reserved up front.
- **Texture uploads only on change.** Verified: mark labels regenerate only when obsolete, the play marker only on a
  height or DPR change, the preroll pattern only on a size change. The waveform buffer uploads only rebuilt chunks.
- **Beat grid, marks, play marker.** These still write their (small) vertex arrays each frame: about 20 beat lines,
  one quad per visible mark and one play-marker quad. The beat grid measured 0.7 us per frame on CI (below), about 4% of
  the upstream signal renderer, so a translation-offset rewrite was not worth its risk this round.
- **MIDI SysEx state bridge.** `DeckLab.midi.xml`/`DeckLab.js` on MIDI Through polls about 40 controls and sends a JSON
  report every 250 ms, also in native mode (`nativeReport`). On the Pi its controller thread uses about **2% of a core**
  (`top -H`, idle). `DECKLAB_MIDI_BRIDGE=0` skips opening that mapping at startup, matched by file name
  (`src/controllers/decklabbridgegate.h`, a six-line gate in `ControllerManager::slotSetUpDevices`). **Only set it when
  decklabd handles tool launching and system state**: the companion's tool requests (`tool_request`/`tool_serial`,
  native tools ready/close, sources, deck setup) travel over this bridge. The legacy rollback launcher does not set it,
  so it keeps the bridge. A user who enables the mapping in the controller preferences still gets it. Suggested owner
  of the switch: `decklab-mixxx.service` (os-image), once system-setup confirms decklabd covers the tools.
- **CI source mirror.** `wtaymans.fedorapeople.org` stopped answering during this round, so every Flatpak build failed
  while downloading fdk-aac-free. `packaging/flatpak/modules/fdk-aac-free.yaml` now fetches the identical tarball (same
  sha256) from Fedora's lookaside cache, with the old host as a mirror. Other branches hit the same failure until this
  merges.

## 5. Measurement tooling

`DECKLAB_PERF_LOG=1` now logs, every 10 s, besides the existing `waveform` and `render-loop` lines:

```
DECKLAB_PERF renderers [Channel1] frames=<n> background=<mean>/<p95> markrange=... signal=... beat=... mark=... other=... total=<mean>/<p95> cache: frames=<n> resets=<n> chunks=<n> columns=<n> upload=<KiB>KiB
DECKLAB_PERF pacing mode=<6 timer|5 PLL> fps=60 intervals=<n> mean=<ms>ms p95=<ms>ms max=<ms>ms late=<n>
```

- `renderers`: CPU time per **rendered** frame of each renderer of that waveform, mean/p95 in ms: its `preprocess()`
  plus its GL command submission in `render()`, child nodes counted for their renderer (mark labels for `mark`). It
  comes from an optional probe in `rendergraph::Engine`, which is null and costs nothing unless the log is on. `slip`
  sums the slip renderers, `other` the container nodes. GPU time is not included. Categories that never took time are
  omitted.
- `cache`: prepared frames, full rebuilds (`resets`), chunks and columns built, and bytes uploaded in the window.
- `pacing`: the VSync mode, then the swap-slot interval statistics; `late` counts slots more than 1.5 frames apart.

`DeckLabWaveformCacheTest.BenchmarkRendererCpuPerFrameReportOnly` (report only, never fails on timing) plays 1,200
frames at 2.45 px per frame at widths 800 and 392, for RGB and Filtered, and prints `DECKLAB_BENCH` lines: CPU per
frame of the upstream and the cached renderer (mean and p95), bytes uploaded per frame, chunks built, and the beat grid
renderer.

## CI

Workflow `decklab-pi.yml` on the ARM64 runner (a server-class ARMv8 core, roughly 3 to 4 times a Pi 4 core):

| Run | Commit | Result |
|---|---|---|
| 39853994087 | 36c7239c8a | Failed before compiling: the fdk-aac-free download timed out (upstream host down). Fixed by the mirror. |
| 39854634480 | 94564c026f | **Success.** 315 native tests passed, 0 failures, smoke check passed. All 9 RGB cache tests and the bridge gate test passed; every pixel comparison was identical (0 differing pixels). |
| 39859054139 | 8ba1bdd128 | **Success.** 316 native tests passed, 0 failures, smoke check passed (43 screens). All 11 `DeckLabWaveformCacheTest` tests (RGB, slip, Simple, Filtered, Stacked) and the bridge gate test passed, every pixel comparison identical. GPU compare: no GL on the runner (`gpu_pixel_compare=unavailable`). |

CPU per frame while a deck plays (benchmark, 1,200 frames at 2.45 px per frame, zoom 3), run 39854634480:

| Width | Upstream RGB signal | Cached RGB signal | Upload per frame (upstream / cached) | Beat grid |
|---|---|---|---|---|
| 800 px (two-deck layout) | 16.6 us mean, 16.7 us p95 | 0.2 us mean, 0.3 us p95 | 96,120 B / 184 B | 0.7 us |
| 392 px (side by side) | 8.2 us mean, 8.3 us p95 | 0.2 us mean, 0.2 us p95 | 47,160 B / 184 B | 0.5 us |

The cached renderer built 24 chunks of 128 columns in 1,200 frames (2,940 px scrolled). The upstream numbers are the
CPU column build only; on the device the GL driver also copies the 96 KB vertex array from client memory into a GPU
buffer every frame, which the cached path no longer does. Expect the Pi 4 to be several times slower in absolute
terms (Cortex-A72 at 1.5 GHz). The beat grid costs under 1 us per frame, which is why it was left as it is.

Run 39859054139 repeated the RGB numbers (16.6 -> 0.2 us at 800 px, 8.2 -> 0.2 us at 392 px) and added Filtered:

| Width | Upstream Filtered | Cached Filtered | Upload per frame (upstream / cached) |
|---|---|---|---|
| 800 px | 17.8 us mean, 18.0 us p95 | 0.2 us mean, 0.2 us p95 | 288,120 B / 552 B |
| 392 px | 8.8 us mean, 8.9 us p95 | 0.2 us mean, 0.2 us p95 | 141,240 B / 552 B |

"Before" is the upstream renderer in the same binary (`DECKLAB_WAVEFORM_CACHE=0` path), measured side by side.

## Measure on the Pi

With the new build, two and then four decks loaded, Decks page:

1. **Playing decks.** `DECKLAB_PERF_LOG=1`, play 2 and then 4 decks for a minute. Read `renderers ... signal=` and
   `total=`, the `cache` counters (expect `resets=0` and about 11 chunks per 10 s per playing deck at zoom 3) and the
   `render-loop` mean/max. `top -H -p $(pgrep -f mixxx.bin)`: GUI thread % with 2 and 4 playing decks.
2. **A/B the cache** on the same build: `DECKLAB_WAVEFORM_CACHE=0` restores upstream. Compare `signal=` and the GUI
   thread %. The difference is the upstream column build plus its 96 KB per frame client-memory upload per deck.
3. **Look**: the waveform must look exactly as before (colours, heights, axis line, the half-height slip waveform),
   including while turning EQs (the waveform recolours live), after zoom and pitch-fader changes, while a track is
   still analysing (the waveform fills in), right after loading a track, and in the Filtered, Stacked and Simple
   styles.
4. **Touch scratch** a playing and a paused deck, fast back and forth: no lag, no gaps at chunk edges.
5. **GPU pixel compare on V3D**: run `mixxx-test --gtest_filter=DeckLabWaveformCacheTest.Gpu*` in the X session
   (`QT_QPA_PLATFORM=xcb`). It must report `gpu_pixel_compare=run` and pass.
6. **Pacing A/B**: `VSync 0` vs `VSync 5` in the profile, `DECKLAB_PERF_LOG=1`, four decks playing. Compare the
   `pacing` p95/max/late, the GUI thread %, `dropped`, and watch for the ~34 s hitch and for tearing. Set the winner as
   the appliance default. Watch PLL for stability on Xorg (it crashed under Wayland before).
7. `DECKLAB_MIDI_BRIDGE=0` (only with decklabd tools): the controller thread should drop from about 2% to about 0%,
   and Get Music and Sample Maker must still open through decklabd.

## Risks

- **Driver differences.** The cached renderer uses a vertex buffer, an unsigned-byte vertex attribute and branches on
  uniforms in the vertex shader; all are core in OpenGL 2.0 and ES 2.0. It was compiled and pixel-checked on Mesa
  llvmpipe, not yet on V3D. If the program fails to link, the waveform stays empty and the log names
  `DECKLAB_WAVEFORM_CACHE=0`. Check the log on the first Pi start.
- **Column edges sit on pixel centres** in upstream's geometry (x = k +/- 0.5). The cached path feeds the GPU
  bit-identical positions at DPR 1, but a different shader may round the last bit differently on some GPUs, which
  would show as single-pixel column shifts. Step 5 checks this on the Pi.
- **Fractional DPR** (not the Pi panel): positions are exact only for power-of-two DPR; at 1.25 a position may differ
  from upstream in its last float bit.
- **Tempo moves.** Every `rate_ratio` change rebuilds the visible chunks (about 1.2 to 1.5 times one upstream frame of
  CPU), so a moving pitch fader costs a little more than upstream while it moves, and much less once it stops.
- **Merge hot spots:** `CMakeLists.txt`, `tools/decklab/pi-build-lock.json` (test suites and hashes),
  `packaging/flatpak/modules/fdk-aac-free.yaml`, `src/controllers/controllermanager.cpp` (system-setup's area, a
  six-line gate), `src/rendergraph/opengl/engine.*` (probe) and `WaveformRendererSignalBase::setup()` (colours moved
  into `setSignalColors()`, same order). Re-run `python tools/decklab/build_pi.py --check-inputs` after merging.

## Next

1. Run steps 1 to 6 above on the Pi; pick the VSync default.
2. Port HSV to the cache (per-column hue from the band ratios fits the RGB layout).
3. If the perf log shows the swap slot dominating with four decks, try the `doneCurrent()` removal, then the single
   wave-zone window (section 3).
