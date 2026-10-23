# Deck Lab library quick wins: next steps (qw-library workstream)

Branch `decklab/qw-library`, based on `decklab/round3` (usb-first included). Implements the library quick wins of the
UI/UX audit (`audit/library.md` §5, Investigations A and B). Generated fixtures only: no personal media, names or paths.

## What changed

### 1. The knob always works (C-01, Investigation B)
- **Ownership is decided when the event arrives**, from what is really open: `QApplication::activePopupWidget()` and
  `activeModalWidget()` (`src/library/decklabknob.h`, header-only, shared by both entry points). The cached
  `m_focusedWidget` no longer decides anything in Deck Lab.
- `LibraryControl::nativeKnob()` refreshes the focus classification at the top of every browse slot, then routes:
  Browse (`[DeckLabNative] browse_*`) or, when a menu/modal dialog really is open, that window (turn = Up/Down in menus,
  Tab/Backtab in dialogs; press = Return/Space; back = Escape, which closes it). Covers `MoveVertical`, `MoveUp/Down`,
  `ScrollVertical`, `MoveLeft/Right/Horizontal`, `MoveFocus*`, `GoToItem`, `[Playlist] SelectTrackKnob`,
  `SelectNext/PrevTrack`, `SelectPlaylist`, `SelectNext/PrevPlaylist`, `ToggleSelectedSidebarItem` (the last one had no
  check before).
- The Browse page applies the same check to `[DeckLabNative] browse_press/back/move/select/enter`, which scripts write
  directly (the Numark and SB2 overlays).
- **`[Library] focused_widget` tells scripts the truth in Deck Lab**: 4/5/6 only while a menu or dialog is really open;
  otherwise 1/2/3 (an unknown or missing focus is reported as the visible list, 2 or 3). While a menu/dialog is
  reported, a 250 ms poll re-checks it, because a kiosk without a window manager may never send a focus signal when it
  closes. A hidden popup/dialog window that stays `focusWindow()` is treated as gone.
- `WDeckLab` re-activates the main window (and focuses the visible list on Browse) after any of its popups, dialogs or
  tool windows hides, and when the application becomes active again (a utility window closed).
- **Log line** (debug level, always written to `mixxx.log`, at most twice a second per reason):
  `DeckLab knob: [Library] MoveVertical ignored by Browse because a menu is open (QMenu "decklab-browse-tools-menu"); the menu gets it`
  and `DeckLab knob: main window re-activated after a menu or dialog closed`.
- `setLibraryFocus()` no longer logs "No Mixxx window…" on every knob tick in Deck Lab.

**The owner's Numark Mixtrack Pro 3 (appliance repo, `mixxx/controllers/DeckLab-Numark.js`)** sends: turn =
`[Playlist] SelectPlaylist` (list levels, `[Sidebar] sidebar_visible` = 1) or `[Playlist] SelectTrackKnob` (track
lists), batched every 40 ms; press = `[DeckLabNative] browse_press` (Shift: `browse_back`). Both now go through the
ownership check above. **Two gates in that script are outside this fork** and need an appliance-repo change
(coordinator):
1. `rotate` and the release branch of `press` return early when `[Library] focused_widget >= 4`. With this branch the
   value is honest (only a really open menu/dialog), so a stale value no longer blocks it.
2. `rotate` returns early unless `[Tab] current == 1` (Browse). **On Decks, Sampler, Lab FX and Settings a turn does
   nothing; only a press opens Browse** ("I had to tap it and then use the knob" may also be this). Suggested script
   change: when `[DeckLabNative] active`, skip both gates and send `SelectPlaylist`/`SelectTrackKnob` anyway; the fork
   opens Browse on the first tick and never turns a tick into a key press on another page.

### 2. Search where you are (B-35, B-36)
- Search from a source's categories or values (a stick, the Library) searches that source's own tracks; from a stick's
  playlists it searches the stick; from a stick folder it searches that folder's files. Only the source list itself
  searches the whole Library. Typing replaces a category value filter but keeps the source.
