#include "waveform/renderers/allshader/waveformrendererrgbcached.h"

#include <QOpenGLContext>
#include <QOpenGLShaderProgram>
#include <cstddef>

#include "rendergraph/engine.h"
#include "track/track.h"
#include "util/colorcomponents.h"
#include "waveform/renderers/waveformwidgetrenderer.h"
#include "waveform/waveform.h"

namespace allshader {

namespace {

// The vertex shader applies, per vertex, exactly what
// WaveformRendererRGB::preprocessInner() computes per column on the CPU, in
// the same operation order. GLSL without a #version line, like the other
// inline Mixxx shaders: Qt defines the precision qualifiers away on desktop
// OpenGL and keeps them on OpenGL ES.
const char kVertexShader[] = R"--(
uniform highp mat4 matrix;
uniform highp float offsetX;
uniform highp float halfBreadth;
uniform highp float heightFactor;
uniform highp vec3 bandGain;
uniform highp vec3 lowColor;
uniform highp vec3 midColor;
uniform highp vec3 highColor;
uniform highp vec3 axisColor;
uniform highp float axisMode;
attribute highp vec2 position;
attribute highp vec4 bands;
varying highp vec3 vColor;
void main()
{
    highp float y;
    highp float x;
    if (axisMode > 0.5) {
        vColor = axisColor;
        y = halfBreadth + position.y * 0.5;
        x = position.x;
    } else {
        highp float maxLow = bands.x * bandGain.x;
        highp float maxMid = bands.y * bandGain.y;
        highp float maxHigh = bands.z * bandGain.z;
        highp float allUnscaled = bands.x + bands.y + bands.z;
        highp float eqGain = 1.0;
        if (allUnscaled > 0.0) {
            eqGain = (maxLow + maxMid + maxHigh) / allUnscaled;
        }
        highp vec3 color = maxLow * lowColor + maxMid * midColor + maxHigh * highColor;
        highp float maxComponent = max(max(color.r, color.g), color.b);
        if (maxComponent == 0.0) {
            vColor = vec3(0.0, 0.0, 0.0);
        } else {
            vColor = color * (1.0 / maxComponent);
        }
        y = halfBreadth + position.y * (heightFactor * eqGain * bands.w);
        x = position.x + offsetX;
    }
    gl_Position = matrix * vec4(x, y, 0.0, 1.0);
}
)--";

const char kFragmentShader[] = R"--(
varying highp vec3 vColor;
void main()
{
    gl_FragColor = vec4(vColor, 1.0);
}
)--";

QVector3D rgbOf(float r, float g, float b) {
    return QVector3D(r, g, b);
}

} // namespace

WaveformRendererRGBCached::WaveformRendererRGBCached(WaveformWidgetRenderer* waveformWidget,
        ::WaveformRendererAbstract::PositionSource type,
        WaveformRendererSignalBase::Options options)
        : WaveformRendererSignalBase(waveformWidget),
          m_isSlipRenderer(type == ::WaveformRendererAbstract::Slip),
          m_options(options) {
    setUsePreprocess(true);
}

WaveformRendererRGBCached::~WaveformRendererRGBCached() {
    releaseBuffer();
}

// static
const char* WaveformRendererRGBCached::vertexShaderSource() {
    return kVertexShader;
}

// static
const char* WaveformRendererRGBCached::fragmentShaderSource() {
    return kFragmentShader;
}

void WaveformRendererRGBCached::releaseBuffer() {
    // Nodes are destroyed with their widget's context current (see
    // ~allshader::WaveformWidget). Without a context the buffer goes away
    // with the context itself.
    if (m_buffer != 0 && QOpenGLContext::currentContext() != nullptr) {
        glDeleteBuffers(1, &m_buffer);
    }
    m_buffer = 0;
    m_bufferVertices = 0;
}

void WaveformRendererRGBCached::setAxesColor(const QColor& axesColor) {
    getRgbF(axesColor, &m_axesColor_r, &m_axesColor_g, &m_axesColor_b, &m_axesColor_a);
}

void WaveformRendererRGBCached::setLowColor(const QColor& lowColor) {
    getRgbF(lowColor, &m_rgbLowColor_r, &m_rgbLowColor_g, &m_rgbLowColor_b);
}

void WaveformRendererRGBCached::setMidColor(const QColor& midColor) {
    getRgbF(midColor, &m_rgbMidColor_r, &m_rgbMidColor_g, &m_rgbMidColor_b);
}

void WaveformRendererRGBCached::setHighColor(const QColor& highColor) {
    getRgbF(highColor, &m_rgbHighColor_r, &m_rgbHighColor_g, &m_rgbHighColor_b);
}

void WaveformRendererRGBCached::onSetup(const QDomNode&) {
}

void WaveformRendererRGBCached::preprocess() {
    m_hasFrame = preprocessInner();
}

