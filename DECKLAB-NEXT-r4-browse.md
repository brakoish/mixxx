# Deck Lab round 4, Browse: next steps (r4-browse workstream)

Branch `decklab/r4-browse`, based on `decklab/round3` (5d4f4570f7). Builds the Browse signature moves of the UI/UX audit
(`audit/library.md` section 3) in the brief's order: move 1 (Source home), move 3 (the deck dock is the LOAD target),
move 4 part 2 (the knob legend) and move 5 ("Mixes with deck N"). Move 6 (the shared keyboard) is not in this branch.
Round 3's plumbing is kept: USB-first stick reading with its timeout and Try again, relink, phrase data, the folder
view for plain sticks, the Match bar, the stale-tap guards, knob ownership (`decklabknob.h`) and "never activate what
a fast knob passes". Generated demo data only.

## What changed

### Move 1: the Source home (B-01..B-06, U-01, U-03)
- The first Browse screen is a **2x2 grid of large source tiles and an "Also" row of chips**. The tiles are the rows of
  the Browse list widget laid out left to right (`QListView` flow layout, wrapping), so every list behaviour carries
  over: one current row, the knob, touch scrolling, the stale-tap rule (`clicked` = press and release on the same
  tile) and focus. Painted by `DeckLabBrowseDelegate` (`decklabbrowsehome.cpp`).
- Reading order: every mounted **USB stick first** (by label), then the library's sources in sidebar order (Library,
  Playlists, Crates, Recordings, History...), then More sources. The first four are tiles (about 391x116 at 800x480),
  the rest are 44 px chips after an "Also" label ("Recordings 3", "History 12", "More sources").
- With **no stick in**, the first tile is a dashed "No USB stick / Plug one in to play its music" tile. It is disabled:
  the knob and the default highlight skip it, so the layout stays the same when a stick arrives (the home refreshes
  every 1.5 s while shown, as in round 3).
- **Each tile says its name, what is inside and its state**: 24 px glyph in text colour, 18 px name, two 15 px muted
  lines, a state at top right and a tag row.
  - rekordbox stick: "rekordbox · 1,204 tracks · 14 playlists" / "exported 21 Oct" (the `export.pdb` date);
    states READY (filled white lamp), READING… (outlined lamp), COULDN'T READ (warning glyph) with a 44 px
    **Try again** target inside the tile, which starts a new read. A read that never finishes times out on the home
    too (`[DeckLab] RekordboxReadTimeoutMs`), so the tile can't stay READING forever (B-10).
  - plain stick: "Files · 4 folders · 12 tracks" / "29.8 GB · 12.1 GB free".
  - read-only mounts carry a READ-ONLY tag (`QStorageInfo`). No amber and no red: lamps are white (brand: deck colours
    for decks, live red for live things).
  - Library: "3,812 tracks on this unit" / "By artist, album, BPM and key"; Playlists and Crates name their first
    three entries.
- On the home the info pane, Back (nowhere to go) and MATCH (nothing to match) step aside; the header is "Sources",
  Search and More.
- **Rail USB lands on the home with the stick highlighted** (was: straight into a single stick). A press opens it.
- Round 3's data plumbing is unchanged: `findUsbSticks()`, `stickSubtitle()`, `homeRootSubtitle()`,
  `libraryTrackCount()`, `openHomeRow()` keys, `openStick()`, the folder view and More sources.

### Move 3: the deck dock is the LOAD target (B-25, B-29..B-31, B-56..B-59, B-33)
- Browse no longer shows the 104 px strip of mini readouts over waveform lanes. A **dock of deck tiles** runs along
  the bottom: two tiles of about 394x64 or four of 195x64 (four-deck layout, so decks 3 and 4 are always visible).
  The GL waveform lanes are hidden on Browse (they come back on Decks and Lab FX), so Browse no longer renders them.
