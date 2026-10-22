"""Pure input/provenance guards; never invokes a build, device or network."""
import json
from pathlib import Path
import tempfile
import unittest

import build_pi


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
