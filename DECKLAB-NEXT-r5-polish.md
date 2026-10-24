# Deck Lab r5-polish: five fixes from round 4 on the Pi

Branch `decklab/r5-polish`, based on `decklab/round4` (d381cded9e), the build on the owner's Pi 4.
Only generated values are used in tests and renders (USB locations, keys, stick UUIDs).

## 1. One controller showed as two rows, and its vendor HID interface was opened

**Cause.** Mixxx lists a USB DJ controller once per interface: the MixTrack Pro 3 is the PortMidi port
`MixTrack Pro 3 MIDI 1` and the HID interface `MixTrack Pro 3 _3` (usage page 0xFFA0, vendor-defined,
interface 3). `DeckLabSystem::controllers()` matched decklabd's device to the first Mixxx controller only
and listed the HID interface as a second row. That row got its own key from its USB ids, `15e4:0035`:
the same key as decklabd's device. Both rows showed **HID** because decklabd's `hid: true` (the device has
a hidraw node) was used as the badge.

**What opened the HID interface.** Not a probe, and not the enumerators (they never open). Deck Lab did:
`DeckLabSystem::reapply()` looks a saved profile up by key and took the *last* row with that key, the HID
interface, then `applyMapping()` gave it the MIDI mapping. `ControllerManager::slotApplyMapping()`
enables and opens it ("Opening HID device ... by HID path /dev/hidraw0") and saves `[Controller]
MixTrack_Pro_3__3 = 1` with the `.midi.xml` mapping, so every later rescan (Detect again, the new-device
rescan) and every start reopened it in `slotSetUpDevices()`. The wizard also preselected that row
("prefer a controller that is not working yet"). Reproduced with the round-4 code and a fake enumeration
(throwaway branch, not pushed): two rows, both HID, same key; a replug with the saved profile opened
`MixTrack Pro 3 _3 <- Numark-Mixtrack-3.midi.xml`.

