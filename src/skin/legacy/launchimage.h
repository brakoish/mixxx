#pragma once

#include <QWidget>

QT_FORWARD_DECLARE_CLASS(QProgressBar);
QT_FORWARD_DECLARE_CLASS(QLabel);
QT_FORWARD_DECLARE_CLASS(QVBoxLayout);

// This is a widget that is shown in the Mixxx main window
// until the skin is ready to use.
// It shows a centered Image and a progress bar below.
// By default a symbolic Mixxx icon and logo is shown.
// It can be modified in the skin.xml file at <skin> section,
// to match the skin like that:
//    <LaunchImageStyle>
//        LaunchImage { background-color: #202020; }
//        QLabel {
//            image: url(skin:/style/mixxx-icon-logo-symbolic.svg);
//            padding:0;
//            margin:0;
//            border:none;
//            min-width: 208px;
//            min-height: 48px;
//            max-width: 208px;
//            max-height: 48px;
//        }
//        QProgressBar {
//            background-color: #202020;
//            border:none;
//            min-width: 208px;
//            min-height: 3px;
//            max-width: 208px;
//            max-height: 3px;
//        }
//        QProgressBar::chunk { background-color: #ec4522; }
//    </LaunchImageStyle>
// A skin's own style always wins over the holiday logo, which only replaces the default one.
// "LaunchImage { qproperty-spacing: 0; }" removes the style's gap between the image and the
// progress bar, so a skin can place both to the pixel with margins (Deck Lab lines them up with
// its boot screen).

class LaunchImage: public QWidget {
    Q_OBJECT
    Q_PROPERTY(int spacing READ spacing WRITE setSpacing)
  public:
    LaunchImage(QWidget* pParent, const QString& styleSheet);
    ~LaunchImage() override = default;
    void progress(int value, const QString& serviceName);

    int spacing() const;
    void setSpacing(int spacing);

  protected:
    void paintEvent(QPaintEvent*) override;

  private:
    QProgressBar* m_pProgressBar;
    QVBoxLayout* m_pImageAndBar;
};
