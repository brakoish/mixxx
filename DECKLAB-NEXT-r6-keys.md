# Deck Lab r6-keys: one Key format, for every key on screen

Branch `decklab/r6-keys`, based on `decklab/round5` (4afab9fff9, the deployed build). Owner report: "library
looks like it's not in [the wheel notation] but that's what shows on decks. can this be a setting". The
setting existed (Settings › Display › Key format) but only reached the decks. Only generated key texts and
tracks are used in tests and captures.

## Why some keys ignored the setting

| Where | What it showed | Why |
|---|---|---|
| Browse rows, rekordbox USB | the export's text ("Am", "F#m", or "8A") | `rekordbox_library` has no `key_id`; Mixxx's `keyFromKeyTextAndIdFields()` passes the text through |
| Browse rows, USB folders | the file tag as written | Mixxx's file browser (`BrowseTableModel`) keeps the tag text only |
| Browse rows, library tracks without a key id | the stored text | same pass-through |
| History rows | key id formatted once, else the stored text | `entryKeyText()` built at list fill; no reformat on change |
| Mixes rows, Mixes row subtitle, Match button and chip | formatted once | text built ahead, never rebuilt on a change |
| Track card KEY chip | formatted once at `showTrack()` | cached `m_keyText` |
| "Relative 8A→8B" (card) | always the wheel names | hard-coded `lancelotText()` |
| Browse › Key values | global notation, wheel-number order always | labels from `KeyUtils`, sort by Lancelot number |
| Sort by key, rekordbox | broken (SQL `CASE key_id` on a table without `key_id`) | `ColumnCache` key sort assumes `key_id` |
| Sort by key, USB folders | alphabetical ("10A" before "1A") | `QSortFilterProxyModel` string compare |
| Musical format | "F♯/G♭", "D♯m/E♭m" | Mixxx's Traditional names |
| Start-up | whatever `DlgPrefKey` loaded (its default is Traditional) | Deck Lab only applied its format on a change |

## What changed

- **`src/decklab/decklabkeys.{h,cpp}` (new)**: the one helper. `parseKeyText()` reads Lancelot (1A-12B,
  any case, leading zeros), Open Key (1m-12d) and musical names (sharps/flats in ASCII or signs, m/min/minor,
  maj/major, lower case for minor as Mixxx reads it), and combined forms whose parts agree ("8A (Am)",
  "F♯/G♭"). Anything else is INVALID: digit forms with other letters ("8D", "8M") are not guessed.
  `displayKey(id, text)`: a valid key id first, else the parsed text, else the text exactly as stored
  (never blank, never another key). `formatKey()` returns cached names (3 × 24, built once); `parseKey()`
  caches each distinct text (bounded hash), so painting a row is one lookup. `keyWheelOrder()` is Mixxx's
  library sort order (Lancelot order for Lancelot, Open Key order otherwise). `applyKeyFormat()` also sets
  `KeyUtils`' names and `[Library],key_notation`, so Mixxx's own tables, sort and filters agree.
  Musical names are single: F♯ and E♭m (no slashed pairs).
- **Paint-time formatting** (a format change is a repaint): Browse key cells
  (`wtracktableview.cpp` `DeckLabKeyDelegate`), History rows, Mixes pills, the track card's KEY chip.
  Long stored text is elided ("Unknown" read "known" before).
- **Rebuilt on a change** (`WDeckLab::refreshKeyText()`): deck readouts, key sheet texts, Match button and
  chip, Mixes row subtitle, Browse › Key values (relabelled and reordered in place, highlight kept,
  `relabelKeyValues()`), an open key value's breadcrumb, the card; a table sorted by key sorts again.
- **rekordbox**: `rekordbox_library.key_id` is filled at import from the export's text (worker thread,
  `parseKeyText()`, 0 when unsure); internal column. Rows then format, sort (`CASE key_id`) and filter
  (`key:8A`, the Match filter) as keys. The table is dropped and recreated at every start, so no migration.
- **USB folders**: `ProxyTrackModel::lessThan()` sorts the key column round the wheel of
  `[Library],key_notation` (unreadable tags first, as in the library); other columns as before.
- **Settings**: the row moves to the top of **Library** (where the owner looked): "Key format", options
  "Lancelot (8A)", "Open Key (1m)", "Musical (Am)" (all A minor), hint "How keys read in Browse, on the
  decks and in Match". It shows the full name (the old segments printed "8M" and "AM" in capitals). The
  category reads "Keys · Lancelot"; Display now reads its waveform layout. `[Key] KeyNotation` keeps
  Mixxx's values (Lancelot / OpenKey / Traditional); missing or other values read as Lancelot (Open Key for
  OpenKeyAndTraditional). Default stays Lancelot and is applied at start-up. A notation chosen in the
  desktop preferences is followed (watch on `[Library],key_notation`, Custom ignored).

## Renders (800x480, `NativeDeckLabTest.KeyFormatSettingReachesEveryKeyLive`)

