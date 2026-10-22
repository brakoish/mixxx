# Deck Lab perf-engine workstream (2026-10-22)

Branch `decklab/perf-engine` (based on `codex/native-appliance` d22d6e4a15). Goal: a nearly idle GUI
thread when nothing moves, smooth four-deck playback, and a harder, faster audio path.

Baseline on the Pi (two decks loaded and paused): GUI thread 54% of a core, Xwayland 14%, about
1,340 v3d IRQ/s. Upstream renders and swaps every visible waveform at 60 fps even when paused, and each
waveform is its own `QOpenGLWindow`, so every swap crosses XWayland and the compositor.

## Commits

| Commit | Subject |
|---|---|
| 5ce321c00c | Render paused waveforms on demand instead of every frame |
| cccbf0c0ca | Repaint a hovered overview once per second, not at the indicator rate |
| e1598b5854 | Pin the engine audio thread and idle the analyzers on request |
| aedc1e007b | Never instantiate LV2 plugins on the audio thread |
| 3b82419a24 | Speed up the RMX DSP and tolerate 1.5 s GUI stalls |
| 0f06d18fc5 | Test the render gate, thread policy and LV2 null-instance safety in CI |
| f4b9f049e0 | Keep the RMX build portable and accept DECKLAB_AUDIO_CPU=last |
| 375429e8b1 | CI: fetch full history so the upstream ancestry check finds its pin |
| a390fc5476 | Raise the engine audio thread priority with DECKLAB_AUDIO_PRIO |
| (last) | Add the perf-engine hand-off note (this file) |

"RMX" in two pushed commit subjects refers to the existing effect name. No new UI strings use it, and
this note says "performance effect".

## Environment variables (all optional; unset = upstream behaviour)

| Variable | Effect | Set by |
|---|---|---|
| `DECKLAB_AUDIO_CPU=<n>` or `last` | Pins the clock-reference PortAudio callback thread (the engine thread) to CPU n on its first callback. `last` picks the highest CPU the process may run on (its affinity mask, so cpusets and offline CPUs are respected): CPU 3 on a Pi 4 or Pi 5. A value that is not a usable CPU is ignored with one warning; nothing fails. | `decklab-mixxx.service` (`3`; `last` is the portable choice) |
| `DECKLAB_AUDIO_PRIO=<1-95>` | Raises that same thread to `SCHED_FIFO` at this priority on its first callback (never lowers it). PortAudio's ALSA host leaves it at FIFO 1, below threaded IRQ handlers (50). The appliance uses about 70, which is below the xHCI IRQ thread (about 80) and needs the `decklab` group's rtprio 95 limit. On `EPERM` it logs once and keeps running. Values outside 1-95 are ignored with a warning. | launcher passes it through |
| `DECKLAB_ANALYZER_IDLE=1` | Analyzer worker threads run at `SCHED_IDLE`, falling back to nice 19. | `decklab-mixxx.service` |
| `DECKLAB_PERF_LOG=1` | Every 10 s, one `qInfo` line per waveform with rendered/skipped counts and one line with the GUI render-loop mean/max time and dropped frames. No cost when unset. | manual, for measurements |
| `DECKLAB_RENDER_GATE=0` | **New, optional field kill switch.** Disables render-on-demand and restores upstream every-frame rendering. | only if a visual problem shows up |

`DECKLAB_RENDER_GATE` is not in the PLAN interface list. It is additive and needs no service change.

## 1. Render-on-demand waveforms

Files: `src/waveform/rendergate.{h,cpp}` (new), `src/waveform/waveformwidgetfactory.*`,
`src/waveform/renderers/waveformwidgetrenderer.*`, the renderers under `src/waveform/renderers/**`,
`src/waveform/visualplayposition.*`, `src/widget/wglwidgetqopengl.*`, `src/widget/wglwidgetqglwidget.h`.

`RenderGate` is a small class with no widget dependencies, so it can be unit tested. The factory keeps
one gate per waveform holder. Each tick, for every visible waveform, the factory still runs `preRender()`,
which reads ControlObjects and the visual play position and costs very little. It then asks the gate
whether this frame can differ from the last frame that was swapped:

- **(a) Moving.** The visual play rate or slip rate is non-zero (playing, touch or jog scratching, cue
  preview), or a renderer animates by itself (end-of-track warning blink, slip outline blink). These render
  every frame, exactly as before. **Touch scratching stays instant:** the scratch rate is non-zero, and
  every touch event on the viewer also forces a frame.
