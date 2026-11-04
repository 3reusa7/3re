#pragma once
// Pixel3D main window: a start screen until there is a model, then the
// workspace — toolbar of primary actions and tools, Views panel with the
// editable drawing preview (left), the dominant 3D viewport (centre) with
// floating controls and tool options, Inspector (right), Checks & History
// (bottom), a compact status bar and a hidden Log. Every engine action goes
// through AppController as one Session command; the window owns selection,
// highlighting and navigation, never engine state.
#include <QFileSystemWatcher>
#include <QMainWindow>
#include <QTimer>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>

#include "app_controller.hpp"
#include "editor_state.hpp"
#include "issues.hpp"
#include "view_state.hpp"

class QAction;
class QActionGroup;
class QCheckBox;
class QComboBox;
class QDockWidget;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QToolBar;
class QToolButton;

namespace p3dgui {

class Viewport;
class ViewsPanel;
class Inspector;
class LogPanel;
class HistoryPanel;
class ExportDialog;
class StartScreen;

struct WindowOptions {
  QString autosave_dir;  // empty = no autosave
  bool software_view = false;
};

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(const WindowOptions& opt, QWidget* parent = nullptr);
  ~MainWindow() override;

  AppController& controller() { return *controller_; }
  EditorState& editor() { return *editor_; }
  Viewport& viewport() { return *viewport_; }
  Inspector& inspector() { return *inspector_; }
  ViewsPanel& viewsPanel() { return *views_; }
  StartScreen& startScreen() { return *start_; }
  LogPanel& logPanel() { return *log_; }
  HistoryPanel& historyPanel() { return *history_; }
  QToolBar* mainToolBar() const { return toolbar_; }
  QDockWidget* dock(const QString& name) const;  // views, inspector, history, log
  QAction* action(const QString& key) const { return actions_.at(key); }
  bool hasAction(const QString& key) const { return actions_.contains(key); }
  bool startScreenVisible() const;
  QString hintText() const;
  QString bannerText() const;
  Tool tool() const { return editor_->tool(); }
  const std::set<int>& selectedParts() const { return sel_parts_; }
  int selectedTriangle() const { return sel_tri_; }

  // Used by dialogs/actions and by tests (which replace the confirmation prompt).
  void importFiles(const QStringList& files, std::optional<pixel3d::ViewRole> role = std::nullopt);
  void exportTo(const QString& path);  // format from the extension, generic preset
  void exportModel(const pixel3d::ModelExportRequest& r);
  // The Export dialog as File > Export Model opens it (suggested file, last preset).
  std::unique_ptr<ExportDialog> newExportDialog();
  void openProjectFile(const QString& path);
  // Drawings opened from File > Open: a new Low-Poly Studio
  // project with them as its views (one drawing = Front only).
  void openDrawingsAsLowpoly(const QStringList& files);
  // Layered drawings: the layers as suggested parts; the user picks the ones to keep.
  void suggestLayerParts();
  // Picks the layers to keep from a suggest_layer_parts proposal (null = cancelled), replaceable by tests.
  using ChooseLayers = std::function<std::optional<QStringList>(const nlohmann::json& layers)>;
  void setLayerChooser(ChooseLayers c) { choose_layers_ = std::move(c); }
  void saveProjectFile(const QString& path);
  void openIssue(const QString& key);
  bool openFirstIssue();
  void selectPart(int part, bool frame);
  void selectFace(int triangle);
  void clearSelection();
  void setTool(Tool t);
  void resetLayout();
  void showWorkspace();
  // A rectangle of a drawing (Volume tool) turned into an edit:
  // "add", "erase", "erase_through", "extrude", "push", "mask", "new_part".
  void regionAction(pixel3d::ViewRole role, QRect pixels, const QString& what);
  // The part actions of the Part menu and the inspector ("part_rename", "part_mirror_copy_2", ...).
  void runAction(const QString& key);
  using Confirm = std::function<bool(const QString& title, const QString& text)>;
  void setConfirmHandler(Confirm c) { confirm_ = std::move(c); }
  // Text input (part names), replaceable by tests.
  using AskText = std::function<std::optional<QString>(const QString& title, const QString& label, const QString& value)>;
  void setTextPrompt(AskText a) { ask_text_ = std::move(a); }
  // Save-file prompt (low-poly export), replaceable by tests.
  using AskPath = std::function<QString(const QString& suggestion)>;
  void setSavePathPrompt(AskPath a) { ask_save_path_ = std::move(a); }
  // LOW-POLY STUDIO
  void suggestLowpolyParts();
  void setLowpolyPose(bool posed);
  bool lowpolyPose() const { return lowpoly_pose_; }
  // The values of the Project Settings dialog (name, scale, normalisation, style, project type).
  void applySettings(const pixel3d::ProjectSettings& s);
  QString lastError() const { return last_error_; }
  // PROJECT TYPE: Low-Poly Studio (the default) or Visual Hull; switching keeps the drawings (one undoable step).
  void switchProjectType(bool lowpoly);
  QToolButton* projectTypeChip() const { return mode_chip_; }
  // A visual-hull step asked for without Top (or Side): what the user wants instead, replaceable by tests.
  enum class MissingViewsChoice { SwitchToLowpoly, AddView, Cancel };
  using AskMissingViews = std::function<MissingViewsChoice(const QString& text, pixel3d::ViewRole missing)>;
  void setMissingViewsPrompt(AskMissingViews a) { ask_missing_views_ = std::move(a); }

 signals:
  // File > Image to 3D and the start screen's TRELLIS button: the 3re studio opens its AI workspace.
  void imageTo3DRequested();

