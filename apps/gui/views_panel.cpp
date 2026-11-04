#include "views_panel.hpp"

#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "editor_state.hpp"
#include "issues.hpp"
#include "pixel3d/edit/selection.hpp"
#include "qt_util.hpp"
#include "theme.hpp"
#include "widgets.hpp"

namespace p3dgui {

using namespace pixel3d;

namespace {

QStringList png_files(const QMimeData* md) {
  QStringList out;
  for (const QUrl& u : md->urls()) {
    const QString f = u.toLocalFile();
    if (is_source_image(f)) out << f;
  }
  return out;
}

bool is_required(ViewRole r) { return r == ViewRole::Front || r == ViewRole::Side || r == ViewRole::Top; }

Icon view_icon(ViewRole r) {
  switch (r) {
    case ViewRole::Front:
    case ViewRole::Back: return Icon::ViewFront;
    case ViewRole::Side:
    case ViewRole::Right: return Icon::ViewSide;
    case ViewRole::Top:
    case ViewRole::Bottom: return Icon::ViewTop;
  }
  return Icon::View3D;
}

void draw_marks(QPainter& p, const std::vector<QRectF>& rects, const QRectF& img_rect, QSizeF img_size, const QColor& col,
                Qt::PenStyle style, qreal width, double fill_alpha) {
  if (img_size.isEmpty()) return;
  const qreal sx = img_rect.width() / img_size.width(), sy = img_rect.height() / img_size.height();
  p.save();
  p.setRenderHint(QPainter::Antialiasing, false);
  QPen pen(col, width, style);
  pen.setCosmetic(true);
  p.setPen(pen);
  p.setBrush(fill_alpha > 0 ? QBrush(theme::with_alpha(col, fill_alpha)) : QBrush(Qt::NoBrush));
  for (const QRectF& r : rects) {
    QRectF q(img_rect.x() + r.x() * sx, img_rect.y() + r.y() * sy, r.width() * sx, r.height() * sy);
    p.drawRect(q.adjusted(-1, -1, 1, 1));
  }
  p.restore();
}

}  // namespace

QColor part_color(int part) {
  // Golden-angle hues: neighbouring ids get clearly different colours.
  const int hue = static_cast<int>(std::fmod(std::abs(part) * 137.508 + 25.0, 360.0));
  return QColor::fromHsv(hue, 170, 235);
}

std::optional<ViewRole> role_from_filename(const QString& path) {
  const QString n = QFileInfo(path).completeBaseName().toLower();
  const std::pair<const char*, ViewRole> keys[] = {{"front", ViewRole::Front}, {"back", ViewRole::Back},
                                                   {"left", ViewRole::Side},   {"side", ViewRole::Side},
                                                   {"right", ViewRole::Right}, {"bottom", ViewRole::Bottom},
                                                   {"top", ViewRole::Top}};
  for (const auto& [k, r] : keys)
    if (n.contains(QLatin1String(k))) return r;
  return std::nullopt;
}

DrawingInfo drawing_info(const SessionView& v, ViewRole r) {
  DrawingInfo d;
  d.role = r;
  d.optional = !is_required(r) || (r != ViewRole::Front && (v.doc.settings.lowpoly.enabled || v.doc.single_view.enabled));
  for (const SourceView& s : v.doc.views) {
    if (s.role != r || !s.original) continue;
    d.assigned = true;
    d.image = to_qimage(s.aligned ? *s.aligned : *s.original);
    d.file = QFileInfo(qs(s.original_path)).fileName();
    d.size = QSize(s.original->width(), s.original->height());
    QStringList notes;
    if (s.aligned && s.scale.k > 1) notes << QObject::tr("scaled ÷%1").arg(s.scale.k);
    // What input normalisation did (the drawing itself is never changed).
    if (s.aligned && s.norm.applied) {
      if (s.norm.background_removed) notes << QObject::tr("background removed");
      if (s.norm.colours_after > 0 && s.norm.colours_after < s.norm.colours_before)
        notes << QObject::tr("%1 → %2 colours").arg(s.norm.colours_before).arg(s.norm.colours_after);
      if (s.norm.factor > 1.0001)
        notes << QObject::tr("÷%1").arg(QString::number(s.norm.factor, 'g', s.norm.factor < 10 ? 2 : 3));
    }
    if (s.aligned && s.flipped_x) notes << QObject::tr("mirrored");
    d.notes = notes.join(QStringLiteral(" · "));
  }
  return d;
}

QRectF paint_pixel_image(QPainter& p, const QImage& img, const QRectF& area, int max_zoom) {
  if (img.isNull() || area.isEmpty()) return {};
  const double fit = std::min(area.width() / img.width(), area.height() / img.height());
  // Integer zoom keeps every source pixel the same size; below 3x (thumbnails)
  // a fractional nearest-neighbour scale uses the space better.
  const double zoom = fit >= 3 ? std::min<double>(std::floor(fit), max_zoom) : fit;
  const QSizeF s(img.width() * zoom, img.height() * zoom);
  const QRectF r(std::round(area.center().x() - s.width() / 2), std::round(area.center().y() - s.height() / 2), s.width(),
                 s.height());
  // Checkerboard under transparency (one cell per source pixel when zoomed in).
  const theme::Colors& c = theme::colors();
  p.fillRect(r, c.preview_bg);
  const double cell = zoom >= 4 ? zoom : 4;
  for (double y = 0; y < s.height(); y += cell)
    for (double x = 0; x < s.width(); x += cell)
      if ((static_cast<int>(x / cell) + static_cast<int>(y / cell)) % 2)
        p.fillRect(QRectF(r.x() + x, r.y() + y, std::min(cell, s.width() - x), std::min(cell, s.height() - y)), c.checker);
  p.save();
  p.setRenderHint(QPainter::SmoothPixmapTransform, false);
  p.drawImage(r, img);
  p.restore();
  return r;
}

// ---- card -----------------------------------------------------------------------

ViewCard::ViewCard(ViewRole role, QWidget* parent) : QFrame(parent) {
  info_.role = role;
  setAcceptDrops(true);
  setCursor(Qt::PointingHandCursor);
  setMinimumHeight(76);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  setToolTip(tr("%1 drawing (%2). Drop a drawing here (PNG, JPEG, BMP, GIF, TGA, WebP, Aseprite, PSD), or click to choose one.").arg(view_title(role), view_orientation(role)));
}

QSize ViewCard::sizeHint() const { return QSize(240, 76); }

void ViewCard::setInfo(const DrawingInfo& info) {
  info_ = info;
  update();
}

void ViewCard::setMarks(const std::vector<QRectF>& rects, int issues) {
  marks_ = rects;
  issues_ = issues;
  update();
}

void ViewCard::setSelected(bool on) {
  selected_ = on;
  update();
}

void ViewCard::paintEvent(QPaintEvent*) {
  const theme::Colors& c = theme::colors();
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const QRectF box = QRectF(rect()).adjusted(0.75, 0.75, -0.75, -0.75);
  QPainterPath path;
  path.addRoundedRect(box, theme::radius::lg, theme::radius::lg);
  QColor bg = info_.assigned ? c.raised : c.panel;
  if (hover_) bg = c.hover;
  if (drag_over_) bg = theme::with_alpha(c.accent, 0.14);
  p.fillPath(path, bg);
  QPen border(selected_ || drag_over_ ? c.accent : info_.assigned ? c.border : c.border_strong, selected_ ? 1.5 : 1.0);
  if (!info_.assigned || drag_over_) border.setStyle(Qt::DashLine);
  p.setPen(border);
  p.drawPath(path);

  // Thumbnail.
  const QRectF thumb(10, (height() - 56) / 2.0, 56, 56);
  QPainterPath tp;
  tp.addRoundedRect(thumb, theme::radius::md, theme::radius::md);
  p.fillPath(tp, c.preview_bg);
  if (info_.assigned) {
    p.save();
    p.setClipPath(tp);
    const QRectF ir = paint_pixel_image(p, info_.image, thumb.adjusted(4, 4, -4, -4), 8);
    draw_marks(p, marks_, ir, info_.image.size(), c.review, Qt::SolidLine, 1.0, 0.25);
    p.restore();
  } else {
    icon(view_icon(info_.role)).paint(&p, thumb.adjusted(14, 14, -14, -14).toRect(), Qt::AlignCenter, QIcon::Disabled);
  }

  // Text.
  const int tx = 78, right = width() - 12;
  QFont title = theme::font(theme::Type::Body);
  title.setWeight(QFont::DemiBold);
  p.setFont(title);
  p.setPen(info_.assigned ? c.text : c.text_secondary);
  const int ty = static_cast<int>(thumb.top()) - 1;
  QString name = view_title(info_.role);
  p.drawText(QRect(tx, ty, right - tx - 40, 20), Qt::AlignLeft | Qt::AlignVCenter, name);
  const QFont small = theme::font(theme::Type::Small);
  p.setFont(small);
  const QFontMetrics fm(small);
  p.setPen(c.text_muted);
  p.drawText(QRect(tx, ty + 19, right - tx, 18), Qt::AlignLeft | Qt::AlignVCenter,
             fm.elidedText(view_orientation(info_.role) + (info_.optional ? tr(" · optional") : QString()),
                           Qt::ElideRight, right - tx));
  QString meta;
  if (info_.assigned) {
    meta = tr("%1 × %2 px").arg(info_.size.width()).arg(info_.size.height());
    if (!info_.notes.isEmpty()) meta += QStringLiteral(" · ") + info_.notes;
    if (!info_.file.isEmpty()) meta += QStringLiteral(" · ") + info_.file;
    p.setPen(c.text_secondary);
  } else {
    meta = info_.optional ? tr("Optional · drop an image here or click") : tr("Drop an image here or click");
    p.setPen(drag_over_ ? c.accent_hover : c.text_muted);
  }
  p.drawText(QRect(tx, ty + 37, right - tx, 18), Qt::AlignLeft | Qt::AlignVCenter, fm.elidedText(meta, Qt::ElideMiddle, right - tx));

  // State, top right: issue count or a quiet check.
  const QRect state(right - 18, ty + 1, 18, 18);
  if (issues_ > 0) {
    const QString n = QString::number(issues_);
    QFont bf = small;
    bf.setBold(true);
    const int w = std::max(18, QFontMetrics(bf).horizontalAdvance(n) + 10);
    const QRectF pill(right - w, ty + 1, w, 18);
    p.setPen(Qt::NoPen);
    p.setBrush(theme::with_alpha(c.review, 0.2));
    p.drawRoundedRect(pill, 9, 9);
    p.setPen(c.review);
    p.setFont(bf);
    p.drawText(pill, Qt::AlignCenter, n);
  } else if (info_.assigned) {
    icon(Icon::StatusPass, IconStyle::Toned, theme::Tone::Pass).paint(&p, state);
  }
}

void ViewCard::mouseReleaseEvent(QMouseEvent* e) {
  if (e->button() != Qt::LeftButton || !rect().contains(e->position().toPoint())) return;
  if (info_.assigned) emit clickedCard(info_.role);
  else emit browse(info_.role);
}

void ViewCard::contextMenuEvent(QContextMenuEvent* e) {
  QMenu m(this);
  m.addAction(info_.assigned ? tr("Replace %1 drawing…").arg(view_title(info_.role))
                             : tr("Choose %1 drawing…").arg(view_title(info_.role)),
              this, [this] { emit browse(info_.role); });
  if (info_.assigned) m.addAction(tr("Show in preview"), this, [this] { emit clickedCard(info_.role); });
  m.exec(e->globalPos());
}

void ViewCard::dragEnterEvent(QDragEnterEvent* e) {
  if (png_files(e->mimeData()).isEmpty()) return;
  e->acceptProposedAction();
  drag_over_ = true;
  update();
}

void ViewCard::dragLeaveEvent(QDragLeaveEvent*) {
  drag_over_ = false;
  update();
}

void ViewCard::dropEvent(QDropEvent* e) {
  drag_over_ = false;
  update();
  const QStringList files = png_files(e->mimeData());
  if (files.isEmpty()) return;
  e->acceptProposedAction();
  emit filesDropped(files, info_.role);
}

void ViewCard::enterEvent(QEnterEvent*) {
  hover_ = true;
  update();
}

void ViewCard::leaveEvent(QEvent*) {
  hover_ = false;
  update();
}

// ---- preview ----------------------------------------------------------------------

SourcePreview::SourcePreview(QWidget* parent) : QWidget(parent) {
  setMinimumHeight(140);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  setMouseTracking(true);
}

void SourcePreview::setEditMarks(const EditMarks& m) {
  edit_ = m;
  update();
}

void SourcePreview::setInput(Input mode, double brush_radius) {
  input_ = mode;
  brush_radius_ = brush_radius;
  setCursor(mode == Input::None ? Qt::ArrowCursor : Qt::CrossCursor);
  update();
}

QPointF SourcePreview::toPixel(QPointF pos) const {
  if (img_rect_.isEmpty() || img_.isNull()) return {-1, -1};
  return {(pos.x() - img_rect_.x()) / img_rect_.width() * img_.width(),
          (pos.y() - img_rect_.y()) / img_rect_.height() * img_.height()};
}

QPoint SourcePreview::widgetPos(QPointF pixel) const {
  return QPoint(static_cast<int>(std::floor(img_rect_.x() + pixel.x() / img_.width() * img_rect_.width())),
                static_cast<int>(std::floor(img_rect_.y() + pixel.y() / img_.height() * img_rect_.height())));
}

void SourcePreview::mousePressEvent(QMouseEvent* e) {
  if (e->button() != Qt::LeftButton || input_ == Input::None || img_.isNull()) return;
  pressed_ = true;
  press_px_ = hover_ = toPixel(e->position());
  points_ = {press_px_};
  if (input_ == Input::Brush) emit stroke(points_, false);
  update();
}

void SourcePreview::mouseMoveEvent(QMouseEvent* e) {
  hover_ = toPixel(e->position());
  if (pressed_ && input_ == Input::Brush) {
    points_.push_back(hover_);
    emit stroke(points_, false);
  }
  update();
}

void SourcePreview::mouseReleaseEvent(QMouseEvent* e) {
  if (!pressed_ || e->button() != Qt::LeftButton) return;
  pressed_ = false;
  const QPointF px = toPixel(e->position());
  const double zoom = img_.isNull() ? 1.0 : img_rect_.width() / img_.width();
  const bool moved = (px - press_px_).manhattanLength() * zoom > 3;
  if (input_ == Input::Rect) {
    if (moved) {
      const int x0 = static_cast<int>(std::floor(std::min(px.x(), press_px_.x())));
      const int y0 = static_cast<int>(std::floor(std::min(px.y(), press_px_.y())));
      const int x1 = static_cast<int>(std::ceil(std::max(px.x(), press_px_.x())));
      const int y1 = static_cast<int>(std::ceil(std::max(px.y(), press_px_.y())));
      emit rectDragged(QRect(QPoint(x0, y0), QPoint(x1 - 1, y1 - 1)).intersected(QRect(0, 0, img_.width(), img_.height())));
    }
  } else if (input_ == Input::Brush) {
    if (points_.isEmpty() || points_.back() != px) points_.push_back(px);
    emit stroke(points_, true);
  } else if (input_ == Input::Click && !moved) {
    emit clicked(px);
  }
  points_.clear();
  update();
}

void SourcePreview::leaveEvent(QEvent*) {
  hover_ = {-1, -1};
  update();
}

void SourcePreview::setImage(const QImage& img, const QString& empty_text) {
  img_ = img;
  empty_text_ = empty_text;
  update();
}

void SourcePreview::setCompare(const QImage& img) {
  compare_ = img;
  update();
}

void SourcePreview::setMarks(const std::vector<QRectF>& issues, const std::vector<QRectF>& focus,
                             const std::vector<QRectF>& accent) {
  issues_ = issues;
  focus_ = focus;
  accent_ = accent;
  update();
}

void SourcePreview::setOptions(bool compare, bool issues, bool grid) {
  show_compare_ = compare;
  show_issues_ = issues;
  show_grid_ = grid;
  update();
}

void SourcePreview::paintEvent(QPaintEvent*) {
  const theme::Colors& c = theme::colors();
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  QPainterPath bg;
  bg.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), theme::radius::lg, theme::radius::lg);
  p.fillPath(bg, c.preview_bg);
  p.setPen(QPen(c.border, 1));
  p.drawPath(bg);
  if (img_.isNull()) {
    p.setPen(c.text_muted);
    p.setFont(theme::font(theme::Type::Small));
    p.drawText(rect().adjusted(16, 16, -16, -16), Qt::AlignCenter | Qt::TextWordWrap, empty_text_);
    return;
  }
  p.setClipPath(bg);
  const QRectF r = paint_pixel_image(p, img_, QRectF(rect()).adjusted(12, 12, -12, -12));
  img_rect_ = r;
  const double zoom = r.width() / img_.width();
  if (show_compare_ && !compare_.isNull()) {
    p.setOpacity(0.75);
    p.drawImage(r, compare_);
    p.setOpacity(1);
  }
  // Editor overlays: part masks, unmatched mask pixels, the selection (with an outline), a stroke in progress.
  p.save();
  p.setRenderHint(QPainter::SmoothPixmapTransform, false);
  for (const QImage* layer : {&edit_.masks, &edit_.unmatched, &edit_.selection, &edit_.pending})
    if (!layer->isNull()) p.drawImage(r, *layer);
  p.restore();
  if (!edit_.selection.isNull()) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    QPen pen(theme::colors().accent_hover, 1.5);
    pen.setCosmetic(true);
    p.setPen(pen);
    const QImage& s = edit_.selection;
    auto on = [&](int x, int y) { return x >= 0 && y >= 0 && x < s.width() && y < s.height() && qAlpha(s.pixel(x, y)) > 0; };
    for (int y = 0; y < s.height(); ++y)
      for (int x = 0; x < s.width(); ++x) {
        if (!on(x, y)) continue;
        const qreal X = r.x() + x * zoom, Y = r.y() + y * zoom;
        if (!on(x - 1, y)) p.drawLine(QPointF(X, Y), QPointF(X, Y + zoom));
        if (!on(x + 1, y)) p.drawLine(QPointF(X + zoom, Y), QPointF(X + zoom, Y + zoom));
        if (!on(x, y - 1)) p.drawLine(QPointF(X, Y), QPointF(X + zoom, Y));
        if (!on(x, y + 1)) p.drawLine(QPointF(X, Y + zoom), QPointF(X + zoom, Y + zoom));
      }
    p.restore();
  }
  if (show_grid_ && zoom >= 6) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(c.pixel_grid);
    for (int x = 0; x <= img_.width(); ++x) p.drawLine(QPointF(r.x() + x * zoom, r.top()), QPointF(r.x() + x * zoom, r.bottom()));
    for (int y = 0; y <= img_.height(); ++y) p.drawLine(QPointF(r.left(), r.y() + y * zoom), QPointF(r.right(), r.y() + y * zoom));
    p.restore();
  }
  if (show_issues_) {
    draw_marks(p, issues_, r, img_.size(), c.review, Qt::DashLine, 1.5, 0);
    draw_marks(p, focus_, r, img_.size(), c.review, Qt::SolidLine, 2.0, 0.18);
  }
  draw_marks(p, accent_, r, img_.size(), c.accent_hover, Qt::SolidLine, 2.0, 0.15);
  // Tool cursor: the brush footprint, or the rectangle being dragged.
  if (input_ == Input::Brush && hover_.x() >= 0 && brush_radius_ > 0) {
    p.setPen(QPen(theme::with_alpha(c.text, 0.7), 1, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    const double rad = std::max(0.5, brush_radius_) * zoom;
    p.drawEllipse(QPointF(r.x() + hover_.x() * zoom, r.y() + hover_.y() * zoom), rad, rad);
  }
  if (input_ == Input::Rect && pressed_) {
    const QRectF box = QRectF(QPointF(r.x() + press_px_.x() * zoom, r.y() + press_px_.y() * zoom),
                              QPointF(r.x() + hover_.x() * zoom, r.y() + hover_.y() * zoom))
                           .normalized();
    p.setPen(QPen(c.accent_hover, 1.5, Qt::DashLine));
    p.setBrush(theme::with_alpha(c.accent, 0.15));
    p.drawRect(box);
  }
}

// ---- missing views ------------------------------------------------------------------

MissingViewsNotice::MissingViewsNotice(bool narrow, QWidget* parent) : Card(parent) {
  setObjectName(QStringLiteral("missingViewsNotice"));
  setTone(theme::Tone::Review);
  auto* lay = new QVBoxLayout(this);
  lay->setContentsMargins(theme::space::lg, theme::space::md, theme::space::md, theme::space::md);
  lay->setSpacing(theme::space::sm);
  text_ = make_label(QString(), nullptr);
  text_->setObjectName(QStringLiteral("missingViewsText"));
  text_->setWordWrap(true);
  lay->addWidget(text_);
  const QString label = tr("Switch to Low-Poly Studio (no Top needed)");
  switch_ = make_button(narrow ? QString(label).replace(QStringLiteral(" ("), QStringLiteral("\n(")) : label, "primary");
  switch_->setObjectName(QStringLiteral("switchToLowpoly"));
  switch_->setToolTip(tr("Keeps your drawings; one undoable step (Ctrl+Z switches back)"));
  add_ = make_button(QString(), "link");
  add_->setObjectName(QStringLiteral("addMissingView"));
  if (narrow)  // never wider than the panel: the text is short enough for its usual width
    for (QPushButton* b : {switch_, add_}) b->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  lay->addWidget(switch_);
  lay->addWidget(add_, 0, narrow ? Qt::Alignment() : Qt::AlignLeft);
  connect(switch_, &QPushButton::clicked, this, &MissingViewsNotice::switchRequested);
  connect(add_, &QPushButton::clicked, this, [this] { emit addRequested(add_role_); });
  hide();
}

void MissingViewsNotice::update_from(const Document& d) {
  const bool show = lowpoly_can_rescue(d);
  if (show) {
    const std::vector<ViewRole> m = missing_views(d);
    add_role_ = std::find(m.begin(), m.end(), ViewRole::Top) != m.end() ? ViewRole::Top : m.front();
    text_->setText(missing_views_text(d));
    add_->setText(tr("or add the %1 drawing…").arg(view_title(add_role_)));
  }
  setVisible(show);
}

// ---- panel --------------------------------------------------------------------------

ViewsPanel::ViewsPanel(QWidget* parent) : QWidget(parent) {
  setAcceptDrops(true);
  setProperty("surface", "panel");
  setAttribute(Qt::WA_StyledBackground, true);
  auto* lay = new QVBoxLayout(this);
  lay->setContentsMargins(theme::space::lg, theme::space::xs, theme::space::lg, theme::space::lg);
  lay->setSpacing(theme::space::md);
  missing_ = new MissingViewsNotice(true);
  lay->addWidget(missing_);
  connect(missing_, &MissingViewsNotice::switchRequested, this, [this] { emit projectTypeRequested(true); });
  connect(missing_, &MissingViewsNotice::addRequested, this, &ViewsPanel::browseRequested);

  auto* cards = new QWidget;
  auto* cl = new QVBoxLayout(cards);
  cl->setContentsMargins(0, 0, 0, 0);
  cl->setSpacing(theme::space::sm);
  more_ = new Section(tr("More views · optional"), nullptr, false);
  auto* hint = make_label(tr("Back, Right and Bottom remove guesswork when parts don't match between views."), "small");
  hint->setWordWrap(true);
  more_->body()->addWidget(hint);
  for (ViewRole r : kAllViewRoles) {
    auto* card = new ViewCard(r);
    cards_[static_cast<size_t>(r)] = card;
    if (is_required(r)) cl->addWidget(card);
    else more_->body()->addWidget(card);
    connect(card, &ViewCard::clickedCard, this, [this](ViewRole role) {
      setRole(role);
      emit roleChanged(role);
    });
    connect(card, &ViewCard::browse, this, &ViewsPanel::browseRequested);
    connect(card, &ViewCard::filesDropped, this, [this](const QStringList& files, ViewRole role) {
      emit filesDropped(files, files.size() == 1 ? std::optional<ViewRole>(role) : std::nullopt);
    });
  }
  cl->addSpacing(theme::space::xs);
  cl->addWidget(more_);
  auto* scroll = new QScrollArea;
  scroll->setWidget(cards);
  scroll->setWidgetResizable(true);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setMinimumHeight(120);
  lay->addWidget(scroll);
  auto fit_cards = [scroll, cards] { scroll->setMaximumHeight(cards->sizeHint().height() + 2); };
  connect(more_, &Section::toggled, this, [fit_cards] { fit_cards(); });
  fit_cards();

  // Preview of the selected drawing.
  auto* head = new QHBoxLayout;
  head->setSpacing(theme::space::xs);
  preview_title_ = make_label(QString(), "section");
  head->addWidget(preview_title_, 1);
  auto toggle = [&](Icon ic, const QString& tip, bool on) {
    auto* b = new QToolButton;
    b->setIcon(icon(ic));
    b->setIconSize(QSize(theme::icon_size::sm, theme::icon_size::sm));
    b->setCheckable(true);
    b->setChecked(on);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setCursor(Qt::PointingHandCursor);
    head->addWidget(b);
    connect(b, &QToolButton::toggled, this, [this] { refreshPreview(); });
    return b;
  };
  compare_ = toggle(Icon::Compare, tr("Compare with the model: grey matches, red = only in the model, blue = only in the drawing, yellow = colour differs"), false);
  issues_ = toggle(Icon::IssuesOverlay, tr("Show issue areas"), true);
  grid_ = toggle(Icon::PixelGrid, tr("Pixel grid"), true);
  connect(compare_, &QToolButton::toggled, this, [this](bool on) {
    if (on) emit compareRequested(role_);
  });
  lay->addLayout(head);
  preview_ = new SourcePreview;
  lay->addWidget(preview_, 1);
  legend_ = make_label(QString(), "small");
  legend_->setWordWrap(true);
  legend_->setTextFormat(Qt::RichText);
  lay->addWidget(legend_);
  connect(preview_, &SourcePreview::clicked, this, [this](QPointF px) {
    emit pixelClicked(role_, static_cast<int>(std::floor(px.x())), static_cast<int>(std::floor(px.y())));
  });
  connect(preview_, &SourcePreview::stroke, this,
          [this](const QVector<QPointF>& pts, bool finished) { emit strokeChanged(role_, pts, finished); });
  connect(preview_, &SourcePreview::rectDragged, this, [this](QRect r) { emit regionChosen(role_, r); });
  setRole(ViewRole::Front);
}

void ViewsPanel::setEditor(EditorState* e) {
  editor_ = e;
  connect(e, &EditorState::changed, this, [this] { refreshPreview(); });
  refreshPreview();
}

void ViewsPanel::setSelection(const std::set<int>& parts, int region) {
  if (parts == sel_parts_ && region == sel_region_) return;
  sel_parts_ = parts;
  sel_region_ = region;
  refreshPreview();
}

void ViewsPanel::setPending(const QVector<QPoint>& pixels, const QColor& color) {
  pending_ = pixels;
  pending_color_ = color;
  refreshPreview();
}

bool ViewsPanel::compareOn() const { return compare_->isChecked(); }

void ViewsPanel::setRole(ViewRole r) {
  const bool changed = r != role_;
  role_ = r;
  for (ViewCard* c : cards_) c->setSelected(c->role() == r);
  if (changed && compare_->isChecked()) emit compareRequested(r);
  refreshPreview();
}

void ViewsPanel::update_from(const SessionView& v) {
  const bool changed = v.doc.revision != view_.doc.revision;
  view_ = v;
  bool optional_assigned = false;
  for (ViewCard* c : cards_) {
    const DrawingInfo info = drawing_info(v, c->role());
    c->setInfo(info);
    optional_assigned |= info.assigned && !is_required(c->role());
  }
  missing_->update_from(v.doc);
  if (optional_assigned && !more_auto_opened_) {
    more_->setExpanded(true, false);
    more_auto_opened_ = true;
  }
  if (changed) {
    compare_img_.clear();
    counts_.clear();
    pending_.clear();
    if (compare_->isChecked() && v.doc.texture) emit compareRequested(role_);
  }
  refreshPreview();
}

void ViewsPanel::setMarks(const ViewMarks& all, const ViewMarks& focus, const ViewMarks& accent,
                          const std::map<ViewRole, int>& counts) {
  all_ = all;
  focus_ = focus;
  accent_ = accent;
  for (ViewCard* c : cards_) {
    const auto it = all.find(c->role());
    const auto n = counts.find(c->role());
    c->setMarks(it == all.end() ? std::vector<QRectF>{} : it->second, n == counts.end() ? 0 : n->second);
  }
  refreshPreview();
}

void ViewsPanel::setCompare(int r, const QImage& img, const QString& counts) {
  compare_img_[r] = img;
  counts_[r] = counts;
  refreshPreview();
}

void ViewsPanel::refreshPreview() {
  const theme::Colors& c = theme::colors();
  const DrawingInfo info = drawing_info(view_, role_);
  preview_title_->setText(tr("%1 drawing").arg(view_title(role_)).toUpper());
  preview_->setImage(info.image, tr("No %1 drawing yet.\nDrop an image on its card above.").arg(view_title(role_)));
  auto get = [&](const ViewMarks& m) {
    const auto it = m.find(role_);
    return it == m.end() ? std::vector<QRectF>{} : it->second;
  };
  preview_->setMarks(get(all_), get(focus_), get(accent_));
  const auto it = compare_img_.find(static_cast<int>(role_));
  preview_->setCompare(it == compare_img_.end() ? QImage() : it->second);
  preview_->setOptions(compare_->isChecked(), issues_->isChecked(), grid_->isChecked());
  // Editing: what the mouse does, and the editor's marks on this drawing.
  const Tool tool = editor_ ? editor_->tool() : Tool::Select;
  SourcePreview::Input input = SourcePreview::Input::Click;
  double radius = 0;
  if (tool == Tool::Depth) input = SourcePreview::Input::Brush, radius = editor_->depth_brush.radius;
  if (tool == Tool::Parts && editor_->mask_part >= 0) input = SourcePreview::Input::Brush, radius = editor_->mask_radius;
  if (tool == Tool::Volume) input = SourcePreview::Input::Rect;
  preview_->setInput(info.assigned ? input : SourcePreview::Input::None, radius);
  EditMarks marks;
  const SourceView* src = nullptr;
  for (const SourceView& sv : view_.doc.views)
    if (sv.role == role_) src = &sv;
  if (src && src->aligned) {
    const int w = src->aligned->width(), h = src->aligned->height();
    auto layer = [&] {
      QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
      img.fill(Qt::transparent);
      return img;
    };
    auto fill = [&](QImage& img, const Mask2D& m, QColor col) {
      for (int y = 0; y < std::min(h, m.height()); ++y)
        for (int x = 0; x < std::min(w, m.width()); ++x)
          if (m.get(x, y)) img.setPixelColor(x, y, col);
    };
    if (!view_.doc.masks.empty()) {
      marks.masks = layer();
      for (const auto& [id, up] : view_.doc.masks.parts)
        if (const auto it = up.masks.find(role_); it != up.masks.end())
          fill(marks.masks, it->second, theme::with_alpha(part_color(id), editor_ && id == editor_->mask_part ? 0.6 : 0.38));
    }
    if (view_.doc.mask_conflicts) {
      bool any = false;
      QImage un = layer();
      for (const MaskConflict& mc : *view_.doc.mask_conflicts)
        if (mc.view == role_) fill(un, mc.unmatched, theme::with_alpha(c.fail, 0.75)), any = true;
      if (any) marks.unmatched = un;
    }
    if ((!sel_parts_.empty() || sel_region_ >= 0) && view_.doc.solid && view_.doc.align) {
      Selection sel;
      sel.mode = sel_region_ >= 0 ? SelectMode::Face : SelectMode::Part;
      sel.parts.assign(sel_parts_.begin(), sel_parts_.end());
      sel.face = sel_region_;
      const auto masks = selection_masks(view_.doc.solid->mesh, sel, view_.doc.align->dims, {role_});
      if (const auto it = masks.find(role_); it != masks.end() && it->second.count() > 0) {
        marks.selection = layer();
        fill(marks.selection, it->second, theme::with_alpha(c.accent, 0.35));
      }
    }
    if (!pending_.isEmpty()) {
      marks.pending = layer();
      for (const QPoint& q : pending_)
        if (q.x() >= 0 && q.y() >= 0 && q.x() < w && q.y() < h) marks.pending.setPixelColor(q, pending_color_);
    }
  }
  preview_->setEditMarks(marks);
  if (compare_->isChecked()) {
    if (!view_.doc.texture) {
      legend_->setText(tr("Comparing needs a reconstructed, textured model."));
    } else if (it == compare_img_.end()) {
      legend_->setText(tr("Comparing…"));
    } else {
      auto dot = [](const QColor& col, const QString& t) {
        return QStringLiteral("<span style='color:%1'>■</span>&nbsp;%2").arg(col.name(), t);
      };
      legend_->setText(QStringList{dot(c.compare_match, tr("matches")), dot(c.compare_extra, tr("extra")),
                                   dot(c.compare_missing, tr("missing")), dot(c.compare_colour, tr("colour"))}
                           .join(QStringLiteral("&nbsp;&nbsp; ")));
      legend_->setToolTip(counts_[static_cast<int>(role_)]);
    }
    legend_->show();
  } else {
    legend_->clear();
    legend_->hide();
  }
}

void ViewsPanel::dragEnterEvent(QDragEnterEvent* e) {
  if (!png_files(e->mimeData()).isEmpty()) e->acceptProposedAction();
}

void ViewsPanel::dropEvent(QDropEvent* e) {
  const QStringList files = png_files(e->mimeData());
  if (files.isEmpty()) return;
  e->acceptProposedAction();
  emit filesDropped(files, std::nullopt);
}

}  // namespace p3dgui