- **(b) Changed.** A 64-bit fingerprint of everything drawn has changed. It covers: the pixel-rounded
  play and slip positions (sub-pixel jitter does not count), first and last displayed positions, zoom,
  rate-dependent samples per pixel, size and DPR, deck gain, play-marker position, beat-grid alpha,
  orientation, scale, stem selection, track pointer, waveform pointer, completion and size (analysis
  progress), every visible mark's position and end position and visibility (hotcues, cue, loop, intro and
  outro), a mark revision bumped on label, colour or list changes, mark ranges (start, end, enabled,
  visible), EQ knobs and kills, visual gains, stem gains and mutes, stem display settings, and each
  renderer's animation flag (so the last frame of a blink is drawn).
- **(c) Forced.** Track signals (`cuesUpdated`, `beatsUpdated`, `waveformUpdated`, `analyzed`,
  `stemsUpdated`), skin setup (theme colours), resize, passthrough toggle, any shared waveform setting
  (visual gain, until-mark and stem options), mouse, touch and wheel input or enter and leave on the
  viewer, show and resize of the viewer, and **show, resize, expose and screen or DPR change of the native
  `QOpenGLWindow`** (an event filter in `WGLWidget`). Recreating widgets (waveform type or options) resets
  the gate, and the first visible frame always renders.
- **(d) Safety.** At most one refresh every 500 ms (2 Hz) while visible and unchanged.

Only waveforms rendered in the last pass are swapped (`m_pendingSwap`). Hidden waveforms (decks 3 and 4
in a two-deck layout, other pages) are neither rendered nor swapped. A waveform that becomes visible
again always renders its first frame, so a skipped swap can never leave stale or black content after
expose, resize or show. Qt's own expose repaint of the `QOpenGLWindow` still paints the current state
too. Spinny and VU-meter GL signals, `waveformUpdateTick`, `VisualsManager` and `GuiTick` are unchanged.

Expected effect: with two paused decks, waveform renders and swaps drop from 120/s to about 4/s (the
safety refresh). Playing decks render as before.

## 2. Overviews

Verified rather than throttled. `WOverview` repaints only when the cursor moves a whole pixel
(`onConnectedControlChanged`), or when content changes (track, waveform summary or analysis progress,
cues, mark ranges, end of track, rate, passthrough). The engine publishes `playposition` at 15 Hz and
no-op sets do not emit a signal, so a paused overview never repaints, and a playing one repaints about
2 to 3 times a second at Deck Lab widths. The c52e3da skin binding uses this path. One bug fixed: while a
cue was hovered or the time ruler was shown, `m_iPosSeconds` held the sample index instead of seconds, so
the overview repainted at 15 Hz instead of once a second. Deck Lab overviews are transparent for mouse
input and never hover, so this matters only for other skins.

## 3. Perf logging (`DECKLAB_PERF_LOG=1`)

```
DECKLAB_PERF waveform [Channel1] rendered=20 (forced=1 moving=0 changed=0 safety=19 ungated=0) skipped=581 hidden=0
DECKLAB_PERF render-loop ticks=600 mean=0.412ms max=2.950ms dropped=0 gate=on
```

A tick is one render slot plus the following swap slot on the GUI thread. That covers waveform pre-render,
render and swap, spinnies and VU meters, `VisualsManager` and `GuiTick`. `dropped` is the
`VSyncThread` dropped-frame delta over the window.

## 4. Audio-thread pinning and priority (`DECKLAB_AUDIO_CPU`, `DECKLAB_AUDIO_PRIO`)

`src/soundio/decklabthreadpolicy.{h,cpp}` (new) and `src/soundio/sounddeviceportaudio.*`. `open()`
resolves the variable on the main thread, only for the clock-reference device, whose callback runs the
engine. It accepts a number or `last`, checked against the CPUs in the process affinity mask, so it is
the same on a Pi 4, a Pi 5 or another ARMv8 board. The callback's existing
first-callback setup block consumes an atomic flag and calls `pthread_setaffinity_np` once per stream. It
logs once (`DECKLAB_AUDIO_CPU: audio callback thread pinned to CPU 3`) and never repeats in the steady
state. Linux only. A network-clock reference device is not pinned; the appliance does not use one.

