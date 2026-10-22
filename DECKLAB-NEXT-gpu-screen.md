# Deck Lab next: gpu-screen spike (QML on EGLFS/KMS)

Branch `decklab/gpu-screen`, based on `origin/decklab/getmusic-plus` (4f5786df38). **Experimental. Do not merge as is.**

**Question.** Can the performance screen be one GPU-drawn Qt Quick scene, shown directly on the display through
EGLFS/KMS with no X server, no compositor and no native window per widget? If so, what does it cost and save on a Pi 4,
and what is the path?

**Short answer.** Yes, technically. Mixxx's QML layer builds in the Deck Lab Flatpak and runs (CI run 39859468647:
QML smoke green). It draws the real scrolling waveforms in the Qt Quick scene graph, and the Pi's KDE 6.10 runtime
ships `eglfs` and `eglfs_kms`. A Deck Lab Decks screen prototype with real controls, touch scratch and pinch zoom runs
in that scene. While paused it drew 3 frames in 3 s. But upstream's QML UI only covers decks, mixer and a flat track
list. Browse, Settings, system pages and every dialog would have to be rewritten, which is about 17 to 22
engineer-weeks. Most of the CPU win (no X, one swap per frame, nothing drawn while
idle) can be had sooner and more cheaply. **Recommendation: (b) now, and (a) later only if the Pi measurement backs it.**
Details are in section 6.

## 1. Survey: Mixxx's QML layer at this revision

Upstream base 9dedd4a80a (2026-08-26, 2.6 development). The relevant files are `res/qml/**` (55 QML files),
`src/qml/**` (21 classes) and `src/rendergraph/scenegraph`.

