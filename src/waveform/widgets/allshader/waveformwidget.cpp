
#include "waveform/widgets/allshader/waveformwidget.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QWheelEvent>

#include "waveform/renderers/allshader/waveformrenderbackground.h"
#include "waveform/renderers/allshader/waveformrenderbeat.h"
#include "waveform/renderers/allshader/waveformrendererendoftrack.h"
#include "waveform/renderers/allshader/waveformrendererfiltered.h"
#include "waveform/renderers/allshader/waveformrendererhsv.h"
#include "waveform/renderers/allshader/waveformrendererpreroll.h"
#include "waveform/renderers/allshader/waveformrendererrgb.h"
#include "waveform/renderers/allshader/waveformrendererrgbcached.h"
#include "waveform/renderers/allshader/waveformrenderersimple.h"
#include "waveform/renderers/allshader/waveformrendererslipmode.h"
#include "waveform/renderers/allshader/waveformrendererstem.h"
#include "waveform/renderers/allshader/waveformrenderertextured.h"
#include "waveform/renderers/allshader/waveformrendermark.h"
#include "waveform/renderers/allshader/waveformrendermarkrange.h"
#include "waveform/rendergate.h"
#include "waveform/waveformwidgetfactory.h"
#include "waveform/widgets/allshader/moc_waveformwidget.cpp"

