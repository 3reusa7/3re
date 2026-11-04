#include "studio/preview_bar.hpp"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTimer>
#include <QToolButton>

#include "icons.hpp"
#include "viewport.hpp"
#include "widgets.hpp"

namespace p3dgui {

PreviewBar::PreviewBar(Viewport* viewport, QWidget* parent, bool two_rows) : QWidget(parent), viewport_(viewport) {
  setObjectName("PreviewBar");
  auto* row = new QHBoxLayout;
  auto* display_row = row;
  if (two_rows) {
    auto* stack = new QVBoxLayout(this);
    stack->setContentsMargins(0, 0, 0, 0);
    stack->setSpacing(2);
    stack->addLayout(row);
    display_row = new QHBoxLayout;
    display_row->setContentsMargins(0, 0, 0, 0);
    display_row->setSpacing(2);
    stack->addLayout(display_row);
  } else {
    setLayout(row);
  }
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(2);
  auto text_button = [this, row](const QString& text, const QString& tip, auto fn) {
    auto* b = new QToolButton(this);
    b->setText(text);
    b->setProperty("kind", "text");
    b->setToolTip(tip);
    b->setCursor(Qt::PointingHandCursor);
    connect(b, &QToolButton::clicked, this, fn);
    row->addWidget(b);
    return b;
  };
  auto yaw = [this](float y) {
    auto& s = viewport_->state();
    s.yaw = y;
    s.pitch = 0;
    s.ortho = true;
    s.preset.reset();
    viewport_->refresh();
  };
  text_button(tr("Front"), tr("Look from the front"), [this] { viewport_->setPreset(pixel3d::ViewRole::Front); });
  text_button(tr("Back"), tr("Look from the back"), [yaw] { yaw(180.f); });
  text_button(tr("Left"), tr("Look from the left"), [yaw] { yaw(270.f); });
  text_button(tr("Right"), tr("Look from the right"), [this] { viewport_->setPreset(pixel3d::ViewRole::Side); });
  text_button(tr("Top"), tr("Look from above"), [this] { viewport_->setPreset(pixel3d::ViewRole::Top); });
  text_button(tr("3D"), tr("Perspective view (drag to orbit, right-drag to pan, scroll to zoom)"), [this] { viewport_->setIso(); })
      ->setObjectName("previewIso");
  if (two_rows) row->addStretch(1);
  else row->addWidget(make_vdivider(6, 6));
  auto toggle = [this, display_row](Icon ic, const QString& tip, auto fn) {
    auto* b = new QToolButton(this);
    b->setIcon(icon(ic));
    b->setIconSize(QSize(theme::icon_size::md, theme::icon_size::md));
    b->setCheckable(true);
    b->setToolTip(tip);
    b->setCursor(Qt::PointingHandCursor);
    connect(b, &QToolButton::toggled, this, fn);
    display_row->addWidget(b);
    return b;
  };
  auto* wire = toggle(Icon::Wireframe, tr("Wireframe: show the triangles"), [this](bool on) {
    viewport_->state().wireframe = on;
    viewport_->refresh();
  });
  wire->setObjectName("previewWireframe");
  auto* flat = toggle(Icon::Lighting, tr("True colour: base colours without preview lighting (exported materials are unchanged)"), [this](bool on) {
    viewport_->state().lit = !on;
    viewport_->refresh();
  });
  flat->setObjectName("image3dTrueColor");
  spin_ = new QTimer(this);
  spin_->setInterval(16);
  connect(spin_, &QTimer::timeout, this, [this] {
    viewport_->state().orbit(0.6f, 0);
    viewport_->refresh();
  });
  turn_ = toggle(Icon::Turntable, tr("Turntable: rotate the model slowly"), [this](bool on) {
    if (on) spin_->start();
    else spin_->stop();
  });
  turn_->setObjectName("previewTurntable");
  display_row->addStretch(1);
}

bool PreviewBar::turntable() const { return spin_->isActive(); }

void PreviewBar::setTurntable(bool on) { turn_->setChecked(on); }

}  // namespace p3dgui
