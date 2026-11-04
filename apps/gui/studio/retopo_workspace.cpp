#include "studio/retopo_workspace.hpp"

#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>
#include <fstream>
#include <utility>

#include "backends/mesh_asset.hpp"
#include "backends/retopo_engine.hpp"
#include "backends/trellis_backend.hpp"
#include "icons.hpp"
#include "pixel3d/common/path_utf8.hpp"
#include "shell/feature_badge.hpp"
#include "studio/image3d_workspace.hpp"
#include "studio/retopo_mesh_editor.hpp"
#include "widgets.hpp"

namespace p3dgui {
namespace {
std::filesystem::path fs_path(const QString& s) { return std::filesystem::path(s.toStdWString()); }
QString percent(double v) { return QString::number(v * 100.0, 'f', v < 0.001 ? 3 : 2) + "%"; }
// QuadWild's two programs and its config folder.
bool engine_complete(const QString& dir) {
  const QDir d(dir);
  return !dir.isEmpty() && QFileInfo(d.filePath("quadwild.exe")).isFile() && QFileInfo(d.filePath("quad_from_patches.exe")).isFile() &&
         QFileInfo(d.filePath("config/main_config/flow.txt")).isFile();
}
}  // namespace

RetopoWorkspace::RetopoWorkspace(bool software, QWidget* parent) : QWidget(parent) {
  setObjectName("RetopoWorkspace");
  auto* row = new QHBoxLayout(this);
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(0);

  auto* scroll = new QScrollArea(this);
  scroll->setObjectName("retopoOptionsScroll");
  scroll->setWidgetResizable(true);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setFixedWidth(380);
  auto* side = new QWidget(scroll);
  side->setProperty("surface", "app");
  side->setAttribute(Qt::WA_StyledBackground);
  auto* col = new QVBoxLayout(side);
  col->setContentsMargins(theme::space::xl, theme::space::lg, theme::space::lg, theme::space::lg);
  col->setSpacing(theme::space::md);
  scroll->setWidget(side);
  row->addWidget(scroll);

  // 1 · Model
  auto* model = new Card(side);
  auto* ml = new QVBoxLayout(model);
  ml->setContentsMargins(18, 16, 18, 16);
  ml->setSpacing(8);
  ml->addWidget(make_label(tr("1 · Model"), "section", model));
  source_name_ = make_label(tr("No model yet"), "title", model);
  source_name_->setObjectName("retopoSourceName");
  source_name_->setWordWrap(true);
  QSizePolicy source_policy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  source_policy.setHeightForWidth(true);
  source_name_->setSizePolicy(source_policy);
  ml->addWidget(source_name_);
  source_info_ = make_label(tr("Use the model from Image to 3D, or open a GLB or OBJ file. The original is never changed."), "secondary", model);
  source_info_->setWordWrap(true);
  ml->addWidget(source_info_);
  // Full-width actions fit the sidebar at Windows display scaling too.
  // A horizontal pair imposed a minimum width larger than the scroll viewport.
  auto* mb = new QVBoxLayout;
  use_generated_ = make_button(tr("Use Generated Model"), nullptr, icon(Icon::ImageTo3D), model);
  use_generated_->setObjectName("retopoUseGenerated");
  open_ = make_button(tr("Open GLB / OBJ…"), "ghost", icon(Icon::Open), model);
  open_->setObjectName("retopoOpen");
  mb->addWidget(use_generated_);
  mb->addWidget(open_);
  ml->addLayout(mb);
  col->addWidget(model);

  // 2 · Retopology
  auto* settings = new Card(side);
  auto* sl = new QVBoxLayout(settings);
  sl->setContentsMargins(18, 16, 18, 16);
  sl->setSpacing(8);
  sl->addWidget(make_label(tr("2 · Retopology"), "section", settings));
  auto* target_label = make_label(tr("Target polygons (quads, approximate)"), "secondary", settings);
  target_label->setWordWrap(true);
  sl->addWidget(target_label);
  auto* fr = new QHBoxLayout;
  density_ = new QSlider(Qt::Horizontal, settings);
  density_->setRange(5, 200);  // x 100 faces
  density_->setObjectName("retopoDensity");
  density_->setToolTip(tr("Quad density: fewer, larger quads to the left; more, smaller quads to the right"));
  faces_ = new QSpinBox(settings);
  faces_->setRange(500, 20000);
  faces_->setSingleStep(500);
  faces_->setObjectName("retopoFaces");
  fr->addWidget(density_, 1);
  fr->addWidget(faces_);
  sl->addLayout(fr);
  connect(density_, &QSlider::valueChanged, this, [this](int v) { if (faces_->value() / 100 != v) faces_->setValue(v * 100); });
  connect(faces_, &QSpinBox::valueChanged, this, [this](int v) { density_->setValue(v / 100); });
  faces_->setValue(4000);
  sl->addWidget(make_label(tr("Shape"), "secondary", settings));
  mode_ = new QComboBox(settings);
  mode_->setObjectName("retopoMode");
  mode_->addItem(tr("Organic"), "organic");
  mode_->setItemData(0, tr("Characters, creatures and soft shapes"), Qt::ToolTipRole);
  mode_->addItem(tr("Hard surface"), "mechanical");
  mode_->setItemData(1, tr("Props, weapons and machines"), Qt::ToolTipRole);
  sl->addWidget(mode_);
  auto* rb = new QHBoxLayout;
  run_ = make_button(tr("Retopologize"), "primary", icon(Icon::Retopo, IconStyle::OnAccent), settings);
  run_->setObjectName("retopoRun");
  run_->setToolTip(tr("Rebuilds the model as clean quads with QuadWild (runs on the CPU, a few minutes for a generated model)"));
  cancel_ = make_button(tr("Cancel"), "ghost", {}, settings);
  cancel_->setObjectName("retopoCancel");
  cancel_->setVisible(false);
  rb->addWidget(run_);
  rb->addWidget(cancel_);
  rb->addStretch(1);
  sl->addLayout(rb);
  progress_ = new QProgressBar(settings);
  progress_->setRange(0, 100);
  progress_->setTextVisible(false);
  progress_->setFixedHeight(6);
  progress_->setVisible(false);
  sl->addWidget(progress_);
  status_ = make_label(QString(), nullptr, settings);
  status_->setObjectName("retopoStatus");
  status_->setWordWrap(true);
  sl->addWidget(status_);
  col->addWidget(settings);

  // 3 · Result
  auto* result = new Card(side);
  auto* rl = new QVBoxLayout(result);
  rl->setContentsMargins(18, 16, 18, 16);
  rl->setSpacing(8);
  auto* rh = new QVBoxLayout;
  rh->addWidget(make_label(tr("3 · Result"), "section", result));
  verdict_ = make_chip(QString(), theme::Tone::Neutral, result);
  verdict_->setObjectName("retopoVerdict");
  verdict_->setWordWrap(true);
  verdict_->setVisible(false);
  rh->addWidget(verdict_);
  rl->addLayout(rh);
  stats_ = make_label(tr("Before/after counts and the distance to the original surface appear here."), "secondary", result);
  stats_->setObjectName("retopoStats");
  stats_->setWordWrap(true);
  stats_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  rl->addWidget(stats_);
  auto* sb = new QVBoxLayout;
  save_ = make_button(tr("Save GLB…"), "primary", icon(Icon::Export, IconStyle::OnAccent), result);
  save_->setObjectName("retopoSave");
  save_->setToolTip(tr("GLB with new UVs and the model's colours (plus an OBJ with the quads). Read back and compared before success."));
  folder_ = make_button(tr("Show in Folder"), "ghost", icon(Icon::Folder), result);
  folder_->setEnabled(false);
  sb->addWidget(save_);
  sb->addWidget(folder_);
  rl->addLayout(sb);
  col->addWidget(result);

  // Advanced
  advanced_ = new Section(tr("Advanced"), side, false);
  advanced_->setObjectName("retopoAdvanced");
  auto* al = advanced_->body();
  al->addWidget(make_label(tr("Texture size"), "secondary", advanced_));
  texture_size_ = new QComboBox(advanced_);
  texture_size_->addItem("1024", 1024);
  texture_size_->addItem("2048", 2048);
  texture_size_->addItem("4096", 4096);
  texture_size_->setCurrentIndex(1);
  al->addWidget(texture_size_);
  for (const auto& [name, why] : {std::pair{tr("Density brush"), tr("Paint where quads should be denser. The engine has only a global size today.")},
                                  std::pair{tr("Local re-retopology"), tr("Rebuild one selected area only.")}}) {
    auto* fr2 = new QVBoxLayout;
    auto* l = make_label(name, nullptr, advanced_);
    l->setToolTip(why);
    fr2->addWidget(l);
    fr2->addWidget(make_feature_badge(Feature::ComingSoon, advanced_), 0, Qt::AlignLeft);
    al->addLayout(fr2);
  }
  engine_info_ = make_label(QString(), "secondary", advanced_);
  engine_info_->setObjectName("retopoEngine");
  engine_info_->setWordWrap(true);
  engine_info_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  al->addWidget(engine_info_);
  auto* locate = make_button(tr("Locate Engine Folder…"), "link", {}, advanced_);
  al->addWidget(locate, 0, Qt::AlignLeft);
  col->addWidget(advanced_);
  col->addStretch(1);

  editor_ = new RetopoMeshEditor(software, this);
  row->addWidget(editor_, 1);

  engine_override_ = QSettings().value("retopo/engine").toString();
  connect(use_generated_, &QPushButton::clicked, this, [this] { useGenerated(); });
  connect(open_, &QPushButton::clicked, this, [this] {
    const QString file = QFileDialog::getOpenFileName(this, tr("Open Model"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
                                                      tr("3D models (*.glb *.obj)"));
    if (!file.isEmpty()) openModel(file);
  });
  connect(run_, &QPushButton::clicked, this, [this] { retopologize(); });
  connect(cancel_, &QPushButton::clicked, this, &RetopoWorkspace::cancel);
  connect(save_, &QPushButton::clicked, this, [this] {
    const QString base = QFileInfo(source_label_).completeBaseName().isEmpty() ? QStringLiteral("model") : QFileInfo(source_label_).completeBaseName();
    const QString file = QFileDialog::getSaveFileName(this, tr("Save Retopology"), outputParent() + "/" + base + "_retopo.glb", tr("glTF binary (*.glb)"));
    if (!file.isEmpty()) saveGlb(file);
  });
  connect(folder_, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(export_file_).absolutePath())); });
  connect(locate, &QPushButton::clicked, this, [this] {
    const QString dir = QFileDialog::getExistingDirectory(this, tr("QuadWild Engine Folder"), engineDir());
    if (dir.isEmpty()) return;
    engine_override_ = dir;
    QSettings().setValue("retopo/engine", dir);
    refresh();
  });
  connect(editor_, &RetopoMeshEditor::edited, this, [this] {
    result_edited_ = true;
    set_chip(verdict_, tr("Edited / safety unverified"), theme::Tone::Review);
    verdict_->setVisible(true);
    verdict_->setToolTip(tr("The measurements describe the pre-edit result. Edited geometry needs a new audit."));
  });
  connect(&process_, &QProcess::readyReadStandardOutput, this, &RetopoWorkspace::readOutput);
  connect(&process_, &QProcess::finished, this, &RetopoWorkspace::processFinished);
  connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
    if (e == QProcess::FailedToStart) processFinished(-1, QProcess::CrashExit);
  });
  refresh();
}

