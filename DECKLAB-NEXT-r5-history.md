# Deck Lab round 5, History: next steps (r5-history workstream)

Branch `decklab/r5-history`, based on `decklab/round4` (d381cded9e, the build on the owner's Pi). Builds the owner's
pick #2: track history and "played" marks, the rekordbox/CDJ way at 800x480. Generated demo data only. A second pass
fixes everything the 6-lens review confirmed (see "Review fixes").

## What changed

### Played marks in every Browse track list
- **This set**: the title dims to the secondary tone (#9AA3AF) and a muted disc with a dark check sits at the title's
  right edge. **Earlier set**: a quieter dim ring with a check, the row stays bright. Never red (red is live only). The
  highlighted row keeps its white text and shows the mark. Secondary 15 px text never goes darker than muted (brand).
- Where: Mixxx's track tables in Browse (Library, playlists, crates, search results, a rekordbox stick's lists, a plain
  stick's folders), "Mixes with deck N" rows, and earlier sets' rows in History (a track also played in this set).
- **Rule: Mixxx's own.** SetlogFeature adds the track that became audible (PlayerInfo: playing, volume, crossfader;
  not again within the duplicate distance) to the current history playlist; `PlaylistDAO::trackAdded` is that moment.
- In Deck Lab cells the history replaces Mixxx's per-start played colour (#555, which survives "New set" and ignores
  earlier sets), in the text and in the highlighted text Mixxx blends it into. Settings › Library › "Dim played tracks"
  drives the history's dimming too (the check stays).
- **Performance**: two hash sets of file locations (this set, earlier sets), read in one query when Browse is first
  shown (never at start-up), kept live from PlaylistDAO's signals. Only the title cell looks a row up (once per row,
  never per column), with `TrackModel::getTrackLocation` (the row's own data, no allocation, also for a stick's lists)
  and a hash lookup: never a database query. A change repaints only the visible rows whose mark changed, coalesced into
  one queued pass, and only while Browse is shown.
- rekordbox sticks: the marks match by file location, so a stick track played earlier (Mixxx adds it to the library
  on load, then to the history) is marked in the stick's own lists. An export's doubled slash is cleaned on a miss.
