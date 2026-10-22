#pragma once

#ifndef WGLWIDGET_H
#error "Do not include this file, include wglwidget.h instead"
#endif

#include <QGLWidget>

class WGLWidget : public QGLWidget {
  public:
    WGLWidget(QWidget* pParent);

    bool isContextValid() const;
    bool shouldRender() const;
    void makeCurrentIfNeeded();

    // Deck Lab render gate. QGLWidget repaints itself on expose and resize,
    // so there is nothing extra to request on this legacy path.
    void requestGLRender() {
    }
    bool takeGLRenderRequest() {
        return false;
    }

  protected:
    QPaintDevice* paintDevice() {
        return this;
    }
};
