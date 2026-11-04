#pragma once
// Empty state: shown instead of the workspace until a model exists (or a
// project is opened). Three large drop slots for Front, Side and Top, a step
// row that ticks off as they fill, the project type (with a switch), and the
// few actions a new user needs.
#include <QFrame>
#include <QImage>
#include <QStringList>
#include <array>
#include <optional>

#include "pixel3d/engine/job_runner.hpp"
#include "views_panel.hpp"

class QLabel;
class QPushButton;
class QHBoxLayout;

namespace p3dgui {

class StartSlot : public QFrame {
  Q_OBJECT
 public:
  explicit StartSlot(pixel3d::ViewRole role, QWidget* parent = nullptr);
  void setInfo(const DrawingInfo& info);
  void setCurrent(bool on);  // the next slot to fill
  QSize sizeHint() const override { return {210, 220}; }
  QSize minimumSizeHint() const override { return {150, 180}; }

 signals:
  void browse(pixel3d::ViewRole role);
  void filesDropped(const QStringList& files, pixel3d::ViewRole role);

 protected:
  void paintEvent(QPaintEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dragLeaveEvent(QDragLeaveEvent* e) override;
  void dropEvent(QDropEvent* e) override;
  void enterEvent(QEnterEvent* e) override;
  void leaveEvent(QEvent* e) override;

 private:
  DrawingInfo info_;
  bool current_ = false, drag_over_ = false, hover_ = false;
};

class StartScreen : public QWidget {
  Q_OBJECT
 public:
  explicit StartScreen(QWidget* parent = nullptr);
  void update_from(const pixel3d::SessionView& v, bool busy);
  void setRecent(const QStringList& paths);
  QPushButton* reconstructButton() const { return reconstruct_; }
  QLabel* projectTypeLabel() const { return mode_; }
  QPushButton* projectTypeSwitch() const { return mode_switch_; }
  MissingViewsNotice* missingNotice() const { return missing_; }

 signals:
  void filesDropped(const QStringList& files, std::optional<pixel3d::ViewRole> role);
  void browseRequested(pixel3d::ViewRole role);
  void importRequested();
  void image3dRequested();
  void openRequested();
  void reconstructRequested();
  void workspaceRequested();
  void singleViewRequested();
  void recentRequested(const QString& path);
  void projectTypeRequested(bool lowpoly);  // switch this project's type, keeping its drawings

 protected:
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dropEvent(QDropEvent* e) override;

 private:
  std::array<StartSlot*, 3> slots_{};
  std::array<QLabel*, 4> steps_{};
  QPushButton* reconstruct_ = nullptr;
  QPushButton* workspace_ = nullptr;
  QLabel* recent_ = nullptr;
  QLabel* lead_ = nullptr;
  QLabel* mode_ = nullptr;              // the project type, always shown
  QPushButton* mode_switch_ = nullptr;  // "Use Visual Hull instead" / "Switch to Low-Poly Studio"
  MissingViewsNotice* missing_ = nullptr;  // Visual Hull without its Top (or Side): the way out
  bool lowpoly_ = true;
};

}  // namespace p3dgui
