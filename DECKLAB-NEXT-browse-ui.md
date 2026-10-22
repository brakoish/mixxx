# Deck Lab Browse: next steps (browse-ui workstream)

Branch `decklab/browse-ui`. Notes for the integration pass. Generated fixtures only; no personal media, names or paths.

## What changed

### Layout (800x480 design target, scales to 1024x600 and 1280x800)
- **Header (44 px):** icon Back, a painted breadcrumb that drops middle levels first (`Rekordbox › … › Playlist`) and never
  truncates the deepest level, a **Match** toggle (state line shows `D1 · 8A · 122`, `No deck` or `Off`), Search and Tools.
- **Right pane** (236–340 px, follows the surface width): a details card, a one-tap **Preview** button and the protected
  **LOAD** targets. LOAD buttons keep the press/target/selection guard, show `Empty deck`, `Paused` or a red
  `On air`, and stay 52 px tall. With the search keyboard open the card hides and Preview/LOAD stay.
- **Details card** (custom painted, fixed geometry, so a selection change only repaints it): artwork from Mixxx's
  `CoverArtCache` (asynchronous, 128 px cached copies, never blocks the GUI), title, artist, BPM/KEY/TIME chips (key chip
  green when harmonically compatible, BPM chip bright-edged within ±6%), rating, track colour, a `Same key · −1.6%`
  line, album/genre and comment. On list levels it describes the highlighted source, category or value.
- **Feedback:** tapping an ON AIR target, or a controller LOAD that Mixxx refuses (upstream `LoadWhenDeckPlaying`
  unchanged), shows a transient toast: "Deck 2 is playing – pause it to load". Tapping a disabled LOAD with no track says
  what to do instead.

### Typography and colour (brand rules, binding)
- Painted text uses the on-device scale from `brand/README.md`: rows and buttons 16 px Medium/SemiBold, titles 18 px
  SemiBold, secondary 15 px Medium muted `#9AA3AF`, labels and column headers 14 px SemiBold uppercase, nothing below 14 px,
  no italics. Helpers `decklab::titleFont/bodyFont/strongFont/secondaryFont/labelFont/numberFont` request Barlow (and
  Barlow Semi Condensed SemiBold with tabular figures for BPM, key, time and counts) and fall back to the surface font.
- Tokens: ground `#0D0F12`, panel `#16191E`, raise `#20242B`, line `#2C313A`, text `#F2F4F7`, muted `#9AA3AF`,
  live `#FF3D2E` (ON AIR and refused loads only). Selection is white text on raise with a white edge, never red. Key
  matches are green (`#3ECF8E`), distinct from the four deck colours.
- Browse-scoped style sheet on the page widget sets header/table sizes and selection colours, so other pages are untouched.
- Touch targets: the surface style sheet's `QPushButton` `min-height` overrides `setFixedHeight()` in the real app, so
  every Browse button pins its height in its own style sheet (`holdHeight`: header and Preview 44 px, LOAD 52 px, search
  keyboard 5 rows x 44 px with 56 px punctuation keys). The smoke Browse captures fail on any visible Browse button under
  44 px; the scaling test checks the same in the fixture.

### Categories (existing library features and models only)
- Opening the collection root (`tracks` feature) or a rekordbox USB export shows categories first: All Tracks,
  Playlists, Crates, Artist, Album, BPM, Key, History (each only where the data exists; USB exports have no History).
- Artist/Album/BPM/Key values are collected from the source's own track model and open with native search queries:
  `artist:="…"`, `album:="…"`, `bpm:121.5-122.49`, `key:8A`, `…:""` for missing values. No new database.
- Playlists/Crates/History jump into the existing sidebar subtree; Back from its top returns to the categories.
- USB exports import asynchronously; categories show "Loading…" and enable when the import finishes.
- `SearchQueryParser::setAvailableColumns()` (set by `BaseTrackCache`) makes `artist:` and `key:` work on external tables
  that lack `album_artist`/`key_id` (key text is matched in Lancelot, Open Key and traditional spellings). The Mixxx
  library is unchanged (parser test covers both).

### Harmonic and tempo match
- Reference deck = the same rule as Tools > Match Deck (playing sync leader, else playing deck, else a loaded displayed
  deck). Its `visual_key` and `bpm` are cached and refreshed through a 150 ms coalescing timer; only a meaningful change
  repaints the visible rows (`viewport()->update()`), never a model reset.
