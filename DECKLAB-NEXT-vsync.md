# Deck Lab vsync-lock workstream (2026-10-22)

Branch `decklab/vsync` of the fork, based on `origin/decklab/pi4-perf` (44b962e3bf). Appliance side:
branch `decklab/vsync` of the appliance repo (`DECKLAB_VSYNC` in `config/kiosk/mixxx.env` and
`mixxx-launch`, see its `docs/next/vsync.md`).

Goal: waveform motion locked to the vblank and tear-free while anything moves, at about timer-mode cost
while idle, as the default of the Xorg kiosk.

## 1. Measured facts

- **The panel runs at 60.0742 Hz, not 60.029 Hz.** DRM vblank timestamps on the Pi 4 DSI panel
  (read-only `DRM_IOCTL_WAIT_VBLANK` on the primary node, no DRM master needed, script in section 7)
  gave a period of **16646.09 us** over 1,203 consecutive vblanks, with a largest fit residual of 41 us.
  60.029 Hz is the nominal mode rate that `wlr-randr` reports. The pixel clock the hardware really
  produces is a little faster.
- So timer mode (60.000 Hz, swap interval 0) slips against the panel once every
  1 / (60.0742 - 60.000) = **13.5 s**, not 34 s. Each slip shows one display frame with a repeated
  waveform position, and with swap interval 0 the frames can tear under Xorg.
- Upstream PLL **does** lock, but only by accident of the swap queue (section 2). Today's XWayland test log
  agrees with that: the loop held 16646.7, 16645.8 and 16645.6 us (the real period). After one 2.2 ms
  disturbance it moved to 16632.2 us, consistent with the drift that follows a drained queue.

## 2. Why the PLL works on X11, and why the brief's waveform-feed cannot

Each `QOpenGLWindow` swaps with swap interval 1 through Mesa DRI3, copy-mode Present, two back buffers.

- A swap returns at once and is shown at the next vblank. A paint waits inside the driver only when both
  back buffers are still queued.
- PLL init repaints the 3x3 shared window on every `frameSwapped`. That is faster than the display, so
  the queue fills and from then on **every paint waits until a vblank frees a buffer**. Each
  `frameSwapped` lands just after a vblank. That is the only vblank information the loop gets.
- Upstream then repaints once per slot, which keeps the queue full, as long as no frame is ever missed.
  One missed paint drains it for good. From then on every sample is the GUI thread's own timing, the
  loop locks onto itself, and its period drifts. In the model it went from 16658.6 to 16671 us within
  seconds, which means a hitch about every 22 s.
- **The waveform windows cannot be the reference.**
  - They swap through `QOpenGLContext::swapBuffers()` in `WGLWidget::swapBuffers()`, which never emits
    `QOpenGLWindow::frameSwapped`. Only `QOpenGLWindow`'s own paint path does.
  - Timed directly, their swaps never wait, because each swaps once per frame and its queue never fills.
    So their timestamps are the slot's own wake-up time. Fed to the loop, it locks onto itself (the
    failure above).
  - Making them wait would mean keeping their queue full, which adds two frames (33 ms) of waveform
    latency.

  So the brief's step "feed the PLL from the waveform windows' frameSwapped" is not implemented. The
  shared window stays the reference, and it repaints only while something moves.

A discrete-event model of this swap path, written before the code, reproduced both upstream
behaviours: lock while the queue stays full, and drift after one missed paint. The C++ port of it is in
`DeckLabVSyncTest`.

## 3. What changed (fork)

| File | Change |
|---|---|
| `src/waveform/vsyncpll.{h,cpp}` (new, pure) | `VSyncPll` loop filter, `VSyncKeepalive` policy, `VSyncLockWatchdog`, slot schedule, `DECKLAB_VSYNC` parsing |
| `src/waveform/vsyncthread.{h,cpp}` | `runPLL` uses `VSyncPll`; new slot schedule; `updatePLL(vblankReferenced)`, `markSlotStart()`, `fallBackToTimer()`, `pllSnapshot()` |
| `src/waveform/waveformwidgetfactory.{h,cpp}` | keepalive gating, paint timing, startup watchdog, env override in `startVSync()` and `getSurfaceFormat()`, perf log line |
| `src/test/decklabvsynctest.cpp` (new) | `DeckLabVSyncTest`, 16 tests, simulated clocks, no GL |
| `CMakeLists.txt`, `tools/decklab/pi-build-lock.json` | new sources; the lock hash and `DeckLabVSyncTest.*` in `test_suites` |

