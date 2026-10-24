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
| Track card KEY chip | formatted once at `showTrack()`; "—" for a key Mixxx could not read | cached `m_keyText`; `Track::getKeyText()` is empty without a key id |
| "Relative 8A→8B" (card) | always the wheel names | hard-coded `lancelotText()` |
| Browse › Key values | global notation, wheel-number order always | labels from `KeyUtils`, sort by Lancelot number |
| Sort by key, rekordbox | broken (SQL `CASE key_id` on a table without `key_id`) | `ColumnCache`'s key sort assumes `key_id`. The `key:` and Match filters were not broken: `SearchQueryParser` already falls back to matching the key text when a table has no `key_id` |
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
  `relabelKeyValues()`), an open key value's breadcrumb, the card; a table sorted by key sorts again
  (also a USB folder list, whose sort Qt would skip for the same column and order).
- **Stored text is never lost**: a track Mixxx has no key for shows the key its row stores (a rekordbox
  export's "8A", a USB file's tag) on the card and in Mixes; text that is not a key shows as stored.
- **rekordbox**: `rekordbox_library.key_id` is filled at import from the export's text (worker thread,
  `parseKeyText()`, 0 when unsure); internal column. Rows then format and sort (`CASE key_id`) as keys, and
  `key:` / Match filters select by key id instead of matching text. The table is dropped and recreated at
  every start, so no migration. The import writes its rows through one shared INSERT and binding
  (`insertLibraryTrackRow()`), which the tests use too.
- **USB folders**: `ProxyTrackModel::lessThan()` sorts the key column round the wheel of
  `[Library],key_notation` (unreadable tags first, as in the library; Lancelot order only for the
  Lancelot notations, as `ColumnCache`); a notation change re-sorts even with the same column and order.
- **Settings**: the row moves to the top of **Library** (where the owner looked): "Key format", options
  "Lancelot (8A)", "Open Key (1m)", "Musical (Am)" (all A minor), hint "How keys read in Browse, on the
  decks and in Match". It shows the full name (the old segments printed "8M" and "AM" in capitals) and
  starts on the format on screen. The category reads "Keys · Lancelot"; Display now reads its waveform
  layout. Default stays Lancelot and is applied at start-up.
- **Config**: `[Key] KeyNotation` keeps Mixxx's values (Lancelot / OpenKey / Traditional). The desktop's
  combined values read as the closest format ("OpenKey/Traditional" is Open Key, "Lancelot/Traditional"
  Lancelot); missing, Custom and junk read as Lancelot. At start-up a value Deck Lab does not show is written
  as the one it shows, so the config always says what is on screen.
- **Desktop preferences**: a notation chosen there is followed (watch on `[Library],key_notation`,
  acting on the notation at that moment, never a queued old value; Custom ignored). `DlgPrefKey` now shows
  the saved notation each time it opens (`slotShow()`), so its OK or Apply never brings back the notation it
  loaded at start-up.

## Review fixes (second pass)

1. USB folder re-sort on a format change did nothing (Qt skips a sort with the same column and order,
   `dynamicSortFilter`): `ProxyTrackModel::sort()` keeps the wheel per model and calls `invalidate()` when
   only the wheel changed. Test: same column and order after a notation change.
2. The `[Library],key_notation` watch (queued) acted on the value captured at send time; two quick changes
   could swap formats forever. It now reads the control's current value. Test: two changes in one pass
   (Settings row, desktop side), no further notation changes over six event passes.
3. OK in the desktop preferences reset Deck Lab's format (radios loaded once; Traditional when the value
   was missing): the config is written at start-up, and `DlgPrefKey::slotShow()` re-reads it. Test:
   start-up values (missing, "OpenKey/Traditional", "Traditional", "Custom", "Lancelot/Traditional").
   `DlgPrefKey` itself cannot be built in the test target (its generated UI header is private to
   `mixxx-lib`); covered by the build.