- Rows whose key is the same, relative (same Lancelot number) or ±1 on the wheel get a green key pill, a row marker and a
  faint tint; BPM within ±6% is bright text on the raise fill (other BPM/key cells are muted secondary text). Diagonal moves are not highlighted (Mixxx's broader `~key:` search still finds
  them). On by default; the Match toggle persists in `[DeckLab] BrowseMatch`.

### Track table
- **Key → BPM → Title → Artist** (after playlist order) is now Deck Lab's **default**, not a forced layout: header dragging
  and Mixxx's own column chooser (Tools > Choose columns…, since touch has no right click) change it, saved per view in the
  model setting `decklab_columns`; Tools > Default columns resets. The desktop header state is still never overwritten.
- Title stretches, Artist gets ~40% of the free width (110–320 px), recomputed only on width changes.
- Pi scrolling: `ScrollPerPixel`, fixed 44 px rows, no hover tracking, overshoot off, 60 fps scroller,
  and `moveSelection()` jumps once per encoder tick instead of selecting every row in between.

### Generic controller browsing (any mapping)
`LibraryControl` forwards to Deck Lab whenever no popup menu or dialog owns navigation:
| Control | Deck Lab action |
|---|---|
| `MoveVertical`, `MoveUp/Down`, `[Playlist]SelectTrackKnob`, `SelectNext/PrevTrack`, `ScrollVertical` (×5) | `browse_move`: opens Browse if needed, moves the visible list |
| `[Playlist]SelectPlaylist`, `SelectNext/PrevPlaylist` | `browse_select`: moves the source/category list |
| `MoveFocusForward`, `MoveFocus>0`, `GoToItem`, `ToggleSelectedSidebarItem` | `browse_press`: open Browse, enter, or Preview in a track list |
| `MoveRight`, `MoveHorizontal>0` | `browse_enter`: enter only (nothing in a track list) |
| `MoveLeft`, `MoveFocusBackward`, `MoveHorizontal<0` | `browse_back` |
A turn on Decks/Sampler/FX/Settings opens Browse at its last location and never becomes a key press on a deck, tempo
or settings control. The existing SB2/Numark script redirects keep working (same controls).

## wdecklab.cpp functions changed (for merging)
- Rewritten: `buildBrowse`, `bindBrowser`, `showSources`, `showTracks`, `browsePress`, `openSource`, `browseBack`,
  `updateBreadcrumb`, `loadDeck`, `refreshLoadButtons`, `setSearchOpen`, `openCollection`.
- New (browse only): `browseMove`, `browseEnter`, `resetBrowseLevels`, `enterCategories`, `showCategories`,
  `openCategory`, `openCategoryValue`, `activateBrowseSource`, `clearBrowseFilter`, `checkRekordboxImport`,
  `syncSelectedTrack`, `refreshBrowseCard`, `refreshPreviewButton`, `togglePreview`, `stopPreview`,
  `scheduleMatchRefresh`, `refreshMatchReference`, `showLoadRefused`, `showLoadUnavailable`, `matchReferenceDeck`,
  `browseSegments`; struct `DeckLabBrowseState` (defined just before the constructor) and an anonymous-namespace block
  before `buildBrowse`.
- Touched outside the list, two lines each: `~WDeckLab` (disconnect the browse list and sidebar selection model before
  children are destroyed) and the header (`library()` accessor, browse declarations, `m_browseState`).
- Not touched: Decks page, Settings, FX/Sampler, `tool()`, helper signatures, `tickEffects`, `selectPage`.

## Tests
`NativeDeckLabTest`: GenericControllerBrowseOpensBrowseWithoutTouchingDecks, PlayingDeckLoadShowsWhyAndKeepsTheDeck,
CollectionOpensCategoriesAndBackReturnsToSources, BrowseScalesCleanlyTo1024x600And1280x800.
`DeckLabBrowseMatchTest`: LancelotWheelHarmonicMatchTable (all 24 keys, wrap-around, Mixxx Lancelot notation agrees,
subset of `getCompatibleKeys`), TempoMatchesWithinSixPercentOnly.
`DeckLabBrowseTest`: BreadcrumbDropsMiddleSegmentsBeforeTheDeepestLevel, CategoryValuesComeFromTheTrackModelWithNativeQueries.
`SearchQueryParserTest.ExternalTablesFilterOnlyTheirOwnColumns`.
The fixture adds a generated "Collection" root (`tracks` icon) next to the generated "USB library".

