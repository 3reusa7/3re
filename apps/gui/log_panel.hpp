#pragma once
// Operation log (hidden by default): the messages of the last operation and
// the session history, monospace, copyable. Normal use never needs it.
#include <QWidget>

#include "pixel3d/engine/job_runner.hpp"

class QPlainTextEdit;

namespace p3dgui {

class LogPanel : public QWidget {
  Q_OBJECT
 public:
  explicit LogPanel(QWidget* parent = nullptr);
  void update_from(const pixel3d::SessionView& v);
  void showResult(const QString& op, const pixel3d::OpResult& r);
  QString lastText() const;

 private:
  QPlainTextEdit* last_ = nullptr;
  QPlainTextEdit* log_ = nullptr;
};

}  // namespace p3dgui
