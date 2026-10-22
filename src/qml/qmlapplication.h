#pragma once

#include <QApplication>
#include <QQmlApplicationEngine>
#include <QStringList>

#include "coreservices.h"
#include "qmlautoreload.h"

class GuiTick;
class VisualsManager;

namespace mixxx {
namespace qml {

class QmlApplication : public QObject {
    Q_OBJECT
  public:
    QmlApplication(
            QApplication* app,
            const CmdlineArgs& args);
    ~QmlApplication() override;

  public slots:
    void loadQml(const QString& path);

  private:
    // Deck Lab gpu-screen spike (all opt-in through the environment):
    // DECKLAB_PERF_LOG=1 logs frames per second and waveform renders every
    // 10 s; DECKLAB_QML_SMOKE_DIR runs the CI screen capture and quits.
    void installDeckLabPerfLog();
    void runDeckLabSmoke(const QString& outputDir);

    std::unique_ptr<CoreServices> m_pCoreServices;
    std::unique_ptr<::VisualsManager> m_visualsManager;

    QString m_mainFilePath;

    std::unique_ptr<QQmlApplicationEngine> m_pAppEngine;
    QmlAutoReload m_autoReload;
    QStringList m_qmlWarnings;
};

} // namespace qml
} // namespace mixxx
