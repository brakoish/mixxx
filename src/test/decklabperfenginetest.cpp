// SPDX-License-Identifier: GPL-2.0-or-later
// Deck Lab perf-engine: render-on-demand gate, thread policy helpers and LV2
// audio-thread safety.
#include <gtest/gtest.h>

#include <QSet>
#include <QVector>
#include <atomic>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

#include "control/controlobject.h"
#include "soundio/decklabthreadpolicy.h"
#include "soundio/sounddevice.h"
#include "test/mixxxtest.h"
#include "waveform/isynctimeprovider.h"
#include "waveform/rendergate.h"
#include "waveform/renderers/waveformwidgetrenderer.h"
#include "waveform/visualplayposition.h"

#ifdef __LINUX__
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#ifdef __LILV__
#include "effects/backends/lv2/lv2backend.h"
#include "effects/backends/lv2/lv2effectprocessor.h"
#include "engine/channelhandle.h"
#endif

namespace {

using Decision = RenderGate::Decision;

RenderGate::Frame frame(qint64 nowMillis, quint64 key, bool moving = false, bool visible = true) {
    RenderGate::Frame result;
    result.visible = visible;
    result.moving = moving;
    result.stateKey = key;
    result.nowMillis = nowMillis;
    return result;
}

} // namespace

TEST(DeckLabRenderGateTest, FirstVisibleFrameRendersThenUnchangedFramesSkip) {
    RenderGate gate;
    EXPECT_EQ(Decision::Forced, gate.evaluate(frame(0, 7)));
    for (qint64 now = 16; now < 480; now += 16) {
        EXPECT_EQ(Decision::Skip, gate.evaluate(frame(now, 7))) << now;
    }
    EXPECT_EQ(1u, gate.stats().rendered());
    EXPECT_EQ(29u, gate.stats().skipped);
}

TEST(DeckLabRenderGateTest, ChangedStateRendersExactlyOnce) {
    RenderGate gate;
    gate.evaluate(frame(0, 1));
    EXPECT_EQ(Decision::Changed, gate.evaluate(frame(16, 2)));
    EXPECT_EQ(Decision::Skip, gate.evaluate(frame(33, 2)));
    EXPECT_EQ(Decision::Changed, gate.evaluate(frame(50, 1)));
    EXPECT_EQ(Decision::Skip, gate.evaluate(frame(66, 1)));
}

TEST(DeckLabRenderGateTest, MovingDeckRendersEveryFrame) {
    RenderGate gate;
    gate.evaluate(frame(0, 1));
    // Playing or scratching renders even when the rounded position (and so
    // the key) did not move this frame.
    for (qint64 now = 16; now < 1000; now += 16) {
        EXPECT_EQ(Decision::Moving, gate.evaluate(frame(now, 1, true))) << now;
    }
    // The first still frame after a scratch/pause compares against the last
    // moving frame, so a final position change is never missed.
    EXPECT_EQ(Decision::Changed, gate.evaluate(frame(1008, 2)));
    EXPECT_EQ(Decision::Skip, gate.evaluate(frame(1024, 2)));
}

TEST(DeckLabRenderGateTest, SafetyRefreshIsAtMostTwoHertz) {
    RenderGate gate;
    gate.evaluate(frame(0, 5));
    std::vector<qint64> refreshes;
    for (qint64 now = 16; now <= 5000; now += 16) {
        const Decision decision = gate.evaluate(frame(now, 5));
        if (decision == Decision::Safety) {
            refreshes.push_back(now);
        } else {
            EXPECT_EQ(Decision::Skip, decision);
        }
    }
    ASSERT_GE(refreshes.size(), 9u);
    ASSERT_LE(refreshes.size(), 10u);
    qint64 previous = 0;
    for (qint64 at : refreshes) {
        EXPECT_GE(at - previous, RenderGate::kDefaultSafetyIntervalMillis);
        previous = at;
    }
}

TEST(DeckLabRenderGateTest, HiddenWidgetsNeverRenderAndRenderOnceWhenShownAgain) {
    RenderGate gate;
    gate.evaluate(frame(0, 3));
    for (qint64 now = 16; now < 3000; now += 16) {
        EXPECT_EQ(Decision::Hidden, gate.evaluate(frame(now, 3, true, false)));
    }
    // Same key, not moving: shown again it must still be redrawn, because a
    // re-shown native window may hold stale or black content.
    EXPECT_EQ(Decision::Forced, gate.evaluate(frame(3000, 3)));
    EXPECT_EQ(Decision::Skip, gate.evaluate(frame(3016, 3)));
    EXPECT_EQ(1u, gate.stats().skipped);
    EXPECT_EQ(187u, gate.stats().hidden);
}

