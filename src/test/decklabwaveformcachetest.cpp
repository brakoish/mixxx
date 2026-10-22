// SPDX-License-Identifier: GPL-2.0-or-later
// Deck Lab pi4-perf: the cached RGB waveform renderer against the upstream
// renderer (pixel compare at several positions, zooms, sizes and gains),
// cache invalidation and upload volume, and a report-only CPU benchmark of
// the renderers per frame.
//
// The pixel compare rasterises what each renderer submits: the upstream
// vertex array, and the cached vertex buffer with the vertex shader applied
// on the CPU in the same operation order as the GLSL code. Both images use
// the same rasteriser (pixel centres, left/top edges inclusive), so this
// checks the geometry and colour logic exactly without a GPU. When an
// OpenGL context is available (a desktop or the Pi, not the headless CI
// runner) a second test renders both renderers into framebuffer objects and
// compares the pixels the GPU produced.
#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

#include "control/controlobject.h"
#include "controllers/decklabbridgegate.h"
#include "rendergraph/engine.h"
#include "rendergraph/geometry.h"
#include "test/mixxxtest.h"
#include "track/beats.h"
#include "track/track.h"
#include "waveform/isynctimeprovider.h"
#include "waveform/rendergate.h"
#include "waveform/renderers/allshader/waveformrenderbeat.h"
#include "waveform/renderers/allshader/waveformrendererrgb.h"
#include "waveform/renderers/allshader/waveformrendererrgbcached.h"
#include "waveform/renderers/waveformcolumncache.h"
#include "waveform/renderers/waveformwidgetrenderer.h"
#include "waveform/visualplayposition.h"
#include "waveform/waveform.h"

namespace {

using Options = allshader::WaveformRendererSignalBase::Options;
using Option = allshader::WaveformRendererSignalBase::Option;

class NoVSyncDelay final : public VSyncTimeProvider {
  public:
    std::chrono::microseconds fromTimerToNextSync(const PerformanceTimer&) override {
        return std::chrono::microseconds(0);
    }
    std::chrono::microseconds getSyncInterval() const override {
        return std::chrono::microseconds(16667);
    }
};

constexpr int kSampleRate = 44100;
constexpr int kTrackSeconds = 300;
constexpr SINT kTrackFrames = static_cast<SINT>(kSampleRate) * kTrackSeconds;
constexpr int kHeight = 96;

/// Deterministic, music-like waveform data: a kick every 0.469 s (128 BPM),
/// a busy mid and high band, breakdowns with silence, and full-scale peaks.
WaveformPointer makeWaveform() {
    auto pWaveform = WaveformPointer(new Waveform(kSampleRate, kTrackFrames, 441, -1, 0));
    WaveformData* pData = pWaveform->data();
    const int dataSize = pWaveform->getDataSize();
    quint32 seed = 0x2545F491u;
    const auto next = [&seed] {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    };
    for (int i = 0; i < dataSize; ++i) {
        const int frame = i / 2;
        const double seconds = frame / 441.0;
        const double beatPhase = std::fmod(seconds, 60.0 / 128.0) / (60.0 / 128.0);
        const bool breakdown = std::fmod(seconds, 64.0) > 56.0;
        const bool silent = std::fmod(seconds, 97.0) < 0.6;
        WaveformFilteredData& filtered = pData[i].filtered;
        if (silent) {
            filtered = {0, 0, 0, 0};
            continue;
        }
        const int kick = breakdown ? 0 : static_cast<int>(255.0 * std::exp(-beatPhase * 9.0));
        const int low = std::min(255, kick + static_cast<int>(next() % 24));
        const int mid = static_cast<int>(40 + next() % 150);
        const int high = static_cast<int>((next() % 7 == 0) ? 255 : next() % 90);
        filtered.low = static_cast<unsigned char>(low);
        filtered.mid = static_cast<unsigned char>(mid);
        filtered.high = static_cast<unsigned char>(high);
        filtered.all = static_cast<unsigned char>(std::min(255, std::max({low, mid, high}) + 10));
    }
    pWaveform->setCompletion(dataSize);
    return pWaveform;
}

TrackPointer makeTrack(ConstWaveformPointer pWaveform) {
    TrackPointer pTrack = Track::newTemporary();
    pTrack->setAudioProperties(mixxx::audio::ChannelCount(2),
            mixxx::audio::SampleRate(kSampleRate),
            mixxx::audio::Bitrate(),
            mixxx::Duration::fromSeconds(kTrackSeconds));
    pTrack->trySetBeats(mixxx::Beats::fromConstTempo(mixxx::audio::SampleRate(kSampleRate),
            mixxx::audio::kStartFramePos,
            mixxx::Bpm(128.0)));
    pTrack->setWaveform(pWaveform);
    return pTrack;
}

quint32 toPixel(float r, float g, float b) {
    const auto channel = [](float value) {
        const float clamped = std::clamp(value, 0.f, 1.f);
        return static_cast<quint32>(std::lround(clamped * 255.f));
    };
    return 0xff000000u | (channel(r) << 16) | (channel(g) << 8) | channel(b);
}

/// Rasterises axis-aligned rectangles at pixel centres, left/top inclusive.
class Raster {
  public:
    Raster(int width, int height)
            : m_width(width),
              m_height(height),
              m_pixels(static_cast<std::size_t>(width) * height, 0xff000000u) {
    }
    void fillRect(float xa, float ya, float xb, float yb, quint32 pixel) {
        const float x1 = std::min(xa, xb);
        const float x2 = std::max(xa, xb);
        const float y1 = std::min(ya, yb);
        const float y2 = std::max(ya, yb);
        for (int j = 0; j < m_height; ++j) {
            const float cy = static_cast<float>(j) + 0.5f;
            if (!(cy >= y1 && cy < y2)) {
                continue;
            }
            for (int i = 0; i < m_width; ++i) {
                const float cx = static_cast<float>(i) + 0.5f;
                if (cx >= x1 && cx < x2) {
                    m_pixels[static_cast<std::size_t>(j) * m_width + i] = pixel;
                }
            }
        }
    }
    const std::vector<quint32>& pixels() const {
        return m_pixels;
    }

