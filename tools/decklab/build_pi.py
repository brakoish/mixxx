#!/usr/bin/env python3
"""Build/test an ARM64 candidate in an isolated Flatpak installation. Never deploy."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time
from urllib.parse import urlparse
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
LOCK = Path(__file__).with_name("pi-build-lock.json")
MANIFEST = ROOT / "packaging/flatpak/org.mixxx.Mixxx.yaml"
# Experimental gpu-screen spike (DECKLAB_QML=1): a QML-enabled candidate on its
# own Flatpak branch, so it installs beside the product and never replaces it.
QML_BRANCH = "decklab-qml"
QML_MAIN = "qml/DeckLab/main.qml"


def qml_requested(environ=None):
    """DECKLAB_QML=1 builds the QML candidate. Unset, empty or 0 is the product build."""
    value = (os.environ if environ is None else environ).get("DECKLAB_QML", "")
    if value not in ("", "0", "1"):
        raise ValueError("DECKLAB_QML must be 0 or 1")
    return value == "1"


def configure_qml(module):
    """Enable Mixxx's QML UI. No test binary: the product job runs the gates."""
    options = [option for option in module.get("config-opts", [])
               if not option.startswith(("-DQML=", "-DBUILD_TESTING="))]
    module["config-opts"] = options + ["-DQML=ON", "-DBUILD_TESTING=OFF"]


def gl_extension_ref(metadata):
    """The GL driver extension the runtime expects, e.g. org.freedesktop.Platform.GL.default//25.08."""
    section = False
    for line in metadata.splitlines():
        line = line.strip()
        if line.startswith("["):
            section = line == "[Extension org.freedesktop.Platform.GL]"
        elif section and line.startswith("versions="):
            version = line.split("=", 1)[1].split(";")[0].strip()
            if version:
                return "org.freedesktop.Platform.GL.default//" + version
    raise ValueError("Runtime metadata names no GL extension version")


def archive_sources(manifest):
    """(url, sha256, flatpak-builder download path) for every pinned archive, nested modules included."""
    def visit(modules):
        for module in modules:
            for source in module.get("sources", []):
                if source.get("type") == "archive" and source.get("url") and len(source.get("sha256", "")) == 64:
                    name = Path(urlparse(source["url"]).path).name
                    yield source["url"], source["sha256"], Path(source["sha256"]) / name
            yield from visit(module.get("modules", []))
    return list(visit(manifest["modules"]))


def prefetch_archives(manifest, downloads, env):
    """QML job only: fill flatpak-builder's download cache over IPv4 with retries.

    Runs 39856968708 (both attempts) timed out reaching one archive host from the
    runner while it answered elsewhere. Best effort: a wrong file is discarded here
    and flatpak-builder still verifies every checksum itself."""
    for url, sha256, relative in archive_sources(manifest):
        target = downloads / relative
        if target.is_file():
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        partial = target.with_name(target.name + ".part")
        fetched = subprocess.run(["curl", "-4", "-fsSL", "--retry", "5", "--retry-all-errors",
                                  "--connect-timeout", "20", "--max-time", "300", "-o", str(partial), url],
                                 env=env).returncode == 0
        if fetched and digest(partial) == sha256:
            partial.rename(target)
        else:
            print("prefetch failed (flatpak-builder will retry): " + url, flush=True)
            partial.unlink(missing_ok=True)


