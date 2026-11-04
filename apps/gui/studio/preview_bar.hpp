#pragma once
// Camera and display controls under a model preview: view buttons, reset, wireframe,
// true colour and a turntable.
#include <QWidget>

class QTimer;
class QToolButton;

namespace p3dgui {

class Viewport;

class PreviewBar : public QWidget {
  Q_OBJECT
 public:
  explicit PreviewBar(Viewport* viewport, QWidget* parent = nullptr, bool two_rows = false);
  bool turntable() const;
  void setTurntable(bool on);

 private:
  Viewport* viewport_ = nullptr;
  QTimer* spin_ = nullptr;
  QToolButton* turn_ = nullptr;
};

}  // namespace p3dgui