 protected:
  void closeEvent(QCloseEvent* e) override;
  void resizeEvent(QResizeEvent* e) override;
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dropEvent(QDropEvent* e) override;

 private:
  void buildActions();
  void buildPartMenu(QMenu* menu);
  void buildToolbar();
  void buildDocks();
  void buildStatusBar();
  void buildViewportOverlays();
  void connectEditing();
  void onViewChanged();
  void onJobStarted(const QString& name);
  void onJobFinished(const QString& name, const pixel3d::OpResult& r);
  void updateActions();
  void updateTitle();
  void updateGuidance();
  void updateHighlight(bool marks = true);  // marks = also the drawings (not needed for hover)
  void updateCameraButtons();
  void updateToolOptions();
  void updateBanner();
  void setStartScreen(bool on);
  bool maybeSave();
  void addRecent(const QString& path);
  void rebuildRecentMenu();
  QStringList recentFiles() const;
  void browseRole(pixel3d::ViewRole r);
  void openSettings();
  void newSingleView(const QString& path = {});
  void frameSelection();
  void onTriangleClicked(int tri, Qt::KeyboardModifiers mods);
  void onTriangleHovered(int tri);
  QStringList reviewLines(const nlohmann::json& why) const;
  void saveLayout();
  // Editing input from the drawing preview and the viewport.
  void onStroke(pixel3d::ViewRole role, const QVector<QPointF>& points, bool finished);
  void onPixelClicked(pixel3d::ViewRole role, int u, int v);
  void onRegion(pixel3d::ViewRole role, QRect pixels);
  void onPushPull(int face, double distance, bool finished);
  void onSelectionResult(const pixel3d::OpResult& r);
  void syncSessionSelection();
  void cancelPreview();
  void newPart(bool define);
  int selectedPart() const;  // the first selected part, or -1
  std::optional<QString> askText(const QString& title, const QString& label, const QString& value);
  // Aseprite workflow.
  void watchViews();
  void viewsChangedOnDisk(const QString& path);
  // True (and the user is offered the way out) when a visual-hull step cannot run: Front, Side or Top is missing.
  bool visualHullBlocked();

  std::unique_ptr<AppController> controller_;
  EditorState* editor_ = nullptr;
  QStackedWidget* center_ = nullptr;
  StartScreen* start_ = nullptr;
  Viewport* viewport_ = nullptr;
  ViewsPanel* views_ = nullptr;
  Inspector* inspector_ = nullptr;
  HistoryPanel* history_ = nullptr;
  LogPanel* log_ = nullptr;
  std::map<QString, QDockWidget*> docks_;
  QToolBar* toolbar_ = nullptr;
  std::map<QString, QAction*> actions_;
  QActionGroup* tools_ = nullptr;
  QActionGroup* cameras_ = nullptr;
  QMenu* recent_menu_ = nullptr;
  QMenu* part_menu_ = nullptr;
  QMenu* mirror_menu_ = nullptr;
  // Viewport overlays.
  QFrame* hint_ = nullptr;
  QLabel* hint_icon_ = nullptr;
  QLabel* hint_text_ = nullptr;
  QPushButton* hint_button_ = nullptr;
  QString hint_action_;
  QFrame* tool_options_ = nullptr;
  QLabel* tool_hint_ = nullptr;
  QPushButton* mode_object_ = nullptr;
  QPushButton* mode_parts_ = nullptr;
  QPushButton* mode_faces_ = nullptr;
  QWidget* depth_opts_ = nullptr;
  QPushButton* depth_raise_ = nullptr;
  QPushButton* depth_lower_ = nullptr;
  QDoubleSpinBox* depth_size_ = nullptr;
  QDoubleSpinBox* depth_strength_ = nullptr;
  QCheckBox* depth_selected_ = nullptr;
  QWidget* push_opts_ = nullptr;
  QCheckBox* push_snap_ = nullptr;
  QCheckBox* push_bounds_ = nullptr;
  QWidget* mask_opts_ = nullptr;
  QComboBox* mask_part_ = nullptr;
  QDoubleSpinBox* mask_size_ = nullptr;
  QCheckBox* mask_erase_ = nullptr;
  QFrame* banner_ = nullptr;
  QLabel* banner_text_ = nullptr;
  bool updating_options_ = false;
  // Status bar.
  QProgressBar* progress_ = nullptr;
  QPushButton* cancel_ = nullptr;
  QToolButton* log_button_ = nullptr;
  QToolButton* status_pill_ = nullptr;
  QToolButton* reload_button_ = nullptr;
  int unseen_warnings_ = 0;
  // State.
  std::vector<Issue> issues_;
  bool face_mode_ = false;
  std::set<int> sel_parts_;
  int sel_tri_ = -1;
  QString open_issue_;
  int hover_tri_ = -1;
  std::vector<int> hover_candidates_;
  bool start_visible_ = false;
  QByteArray workspace_state_;
  std::set<QString> docks_before_start_;
  bool toolbar_compact_ = false;
  bool workspace_seen_ = false;
  uint64_t shown_revision_ = ~0ull;
  QString pending_export_;
  pixel3d::ModelExportRequest pending_request_;
  QString last_error_;
  Confirm confirm_;
  AskText ask_text_;
  AskPath ask_save_path_;
  ChooseLayers choose_layers_;
  AskMissingViews ask_missing_views_;
  QToolButton* mode_chip_ = nullptr;  // the project type in the Views panel header
  bool lowpoly_pose_ = false;
  bool select_from_drawing_ = false;  // a drawing click asked the engine; mirror its answer
  std::shared_ptr<const pixel3d::TexturedMesh> preview_;
  QString preview_text_;
  QFileSystemWatcher watcher_;
  QTimer reload_timer_;
};

}  // namespace p3dgui
