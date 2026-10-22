#include "waveform/waveformwidgetfactory.h"

#include "waveform/waveform.h"

#ifdef MIXXX_USE_QOPENGL
#include <QGuiApplication>
#include <QOpenGLShaderProgram>
#include <QOpenGLWindow>
#else
#include <QGLFormat>
#include <QGLShaderProgram>
#endif

#include <QMetaMethod>
#include <QOpenGLFunctions>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include "control/controlobject.h"
#include "moc_waveformwidgetfactory.cpp"
#include "util/cmdlineargs.h"
#include "util/math.h"
#include "util/performancetimer.h"
#include "util/timer.h"
#include "waveform/guitick.h"
#include "waveform/rendergate.h"
#include "waveform/sharedglcontext.h"
#include "waveform/visualsmanager.h"
#include "waveform/vsyncthread.h"
#ifdef MIXXX_USE_QOPENGL
#include "waveform/renderers/allshader/waveformrenderersignalbase.h"
#include "waveform/widgets/allshader/waveformwidget.h"
#include "waveform/widgets/glvsynctestwidget.h"
#endif
#include "waveform/widgets/emptywaveformwidget.h"
#include "waveform/widgets/hsvwaveformwidget.h"
#include "waveform/widgets/rgbwaveformwidget.h"
#include "waveform/widgets/simplesignalwaveformwidget.h"
#include "waveform/widgets/softwarewaveformwidget.h"
#include "waveform/widgets/waveformwidgetabstract.h"
#include "widget/wvumeterbase.h"
#include "widget/wvumeterlegacy.h"
#include "widget/wwaveformviewer.h"

namespace {

// We use an AllBand gain default of 2, because default ReplayGain is "enabled" at -18 LUFS
// which gives at least 6 dB headroom with modern pop tracks.
constexpr double kVisualGainDefault[] = {2, 1, 1, 1};
constexpr bool kOverviewNormalizedDefault = false;

// Returns true if the given waveform should be rendered.
bool shouldRenderWaveform(WaveformWidgetAbstract* pWaveformWidget) {
    if (pWaveformWidget == nullptr ||
        pWaveformWidget->getWidth() == 0 ||
        pWaveformWidget->getHeight() == 0) {
        return false;
    }

    auto* glw = pWaveformWidget->getGLWidget();
    if (glw == nullptr) {
        // Not a WGLWidget. We can simply use QWidget::isVisible.
        auto* qwidget = qobject_cast<QWidget*>(pWaveformWidget->getWidget());
        return qwidget != nullptr && qwidget->isVisible();
    }

    return glw->shouldRender();
}

const QRegularExpression openGLVersionRegex(QStringLiteral("^(\\d+)\\.(\\d+).*$"));

const QString kWaveformGroup(QStringLiteral("[Waveform]"));
const ConfigKey kWaveformTypeKey =
        ConfigKey(kWaveformGroup, QStringLiteral("WaveformType"));
const ConfigKey kHardwareAccelerationKey =
        ConfigKey(kWaveformGroup, QStringLiteral("use_hardware_acceleration"));
const ConfigKey kZoomSyncKey = ConfigKey(
        kWaveformGroup, QStringLiteral("ZoomSynchronization"));
const ConfigKey kEndOfTrackWarningKey = ConfigKey(
        kWaveformGroup, QStringLiteral("EndOfTrackWarningTime"));
const ConfigKey kDefaultZoomKey =
        ConfigKey(kWaveformGroup, QStringLiteral("DefaultZoom"));
const ConfigKey kFrameRateKey =
        ConfigKey(kWaveformGroup, QStringLiteral("FrameRate"));
const ConfigKey kVSyncKey = ConfigKey(kWaveformGroup, QStringLiteral("VSync"));

// Deck Lab idle-aware PLL: a shared-window paint that took this long waited
// inside the driver for a vblank (its swap queue was full). An unqueued paint
// of the 3x3 window returns in well under a millisecond; a queued one in steady
// state waits about two thirds of a frame.
constexpr qint64 kPllKeepaliveBlockedMicros = 2000;
constexpr int kPllLockWatchdogIntervalMillis = 100;

QString vsyncModeName(int mode) {
    switch (mode) {
    case VSyncThread::ST_DEFAULT:
        return QStringLiteral("default");
    case VSyncThread::ST_FREE:
        return QStringLiteral("free");
    case VSyncThread::ST_PLL:
        return QStringLiteral("pll");
    case VSyncThread::ST_TIMER:
        return QStringLiteral("timer");
    default:
        return QString::number(mode);
    }
}

ConfigKey visualGainKey(int index) {
    return ConfigKey(kWaveformGroup, QStringLiteral("VisualGain_") + QString::number(index));
}

}  // anonymous namespace

///////////////////////////////////////////

WaveformWidgetAbstractHandle::WaveformWidgetAbstractHandle()
        : m_type(WaveformWidgetType::Invalid) {
}

///////////////////////////////////////////

WaveformWidgetHolder::WaveformWidgetHolder()
        : m_waveformWidget(nullptr),
          m_waveformViewer(nullptr),
          m_skinContextCache(UserSettingsPointer(), QString()) {
}

WaveformWidgetHolder::WaveformWidgetHolder(WaveformWidgetAbstract* waveformWidget,
                                           WWaveformViewer* waveformViewer,
                                           const QDomNode& node,
                                           const SkinContext& parentContext)
    : m_waveformWidget(waveformWidget),
      m_waveformViewer(waveformViewer),
      m_skinNodeCache(node.cloneNode()),
      m_skinContextCache(&parentContext) {
}

///////////////////////////////////////////

