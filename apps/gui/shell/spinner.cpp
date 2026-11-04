#include "shell/spinner.hpp"

#include <QPainter>
#include <QTimer>

#include "theme.hpp"

namespace p3dgui {

Spinner::Spinner(QWidget* parent, int size) : QWidget(parent), size_(size) {
  setFixedSize(size, size);
  timer_ = new QTimer(this);
  timer_->setInterval(16);
  connect(timer_, &QTimer::timeout, this, [this] {
    angle_ = (angle_ + 6) % 360;
    update();
  });
  hide();
}

void Spinner::start() {
  show();
  timer_->start();
}

void Spinner::stop() {
  timer_->stop();
  hide();
}

bool Spinner::running() const { return timer_->isActive(); }

void Spinner::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const QRectF r = QRectF(rect()).adjusted(2, 2, -2, -2);
  p.setPen(QPen(theme::with_alpha(theme::colors().accent_hover, 0.22), 2.2));
  p.drawEllipse(r);
  p.setPen(QPen(theme::colors().accent_hover, 2.2, Qt::SolidLine, Qt::RoundCap));
  p.drawArc(r, -angle_ * 16, 100 * 16);
}

}  // namespace p3dgui