4. `keyFormatFromConfig()` compared against "OpenKeyAndTraditional", a string Mixxx never writes: it now uses
   `KeyUtils::keyNotationFromString()`; the Settings row starts from `decklab::keyFormat()`. Tests use the
   real values.
5. The card and Mixes pills showed "—" for stored text that is not a key: both fall back to the row's
   stored text. Tests: card (`TrackCardShowsTheRowsKeyWhenTheTrackHasNone`), Mixes with a keyless reference.
6. Coverage: the `NativeDeckLabTest` fixture now has `[Library],key_notation` (Library makes it before the
   skin in the app); a live `WTrackTableView` test; a rekordbox table test.
7. These notes: relative capture path, corrected rekordbox root cause, track-table captures.

## Renders (800x480)

`local/wt/audit/screens/r6-keys/` (outside the repo):
- **Track table** (the reported surface: Mixxx's `WTrackTableView` with Deck Lab's cells over rekordbox /
  USB-folder style key text "Am", "F#m", "1d", "Ebm", "Unknown", "12B", "4A", sorted by KEY):
  `before-table-lancelot.png`, `before-table-musical.png` (the deployed code with the format set to
  Lancelot, then Musical: the KEY column shows the stored text and sorts it as text);
  `after-table-lancelot.png` (Unknown 2A 4A 8A 8B 11A 12B, round the wheel),
  `after-table-musical.png` (Unknown C Am F♯m E E♭m Fm, Open Key order).
- **History rows** (`KeyFormatSettingReachesEveryKeyLive`): `before-browse-lancelot.png`,
  `before-browse-musical.png` (deployed code, Lancelot applied first as a unit whose config says Lancelot:
  "Am", "F#m", "1m", "Ebm", "known" stay as stored); `after-browse-lancelot.png` (8A 11A 8A 2A Unk… 8B),
  `after-browse-musical.png` (Am F♯m Am E♭m Unk… C), `after-browse-openkey.png` (1m 4m 1m 7m Unk… 1d).
- `before-/after-settings-library.png`, `before-/after-settings-display.png`.

The before captures come from capture-only tests on a local throwaway branch (`decklab/r6-keys-before`,
never pushed, deleted after the captures).

## Verified

Local WSL build (`local-build/decklab-local.sh`, Qt 6.4, x86_64, own slot `decklab-build-keys`):
- New and changed tests: parse + format helper (all three input forms, minors/majors, enharmonics, combined
  forms, junk shown as stored, 24 distinct names per format, wheel orders equal to Mixxx's sort, Mixxx's
  real config values); Browse's real KEY cell painting rekordbox-style "Am"/"8A", tag "F#m"/"1m", a key id
  and junk (text recorded from the paint engine, `src/test/decklabpaintedtext.h`); Key values relabel and
  reorder; USB folder sort round the wheel, including a notation change with the same column and order;
  the rekordbox table through the import's own INSERT (key_id per row, the row text in Lancelot and
  Musical, `CASE key_id` sort in Lancelot and Open Key order, `key:8A`, `key:Am` and the Match filter);
  the card's stored-key fallback; `NativeDeckLabTest`: a live Browse track table (painted keys, repaint,
  wheel order after a Settings change and a desktop change, repaint when not sorted by key), Mixes rows
  with stored text, History rows, decks and Match following, quick changes, start-up config values, the
  row in Library.