WaveformWidgetFactory::WaveformWidgetFactory()
        // Set an empty waveform initially. We will set the correct one when skin load finishes.
        // Concretely, we want to set a non-GL waveform when loading the skin so that the window
        // loads correctly.
        : m_type(WaveformWidgetType::Empty),
          m_configType(WaveformWidgetType::Empty),
          m_config(nullptr),
          m_skipRender(false),
          m_frameRate(60),
          m_endOfTrackWarningTime(30),
          m_defaultZoom(WaveformWidgetRenderer::s_waveformDefaultZoom),
          m_zoomSync(true),
          m_overviewNormalized(kOverviewNormalizedDefault),
          m_untilMarkShowBeats(false),
          m_untilMarkShowTime(false),
          m_untilMarkAlign(Qt::AlignVCenter),
          m_untilMarkTextPointSize(24),
          m_untilMarkTextHeightLimit(toUntilMarkTextHeightLimit(0)),
          m_stemSplitTracks(false),
          m_openGlAvailable(false),
          m_openGlesAvailable(false),
          m_openGLShaderAvailable(false),
          m_beatGridAlpha(90),
          m_vsyncThread(nullptr),
          m_pGuiTick(nullptr),
          m_pVisualsManager(nullptr),
          m_frameCnt(0),
          m_actualFrameRate(0),
          m_playMarkerPosition(WaveformWidgetRenderer::s_defaultPlayMarkerPosition),
          m_renderGateEnabled(mixxx::decklab::renderGateEnabledFromEnvironment()),
          m_perfLogEnabled(mixxx::decklab::perfLogEnabledFromEnvironment()),
          m_perfTickNanos(0),
          m_perfTicks(0),
          m_perfTickNanosSum(0),
          m_perfTickNanosMax(0),
          m_perfDroppedFramesBaseline(0),
          m_perfSwapTimerStarted(false),
          m_pllKeepaliveEnabled(false),
          m_renderPassActive(false),
          m_pPllLockWatchdogTimer(nullptr),
          m_pPllKeepaliveWindow(nullptr),
          m_pllKeepalivePaintStarted(false),
          m_perfSkippedSlotsBaseline(0) {
    if (m_perfLogEnabled) {
        m_perfSwapIntervals.reserve(2048);
        m_perfScratch.reserve(2048);
    }
    m_pStemSplitTracksControl = std::make_unique<ControlObject>(
            ConfigKey(kWaveformGroup, QStringLiteral("stem_split_tracks")));
    m_visualGain[AllBand] = kVisualGainDefault[AllBand];
    m_visualGain[Low] = kVisualGainDefault[Low];
    m_visualGain[Mid] = kVisualGainDefault[Mid];
    m_visualGain[High] = kVisualGainDefault[High];

#ifdef MIXXX_USE_QOPENGL
    WGLWidget* widget = SharedGLContext::getWidget();
    if (widget) {
        widget->makeCurrentIfNeeded();
        auto* pContext = QOpenGLContext::currentContext();
        if (pContext) {
            auto* glFunctions = pContext->functions();
            glFunctions->initializeOpenGLFunctions();
            QString versionString(QLatin1String(
                    reinterpret_cast<const char*>(glFunctions->glGetString(GL_VERSION))));
            QString vendorString(QLatin1String(
                    reinterpret_cast<const char*>(glFunctions->glGetString(GL_VENDOR))));
            QString rendererString = QString(QLatin1String(
                    reinterpret_cast<const char*>(glFunctions->glGetString(GL_RENDERER))));
            qDebug().noquote() << QStringLiteral(
                    "OpenGL driver version string \"%1\", vendor \"%2\", "
                    "renderer \"%3\"")
                                          .arg(versionString, vendorString, rendererString);

            GLint majorVersion, minorVersion = GL_INVALID_ENUM;
            glFunctions->glGetIntegerv(GL_MAJOR_VERSION, &majorVersion);
            glFunctions->glGetIntegerv(GL_MINOR_VERSION, &minorVersion);
            if (majorVersion == GL_INVALID_ENUM || minorVersion == GL_INVALID_ENUM) {
                // GL_MAJOR/MINOR_VERSION are not supported below OpenGL 3.0, so
                // parse GL_VERSION string as a fallback.
                // https://www.khronos.org/opengl/wiki/OpenGL_Context#OpenGL_version_number
                auto match = openGLVersionRegex.match(versionString);
                DEBUG_ASSERT(match.hasMatch());
                majorVersion = match.captured(1).toInt();
                minorVersion = match.captured(2).toInt();
            }

            qDebug().noquote()
                    << QStringLiteral("Supported OpenGL version: %1.%2")
                               .arg(QString::number(majorVersion), QString::number(minorVersion));

            m_openGLShaderAvailable = QOpenGLShaderProgram::hasOpenGLShaderPrograms(pContext);

            // With EGLFS there is always exactly one native window and one EGL window surface
            // OpenGL windows cannot be embedded into our QWidgets main window we already have.
            // That's why m_openGlesAvailable is not set to true. TODO: use GL Widgets for all
            // https://doc.qt.io/qt-6/embedded-linux.html
            // See https://doc.qt.io/qt-6/qguiapplication.html#platformName-prop for possible values
            bool isEglfs = QGuiApplication::platformName() == QStringLiteral("eglfs");
            bool isOpenGles = pContext->isOpenGLES();

            if (isEglfs) {
                m_openGLVersion = QStringLiteral("EGLFS ");
            } else if (isOpenGles) {
                m_openGLVersion = QStringLiteral("ES ");
            }
            // else m_openGLVersion is still empty

            //: This refers to a missing openGL version
            m_openGLVersion += majorVersion == 0 ? tr("None") : versionString;

            if (!isEglfs) {
                // Qt >= 5 requires at least OpenGL 2.1 or OpenGL ES 2.0
                int combinedVersion = majorVersion * 100 + minorVersion;
                m_openGlesAvailable = isOpenGles && combinedVersion >= 200;
                m_openGlAvailable = !isOpenGles && combinedVersion >= 201;
            }

            if (!rendererString.isEmpty()) {
                m_openGLVersion += QStringLiteral(" (") + rendererString + QChar(')');
            }
        } else {
            qDebug() << "QOpenGLContext::currentContext() returns nullptr";
        }
        widget->doneCurrent();
        widget->hide();
    }
#else
    QGLWidget* pGlWidget = SharedGLContext::getWidget();
    if (pGlWidget && pGlWidget->isValid()) {
        // will be false if SafeMode is enabled

        pGlWidget->show();
        // Without a makeCurrent, hasOpenGLShaderPrograms returns false on Qt 5.
        // and QGLFormat::openGLVersionFlags() returns the maximum known version
        pGlWidget->makeCurrent();

        QGLFormat::OpenGLVersionFlags version = QGLFormat::openGLVersionFlags();

        auto rendererString = QString();
        if (QOpenGLContext::currentContext()) {
            auto glFunctions = QOpenGLFunctions();

            glFunctions.initializeOpenGLFunctions();
            QString versionString(QLatin1String(
                    reinterpret_cast<const char*>(glFunctions.glGetString(GL_VERSION))));
            QString vendorString(QLatin1String(
                    reinterpret_cast<const char*>(glFunctions.glGetString(GL_VENDOR))));
            rendererString = QString(QLatin1String(
                    reinterpret_cast<const char*>(glFunctions.glGetString(GL_RENDERER))));

            // Either GL or GL ES Version is set, not both.
            qDebug() << QString("openGLVersionFlags 0x%1").arg(version, 0, 16) << versionString << vendorString << rendererString;
        } else {
            qDebug() << "QOpenGLContext::currentContext() returns nullptr";
            qDebug() << "pGlWidget->->windowHandle() =" << pGlWidget->windowHandle();
        }

        int majorGlVersion = 0;
        int minorGlVersion = 0;
        int majorGlesVersion = 0;
        int minorGlesVersion = 0;
        if (version == QGLFormat::OpenGL_Version_None) {
            m_openGLVersion = "None";
        } else if (version & QGLFormat::OpenGL_Version_4_3) {
            majorGlVersion = 4;
            minorGlVersion = 3;
        } else if (version & QGLFormat::OpenGL_Version_4_2) {
            majorGlVersion = 4;
            minorGlVersion = 2;
        } else if (version & QGLFormat::OpenGL_Version_4_1) {
            majorGlVersion = 4;
            minorGlVersion = 1;
        } else if (version & QGLFormat::OpenGL_Version_4_0) {
            majorGlVersion = 4;
            minorGlVersion = 0;
        } else if (version & QGLFormat::OpenGL_Version_3_3) {
            majorGlVersion = 3;
            minorGlVersion = 3;
        } else if (version & QGLFormat::OpenGL_Version_3_2) {
            majorGlVersion = 3;
            minorGlVersion = 2;
        } else if (version & QGLFormat::OpenGL_Version_3_1) {
            majorGlVersion = 3;
            minorGlVersion = 1;
        } else if (version & QGLFormat::OpenGL_Version_3_0) {
            majorGlVersion = 3;
        } else if (version & QGLFormat::OpenGL_Version_2_1) {
            majorGlVersion = 2;
            minorGlVersion = 1;
        } else if (version & QGLFormat::OpenGL_Version_2_0) {
            majorGlVersion = 2;
            minorGlVersion = 0;
        } else if (version & QGLFormat::OpenGL_Version_1_5) {
            majorGlVersion = 1;
            minorGlVersion = 5;
        } else if (version & QGLFormat::OpenGL_Version_1_4) {
            majorGlVersion = 1;
            minorGlVersion = 4;
        } else if (version & QGLFormat::OpenGL_Version_1_3) {
            majorGlVersion = 1;
            minorGlVersion = 3;
        } else if (version & QGLFormat::OpenGL_Version_1_2) {
            majorGlVersion = 1;
            minorGlVersion = 2;
        } else if (version & QGLFormat::OpenGL_Version_1_1) {
            majorGlVersion = 1;
            minorGlVersion = 1;
        } else if (version & QGLFormat::OpenGL_ES_Version_2_0) {
            m_openGLVersion = "ES 2.0";
            majorGlesVersion = 2;
            minorGlesVersion = 0;
        } else if (version & QGLFormat::OpenGL_ES_CommonLite_Version_1_1) {
            if (version & QGLFormat::OpenGL_ES_Common_Version_1_1) {
                m_openGLVersion = "ES 1.1";
            } else {
                m_openGLVersion = "ES Common Lite 1.1";
            }
            majorGlesVersion = 1;
            minorGlesVersion = 1;
        } else if (version & QGLFormat::OpenGL_ES_Common_Version_1_1) {
            m_openGLVersion = "ES Common Lite 1.1";
            majorGlesVersion = 1;
            minorGlesVersion = 1;
        } else if (version & QGLFormat::OpenGL_ES_CommonLite_Version_1_0) {
            if (version & QGLFormat::OpenGL_ES_Common_Version_1_0) {
                m_openGLVersion = "ES 1.0";
            } else {
                m_openGLVersion = "ES Common Lite 1.0";
            }
            majorGlesVersion = 1;
            minorGlesVersion = 0;
        } else if (version & QGLFormat::OpenGL_ES_Common_Version_1_0) {
            m_openGLVersion = "ES Common Lite 1.0";
            majorGlesVersion = 1;
            minorGlesVersion = 0;
        } else {
            m_openGLVersion = QString("Unknown 0x%1")
                .arg(version, 0, 16);
        }

        if (majorGlVersion != 0) {
            m_openGLVersion = QString::number(majorGlVersion) + "."
                    + QString::number(minorGlVersion);

#if !defined(QT_NO_OPENGL) && !defined(QT_OPENGL_ES_2)
            if (majorGlVersion * 100 + minorGlVersion >= 201) {
                // Qt5 requires at least OpenGL 2.1 or OpenGL ES 2.0
                m_openGlAvailable = true;
            }
#endif
        } else {
            if (majorGlesVersion * 100 + minorGlesVersion >= 200) {
                // Qt5 requires at least OpenGL 2.1 or OpenGL ES 2.0
                m_openGlesAvailable = true;
            }
        }

        m_openGLShaderAvailable =
                QGLShaderProgram::hasOpenGLShaderPrograms(
                        pGlWidget->context());

        if (!rendererString.isEmpty()) {
            m_openGLVersion += " (" + rendererString + ")";
        }

        pGlWidget->hide();
    }
#endif
    evaluateWidgets();
    m_time.start();
    m_renderGateClock.start();
    m_perfWindowTimer.start();
    if (!m_renderGateEnabled) {
        qInfo() << "DECKLAB_RENDER_GATE=0: rendering every visible waveform each frame";
    }
}

WaveformWidgetFactory::~WaveformWidgetFactory() {
    if (m_vsyncThread) {
        delete m_vsyncThread;
    }
}

bool WaveformWidgetFactory::setConfig(UserSettingsPointer config) {
    m_config = config;
    if (!m_config) {
        return false;
    }

    bool ok = false;

    int frameRate = m_config->getValue(kFrameRateKey, m_frameRate);
    m_frameRate = math_clamp(frameRate, 1, 120);

    int endTime = m_config->getValueString(kEndOfTrackWarningKey).toInt(&ok);
    if (ok) {
        setEndOfTrackWarningTime(endTime);
    } else {
        m_config->setValue(kEndOfTrackWarningKey, m_endOfTrackWarningTime);
    }

    double defaultZoom = m_config->getValueString(kDefaultZoomKey).toDouble(&ok);
    if (ok) {
        setDefaultZoom(defaultZoom);
    } else{
        m_config->setValue(kDefaultZoomKey, m_defaultZoom);
    }

    bool zoomSync = m_config->getValue(kZoomSyncKey, m_zoomSync);
    setZoomSync(zoomSync);

    int beatGridAlpha =
            m_config->getValue(ConfigKey(kWaveformGroup, QStringLiteral("beatGridAlpha")),
                    m_beatGridAlpha);
    setDisplayBeatGridAlpha(beatGridAlpha);

    WaveformWidgetType::Type type = static_cast<WaveformWidgetType::Type>(
            m_config->getValueString(kWaveformTypeKey).toInt(&ok));
    // Store the widget type on m_configType for later initialization.
    // We will initialize the objects later because of a problem with GL on QT 5.14.2 on Windows
    if (!ok || !setWidgetType(type, &m_configType)) {
        setWidgetType(WaveformWidgetType::RGB, &m_configType);
    }

    for (int i = 0; i < BandCount; i++) {
        m_visualGain[i] = m_config->getValue(visualGainKey(i), kVisualGainDefault[i]);
    }
    m_overviewNormalized = m_config->getValue(
            ConfigKey(kWaveformGroup, QStringLiteral("OverviewNormalized")),
            kOverviewNormalizedDefault);

    emit visualGainChanged(
            m_visualGain[BandIndex::AllBand],
            m_visualGain[BandIndex::Low],
            m_visualGain[BandIndex::Mid],
            m_visualGain[BandIndex::High]);
    emit overviewScalingChanged();

    m_playMarkerPosition =
            m_config->getValue(ConfigKey(kWaveformGroup, QStringLiteral("PlayMarkerPosition")),
                    WaveformWidgetRenderer::s_defaultPlayMarkerPosition);
    setPlayMarkerPosition(m_playMarkerPosition);

    int untilMarkShowBeats =
            m_config->getValueString(
                            ConfigKey(kWaveformGroup, QStringLiteral("UntilMarkShowBeats")))
                    .toInt(&ok);
    if (ok) {
        setUntilMarkShowBeats(static_cast<bool>(untilMarkShowBeats));
    } else {
        m_config->setValue(ConfigKey(kWaveformGroup, QStringLiteral("UntilMarkShowBeats")),
                m_untilMarkShowBeats);
    }
    int untilMarkShowTime =
            m_config->getValueString(
                            ConfigKey(kWaveformGroup, QStringLiteral("UntilMarkShowTime")))
                    .toInt(&ok);
    if (ok) {
        setUntilMarkShowTime(static_cast<bool>(untilMarkShowTime));
    } else {
        m_config->setValue(ConfigKey(kWaveformGroup, QStringLiteral("UntilMarkShowTime")),
                m_untilMarkShowTime);
    }

    setUntilMarkAlign(toUntilMarkAlign(
            m_config->getValue(ConfigKey(kWaveformGroup, QStringLiteral("UntilMarkAlign")),
                    toUntilMarkAlignIndex(m_untilMarkAlign))));
    setUntilMarkTextPointSize(m_config->getValue(
            ConfigKey(kWaveformGroup, QStringLiteral("UntilMarkTextPointSize")),
            m_untilMarkTextPointSize));
    setUntilMarkTextHeightLimit(toUntilMarkTextHeightLimit(m_config->getValue(
            ConfigKey(kWaveformGroup, QStringLiteral("UntilMarkTextHeightLimit")),
            toUntilMarkTextHeightLimitIndex(m_untilMarkTextHeightLimit))));
    setStemReorderOnChange(m_config->getValue(
            ConfigKey(kWaveformGroup, QStringLiteral("stem_reorder_on_change")),
            true));
    setStemOpacity(static_cast<float>(
            m_config->getValue(ConfigKey(kWaveformGroup, QStringLiteral("stem_opacity")),
                    0.75)));
    setStemOutlineOpacity(static_cast<float>(
            m_config->getValue(ConfigKey(kWaveformGroup, QStringLiteral("stem_outline_opacity")),
                    0.15)));
    setStemSplitTracks(m_config->getValue(
            ConfigKey(kWaveformGroup, QStringLiteral("stem_split_tracks")),
            false));

    return true;
}