bool WaveformRendererRGBCached::preprocessInner() {
    // Mirrors allshader::WaveformRendererRGB::preprocessInner(); only the
    // per-column work moved to WaveformColumnCache and the vertex shader.
    TrackPointer pTrack = m_waveformRenderer->getTrackInfo();
    if (!pTrack) {
        m_cache.clear();
        return false;
    }
    if (m_isSlipRenderer && !m_waveformRenderer->isSlipActive()) {
        return false;
    }

    const auto positionType = m_isSlipRenderer ? ::WaveformRendererAbstract::Slip
                                               : ::WaveformRendererAbstract::Play;

    ConstWaveformPointer waveform = pTrack->getWaveform();
    if (waveform.isNull()) {
        m_cache.clear();
        return false;
    }

    const int dataSize = waveform->getDataSize();
    if (dataSize <= 1) {
        return false;
    }
    if (waveform->data() == nullptr) {
        return false;
    }
#ifdef __STEM__
    auto stemInfo = pTrack->getStemInfo();
    // If this track is a stem track, skip the rendering
    if (!stemInfo.isEmpty() && waveform->hasStem()) {
        return false;
    }
#endif

    const float devicePixelRatio = m_waveformRenderer->getDevicePixelRatio();
    const int length = static_cast<int>(m_waveformRenderer->getLength());
    const int pixelLength = static_cast<int>(m_waveformRenderer->getLength() * devicePixelRatio);
    const float invDevicePixelRatio = 1.f / devicePixelRatio;

    const int visualFramesSize = dataSize / 2;
    const double firstVisualFrame =
            m_waveformRenderer->getFirstDisplayedPosition(positionType) * visualFramesSize;
    const double lastVisualFrame =
            m_waveformRenderer->getLastDisplayedPosition(positionType) * visualFramesSize;

    const double visualIncrementPerPixel =
            (lastVisualFrame - firstVisualFrame) / static_cast<double>(pixelLength);
    if (visualIncrementPerPixel == 0.0) {
        return false;
    }

    // The global index of the first visible column. Upstream starts its
    // per-frame loop at the visual frame qRound(first / increment) * increment.
    const int firstColumn = qRound(firstVisualFrame / visualIncrementPerPixel);

    const bool splitLeftRight = m_options & WaveformRendererSignalBase::Option::SplitStereoSignal;
    if (!m_cache.prepare(waveform,
                visualIncrementPerPixel,
                devicePixelRatio,
                pixelLength,
                length,
                splitLeftRight,
                m_isSlipRenderer,
                firstColumn)) {
        return false;
    }

    float allGain = 1.0f;
    float lowGain = 1.0f;
    float midGain = 1.0f;
    float highGain = 1.0f;
    getGains(&allGain, &lowGain, &midGain, &highGain);

    const float breadth = static_cast<float>(m_waveformRenderer->getBreadth());
    const float halfBreadth = breadth / 2.0f;

    m_uniforms.offsetX = static_cast<float>(-firstColumn) * invDevicePixelRatio;
    m_uniforms.halfBreadth = halfBreadth;
    m_uniforms.heightFactor = allGain * halfBreadth / m_maxValue;
    m_uniforms.bandGain = QVector3D(lowGain, midGain, highGain);
    m_uniforms.lowColor = rgbOf(static_cast<float>(m_rgbLowColor_r),
            static_cast<float>(m_rgbLowColor_g),
            static_cast<float>(m_rgbLowColor_b));
    m_uniforms.midColor = rgbOf(static_cast<float>(m_rgbMidColor_r),
            static_cast<float>(m_rgbMidColor_g),
            static_cast<float>(m_rgbMidColor_b));
    m_uniforms.highColor = rgbOf(static_cast<float>(m_rgbHighColor_r),
            static_cast<float>(m_rgbHighColor_g),
            static_cast<float>(m_rgbHighColor_b));
    m_uniforms.axisColor = rgbOf(static_cast<float>(m_axesColor_r),
            static_cast<float>(m_axesColor_g),
            static_cast<float>(m_axesColor_b));
    return true;
}

void WaveformRendererRGBCached::initializeGL() {
    m_pProgram = std::make_unique<QOpenGLShaderProgram>();
    if (!m_pProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader) ||
            !m_pProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader) ||
            !m_pProgram->link()) {
        qWarning() << "WaveformRendererRGBCached: shader program failed;"
                   << "set DECKLAB_WAVEFORM_CACHE=0 to use the upstream renderer."
                   << m_pProgram->log();
        m_programFailed = true;
        return;
    }
    m_matrixLocation = m_pProgram->uniformLocation("matrix");
    m_offsetLocation = m_pProgram->uniformLocation("offsetX");
    m_halfBreadthLocation = m_pProgram->uniformLocation("halfBreadth");
    m_heightFactorLocation = m_pProgram->uniformLocation("heightFactor");
    m_bandGainLocation = m_pProgram->uniformLocation("bandGain");
    m_lowColorLocation = m_pProgram->uniformLocation("lowColor");
    m_midColorLocation = m_pProgram->uniformLocation("midColor");
    m_highColorLocation = m_pProgram->uniformLocation("highColor");
    m_axisColorLocation = m_pProgram->uniformLocation("axisColor");
    m_axisModeLocation = m_pProgram->uniformLocation("axisMode");
    m_positionLocation = m_pProgram->attributeLocation("position");
    m_bandsLocation = m_pProgram->attributeLocation("bands");
}