RetopoWorkspace::~RetopoWorkspace() {
  if (process_.state() != QProcess::NotRunning) {
    process_.kill();
    process_.waitForFinished(3000);
  }
}

QString RetopoWorkspace::worker() const { return QDir(QCoreApplication::applicationDirPath()).filePath("pixel3d-retopo.exe"); }

QString RetopoWorkspace::engineDir() const {
  const QString app = QCoreApplication::applicationDirPath();
  for (const QString& dir : {qEnvironmentVariable("PIXEL3D_QUADWILD_DIR"), engine_override_, app + "/retopo", app + "/../retopo"})
    if (engine_complete(dir)) return QDir::cleanPath(dir);
  return {};
}

QString RetopoWorkspace::outputParent() const {
  if (!export_file_.isEmpty()) return QFileInfo(export_file_).absolutePath();
  if (ai_ && ai_->project() && source_file_.startsWith(ai_->project()->dir)) return ai_->project()->dir;
  return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/3re/Retopology";
}

void RetopoWorkspace::refresh() {
  const bool busy = running() || job_ != Job::None;
  const QString engine = engineDir();
  const bool generated = ai_ && ai_->hasResult();
  use_generated_->setEnabled(!busy && generated);
  use_generated_->setToolTip(generated ? tr("The model from Image to 3D") : tr("Generate a model in Image to 3D first"));
  open_->setEnabled(!busy);
  run_->setEnabled(!busy && source_ && !engine.isEmpty());
  save_->setEnabled(!busy && editor_->hasMesh());
  faces_->setEnabled(!busy);
  density_->setEnabled(!busy);
  mode_->setEnabled(!busy);
  engine_info_->setText(engine.isEmpty() ? tr("QuadWild engine: not found. Put it in the app's retopo folder or locate it.")
                                         : tr("Engine: QuadWild-BiMDF 0.0.2 (GPL-3.0), %1").arg(QDir::toNativeSeparators(engine)));
  if (!busy && engine.isEmpty() && source_) setStatus(tr("The QuadWild engine was not found (Advanced → Locate Engine Folder)."), "fail");
}