void WaveformWidgetFactory::destroyWidgets() {
    for (auto& holder : m_waveformWidgetHolders) {
        WaveformWidgetAbstract* pWidget = holder.m_waveformWidget;
        holder.m_waveformWidget = nullptr;
        delete pWidget;
    }
    m_waveformWidgetHolders.clear();
}

void WaveformWidgetFactory::addVuMeter(WVuMeterLegacy* pVuMeter) {
    // Do not hold the pointer to of timer listeners since they may be deleted.
    // We don't activate update() or repaint() directly so listener widgets
    // can decide whether to paint or not.
    connect(this,
            &WaveformWidgetFactory::waveformUpdateTick,
            pVuMeter,
            &WVuMeterLegacy::maybeUpdate,
            Qt::DirectConnection);
}

void WaveformWidgetFactory::addVuMeter(WVuMeterBase* pVuMeter) {
    // WVuMeterGLs to be rendered and swapped from the vsync thread
    connect(this,
            &WaveformWidgetFactory::renderVuMeters,
            pVuMeter,
            &WVuMeterBase::render);
    connect(this,
            &WaveformWidgetFactory::swapVuMeters,
            pVuMeter,
            &WVuMeterBase::swap);
}

void WaveformWidgetFactory::slotSkinLoaded() {
    setWidgetTypeFromConfig();
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0) && defined __WINDOWS__
    // This regenerates the waveforms twice because of a bug found on Windows
    // where the first one fails.
    // The problem is that the window of the widget thinks that it is not exposed.
    // (https://doc.qt.io/qt-5/qwindow.html#exposeEvent )
    setWidgetTypeFromConfig();
#endif
}

bool WaveformWidgetFactory::setWaveformWidget(WWaveformViewer* viewer,
                                              const QDomElement& node,
                                              const SkinContext& parentContext) {
    int index = findIndexOf(viewer);
    if (index != -1) {
        qDebug() << "WaveformWidgetFactory::setWaveformWidget - "\
                    "viewer already have a waveform widget but it's not found by the factory !";
        delete viewer->getWaveformWidget();
    }

    // Cast to widget done just after creation because it can't be perform in
    // constructor (pure virtual)
    WaveformWidgetAbstract* waveformWidget = createWaveformWidget(m_type, viewer);
    viewer->setWaveformWidget(waveformWidget);
    viewer->setup(node, parentContext);

    // create new holder
    WaveformWidgetHolder holder(waveformWidget, viewer, node, &parentContext);
    holder.m_renderGate.setEnabled(m_renderGateEnabled);
    // Touch/mouse input on the viewer forces a frame (scratch, mark hover).
    viewer->installEventFilter(this);
    if (index == -1) {
        // add holder
        m_waveformWidgetHolders.push_back(std::move(holder));
        index = static_cast<int>(m_waveformWidgetHolders.size()) - 1;
    } else {
        // update holder
        DEBUG_ASSERT(index >= 0);
        m_waveformWidgetHolders[index] = std::move(holder);
    }

    viewer->setZoom(m_defaultZoom);
    viewer->setDisplayBeatGridAlpha(m_beatGridAlpha);
    viewer->setPlayMarkerPosition(m_playMarkerPosition);
    waveformWidget->resize(viewer->width(), viewer->height());
    waveformWidget->getWidget()->show();
    viewer->update();

    qDebug() << "WaveformWidgetFactory::setWaveformWidget - waveform widget added in factory, index" << index;

    return true;
}

void WaveformWidgetFactory::setFrameRate(int frameRate) {
    m_frameRate = math_clamp(frameRate, 1, 120);
    if (m_config) {
        m_config->setValue(kFrameRateKey, m_frameRate);
    }
    if (m_vsyncThread) {
        m_vsyncThread->setSyncIntervalTimeMicros(static_cast<int>(1e6 / m_frameRate));
    }
}

void WaveformWidgetFactory::setEndOfTrackWarningTime(int endTime) {
    m_endOfTrackWarningTime = endTime;
    if (m_config) {
        m_config->setValue(kEndOfTrackWarningKey, m_endOfTrackWarningTime);
    }
}

bool WaveformWidgetFactory::setWidgetType(WaveformWidgetType::Type type) {
    return setWidgetType(type, &m_type);
}

bool WaveformWidgetFactory::setWidgetType(
        WaveformWidgetType::Type type,
        WaveformWidgetType::Type* pCurrentType) {
    if (type == *pCurrentType) {
        return true;
    }

    // check if type is acceptable
    int index = findHandleIndexFromType(type);
    bool isAcceptable = index > -1;
    *pCurrentType = isAcceptable ? type : WaveformWidgetType::Empty;
    if (m_config) {
        m_configType = *pCurrentType;
        // TODO do not set "Empty"?
        m_config->setValue(kWaveformTypeKey, *pCurrentType);
    }
    return isAcceptable;
}

bool WaveformWidgetFactory::widgetTypeSupportsUntilMark() const {
    switch (m_configType) {
    case WaveformWidgetType::RGB:
    case WaveformWidgetType::Filtered:
    case WaveformWidgetType::Simple:
    case WaveformWidgetType::HSV:
    case WaveformWidgetType::Stacked:
        return true;
    default:
        break;
    }
    return false;
}

bool WaveformWidgetFactory::widgetTypeSupportsStems() const {
    switch (m_configType) {
    case WaveformWidgetType::RGB:
    case WaveformWidgetType::Filtered:
    case WaveformWidgetType::Simple:
    case WaveformWidgetType::HSV:
    case WaveformWidgetType::Stacked:
        return true;
    default:
        break;
    }
    return false;
}

/// Called by MixxxMainWindow after skin loaded via slotSkinLoaded
bool WaveformWidgetFactory::setWidgetTypeFromConfig() {
    int empty = findHandleIndexFromType(WaveformWidgetType::Empty);
    int desired = findHandleIndexFromType(m_configType);
    if (desired == -1) {
        qDebug() << "WaveformWidgetFactory::setWidgetTypeFromConfig"
                 << " - configured type" << static_cast<int>(m_configType)
                 << "not found -- using 'EmptyWaveform'";
        desired = empty;
    }

    // Because of a previous bug, "use_hardware_acceleration" may be 0
    // which prevents loading the desired type.
    // Fix: enable acceleration if available and if type requires it.
    auto backend = getBackendFromConfig();
    // True if type and available backends support acceleration
    bool typeSupportsAcceleration = widgetTypeSupportsAcceleration(m_configType);
    bool typeSupportsSoftware = widgetTypeSupportsSoftware(m_configType);
    if (backend == WaveformWidgetBackend::None &&
            typeSupportsAcceleration && !typeSupportsSoftware) {
        qDebug() << "WaveformWidgetFactory::setWidgetTypeFromConfig"
                 << " - select accelerated backend because configured type"
                 << WaveformWidgetAbstractHandle::getDisplayName(m_configType)
                 << "requires it.";
        setDefaultBackend();
    }

    return setWidgetTypeFromHandle(desired, true);
}

bool WaveformWidgetFactory::setWidgetTypeFromHandle(int handleIndex, bool force) {
    if (handleIndex < 0 || handleIndex >= m_waveformWidgetHandles.size()) {
        qDebug() << "WaveformWidgetFactory::setWidgetTypeFromHandle"
                    " - invalid handle --> using 'EmptyWaveform'";
        // fallback empty type
        setWidgetType(WaveformWidgetType::Empty);
        return false;
    }

    WaveformWidgetAbstractHandle& handle = m_waveformWidgetHandles[handleIndex];
    if (handle.m_type == m_type && !force) {
        qDebug() << "WaveformWidgetFactory::setWidgetTypeFromHandle - type"
                 << handle.getDisplayName() << "already in use";
        return true;
    }

    // change the type
    setWidgetType(handle.m_type);

    m_skipRender = true;

    //re-create/setup all waveform widgets
    for (auto& holder : m_waveformWidgetHolders) {
        WaveformWidgetAbstract* previousWidget = holder.m_waveformWidget;
        TrackPointer pTrack = previousWidget->getTrackInfo();
        //previousWidget->hold();
        double previousZoom = previousWidget->getZoom();
        double previousPlayMarkerPosition = previousWidget->getPlayMarkerPosition();
        int previousbeatgridAlpha = previousWidget->getBeatGridAlpha();
        delete previousWidget;
        WWaveformViewer* viewer = holder.m_waveformViewer;
        WaveformWidgetAbstract* widget = createWaveformWidget(m_type, holder.m_waveformViewer);
        holder.m_waveformWidget = widget;
        holder.m_renderGate = RenderGate();
        holder.m_renderGate.setEnabled(m_renderGateEnabled);
        holder.m_pendingSwap = false;
        viewer->setWaveformWidget(widget);
        viewer->setup(holder.m_skinNodeCache, holder.m_skinContextCache);
        viewer->setZoom(previousZoom);
        viewer->setPlayMarkerPosition(previousPlayMarkerPosition);
        viewer->setDisplayBeatGridAlpha(previousbeatgridAlpha);
        // resize() doesn't seem to get called on the widget. I think Qt skips
        // it since the size didn't change.
        //viewer->resize(viewer->size());
        widget->resize(viewer->width(), viewer->height());
        widget->setTrack(pTrack);
        widget->getWidget()->show();
        viewer->update();
    }

    m_skipRender = false;
    return true;
}

