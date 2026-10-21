# Deck Lab native Mixxx fork

This fork maintains two narrow native repairs on upstream commit
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

The Deck Lab interface, controller overlays, custom LV2 effects and appliance
setup remain in the separate Deck Lab repository. This public fork contains
native Mixxx source/build tooling, not personal profiles, music, libraries or
appliance access credentials. Original upstream licenses/notices remain intact.

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
memory-conscious packaging settings used for the tested repair. It builds
`mixxx-test`, runs the engine/preparation/sync/mixer and beat/search regression
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
deployment. The current tested appliance remains on its selected `rt1`
executable until a later supervised acceptance/promotion decision.
