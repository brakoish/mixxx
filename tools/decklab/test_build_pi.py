"""Pure input/provenance guards; never invokes a build, device or network."""
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest

import build_pi
# The DeckLab loading screen's numbers against its image run with these guards (CI runs this file).
from test_launch_image import LaunchImageGeometry  # noqa: F401


class BuildGuards(unittest.TestCase):
    def test_diagnostic_run_preserves_original_test_output_and_safety_boundary(self):
        tree = Path("/scratch/build")
        original = ["flatpak", "build", "--nodevice=all", "--unshare=network",
                    "--nosocket=pulseaudio", tree, "/run/build/mixxx/mixxx-test",
                    "--gtest_filter=NativeDeckLabTest.*", "--gtest_output=xml:/scratch/native-tests.xml"]
        snapshot = original[:]
        debug = build_pi.diagnostic_arguments(original, tree, Path("/scratch"))
        self.assertEqual(original, snapshot)
        self.assertIn("--gtest_output=xml:" + str(Path("/scratch") / "native-test-backtrace.xml"), debug)
        self.assertNotIn("--gtest_output=xml:/scratch/native-tests.xml", debug)
        for option in ("--nodevice=all", "--unshare=network", "--nosocket=pulseaudio",
                       "--gtest_filter=NativeDeckLabTest.*"):
            self.assertIn(option, debug)
        self.assertEqual(debug[debug.index("--args") + 1], "/run/build/mixxx/mixxx-test")

    def test_windows_and_linux_checkout_hashes_agree(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "source.cpp"
            path.write_bytes(b"first\nsecond\n")
            expected = build_pi.text_digest(path)
            path.write_bytes(b"first\r\nsecond\r\n")
            build_pi.audit_inputs(root, {"input_sha256": {"source.cpp": expected}})

    def test_rejects_changed_or_escaped_locked_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "source.cpp").write_text("tested code")
            lock = {"input_sha256": {"source.cpp": build_pi.digest(root / "source.cpp")}}
            build_pi.audit_inputs(root, lock)
            (root / "source.cpp").write_text("changed code")
            with self.assertRaises(ValueError):
                build_pi.audit_inputs(root, lock)
            with self.assertRaises(ValueError):
                build_pi.audit_inputs(root, {"input_sha256": {"../outside": "0" * 64}})

    def test_rejects_mutable_dependencies_in_nested_modules(self):
        good = {"modules": [{"name": "library", "sources": [{"type": "archive", "sha256": "a" * 64}]}]}
        build_pi.audit_sources(good)
        bad = {"modules": [{"name": "parent", "modules": [{"name": "library", "sources": [{"type": "git", "branch": "main"}]}]}]}
        with self.assertRaises(ValueError):
            build_pi.audit_sources(bad)
        with self.assertRaises(ValueError):
            build_pi.audit_sources({"modules": [{"name": "local", "sources": [{"type": "dir", "path": "."}]}]})

    def test_emulated_aarch64_is_an_explicit_opt_in(self):
        binfmt = "enabled\ninterpreter /usr/libexec/qemu-binfmt/aarch64-binfmt-P\nflags: POF\noffset 0\n"
        # CI's native ARM64 runner gets no extra Flatpak options; other hosts are refused.
        self.assertEqual(build_pi.build_target("aarch64", False), [])
        for machine in ("x86_64", "armv7l"):
            with self.assertRaises(RuntimeError):
                build_pi.build_target(machine, False, binfmt)
        self.assertEqual(build_pi.build_target("x86_64", True, binfmt), ["--arch=aarch64"])
        # Emulation needs an enabled qemu-aarch64 handler whose F flag reaches the sandbox.
        for broken in (None, binfmt.replace("POF", "PO"), binfmt.replace("enabled", "disabled")):
            with self.assertRaises(RuntimeError):
                build_pi.build_target("x86_64", True, broken)
        with self.assertRaises(RuntimeError):
            build_pi.build_target("aarch64", True, binfmt)

    def test_qemu_settings_are_set_by_bubblewrap_inside_every_sandbox(self):
        self.assertEqual(build_pi.EMULATED_CPU, "cortex-a72")  # the Pi 4's core
        self.assertEqual(build_pi.qemu_settings({"HOME": "/home/x"}), {"QEMU_CPU": "cortex-a72"})
        settings = build_pi.qemu_settings({"QEMU_UNAME": "6.6-generic", "QEMU_CPU": "max", "PATH": "/bin"})
        self.assertEqual(settings, {"QEMU_CPU": "max", "QEMU_UNAME": "6.6-generic"})
        script = build_pi.qemu_bwrap("/usr/bin/bwrap", build_pi.qemu_settings({}))
        self.assertEqual(script, '#!/bin/sh\nexec /usr/bin/bwrap --setenv QEMU_CPU cortex-a72 "$@"\n')
        self.assertIn("exec '/opt/my tools/bwrap' --setenv QEMU_CPU 'a b' --setenv QEMU_UNAME 6.6 \"$@\"",
                      build_pi.qemu_bwrap("/opt/my tools/bwrap", {"QEMU_CPU": "a b", "QEMU_UNAME": "6.6"}))

    def test_emulation_options_leave_native_defaults_unchanged(self):
        _, default = build_pi.options([])
        self.assertEqual((default.jobs, default.emulate_aarch64, default.timeout_scale), (3, False, 1))
        _, native = build_pi.options(["--work-dir", "/scratch", "--jobs", "4"])
        self.assertEqual((native.jobs, native.emulate_aarch64, native.timeout_scale), (4, False, 1))
        _, emulated = build_pi.options(["--work-dir", "/scratch", "--jobs", "24", "--emulate-aarch64"])
        self.assertEqual((emulated.jobs, emulated.timeout_scale), (24, build_pi.EMULATED_TIMEOUT_SCALE))
        _, scaled = build_pi.options(["--emulate-aarch64", "--timeout-scale", "40"])
        self.assertEqual(scaled.timeout_scale, 40)
        for argv in (["--jobs", "9"], ["--timeout-scale", "5"], ["--emulate-aarch64", "--timeout-scale", "0"],
                     ["--emulate-aarch64", "--jobs", "33"]):
            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                build_pi.options(argv)

    def test_refuses_source_recursion(self):
        root = Path.cwd()
        with self.assertRaises(ValueError):
            build_pi.external_workdir(root, root / "nested")

    def test_cmake_libdir_matches_flatpak_runtime(self):
        module = {"name": "library", "buildsystem": "cmake-ninja",
                  "config-opts": ["-DCMAKE_INSTALL_LIBDIR=lib"]}
        build_pi.audit_sources({"modules": [module]})
        for options in ([], ["-DCMAKE_INSTALL_LIBDIR=lib64"],
                        ["-DCMAKE_INSTALL_LIBDIR=lib", "-DCMAKE_INSTALL_LIBDIR=lib64"]):
            module["config-opts"] = options
            with self.assertRaises(ValueError):
                build_pi.audit_sources({"modules": [module]})

    def test_disabled_or_missing_preparation_regression_cannot_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "tests.xml"
            common = ''.join('<testcase name="test%d" classname="Other" status="run"/>' % i for i in range(190))
            for required in ('', '<testcase name="PrepareScalersPreservesDeckState" classname="EngineBufferE2ETest" status="notrun"/>'):
                path.write_text('<testsuites failures="0" errors="0"><testsuite>' + common + required + '</testsuite></testsuites>')
                with self.assertRaises(ValueError):
                    build_pi.test_summary(path)
            required = '<testcase name="PrepareScalersPreservesDeckState" classname="EngineBufferE2ETest" status="run"/>'
            for name in ("FirstBrowsePressDoesNotOpenSource", "SingleTapOpensFolderAndBackRestoresSelection",
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
                         "SettingsTouchSwipeScrollsWithoutTogglingQuantize",
                         "SourcesTouchSwipeRevealsOffscreenRowsWithoutOpeningThem",
                         "LinkedZoomFollowsTheLastStableExternalChangeWithoutFeedback",
                         "MasterLockMissingBeatGridAndSavedLoopIndicatorsUseRealState",
                         "NativeComboPopupRowsHaveTouchHeight",
                         "FourthDeckCuePopupTargetsActualDecksAndCancelsChangedContext",
                         "GridModeEditsTheRealGridBesideItsWaveformAndOwnsTheKnob",
                         "GridModeWaitsWhileTheSameTrackPlaysOrSyncsOnAnotherDeck",
                         "KeySheetMatchesShiftsAndResetsWithMixxxKeyControls",
                         "Lv2PreparationUsesSelectedRateWithoutAudio", "TouchSearchUsesNativeKeyboardAndReturnsWaveforms"):
                required += '<testcase name="%s" classname="NativeDeckLabTest" status="run"/>' % name
            for name in ("EdgeLabelsStayInsideWithoutChangingWidth", "OverwideLabelsRemainInsideAtBothEdges",
                         "HotcueIdentifiersPreserveNumericDefaultAndSavedLabels", "LetterIdentifiersRequireExplicitSkinOptIn",
                         "CompactMarkerLanesRequireOptInAndPreserveTwoDeckImages",
                         "CompactMarkerOverflowOmitsOnlyLabelAndKeepsExactLine"):
                required += '<testcase name="%s" classname="WaveformMarkLabelTest" status="run"/>' % name
            # Without the system-setup suite the gate still fails.
            path.write_text('<testsuites failures="0" errors="0"><testsuite>' + common + required + '</testsuite></testsuites>')
            with self.assertRaises(ValueError):
                build_pi.test_summary(path)
            for name in sorted(build_pi.SYSTEM_SETUP_TESTS):
                required += '<testcase name="%s" classname="DeckLabSystemTest" status="run"/>' % name
            path.write_text('<testsuites failures="0" errors="0"><testsuite>' + common + required + '</testsuite></testsuites>')
            self.assertEqual(build_pi.test_summary(path)["enabled_passed"], 235 + len(build_pi.SYSTEM_SETUP_TESTS))
            path.write_text(path.read_text().replace('failures="0"', 'failures="1"'))
            with self.assertRaises(ValueError):
                build_pi.test_summary(path)


if __name__ == "__main__":
    unittest.main()