void WaveformWidgetFactory::setDefaultZoom(double zoom) {
    m_defaultZoom = math_clamp(zoom, WaveformWidgetRenderer::s_waveformMinZoom,
                               WaveformWidgetRenderer::s_waveformMaxZoom);
    if (m_config) {
        m_config->setValue(kDefaultZoomKey, m_defaultZoom);
    }

    for (const auto& holder : std::as_const(m_waveformWidgetHolders)) {
        holder.m_waveformViewer->setZoom(m_defaultZoom);
    }
}

void WaveformWidgetFactory::setZoomSync(bool sync) {
    m_zoomSync = sync;
    if (m_config) {
        m_config->setValue(kZoomSyncKey, m_zoomSync);
    }

    if (m_waveformWidgetHolders.size() == 0) {
        return;
    }

    double refZoom = m_waveformWidgetHolders[0].m_waveformWidget->getZoom();
    for (const auto& holder : std::as_const(m_waveformWidgetHolders)) {
        holder.m_waveformViewer->setZoom(refZoom);
    }
}

void WaveformWidgetFactory::setDisplayBeatGridAlpha(int alpha) {
    m_beatGridAlpha = alpha;
    if (m_waveformWidgetHolders.size() == 0) {
        return;
    }

    for (const auto& holder : std::as_const(m_waveformWidgetHolders)) {
        holder.m_waveformWidget->setDisplayBeatGridAlpha(m_beatGridAlpha);
    }
}

void WaveformWidgetFactory::setVisualGain(BandIndex index, double gain) {
    m_visualGain[index] = gain;
    if (m_config) {
        m_config->setValue(visualGainKey(index), m_visualGain[index]);
    }
    emit visualGainChanged(
            m_visualGain[BandIndex::AllBand],
            m_visualGain[BandIndex::Low],
            m_visualGain[BandIndex::Mid],
            m_visualGain[BandIndex::High]);
    if (index == BandIndex::AllBand && !m_overviewNormalized) {
        emit overviewScalingChanged();
    }
    requestRenderAll();
}

double WaveformWidgetFactory::getVisualGain(BandIndex index) const {
    return m_visualGain[index];
}

// static
double WaveformWidgetFactory::getVisualGainDefault(BandIndex index) {
    return kVisualGainDefault[index];
}

void WaveformWidgetFactory::setOverviewNormalized(bool normalize) {
    m_overviewNormalized = normalize;
    if (m_config) {
        m_config->set(ConfigKey(kWaveformGroup, QStringLiteral("OverviewNormalized")),
                ConfigValue(m_overviewNormalized));
    }
    emit overviewScalingChanged();
}

// static
bool WaveformWidgetFactory::isOverviewNormalizedDefault() {
    return kOverviewNormalizedDefault;
}

void WaveformWidgetFactory::setPlayMarkerPosition(double position) {
    m_playMarkerPosition = position;
    if (m_config) {
        m_config->setValue(ConfigKey(kWaveformGroup, QStringLiteral("PlayMarkerPosition")),
                m_playMarkerPosition);
    }

    for (const auto& holder : std::as_const(m_waveformWidgetHolders)) {
        holder.m_waveformWidget->setPlayMarkerPosition(m_playMarkerPosition);
    }
}

void WaveformWidgetFactory::notifyZoomChange(WWaveformViewer* viewer) {
    WaveformWidgetAbstract* pWaveformWidget = viewer->getWaveformWidget();
    if (pWaveformWidget == nullptr || !isZoomSync()) {
        return;
    }
    double refZoom = pWaveformWidget->getZoom();

    for (const auto& holder : std::as_const(m_waveformWidgetHolders)) {
        if (holder.m_waveformViewer != viewer) {
            holder.m_waveformViewer->setZoom(refZoom);
        }
    }
}

void WaveformWidgetFactory::renderSelf() {
    ScopedTimer t(QStringLiteral("WaveformWidgetFactory::render() %1waveforms"),
            static_cast<int>(m_waveformWidgetHolders.size()));
    if (m_perfLogEnabled) {
        m_perfSlotTimer.start();
    }

    m_renderPassActive = false;
    if (!m_skipRender) {
        if (m_type) {   // no regular updates for an empty waveform
            // next rendered frame is displayed after next buffer swap and than after VSync
            QVarLengthArray<bool, 10> shouldRenderWaveforms(
                    static_cast<int>(m_waveformWidgetHolders.size()));
            const qint64 nowMillis = m_renderGateClock.elapsed().toIntegerMillis();
            for (decltype(m_waveformWidgetHolders)::size_type i = 0;
                    i < m_waveformWidgetHolders.size();
                    i++) {
                WaveformWidgetHolder& holder = m_waveformWidgetHolders[i];
                WaveformWidgetAbstract* pWaveformWidget = holder.m_waveformWidget;
                // Render gate: render (and later swap) only when this frame
                // can differ from the last one swapped. Paused, unchanged
                // waveforms keep their last frame on screen.
                RenderGate::Frame frame;
                frame.nowMillis = nowMillis;
                // Don't bother doing the pre-render work if we aren't going to
                // render this widget.
                frame.visible = shouldRenderWaveform(pWaveformWidget);
                if (frame.visible) {
                    // Calculate play position for the new Frame in following run
                    pWaveformWidget->preRender(m_vsyncThread);
                    // Consume both requests; neither may be short-circuited.
                    WGLWidget* pGlw = pWaveformWidget->getGLWidget();
                    const bool exposed = pGlw != nullptr && pGlw->takeGLRenderRequest();
                    const bool requested = pWaveformWidget->takeRenderRequest();
                    frame.forced = exposed || requested;
                    if (holder.m_renderGate.isEnabled()) {
                        frame.moving = pWaveformWidget->needsContinuousRender();
                        frame.stateKey = pWaveformWidget->renderStateKey();
                    }
                }
                const RenderGate::Decision decision = holder.m_renderGate.evaluate(frame);
                shouldRenderWaveforms[static_cast<int>(i)] = RenderGate::renders(decision);
                // Safety refreshes are single frames; they need no vblank lock.
                if (decision == RenderGate::Decision::Forced ||
                        decision == RenderGate::Decision::Moving ||
                        decision == RenderGate::Decision::Changed ||
                        decision == RenderGate::Decision::Ungated) {
                    m_renderPassActive = true;
                }
            }
            //qDebug() << "prerender" << m_vsyncThread->elapsed();

            // It may happen that there is an artificially delayed due to
            // anti tearing driver settings
            // all render commands are delayed until the swap from the previous run is executed
            for (decltype(m_waveformWidgetHolders)::size_type i = 0;
                    i < m_waveformWidgetHolders.size();
                    i++) {
                WaveformWidgetHolder& holder = m_waveformWidgetHolders[i];
                WaveformWidgetAbstract* pWaveformWidget = holder.m_waveformWidget;
                if (!shouldRenderWaveforms[static_cast<int>(i)]) {
                    continue;
                }
                pWaveformWidget->render();
                holder.m_pendingSwap = true;
                //qDebug() << "render" << i << m_vsyncThread->elapsed();
            }
        }

        // WSpinnys are also double-buffered WGLWidgets, like all the waveform
        // renderers. Render all the WSpinny widgets now.
        emit renderSpinnies(m_vsyncThread);
        // Same for WVuMeterGL. Note that we are either using WVuMeter or WVuMeterGL.
        // If we are using WVuMeter, this does nothing
        emit renderVuMeters(m_vsyncThread);

        // Notify all other waveform-like widgets (e.g. WSpinny's) that they should
        // update.
        //int t1 = m_vsyncThread->elapsed();
        emit waveformUpdateTick();
        //qDebug() << "emit" << m_vsyncThread->elapsed() - t1;

        m_frameCnt += 1.0f;
        mixxx::Duration timeCnt = m_time.elapsed();
        if (timeCnt > mixxx::Duration::fromSeconds(1)) {
            m_time.start();
            m_frameCnt = m_frameCnt * 1000 / timeCnt.toIntegerMillis(); // latency correction
            emit waveformMeasured(m_frameCnt, m_vsyncThread->droppedFrames());
            m_frameCnt = 0.0;
        }
    }

    m_pVisualsManager->process(m_endOfTrackWarningTime);
    m_pGuiTick->process();

    if (m_perfLogEnabled) {
        m_perfTickNanos += m_perfSlotTimer.elapsed().toIntegerNanos();
    }
    //qDebug() << "refresh end" << m_vsyncThread->elapsed();
}

void WaveformWidgetFactory::render() {
    renderSelf();
    m_vsyncThread->vsyncSlotFinished();
}

