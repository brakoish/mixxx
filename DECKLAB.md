# Deck Lab native Mixxx fork

The tested integration maintains two native repairs on upstream commit
`9dedd4a80af435a10122eb38ba7dc34ad48773d6`:

1. **Rekordbox worker ownership:** snapshot the device label/path and build a
   detached worker-owned playlist tree. Apply completed children on the GUI
   thread using a persistent device index. Removed devices and failed imports
   remain safe/retryable.
2. **Audio startup preparation:** prepare deck scalers while sound devices are
   closed, before opening outputs. This moves initial Rubber Band filter/FFT/
   window construction off the first real-time audio callback. Transport and
   normal per-callback signal checks remain intact.

The integration branch is `codex/decklab-pi`. Independent repair branches are
`codex/rekordbox-worker-snapshot` and `codex/audio-startup-preparation`; each starts
from the pinned upstream revision. The integration branch contains both fixes
as separate commits plus build tooling. Upstream `main` is retained separately.
No upstream pull request or external message is sent automatically.

The `codex/native-appliance` candidate additionally owns the native Deck Lab
surface (`res/skins/DeckLab`, `src/widget/wdecklab.*`) and the owned three-band
LV2 processor. Browsing uses Mixxx's actual library models and controls, without
desktop focus polling, accessibility scraping or an IPC server. Two dedicated
`[DeckLabEffectUnit1/2]` chains are independent of all four controller chains.
The processor fails back to dry if its native heartbeat disappears. The host
companion retains focused OS/media tools; their explicit control handshake is
versioned alongside the controller overlays in the separate Deck Lab repo.
No personal profiles, music, libraries or appliance credentials belong here.
Original upstream licenses/notices remain intact. Waveform skin markup is
adapted from Pioneered by Sven Boekelder (GPL-3.0, as is this adapted skin); the new UI is native
Qt Widgets, not a renamed copy of the desktop skin.

The native candidate is not selected or installed automatically on the Pi.
The tested rt1 installation remains the normal boot target. Scheduled analysis
is deliberately excluded. CI checks generated audio signals and offscreen
800×480 widget geometry; these do not certify touch, heard audio or long-set
performance on the physical Pi.

Native browsing uses a small set of readable columns and an internal touch
keyboard. Original desktop header state is retained. All sixteen sampler pads
remain visible; the RMX's A–D sample triggers/rolls use the first four slots.
This keeps optional sample-FX routing bounded rather than creating sixteen
simultaneous copies of the heavy processor. The selected LV2 rate is prepared
with devices closed, including deck-state allocation and delay-page warmup.

The optional host tool handshake uses numbered requests with serial-matched
acknowledgements and a matching-profile heartbeat. A mismatched QA profile does
not enable these tools. Advanced preferences is a native signal to the main
window and does not require the companion. A fresh appliance without outputs
keeps its UI available with a persistent Audio disconnected message.

Candidate CI also launches the real packaged application in a dedicated
`decklab-native-smoke*` profile, safe mode, without audio/display/network access.
`--developer` plus `DECKLAB_SMOKE_DIR` opt into the screenshot check. Seventeen
page/subview captures and JSON/logs are retained; unexpected playback, missing
captures or clipped controls fail the gate. Ordinary launches ignore this check.
Compiler cache reuse accelerates edits but does not bypass source locks, native
tests, DSP checks or the runtime gate.

## Build an ARM64 candidate

Prefer **Actions → Deck Lab Pi ARM64 → Run workflow** on the integration branch.
It uses the standard public `ubuntu-24.04-arm` runner. Other inherited upstream
workflows are disabled in this fork. The workflow has read-only repository
permission, pinned action revisions and no deployment credentials or Pi access.
Pushes to the integration branch and `decklab-pi-*` tags also build candidates.

From a development computer, dispatch the same workflow without Pi access:

```sh
gh workflow run decklab-pi.yml --repo brakoish/mixxx --ref codex/decklab-pi
```

For a separate ARM64 Linux build machine, install `git`, Python 3, `flatpak`,
`flatpak-builder`, D-Bus and `elfutils` using its normal package manager. Commit
source changes, then run from this checkout:

```sh
python3 tools/decklab/build_pi.py --check-inputs
python3 tools/decklab/test_build_pi.py
dbus-run-session -- python3 tools/decklab/build_pi.py \
  --work-dir "$HOME/decklab-pi-build" --jobs 3
```

Allow at least 20 GB free disk space. Three compiler jobs are the default; the
hosted runner uses four. A full first build is substantial. Builder caches are
retained in the work directory for later local runs. Use a directory outside the
checkout to avoid recursive source copying.

