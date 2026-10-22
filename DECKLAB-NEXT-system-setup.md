# Deck Lab: system setup (workstream notes)

Branch `decklab/system-setup`. This covers the decklabd client, native system settings, the first-run flow, Deck Setup
(controller, audio and layout), banners, graceful SIGTERM and in-process controller rescans.

## What a DJ gets

- **First run.** If `[DeckLab] first_run_done` is unset, the unit runs a touch flow at startup: Welcome, Wi-Fi
  (optional), time zone, controller, audio, layout, then Done, which sets the flag. You can finish it with no
  controller connected by tapping "Skip – set up later" or by doing audio only. The Done page then points to
  Settings → Setup → Deck Setup. "Later" closes the flow without setting the flag. Safe mode and the smoke profile
  never start it.
- **Deck Setup** (Settings → Setup → Deck Setup, the first-run flow, or the "New controller" banner):
  1. **Controller.** It lists Mixxx's controllers merged with decklabd `devices.list` (MIDI and HID). It suggests the
     best bundled or user mapping, which you can change from a searchable list using the touch keyboard. "Next"
     enables the mapping in-process through `ControllerManager::slotApplyMapping`, the same path Preferences uses.
     "Detect again" runs an in-process rescan. "Restart to detect" calls `session.restart_mixxx` and is offered only
     while decklabd is up and nothing is playing.
  2. **Audio.** It reads the outputs from SoundManager/PortAudio and matches them to decklabd audio cards by ALSA
     card index, then by name. It applies the policy in-process with `SoundManager::setConfig`:
     - 4 or more outputs: Main on 1–2, Headphones on 3–4.
     - 2 outputs: Main on the controller; headphones on a chosen device, or off.
     - No controller audio: a USB DAC, otherwise the Pi jack, otherwise HDMI.

     Bluetooth/PipeWire outputs are listed as "high latency" and never picked automatically. Buffer size, latency and
     sample rate are left alone. Test buttons play a short tone on Main L, Main R or Headphones. The tone uses
     PortAudio directly on the chosen pair while Mixxx's streams are briefly closed, so it only runs while idle.
  3. **Layout.** Two or four decks. The suggestion is four when the mapping or its scripts address
     `[Channel3]`/`[Channel4]`.
  4. **Done.** A summary, then "Play" returns to Decks. Setup is saved per `vid:pid[:serial]`, or per controller name,
     in `[DeckLabDevices]` as compact JSON.
- **Known controllers come back on their own.** When `devices.changed` reports a saved controller, Mixxx re-enumerates
  in-process. It does this right away if nothing is playing or no other controller is open; otherwise it shows a
  notice instead. Enabled mappings reopen, the saved audio routing is re-applied (never while music plays on a working
  output) and the layout is restored. A new, unknown controller shows a tappable "New controller: X · Set up" banner.
- **Settings → System** has eight tiles; each page is two taps from Settings:
  - **Wi-Fi:** status, scan list with signal bars and saved/connected tags, join with a password on the internal
    keyboard (Shift and symbols), hidden networks, forget, and on/off.
  - **Bluetooth:** power, find speakers, pair, connect, disconnect and forget. A banner reads "Bluetooth audio is high
    latency – for practice, not mixing."
  - **Screen:** a brightness slider with −/+ buttons. The minimum is 5% and changes are debounced.
  - **Date & time:** a live clock, NTP state, and a region-then-city time zone picker.
  - **Storage:** drives with label, free and total space, filesystem, rekordbox-export detection, read-only/writable,
    busy and mounted state.
    - **Eject** is refused with the reason (for example "Deck 1, Sampler 3 are playing from…") if any loaded deck,
      sampler or preview track is on that drive, or a recording is writing to it.
    - **Use for recordings** remounts the drive writable and sets `[Recording] Directory` to
      `<mount>/Deck Lab Recordings` and Encoding to WAV in-process, with no restart. It needs at least 1 GB free.
      When the drive disappears, the previous directory comes back, so Mixxx never recreates a mount point on the SD
      card.
    - A start/stop recording button is enabled only once a writable USB target is set.
  - **Power:** Restart Deck Lab, Reboot and Shut down. Each one asks for confirmation, and each is refused (before and
    after confirming) while any deck, sampler or preview is playing. Restart uses `session.restart_mixxx`; if that
    is unavailable it does a graceful quit and the kiosk unit restarts it.
  - **About:** Deck Lab and Mixxx versions, service state, IP addresses, temperature and throttling, uptime, free
    memory and storage, model and OS. It refreshes every 5 seconds while visible.
  - **Maintenance:** "Open maintenance desktop…" behind a confirmation, using `session.desktop`.

  When decklabd is offline, every page says so. Wi-Fi and Maintenance then offer the old companion tool if it is
  online.
