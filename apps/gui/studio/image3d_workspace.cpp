#include "studio/image3d_workspace.hpp"

#include <QCheckBox>
#include <cmath>
#include <QComboBox>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include "backends/settings_env.hpp"
#include "backends/trellis_diagnostics.hpp"
#include "segmentation/color_regions.hpp"
#include "segmentation/segment_editor.hpp"
#include "segmentation/segmentation_store.hpp"
#include "icons.hpp"
#include "shell/feature_badge.hpp"
#include "shell/step_bar.hpp"
#include "studio/angles_panel.hpp"
#include "studio/preview_bar.hpp"
#include "viewport.hpp"
#include "widgets.hpp"

namespace p3dgui {
namespace {

constexpr double GiB = 1024.0 * 1024 * 1024;

// One honest line for the Auto Texture result: quality verdict, how much was really seen, first issue.
QString autoTextureSummary(const QJsonObject& report) {
  const auto a = report.value("auto_texture").toObject();
  const double seen = a.value("observed_fraction").toDouble() + a.value("blended_fraction").toDouble();
  const bool hybrid = a.value("native_fraction").toDouble() > 0;
  QString text = (hybrid ? QObject::tr("Hybrid Texture · quality %1 · %2% of the surface seen in the picture; hidden sides show TRELLIS's "
                                       "own texture, colour-matched to the picture (model-inferred)")
                         : QObject::tr("Auto Texture · quality %1 · %2% of the surface seen in the picture; hidden sides are reconstructed estimates"))
                     .arg(a.value("verdict").toString("UNKNOWN"))
                     .arg(qRound(seen * 100));
  const auto views = a.value("views").toArray();
  if (!views.isEmpty()) {
    int used = 0;
    for (const auto& v : views) used += v.toObject().value("status").toString() == "accepted";
    const auto own = a.value("picture_ownership").toObject();
    text += QObject::tr("\nPictures: front + %1 of %2 other angle(s) used · %3% of the surface seen by none of them")
                .arg(used).arg(views.size()).arg(qRound(own.value("not_seen_by_any_picture").toDouble() * 100));
  }
  const auto issues = a.value("issues").toArray();
  if (!issues.isEmpty()) text += "\n" + issues.first().toString();
  if (a.value("registration").toObject().value("ambiguous").toBool())
    text += QObject::tr("\nThe outline fits more than one viewpoint; the model front was assumed.");
  return text;
}

// A saved result's own texture report (<glb>.texture.json), in the shape the backend reports a fresh pass.
QJsonObject savedTextureReport(const QString& glb) {
  QFile f(glb + ".texture.json");
  if (!QFileInfo(f.fileName()).isFile() || f.size() > 64 * 1024 * 1024 || !f.open(QIODevice::ReadOnly)) return {};
  const auto j = QJsonDocument::fromJson(f.readAll()).object();
  QJsonObject bake;
  for (const auto& at : j.value("attempts").toArray())
    if (!at.toObject().value("bake").toObject().isEmpty()) bake = at.toObject().value("bake").toObject();
  const auto reg = j.value("registration").toObject();
  return {{"verdict", j.value("verdict")}, {"issues", j.value("issues")}, {"views", j.value("views")},
          {"observed_fraction", bake.value("observed_fraction")}, {"blended_fraction", bake.value("blended_fraction")},
          {"native_fraction", bake.value("native_fraction")}, {"picture_ownership", bake.value("picture_ownership")},
          {"registration", QJsonObject{{"ambiguous", reg.value("ambiguous")}}}};
}

QString validationSummary(const QJsonObject& report) {
  const auto t = report.value("topology").toObject();
  const auto i = report.value("intersections").toObject();
  const auto p = i.value("pair_classification").toObject();
  return QObject::tr("%1 vertices · %2 triangles · %3 components\n"
                     "Self-crossing pairs: %4 · overlapping pairs: %5\n"
                     "Nonmanifold edges: %6 · invalid vertex fans: %7\n"
                     "Duplicate/internal faces: %8 · boundaries: %9\n"
                     "Winding errors: %10 · zero-area faces: %11\n"
                     "Audit complete: %12")
      .arg(t.value("vertices").toInt()).arg(t.value("triangles").toInt()).arg(t.value("components").toInt())
      .arg(p.value("transverse_crossing").toInt()).arg(p.value("coplanar_area_overlap").toInt())
      .arg(t.value("overfull_edges").toInt()).arg(t.value("invalid_vertex_links").toInt())
      .arg(t.value("duplicate_faces").toInt() + t.value("internal_faces").toInt()).arg(t.value("boundary_edges").toInt())
      .arg(t.value("inconsistent_winding_edges").toInt()).arg(t.value("zero_area_faces").toInt())
      .arg(i.value("complete").toBool() ? QObject::tr("yes") : QObject::tr("no"));
}

// What a validation result means, in words a beginner can act on.
QString plainVerdict(p3dasset::Validation v) {
  switch (v) {
    case p3dasset::Validation::Pass: return QObject::tr("Clean, closed model. Ready to export.");
    case p3dasset::Validation::Review:
      return QObject::tr("Usable, with reported issues (for example touching or crossing surfaces). Check the model from "
                         "every side before you rely on it; you can still export it.");
    case p3dasset::Validation::Unsafe:
      return QObject::tr("The shape has defects that can break games or 3D printing. It is kept as generated, never "
                         "silently changed. Try another seed or a cleaner picture, or export it knowingly.");
    case p3dasset::Validation::Unknown: break;
  }
  return QObject::tr("Not checked.");
}

QFrame* card(QWidget* parent) {
  auto* c = new Card(parent);
  return c;
}

QVBoxLayout* card_layout(QFrame* c, const QString& title) {
  auto* l = new QVBoxLayout(c);
  l->setContentsMargins(theme::space::lg, theme::space::lg, theme::space::lg, theme::space::lg);
  l->setSpacing(theme::space::sm);
  if (!title.isEmpty()) l->addWidget(make_label(title, "section"));
  return l;
}

QScrollArea* scrolled(QWidget* inner) {
  auto* s = new QScrollArea;
  s->setWidgetResizable(true);
  s->setFrameShape(QFrame::NoFrame);
  s->setWidget(inner);
  return s;
}

}  // namespace

Image3DWorkspace::Image3DWorkspace(bool force_cpu, QWidget* parent) : QWidget(parent), backend_(this) {
  setObjectName("Image3DWorkspace");
  setAcceptDrops(true);
  projects_dir_ = default_projects_dir();
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(theme::space::xl, theme::space::md, theme::space::xl, theme::space::lg);
  root->setSpacing(theme::space::md);

  auto* top = new QHBoxLayout;
  steps_ = new StepBar({tr("Import"), tr("Segment"), tr("Generate"), tr("Preview")}, this);
  top->addWidget(steps_, 1);
  project_label_ = make_label(tr("No project yet"), "secondary", this);
  project_label_->setObjectName("image3dProject");
  project_label_->setToolTip(tr("A project is created when you segment or generate; it keeps the image, regions and every result."));
  top->addWidget(project_label_);
  auto* openProject = make_button(tr("Open Project…"), "ghost", icon(Icon::Folder), this);
  openProject->setObjectName("image3dOpenProject");
  top->addWidget(openProject);
  root->addLayout(top);

  viewport_ = new Viewport(force_cpu);
  viewport_->setObjectName("image3dPreview");
  pages_ = new QStackedWidget(this);
  pages_->addWidget(buildImportStep());
  pages_->addWidget(buildSegmentStep());
  pages_->addWidget(buildGenerateStep());
  pages_->addWidget(buildPreviewStep());
  root->addWidget(pages_, 1);

  connect(steps_, &StepBar::stepClicked, this, &Image3DWorkspace::showStep);
  connect(openProject, &QPushButton::clicked, this, [this] {
    const auto f = QFileDialog::getOpenFileName(this, tr("Open 3re project"), projects_dir_, tr("3re projects (project.3re)"));
    if (!f.isEmpty()) this->openProject(f);
  });

  // Backend signals.
  connect(&backend_, &p3dasset::TrellisBackend::progress, this, [this](int p, const QString& stage) {
    progress_->setValue(p);
    status_->setText(stage);
    emit busyChanged(true, stage);
  });
  connect(&backend_, &p3dasset::TrellisBackend::resources, this, [this](double ram, double gpu, bool measured) {
    resources_->setText(tr("Worker RAM: %1 MiB · sampled VRAM: %2").arg(ram, 0, 'f', 1).arg(measured ? QString::number(gpu, 'f', 1) + " MiB" : tr("not measured")));
  });
  connect(&backend_, &p3dasset::TrellisBackend::logLine, log_, &QPlainTextEdit::appendPlainText);
  connect(&backend_, &p3dasset::TrellisBackend::finished, this, &Image3DWorkspace::showResult);
  connect(&image_job_, &QFutureWatcher<p3dasset::ImageInput>::finished, this, [this] {
    p3dasset::ImageInput input;
    try {
      input = image_job_.result();
    } catch (const std::exception& e) {
      status_->setText(QString::fromUtf8(e.what()));
      import_hint_->setText(status_->text());
      setBusy(false);
      return;
    }
    cutout_ = input.error.isEmpty() ? input.cutout : QImage();
    appearance_source_ = input.error.isEmpty() ? input.appearance : QImage();
    crisp_ = input.crisp;
    setBusy(false);
    const auto ready = !generate_->isEnabled() ? tr("Cutout ready · generation requires RAM and verified GPU readiness.")
                                               : tr("Cutout ready · review the subject, then Generate 3D.");
    status_->setText(input.error.isEmpty() ? ready + (input.warning.isEmpty() ? QString() : "\n" + input.warning) : input.error);
    image_->setToolTip(tr("Original: %1 × %2. Working cutout: %3 × %4. Original file unchanged.")
                           .arg(input.original_size.width()).arg(input.original_size.height()).arg(cutout_.width()).arg(cutout_.height()));
    if (!cutout_.isNull()) {
      image_->setPixmap(QPixmap::fromImage(cutout_).scaled(360, 360, Qt::KeepAspectRatio, crisp_ ? Qt::FastTransformation : Qt::SmoothTransformation));
      import_hint_->setText(input.warning.isEmpty() ? tr("The subject was isolated. Check the outline, then continue.") : input.warning);
      steps_->setCompleted(Import, true);
      to_segment_->setEnabled(true);
      pixel_hint_->setVisible(crisp_);
      if (segment_) loadSegmentation();
    } else {
      image_->setText(tr("Image could not be prepared"));
      import_hint_->setText(input.error);
    }
    refreshMemoryState();
    angles_->setEnabled(!cutout_.isNull());
    angles_->setInputType(input_type_->currentIndex(), white_->isChecked());
    if (!pending_result_.isEmpty() && !cutout_.isNull()) {
      const QString result = pending_result_;
      pending_result_.clear();
      if (QFileInfo(result).isFile()) openSaved(result);
    }
  });
  connect(&scene_job_, &QFutureWatcher<Scene>::finished, this, [this] {
    try {
      viewport_->setScene(scene_job_.result());
      viewport_->setIso();
      setBusy(false);
      export_->setEnabled(true);
    } catch (const std::exception& e) {
      status_->setText(QString::fromUtf8(e.what()));
      setBusy(false);
      export_->setEnabled(false);
    }
  });
  connect(&gpu_job_, &QFutureWatcher<QJsonObject>::finished, this, [this] {
    const auto health = gpu_job_.result();
    const bool current = health.value("setup").toString() == setup_->text();
    generate_->setProperty("cudaReady", current && health.value("ready").toBool());
    gpu_status_->setText(current ? health.value("message").toString() : tr("Setup changed; check GPU readiness."));
    theme::set_prop(gpu_status_, "tone", current && health.value("ready").toBool() ? "pass" : "review");
    gpu_check_->setEnabled(!backend_.busy());
    refreshMemoryState();
    // The import message said the GPU still had to be checked; say what is true now.
    if (status_->text().startsWith(tr("Cutout ready")) && generate_->isEnabled()) status_->setText(tr("Ready to generate. Press Generate 3D."));
  });
  memory_timer_ = new QTimer(this);
  memory_timer_->setInterval(3000);
  connect(memory_timer_, &QTimer::timeout, this, &Image3DWorkspace::refreshMemoryState);
  memory_timer_->start();
  clock_ = new QTimer(this);
  clock_->setInterval(1000);
  connect(clock_, &QTimer::timeout, this, [this] {
    const qint64 s = (QDateTime::currentMSecsSinceEpoch() - started_ms_) / 1000;
    elapsed_->setText(tr("Elapsed %1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0')));
  });
  setAdvanced(false);
  updateSamPaths();
  connect(setup_, &QLineEdit::textChanged, this, &Image3DWorkspace::updateSamPaths);
  refreshMemoryState();
  showStep(Import);
}

void Image3DWorkspace::updateSamPaths() {
  QString dir, dll;
  try {
    const auto sam = p3dasset::SamConfig::read(setup_->text());
    dir = sam.dir;
    dll = sam.onnxruntime;
  } catch (...) {
  }
  segment_->setSamPaths(dir, dll);
}

Image3DWorkspace::~Image3DWorkspace() { backend_.cancel(); }

QWidget* Image3DWorkspace::buildImportStep() {
  auto* page = new QWidget;
  auto* row = new QHBoxLayout(page);
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(theme::space::lg);

  auto* left = card(page);
  auto* l = card_layout(left, tr("1 · Choose a picture"));
  auto* intro = make_label(tr("One object or character per picture works best: a plain background, the whole subject "
                              "visible, front or three-quarter view. Photos, illustrations, characters and AI art all work."),
                           "secondary", left);
  intro->setWordWrap(true);
  l->addWidget(intro);
  import_ = make_button(tr("Choose Image…"), "primary", icon(Icon::Import, IconStyle::OnAccent), left);
  import_->setObjectName("image3dImport");
  import_->setMinimumHeight(40);
  import_->setToolTip(tr("PNG, JPG, WebP or BMP. The original file is never changed."));
  l->addWidget(import_);
  auto* form = new QFormLayout;
  form->setLabelAlignment(Qt::AlignLeft);
  input_type_ = new QComboBox(left);
  input_type_->setObjectName("image3dInputType");
  input_type_->addItems({tr("Auto"), tr("Photo"), tr("Illustration"), tr("Character"), tr("Pixel Art")});
  input_type_->setToolTip(tr("Helps the cutout: pixel art keeps hard pixel edges; photos get smooth edges."));
  form->addRow(tr("What is in the picture?"), input_type_);
  l->addLayout(form);
  white_ = new QCheckBox(tr("Remove the background automatically"), left);
  white_->setChecked(true);
  white_->setObjectName("image3dWhite");
  white_->setToolTip(tr("Removes uniform exterior backgrounds. Complex scenes remain full-frame and are reported as approximate; "
                        "review the subject before generation."));
  l->addWidget(white_);
  import_hint_ = make_label(tr("No picture yet."), "secondary", left);
  import_hint_->setWordWrap(true);
  l->addWidget(import_hint_);
  pixel_hint_ = make_label(tr("This looks like pixel art. For clean low-poly models from drawings, the Pixel Art Builder "
                              "usually gives better results."),
                           "secondary", left);
  pixel_hint_->setWordWrap(true);
  pixel_hint_->hide();
  l->addWidget(pixel_hint_);
  l->addStretch(1);
  to_segment_ = make_button(tr("Continue"), "primary", icon(Icon::ChevronRight, IconStyle::OnAccent), left);
  to_segment_->setObjectName("image3dToSegment");
  to_segment_->setEnabled(false);
  to_segment_->setToolTip(tr("Choose a picture first."));
  l->addWidget(to_segment_, 0, Qt::AlignRight);
  left->setMaximumWidth(420);
  row->addWidget(left);

  auto* right = card(page);
  auto* r = card_layout(right, tr("Subject that will be turned into 3D"));
  image_ = new QLabel(tr("Drop a picture here\nor use Choose Image…"), right);
  image_->setObjectName("image3dImage");
  image_->setAlignment(Qt::AlignCenter);
  image_->setMinimumSize(320, 320);
  image_->setProperty("checker", true);
  r->addWidget(image_, 1);
  auto* anglesSection = angles_section_ = new Section(tr("Other angles (optional)"), right, false);
  angles_ = new AnglesPanel(right);
  angles_->setEnabled(false);
  anglesSection->body()->addWidget(angles_);
  r->addWidget(anglesSection);
  row->addWidget(right, 1);
  connect(angles_, &AnglesPanel::changed, this, &Image3DWorkspace::saveAngles);
  connect(angles_, &AnglesPanel::sheetSplit, this, [this](const QString& front) {
    if (image_path_.isEmpty()) importImage(front);
  });

  connect(import_, &QPushButton::clicked, this, [this] {
    const auto file = QFileDialog::getOpenFileName(this, tr("Choose a picture"), {}, tr("Images (*.png *.jpg *.jpeg *.webp *.bmp)"));
    if (!file.isEmpty()) importImage(file);
  });
  connect(white_, &QCheckBox::toggled, this, [this] { if (!image_path_.isEmpty()) importImage(image_path_); });
  connect(input_type_, &QComboBox::currentIndexChanged, this, [this] { if (!image_path_.isEmpty()) importImage(image_path_); });
  connect(to_segment_, &QPushButton::clicked, this, [this] { showStep(Segment); });
  return page;
}

QWidget* Image3DWorkspace::buildSegmentStep() {
  auto* page = new QWidget;
  auto* col = new QVBoxLayout(page);
  col->setContentsMargins(0, 0, 0, 0);
  col->setSpacing(theme::space::md);
  auto* head = new QHBoxLayout;
  auto* info = make_label(tr("Optional. Mark the regions of the picture to keep: click a region to select it, then label it. "
                             "Only included regions are turned into 3D. Skip this to use the automatic cutout."),
                          "secondary", page);
  info->setWordWrap(true);
  head->addWidget(info, 1);
  skip_segment_ = make_button(tr("Skip — use the automatic cutout"), "ghost", {}, page);
  skip_segment_->setObjectName("image3dSkipSegment");
  head->addWidget(skip_segment_);
  to_generate_ = make_button(tr("Continue"), "primary", icon(Icon::ChevronRight, IconStyle::OnAccent), page);
  to_generate_->setObjectName("image3dToGenerate");
  head->addWidget(to_generate_);
  col->addLayout(head);
  auto* host = new QWidget(page);
  host->setObjectName("image3dSegmentHost");
  auto* hl = new QVBoxLayout(host);
  hl->setContentsMargins(0, 0, 0, 0);
  segment_ = new SegmentEditor(host);
  hl->addWidget(segment_);
  col->addWidget(host, 1);
  seg_save_ = new QTimer(this);
  seg_save_->setSingleShot(true);
  seg_save_->setInterval(500);
  connect(seg_save_, &QTimer::timeout, this, &Image3DWorkspace::saveSegmentation);
  connect(segment_, &SegmentEditor::regionsChanged, seg_save_, qOverload<>(&QTimer::start));
  connect(segment_, &SegmentEditor::engineMetrics, this, &Image3DWorkspace::segmentationMetrics);
  auto next = [this] {
    steps_->setCompleted(Segment, true);
    showStep(Generate);
  };
  connect(skip_segment_, &QPushButton::clicked, this, next);
  connect(to_generate_, &QPushButton::clicked, this, next);
  return page;
}

QWidget* Image3DWorkspace::buildGenerateStep() {
  auto* page = new QWidget;
  auto* row = new QHBoxLayout(page);
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(theme::space::lg);

  auto* inner = new QWidget;
  auto* left = new QVBoxLayout(inner);
  left->setContentsMargins(0, 0, 8, 0);
  left->setSpacing(theme::space::md);

  auto* quality = card(inner);
  auto* q = card_layout(quality, tr("Quality"));
  auto* form = new QFormLayout;
  profile_ = new QComboBox(quality);
  profile_->setObjectName("image3dProfile");
  profile_->addItems({tr("Fast / 4GB Safe"), tr("Balanced"), tr("High Quality — unavailable"), tr("Ultra — unavailable")});
  profile_->setCurrentIndex(1);
  resolution_ = new QComboBox(quality);
  resolution_->setObjectName("image3dResolution");
  resolution_->addItems({tr("512 — Low VRAM"), tr("1024 — High Detail (unavailable)"), tr("1536 — unavailable")});
  auto disable = [](QComboBox* box, int index, const QString& reason) {
    auto* item = qobject_cast<QStandardItemModel*>(box->model())->item(index);
    item->setEnabled(false);
    item->setToolTip(reason);
  };
  const auto reason = tr("True 1024 needs the separate shape_flow_1024 weights, which are not installed. The pinned 4GB runtime "
                         "explicitly rejects the 1024 cascade; its memory path and diagnostic capture support 512 only. No "
                         "substitution, download or inference will occur.");
  disable(profile_, 2, reason);
  disable(profile_, 3, reason);
  disable(resolution_, 1, reason);
  disable(resolution_, 2, reason);
  profile_->setToolTip(tr("Both available profiles use verified 512 geometry / Q4 / 12 steps. Fast: 512 px source texture. "
                          "Balanced: up to 1024 px."));
  form->addRow(tr("Profile"), profile_);
  form->addRow(tr("Geometry"), resolution_);
  q->addLayout(form);
  appearance_mode_ = new QComboBox(quality);
  appearance_mode_->setObjectName("image3dAppearanceMode");
  appearance_mode_->addItems({tr("Geometry Only"), tr("Auto Texture (recommended)"), tr("Generated PBR — Experimental (saved session)"),
                              tr("Auto Texture HD — full-resolution picture")});
  appearance_mode_->setCurrentIndex(1);
  appearance_mode_->setToolTip(tr("Auto Texture finds the picture's viewpoint and projects it only onto surfaces it really shows. "
                                  "When TRELLIS's texture weights are installed, the same generation also paints the whole object and "
                                  "hidden sides use that texture, colour-calibrated to the picture (Hybrid); otherwise they are filled "
                                  "from nearby visible colours. Quality checks, including the back, are in the report. "
                                  "Generated PBR (Experimental) needs a saved session (Advanced)."));
  auto* look = new QHBoxLayout;
  look->addWidget(make_label(tr("Colours"), nullptr, quality));
  look->addWidget(appearance_mode_, 1);
  q->addLayout(look);
  texture_ = new QCheckBox(quality);
  texture_->setChecked(true);
  texture_->setObjectName("image3dTexture");
  texture_->hide();
  left->addWidget(quality);

  auto* ready = card(inner);
  auto* rl = card_layout(ready, tr("Readiness"));
  memory_ = make_label(QString(), "secondary", ready);
  memory_->setObjectName("image3dMemory");
  memory_->setWordWrap(true);
  rl->addWidget(memory_);
  gpu_status_ = make_label(tr("GPU readiness not checked. Image preview is available."), "secondary", ready);
  gpu_status_->setObjectName("image3dGpuStatus");
  gpu_status_->setWordWrap(true);
  rl->addWidget(gpu_status_);
  gpu_check_ = make_button(tr("Check GPU"), "ghost", icon(Icon::Validate), ready);
  gpu_check_->setObjectName("image3dCheckGpu");
  gpu_check_->setToolTip(tr("Runs a short CUDA probe. No model is loaded and nothing is generated."));
  rl->addWidget(gpu_check_, 0, Qt::AlignLeft);
  left->addWidget(ready);

  // Advanced: everything a beginner never needs.
  advanced_box_ = card(inner);
  auto* al = card_layout(static_cast<QFrame*>(advanced_box_), tr("Advanced"));
  auto* af = new QFormLayout;
  seed_ = new QSpinBox(advanced_box_);
  seed_->setRange(0, 2147483647);
  seed_->setValue(42);
  seed_->setObjectName("image3dSeed");
  seed_->setToolTip(tr("Another seed gives another shape from the same picture."));
  af->addRow(tr("Seed"), seed_);
  output_ = new QLineEdit(advanced_box_);
  output_->setObjectName("image3dOutput");
  const auto portableData = p3dasset::setting_env("PORTABLE_USERDATA");
  output_->setText(portableData.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/3re/Generated"
                                          : QDir(portableData).absoluteFilePath("Generated"));
  auto_output_ = output_->text();
  output_->setToolTip(tr("Results go into the project's runs folder; set a folder here to keep them elsewhere."));
  af->addRow(tr("Results folder"), output_);
  setup_ = new QLineEdit(advanced_box_);
  setup_->setObjectName("image3dSetup");
  QSettings preferences;
  const auto explicitSetup = p3dasset::setting_env("TRELLIS_CONFIG");
  const auto savedSetup = preferences.value("trellis/setup").toString();
  const bool customSaved = QFileInfo(savedSetup).isFile() && QFileInfo(savedSetup).fileName() != "trellis-backend.local.json" &&
                           QFileInfo(savedSetup).fileName() != "3re.config.json";
  setup_->setText(!explicitSetup.isEmpty() ? explicitSetup : customSaved ? savedSetup : p3dasset::find_backend_config());
  setup_->setToolTip(tr("The generation setup (runtime, models, checksums). The release's own 3re.config.json is used by default."));
  af->addRow(tr("Setup file"), setup_);
  al->addLayout(af);
  capture_ = new QCheckBox(tr("Save diagnostic stage captures"), advanced_box_);
  capture_->setChecked(true);
  capture_->setObjectName("image3dCapture");
  al->addWidget(capture_);
  auto* tools = new QHBoxLayout;
  saved_ = make_button(tr("Open Saved Result…"), "ghost", {}, advanced_box_);
  saved_->setObjectName("image3dSaved");
  resume_ = make_button(tr("Resume Cleanup…"), "ghost", {}, advanced_box_);
  resume_->setObjectName("image3dResume");
  resume_->setToolTip(tr("Re-run the cleanup on a saved raw.glb (requires its sibling raw.ply). No inference."));
  tools->addWidget(saved_);
  tools->addWidget(resume_);
  al->addLayout(tools);
  auto* pbr = new QHBoxLayout;
  texture_session_button_ = make_button(tr("Saved Texture Session…"), "ghost", {}, advanced_box_);
  texture_session_button_->setObjectName("image3dTextureSession");
  pbr->addWidget(texture_session_button_);
  pbr->addWidget(make_feature_badge(Feature::Experimental, advanced_box_));
  pbr->addStretch(1);
  al->addLayout(pbr);
  left->addWidget(advanced_box_);
  left->addStretch(1);
  auto* scroll = scrolled(inner);
  scroll->setMinimumWidth(360);
  scroll->setMaximumWidth(430);
  row->addWidget(scroll);

  auto* run = card(page);
  auto* r = card_layout(run, tr("2 · Generate the 3D model"));
  auto* explain = make_label(tr("TRELLIS.2 runs on your GPU (4 GB profile, 512 geometry). It takes a few minutes; the result is "
                                "cleaned without changing its shape and then checked. Your picture never leaves this computer."),
                             "secondary", run);
  explain->setWordWrap(true);
  r->addWidget(explain);
  auto* buttons = new QHBoxLayout;
  generate_ = make_button(tr("Generate 3D"), "primary", icon(Icon::Reconstruct, IconStyle::OnAccent), run);
  generate_->setObjectName("image3dGenerate");
  generate_->setEnabled(false);
  generate_->setMinimumHeight(40);
  cancel_ = make_button(tr("Cancel"), "ghost", icon(Icon::Close), run);
  cancel_->setEnabled(false);
  cancel_->setObjectName("image3dCancel");
  buttons->addWidget(generate_);
  buttons->addWidget(cancel_);
  buttons->addStretch(1);
  r->addLayout(buttons);
  status_ = make_label(tr("Import an isolated object to begin."), "status", run);
  status_->setObjectName("image3dStatus");
  status_->setWordWrap(true);
  status_->setStyleSheet("font-size: 12pt;");
  r->addWidget(status_);
  progress_ = new QProgressBar(run);
  progress_->setRange(0, 100);
  progress_->setValue(0);
  r->addWidget(progress_);
  auto* meta = new QHBoxLayout;
  elapsed_ = make_label(QString(), "secondary", run);
  resources_ = make_label(tr("Worker RAM / VRAM: not measured"), "secondary", run);
  meta->addWidget(elapsed_);
  meta->addStretch(1);
  meta->addWidget(resources_);
  r->addLayout(meta);
  auto* details = new Section(tr("Details"), run, false);
  log_ = new QPlainTextEdit(run);
  log_->setReadOnly(true);
  log_->setMaximumBlockCount(1000);
  log_->setMinimumHeight(160);
  details->body()->addWidget(log_);
  r->addWidget(details);
  r->addStretch(1);
  row->addWidget(run, 1);

  connect(generate_, &QPushButton::clicked, this, &Image3DWorkspace::generate);
  connect(cancel_, &QPushButton::clicked, this, &Image3DWorkspace::cancel);
  connect(gpu_check_, &QPushButton::clicked, this, &Image3DWorkspace::checkGpu);
  connect(setup_, &QLineEdit::textChanged, this, [this] {
    generate_->setProperty("cudaReady", false);
    generate_->setEnabled(false);
    gpu_status_->setText(tr("Setup changed; check GPU readiness."));
  });
  connect(appearance_mode_, &QComboBox::currentIndexChanged, this, [this](int mode) {
    texture_->setChecked(mode == 1 || mode == 3);
    generate_->setText(mode == 2 ? tr("Generate PBR on saved geometry") : tr("Generate 3D"));
    refreshMemoryState();
  });
  connect(texture_session_button_, &QPushButton::clicked, this, [this] {
    const auto f = QFileDialog::getOpenFileName(this, tr("Validated saved texture session"), {}, tr("JSON (*.json)"));
    if (!f.isEmpty()) setTextureSession(f);
  });
  connect(saved_, &QPushButton::clicked, this, [this] {
    const auto file = QFileDialog::getOpenFileName(this, tr("Open generated geometry"), {}, tr("Geometry GLB (*.glb)"));
    if (!file.isEmpty()) openSaved(file);
  });
  connect(resume_, &QPushButton::clicked, this, [this] {
    const auto raw = QFileDialog::getOpenFileName(this, tr("Resume saved raw.glb (requires sibling raw.ply)"), {}, tr("GLB (*.glb)"));
    if (raw.isEmpty()) return;
    try {
      auto config = p3dasset::BackendConfig::read(setup_->text());
      QDir().mkpath(output_->text());
      setBusy(true);
      applyAppearance();
      if (!backend_.resumeCleanup(config, raw, output_->text())) setBusy(false);
    } catch (const std::exception& e) {
      status_->setText(QString::fromUtf8(e.what()));
      setBusy(false);
    }
  });
  return page;
}

QWidget* Image3DWorkspace::buildPreviewStep() {
  auto* page = new QWidget;
  auto* row = new QHBoxLayout(page);
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(theme::space::lg);
  auto* view = card(page);
  auto* v = card_layout(view, QString());
  viewport_->setMinimumHeight(320);
  v->addWidget(viewport_, 1);
  preview_bar_ = new PreviewBar(viewport_, view);
  v->addWidget(preview_bar_);
  row->addWidget(view, 1);

  auto* side = card(page);
  side->setMinimumWidth(320);
  side->setMaximumWidth(400);
  auto* s = card_layout(side, tr("3 · Check the result"));
  result_chip_ = make_chip(tr("No result yet"), theme::Tone::Neutral, side);
  result_chip_->setObjectName("image3dResultChip");
  s->addWidget(result_chip_, 0, Qt::AlignLeft);
  result_summary_ = make_label(tr("Generate a model to see it here."), nullptr, side);
  result_summary_->setWordWrap(true);
  s->addWidget(result_summary_);
  material_ = make_label(QString(), "secondary", side);
  material_->setWordWrap(true);
  s->addWidget(material_);
  auto* details = new Section(tr("Measurements"), side, false);
  checks_ = make_label(tr("Validation pending. Generation success will not mark unsafe geometry PASS."), "secondary", side);
  checks_->setObjectName("image3dChecks");
  checks_->setWordWrap(true);
  checks_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  details->body()->addWidget(checks_);
  s->addWidget(details);
  apply_source_ = make_button(tr("Re-texture from the pictures"), "ghost", {}, side);
  apply_source_->setObjectName("image3dApplySource");
  apply_source_->setEnabled(false);
  apply_source_->setToolTip(tr("Textures this mesh again from the front picture and any other angles added on the Import "
                               "step. The geometry stays unchanged; surfaces no picture shows use TRELLIS's texture from "
                               "the same generation when it is in the results folder, otherwise reconstructed estimates."));
  s->addWidget(apply_source_);
  s->addStretch(1);
  folder_ = make_button(tr("Open Results Folder"), "ghost", icon(Icon::Folder), side);
  folder_->setEnabled(false);
  s->addWidget(folder_);
  export_ = make_button(tr("Continue to Export"), "primary", icon(Icon::Export, IconStyle::OnAccent), side);
  export_->setObjectName("image3dExport");
  export_->setEnabled(false);
  export_->setMinimumHeight(40);
  s->addWidget(export_);
  row->addWidget(side);

  connect(folder_, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(backend_.runDirectory())); });
  connect(export_, &QPushButton::clicked, this, &Image3DWorkspace::continueToExport);
  connect(apply_source_, &QPushButton::clicked, this, [this] {
    if (backend_.busy() || cutout_.isNull() || !backend_.asset().geometry) return;
    const int mode = appearance_mode_->currentIndex();
    if (mode != 1 && mode != 3) {
      status_->setText(tr("Select an Auto Texture mode first."));
      result_summary_->setText(status_->text());
      return;
    }
    const auto saved = QString::fromStdWString(backend_.asset().geometry_glb.wstring());
    applyAppearance();
    setBusy(true);
    if (!backend_.openSaved(saved, output_->text(), true)) setBusy(false);
  });
  return page;
}

void Image3DWorkspace::showStep(int step) {
  step = std::clamp(step, int(Import), int(Preview));
  pages_->setCurrentIndex(step);
  steps_->setCurrent(step);
  if (step == Generate && !gpu_checked_ && !gpu_job_.isRunning() && !backend_.busy()) {
    gpu_checked_ = true;
    checkGpu();
  }
}

int Image3DWorkspace::currentStep() const { return pages_->currentIndex(); }

void Image3DWorkspace::setAdvanced(bool on) {
  if (!advanced_box_) return;
  advanced_box_->setVisible(on);
}

bool Image3DWorkspace::hasGenerationInput() const {
  return appearance_mode_->currentIndex() == 2 ? !texture_session_.isEmpty() : !cutout_.isNull();
}

void Image3DWorkspace::refreshMemoryState() {
  constexpr quint64 required = p3dasset::generation_admission_ram();
  const auto free = p3dasset::available_ram();
  const bool ready = free >= required;
  const bool busy = property("image3dUiBusy").toBool();
  const bool input = hasGenerationInput();
  generate_->setEnabled(!busy && input && ready && generate_->property("cudaReady").toBool());
  const auto amount = QString::number(free / GiB, 'f', 2);
  memory_->setText(ready ? tr("Free RAM: %1 GiB\nGeneration requires 3.25 GiB free. Offline · CUDA 4GB · originals kept.").arg(amount)
                         : tr("Generation disabled: %1 GiB RAM available; at least 3.25 GiB required.\nYou can still import and "
                              "review images. Close other programs to free memory.").arg(amount));
  theme::set_prop(memory_, "tone", ready ? "" : "review");
  generate_->setToolTip(!ready ? tr("Generation is disabled until at least 3.25 GiB physical RAM is available.")
                        : !input ? tr("Import and review an isolated object first.")
                        : !generate_->property("cudaReady").toBool() ? tr("Check the GPU first.")
                                                                     : tr("Generate geometry with the guarded 4GB profile."));
}

void Image3DWorkspace::checkGpu() {
  if (gpu_job_.isRunning() || backend_.busy()) return;
  generate_->setProperty("cudaReady", false);
  generate_->setEnabled(false);
  gpu_check_->setEnabled(false);
  gpu_status_->setText(tr("Checking GPU availability; no inference…"));
  const auto setup = setup_->text();
  gpu_job_.setFuture(QtConcurrent::run([setup] {
    QJsonObject result;
    try {
      result = p3dasset::runCudaHealth(p3dasset::BackendConfig::read(setup).runtime_dir);
    } catch (const std::exception& e) {
      result = {{"ready", false}, {"message", QString::fromUtf8(e.what())}};
    }
    result["setup"] = setup;
    return result;
  }));
}

QString Image3DWorkspace::statusText() const { return status_->text(); }

QString Image3DWorkspace::projectName() const {
  if (project_) return project_->name;
  return image_path_.isEmpty() ? QString() : QFileInfo(image_path_).completeBaseName();
}

void Image3DWorkspace::setBusy(bool busy) {
  setProperty("image3dUiBusy", busy);
  if (busy) status_->setStyleSheet("font-size: 12pt;");
  for (auto* b : {import_, saved_, resume_, generate_}) b->setEnabled(!busy);
  refreshMemoryState();
  for (QWidget* w : std::initializer_list<QWidget*>{white_, seed_, capture_, setup_, output_, texture_session_button_, input_type_,
                                                    profile_, resolution_, texture_, appearance_mode_})
    w->setEnabled(!busy);
  gpu_check_->setEnabled(!busy && !gpu_job_.isRunning());
  apply_source_->setEnabled(!busy && !cutout_.isNull() && bool(backend_.asset().geometry));
  cancel_->setEnabled(backend_.busy());
  if (busy) export_->setEnabled(false);
  if (busy && backend_.busy()) {
    started_ms_ = QDateTime::currentMSecsSinceEpoch();
    elapsed_->setText(tr("Elapsed 0:00"));
    clock_->start();
  } else if (!busy) {
    clock_->stop();
  }
  emit busyChanged(busy, busy ? status_->text() : QString());
}

void Image3DWorkspace::importImage(const QString& file) {
  if (backend_.busy() || image_job_.isRunning() || scene_job_.isRunning()) return;
  if (project_ && QDir::cleanPath(file) != QDir::cleanPath(project_->sourcePath())) project_.reset();
  image_path_ = file;
  if (angles_) {  // other angles belong to a front picture: a new front starts without them (a project reloads its own)
    const QSignalBlocker block(angles_);
    angles_->clear();
  }
  cutout_ = QImage();
  appearance_source_ = QImage();
  original_ = QImage(file);
  steps_->setCompleted(Import, false);
  to_segment_->setEnabled(false);
  setBusy(true);
  status_->setText(tr("Preparing image preview…"));
  import_hint_->setText(status_->text());
  project_label_->setText(project_ ? project_->name : tr("Not saved yet"));
  const p3dasset::ImageInputOptions options{p3dasset::InputType(input_type_->currentIndex()), white_->isChecked()};
  image_job_.setFuture(QtConcurrent::run([file, options] { return p3dasset::prepare_image(file, options); }));
}

QImage Image3DWorkspace::generationCutout() const {
  if (segment_ && segment_->hasIncludedRegions() && !original_.isNull())
    return p3dseg::apply_cutout(original_, segment_->masks()->unionMask(true));
  return cutout_;
}

void Image3DWorkspace::applyAppearance() {
  const int mode = appearance_mode_->currentIndex();
  const QImage& cut = use_segmented_ ? seg_cutout_ : cutout_;
  const QImage& source = use_segmented_ ? seg_appearance_ : appearance_source_;
  backend_.setAppearance(mode == 3 ? source : cut, texture_->isChecked(),
                         mode == 3 ? 2048 : (profile_->currentIndex() == 0 ? 512 : 1024), input_type_->currentIndex() == 4);
  QVector<p3dasset::TrellisBackend::ExtraPicture> extra;
  if (mode == 1)
    for (const auto& it : angles_->items()) extra.push_back({it.cutout, it.label, QFileInfo(it.file).fileName()});
  backend_.setExtraPictures(extra);
}

void Image3DWorkspace::saveAngles() {
  if (!project_) return;
  try {
    QVector<ThreeReProject::View> views;
    auto items = angles_->items();
    bool moved = false;
    for (auto& it : items) {
      const QString rel = project_->importView(it.file);
      const QString inside = project_->dir + "/" + rel;
      if (QDir::cleanPath(inside) != QDir::cleanPath(it.file)) { it.file = inside; moved = true; }
      views.push_back({rel, it.label, file_sha256(inside)});
    }
    project_->views = views;
    project_->save();
    if (moved) {
      const QSignalBlocker block(angles_);
      angles_->setItems(items);
    }
  } catch (const std::exception& e) {
    status_->setText(QString::fromUtf8(e.what()));
  }
}

void Image3DWorkspace::loadAngles() {
  QVector<AnglesPanel::Item> items;
  if (project_)
    for (const auto& v : project_->views) {
      const QString file = project_->viewPath(v);
      if (!QFileInfo(file).isFile()) continue;
      const auto input = p3dasset::prepare_image(file, p3dasset::ImageInputOptions{p3dasset::InputType(input_type_->currentIndex()), white_->isChecked()});
      if (!input.cutout.isNull()) items.push_back({file, v.label, input.cutout, {}});
    }
  const QSignalBlocker block(angles_);
  angles_->setItems(items);
  if (!items.isEmpty()) angles_section_->setExpanded(true, false);
}

void Image3DWorkspace::lookAt(double yaw, double pitch, double x, double y, double z, double radius) {
  const auto mesh = backend_.asset().geometry;
  if (!mesh || mesh->positions.empty()) return;
  auto lo = mesh->positions.front(), hi = lo;
  for (auto p : mesh->positions) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  // The same display transform as makeScene.
  const auto center = (lo + hi) * .5;
  const double scale = 100 / std::max(1e-12, pixel3d::length(hi - lo));
  const auto extent = (hi - lo) * scale;
  const pixel3d::vec3d mid{(int(std::ceil(extent.x * 1.08)) + 2) * 0.5, (int(std::ceil(extent.y * 1.08)) + 2) * 0.5, (int(std::ceil(extent.z * 1.08)) + 2) * 0.5};
  pixel3d::vec3d c{x, y, z};
  if (radius <= 0) { c = center; radius = pixel3d::length(hi - lo) / 2; }
  const auto s = (c - center) * scale + mid;
  const float r = float(radius * scale);
  viewport_->setTurntable(false);
  auto& st = viewport_->state();
  st.preset.reset();
  st.yaw = float(yaw);
  st.pitch = float(pitch);
  viewport_->focusOn(Region3{QVector3D(float(s.x) - r, float(s.y) - r, float(s.z) - r), QVector3D(float(s.x) + r, float(s.y) + r, float(s.z) + r)}, false);
  // Close-ups: as close as the mouse wheel goes (focusOn stops at 4x, meant for framing issues).
  const auto& d = viewport_->scene().dims;
  const float model = 0.5f * QVector3D(float(std::max(d.W, 1)), float(std::max(d.H, 1)), float(std::max(d.D, 1))).length();
  st.zoom = std::clamp(model / std::max(r, 1.0f) * 0.5f, 1.0f, 40.0f);
  viewport_->refresh();
}

void Image3DWorkspace::ensureProject() {
  if (project_ || image_path_.isEmpty()) return;
  try {
    project_ = ThreeReProject::create(projects_dir_, QFileInfo(image_path_).completeBaseName(), image_path_);
    remember_recent(project_->projectFile());
    saveAngles();
    project_label_->setText(project_->name);
    project_label_->setToolTip(QDir::toNativeSeparators(project_->dir));
    if (output_->text() == auto_output_) {
      output_->setText(project_->runsDir());
      auto_output_ = output_->text();
    }
  } catch (const std::exception& e) {
    project_label_->setText(tr("Not saved (%1)").arg(QString::fromUtf8(e.what())));
  }
}

void Image3DWorkspace::loadSegmentation() {
  if (original_.isNull()) return;
  const QString sha = file_sha256(image_path_);
  segment_->setImage(original_, sha);
  if (!project_) return;
  QString error;
  if (auto masks = p3dseg::SegmentationStore::load(project_->segmentationDir(), sha, &error)) {
    seg_save_->stop();
    segment_->setMasks(std::move(masks));
    steps_->setCompleted(Segment, segment_->hasIncludedRegions());
  } else if (!error.isEmpty()) {
    segment_->setNotice(error);
  }
}

void Image3DWorkspace::saveSegmentation() {
  if (!segment_->masks() || original_.isNull()) return;
  if (segment_->masks()->regions().isEmpty() && !project_) return;  // nothing to keep yet
  ensureProject();
  if (!project_) return;
  try {
    p3dseg::SegmentationStore::save(project_->segmentationDir(), *segment_->masks(), segment_->sha256(), segment_->originalSize());
  } catch (const std::exception& e) {
    segment_->setNotice(tr("The regions could not be saved: %1").arg(QString::fromUtf8(e.what())));
  }
}

bool Image3DWorkspace::prepareSegmentedInput(QString* error) {
  use_segmented_ = segment_->hasIncludedRegions();
  if (!use_segmented_) return true;
  ensureProject();
  const QString file = (project_ ? project_->segmentationDir() : QDir::tempPath()) + "/cutout.png";
  QDir().mkpath(QFileInfo(file).absolutePath());
  if (!generationCutout().save(file, "PNG")) {
    *error = tr("Could not write the regions cutout.");
    return false;
  }
  // The same preparation as an imported picture: square framing, bounded size, crisp handling.
  const auto input = p3dasset::prepare_image(file, {p3dasset::InputType(input_type_->currentIndex()), true});
  if (!input.error.isEmpty()) {
    *error = tr("The included regions could not be prepared: %1").arg(input.error);
    return false;
  }
  seg_cutout_ = input.cutout;
  seg_appearance_ = input.appearance;
  return true;
}

void Image3DWorkspace::generate() {
  if (backend_.busy() || image_job_.isRunning() || scene_job_.isRunning()) return;
  // SAM holds about 1 GB of RAM: release it so it never counts against the generation admission.
  segment_->releaseEngine();
  if (p3dasset::available_ram() < p3dasset::generation_admission_ram()) {
    refreshMemoryState();
    status_->setText(tr("Generation disabled: at least 3.25 GiB free physical RAM is required. Image preview remains available."));
    return;
  }
  if (!generate_->property("cudaReady").toBool()) {
    status_->setText(tr("Generation disabled: check GPU readiness first."));
    return;
  }
  try {
    auto config = p3dasset::BackendConfig::read(setup_->text());
    if (appearance_mode_->currentIndex() != 2) ensureProject();
    if (!QDir(output_->text()).exists() && !QDir().mkpath(output_->text())) throw std::runtime_error("Could not create results location");
    setBusy(true);
    log_->clear();
    checks_->setText(tr("Validation pending"));
    if (appearance_mode_->currentIndex() == 2) {
      if (texture_session_.isEmpty()) throw std::runtime_error("Choose a validated saved texture session; a GLB alone lacks shape SLAT and conditioning.");
      if (backend_.generateTexture(config, texture_session_, output_->text())) cancel_->setEnabled(true);
      else setBusy(false);
      return;
    }
    if (profile_->currentIndex() > 1 || resolution_->currentIndex() != 0) throw std::runtime_error("The selected profile is unsupported by the pinned 4GB backend");
    QString prepError;
    if (!prepareSegmentedInput(&prepError)) throw std::runtime_error(prepError.toStdString());
    applyAppearance();
    const QImage input = use_segmented_ ? seg_cutout_ : cutout_;
    const QString source = project_ ? project_->sourcePath() : image_path_;
    if (use_segmented_) log_->appendPlainText(tr("Using %1 included region(s) as the generation input.").arg(
        std::count_if(segment_->masks()->regions().begin(), segment_->masks()->regions().end(), [](const p3dseg::Region& r) { return r.include; })));
    if (backend_.generate(config, input, source, output_->text(), quint32(seed_->value()), capture_->isChecked())) {
      QSettings().setValue("trellis/setup", setup_->text());
      cancel_->setEnabled(true);
      setBusy(true);
    } else {
      setBusy(false);
    }
  } catch (const std::exception& e) {
    status_->setText(QString::fromUtf8(e.what()));
    setBusy(false);
  }
}

void Image3DWorkspace::cancel() { backend_.cancel(); }

void Image3DWorkspace::setTextureSession(const QString& file) {
  if (property("image3dUiBusy").toBool()) return;
  texture_session_ = file;
  appearance_mode_->setCurrentIndex(2);
  status_->setText(tr("Saved texture session selected. Experimental PBR: previous visual-quality tests failed. Geometry will not "
                      "be regenerated. Check GPU, then generate."));
  refreshMemoryState();
}

void Image3DWorkspace::openSaved(const QString& file) {
  if (backend_.busy() || image_job_.isRunning() || scene_job_.isRunning()) return;
  QDir().mkpath(output_->text());
  setBusy(true);
  log_->clear();
  checks_->setText(tr("Validation pending"));
  applyAppearance();
  if (!backend_.openSaved(file, output_->text())) setBusy(false);
  else cancel_->setEnabled(true);
}

void Image3DWorkspace::openProject(const QString& projectFile) {
  try {
    auto p = ThreeReProject::open(projectFile);
    if (!QFileInfo(p.sourcePath()).isFile()) throw std::runtime_error(tr("The project's picture is missing: %1").arg(QDir::toNativeSeparators(p.sourcePath())).toStdString());
    project_ = p;
    remember_recent(p.projectFile());
    if (output_->text() == auto_output_) {
      output_->setText(p.runsDir());
      auto_output_ = output_->text();
    }
    importImage(p.sourcePath());
    loadAngles();
    if (!p.result_glb.isEmpty() && QFileInfo(QDir(p.dir).filePath(p.result_glb)).isFile()) pending_result_ = QDir(p.dir).filePath(p.result_glb);
    project_label_->setText(p.name);
    project_label_->setToolTip(QDir::toNativeSeparators(p.dir));
    if (!p.sourceIntact())
      status_->setText(tr("The project's picture changed since it was saved. Its regions are kept on disk but not applied."));
    showStep(Import);
  } catch (const std::exception& e) {
    QMessageBox::warning(this, tr("Open project"), QString::fromUtf8(e.what()));
  }
}

bool Image3DWorkspace::hasResult() const { return bool(backend_.asset().geometry); }

void Image3DWorkspace::recordResult() {
  if (!project_ || backend_.runDirectory().isEmpty()) return;
  const QDir dir(project_->dir);
  const QString run = dir.relativeFilePath(backend_.runDirectory());
  if (run.startsWith("..")) return;  // results kept outside the project (Advanced results folder)
  if (!project_->runs.contains(run)) project_->runs << run;
  const auto& a = backend_.asset();
  const QString glb = QString::fromStdWString(a.geometry_glb.wstring());
  if (!glb.isEmpty()) project_->result_glb = dir.relativeFilePath(glb);
  try {
    project_->save();
  } catch (const std::exception& e) {
    material_->setText(material_->text() + "\n" + QString::fromUtf8(e.what()));
  }
}

void Image3DWorkspace::showResult(bool available, const QString& message) {
  status_->setText(message);
  folder_->setEnabled(!backend_.runDirectory().isEmpty());
  if (!available) {
    status_->setStyleSheet("font-size: 12pt; color: " + theme::colors().fail.name() + ";");
    setBusy(false);
    export_->setEnabled(false);
    return;
  }
  const auto& assetInfo = backend_.asset();
  const auto kind = assetInfo.materials.value("kind", std::string("unknown"));
  const auto materialLabel =
      assetInfo.geometry_only ? tr("Geometry only · no texture available")
      : kind == "trellis_generated" ? tr("Experimental TRELLIS PBR · prior color/detail quality FAIL · hidden surfaces are inferred · preview: base color only; export retains PBR maps")
      : kind == "auto_texture" ? autoTextureSummary(!backend_.report().value("auto_texture").toObject().isEmpty() ? backend_.report()
                                     : QJsonObject{{"auto_texture", savedTextureReport(QString::fromStdWString(assetInfo.geometry_glb.wstring()))}})
      : kind == "source_projected" ? tr("Approximate front-image projection · back and sides are not reconstructed")
                                   : tr("Embedded material retained · origin unknown · preview: base color only");
  checks_->setText(validationSummary(backend_.report()));
  material_->setText(materialLabel);
  if (backend_.report().value("texture_only").toBool()) {
    const auto r = backend_.report();
    const auto memory = r.value("memory").toObject();
    checks_->setText(checks_->text() + tr("\n2048 Base Color + Metallic-Roughness: %1 · zero-area UV triangles: %2\nTexture inference: %3 s · bake: %4 s · GPU sampled: %5 MiB · RAM sampled: %6 MiB")
                                           .arg(r.value("pbr_maps_valid").toBool() ? "PASS" : "FAIL").arg(r.value("texture_validation").toObject().value("uv_zero_area_triangles").toInt(-1))
                                           .arg(r.value("phases").toObject().value("texture_inference").toDouble()).arg(r.value("phases").toObject().value("texture_bake").toDouble())
                                           .arg(memory.value("gpu_measured").toBool() ? QString::number(memory.value("sampled_peak_pid_dedicated_bytes").toDouble() / 1048576., 'f', 1) : tr("unmeasured"))
                                           .arg(memory.value("sampled_peak_rss_bytes").toDouble() / 1048576., 0, 'f', 1));
  }
  if (backend_.report().contains("material_error")) material_->setText(material_->text() + "\n" + backend_.report().value("material_error").toString());
  const auto validation = backend_.asset().validation;
  const auto tone = validation == p3dasset::Validation::Pass ? theme::Tone::Pass : validation == p3dasset::Validation::Unsafe ? theme::Tone::Fail : theme::Tone::Review;
  status_->setStyleSheet("font-size: 12pt; color: " + theme::tone_color(tone).name() + ";");
  set_chip(result_chip_, QString::fromUtf8(p3dasset::validation_name(validation)).toUpper(), tone);
  result_summary_->setText(plainVerdict(validation));
  export_->setText(validation == p3dasset::Validation::Pass ? tr("Continue to Export") : tr("Continue to Export (with issues)"));
  recordResult();
  {
    // A saved result (an opened project): the statuses of the pass that made it, from its texture report.
    const auto a = !backend_.report().value("auto_texture").toObject().isEmpty() ? backend_.report().value("auto_texture").toObject()
                                                               : savedTextureReport(QString::fromStdWString(backend_.asset().geometry_glb.wstring()));
    angles_->setResults(a.value("views").toArray(), a.value("picture_ownership").toObject());
  }
  // A saved result opened on its own has no picture: only the steps that really happened are done.
  const bool picture = !cutout_.isNull();
  steps_->setCompleted(Import, picture);
  steps_->setCompleted(Segment, picture);
  steps_->setCompleted(Generate, true);
  steps_->setCompleted(Preview, true);
  showStep(Preview);
  const auto asset = backend_.asset();
  scene_job_.setFuture(QtConcurrent::run([asset, crisp = crisp_] { return makeScene(asset, crisp); }));
  emit resultReady();
}

Scene Image3DWorkspace::makeScene(const p3dasset::MeshAsset& asset, bool crisp) {
  const auto mesh = asset.geometry;
  auto lo = mesh->positions.front(), hi = lo;
  for (auto p : mesh->positions) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  const auto center = (lo + hi) * .5;
  const double scale = 100 / std::max(1e-12, pixel3d::length(hi - lo));
  // The scene box is the model's own box (with a margin), so the camera frames the model.
  const auto extent = (hi - lo) * scale;
  const int W = int(std::ceil(extent.x * 1.08)) + 2, H = int(std::ceil(extent.y * 1.08)) + 2, D = int(std::ceil(extent.z * 1.08)) + 2;
  const pixel3d::vec3d mid{W * 0.5, H * 0.5, D * 0.5};
  auto display = std::make_shared<pixel3d::Mesh>(*mesh);
  for (auto& p : display->positions) p = (p - center) * scale + mid;
  Scene scene;
  scene.dims = {W, H, D};
  scene.mesh = flat_mesh(*display);
  scene.solid = display;
  scene.revision = 1;
  if (asset.appearance) {
    auto colored = std::make_shared<pixel3d::TexturedMesh>(*asset.appearance);
    for (auto& p : colored->positions) p = (p - center) * scale + mid;
    scene.mesh = colored;
    scene.textured = true;
    scene.smooth_texture = !crisp;
  }
  scene.tri_part.assign(display->tris.size(), -1);
  scene.tri_region.assign(display->tris.size(), -1);
  scene.tri_flags.assign(display->tris.size(), 0);
  return scene;
}

QString Image3DWorkspace::droppedImage(const QMimeData* mime) {
  if (!mime || !mime->hasUrls() || mime->urls().size() != 1) return {};
  const QString f = mime->urls().front().toLocalFile();
  static const QStringList ext{"png", "jpg", "jpeg", "webp", "bmp"};
  return ext.contains(QFileInfo(f).suffix().toLower()) && QFileInfo(f).isFile() ? f : QString();
}

void Image3DWorkspace::dragEnterEvent(QDragEnterEvent* e) {
  if (!droppedImage(e->mimeData()).isEmpty() && !backend_.busy()) e->acceptProposedAction();
  else e->ignore();
}

void Image3DWorkspace::dropEvent(QDropEvent* e) {
  const QString f = droppedImage(e->mimeData());
  if (f.isEmpty() || backend_.busy()) return;
  e->acceptProposedAction();
  showStep(Import);
  importImage(f);
}

bool Image3DWorkspace::confirmClose() {
  if (!backend_.busy()) return true;
  const auto answer = QMessageBox::question(this, tr("Generation is running"), tr("A generation is still running. Stop it and close 3re?"),
                                            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  if (answer != QMessageBox::Yes) return false;
  backend_.cancel();
  return true;
}

}  // namespace p3dgui