- **Moved files** (USB relink, Mixxx's relocate): TrackDAO's change signals are checked a moment later, only for
  history tracks and only when their location really changed; then the history is read again, so marks and History
  rows follow the new path. A save that moves nothing (play count, analysis) costs one small query and no reload.

### A set spans restarts
- Mixxx starts a new history playlist on every launch, and Deck Lab restarts Mixxx in normal use (Restart to detect
  after a controller replug, Power › Restart app, a crash, a power blip). A set is therefore every history playlist
  from the one where the DJ last held New set on. The set starts (history playlist ids, never wall-clock times) are
  kept in their own small file in the profile, `decklab-history-sets.json` (r5-fix2-history; earlier round-5 builds
  used `[DeckLab] HistorySetStarts`, moved once). The first run starts a set at that run's playlist; playlists from
  before it (older history) stay one set each and group by day as before.
- The sets list shows a set as one entry whatever restarts it spans; its tracks list in play order across them, each
  row bound to its own Mixxx playlist for LOAD.

### History, a first-class view
- The home's History tile or chip counts this set ("History 3"; tile lines "This set · 3 tracks" / "1 earlier set"),
  updated in place when a track plays (the home is not rebuilt). It and the Library's History category open the view;
  Back returns to where it came from.
- **Sets**: "This set" first ("Since 21:04 · 14 tracks"), then **New set**, then earlier sets under the day they began
  (Today, Yesterday, the weekday this week, "Sat 11 Oct", "31 Dec 2025"), each as its time span with
  "37 tracks · 4 h 09 min" and a count. Date headers are 14 px labels: the knob skips them and taps on them do nothing.
- **A set's tracks in play order**: time played (Barlow Semi Condensed, "21:04"), the deck badge in the deck's colour,
  title over artist, BPM and key; this set opens on its latest track, an earlier set on its first. A track that plays
  while the list is open is appended (the highlight follows the newest only when it was on the latest row; the sets
  level updates this set's row in place); while another page is shown it joins on the next visit. The deck is what
  this unit saw (Mixxx does not store it): tracks this unit did not see show a quiet "–" badge.
- **Clock**: a Pi 4 has no real-time clock. The view asks decklabd (`time.status`: synced or rtc, at most once a
  minute while Browse is shown); without decklabd it distrusts the clock when the history holds times later than now.
  Without a trusted clock earlier sets are "Set 12" under one "EARLIER SETS" label, this set's detail drops "Since",
  and track rows show "+0:12" after the set's first track while the times run forward within a day, else "#3".
  Durations over a day are never shown.
- Knob: turn moves (accelerating in track lists, C-03), press opens a set or previews a track, Shift+press goes back;
  the legend follows (LIST, then TRACKS). Touch: a tap opens a set or highlights a track.
- **The card follows the highlight at once**: the row's own facts (artist, key, BPM, time, deck in its colour) until
  the real Track is read 120 ms after the knob settles; never the track highlighted before.
- **LOAD from history**: a highlighted track row shows its own Mixxx history playlist in the hidden track table and
  selects its row by position and library id (ids survive a relocation; never another track; nothing when the table
  is not that playlist), so LOAD on the deck dock, Preview, the sampler pad flow and every stale-tap guard use Mixxx's
  own `LoadSelectedTrack`, as for Mixes.
- **New set**: the pane's hold button on the History levels (the System kit's `HoldButton`, 1.2 s sweep; a tap does
  nothing), and from the knob the "New set" row: press, then press again within 3 s, or a held press when the
  controller reports `browse_hold`. It runs SetlogFeature's "Finish current and start new"; the new playlist becomes the
  next set start; the last set stays listed with the quieter mark; the highlight lands on the new set. One read of the
  history at most: the store updates itself and the backend skips the reload its own new playlist would trigger. An
  empty set refuses another ("This set is still empty"; the row is disabled).
- Data: Mixxx's history playlists only (Playlists/PlaylistTracks, hidden = PLHT_SET_LOG) plus the set starts in the
  settings. No new tables or files.

## Review fixes (6-lens review, all confirmed items)
- Major: a set spans restarts (above). Correctness: the home count is updated in place on any page (no signature
  sentinel); the card and Preview never show the previous track; taps on date headers are ignored and the card falls
  back to the store; Mixxx's played colour is undone in HighlightedText too (and `drawBase` skips the blend); marks,
  History rows and LOAD follow relocations. Pi performance: in-place home tile, one read for New set, append/in-place
  list updates, one lookup per row without allocations. UX: New set from the knob; order and offsets without a
  trusted clock. Tests: below.

## Integration fixes (decklab/r5-fix-history, on merged round 5)
- **New set from the knob, as a whole.** A knob hold counts only when Browse owns the knob
  (`decklab::knobOwner().browse()`: never behind the status sheet, a dialog, a menu or the wizard). Any turn, Back
  or the highlight leaving the row cancels it. A full hold no longer fires at 1.2 s: the row says "Let go to end
  this set", and the gesture's release press starts it, then stops (it never opens the new, empty set). So Shift+press
  (Back on release for the Numark, at once for the SB2) never counts, and a push-turn never ends a set. "Press twice"
  now works with the Numark's (browse_hold 1, 0, press) sequence. Copy: "Hold the knob and let go, or press it twice".
- **Controller LOAD on list levels.** `[DeckLabNative] track_level` is 1 exactly when Browse highlights a loadable
  track row (a track table, a Mixes row, a History track row whose table row is its own track) and 0 on sources,
  categories, set lists and headers; the appliance overlay (`decklab/round5-app`) lets LOAD through on it.
- **Exactly the highlighted row.** LOAD (dock, sampler pad, controller) and Preview at History and Mixes track level
  check that the hidden table is the row's own playlist and its selected row the row's track; otherwise they resync
  and refuse. A table Mixxx swaps in underneath (a rekordbox read finishing) triggers the same resync.
- **Persistence.** `[DeckLab] HistorySetStarts` is written to disk at once (first run and each New set). Replaced
  in r5-fix2-history: Mixxx's config save is not crash-safe.
- **Pi budget.** A History knob step selects at once only within the playlist already shown; switching Mixxx's
  hidden playlist table waits for the knob to settle (120 ms, with the Track read). LOAD is not offered meanwhile.
- Tests: `NativeDeckLabTest.NewSetKnobFollowsTheControllersRealSequences` (push-turn, Numark and SB2 Shift+press,
  a hold under a menu, press twice, full hold with its release press, hold then turn), `track_level` assertions;
  the smoke checks `track_level` (0 on the sets, 1 on a set's track) and loads through the controller path
  (`LoadSelectedTrack`, into the preview deck, never played), expecting exactly the highlighted track.

## Verification fixes (decklab/r5-fix2-history, on round 5 at 95c18e0c18)
- **Set starts, crash-safe (major).** No more `m_config->save()`: a power cut during Mixxx's save (it deletes
  `mixxx.cfg` before renaming the new one, with no fsync) could leave every setting, the mapping and the sound setup
  empty. The starts live in `decklab-history-sets.json` in the profile (`{"version":1,"starts":[...]}`), replaced
  with `QSaveFile` after an `fsync` of the new file, then an `fsync` of the folder. Read at start; a profile from an
  earlier round-5 build moves its `[DeckLab] HistorySetStarts` once (the file is written, then the key is removed
  from the settings in memory; Mixxx's own next save drops it). A damaged file reads as "none yet".
- **`track_level`, exactly.** 1 exactly while Browse highlights a loadable track and the hidden table's selected row
  is that track, on every page: it follows the hidden table's selection, model resets and removals (re-hooked when
  Mixxx shows another model), and Deck Lab's own refreshes. Home, Sources, Categories, Values, Folder, the sets list
  and headers unselect the hidden table (a track table gets its row back when it is shown again), so hold-Back to
  Home, page switches and stick reads leave nothing for a LOAD.
- **Load-time guard.** `LibraryControl` (LoadSelectedTrack, LoadSelectedTrackAndPlay, the stems load and
  LoadSelectedIntoFirstStopped, for every deck, sampler and preview deck) asks the surface in native mode
  (`decklab::nativeLoadGuard()`, `decklabknob.h`) and refuses unless the highlighted row is loadable and selected;
  a History or Mixes row still waiting for its table is settled first, on Browse only. The Tools menu's "Load into
  sampler" uses the same check. A table swapped in under a History or Mixes row drops the selection and
  `track_level` at once (a direct `showTrackModel` handler), then the row comes back queued.
- **Order.** `browse_move` and `browse_select` run where LibraryControl sets them (the GUI thread, synchronously),
  so a turn always lands before a press or LOAD sent after it; a script's own write still queues.
- **New set.** A menu, sheet or dialog taking the knob (`decklab::onKnobOwnerChanged`) calls off an armed "press
  twice" and a hold in progress.
- **Mixes.** A swapped-in table is undone: the source is shown again (on Browse, once per swap) and the rows re-rank
  if the reference moved meanwhile. The candidates and the list are never rebuilt from another model of the same
  source; the card never reads another model's row.
- **Settle.** A knob press, a dock or pad LOAD and a controller's LOAD inside the 120 ms History settle switch the
  table now and act on the highlighted row; a track row that cannot load yet never just stops the running preview.
  LOAD taps no longer restart the settle, and a resync never postpones one already running. Nothing switches Mixxx's
  table while Browse is hidden: the row is marked and comes back on the next visit.
- Tests: `DeckLabHistoryDbTest.SetStartsLiveInTheirOwnFileAndMoveOnceFromTheConfig` (write, replace, no temporary
  file left, damaged and missing files, a missing folder, the one-time move, the file winning over a stale key) and
  `ASetSpansRestartsUntilNewSet` (reads the file; Mixxx's config file is never written).
  `NewSetKnobFollowsTheControllersRealSequences` adds the latched-Shift Numark sequence, the status sheet opened and
  closed between two presses (no new set), a synchronous turn, and the guard on the sets list and the home. The smoke
  swaps the collection's table in under a History row (`track_level` 0 and no selection at once; the controller LOAD
  then loads exactly the highlighted track) and checks the home (no selection, `track_level` 0). Its Sampler and
  Decks demo loads now go through the Library's track list, as a DJ's would.
- Hooks, each marked `r5-fix2-history`: `decklabknob.h` (`nativeLoadGuard`), `librarycontrol.cpp`
  (`nativeLoadAllowed` and one line in each load slot), `wdecklab.{h,cpp}` (the turn proxies, the Tools menu,
  `loadDeck`, `togglePreview`, `showTracks`, the Mixes functions, `initHistory`, new functions in the History block),
  `decklabhistory.{h,cpp}`, `decklabsmoke.cpp`; the lock's hashes refreshed.

## Pre-deploy fixes (decklab/r5-fix3-history, on round 5 at f1cf2c36d0)
- **The load-time check is the authority.** The appliance overlay (`decklab/round5-app` 51df24a) no longer gates
  LOAD, empty-deck PLAY, empty pads, eject or Shift+LOAD on `track_level` in native mode; only the playing-deck check
  stays. `LibraryControl`'s check (`decklab::nativeLoadGuard()`) therefore settles and allows every legitimate load:
  a highlighted History or Mixes track row still waiting for its table gets it at the press, on any page (Browse,
  Decks, Sampler, Lab FX: one table switch at press time, as a CDJ loads from every screen), then exactly that row
  loads. A turn still never switches the table while Browse is hidden. `track_level` keeps its exact meaning.
- **Mixes rows by file (major).** A Mixes row is its track's file (`LocationRole`); the index kept with it is only a
  checked shortcut. Any `select()` on the source model (the swapped table undone, a rescan, a sort) used to kill every
  row's persistent index, leaving the list unloadable until it was rebuilt. Now a row whose index went reads the
  rows again (the cache is marked stale by the model's resets, inserts, removals, moves and layout changes), keeping
  the highlighted file.
- **Touch.** While a History or Mixes track row waits for its table, the dock and pad LOAD tiles (and Preview) stay
  offered; the tap settles the row and loads it. "Pick a track first" is never shown while a track is highlighted;
  a row that still cannot load says "This track can't be loaded right now".
- **Set file.** The file is read once per session; the starts in memory are the truth afterwards, so a New set
  whose write failed is never undone by a reload. The write is retried on each later read, and the toast says when
  a New set could not be saved. A failed history read never prunes the starts or writes. A file that is there but
  unreadable (read twice) is copied to `decklab-history-sets.json.bad` before anything replaces it; without a copy
  nothing writes it this session. When Mixxx makes no new history playlist, nothing starts (only a playlist newer
  than every history playlist before the call counts, and never one the history already holds).
- Tests: `NativeDeckLabTest.OverlayLoadsSettleTheHighlightedRowOnEveryPage` drives LibraryControl's own controls
  as the overlay sends them (`[Channel1] LoadSelectedTrack`, `[Channel2] LoadSelectedTrackAndPlay`, `[Sampler1]
  LoadSelectedTrack`) on the sets list (refused, nothing settled), on a History track waiting for its table (each
  press settles the row first) and off Browse on Decks and Lab FX; `HistoryIsAFirstClassBrowseViewInPlayOrder` checks
  the offered tile and its message. `DeckLabHistoryDbTest.TheSetFileNeverLosesASet` covers a failed write kept and
  retried, a failed history read, an unreadable file copied aside, and Mixxx making no playlist. The smoke loads
  through the real controls after a swapped-in table: the preview deck and a pad inside the settle, and deck 2 off
  Browse from the Decks page, each expecting exactly the highlighted History track; and in Mixes it re-selects the
  Library's model, then the controller LOAD loads exactly the highlighted row.
- Hooks, marked `r5-fix3-history`: `wdecklab.{h,cpp}` (Mixes functions, `refreshLoadButtons`, `refreshPreviewButton`,
  `showLoadUnavailable`, `loadDeck`, `startNewHistorySet`, the History block), `decklabhistory.{h,cpp}` (the Backend
  gains `bool saveSetStarts`, `readOk`, `startsSaved`), `decklabsmoke.cpp`; the lock's hashes refreshed.

## Renders
`local/wt/audit/screens/r5-history/` (not committed):
- `after/` from the real app smoke (generated demo library, 800x480): `app-browse-12-history.png` (sets),
  `app-browse-13-history-set.png` (this set, LOAD ready on the dock, real track card), `app-browse-14-history-marks.png`
  (All Tracks: this set dimmed with a disc, last night's with a ring), `app-browse-15-history-mixes.png` (Mixes rows).
- `tests/` from NativeDeckLabTest: `browse-history-sets.png`, `browse-history-set.png`, `browse-history-new-set.png`.
- `before/` round 4 (no marks; History opened Mixxx's sidebar tree): `app-browse-09-four-decks.png`,
  `app-browse-11-mixes.png`.

## Verified locally (WSL 2, g++ 13, Qt 6.4, own build slot)
- `decklab-local.sh` full run on this branch: see the summary path in the final report. Lock suites, CI acceptance,
  DSP, GL pass and the app smoke (48 screens, no failures). The smoke opens the stick's files before History, then
  checks that the hidden table is the set's own Mixxx history playlist, that its selected row is the highlighted track
  by library id, and that the dock offers LOAD.
- `DeckLabHistoryDbTest` (new suite, in the lock), against Mixxx's real PlaylistDAO and schema: sets and play order;
  live marks with only the changed location and the added entry reported; New set with exactly one change and no
  second read; **a set across simulated restarts** (new store and backend, a new history playlist, starts in a
  settings file) and New set splitting it; **relocation** (marks and rows follow, an unmoved save does not reload, the
  set's real `PlaylistTableModel` row found by position and id); **Mixxx track tables through Deck Lab's real cell
  delegate** (a recording style captures what is painted: Mixxx's played colour undone, muted text and the disc for
  this set, the ring for an earlier set, white highlighted text, secondary cells unmarked, dim off).
- `DeckLabBrowseTest`: grouping, wording, the untrusted-clock rows and offsets, the store's clock judgement, the
  id-based row mapping, row roles and appends, the mark's pixels (the ring really drawn in the dim tone, hollow, never
  red, never tinted) on Mixes rows for both marks.
- `NativeDeckLabTest`: `HistoryIsAFirstClassBrowseViewInPlayOrder` (home tile in place, sets with New set, headers
  never current by knob or tap, play order with deck and time, the card following at once, no LOAD without the set's
  table, append and highlight rules, hidden-page join, Back) and `NewSetHoldStartsAnotherHistorySetAndKeepsTheLastOne`
  (knob press-twice, touch hold, `browse_hold`, starts saved, empty set refused).
- `python tools/decklab/build_pi.py --check-inputs`: PASS. `test_build_pi.py`: 10/10.
- Stricter Pi toolchain (Qt 6.10 / GCC 15) habits: every concatenation in a `?:` or passed as QVariant is wrapped in
  `QString(...)`; `findChild<T>` only on Q_OBJECT classes (`SetlogFeature`, `HoldButton`).

## Needs CI and the Pi
- CI (blocked): the aarch64 build with Qt 6.10 / GCC 15 and the lock suites (with `DeckLabHistoryDbTest`).
- Pi: a real set across a Restart to detect; marks appearing when a deck becomes audible (crossfader/volume); the
  dimmed rows and checks at arm's length in a dark booth; a rekordbox stick track's mark in the stick's lists and after
  a relink; LOAD from an earlier set that spans a restart; New set by finger and by knob; the clock wording on a Pi 4
  booted without network; first-visit cost of the played sets on a large history.

## Merge notes
- **New files**: `src/widget/decklabhistory.{h,cpp}` (Store, the Mixxx backend over PlaylistDAO and TrackDAO, set
  starts, clock judgement, wording, grouping, row mapping), `src/widget/decklabhistoryview.{h,cpp}` (the mark, the
  History rows, `DeckLabHistoryDelegate`, the table-cell styling), `src/test/decklabhistorytest.cpp`. No Q_OBJECT.
- **CMakeLists.txt**: the two `.cpp` after `src/widget/decklabbrowsemix.cpp`; the test after `decklabpadstest.cpp`.
- **`src/widget/wdecklab.h`**: `#include <QSet>`, two forward declarations, a declaration block after
  `syncMixSelection()`.
- **`src/widget/wdecklab.cpp`** (each line marked `r5-history`): includes (`decklabdclient.h`, `decklabsystem.h`,
  `playlisttablemodel.h`, `setlogfeature.h`, `<QJsonObject>`, the two new headers); `DeckLabBrowseState::List::History`;
  a field block at the end of `DeckLabBrowseState`; `buildBrowse()` list click (header guard, history tracks) and
  currentRowChanged; the settings `dimPlayedChosen` hook; `bindBrowser()`: the `trackSelected` skip and
  `initHistory()`; `attachSystem()` (one line); one case each in `browseEnter`, `browseBack`, `browseSegments`; one
  line each in `browseMove`, `openCategory`, `refreshBrowseCard`, `rebuildHome`, `openHomeRow`, `browseTrackLevel`,
  `applyBrowsePresentation`, `fillMixes`; `updateMatchBar()` re-lays its column after the bar's visibility is set
  (one line: after a stick's tracks the bar's 46 px stayed reserved over History and the home); a block of new
  functions appended at the end of the file.
- **`src/widget/wtracktableview.cpp`**: one include, `DeckLabCellDelegate::initStyleOption`/`mixxxPlayedColor`, the
  `drawBase` blend guard and `decklab::history::makeTrackCellDelegate` (for tests), all marked.
- **`src/widget/decklabbrowsemix.cpp`**: `paintMixRow` reserves the mark's slot and dims the title (marked).
- **`src/library/baseexternaltrackmodel.cpp`, `baseexternalplaylistmodel.cpp`**: `fieldIndex(QStringLiteral(...))`
  in `getTrackLocation` (no allocation per call; marked).
- **`src/widget/decklabsmoke.cpp`**: `seedDemoHistory()` and four Browse captures after "mixes".
- **Lock**: hashes refreshed for `CMakeLists.txt`, `decklabtest.cpp`, `decklabsmoke.cpp`, `wdecklab.{h,cpp}`,
  `wtracktableview.cpp`; `test_suites` gains `DeckLabHistoryDbTest.*` after `DeckLabBrowseTest.*`. New files are not
  locked inputs (as round 4).
- **Smoke screen count: +0 screens** (Browse captures are not counted; `build_pi.py` keeps 48). Browse captures go
  from 11 to 15: `app-browse-12-history`, `13-history-set`, `14-history-marks`, `15-history-mixes`.
- Not touched: `kStyle`, the rail, Decks, Lab FX, Sampler, Settings builders, LaunchImage, the setup wizard.

## Follow-ups
- The deck badge is remembered in memory only; a restart forgets it for tracks already played (Mixxx's schema has no
  deck column). The `[DeckLab]` record could keep it if the owner wants.
- A set now runs until New set, also across nights: the card could suggest New set when the last track is many hours
  old (a hint, never automatic).
- An "older than tonight" filter or "hide played" toggle on track lists, if DJs ask.
