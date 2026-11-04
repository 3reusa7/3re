#pragma once
// Bottom panel: live checks of the current model (recomputed by the engine
// after every edit, without a full validation) beside the undo history (every
// committed step; double-click returns to it).
#include <QWidget>

#include "pixel3d/engine/job_runner.hpp"

class QLabel;
class QListWidget;

namespace p3dgui {

class HistoryPanel : public QWidget {
  Q_OBJECT
 public:
  explicit HistoryPanel(QWidget* parent = nullptr);
  void update_from(const pixel3d::SessionView& v);
  QListWidget* list() const { return list_; }
  QString liveText() const;

 signals:
  void jumpRequested(size_t index);

 private:
  QLabel* live_ = nullptr;
  QListWidget* list_ = nullptr;
};

}  // namespace p3dgui