## CI
| Run | Result | Notes |
|---|---|---|
| 39801050487 (2 attempts) | failed before build | 1st: runner never started; 2nd: shallow clone missed the pinned upstream revision |
| 39801594624 | compile error | `~WDeckLab` used the browse state before its definition |
| 39805583696 | 263/264 tests | Back from categories lost the highlighted source (sidebar clears it on focus-in) |
| 39807898264 | 263/264 tests | same failure; the polish in this run (sidebar rows, Match toggle) passed |
| 39809298668 | **passed** | 268 tests, smoke 21 screens + 8 Browse captures, no failures |
| 39810626664 | **passed** | capture polish, fixture reference deck with tempo |
| 39812416062 | **passed** | on-device type scale and brand colours |
| 39813563633 | **passed** | card numerals fit, shorter match line |
| 39814887683 | **passed** (final) | 268 tests, smoke 21 screens + 8 Browse captures, 0 failures; Browse buttons and keyboard hold 44/52 px under the surface style sheet; smoke fails on smaller Browse targets |

The CI fetch-depth fix is the same change as `decklab/perf-engine` commit `375429e8b1`. New unit suites
`DeckLabBrowseMatchTest.*` and `DeckLabBrowseTest.*` were added to `test_suites` in `pi-build-lock.json`. Re-run
`update_lock`-style hashing after merging: `CMakeLists.txt`, `wdecklab.*`, `wtracktableview*`, `librarycontrol.cpp`,
`decklabsmoke.cpp`, `decklabtest.cpp`, `decklabbrowse.*` and the workflow are all locked inputs.

## Screenshots
All from CI artifacts (`gh run download <id>`), generated data only.
- Real application smoke (800x480, generated demo library: silent WAVs, drawn covers, "Demo Artist A–F"):
  `app-browse-01-sources.png` (unified source rows, details card), `-02-categories.png`, `-03-artists.png`
  (values with counts), `-04-artist-tracks.png` (filtered tracks, cover art, chips, rating/colour/album/comment),
  `-05-keys.png` (key values with wheel colours), `-06-reference-deck.png` (Decks after loading the paused reference),
  `-07-match.png` (same/relative/±1 key pills, ±6% BPM, Match `D1 · 8A · 122`, LOAD 1 `Paused`),
  `-08-four-decks.png` (four LOAD targets).
- Native test fixture: `browse-categories-fixture.png`, `browse-on-air-feedback.png` (toast + red ON AIR target),
  `browse-800x480.png`, `browse-1024x600.png`, `browse-1280x800.png`, `screen-1.png`.

## Gaps and follow-ups
- **Fonts are not bundled.** Linux/Flatpak builds do not install `res/fonts`, and Barlow is not in the runtime, so the
  CI captures show the fallback face (wider). Bundling Barlow/Barlow Semi Condensed (OFL, `app-brand/brand/fonts/`) needs
  an install rule and registration owned by whoever ships the brand assets; the Browse code already asks for those families.
- **rekordbox USB categories are untested against a real export** in CI (no generated PDB fixture). The import/polling
  path, `artist:`/`key:` on the external table (parser test) and Playlists via the device subtree are covered by code and
  unit tests only. Mixxx does not import rekordbox history, so USB exports show no History category.
- **No on-Pi measurement** (no deploys). Expected wins: pixel scrolling with fixed rows and no hover repaints, one
  selection change per encoder tick, painted card/breadcrumb (no relayout on selection or tick), match repaint only when
  the reference changes. Category values are collected synchronously when a category opens (O(rows), estimated tens of
  ms per 10k tracks on a Pi 4); measure on a large library.
- **Smoke fixture:** silent WAVs have no beat grid until loaded, so the smoke sets the paused reference deck's tempo after
  loading. Production tracks are analysed normally.
- **Not mine, seen in captures:** the four-deck strip under Browse truncates titles (Decks `updateLayers`); Settings still names
  the Lancelot notation with the trademarked wheel name (`buildSettings`, and `decklabtest.cpp` ~line 689 looks for it); Decks
  still shows "RMX" strings. These belong to decks-ui / system-setup or the naming pass.
- Column dragging on touch is enabled but untested on the panel; Tools > Choose columns… and Default columns are the
  touch path. Typing a search replaces a category filter (breadcrumb follows). `LoadSelectedIntoFirstStopped` stays
  silent in native mode. Preview stops whenever Browse is left.
- `pi-build-lock.json`, `decklabsmoke.cpp`, `wdecklab.h` and `~WDeckLab` will conflict textually with other workstreams;
  the browse hunks are self-contained.
