# Deck Lab round 4: system signature moves (r4-system)

Branch `decklab/r4-system`, based on `decklab/round3` (5d4f4570f7). It builds the system audit's signature moves
(`audit/system.md` section 3, with `audit/performance.md` Move 5 for the rail) on top of the qw-system quick wins.
Browse, Lab FX, Sampler and Decks builders are untouched; `kStyle` is owned here and only gained/adjusted Settings
and rail rules (all existing selectors still apply).

## What changed

### 1. The rail as a status bar that teaches (3.2 + performance Move 5)
New `src/decklab/decklabrail.{h,cpp}`.
- **Fixed tab widths** (R-01, PF-R09): Decks, Browse, Sampler and Lab FX get one width for the session, computed from
  what the status zone needs (76 px at 800 px); spare width on bigger panels goes into a stretch, never the tabs.
  **Settings is a gear** at the far right (R-10, PF-R01), the CDJ's UTILITY button. `DeckLabRailTab` paints the tab
  and its lamp: Sampler shows a green dot while any sample plays (PF-R12); Lab FX shows a white dot while wet (it
  reads the existing `live` property, PF-R11); the gear carries the unit's health (caution when hot, low power or
  the system service is offline).
- **A fixed status zone in signal-flow order**: `USB` (its own lamp button) then `CTRL`, `WI-FI`, `REC`, `OUT`, then
  the main meter and the gear. Each slot is an 8 px lamp over a 14 px caps label, so they read as lamps, not
  buttons (R-05..R-07, PF-R03..R-08). Lamp states: hollow (nothing), green (working), white (on/armed), caution
  hatch (no hue: amber is deck 2, G-06), live red (recording), live-red ring (no output). **Red is only for live**:
  REC fills live red and prints the elapsed time (`12:34`, `1h02`, R-04); NO OUTPUT is the one red alert.
- **USB** keeps the qw-library call: a tap opens the stick in Browse (`openUsbInBrowse()`, unchanged); a long
  press (600 ms) opens the status sheet. It shows presence, the count (`USB 2`), a rekordbox export (`USB RB`) and
  the recording drive (a second dot, red while recording).
- **Alerts hold the zone** and the whole zone is the button (A-03, R-03, R-05): `⚠ NO OUTPUT · FIX ›` (live-red
  outline) opens Deck Setup at the Sound step; `⚠ LOW POWER ›` (caution hatch) opens the sheet. A 1.5 s start-up
  grace keeps NO OUTPUT from flashing while Mixxx opens its devices.