- Breadcrumb: `DJ USB › Search “night” · 14` (count = rows in the filtered list).
- A stick that is still being read says "Still reading DJ USB…" instead of jumping elsewhere.

### 3. No hidden filter (B-42)
- Tools › Match Deck N filters the list you are in (or the source you are inside), never switches to the collection,
  never writes into the search box, and shows the Match bar with **Only** selected and a breadcrumb
  `… › Matches deck 1 · 37`. The bar's ✕ clears it. Leaving the list clears it too.
- Highlight and filter share one matcher (`decklab::matchFilterQuery`): compatible key (same, relative, ±1) AND the BPM
  window, as Mixxx search syntax (`bpm:114.68-129.32 key:8A | … key:8B | … key:7A | … key:9A`). Half/double tempo is no
  longer included by one and excluded by the other. A category filter or typed words stay required in every
  alternative. Pitch drift re-filters only when the window moves by ¼ BPM.

### 4. The match filter is obvious (B-49..B-53, B-22)
- **Match bar** (46 px, over track lists while matching is on): reference deck chip (deck-colour bar, `DECK 1 · AUTO`,
  `8A · 122.0`; a tap pins Auto/1/2/3/4), three 44 px segments ▼ slower / ⇅ both / ▲ faster (saved in
  `[DeckLab] BrowseMatchDirection`), the window `115–129 BPM` and key rule `KEY: SAME ±1 REL`, **Only** with the count of
  matching tracks, and ✕ (matching off). Hidden while the keyboard is up unless Only is on.
- No reference deck: chip "No deck", summary "Load a deck to match", header MATCH neutral with "Load a deck" (green only
  with a reference).
- BPM column (now 104 px): value plus a painted ▲/▼ delta (`▲1.0`, `▼2.0`, `±0`), bright inside the window.
- Card line: `Same key · ▼ 2.0 BPM slower`, `Relative 8A→8B`, `Key +1 (up)`, `Key −1 (down)`. Card empty state: "Pick a
  track" (B-21).
- Count: rows of the filtered list with Only; otherwise counted from the visible model (lists up to 4,000 rows; larger
  lists show the count once Only is on).

### 5. No eternal "Loading…" (B-10)
- After 20 s without a read (`[DeckLab] RekordboxReadTimeoutMs`) the categories become an error row "Couldn't read this
  stick's rekordbox export", **Try again** (reads it again) and **Browse its files** (the stick's folder view). A read
  that finishes late still opens the categories. The source list shows "rekordbox · couldn't read the export".
- "Loading…" is now "Reading…", and the card says "Reading DJ USB…".

### 6. The first screen says what's inside (B-01, B-03, B-05, U-03)
- New first level (`List::Home`, the category list widget with two-line 60 px rows, 24 px glyphs in text colour):
  1. **Every mounted USB stick first**, by filesystem label (`/dev/disk/by-label`, fallback "USB stick (3A1F)"), USB
     glyph, whether or not it holds a rekordbox export: "rekordbox · 1,204 tracks · 14 playlists", "rekordbox · tap to
     read", or for a plain stick "Files · 4 folders · 12 tracks" (top level only, one directory read).
  2. The library's sources in Mixxx's order: **Library** (was "Tracks"; "3,812 tracks on this unit"), Playlists
     ("22 playlists"), Crates, History ("Played in earlier sessions"), Recordings, external libraries.
  3. **More sources**: Auto DJ, Analyze, Computer and the Rekordbox folder, kept reachable (the full Mixxx list).
- Sticks are scanned at `/media/decklab/*` (appliance) and `/media/$USER/*`, `/run/media/$USER/*` (desktop), mounts only;
  the list refreshes every 1.5 s while it is shown (plug/unplug).
- A rekordbox stick opens its categories (if the rekordbox feature has not scanned yet, the fork starts the scan and
  reads the stick when its row appears). A plain stick opens a **folder view**: "Tracks in this folder · n" plus
  subfolders, system folders hidden (`System Volume Information`, `$RECYCLE.BIN`, `PIONEER`, dot folders). Tracks come
  from Mixxx's file browser (`Computer` feature) for that folder. Breadcrumbs use the label: `MUSIC › House`.