  private:
    int m_width;
    int m_height;
    std::vector<quint32> m_pixels;
};

struct Comparison {
    int pixels = 0;
    int drawn = 0;        // non-background pixels in the upstream image
    int identical = 0;
    int offByOne = 0;     // every channel within 1/255
    int different = 0;    // some channel off by more than 1/255
};

Comparison compare(const std::vector<quint32>& expected, const std::vector<quint32>& actual) {
    Comparison result;
    result.pixels = static_cast<int>(expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (expected[i] != 0xff000000u) {
            ++result.drawn;
        }
        if (expected[i] == actual[i]) {
            ++result.identical;
            continue;
        }
        int maxDiff = 0;
        for (int shift = 0; shift <= 16; shift += 8) {
            const int a = static_cast<int>((expected[i] >> shift) & 0xff);
            const int b = static_cast<int>((actual[i] >> shift) & 0xff);
            maxDiff = std::max(maxDiff, std::abs(a - b));
        }
        if (maxDiff <= 1) {
            ++result.offByOne;
        } else {
            ++result.different;
        }
    }
    return result;
}

std::ostream& operator<<(std::ostream& os, const Comparison& c) {
    return os << "pixels=" << c.pixels << " drawn=" << c.drawn << " identical=" << c.identical
              << " offByOne=" << c.offByOne << " different=" << c.different;
}

/// What the upstream renderer submitted this frame.
std::vector<quint32> rasterizeUpstream(allshader::WaveformRendererRGB* pRenderer, int width) {
    Raster raster(width, kHeight);
    rendergraph::Geometry& geometry = pRenderer->geometry();
    const auto* pVertices = geometry.vertexDataAs<rendergraph::Geometry::RGBColoredPoint2D>();
    for (int i = 0; i + 5 < geometry.vertexCount(); i += 6) {
        float x1 = pVertices[i].position2D.x();
        float x2 = x1;
        float y1 = pVertices[i].position2D.y();
        float y2 = y1;
        for (int k = 1; k < 6; ++k) {
            x1 = std::min(x1, pVertices[i + k].position2D.x());
            x2 = std::max(x2, pVertices[i + k].position2D.x());
            y1 = std::min(y1, pVertices[i + k].position2D.y());
            y2 = std::max(y2, pVertices[i + k].position2D.y());
        }
        const QVector3D& color = pVertices[i].color3D;
        raster.fillRect(x1, y1, x2, y2, toPixel(color.x(), color.y(), color.z()));
    }
    return raster.pixels();
}

struct EmulatedVertex {
    float x;
    float y;
    float r;
    float g;
    float b;
};

/// The vertex shader of WaveformRendererRGBCached, on the CPU, in the same
/// operation order.
EmulatedVertex emulateVertex(const WaveformColumnCache::Vertex& vertex,
        const allshader::WaveformRendererRGBCached::FrameUniforms& u,
        bool axisMode) {
    if (axisMode) {
        return EmulatedVertex{vertex.x,
                u.halfBreadth + vertex.sign * 0.5f,
                u.axisColor.x(),
                u.axisColor.y(),
                u.axisColor.z()};
    }
    const float low = static_cast<float>(vertex.low);
    const float mid = static_cast<float>(vertex.mid);
    const float high = static_cast<float>(vertex.high);
    const float all = static_cast<float>(vertex.all);
    const float maxLow = low * u.bandGain.x();
    const float maxMid = mid * u.bandGain.y();
    const float maxHigh = high * u.bandGain.z();
    const float allUnscaled = low + mid + high;
    float eqGain = 1.0f;
    if (allUnscaled > 0.0f) {
        eqGain = (maxLow + maxMid + maxHigh) / allUnscaled;
    }
    float red = maxLow * u.lowColor.x() + maxMid * u.midColor.x() + maxHigh * u.highColor.x();
    float green = maxLow * u.lowColor.y() + maxMid * u.midColor.y() + maxHigh * u.highColor.y();
    float blue = maxLow * u.lowColor.z() + maxMid * u.midColor.z() + maxHigh * u.highColor.z();
    const float maxComponent = std::max(std::max(red, green), blue);
    if (maxComponent == 0.f) {
        red = 0.f;
        green = 0.f;
        blue = 0.f;
    } else {
        const float normFactor = 1.f / maxComponent;
        red *= normFactor;
        green *= normFactor;
        blue *= normFactor;
    }
    const float y = u.halfBreadth + vertex.sign * (u.heightFactor * eqGain * all);
    return EmulatedVertex{vertex.x + u.offsetX, y, red, green, blue};
}

void rasterizeRectangles(Raster* pRaster,
        const std::vector<WaveformColumnCache::Vertex>& vertices,
        int first,
        int count,
        const allshader::WaveformRendererRGBCached::FrameUniforms& uniforms,
        bool axisMode) {
    for (int i = first; i + 5 < first + count; i += 6) {
        EmulatedVertex corners[6];
        for (int k = 0; k < 6; ++k) {
            corners[k] = emulateVertex(vertices[static_cast<std::size_t>(i + k)],
                    uniforms,
                    axisMode);
        }
        float x1 = corners[0].x;
        float x2 = x1;
        float y1 = corners[0].y;
        float y2 = y1;
        for (const auto& corner : corners) {
            x1 = std::min(x1, corner.x);
            x2 = std::max(x2, corner.x);
            y1 = std::min(y1, corner.y);
            y2 = std::max(y2, corner.y);
        }
        pRaster->fillRect(x1, y1, x2, y2, toPixel(corners[0].r, corners[0].g, corners[0].b));
    }
}

/// What the cached renderer would draw this frame.
std::vector<quint32> rasterizeCached(allshader::WaveformRendererRGBCached* pRenderer, int width) {
    Raster raster(width, kHeight);
    if (!pRenderer->hasFrame()) {
        return raster.pixels();
    }
    const WaveformColumnCache& cache = pRenderer->columnCache();
    const auto& uniforms = pRenderer->frameUniforms();
    rasterizeRectangles(&raster,
            cache.vertices(),
            0,
            WaveformColumnCache::kAxisVertices,
            uniforms,
            true);
    for (const auto& range : cache.drawRanges()) {
        rasterizeRectangles(&raster,
                cache.vertices(),
                range.firstVertex,
                range.vertexCount,
                uniforms,
                false);
    }
    return raster.pixels();
}

class DeckLabWaveformCacheTest : public MixxxTest {
  protected:
    const QString m_group = QStringLiteral("[DeckLabWaveformCache]");
    const QString m_eqGroup = QStringLiteral("[EqualizerRack1_[DeckLabWaveformCache]_Effect1]");
    ControlObject m_rateRatio{ConfigKey(m_group, "rate_ratio")};
    ControlObject m_totalGain{ConfigKey(m_group, "total_gain")};
    ControlObject m_trackSamples{ConfigKey(m_group, "track_samples")};
    ControlObject m_eqEnabled{ConfigKey(m_group, "filterWaveformEnable")};
    ControlObject m_eqLow{ConfigKey(m_eqGroup, "parameter1")};
    ControlObject m_eqMid{ConfigKey(m_eqGroup, "parameter2")};
    ControlObject m_eqHigh{ConfigKey(m_eqGroup, "parameter3")};
    ControlObject m_killLow{ConfigKey(m_eqGroup, "button_parameter1")};
    ControlObject m_killMid{ConfigKey(m_eqGroup, "button_parameter2")};
    ControlObject m_killHigh{ConfigKey(m_eqGroup, "button_parameter3")};