- **Banners** are pills just below the tab rail:
  - Thermal: warm or hot, from `thermal.changed`.
  - Undervoltage, from `power.undervoltage`. This one is touch-through.
  - Persistent "No audio output · Set up". It opens Deck Setup's audio step and is hidden on Browse and Settings.
  - "New controller · Set up" offer.
  - Short notices, such as "Restarting Deck Lab…" after `session.quit_requested`.

  Mixxx's "No Output" dialog stays suppressed.
- **Tools.** The native tool buttons are enabled while decklabd is connected or the legacy companion heartbeat is
  fresh. With decklabd:
  - USB, Record mix and Check USB drives open the Storage page.
  - Get music and Sample Maker use `apps.launch`. A `not_available` reply hides that button for the session, since
    Get Music is an optional add-on.
  - Controller & audio setup opens Deck Setup.
  - The old Wi-Fi and Desktop tool ids open the matching System pages.

  Without decklabd, the SysEx companion path is unchanged.
- **Graceful SIGTERM/SIGINT.** A self-pipe and `QSocketNotifier` handle the signal. If it arrives during startup, it
  is held until the event loop runs. Open dialogs are closed, then the main window closes through the normal path:
  settings are saved and confirmations and the preferences prompt are skipped. A second signal falls back to the
  default action. `--debug-assert-break` keeps its SIGINT.

## Contract coverage (protocol v1)

The client implements all of the following:

- Transport and setup: socket resolution (`DECKLAB_SOCKET`, then `$XDG_RUNTIME_DIR/decklab/decklabd.sock`), `hello`
  (protocol 1, features, optional `read_only`) and `events.subscribe` (usb, devices, thermal, wifi, bt, power,
  session, apps).
- Framing: newline-delimited JSON. Partial and coalesced reads are handled, and lines over 64 KiB drop the
  connection.
- Requests: ids with per-request callbacks and timeouts (2.5 s by default). Disconnects fail pending calls with
  `not_available`. Reconnects back off from 0.5 to 5 s.

The UI uses these methods:

- `system.info`, `system.power`
- `display.get/set`
- `wifi.status/scan/connect/forget/enable`; hidden networks also send the optional `security` param
- `bt.status/power/scan/pair/connect/disconnect/forget`
- `time.status/timezones/set_timezone`
- `usb.list/eject/set_writable`
- `devices.list`
- `apps.launch`
- `session.restart_mixxx`, `session.desktop`

It handles these events:

- `usb.changed`, with optional `op`/`id`/`error`
- `devices.changed`, with `added`
- `thermal.changed`, `power.undervoltage`
- `wifi.changed` (with `error`), `wifi.scan_done` (with `error`)
- `bt.changed`, with `error`
- `session.quit_requested`

It also reads these optional fields: drive `mounted`, audio card `index`, and the server's `read_only`.

**Deviations:** none. `apps.close` and `apps.changed` are not used yet.

## In-process controller rescan

`ControllerManager::rescanDevices()` runs on the controller thread. It closes every controller, then each enumerator
refreshes:

- PortMidi deletes its devices, then calls `Pm_Terminate` and `Pm_Initialize`. This is PortMidi's documented way to
  refresh its device list.
- HID and bulk enumerators now drop stale devices before rescanning. Previously `queryDevices()` appended duplicates.

It then calls `slotSetUpDevices()`, which reopens every enabled controller with its saved mapping, and emits
`rescanFinished`. The rescan is refused while Advanced settings is open, because `DlgPrefController` holds controller
pointers. Every open controller pauses briefly, so automatic rescans run only when that can't disturb a playing set.

## Mapping matching

`ControllerMappingMatcher`, in `src/controllers/`, does the matching:

- **HID**, and MIDI mappings that declare USB ids: exact product match, scored 100.
- **Name heuristic** against the mapping's file name, `<info><name>` and `<controller id>`:
  - ALSA port decorations (`MIDI 1`, `client:port`, `20:0`) are stripped.
  - Brand and noise tokens are dropped, and `mkII` becomes `mk2`.
  - Parenthesized words are optional, for example "Mixtrack (Pro) 3".
  - Every model-number token must agree.
  - A mismatched mkN is penalized or rejected.
  - At least one distinctive shared token is required.
