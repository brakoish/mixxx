#pragma once

#include <QElapsedTimer>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGNode>
#include <QSGSimpleRectNode>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <vector>

#include "control/controlproxy.h"
#include "qml/qmlplayerproxy.h"
#include "qml/qmlwaveformrenderer.h"
#include "track/track.h"
#include "util/performancetimer.h"
#include "waveform/isynctimeprovider.h"
#include "waveform/renderers/waveformwidgetrenderer.h"

class WaveformRendererAbstract;

namespace allshader {
class WaveformWidget;
class WaveformRenderMark;
class WaveformRenderMarkRange;
} // namespace allshader
namespace rendergraph {
class Node;
class OpacityNode;
class TreeNode;
} // namespace rendergraph

namespace mixxx {
namespace qml {

class QmlPlayerProxy;

class QmlWaveformDisplay : public QQuickItem, VSyncTimeProvider, public WaveformWidgetRenderer {
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    Q_PROPERTY(QmlPlayerProxy* player READ getPlayer WRITE setPlayer
                    NOTIFY playerChanged REQUIRED)
    Q_PROPERTY(QString group READ getGroup WRITE setGroup NOTIFY groupChanged REQUIRED)
    Q_PROPERTY(QQmlListProperty<mixxx::qml::QmlWaveformRendererFactory> renderers READ renderers)
    Q_PROPERTY(double zoom READ getZoom WRITE setZoom NOTIFY zoomChanged)
    Q_PROPERTY(QColor backgroundColor READ getBackgroundColor WRITE
                    setBackgroundColor NOTIFY backgroundColorChanged)
    Q_CLASSINFO("DefaultProperty", "renderers")
    QML_NAMED_ELEMENT(WaveformDisplay)

  public:
    QmlWaveformDisplay(QQuickItem* parent = nullptr);
    ~QmlWaveformDisplay() override;

    void setPlayer(QmlPlayerProxy* player);
    QmlPlayerProxy* getPlayer() const;

    QColor getBackgroundColor() const {
        return m_backgroundColor;
    }
    void setBackgroundColor(QColor color) {
        m_backgroundColor = color;
        m_dirtyFlag.setFlag(DirtyFlag::Background, true);
        emit backgroundColorChanged();
    }

    void setGroup(const QString& group) override;
    void setZoom(double zoom) {
        WaveformWidgetRenderer::setZoom(zoom);
        // The render gate does not poll the zoom.
        requestRender();
        update();
        emit zoomChanged();
    }

    std::chrono::microseconds fromTimerToNextSync(const PerformanceTimer& timer) override;
    std::chrono::microseconds getSyncInterval() const override {
        return m_syncInterval;
    }

    void componentComplete() override;

    QQmlListProperty<mixxx::qml::QmlWaveformRendererFactory> renderers();

    /// Deck Lab touch scratch: interleaved audio samples per logical pixel
    /// at the current zoom (valid after the first rendered frame).
    Q_INVOKABLE double audioSamplesPerPixel() const {
        return getAudioSamplePerPixel();
    }

    /// Deck Lab render gate (gpu-screen spike). Upstream redraws every frame
    /// forever; with the gate a paused, unchanged waveform requests no frames.
    /// DECKLAB_RENDER_GATE=0 restores upstream behaviour.
    static bool renderGateEnabled();
    quint64 renderCount() const {
        return m_renderCount.load(std::memory_order_relaxed);
    }

  protected:
    QSGNode* updatePaintNode(QSGNode* old, QQuickItem::UpdatePaintNodeData*) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;
  private slots:
    void slotTrackLoaded(TrackPointer pLoadedTrack);
    void slotTrackLoading(TrackPointer pNewTrack, TrackPointer pOldTrack);
    void slotTrackUnloaded();
    void slotWaveformUpdated();

    void slotFrameSwapped();
    void slotWindowChanged(QQuickWindow* window);
    void slotGateTick();
    void slotGateControlChanged();
  signals:
    void playerChanged();
    void zoomChanged();
    void groupChanged(const QString& group);
    void backgroundColorChanged();

  private:
    void setCurrentTrack(TrackPointer pTrack);
    void connectGateControls();
    void requestFrame();

    // Properties
    QPointer<QmlPlayerProxy> m_pPlayer;
    QColor m_backgroundColor{QColor(0, 0, 0, 255)};

    PerformanceTimer m_timer;

    std::chrono::milliseconds m_syncInterval;
    enum class DirtyFlag : int {
        None = 0x0,
        Geometry = 0x1,
        Window = 0x2,
        Background = 0x4,
    };
    Q_DECLARE_FLAGS(DirtyFlags, DirtyFlag)

    DirtyFlags m_dirtyFlag{DirtyFlag::None};
    QList<QmlWaveformRendererFactory*> m_waveformRenderers;

    // Render gate. Written in updatePaintNode() (scene graph sync, GUI thread
    // blocked), read on the GUI thread.
    const bool m_gateEnabled;
    QTimer m_gateTimer;
    QElapsedTimer m_sinceFrame;
    std::atomic<bool> m_lastFrameMoving{true};
    std::atomic<quint64> m_renderCount{0};
    qint64 m_lastPixel{std::numeric_limits<qint64>::min()};
    std::vector<std::unique_ptr<ControlProxy>> m_gateControls;
};

} // namespace qml
} // namespace mixxx