Behaviour, ST_PLL only. Timer, free-run and QML are unchanged.

1. **Samples.** The factory times each shared-window paint, from the `UpdateRequest`, `Paint` or
   `Expose` event to `frameSwapped`, through an event filter on that window. A paint of at least 2 ms waited for a
   vblank (a full queue): in steady state it waits about 10 ms; one that did not wait takes well under
   1 ms. Only waited-for paints feed the loop, one per frame (anything within half a period of the last
   sample is a duplicate). Init is unchanged from upstream: it accepts every sample and needs 20 stable
   deltas.
2. **Keepalive.** The shared window repaints once per slot (upstream geometry: `update()` at the slot,
   paint 5 ms later) only while something is active:
   - a render-gate decision of Forced, Moving, Changed or Ungated in the last render pass. A Safety
     refresh does not count; it is a single frame and needs no lock.
   - or a spinny or GL VU meter is connected. They render every frame, so the keepalive stays on, as
     upstream.

   It keeps running for **500 ms** after the last activity, then stops; idle, nothing repaints for the
   loop.

   - After the startup lock it also runs for **30 s** (the settle time), so upstream's filter can
     refine the 20-frame init average (good to 5 to 15 us) before the first freewheel.
   - A paint that did not wait (resume, a stall) is followed at once by another one, up to 4 per slot,
     which refills the queue within a frame; upstream never refilled.
   - While idle, a **250 ms refresh once a minute** keeps the freewheeled phase within a fraction of a
     millisecond.
3. **Freewheel and resume.** With no samples, slots keep coming on the last period. When samples resume
   after more than 6 periods, the loop keeps the period and pulls the phase in with gain 0.1 for 60
   samples (one second; 0.9^60 leaves 0.2 %), then hands back to upstream's filter (alpha 0.01,
   beta = alpha^2 / 2). The slots move by at most a tenth of the remaining error per frame, so a
   1.5 ms error shifts the waveform by at most 0.15 ms of motion per frame. Only if the error left the
   slots before the real vblank does the phase snap, crossing it once (one repeated frame instead of
   several). The upstream drop counter and offset adaptation are unchanged.
4. **Slot schedule** (`VSyncPll::nextSlotMicros`).
   - Slots run on the grid `phase + 1.5 ms + offset + n * period`. The 1.5 ms lead puts the slot just
     after the vblank instead of on it (upstream), so a freewheel error up to 1.5 ms cannot push it
     across the vblank.
   - The grid follows corrections in both directions. Upstream's `nextSwap` only ever moved later, so
     after a backward correction it kept the old schedule.
   - No vblank interval gets two slots: when the GUI thread started a slot late, into the next
     interval, the next grid point in that interval is skipped. The interval boundaries come from the
     last waited-for sample, so they are right even while the phase is still being pulled in.
5. **`DECKLAB_VSYNC=pll|timer`** (case-insensitive, blanks ignored) overrides `[Waveform] VSync`:
   - in `startVSync()` for the loop;
   - in `getSurfaceFormat()` for the swap interval (pll: 1, timer: 0).

   Unset or empty keeps today's behaviour. Any other value logs one warning and is ignored. The mode is
   logged once at startup (section 6).
6. **Startup safety.** If the loop has not locked after 3 s of usable time, Mixxx falls back to the timer
   loop and logs it. Usable time is a 100 ms GUI timer, each tick counting at most 250 ms and only while
   the shared window is exposed; 10 s in total if the window is never exposed. A skin load that blocks
   the GUI for seconds therefore does not count. Covered: no `frameSwapped` at all, or `frameSwapped`
   without vblank pacing. The swap interval stays as the contexts were created (1). No shared GL window
   at all means an immediate fallback.

## 4. Pacing math

- **Panel:** T = 16646.09 us (60.0742 Hz). Timer at 60.000 Hz: a slip every 13.5 s. Upstream PLL after a
  drained queue: the period wanders by about 10 us, a hitch every 20 to 30 s.
