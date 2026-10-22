#include "qml/qmlwaveformdisplay.h"

#include <qnamespace.h>

#include <QQuickWindow>
#include <QScreen>
#include <QSGFlatColorMaterial>
#include <QSGSimpleRectNode>
#include <QSGVertexColorMaterial>
#include <QtQuick/QSGGeometryNode>
#include <QtQuick/QSGMaterial>
#include <QtQuick/QSGRectangleNode>
#include <QtQuick/QSGTexture>
#include <QtQuick/QSGTextureProvider>
#include <cmath>

#include "mixer/basetrackplayer.h"
#include "moc_qmlwaveformdisplay.cpp"
#include "qml/qmlplayerproxy.h"
#include "rendergraph/context.h"
#include "rendergraph/node.h"
#include "util/assert.h"
#include "waveform/visualplayposition.h"

using namespace allshader;

namespace {
constexpr int kDefaultSyncInternalMs = 100;
// Render gate: how often a paused waveform checks for motion (cheap atomic
// reads, no frame), and the longest it goes without a frame while visible.
constexpr int kGateTickMs = 50;
constexpr qint64 kGateSafetyMs = 1000;
// Controls that change what a paused waveform shows without moving it.
// Hot cues, beats and waveform data arrive through the track's signals.
const char* const kGateControls[] = {
        "loop_start_position",
        "loop_end_position",
        "loop_enabled",
        "cue_point",
        "intro_start_position",
        "intro_end_position",
        "outro_start_position",
        "outro_end_position",
        "rate_ratio",
        "track_loaded",
        "end_of_track",
        "passthrough",
};
} // namespace

