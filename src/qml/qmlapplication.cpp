#include "qmlapplication.h"

#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QMutex>
#include <QMutexLocker>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPointingDevice>
#include <QQmlComponent>
#include <QQmlEngineExtensionPlugin>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>

#include "control/controlobject.h"
#include "controllers/controllermanager.h"
#include "decklab/decklabsignals.h"
#include "mixer/basetrackplayer.h"
#include "mixer/playermanager.h"
#include "moc_qmlapplication.cpp"
#include "qml/asyncimageprovider.h"
#include "qml/qmldlgpreferencesproxy.h"
#include "qml/qmlwaveformdisplay.h"
#include "soundio/soundmanager.h"
#include "track/track.h"
#include "waveform/visualplayposition.h"
#include "waveform/visualsmanager.h"
#include "waveform/waveformwidgetfactory.h"
Q_IMPORT_QML_PLUGIN(MixxxPlugin)
Q_IMPORT_QML_PLUGIN(Mixxx_ControlsPlugin)

namespace {
const QString kMainQmlFileName = QStringLiteral("qml/main.qml");

// Converts a (capturing) lambda into a function pointer that can be passed to
// qmlRegisterSingletonType.
template<class F>
auto lambda_to_singleton_type_factory_ptr(F&& f) {
    static F fn = std::forward<F>(f);
    return [](QQmlEngine* pEngine, QJSEngine* pScriptEngine) -> QObject* {
        return fn(pEngine, pScriptEngine);
    };
}

/// DECKLAB_QML_MAIN selects another QML entry point (relative to the
/// resource path, or absolute), e.g. qml/DeckLab/main.qml. Unset = upstream.
QString mainQmlPath(const QString& resourcePath) {
    const QString override = qEnvironmentVariable("DECKLAB_QML_MAIN");
    if (override.isEmpty()) {
        return resourcePath + kMainQmlFileName;
    }
    if (QDir::isAbsolutePath(override)) {
        return override;
    }
    return resourcePath + override;
}

QList<QQuickWindow*> rootWindows(QQmlApplicationEngine* pEngine) {
    QList<QQuickWindow*> windows;
    if (!pEngine) {
        return windows;
    }
    const auto roots = pEngine->rootObjects();
    for (QObject* pRoot : roots) {
        if (auto* pWindow = qobject_cast<QQuickWindow*>(pRoot)) {
            windows.append(pWindow);
        }
    }
    return windows;
}

// Walks the visual item tree; Repeater and Loader children are not always
// QObject children of their visual parent.
template<class T>
void collectItems(QQuickItem* pItem, QList<T*>* pOut) {
    if (!pItem) {
        return;
    }
    if (auto* pMatch = qobject_cast<T*>(pItem)) {
        pOut->append(pMatch);
    }
    const auto children = pItem->childItems();
    for (QQuickItem* pChild : children) {
        collectItems(pChild, pOut);
    }
}

QQuickItem* findItem(QQuickItem* pItem, const QString& objectName) {
    if (!pItem) {
        return nullptr;
    }
    if (pItem->objectName() == objectName) {
        return pItem;
    }
    const auto children = pItem->childItems();
    for (QQuickItem* pChild : children) {
        if (QQuickItem* pFound = findItem(pChild, objectName)) {
            return pFound;
        }
    }
    return nullptr;
}

/// Generated 44.1 kHz mono groove (kick, off-beat hat, bass, minor pad) so the
/// analyzers find a tempo and key and the waveform has visible structure.
/// No recorded material: fixtures must never carry personal data.
bool writeGeneratedGroove(const QString& path, double bpm, double rootHz, quint32 seed) {
    constexpr int kRate = 44100;
    constexpr int kSeconds = 40;
    constexpr double kTwoPi = 6.283185307179586;
    const int frames = kRate * kSeconds;
    QByteArray pcm(frames * 2, Qt::Uninitialized);
    auto* pOut = reinterpret_cast<qint16*>(pcm.data());
    const double beat = 60.0 / bpm;
    quint32 noise = seed;
    double kickPhase = 0.0;
    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kRate;
        const int beatIndex = static_cast<int>(t / beat);
        const double inBeat = t - beatIndex * beat;
        // Beats 32-47 are a breakdown: no kick, louder pad.
        const bool breakdown = beatIndex >= 32 && beatIndex < 48;
        double sample = 0.0;
        if (inBeat < 0.25 && !breakdown) {
            const double frequency = 50.0 + 70.0 * std::exp(-inBeat * 30.0);
            kickPhase += kTwoPi * frequency / kRate;
            sample += 0.85 * std::sin(kickPhase) * std::exp(-inBeat * 12.0);
        } else if (inBeat < 0.001) {
            kickPhase = 0.0;
        }
        const double offBeat = std::fmod(t + beat / 2.0, beat);
        noise = noise * 1664525u + 1013904223u;
        const double white = static_cast<double>(static_cast<qint32>(noise)) / 2147483648.0;
        if (offBeat < 0.05) {
            sample += 0.22 * white * std::exp(-offBeat * 80.0);
        }
        const double eighth = std::fmod(t, beat / 2.0);
        sample += (breakdown ? 0.1 : 0.3) * std::sin(kTwoPi * rootHz * t) * std::exp(-eighth * 5.0);
        const double pad = std::sin(kTwoPi * 2.0 * rootHz * t) +
                std::sin(kTwoPi * 2.0 * rootHz * 1.189207 * t) +
                std::sin(kTwoPi * 2.0 * rootHz * 1.498307 * t);
        sample += (breakdown ? 0.12 : 0.05) * pad;
        pOut[i] = static_cast<qint16>(std::lround(std::clamp(sample, -1.0, 1.0) * 30000.0));
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    QByteArray header;
    QDataStream out(&header, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    const quint32 bytes = static_cast<quint32>(pcm.size());
    out.writeRawData("RIFF", 4);
    out << quint32(36 + bytes);
    out.writeRawData("WAVE", 4);
    out.writeRawData("fmt ", 4);
    out << quint32(16) << quint16(1) << quint16(1) << quint32(kRate) << quint32(kRate * 2)
        << quint16(2) << quint16(16);
    out.writeRawData("data", 4);
    out << bytes;
    return file.write(header) == header.size() && file.write(pcm) == pcm.size();
}

void sendMouse(QQuickWindow* pWindow, QEvent::Type type, QPointF position, Qt::MouseButtons buttons) {
    QMouseEvent event(type,
            position,
            pWindow->mapToGlobal(position),
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            buttons,
            Qt::NoModifier,
            QPointingDevice::primaryPointingDevice());
    QCoreApplication::sendEvent(pWindow, &event);
}

QPointF itemCenter(QQuickItem* pItem) {
    return pItem->mapToScene(QPointF(pItem->width() / 2.0, pItem->height() / 2.0));
}

double controlValue(const QString& group, const QString& item) {
    return ControlObject::get(ConfigKey(group, item));
}
} // namespace

namespace mixxx {
namespace qml {

QmlApplication::QmlApplication(
        QApplication* app,
        const CmdlineArgs& args)
        : m_pCoreServices(std::make_unique<mixxx::CoreServices>(args, app)),
          m_visualsManager(std::make_unique<VisualsManager>()),
          m_mainFilePath(mainQmlPath(m_pCoreServices->getSettings()->getResourcePath())),
          m_pAppEngine(nullptr),
          m_autoReload() {
    QQuickStyle::setStyle("Basic");

    m_pCoreServices->initialize(app);
    SoundDeviceStatus result = m_pCoreServices->getSoundManager()->setupDevices();
    if (result != SoundDeviceStatus::Ok) {
        const int reInt = static_cast<int>(result);
        qCritical() << "Error setting up sound devices:" << reInt;
        exit(reInt);
    }

    // FIXME: DlgPreferences has some initialization logic that must be executed
    // before the GUI is shown, at least for the effects system.
    std::shared_ptr<QDialog> pDlgPreferences = m_pCoreServices->makeDlgPreferences();
    // Without this, QApplication will quit when the last QWidget QWindow is
    // closed because it does not take into account the window created by
    // the QQmlApplicationEngine.
    pDlgPreferences->setAttribute(Qt::WA_QuitOnClose, false);

    // Since DlgPreferences is only meant to be used in the main QML engine, it
    // follows a strict singleton pattern design
    QmlDlgPreferencesProxy::s_pInstance =
            std::make_unique<QmlDlgPreferencesProxy>(pDlgPreferences, this);
    loadQml(m_mainFilePath);

    m_pCoreServices->getControllerManager()->setUpDevices();

    connect(&m_autoReload,
            &QmlAutoReload::triggered,
            this,
            [this]() {
                loadQml(m_mainFilePath);
            });

    const QStringList visualGroups =
            m_pCoreServices->getPlayerManager()->getVisualPlayerGroups();
    for (const QString& group : visualGroups) {
        m_visualsManager->addDeck(group);
    }

    m_pCoreServices->getPlayerManager()->connect(
            m_pCoreServices->getPlayerManager().get(),
            &PlayerManager::numberOfDecksChanged,
            this,
            [this](int decks) {
                for (int i = 0; i < decks; ++i) {
                    QString group = PlayerManager::groupForDeck(i);
                    m_visualsManager->addDeckIfNotExist(group);
                }
            });

    // SIGTERM/SIGINT quit through the event loop, so CoreServices shuts down
    // and saves settings (a kiosk unit stop, or Ctrl+C on the console).
    if (DeckLabTerminationSignals* pTermination = DeckLabTerminationSignals::install(this)) {
        connect(pTermination,
                &DeckLabTerminationSignals::terminationRequested,
                this,
                [](int) {
                    QCoreApplication::quit();
                });
        QTimer::singleShot(0, pTermination, &DeckLabTerminationSignals::arm);
    }

    installDeckLabPerfLog();
    const QString smokeDir = qEnvironmentVariable("DECKLAB_QML_SMOKE_DIR");
    if (!smokeDir.isEmpty()) {
        runDeckLabSmoke(smokeDir);
    }
}

QmlApplication::~QmlApplication() {
    // Delete all the QML singletons in order to prevent leak detection in CoreService
    QmlDlgPreferencesProxy::s_pInstance.reset();
    m_visualsManager.reset();
    m_pAppEngine.reset();
    m_pCoreServices.reset();
}

void QmlApplication::loadQml(const QString& path) {
    // QQmlApplicationEngine::load creates a new window but also leaves the old one,
    // so it is necessary to destroy the old QQmlApplicationEngine and create a new one.
    m_pAppEngine = std::make_unique<QQmlApplicationEngine>();
    connect(m_pAppEngine.get(),
            &QQmlEngine::warnings,
            this,
            [this](const QList<QQmlError>& warnings) {
                for (const QQmlError& warning : warnings) {
                    if (m_qmlWarnings.size() < 50) {
                        m_qmlWarnings.append(warning.toString());
                    }
                }
            });

    m_autoReload.clear();
    m_pAppEngine->addUrlInterceptor(&m_autoReload);
    m_pAppEngine->addImportPath(QStringLiteral(":/mixxx.org/imports"));

    // No memory leak here, the QQmlEngine takes ownership of the provider
    QQuickAsyncImageProvider* pImageProvider = new AsyncImageProvider(
            m_pCoreServices->getTrackCollectionManager());
    m_pAppEngine->addImageProvider(AsyncImageProvider::kProviderName, pImageProvider);

    m_pAppEngine->load(path);
    if (m_pAppEngine->rootObjects().isEmpty()) {
        qCritical() << "Failed to load QML file" << path;
    }
}

void QmlApplication::installDeckLabPerfLog() {
    if (qEnvironmentVariable("DECKLAB_PERF_LOG") != QStringLiteral("1")) {
        return;
    }
    const auto windows = rootWindows(m_pAppEngine.get());
    for (QQuickWindow* pWindow : windows) {
        auto frames = std::make_shared<std::atomic<quint64>>(0);
        connect(
                pWindow,
                &QQuickWindow::frameSwapped,
                pWindow,
                [frames]() {
                    frames->fetch_add(1, std::memory_order_relaxed);
                },
                Qt::DirectConnection);
        connect(
                pWindow,
                &QQuickWindow::sceneGraphInitialized,
                pWindow,
                [pWindow]() {
                    // Render thread, context current.
                    QOpenGLContext* pContext = QOpenGLContext::currentContext();
                    if (!pContext) {
                        return;
                    }
                    QOpenGLFunctions* f = pContext->functions();
                    qInfo().noquote() << "DECKLAB_QML_PERF gl renderer="
                                      << reinterpret_cast<const char*>(f->glGetString(GL_RENDERER))
                                      << "version="
                                      << reinterpret_cast<const char*>(f->glGetString(GL_VERSION))
                                      << "platform=" << QGuiApplication::platformName()
                                      << "size=" << pWindow->size();
                },
                Qt::DirectConnection);
        auto* pTimer = new QTimer(pWindow);
        pTimer->setInterval(10000);
        auto lastFrames = std::make_shared<quint64>(0);
        auto lastRenders = std::make_shared<QHash<QString, quint64>>();
        connect(pTimer, &QTimer::timeout, pWindow, [pWindow, frames, lastFrames, lastRenders]() {
            const quint64 now = frames->load(std::memory_order_relaxed);
            const quint64 delta = now - *lastFrames;
            *lastFrames = now;
            QStringList waveforms;
            QList<QmlWaveformDisplay*> displays;
            collectItems(pWindow->contentItem(), &displays);
            for (QmlWaveformDisplay* pDisplay : std::as_const(displays)) {
                const quint64 renders = pDisplay->renderCount();
                const quint64 previous = lastRenders->value(pDisplay->getGroup(), 0);
                lastRenders->insert(pDisplay->getGroup(), renders);
                waveforms.append(QStringLiteral("%1=%2").arg(pDisplay->getGroup()).arg(renders - previous));
            }
            qInfo().noquote() << QStringLiteral(
                    "DECKLAB_QML_PERF window frames=%1 fps=%2 gate=%3 waveform_renders %4")
                                         .arg(delta)
                                         .arg(static_cast<double>(delta) / 10.0, 0, 'f', 1)
                                         .arg(QmlWaveformDisplay::renderGateEnabled()
                                                         ? QStringLiteral("on")
                                                         : QStringLiteral("off"))
                                         .arg(waveforms.join(QLatin1Char(' ')));
        });
        pTimer->start();
    }
}

void QmlApplication::runDeckLabSmoke(const QString& outputDir) {
    // CI evidence for the gpu-screen spike: generated fixtures, real controls,
    // synthetic touch, window grabs. Audio is never opened (api=None profile).
    struct State {
        QJsonObject result;
        QJsonArray failures;
        QJsonArray qmlErrors;
        int screens = 0;
        std::atomic<quint64> frames{0};
        QMutex glMutex;
        QString glRenderer;
        QString glVersion;
        QElapsedTimer clock;
    };
    auto state = std::make_shared<State>();
    state->clock.start();
    QDir().mkpath(outputDir);
    const QDir out(outputDir);

    const auto windows = rootWindows(m_pAppEngine.get());
    QQuickWindow* pWindow = windows.isEmpty() ? nullptr : windows.first();

    // Compile every Deck Lab QML file so one CI run reports all errors.
    const QFileInfo mainInfo(m_mainFilePath);
    const auto qmlFiles = mainInfo.dir().entryInfoList({QStringLiteral("*.qml")}, QDir::Files);
    for (const QFileInfo& file : qmlFiles) {
        QQmlComponent component(m_pAppEngine.get(), QUrl::fromLocalFile(file.absoluteFilePath()));
        if (component.isError()) {
            const auto errors = component.errors();
            for (const QQmlError& error : errors) {
                state->qmlErrors.append(error.toString());
            }
        }
    }
    if (!pWindow) {
        state->qmlErrors.append(QStringLiteral("No QQuickWindow root for ") + m_mainFilePath);
    }

    const auto finish = [this, state, out](int exitCode) {
        QJsonObject result = state->result;
        result.insert(QStringLiteral("screens"), state->screens);
        result.insert(QStringLiteral("failures"), state->failures);
        result.insert(QStringLiteral("qml_errors"), state->qmlErrors);
        QJsonArray warnings;
        for (const QString& warning : std::as_const(m_qmlWarnings)) {
            warnings.append(warning);
        }
        result.insert(QStringLiteral("qml_warnings"), warnings);
        result.insert(QStringLiteral("audio_started"), false);
        result.insert(QStringLiteral("platform"), QGuiApplication::platformName());
        result.insert(QStringLiteral("render_gate"), QmlWaveformDisplay::renderGateEnabled());
        {
            QMutexLocker lock(&state->glMutex);
            result.insert(QStringLiteral("gl_renderer"), state->glRenderer);
            result.insert(QStringLiteral("gl_version"), state->glVersion);
        }
        result.insert(QStringLiteral("elapsed_ms"), static_cast<qint64>(state->clock.elapsed()));
        QFile file(out.filePath(QStringLiteral("qml-smoke.json")));
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QJsonDocument(result).toJson());
        }
        qInfo().noquote() << "DECKLAB_QML_SMOKE" << QJsonDocument(result).toJson(QJsonDocument::Compact);
        QCoreApplication::exit(exitCode);
    };

