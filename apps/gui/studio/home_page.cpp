#include "studio/home_page.hpp"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include "backends/trellis_backend.hpp"
#include "backends/trellis_diagnostics.hpp"
#include "icons.hpp"
#include "project/project_store.hpp"
#include "widgets.hpp"

namespace p3dgui {
namespace {

QFrame* start_card(QWidget* parent, Icon ic, const QString& title, const QString& text, const QString& cta, const char* name,
                   QPushButton** out) {
  auto* c = new QFrame(parent);
  c->setProperty("homeCard", true);
  c->setMinimumHeight(200);
  auto* l = new QVBoxLayout(c);
  l->setContentsMargins(24, 22, 24, 22);
  l->setSpacing(10);
  auto* glyph = new QLabel(c);
  glyph->setPixmap(icon(ic, IconStyle::Default).pixmap(QSize(40, 40), c->devicePixelRatioF()));
  l->addWidget(glyph);
  l->addWidget(make_label(title, "title", c));
  auto* body = make_label(text, "secondary", c);
  body->setWordWrap(true);
  l->addWidget(body, 1);
  auto* b = make_button(cta, "primary", icon(Icon::ChevronRight, IconStyle::OnAccent), c);
  b->setObjectName(name);
  b->setMinimumHeight(38);
  b->setLayoutDirection(Qt::RightToLeft);
  l->addWidget(b, 0, Qt::AlignLeft);
  *out = b;
  return c;
}

}  // namespace

HomePage::HomePage(QWidget* parent) : QWidget(parent) {
  auto* inner = new QWidget;
  auto* col = new QVBoxLayout(inner);
  col->setContentsMargins(48, 36, 48, 36);
  col->setSpacing(theme::space::lg);

  // The symbol (128 px source, scaled smoothly to the screen's pixel ratio) beside the welcome text.
  auto* head_row = new QHBoxLayout;
  head_row->setSpacing(theme::space::lg);
  auto* mark = new QLabel(inner);
  mark->setPixmap(QPixmap(QStringLiteral(":/3re-mark.png")));
  mark->setScaledContents(true);
  mark->setFixedSize(72, 72);
  mark->setAccessibleName(tr("3re logo"));
  head_row->addWidget(mark, 0, Qt::AlignTop);
  auto* head = new QVBoxLayout;
  head->setSpacing(theme::space::lg);
  head->addWidget(make_label(tr("Welcome to 3re"), "hero", inner));
  auto* lead = make_label(tr("Turn pictures and pixel art into clean, checked 3D models — offline, on your own computer."), "lead", inner);
  lead->setWordWrap(true);
  head->addWidget(lead);
  head_row->addLayout(head, 1);
  col->addLayout(head_row);
  col->addSpacing(theme::space::sm);

  auto* cards = new QHBoxLayout;
  cards->setSpacing(theme::space::lg);
  QPushButton *image = nullptr, *pixel = nullptr;
  cards->addWidget(start_card(inner, Icon::ImageTo3D, tr("Image to 3D"),
                              tr("A photo, illustration, character or AI picture becomes a full 3D model. Mark the regions to "
                                 "keep, generate on your GPU, then check and export."),
                              tr("Start with a picture"), "homeStartImage", &image));
  cards->addWidget(start_card(inner, Icon::Reconstruct, tr("Pixel Art to 3D"),
                              tr("Pixel-art drawings — a Front view, and a Side view if you have one — become a low-poly or "
                                 "solid model you can edit, part by part."),
                              tr("Start with drawings"), "homeStartPixel", &pixel));
  col->addLayout(cards);
  connect(image, &QPushButton::clicked, this, &HomePage::startImage);
  connect(pixel, &QPushButton::clicked, this, &HomePage::startPixel);

  auto* lower = new QHBoxLayout;
  lower->setSpacing(theme::space::lg);

  auto* recent = new Card(inner);
  auto* rl = new QVBoxLayout(recent);
  rl->setContentsMargins(20, 18, 20, 18);
  rl->setSpacing(6);
  auto* rh = new QHBoxLayout;
  rh->addWidget(make_label(tr("Recent projects"), "section", recent));
  rh->addStretch(1);
  auto* open = make_button(tr("Open…"), "ghost", icon(Icon::Folder), recent);
  open->setObjectName("homeOpen");
  open->setToolTip(tr("Open a 3re project (project.3re) or a pixel project (.p3d)"));
  rh->addWidget(open);
  rl->addLayout(rh);
  recent_list_ = new QWidget(recent);
  recent_layout_ = new QVBoxLayout(recent_list_);
  recent_layout_->setContentsMargins(0, 0, 0, 0);
  recent_layout_->setSpacing(2);
  rl->addWidget(recent_list_);
  lower->addWidget(recent, 3, Qt::AlignTop);
  connect(open, &QPushButton::clicked, this, [this] {
    const auto f = QFileDialog::getOpenFileName(this, tr("Open project"), default_projects_dir(),
                                                tr("3re projects (project.3re *.p3d);;Image-to-3D projects (project.3re);;Pixel projects (*.p3d)"));
    if (f.endsWith(".p3d", Qt::CaseInsensitive)) emit openPixelProject(f);
    else if (!f.isEmpty()) emit openImageProject(f);
  });

  auto* ready = new Card(inner);
  auto* sl = new QVBoxLayout(ready);
  sl->setContentsMargins(20, 18, 20, 18);
  sl->setSpacing(8);
  sl->addWidget(make_label(tr("This computer"), "section", ready));
  auto row = [&](const QString& name, QLabel** chip, QLabel** detail) {
    auto* r = new QHBoxLayout;
    r->addWidget(make_label(name, nullptr, ready), 1);
    *chip = make_chip(tr("…"), theme::Tone::Neutral, ready);
    r->addWidget(*chip);
    sl->addLayout(r);
    *detail = make_label(QString(), "secondary", ready);
    (*detail)->setWordWrap(true);
    sl->addWidget(*detail);
  };
  row(tr("Generation models"), &models_chip_, &models_detail_);
  row(tr("Graphics card (CUDA)"), &gpu_chip_, &gpu_detail_);
  row(tr("Free memory"), &ram_chip_, &ram_detail_);
  row(tr("Segmentation (CPU)"), &sam_chip_, &sam_detail_);
  gpu_check_ = make_button(tr("Check Graphics Card"), "ghost", icon(Icon::Validate), ready);
  gpu_check_->setObjectName("homeCheckGpu");
  gpu_check_->setToolTip(tr("A short CUDA probe; nothing is generated."));
  sl->addWidget(gpu_check_, 0, Qt::AlignLeft);
  lower->addWidget(ready, 2, Qt::AlignTop);
  col->addLayout(lower);
  col->addStretch(1);

  auto* scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidget(inner);
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->addWidget(scroll);

  gpu_job_ = new QFutureWatcher<QJsonObject>(this);
  connect(gpu_check_, &QPushButton::clicked, this, &HomePage::checkGpu);
  connect(gpu_job_, &QFutureWatcher<QJsonObject>::finished, this, [this] {
    const auto h = gpu_job_->result();
    const bool ok = h.value("ready").toBool();
    set_chip(gpu_chip_, ok ? tr("Ready") : tr("Not available"), ok ? theme::Tone::Pass : theme::Tone::Review);
    gpu_detail_->setText(ok ? h.value("message").toString()
                            : h.value("message").toString() + "\n" + tr("Image to 3D generation needs an NVIDIA GPU with 4 GB. Everything else still works."));
    gpu_check_->setEnabled(true);
  });
  refresh();
}

void HomePage::checkGpu() {
  if (gpu_job_->isRunning()) return;
  gpu_check_->setEnabled(false);
  set_chip(gpu_chip_, tr("Checking…"), theme::Tone::Neutral);
  const QString setup = p3dasset::find_backend_config();
  gpu_job_->setFuture(QtConcurrent::run([setup] {
    try {
      return p3dasset::runCudaHealth(p3dasset::BackendConfig::read(setup).runtime_dir);
    } catch (const std::exception& e) {
      return QJsonObject{{"ready", false}, {"message", QString::fromUtf8(e.what())}};
    }
  }));
}

void HomePage::refresh() {
  // Recent projects.
  while (auto* item = recent_layout_->takeAt(0)) {
    delete item->widget();
    delete item;
  }
  const QStringList recent = recent_projects();
  if (recent.isEmpty()) {
    auto* empty = make_label(tr("No projects yet. Your Image-to-3D projects appear here, with their picture, regions and results."),
                             "secondary", recent_list_);
    empty->setWordWrap(true);
    recent_layout_->addWidget(empty);
  }
  for (const QString& f : recent) {
    QString title = QFileInfo(f).dir().dirName();
    try {
      title = ThreeReProject::open(f).name;
    } catch (...) {
    }
    auto* b = make_button(title, "row", icon(Icon::ImageTo3D), recent_list_);
    b->setToolTip(QDir::toNativeSeparators(QFileInfo(f).absolutePath()));
    connect(b, &QPushButton::clicked, this, [this, f] { emit openImageProject(f); });
    recent_layout_->addWidget(b);
  }

  // Readiness (instant checks; the GPU probe runs on request).
  const QString setup = p3dasset::find_backend_config();
  bool models = false;
  QString why;
  try {
    const auto c = p3dasset::BackendConfig::read(setup);
    models = QFileInfo(c.engine()).isFile() && QFileInfo(c.weights_manifest).isFile();
    for (const char* n : {"dinov3.gguf", "ss_flow.gguf", "ss_dec.gguf", "shape_flow_512.gguf", "shape_dec.gguf"})
      if (!QFileInfo(QDir(c.models_dir).filePath(n)).isFile()) {
        models = false;
        why = tr("Missing %1 in %2").arg(n, QDir::toNativeSeparators(c.models_dir));
      }
  } catch (const std::exception& e) {
    why = setup.isEmpty() ? tr("No generation setup found next to 3re (app\\3re.config.json).") : QString::fromUtf8(e.what());
  }
  set_chip(models_chip_, models ? tr("Installed") : tr("Missing"), models ? theme::Tone::Pass : theme::Tone::Fail);
  models_detail_->setText(models ? tr("TRELLIS.2 Q4 · 512 geometry · verified when you generate") : why);
  const double free = p3dasset::available_ram() / (1024.0 * 1024 * 1024);
  const bool enough = p3dasset::available_ram() >= p3dasset::generation_admission_ram();
  set_chip(ram_chip_, QStringLiteral("%1 GiB").arg(free, 0, 'f', 1), enough ? theme::Tone::Pass : theme::Tone::Review);
  ram_detail_->setText(enough ? tr("Enough for generation (3.25 GiB needed)") : tr("Generation needs 3.25 GiB free: close other programs first."));
  bool sam = false;
  if (!setup.isEmpty()) {
    try {
      const auto s = p3dasset::SamConfig::read(setup);
      sam = QFileInfo(QDir(s.dir).filePath("sam2.1_hiera_tiny.encoder.onnx")).isFile() && QFileInfo(s.onnxruntime).isFile();
    } catch (...) {
    }
  }
  set_chip(sam_chip_, sam ? tr("Installed") : tr("Colour regions only"), sam ? theme::Tone::Pass : theme::Tone::Review);
  sam_detail_->setText(sam ? tr("SAM 2.1-tiny on the CPU · no graphics memory used") : tr("Click-to-select uses colour regions without SAM."));
  if (gpu_chip_->text() == tr("…")) {
    set_chip(gpu_chip_, tr("Not checked"), theme::Tone::Neutral);
    gpu_detail_->setText(tr("Check once to know whether this computer can generate."));
  }
}

}  // namespace p3dgui