- Each tile (`DeckLabDockTile`): a 6 px deck-colour bar (full colour even when disabled), the numbered badge, a state
  word (EMPTY, CUED, PAUSED, or ON AIR in a live-red outline), the remaining time (blinks red at the end-of-track
  warning), and the title with key and BPM in white/muted (never Mixxx's key palette, so 8A no longer reads red, B-58).
- **With a track highlighted**, every loadable tile shows a 44 px white-outlined **LOAD ›** target and says what it
  replaces ("replaces Sweet Talk", "empty deck"); narrow tiles say LOAD on their first line. **An on-air tile refuses
  in place**: live-red edge and wash with "On air. Pause it first." for 2.4 s, and the toast now sits just above the
  tapped tile instead of over the list (B-33). Copy from the audit's copy deck: "Deck 2 is on air. Pause it first.",
  "Open a list, then pick a track", "Pick a track first".
- The tiles **are** the old LOAD buttons (`DeckLabDockTile` derives from `DeckLabActionButton`, names `decklab-load-N`,
  text "Load N", detail "Empty"/"Replaces X"/"ON AIR", tones), so the stale-tap guards (target, interaction epoch,
  sampler target and selection captured on press, checked on click), the disabled-tap explanation and the sampler pad
  mode ("Load pad 5" / "Cancel") work unchanged. Round 3's deck-colour bars and "Replaces <title>" are folded in.
- The right pane keeps only the info card and Preview; Preview hides at list levels (B-31). The card gets the full
  pane height in the four-deck layout (B-25).
- While the search keyboard is up the dock moves into the pane above it (52 px tiles, 2x2 for four decks), so the list
  keeps its rows.
- The dock's clocks tick on Browse from the existing 100 ms deck tick (no new timer; a tile repaints only when its
  state changes, about once a second for a playing deck).

### Move 4, part 2: the knob legend (C-02, C-03, C-05, B-48)
- A 24 px TE-style strip above the dock (`DeckLabKnobLegend`): key-cap glyphs and 14 px uppercase verbs.
  - Home: `[TURN] CHOOSE  [PRESS] OPEN  [SHIFT+PRESS ×2] DECKS`
  - lists: `[TURN] CHOOSE  [PRESS] OPEN  [SHIFT+PRESS] BACK` … `HOLD BACK: SOURCES`
  - track lists and Mixes: `[TURN] CHOOSE  [PRESS] PREVIEW  [SHIFT+PRESS] BACK` … `TAP A DECK TO LOAD` (C-02)
  - a menu or dialog really owns the knob: a filled `MENU` tag, `[TURN] CHOOSE  [PRESS] SELECT  [SHIFT+PRESS] CLOSE`
    (C-05). It follows real popups and dialogs (Show/Hide of popup, dialog, sheet and tool windows, queued once).
  - It inverts for 150 ms when a knob event went to a menu instead of Browse (a listener in `decklabknob.h`) and when
    Back has nowhere to go.
- **B-48**: Back on the home no longer leaves Browse; it flashes the legend, which then says "AGAIN: DECKS"; a second
  Back within 2 s goes to Decks (controllers without a page button are never stuck). **Hold Back for 600 ms** (the
  header's Back, or a controller's `browse_back` held) goes to the source home from anywhere.
- **C-03, acceleration**: in track lists, value lists and Mixes, from the third tick under 45 ms apart in one
  direction a tick moves 4 rows (8 under 20 ms); a pause or a direction change starts over. Not on the home or
  category lists (no overshoot on short lists).

### Move 5: "Mixes with deck N" (B-11)
- Every source's category list (Library, a read rekordbox stick) starts with a live row **"MIXES WITH DECK 1"** in the
  deck colour (6 px bar, glyph, name) with "8A · 122.0 · 115–129 BPM" and the count. A new source opens on it.
- It follows the Match bar's reference deck, direction and window (`decklab::matchReference()`, also while the MATCH
  highlight is off), with the same rule as round 3's `decklab::matchFilterQuery`: compatible key (same, relative,
  +1, -1) AND inside the window. The reference deck's own track is never listed.
- Opening it lists the matches **sorted by closeness**: same key first, then relative/+1/-1, then by the absolute BPM
  difference. Each 44 px row: key pill (solid green same key, outlined for the others), BPM with `▲1.0`/`▼2.0`, title
  and artist, and the relation in words on the right: "same key, ▼1 BPM", "+1 key, ▲2 BPM", "relative, ▲0.5 BPM",
  "−1 key, same BPM". Breadcrumb "DJ USB › Mixes with deck 1 · 37"; the card explains the row
  ("Night Drive · 8A · 122.0 · on air" / "37 tracks in key and within 115–129 BPM").
- Live: the count updates when the reference moves by a quarter BPM or changes deck/key/direction; an open list
  rebuilds and keeps the highlighted track.
- A Mixes row is the highlighted track: it is selected in the (hidden) track table at once, so the dock's LOAD,
  Preview and every guard use Mixxx's own `LoadSelectedTrack`/`LoadSelectedTrackAndPlay`, and rekordbox tracks still
  load through `RekordboxPlaylistModel::getTrack()` (cues, grid, phrases). The card's Track object is read 120 ms
  after the knob settles, never for each row a fast knob passes. Press previews; a tap highlights.