namespace allshader {

WaveformWidget::WaveformWidget(QWidget* parent,
        WaveformWidgetType::Type type,
        const QString& group,
        WaveformRendererSignalBase::Options options)
        : WGLWidget(parent),
          WaveformWidgetAbstract(group),
          m_type(type),
          m_pWaveformRenderMarkSlip(nullptr),
          m_pWaveformRendererSignal(nullptr) {
    // Deck Lab perf log: attribute the CPU time of every rendergraph node to
    // the renderer it belongs to. Nothing is timed without DECKLAB_PERF_LOG.
    if (mixxx::decklab::perfLogEnabledFromEnvironment()) {
        m_pTimings = std::make_unique<RendererTimings>();
    }
    const auto track = [this](rendergraph::BaseNode* pNode, const char* name) {
        if (m_pTimings) {
            m_nodeCategories.append(NodeCategory{pNode, m_pTimings->addCategory(name)});
        }
    };

    auto pTopNode = std::make_unique<rendergraph::Node>();
    auto pOpacityNode = std::make_unique<rendergraph::OpacityNode>();

    {
        auto pNode = addRendererNode<WaveformRenderBackground>();
        track(pNode.get(), "background");
        pTopNode->appendChildNode(std::move(pNode));
    }
    auto pEndOfTrackRenderer = addRendererNode<WaveformRendererEndOfTrack>();
    pEndOfTrackRenderer->setEndOfTrackWarningTime(
            WaveformWidgetFactory::instance()->getEndOfTrackWarningTime());
    track(pEndOfTrackRenderer.get(), "endoftrack");
    pOpacityNode->appendChildNode(std::move(pEndOfTrackRenderer));
    {
        auto pNode = addRendererNode<WaveformRendererPreroll>();
        track(pNode.get(), "preroll");
        pOpacityNode->appendChildNode(std::move(pNode));
    }
    {
        auto pNode = addRendererNode<WaveformRenderMarkRange>();
        track(pNode.get(), "markrange");
        m_pWaveformRenderMarkRange = pOpacityNode->appendChildNode(std::move(pNode));
    }

#ifdef __STEM__
    // The following two renderers work in tandem: if the rendered waveform is
    // for a stem track, WaveformRendererSignalBase will skip rendering and let
    // WaveformRendererStem do the rendering, and vice-versa.
    {
        auto pNode = addRendererNode<WaveformRendererStem>();
        track(pNode.get(), "stem");
        pOpacityNode->appendChildNode(std::move(pNode));
    }
#endif
    std::unique_ptr<WaveformRendererSignalBase> pWaveformRendererSignal = addWaveformSignalRenderer(
            type, options, ::WaveformRendererAbstract::Play);
    m_pWaveformRendererSignal = pWaveformRendererSignal.get();
    if (pWaveformRendererSignal) {
        auto* pNode = dynamic_cast<rendergraph::BaseNode*>(pWaveformRendererSignal.release());
        DEBUG_ASSERT(pNode);
        track(pNode, "signal");
        pOpacityNode->appendChildNode(std::unique_ptr<rendergraph::BaseNode>(pNode));
    }
    {
        auto pNode = addRendererNode<WaveformRenderBeat>();
        track(pNode.get(), "beat");
        pOpacityNode->appendChildNode(std::move(pNode));
    }
    {
        auto pNode = addRendererNode<WaveformRenderMark>();
        track(pNode.get(), "mark");
        m_pWaveformRenderMark = pOpacityNode->appendChildNode(std::move(pNode));
    }

    // if the added signal renderer supports slip, we add it again, now for
    // slip, together with the other slip renderers
    if (m_pWaveformRendererSignal && m_pWaveformRendererSignal->supportsSlip()) {
        // The following renderer will add an overlay waveform if a slip is in progress
        {
            auto pNode = addRendererNode<WaveformRendererSlipMode>();
            track(pNode.get(), "slip");
            pOpacityNode->appendChildNode(std::move(pNode));
        }
        {
            auto pNode = addRendererNode<WaveformRendererPreroll>(
                    ::WaveformRendererAbstract::Slip);
            track(pNode.get(), "slip");
            pOpacityNode->appendChildNode(std::move(pNode));
        }
#ifdef __STEM__
        {
            auto pNode = addRendererNode<WaveformRendererStem>(
                    ::WaveformRendererAbstract::Slip);
            track(pNode.get(), "slip");
            pOpacityNode->appendChildNode(std::move(pNode));
        }
#endif
        std::unique_ptr<WaveformRendererSignalBase> pSlipNode = addWaveformSignalRenderer(
                type, options, ::WaveformRendererAbstract::Slip);
        m_pWaveformRendererSlipSignal = pSlipNode.get();
        auto* pNode = dynamic_cast<rendergraph::BaseNode*>(pSlipNode.release());
        DEBUG_ASSERT(pNode);
        track(pNode, "slip");
        pOpacityNode->appendChildNode(std::unique_ptr<rendergraph::BaseNode>(pNode));
        {
            auto pBeatNode = addRendererNode<WaveformRenderBeat>(
                    ::WaveformRendererAbstract::Slip);
            track(pBeatNode.get(), "slip");
            pOpacityNode->appendChildNode(std::move(pBeatNode));
        }
        {
            auto pMarkNode = addRendererNode<WaveformRenderMark>(
                    ::WaveformRendererAbstract::Slip);
            track(pMarkNode.get(), "slip");
            m_pWaveformRenderMarkSlip = pOpacityNode->appendChildNode(std::move(pMarkNode));
        }
    }

    m_initSuccess = init();

    m_pOpacityNode = pTopNode->appendChildNode(std::move(pOpacityNode));

    m_pEngine = std::make_unique<rendergraph::Engine>(std::move(pTopNode));
    if (m_pTimings) {
        m_markCategory = m_pTimings->addCategory("mark");
        m_markRangeCategory = m_pTimings->addCategory("markrange");
        m_slipCategory = m_pTimings->addCategory("slip");
        m_otherCategory = m_pTimings->addCategory("other");
        m_pProbe = std::make_unique<NodeProbe>(this);
        m_pEngine->setProbe(m_pProbe.get());
    }
}

WaveformWidget::~WaveformWidget() {
    makeCurrentIfNeeded();
    m_rendererStack.clear();
    // destruction of nodes needs to happen within the opengl context
    m_pEngine.reset();
    doneCurrent();
}

std::unique_ptr<WaveformRendererSignalBase>
WaveformWidget::addWaveformSignalRenderer(WaveformWidgetType::Type type,
        WaveformRendererSignalBase::Options options,
        ::WaveformRendererAbstract::PositionSource positionSource) {
#ifndef QT_OPENGL_ES_2
    if (options & WaveformRendererSignalBase::Option::HighDetail) {
        switch (type) {
        case ::WaveformWidgetType::RGB:
        case ::WaveformWidgetType::Filtered:
        case ::WaveformWidgetType::Stacked:
            return addWaveformSignalRenderer<WaveformRendererTextured>(
                    type, positionSource, options);
        default:
            break;
        }
    }
#endif

    switch (type) {
    case ::WaveformWidgetType::Simple:
        return addWaveformSignalRenderer<WaveformRendererSimple>();
    case ::WaveformWidgetType::RGB:
        // Deck Lab (pi4-perf): geometry built once per chunk of columns
        // instead of every frame. DECKLAB_WAVEFORM_CACHE=0 selects upstream.
        if (mixxx::decklab::waveformCacheEnabledFromEnvironment()) {
            return addWaveformSignalRenderer<WaveformRendererRGBCached>(positionSource, options);
        }
        return addWaveformSignalRenderer<WaveformRendererRGB>(positionSource, options);
    case ::WaveformWidgetType::HSV:
        return addWaveformSignalRenderer<WaveformRendererHSV>();
    case ::WaveformWidgetType::Filtered:
        return addWaveformSignalRenderer<WaveformRendererFiltered>(false);
    case ::WaveformWidgetType::Stacked:
        return addWaveformSignalRenderer<WaveformRendererFiltered>(
                true); // true for RGB Stacked
    default:
        break;
    }
    return nullptr;
}

mixxx::Duration WaveformWidget::render() {
    makeCurrentIfNeeded();
    paintGL();
    doneCurrent();
    // In the legacy widgets, this is used to "return timer for painter setup"
    // which is not relevant here. Also note that the return value is not used
    // at all, so it might be better to remove it everywhere. In the meantime.
    // we need to return something for API compatibility.
    return mixxx::Duration();
}

void WaveformWidget::paintGL() {
    // opacity of 0.f effectively skips the subtree rendering
    m_pOpacityNode->setOpacity(shouldOnlyDrawBackground() ? 0.f : 1.f);

    if (m_pTimings) {
        paintGLTimed();
        return;
    }

    m_pWaveformRenderMark->update();
    m_pWaveformRenderMarkRange->update();
    if (m_pWaveformRenderMarkSlip) {
        m_pWaveformRenderMarkSlip->update();
    }

    m_pEngine->preprocess();
    m_pEngine->render();
}

void WaveformWidget::paintGLTimed() {
    // Same work as paintGL(), timed per renderer (DECKLAB_PERF_LOG=1).
    m_pTimings->beginFrame();
    QElapsedTimer timer;
    timer.start();
    m_pWaveformRenderMark->update();
    m_pTimings->add(m_markCategory, timer.nsecsElapsed());
    timer.start();
    m_pWaveformRenderMarkRange->update();
    m_pTimings->add(m_markRangeCategory, timer.nsecsElapsed());
    if (m_pWaveformRenderMarkSlip) {
        timer.start();
        m_pWaveformRenderMarkSlip->update();
        m_pTimings->add(m_slipCategory, timer.nsecsElapsed());
    }

    m_pEngine->preprocess();
    m_pEngine->render();
    m_pTimings->endFrame();
}

void WaveformWidget::NodeProbe::record(rendergraph::BaseNode* pNode, qint64 nanos) {
    // Child nodes (mark labels, ranges) count for the renderer they belong to.
    for (const rendergraph::BaseNode* pAncestor = pNode; pAncestor;
            pAncestor = pAncestor->parent()) {
        for (const auto& entry : std::as_const(m_pOwner->m_nodeCategories)) {
            if (entry.pNode == pAncestor) {
                m_pOwner->m_pTimings->add(entry.category, nanos);
                return;
            }
        }
    }
    m_pOwner->m_pTimings->add(m_pOwner->m_otherCategory, nanos);
}

QString WaveformWidget::takePerfReport() {
    if (!m_pTimings) {
        return QString();
    }
    QString report = QStringLiteral("frames=%1 %2")
                             .arg(QString::number(m_pTimings->frames()),
                                     m_pTimings->report());
    m_pTimings->reset();
    WaveformColumnCache::Stats cache;
    bool cached = false;
    for (auto* pSignal : {m_pWaveformRendererSignal, m_pWaveformRendererSlipSignal}) {
        auto* pCached = dynamic_cast<WaveformRendererRGBCached*>(pSignal);
        if (!pCached) {
            continue;
        }
        cached = true;
        const WaveformColumnCache::Stats stats = pCached->takeCacheStats();
        cache.frames += stats.frames;
        cache.resets += stats.resets;
        cache.chunksBuilt += stats.chunksBuilt;
        cache.columnsBuilt += stats.columnsBuilt;
        cache.bytesDirty += stats.bytesDirty;
    }
    if (cached) {
        report += QStringLiteral(" cache: frames=%1 resets=%2 chunks=%3 columns=%4 upload=%5KiB")
                          .arg(QString::number(cache.frames),
                                  QString::number(cache.resets),
                                  QString::number(cache.chunksBuilt),
                                  QString::number(cache.columnsBuilt),
                                  QString::number(cache.bytesDirty / 1024));
    } else {
        report += QStringLiteral(" cache: off");
    }
    return report;
}

void WaveformWidget::castToQWidget() {
    m_widget = this;
}

void WaveformWidget::initializeGL() {
}

void WaveformWidget::resizeRenderer(int, int, float) {
    // This is called when the widget is resized, but as this is a WGLWidget, we
    // also get the resizeGL call and use that instead, as it has the opengl
    // context set.
}

void WaveformWidget::resizeGL(int w, int h) {
    w = static_cast<int>(std::lround(static_cast<qreal>(w) / devicePixelRatioF()));
    h = static_cast<int>(std::lround(static_cast<qreal>(h) / devicePixelRatioF()));

    // Many allshader components relies on WaveformWidgetRenderer::getWidth and
    // WaveformWidgetRenderer::getHeight to update their rendering stack, so we
    // must resize the renderer first, before updating the rendergraph
    WaveformWidgetRenderer::resizeRenderer(w, h, static_cast<float>(devicePixelRatio()));
    m_pEngine->resize(w, h);
}

void WaveformWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
}