void RetopoWorkspace::setAdvanced(bool on) { advanced_->setVisible(on); }

void RetopoWorkspace::setStatus(const QString& text, const char* tone) {
  status_->setText(text);
  theme::set_prop(status_, "tone", tone);
}

QString RetopoWorkspace::statusText() const { return status_->text(); }

void RetopoWorkspace::setSource(const QString& name, const QString& file, std::shared_ptr<const pixel3d::Mesh> geometry,
                                std::shared_ptr<const pixel3d::TexturedMesh> appearance) {
  source_label_ = name;
  source_file_ = file;
  source_ = std::move(geometry);
  look_ = std::move(appearance);
  report_ = {};
  result_edited_ = false;
  export_file_.clear();
  folder_->setEnabled(false);
  verdict_->setVisible(false);
  editor_->clear();
  editor_->setReference(source_, look_);
  source_name_->setText(QFileInfo(name).completeBaseName().isEmpty() ? name : QFileInfo(name).completeBaseName());
  source_info_->setText(tr("%1 triangles · %2 vertices%3")
                            .arg(source_->tris.size())
                            .arg(source_->positions.size())
                            .arg(look_ ? tr(" · with colours") : tr(" · geometry only")));
  // About one quad per 20 source triangles, kept within the engine's comfortable range.
  faces_->setValue(std::clamp(int(source_->tris.size() / 20 / 500 * 500), 1000, 12000));
  stats_->setText(tr("Before/after counts and the distance to the original surface appear here."));
  setStatus(tr("Ready. Choose the polygon target and press Retopologize."));
  refresh();
}