- **Notices never cover content** (A-01, A-02, A-06..A-09, `DECKLAB-NEXT-overlay.md`): the system banners and the
  system toast are gone. `DeckLabRailNotice` slides into the rail (over the FX lamps, USB and the zone only, never
  the tabs or a page) for 4 s, then collapses into its lamp; actionable notices are buttons ("New controller: X ·
  set up ›" opens Deck Setup on that controller; the CTRL slot then reads `NEW` for 30 s). Hot: one notice, then the
  gear's lamp and the sheet. Warm: sheet only. Undervoltage: the LOW POWER alert. The only overlay left over the
  waveforms is Browse's own toast (Browse territory).
- **Status sheet** (R-02): tapping the zone opens `DeckLabStatusSheet` over the page below the rail (waveforms
  suppressed while shown): one line per subsystem with its lamp, what it is doing and one action: Sound (Deck
  Setup / Fix sound), Recording (**hold to record / hold to stop**, Y-36; locked with the reason when no writable
  drive is the target), USB (Storage), Network (Wi-Fi), Controller (Deck Setup / Set up / Fix), Unit (heat, power).
  The knob drives it; back closes it.
- Also fixed: `[Recording] status` 3 is "recording into the next split file", not "paused"; the rail no longer
  shows "REC II" for it. The rail's disabled colour is the dim token (#6B7380, was 2.2:1 #4B525C).

### 2. Settings as a utility list you drive with the knob (3.1)
New `src/decklab/decklabsettings.{h,cpp}` (the page) and `src/decklab/decklabutility.{h,cpp}` (the row kit);
`WDeckLab::buildSettings()` is now hooks plus `new DeckLabSettingsPage(...)`.
- **One row anatomy**: label left, value right, 52 px (64 with a second line), panel fill, line separator. Rows:
  `SwitchRow` (ON/OFF printed inside the switch, S-05, S-10, S-11, S-19), `ChoiceRow` (2-4 segments; more open a
  picker sheet, S-02, S-13..S-15, S-18), `StepperRow` (readout with drawn − and +, hold repeats, double-tap resets,
  S-12, S-16, S-17), `ActionRow` (value + chevron), `InfoRow` (read-only with a lamp, S-22), `SectionRule` (TE
  label with a line). **No desktop combo boxes are shown**; the old ones stay as hidden models with the same object
  names, items and `activated` signal, so every existing caller (`decklab-deck-layout`, `decklab-waveform-layout`,
  `decklab-tempo-range-N`, the key-format combo) keeps working.
- **Categories** (S-24, R-12, R-13): Controller & audio · Decks · Headphones · Display · Library · Unit, each with a
  pictogram and a live value (READY / NO SOUND, AUTO · 2 DECKS, SPLIT CUE, KEY 8A, the clock or OFFLINE). Settings
  opens on Decks (where the stems setting lives). **Unit takes the full width** with a breadcrumb header (R-14).
- **Decks**: Decks on screen `[AUTO (2)] [2] [4]` (S-01); the stems setting (stems workstream's widget, unchanged);
  a deck selector `[1] [2] ([3] [4])` in deck colours, decks 3-4 only in the four-deck layout (S-04); Quantize, Key
  lock, Slip as switches with the deck badge; Tempo range `[±8 %] [±16 %] [±50 %]` with the lock reason printed
  ("Locked while deck 1 plays / syncs", S-07); a controller-set range shows as "(from controller)" (S-08).
- **Headphones** (S-09, S-10..S-12): Deck N cue switches with deck badges; Split cue with "Cue in your left ear,
  Main in your right"; Headphone mix `CUE 70 · MAIN 30` in steps of 10, double-tap for even.
- **Display**: Key format `[8A] [8m] [Am]`; Waveforms `[STACKED] [SIDE BY SIDE]`; Waveform motion `[SMOOTH 60]
  [LIGHT 30]`; Zoom `×4` written to all four decks (S-16); Beat grid `60 %`; Waveform style with plain names (One
  colour, Three bands, Colour by energy, Colour by frequency, Stacked bands) in a picker; VSyncTest is hidden (S-18).
- **Library**: Dim played tracks switch; APPS: Get music…, Sample Maker…, USB drives & recording (the two duplicate
  Storage launchers became one, S-20), each saying "Needs the system service" when it can't run.
- **Controller & audio** (S-21..S-23): Controller, Sound and Run Deck Setup rows; Health ("Smooth · no dropouts"
  or "N dropouts" with a lamp); "Advanced (desktop)…" opens a sheet that says it's built for a mouse.
- **The knob** (G-18): `decklab::ui::KnobNavigator` moves a 2 px white focus ring over the visible controls in
  reading order. Turn = move, press = open a category / flip a switch / edit a choice or number, turn = change,
  press = set, back = restore and leave editing, then up a level, then (from the category list) back to Decks.
  Unit's tiles and pages are navigable the same way (back closes a panel, then returns to the home, then to the
  list).

### 3. Hold-to-confirm instead of modal dialogs (3.4)
`decklab::ui::HoldButton` (kit): hold 1.2 s while a white fill sweeps left to right like a playhead; let go or slide
off and it snaps back; only a completed hold acts (`confirmed`, never `clicked`). From the knob: a held press when the
script reports the release (`[DeckLabNative] browse_hold`, new), otherwise press, then press again within 3 s
("PRESS AGAIN"). A gate refuses up front with the reason.
- Power page (Y-45..Y-48, G-12, G-13): hold rows **Restart app** ("About 15 s. Settings are kept."), **Restart
  unit** ("About a minute."), **Turn off** ("Do this before unplugging."); while anything plays they dim with "Stop
  all decks and samplers first." (polled 1 s while shown); the result shows in place ("Turning off… unplug when the
  green light stops blinking.").
- Wi-Fi **Forget**, Bluetooth **Forget** and Storage **Eject** are hold buttons (eject refusals still come first).
- Modal sheets remain only for data risk: making a read-only drive writable, opening the desktop.

### 4. System home as the back panel of the unit (3.3)
Eight live tiles, 4 × 2 at 186 × 176 (`DeckLabLiveTile` in `src/decklab/decklabinstruments.h`), each with one lamp:
Wi-Fi (a fan whose arcs light with the signal, the SSID, "Signal 78 %"), Bluetooth ("On · Nothing connected",
Y-03), **Screen** (hero `80 %`, inline −/+ snapped to 10s, never below 5 %, **Booth 30 %** / **Daylight 100 %**,
Y-27, Y-28), Time (hero clock, date, "London (Europe)", Y-32; refreshed at the minute boundary only while shown),
**USB** (a capacity bar per drive, read-only marked, the recording drive with "REC · 19 h free", Y-41), **Heat**
(°C with a lamp: fine / warm / hot / low power), **Power** (hold Restart app / Turn off right on the tile), About
(version, "Source & licences"; Maintenance moved to a row on the About page). The service line shows only when the
service is offline (Y-01, Y-05). Pages got one header: a 44 px back glyph, a "UNIT /" breadcrumb and the title
(R-15, R-16).

### 5. Sound check (3.5)
Deck Setup's audio step: SOUND CHECK with the levels warning always shown; MAIN and HEADPHONES panels, each with its
output and big tone pads (LEFT, RIGHT / TEST) whose 12-segment meter moves with the tone; after a tone, **Yes /
No, try next** ("Only the LEFT speaker?", "Heard it in the phones?"); No moves to the next output pair. The first
tone is −24 dBFS; a **second tap on the same pad and output adds 6 dB** (`decklab::kTestToneBoostDbfs = −18`, never
more; `DeckLabSystem::playTestTone(..., dbfs)`). Raw PortAudio errors go to the log; the DJ reads "Couldn't play on
that output. Pick another one." (F-19, F-20). The stepper is a **progress line** (`DeckLabProgressLine`): step names
on a line, a dot for now, checks for done, white because it isn't live (F-02, F-16; steps WELCOME · WI-FI · TIME ·
CONTROLLER · SOUND · DECKS · PLAY). "Later" reads "Set up later" and is hidden on the last step (F-03); Back is hidden
on the first (F-05). The knob drives Deck Setup.

