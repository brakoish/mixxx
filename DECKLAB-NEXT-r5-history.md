# Deck Lab round 5, History: next steps (r5-history workstream)

Branch `decklab/r5-history`, based on `decklab/round4` (d381cded9e, the build on the owner's Pi). Builds the owner's
pick #2: track history and "played" marks, the rekordbox/CDJ way at 800x480. Generated demo data only.

## What changed

### Played marks in every Browse track list
- **This set**: the title dims to the secondary tone (#9AA3AF) and a muted disc with a dark check sits at the title's
  right edge. **Earlier set**: a quieter dim ring with a check, the row stays bright. Never red (red is live only). The
  highlighted row keeps its white text and shows the mark. Secondary 15 px text never goes darker than muted (brand).
- Where: Mixxx's track tables in Browse (Library, playlists, crates, search results, a rekordbox stick's lists, a plain
  stick's folders), "Mixes with deck N" rows, and earlier sets' rows in History (a track also played in this set).
- **Rule: Mixxx's own.** SetlogFeature adds the track that became audible (PlayerInfo: playing, volume, crossfader;
  not again within the duplicate distance) to the current history playlist; `PlaylistDAO::trackAdded` is that moment.
  This set = the newest history playlist (SetlogFeature makes one at start-up and for "Finish current and start new");
  every other history playlist is an earlier set.
- In Deck Lab cells the history replaces Mixxx's per-start played colour (#555, which survives "New set" and ignores
  earlier sets). Settings › Library › "Dim played tracks" now drives the history's dimming too (the check stays).