- One glyph per concept (B-04): Library, USB stick (with label notch), Playlist, Crate, History, Recordings (mic),
  Auto DJ, Analyze, More sources (grid). Chevron in `muted`, line separators instead of the zebra stripe (B-07).
- Breadcrumbs say "Library" for the collection and drop the "Rekordbox" level for sticks.

### 7. LOAD targets with identity (B-29, B-30)
- Each LOAD target paints a 6 px deck-colour bar and a numbered badge (dimmed, not removed, when disabled). Second line:
  "Empty", "Replaces Night Drive" (elided), or "ON AIR" (live-red frame, unchanged). Ready = raised fill + full-contrast
  text. Accessible names include what is replaced. `text()` stays "Load N".

### 8. Rail USB opens the stick (U-01, U-02)
- Rail USB (one lambda changed in the rail code) → Browse: one stick opens directly; several show the source list on the
  first; none shows the list with "No USB stick. Plug one in to play its music." It no longer waits for decklabd
  (always enabled). Storage stays in Settings (tool 10, "Check USB drives").

### 9. Rules
Text ≥ 14 px (rows 16, second lines 15, labels 14), 44 px targets (smoke enforces it on the Match bar too), brand tokens
and deck colours, `tr()` for new strings, no live red outside ON AIR/refusals.

## Files (for merging)
- New: `src/library/decklabknob.h` (header-only; no CMake change).
- Owned: `src/library/librarycontrol.{h,cpp}`, `src/widget/decklabbrowse.{h,cpp}`, Deck Lab delegates in
  `src/widget/wtracktableview.cpp` (BPM delegate, row match, BPM width), `src/widget/decklabsmoke.cpp` (Browse captures).
- `src/widget/wdecklab.cpp` browse functions: rewritten `showSources`, `showTracks`, `browseEnter`, `browseBack`,
  `browseSegments`, `refreshLoadButtons`, `resetBrowseLevels`, `clearBrowseFilter`, `enterCategories`, `showCategories`,
  `checkRekordboxImport`, `refreshBrowseCard`, `matchReferenceDeck`, `refreshMatchReference`, `setSearchOpen`; edited
  `buildBrowse` (Tools › Match Deck, match bar, timers, checked watchers), `bindBrowser` (search handler), `openCategory`
  (error-row actions), `eventFilter` (re-activation, appended block); new functions listed in `wdecklab.h` under
  "qw-library"; `DeckLabBrowseState` fields.
- Outside browse, minimal: the constructor's rail USB lambda (and USB no longer in `m_toolButtons`) and the
  `browse_press`/`browse_back` watchers (ownership check). `wdecklab.h`: one forward declaration and the browse
  declarations.
- Not touched: `kStyle`, rail chips, Settings, Decks/Lab FX/Sampler, toast stacking (`DeckLabToast` unchanged).
- `tools/decklab/pi-build-lock.json`: hashes refreshed (8 inputs); no new suites (new tests are in `NativeDeckLabTest`,
  `DeckLabBrowseMatchTest`, `DeckLabBrowseTest`).

## Tests
New: `NativeDeckLabTest.BrowseKnobKeepsWorkingAfterMenusAndDialogsCloseWithoutATap` (menu, modal QDialog and
QMessageBox open → knob goes to them, Browse stays; closed with `QSignalBlocker(qApp)` so no focus signal follows → the
next `MoveVertical`/`SelectPlaylist`/`browse_press` works without a tap and `focused_widget` < 4),
`SearchStaysInTheSourceYouAreIn`, `SourceListPutsSticksFirstAndSaysWhatIsInside` (also More sources and rail USB),
`UnreadableRekordboxStickStopsReadingAndOffersItsFiles`, `MatchBarShowsDeckWindowDirectionAndClears`,
`LoadTargetsCarryDeckIdentityAndWhatTheyReplace`; `DeckLabBrowseMatchTest.TempoWindowDirectionDeltaAndKeyWords`,
`MatchFilterIsOneQueryForKeyAndTempo`; `DeckLabBrowseTest.UsbSticksAreListedByLabelWithExportsMarked`.
Updated for the new first screen: `UsbUtilityReturnsDirectlyToNativeSources`, `GenericControllerBrowse…`,
`CollectionOpensCategories…` ("Library"), `BrowseToolsMatch…` (no hidden search), `ToolRequestCanRepeat…` (Settings'
storage tool instead of the rail USB). The fixture adds generated Auto DJ, Rekordbox and Computer roots and points the
stick scan at an empty temporary folder.
Captures: `browse-source-home.png`, `browse-usb-read-error.png`, `browse-match-bar.png`, `browse-load-identity.png`
(native), and in the app smoke `app-browse-01-sources.png` (a generated plain stick first) and the new
`app-browse-08-match-bar.png` (faster only + Only).