    if (!pWindow) {
        QTimer::singleShot(0, this, [finish]() {
            finish(2);
        });
        return;
    }

    connect(
            pWindow,
            &QQuickWindow::frameSwapped,
            pWindow,
            [state]() {
                state->frames.fetch_add(1, std::memory_order_relaxed);
            },
            Qt::DirectConnection);
    connect(
            pWindow,
            &QQuickWindow::sceneGraphInitialized,
            pWindow,
            [state]() {
                QOpenGLContext* pContext = QOpenGLContext::currentContext();
                if (!pContext) {
                    return;
                }
                QOpenGLFunctions* f = pContext->functions();
                QMutexLocker lock(&state->glMutex);
                state->glRenderer = QString::fromLatin1(
                        reinterpret_cast<const char*>(f->glGetString(GL_RENDERER)));
                state->glVersion = QString::fromLatin1(
                        reinterpret_cast<const char*>(f->glGetString(GL_VERSION)));
            },
            Qt::DirectConnection);

    // Hard stop: never hang CI.
    QTimer::singleShot(150000, this, [state, finish]() {
        state->failures.append(QStringLiteral("Smoke timed out"));
        finish(3);
    });

    const auto grab = [state, pWindow, out](const QString& name) {
        const QImage image = pWindow->grabWindow();
        if (image.isNull() || !image.save(out.filePath(name))) {
            state->failures.append(QStringLiteral("Screenshot failed: ") + name);
        } else {
            state->screens++;
        }
    };

