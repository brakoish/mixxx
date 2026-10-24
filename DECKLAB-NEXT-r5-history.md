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
  from the one where the DJ last held New set on. The set starts are kept in the settings (`[DeckLab]
  HistorySetStarts`: history playlist ids, never wall-clock times). The first run starts a set at that run's playlist;
  playlists from before it (older history) stay one set each and group by day as before.
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
  `applyBrowsePresentation`, `fillMixes`; a block of new functions appended at the end of the file.
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
