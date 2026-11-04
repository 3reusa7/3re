#include "shell/step_bar.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

#include "theme.hpp"

namespace p3dgui {

StepBar::StepBar(const QStringList& steps, QWidget* parent) : QWidget(parent), steps_(steps) {
  setObjectName("StepBar");
  auto* row = new QHBoxLayout(this);
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(6);
  for (int i = 0; i < steps.size(); ++i) {
    if (i) {
      auto* line = new QLabel(this);
      line->setProperty("stepLink", true);
      line->setFixedSize(28, 2);
      row->addWidget(line, 0, Qt::AlignVCenter);
    }
    auto* b = new QPushButton(this);
    b->setObjectName(QStringLiteral("step_%1").arg(i));
    b->setProperty("kind", "step");
    b->setCursor(Qt::PointingHandCursor);
    b->setFocusPolicy(Qt::TabFocus);
    connect(b, &QPushButton::clicked, this, [this, i] { emit stepClicked(i); });
    buttons_.append(b);
    done_.append(false);
    row->addWidget(b);
  }
  row->addStretch(1);
  refresh();
}

void StepBar::setCurrent(int index) {
  current_ = std::clamp(index, 0, int(steps_.size()) - 1);
  refresh();
}

void StepBar::setCompleted(int index, bool done) {
  if (index < 0 || index >= done_.size()) return;
  done_[index] = done;
  refresh();
}

void StepBar::refresh() {
  for (int i = 0; i < buttons_.size(); ++i) {
    const bool cur = i == current_, done = done_[i];
    auto* b = buttons_[i];
    b->setText(QStringLiteral("%1  %2").arg(done && !cur ? QStringLiteral("✓") : QString::number(i + 1), steps_[i]));
    b->setEnabled(cur || done);
    theme::set_prop(b, "step", cur ? "current" : done ? "done" : "todo");
    b->setToolTip(cur ? tr("Current step") : done ? tr("Go back to %1").arg(steps_[i]) : tr("Finish the earlier steps first"));
    b->setAccessibleName(QStringLiteral("%1 %2 (%3)").arg(i + 1).arg(steps_[i], cur ? tr("current") : done ? tr("done") : tr("not reached")));
  }
}

}  // namespace p3dgui