    // Step chain on the GUI thread. A step that returns false runs again
    // after its delay (polling); true moves on to the next step.
    using Step = std::pair<int, std::function<bool()>>;
    auto steps = std::make_shared<QList<Step>>();
    auto runNext = std::make_shared<std::function<void()>>();
    *runNext = [this, steps, runNext]() {
        if (steps->isEmpty()) {
            return;
        }
        QTimer::singleShot(steps->first().first, this, [steps, runNext]() {
            if (steps->isEmpty()) {
                return;
            }
            if (steps->first().second()) {
                steps->removeFirst();
            }
            (*runNext)();
        });
    };
    const auto add = [steps](int delayMs, std::function<void()> fn) {
        steps->append({delayMs, [fn = std::move(fn)]() {
                           fn();
                           return true;
                       }});
    };
    const auto poll = [steps](int delayMs, std::function<bool()> fn) {
        steps->append({delayMs, std::move(fn)});
    };

    const QString fixtureDir = out.filePath(QStringLiteral("fixtures"));
    const QStringList groups = {QStringLiteral("[Channel1]"), QStringLiteral("[Channel2]")};
    auto pPlayerManager = m_pCoreServices->getPlayerManager();

    add(500, [state, fixtureDir, groups, pPlayerManager]() {
        QDir().mkpath(fixtureDir);
        const double bpms[] = {124.0, 128.0};
        const double roots[] = {110.0, 82.41}; // A and E
        for (int deck = 0; deck < 2; ++deck) {
            const QString path = QDir(fixtureDir).filePath(
                    QStringLiteral("generated-groove-%1.wav").arg(deck + 1));
            if (!writeGeneratedGroove(path, bpms[deck], roots[deck], 0x5eed0000u + deck)) {
                state->failures.append(QStringLiteral("Fixture write failed"));
                continue;
            }
            pPlayerManager->slotLoadLocationToPlayer(path, groups[deck], false);
        }
    });