- Candidates are read once per source from its own track model (key and BPM columns, up to 20,000 rows, no database);
  recounting is arithmetic.

## Renders (local only, not committed)
`local/wt/audit/screens/r4-browse/` (offscreen harness, 800x480, real widget code, generated data):
- `before/` (round 3 code): `home-list.png`, `categories-load-buttons.png`, `categories-four-load-buttons.png`.
- `after/`: `home-stick.png`, `home-no-stick.png` (legend flashed, "AGAIN: DECKS"), `home-reading.png`,
  `home-failed-and-plain.png`, `categories-mixes-row.png`, `mixes-two-decks-load-refused.png`,
  `mixes-four-decks-load.png`, `search-pane-dock-two.png`, `search-pane-dock-four.png`, `legend-levels.png`.
The harness can't build a real `Track`, so in the Mixes renders the card is drawn in item mode; the real app shows the
track card. The rail is the real `kRailStyle` with stand-in tabs.

## Verified locally (WSL, g++ 13, Qt 6.4, offscreen)
- Syntax-only `-Wall -Wextra` against the real headers (moc for `wdecklab.h`): `wdecklab.cpp`, `decklabbrowse.cpp`, the
  four new files, `decklabsmoke.cpp` and `decklabtest.cpp`: no errors, no new warnings (`decklabtest.cpp` keeps its
  existing gtest dangling-else warnings).
- The offscreen harness links the real `decklabbrowse.cpp`, `decklabbrowse{home,dock,legend,mix}.cpp` and
  `decklabdeckwidgets.cpp` with thin stubs (Track getters, covers, overlays, a Lancelot key parser) and checks 23
  layout facts: tiles 1 and 2 share a row and tile 3 starts row two, chips start their own row and fit without
  scrolling, tiles at least 380x96, chips and Try again 44 px, dock 64 px tiles at the bottom (two at ~400 px, four at
  ~200 px), pane dock 52 px fits over the keyboard, legend 24 px, Mixes row 60 px, the list shows every counted mix with
  the same key first, deck 2 loads while deck 1 (on air) refuses in place. All pass.
- The four new pure/widget tests, extracted verbatim from `decklabtest.cpp`, built with gtest against the real code:
  `DeckLabBrowseTest.HomeArrangesSticksFirstIntoFourTilesAndAnAlsoRow`,
  `DeckLabBrowseTest.KnobLegendVerbsFollowTheLevelAndFastTurnsAccelerate`,
  `DeckLabBrowseTest.DockTilesSayTheirDeckStateAndWhatALoadReplaces`,
  `DeckLabBrowseMatchTest.MixesRankByKeyThenTempoAndSayTheRelation`: 4/4 pass.
- `python tools/decklab/build_pi.py --check-inputs`: PASS after the lock refresh. `test_build_pi.py`: 7/7.
- Not available locally: the full app link, `NativeDeckLabTest` (needs the real `LibraryControl`, `SidebarModel`,
  `WLibrary`, engine) and the app smoke. WSL has gtest but not gmock.

## Needs CI and the Pi
- CI (ARM64 build, blocked for now): the full link; `NativeDeckLabTest` with four new tests
  (`SourceHomeIsTilesInReadingOrderWithStates`, `DeckDockIsTheLoadTargetAndRefusesOnAirInPlace`,
  `KnobLegendFollowsTheLevelAndSaysMenu`, `MixesWithDeckRowLeadsTheCategoriesAndOpensItsList`) and six updated
  on purpose (`TouchSearchUsesNativeKeyboardAndReturnsWaveforms`, `WaveformLayoutPreferenceRoundTrips…`,
  `GenericControllerBrowseOpensBrowseWithoutTouchingDecks`, `BrowseKnobKeepsWorkingAfterMenusAndDialogsClose…`,
  `SourceListPutsSticksFirstAndSaysWhatIsInside`, `PlayingDeckLoadShowsWhyAndKeepsTheDeck`); the app smoke and its
  Browse captures. Risk notes: the knob-row tests now start on row 1 (row 0 is the disabled no-stick tile); the hold
  test waits 750 ms; the legend test relies on the offscreen platform reporting the Tools menu as the active popup
  (round 3's knob test already does).
- Pi: the tile layout and fonts at arm's length; that hiding the GL lanes on Browse and showing them on Decks is
  smooth (it already happens for Sampler and Settings); a real rekordbox stick through NotRead → READING → READY and a
  damaged export → COULDN'T READ → Try again on the home; the legend saying MENU over the Tools menu, a deck More menu
  and a Settings combo; the dock's clocks while Browse is open; Mixes on a large library (candidate read time on
  entering a source with 10k+ tracks).