    WaveformPointer m_pWaveform;
    TrackPointer m_pTrack;
    std::unique_ptr<WaveformWidgetRenderer> m_pRenderer;
    QSharedPointer<VisualPlayPosition> m_pPosition;
    NoVSyncDelay m_vsync;
    int m_width = 800;

    void SetUp() override {
        m_rateRatio.set(1.0);
        m_totalGain.set(1.0);
        m_trackSamples.set(static_cast<double>(kTrackFrames) * 2.0);
        m_eqEnabled.set(1.0);
        m_eqLow.set(1.0);
        m_eqMid.set(1.0);
        m_eqHigh.set(1.0);
        m_killLow.set(0.0);
        m_killMid.set(0.0);
        m_killHigh.set(0.0);
        m_pWaveform = makeWaveform();
        m_pTrack = makeTrack(m_pWaveform);
        m_pPosition = VisualPlayPosition::getVisualPlayPosition(m_group);
        createRenderer(800);
    }

    void TearDown() override {
        m_pRenderer.reset();
        m_pPosition->setInvalid();
    }

    void createRenderer(int width) {
        m_width = width;
        m_pRenderer = std::make_unique<WaveformWidgetRenderer>(m_group);
        ASSERT_TRUE(m_pRenderer->init());
        m_pRenderer->resizeRenderer(width, kHeight, 1.0f);
        m_pRenderer->setZoom(3.0);
        m_pRenderer->setTrack(m_pTrack);
    }

    /// Track position (0..1) of a play position `pixels` track pixels away.
    double trackPixel() const {
        return m_pRenderer->getAudioSamplePerPixel() * 2.0 / (static_cast<double>(kTrackFrames) * 2.0);
    }