bool RetopoWorkspace::useGenerated() {
  if (running() || !ai_ || !ai_->hasResult()) return false;
  const auto& asset = ai_->backend().asset();
  const QString file = QString::fromStdWString(asset.geometry_glb.wstring());
  setSource(ai_->projectName().isEmpty() ? QFileInfo(file).fileName() : ai_->projectName(), file, asset.geometry, asset.appearance);
  return true;
}

bool RetopoWorkspace::openModel(const QString& file) {
  if (running()) return false;
  try {
    std::shared_ptr<const pixel3d::Mesh> geometry;
    std::shared_ptr<const pixel3d::TexturedMesh> look;
    if (file.endsWith(".obj", Qt::CaseInsensitive)) {
      geometry = std::make_shared<pixel3d::Mesh>(p3dasset::retopo::triangulate(p3dasset::retopo::read_obj(fs_path(file))));
    } else {
      geometry = std::make_shared<pixel3d::Mesh>(p3dasset::load_geometry_glb(fs_path(file)));
      try {
        look = p3dasset::load_generated_appearance(fs_path(file), true);
      } catch (const std::exception&) {
        look.reset();  // unsupported material: geometry only, said in the info line
      }
    }
    if (geometry->tris.empty()) throw std::runtime_error("no triangles");
    setSource(QFileInfo(file).fileName(), file, geometry, look);
    return true;
  } catch (const std::exception& e) {
    setStatus(tr("Cannot open %1: %2").arg(QFileInfo(file).fileName(), QString::fromUtf8(e.what())), "fail");
    return false;
  }
}

void RetopoWorkspace::setRunning(bool on, const QString& text) {
  progress_->setVisible(on);
  progress_->setValue(0);
  cancel_->setVisible(on);
  setStatus(text);
  emit busyChanged(on, text);
  refresh();
}