void WaveformWidgetFactory::swapSelf() {
    ScopedTimer t(QStringLiteral("WaveformWidgetFactory::swap() %1waveforms"),
            static_cast<int>(m_waveformWidgetHolders.size()));
    if (m_perfLogEnabled) {
        m_perfSlotTimer.start();
        // Frame pacing as the GUI thread sees it: the swap slot should start
        // once per frame interval. Late slots show up as long intervals.
        if (m_perfSwapTimerStarted) {
            m_perfSwapIntervals.push_back(m_perfSwapTimer.restart().toIntegerNanos());
        } else {
            m_perfSwapTimer.start();
            m_perfSwapTimerStarted = true;
        }
    }

    // Do this in an extra slot to be sure to hit the desired interval
    if (!m_skipRender) {
        if (m_type) {   // no regular updates for an empty waveform
            // Show rendered buffer from last render() run
            //qDebug() << "swap() start" << m_vsyncThread->elapsed();
            for (auto& holder : m_waveformWidgetHolders) {
                // Swap only what the last render pass drew. Skipped widgets
                // keep showing their previous front buffer.
                if (!holder.m_pendingSwap) {
                    continue;
                }
                holder.m_pendingSwap = false;
                WaveformWidgetAbstract* pWaveformWidget = holder.m_waveformWidget;

                // Don't swap invalid / invisible widgets or widgets with an
                // unexposed window. Prevents continuous log spew of
                // "QOpenGLContext::swapBuffers() called with non-exposed
                // window, behavior is undefined" on Qt5. See issue #9360.
                if (!shouldRenderWaveform(pWaveformWidget)) {
                    continue;
                }
                WGLWidget* pGlw = pWaveformWidget->getGLWidget();
                if (pGlw != nullptr) {
                    pGlw->makeCurrentIfNeeded();
                    pGlw->swapBuffers();
                    pGlw->doneCurrent();
                }
                //qDebug() << "swap x" << m_vsyncThread->elapsed();
            }
        }
        // WSpinnys are also double-buffered QGLWidgets, like all the waveform
        // renderers. Swap all the WSpinny widgets now.
        emit swapSpinnies();
        // Same for WVuMeterGL. Note that we are either using WVuMeter or WVuMeterGL
        // If we are using WVuMeter, this does nothing
        emit swapVuMeters();
    }
    if (m_perfLogEnabled) {
        finishPerfTick(m_perfSlotTimer.elapsed().toIntegerNanos());
    }
}

void WaveformWidgetFactory::requestRenderAll() {
    for (auto& holder : m_waveformWidgetHolders) {
        holder.m_renderGate.requestRender();
    }
}

bool WaveformWidgetFactory::eventFilter(QObject* pWatched, QEvent* pEvent) {
    if (pWatched != nullptr && pWatched == m_pPllKeepaliveWindow) {
        // A paint of the shared GL window starts here and ends with its
        // frameSwapped (Deck Lab idle-aware PLL).
        if (pEvent->type() == QEvent::UpdateRequest || pEvent->type() == QEvent::Paint) {
            m_pllKeepalivePaintTimer.start();
            m_pllKeepalivePaintStarted = true;
        }
        return QObject::eventFilter(pWatched, pEvent);
    }
    switch (pEvent->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
    case QEvent::Wheel:
    case QEvent::Enter:
    case QEvent::Leave:
    case QEvent::Show:
    case QEvent::Resize:
        for (auto& holder : m_waveformWidgetHolders) {
            if (holder.m_waveformViewer == pWatched) {
                holder.m_renderGate.requestRender();
                break;
            }
        }
        break;
    default:
        break;
    }
    return QObject::eventFilter(pWatched, pEvent);
}

void WaveformWidgetFactory::finishPerfTick(qint64 swapNanos) {
    // One render + swap pair is one tick of the GUI-thread render loop.
    const qint64 tickNanos = m_perfTickNanos + swapNanos;
    m_perfTickNanos = 0;
    ++m_perfTicks;
    m_perfTickNanosSum += tickNanos;
    m_perfTickNanosMax = std::max(m_perfTickNanosMax, tickNanos);

    constexpr qint64 kPerfWindowMillis = 10000;
    if (m_perfWindowTimer.elapsed().toIntegerMillis() < kPerfWindowMillis) {
        return;
    }
    m_perfWindowTimer.start();
    for (auto& holder : m_waveformWidgetHolders) {
        const RenderGate::Stats& stats = holder.m_renderGate.stats();
        const QString group = holder.m_waveformWidget
                ? holder.m_waveformWidget->getGroup()
                : QStringLiteral("?");
        qInfo().noquote()
                << QStringLiteral(
                           "DECKLAB_PERF waveform %1 rendered=%2 "
                           "(forced=%3 moving=%4 changed=%5 safety=%6 "
                           "ungated=%7) skipped=%8 hidden=%9")
                           .arg(group,
                                   QString::number(stats.rendered()),
                                   QString::number(stats.forced),
                                   QString::number(stats.moving),
                                   QString::number(stats.changed),
                                   QString::number(stats.safety),
                                   QString::number(stats.ungated),
                                   QString::number(stats.skipped),
                                   QString::number(stats.hidden));
        holder.m_renderGate.resetStats();
        if (holder.m_waveformWidget) {
            const QString renderers = holder.m_waveformWidget->takePerfReport();
            if (!renderers.isEmpty()) {
                // CPU per rendered frame, mean/p95 in ms, per renderer.
                qInfo().noquote() << QStringLiteral("DECKLAB_PERF renderers %1 %2")
                                             .arg(group, renderers);
            }
        }
    }
    logVSyncPerf();
    {
        // Frame pacing over the window: mean, 95th percentile and maximum of
        // the swap slot intervals, and how many exceeded 1.5 frame periods.
        const qint64 nominalNanos = m_frameRate > 0 ? 1000000000LL / m_frameRate : 0;
        qint64 sum = 0;
        qint64 maxInterval = 0;
        int late = 0;
        for (const qint64 interval : m_perfSwapIntervals) {
            sum += interval;
            maxInterval = std::max(maxInterval, interval);
            if (nominalNanos > 0 && interval * 2 > nominalNanos * 3) {
                ++late;
            }
        }
        const auto count = static_cast<qint64>(m_perfSwapIntervals.size());
        const qint64 p95 = RendererTimings::percentile95(m_perfSwapIntervals, &m_perfScratch);
        qInfo().noquote()
                << QStringLiteral(
                           "DECKLAB_PERF pacing mode=%1 fps=%2 intervals=%3 "
                           "mean=%4ms p95=%5ms max=%6ms late=%7")
                           .arg(QString::number(m_vsyncThread
                                                ? static_cast<int>(
                                                          m_vsyncThread->vsyncMode())
                                                : -1),
                                   QString::number(m_frameRate),
                                   QString::number(count),
                                   QString::number(count ? static_cast<double>(sum) /
                                                           static_cast<double>(count) / 1e6
                                                         : 0.0,
                                           'f',
                                           3),
                                   QString::number(p95 > 0 ? static_cast<double>(p95) / 1e6
                                                           : 0.0,
                                           'f',
                                           3),
                                   QString::number(static_cast<double>(maxInterval) / 1e6,
                                           'f',
                                           3),
                                   QString::number(late));
        m_perfSwapIntervals.clear();
    }
    const int droppedFrames = m_vsyncThread ? m_vsyncThread->droppedFrames() : 0;
    const double meanMillis = m_perfTicks
            ? static_cast<double>(m_perfTickNanosSum) /
                    static_cast<double>(m_perfTicks) / 1e6
            : 0.0;
    const double maxMillis = static_cast<double>(m_perfTickNanosMax) / 1e6;
    qInfo().noquote()
            << QStringLiteral(
                       "DECKLAB_PERF render-loop ticks=%1 mean=%2ms max=%3ms "
                       "dropped=%4 gate=%5")
                       .arg(QString::number(m_perfTicks),
                               QString::number(meanMillis, 'f', 3),
                               QString::number(maxMillis, 'f', 3),
                               QString::number(droppedFrames - m_perfDroppedFramesBaseline),
                               m_renderGateEnabled ? QStringLiteral("on")
                                                   : QStringLiteral("off"));
    m_perfDroppedFramesBaseline = droppedFrames;
    m_perfTicks = 0;
    m_perfTickNanosSum = 0;
    m_perfTickNanosMax = 0;
}

void WaveformWidgetFactory::logVSyncPerf() {
    if (!m_vsyncThread) {
        return;
    }
    const VSyncThread::PllSnapshot snapshot = m_vsyncThread->pllSnapshot();
    if (!m_pllKeepaliveEnabled && !snapshot.fellBack) {
        return; // not ST_PLL
    }
    const mixxx::decklab::VSyncPll::Stats& pll = snapshot.stats;
    const mixxx::decklab::VSyncPll::Stats& pllBase = m_perfPllStatsBaseline;
    const mixxx::decklab::VSyncKeepalive::Stats& keep = m_pllKeepalive.stats();
    const mixxx::decklab::VSyncKeepalive::Stats& keepBase = m_perfKeepaliveStatsBaseline;
    QString state;
    if (snapshot.fellBack) {
        state = QStringLiteral("fallback");
    } else if (snapshot.initializing) {
        state = QStringLiteral("init");
    } else if (m_pllKeepalive.running()) {
        state = QStringLiteral("locked");
    } else {
        state = QStringLiteral("freewheel");
    }
    // Per 10 s window: referenced = vblank samples the loop used,
    // unreferenced = paints that did not wait (ignored), relocks/snaps =
    // resumes after a gap (snaps: the slots had to cross the vblank),
    // skipped = slots moved on so that no vblank interval got two,
    // paints/refills = keepalive repaints per slot / to refill the swap
    // queue, starts/stops = keepalive runs (refreshes: idle phase checks).
    qInfo().noquote()
            << QStringLiteral(
                       "DECKLAB_PERF vsync mode=%1 state=%2 period=%3us rate=%4Hz "
                       "err=%5us referenced=%6 unreferenced=%7 duplicates=%8 "
                       "relocks=%9 snaps=%10 skipped=%11 keepalive=%12 paints=%13 "
                       "refills=%14 starts=%15 stops=%16 refreshes=%17")
                       .arg(vsyncModeName(m_vsyncThread->vsyncMode()),
                               state,
                               QString::number(snapshot.periodMicros, 'f', 2),
                               QString::number(snapshot.periodMicros > 0
                                               ? 1e6 / snapshot.periodMicros
                                               : 0.0,
                                       'f',
                                       4),
                               QString::number(std::llround(snapshot.lastPhaseErrorMicros)),
                               QString::number((pll.tracked + pll.relocks) -
                                       (pllBase.tracked + pllBase.relocks)),
                               QString::number(pll.unreferenced - pllBase.unreferenced),
                               QString::number(pll.duplicates - pllBase.duplicates),
                               QString::number(pll.relocks - pllBase.relocks))
                       .arg(QString::number(pll.snaps - pllBase.snaps),
                               QString::number(snapshot.skippedSlots - m_perfSkippedSlotsBaseline),
                               m_pllKeepalive.running() ? QStringLiteral("on")
                                                        : QStringLiteral("off"),
                               QString::number(keep.slotPaints - keepBase.slotPaints),
                               QString::number(keep.refillPaints - keepBase.refillPaints),
                               QString::number(keep.starts - keepBase.starts),
                               QString::number(keep.stops - keepBase.stops),
                               QString::number(keep.refreshes - keepBase.refreshes));
    m_perfPllStatsBaseline = pll;
    m_perfKeepaliveStatsBaseline = keep;
    m_perfSkippedSlotsBaseline = snapshot.skippedSlots;
}