- **Performance**: two hash sets of file locations (this set, earlier sets), read in one query when Browse is first
  shown (never at start-up), kept live from PlaylistDAO's signals. A row asks "played?" while painting with one
  `TrackModel::getTrackLocation` (the row's own data) and a hash lookup: never a database query. A change repaints only
  the visible rows whose mark changed (`repaintPlayed`), coalesced into one queued pass, and only while Browse is shown.
- rekordbox sticks: the marks match by file location, so a stick track played earlier (Mixxx adds it to the library
  on load, then to the history) is marked in the stick's own lists. An export's doubled slash is cleaned on a miss.

### History, a first-class view
- The home's History tile or chip counts this set ("History 3"; tile lines "This set · 3 tracks" / "1 earlier set").
  It and the Library's History category open the view; Back returns to where it came from.
- **Sets**: "This set" first ("Since 21:04 · 14 tracks"), then earlier sets under the day they began (Today,
  Yesterday, the weekday this week, "Sat 11 Oct", "31 Dec 2025"), each as its time span with "37 tracks · 4 h 09 min"
  and a count. Date headers are 14 px labels the knob and taps never land on.
- **A set's tracks in play order**: time played (Barlow Semi Condensed, "21:04"), the deck badge in the deck's colour,
  title over artist, BPM and key; this set opens on its latest track, an earlier set on its first. A track that plays
  while the list is open joins it in place and the highlight stays; while another page is shown it joins on the next
  visit (no work on hidden pages). The deck is what this unit saw (Mixxx does not store it): sets from before a
  restart show a quiet "–" badge.
- Knob: turn moves (accelerating in track lists, C-03), press opens a set or previews a track, Shift+press goes back;
  the legend follows (LIST, then TRACKS). Touch: a tap opens a set or highlights a track.
- **LOAD from history**: opening a set shows its Mixxx history playlist in the hidden track table; a highlighted row
  selects its table row (same position and file, else the same file, never another track), so LOAD on the deck dock,
  Preview, the sampler pad flow and every stale-tap guard use Mixxx's own `LoadSelectedTrack`, as for Mixes. The card
  shows the real Track once the knob settles (120 ms).
- **New set**: a hold button in the pane on the History levels (the System kit's `HoldButton`, 1.2 s sweep; a tap does
  nothing). It runs SetlogFeature's "Finish current and start new"; the last set stays listed under its day and its
  tracks keep the quieter mark. An empty set refuses another ("This set is still empty").
- Data: Mixxx's history playlists only (Playlists/PlaylistTracks, hidden = PLHT_SET_LOG). No new tables or files.

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
  DSP, GL pass and the app smoke (48 screens, no failures; the History captures check that a history track is
  loadable from the dock).
- New tests: `DeckLabHistoryDbTest` (new suite, added to the lock) against Mixxx's real PlaylistDAO and schema:
  sets and play order from history playlists, other playlists ignored; marks follow `trackAdded` live with only the
  changed location reported; New set turns this set's marks into earlier marks and an empty set refuses another.
  `DeckLabBrowseTest`: grouping and wording of the sets, the table-row mapping, the rows' roles and opening row, the
  mark (never red, selected never tinted, on Mixes rows, doubled-slash paths). `NativeDeckLabTest`:
  `HistoryIsAFirstClassBrowseViewInPlayOrder` (home tile, sets, knob skips headers, play order with deck and time,
  LOAD through LoadSelectedTrack, live join, hidden-page join, Back) and
  `NewSetHoldStartsAnotherHistorySetAndKeepsTheLastOne`.
- `python tools/decklab/build_pi.py --check-inputs`: PASS. `test_build_pi.py`: 10/10.
- Stricter Pi toolchain (Qt 6.10 / GCC 15) habits: every concatenation in a `?:` or passed as QVariant is wrapped in
  `QString(...)`; `findChild<T>` only on Q_OBJECT classes (`SetlogFeature`, `HoldButton`).

## Needs CI and the Pi
- CI (blocked): the aarch64 build with Qt 6.10 / GCC 15 and the lock suites (with `DeckLabHistoryDbTest`).
- Pi: a real set: marks appearing when a deck becomes audible (crossfader/volume, PlayerInfo), the dimmed rows and
  checks at arm's length in a dark booth, a rekordbox stick track's mark in the stick's lists, LOAD from an earlier
  set, the New set hold with a finger, first-visit cost of the played sets on a large history.

## Merge notes
- **New files**: `src/widget/decklabhistory.{h,cpp}` (Store, the Mixxx backend over PlaylistDAO, wording, grouping,
  row mapping), `src/widget/decklabhistoryview.{h,cpp}` (the mark, the History rows, `DeckLabHistoryDelegate`, the
  table-cell styling), `src/test/decklabhistorytest.cpp`. No Q_OBJECT.
- **CMakeLists.txt**: the two `.cpp` after `src/widget/decklabbrowsemix.cpp`; the test after `decklabpadstest.cpp`.
- **`src/widget/wdecklab.h`**: `#include <QSet>`, two forward declarations, a declaration block after
  `syncMixSelection()`.
- **`src/widget/wdecklab.cpp`** (each line marked `r5-history`): includes (`setlogfeature.h`, the two new headers);
  `DeckLabBrowseState::List::History`; a field block at the end of `DeckLabBrowseState`; `buildBrowse()` list click
  and currentRowChanged (one line each); the settings `dimPlayedChosen` hook (one line); `bindBrowser()`: the
  `trackSelected` skip and `initHistory()`; one case each in `browseEnter`, `browseBack`, `browseSegments`; one line
  each in `browseMove`, `openCategory`, `refreshBrowseCard`, `rebuildHome`, `openHomeRow`, `browseTrackLevel`,
  `applyBrowsePresentation`, `fillMixes`; a block of new functions appended at the end of the file.
- **`src/widget/wtracktableview.cpp`**: one include and `DeckLabCellDelegate::initStyleOption` (marked).
- **`src/widget/decklabbrowsemix.cpp`**: `paintMixRow` reserves the mark's slot and dims the title (marked).
- **`src/widget/decklabsmoke.cpp`**: `seedDemoHistory()` and four Browse captures after "mixes".
- **Lock**: hashes refreshed for `CMakeLists.txt`, `decklabtest.cpp`, `decklabsmoke.cpp`, `wdecklab.{h,cpp}`,
  `wtracktableview.cpp`; `test_suites` gains `DeckLabHistoryDbTest.*` after `DeckLabBrowseTest.*`. New files are not
  locked inputs (as round 4).
- **Smoke screen count: +0 screens** (Browse captures are not counted; `build_pi.py` keeps 48). Browse captures go
  from 11 to 15: `app-browse-12-history`, `13-history-set`, `14-history-marks`, `15-history-mixes`.
- Not touched: `kStyle`, the rail, Decks, Lab FX, Sampler, Settings builders, LaunchImage, the setup wizard.

## Follow-ups
- The deck badge is remembered in memory only; a restart forgets it for sets already played (Mixxx's schema has no
  deck column). A small `[DeckLab]` record could keep it if the owner wants.
- After a power cut Mixxx starts a new history playlist: "Join with previous" (desktop only today) could become a
  hold on the History view ("Continue last set").
- New set from the knob (HoldButton supports `browse_hold`) once Browse routes the knob to pane controls.
- An "older than tonight" filter or "hide played" toggle on track lists, if DJs ask.