bool RetopoWorkspace::retopologize() {
  const QString engine = engineDir();
  if (running() || !source_ || engine.isEmpty()) return false;
  if (!QFileInfo(worker()).isFile()) {
    setStatus(tr("The retopology worker (pixel3d-retopo.exe) is missing from this installation."), "fail");
    return false;
  }
  if (p3dasset::available_ram() < 1536ull * 1024 * 1024) {
    setStatus(tr("Not enough free memory: retopology needs about 1.5 GiB. Close other programs and try again."), "fail");
    return false;
  }
  QString input = source_file_;
  run_dir_ = outputParent() + "/retopo/" + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss");
  if (!QDir().mkpath(run_dir_)) {
    setStatus(tr("Cannot create %1").arg(QDir::toNativeSeparators(run_dir_)), "fail");
    return false;
  }
  if (input.isEmpty() || !QFileInfo(input).isFile()) {  // in memory only: hand the worker an OBJ copy
    input = run_dir_ + "/source.obj";
    p3dasset::retopo::write_obj(p3dasset::retopo::to_poly(*source_), fs_path(input));
  }
  report_ = {};
  job_ = Job::Remesh;
  setRunning(true, tr("Retopologizing… QuadWild runs on the CPU; a generated model takes a few minutes."));
  process_.setProcessChannelMode(QProcess::SeparateChannels);
  process_.start(worker(), {"remesh", QDir::toNativeSeparators(input), QDir::toNativeSeparators(run_dir_), "--engine", QDir::toNativeSeparators(engine),
                            "--faces", QString::number(faces_->value()), "--mode", mode_->currentData().toString(), "--seconds", "900"});
  return true;
}

bool RetopoWorkspace::saveGlb(const QString& file) {
  if (running() || !editor_->hasMesh() || file.isEmpty()) return false;
  QString dir = run_dir_;
  if (dir.isEmpty() || !QDir(dir).exists()) dir = QFileInfo(file).absolutePath();
  const QString mesh = dir + "/edited.obj";
  try {
    p3dasset::retopo::PolyMesh poly;
    poly.positions = editor_->document().positions();
    poly.faces = editor_->document().faces();
    p3dasset::retopo::write_obj(poly, fs_path(mesh));
  } catch (const std::exception& e) {
    setStatus(tr("Cannot write the mesh: %1").arg(QString::fromUtf8(e.what())), "fail");
    return false;
  }
  QDir().mkpath(QFileInfo(file).absolutePath());
  export_file_ = file;
  job_ = Job::Export;
  setRunning(true, tr("Saving: new UVs, colours from the model, then reading the GLB back…"));
  const QString source = source_file_.isEmpty() ? mesh : source_file_;
  process_.start(worker(), {"export", QDir::toNativeSeparators(source), QDir::toNativeSeparators(mesh), QDir::toNativeSeparators(file), "--size",
                            texture_size_->currentData().toString(), "--seconds", "600"});
  return true;
}

void RetopoWorkspace::cancel() {
  if (!running()) return;
  cancelled_ = true;
  process_.kill();  // the worker's job object takes the engine processes with it
}

void RetopoWorkspace::readOutput() {
  while (process_.canReadLine()) {
    const QString line = QString::fromUtf8(process_.readLine()).trimmed();
    if (!line.startsWith("RETOPO_PROGRESS ")) continue;
    const QString rest = line.mid(16);
    const int space = rest.indexOf(' ');
    progress_->setValue(rest.left(space).toInt());
    const QString text = rest.mid(space + 1);
    setStatus(text + QStringLiteral("…"));
    emit busyChanged(true, text);
  }
}

void RetopoWorkspace::processFinished(int code, QProcess::ExitStatus status) {
  const Job job = job_;
  job_ = Job::None;
  const bool cancelled = std::exchange(cancelled_, false);
  const QString errors = QString::fromUtf8(process_.readAllStandardError()).trimmed();
  const QString report_file = job == Job::Remesh ? run_dir_ + "/retopo.json" : export_file_ + ".retopo.json";
  nlohmann::json report;
  try {
    std::ifstream in(fs_path(report_file));
    if (in) report = nlohmann::json::parse(in);
  } catch (...) {
    report = {};
  }
  progress_->setVisible(false);
  cancel_->setVisible(false);
  emit busyChanged(false, QString());
  const bool ok = status == QProcess::NormalExit && code == 0 && report.value("status", std::string()) == "OK";
  QString message;
  if (!ok) {
    message = cancelled                                  ? tr("Cancelled.")
              : report.contains("error")                 ? QString::fromStdString(report["error"].get<std::string>())
              : errors.isEmpty()                         ? tr("The worker stopped (exit %1).").arg(code)
                                                         : errors;
    setStatus((job == Job::Export ? tr("Not saved: %1") : tr("Retopology failed: %1")).arg(message), "fail");
    refresh();
    emit finished(false, message);
    return;
  }
  if (job == Job::Remesh) {
    report_ = report;
    try {
      editor_->setMesh(p3dasset::retopo::read_obj(fs_path(run_dir_ + "/result.obj")));
      result_edited_ = false;
      showResult();
      message = tr("Done in %1 s.").arg(report.value("seconds", 0.0), 0, 'f', 0);
      setStatus(message + " " + tr("Compare with Original model above the view; select and move vertices to correct it."), "pass");
    } catch (const std::exception& e) {
      message = QString::fromUtf8(e.what());
      setStatus(tr("Cannot read the result: %1").arg(message), "fail");
      refresh();
      emit finished(false, message);
      return;
    }
  } else {
    folder_->setEnabled(true);
    const auto& re = report["reimport"];
    message = tr("Saved and re-imported: %1 · %2 triangles identical%3. Quads: %4")
                  .arg(QDir::toNativeSeparators(export_file_))
                  .arg(re.value("triangles", 0))
                  .arg(look_ ? tr(", colours from the model") : tr(", plain grey (the model has no colours)"))
                  .arg(QDir::toNativeSeparators(QFileInfo(export_file_).path() + "/" + QFileInfo(export_file_).completeBaseName() + ".obj"));
    setStatus(message, "pass");
    // Show what was written: the GLB read back from disk, with its texture, in the same view.
    try {
      // As the worker read it back: the export keeps the source's texture filtering (mipmaps included).
      editor_->setSaved(p3dasset::load_generated_appearance(fs_path(export_file_), true));
    } catch (const std::exception& e) {
      setStatus(message + "\n" + tr("(The saved texture could not be shown here: %1)").arg(QString::fromUtf8(e.what())), "pass");
    }
  }
  refresh();
  emit finished(true, message);
}