### Build and launch
- `option(QML "Build with QML" OFF)`. Upstream keeps it off because the rendergraph sources are compiled twice, once
  as `rendergraph_gl`/`allshader_gl` in `mixxx-lib` and once as `rendergraph_sg`/`allshader_sg` in `mixxx-qml-lib`.
  Shared headers such as `WaveformWidgetRenderer` (which holds `rendergraph::Context*`) are then defined differently
  in two places, an ODR violation (mixxxdj/mixxx#14766). `-DQML=ON` adds `MIXXX_USE_QML` to every translation unit,
  needs Qt Quick, QuickControls2, QuickShapesPrivate, LabsQmlModels and ShaderTools, and installs `res/qml`.
- Launch: `mixxx --qml`. `QmlApplication` creates `CoreServices`, opens audio, builds a hidden `DlgPreferences`
  (its constructor still initializes effects) and loads `<resources>/qml/main.qml` in a `QQmlApplicationEngine`. Saving
  a QML file reloads it (`QmlAutoReload`). The scene graph is forced to the OpenGL RHI backend.

### What is bound
- **Controls.** `Mixxx.ControlProxy {group; key}` reads and writes any control object (value or parameter). Upstream
  skins bind about 55 keys: play, cue, sync, rate, bpm, hot cues, loops, intro/outro, EQ, gain, crossfader, VU,
  effects, samplers, microphones and auxiliary inputs.
- **Singletons/models.** `PlayerManager` (`getPlayer(group)`, load a location into a deck or the next free deck),
  `Player` (title, artist, album, key text, colour, cover URL, `beatsModel`, `hotcuesModel`, `stemsModel`, waveform
  texture), `EffectsManager` (visible-effects model, quick-chain presets, `getEffectSlot`), effect-slot and parameter
  models, `Library` (one flat all-tracks model with title, artist, album, albumArtist and fileUrl roles),
  `Config` (hot cue and track colour palettes) and `PreferencesDialog.show()`.
- **Upstream skin.** It provides 4 decks (info bar, tempo slider, hot cues, intro/outro, sync), mixer, crossfader,
  effect units, samplers, mic/aux, the flat library and a developer-tools window. It was designed for 1920x1080
  with mouse input.

### Waveforms
- `Mixxx.WaveformDisplay` (`QmlWaveformDisplay`, a `QQuickItem` + `WaveformWidgetRenderer`) runs **the same
  allshader renderers as the widget UI**, built against `rendergraph_sg` so they become `QSGGeometryNode`s with
  compiled `.qsb` shaders. Renderers exposed to QML: RGB, Stem, Beat, Mark (cues, hot cues, loops, intro/outro,
  play marker, until-mark), MarkRange, EndOfTrack and Preroll. The Filtered, HSV and Simple renderers are compiled but
  not exposed. `Mixxx.WaveformOverview` is a `QQuickPaintedItem` (QPainter into a texture).
- Upstream's `WaveformDisplay.qml` scratches with a mouse drag; the position maths is a TODO.

### Library, effects, settings
- **Library.** A flat `ListView` of every track, with keyboard and controller navigation only. There is no sidebar
  (playlists, crates, USB, rekordbox), no search, no columns or sorting, and no track info.
- **Effects.** Usable: effect units, slots, parameter knobs and quick-effect chains.
- **Settings.** None in QML. "Prefs" opens the QWidget `DlgPreferences`.

### Known broken or risky
1. The ODR risk above (why QML is off upstream).
2. `QmlWaveformDisplay` calls `update()` on every `afterFrameEnd`, so a visible waveform keeps the scene rendering
   at full frame rate forever, paused or not. **Fixed in this spike** with a render gate (section 3).
3. It predicts the play position for a fixed 100 ms "sync interval", so the waveform runs about 80 ms ahead of the
   audio at 60 Hz. **Fixed in this spike**: one display refresh.
4. `init()` runs inside `updatePaintNode()`, which is the render thread with the threaded loop. It creates
   ControlProxies there and calls the main-thread-only `VisualPlayPosition::getVisualPlayPosition`. That works in
   practice but is not thread safe. Not fixed.
5. In QML mode `WaveformRenderMark` connects to the `WaveformWidgetFactory` singleton, which QML mode never creates
   (the CI log shows "invalid nullptr parameter" 10 times). Mark and until-mark setting changes are therefore not
   followed. This is harmless for the prototype. Not fixed.
6. **EGLFS-specific.** EGLFS allows one OpenGL window. A QWidget window (`DlgPreferences`, every `QMessageBox`
   from `ErrorDialogHandler`, the file chooser shown when there is no music folder) or a second QML `Window`
   (developer tools) cannot be shown next to the `QQuickWindow`. Qt documents that EGLFS does not support mixing
   them and terminates. All of these need QML replacements.
7. The Pi runtime (org.kde.Platform 6.10, Qt 6.10.3) has `eglfs`, `eglfs_kms`, `linuxfb`, `vkkhrdisplay` and
   **QtWaylandCompositor**, but **no Qt Virtual Keyboard** (read-only listing on the unit).

## 2. Gap table: Deck Lab feature to QML

Legend: **available** = works from QML today; **needs work** = the building blocks exist but Deck Lab behaviour must
be written; **missing** = no QML path, new C++ models or UI needed. "Spike" = done in this branch.

| Area | Deck Lab feature | QML status |
|---|---|---|
| Decks | Scrolling RGB waveform, beat grid, cue/loop/intro/outro marks, end-of-track | available (same allshader renderers) |
| Decks | Red playhead, overlays on the waveform (tags, chips, labels) | available: one scene, no native window (spike) |
| Decks | Render only while moving or changed | needs work: upstream draws every frame; gate in spike |
| Decks | Waveform touch scratch (one-to-one) | needs work: upstream TODO maths; spike does it as WWaveformViewer does |
| Decks | Pinch zoom, linked zoom | missing upstream; pinch in spike, linked zoom trivial |
| Decks | Hot cue letter labels A-H (skin opt-in) | needs work: `WaveformMark` has no letter option |
| Decks | Readouts: BPM, Lancelot key, remaining time at tempo, tempo %, state | available controls; formatting in spike (key table duplicated; expose `KeyUtils` instead) |
| Decks | Beat meter, harmonic hint against master, tempo-centre lamp | missing: needs `deckLabBeatPhase`/`KeyUtils::getCompatibleKeys` exposed |
| Decks | Transport: play, cue, sync, loop, key lock, quantize, slip | available (spike binds 6) |
| Decks | Hot cues A-H set/jump/colour | available (spike); cue popup (delete, colour, label) missing |
| Decks | Overview with needle search, hot cue markers | available; needle search in spike |
| Decks | 4-deck layouts, layer follow, `[DeckLabLeft/Right]` | needs work: logic lives in `wdecklab.cpp` |
| Decks | More panel: track info, beat-grid editor | missing |
| Decks | VU meters, status lamps (AUDIO OFF, OVERLOAD, REC) | available controls; SYSTEM lamp needs the decklabd client exposed |
| Browse | Sources sidebar: library, playlists, crates, USB drives, rekordbox | missing (upstream QML library is one flat list) |
| Browse | Track table: columns, sort, touch scroll, cover art | needs work: model has 5 roles, no BPM/key/duration; covers available |
| Browse | Search with touch keyboard | missing: no QML search; no Qt Virtual Keyboard in the runtime |
| Browse | Load to deck 1-4, match filter, USB eject/read-write | load available; filter and USB (decklabd) missing |
| FX | Lab FX performance effect page | needs work: effect slot and parameter proxies exist |
| FX | Controller FX unit lamps and details | needs work |
| Sampler | Sampler page | needs work: upstream `Sampler.qml` exists |
| Settings | Audio output, controller setup and mapping match | missing: QWidget only |
| System | Wi-Fi, Bluetooth, brightness, time zone, storage, power (decklabd) | missing UI; the C++ client can be reused |
| System | First-run flow, banners, About | missing |
| Platform | Direct to display without X (EGLFS/KMS) | available in the runtime (verified on the unit) |
| Platform | Touch (evdev/libinput) | available; udev inside the Flatpak is uncertain, so the script passes the device |
| Platform | Portrait official 7" v2, 1024x600, 1280x800 | letterbox in spike; portrait needs KMS `rotation` or a QML rotation |
| Platform | Errors and dialogs (`ErrorDialogHandler`, `QMessageBox`) | broken on EGLFS: needs a QML dialog route |
| Platform | Get Music / Sample Maker (`chromium --kiosk` on `DISPLAY=:0`) | missing: no X. Options: QtWaylandCompositor inside Deck Lab, cage only while a utility is open, or QtWebEngine (not in the runtime) |
| Platform | Graceful SIGTERM | spike |

## 3. What the spike adds

Commits on `decklab/gpu-screen`:

| Commit | What |
|---|---|
| 86cb8235a2 | `DECKLAB_QML=1` makes `build_pi.py` build `-DQML=ON -DBUILD_TESTING=OFF` on Flatpak branch **`decklab-qml`**, install the runtime's GL extension, and capture the QML screen under Xvfb (Mesa llvmpipe). The workflow gets a `qml` dispatch input that runs this as a **separate job** (`build-qml`, own cache and artifact). The product job, its gates and its outputs do not change. Unit tests cover the switch, the option rewrite, the GL extension lookup and the smoke gate. |
| 6c73e9b974 | QML-only C++ (`src/qml/*`, compiled only with `-DQML=ON`): the render gate, the refresh-interval prediction fix, `audioSamplesPerPixel()`, `DECKLAB_QML_MAIN`, graceful SIGTERM, `DECKLAB_PERF_LOG=1` fps logging and the CI smoke (`DECKLAB_QML_SMOKE_DIR`). |
| 5c8eb1f1ae | `res/qml/DeckLab/`: the Decks screen prototype. |
| a3cad8d401 | `tools/decklab/run-qml-eglfs.sh`. |
| 1df6f1e009, 938ef22c22, 356239ea43 | CI smoke fixes. Flatpak's `--nosocket=fallback-x11` also revokes `x11`, so the smoke no longer passes it. In the QML job only: IPv4 archive prefetch and download retries. Scene-graph errors are recorded instead of aborting. |
| 19f52a23b4 | `fdk-aac-free` from Fedora's lookaside mirror (the coordinator's fix from `decklab/pi4-perf`, same sha256, lock hash refreshed). |
| 63fa55408b | Run script logs at `info`. Mixxx's console default is `warning`, which hid the fps and GL lines. |