- **Loop:**
  - alpha = 0.01 and beta = 5e-5 give a natural frequency of sqrt(beta) = 0.0071 rad per frame,
    damping alpha / (2 sqrt(beta)) = 0.71, and a time constant of about 200 frames (3.3 s).
  - After 30 s of lock with +-150 us of sample jitter the period is off by 0.03 us
    (`FreewheelPhaseErrorAfterIdleStaysUnderHalfAFrame`).
- **Freewheel phase error** (test output, both 60.029 and 60.0742 Hz):

  | Idle | Error |
  |---|---|
  | 1 s | 9 us |
  | 10 s | 26 us |
  | 60 s | 124 us |
  | 300 s | 592 us |
  | 600 s | 1178 us |

  Half a frame is 8323 us. With the 60 s idle refresh, a resume never sees more than about 0.15 ms.
- **Re-lock:** the first 2 to 3 keepalive paints after a resume do not wait (the queue is refilling)
  and are ignored. The first waited-for paint arrives 2 to 3 frames after the keepalive restarts, which
  is one slot after the first moving frame. The slots never stop meanwhile: they run on the freewheeled
  grid, so the first moving frames are paced like any other.
- **Latency:** from the slot's swap to the screen is T - 1.5 ms - offset, about 15 ms. Upstream's slot
  sat on the vblank, which gave about 16.4 ms.
- **Cost:**

  | State | Keepalive paints | GUI thread waiting in the driver |
  |---|---|---|
  | Active, and the first 30 s after startup | 60/s, as upstream | about 10 ms per frame, as upstream |
  | Idle | 0, plus 15 per minute (refresh) | none |
  | Upstream PLL, idle | 60/s | about 10 ms per frame |

## 5. Tests (`DeckLabVSyncTest`, in `test_suites`)

| Test | What it shows |
|---|---|
| `EnvironmentOverrideParsing` | pll/timer/empty/invalid, case and blanks; override vs config; reads the real environment |
| `LocksToThePanelRate` | init through unqueued paints, then lock at 60.029 and 60.0742 Hz: period within 0.05 us, phase within 100 us after 20 s |
| `IgnoresPaintsThatDidNotWaitAndDuplicates` | unqueued and same-frame samples change nothing |
| `FreewheelPhaseErrorAfterIdleStaysUnderHalfAFrame` | 1 s to 600 s of idle at both rates: under half a frame and under the 1.5 ms lead |
| `ResumePullsThePhaseBackGraduallyAndKeepsThePeriod` | 7 ms error: a tenth per frame, period untouched for 60 samples, then normal |
| `SlotScheduleFollowsCorrectionsAndNeverDoublesAnInterval` | grid, backward corrections, the late-slot skip, boundaries during a pull-in |
| `KeepaliveStopsWhenIdleAndRefillsTheQueue` | 30 s settle, start/stop/500 ms tail, refill budget, refreshes at 60 s, 120 s and 180 s |
| `WatchdogFallsBackWhenNoFrameSwappedArrives` | no frameSwapped: 3 s; a blocked GUI counts 250 ms per tick; hidden window: 10 s; unstable frameSwapped |
| `ClosedLoop*` (6 tests) | VSyncThread schedule + factory keepalive + VSyncPll against the swap-queue model (see below) |
| `ThreadEmitsSlotsOnlyAfterLockAndFreewheels` | the real `VSyncThread`, real time: no slots during init, slots after lock, slots continue without samples |
| `ThreadFallsBackToTheTimerLoop` | the real `VSyncThread` in ST_PLL without samples: `fallBackToTimer()` switches to the timer loop |

The `ClosedLoop*` tests run the swap-queue model with GUI work, jitter and stalls. Each checks every play
window for repeated frames (a vblank with no new frame), queued frames (shown later than the vblank after
their swap), and the frame-to-frame change of shown time minus predicted time (a visible jump):