void WaveformWidget::wheelEvent(QWheelEvent* pEvent) {
    QApplication::sendEvent(parentWidget(), pEvent);
    pEvent->accept();
}

void WaveformWidget::leaveEvent(QEvent* pEvent) {
    QApplication::sendEvent(parentWidget(), pEvent);
    pEvent->accept();
}

/* static */
WaveformRendererSignalBase::Options WaveformWidget::supportedOptions(
        WaveformWidgetType::Type type, bool useGles) {
    WaveformRendererSignalBase::Options options = WaveformRendererSignalBase::Option::None;
    switch (type) {
    case WaveformWidgetType::Type::RGB:
        options = WaveformRendererSignalBase::Option::AllOptionsCombined;
        break;
    case WaveformWidgetType::Type::Filtered:
        options = WaveformRendererSignalBase::Option::HighDetail;
        break;
    case WaveformWidgetType::Type::Stacked:
        options = WaveformRendererSignalBase::Option::HighDetail;
        break;
    default:
        break;
    }
    if (useGles) {
        // High detail (textured) waveforms are not supported on OpenGL ES.
        // See https://github.com/mixxxdj/mixxx/issues/13385
        options &= ~WaveformRendererSignalBase::Options(
                WaveformRendererSignalBase::Option::HighDetail);
    }
    return options;
}

/* static */
WaveformWidgetVars WaveformWidget::vars() {
    WaveformWidgetVars result;
    result.m_useGL = true;
    result.m_useGLES = true;
    result.m_useGLSL = true;
    result.m_category = WaveformWidgetCategory::AllShader;
    return result;
}

} // namespace allshader