**The product build is unchanged.** Every C++ change is in files built only with `-DQML=ON`. `res/qml` is installed
only with `-DQML=ON`. `build_pi.py` without `DECKLAB_QML` builds the same manifest with the same options and branch
(`decklab`). The lock file refreshes the hashes of the edited inputs (workflow, `build_pi.py`, its test) and of the
`fdk-aac-free` module taken from pi4-perf.

### Render gate (QML waveform)
After each frame, a waveform asks for another only while its deck moves (visual play or slip rate is not 0) or a
renderer animates (end-of-track blink). While idle, a 20 Hz timer does three cheap lock-free checks and does not render:
- the visual play position (motion, or a whole-pixel seek or cue jump);
- a render request from track signals (cues, beats, waveform, analysis);
- 12 controls that change a paused waveform (loop, cue, intro/outro, rate, loaded, end of track, passthrough).

Zoom and resize also request a frame, and a 1 s safety frame covers anything missed. Qt Quick keeps the last
frame on screen, so with no request **nothing is drawn or swapped**. `DECKLAB_RENDER_GATE=0` restores upstream
behaviour (the same switch as the widget path).

### Prototype (res/qml/DeckLab, 800x480)
- 44 px rail: brand mark, Decks / Browse / Sampler / Lab FX / Settings / USB (the others are placeholders), a
  "QML PREVIEW" tag and the main L/R meter.