- **Overrides:** a built-in JSON override table (port regex or `vid:pid` → mapping file). It is merged with an optional
  `<settings>/decklab-controller-overrides.json`.

`MidiController::matchMapping` returns true at 95 or above (same model by name), so Preferences preselects it too.
The matcher was first validated in Python against all 163 bundled MIDI mappings and about 40 real port names.

## Tests (`src/test/decklabsystemtest.cpp`, suite `DeckLabSystemTest`, gated in `build_pi.py`)

| Test | What it checks |
|---|---|
| ClientHandshakesSubscribesAndParsesSplitAndCoalescedFrames | Handshake, coalesced and split frames, error replies. |
| ClientDropsOverlongLinesAndReconnectsWithBackoff | Line limit, reconnect, pending calls failed on disconnect. |
| ClientTimesOutUnansweredRequestsAndFailsFastWhenOffline | Timeout; offline calls fail asynchronously with `not_available`. |
| SocketPathComesFromEnvironmentThenRuntimeDirectory | Socket path resolution. |
| MidiPortNamesMatchTheirBundledMappings | DDJ-FLX4, Mixtrack Pro 3, Inpulse 500, MC7000 and DDJ-SB2 against all bundled mappings; neighbours rejected; MIDI Through matches nothing. |
| HidControllersMatchByProductIdsAndUsbOverridesApply | Traktor S2 MK3 HID by vid/pid, USB overrides, user overrides, 2- and 4-deck suggestions. |
| AudioPolicyRoutesFourTwoAndNoControllerOutputs | 4-out, 2-out (with and without a headphone choice) and none (DAC, jack, HDMI, Bluetooth never automatic); name classification. |
| EjectIsRefusedWhileALoadedTrackLivesOnThatDrive | Blockers, sibling-prefix safety, refusal before any `usb.eject`, a real eject of the other drive. |
| ControllerSetupPersistsPerDeviceKeyAcrossRestart | `[DeckLabDevices]` round trip through save and reload, plus lookup by controller name. |
| TerminationSignalBecomesOneGracefulQtRequest | SIGTERM held until armed, delivered once, default action restored. |
| SystemPagesShowServiceStateAndRefuseUnsafeEject | Wi-Fi list from scan events, eject refusal shown, brightness floor at 5%, 44 px targets. |
| FirstRunFlowCompletesWithoutAControllerAndSetsTheFlag | Every step, mapping suggestion from decklabd metadata, 4-out audio policy, skip-later path sets `first_run_done`. |

Fixtures are generated; nothing personal is used. The fake service uses documentation IPs (192.0.2.x).

## CI and screenshots

| Run | Result |
|---|---|
| 39801535641 | Failed before the build. The 50-commit shallow clone no longer reached the pinned upstream revision. |
| 39801940568 | Compile errors: a raw-string delimiter, and `QPointer` on an incomplete type. |
| 39807286053 | Passed: 271 tests, 12 of them system-setup, and 43 smoke screens. Screenshots showed squashed rows and keys under the real surface style. |
| 39810328603 | Passed: touch heights hold (56 px rows, 44 px keys), white selection, banners return after Deck Setup. |
| 39811868158 | Passed: brand type scale (18/16/15/14 px, nothing smaller); About wordmark, credit and source line. |
| 39813071025 | Passed (final): 271 tests (12 system-setup), 43 smoke screens, touch-target gate clean. Copy polish. |

After run 2, every changed source was compiled locally before pushing: zig clang with Qt 6.8 Linux headers, generated
protobuf and uic headers, and the real third-party headers. This was a syntax check only; tests still run in CI.

The smoke check now produces 43 screens: 21 existing, plus these 22, against a canned decklabd on a private socket. The
fixtures use made-up SSIDs, locally administered MACs and documentation IPs.

