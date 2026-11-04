#include "icons.hpp"

#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <cmath>
#include <initializer_list>
#include <numbers>

namespace p3dgui {

namespace {

using theme::colors;

// Drawing helpers on the 24-unit icon grid.
struct Pen {
  QPainter& p;
  QColor c;

  void stroke(const QPainterPath& path) {
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
  }
  void soft(const QPainterPath& path, double alpha = 0.22) {
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(theme::with_alpha(c, alpha * c.alphaF()));
    p.drawPath(path);
    p.restore();
  }
  void solid(const QPainterPath& path) {
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawPath(path);
    p.restore();
  }
  void line(double x1, double y1, double x2, double y2) { p.drawLine(QPointF(x1, y1), QPointF(x2, y2)); }
  void dot(double x, double y, double r) {
    QPainterPath d;
    d.addEllipse(QPointF(x, y), r, r);
    solid(d);
  }
};

QPainterPath poly(std::initializer_list<QPointF> pts, bool closed) {
  QPainterPath path;
  bool first = true;
  for (const QPointF& q : pts) {
    if (first) path.moveTo(q);
    else path.lineTo(q);
    first = false;
  }
  if (closed) path.closeSubpath();
  return path;
}

QPainterPath rrect(double x, double y, double w, double h, double r) {
  QPainterPath path;
  path.addRoundedRect(QRectF(x, y, w, h), r, r);
  return path;
}

QPainterPath circle(double cx, double cy, double r) {
  QPainterPath path;
  path.addEllipse(QPointF(cx, cy), r, r);
  return path;
}

// Isometric cube: hexagon outline, inner "Y", and its three faces.
struct Cube {
  QPointF top, ur, lr, bottom, ll, ul, c;
  Cube(double cx, double cy, double s)
      : top(cx, cy - 9.2 * s), ur(cx + 8.2 * s, cy - 4.6 * s), lr(cx + 8.2 * s, cy + 4.6 * s), bottom(cx, cy + 9.2 * s),
        ll(cx - 8.2 * s, cy + 4.6 * s), ul(cx - 8.2 * s, cy - 4.6 * s), c(cx, cy) {}
  QPainterPath outline() const { return poly({top, ur, lr, bottom, ll, ul}, true); }
  QPainterPath inner() const {
    QPainterPath p = poly({ul, c, ur}, false);
    p.moveTo(c);
    p.lineTo(bottom);
    return p;
  }
  QPainterPath top_face() const { return poly({top, ur, c, ul}, true); }
  QPainterPath left_face() const { return poly({ul, c, bottom, ll}, true); }
  QPainterPath right_face() const { return poly({c, ur, lr, bottom}, true); }
};

QPainterPath tray() {
  QPainterPath t;
  t.moveTo(4, 13.5);
  t.lineTo(4, 17.5);
  t.quadTo(4, 19.5, 6, 19.5);
  t.lineTo(18, 19.5);
  t.quadTo(20, 19.5, 20, 17.5);
  t.lineTo(20, 13.5);
  return t;
}

QPainterPath arrow_head(QPointF tip, QPointF dir, double len) {
  const double l = std::hypot(dir.x(), dir.y());
  const QPointF d = l > 0 ? dir / l : QPointF(1, 0);
  const QPointF n(-d.y(), d.x());
  return poly({tip - d * len + n * len * 0.8, tip, tip - d * len - n * len * 0.8}, false);
}

void paint_icon(QPainter& p, Icon id, const QColor& c, theme::Tone tone) {
  QPen pen(c, 1.75, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
  Pen d{p, c};
  switch (id) {
    case Icon::Import:
    case Icon::Drop:
      d.line(12, 3.5, 12, 14);
      d.stroke(poly({{7.5, 9.5}, {12, 14}, {16.5, 9.5}}, false));
      d.stroke(tray());
      break;
    case Icon::Export:
      d.line(12, 14, 12, 3.5);
      d.stroke(poly({{7.5, 8}, {12, 3.5}, {16.5, 8}}, false));
      d.stroke(tray());
      break;
    case Icon::Reconstruct: {
      const Cube k(10.6, 13.4, 0.84);
      d.soft(k.top_face(), 0.35);
      d.stroke(k.outline());
      d.stroke(k.inner());
      QPainterPath star;
      star.moveTo(19.5, 1.6);
      star.quadTo(19.5, 4.6, 22.5, 4.6);
      star.quadTo(19.5, 4.6, 19.5, 7.6);
      star.quadTo(19.5, 4.6, 16.5, 4.6);
      star.quadTo(19.5, 4.6, 19.5, 1.6);
      d.solid(star);
      break;
    }
    case Icon::Select: {
      const QPainterPath arrow = poly({{6, 3.5}, {6, 18.5}, {10, 14.8}, {12.8, 20.5}, {15.4, 19.3}, {12.7, 13.7}, {18, 13.5}}, true);
      d.soft(arrow);
      d.stroke(arrow);
      break;
    }
    case Icon::Depth:
      d.stroke(rrect(3.5, 3.5, 17, 17, 2.5));
      d.soft(rrect(8.5, 8.5, 7, 7, 1), 0.3);
      d.stroke(rrect(8.5, 8.5, 7, 7, 1));
      d.line(4.4, 4.4, 8.5, 8.5);
      d.line(19.6, 4.4, 15.5, 8.5);
      d.line(4.4, 19.6, 8.5, 15.5);
      d.line(19.6, 19.6, 15.5, 15.5);
      break;
    case Icon::PushPull: {
      const QPainterPath plane = poly({{2.5, 18}, {8, 14}, {21.5, 14}, {16, 18}}, true);
      d.soft(plane);
      d.stroke(plane);
      d.line(12, 16, 12, 3.5);
      d.stroke(poly({{8, 7.5}, {12, 3.5}, {16, 7.5}}, false));
      break;
    }
    case Icon::Parts:
      d.soft(rrect(3.5, 3.5, 7.5, 7.5, 2), 0.4);
      for (double x : {3.5, 13.0})
        for (double y : {3.5, 13.0}) d.stroke(rrect(x, y, 7.5, 7.5, 2));
      break;
    case Icon::Mirror: {
      const QPainterPath left = poly({{9.5, 6}, {9.5, 18}, {3, 18}}, true);
      d.soft(left, 0.4);
      d.stroke(left);
      d.stroke(poly({{14.5, 6}, {14.5, 18}, {21, 18}}, true));
      QPen dash = pen;
      dash.setDashPattern({1.2, 1.8});
      p.setPen(dash);
      d.line(12, 2.5, 12, 21.5);
      p.setPen(pen);
      break;
    }
    case Icon::Validate: {
      QPainterPath shield;
      shield.moveTo(12, 2.8);
      shield.lineTo(19.5, 5.6);
      shield.lineTo(19.5, 11);
      shield.cubicTo(19.5, 15.8, 16.4, 19.3, 12, 21.2);
      shield.cubicTo(7.6, 19.3, 4.5, 15.8, 4.5, 11);
      shield.lineTo(4.5, 5.6);
      shield.closeSubpath();
      d.stroke(shield);
      d.stroke(poly({{8.6, 11.8}, {11, 14.2}, {15.6, 9.4}}, false));
      break;
    }
    case Icon::New:
      d.line(12, 5, 12, 19);
      d.line(5, 12, 19, 12);
      break;
    case Icon::Open:
      d.stroke(poly({{3.5, 18.5}, {3.5, 5.5}, {9, 5.5}, {11, 7.5}, {20.5, 7.5}, {20.5, 18.5}}, true));
      d.line(3.5, 10.5, 20.5, 10.5);
      break;
    case Icon::Save:
      d.stroke(rrect(4, 4, 16, 16, 2.5));
      d.stroke(poly({{8, 4}, {8, 9}, {15, 9}, {15, 4}}, false));
      d.line(8, 15.5, 16, 15.5);
      break;
    case Icon::Undo:
    case Icon::Redo: {
      p.save();
      if (id == Icon::Redo) {
        p.translate(24, 0);
        p.scale(-1, 1);
      }
      d.stroke(poly({{8.5, 5.5}, {4, 10}, {8.5, 14.5}}, false));
      QPainterPath arc;
      arc.moveTo(4, 10);
      arc.lineTo(14.5, 10);
      arc.arcTo(QRectF(10, 10, 9, 9), 90, -180);
      arc.lineTo(10, 19);
      d.stroke(arc);
      p.restore();
      break;
    }
    case Icon::ViewFront:
    case Icon::ViewSide:
    case Icon::ViewTop:
    case Icon::View3D: {
      const Cube k(12, 12, 1);
      if (id == Icon::ViewFront) d.soft(k.left_face(), 0.55);
      if (id == Icon::ViewSide) d.soft(k.right_face(), 0.55);
      if (id == Icon::ViewTop) d.soft(k.top_face(), 0.55);
      d.stroke(k.outline());
      d.stroke(k.inner());
      break;
    }
    case Icon::Wireframe:
      d.stroke(rrect(3.5, 3.5, 17, 17, 2));
      d.line(12, 3.5, 12, 20.5);
      d.line(3.5, 12, 20.5, 12);
      d.line(3.5, 12, 12, 3.5);
      d.line(12, 12, 20.5, 3.5);
      d.line(3.5, 20.5, 12, 12);
      d.line(12, 20.5, 20.5, 12);
      break;
    case Icon::Lighting:
      d.stroke(circle(12, 12, 4));
      for (int k = 0; k < 8; ++k) {
        const double a = k * std::numbers::pi / 4;
        d.line(12 + 7 * std::cos(a), 12 + 7 * std::sin(a), 12 + 9.5 * std::cos(a), 12 + 9.5 * std::sin(a));
      }
      break;
    case Icon::Grid:
      d.stroke(poly({{2.5, 18}, {7, 8}, {17, 8}, {21.5, 18}}, true));
      d.line(10.33, 8, 8.83, 18);
      d.line(13.67, 8, 15.17, 18);
      d.line(5.5, 11.33, 18.5, 11.33);
      d.line(4, 14.67, 20, 14.67);
      break;
    case Icon::DrawingOverlay:
      d.stroke(rrect(3.5, 4.5, 17, 15, 2.5));
      d.stroke(poly({{6, 16.5}, {10, 12}, {13, 15}, {15, 13}, {18, 16.5}}, false));
      d.dot(15.5, 8.8, 1.5);
      break;
    case Icon::IssuesOverlay: {
      const QPainterPath tri = poly({{12, 3.5}, {21, 19.5}, {3, 19.5}}, true);
      d.stroke(tri);
      d.line(12, 9.5, 12, 14);
      d.dot(12, 16.8, 1.0);
      break;
    }
    case Icon::EstimatedOverlay: {
      const Cube k(12, 12, 1);
      QPen dash = pen;
      dash.setDashPattern({1.4, 1.6});
      p.setPen(dash);
      d.stroke(k.outline());
      d.stroke(k.inner());
      p.setPen(pen);
      break;
    }
    case Icon::Turntable: {
      const QRectF r(4.5, 4.5, 15, 15);
      QPainterPath arc;
      arc.arcMoveTo(r, 40);
      arc.arcTo(r, 40, 280);
      d.stroke(arc);
      const double a = 320 * std::numbers::pi / 180;
      const QPointF tip(12 + 7.5 * std::cos(a), 12 - 7.5 * std::sin(a));
      d.stroke(arrow_head(tip, QPointF(-std::sin(a), -std::cos(a)), 3.2));
      break;
    }
    case Icon::Frame:
      d.stroke(poly({{4, 9}, {4, 4}, {9, 4}}, false));
      d.stroke(poly({{15, 4}, {20, 4}, {20, 9}}, false));
      d.stroke(poly({{20, 15}, {20, 20}, {15, 20}}, false));
      d.stroke(poly({{9, 20}, {4, 20}, {4, 15}}, false));
      d.dot(12, 12, 2);
      break;
    case Icon::StatusPass:
    case Icon::StatusReview:
    case Icon::StatusFail:
    case Icon::StatusNone:
    case Icon::Info: {
      const QPainterPath ring = circle(12, 12, 8.5);
      d.soft(ring, 0.2);
      d.stroke(ring);
      if (id == Icon::StatusPass) d.stroke(poly({{8.2, 12.2}, {10.8, 14.8}, {15.8, 9.6}}, false));
      if (id == Icon::StatusReview) {
        d.line(12, 7.5, 12, 13);
        d.dot(12, 16.2, 1.05);
      }
      if (id == Icon::StatusFail) {
        d.line(9.2, 9.2, 14.8, 14.8);
        d.line(14.8, 9.2, 9.2, 14.8);
      }
      if (id == Icon::StatusNone) d.line(8.5, 12, 15.5, 12);
      if (id == Icon::Info) {
        d.dot(12, 8, 1.05);
        d.line(12, 11, 12, 16.5);
      }
      break;
    }
    case Icon::ChevronRight: d.stroke(poly({{9.5, 6}, {15.5, 12}, {9.5, 18}}, false)); break;
    case Icon::ChevronDown: d.stroke(poly({{6, 9.5}, {12, 15.5}, {18, 9.5}}, false)); break;
    case Icon::ChevronLeft: d.stroke(poly({{14.5, 6}, {8.5, 12}, {14.5, 18}}, false)); break;
    case Icon::Close:
      d.line(7, 7, 17, 17);
      d.line(17, 7, 7, 17);
      break;
    case Icon::More:
      for (double x : {6.0, 12.0, 18.0}) d.dot(x, 12, 1.5);
      break;
    case Icon::Log:
      d.stroke(rrect(3.5, 4.5, 17, 15, 2.5));
      d.stroke(poly({{7.5, 9.5}, {10.5, 12}, {7.5, 14.5}}, false));
      d.line(12.5, 14.5, 16.5, 14.5);
      break;
    case Icon::Settings:
      d.line(4, 7, 20, 7);
      d.line(4, 12, 20, 12);
      d.line(4, 17, 20, 17);
      d.dot(15, 7, 2.2);
      d.dot(9, 12, 2.2);
      d.dot(13, 17, 2.2);
      break;
    case Icon::Compare:
      d.soft(rrect(12, 4.5, 8.5, 15, 2), 0.35);
      d.stroke(rrect(3.5, 4.5, 17, 15, 2.5));
      d.line(12, 4.5, 12, 19.5);
      break;
    case Icon::PixelGrid:
      d.stroke(rrect(3.5, 3.5, 17, 17, 2));
      d.line(9.17, 3.5, 9.17, 20.5);
      d.line(14.83, 3.5, 14.83, 20.5);
      d.line(3.5, 9.17, 20.5, 9.17);
      d.line(3.5, 14.83, 20.5, 14.83);
      break;
    case Icon::Volume: {
      const Cube k(10.5, 13, 0.82);
      d.soft(k.left_face(), 0.35);
      d.stroke(k.outline());
      d.stroke(k.inner());
      d.line(19.5, 2.5, 19.5, 8.5);
      d.line(16.5, 5.5, 22.5, 5.5);
      break;
    }
    case Icon::Brush: {
      QPainterPath handle = poly({{20.5, 3.5}, {11.5, 12.5}}, false);
      d.stroke(handle);
      QPainterPath tip;
      tip.moveTo(11.5, 12.5);
      tip.cubicTo(8, 12, 6.5, 15, 6, 17.5);
      tip.cubicTo(5.6, 19.4, 4.4, 20.2, 3.5, 20.5);
      tip.cubicTo(7.5, 21, 12.6, 19.8, 12.6, 15.4);
      tip.closeSubpath();
      d.soft(tip, 0.4);
      d.stroke(tip);
      break;
    }
    case Icon::Normals:
      d.stroke(poly({{3, 19}, {8, 15}, {21, 15}, {16, 19}}, true));
      for (double x : {8.0, 12.5, 17.0}) {
        d.line(x, 17, x, 6);
        d.stroke(arrow_head(QPointF(x, 5.5), QPointF(0, -1), 2.4));
      }
      break;
    case Icon::Ortho:
      d.stroke(rrect(3.5, 6, 11, 11, 1.5));
      d.stroke(rrect(9.5, 3.5, 11, 11, 1.5));
      d.line(3.5, 6, 9.5, 3.5);
      d.line(14.5, 17, 20.5, 14.5);
      break;
    case Icon::Texture:
      d.stroke(rrect(3.5, 3.5, 17, 17, 2));
      d.soft(rrect(3.5, 3.5, 8.5, 8.5, 1), 0.45);
      d.soft(rrect(12, 12, 8.5, 8.5, 1), 0.45);
      break;
    case Icon::PixelPreview:
      for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
          if ((x + y) % 2 == 0) d.solid(rrect(4 + x * 5.5, 4 + y * 5.5, 4.8, 4.8, 0.8));
          else d.stroke(rrect(4 + x * 5.5, 4 + y * 5.5, 4.8, 4.8, 0.8));
      break;
    case Icon::History: {
      d.stroke(circle(12, 12, 8.5));
      d.stroke(poly({{12, 7}, {12, 12}, {15.5, 14}}, false));
      break;
    }
    case Icon::Snap: {
      QPainterPath u;
      u.moveTo(6, 4);
      u.lineTo(6, 12);
      u.arcTo(QRectF(6, 6, 12, 12), 180, 180);
      u.lineTo(18, 4);
      d.stroke(u);
      d.line(6, 8, 9.5, 8);
      d.line(14.5, 8, 18, 8);
      break;
    }
    case Icon::Home:
      d.soft(poly({{5.5, 11}, {12, 5}, {18.5, 11}, {18.5, 19.5}, {5.5, 19.5}}, true), 0.18);
      d.stroke(poly({{3.5, 11.5}, {12, 4}, {20.5, 11.5}}, false));
      d.stroke(poly({{6, 10}, {6, 19.5}, {18, 19.5}, {18, 10}}, false));
      d.stroke(rrect(10, 13.5, 4, 6, 1));
      break;
    case Icon::ImageTo3D: {
      d.stroke(rrect(2.5, 5, 9, 9, 1.6));
      d.stroke(poly({{3.5, 12.5}, {6.5, 9.5}, {9, 11.5}, {11, 10}}, false));
      d.dot(8.5, 7.5, 0.9);
      d.stroke(poly({{12.5, 9.5}, {14.5, 9.5}}, false));
      d.stroke(arrow_head({15.2, 9.5}, {1, 0}, 1.6));
      const Cube k(18, 15.5, 0.52);
      d.soft(k.top_face(), 0.35);
      d.stroke(k.outline());
      d.stroke(k.inner());
      break;
    }
    case Icon::Retopo:
      d.stroke(rrect(4, 4, 16, 16, 2));
      d.line(9.3, 4, 9.3, 20);
      d.line(14.7, 4, 14.7, 20);
      d.line(4, 9.3, 20, 9.3);
      d.line(4, 14.7, 20, 14.7);
      d.dot(14.7, 9.3, 1.4);
      break;
    case Icon::Bone: {
      d.line(8.2, 15.8, 15.8, 8.2);
      d.stroke(circle(6.4, 15.5, 2.1));
      d.stroke(circle(8.5, 17.6, 2.1));
      d.stroke(circle(15.5, 6.4, 2.1));
      d.stroke(circle(17.6, 8.5, 2.1));
      break;
    }
    case Icon::Animation:
      d.stroke(rrect(3, 5, 18, 14, 2.2));
      d.soft(poly({{10, 9}, {15.5, 12}, {10, 15}}, true), 0.5);
      d.stroke(poly({{10, 9}, {15.5, 12}, {10, 15}}, true));
      break;
    case Icon::Regions:
      d.soft(poly({{4, 6}, {11, 4}, {12.5, 11}, {5.5, 13.5}}, true), 0.4);
      d.stroke(poly({{4, 6}, {11, 4}, {12.5, 11}, {5.5, 13.5}}, true));
      d.stroke(poly({{12.5, 11}, {19.5, 9}, {20, 18}, {9, 20}, {5.5, 13.5}}, true));
      break;
    case Icon::Eraser:
      d.stroke(poly({{4.5, 15}, {13.5, 6}, {19.5, 12}, {12, 19.5}, {9, 19.5}}, true));
      d.line(9, 10.5, 15, 16.5);
      d.line(9, 19.5, 20, 19.5);
      break;
    case Icon::BoxSelect: {
      QPen dash(p.pen());
      dash.setDashPattern({2.2, 2.0});
      p.setPen(dash);
      d.stroke(rrect(4, 4, 16, 16, 1.5));
      p.setPen(QPen(c, 1.75, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      d.dot(4, 4, 1.7);
      d.dot(20, 20, 1.7);
      break;
    }
    case Icon::ClickSelect:
      d.soft(poly({{6, 4}, {18, 12}, {12.5, 13.2}, {15.5, 19.5}, {13, 20.5}, {10.2, 14.3}, {6, 18}}, true), 0.3);
      d.stroke(poly({{6, 4}, {18, 12}, {12.5, 13.2}, {15.5, 19.5}, {13, 20.5}, {10.2, 14.3}, {6, 18}}, true));
      break;
    case Icon::Folder:
      d.soft(poly({{3.5, 7}, {9.5, 7}, {11.5, 9}, {20.5, 9}, {20.5, 19}, {3.5, 19}}, true), 0.18);
      d.stroke(poly({{3.5, 19}, {3.5, 5.5}, {9.5, 5.5}, {11.5, 7.5}, {20.5, 7.5}, {20.5, 19}}, true));
      break;
  }
  (void)tone;
}

class VectorIconEngine : public QIconEngine {
 public:
  VectorIconEngine(Icon id, IconStyle style, theme::Tone tone) : id_(id), style_(style), tone_(tone) {}

  void paint(QPainter* p, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const qreal s = std::min(rect.width(), rect.height()) / 24.0;
    p->translate(rect.x() + (rect.width() - 24 * s) / 2, rect.y() + (rect.height() - 24 * s) / 2);
    p->scale(s, s);
    paint_icon(*p, id_, color(mode, state), tone_);
    p->restore();
  }

  QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
    QPixmap pm(size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    paint(&p, QRect(QPoint(0, 0), size), mode, state);
    return pm;
  }

  // `size` is already in device pixels; QIcon sets the device pixel ratio.
  QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal) override {
    return pixmap(size, mode, state);
  }

  QIconEngine* clone() const override { return new VectorIconEngine(*this); }
  QString key() const override { return QStringLiteral("pixel3d-vector"); }

 private:
  QColor color(QIcon::Mode mode, QIcon::State state) const {
    const theme::Colors& c = colors();
    if (mode == QIcon::Disabled) return c.text_disabled;
    switch (style_) {
      case IconStyle::OnAccent: return c.on_accent;
      case IconStyle::Toned: return theme::tone_color(tone_);
      case IconStyle::Default: break;
    }
    if (state == QIcon::On) return c.accent_hover;
    if (mode == QIcon::Active || mode == QIcon::Selected) return c.text;
    return c.icon;
  }

  Icon id_;
  IconStyle style_;
  theme::Tone tone_;
};

}  // namespace

QIcon icon(Icon id, IconStyle style, theme::Tone tone) { return QIcon(new VectorIconEngine(id, style, tone)); }

QIcon status_icon(pixel3d::ValidationStatus s) {
  switch (s) {
    case pixel3d::ValidationStatus::Pass: return icon(Icon::StatusPass, IconStyle::Toned, theme::Tone::Pass);
    case pixel3d::ValidationStatus::NeedsReview: return icon(Icon::StatusReview, IconStyle::Toned, theme::Tone::Review);
    case pixel3d::ValidationStatus::Fail: return icon(Icon::StatusFail, IconStyle::Toned, theme::Tone::Fail);
    case pixel3d::ValidationStatus::NotValidated: break;
  }
  return icon(Icon::StatusNone, IconStyle::Toned, theme::Tone::Neutral);
}

}  // namespace p3dgui