namespace mixxx {
namespace qml {

QmlWaveformDisplay::QmlWaveformDisplay(QQuickItem* parent)
        : QQuickItem(parent),
          WaveformWidgetRenderer(),
          m_syncInterval(kDefaultSyncInternalMs),
          m_pPlayer(nullptr),
          m_gateEnabled(renderGateEnabled()) {
    setFlag(QQuickItem::ItemHasContents, true);

    m_sinceFrame.start();
    m_gateTimer.setInterval(kGateTickMs);
    connect(&m_gateTimer, &QTimer::timeout, this, &QmlWaveformDisplay::slotGateTick);

    connect(this,
            &QmlWaveformDisplay::windowChanged,
            this,
            &QmlWaveformDisplay::slotWindowChanged,
            Qt::DirectConnection);
    slotWindowChanged(window());
}

QmlWaveformDisplay::~QmlWaveformDisplay() {
    // The stack contains references to Renderer that are owned and cleared by a BaseNode
    m_rendererStack.clear();
}

void QmlWaveformDisplay::componentComplete() {
    qDebug() << "QmlWaveformDisplay ready for group" << getGroup() << "with"
             << m_waveformRenderers.count() << "renderer(s)"
             << (m_gateEnabled ? "render gate on" : "render gate off");
    QQuickItem::componentComplete();
    connectGateControls();
    if (m_gateEnabled) {
        m_gateTimer.start();
    }
}

// static
bool QmlWaveformDisplay::renderGateEnabled() {
    return qEnvironmentVariable("DECKLAB_RENDER_GATE") != QStringLiteral("0");
}

void QmlWaveformDisplay::connectGateControls() {
    m_gateControls.clear();
    if (!m_gateEnabled || getGroup().isEmpty()) {
        return;
    }
    for (const char* key : kGateControls) {
        auto pControl = std::make_unique<ControlProxy>(getGroup(),
                QString::fromLatin1(key),
                this,
                ControlFlag::NoWarnIfMissing);
        pControl->connectValueChanged(this, &QmlWaveformDisplay::slotGateControlChanged);
        m_gateControls.push_back(std::move(pControl));
    }
}

void QmlWaveformDisplay::slotGateControlChanged() {
    requestRender();
    requestFrame();
}

void QmlWaveformDisplay::requestFrame() {
    update();
}

void QmlWaveformDisplay::slotGateTick() {
    if (!isVisible() || !window()) {
        return;
    }
    bool frame = takeRenderRequest();
    // Motion check without rendering: the visual play position is lock free.
    if (m_visualPlayPosition) {
        double playPosition = 0.0;
        double slipPosition = 0.0;
        double motionRate = 0.0;
        m_visualPlayPosition->getPlaySlipAtNextVSync(
                this, &playPosition, &slipPosition, &motionRate);
        if (motionRate != 0.0) {
            frame = true;
        }
        // A seek or cue jump while paused moves the waveform by whole pixels.
        const qint64 pixel = m_trackPixelCount > 0
                ? static_cast<qint64>(std::llround(playPosition * m_trackPixelCount))
                : 0;
        if (pixel != m_lastPixel) {
            m_lastPixel = pixel;
            frame = true;
        }
    }
    if (m_sinceFrame.elapsed() >= kGateSafetyMs) {
        frame = true;
    }
    if (frame) {
        requestFrame();
    }
}

void QmlWaveformDisplay::slotWindowChanged(QQuickWindow* window) {
    m_rendererStack.clear();

    m_dirtyFlag.setFlag(DirtyFlag::Window, true);
    if (window) {
        connect(window, &QQuickWindow::afterFrameEnd, this, &QmlWaveformDisplay::slotFrameSwapped);
        // Predict the play position for the next refresh, not 100 ms ahead:
        // the scrolling waveform must sit on the beat the DJ hears.
        const QScreen* pScreen = window->screen();
        const qreal refreshRate = pScreen ? pScreen->refreshRate() : 0.0;
        if (refreshRate >= 20.0 && refreshRate <= 240.0) {
            m_syncInterval = std::chrono::milliseconds(
                    static_cast<int>(std::lround(1000.0 / refreshRate)));
        }
    }
    m_timer.restart();
}

std::chrono::microseconds QmlWaveformDisplay::fromTimerToNextSync(const PerformanceTimer& timer) {
    // TODO @m0dB probably better to use a singleton instead of deriving QmlWaveformDisplay from
    // ISyncTimeProvider and have each keep track of this.
    return m_syncInterval + std::chrono::microseconds(m_timer.difference(timer).toIntegerMicros());
}

void QmlWaveformDisplay::slotFrameSwapped() {
    m_timer.restart();

    if (!m_gateEnabled) {
        // Upstream: continuous redraw
        update();
        return;
    }
    // Keep drawing every frame only while the deck moves or a renderer
    // animates (end-of-track blink). Otherwise the gate tick decides.
    if (m_lastFrameMoving.load(std::memory_order_relaxed)) {
        update();
    }
}

void QmlWaveformDisplay::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    m_dirtyFlag.setFlag(DirtyFlag::Geometry, true);
    update();
    QQuickItem::geometryChange(newGeometry, oldGeometry);
}

QSGNode* QmlWaveformDisplay::updatePaintNode(QSGNode* node, UpdatePaintNodeData*) {
    if (m_dirtyFlag.testFlag(DirtyFlag::Window)) {
        delete node;
        m_dirtyFlag.setFlag(DirtyFlag::Window, false);
    }

    DEBUG_ASSERT(!node || node->childCount() == 1);

    auto* pClipNode = dynamic_cast<QSGClipNode*>(node);
    auto* pBgNode = pClipNode ? dynamic_cast<QSGSimpleRectNode*>(pClipNode->lastChild()) : nullptr;

    if (!pBgNode) {
        if (pClipNode) {
            delete pClipNode;
        }
        pClipNode = new QSGClipNode();
        pBgNode = new QSGSimpleRectNode();
        m_dirtyFlag.setFlag(DirtyFlag::Background, true);
        m_dirtyFlag.setFlag(DirtyFlag::Geometry, true);

        pClipNode->setIsRectangular(true);
        pClipNode->setGeometry(pBgNode->geometry());

        pClipNode->setClipRect(boundingRect());
        pBgNode->setRect(boundingRect());

        if (getContext()) {
            delete getContext();
        }
        setContext(new rendergraph::Context(window()));
        rendergraph::Node* pTopNode = new rendergraph::Node;

        m_rendererStack.clear();
        for (auto* pQmlRenderer : std::as_const(m_waveformRenderers)) {
            if (!pQmlRenderer->isSupported()) {
                qDebug() << "Ignoring the unsupported" << pQmlRenderer << "renderer";
            }
            auto renderer = pQmlRenderer->create(this);
#ifdef __STEM__
            VERIFY_OR_DEBUG_ASSERT(renderer.renderer) {
#else
            // It is expected for the stem renderer to return a null value in
            // case STEM are not enabled.
            if (!renderer.renderer) {
#endif
                continue;
            }
            addRenderer(renderer.renderer);
            pTopNode->appendChildNode(std::move(renderer.node));
        }

        pBgNode->appendChildNode(pTopNode);
        pClipNode->appendChildNode(pBgNode);
        init();
    }

    if (m_dirtyFlag.testFlag(DirtyFlag::Background)) {
        m_dirtyFlag.setFlag(DirtyFlag::Background, false);
        pBgNode->setColor(m_backgroundColor);
    }

    if (m_dirtyFlag.testFlag(DirtyFlag::Geometry)) {
        m_dirtyFlag.setFlag(DirtyFlag::Geometry, false);
        pBgNode->setRect(boundingRect());
        pClipNode->setClipRect(boundingRect());
        resizeRenderer(boundingRect().width(),
                boundingRect().height(),
                window()->devicePixelRatio());
    }

    onPreRender(this);

    for (auto* pRenderer : std::as_const(m_rendererStack)) {
        pRenderer->update();
    }

    m_lastFrameMoving.store(needsContinuousRender(), std::memory_order_relaxed);
    m_renderCount.fetch_add(1, std::memory_order_relaxed);
    m_sinceFrame.restart();

    pBgNode->markDirty(QSGNode::DirtyForceUpdate);
    return pClipNode;
}

QmlPlayerProxy* QmlWaveformDisplay::getPlayer() const {
    return m_pPlayer;
}

void QmlWaveformDisplay::setPlayer(QmlPlayerProxy* pPlayer) {
    if (m_pPlayer == pPlayer) {
        return;
    }

    if (m_pPlayer != nullptr) {
        m_pPlayer->internalTrackPlayer()->disconnect(this);
    }

    m_pPlayer = pPlayer;

    if (m_pPlayer != nullptr) {
        setCurrentTrack(m_pPlayer->internalTrackPlayer()->getLoadedTrack());
        connect(m_pPlayer->internalTrackPlayer(),
                &BaseTrackPlayer::newTrackLoaded,
                this,
                &QmlWaveformDisplay::slotTrackLoaded);
        connect(m_pPlayer->internalTrackPlayer(),
                &BaseTrackPlayer::loadingTrack,
                this,
                &QmlWaveformDisplay::slotTrackLoading);
        connect(m_pPlayer->internalTrackPlayer(),
                &BaseTrackPlayer::playerEmpty,
                this,
                &QmlWaveformDisplay::slotTrackUnloaded);
    }

    emit playerChanged();
    update();
}

void QmlWaveformDisplay::setGroup(const QString& group) {
    if (getGroup() == group) {
        return;
    }

    WaveformWidgetRenderer::setGroup(group);
    if (isComponentComplete()) {
        connectGateControls();
    }
    emit groupChanged(group);
}

void QmlWaveformDisplay::slotTrackLoaded(TrackPointer pTrack) {
    // TODO: Investigate if it's a bug that this debug assertion fails when
    // passing tracks on the command line
    // DEBUG_ASSERT(m_pCurrentTrack == pTrack);
    setCurrentTrack(pTrack);
}

void QmlWaveformDisplay::slotTrackLoading(TrackPointer pNewTrack, TrackPointer pOldTrack) {
    Q_UNUSED(pOldTrack); // only used in DEBUG_ASSERT
    DEBUG_ASSERT(getTrackInfo() == pOldTrack);
    setCurrentTrack({});
}

void QmlWaveformDisplay::slotTrackUnloaded() {
    setCurrentTrack(nullptr);
}

void QmlWaveformDisplay::setCurrentTrack(TrackPointer pTrack) {
    auto pCurrentTrack = getTrackInfo();
    // TODO: Check if this is actually possible
    if (pCurrentTrack == pTrack) {
        return;
    }

    if (pCurrentTrack != nullptr) {
        disconnect(pCurrentTrack.get(), nullptr, this, nullptr);
    }

    setTrack(pTrack);
    if (pTrack != nullptr) {
        connect(pTrack.get(),
                &Track::waveformSummaryUpdated,
                this,
                &QmlWaveformDisplay::slotWaveformUpdated);
    }
    slotWaveformUpdated();
}

void QmlWaveformDisplay::slotWaveformUpdated() {
    requestRender();
    update();
}

QQmlListProperty<QmlWaveformRendererFactory> QmlWaveformDisplay::renderers() {
    return {this, &m_waveformRenderers};
}

} // namespace qml
} // namespace mixxx