The script creates an **isolated Flatpak user installation inside the work
directory**, fetches trusted Flathub runtimes, and requires the exact ARM64 KDE
6.10 SDK/runtime commits in `tools/decklab/pi-build-lock.json`. It does not update
the build host's ordinary Flatpak installation. Every external dependency is
locked by SHA-256 or Git commit; the native source and dependency manifests are
hash-checked before building. Input text hashes normalize Windows CRLF to LF.
Dirty tracked source is rejected and the source module uses the exact committed
Git revision, excluding untracked files.

The build retains optimization and uses `-g1` for Mixxx/protobuf, matching the
memory-conscious packaging settings used for the tested repair. An explicit
`CMAKE_INSTALL_LIBDIR=lib` for every CMake module keeps
dependencies in the Flatpak runtime's `/app/lib` search directory on ARM64.
The first hosted build passed 197 enabled native tests but its final runtime
probe rejected the upstream manifests' automatic `lib64` layout. The corrected
recipe and a build guard prevent publishing that incomplete package as verified.
The recipe builds `mixxx-test`, runs the engine/preparation/sync/mixer and beat/search regression
suites offscreen with audio devices, display sockets and networking denied, and
requires the preparation regression to have actually run. Disabled upstream
tests are reported separately. It then probes the packaged executable against
the pinned runtime using `--version`, again without audio devices or playback.

## Outputs and acceptance

Successful runs provide a short-lived workflow artifact containing:

- `decklab-mixxx-aarch64-<source-revision>.flatpak`, branch `decklab`;
- `mixxx.bin`, the AArch64 executable from the packaged application;
- native test XML/log, runtime version probe and `SHA256SUMS`;
- `provenance.json`, resolved manifest and the build lock, including source,
  SDK/runtime, dependency and executable identities and test counts.

The build recipe fixes source and SDK/runtime inputs. **Byte-for-byte binary
reproducibility is not claimed.** Git version metadata, packaging tools and
builder image versions can affect outputs; those identities and checksums are
recorded. Remote input unavailability fails the build instead of silently using
new versions. Updating a lock is a deliberate, reviewed change.

Artifacts are **candidates**. CI does not certify physical Pi timing, touch,
controller behavior, heard main/headphone sound or long-set stability. There is
no SSH, installer, automatic deployment or promotion step. Downloading/building
a candidate does not change the currently tested Pi executable.

The bundle uses the upstream application ID. A different Flatpak branch is not
a separate personal profile: use an explicitly separate `--settings-path` for
future QA and keep production databases closed/backed up before any deliberate
deployment. The original tested appliance remains recoverable on its selected
`rt1` executable. Deliberate native Pi trials and their recovery selections are
recorded in the companion appliance repository's `versions.json`; this build
never selects one automatically.

## Native Deck Lab performance surface

The `codex/native-appliance` branch adds a native 800×480 controller-first UI.
Its two-channel view stacks two real waveform viewers with a shared center and
linked zoom above two deck-information cards. Actual cue colors, A–H access,
read-only whole-track overviews, loop controls and explicit transport/Sync states
remain connected to the existing engine. A four-channel mapping or manual choice
selects four direct cards; upper software layers alone do not imply four physical
mixer channels. The helper sends controller capability independently of software
deck count. Unknown hardware retains a manual layout choice.

Touch hot cues jump on release while preserving playback state. An empty pad can
set a cue while paused; it is disabled while playing. This does not replace the
controller mapping's held-preview behavior. Gesture guards capture the actual
deck, loaded track and interaction epoch, rejecting stale releases after a page,
layout, layer, track or touch-context change. Musical position is grid-relative
BEAT, not an inferred bar or phrase.

SYNC works like a Pioneer CDJ-3000's BEAT SYNC. A tap turns Sync Lock on or off
and the lamp stays lit while it is on; the deck follows the tempo master's tempo
and beat, and keeps its tempo when sync goes off. One deck is the tempo master
(Mixxx's sync leader): the deck on air when sync first goes on, or the deck MORE's
MASTER picks while it plays; pausing the master hands it on. A synced deck that
is not the master ignores its tempo fader; after sync is off, the controller's
soft takeover picks the fader up when it passes the tempo. Holding SYNC (or MORE's
INST DOUBLES) loads the master's track, else a playing deck's, at the same
position, only onto a stopped deck. The engine half lives behind
`[DeckLab],cdj_sync` (`src/engine/sync/decklabcdjsync.h`), which the native
surface turns on, so every controller mapping that sends `beatsync` gets the
same SYNC button; without it Mixxx's own sync behaviour applies.

Deck Lab opts into `<HotcueLabelFormat>letters</HotcueLabelFormat>` for waveform
and overview cue identifiers. Other skins keep numeric labels. Stored cue names
are not changed. Its optional preroll color and overview normalization are visual
only and do not affect audio gain or other skins' defaults.
