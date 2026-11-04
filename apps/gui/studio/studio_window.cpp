#include "studio/studio_window.hpp"

#include <QButtonGroup>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "app_controller.hpp"
#include "icons.hpp"
#include "pixel3d/common/version.hpp"
#include "shell/coming_soon.hpp"
#include "shell/nav_rail.hpp"
#include "shell/spinner.hpp"
#include "studio/agent_page.hpp"
#include "studio/export_page.hpp"
#include "studio/home_page.hpp"
#include "studio/image3d_workspace.hpp"
#include "studio/retopo_workspace.hpp"
#include "studio/rigging_page.hpp"
#include "studio/settings_page.hpp"
#include "widgets.hpp"

namespace p3dgui {

StudioWindow::StudioWindow(const WindowOptions& opt, QWidget* parent) : QMainWindow(parent) {
  setObjectName("StudioWindow");
  setWindowTitle(QStringLiteral("3re"));
  resize(1440, 900);
  setMinimumSize(1100, 700);

  auto* central = new QWidget(this);
  central->setProperty("surface", "app");
  central->setAttribute(Qt::WA_StyledBackground);
  auto* row = new QHBoxLayout(central);
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(0);
  rail_ = new NavRail(central);
  row->addWidget(rail_);

  auto* right = new QVBoxLayout;
  right->setContentsMargins(0, 0, 0, 0);
  right->setSpacing(0);
  auto* header = new QWidget(central);
  header->setObjectName("StudioHeader");
  header->setProperty("surface", "header");
  header->setAttribute(Qt::WA_StyledBackground);
  header->setFixedHeight(64);
  auto* h = new QHBoxLayout(header);
  h->setContentsMargins(theme::space::xl, 0, theme::space::xl, 0);
  h->setSpacing(theme::space::md);
  auto* titles = new QVBoxLayout;
  titles->setSpacing(0);
  titles->addStretch(1);
  title_ = make_label(QString(), "title", header);
  title_->setObjectName("studioTitle");
  subtitle_ = make_label(QString(), "secondary", header);
  titles->addWidget(title_);
  titles->addWidget(subtitle_);
  titles->addStretch(1);
  h->addLayout(titles, 1);
  spinner_ = new Spinner(header, 18);
  busy_text_ = make_label(QString(), "secondary", header);
  busy_text_->setObjectName("studioBusy");
  h->addWidget(spinner_);
  h->addWidget(busy_text_);
  const auto v = pixel3d::version();
  auto* version = make_chip(QStringLiteral("3re %1").arg(QString::fromUtf8(v.data(), qsizetype(v.size()))), theme::Tone::Neutral, header);
  version->setToolTip(tr("Offline · local GPU · originals are never modified"));
  h->addWidget(version, 0, Qt::AlignVCenter);
  right->addWidget(header);

  stack_ = new QStackedWidget(central);
  right->addWidget(stack_, 1);
  row->addLayout(right, 1);
  setCentralWidget(central);

  // Pixel Art Builder: the existing editor window, embedded.
  pixel_ = new MainWindow(opt, nullptr);
  pixel_->setWindowFlags(Qt::Widget);
  ai_ = new Image3DWorkspace(opt.software_view, nullptr);

  // 2.10.0: Home is the first page and the page 3re opens on; the Agent directly under it.
  home_ = new HomePage(this);
  addPage("home", tr("Home"), icon(Icon::Home), tr("Home"), tr("Start a new model or continue where you left off"), home_);
  agent_ = new AgentPage(this);
  addPage("agent", tr("Agent"), icon(Icon::Validate), tr("Asset Agent"),
          tr("Tell the agent what you need, or watch a run: every stage, tool, decision and candidate; answer its questions"), agent_);
  addPage("image", tr("Image\nto 3D"), icon(Icon::ImageTo3D), tr("Image to 3D"),
          tr("One picture of an object or character becomes a 3D model"), buildImagePage());
  retopo_ = new RetopoWorkspace(opt.software_view, this);
  retopo_->setGenerator(ai_);
  addPage("retopo", tr("Retopology"), icon(Icon::Retopo), tr("Smart Retopology"),
          tr("Clean quads from a generated model · compare, edit and save (Experimental)"), retopo_);
  export_ = new ExportPage(this);
  addPage("export", tr("Export"), icon(Icon::Export), tr("Export"), tr("Save your model for a game engine or another tool"), export_);
  rigging_ = new RiggingPage(*pixel_, this);
  addPage("rigging", tr("Rigging"), icon(Icon::Bone), tr("Rigging"), tr("Skeletons for posing and animation"), rigging_);
  addPage("animation", tr("Animation"), icon(Icon::Animation), tr("Animation"), tr("Make your model move"),
          new ComingSoonPanel(tr("Animation"),
                              tr("Idle, walk and attack clips for rigged characters, previewed here and exported with the model."),
                              {tr("A rigged model (Rigging)"), tr("Clip editing and preview"), tr("Animation export in GLB")}));
  rail_->addStretch();
  settings_ = new SettingsPage(this);
  addPage("settings", tr("Settings"), icon(Icon::Settings), tr("Settings"), tr("Generation, display and information about 3re"), settings_);

  connect(rail_, &NavRail::selected, this, [this](const QString& id) { showPage(id); });
  connect(home_, &HomePage::startImage, this, [this] { showPage("image"); setImageMode("ai"); });
  connect(home_, &HomePage::startPixel, this, [this] { showPage("image"); setImageMode("pixel"); });
  connect(home_, &HomePage::openPixelProject, this, [this](const QString& file) {
    showPage("image");
    setImageMode("pixel");
    pixel_->openProjectFile(file);
  });
  connect(home_, &HomePage::openImageProject, this, [this](const QString& file) {
    showPage("image");
    setImageMode("ai");
    ai_->openProject(file);
  });
  connect(pixel_, &MainWindow::imageTo3DRequested, this, [this] { showPage("image"); setImageMode("ai"); });
  connect(pixel_, &QWidget::windowTitleChanged, this, [this] { updateHeader(); });
  connect(ai_, &Image3DWorkspace::busyChanged, this, [this](bool busy, const QString& text) { setBusy(busy, text); });
  connect(ai_, &Image3DWorkspace::resultReady, this, [this] { export_->refresh(); retopo_->refresh(); });
  connect(retopo_, &RetopoWorkspace::busyChanged, this, [this](bool busy, const QString& text) { setBusy(busy, text); });
  connect(agent_, &AgentPage::busyChanged, this, [this](bool busy, const QString& text) { setBusy(busy, text); });
  connect(ai_, &Image3DWorkspace::continueToExport, this, [this] { showPage("export"); });
  connect(export_, &ExportPage::goToImage, this, [this] { showPage("image"); });
  export_->setSources(ai_, pixel_);
  connect(settings_, &SettingsPage::advancedChanged, ai_, &Image3DWorkspace::setAdvanced);
  connect(settings_, &SettingsPage::advancedChanged, retopo_, &RetopoWorkspace::setAdvanced);
  connect(ai_, &Image3DWorkspace::segmentationMetrics, settings_, &SettingsPage::setDiagnostics);
  ai_->setAdvanced(QSettings().value("ui/advanced", false).toBool());
  retopo_->setAdvanced(QSettings().value("ui/advanced", false).toBool());

  for (int i = 0; i < ids_.size(); ++i) {
    auto* sc = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key(Qt::Key_1 + i)), this);
    sc->setContext(Qt::WindowShortcut);
    connect(sc, &QShortcut::activated, this, [this, i] { showPage(ids_[i]); });
  }
  showPage("home");
}

