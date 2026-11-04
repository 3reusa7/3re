#pragma once
// 3re studio window (spec 2026-10-20-3re-design.md §5): a navigation rail with the
// workspaces (Agent first since milestone 4), a header (workspace title, project, busy state) and the page
// stack. The Pixel Art Builder is the existing MainWindow, embedded unchanged.
#include <QMainWindow>
#include <QMap>

#include "main_window.hpp"

class QLabel;
class QPushButton;
class QStackedWidget;

namespace p3dgui {

class NavRail;
class Spinner;
class HomePage;
class SettingsPage;
class RiggingPage;
class ExportPage;
class RetopoWorkspace;
class Image3DWorkspace;
class AgentPage;

class StudioWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit StudioWindow(const WindowOptions& opt, QWidget* parent = nullptr);
  ~StudioWindow() override;

  MainWindow& pixelBuilder() { return *pixel_; }
  Image3DWorkspace& imageWorkspace() { return *ai_; }
  RetopoWorkspace& retopoWorkspace() { return *retopo_; }
  AgentPage& agentPage() { return *agent_; }
  QStringList pageIds() const { return ids_; }
  QWidget* page(const QString& id) const { return pages_.value(id); }
  void showPage(const QString& id);
  QString currentPage() const { return current_; }
  // Image to 3D: "ai" (TRELLIS.2 from one image) or "pixel" (the Pixel Art Builder).
  void setImageMode(const QString& mode);
  QString imageMode() const { return image_mode_; }
  void setBusy(bool busy, const QString& text = {});

 protected:
  void closeEvent(QCloseEvent* e) override;

 private:
  void addPage(const QString& id, const QString& label, const QIcon& icon, const QString& title, const QString& subtitle, QWidget* w);
  void updateHeader();
  QWidget* buildImagePage();

  NavRail* rail_ = nullptr;
  QStackedWidget* stack_ = nullptr;
  QLabel *title_ = nullptr, *subtitle_ = nullptr, *busy_text_ = nullptr;
  Spinner* spinner_ = nullptr;
  QStackedWidget* image_stack_ = nullptr;
  QPushButton *mode_ai_ = nullptr, *mode_pixel_ = nullptr;
  MainWindow* pixel_ = nullptr;
  Image3DWorkspace* ai_ = nullptr;
  HomePage* home_ = nullptr;
  SettingsPage* settings_ = nullptr;
  RiggingPage* rigging_ = nullptr;
  ExportPage* export_ = nullptr;
  RetopoWorkspace* retopo_ = nullptr;
  AgentPage* agent_ = nullptr;
  QStringList ids_;
  QMap<QString, QWidget*> pages_;
  QMap<QString, QPair<QString, QString>> headings_;
  QString current_, image_mode_ = "ai";
};

}  // namespace p3dgui