- Two 95 px waveform lanes: an 18 px deck tag in the deck colour, Mixxx's scene-graph waveform with the skin's
  colours, and a **red #FF3D2E playhead** drawn as an item above it. One finger **scratches** (scratch_position
  one-to-one, as `WWaveformViewer`). Two fingers **pinch-zoom** `waveform_zoom` (1 to 10). A SCRATCH chip and the
  zoom level overlay the waveform. Those overlays are trivial here and impossible over today's GL window containers.
  CI exercises tap and scratch with synthetic mouse input. Pinch needs two real touch points and is untested until
  the Pi run.
- Two deck strips: badge, state (PLAY/CUE/PAUSE/EMPTY), title, BPM 36/24 px, Lancelot key, remaining time at tempo
  (flashes red in the last 30 s), MASTER/SYNC/LOOP lamps, tempo %, a channel meter, the overview with played
  dimming, red needle and needle search while paused, hot cues A-H in their colours, and CUE (hold), PLAY (on press),
  SYNC, LOOP 4, KEY LOCK and QNT.
- Brand tokens are a singleton (`Brand/Brand.qml`). The Barlow and Barlow Semi Condensed files load from
  `skins/DeckLab/fonts`. Other screen sizes letterbox the 800x480 canvas.

## 4. CI evidence