## Merge notes
- **New files**: `src/widget/decklabbrowsehome.{h,cpp}` (move 1: arrangement, states, tile/chip/Mixes-row
  delegate), `src/widget/decklabbrowsedock.{h,cpp}` (move 3), `src/widget/decklabbrowselegend.{h,cpp}` (move 4
  legend, knob accelerator), `src/widget/decklabbrowsemix.{h,cpp}` (move 5 ranking, words, row painting). No
  `Q_OBJECT`, so no moc entries.
- **CMakeLists.txt**: the four `.cpp` after `src/widget/decklabbrowse.cpp`.
- **Owned, edited**: `src/widget/decklabbrowse.{h,cpp}` (roles appended to `DeckLabListDelegate::Role`,
  `Glyph::Mixes` appended, `DeckLabToast::showMessage` anchor parameter), `src/library/decklabknob.h`
  (`knobRedirectedListener()`), `src/widget/decklabsmoke.cpp` (Browse captures), `src/test/decklabtest.cpp`.
- **`src/widget/wdecklab.h`**: two forward declarations and a block of declarations under "r4-browse" after
  `reclaimFocus()`.
- **`src/widget/wdecklab.cpp`** (all marked `r4-browse`): includes; `DeckLabBrowseState` (`List::Mixes`, a field
  block at its end); the constructor's `browse_back` watcher (hold); `buildBrowse()` (page frame, Back hold, delegate,
  list click/row hooks, the dock replacing the LOAD grid, legend, timers, knob listener); `bindBrowser()` (one line in
  the `trackSelected` handler); **`selectPage()` one line: `m_waveforms->setVisible(page == 0 || page == 3)`**;
  `showSources`, `showTracks`, `browsePress`, `browseEnter`, `browseMove`, `browseBack`, `browseSegments`, `loadDeck`,
  `refreshLoadButtons`, `enterCategories`, `showCategories`, `openCategory`, `checkRekordboxImport`,
  `refreshBrowseCard`, `refreshPreviewButton`, `togglePreview`, `refreshMatchReference`, `showLoadRefused`,
  `showLoadUnavailable`, `setSearchOpen`, `rebuildHome`, `refreshHomeIfSticksChanged`, `openStick`,
  `retryRekordboxRead`, `openUsbInBrowse`, `showOnlyMatches`, `reclaimFocus`; **Decks-side hooks, one line each:
  `refreshDeckClock()` and `applyDeck()` call `refreshDockFor(deck)`, `deckShown()` counts page 1 in four-deck mode,
  `tickDecks()`'s visibility check lets page 1 tick and its end-warning blink reaches the dock**; `eventFilter()`'s
  qw-library popup block also reacts to Show; a new marked block of functions appended at the end of the file.
  Not touched: `kStyle`, `kRailStyle`, the rail, Decks/Lab FX/Sampler/Settings builders, `updateLayers()`.
- **Lock**: `tools/decklab/pi-build-lock.json` hashes refreshed for `CMakeLists.txt`, `decklabtest.cpp`,
  `decklabbrowse.{h,cpp}`, `decklabsmoke.cpp`, `wdecklab.{h,cpp}`. No new suites (new tests are in
  `NativeDeckLabTest`, `DeckLabBrowseTest`, `DeckLabBrowseMatchTest`). The new files are not locked inputs (as round
  3's `decklabknob.h`); the coordinator may add them.
- **Smoke screen count: +0 screens** (Browse captures are not counted; `build_pi.py` keeps 46). Browse captures go
  from 9 to 11: `app-browse-01-sources.png` now shows the tile home with the generated demo stick highlighted, reached
  through the rail's USB; new `app-browse-10-mixes-row.png` and `app-browse-11-mixes.png` (four-deck dock, LOAD on
  every paused deck).

## Follow-ups
- Appliance repo (not touched): the Numark and SB2 overlays pulse `browse_back` on release, so a controller's
  hold-Back to the home needs them to send 1 on press and 0 on release.
- C-04 (`LoadSelectedIntoFirstStopped` silent in native mode) and the Shift+turn letter jump of C-03 are not done.
- The rail USB lamp with the stick's label (U-01's second half) belongs to the rail's owner.
- Mixes rebuilds on reference changes but not when the reference deck loads another track with the same key and BPM.
- Move 6 (one keyboard), B-47 tappable breadcrumb, B-12 BPM bands, B-14 letter rail: later.
- The first tile could show the newest four covers of a stick (B-02); the tiles' second lines leave room for it.