TEST(DeckLabRenderGateTest, RequestsForceExactlyTheNextFrame) {
    RenderGate gate;
    gate.evaluate(frame(0, 9));
    gate.requestRender();
    EXPECT_EQ(Decision::Forced, gate.evaluate(frame(16, 9)));
    EXPECT_EQ(Decision::Skip, gate.evaluate(frame(33, 9)));
    RenderGate::Frame exposed = frame(50, 9);
    exposed.forced = true;
    EXPECT_EQ(Decision::Forced, gate.evaluate(exposed));
    EXPECT_EQ(Decision::Skip, gate.evaluate(frame(66, 9)));
}

TEST(DeckLabRenderGateTest, DisabledGateRestoresUpstreamBehaviour) {
    RenderGate gate;
    gate.setEnabled(false);
    for (qint64 now = 0; now < 500; now += 16) {
        EXPECT_EQ(Decision::Ungated, gate.evaluate(frame(now, 4)));
    }
    EXPECT_EQ(Decision::Hidden, gate.evaluate(frame(500, 4, false, false)));
    EXPECT_EQ(0u, gate.stats().skipped);
    gate.resetStats();
    EXPECT_EQ(0u, gate.stats().rendered());
}

TEST(DeckLabRenderGateTest, HasherDetectsEverySingleValueChange) {
    const auto hash = [](double a, qint64 b, bool c, const void* d) {
        RenderStateHasher hasher;
        hasher.addDouble(a);
        hasher.addInt(b);
        hasher.addBool(c);
        hasher.addPointer(d);
        return hasher.value();
    };
    int anchor = 0;
    const quint64 base = hash(0.25, 3, false, &anchor);
    EXPECT_EQ(base, hash(0.25, 3, false, &anchor));
    EXPECT_NE(base, hash(0.25 + 1e-12, 3, false, &anchor));
    EXPECT_NE(base, hash(0.25, 4, false, &anchor));
    EXPECT_NE(base, hash(0.25, 3, true, &anchor));
    EXPECT_NE(base, hash(0.25, 3, false, nullptr));
    RenderStateHasher swapped;
    swapped.addInt(1);
    swapped.addInt(2);
    RenderStateHasher ordered;
    ordered.addInt(2);
    ordered.addInt(1);
    EXPECT_NE(swapped.value(), ordered.value());
}

namespace {

class NoSyncDelay final : public VSyncTimeProvider {
  public:
    std::chrono::microseconds fromTimerToNextSync(const PerformanceTimer&) override {
        return std::chrono::microseconds(0);
    }
    std::chrono::microseconds getSyncInterval() const override {
        return std::chrono::microseconds(16667);
    }
};

class DeckLabRendererStateTest : public MixxxTest {
  protected:
    static constexpr double kTrackSamples = 44100.0 * 2 * 60;
    const QString m_group = QStringLiteral("[DeckLabRenderGate]");
    ControlObject m_rateRatio{ConfigKey(m_group, "rate_ratio")};
    ControlObject m_totalGain{ConfigKey(m_group, "total_gain")};
    ControlObject m_trackSamples{ConfigKey(m_group, "track_samples")};

    void SetUp() override {
        m_rateRatio.set(1.0);
        m_totalGain.set(1.0);
        m_trackSamples.set(kTrackSamples);
    }

    void setPosition(VisualPlayPosition* pPosition, double playPosition, double rate) {
        pPosition->set(playPosition, rate, 0.0, playPosition, 0.0,
                SlipModeState::Disabled, false, false, false, 0.0, 0.0, 60.0,
                0.0);
    }
};

} // namespace

