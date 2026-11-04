#include "studio/splash.hpp"

#include <QApplication>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>

#include "pixel3d/common/version.hpp"
#include "theme.hpp"

namespace p3dgui {
namespace {

QPixmap background() {
  const qreal dpr = QGuiApplication::primaryScreen() ? QGuiApplication::primaryScreen()->devicePixelRatio() : 1.0;
  const QSize size(560, 320);
  QPixmap pm(size * dpr);
  pm.setDevicePixelRatio(dpr);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  const auto& c = theme::colors();
  QLinearGradient g(0, 0, 0, size.height());
  g.setColorAt(0, c.raised);
  g.setColorAt(1, c.app);
  QPainterPath r;
  r.addRoundedRect(QRectF(0.5, 0.5, size.width() - 1, size.height() - 1), 16, 16);
  p.fillPath(r, g);
  p.setPen(QPen(c.border_strong, 1));
  p.drawPath(r);
  const QPixmap mark(QStringLiteral(":/3re-512.png"));
  p.drawPixmap(QRectF(40, 72, 120, 120), mark, mark.rect());
  p.setPen(c.text);
  QFont big = theme::font(theme::Type::Display);
  big.setPointSizeF(40);
  p.setFont(big);
  p.drawText(QRectF(184, 70, 340, 70), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("3re"));
  p.setPen(c.text_secondary);
  p.setFont(theme::font(theme::Type::Body));
  p.drawText(QRectF(186, 140, 340, 50), Qt::AlignLeft | Qt::TextWordWrap,
             QObject::tr("Pictures and pixel art to clean, checked 3D models."));
  const auto v = pixel3d::version();
  p.setPen(c.text_muted);
  p.setFont(theme::font(theme::Type::Small));
  p.drawText(QRectF(186, 192, 340, 20), Qt::AlignLeft, QStringLiteral("Version %1 · offline").arg(QString::fromUtf8(v.data(), qsizetype(v.size()))));
  return pm;
}

}  // namespace

Splash::Splash() : QSplashScreen(background()) {
  setAttribute(Qt::WA_TranslucentBackground);
  setWindowFlag(Qt::WindowStaysOnTopHint, false);
}

void Splash::setStatus(const QString& text) {
  status_ = text;
  repaint();
  QApplication::processEvents();
}

void Splash::drawContents(QPainter* p) {
  const auto& c = theme::colors();
  const QRectF bar(40, 268, 480, 3);
  p->setRenderHint(QPainter::Antialiasing);
  p->fillRect(bar, theme::with_alpha(c.accent_hover, 0.2));
  p->fillRect(QRectF(bar.x(), bar.y(), bar.width() * 0.62, bar.height()), c.accent_hover);
  p->setPen(c.text_secondary);
  p->setFont(theme::font(theme::Type::Small));
  p->drawText(QRectF(40, 240, 480, 22), Qt::AlignLeft | Qt::AlignVCenter, status_);
}

}  // namespace p3dgui