void WaveformWidgetFactory::swap() {
    swapSelf();
    m_vsyncThread->vsyncSlotFinished();
}

void WaveformWidgetFactory::swapAndRender() {
    // used for PLL
    m_vsyncThread->markSlotStart();
    // Deck Lab: repaint the shared window only while something moves (or for
    // 500 ms after). Idle, the loop freewheels on its measured period and the
    // GUI thread, X server and GPU do no work for it. Upstream repainted every
    // slot.
    if (m_pllKeepaliveEnabled &&
            m_pllKeepalive.onSlot(m_renderGateClock.elapsed().toIntegerMillis(),
                    m_renderPassActive || hasUngatedVSyncWidgets())) {
        requestPllKeepalivePaint();
    }

    swapSelf();
    renderSelf();

    m_vsyncThread->vsyncSlotFinished();
}

void WaveformWidgetFactory::slotFrameSwapped() {
#ifdef MIXXX_USE_QOPENGL
    if (!m_pllKeepaliveEnabled || !m_vsyncThread) {
        return;
    }
    // Deck Lab: only a paint that waited in the driver for a vblank (its swap
    // queue was full) tells where the vblank is. See waveform/vsyncpll.h.
    const bool blocked = m_pllKeepalivePaintStarted &&
            m_pllKeepalivePaintTimer.elapsed().toIntegerMicros() >=
                    kPllKeepaliveBlockedMicros;
    m_pllKeepalivePaintStarted = false;
    m_pllLockWatchdog.onFrameSwapped();
    // update the phase-locked-loop
    m_vsyncThread->updatePLL(blocked);
    if (m_vsyncThread->pllInitializing()) {
        // continuously trigger redraws during PLL init
        requestPllKeepalivePaint();
    } else if (m_pllKeepalive.onFrameSwapped(blocked)) {
        // This paint did not wait, so the queue is not full: paint again now.
        requestPllKeepalivePaint();
    }
#endif
}

void WaveformWidgetFactory::requestPllKeepalivePaint() {
#ifdef MIXXX_USE_QOPENGL
    WGLWidget* widget = SharedGLContext::getWidget();
    if (widget && widget->getOpenGLWindow()) {
        widget->getOpenGLWindow()->update();
    }
#endif
}

bool WaveformWidgetFactory::hasUngatedVSyncWidgets() const {
    static const QMetaMethod renderSpinniesSignal =
            QMetaMethod::fromSignal(&WaveformWidgetFactory::renderSpinnies);
    static const QMetaMethod renderVuMetersSignal =
            QMetaMethod::fromSignal(&WaveformWidgetFactory::renderVuMeters);
    return isSignalConnected(renderSpinniesSignal) || isSignalConnected(renderVuMetersSignal);
}

void WaveformWidgetFactory::slotPllLockWatchdog() {
    if (!m_vsyncThread || !m_pPllLockWatchdogTimer) {
        return;
    }
    const qint64 elapsedMillis = m_pllLockWatchdogClock.restart().toIntegerMillis();
    bool exposed = false;
#ifdef MIXXX_USE_QOPENGL
    if (auto* pWindow = qobject_cast<QWindow*>(m_pPllKeepaliveWindow)) {
        exposed = pWindow->isExposed();
    }
#endif
    const auto state = m_pllLockWatchdog.tick(
            elapsedMillis, exposed, m_vsyncThread->pllInitializing());
    if (state == mixxx::decklab::VSyncLockWatchdog::State::Waiting) {
        return;
    }
    m_pPllLockWatchdogTimer->stop();
    if (state == mixxx::decklab::VSyncLockWatchdog::State::Locked) {
        const VSyncThread::PllSnapshot snapshot = m_vsyncThread->pllSnapshot();
        qInfo().noquote() << QStringLiteral(
                "DECKLAB_VSYNC pll locked period=%1us rate=%2Hz after %3 ms "
                "(%4 frameSwapped)")
                                     .arg(QString::number(snapshot.periodMicros, 'f', 2),
                                             QString::number(snapshot.periodMicros > 0
                                                             ? 1e6 / snapshot.periodMicros
                                                             : 0.0,
                                                     'f',
                                                     4),
                                             QString::number(m_pllLockWatchdog.totalMillis()),
                                             QString::number(m_pllLockWatchdog.framesSwapped()));
        return;
    }
    m_pllKeepaliveEnabled = false;
    m_vsyncThread->fallBackToTimer();
    qWarning().noquote() << QStringLiteral(
            "DECKLAB_VSYNC pll did not lock within %1 ms (%2 frameSwapped, "
            "window exposed for %3 ms): falling back to timer mode, swap "
            "interval stays %4")
                                    .arg(QString::number(m_pllLockWatchdog.totalMillis()),
                                            QString::number(m_pllLockWatchdog.framesSwapped()),
                                            QString::number(m_pllLockWatchdog.exposedMillis()),
                                            QString::number(getSurfaceFormat().swapInterval()));
}

void WaveformWidgetFactory::addHandle(
        QHash<WaveformWidgetType::Type, QList<WaveformWidgetBackend>>&
                collectedHandles,
        WaveformWidgetType::Type type,
        const WaveformWidgetVars& vars) const {
    WaveformWidgetBackend backend = WaveformWidgetBackend::None;
    bool active = true;
    if (isOpenGlAvailable()) {
        if (vars.m_useGLES && !vars.m_useGL) {
            active = false;
        } else if (vars.m_useGLSL && !isOpenGlShaderAvailable()) {
            active = false;
        }
    } else if (isOpenGlesAvailable()) {
        if (vars.m_useGL && !vars.m_useGLES) {
            active = false;
        } else if (vars.m_useGLSL && !isOpenGlShaderAvailable()) {
            active = false;
        }
    } else {
        // No sufficient GL support
        if (vars.m_useGLES || vars.m_useGL || vars.m_useGLSL) {
            active = false;
        }
    }

    if (vars.m_category == WaveformWidgetCategory::DeveloperOnly &&
            !CmdlineArgs::Instance().getDeveloper()) {
        active = false;
    }
#ifdef MIXXX_USE_QOPENGL
    else if (vars.m_category == WaveformWidgetCategory::AllShader) {
        backend = WaveformWidgetBackend::AllShader;
    }
#endif
    else if (vars.m_category == WaveformWidgetCategory::Legacy && vars.m_useGLSL) {
        backend = WaveformWidgetBackend::GLSL;
    } else if (vars.m_category == WaveformWidgetCategory::Legacy) {
        backend = WaveformWidgetBackend::GL;
    }

    if (active) {
        if (collectedHandles.contains(type)) {
            collectedHandles[type].push_back(backend);
        } else {
            collectedHandles.insert(type,
                    QList<WaveformWidgetBackend>{backend});
        }
    }
}

namespace {
template<typename WaveformT>
WaveformWidgetVars waveformWidgetVars() {
    WaveformWidgetVars result;
    result.m_useGL = WaveformT::useOpenGl();
    result.m_useGLES = WaveformT::useOpenGles();
    result.m_useGLSL = WaveformT::useOpenGLShaders();
    result.m_category = WaveformT::category();

    return result;
}
} // namespace

