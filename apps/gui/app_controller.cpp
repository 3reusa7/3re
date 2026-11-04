#include "app_controller.hpp"

#include <QMetaObject>

#include "pixel3d/common/sha256.hpp"
#include "pixel3d/io/json_io.hpp"
#include "qt_util.hpp"

namespace p3dgui {

using namespace pixel3d;

namespace {

constexpr int kAutosaveDelayMs = 20000;

// Messages of the earlier steps of a chained job are kept on the final result.
void merge_notes(OpResult& into, const OpResult& from) {
  into.warnings.insert(into.warnings.end(), from.warnings.begin(), from.warnings.end());
  into.info.insert(into.info.end(), from.info.begin(), from.info.end());
}

}  // namespace

AppController::AppController(const QString& autosave_dir, QObject* parent)
    : QObject(parent), autosave_dir_(autosave_dir) {
  qRegisterMetaType<pixel3d::OpResult>("pixel3d::OpResult");
  runner_ = std::make_unique<JobRunner>(Source::Gui, [this](const std::string& name, float f, std::string_view stage) {
    const QString n = qs(name), st = qs(stage);
    QMetaObject::invokeMethod(this, [this, n, f, st] { emit progress(n, f, st); }, Qt::QueuedConnection);
  });
  view_ = runner_->view();
  saved_key_ = inputsKey(view_.doc);
  autosaved_key_ = saved_key_;
  if (!autosave_dir_.isEmpty()) {
    auto slot = AutosaveSlot::acquire(to_path(autosave_dir_));
    if (slot) autosave_ = std::move(*slot);
  }
  autosave_timer_.setSingleShot(true);
  connect(&autosave_timer_, &QTimer::timeout, this, &AppController::autosaveNow);
}

AppController::~AppController() { runner_.reset(); }

std::string AppController::inputsKey(const Document& d) const {
  return sha256_hex(inputs_to_json(d).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

bool AppController::dirty() const { return !view_.doc.views.empty() && inputsKey(view_.doc) != saved_key_; }

void AppController::post(const QString& name, JobRunner::Op op, After after) {
  ++pending_;
  emit jobStarted(name);
  runner_->post(utf8(name), std::move(op), [this, name, after](const std::string&, const OpResult& r) {
    QMetaObject::invokeMethod(this, [this, name, r, after] { finished(name, r, after); }, Qt::QueuedConnection);
  });
}

void AppController::finished(const QString& name, const OpResult& r, const After& after) {
  view_ = runner_->view();
  --pending_;
  if (after) after(r);
  emit viewChanged();
  emit jobFinished(name, r);
  if (r.success) scheduleAutosave();
}

void AppController::newProject(const ProjectSettings& settings) {
  post("New project", [settings](Session& s, const JobContext&) { return s.new_project(settings); },
       [this](const OpResult& r) {
         if (!r.success) return;
         project_path_.clear();
         saved_key_ = inputsKey(runner_->view().doc);
         emit projectPathChanged(project_path_);
       });
}

void AppController::startProject(const ProjectSettings& settings) {
  runner_->post("new_project", [settings](Session& s, const JobContext&) { return s.new_project(settings); });
  runner_->wait_idle();
  view_ = runner_->view();
  project_path_.clear();
  saved_key_ = inputsKey(view_.doc);
  autosaved_key_ = saved_key_;
}

void AppController::openProject(const QString& path) {
  const auto p = to_path(path);
  post("Open project", [p](Session& s, const JobContext&) { return s.load(p); }, [this, path](const OpResult& r) {
    if (!r.success) return;
    project_path_ = path;
    saved_key_ = inputsKey(runner_->view().doc);
    emit projectPathChanged(project_path_);
  });
}

void AppController::saveProject(const QString& path) {
  const auto p = to_path(path);
  post("Save project", [p](Session& s, const JobContext&) { return s.save(p); }, [this, path](const OpResult& r) {
    if (!r.success) return;
    project_path_ = path;
    saved_key_ = inputsKey(runner_->view().doc);
    emit projectPathChanged(project_path_);
  });
}

void AppController::recover(const RecoverableSession& rs) {
  const auto file = rs.project_file;
  post("Recover autosave", [file](Session& s, const JobContext&) { return s.load(file); },
       [this, rs](const OpResult& r) {
         if (!r.success) return;
         project_path_ = qs(rs.original_project);
         saved_key_.clear();  // recovered work is unsaved until the user saves it
         discard(to_path(autosave_dir_), rs);
         emit projectPathChanged(project_path_);
       });
}

void AppController::updateSettings(const SettingsUpdate& u) {
  post("Update settings", [u](Session& s, const JobContext&) { return s.update_settings(u); });
}

void AppController::importViews(const std::vector<ViewImport>& views) {
  post("Import views", [views](Session& s, const JobContext&) { return s.import_views(views); });
}

void AppController::autoAlign() {
  post("Auto-align", [](Session& s, const JobContext&) { return s.auto_align(); });
}

void AppController::reconstruct(ReconstructMode mode) {
  post("Reconstruct", [mode](Session& s, const JobContext& j) { return s.reconstruct(ReconstructOptions{mode}, j); });
}

void AppController::projectTextures() {
  post("Project textures",
       [](Session& s, const JobContext&) { return s.project_textures(s.document().texture_options); });
}

void AppController::validate() {
  post("Validate", [](Session& s, const JobContext& j) { return s.validate(ValidateOptions{}, j); });
}

void AppController::cleanup() {
  post("Clean up", [](Session& s, const JobContext&) { return s.cleanup(s.document().cleanup_options); });
}

void AppController::runAll(ReconstructMode mode) {
  post("Reconstruct and validate", [mode](Session& s, const JobContext& j) {
    OpResult notes;
    const std::function<OpResult()> steps[] = {
        [&] { return s.auto_align(); },
        [&] { return s.reconstruct(ReconstructOptions{mode}, j); },
        [&] { return s.project_textures(s.document().texture_options); },
        [&] { return s.validate(ValidateOptions{}, j); },
    };
    OpResult r;
    for (const auto& step : steps) {
      r = step();
      merge_notes(notes, r);
      if (!r.success) break;
    }
    r.warnings = std::move(notes.warnings);  // every step's warnings and info, in order
    r.info = std::move(notes.info);
    return r;
  });
}

void AppController::editParts(const std::vector<UserDecision>& decisions) {
  post("Edit parts", [decisions](Session& s, const JobContext&) { return s.edit_part(PartEdit{decisions}); });
}

void AppController::setDepth(const DepthConstraint& c) {
  post("Set depth", [c](Session& s, const JobContext&) { return s.set_depth(c); });
}

void AppController::undo() {
  post("Undo", [](Session& s, const JobContext&) { return s.undo(); });
}

void AppController::redo() {
  post("Redo", [](Session& s, const JobContext&) { return s.redo(); });
}

void AppController::createCheckpoint(const QString& name) {
  const std::string n = utf8(name);
  post("Create checkpoint", [n](Session& s, const JobContext&) { return s.create_checkpoint(n); });
}

void AppController::restoreCheckpoint(const QString& name) {
  const std::string n = utf8(name);
  post("Restore checkpoint", [n](Session& s, const JobContext&) { return s.restore_checkpoint(n); });
}

void AppController::exportModel(const ModelExportRequest& r) {
  post("Export model", [r](Session& s, const JobContext&) { return s.export_model(r); });
}

void AppController::writeArtifacts(const QString& dir, bool turntable) {
  ArtifactRequest req;
  req.dir = to_path(dir);
  req.turntable = turntable;
  post("Write renders", [req](Session& s, const JobContext& j) { return s.write_artifacts(req, j); });
}

void AppController::requestCompare(ViewRole role) {
  // Read-only and quick: not counted as a user job (actions stay enabled).
  auto images = std::make_shared<std::vector<NamedImage>>();
  runner_->post(
      "compare",
      [role, images](Session& s, const JobContext&) {
        RenderRequest rr;
        rr.views = {role};
        rr.pixels_per_unit = 1;
        rr.compare = true;
        return s.render_views(rr, images.get());
      },
      [this, role, images](const std::string&, const OpResult& r) {
        QImage img;
        QString counts;
        for (size_t i = 0; r.success && i < images->size(); ++i)
          if ((*images)[i].name.find("_compare") != std::string::npos) img = to_qimage((*images)[i].image);
        if (r.success)
          for (const auto& im : r.data["images"])
            if (im.contains("pixels")) counts = json_text(im["pixels"], -1);
        QMetaObject::invokeMethod(
            this, [this, role, img, counts] { emit compareReady(static_cast<int>(role), img, counts); },
            Qt::QueuedConnection);
      });
}

// ---- M4 editing ------------------------------------------------------------------

void AppController::paintDepth(const DepthPaintRequest& r) {
  post("Depth brush", [r](Session& s, const JobContext&) { return s.paint_depth(r); });
}

void AppController::paintPartMask(const MaskPaintRequest& r) {
  post(r.erase ? "Erase part mask" : "Paint part mask", [r](Session& s, const JobContext&) { return s.paint_part_mask(r); });
}

void AppController::pushPull(const PushPullRequest& r) {
  post("Push/pull", [r](Session& s, const JobContext&) { return s.push_pull(r); });
}

void AppController::addVolume(ViewRole view, const Polygons2D& region, double from, double to, int part) {
  post("Add volume", [=](Session& s, const JobContext&) { return s.add_volume(view, region, from, to, part); });
}

void AppController::eraseVolume(ViewRole view, const Polygons2D& region, double from, double to, int part) {
  post("Erase volume", [=](Session& s, const JobContext&) { return s.erase_volume(view, region, from, to, part); });
}

void AppController::extrude(ViewRole view, const Polygons2D& region, double distance, int part) {
  post("Extrude", [=](Session& s, const JobContext&) { return s.extrude(view, region, distance, part); });
}

void AppController::cut(int part, int axis, double at, int keep) {
  post("Cut", [=](Session& s, const JobContext&) { return s.cut(part, axis, at, keep); });
}

void AppController::splitPart(int part, int axis, double at) {
  post("Split part", [=](Session& s, const JobContext&) { return s.split_part(part, axis, at); });
}

void AppController::splitPartShells(int part) {
  post("Split into pieces", [=](Session& s, const JobContext&) { return s.split_part_shells(part); });
}

void AppController::mergeParts(const std::vector<int>& parts) {
  post("Merge parts", [parts](Session& s, const JobContext&) { return s.merge_parts(parts); });
}

void AppController::mirror(int part, int axis, std::optional<double> at, bool copy) {
  post(copy ? "Mirror copy" : "Mirror", [=](Session& s, const JobContext&) { return s.mirror(part, axis, at, copy); });
}

void AppController::transformPart(const TransformRequest& r) {
  post(r.copy ? "Duplicate part" : "Transform part", [r](Session& s, const JobContext&) { return s.transform_part(r); });
}

void AppController::deletePart(int part) {
  post("Delete part", [part](Session& s, const JobContext&) { return s.delete_part(part); });
}

void AppController::createPart(const QString& name, bool define, std::function<void(int)> then) {
  const std::string n = utf8(name);
  post("New part", [n, define](Session& s, const JobContext&) { return s.create_part(n, define); },
       [then](const OpResult& r) {
         if (r.success && then && r.data.contains("part")) then(r.data["part"].get<int>());
       });
}

void AppController::renamePart(int part, const QString& name) {
  const std::string n = utf8(name);
  post("Rename part", [part, n](Session& s, const JobContext&) { return s.rename_part(part, n); });
}

void AppController::syncPartMasks(int part) {
  post("Sync part masks", [part](Session& s, const JobContext&) { return s.sync_part_masks(part); });
}

void AppController::postQuiet(const QString& name, JobRunner::Op op) {
  runner_->post(utf8(name), std::move(op), [this](const std::string&, const OpResult& r) {
    QMetaObject::invokeMethod(
        this,
        [this, r] {
          view_ = runner_->view();
          emit viewChanged();
          emit selectionResult(r);
        },
        Qt::QueuedConnection);
  });
}

void AppController::selectAtPixel(ViewRole view, int u, int v, SelectMode mode) {
  postQuiet("select", [=](Session& s, const JobContext&) { return s.select_at_pixel(view, u, v, mode); });
}

void AppController::selectAtRay(vec3d origin, vec3d dir, SelectMode mode) {
  postQuiet("select", [=](Session& s, const JobContext&) { return s.select_at_ray(origin, dir, mode); });
}

void AppController::selectParts(const std::vector<int>& parts, SelectMode mode) {
  postQuiet("select", [=](Session& s, const JobContext&) { return s.select_parts(parts, mode); });
}

void AppController::clearSelection() {
  postQuiet("select", [](Session& s, const JobContext&) { return s.clear_selection(); });
}

void AppController::newSingleViewProject(const ProjectSettings& settings, const QString& path, ViewRole role,
                                         const SingleViewSettings& sv) {
  const auto p = to_path(path);
  post(
      "New single-view project",
      [settings, p, role, sv](Session& s, const JobContext&) {
        // The same three core commands the MCP tool new_single_view_project runs.
        OpResult r = s.new_project(settings);
        if (r.success) r = s.import_views({ViewImport{role, p, std::nullopt}});
        if (r.success) r = s.set_single_view(sv);
        return r;
      },
      [this](const OpResult& r) {
        if (!r.success) return;
        project_path_.clear();
        saved_key_.clear();  // a new project with a drawing is unsaved work
        emit projectPathChanged(project_path_);
      });
}

void AppController::setSingleViewDepth(const std::string& preset, std::optional<double> depth) {
  post("Single view depth", [=](Session& s, const JobContext&) { return s.set_single_view_depth(preset, depth); });
}

void AppController::setLowpolySettings(const LowpolyUpdate& u) {
  post("Low-poly settings", [u](Session& s, const JobContext&) { return s.set_lowpoly_settings(u); });
}

void AppController::setPartType(const PartTypeUpdate& u) {
  post("Part type", [u](Session& s, const JobContext&) { return s.set_part_type(u); });
}

void AppController::suggestLowpolyParts(const LowpolySuggestRequest& r, std::function<void(const OpResult&)> then) {
  post(r.apply ? "Apply suggested parts" : "Suggest parts",
       [r](Session& s, const JobContext&) { return s.lowpoly_suggest_parts(r); },
       [then](const OpResult& res) {
         if (then) then(res);
       });
}

void AppController::suggestLayerParts(const LayerPartsRequest& r, std::function<void(const OpResult&)> then) {
  post(r.apply ? "Apply layer parts" : "Layer parts",
       [r](Session& s, const JobContext&) { return s.suggest_layer_parts(r); },
       [then](const OpResult& res) {
         if (then) then(res);
       });
}

void AppController::buildLowpoly(const LowpolyUpdate& u) {
  post("Build low-poly", [u](Session& s, const JobContext& j) { return s.lowpoly_build(u, j); });
}

void AppController::exportLowpoly(const LowpolyExportRequest& r) {
  post("Export low-poly", [r](Session& s, const JobContext&) { return s.lowpoly_export(r); });
}

void AppController::reloadViews() {
  post("Reload views", [](Session& s, const JobContext&) { return s.reload_views(); });
}

void AppController::jumpToHistory(size_t index) {
  post("History", [index](Session& s, const JobContext&) {
    OpResult r = s.current_status();
    while (r.success && s.history_cursor() > index) r = s.undo();
    while (r.success && s.history_cursor() < index && s.can_redo()) r = s.redo();
    return r;
  });
}

void AppController::requestPreview(PreviewOp op) {
  preview_waiting_ = std::move(op);
  if (!preview_running_) runPreview();
}

void AppController::runPreview() {
  if (!preview_waiting_) return;
  PreviewOp op = std::move(preview_waiting_);
  preview_waiting_ = nullptr;
  preview_running_ = true;
  const uint64_t gen = preview_generation_;
  auto mesh = std::make_shared<Mesh>();
  runner_->post("preview", [op, mesh](Session& s, const JobContext&) { return op(s, mesh.get()); },
                [this, gen, mesh](const std::string&, const OpResult& r) {
                  QMetaObject::invokeMethod(
                      this,
                      [this, gen, mesh, r] {
                        preview_running_ = false;
                        if (gen == preview_generation_) {
                          QString summary = tr("PREVIEW — release to apply");
                          if (r.success) {
                            QStringList worse;
                            const nlohmann::json views = r.data.value("views", nlohmann::json::object());
                            for (const auto& [role, vm] : views.items()) {
                              const auto vr = view_role_from_string(role);
                              int before = 0;
                              if (vr && view_.doc.live && view_.doc.live->views.contains(*vr))
                                before = view_.doc.live->views.at(*vr).mismatch_px;
                              const int after = vm.value("mismatch_px", 0);
                              if (after > before) worse << tr("%1 +%2 px").arg(qs(role)).arg(after - before);
                            }
                            if (!worse.isEmpty()) summary += tr(" — no longer matches the drawing: %1").arg(worse.join(", "));
                            emit previewReady(mesh, summary);
                          } else {
                            emit previewReady(nullptr, r.errors.empty() ? QString() : qs(r.errors.front().text));
                          }
                        }
                        runPreview();
                      },
                      Qt::QueuedConnection);
                });
}

void AppController::previewDepth(const DepthPaintRequest& r) {
  requestPreview([r](const Session& s, Mesh* m) { return s.preview_paint_depth(r, m); });
}

void AppController::previewPushPull(const PushPullRequest& r) {
  requestPreview([r](const Session& s, Mesh* m) { return s.preview_push_pull(r, m); });
}

void AppController::clearPreview() {
  ++preview_generation_;
  preview_waiting_ = nullptr;
  emit previewReady(nullptr, QString());
}

void AppController::cancel() {
  runner_->cancel();
  // A dropped preview job never reports back: forget it.
  preview_running_ = false;
  clearPreview();
}

void AppController::waitIdle() { runner_->wait_idle(); }

void AppController::scheduleAutosave() {
  if (!autosave_ || view_.doc.views.empty()) return;
  if (inputsKey(view_.doc) == autosaved_key_) return;
  if (!autosave_timer_.isActive()) autosave_timer_.start(kAutosaveDelayMs);
}

void AppController::autosaveNow() {
  if (!autosave_) return;
  const auto file = autosave_->project_file();
  const std::string key = inputsKey(view_.doc);
  const uint64_t revision = view_.doc.revision;
  runner_->post("autosave", [file](Session& s, const JobContext&) { return s.save_copy(file); },
                [this, key, revision, file](const std::string&, const OpResult& r) {
                  QMetaObject::invokeMethod(
                      this,
                      [this, key, revision, file, ok = r.success] {
                        if (!ok || !autosave_) return;
                        autosave_->write_info(utf8(project_path_), revision);
                        autosaved_key_ = key;
                        emit autosaved(from_path(file));
                      },
                      Qt::QueuedConnection);
                });
}

void AppController::cleanShutdown() {
  runner_->cancel();
  runner_->wait_idle();
  if (autosave_) autosave_->clean_exit();
  autosave_.reset();
}

}  // namespace p3dgui