TEST_F(DeckLabRendererStateTest, KeyFollowsWholePixelsZoomSizeAndMarkerOnly) {
    WaveformWidgetRenderer renderer(m_group);
    ASSERT_TRUE(renderer.init());
    renderer.resizeRenderer(400, 100, 1.0f);
    renderer.setZoom(3.0);
    QSharedPointer<VisualPlayPosition> pPosition =
            VisualPlayPosition::getVisualPlayPosition(m_group);
    NoSyncDelay vsync;
    // A fresh renderer and a resize both ask for a frame.
    EXPECT_TRUE(renderer.takeRenderRequest());
    EXPECT_FALSE(renderer.takeRenderRequest());

    setPosition(pPosition.get(), 0.5, 0.0);
    renderer.onPreRender(&vsync);
    const quint64 paused = renderer.renderStateKey();
    EXPECT_FALSE(renderer.needsContinuousRender());
    renderer.onPreRender(&vsync);
    EXPECT_EQ(paused, renderer.renderStateKey());

    // Without a loaded waveform one track pixel is one stereo frame, see
    // WaveformWidgetRenderer::onPreRender() (audio samples per pixel = 1).
    const double pixel = 2.0 / kTrackSamples;
    setPosition(pPosition.get(), 0.5 + pixel * 0.2, 0.0);
    renderer.onPreRender(&vsync);
    EXPECT_EQ(paused, renderer.renderStateKey()) << "sub-pixel jitter must not render";
    setPosition(pPosition.get(), 0.5 + pixel * 2, 0.0);
    renderer.onPreRender(&vsync);
    EXPECT_NE(paused, renderer.renderStateKey());

    // Playing or scratching (any non-zero rate) renders continuously.
    setPosition(pPosition.get(), 0.5, 1.0);
    renderer.onPreRender(&vsync);
    EXPECT_TRUE(renderer.needsContinuousRender());
    setPosition(pPosition.get(), 0.5, -0.3);
    renderer.onPreRender(&vsync);
    EXPECT_TRUE(renderer.needsContinuousRender());
    setPosition(pPosition.get(), 0.5, 0.0);
    renderer.onPreRender(&vsync);
    EXPECT_FALSE(renderer.needsContinuousRender());
    EXPECT_EQ(paused, renderer.renderStateKey());

    renderer.setZoom(4.0);
    renderer.onPreRender(&vsync);
    const quint64 zoomed = renderer.renderStateKey();
    EXPECT_NE(paused, zoomed);
    renderer.setPlayMarkerPosition(0.25);
    EXPECT_NE(zoomed, renderer.renderStateKey());
    renderer.setPlayMarkerPosition(0.5);
    EXPECT_EQ(zoomed, renderer.renderStateKey());

    // The deck gain scales the drawn signal.
    m_totalGain.set(0.5);
    renderer.onPreRender(&vsync);
    EXPECT_NE(zoomed, renderer.renderStateKey());
    m_totalGain.set(1.0);
    renderer.onPreRender(&vsync);
    EXPECT_EQ(zoomed, renderer.renderStateKey());

    EXPECT_FALSE(renderer.takeRenderRequest());
    renderer.resizeRenderer(401, 100, 1.0f);
    EXPECT_TRUE(renderer.takeRenderRequest());
    renderer.onPreRender(&vsync);
    EXPECT_NE(zoomed, renderer.renderStateKey());

    // An invalidated position (track loading) is not motion.
    pPosition->setInvalid();
    renderer.onPreRender(&vsync);
    EXPECT_FALSE(renderer.needsContinuousRender());
    renderer.setTrack(TrackPointer());
    EXPECT_TRUE(renderer.takeRenderRequest());
}

TEST(DeckLabThreadPolicyTest, AudioCpuParsingAcceptsOnlyPlainIndicesBelowTheCpuCount) {
    using mixxx::decklab::parseCpuIndex;
    EXPECT_EQ(3, parseCpuIndex("3", 4));
    EXPECT_EQ(0, parseCpuIndex("0", 4));
    EXPECT_EQ(2, parseCpuIndex(" 2\n", 4));
    EXPECT_EQ(-1, parseCpuIndex("4", 4));
    EXPECT_EQ(-1, parseCpuIndex("", 4));
    EXPECT_EQ(-1, parseCpuIndex("-1", 4));
    EXPECT_EQ(-1, parseCpuIndex("+1", 4));
    EXPECT_EQ(-1, parseCpuIndex("1x", 4));
    EXPECT_EQ(-1, parseCpuIndex("0x1", 4));
    EXPECT_EQ(-1, parseCpuIndex("1,2", 4));
    EXPECT_EQ(-1, parseCpuIndex("99999999999999999999", 4));
    EXPECT_EQ(-1, parseCpuIndex("1", 0));
}