    // Wait for both decks to load and analyse (tempo found), at most 40 s.
    auto analysisStart = std::make_shared<QElapsedTimer>();
    add(0, [analysisStart]() {
        analysisStart->start();
    });
    poll(250, [state, groups, analysisStart]() {
        bool ready = true;
        for (const QString& group : groups) {
            ready = ready && controlValue(group, QStringLiteral("track_loaded")) > 0 &&
                    controlValue(group, QStringLiteral("bpm")) > 0;
        }
        if (ready) {
            state->result.insert(QStringLiteral("analysis_ms"),
                    static_cast<qint64>(analysisStart->elapsed()));
        }
        return ready || analysisStart->elapsed() > 40000;
    });

    add(1500, [state, groups, pPlayerManager]() {
        if (!state->result.contains(QStringLiteral("analysis_ms"))) {
            state->failures.append(QStringLiteral("Decks did not load and analyse in time"));
        }
        QJsonObject decks;
        const double positions[] = {0.32, 0.58};
        const char* const titles[] = {"Generated Groove A", "Generated Groove B"};
        for (int deck = 0; deck < 2; ++deck) {
            const QString& group = groups[deck];
            BaseTrackPlayer* pPlayer = pPlayerManager->getPlayer(group);
            const TrackPointer pTrack = pPlayer ? pPlayer->getLoadedTrack() : TrackPointer();
            if (pTrack) {
                pTrack->setTitle(QString::fromLatin1(titles[deck]));
                pTrack->setArtist(QStringLiteral("Deck Lab test signal"));
            }
            // Stand in for the engine, which never runs without audio: put
            // each deck mid-track so both sides of the playhead have audio.
            ControlObject* pPosition = ControlObject::getControl(
                    ConfigKey(group, QStringLiteral("playposition")));
            if (pPosition) {
                pPosition->setAndConfirm(positions[deck]);
            }
            const auto pVisual = VisualPlayPosition::getVisualPlayPosition(group);
            if (pVisual) {
                pVisual->set(positions[deck],
                        0.0,
                        0.0,
                        positions[deck],
                        0.0,
                        SlipModeState::Disabled,
                        false,
                        false,
                        false,
                        0.0,
                        0.0,
                        controlValue(group, QStringLiteral("duration")),
                        0.0);
            }
            decks.insert(group,
                    QJsonObject{
                            {QStringLiteral("bpm"), controlValue(group, QStringLiteral("bpm"))},
                            {QStringLiteral("visual_key"),
                                    controlValue(group, QStringLiteral("visual_key"))},
                            {QStringLiteral("duration"),
                                    controlValue(group, QStringLiteral("duration"))},
                    });
        }
        state->result.insert(QStringLiteral("decks"), decks);
    });