- Review-fix runs: a filtered run (key, Mixes, History, Settings, Match bar, KeyUtils and search parser
  suites, 111 tests) found one failure, a fixture artifact (generated rows had empty paths, so the
  reference deck's path matched one and Mixes skipped it); with a generated path per row, the key suites
  (`DeckLabBrowseMatchTest.*`, `DeckLabBrowseTest.*`, `NativeDeckLabTest.Key*`, `NativeDeckLabTest.Mixes*`,
  37 tests) all pass, with no build warnings in the changed files.
- Before captures: the capture-only tests on the throwaway branch pass (2 tests).
- `python tools/decklab/build_pi.py --check-inputs` PASS; `test_build_pi.py` 11 tests OK.
- The full run (no flags): see the final report of this branch (summary path and result).

## Needs CI and the Pi

- On the Pi: a rekordbox USB in Browse (Classic and Alphanumeric exports), a USB folder, History; switch
  the format on Settings › Library and back; sort by KEY on each source; the Match "Only" filter on a
  rekordbox source; Settings › Advanced (desktop) OK afterwards keeps the format.
- CI's aarch64 build and the full locked suites (nothing here is architecture-specific).

## Merge notes

- New files: `src/decklab/decklabkeys.{h,cpp}` (CMake: after `decklabusbname.cpp`),
  `src/test/decklabkeystest.cpp` (CMake test list, after `decklablanestest.cpp`),
  `src/test/decklabpaintedtext.h` and `src/test/decklabgeneratedfolder.h` (header only).
- `wdecklab.cpp` hooks (`// r6-keys:`): include; constructor (apply the saved format, write the config
  when it differs, watch `[Library],key_notation` reading the current value); `refreshBrowseCard()` (Mixes
  card key; a track level card gets the row's stored key text); `refreshMatchReference()` Match key;
  `refreshDeck()` key; `setKeyNotation()` rewritten + new `refreshKeyText()` after it; `updateMatchBar()`
  chip key; `updateMixesRow()` key; `fillMixes()` key (stored text for a key that is not one);
  `refreshHistoryCard()` key fact and the card's stored key text. `wdecklab.h`: `refreshKeyText()`
  declaration after `setKeyNotation()`.
- Other edits: `decklabbrowse.{h,cpp}` (keyFromText → parseKey, keyRelationText, Key values sort/labels,
  `relabelKeyValues()`, card `keyText()`, `showTrack(track, storedKeyText)`), `decklabhistoryview.{h,cpp}`,
  `decklabbrowsemix.cpp`, `decklabdecktools.cpp`, `decklabsettings.{h,cpp}` (row moved Display → Library),
  `wtracktableview.cpp` (key cell, `keyNotationChanged()` repaints the viewport, `makeKeyCellDelegate()`
  for tests, `selectionChanged()` tolerates no Library for tests), `proxytrackmodel.{h,cpp}` (`lessThan()`,
  per-model wheel, re-sort), `rekordboxfeature.{h,cpp}` (`key_id`, shared INSERT/binding,
  `libraryColumns()` and two `...ForTesting()` hooks), `dlgprefkey.{h,cpp}` (`slotShow()`),
  `decklabtest.cpp` (fixture: `[Library],key_notation` control, `installGeneratedTable()`,
  `openTrackLevel()`, `paintedKeys()`; 4 tests appended; includes).
- Lock: refreshed `input_sha256` for CMakeLists.txt, rekordboxfeature.{cpp,h}, decklabtest.cpp,
  decklabbrowse.{cpp,h}, wdecklab.{cpp,h}, wtracktableview.cpp (CRLF→LF digest). New tests use the
  locked suites `DeckLabBrowseMatchTest`, `DeckLabBrowseTest`, `NativeDeckLabTest`: no suite change.
- Smoke: no new captures (+0 screens).

## Follow-ups

- USB folder search (`ProxyTrackModel::filterAcceptsRow`) matches typed words against the tag text, so
  typing "8A" does not find a file tagged "Am", and the Match "Only" filter (`key:` terms) does not apply to
  folder lists. Pre-existing; a key-aware folder filter would fix both.
- Traktor/Serato/iTunes libraries have the same missing `key_id`; their rows already display correctly
  through the Deck Lab key cell, but sort and `key:` filters would need the same import change.
- Pressing OK in the desktop preferences with Musical chosen sets Mixxx's two-name Traditional names in
  `KeyUtils` again (the notation value does not change, so Deck Lab is not told). Every Deck Lab surface
  formats through `decklab::displayKey()`, which reads them, so nothing on screen changes.