TEST(DeckLabThreadPolicyTest, EnvironmentSwitchesDefaultToUpstreamBehaviour) {
    qunsetenv("DECKLAB_AUDIO_CPU");
    qunsetenv("DECKLAB_ANALYZER_IDLE");
    qunsetenv("DECKLAB_RENDER_GATE");
    qunsetenv("DECKLAB_PERF_LOG");
    EXPECT_EQ(-1, mixxx::decklab::audioCpuFromEnvironment());
    EXPECT_FALSE(mixxx::decklab::analyzerIdleRequested());
    EXPECT_TRUE(mixxx::decklab::renderGateEnabledFromEnvironment());
    EXPECT_FALSE(mixxx::decklab::perfLogEnabledFromEnvironment());
    qputenv("DECKLAB_AUDIO_CPU", "0");
    EXPECT_EQ(0, mixxx::decklab::audioCpuFromEnvironment());
    qputenv("DECKLAB_AUDIO_CPU", "4096");
    EXPECT_EQ(-1, mixxx::decklab::audioCpuFromEnvironment());
    qputenv("DECKLAB_ANALYZER_IDLE", "1");
    EXPECT_TRUE(mixxx::decklab::analyzerIdleRequested());
    qputenv("DECKLAB_ANALYZER_IDLE", "0");
    EXPECT_FALSE(mixxx::decklab::analyzerIdleRequested());
    qputenv("DECKLAB_RENDER_GATE", "0");
    EXPECT_FALSE(mixxx::decklab::renderGateEnabledFromEnvironment());
    qputenv("DECKLAB_PERF_LOG", "1");
    EXPECT_TRUE(mixxx::decklab::perfLogEnabledFromEnvironment());
    qunsetenv("DECKLAB_AUDIO_CPU");
    qunsetenv("DECKLAB_ANALYZER_IDLE");
    qunsetenv("DECKLAB_RENDER_GATE");
    qunsetenv("DECKLAB_PERF_LOG");
}

#ifdef __LINUX__
TEST(DeckLabThreadPolicyTest, PinningAndIdlePolicyAffectOnlyTheCallingThread) {
    cpu_set_t before;
    CPU_ZERO(&before);
    ASSERT_EQ(0, pthread_getaffinity_np(pthread_self(), sizeof(before), &before));
    int firstAllowed = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE && firstAllowed < 0; ++cpu) {
        if (CPU_ISSET(cpu, &before)) {
            firstAllowed = cpu;
        }
    }
    ASSERT_GE(firstAllowed, 0);

    std::atomic<bool> pinned{false};
    std::atomic<int> cpusAfterPin{0};
    std::atomic<int> policy{-1};
    std::atomic<int> niceness{0};
    qputenv("DECKLAB_ANALYZER_IDLE", "1");
    std::thread worker([&] {
        QString error;
        pinned = mixxx::decklab::pinCurrentThreadToCpu(firstAllowed, &error);
        cpu_set_t after;
        CPU_ZERO(&after);
        pthread_getaffinity_np(pthread_self(), sizeof(after), &after);
        cpusAfterPin = CPU_COUNT(&after);
        mixxx::decklab::applyAnalyzerThreadPolicy();
        policy = sched_getscheduler(0);
        niceness = getpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)));
    });
    worker.join();
    qunsetenv("DECKLAB_ANALYZER_IDLE");

    EXPECT_TRUE(pinned);
    EXPECT_EQ(1, cpusAfterPin.load());
    EXPECT_TRUE(policy.load() == SCHED_IDLE || niceness.load() == 19)
            << "policy" << policy.load() << "nice" << niceness.load();

    // The calling (test) thread keeps its affinity and policy.
    cpu_set_t now;
    CPU_ZERO(&now);
    ASSERT_EQ(0, pthread_getaffinity_np(pthread_self(), sizeof(now), &now));
    EXPECT_TRUE(CPU_EQUAL(&before, &now));
    EXPECT_NE(SCHED_IDLE, sched_getscheduler(0));
}
#endif

#ifdef __LILV__
namespace {

class DeckLabLv2SafetyTest : public MixxxTest {
  protected:
    static constexpr SINT kFrames = 64;

    void SetUp() override {
        SoundDevice::publishOpenDeviceCount(0);
        m_pBackend = std::make_unique<LV2Backend>();
        m_pManifest = m_pBackend->getLV2Manifest(
                QStringLiteral("urn:decklab:threeband-performance-native-v1"));
        m_input.resize(kFrames * 2);
        for (int i = 0; i < m_input.size(); ++i) {
            m_input[i] = static_cast<CSAMPLE>(0.4 * std::sin(i * 0.37));
        }
        m_inputHandle = m_handles.getOrCreateHandle(QStringLiteral("[Channel1]"));
        m_outputHandle = m_handles.getOrCreateHandle(QStringLiteral("[Main]"));
    }