| Scenario | Result |
|---|---|
| Lock at 60.029 and 60.0742 Hz, 60 s idle, resume | 0 repeated, 0 queued, jumps under 500 us (the Python prototype of the model: 0 to 4 us), period exact, no keepalive paint while idle |
| Idle from startup through the settle time, first play at 50 s | same |
| Freewheel errors of +-0.7 ms, +-1.4 ms and -3 ms | same |
| Errors that leave the slots before the vblank (+3 to +8 ms) or 9.7 ms after it (-8 ms) | at most 1 repeated frame, never a queued one |
| 40 ms and 25 ms GUI stalls during play, 9 ms of render work per frame | at most 3 repeats; a stall longer than a frame cannot be hidden. The lock recovers and nothing queues. |
| 1.5 ms of jitter | smooth |

The same tests built and passed locally (g++ 13, Qt 6.4, `-Wall -Wextra -Wfloat-conversion`, no warnings).

## 6. Logs

At startup (Info). The numbers are an example:

```
DECKLAB_VSYNC mode=pll source=env DECKLAB_VSYNC=pll config=0 swap_interval=1 fps=60
DECKLAB_VSYNC pll locked period=<us> rate=<Hz> after <ms> ms (<n> frameSwapped)
```

Must not appear (Warning): `DECKLAB_VSYNC pll did not lock within ... falling back to timer mode`, or
`DECKLAB_VSYNC=<x> is not pll or timer`.

With `DECKLAB_PERF_LOG=1`, every 10 s, a new line before the existing `pacing` line:

```
DECKLAB_PERF vsync mode=pll state=locked|freewheel|init|fallback period=<us> rate=<Hz> err=<us>
  referenced=<n> unreferenced=<n> duplicates=<n> relocks=<n> snaps=<n> skipped=<n>
  keepalive=on|off paints=<n> refills=<n> starts=<n> stops=<n> refreshes=<n>
```

All counts are per 10 s window. `err` is the last phase error. `paints` is keepalive repaints per slot,
`refills` the extra ones after a paint that did not wait, `skipped` slots moved on so that no vblank got
two. The existing `phase-locked-loop:` debug line (every 10 s) and the `DROP` lines are kept.

## 7. On-Pi verification plan (coordinator)

Setup:

- Flatpak from the CI artifact of the final run (section 9), Xorg kiosk session.
- In `~/.config/decklab/mixxx.env`: `DECKLAB_PERF_LOG=1`. `DECKLAB_VSYNC` comes from the appliance
  default `auto`, which gives `pll` on Xorg.
- Log: `<profile>/mixxx.log`.
- For the timer baseline, the same runs with `DECKLAB_VSYNC=timer`.

A. **Startup.** `grep DECKLAB_VSYNC mixxx.log`:

- `mode=pll ... swap_interval=1` and `pll locked period=166xx.xxus rate=60.0xxxHz` within a second or two
  of the skin appearing;
- no `did not lock`.

The `period` should match the DRM measurement below (16646.09 us on the test Pi) to within about
0.1 us after a minute of play.

B. **Idle, 20 s** (decks empty, then two decks loaded and paused). Measure at least 30 s after Mixxx
locked: the keepalive deliberately runs through the settle time after startup.

| Check | Target |
|---|---|
| `DECKLAB_PERF vsync` | `state=freewheel keepalive=off paints=0 referenced=0` (a window holding the once-a-minute refresh shows `refreshes=1 paints≈15`) |
| CPU, `top -b -d 20 -n 2 -H -p <mixxx.bin pid>`, plus the Xorg process | about timer mode: Mixxx about 9 %, GUI thread about 1 %, Xorg about 0 % (PLL before: 13 / 5 / 8 % on XWayland) |
| `grep v3d /proc/interrupts`, twice 20 s apart | about 32/s (PLL before: 536/s) |

C. **Two decks playing, 60 s** (six perf windows):

| Line | Target |
|---|---|
| `DECKLAB_PERF pacing mode=5` | `mean` = the panel period (16.646 ms, not 16.667), `p95` about 16.6 to 16.7 ms, `max` < 33 ms, `late=0` |
| `DECKLAB_PERF vsync` | `state=locked keepalive=on referenced` about 600 (a few `unreferenced`/`refills` only right after a stall), `skipped=0 relocks=0`, `err` within a few hundred us |
| `DECKLAB_PERF render-loop` | `dropped=0` |

Visual checks:

- **No periodic hitch.** Watch a fast-scrolling section at high zoom for the full 60 s. Timer mode
  shows one every 13.5 s.