def check_qml_smoke(result):
    if (result.get("failures") != [] or result.get("qml_errors") != [] or result.get("screens", 0) < 3
            or result.get("waveform_items") != 2 or result.get("scratch_engaged") is not True
            or result.get("scratch_released") is not True or result.get("play_tap") is not True
            or result.get("audio_started") is not False):
        raise RuntimeError("QML smoke check failed: " + json.dumps(result, sort_keys=True))
    return result


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def text_digest(path):
    return hashlib.sha256(Path(path).read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def command(args, *, env=None, capture=False):
    print("+", " ".join(map(str, args)), flush=True)
    result = subprocess.run(list(map(str, args)), cwd=ROOT, env=env,
                            check=True, text=True,
                            stdout=subprocess.PIPE if capture else None)
    return result.stdout.strip() if capture else None


def audit_inputs(root, lock):
    for name, expected in lock["input_sha256"].items():
        path = (root / name).resolve()
        if not path.is_relative_to(root.resolve()) or text_digest(path) != expected:
            raise ValueError("Locked source differs: " + name)


def audit_sources(manifest):
    """Every downloaded dependency must carry a content hash or exact commit."""
    def visit(modules):
        for module in modules:
            if not isinstance(module, dict):
                raise ValueError("Manifest includes must be expanded first")
            if module.get("buildsystem") in ("cmake", "cmake-ninja"):
                libdirs = [option for option in module.get("config-opts", [])
                           if option.startswith("-DCMAKE_INSTALL_LIBDIR=")]
                if libdirs != ["-DCMAKE_INSTALL_LIBDIR=lib"]:
                    raise ValueError("CMake libraries must use /app/lib: " + module["name"])
            for source in module.get("sources", []):
                kind = source.get("type")
                if kind in ("archive", "file"):
                    if len(source.get("sha256", "")) != 64:
                        raise ValueError("Unpinned archive: " + module["name"])
                elif kind == "git":
                    if len(source.get("commit", "")) != 40:
                        raise ValueError("Unpinned Git source: " + module["name"])
                elif kind != "script":
                    raise ValueError("Unexpected source kind: " + str(kind))
            visit(module.get("modules", []))
    visit(manifest["modules"])


# Deck Lab system integration (decklabd client, Deck Setup, first run).
SYSTEM_SETUP_TESTS = frozenset({
    "ClientHandshakesSubscribesAndParsesSplitAndCoalescedFrames",
    "ClientDropsOverlongLinesAndReconnectsWithBackoff",
    "ClientTimesOutUnansweredRequestsAndFailsFastWhenOffline",
    "SocketPathComesFromEnvironmentThenRuntimeDirectory",
    "MidiPortNamesMatchTheirBundledMappings",
    "HidControllersMatchByProductIdsAndUsbOverridesApply",
    "AudioPolicyRoutesFourTwoAndNoControllerOutputs",
    "EjectIsRefusedWhileALoadedTrackLivesOnThatDrive",
    "ControllerSetupPersistsPerDeviceKeyAcrossRestart",
    "TerminationSignalBecomesOneGracefulQtRequest",
    "SystemPagesShowServiceStateAndRefuseUnsafeEject",
    "FirstRunFlowCompletesWithoutAControllerAndSetsTheFlag",
    # Get Music / Sample Maker lifecycle (getmusic-plus).
    "UtilityCloseAddsDownloadFolderAndRescansAfterDeckLoads",
})


def test_summary(path):
    report = ET.parse(path).getroot()
    cases = report.findall(".//testcase")
    enabled = [case for case in cases if case.get("status") == "run"]
    required = [case for case in enabled
                if case.get("classname") == "EngineBufferE2ETest"
                and case.get("name") == "PrepareScalersPreservesDeckState"]
    native_ui = {case.get("name") for case in enabled if case.get("classname") == "NativeDeckLabTest"}
    edge_labels = {case.get("name") for case in enabled if case.get("classname") == "WaveformMarkLabelTest"}
    system_setup = {case.get("name") for case in enabled if case.get("classname") == "DeckLabSystemTest"}
    if (int(report.get("failures", 0)) or int(report.get("errors", 0))
            or len(enabled) < 190 or len(required) != 1
            or not {"FirstBrowsePressDoesNotOpenSource", "SingleTapOpensFolderAndBackRestoresSelection",
                    "IndependentRmxDoesNotBorrowControllerChains", "PagesFit800By480",
                    "DecksTransportHasTruthfulStatesAndLargerTargets",
                    "LayerChangeReleasesOnlyTheReplacedTouchCue", "SyncAcknowledgmentCannotTransferToAnotherLayer",
                    "DeckIdentityAndLabelledReadoutsFitWithoutStealingWaveformSpace",
                    "TransportStateAndTargetRemainTruthfulInPlayingFixture",
                    "UpperLayerTransportTargetsLoadedDecksThreeAndFour", "TouchBeatSyncMatchesTempoAndPhaseOnceWithoutStopping",
                    "AutoLayoutShowsFourRealDecksAndRetainsLoadedUpperDecks", "LayerOrLayoutChangeCancelsPendingTransportPress",
                    "BrowseLoadCancelsChangedTargetAndOffersFourDirectDestinations", "LayoutRoundTripsKeepModeAndPitchReadoutsInsideEachDeck",
                    "BrowseToolsMatchPlayingFourthDeckAndGuardStaleUnload",
                    "SideUtilitiesOperateRealLoopsAndModesWithoutRetargetingHeldActions",
                    "TouchHotcuesSetAndJumpWithoutPreviewOrRetargeting",
                    "MorePanelKeepsHeldCueAndFullTrackInformationAccessible",
                    "MoreAndDialogsKeepTouchGeometryAfterShownStylePolish",
                    "FullDeckReadoutOpensTrackInformationAndRejectsStaleGestures",
                    "CompactDeckShowsActualFractionalLoopLength",
                    "DeckAndMasterFeedbackFollowRealControlsInBothLayouts",
                    "EmptyDeckReadoutReturnsToBrowseWithoutRetargetingChangedTrack",
                    "ControllerFxDetailsReadAllRealUnitsWithoutControlWrites",
                    "ControllerFxSummaryIncludesFourthSlotOnlyWithoutControlWrites",
                    "WaveformLayoutPreferenceRoundTripsBothDeckCountsWithoutEngineWrites",
                    "SettingsTouchSwipeRevealsDeckFourWithoutTogglingQuantize",
                    "SourcesTouchSwipeRevealsOffscreenRowsWithoutOpeningThem",
                    "LinkedZoomFollowsTheLastStableExternalChangeWithoutFeedback",
                    "MasterLockMissingBeatGridAndSavedLoopIndicatorsUseRealState",
                    "NativeComboPopupRowsHaveTouchHeight",
                    "FourthDeckCuePopupTargetsActualDecksAndCancelsChangedContext",
                    "BeatgridEditorUsesRealGridControlsAndRefusesUnsafeRelease",
                    "SharedTrackGridEditingRequiresEveryAliasedDeckToBeQuiet",
                    "Lv2PreparationUsesSelectedRateWithoutAudio", "TouchSearchUsesNativeKeyboardAndReturnsWaveforms"}.issubset(native_ui)
            or not {"EdgeLabelsStayInsideWithoutChangingWidth", "OverwideLabelsRemainInsideAtBothEdges",
                    "HotcueIdentifiersPreserveNumericDefaultAndSavedLabels", "LetterIdentifiersRequireExplicitSkinOptIn",
                    "CompactMarkerLanesRequireOptInAndPreserveTwoDeckImages",
                    "CompactMarkerOverflowOmitsOnlyLabelAndKeepsExactLine"}.issubset(edge_labels)
            or not SYSTEM_SETUP_TESTS.issubset(system_setup)
            or any(case.find("skipped") is not None for case in enabled)):
        raise ValueError("Native regression suite incomplete or failed")
    return {"enabled_passed": len(enabled),
            "disabled": len(cases) - len(enabled),
            "failures": int(report.get("failures", 0)),
            "errors": int(report.get("errors", 0)),
            "preparation_regression_passed": True, "native_ui_regressions_passed": len(native_ui),
            "waveform_edge_regressions_passed": len(edge_labels),
            "system_setup_regressions_passed": len(system_setup)}


def diagnostic_arguments(prefix, tree, output):
    # A diagnostic rerun is never acceptance evidence and must not overwrite
    # the original failing XML with a later successful timing run.
    boundary = prefix.index(tree)
    debug = prefix[:boundary] + ["--allow=devel", tree, "gdb", "--batch",
            "-ex", "set pagination off", "-ex", "run", "-ex", "thread apply all bt",
            "--args"] + prefix[boundary + 1:]
    return ["--gtest_output=xml:" + str(output / "native-test-backtrace.xml")
            if str(arg).startswith("--gtest_output=xml:") else arg for arg in debug]


def external_workdir(root, path):
    work = path.expanduser().resolve()
    if work == root.resolve() or work.is_relative_to(root.resolve()):
        raise ValueError("Build directory must be outside the source checkout")
    return work


def build_qml_candidate(env, work, out, tree, repo, generated, revision, lock, actual):
    """Bundle the QML build and capture its Decks screen under Xvfb (Mesa llvmpipe).

    No product gates run here; the default job owns them. Never deployed."""
    binary = tree / "files/bin/mixxx.bin"
    if binary.read_bytes()[:4] != b"\x7fELF" or int.from_bytes(binary.read_bytes()[18:20], "little") != 183:
        raise RuntimeError("Expected an AArch64 ELF")
    shutil.copy2(binary, out / "mixxx-qml.bin")
    bundle = out / ("decklab-mixxx-qml-aarch64-" + revision[:12] + ".flatpak")
    command(["flatpak", "build-bundle", "--arch=aarch64", "--runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo",
             repo, bundle, "org.mixxx.Mixxx", QML_BRANCH], env=env)
    command(["flatpak", "install", "--user", "--noninteractive", "--no-related", "--no-deps", bundle], env=env)
    # Qt Quick needs a real GL driver; the product smoke runs widgets in safe mode without one.
    gl = gl_extension_ref(command(["flatpak", "info", "--user", "--show-metadata", "org.kde.Platform//6.10"],
                                  env=env, capture=True))
    command(["flatpak", "install", "--user", "--noninteractive", "--no-related", "flathub", gl], env=env)
    gl_commit = command(["flatpak", "info", "--user", "--show-commit", gl], env=env, capture=True)
    if not shutil.which("xvfb-run"):
        raise RuntimeError("Missing build tool: xvfb-run")
    scratch = work / "test-home"
    scratch.mkdir(exist_ok=True)
    profile = scratch / "decklab-qml-smoke-profile"
    profile.mkdir(exist_ok=True)
    (profile / "mixxx.cfg").write_text(
        "[Config]\nResizableSkin DeckLab\nshow_menubar_hint 0\nhide_menubar 1\n"
        "[Key]\nKeyNotation Lancelot\n", encoding="utf-8")
    (profile / "soundconfig.xml").write_text(
        '<SoundManagerConfig api="None" samplerate="48000" latency="7" sync_buffers="2" deck_count="4"/>', encoding="utf-8")
    sandbox = ["--nodevice=all", "--nosocket=pulseaudio", "--nosocket=wayland", "--nosocket=fallback-x11",
               "--unshare=network", "--env=HOME=" + str(scratch), "--env=XDG_CONFIG_HOME=" + str(scratch / "qml-config"),
               "--filesystem=" + str(work), "--command=/app/bin/mixxx.bin"]
    ref = "org.mixxx.Mixxx//" + QML_BRANCH
    app = ["--settings-path=" + str(profile), "--resource-path=/app/share/mixxx"]
    version = command(["flatpak", "run", "--nosocket=x11", "--env=QT_QPA_PLATFORM=offscreen"] + sandbox
                      + [ref] + app + ["--version"], env=env, capture=True)
    (out / "runtime-version.txt").write_text(version + "\n")
    # A fixed display, set explicitly: run 39852481763 reached Qt with no DISPLAY. The smoke keeps the
    # host network namespace so libxcb can also use Xvfb's abstract socket (no devices, audio or
    # sockets otherwise); the diagnostics land in qml-smoke.log.
    display = ":99"
    smoke = (["xvfb-run", "--server-num=99", "--server-args=-screen 0 1280x800x24 -ac +extension GLX -nolisten tcp",
              "sh", "-c", 'echo "xvfb DISPLAY=$DISPLAY"; ls -la /tmp/.X11-unix; exec "$@"', "sh",
              "flatpak", "run", "--socket=x11", "--env=DISPLAY=" + display, "--env=QT_QPA_PLATFORM=xcb",
              "--env=LIBGL_ALWAYS_SOFTWARE=1", "--env=QSG_INFO=1", "--env=DECKLAB_PERF_LOG=1",
              "--env=DECKLAB_QML_MAIN=" + QML_MAIN, "--env=DECKLAB_QML_SMOKE_DIR=" + str(out)]
             + [arg for arg in sandbox if arg != "--unshare=network"] + [ref] + app + ["--qml"])
    with (out / "qml-smoke.log").open("w") as log:
        print("+", " ".join(smoke), flush=True)
        subprocess.run(smoke, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
    smoke_result = check_qml_smoke(json.loads((out / "qml-smoke.json").read_text()))
    record = {"source_revision": revision, "upstream_source_revision": lock["upstream_source_revision"],
              "architecture": "aarch64", "experimental": "gpu-screen QML/EGLFS spike, not a product candidate",
              "flatpak_branch": QML_BRANCH, "flatpak_commits": actual, "gl_extension": {gl: gl_commit},
              "native_sha256": digest(out / "mixxx-qml.bin"), "bundle_sha256": digest(bundle),
              "manifest_sha256": digest(generated), "runtime_probe_passed": True, "deployed": False,
              "hardware_audio_tested": False, "qml_smoke": smoke_result,
              "tools": {name: command([name, "--version"], env=env, capture=True)
                        for name in ("flatpak", "flatpak-builder", "git")}}
    (out / "provenance.json").write_text(json.dumps(record, indent=2) + "\n")
    shutil.copy2(generated, out / "resolved-manifest.json")
    sums = [digest(p) + "  " + p.name for p in sorted(out.iterdir()) if p.is_file() and p.name != "SHA256SUMS"]
    (out / "SHA256SUMS").write_text("\n".join(sums) + "\n")
    print(json.dumps(record, indent=2), flush=True)


def build(work, jobs, lock):
    if sys.platform != "linux" or platform.machine().lower() not in ("aarch64", "arm64"):
        raise RuntimeError("Use a native ARM64 Linux build host")
    qml = qml_requested()
    if command(["git", "status", "--porcelain", "--untracked-files=no"], capture=True):
        raise RuntimeError("Commit tracked changes before building")
    command(["git", "merge-base", "--is-ancestor", lock["upstream_source_revision"], "HEAD"])
    revision = command(["git", "rev-parse", "HEAD"], capture=True)
    work.mkdir(parents=True, exist_ok=True)
    out = work / "artifacts"
    out.mkdir(exist_ok=True)
    compiler_cache = work / "compiler-cache"
    compiler_cache.mkdir(exist_ok=True)
    (compiler_cache / "ccache.conf").write_text("max_size = 2G\n")
    # Restrict SDK installs/updates and the optional candidate runtime probe to
    # this build workspace, not the host's normal Flatpak installation.
    env = dict(os.environ, FLATPAK_USER_DIR=str(work / "flatpak-user"),
               XDG_CACHE_HOME=str(work / "cache"), LC_ALL="C.UTF-8", TZ="UTC",
               CCACHE_DIR=str(work / "compiler-cache"),
               SOURCE_DATE_EPOCH=command(["git", "show", "-s", "--format=%ct", "HEAD"], capture=True))
    for tool in ("flatpak", "flatpak-builder", "git"):
        if not shutil.which(tool):
            raise RuntimeError("Missing build tool: " + tool)
    command(["flatpak", "remote-add", "--user", "--if-not-exists", "flathub",
             "https://dl.flathub.org/repo/flathub.flatpakrepo"], env=env)
    actual = {}
    for name, expected in lock["flatpak_commits"].items():
        command(["flatpak", "install", "--user", "--noninteractive", "--no-related",
                 "flathub", name], env=env)
        command(["flatpak", "update", "--user", "--noninteractive", "--no-related",
                 "--no-deps", "--commit=" + expected, name], env=env)
        actual[name] = command(["flatpak", "info", "--user", "--show-commit", name], env=env, capture=True)
        if actual[name] != expected:
            raise RuntimeError("Runtime/SDK pin mismatch: " + name)
    manifest = json.loads(command(["flatpak-builder", "--show-manifest", MANIFEST], env=env, capture=True))
    modules = [module for module in manifest["modules"] if module["name"] == "mixxx"]
    if len(modules) != 1:
        raise ValueError("Expected exactly one native Mixxx module")
    # Build committed source only, excluding all untracked local files.
    modules[0]["sources"][0] = {"type": "git", "url": str(ROOT), "commit": revision}
    if qml:
        configure_qml(modules[0])
    audit_sources(manifest)
    generated = work / "manifest.json"
    generated.write_text(json.dumps(manifest, indent=2) + "\n")
    state = work / "state"
    tree = work / "build"
    repo = work / "repo"
    builder = ["flatpak-builder", "--user", "--disable-rofiles-fuse", "--force-clean",
               "--keep-build-dirs", "--disable-tests", "--ccache", "--jobs=" + str(jobs),
               "--state-dir=" + str(state), "--repo=" + str(repo),
               "--default-branch=" + (QML_BRANCH if qml else "decklab")]
    if qml:
        prefetch_archives(manifest, state / "downloads", env)
        for attempt in range(3):
            try:
                command(builder + ["--download-only", tree, generated], env=env)
                break
            except subprocess.CalledProcessError:
                if attempt == 2:
                    raise
                time.sleep(60 * (attempt + 1))
    command(builder + [tree, generated], env=env)
    if qml:
        build_qml_candidate(env, work, out, tree, repo, generated, revision, lock, actual)
        return
    native = state / "build/mixxx"
    if not (native / "mixxx-test").is_file():
        raise RuntimeError("Native test executable was not retained")
    scratch = work / "test-home"
    scratch.mkdir(exist_ok=True)
    xml = out / "native-tests.xml"
    test_filter = ":".join(lock["test_suites"])
    prefix = ["flatpak", "build", "--readonly", "--bind-mount=/run/build/mixxx=" + str(native.resolve()),
              "--filesystem=" + str(work), "--nodevice=all", "--nosocket=pulseaudio",
              "--nosocket=x11", "--nosocket=fallback-x11", "--nosocket=wayland", "--nosocket=session-bus", "--unshare=network",
              "--env=QT_QPA_PLATFORM=offscreen", "--env=HOME=" + str(scratch),
              "--env=LV2_PATH=/app/lib/lv2", "--env=DECKLAB_SCREENSHOT_DIR=" + str(out),
              "--env=XDG_CONFIG_HOME=" + str(scratch / "config"), "--build-dir=/run/build/mixxx",
              tree, "/run/build/mixxx/mixxx-test", "--settings-path=" + str(scratch / "profile"),
              "--resource-path=/run/build/mixxx/res", "--gtest_filter=" + test_filter,
              "--gtest_output=xml:" + str(xml)]
    with (out / "native-tests.log").open("w") as log:
        try:
            subprocess.run(list(map(str, prefix)), cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
        except subprocess.CalledProcessError:
            # Preserve an actionable native stack for crashes. This diagnostic
            # still has no audio devices, desktop sockets or network, and never
            # turns a failed test run into an accepted candidate.
            debug = diagnostic_arguments(prefix, tree, out)
            with (out / "native-test-backtrace.log").open("w") as trace:
                try:
                    subprocess.run(list(map(str, debug)), cwd=ROOT, env=env,
                            stdout=trace, stderr=subprocess.STDOUT, timeout=90)
                except (OSError, subprocess.TimeoutExpired) as error:
                    trace.write(str(error) + "\n")
            raise
    summary = test_summary(xml)
    dsp_test = ["flatpak", "build", "--readonly", "--filesystem=" + str(work),
                "--nodevice=all", "--nosocket=pulseaudio", "--nosocket=x11", "--nosocket=wayland",
                "--unshare=network", tree, "python3", "/run/build/mixxx/tools/decklab/test_dsp.py",
                "/app/lib/lv2/performance.lv2/performance.so"]
    # Bind committed tests explicitly; these tests use generated signals only.
    dsp_test.insert(3, "--bind-mount=/run/build/mixxx=" + str(native.resolve()))
    with (out / "dsp-tests.log").open("w") as log:
        subprocess.run(dsp_test, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    binary = tree / "files/bin/mixxx.bin"
    if binary.read_bytes()[:4] != b"\x7fELF" or int.from_bytes(binary.read_bytes()[18:20], "little") != 183:
        raise RuntimeError("Expected an AArch64 ELF")
    shutil.copy2(binary, out / "mixxx.bin")
    bundle = out / ("decklab-mixxx-aarch64-" + revision[:12] + ".flatpak")
    command(["flatpak", "build-bundle", "--arch=aarch64", "--runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo",
             repo, bundle, "org.mixxx.Mixxx", "decklab"], env=env)
    # Probe the packaged executable against the runtime, not just SDK libraries.
    command(["flatpak", "install", "--user", "--noninteractive", "--no-related", "--no-deps", bundle], env=env)
    probe = ["flatpak", "run", "--nodevice=all", "--nosocket=pulseaudio", "--nosocket=x11", "--nosocket=fallback-x11",
             "--nosocket=wayland", "--unshare=network", "--env=QT_QPA_PLATFORM=offscreen",
             "--env=HOME=" + str(scratch), "--env=XDG_CONFIG_HOME=" + str(scratch / "runtime-config"),
             "--filesystem=" + str(work), "--command=/app/bin/mixxx.bin", "org.mixxx.Mixxx//decklab",
             "--settings-path=" + str(scratch / "runtime-profile"), "--resource-path=/app/share/mixxx", "--version"]
    version = command(probe, env=env, capture=True)
    (out / "runtime-version.txt").write_text(version + "\n")
    smoke_profile = scratch / "decklab-native-smoke-profile"
    smoke_profile.mkdir(exist_ok=True)
    (smoke_profile / "mixxx.cfg").write_text(
        "[Config]\nResizableSkin DeckLab\nshow_menubar_hint 0\nhide_menubar 1\n"
        "[Key]\nKeyNotation Lancelot\n[Library]\nRowHeight 44\n", encoding="utf-8")
    (smoke_profile / "soundconfig.xml").write_text(
        '<SoundManagerConfig api="None" samplerate="48000" latency="7" sync_buffers="2" deck_count="4"/>', encoding="utf-8")
    smoke = [arg.replace("--settings-path=" + str(scratch / "runtime-profile"),
                         "--settings-path=" + str(smoke_profile)) for arg in probe if arg != "--version"]
    smoke.insert(2, "--env=DECKLAB_SMOKE_DIR=" + str(out))
    smoke.extend(["--safe-mode", "--developer"])
    with (out / "native-smoke.log").open("w") as log:
        subprocess.run(smoke, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=90)
    smoke_result = json.loads((out / "native-smoke.json").read_text())
    # 21 surface captures plus 22 Deck Lab system/first-run captures (two at 1024x600).
    if smoke_result != {"audio_started": False, "failures": [], "screens": 43, "native_rmx_units_loaded": 2}:
        raise RuntimeError("Native application smoke check failed")
    record = {"source_revision": revision, "upstream_source_revision": lock["upstream_source_revision"],
              "architecture": "aarch64", "flatpak_commits": actual, "tests": summary,
              "native_sha256": digest(out / "mixxx.bin"), "bundle_sha256": digest(bundle),
              "manifest_sha256": digest(generated), "input_sha256": lock["input_sha256"],
              "runtime_probe_passed": True, "deployed": False, "hardware_audio_tested": False,
              "native_application_smoke": smoke_result,
              "tools": {name: command([name, "--version"], env=env, capture=True)
                        for name in ("flatpak", "flatpak-builder", "git")}}
    (out / "provenance.json").write_text(json.dumps(record, indent=2) + "\n")
    shutil.copy2(generated, out / "resolved-manifest.json")
    shutil.copy2(LOCK, out / "pi-build-lock.json")
    sums = [digest(p) + "  " + p.name for p in sorted(out.iterdir()) if p.is_file() and p.name != "SHA256SUMS"]
    (out / "SHA256SUMS").write_text("\n".join(sums) + "\n")
    print(json.dumps(record, indent=2), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--jobs", type=int, default=3, choices=range(1, 9))
    parser.add_argument("--check-inputs", action="store_true")
    args = parser.parse_args()
    lock = json.loads(LOCK.read_text())
    audit_inputs(ROOT, lock)
    if args.check_inputs:
        print("PASS: locked native source and dependency manifests")
        return
    if not args.work_dir:
        parser.error("--work-dir is required and must be outside the source checkout")
    build(external_workdir(ROOT, args.work_dir), args.jobs, lock)


if __name__ == "__main__":
    main()