    // Idle cost with both decks paused: frames the window drew in 3 s.
    auto idleStart = std::make_shared<quint64>(0);
    add(1500, [state, idleStart]() {
        *idleStart = state->frames.load(std::memory_order_relaxed);
    });
    add(3000, [state, idleStart, pWindow, grab]() {
        state->result.insert(QStringLiteral("idle_frames_3s"),
                static_cast<qint64>(state->frames.load(std::memory_order_relaxed) - *idleStart));
        QList<QmlWaveformDisplay*> displays;
        collectItems(pWindow->contentItem(), &displays);
        state->result.insert(QStringLiteral("waveform_items"), static_cast<int>(displays.size()));
        QJsonObject renders;
        for (QmlWaveformDisplay* pDisplay : std::as_const(displays)) {
            renders.insert(pDisplay->getGroup(), static_cast<qint64>(pDisplay->renderCount()));
        }
        state->result.insert(QStringLiteral("waveform_renders"), renders);
        grab(QStringLiteral("qml-decks-800x480.png"));
    });

    // Tap PLAY on deck 1 (state only: no audio, so nothing moves).
    add(200, [state, pWindow]() {
        QQuickItem* pPlay = findItem(pWindow->contentItem(), QStringLiteral("decklab-play-[Channel1]"));
        if (!pPlay) {
            state->failures.append(QStringLiteral("PLAY button not found"));
            state->result.insert(QStringLiteral("play_tap"), false);
            return;
        }
        const QPointF center = itemCenter(pPlay);
        sendMouse(pWindow, QEvent::MouseButtonPress, center, Qt::LeftButton);
        sendMouse(pWindow, QEvent::MouseButtonRelease, center, Qt::NoButton);
    });
    add(300, [state]() {
        state->result.insert(QStringLiteral("play_tap"),
                controlValue(QStringLiteral("[Channel1]"), QStringLiteral("play")) > 0);
    });