- **No tear line** in the waveforms. With swap interval 1 the X server copies each frame at the vblank.
  If a line still shows near the top, the copy is racing the scanout, and the fix belongs to the X
  server (modesetting TearFree, where available), not to this loop.

D. **Pause, idle, play re-lock.**

1. Play 20 s.
2. Pause both decks for 70 s, so that a refresh happens.
3. Play again.

| When | `DECKLAB_PERF vsync` should show |
|---|---|
| During the pause | `keepalive=off` and `paints=0`, except one window with `refreshes=1` |
| The window that contains the resume | `relocks=1 snaps=0 skipped=0`, `unreferenced` ≤ 4, `err` small |

The `pacing` line for that window: `max` < 33 ms and `late=0`. Visually, the first frames after Play
neither jump nor stutter.

E. **Optional.** The same B to D under labwc with `DECKLAB_VSYNC=pll` (XWayland was stable in today's
test). The appliance default keeps labwc on timer.

Objective vblank reference (read-only; prints the real panel period). Arguments: the DRM card and the
CRTC index of the panel. On the test Pi that is `/dev/dri/card1 2`; an inactive CRTC fails with
`EINVAL`, so try 0 to 5.

```python
import os, struct, fcntl, time, sys
IOC = 0xC018643A  # DRM_IOCTL_WAIT_VBLANK, 24-byte union on 64-bit
dev, crtc, secs = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])  # e.g. /dev/dri/card1 2 20
def wait(fd, seq):
    t = 0x1 | (0x20000000 if crtc == 1 else ((crtc << 1) & 0x3e if crtc > 1 else 0))
    buf = bytearray(struct.pack('=IIQ', t, seq, 0) + b'\0' * 8)
    fcntl.ioctl(fd, IOC, buf, True)
    _, s, sec, usec = struct.unpack('=IIqq', bytes(buf))
    return s, sec * 1000000 + usec
fd = os.open(dev, os.O_RDONLY | os.O_CLOEXEC)
pts, end = [], time.monotonic() + secs
while time.monotonic() < end:
    pts.append(wait(fd, 1))
n = len(pts); xs = [p[0] - pts[0][0] for p in pts]; ys = [p[1] - pts[0][1] for p in pts]
mx, my = sum(xs) / n, sum(ys) / n
per = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sum((x - mx) ** 2 for x in xs)
print('period_us %.3f  rate_Hz %.5f  vblanks %d' % (per, 1e6 / per, n))
```

## 8. Risks and follow-ups

- **Not run on hardware.** No deploy was allowed. The model is built from Mesa's DRI3 behaviour (two
  back buffers in copy mode, swap interval 1). Upstream init only works because of the same behaviour,
  and today's log is consistent with it. A different buffer count changes how many refills a resume
  needs (the budget of 4 covers three back buffers), not whether the loop locks.
- **GUI thread waits in the driver while active.** About 10 ms per frame, as upstream PLL. Touch input
  is handled between those waits.
- **Better reference (follow-up).** DRM vblank timestamps proved accurate and readable without DRM
  master on the Pi (section 1). A VSync source that waits for the vblank in `VSyncThread` itself would
  need no shared-window paints at all and would not hold up the GUI thread. It would need Linux-only
  code, CRTC selection for multi-display setups, and `/dev/dri/card*` in the Flatpak (already
  `--device=dri`). Estimate: 1 to 2 days with Pi testing.
- **Display off and on (DPMS) or a mode change** moves the vblank phase. The Xorg kiosk runs with DPMS
  off. Otherwise the next resume costs at most one repeated frame, and the loop re-locks.
- **Spinnies and GL VU meters** keep the keepalive on whenever they exist, which is upstream behaviour.
  The Deck Lab skin has none.

## 9. CI

`decklab-pi.yml` on the ARM64 runner:

| Run | Commit | Result |
|---|---|---|
| 39869841989 | 91804c028f | **Success.** 331 native tests passed (316 before + 15 `DeckLabVSyncTest`), 0 failures, 4 disabled; smoke check passed (43 screens). |
| 39873595712 | ddfa602d95 | **Success.** 332 native tests passed (316 before + 16 `DeckLabVSyncTest`), 0 failures, 4 disabled; smoke check passed (43 screens). This is the candidate for the Pi checks. |