void RetopoWorkspace::showResult() {
  const auto& r = report_["result"];
  // A generated mesh with tiny tunnels and internal sheets is compared with its outer surface (the full
  // model's numbers stay in retopo.json).
  const bool outer = r.contains("deviation_outer");
  const auto& there = outer ? r["deviation_outer"]["source_to_result"] : r["deviation"]["source_to_result"];
  const auto& back = r["deviation"]["result_to_source"];
  const double p95 = std::max(there.value("p95", 1.0), back.value("p95", 1.0));
  const double worst = std::max(there.value("max", 1.0), back.value("max", 1.0));
  stats_->setText(tr("Before: %1 triangles\nAfter: %2 polygons (%3 quads, %4 triangles), %5 vertices · %6 quads\n"
                     "Surface distance (of the model size): mean %7, 95% within %8, largest %9\n"
                     "Open edges: %10 · non-manifold edges: %11")
                      .arg(r.value("source_triangles", 0))
                      .arg(r.value("faces", 0))
                      .arg(r.value("quads", 0))
                      .arg(r.value("triangles", 0) + r.value("other_polygons", 0))
                      .arg(r.value("vertices", 0))
                      .arg(percent(r.value("quad_share", 0.0)))
                      .arg(percent(std::max(there.value("mean", 0.0), back.value("mean", 0.0))))
                      .arg(percent(p95))
                      .arg(percent(worst))
                      .arg(r.value("open_edges", 0))
                      .arg(r.value("non_manifold_edges", 0)) +
                  (report_["input"].contains("shell")
                       ? tr("\nGenerated mesh: %1 tiny tunnels and internal surfaces closed first (%2 remain); distances are to its outer surface.")
                             .arg(report_["input"].value("handles", 0))
                             .arg(report_["input"]["shell"].value("handles", 0))
                       : QString()) +
                  (r.value("intersection_status", std::string()) == "CROSSING"
                       ? tr("\nSelf-intersections: %1 face pairs cross, where the model is thinner than a quad. More polygons "
                            "reduce them; the engine has one quad size for the whole model.")
                             .arg(r.value("self_intersections", 0))
                       : r.value("intersection_status", std::string()) == "CLEAR" ? tr("\nSelf-intersections: none.")
                                                                                   : tr("\nSelf-intersections: not checked.")));
  // Honest summary only; the numbers above are the evidence.
  const bool close = p95 <= 0.005 && worst <= 0.03;
  const std::string safety = r.value("safety", "UNKNOWN");
  if (safety == "UNSAFE")
    set_chip(verdict_, tr("Unsafe: self-intersections"), theme::Tone::Fail);
  else if (safety == "CLEAR" && close)
    set_chip(verdict_, tr("Shape kept · no intersections"), theme::Tone::Pass);
  else
    set_chip(verdict_, close ? tr("Shape close / safety unverified") : tr("Check shape / safety"), theme::Tone::Review);
  verdict_->setToolTip(tr("Shape kept: 95% of both surfaces within 0.5% of the model size and nothing further than 3%; "
                          "no faces crossing each other (3re geometry audit)."));
  verdict_->setVisible(true);
}

}  // namespace p3dgui
