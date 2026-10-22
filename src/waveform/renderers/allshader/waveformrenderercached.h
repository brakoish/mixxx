#pragma once

#include <QVector3D>
#include <memory>

#include "rendergraph/openglnode.h"
#include "util/class.h"
#include "waveform/renderers/allshader/waveformrenderersignalbase.h"
#include "waveform/renderers/waveformcolumncache.h"

class QOpenGLShaderProgram;

namespace allshader {
class WaveformRendererCached;
} // namespace allshader

/// Deck Lab (pi4-perf): the scrolling waveforms of the allshader RGB, Simple,
/// Filtered and RGB Stacked renderers, drawn from geometry that is built once
/// per chunk of columns instead of once per frame (see WaveformColumnCache).
/// Per frame it only computes the first visible column and sets a handful of
/// uniforms: the scroll offset, the gains and the colours. Vertex data is
/// uploaded to a buffer object only when a chunk is (re)built, about once a
/// second while a deck plays.
///
/// The output matches the upstream renderers: same columns, same sampling,
/// same colours and heights, applied in the vertex shader with the same
/// operation order. DECKLAB_WAVEFORM_CACHE=0 selects the upstream renderers.
class allshader::WaveformRendererCached final
        : public allshader::WaveformRendererSignalBase,
          public rendergraph::OpenGLNode {
  public:
    /// The upstream renderer this one replaces.
    enum class Style {
        RGB,      ///< allshader::WaveformRendererRGB
        Simple,   ///< allshader::WaveformRendererSimple
        Filtered, ///< allshader::WaveformRendererFiltered
        Stacked,  ///< allshader::WaveformRendererFiltered, RGB stacked colours
    };

    explicit WaveformRendererCached(WaveformWidgetRenderer* waveformWidget,
            Style style,
            ::WaveformRendererAbstract::PositionSource type =
                    ::WaveformRendererAbstract::Play,
            WaveformRendererSignalBase::Options options =
                    WaveformRendererSignalBase::Option::None);
    ~WaveformRendererCached() override;

    void onSetup(const QDomNode& node) override;

    /// Like upstream, only the RGB renderer draws the slip waveform.
    bool supportsSlip() const override {
        return m_style == Style::RGB;
    }

    // rendergraph::BaseNode / OpenGLNode
    void preprocess() override;
    void initializeGL() override;
    void paintGL() override;

    // The setters of allshader::WaveformRendererRGB.
    void setAxesColor(const QColor& axesColor);
    void setLowColor(const QColor& lowColor);
    void setMidColor(const QColor& midColor);
    void setHighColor(const QColor& highColor);

    /// Everything the vertex shader needs for one frame, computed on the CPU
    /// exactly like the upstream renderer computes it. Public for the
    /// pixel-compare test, which applies the shader on the CPU.
    struct FrameUniforms {
        float offsetX = 0.f;
        float halfBreadth = 0.f;
        float heightFactor = 0.f;
        /// EQ gains (low, mid, high).
        QVector3D bandGain;
        /// RGB: band colours mixed per column. Filtered/Stacked: the colour
        /// of each band plane.
        QVector3D lowColor;
        QVector3D midColor;
        QVector3D highColor;
        /// Simple: the one signal colour.
        QVector3D signalColor;
        QVector3D axisColor;
    };
    /// Shader modes (uniform `mode`).
    static constexpr float kModeRgb = 0.f;
    static constexpr float kModeSolid = 1.f;     // Simple
    static constexpr float kModeBandPlane = 2.f; // Filtered, Stacked

    Style style() const {
        return m_style;
    }
    /// Valid after preprocess(); false when nothing is drawn this frame.
    bool hasFrame() const {
        return m_hasFrame;
    }
    const FrameUniforms& frameUniforms() const {
        return m_uniforms;
    }
    const WaveformColumnCache& columnCache() const {
        return m_cache;
    }
    WaveformColumnCache::Stats takeCacheStats() {
        const WaveformColumnCache::Stats stats = m_cache.stats();
        m_cache.resetStats();
        return stats;
    }

    /// The GLSL sources (also used by the GL pixel-compare test).
    static const char* vertexShaderSource();
    static const char* fragmentShaderSource();

  private:
    static WaveformColumnCache::Kind kindOf(Style style);
    bool preprocessInner();
    void uploadDirty();
    void releaseBuffer();
    void drawPlane(int plane);

    const Style m_style;
    const bool m_isSlipRenderer;
    const WaveformRendererSignalBase::Options m_options;

    WaveformColumnCache m_cache;
    FrameUniforms m_uniforms;
    bool m_hasFrame = false;

    std::unique_ptr<QOpenGLShaderProgram> m_pProgram;
    bool m_programFailed = false;
    GLuint m_buffer = 0;
    int m_bufferVertices = 0;
    int m_matrixLocation = -1;
    int m_offsetLocation = -1;
    int m_halfBreadthLocation = -1;
    int m_heightFactorLocation = -1;
    int m_bandGainLocation = -1;
    int m_lowColorLocation = -1;
    int m_midColorLocation = -1;
    int m_highColorLocation = -1;
    int m_axisColorLocation = -1;
    int m_solidColorLocation = -1;
    int m_planeGainLocation = -1;
    int m_modeLocation = -1;
    int m_axisModeLocation = -1;
    int m_positionLocation = -1;
    int m_bandsLocation = -1;

    DISALLOW_COPY_AND_ASSIGN(WaveformRendererCached);
};