`DECKLAB_AUDIO_PRIO` works the same way: parsed on the main thread, consumed through its own atomic flag
in the same first-callback block, one `pthread_setschedparam(SCHED_FIFO, n)` per stream, logged once.
A thread already at that real-time priority or higher is left alone. On `EPERM` (no rtprio limit) it
logs once, with the current priority, and continues unchanged. It runs before Mixxx's existing
"not scheduled with SCHED_FIFO" check, so that check reflects the result. Pinning is what puts the
engine onto the CPU that os-image isolates (`isolcpus`/`nohz_full`). Without it, that CPU would sit idle,
so the appliance should use `DECKLAB_AUDIO_CPU=last` together with the isolation arguments.

## 5. Analyzer priority (`DECKLAB_ANALYZER_IDLE`)

At the start of `AnalyzerThread::doRun()` (`src/analyzer/analyzerthread.cpp`, the single allowed hook),
after `WorkerThread` has applied its Qt priority: `pthread_setschedparam(SCHED_IDLE)`, falling back to
`setpriority(PRIO_PROCESS, tid, 19)`. The thread logs which policy it got. This covers both batch
analysis and the analyze-on-load scheduler, which share `AnalyzerThread`.

## 6. LV2 safety (`src/effects/backends/lv2/lv2effectprocessor.*`)

- `processChannel()` no longer calls `lilvInstance()`, which instantiated on the audio thread. It reads
  `instance()`. If the instance is null, it copies input to output and returns, so `lilv_instance_*` is
  never called with null.
- A failed instantiation in `createSpecificState()` now warns instead of debug-asserting. The main-thread
  `prepareSpecificState()` retries it.
- **Devices closed are enforced:** `SoundManager` publishes `SoundDevice::openDeviceCount()` after it
  opens or closes devices. The LV2 prepare path refuses, with a warning, while any device is open. The
  state keeps its old instance, and the existing rate-mismatch pass-through keeps that deck dry until the
  next preparation.
- `SoundManager::setupDevices()` now closes any devices still open from a partly failed earlier attempt
  (the main-window retry loop) before it prepares and reconfigures. Before this, that path could race
  `prepareAudio()` with a live callback.

## 7. Performance-effect DSP (`src/effects/decklab/performance.c`)

Optimisations that keep the output audibly identical:

- State is copied into locals and the ring buffers are `restrict`.
- Ring indices wrap with a compare instead of four `%` per frame.
- The ducker beat gate uses a double-precision rotating phasor instead of per-frame `cosf`. It is
  re-anchored to the exact `cosf` every 64 frames and computed only while mode 5 is audible, including
  during its fade.
- Echo taps are read only in delay modes, and only for bands whose ring holds an echo or receives a
  send. Per-band liveness counters skip rewriting rings known to be all zero. They are bookkeeping and
  never affect the output.
- The old/new mode crossfade is evaluated only while a fade runs.
- In release mode the band output is thrown away, so only the ring state is updated. The isolator result
  is computed only where it feeds a held lever's echo.
- **Per-block ramps:** a smoothed control at its fixed point stays constant for the block. That includes
  the delay time and its tap indices, and it is bit-exact. A true linear per-block ramp was not used
  because the 8 ms slews are strongly exponential inside a 23 ms block, so it would not sound the same.
- **Dry pass-through:** lever and isolator tails below -120 dB snap to zero once their target is zero.
  With every section inactive, no release and silent echo rings (the release tail and echoes have ended),
  the block takes a dry path. It keeps every state variable running, including the release ring, beat
  phase, delay smoothing and band and filter states, but skips the band processing. The amount knob is
  irrelevant when no section is active.

Build: on aarch64 the `decklab-performance` target gets `-O3 -march=armv8-a -mtune=cortex-a72
-fno-math-errno`: baseline ARMv8-A code, so the single aarch64 Flatpak runs on the Pi 4, the Pi 5 and
other ARMv8 boards, with scheduling tuned for the Pi 4 baseline. It does not get `-ffast-math`, so the
`isfinite` guards keep working. The target is still defined before Mixxx's
global `add_compile_options(-ffast-math ...)`, so it does not inherit that.

**Regression evidence.** The pre-change processor is kept verbatim as `tools/decklab/performance_reference.c`.
It is built as `performance_reference.so` with the default flags and never installed. `test_dsp.py` runs
both on 18 generated stereo scenarios at 44.1 and 48 kHz with 1024-frame and odd (1024/333/64/1) blocks.
The scenarios are: every lever and isolator mode, mode crossfades, all release types, tempo, beat-fraction
and phase changes, everything at once, idle to active to idle, and NaN and inf input.

