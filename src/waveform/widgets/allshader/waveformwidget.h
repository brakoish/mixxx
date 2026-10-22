#pragma once

#include <QVarLengthArray>
#include <memory>

#include "rendergraph/engine.h"
#include "rendergraph/opacitynode.h"
#include "waveform/rendergate.h"
#include "waveform/renderers/allshader/waveformrenderersignalbase.h"
#include "waveform/widgets/waveformwidgetabstract.h"
#include "waveform/widgets/waveformwidgetvars.h"
#include "widget/wglwidget.h"

namespace allshader {
class WaveformWidget;
class WaveformRenderMark;
class WaveformRenderMarkRange;
}

class allshader::WaveformWidget final : public ::WGLWidget,
                                        public ::WaveformWidgetAbstract {
    Q_OBJECT
  public:
    explicit WaveformWidget(QWidget* parent,
            WaveformWidgetType::Type type,
            const QString& group,
            WaveformRendererSignalBase::Options options);
    ~WaveformWidget() override;

    WaveformWidgetType::Type getType() const override {
        return m_type;
    }

    void resizeRenderer(int width, int height, float devicePixelRatio) override;

    // override for WaveformWidgetAbstract
    mixxx::Duration render() override;

    // overrides for WGLWidget
    void paintGL() override;
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    WGLWidget* getGLWidget() override {
        return this;
    }
    static WaveformWidgetVars vars();
    static WaveformRendererSignalBase::Options supportedOptions(
            WaveformWidgetType::Type type, bool useGles);

    QString takePerfReport() override;

  private:
    class NodeProbe : public rendergraph::Engine::Probe {
      public:
        explicit NodeProbe(WaveformWidget* pOwner)
                : m_pOwner(pOwner) {
        }
        void record(rendergraph::BaseNode* pNode, qint64 nanos) override;

      private:
        WaveformWidget* m_pOwner;
    };
    struct NodeCategory {
        const rendergraph::BaseNode* pNode;
        int category;
    };

    void paintGLTimed();

    void castToQWidget() override;
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void leaveEvent(QEvent* event) override;

    template<class T_Renderer, typename... Args>
    inline std::unique_ptr<T_Renderer> addRendererNode(Args&&... args) {
        return std::unique_ptr<T_Renderer>(addRenderer<T_Renderer>(std::forward<Args>(args)...));
    }

    template<class T_Renderer, typename... Args>
    inline std::unique_ptr<T_Renderer> addWaveformSignalRenderer(Args&&... args) {
        auto pRenderer = addRenderer<T_Renderer>(std::forward<Args>(args)...);
        return std::unique_ptr<T_Renderer>(pRenderer);
    }

    std::unique_ptr<allshader::WaveformRendererSignalBase> addWaveformSignalRenderer(
            WaveformWidgetType::Type type,
            WaveformRendererSignalBase::Options options,
            ::WaveformRendererAbstract::PositionSource positionSource);

    WaveformWidgetType::Type m_type;
    std::unique_ptr<rendergraph::Engine> m_pEngine;
    rendergraph::OpacityNode* m_pOpacityNode;
    WaveformRenderMark* m_pWaveformRenderMark;
    WaveformRenderMarkRange* m_pWaveformRenderMarkRange;
    WaveformRenderMark* m_pWaveformRenderMarkSlip;

    WaveformRendererSignalBase* m_pWaveformRendererSignal;
    WaveformRendererSignalBase* m_pWaveformRendererSlipSignal{};

    // Deck Lab perf log (DECKLAB_PERF_LOG=1 only).
    std::unique_ptr<RendererTimings> m_pTimings;
    std::unique_ptr<NodeProbe> m_pProbe;
    QVarLengthArray<NodeCategory, 16> m_nodeCategories;
    int m_markCategory{-1};
    int m_markRangeCategory{-1};
    int m_slipCategory{-1};
    int m_otherCategory{-1};

    DISALLOW_COPY_AND_ASSIGN(WaveformWidget);
};