void WaveformWidgetFactory::evaluateWidgets() {
    m_waveformWidgetHandles.clear();
    QHash<WaveformWidgetType::Type, QList<WaveformWidgetBackend>> collectedHandles;
    QHash<WaveformWidgetType::Type,
            allshader::WaveformRendererSignalBase::Options>
            supportedOptions;
    bool useGles = isOpenGlesAvailable(); // we can make use of GLES waveforms
    for (WaveformWidgetType::Type type : WaveformWidgetType::kValues) {
        switch (type) {
        case WaveformWidgetType::Empty:
            addHandle(collectedHandles, type, waveformWidgetVars<EmptyWaveformWidget>());
            break;
        case WaveformWidgetType::Simple:
#ifdef MIXXX_USE_QOPENGL
            addHandle(collectedHandles, type, allshader::WaveformWidget::vars());
            supportedOptions[type] =
                    allshader::WaveformWidget::supportedOptions(
                            type, useGles);
#endif
            addHandle(collectedHandles, type, waveformWidgetVars<SimpleSignalWaveformWidget>());
            break;
        case WaveformWidgetType::Filtered:
#ifdef MIXXX_USE_QOPENGL
            addHandle(collectedHandles, type, allshader::WaveformWidget::vars());
            supportedOptions[type] =
                    allshader::WaveformWidget::supportedOptions(
                            type, useGles);
#endif
            addHandle(collectedHandles, type, waveformWidgetVars<SoftwareWaveformWidget>());
            break;
        case WaveformWidgetType::VSyncTest:
#if defined(MIXXX_USE_QOPENGL) && !defined(QT_OPENGL_ES_2)
            addHandle(collectedHandles, type, waveformWidgetVars<GLVSyncTestWidget>());
#endif
            break;
        case WaveformWidgetType::RGB:
#ifdef MIXXX_USE_QOPENGL
            addHandle(collectedHandles, type, allshader::WaveformWidget::vars());
            supportedOptions[type] =
                    allshader::WaveformWidget::supportedOptions(
                            type, useGles);
#endif
            addHandle(collectedHandles, type, waveformWidgetVars<RGBWaveformWidget>());
            break;
        case WaveformWidgetType::HSV:
#ifdef MIXXX_USE_QOPENGL
            addHandle(collectedHandles, type, allshader::WaveformWidget::vars());
            supportedOptions[type] =
                    allshader::WaveformWidget::supportedOptions(
                            type, useGles);
#endif
            addHandle(collectedHandles, type, waveformWidgetVars<HSVWaveformWidget>());
            break;
        case WaveformWidgetType::Stacked:
#ifdef MIXXX_USE_QOPENGL
            addHandle(collectedHandles, type, allshader::WaveformWidget::vars());
            supportedOptions[type] =
                    allshader::WaveformWidget::supportedOptions(
                            type, useGles);
#endif
            break;
        default:
            DEBUG_ASSERT(!"Unexpected WaveformWidgetType");
            continue;
        }
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    for (auto [type, backends] : collectedHandles.asKeyValueRange()) {
#else
    QHashIterator<WaveformWidgetType::Type,
            QList<WaveformWidgetBackend>>
            handleIter(collectedHandles);
    while (handleIter.hasNext()) {
        handleIter.next();
        const auto& type = handleIter.key();
        const auto& backends = handleIter.value();
#endif
        m_waveformWidgetHandles.push_back(WaveformWidgetAbstractHandle(type, backends
#ifdef MIXXX_USE_QOPENGL
                ,
                supportedOptions.value(type, allshader::WaveformRendererSignalBase::Option::None)
#endif
                        ));
    }
}

WaveformWidgetAbstract* WaveformWidgetFactory::createAllshaderWaveformWidget(
        WaveformWidgetType::Type type, WWaveformViewer* viewer) {
    allshader::WaveformRendererSignalBase::Options options =
            m_config->getValue(ConfigKey("[Waveform]", "waveform_options"),
                    allshader::WaveformRendererSignalBase::Option::None);
    return new allshader::WaveformWidget(viewer, type, viewer->getGroup(), options);
}

WaveformWidgetAbstract* WaveformWidgetFactory::createFilteredWaveformWidget(
        WWaveformViewer* viewer) {
    WaveformWidgetBackend backend = getBackendFromConfig();

    switch (backend) {
#ifdef MIXXX_USE_QOPENGL
    case WaveformWidgetBackend::AllShader: {
        return createAllshaderWaveformWidget(WaveformWidgetType::Type::Filtered, viewer);
    }
#endif
    default:
        return new SoftwareWaveformWidget(viewer->getGroup(), viewer);
    }
}

WaveformWidgetAbstract* WaveformWidgetFactory::createHSVWaveformWidget(WWaveformViewer* viewer) {
    WaveformWidgetBackend backend = getBackendFromConfig();

    switch (backend) {
#ifdef MIXXX_USE_QOPENGL
    case WaveformWidgetBackend::AllShader:
        return createAllshaderWaveformWidget(WaveformWidgetType::HSV, viewer);
#endif
    default:
        return new HSVWaveformWidget(viewer->getGroup(), viewer);
    }
}

WaveformWidgetAbstract* WaveformWidgetFactory::createRGBWaveformWidget(WWaveformViewer* viewer) {
    WaveformWidgetBackend backend = getBackendFromConfig();

    switch (backend) {
#ifdef MIXXX_USE_QOPENGL
    case WaveformWidgetBackend::AllShader:
        return createAllshaderWaveformWidget(WaveformWidgetType::Type::RGB, viewer);
#endif
    default:
        return new RGBWaveformWidget(viewer->getGroup(), viewer);
    }
}

WaveformWidgetAbstract* WaveformWidgetFactory::createStackedWaveformWidget(
        WWaveformViewer* viewer) {
#ifdef MIXXX_USE_QOPENGL
    WaveformWidgetBackend backend = getBackendFromConfig();
    switch (backend) {
    case WaveformWidgetBackend::AllShader:
        return createAllshaderWaveformWidget(WaveformWidgetType::Type::Stacked, viewer);
#endif
    default:
        return new EmptyWaveformWidget(viewer->getGroup(), viewer);
    }
}

WaveformWidgetAbstract* WaveformWidgetFactory::createSimpleWaveformWidget(WWaveformViewer* viewer) {
    WaveformWidgetBackend backend = getBackendFromConfig();

    switch (backend) {
#ifdef MIXXX_USE_QOPENGL
    case WaveformWidgetBackend::AllShader:
        return createAllshaderWaveformWidget(WaveformWidgetType::Type::Simple, viewer);
#endif
    default:
        return new SimpleSignalWaveformWidget(viewer->getGroup(), viewer);
    }
}

WaveformWidgetAbstract* WaveformWidgetFactory::createVSyncTestWaveformWidget(
        WWaveformViewer* viewer) {
#ifdef MIXXX_USE_QOPENGL
    return new GLVSyncTestWidget(viewer->getGroup(), viewer);
#else
    return new EmptyWaveformWidget(viewer->getGroup(), viewer);
#endif
}

WaveformWidgetAbstract* WaveformWidgetFactory::createWaveformWidget(
        WaveformWidgetType::Type type, WWaveformViewer* pViewer) {
    WaveformWidgetAbstract* pWidget = nullptr;
    if (pViewer) {
        if (CmdlineArgs::Instance().getSafeMode()) {
            type = WaveformWidgetType::Empty;
        }

        switch (type) {
        case WaveformWidgetType::Simple:
            pWidget = createSimpleWaveformWidget(pViewer);
            break;
        case WaveformWidgetType::Filtered:
            pWidget = createFilteredWaveformWidget(pViewer);
            break;
        case WaveformWidgetType::HSV:
            pWidget = createHSVWaveformWidget(pViewer);
            break;
        case WaveformWidgetType::VSyncTest:
            pWidget = createVSyncTestWaveformWidget(pViewer);
            break;
        case WaveformWidgetType::RGB:
            pWidget = createRGBWaveformWidget(pViewer);
            break;
        case WaveformWidgetType::Stacked:
            pWidget = createStackedWaveformWidget(pViewer);
            break;
        default:
            pWidget = new EmptyWaveformWidget(pViewer->getGroup(), pViewer);
            break;
        }
        pWidget->castToQWidget();
        if (!pWidget->isValid()) {
            qWarning() << "failed to init WaveformWidget" << type << "fall back to \"Empty\"";
            delete pWidget;
            pWidget = new EmptyWaveformWidget(pViewer->getGroup(), pViewer);
            pWidget->castToQWidget();
            if (!pWidget->isValid()) {
                qWarning() << "failed to init EmptyWaveformWidget";
                delete pWidget;
                pWidget = nullptr;
            }
        }
    }
    return pWidget;
}

int WaveformWidgetFactory::findIndexOf(WWaveformViewer* viewer) const {
    for (int i = 0; i < (int)m_waveformWidgetHolders.size(); i++) {
        if (m_waveformWidgetHolders[i].m_waveformViewer == viewer) {
            return i;
        }
    }
    return -1;
}

void WaveformWidgetFactory::startVSync(
        GuiTick* pGuiTick, VisualsManager* pVisualsManager, bool useQML) {
    // DECKLAB_VSYNC=pll|timer overrides [Waveform] VSync, here and for the
    // swap interval in getSurfaceFormat().
    const int configuredMode = m_config->getValue(kVSyncKey, 0);
    const auto vsyncOverride = mixxx::decklab::vsyncOverrideFromEnvironment();
    const auto vSyncMode = useQML
            ? VSyncThread::ST_TIMER
            : static_cast<VSyncThread::VSyncMode>(
                      mixxx::decklab::effectiveVSyncMode(configuredMode, vsyncOverride));

    m_pGuiTick = pGuiTick;
    m_pVisualsManager = pVisualsManager;
    m_vsyncThread = new VSyncThread(this, vSyncMode);
    m_vsyncThread->setObjectName(QStringLiteral("VSync"));
    m_vsyncThread->setSyncIntervalTimeMicros(static_cast<int>(1e6 / m_frameRate));

    QString source = QStringLiteral("config");
    if (useQML) {
        source = QStringLiteral("qml");
    } else if (vsyncOverride != mixxx::decklab::VSyncOverride::None) {
        source = QStringLiteral("env DECKLAB_VSYNC=") +
                mixxx::decklab::vsyncOverrideName(vsyncOverride);
    }
    qInfo().noquote() << QStringLiteral(
            "DECKLAB_VSYNC mode=%1 source=%2 config=%3 swap_interval=%4 fps=%5")
                                 .arg(vsyncModeName(m_vsyncThread->vsyncMode()),
                                         source,
                                         QString::number(configuredMode),
                                         QString::number(
                                                 getSurfaceFormat(m_config).swapInterval()),
                                         QString::number(m_frameRate));

    if (m_vsyncThread->vsyncMode() == VSyncThread::ST_PLL) {
#ifdef MIXXX_USE_QOPENGL
        WGLWidget* widget = SharedGLContext::getWidget();
        if (widget && widget->getOpenGLWindow()) {
            connect(widget->getOpenGLWindow(),
                    &QOpenGLWindow::frameSwapped,
                    this,
                    &WaveformWidgetFactory::slotFrameSwapped,
                    Qt::DirectConnection);
            // Time the shared-window paints (Deck Lab idle-aware PLL).
            m_pPllKeepaliveWindow = widget->getOpenGLWindow();
            m_pPllKeepaliveWindow->installEventFilter(this);
            m_pllKeepaliveEnabled = true;
            widget->show();
        }
#endif
        if (m_pllKeepaliveEnabled) {
            // Startup safety: timer mode if the loop does not lock.
            m_pPllLockWatchdogTimer = new QTimer(this);
            m_pPllLockWatchdogTimer->setInterval(kPllLockWatchdogIntervalMillis);
            connect(m_pPllLockWatchdogTimer,
                    &QTimer::timeout,
                    this,
                    &WaveformWidgetFactory::slotPllLockWatchdog);
            m_pllLockWatchdogClock.start();
            m_pPllLockWatchdogTimer->start();
        } else {
            qWarning() << "DECKLAB_VSYNC pll needs the shared OpenGL window, "
                          "which does not exist: falling back to timer mode";
            m_vsyncThread->fallBackToTimer();
        }
    }

    connect(m_vsyncThread,
            &VSyncThread::vsyncRender,
            this,
            &WaveformWidgetFactory::render);
    connect(m_vsyncThread,
            &VSyncThread::vsyncSwap,
            this,
            &WaveformWidgetFactory::swap);
    connect(m_vsyncThread,
            &VSyncThread::vsyncSwapAndRender,
            this,
            &WaveformWidgetFactory::swapAndRender);

    m_vsyncThread->start(QThread::NormalPriority);
}

void WaveformWidgetFactory::getAvailableVSyncTypes(QList<QPair<int, QString>>* pList) {
    m_vsyncThread->getAvailableVSyncTypes(pList);
}

WaveformWidgetType::Type WaveformWidgetFactory::findTypeFromHandleIndex(int index) {
    WaveformWidgetType::Type type = WaveformWidgetType::Invalid;
    if (index >= 0 && index < m_waveformWidgetHandles.size()) {
        type = m_waveformWidgetHandles[index].m_type;
    }
    return type;
}

int WaveformWidgetFactory::findHandleIndexFromType(WaveformWidgetType::Type type) {
    for (int i = 0; i < m_waveformWidgetHandles.size(); i++) {
        const WaveformWidgetAbstractHandle& handle = m_waveformWidgetHandles[i];
        if (handle.m_type == type) {
            return i;
        }
    }
    return -1;
}

bool WaveformWidgetFactory::widgetTypeSupportsAcceleration(WaveformWidgetType::Type type) {
    for (const auto& handle : std::as_const(m_waveformWidgetHandles)) {
        if (handle.m_type == type) {
            return handle.supportAcceleration();
        }
    }
    return false;
}

bool WaveformWidgetFactory::widgetTypeSupportsSoftware(WaveformWidgetType::Type type) {
    for (const auto& handle : std::as_const(m_waveformWidgetHandles)) {
        if (handle.m_type == type) {
            return handle.supportSoftware();
        }
    }
    return false;
}

WaveformWidgetBackend WaveformWidgetFactory::getBackendFromConfig() const {
    // On the UI, hardware acceleration is a boolean (0 => software rendering, 1
    // => hardware acceleration), but in the setting, we keep the granularity so
    // in case of issue when we release, we can communicate workaround on
    // editing the INI file to target a specific rendering backend. If no
    // complains come back, we can convert this safely to a backend eventually.
    return m_config->getValue(
            ConfigKey(QStringLiteral("[Waveform]"), QStringLiteral("use_hardware_acceleration")),
            preferredBackend());
}

WaveformWidgetBackend WaveformWidgetFactory::preferredBackend() const {
#ifdef MIXXX_USE_QOPENGL
    if (m_openGlAvailable || m_openGlesAvailable) {
        return WaveformWidgetBackend::AllShader;
    }
#endif
    if (m_openGlAvailable && m_openGLShaderAvailable) {
        return WaveformWidgetBackend::GLSL;
    } else if (m_openGlAvailable) {
        return WaveformWidgetBackend::GL;
    }
    return WaveformWidgetBackend::None;
}

void WaveformWidgetFactory::setDefaultBackend() {
    m_config->setValue(kHardwareAccelerationKey, preferredBackend());
}

QString WaveformWidgetAbstractHandle::getDisplayName() const {
    return getDisplayName(m_type);
}

// Static
QString WaveformWidgetAbstractHandle::getDisplayName(WaveformWidgetType::Type type) {
    switch (type) {
    case WaveformWidgetType::Empty:
        return QObject::tr("Empty");
    case WaveformWidgetType::Simple:
        return QObject::tr("Simple");
    case WaveformWidgetType::Filtered:
        return QObject::tr("Filtered");
    case WaveformWidgetType::HSV:
        return QObject::tr("HSV");
    case WaveformWidgetType::VSyncTest:
        return QObject::tr("VSyncTest");
    case WaveformWidgetType::RGB:
        return QObject::tr("RGB");
    case WaveformWidgetType::Stacked:
        return QObject::tr("Stacked");
    default:
        return QObject::tr("Unknown");
    }
}

// static
QSurfaceFormat WaveformWidgetFactory::getSurfaceFormat(UserSettingsPointer pConfig) {
    // The first call should pass the config to set the vsync mode. Subsequent
    // calls will use the value as set on the first call.
    // DECKLAB_VSYNC=pll|timer overrides [Waveform] VSync, as in startVSync().
    static const VSyncThread::VSyncMode vsyncMode =
            static_cast<VSyncThread::VSyncMode>(mixxx::decklab::effectiveVSyncMode(
                    pConfig ? pConfig->getValue(
                                      kVSyncKey, static_cast<int>(VSyncThread::ST_DEFAULT))
                            : static_cast<int>(VSyncThread::ST_DEFAULT),
                    mixxx::decklab::vsyncOverrideFromEnvironment()));

    QSurfaceFormat format;
    // Qt5 requires at least OpenGL 2.1 or OpenGL ES 2.0, default is 2.0
    // format.setVersion(2, 1);
    // Core and Compatibility contexts have been introduced in openGL 3.2
    // From 3.0 to 3.1 we have implicit the Core profile and Before 3.0 we have the
    // Compatibility profile
    // format.setProfile(QSurfaceFormat::CoreProfile);

    // setSwapInterval sets the application preferred swap interval
    // in minimum number of video frames that are displayed before a buffer swap occurs
    // - 0 will turn the vertical refresh syncing off
    // - 1 (default) means swapping after drawig a video frame to the buffer
    // - n means swapping after drawing n video frames to the buffer
    //
    // The vertical sync setting requested by the OpenGL application, can be overwritten
    // if a user changes the "Wait for vertical refresh" setting in AMD graphic drivers
    // for Windows.

#if defined(__APPLE__)
    // On OS X, syncing to vsync has good performance FPS-wise and
    // eliminates tearing. (This is an comment from pre QOpenGLWindow times)
    format.setSwapInterval(1);
    (void)vsyncMode;
#else
    // It seems that on Windows (at least for some AMD drivers), the setting 1 is not
    // not properly handled. We saw frame rates divided by exact integers, like it should
    // be with values >1 (see https://github.com/mixxxdj/mixxx/issues/11617)
    // Reported as https://bugreports.qt.io/browse/QTBUG-114882
    // On Linux, horrible FPS were seen with "VSync off" before switching to QOpenGLWindow too
    format.setSwapInterval(vsyncMode == VSyncThread::ST_PLL ? 1 : 0);
#endif

#ifdef FORCE_GLES
    // Define FORCE_GLES to test GLES waveforms on a GLSL Hardware
    qDebug() << "QOpenGLContext::openGLModuleType()" << QOpenGLContext::openGLModuleType();

    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(3, 0);
    QSurfaceFormat::setDefaultFormat(format);
#endif

    return format;
}

void WaveformWidgetFactory::setUntilMarkShowBeats(bool value) {
    m_untilMarkShowBeats = value;
    if (m_config) {
        m_config->setValue(ConfigKey(kWaveformGroup, QStringLiteral("UntilMarkShowBeats")),
                m_untilMarkShowBeats);
    }
    emit untilMarkShowBeatsChanged(value);
    requestRenderAll();
}

void WaveformWidgetFactory::setUntilMarkShowTime(bool value) {
    m_untilMarkShowTime = value;
    if (m_config) {
        m_config->setValue(ConfigKey(kWaveformGroup, QStringLiteral("UntilMarkShowTime")),
                m_untilMarkShowTime);
    }
    emit untilMarkShowTimeChanged(value);
    requestRenderAll();
}

void WaveformWidgetFactory::setUntilMarkAlign(Qt::Alignment align) {
    m_untilMarkAlign = align;
    if (m_config) {
        m_config->setValue(ConfigKey(kWaveformGroup, QStringLiteral("UntilMarkAlign")),
                toUntilMarkAlignIndex(m_untilMarkAlign));
    }
    emit untilMarkAlignChanged(align);
    requestRenderAll();
}

void WaveformWidgetFactory::setUntilMarkTextPointSize(int value) {
    m_untilMarkTextPointSize = value;
    if (m_config) {
        m_config->setValue(ConfigKey(kWaveformGroup, QStringLiteral("UntilMarkTextPointSize")),
                m_untilMarkTextPointSize);
    }
    emit untilMarkTextPointSizeChanged(value);
    requestRenderAll();
}

void WaveformWidgetFactory::setUntilMarkTextHeightLimit(float value) {
    m_untilMarkTextHeightLimit = value;
    if (m_config) {
        m_config->setValue(ConfigKey(kWaveformGroup, QStringLiteral("UntilMarkTextHeightLimit")),
                toUntilMarkTextHeightLimitIndex(m_untilMarkTextHeightLimit));
    }
    emit untilMarkTextHeightLimitChanged(value);
    requestRenderAll();
}

void WaveformWidgetFactory::setStemReorderOnChange(bool value) {
    m_stemReorderOnChange = value;
    if (m_config) {
        m_config->setValue(ConfigKey(kWaveformGroup, QStringLiteral("stem_reorder_on_change")),
                value);
    }
    emit stemReorderOnChangeChanged(value);
    requestRenderAll();
}

void WaveformWidgetFactory::setStemOutlineOpacity(float value) {
    m_stemOutlineOpacity = value;
    if (m_config) {
        m_config->setValue(ConfigKey(kWaveformGroup, QStringLiteral("stem_outline_opacity")),
                static_cast<double>(value));
    }
    emit stemOutlineOpacityChanged(value);
    requestRenderAll();
}

void WaveformWidgetFactory::setStemOpacity(float value) {
    m_stemOpacity = value;
    if (m_config) {
        m_config->setValue(ConfigKey(kWaveformGroup, QStringLiteral("stem_opacity")),
                static_cast<double>(value));
    }
    emit stemOpacityChanged(value);
    requestRenderAll();
}

void WaveformWidgetFactory::setStemSplitTracks(bool value) {
    m_stemSplitTracks = value;
    if (m_config) {
        m_config->setValue(ConfigKey(kWaveformGroup, QStringLiteral("stem_split_tracks")),
                value);
    }
    m_pStemSplitTracksControl->set(value ? 1.0 : 0.0);
    emit stemSplitTracksChanged(value);
    requestRenderAll();
}

// static
Qt::Alignment WaveformWidgetFactory::toUntilMarkAlign(int index) {
    switch (index) {
    case 0:
        return Qt::AlignTop;
    case 1:
        return Qt::AlignVCenter;
    case 2:
        return Qt::AlignBottom;
    }
    DEBUG_ASSERT(!"unsupported align");
    return Qt::AlignVCenter;
}
// static
int WaveformWidgetFactory::toUntilMarkAlignIndex(Qt::Alignment align) {
    switch (align) {
    case Qt::AlignTop:
        return 0;
    case Qt::AlignVCenter:
        return 1;
    case Qt::AlignBottom:
        return 2;
    default:
        break;
    }
    DEBUG_ASSERT(!"unsupported align index");
    return 1;
}
// static
float WaveformWidgetFactory::toUntilMarkTextHeightLimit(int index) {
    switch (index) {
    case 0:
        return 0.333f;
    case 1:
        return 1.f;
    }
    DEBUG_ASSERT(!"unsupported height limit");
    return 0.33f;
}
// static
int WaveformWidgetFactory::toUntilMarkTextHeightLimitIndex(float value) {
    if (value == 0.333f) {
        return 0;
    }
    if (value == 1.f) {
        return 1;
    }
    DEBUG_ASSERT(!"unsupported height limit");
    return 0;
}