Local run (x86-64 WSL, gcc 13):

| Scenario | Max \|optimised - reference\| |
|---|---|
| idle, lever-solo, iso-filter, release-fade-in, release-with-held-lever, tempo-and-fraction, everything-hot, nan-and-inf-input | 0 |
| lever-echo, lever-short-echo, iso-short-echo, iso-drive, mode-crossfades, release-echo, release-mute, idle-active-idle | 1.5e-8 to 4.5e-8 |
| iso-cut-and-echo | 6.0e-8 |
| iso-ducker (phasor) | 3.95e-7 |

The gate is 1e-4 (-80 dBFS). The worst case is -128 dBFS.

Median `run()` time per 1024-frame stereo block at 44.1 kHz (x86-64 WSL; not gated):

| Case | Reference | Optimised |
|---|---|---|
| idle (all sections off) | 0.072 ms | 0.015 ms (4.8x) |
| lever echo, low+mid | 0.075 ms | 0.051 ms (1.5x) |
| isolator ducker | 0.074 ms | 0.046 ms (1.6x) |
| release tail | 0.109 ms | 0.039 ms (2.8x) |
| all sections hot | 0.092 ms | 0.071 ms (1.3x) |

The ARM64 CI runner (a server-class ARMv8 core, not a Pi) gives the same picture: idle 0.069 -> 0.015 ms
(4.5x), lever echo 0.074 -> 0.048 ms (1.5x), ducker 0.077 -> 0.047 ms (1.6x), release tail 0.076 ->
0.038 ms (2.0x), all sections hot 0.088 -> 0.068 ms (1.3x). On the Pi's Cortex-A72 the removed `udiv`
and `cosf` cost relatively more. The earlier review measured 0.58 ms median there, so expect roughly
0.1 to 0.4 ms. That has to be confirmed on the Pi.

**Heartbeat deadman: 0.65 s -> 1.5 s.** The UI bumps the heartbeat every 100 ms. Expiry latches the
effect dry until it is re-enabled, so a GUI stall of more than 0.65 s during a track load would kill
the FX mid-set. 1.5 s rides out such stalls, and a frozen or lost owner still goes dry within about
1.5 s. No safety reason to keep 0.65 s was found: the deadman only protects against a stuck wet effect,
and 1.5 s of a stuck echo or filter is acceptable. The stale counter cap moved from 1 s to 2 s so the new
threshold is reachable. `test_dsp.py` checks that a 1.4 s stall keeps the effect and that expiry is dry.

**Decks-ui / coordinator must know:** `WDeckLab::tickEffects()` (`wdecklab.cpp`, about line 2124) still
calls `releaseEffects()` when its own tick gap exceeds **550 ms**. After a GUI stall the UI therefore
releases the effect itself when it recovers, whatever the DSP deadman says. To actually tolerate stalls,
the owner of `wdecklab.cpp` should raise that 550 ms to about 1500 ms. Only the DSP constant changed here.

## 8. Tests

`src/test/decklabperfenginetest.cpp` (new; added to CMake and to `test_suites` in
`tools/decklab/pi-build-lock.json` so CI runs them):

- `DeckLabRenderGateTest` (8): first frame, skip when unchanged, changed state, moving decks, 2 Hz
  safety, hidden and re-shown, forced requests, the kill switch, and the fingerprint.
- `DeckLabRendererStateTest` (1): a real `WaveformWidgetRenderer` driven through `VisualPlayPosition`.
  Sub-pixel jitter keeps the key. Whole pixels, zoom, size, marker and gain change it. A play or scratch
  rate means continuous rendering. Resize and track changes request a frame.
- `DeckLabThreadPolicyTest` (4): `DECKLAB_AUDIO_CPU` (`last`, usable indices, restricted cpusets) and
  `DECKLAB_AUDIO_PRIO` parsing, defaults off, and (Linux) that pinning, the `SCHED_FIFO` raise (or a
  clean `EPERM` failure) and `SCHED_IDLE` or nice 19 affect only the calling thread.
- `DeckLabLv2SafetyTest` (2): a null instance passes audio through for every enable state without
  instantiating, and preparation is refused while a device is open and instantiates once devices are
  closed.

