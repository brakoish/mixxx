#pragma once

#ifndef WGLWIDGET_H
#error "Do not include this file, include wglwidget.h instead"
#endif

#include <QWidget>

////////////////////////////////
// QOpenGLWindow based WGLWidget
////////////////////////////////

class QPaintDevice;
class QOpenGLWindow;
class OpenGLWindow;
class TrackDropTarget;

class WGLWidget : public QWidget {
  public:
    WGLWidget(QWidget* parent);
    ~WGLWidget();

    bool isContextValid() const;

    bool shouldRender() const;

    void makeCurrentIfNeeded();
    void doneCurrent();

    void swapBuffers();

    /// Deck Lab render gate: show, resize and expose of the native window
    /// request a full render and swap of the next frame, so skipped frames
    /// can never leave stale or black content behind.
    void requestGLRender() {
        m_glRenderRequested = true;
    }
    bool takeGLRenderRequest() {
        const bool requested = m_glRenderRequested;
        m_glRenderRequested = false;
        return requested;
    }

    // called (indirectly) by WaveformWidgetFactory
    virtual void paintGL();
    // called by OpenGLWindow
    virtual void resizeGL(int w, int h);
    virtual void initializeGL();

    void setTrackDropTarget(TrackDropTarget* pTarget);
    TrackDropTarget* trackDropTarget() const;

    QOpenGLWindow* getOpenGLWindow() const;

  protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* pWatched, QEvent* pEvent) override;

    QPaintDevice* paintDevice();

  private:
    OpenGLWindow* m_pOpenGLWindow;
    QWidget* m_pContainerWidget;
    TrackDropTarget* m_pTrackDropTarget;
    bool m_glRenderRequested;
};
