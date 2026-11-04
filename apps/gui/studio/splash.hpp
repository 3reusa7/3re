#pragma once
// The 3re start screen while the window and its engines load: mark, name, version, status.
#include <QSplashScreen>

namespace p3dgui {

class Splash : public QSplashScreen {
  Q_OBJECT
 public:
  Splash();
  void setStatus(const QString& text);

 protected:
  void drawContents(QPainter* p) override;

 private:
  QString status_;
};

}  // namespace p3dgui