StudioWindow::~StudioWindow() = default;

void StudioWindow::addPage(const QString& id, const QString& label, const QIcon& icon_, const QString& title, const QString& subtitle,
                           QWidget* w) {
  ids_ << id;
  pages_.insert(id, w);
  headings_.insert(id, {title, subtitle});
  w->setObjectName("page_" + id);
  stack_->addWidget(w);
  rail_->addItem(icon_, label, id);
  rail_->button(id)->setToolTip(QStringLiteral("%1 (Ctrl+%2)").arg(title).arg(ids_.size()));
  rail_->button(id)->setAccessibleName(title);
}

QWidget* StudioWindow::buildImagePage() {
  auto* page = new QWidget(this);
  auto* col = new QVBoxLayout(page);
  col->setContentsMargins(0, 0, 0, 0);
  col->setSpacing(0);
  auto* bar = new QWidget(page);
  bar->setProperty("surface", "app");
  bar->setAttribute(Qt::WA_StyledBackground);
  auto* b = new QHBoxLayout(bar);
  b->setContentsMargins(theme::space::xl, theme::space::sm, theme::space::xl, theme::space::sm);
  b->setSpacing(theme::space::xs);
  auto* group = new QButtonGroup(bar);
  mode_ai_ = make_button(tr("AI Generation"), "segment", icon(Icon::ImageTo3D), bar);
  mode_ai_->setObjectName("imageModeAi");
  mode_ai_->setToolTip(tr("Photos, illustrations, characters and AI art: TRELLIS.2 builds a full 3D model on your GPU"));
  mode_pixel_ = make_button(tr("Pixel Art Builder"), "segment", icon(Icon::PixelGrid), bar);
  mode_pixel_->setObjectName("imageModePixel");
  mode_pixel_->setToolTip(tr("Pixel-art drawings (Front, optional Side and Top) become a low-poly or solid model you can edit"));
  for (auto* m : {mode_ai_, mode_pixel_}) {
    m->setCheckable(true);
    group->addButton(m);
    b->addWidget(m);
  }
  b->addStretch(1);
  col->addWidget(bar);
  image_stack_ = new QStackedWidget(page);
  image_stack_->addWidget(ai_);
  image_stack_->addWidget(pixel_);
  col->addWidget(image_stack_, 1);
  connect(mode_ai_, &QPushButton::clicked, this, [this] { setImageMode("ai"); });
  connect(mode_pixel_, &QPushButton::clicked, this, [this] { setImageMode("pixel"); });
  mode_ai_->setChecked(true);
  return page;
}