### Also
- Knob ownership (`src/library/decklabknob.h`, additive): `claimKnob()/releaseKnob()/knobClaimant()`. After real
  popups and modal dialogs, the most recent visible claim owns the knob and gets Up/Down, Return, Escape. Settings,
  the status sheet, Deck Lab sheets (`Confirm`, which no longer let a turn open Browse behind them), the picker
  sheet and Deck Setup claim it while shown. Browse behaviour on every other page is unchanged.
- Kit additions (`decklabui`): `kGo`, `Lamp` + `paintLamp`, `paintCautionHatch`, `Glyph` + `paintGlyph` (back,
  chevron, gear, check, warning, minus, plus, close, lock), `numberFont`, `capsFont`, `KnobTarget`,
  `KnobNavigator`, `HoldButton`, `GlyphButton`.
- Copy deck items applied where these screens were touched (Restart app/unit, Turn off, Set up later, Sound,
  Decks, Wi-Fi/Time step messages, "Couldn't open %1", "%1 isn't installed on this unit", tile values).

## Renders (local, not published)
`local/wt/audit/screens/r4-system/`
- `before/`: `round2-app-screen-00` (rail), `-10..-14` (Settings), `-21` (System home), `-25` (Screen), `-29` (Power),
  `-37` (Audio), `-40` (banners over Decks); `qw-status-sheet`, `qw-settings-deck`, `qw-system-home` (round 3).
- `after/`: `rail-01-normal`, `rail-02-recording`, `rail-03-no-output`, `rail-04-low-power`, `rail-05-notice`,
  `rail-06-new-lamp`, `status-sheet`, `status-sheet-knob`, `settings-01-decks`, `settings-02-knob-focus`,
  `settings-03-knob-edit`, `settings-04-headphones`, `settings-05-display`, `settings-06-picker`,
  `settings-07-controller-audio`, `settings-08-library`, `unit-01-home`, `unit-02-hold-sweep`, `unit-03-knob-armed`,
  `power-01-hold`, `power-02-playing`, `power-03-result`, `sound-check`, `sound-check-first-run`, plus `report.txt`.
  The rail renders sit on the round-2 Decks capture for context.

