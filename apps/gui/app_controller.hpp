#pragma once
// The GUI's only path to the engine. Every action posts one Session call to the
// JobRunner (worker thread); results, progress and the new SessionView come
// back on the UI thread as signals. Also owns project path / dirty state and
// autosave. No modelling logic here: actions are thin wrappers.
#include <QImage>
#include <QObject>
#include <QString>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>

#include "pixel3d/engine/autosave.hpp"
#include "pixel3d/engine/job_runner.hpp"

namespace p3dgui {

class AppController : public QObject {
  Q_OBJECT
 public:
  // `autosave_dir` empty = no autosave.
  explicit AppController(const QString& autosave_dir, QObject* parent = nullptr);
  ~AppController() override;

  const pixel3d::SessionView& view() const { return view_; }
  bool busy() const { return pending_ > 0; }
  QString projectPath() const { return project_path_; }
  bool dirty() const;
  QString autosaveDir() const { return autosave_dir_; }

  // Project
  void newProject(const pixel3d::ProjectSettings& settings);
  // The same, finished before it returns and without job signals: the window's first project.
  void startProject(const pixel3d::ProjectSettings& settings);
  void openProject(const QString& path);
  void saveProject(const QString& path);
  void recover(const pixel3d::RecoverableSession& s);
  void updateSettings(const pixel3d::SettingsUpdate& u);
  // Pipeline
  void importViews(const std::vector<pixel3d::ViewImport>& views);
  void autoAlign();
  void reconstruct(pixel3d::ReconstructMode mode);
  void projectTextures();
  void validate();
  void cleanup();
  void runAll(pixel3d::ReconstructMode mode);  // align → reconstruct → textures → validate
  // Decisions
  void editParts(const std::vector<pixel3d::UserDecision>& decisions);
  void setDepth(const pixel3d::DepthConstraint& c);
  // History
  void undo();
  void redo();
  void createCheckpoint(const QString& name);
  void restoreCheckpoint(const QString& name);
  // Output
  void exportModel(const pixel3d::ModelExportRequest& r);
  void writeArtifacts(const QString& dir, bool turntable);
  // Inspection renders (read-only)
  void requestCompare(pixel3d::ViewRole role);

  // ---- M4 editing: one Session command each (the same commands MCP calls) ----
  void paintDepth(const pixel3d::DepthPaintRequest& r);
  void paintPartMask(const pixel3d::MaskPaintRequest& r);
  void pushPull(const pixel3d::PushPullRequest& r);
  void addVolume(pixel3d::ViewRole view, const pixel3d::Polygons2D& region, double from, double to, int part);
  void eraseVolume(pixel3d::ViewRole view, const pixel3d::Polygons2D& region, double from, double to, int part);
  void extrude(pixel3d::ViewRole view, const pixel3d::Polygons2D& region, double distance, int part);
  void cut(int part, int axis, double at, int keep);
  void splitPart(int part, int axis, double at);
  void splitPartShells(int part);
  void mergeParts(const std::vector<int>& parts);
  void mirror(int part, int axis, std::optional<double> at, bool copy);
  void transformPart(const pixel3d::TransformRequest& r);
  void deletePart(int part);
  // `then` receives the new part id on success.
  void createPart(const QString& name, bool define, std::function<void(int)> then = {});
  void renamePart(int part, const QString& name);
  void syncPartMasks(int part);
  // Selection (quick, not undoable: does not disable the window's actions)
  void selectAtPixel(pixel3d::ViewRole view, int u, int v, pixel3d::SelectMode mode);
  void selectAtRay(pixel3d::vec3d origin, pixel3d::vec3d dir, pixel3d::SelectMode mode);
  void selectParts(const std::vector<int>& parts, pixel3d::SelectMode mode);
  void clearSelection();
  // SINGLE VIEW and the Aseprite workflow
  void newSingleViewProject(const pixel3d::ProjectSettings& settings, const QString& path, pixel3d::ViewRole role,
                            const pixel3d::SingleViewSettings& sv);
  void setSingleViewDepth(const std::string& preset, std::optional<double> depth);
  void reloadViews();
  // LOW-POLY STUDIO (the same Session commands as MCP and the CLI)
  void setLowpolySettings(const pixel3d::LowpolyUpdate& u);
  void setPartType(const pixel3d::PartTypeUpdate& u);
  // `then` receives the result (proposals when apply is false).
  void suggestLowpolyParts(const pixel3d::LowpolySuggestRequest& r, std::function<void(const pixel3d::OpResult&)> then = {});
  // Layered drawings (.aseprite, .psd): layers as suggested parts (proposal, or apply the accepted ones).
  void suggestLayerParts(const pixel3d::LayerPartsRequest& r, std::function<void(const pixel3d::OpResult&)> then = {});
  void buildLowpoly(const pixel3d::LowpolyUpdate& u);
  void exportLowpoly(const pixel3d::LowpolyExportRequest& r);
  // Undo or redo until history step `index` is current.
  void jumpToHistory(size_t index);
  // Live previews: only the newest request is computed; previewReady delivers it.
  void previewDepth(const pixel3d::DepthPaintRequest& r);
  void previewPushPull(const pixel3d::PushPullRequest& r);
  void clearPreview();

  void cancel();
  void autosaveNow();    // writes the autosave now (normally throttled after edits)
  void waitIdle();       // blocks; tests and shutdown only
  void cleanShutdown();  // removes this instance's autosave

 signals:
  void jobStarted(const QString& name);
  void jobFinished(const QString& name, const pixel3d::OpResult& result);
  void progress(const QString& name, float fraction, const QString& stage);
  void viewChanged();
  void projectPathChanged(const QString& path);
  void compareReady(int role, const QImage& compare, const QString& counts_json);
  void autosaved(const QString& file);
  // A preview mesh (null: preview cleared or failed) and a one-line summary.
  void previewReady(std::shared_ptr<const pixel3d::Mesh> mesh, const QString& summary);
  void selectionResult(const pixel3d::OpResult& result);

 private:
  using After = std::function<void(const pixel3d::OpResult&)>;
  void post(const QString& name, pixel3d::JobRunner::Op op, After after = {});
  void postQuiet(const QString& name, pixel3d::JobRunner::Op op);
  using PreviewOp = std::function<pixel3d::OpResult(const pixel3d::Session&, pixel3d::Mesh*)>;
  void requestPreview(PreviewOp op);
  void runPreview();
  void finished(const QString& name, const pixel3d::OpResult& r, const After& after);
  void scheduleAutosave();
  std::string inputsKey(const pixel3d::Document& d) const;

  QString autosave_dir_;
  std::unique_ptr<pixel3d::AutosaveSlot> autosave_;
  QTimer autosave_timer_;
  std::string autosaved_key_;
  pixel3d::SessionView view_;
  QString project_path_;
  std::string saved_key_;
  int pending_ = 0;
  // Live preview: at most one in flight; a newer request replaces the waiting one.
  PreviewOp preview_waiting_;
  bool preview_running_ = false;
  uint64_t preview_generation_ = 0;
  std::unique_ptr<pixel3d::JobRunner> runner_;  // last: destroyed first (joins the worker)
};

}  // namespace p3dgui

Q_DECLARE_METATYPE(pixel3d::OpResult)
Q_DECLARE_METATYPE(std::shared_ptr<const pixel3d::Mesh>)
