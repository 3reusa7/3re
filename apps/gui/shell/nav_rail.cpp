#include "shell/nav_rail.hpp"

#include <QButtonGroup>
#include <QToolButton>
#include <QVBoxLayout>

#include "theme.hpp"

namespace p3dgui {

NavRail::NavRail(QWidget* parent) : QWidget(parent) {
  setObjectName("NavRail");
  setProperty("surface", "rail");
  setAttribute(Qt::WA_StyledBackground);
  setFixedWidth(88);
  layout_ = new QVBoxLayout(this);
  layout_->setContentsMargins(8, 12, 8, 12);
  layout_->setSpacing(4);
  group_ = new QButtonGroup(this);
  group_->setExclusive(true);
}

int NavRail::addItem(const QIcon& icon, const QString& label, const QString& id) {
  auto* b = new QToolButton(this);
  b->setObjectName("nav_" + id);
  b->setProperty("kind", "nav");
  b->setIcon(icon);
  b->setIconSize(QSize(theme::icon_size::lg, theme::icon_size::lg));
  b->setText(label);
  b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
  b->setCheckable(true);
  b->setCursor(Qt::PointingHandCursor);
  b->setFocusPolicy(Qt::TabFocus);
  b->setMinimumSize(72, 64);
  b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  b->setAccessibleName(label);
  const int index = static_cast<int>(buttons_.size());
  group_->addButton(b, index);
  buttons_.insert(id, b);
  layout_->addWidget(b);
  connect(b, &QToolButton::clicked, this, [this, id] { setCurrent(id); });
  return index;
}

void NavRail::addStretch() { layout_->addStretch(1); }

void NavRail::setCurrent(const QString& id) {
  auto* b = buttons_.value(id);
  if (!b) return;
  b->setChecked(true);
  if (current_ == id) return;
  current_ = id;
  emit selected(id);
}

}  // namespace p3dgui