    void setPosition(double playPosition, double slipPosition) {
        m_pPosition->set(playPosition,
                0.0,
                0.0,
                slipPosition,
                0.0,
                SlipModeState::Disabled,
                false,
                false,
                false,
                0.0,
                0.0,
                60.0,
                0.0);
        m_pRenderer->onPreRender(&m_vsync);
    }

    template<typename T_Renderer>
    std::unique_ptr<T_Renderer> makeSignalRenderer(
            ::WaveformRendererAbstract::PositionSource source, Options options) {
        auto pRenderer = std::make_unique<T_Renderer>(m_pRenderer.get(), source, options);
        EXPECT_TRUE(pRenderer->init());
        pRenderer->setAxesColor(QColor(0x2c, 0x31, 0x3a));
        pRenderer->setLowColor(QColor(0xf5, 0x2a, 0x1e));
        pRenderer->setMidColor(QColor(0x35, 0xc8, 0x50));
        pRenderer->setHighColor(QColor(0x2e, 0x6b, 0xff));
        return pRenderer;
    }

    Comparison compareFrame(allshader::WaveformRendererRGB* pUpstream,
            allshader::WaveformRendererRGBCached* pCached) {
        pUpstream->preprocess();
        pCached->preprocess();
        return compare(rasterizeUpstream(pUpstream, m_width), rasterizeCached(pCached, m_width));
    }