    // Touch-scratch deck 2: hold the waveform and drag.
    auto scratchPoint = std::make_shared<QPointF>();
    add(100, [state, pWindow, scratchPoint]() {
        QQuickItem* pWave = findItem(pWindow->contentItem(), QStringLiteral("decklab-wave-[Channel2]"));
        if (!pWave) {
            state->failures.append(QStringLiteral("Deck 2 waveform not found"));
            return;
        }
        *scratchPoint = itemCenter(pWave);
        sendMouse(pWindow, QEvent::MouseButtonPress, *scratchPoint, Qt::LeftButton);
    });
    for (int i = 1; i <= 6; ++i) {
        add(30, [pWindow, scratchPoint, i]() {
            sendMouse(pWindow,
                    QEvent::MouseMove,
                    *scratchPoint + QPointF(10.0 * i, 0.0),
                    Qt::LeftButton);
        });
    }
    add(400, [state, grab]() {
        const QString group = QStringLiteral("[Channel2]");
        state->result.insert(QStringLiteral("scratch_engaged"),
                controlValue(group, QStringLiteral("scratch_position_enable")) > 0);
        state->result.insert(QStringLiteral("scratch_position"),
                controlValue(group, QStringLiteral("scratch_position")));
        grab(QStringLiteral("qml-decks-touch-800x480.png"));
    });
    add(50, [pWindow, scratchPoint]() {
        sendMouse(pWindow, QEvent::MouseButtonRelease, *scratchPoint + QPointF(60.0, 0.0), Qt::NoButton);
    });
    add(300, [state]() {
        state->result.insert(QStringLiteral("scratch_released"),
                controlValue(QStringLiteral("[Channel2]"),
                        QStringLiteral("scratch_position_enable")) == 0);
    });

    // Other panel sizes letterbox the 800x480 design.
    add(100, [pWindow]() {
        pWindow->resize(1024, 600);
    });
    add(1200, [grab]() {
        grab(QStringLiteral("qml-decks-1024x600.png"));
    });
    add(100, [pWindow]() {
        pWindow->resize(800, 480);
    });

    // A placeholder tab, to show navigation without native windows.
    add(800, [state, pWindow]() {
        QQuickItem* pTab = findItem(pWindow->contentItem(), QStringLiteral("decklab-tab-browse"));
        if (!pTab) {
            state->failures.append(QStringLiteral("Browse tab not found"));
            return;
        }
        const QPointF center = itemCenter(pTab);
        sendMouse(pWindow, QEvent::MouseButtonPress, center, Qt::LeftButton);
        sendMouse(pWindow, QEvent::MouseButtonRelease, center, Qt::NoButton);
    });
    add(600, [grab]() {
        grab(QStringLiteral("qml-tab-browse-800x480.png"));
    });
    add(100, [finish]() {
        finish(0);
    });

    (*runNext)();
}

} // namespace qml
} // namespace mixxx