    void TearDown() override {
        SoundDevice::publishOpenDeviceCount(0);
    }

    std::unique_ptr<LV2EffectProcessor> createProcessor(int sampleRate) {
        auto pProcessor = std::make_unique<LV2EffectProcessor>(m_pManifest);
        const QSet<ChannelHandleAndGroup> inputs{
                ChannelHandleAndGroup(m_inputHandle, QStringLiteral("[Channel1]"))};
        const QSet<ChannelHandleAndGroup> outputs{
                ChannelHandleAndGroup(m_outputHandle, QStringLiteral("[Main]"))};
        pProcessor->initialize(inputs, outputs, parameters(sampleRate));
        return pProcessor;
    }

    static mixxx::EngineParameters parameters(int sampleRate) {
        return mixxx::EngineParameters(mixxx::audio::SampleRate(sampleRate), kFrames);
    }

    QVector<CSAMPLE> process(LV2EffectProcessor* pProcessor,
            int sampleRate,
            EffectEnableState state) {
        QVector<CSAMPLE> output(kFrames * 2, -9.0f);
        pProcessor->process(m_inputHandle,
                m_outputHandle,
                m_input.constData(),
                output.data(),
                parameters(sampleRate),
                state,
                GroupFeatureState());
        return output;
    }

    std::unique_ptr<LV2Backend> m_pBackend;
    LV2EffectManifestPointer m_pManifest;
    QVector<CSAMPLE> m_input;
    ChannelHandleFactory m_handles;
    ChannelHandle m_inputHandle;
    ChannelHandle m_outputHandle;
};

} // namespace

TEST_F(DeckLabLv2SafetyTest, NullInstancePassesAudioThroughWithoutInstantiating) {
    ASSERT_FALSE(m_pManifest.isNull());
    // The processor refuses rates below 8 kHz, so instantiation on the main
    // thread fails and the audio thread sees a null instance.
    LV2EffectGroupState state(parameters(4000));
    EXPECT_EQ(nullptr, state.lilvInstance(m_pManifest->getPlugin(), parameters(4000)));
    LV2EffectProcessor processor(m_pManifest);
    for (const EffectEnableState enable : {EffectEnableState::Enabling,
                 EffectEnableState::Enabled,
                 EffectEnableState::Disabling}) {
        QVector<CSAMPLE> output(kFrames * 2, -9.0f);
        processor.processChannel(&state,
                m_input.constData(),
                output.data(),
                parameters(4000),
                enable,
                GroupFeatureState());
        EXPECT_EQ(m_input, output);
        EXPECT_EQ(nullptr, state.instance()) << "audio thread must not instantiate";
    }
}

TEST_F(DeckLabLv2SafetyTest, PreparationInstantiatesOnlyWithDevicesClosed) {
    ASSERT_FALSE(m_pManifest.isNull());
    auto pProcessor = createProcessor(4000);
    // Failed instantiation: dry pass-through on every callback state.
    EXPECT_EQ(m_input, process(pProcessor.get(), 4000, EffectEnableState::Enabling));
    EXPECT_EQ(m_input, process(pProcessor.get(), 4000, EffectEnableState::Enabled));

    // With a device (and so a callback) running, preparation must not touch
    // the instance; the rate mismatch keeps the deck dry.
    SoundDevice::publishOpenDeviceCount(1);
    pProcessor->prepare(parameters(44100));
    EXPECT_EQ(m_input, process(pProcessor.get(), 44100, EffectEnableState::Enabling));

    // Devices closed: the main thread instantiates at the selected rate.
    SoundDevice::publishOpenDeviceCount(0);
    pProcessor->prepare(parameters(44100));
    const QVector<CSAMPLE> wet = process(pProcessor.get(), 44100, EffectEnableState::Enabling);
    ASSERT_EQ(m_input.size(), wet.size());
    bool processed = false;
    for (int i = 0; i < wet.size(); ++i) {
        // All sections off: the processor runs and returns the dry signal.
        EXPECT_NEAR(m_input[i], wet[i], 1e-5);
        processed |= wet[i] != -9.0f;
    }
    EXPECT_TRUE(processed);
    EXPECT_NEAR(m_input[5], process(pProcessor.get(), 44100, EffectEnableState::Disabling)[5], 1e-5);
}
#endif
