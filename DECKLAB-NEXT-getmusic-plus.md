# Deck Lab: Get Music and Sample Maker in the kiosk (workstream notes)

Branch `decklab/getmusic-plus`, based on `decklab/integration`. The appliance side (pages, bridge, touch keyboard)
is in the appliance repo, `docs/next/getmusic-plus.md`.

## What changes for a DJ

- **New tracks show up after Get Music or Sample Maker closes.** No key injection. When decklabd reports
  `apps.changed {"app": null}`, Deck Lab shows "Looking for new tracks…" and runs an in-process library rescan.
  When the scan finishes, it says "N new tracks are in your library." or "No new tracks found."
- **Downloads land where the library looks.** Before the rescan, Mixxx adds these folders as library folders
  if no existing library folder already covers them:
  - `Deck Lab Downloads` at the top of a mounted drive that decklabd lists (`usb.list` / `usb.changed`), where Get
    Music saves to a USB drive;
  - `~/Music/New Music` (Get Music on the Pi card);
  - `~/Music/Deck Lab Samples` (Sample Maker).

  It only adds plain folders that exist (no links), and never the whole drive.
- **No dialogs over the kiosk.** These rescans show neither the scan progress dialog nor the scan summary
  dialog. Other rescans (menu, startup) behave as before.
- **Sample Maker works in the kiosk again.** It refuses to edit audio while anything plays. In the labwc desktop it
  learned that from the old Python companion's `state.json`, which the Xorg kiosk doesn't run. While a utility is
  open, Mixxx now writes `$XDG_RUNTIME_DIR/decklab/deck-state.json` once a second:
  `{"schema": 1, "writer": "mixxx", "updated_at": <unix s>, "playing": bool, "recording": bool, "app": "<name>"}`.
  It removes the file when the utility closes. The file lives on tmpfs in the directory Mixxx already shares with
  decklabd (`--filesystem=xdg-run/decklab`). `playing` covers decks, samplers and the preview deck.

## Design

- `src/decklab/decklabutilities.{h,cpp}` (new) holds the policy behind `Hooks`, so it is testable without a library,
  players or decklabd:
  - the open/close state comes from `apps.changed`;
  - the deck state file is published every 1 s, written atomically with `QSaveFile`;
  - the refresh waits while a track is loading. It retries every 0.5 s, for 30 s at most, so a stuck load can't
    block it forever. It does not wait for playback: Mixxx scans on one worker thread, which is safe while
    playing, and the old Ctrl+Shift+L path didn't wait either;
  - missing library folders are added, then the quiet scan starts;
  - only the summary of its own quiet scan is reported.
- `DeckLabSystem` owns one `DeckLabUtilities` and wires it up:
  - it forwards `apps.changed`;
  - `Services.library` gives it the `TrackCollectionManager`, which it uses for `getRootDirStrings`,
    `addDirectory` (both `Ok` and `AlreadyWatching` count as covered) and `startLibraryScanQuietly`;
  - for "loading", it watches every deck, sampler and preview player: `loadingTrack(new)` starts a load, and
    `newTrackLoaded` or `playerEmpty` ends it. A load older than 20 s no longer counts;
  - its notices go to the existing toast.
- Library:
  - `LibraryScanner::scanQuietly()` returns false if a scan is already starting or running;
  - `LibraryScanResultSummary.quiet` marks those scans;
  - `LibraryScannerDlg::suppressNextScan()` is queued before `scanStarted`, so the progress dialog stays hidden for
    that one scan;
  - `TrackCollectionManager::startLibraryScanQuietly()` doesn't assert when there is no scanner (tests);
  - `MixxxMainWindow::slotLibraryScanSummaryDlg` ignores quiet summaries.
- `mixxxmainwindow.cpp` passes `services.library = getTrackCollectionManager()`.

## Contract

No contract change. The client already subscribed to the `apps` topic and now handles `apps.changed`. The deck state
file is a new local interface between this fork and Sample Maker (appliance `apps/sample-maker/sample_maker.py`).
Sample Maker accepts a fresh file only: younger than 3 s, at most 1 s in the future, and `schema` 1. Otherwise it
falls back to the legacy companion state, and when neither is fresh it refuses to edit audio.

## Tests

`DeckLabSystemTest.UtilityCloseAddsDownloadFolderAndRescansAfterDeckLoads` is gated in `build_pi.py`. It drives the
real client and `DeckLabSystem` through `DeckLabFakeService` events and checks:

- opening publishes the deck state, and it is republished when playing or recording changes;
- closing removes the file;
- while a deck loads, nothing scans and no notice appears; when the load ends, exactly one scan starts with one
  "Looking for new tracks…";
- only the uncovered download folder is added;
- summaries are reported once, for our quiet scan only;
- a second close only rescans;
- a scan that can't start says nothing;
- folder coverage uses path components, so `/srv/Musical` isn't covered by `/srv/Music`.

`pi-build-lock.json` input hashes were refreshed for `CMakeLists.txt`, `src/mixxxmainwindow.cpp` and
`tools/decklab/build_pi.py`.

## CI

See the run table in the appliance notes (`docs/next/getmusic-plus.md`).

## Follow-ups

- Library folders on removable drives behave as in upstream Mixxx. Tracks there show as missing while the drive is
  out and come back on the next rescan once it's plugged in.
- decklabd emits `apps.changed {"app": null}` only when it knew the app was open. If decklabd restarts while a
  utility is open, the close isn't reported. It would be cleaner if `apps.close` always reported a stopped unit
  (decklabd workstream).