## CI and what is verified
| Run | Result | Notes |
|---|---|---|
| 39878104683 | cancelled by GitHub | "repository invocation blocked" after 1m38s (ARM64 quota for the day; qw-perf and qw-system were cancelled at the same moment). A later dispatch returned "Actions has been disabled for this repository". The coordinator stopped all further runs. |

**Verified locally (WSL, g++ 13, Qt 6.4, offscreen):**
- Every touched source (`wdecklab.cpp`, `decklabbrowse.cpp`, `librarycontrol.cpp`, `wtracktableview.cpp`,
  `decklabsmoke.cpp`, `decklabtest.cpp`) compiles with `-Wall -Wextra` against the real headers (syntax-only, moc for
  `wdecklab.h`/`librarycontrol.h`), no warnings.
- `decklabknob.h` against real Qt popups and dialogs (scratch harness): an open QMenu is the owner and gets Down/Escape;
  after it closes with `QSignalBlocker(qApp)` the focus window is **null and no focus signal fires** (the kiosk bug
  reproduced), while the live check returns Browse; modal QDialog and QMessageBox are detected, Space closes the box.
- `decklabbrowse.cpp` built with thin stubs for Track/KeyUtils/CoverArtCache: the three new unit tests from
  `decklabtest.cpp` (`TempoWindowDirectionDeltaAndKeyWords`, `MatchFilterIsOneQueryForKeyAndTempo`,
  `UsbSticksAreListedByLabelWithExportsMarked`) pass, plus painted-widget checks (LOAD deck bar colour when disabled,
  60 px two-line source rows vs 44 px category rows, Match chip deck colour).
- `build_pi.py --check-inputs` PASS after the lock refresh; `tools/decklab/test_build_pi.py` 7/7.

**Still needs CI (cannot run without the ARM64 Flatpak build):** link of the full app, the `NativeDeckLabTest` suite
(6 new tests and 5 updated ones, which exercise the real `LibraryControl`, `SidebarModel` and `WLibrary`), the app smoke
(`app-browse-01-sources.png` with a generated stick, new `app-browse-08-match-bar.png`) and the native captures
(`browse-source-home.png`, `browse-usb-read-error.png`, `browse-match-bar.png`, `browse-load-identity.png`). Risk notes
for the first run: the knob test closes menus and dialogs under `QSignalBlocker(qApp)` and expects the offscreen
platform to report them gone at once (confirmed in the harness); the rekordbox test waits 900 ms for a 300 ms timeout.

## Gaps and follow-ups
- Numark script gates (see §1): appliance-repo change needed for turns outside Browse.
- `C-05` legend ("MENU" while a popup owns the knob) and in-surface sheets instead of popup windows (B-41/43/45) are not
  done; the log line and the honest `focused_widget` are the feedback for now.
- Rekordbox stick track counts on the source list appear after the stick was opened once in the session.
- Plain-stick folder view counts only the top level. Very large folders are listed by Mixxx's file browser (tag reading
  in its own thread).
- Not verified on the Pi (no deploys). Check: knob after the Tools menu, a deck More menu, a Settings combo and the
  status message box; `grep "DeckLab knob" mixxx.log`; a stick plugged in while the source list is shown.