`tools/decklab/test_dsp.py`: new deadman timing, the reference-equivalence regression, and the benchmark.

## CI

Workflow `decklab-pi.yml` (ARM64 Flatpak build, native tests, DSP tests, packaged-app smoke):

| Run | Commit | Result |
|---|---|---|
| 39798990472 | 0f06d18fc5 | Failure before building. The checkout used `fetch-depth: 50`, and `codex/native-appliance` is already 47 commits past the pinned upstream revision, so `build_pi.py`'s ancestry check failed. Fixed in 375429e8b1 (`fetch-depth: 0`). **This affects every workstream branch with more than 3 commits.** |
| 39799771383 | 375429e8b1 | **Success.** 277 native tests, 0 failures. All 14 new perf-engine tests passed. DSP regression worst 3.97e-7 on ARM64. Smoke check passed. |
| 39805032822 | 6b2d285f2e | Final run, including `DECKLAB_AUDIO_PRIO` (result in the coordinator report) |

## Risks

- **Missed redraw.** If some drawn state is not in the fingerprint and has no signal, a paused waveform
  shows it up to 500 ms late (the safety refresh). Nothing is ever lost. Field fallback:
  `DECKLAB_RENDER_GATE=0`.
- **Stale front buffer with XWayland.** If the compositor ever drops a child window's buffer without
  sending Expose, the waveform could look blank until the next safety refresh, 500 ms at most. Watch for
  this on the Pi after page switches and layout changes.
- **Moving detection relies on `VisualPlayPosition` rate.** If the audio device stops while a deck was
  playing, the last rate stays non-zero and that deck keeps rendering every frame. That is no worse than
  upstream.
- **DSP.** Output differs from the reference by at most 4e-7 in the tests. Not bit-exact: there is the
  64-frame phasor and the -120 dB snap. Built for baseline ARMv8-A, so it runs on any ARMv8 board.
- **Merge hot spots.** `CMakeLists.txt` (new sources, `decklab-performance` flags, reference target) and
  `tools/decklab/pi-build-lock.json` (test suites and input hashes). Other workstreams touch both.
  Re-run `python3 tools/decklab/build_pi.py --check-inputs` after merging and refresh the hashes.

## Measure on the Pi

1. Idle, two decks paused, Decks page: `top -H -p $(pgrep -f mixxx.bin)` for the GUI thread and
   Xwayland, and `grep v3d /proc/interrupts` twice, 10 s apart, for IRQ/s. Compare with the baseline.
   Expect the GUI thread well below 10% and IRQs close to the vblank rate.
2. Same with `DECKLAB_RENDER_GATE=0` for an A/B on the same build.
3. `DECKLAB_PERF_LOG=1`: paused decks should show `skipped` about 580 and `safety` about 20 per 10 s.
   Playing decks should show `moving` about 600. Render-loop `max` and `dropped` with four decks playing.
4. Touch-scratch a paused deck's waveform. It must feel identical: no lag on the first touch, no stutter.
5. While paused, set or delete hotcues and loops, change loop size, move the beat grid, turn EQ knobs,
   zoom, switch layouts or pages, open and close dialogs over the waveforms, and load a new track
   (watch the analysis fill in). Every change must appear immediately, with no black or stale waveform.
6. `DECKLAB_AUDIO_CPU=3` (or `last`): check the log line and `ps -L -o tid,psr,rtprio,comm -p <pid>`
   (the engine thread should show PSR 3 and its FIFO priority). Check xruns (`audio_latency_overload`)
   over a long set. On a Pi 5, confirm `last` resolves to CPU 3 as well.
7. `DECKLAB_ANALYZER_IDLE=1`: `chrt -p <analyzer tid>` should show `SCHED_IDLE` while analyzing, or
   `ps` nice 19.
8. `DECKLAB_AUDIO_PRIO=70`: the log line, then `ps -L -o tid,rtprio,psr,comm` shows rtprio 70 on the
   engine thread. Without the rtprio limit, expect one `EPERM` warning and FIFO 1.
9. Performance effect: the audio-load readout (`audio_latency_usage`) with one or two units active versus
   the previous build. Also a stall test: stall the GUI for about 1 s, for example with a long track
   load (stopping the whole process also stops audio, so it doesn't work for this). Confirm the effect
   stays on. This also needs the `wdecklab.cpp` 550 ms change described above.
