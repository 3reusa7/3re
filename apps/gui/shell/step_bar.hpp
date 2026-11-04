#pragma once
// Numbered workflow steps (Import → Segment → Generate → Preview). Only the current
// step and completed steps can be clicked; later steps are visibly not reachable yet.
#include <QList>
#include <QStringList>
#include <QWidget>

class QPushButton;

namespace p3dgui {

class StepBar : public QWidget {
  Q_OBJECT
 public:
  explicit StepBar(const QStringList& steps, QWidget* parent = nullptr);
  void setCurrent(int index);
  int current() const { return current_; }
  void setCompleted(int index, bool done);
  bool completed(int index) const { return done_.value(index); }

 signals:
  void stepClicked(int index);

 private:
  void refresh();
  QStringList steps_;
  QList<QPushButton*> buttons_;
  QList<bool> done_;
  int current_ = 0;
};

}  // namespace p3dgui