Workflow `decklab-pi.yml`, dispatched with `-f qml=true` (the `qml` input needs that ref's workflow). Five runs:

| Run | Commit | Product job (gates) | QML job |
|---|---|---|---|
| 39852481763 | a3cad8d401 | **success** | built (cold: 978 steps, 18 min for the Mixxx module); smoke: Qt saw no `DISPLAY` |
| 39856968708 #1 | 1df6f1e009 | infrastructure: `fdk-aac-free` host timeout | same |
| 39856968708 #2 | 1df6f1e009 | **success** | infrastructure: same timeout |
| 39858448698 | 938ef22c22 | **success** | smoke: `x11` revoked by `--nosocket=fallback-x11` |
| 39859468647 | 356239ea43 | infrastructure: `fdk-aac-free` host timeout (after its input checks passed) | **success**, all smoke checks |

The product gates passed at three commits. After 938ef22c22 the only change on the product path is the mirror URL
from pi4-perf (19f52a23b4). Commits 19f52a23b4 and 63fa55408b are after the fifth run and have no CI run of their
own.

**Smoke result** (`qml-smoke.json`, Xvfb + Mesa llvmpipe 26.2.5, `platform xcb`, render gate on):

| Check | Result |
|---|---|
| Errors and warnings | 0 QML compile errors, 0 QML warnings; process exit 0 |
| Fixtures | 2 generated fixtures loaded and analysed in 501 ms (BPM 124 and 128); no key detected (`visual_key` 0, shown as "-") |
| Waveforms | `waveform_items` 2 |
| **Idle cost** | **3 frames in 3 s** with both decks paused: the 1 s safety frame. Upstream would draw 60 to 180. |
| PLAY tap | `play` became 1 |
| Touch scratch on deck 2 | `scratch_position_enable` 1 while held (`scratch_position` -90200 after a 60 px drag), 0 after release |

There was no audio device. Mixxx ran its engine on the "Network stream" clock, so deck 1 really played after the tap
(its time counts down and its VU lights).

**Screens.** Artifact `decklab-mixxx-qml-aarch64-356239ea438cb345815a9775d18602b842cfbc9f` of run 39859468647
(3-day retention). Copies are in the coordinator's local `gpu-screen-20261022` folder (beside `wt/`), which is
not published. Generated content only.
- `qml-decks-800x480.png`: both decks loaded and paused on their first-beat cue. RGB waveforms with the beat grid,
  preroll, an orange CUE mark and the red playhead. Readouts show `124.00 BPM` and `−0:39`. Overviews have the red
  needle. Hot cues A-H are unset; CUE and QNT are lit.
- `qml-decks-touch-800x480.png`: deck 1 is playing (green PLAY state and outline, channel meter lit, waveform moved
  on). Deck 2 is held mid-scratch with the amber SCRATCH chip over the waveform, and its CUE mark has moved off the
  playhead.
- `qml-decks-1024x600.png`: the same scene scaled 1.25x and letterboxed, still sharp, because the waveform is
  geometry rather than a texture.
- `qml-tab-browse-800x480.png`: tab switch to a placeholder page.

Polish seen in the captures (not fixed; the run budget was used):
- the BPM decimals sit a few pixels away from the whole number (Row spacing);
- "KEY LOCK" elides at 61 px;
- the smoke's mid-track seed is overwritten by the running engine, so the paused capture shows track starts;
- a few `VisualPlayPosition ... outdated position request` warnings appear when the idle gate checks the position
  long after the last frame. They are logged only on state changes, not continuously.

## 5. Running it on the Pi (coordinator)

`tools/decklab/run-qml-eglfs.sh`. It never touches the product Flatpak or the DJ's profile. It runs a copy in
`~/.cache/decklab-qml/profile`. The analysis cache is linked, not copied, so tracks are not re-analysed during the
measurement.

```sh
# 0. Install the CI candidate beside the product (branch decklab-qml):
flatpak install --user ./decklab-mixxx-qml-aarch64-<rev>.flatpak
# 1. From a text console (Ctrl+Alt+F2, log in as the DJ user) or SSH:
tools/decklab/run-qml-eglfs.sh check           # read-only: output, touch device, ref, kiosk state
tools/decklab/run-qml-eglfs.sh stop-kiosk      # sudo: flag + stop decklab-mixxx + stop getty@tty1 (Xorg ends)
tools/decklab/run-qml-eglfs.sh run 120         # idle 60 s, then press PLAY on both decks for 60 s
DECKLAB_RENDER_GATE=0 tools/decklab/run-qml-eglfs.sh run 60   # A/B: upstream every-frame waveforms
# 2. Exact revert:
tools/decklab/run-qml-eglfs.sh restore-kiosk   # rm /run/decklab/desktop-mode; systemctl restart getty@tty1
#    (a reboot also restores it: the flag lives in /run)
```

What `run` does:
- **Display.** `QT_QPA_PLATFORM=eglfs`, `QT_QPA_EGLFS_INTEGRATION=eglfs_kms`, and a generated
  `QT_QPA_EGLFS_KMS_CONFIG`. It picks the card with the connected DSI connector (`/dev/dri/card1`, `DSI1` 800x480
  on the unit) and turns every other connector off. It also sets `QT_QPA_EGLFS_ALWAYS_SET_MODE=1`, hides the cursor,
  and sets `GBM_BACKENDS_PATH` to the Flatpak GL extension's `gbm` directory.
- **Touch.** evdev with libinput disabled. The device comes from `/proc/bus/input/devices` using the kiosk's
  `TOUCH_MATCH` pattern (`10-0038 generic ft5x06`, `/dev/input/event4` on the unit). Override with
  `DECKLAB_TOUCH_DEVICE`.
- **Audio and threads.** It uses the DJ's sound config (the kiosk Mixxx must be stopped, because ALSA is exclusive),
  `DECKLAB_AUDIO_CPU=last` and `DECKLAB_ANALYZER_IDLE=1`.
- **Logs** go to `~/.cache/decklab-qml/run-<time>/`:
  - `cpu.log`: every 5 s, total and top-6 per-thread CPU % of mixxx.bin, v3d and vc4 interrupts/s, and the latest fps
    line, followed by a summary;
  - `mixxx.log`: `QSG_INFO` (render loop, GL renderer) and a `DECKLAB_QML_PERF` line every 10 s with window fps and
    renders per waveform;
  - `kms.json` and `command.txt`.
- It stops Mixxx with SIGTERM to `mixxx.bin` (bubblewrap does not forward signals). It leaves the kiosk stopped
  until `restore-kiosk`.
- Knobs: `QSG_RENDER_LOOP=basic|threaded`, `DECKLAB_KMS_ATOMIC=1`, `DECKLAB_DRM_OUTPUT`, `DECKLAB_DRM_CARD`.

Not verified on hardware (no deployment by agents): DRM master from SSH, `/run/udev` inside the sandbox, the GBM
path, and real-time priority outside the systemd unit (the script runs without the unit's `LimitRTPRIO`).
`check` and the first `run` will show any of these.

## 6. Estimate and recommendation

No agent deploys to the Pi, so the numbers below are **estimates from the code paths**, not measurements. One
`run-qml-eglfs.sh run 120` (idle, then playing) plus one run with `DECKLAB_RENDER_GATE=0` turns them into data in
about 15 minutes.

### Expected CPU: today against QML on EGLFS (Pi 4, 800x480, two decks)

| State | Today: widget UI on the Xorg kiosk | QML scene on EGLFS/KMS (expected) | Why |
|---|---|---|---|
| Both decks paused | GUI thread ~34% of a core (coordinator's figure) plus Xorg | GUI ≤ 3%, render thread < 1%, no X server | Qt Quick keeps the last frame, so with nothing changing no frame is drawn or swapped. The gate's checks are two 20 Hz timers reading atomics. There are 1 to 2 safety frames per second. The widget UI still runs its VSync loop, VU and readout timers, raster painting and X traffic. |
| Two decks playing | Each waveform is its own `QOpenGLWindow`: render, then **one swap per waveform per frame** through X (2 per frame, 4 with four decks), plus Xorg copying each window to the screen | GUI thread: scene-graph sync, which builds the waveform geometry with **the same allshader code** as today. Render thread (another core): about 150 batched nodes and **one** `eglSwapBuffers` and page flip per frame. No X. | The waveform geometry, the main per-frame CPU cost, is unchanged. The savings are X, the extra swaps and window copies, and widget repaints and uploads. The GPU work moves off the GUI thread. |
| Four decks playing | 4 swaps per frame | still 1 swap per frame | |

My estimate: about **30 points of a core saved while idle**, and **10 to 25 points across GUI and X while playing**
(more with four decks). V3D interrupts drop to one flip per frame while playing, and close to none while idle.
Expected costs:
- about 30 to 60 MB more memory (QML engine, Quick, shaders);
- 0.3 to 1 s more startup to compile the QML (ahead-of-time `qmlcachegen` removes most of this);
- one extra thread.

The gate matters. Without it (upstream behaviour, `DECKLAB_RENDER_GATE=0`), the QML scene redraws the whole screen
at 60 fps even while paused. That would likely be **worse** than today's gated widget UI.

The biggest qualitative advantage is not the average CPU. A scene-graph waveform can advance on the render thread
(in a node's `preprocess`, from the lock-free visual play position) without waiting for the GUI thread. A future QML
waveform could therefore keep scrolling smoothly while Browse or analysis keeps the GUI thread busy. The widget
architecture cannot do that. The spike does not implement it yet: the waveform still syncs from the GUI thread.

### Effort to feature parity with the widget UI

The widget UI is about 15,000 lines (`wdecklab.cpp`, `decklab*.cpp`, `src/decklab/**`) with about 80 native UI
regression tests. Estimates are for one experienced Qt/QML developer:

| Work | Weeks |
|---|---|
| Productise the QML build: resolve or isolate the ODR issue (#14766), `qmlcachegen`, CI gates, QML test harness | 2-3 |
| Decks parity: 2/4-deck layouts, layer follow, beat meter, harmonic hint, cue popups, More panel, beat-grid editor, lamps | 3-4 |
| Browse: C++ models for sources and the track table (roles, sort, search), a QML touch keyboard, USB eject | 3-4 |
| Lab FX, controller FX, Sampler | 1.5-2 |
| Settings, Deck Setup, system pages, first run (reusing the decklabd client and policies) | 3-4 |
| EGLFS platform: a kiosk unit instead of the Xorg session, touch/udev, rotation, QML error dialogs, utilities without X (QtWaylandCompositor or a cage hand-off) | 2-3 |
| Port the regression tests and smoke captures | 2 |
| **Total** | **about 17 to 22 weeks** (4 to 5 months) |

The prototype covers about a week of the Decks line. It shows that the hard parts work: real waveforms, touch
scratch, overlays and the idle gate.

### Options

- **(a) Adopt QML/EGLFS as the next-generation UI.** It is feasible and it is the best long-term architecture: one
  GPU scene, real multi-touch, overlays and animation for free, no X, a render-thread waveform, and a path to host the
  utilities through QtWaylandCompositor. But it costs 4 to 5 months before it matches what ships today, and it rests
  on an upstream UI that is off by default for an ODR risk, has no library or preferences, and still uses QWidget
  dialogs that EGLFS cannot show.
- **(b) A smaller step: one GL surface for all widget-UI waveforms.** Replace the per-waveform
  `QOpenGLWindow` + `createWindowContainer` (`WGLWidget`) with an FBO-backed `QOpenGLWidget` or `QRhiWidget` for the
  waveform lanes. The allshader renderers and the perf-engine gate stay. Qt then composites the window in one pass:
  - **one swap per frame**;
  - overlays become ordinary widgets, which **fixes the stacking bugs**;
  - and, the key point, a QWidget window with `QOpenGLWidget` content **is supported on EGLFS**, where raster windows
    are GL-composited (per Qt's EGLFS documentation; (b) should confirm it on the unit). Today's `QOpenGLWindow`
    children make that impossible.

  So the existing UI, its dialogs and its tests could later drop X too. Effort: 2 to 3 weeks in the perf-engine
  area, plus about 1 week for an EGLFS kiosk unit once the utilities have a non-X path. Risk: the whole 800x480 window
  is recomposited every frame while playing. That is cheap on V3D, but it must be measured; upstream moved away from
  widget-composited GL for desktop performance reasons.
- **(c) Not worth it yet.** Not my recommendation. The costs are real and measured: 34% GUI thread while idle, N
  swaps through X while playing, overlay stacking bugs. The cheap fixes are known.

**Recommendation: (b) now, with (a) as a deliberate later decision.**
1. Run `run-qml-eglfs.sh` on the unit (about 15 minutes). That measures the ceiling of a retained GPU scene without X.
2. Do (b) next. It gets most of the measurable win in about a sixth of the time and keeps 15,000 lines and 80 tests.
3. Keep the `decklab-qml` CI job (opt-in, no cost to product runs) and `res/qml/DeckLab` as the seed of a
   "Deck Lab 2" UI. Revisit (a) when either:
   - the Pi data shows the QML scene at least 15 points of a core lower than (b) while playing;
   - the product needs what only QML does well: render-thread waveforms that never stall, animated transitions, or
     embedded utilities;
   - or upstream's QML UI gains a real library and preferences.
