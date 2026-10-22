#pragma once

#include <QMatrix4x4>
#include <memory>
#include <vector>

namespace rendergraph {
class Engine;
class BaseNode;
} // namespace rendergraph

class rendergraph::Engine {
  public:
    /// Optional CPU-time probe (Deck Lab perf log). Receives each node and
    /// the nanoseconds its preprocess() or render() took, children excluded.
    /// Without a probe nothing is timed.
    class Probe {
      public:
        virtual ~Probe() = default;
        virtual void record(BaseNode* pNode, qint64 nanos) = 0;
    };

    Engine(std::unique_ptr<BaseNode> pRootNode);
    ~Engine();

    void render();
    void resize(int w, int h);
    void preprocess();
    void add(BaseNode* pNode);
    void remove(BaseNode* pNode);
    const QMatrix4x4& matrix() const {
        return m_matrix;
    }
    void setProbe(Probe* pProbe) {
        m_pProbe = pProbe;
    }

  private:
    void render(BaseNode* pNode);
    void resize(BaseNode* pNode, int, int);

    Probe* m_pProbe{};
    QMatrix4x4 m_matrix;
    std::unique_ptr<BaseNode> m_pRootNode;
    std::vector<BaseNode*> m_pPreprocessNodes;
    std::vector<BaseNode*> m_pInitializeNodes;
};
