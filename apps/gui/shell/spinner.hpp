#pragma once
// A small busy indicator (rotating arc) for work whose duration is unknown.
#include <QWidget>

class QTimer;

namespace p3dgui {

class Spinner : public QWidget {
  Q_OBJECT
 public:
  explicit Spinner(QWidget* parent = nullptr, int size = 20);
  void start();
  void stop();
  bool running() const;
  QSize sizeHint() const override { return {size_, size_}; }

 protected:
  void paintEvent(QPaintEvent*) override;

 private:
  QTimer* timer_ = nullptr;
  int angle_ = 0, size_ = 20;
};

}  // namespace p3dgui