void StudioWindow::setImageMode(const QString& mode) {
  image_mode_ = mode == "pixel" ? "pixel" : "ai";
  const bool pixel = image_mode_ == "pixel";
  image_stack_->setCurrentWidget(pixel ? static_cast<QWidget*>(pixel_) : static_cast<QWidget*>(ai_));
  (pixel ? mode_pixel_ : mode_ai_)->setChecked(true);
  updateHeader();
}

void StudioWindow::showPage(const QString& id) {
  QWidget* w = pages_.value(id);
  if (!w) return;
  current_ = id;
  stack_->setCurrentWidget(w);
  rail_->setCurrent(id);
  if (id == "rigging") rigging_->refresh();
  if (id == "export") export_->refresh();
  if (id == "home") home_->refresh();
  if (id == "retopo") retopo_->refresh();
  updateHeader();
}

void StudioWindow::updateHeader() {
  const auto heading = headings_.value(current_);
  QString title = heading.first, subtitle = heading.second;
  if (current_ == "image" && image_mode_ == "pixel") {
    title = tr("Pixel Art Builder");
    subtitle = tr("Drawings to a validated low-poly or solid model");
  }
  title_->setText(title);
  subtitle_->setText(subtitle);
  QString project;
  if (current_ == "image") project = image_mode_ == "pixel" ? pixel_->windowTitle().section(QStringLiteral(" — "), 0, 0) : ai_->projectName();
  setWindowTitle(project.isEmpty() ? QStringLiteral("3re") : QStringLiteral("%1 — 3re").arg(project));
}

void StudioWindow::setBusy(bool busy, const QString& text) {
  if (busy) spinner_->start();
  else spinner_->stop();
  busy_text_->setText(busy ? text : QString());
}

void StudioWindow::closeEvent(QCloseEvent* e) {
  if (!ai_->confirmClose() || !pixel_->close()) {
    e->ignore();
    return;
  }
  e->accept();
}

}  // namespace p3dgui
