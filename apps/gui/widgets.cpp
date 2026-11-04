#include "widgets.hpp"

#include <QAction>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include <QVariantAnimation>

namespace p3dgui {

QLabel* make_label(const QString& text, const char* type, QWidget* parent) {
  auto* l = new QLabel(text, parent);
  if (type) l->setProperty("type", type);
  if (type && qstrcmp(type, "section") == 0) l->setText(text.toUpper());
  return l;
}

QPushButton* make_button(const QString& text, const char* kind, const QIcon& icon, QWidget* parent) {
  auto* b = new QPushButton(icon, text, parent);
  if (kind) b->setProperty("kind", kind);
  b->setCursor(Qt::PointingHandCursor);
  if (!icon.isNull()) b->setIconSize(QSize(theme::icon_size::sm, theme::icon_size::sm));
  return b;
}

QToolButton* make_tool_button(QAction* action, QWidget* parent, int icon_px) {
  auto* b = new QToolButton(parent);
  b->setDefaultAction(action);
  b->setIconSize(QSize(icon_px, icon_px));
  b->setToolButtonStyle(Qt::ToolButtonIconOnly);
  b->setAutoRaise(true);
  b->setCursor(Qt::PointingHandCursor);
  QString tip = action->text().remove('&');
  if (!action->shortcut().isEmpty()) tip += QStringLiteral("  (%1)").arg(action->shortcut().toString(QKeySequence::NativeText));
  action->setToolTip(tip);
  return b;
}

QFrame* make_divider(QWidget* parent) {
  auto* f = new QFrame(parent);
  f->setProperty("divider", true);
  f->setFrameShape(QFrame::NoFrame);
  return f;
}

QWidget* make_vdivider(int hmargin, int vmargin) {
  auto* host = new QWidget;
  auto* l = new QHBoxLayout(host);
  l->setContentsMargins(hmargin, vmargin, hmargin, vmargin);
  auto* f = new QFrame;
  f->setProperty("vdivider", true);
  f->setFrameShape(QFrame::NoFrame);
  l->addWidget(f);
  return host;
}

QLabel* make_chip(const QString& text, theme::Tone tone, QWidget* parent) {
  auto* l = new QLabel(parent);
  l->setProperty("chip", true);
  set_chip(l, text, tone);
  return l;
}

void set_chip(QLabel* chip, const QString& text, theme::Tone tone) {
  chip->setText(text);
  theme::set_prop(chip, "tone", theme::tone_name(tone));
}

// ---- Section ------------------------------------------------------------------

Section::Section(const QString& title, QWidget* parent, bool expanded) : QWidget(parent), expanded_(expanded) {
  auto* lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(theme::space::sm);
  auto* head = new QHBoxLayout;
  head->setContentsMargins(0, 0, 0, 0);
  head->setSpacing(theme::space::sm);
  header_ = new QPushButton;
  header_->setProperty("kind", "section");
  header_->setIconSize(QSize(14, 14));
  header_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  header_->setCursor(Qt::PointingHandCursor);
  header_->setFocusPolicy(Qt::TabFocus);
  head->addWidget(header_, 1);
  trailing_host_ = new QWidget;
  auto* tl = new QHBoxLayout(trailing_host_);
  tl->setContentsMargins(0, 0, 0, 0);
  tl->setSpacing(theme::space::xs);
  head->addWidget(trailing_host_);
  lay->addLayout(head);
  body_ = new QWidget;
  body_layout_ = new QVBoxLayout(body_);
  body_layout_->setContentsMargins(0, 0, 0, theme::space::xs);
  body_layout_->setSpacing(theme::space::sm);
  lay->addWidget(body_);
  anim_ = new QVariantAnimation(this);
  anim_->setDuration(theme::motion::normal);
  anim_->setEasingCurve(QEasingCurve::OutCubic);
  connect(anim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) { body_->setMaximumHeight(v.toInt()); });
  connect(anim_, &QVariantAnimation::finished, this, [this] {
    if (expanded_) body_->setMaximumHeight(QWIDGETSIZE_MAX);
    else body_->hide();
  });
  connect(header_, &QPushButton::clicked, this, [this] {
    if (collapsible_) setExpanded(!expanded_);
  });
  setTitle(title);
  body_->setVisible(expanded_);
  header_->setIcon(icon(expanded_ ? Icon::ChevronDown : Icon::ChevronRight));
}

void Section::setTitle(const QString& t) { header_->setText(t.toUpper()); }

void Section::setTrailing(QWidget* w) { trailing_host_->layout()->addWidget(w); }

void Section::setCollapsible(bool on) {
  collapsible_ = on;
  header_->setIcon(on ? icon(expanded_ ? Icon::ChevronDown : Icon::ChevronRight) : QIcon());
  header_->setCursor(on ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void Section::setExpanded(bool on, bool animate) {
  if (on == expanded_ && body_->isVisible() == on) return;
  expanded_ = on;
  if (collapsible_) header_->setIcon(icon(on ? Icon::ChevronDown : Icon::ChevronRight));
  anim_->stop();
  if (!animate || !isVisible()) {
    body_->setMaximumHeight(QWIDGETSIZE_MAX);
    body_->setVisible(on);
    emit toggled(on);
    return;
  }
  const int full = body_->sizeHint().height();
  body_->show();
  anim_->setStartValue(on ? 0 : body_->height());
  anim_->setEndValue(on ? full : 0);
  anim_->start();
  emit toggled(on);
}

// ---- DockTitle ------------------------------------------------------------------

DockTitle::DockTitle(const QString& title, QWidget* parent) : QWidget(parent) {
  setProperty("surface", "panel");
  setAttribute(Qt::WA_StyledBackground, true);
  row_ = new QHBoxLayout(this);
  row_->setContentsMargins(theme::space::lg, theme::space::md, theme::space::sm, theme::space::xs);
  row_->setSpacing(theme::space::xs);
  row_->addWidget(make_label(title, "section"));
  row_->addStretch(1);
  auto* close = new QToolButton;
  close->setIcon(icon(Icon::Close));
  close->setIconSize(QSize(14, 14));
  close->setAutoRaise(true);
  close->setToolTip(tr("Hide panel (View ▸ Panels shows it again)"));
  close->setCursor(Qt::PointingHandCursor);
  row_->addWidget(close);
  connect(close, &QToolButton::clicked, this, &DockTitle::closeRequested);
}

void DockTitle::addTrailing(QWidget* w) { row_->insertWidget(row_->count() - 1, w); }

// ---- Card -------------------------------------------------------------------------

Card::Card(QWidget* parent) : QFrame(parent) { setFrameShape(QFrame::NoFrame); }

void Card::setTone(std::optional<theme::Tone> t) {
  tone_ = t;
  update();
}

void Card::paintEvent(QPaintEvent*) {
  const theme::Colors& c = theme::colors();
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
  QPainterPath path;
  path.addRoundedRect(r, theme::radius::lg, theme::radius::lg);
  p.fillPath(path, c.raised);
  if (tone_) {
    p.save();
    p.setClipPath(path);
    p.fillRect(QRectF(r.left(), r.top(), 3, r.height()), theme::tone_color(*tone_));
    p.restore();
  }
  p.setPen(QPen(c.border, 1));
  p.drawPath(path);
}

}  // namespace p3dgui