void WaveformRendererRGBCached::uploadDirty() {
    const auto& vertices = m_cache.vertices();
    constexpr int kVertexSize = static_cast<int>(sizeof(WaveformColumnCache::Vertex));
    if (m_buffer == 0) {
        glGenBuffers(1, &m_buffer);
        m_bufferVertices = 0;
    }
    glBindBuffer(GL_ARRAY_BUFFER, m_buffer);
    const bool reallocated = m_cache.takeReallocated();
    if (reallocated || m_bufferVertices != m_cache.vertexCount()) {
        // Allocate and upload everything (track, zoom, tempo or size change).
        m_bufferVertices = m_cache.vertexCount();
        glBufferData(GL_ARRAY_BUFFER,
                static_cast<qopengl_GLsizeiptr>(m_bufferVertices) * kVertexSize,
                vertices.data(),
                GL_DYNAMIC_DRAW);
        m_cache.clearDirty();
        return;
    }
    if (m_cache.isAxisDirty()) {
        glBufferSubData(GL_ARRAY_BUFFER,
                0,
                WaveformColumnCache::kAxisVertices * kVertexSize,
                vertices.data());
    }
    const int slotVertices = m_cache.slotVertexCount();
    for (int slot = 0; slot < m_cache.slotCount(); ++slot) {
        if (!m_cache.isSlotDirty(slot)) {
            continue;
        }
        const int first = m_cache.slotFirstVertex(slot);
        glBufferSubData(GL_ARRAY_BUFFER,
                static_cast<qopengl_GLintptr>(first) * kVertexSize,
                static_cast<qopengl_GLsizeiptr>(slotVertices) * kVertexSize,
                vertices.data() + first);
    }
    m_cache.clearDirty();
}

void WaveformRendererRGBCached::paintGL() {
    if (!m_hasFrame || m_programFailed || !m_pProgram) {
        return;
    }
    const auto& ranges = m_cache.drawRanges();
    if (ranges.isEmpty() || m_cache.vertexCount() == 0) {
        return;
    }
    if (!m_pProgram->bind()) {
        return;
    }
    uploadDirty();

    m_pProgram->setUniformValue(m_matrixLocation, engine()->matrix());
    m_pProgram->setUniformValue(m_halfBreadthLocation, m_uniforms.halfBreadth);
    m_pProgram->setUniformValue(m_heightFactorLocation, m_uniforms.heightFactor);
    m_pProgram->setUniformValue(m_bandGainLocation, m_uniforms.bandGain);
    m_pProgram->setUniformValue(m_lowColorLocation, m_uniforms.lowColor);
    m_pProgram->setUniformValue(m_midColorLocation, m_uniforms.midColor);
    m_pProgram->setUniformValue(m_highColorLocation, m_uniforms.highColor);
    m_pProgram->setUniformValue(m_axisColorLocation, m_uniforms.axisColor);

    constexpr int kVertexSize = static_cast<int>(sizeof(WaveformColumnCache::Vertex));
    glEnableVertexAttribArray(static_cast<GLuint>(m_positionLocation));
    glVertexAttribPointer(static_cast<GLuint>(m_positionLocation),
            2,
            GL_FLOAT,
            GL_FALSE,
            kVertexSize,
            reinterpret_cast<const void*>(offsetof(WaveformColumnCache::Vertex, x)));
    glEnableVertexAttribArray(static_cast<GLuint>(m_bandsLocation));
    glVertexAttribPointer(static_cast<GLuint>(m_bandsLocation),
            4,
            GL_UNSIGNED_BYTE,
            GL_FALSE,
            kVertexSize,
            reinterpret_cast<const void*>(offsetof(WaveformColumnCache::Vertex, low)));

    // Same blending as rendergraph::BaseGeometryNode::render().
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    // The axis line first (screen space), then the columns over it, as in
    // the upstream single draw call.
    m_pProgram->setUniformValue(m_axisModeLocation, 1.0f);
    m_pProgram->setUniformValue(m_offsetLocation, 0.0f);
    glDrawArrays(GL_TRIANGLES, 0, WaveformColumnCache::kAxisVertices);

    m_pProgram->setUniformValue(m_axisModeLocation, 0.0f);
    m_pProgram->setUniformValue(m_offsetLocation, m_uniforms.offsetX);
    for (const auto& range : ranges) {
        glDrawArrays(GL_TRIANGLES, range.firstVertex, range.vertexCount);
    }

    glDisableVertexAttribArray(static_cast<GLuint>(m_positionLocation));
    glDisableVertexAttribArray(static_cast<GLuint>(m_bandsLocation));
    // The other rendergraph nodes draw from client memory, which requires no
    // buffer object to be bound.
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_pProgram->release();
}

} // namespace allshader
