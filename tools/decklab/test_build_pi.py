"""Pure input/provenance guards; never invokes a build, device or network."""
import json
from pathlib import Path
import tempfile
import unittest

import build_pi


class BuildGuards(unittest.TestCase):
    def test_qml_candidate_is_opt_in_and_keeps_product_options(self):
        self.assertFalse(build_pi.qml_requested({}))
        self.assertFalse(build_pi.qml_requested({"DECKLAB_QML": ""}))
        self.assertFalse(build_pi.qml_requested({"DECKLAB_QML": "0"}))
        self.assertTrue(build_pi.qml_requested({"DECKLAB_QML": "1"}))
        with self.assertRaises(ValueError):
            build_pi.qml_requested({"DECKLAB_QML": "yes"})
        product = ["-DCMAKE_INSTALL_LIBDIR=lib", "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DBUILD_TESTING=ON"]
        module = {"name": "mixxx", "config-opts": list(product)}
        build_pi.configure_qml(module)
        self.assertEqual(module["config-opts"],
                         ["-DCMAKE_INSTALL_LIBDIR=lib", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                          "-DQML=ON", "-DBUILD_TESTING=OFF"])
        self.assertNotEqual(build_pi.QML_BRANCH, "decklab")

    def test_gl_extension_follows_runtime_metadata(self):
        metadata = ("[Runtime]\nname=org.kde.Platform\n\n[Extension org.freedesktop.Platform.GL]\n"
                    "versions=25.08;25.08-extra;1.4\nversion=1.4\ndirectory=lib/aarch64-linux-gnu/GL\n")
        self.assertEqual(build_pi.gl_extension_ref(metadata), "org.freedesktop.Platform.GL.default//25.08")
        with self.assertRaises(ValueError):
            build_pi.gl_extension_ref("[Extension org.freedesktop.Platform.Locale]\nversions=6.10\n")

    def test_qml_smoke_requires_every_check(self):
        good = {"failures": [], "qml_errors": [], "screens": 4, "waveform_items": 2, "scratch_engaged": True,
                "scratch_released": True, "play_tap": True, "audio_started": False}
        self.assertEqual(build_pi.check_qml_smoke(dict(good)), good)
        for key, value in (("failures", ["x"]), ("qml_errors", ["main.qml:1"]), ("screens", 2),
                           ("waveform_items", 1), ("scratch_engaged", False), ("scratch_released", False),
                           ("play_tap", False), ("audio_started", True)):
            with self.assertRaises(RuntimeError):
                build_pi.check_qml_smoke(dict(good, **{key: value}))

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
                         "SettingsTouchSwipeRevealsDeckFourWithoutTogglingQuantize",
                         "SourcesTouchSwipeRevealsOffscreenRowsWithoutOpeningThem",
                         "LinkedZoomFollowsTheLastStableExternalChangeWithoutFeedback",
                         "MasterLockMissingBeatGridAndSavedLoopIndicatorsUseRealState",
                         "NativeComboPopupRowsHaveTouchHeight",
                         "FourthDeckCuePopupTargetsActualDecksAndCancelsChangedContext",
                         "BeatgridEditorUsesRealGridControlsAndRefusesUnsafeRelease",
                         "SharedTrackGridEditingRequiresEveryAliasedDeckToBeQuiet",
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
            self.assertEqual(build_pi.test_summary(path)["enabled_passed"], 234 + len(build_pi.SYSTEM_SETUP_TESTS))
            path.write_text(path.read_text().replace('failures="0"', 'failures="1"'))
            with self.assertRaises(ValueError):
                build_pi.test_summary(path)


if __name__ == "__main__":
    unittest.main()