`C:\Users\Me\Desktop\Mixx Pi\local\wt\audit\screens\r6-keys\` (outside the repo):
- `before-browse-lancelot.png`, `before-browse-musical.png`: History rows keyed "Am", "F#m", "1m", "Ebm",
  "known", "C" in both formats; Match "1 · Am" (the deployed code, capture-only test on a throwaway branch).
- `after-browse-lancelot.png` (8A 11A 8A 2A Unk… 8B, Match "1 · 8A"), `after-browse-musical.png`
  (Am F♯m Am E♭m Unk… C), `after-browse-openkey.png` (1m 4m 1m 7m Unk… 1d).
- `before-/after-settings-library.png`, `before-/after-settings-display.png`.

## Verified

Local WSL build (`local-build/decklab-local.sh`, Qt 6.4, x86_64, own slot `decklab-build-keys`):
- Filtered run on 860d7d5cde, 113 tests, all pass, no new build warnings: `DeckLabBrowseMatchTest.*`,
  `DeckLabBrowseTest.*`, `NativeDeckLabTest.Key*`, `.DeckIdentity*`, `.History*`, `.Mixes*`, `.MatchBar*`,
  `.Settings*`, `DeckLabHistoryDbTest.*`, `DeckLabKeyReadoutTest.*`, `DeckLabKeySheetTest.*`,
  `KeyUtilsTest.*`, `SearchQueryParserTest.*`.
- New tests: parse + format helper (all three input forms, minors/majors, enharmonics, combined forms,
  junk shown as stored, 24 distinct names per format, wheel orders equal to Mixxx's sort, config values);
  Browse's real KEY cell delegate painting rekordbox-style "Am"/"8A", tag "F#m"/"1m", a key id and junk
  (text recorded from the paint engine, `src/test/decklabpaintedtext.h`); Key values relabel/reorder;
  USB folder sort round the wheel (Lancelot and Open Key, both directions); `NativeDeckLabTest`: History
  rows with stored rekordbox/tag texts read 8A/11A/8A/2A in Lancelot and Am/F♯m/Am/E♭m in Musical, the
  list repaints on the switch, decks and Match follow, config writes Mixxx's values, the row sits in
  Library; desktop-preference changes followed.
- `python tools/decklab/build_pi.py --check-inputs` PASS; `test_build_pi.py` 11 tests OK.
- The full run (no flags): see the final report of this branch (summary path and result).
- The rekordbox sort/filter failure without `key_id` is from reading the code (`ColumnCache` key sort and
  `KeyFilterNode` both use `key_id`; the SQL error leaves `BaseSqlTableModel::select()` no rows); it was not
  reproduced against a real export here.

## Needs CI and the Pi

- On the Pi: a rekordbox USB in Browse (Classic and Alphanumeric exports), a USB folder, History; switch
  the format on Settings › Library and back; sort by KEY on each source.
- CI's aarch64 build and the full locked suites (nothing here is architecture-specific).

## Merge notes

- New files: `src/decklab/decklabkeys.{h,cpp}` (CMake: after `decklabusbname.cpp`),
  `src/test/decklabkeystest.cpp` (CMake test list, after `decklablanestest.cpp`),
  `src/test/decklabpaintedtext.h` (header only: records the text a delegate paints).
- `wdecklab.cpp` hooks (`// r6-keys:`): include; constructor (apply the saved format, watch
  `[Library],key_notation`); `refreshBrowseCard()` Mixes card key; `refreshMatchReference()` Match key;
  `refreshDeck()` key; `setKeyNotation()` rewritten + new `refreshKeyText()` after it; `updateMatchBar()`
  chip key; `updateMixesRow()` key; `fillMixes()` key; `refreshHistoryCard()` key fact.
  `wdecklab.h`: `refreshKeyText()` declaration after `setKeyNotation()`.
- Other edits: `decklabbrowse.{h,cpp}` (keyFromText → parseKey, keyRelationText, Key values sort/labels,
  `relabelKeyValues()`, card `keyText()`), `decklabhistoryview.{h,cpp}`, `decklabbrowsemix.cpp`,
  `decklabdecktools.cpp`, `decklabsettings.{h,cpp}` (row moved Display → Library), `wtracktableview.cpp`
  (key cell, `keyNotationChanged()` repaints the viewport, `makeKeyCellDelegate()` for tests),
  `proxytrackmodel.{h,cpp}`, `rekordboxfeature.cpp` (`key_id`), `decklabtest.cpp` (2 tests appended,
  3 includes).
- Lock: refreshed `input_sha256` for CMakeLists.txt, rekordboxfeature.cpp, decklabtest.cpp,
  decklabbrowse.{cpp,h}, wdecklab.{cpp,h}, wtracktableview.cpp (CRLF→LF digest). New tests use the
  locked suites `DeckLabBrowseMatchTest`, `DeckLabBrowseTest`, `NativeDeckLabTest`: no suite change.
- Smoke: no new captures (+0 screens).

## Follow-ups

- USB folder search (`ProxyTrackModel::filterAcceptsRow`) matches typed words against the tag text, so
  typing "8A" does not find a file tagged "Am", and the Match "Only" filter (`key:` terms) does not apply to
  folder lists. Pre-existing; a key-aware folder filter would fix both.
- Traktor/Serato/iTunes libraries have the same missing `key_id`; their rows already display correctly
  through the Deck Lab key cell, but sort and `key:` filters would need the same import change.