**Fix.**
- `src/decklab/decklabcontrollers.{h,cpp}` (new, pure): groups Mixxx's interfaces into physical controllers
  by USB location (Linux: the ALSA card's long name via `/proc/asound/seq/clients` and `/proc/asound/cards`,
  against the hidraw node's `HID_PHYS`, both `usb-0000:01:00.0-1.x`), then same USB ids and serial, then a
  model-name fallback (MIDI model vs HID product string). decklabd's devices claim groups by USB port chain
  (`usb_port`), MIDI port name, then USB ids, so two identical controllers stay two rows.
- The interface in use: working, then configured, then a HID interface with a confident `.hid.xml` match,
  then MIDI, then HID alone. The badge (`ControllerInfo::hid`) is that interface's kind: MIDI here.
- A HID interface beside the MIDI port in use (no fitting `.hid.xml` configured) is *suppressed*:
  `applyMapping()` refuses it, and refuses any mapping of the other kind on any interface.
  `closeStrayInterfaces()` disables a suppressed interface that is open or armed (enabled with a mapping),
  queued on the controller thread (never blocking), at start, after a rescan and when the device list
  changes. This repairs the Pi's saved state: the first start of this build may still open the HID
  interface once (Mixxx's own start-up), then Deck Lab closes and disables it for good.
- `reapply()` matches a profile by key or by any interface name and applies to the interface in use.
- HID-only devices and HID interfaces with a matching `.hid.xml` mapping work as before.
- `HidController::getDevicePath()` (one getter) gives the hidraw path; Deck Lab never opens it.
- Settings › Controller & audio and the rail's status sheet use the same grouped list.

## 2. "Set up later" did not stick

**Cause.** `[DeckLab] first_run_done` was only set at Done/Play; "Set up later" just closed the wizard, so
`DeckLabSystemUi::attach()` opened the Welcome flow at every start.

**Fix.** "Set up later" sets `[DeckLab] first_run_deferred`; the flow opens by itself only while neither
flag is set (`DeckLabSystem::firstRunDue()`). Settings › Controller & audio shows **Finish setup**
("Wi-Fi, time zone, controller and sound", lamp on, value "Not finished") and the status sheet gets a
**Setup** line with a Finish setup button, both until setup is done. Finishing Deck Setup (first run or
not) sets `first_run_done` and clears the deferral. A unit that never finished will show Welcome once more
after this update; "Set up later" then sticks.

## 3. "CONTRO…" in the step bar

**Cause.** Seven equal cells in 6/7 of the header (a stretch took the rest): about 77 px for CONTROLLER.

**Fix.** The progress line takes all the room between the title and Close; each step's cell is its
label's width plus an equal share of the rest. Labels are 14 px Barlow SemiBold caps with minimal
tracking (101 %). No abbreviations.

## 4. The USB stick showed its filesystem UUID

**Cause.** decklabd sends the UUID as `label` when a stick has none, and RekordboxFeature named an
unlabeled stick after its mount folder (`/media/decklab/<UUID>`): Browse's breadcrumb read
"<UUID> › All Tracks". Browse's own tile said "USB stick (3A1F)".

**Fix.** `src/decklab/decklabusbname.{h,cpp}` (new): the volume label, else **DJ USB** (rekordbox export)
or **USB stick**. A label equal to the id or shaped like a UUID or serial counts as none. A name, once
shown, stays while the stick is in (`UsbNameBook`, shared by every caller): a later stick with the same
name gets the size it was sold as ("DJ USB 16 GB"), or "2", "3" when the sizes match; nobody already
listed is renamed. Used by Browse's tiles and breadcrumb (`findUsbSticks`), RekordboxFeature's device
names, the rail's status sheet and accessible name, Settings › Unit › Storage and eject messages.

Names are display only. RekordboxFeature keys a device by its mount path: `mergeDevices()` matches
rows by path (a known stick keeps its row and read export and takes a new name in place; a swapped
stick with the same name gets its own row), and the rekordbox tables' `device` column holds the path.
Tests run the real `findDevices()`/`mergeDevices()` over generated stick folders
(`setUsbMediaRootsForTesting`, shared with Browse).

**Rail lamp: "USB" (or "USB 2"), no longer "USB RB".** The 54 px slot is read at arm's length; it says
where and, by its lamp, the state. "RB" is an abbreviation (not allowed) and no name fits the slot; the
stick's name is one tap away (Browse) and in the status sheet.

## 5. "Load a deck to mat…" in the match bar

**Fix.** "Load a deck first". The Only button shows the bare count when "1,234 tracks" would not fit.
`textFits()` on the summary, deck chip and action button checks every text the bar paints.

## Renders (offscreen, 800x480)

`C:\Users\Me\Desktop\Mixx Pi\local\wt\audit\screens\r5-polish\before\` and `...\after\`:
`setup-controller-midi-hid.png`, `settings-*-set-up-later.png` / `settings-finish-setup.png`,
`status-sheet-*.png`, `first-run-*-step-bar.png` / `setup-step-bar*.png`,
`browse-usb-unlabeled-breadcrumb.png`, `browse-match-bar-empty.png`, plus the full smoke set.

## Verified

- Local full run (`decklab-local.sh`, WSL 2, Qt 6.4, no flags): see the coordinator report for the
  `summary.md` path. Every locked suite passes; smoke matches `build_pi.py` (48 screens, unchanged).
- New tests: `DeckLabSystemTest.OneControllerIsOneRowAndItsVendorHidInterfaceStaysClosed`,
  `HidOnlyAndTwinControllersKeepTheirOwnRows`, `UsbLocationsComeFromHidrawAndTheAlsaCard`,
  `SetUpLaterSticksAndSettingsOffersFinishSetup`, `EveryDeckSetupStepNameFitsAt800`,
  `UsbSticksHaveFriendlyNamesNeverTheirUuid`; `NativeDeckLabTest.DeckSetupListsAMidiAndHidControllerOnce`,
  `SettingsAndStatusSheetOfferFinishSetupAfterSetUpLater`, `UnlabeledStickIsNamedDjUsbNotItsUuid`.
  Updated: the match bar test, the stick-name expectations, the rail USB lamp.
- `python tools/decklab/build_pi.py --check-inputs` passes (lock refreshed).

## Needs the ARM build and the Pi

- The KDE 6.10 SDK build (coordinator).
- On the Pi: one row "Numark MixTrack Pro 3 · Working" with MIDI; mixxx.log shows "DeckLab: closed
  MixTrack Pro 3 _3 beside the MIDI port in use" once, and no "Opening HID device" after the next restart;
  "Set up later" then reboot: no Welcome, Settings shows Finish setup; the stick reads "DJ USB".

## Merge notes

- `src/widget/wdecklab.cpp`: two marked hooks (`// r5-polish:`): `railHooks.finishSetup` next to the other
  rail hooks (~784), and `updateMatchBar()` (~4624, ~4633).
- New files: `src/decklab/decklabcontrollers.{h,cpp}`, `src/decklab/decklabusbname.{h,cpp}`
  (CMakeLists: next to `decklabusbrelink.cpp` and before `decklabdclient.cpp`).
- Also touched: `controllers/hid/hidcontroller.h` (getter), `library/rekordbox/rekordboxfeature.cpp`
  (device names), `widget/decklabbrowse.{h,cpp}`, `widget/decklabsmoke.cpp` (stick name), and the decklab
  system files.
- Lock: input hashes refreshed for the changed locked files. Smoke: +0 screens.

## Follow-ups

- Two identical controllers still share one Mixxx MIDI name (PortMidi), so Mixxx's per-name config can't
  tell their mappings apart; Deck Lab lists them correctly but Mixxx maps both alike.
- "Working · Numark-Mixtrack-3-…" in the wizard row shows the mapping's file name; the mapping's own name
  would read better.