| Screen | Content |
|---|---|
| app-screen-21 | System home |
| app-screen-22 | Wi-Fi |
| app-screen-23 | Wi-Fi password with the in-app keyboard |
| app-screen-24 | Bluetooth |
| app-screen-25 | Screen |
| app-screen-26 | Date & time |
| app-screen-27 | Time zone picker |
| app-screen-28 | Storage |
| app-screen-29 | Power |
| app-screen-30 | About |
| app-screen-31 | Maintenance |
| app-screen-32 | First run: Welcome |
| app-screen-33 | First run: Wi-Fi |
| app-screen-34 | First run: Time |
| app-screen-35 | First run: Controller |
| app-screen-36 | Mapping picker |
| app-screen-37 | Audio |
| app-screen-38 | Layout |
| app-screen-39 | Done |
| app-screen-40 | Decks with the thermal and no-audio banners |
| app-screen-41 | System home at 1024x600 |
| app-screen-42 | First run at 1024x600 |

The smoke check fails if any visible control on the System pages or in Deck Setup is below 44 px in the real surface
style.

## Merge notes

- **`wdecklab.h`:** forward declarations, `attachSystem()` and `m_systemUi`.
- **`wdecklab.cpp`:**
  - Includes.
  - `buildSettings`: the system UI and hooks, categories renamed from Device to Setup, a new System category, the
    Setup page, and object names on the Library tool buttons. "Check Rekordbox USB" became "Check USB drives".
  - `tool()`: decklabd routing first.
  - `tickEffects()`: the handshake lapse check.
  - `setAudioOutputConfigured` and `attachSystem`.
- **CI workflow:** `.github/workflows/decklab-pi.yml` uses `fetch-depth: 0`, the same as perf-engine (commit
  375429e8b1).
- **`decklabsmoke.cpp`:** 22 appended steps, a longer settle time for them, and an expected screen count of 43.
  `build_pi.py` checks the count and the `DeckLabSystemTest` names. The `pi-build-lock.json` test suite and hashes
  were updated.
- **Main window:** `mixxxmainwindow.cpp` creates and attaches `DeckLabSystem` in `loadConfiguredSkin`, and
  `closeEvent` skips the confirmation after SIGTERM. `main.cpp` installs and arms the termination handler.

## Style notes

- **Type scale (brand README, binding).** Page titles are 18 px SemiBold. Body text, rows and buttons are 16 px.
  Secondary text is 15 px Medium in `#9AA3AF`, and section labels are 14 px SemiBold. Nothing is under 14 px and no
  Regular weight is used under 18 px. Text is elided, never shrunk.
- **About** shows a text rendering of the wordmark (DECK, a red playhead, LAB), the exact Mixxx credit, and "The
  source code and licences are available at [REPO URL]." The SVG wordmark is not embedded: the fork has no install
  path for appliance brand assets yet.

- **Touch heights.** The surface style sheet sets `min-height` on every `QPushButton` and `QComboBox`. That overrides
  `setFixedHeight()` when the widget is polished. The new widgets use `decklab::ui::TouchButton` and `touchCombo`,
  which restore their height after the Polish and StyleChange events. Other workstreams that rely on
  `setFixedHeight()` for buttons hit the same problem.
- **Selection and colour.** Selection, focus and primary actions are white or a raised fill, following the brand
  direction in PLAN. Red stays for alerts (no audio, errors). Thermal and Bluetooth-latency warnings are amber.
- **Wording and touch input.** Labels use "rekordbox export" only as compatibility wording, with no other vendor
  branding. All text entry, including Wi-Fi password, hidden SSID and mapping search, uses the in-app keyboard. Every
  control is a `QPushButton`, so it works with synthesized mouse events from XI2 touch, and nothing relies on hover.

## Gaps and follow-ups

- **Keyboard.** The touch keyboard is a reusable `decklab::ui::Keyboard`: same key rows as Browse search, 44 px keys,
  plus Shift and symbols. Browse still builds its own; browse-ui could adopt this one.
- **PortMidi rescan.** It relies on `Pm_Terminate`/`Pm_Initialize` re-reading ALSA. This is covered by code review and
  the PortMidi docs, not by hardware in CI. Verify it on the Pi with a hot-plugged controller.
- **Test tone.** It opens the ALSA device with exactly the pair's channels, then with the device's maximum. It is
  verified only by review; test it on hardware.
- **Recording on USB** needs the Flatpak to have write access to `/media/decklab` (os-image/flatpak permissions).
- **Not used yet:** `apps.close` and `apps.changed`. Get Music's availability is learned on first use.
- **SSH.** "Enable SSH from Settings" (PLAN distribution goal) needs a decklabd method that the contract doesn't
  have.
- **Advanced settings.** Deck Setup changes don't refresh an already-open Advanced settings dialog until it is reopened.