## Verified locally (WSL, g++ 13, Qt 6.4, offscreen)
- Syntax-only compile with `-Wall -Wextra` against the real headers (moc'd) of every changed C++ file:
  `wdecklab.cpp`, `decklabui.cpp`, `decklabutility.cpp`, `decklabrail.cpp`, `decklabsettings.cpp`,
  `decklabsystempages.cpp`, `decklabsystemui.cpp`, `decklabsetupwizard.cpp`, `decklabsystem.cpp`,
  `decklabsetupmodel.cpp`, `decklabsmoke.cpp`, `decklabtest.cpp`, `decklabsystemtest.cpp`, and
  `librarycontrol.cpp` (it includes the changed `decklabknob.h`). No errors, no new warnings.
- Offscreen harness linking the **real** `decklabrail.cpp`, `decklabui.cpp`, `decklabutility.cpp`,
  `decklabinstruments.h`, `decklabdeckwidgets.cpp` and the real `kStyle`/`kRailStyle` (page layouts mirror
  `decklabsettings.cpp`/`decklabsystempages.cpp`; the Unit tiles, hold buttons, tone pads and progress line are the
  real classes): 57 checks, all pass. Among them: tabs Barlow 16 px; gear at the right edge; the alert moves no tab
  or the zone; REC `12:34` live red, OUT caution on dropouts; the notice stays in the rail (y 0-43) between FX and the
  zone; tapping it runs its action; CTRL says NEW; the sheet covers only the page and owns the knob, knob back
  closes it; Settings owns the knob; four turns reach Quantize, a press flips it; press-turn previews without
  applying, press sets (model `activated`), back restores; mix steps by 10; five styles use a picker; every visible
  control ≥ 44 px; tiles 186 × 178; a hold sweeps and doesn't fire, letting go snaps back, a full 1.2 s fires, knob
  press arms and a second press fires; rows dim with the reason.
- `python tools/decklab/build_pi.py --check-inputs`: PASS (lock refreshed). `test_build_pi.py`: 7 tests OK.
- Not built here: the full app and the gtest suites (they need the ARM64 Flatpak build).

## Needs CI and the Pi
- Build, `NativeDeckLabTest.*`, `DeckLabSystemTest.*`, `DeckLabOverlayTest.*`. Changed tests:
  `InternalClockCannotClaimAConfiguredAudioOutput` (status zone), `SplitCueToggleReportsItsActualState` (ON/OFF
  switch row), `SettingsTouchSwipeRevealsDeckFourWithoutTogglingQuantize` → `SettingsTouchSwipeScrollsWithoutTogglingQuantize`
  (one set of per-deck rows), the Settings part of the knob test in the generic-mapping navigation test (the knob
  drives Settings now; it never turns a combo box), `EveryOverlayOverTheWaveformsHidesThemOrIsNativeAndRaised` (system
  news in the rail). New: `SettingsIsAUtilityListTheKnobDrives`, `RailKeepsFixedTabsAndTheUsbLampOpensTheStick`,
  `RailLampsTeachAndRedIsOnlyForLive`, `HoldToConfirmNeedsTheWholeHold`, `UnitHomeTilesShowLiveValuesAndBrightnessInline`,
  and sound-check checks in `TestTonesStartQuietBehindALevelsWarning`.
- Smoke: **48 screens** (was 46). app-screen-43 is now the rail status sheet; app-screen-46 is Settings driven by the
  knob, app-screen-47 a rail notice over Decks. Look at app-screen-00, 10-14 (Settings), 21-31 (Unit pages), 37
  (sound check), 40 (heat now in the rail) and 43.
- On the Pi: the knob in Settings with the Mixtrack Pro 3 (see the appliance follow-up below for Shift+press),
  a hold with a finger in a dark booth, the tone meter against the real tone, Booth/Daylight on the real backlight.

## Merge notes
- **New files**: `src/decklab/decklabrail.{h,cpp}`, `decklabsettings.{h,cpp}`, `decklabutility.{h,cpp}`,
  `decklabinstruments.h` (header-only). CMake: the three `.cpp` files next to the other `src/decklab` sources.
- **`wdecklab.cpp`** (line numbers on this branch):
  - includes: `decklab/decklabrail.h`, `decklab/decklabsettings.h` (~48);
  - `kStyle`/`kRailStyle` (~92-158): Settings background, 56 px category rows, rail disabled colour;
  - the old `statusChips()` helper is deleted (~520);
  - constructor rail block (~800-895, marked `r4-system`): `DeckLabRailTab` tabs, gear last, stretch, FX chips,
    `DeckLabRail` (USB lamp, zone, notice), the main meter, fixed tab widths, Sampler lamp watches, `browse_hold`,
    the 1 s recording tick. The old `m_status` chip block and the old meter block further down (~935) are gone;
  - `buildSettings()` (~2399-2462): system UI hooks (+ `notify`, `statusChanged`) and `DeckLabSettingsPage` hooks;
    the old 230-line builder is gone;
  - `updateStatus()` (~4079-4127): builds `decklab::rail::Inputs`; the performance line moved to the Health row.
- **`wdecklab.h`**: forward declarations; `m_status` is a `DeckLabStatusZone*`; new members `m_railUi`,
  `m_settings`, `m_recordTick`, `m_recordingSince`, `m_usbScannedAt`, `m_usbSticksSeen` (all marked).
- **`src/library/decklabknob.h`**: additive (claims); Browse's use is unchanged.
- **Removed**: `DeckLabSystemUi`'s banners (`decklab-banner-audio/-offer/-thermal/-toast`) and their timers. Anything
  that looked for them should look at the rail (`DeckLabRailNotice`, `DeckLabStatusZone`).
- `tools/decklab/build_pi.py`: smoke count 48 (**+2 screens**); the renamed swipe test; three new
  `SYSTEM_SETUP_TESTS`. `test_build_pi.py` follows. `pi-build-lock.json` refreshed for CMakeLists.txt,
  `decklabtest.cpp`, `decklabsmoke.cpp`, `wdecklab.cpp/.h`, `build_pi.py`, `test_build_pi.py`.
- If another branch adds smoke screens, add the counts; keep the three sheet captures at `kSheetFirst` (now bounded
  by `kSheetCount`); r4-system's two new captures come after them.
- decklabd contract: **no new methods or events used**. Proposals for later (not used): `time.set {"iso"}` and
  `time.ntp {"on"}` for setting the clock by hand on a Pi 4 without a real-time clock (Y-30, F-07).

## Follow-ups
- **Appliance (Numark script)**: outside Browse, Shift+press sends `browse_press`, not `browse_back`, and presses are
  pulses. For the full UTILITY gesture the script should send `browse_back` for Shift+press on every page and write
  `[DeckLabNative] browse_hold` 1 on press-down / 0 on release (hold to confirm from the knob; until then the knob
  uses press, then press again).
- **3.6 Boot-to-play continuity** (appliance repo, Plymouth `decklab.script`; not done). Design: map
  `boot_progress` 0-1 onto the playhead travelling from the waveform's left edge to **exactly x = 425, under the
  wordmark's playhead** (the cued position), so the frozen `--retain-splash` frame reads "ready" (B-01, B-02). After
  90 s without progress, dim the wave and show one 14 px caps line "Starting is taking long · check the SD card"
  (B-03). Shutdown's last frame: playhead at the end, all bars dimmed ("track finished") = safe to unplug (B-04).
  App side: the first frame reproduces the cued picture, then the rail slides down and the wave stretches into deck
  1's lane in 400 ms (or into Welcome on a first run, F-28); "Restarting…" and "Turning off" reuse the same art
  (A-11). Needs: the cued x from `logo/decklab-wordmark.svg`, a full-screen `DeckLabBootFrame` overlay that suppresses
  the waveforms while it animates (overlay rules), and a 400 ms transform-only animation.
- Wi-Fi/Bluetooth details sheets (Y-11, Y-12, Y-25), the Wi-Fi on/off as a row (Y-07), hidden network first (Y-13).
- The keyboard spec (6 px gaps, one-shot shift, verb on the action key: Y-18, G-23) and the time zone search (Y-33).
- About: the real wordmark SVG and QR codes (Y-50, Y-52); Maintenance: export logs, reset, run first-time setup
  again (Y-53).
- Rail: rename the FX chips "CTRL FX" and light them in their routed deck colours with a 2 × 2 sheet (performance
  Move 5): the chip widget is shared with Decks and the width budget is tight at 800 px; left to the Lab FX/Decks
  owners.
- The stems setting is still the stems workstream's combo box inside the Decks category (their tests select it);
  it could become a `ChoiceRow` with the same hidden model.
- Settings remembers nothing between sessions (it opens on Decks).