    static void expectMatches(const Comparison& c, const std::string& label) {
        // Exact geometry and the same arithmetic: everything identical except
        // rare float contraction differences (one colour step, or one pixel
        // row at a column end) and a sampling tie at the very first track
        // column. Allow 0.2% of the image for those.
        EXPECT_LE(c.different * 500, c.pixels) << label << ": " << c;
        EXPECT_LE((c.different + c.offByOne) * 200, c.pixels) << label << ": " << c;
        if (c.identical != c.pixels) {
            std::cout << "[ pixel    ] " << label << ": " << c << std::endl;
        }
    }
};

TEST_F(DeckLabWaveformCacheTest, PixelCompareMatchesUpstreamAtPositionsZoomsAndSizes) {
    struct Scenario {
        int width;
        double zoom;
        double position;
        Options options;
    };
    const Scenario scenarios[] = {
            {800, 3.0, 0.37, Options()},
            {800, 3.0, 0.0, Options()},       // track start: negative columns
            {800, 3.0, 0.9995, Options()},    // track end: columns past the data
            {800, 1.0, 0.52, Options()},
            {800, 7.5, 0.11, Options()},
            {800, 10.0, 0.73, Options()},
            {383, 3.0, 0.61, Options()},      // odd width: half-pixel play marker
            {392, 2.0, 0.25, Options()},      // side-by-side four-deck width
            {800, 3.0, 0.44, Options(Option::SplitStereoSignal)},
    };
    for (const auto& scenario : scenarios) {
        createRenderer(scenario.width);
        m_pRenderer->setZoom(scenario.zoom);
        auto pUpstream = makeSignalRenderer<allshader::WaveformRendererRGB>(
                ::WaveformRendererAbstract::Play, scenario.options);
        auto pCached = makeSignalRenderer<allshader::WaveformRendererRGBCached>(
                ::WaveformRendererAbstract::Play, scenario.options);
        setPosition(scenario.position, scenario.position);
        const Comparison c = compareFrame(pUpstream.get(), pCached.get());
        const std::string label = "width " + std::to_string(scenario.width) + " zoom " +
                std::to_string(scenario.zoom) + " position " +
                std::to_string(scenario.position) +
                (scenario.options.testFlag(Option::SplitStereoSignal) ? " split" : "");
        ASSERT_TRUE(pCached->hasFrame()) << label;
        EXPECT_GT(c.drawn, c.pixels / 10) << label << ": nothing drawn";
        expectMatches(c, label);
    }
}

TEST_F(DeckLabWaveformCacheTest, ScrollingAndScratchingReuseChunksAndStayIdentical) {
    auto pUpstream = makeSignalRenderer<allshader::WaveformRendererRGB>(
            ::WaveformRendererAbstract::Play, Options());
    auto pCached = makeSignalRenderer<allshader::WaveformRendererRGBCached>(
            ::WaveformRendererAbstract::Play, Options());
    double position = 0.2;
    setPosition(position, position);
    pCached->preprocess();
    pCached->takeCacheStats();
    const double pixel = trackPixel();

    // Play forward for ten seconds at 60 frames per second (147 px/s at the
    // default zoom), comparing every 25th frame.
    int upstreamBytes = 0;
    constexpr int kFrames = 600;
    for (int frame = 0; frame < kFrames; ++frame) {
        position += pixel * 2.45;
        setPosition(position, position);
        if (frame % 25 == 0) {
            expectMatches(compareFrame(pUpstream.get(), pCached.get()),
                    "playing frame " + std::to_string(frame));
        } else {
            pCached->preprocess();
        }
        upstreamBytes += 6 * 20 * m_width; // one rectangle per column, every frame
    }
    const WaveformColumnCache::Stats playing = pCached->takeCacheStats();
    EXPECT_EQ(0u, playing.resets);
    // 1470 px scrolled: about 12 chunks of 128 columns, plus prefetch.
    EXPECT_LE(playing.chunksBuilt, 16u);
    EXPECT_GE(playing.chunksBuilt, 11u);
    EXPECT_LT(playing.bytesDirty * 50, static_cast<quint64>(upstreamBytes))
            << "upload volume must drop by far more than 50x";
    std::cout << "[ cache    ] 600 playing frames: chunks=" << playing.chunksBuilt
              << " columns=" << playing.columnsBuilt << " upload=" << playing.bytesDirty
              << " B (upstream " << upstreamBytes << " B)" << std::endl;

    // Scratch back and forth (touch scratch on the waveform).
    for (int frame = 0; frame < 120; ++frame) {
        position += pixel * ((frame / 15) % 2 == 0 ? -9.0 : 6.0);
        setPosition(position, position);
        expectMatches(compareFrame(pUpstream.get(), pCached.get()),
                "scratch frame " + std::to_string(frame));
    }
    EXPECT_EQ(0u, pCached->takeCacheStats().resets);

    // A jump (hotcue) far away builds only the newly visible chunks.
    position = 0.81;
    setPosition(position, position);
    expectMatches(compareFrame(pUpstream.get(), pCached.get()), "jump");
    const WaveformColumnCache::Stats jump = pCached->takeCacheStats();
    EXPECT_EQ(0u, jump.resets);
    EXPECT_LE(jump.chunksBuilt, 8u);
}

TEST_F(DeckLabWaveformCacheTest, GainsColoursAndHeightNeverRebuildGeometry) {
    auto pUpstream = makeSignalRenderer<allshader::WaveformRendererRGB>(
            ::WaveformRendererAbstract::Play, Options());
    auto pCached = makeSignalRenderer<allshader::WaveformRendererRGBCached>(
            ::WaveformRendererAbstract::Play, Options());
    setPosition(0.4, 0.4);
    expectMatches(compareFrame(pUpstream.get(), pCached.get()), "neutral");
    pCached->preprocess(); // prefetches the next chunk
    pCached->takeCacheStats();

    m_eqLow.set(0.4);
    m_eqMid.set(1.7);
    m_killHigh.set(1.0);
    m_pRenderer->onPreRender(&m_vsync);
    expectMatches(compareFrame(pUpstream.get(), pCached.get()), "eq and kill");

    m_totalGain.set(0.6);
    m_pRenderer->onPreRender(&m_vsync);
    expectMatches(compareFrame(pUpstream.get(), pCached.get()), "deck gain");

    pUpstream->setAllChannelVisualGain(1.8);
    pCached->setAllChannelVisualGain(1.8);
    pUpstream->setLowVisualGain(0.5);
    pCached->setLowVisualGain(0.5);
    expectMatches(compareFrame(pUpstream.get(), pCached.get()), "visual gains");

    pUpstream->setLowColor(QColor(0xff, 0x3d, 0x2e));
    pCached->setLowColor(QColor(0xff, 0x3d, 0x2e));
    expectMatches(compareFrame(pUpstream.get(), pCached.get()), "theme colour");

    m_eqEnabled.set(0.0);
    m_pRenderer->onPreRender(&m_vsync);
    expectMatches(compareFrame(pUpstream.get(), pCached.get()), "eq display off");

    const WaveformColumnCache::Stats stats = pCached->takeCacheStats();
    EXPECT_EQ(0u, stats.resets);
    EXPECT_EQ(0u, stats.chunksBuilt) << "gains and colours are vertex shader uniforms";

    // The height only changes uniforms too (the breadth is not cached).
    m_pRenderer->resizeRenderer(m_width, kHeight + 40, 1.0f);
    setPosition(0.4, 0.4);
    pCached->preprocess();
    EXPECT_EQ(0u, pCached->takeCacheStats().chunksBuilt);
}

TEST_F(DeckLabWaveformCacheTest, ZoomTempoWidthAndTrackChangesRebuild) {
    auto pUpstream = makeSignalRenderer<allshader::WaveformRendererRGB>(
            ::WaveformRendererAbstract::Play, Options());
    auto pCached = makeSignalRenderer<allshader::WaveformRendererRGBCached>(
            ::WaveformRendererAbstract::Play, Options());
    setPosition(0.3, 0.3);
    pCached->preprocess();
    pCached->takeCacheStats();

    m_pRenderer->setZoom(4.0);
    m_pRenderer->onPreRender(&m_vsync);
    expectMatches(compareFrame(pUpstream.get(), pCached.get()), "zoom");
    EXPECT_EQ(1u, pCached->takeCacheStats().resets);

    m_rateRatio.set(1.08);
    m_pRenderer->onPreRender(&m_vsync);
    expectMatches(compareFrame(pUpstream.get(), pCached.get()), "tempo +8%");
    EXPECT_EQ(1u, pCached->takeCacheStats().resets);
    m_rateRatio.set(1.0);

    m_pRenderer->resizeRenderer(640, kHeight, 1.0f);
    m_width = 640;
    m_pRenderer->onPreRender(&m_vsync);
    expectMatches(compareFrame(pUpstream.get(), pCached.get()), "width");
    EXPECT_EQ(1u, pCached->takeCacheStats().resets);

    // A different track (different waveform object) rebuilds; ejecting
    // releases the cached waveform.
    auto pOtherWaveform = makeWaveform();
    auto pOtherTrack = makeTrack(pOtherWaveform);
    m_pRenderer->setTrack(pOtherTrack);
    m_pRenderer->onPreRender(&m_vsync);
    expectMatches(compareFrame(pUpstream.get(), pCached.get()), "track change");
    EXPECT_EQ(1u, pCached->takeCacheStats().resets);
    EXPECT_TRUE(pCached->columnCache().holdsWaveform());
    m_pRenderer->setTrack(TrackPointer());
    pCached->preprocess();
    EXPECT_FALSE(pCached->hasFrame());
    EXPECT_FALSE(pCached->columnCache().holdsWaveform())
            << "the cache must not keep an ejected track's waveform alive";
}

TEST_F(DeckLabWaveformCacheTest, ChunksDuringAnalysisAreRebuiltAsDataArrives) {
    // A track that is still being analysed: only the first 40% are written.
    auto pWaveform = WaveformPointer(new Waveform(kSampleRate, kTrackFrames, 441, -1, 0));
    const int dataSize = pWaveform->getDataSize();
    const ConstWaveformPointer pReference = makeWaveform();
    const int analysed = (dataSize * 2 / 5) & ~1;
    std::copy(pReference->data(), pReference->data() + analysed, pWaveform->data());
    pWaveform->setCompletion(analysed);
    auto pTrack = makeTrack(pWaveform);
    m_pRenderer->setTrack(pTrack);

    auto pUpstream = makeSignalRenderer<allshader::WaveformRendererRGB>(
            ::WaveformRendererAbstract::Play, Options());
    auto pCached = makeSignalRenderer<allshader::WaveformRendererRGBCached>(
            ::WaveformRendererAbstract::Play, Options());
    // Look at the analysis front.
    const double front = static_cast<double>(analysed) / dataSize;
    setPosition(front, front);
    expectMatches(compareFrame(pUpstream.get(), pCached.get()), "analysis front");
    pCached->preprocess(); // prefetches the next chunk
    pCached->takeCacheStats();

    // Nothing new: nothing rebuilt.
    pCached->preprocess();
    EXPECT_EQ(0u, pCached->takeCacheStats().chunksBuilt);

    // The analyser writes the rest; only the chunks that touched unwritten
    // data are rebuilt, and the picture matches again.
    std::copy(pReference->data() + analysed,
            pReference->data() + dataSize,
            pWaveform->data() + analysed);
    pWaveform->setCompletion(dataSize);
    expectMatches(compareFrame(pUpstream.get(), pCached.get()), "analysis complete");
    pCached->preprocess(); // the provisional prefetched chunk
    const WaveformColumnCache::Stats stats = pCached->takeCacheStats();
    EXPECT_EQ(0u, stats.resets);
    EXPECT_GE(stats.chunksBuilt, 1u);
    EXPECT_LE(stats.chunksBuilt, 6u);
    pCached->preprocess();
    EXPECT_EQ(0u, pCached->takeCacheStats().chunksBuilt);
}

TEST_F(DeckLabWaveformCacheTest, SlipRendererMatchesUpstream) {
    auto pUpstream = makeSignalRenderer<allshader::WaveformRendererRGB>(
            ::WaveformRendererAbstract::Slip, Options());
    auto pCached = makeSignalRenderer<allshader::WaveformRendererRGBCached>(
            ::WaveformRendererAbstract::Slip, Options());
    // Slip inactive: nothing drawn by either.
    setPosition(0.5, 0.5);
    pCached->preprocess();
    EXPECT_FALSE(pCached->hasFrame());
    // Slip active: the slip position scrolls on.
    double slip = 0.5;
    for (int frame = 0; frame < 90; ++frame) {
        slip += trackPixel() * 2.45;
        setPosition(0.47, slip);
        expectMatches(compareFrame(pUpstream.get(), pCached.get()),
                "slip frame " + std::to_string(frame));
    }
}

TEST_F(DeckLabWaveformCacheTest, RendererTimingsReportMeanAndP95) {
    RendererTimings timings;
    const int signal = timings.addCategory("signal");
    const int beat = timings.addCategory("beat");
    EXPECT_EQ(signal, timings.addCategory("signal"));
    for (int frame = 1; frame <= 100; ++frame) {
        timings.beginFrame();
        timings.add(signal, frame * 1000 + 300);
        timings.add(beat, 500);
        timings.add(-1, 999999); // unknown category: ignored
        timings.endFrame();
    }
    EXPECT_EQ(100, timings.frames());
    EXPECT_EQ(50800, timings.meanNanos(signal));
    EXPECT_EQ(95300, timings.p95Nanos(signal));
    EXPECT_EQ(500, timings.p95Nanos(beat));
    EXPECT_EQ(51300, timings.totalMeanNanos());
    EXPECT_EQ(95800, timings.totalP95Nanos());
    const QString report = timings.report();
    EXPECT_TRUE(report.contains(QStringLiteral("signal=0.051/0.095"))) << report.toStdString();
    EXPECT_TRUE(report.contains(QStringLiteral("total="))) << report.toStdString();
    timings.reset();
    EXPECT_EQ(0, timings.frames());
    EXPECT_EQ(-1, timings.meanNanos(signal));

    std::vector<qint64> scratch;
    EXPECT_EQ(-1, RendererTimings::percentile95({}, &scratch));
    EXPECT_EQ(7, RendererTimings::percentile95({7}, &scratch));
    EXPECT_EQ(19, RendererTimings::percentile95({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
                                                        15, 16, 17, 18, 19, 20},
                          &scratch));
}

TEST_F(DeckLabWaveformCacheTest, KillSwitchDefaultsToTheCache) {
    qunsetenv("DECKLAB_WAVEFORM_CACHE");
    EXPECT_TRUE(mixxx::decklab::waveformCacheEnabledFromEnvironment());
    qputenv("DECKLAB_WAVEFORM_CACHE", "0");
    EXPECT_FALSE(mixxx::decklab::waveformCacheEnabledFromEnvironment());
    qputenv("DECKLAB_WAVEFORM_CACHE", "1");
    EXPECT_TRUE(mixxx::decklab::waveformCacheEnabledFromEnvironment());
    qunsetenv("DECKLAB_WAVEFORM_CACHE");
}

TEST(DeckLabMidiBridgeGateTest, OnlyAnExplicitZeroSkipsTheStateBridge) {
    qunsetenv("DECKLAB_MIDI_BRIDGE");
    EXPECT_FALSE(mixxx::decklab::midiStateBridgeDisabledFromEnvironment());
    qputenv("DECKLAB_MIDI_BRIDGE", "1");
    EXPECT_FALSE(mixxx::decklab::midiStateBridgeDisabledFromEnvironment());
    qputenv("DECKLAB_MIDI_BRIDGE", "off");
    EXPECT_FALSE(mixxx::decklab::midiStateBridgeDisabledFromEnvironment());
    qputenv("DECKLAB_MIDI_BRIDGE", "0");
    EXPECT_TRUE(mixxx::decklab::midiStateBridgeDisabledFromEnvironment());
    qunsetenv("DECKLAB_MIDI_BRIDGE");

    EXPECT_TRUE(mixxx::decklab::isMidiStateBridgeMapping(QStringLiteral("DeckLab.midi.xml")));
    EXPECT_TRUE(mixxx::decklab::isMidiStateBridgeMapping(QStringLiteral("decklab.midi.xml")));
    // Controller mappings that merely carry the Deck Lab name stay untouched.
    EXPECT_FALSE(mixxx::decklab::isMidiStateBridgeMapping(
            QStringLiteral("Pioneer-DDJ-SB2-DeckLab.midi.xml")));
    EXPECT_FALSE(mixxx::decklab::isMidiStateBridgeMapping(QStringLiteral("DeckLab.js")));
}

/// Renders a renderer node through a real rendergraph engine into an FBO.
QImage renderWithGl(QOpenGLContext* pContext,
        std::unique_ptr<rendergraph::BaseNode> pNode,
        int width) {
    rendergraph::Engine engine(std::move(pNode));
    QOpenGLFramebufferObject fbo(width, kHeight);
    fbo.bind();
    QOpenGLFunctions* f = pContext->functions();
    f->glViewport(0, 0, width, kHeight);
    f->glClearColor(0.f, 0.f, 0.f, 1.f);
    f->glClear(GL_COLOR_BUFFER_BIT);
    engine.resize(width, kHeight);
    engine.preprocess();
    engine.render();
    f->glFinish();
    QImage image = fbo.toImage();
    fbo.release();
    return image;
}

std::vector<quint32> imagePixels(const QImage& image) {
    const QImage rgb = image.convertToFormat(QImage::Format_RGB32);
    std::vector<quint32> pixels;
    pixels.reserve(static_cast<std::size_t>(rgb.width()) * rgb.height());
    for (int y = 0; y < rgb.height(); ++y) {
        const auto* pLine = reinterpret_cast<const quint32*>(rgb.constScanLine(y));
        for (int x = 0; x < rgb.width(); ++x) {
            pixels.push_back(pLine[x] | 0xff000000u);
        }
    }
    return pixels;
}

TEST_F(DeckLabWaveformCacheTest, GpuPixelCompareWhenOpenGLIsAvailable) {
    QOffscreenSurface surface;
    surface.create();
    QOpenGLContext context;
    if (!surface.isValid() || !context.create() || !context.makeCurrent(&surface)) {
        // The headless CI runner has no OpenGL. The CPU comparison above
        // covers the geometry; run this on a desktop or on the Pi with
        // QT_QPA_PLATFORM=xcb to compare GPU output.
        std::cout << "[ gpu      ] OpenGL unavailable: GPU pixel compare not run" << std::endl;
        RecordProperty("gpu_pixel_compare", "unavailable");
        return;
    }
    RecordProperty("gpu_pixel_compare", "run");
    struct Scenario {
        double zoom;
        double position;
    };
    for (const Scenario scenario : {Scenario{3.0, 0.37}, Scenario{1.0, 0.0}, Scenario{6.0, 0.81}}) {
        m_pRenderer->setZoom(scenario.zoom);
        setPosition(scenario.position, scenario.position);
        auto pUpstream = makeSignalRenderer<allshader::WaveformRendererRGB>(
                ::WaveformRendererAbstract::Play, Options());
        auto pCached = makeSignalRenderer<allshader::WaveformRendererRGBCached>(
                ::WaveformRendererAbstract::Play, Options());
        const QImage upstream = renderWithGl(&context, std::move(pUpstream), m_width);
        const QImage cached = renderWithGl(&context, std::move(pCached), m_width);
        const Comparison c = compare(imagePixels(upstream), imagePixels(cached));
        const std::string label = "gpu zoom " + std::to_string(scenario.zoom) + " position " +
                std::to_string(scenario.position);
        EXPECT_GT(c.drawn, c.pixels / 10) << label;
        // Different shader code may round the last bit differently on the
        // GPU (column edges sit exactly on pixel centres upstream). Allow 1%.
        EXPECT_LE(c.different * 100, c.pixels) << label << ": " << c;
        std::cout << "[ gpu      ] " << label << ": " << c << std::endl;
    }
    context.doneCurrent();
}

struct BenchResult {
    double meanMicros;
    double p95Micros;
};

BenchResult summarize(std::vector<qint64>* pSamples) {
    std::vector<qint64> scratch;
    qint64 sum = 0;
    for (const qint64 value : *pSamples) {
        sum += value;
    }
    const double mean = pSamples->empty()
            ? 0.0
            : static_cast<double>(sum) / static_cast<double>(pSamples->size()) / 1000.0;
    const double p95 = static_cast<double>(RendererTimings::percentile95(*pSamples, &scratch)) /
            1000.0;
    return BenchResult{mean, p95};
}

// Report only, never a gate: CPU per frame of the upstream and the cached
// signal renderer and of the beat grid renderer while a deck plays.
TEST_F(DeckLabWaveformCacheTest, BenchmarkRendererCpuPerFrameReportOnly) {
    for (const int width : {800, 392}) {
        createRenderer(width);
        auto pUpstream = makeSignalRenderer<allshader::WaveformRendererRGB>(
                ::WaveformRendererAbstract::Play, Options());
        auto pCached = makeSignalRenderer<allshader::WaveformRendererRGBCached>(
                ::WaveformRendererAbstract::Play, Options());
        allshader::WaveformRenderBeat beat(m_pRenderer.get(), ::WaveformRendererAbstract::Play);
        std::vector<qint64> upstreamNanos;
        std::vector<qint64> cachedNanos;
        std::vector<qint64> beatNanos;
        constexpr int kFrames = 1200;
        upstreamNanos.reserve(kFrames);
        cachedNanos.reserve(kFrames);
        beatNanos.reserve(kFrames);
        double position = 0.1;
        // Warm up both renderers.
        setPosition(position, position);
        pUpstream->preprocess();
        pCached->preprocess();
        pCached->takeCacheStats();
        quint64 upstreamBytes = 0;
        QElapsedTimer timer;
        for (int frame = 0; frame < kFrames; ++frame) {
            position += trackPixel() * 2.45;
            setPosition(position, position);
            timer.start();
            pUpstream->preprocess();
            upstreamNanos.push_back(timer.nsecsElapsed());
            upstreamBytes += static_cast<quint64>(pUpstream->geometry().vertexCount()) * 20u;
            timer.start();
            pCached->preprocess();
            cachedNanos.push_back(timer.nsecsElapsed());
            timer.start();
            beat.preprocess();
            beatNanos.push_back(timer.nsecsElapsed());
        }
        const WaveformColumnCache::Stats stats = pCached->takeCacheStats();
        const BenchResult upstream = summarize(&upstreamNanos);
        const BenchResult cached = summarize(&cachedNanos);
        const BenchResult beats = summarize(&beatNanos);
        const QString line =
                QStringLiteral(
                        "DECKLAB_BENCH width=%1 frames=%2 signal-upstream mean=%3us "
                        "p95=%4us upload=%5B/frame | signal-cached mean=%6us p95=%7us "
                        "upload=%8B/frame chunks=%9 | beat mean=%10us p95=%11us")
                        .arg(QString::number(width),
                                QString::number(kFrames),
                                QString::number(upstream.meanMicros, 'f', 1),
                                QString::number(upstream.p95Micros, 'f', 1),
                                QString::number(upstreamBytes / kFrames),
                                QString::number(cached.meanMicros, 'f', 1),
                                QString::number(cached.p95Micros, 'f', 1),
                                QString::number(stats.bytesDirty / kFrames),
                                QString::number(stats.chunksBuilt))
                        .arg(QString::number(beats.meanMicros, 'f', 1),
                                QString::number(beats.p95Micros, 'f', 1));
        std::cout << line.toStdString() << std::endl;
        RecordProperty(QStringLiteral("bench_width_%1").arg(width).toStdString(),
                line.toStdString());
        EXPECT_TRUE(pCached->hasFrame());
        EXPECT_GT(upstreamBytes, 0u);
    }
}

} // namespace
