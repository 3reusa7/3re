#include "main_window.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

#include "dialogs.hpp"
#include "history_panel.hpp"
#include "icons.hpp"
#include "inspector.hpp"
#include "log_panel.hpp"
#include "pixel3d/common/version.hpp"
#include "pixel3d/edit/brush.hpp"
#include "pixel3d/polygon/contour.hpp"
#include "qt_util.hpp"
#ifdef PIXEL3D_ENABLE_SMARTQUAD
#include "smart_quad_dialog.hpp"
#endif
#include "start_screen.hpp"
#include "theme.hpp"
#include "viewport.hpp"
#include "views_panel.hpp"
#include "widgets.hpp"

namespace p3dgui {

using namespace pixel3d;
using nlohmann::json;
using theme::Tone;

namespace {

constexpr int kLayoutVersion = 5;  // bump when docks or toolbars change

// Toolbar actions and their icons (primary buttons swap to the white variant).
const std::pair<const char*, Icon> kToolbarIcons[] = {
    {"import", Icon::Import},         {"run_all", Icon::Reconstruct}, {"tool_select", Icon::Select},
    {"tool_depth", Icon::Depth},      {"tool_pushpull", Icon::PushPull}, {"tool_volume", Icon::Volume},
    {"tool_parts", Icon::Parts},      {"tool_mirror", Icon::Mirror},  {"validate", Icon::Validate},
    {"export", Icon::Export}};

QString friendly_job(const QString& name) {
  static const std::map<QString, QString> names = {
      {"Import views", QObject::tr("Drawings imported")},
      {"Reconstruct and validate", QObject::tr("Model rebuilt and checked")},
      {"Reconstruct", QObject::tr("Model rebuilt")},
      {"Validate", QObject::tr("Model checked")},
      {"Edit parts", QObject::tr("Part choice applied")},
      {"Set depth", QObject::tr("Depth applied")},
      {"Save project", QObject::tr("Project saved")},
      {"Open project", QObject::tr("Project opened")},
      {"New project", QObject::tr("New project")},
      {"Project textures", QObject::tr("Textures projected")},
      {"Auto-align", QObject::tr("Drawings aligned")},
      {"Clean up", QObject::tr("Mesh cleaned up")},
      {"Undo", QObject::tr("Undone")},
      {"Redo", QObject::tr("Redone")},
      {"Depth brush", QObject::tr("Depth painted")},
      {"Push/pull", QObject::tr("Face moved")},
      {"Paint part mask", QObject::tr("Part mask painted")},
      {"Erase part mask", QObject::tr("Part mask erased")},
      {"Reload views", QObject::tr("Drawings reloaded")},
      {"New single-view project", QObject::tr("Single-drawing project")},
      {"Single view depth", QObject::tr("Depth set")},
  };
  const auto it = names.find(name);
  return it == names.end() ? name : it->second;
}

std::vector<vec2d> to_points(const QVector<QPointF>& pts) {
  std::vector<vec2d> out;
  out.reserve(static_cast<size_t>(pts.size()));
  for (const QPointF& p : pts) out.push_back({p.x(), p.y()});
  return out;
}

QString settings_key_for(const QString& file) { return "view_roles/" + QFileInfo(file).absoluteFilePath(); }

QDoubleSpinBox* small_spin(double lo, double hi, double step, double value, const QString& suffix, const QString& tip) {
  auto* s = new QDoubleSpinBox;
  s->setRange(lo, hi);
  s->setSingleStep(step);
  s->setDecimals(1);
  s->setValue(value);
  s->setSuffix(suffix);
  s->setToolTip(tip);
  s->setMinimumWidth(92);
  s->setMaximumWidth(104);
  return s;
}

}  // namespace

MainWindow::MainWindow(const WindowOptions& opt, QWidget* parent) : QMainWindow(parent) {
  setObjectName("Pixel3DMainWindow");
  setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
  setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
  setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowTabbedDocks);
  setAcceptDrops(true);
  controller_ = std::make_unique<AppController>(opt.autosave_dir);
  controller_->startProject(default_project_settings());  // Low-Poly Studio: no Top drawing needed
  editor_ = new EditorState(this);
  editor_->setCameraScheme(static_cast<CameraScheme>(std::clamp(QSettings().value("camera/scheme", 0).toInt(), 0, 2)));
  center_ = new QStackedWidget;
  start_ = new StartScreen;
  viewport_ = new Viewport(opt.software_view);
  viewport_->setEditor(editor_);
  center_->addWidget(start_);
  center_->addWidget(viewport_);
  setCentralWidget(center_);
  confirm_ = [this](const QString& title, const QString& text) {
    return QMessageBox::question(this, title, text, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) ==
           QMessageBox::Yes;
  };
  choose_layers_ = [this](const json& layers) -> std::optional<QStringList> {
    // One checkbox per layer; hidden layers and layers without drawn pixels are shown but cannot be picked.
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Parts from layers"));
    auto* lay = new QVBoxLayout(&dlg);
    auto* intro = make_label(tr("Each visible layer can become a part named after it. Pick the ones to keep; you can change "
                                "every part and its type afterwards."),
                             "secondary");
    intro->setWordWrap(true);
    lay->addWidget(intro);
    auto* list = new QListWidget;
    list->setObjectName("layerChoices");
    for (const auto& l : layers) {
      const QString name = qs(l.value("path", l.value("name", std::string())));
      auto* item = new QListWidgetItem(l.contains("type") ? tr("%1  (%2)").arg(name, qs(l["type"].get<std::string>())) : name);
      item->setData(Qt::UserRole, name);
      const bool ok = l.value("suggested", false);
      item->setFlags(ok ? Qt::ItemIsEnabled | Qt::ItemIsUserCheckable : Qt::ItemIsUserCheckable);
      item->setCheckState(ok ? Qt::Checked : Qt::Unchecked);
      if (!ok) item->setToolTip(qs(l.value("why_not", std::string())));
      list->addItem(item);
    }
    lay->addWidget(list);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted) return std::nullopt;
    QStringList keep;
    for (int i = 0; i < list->count(); ++i)
      if (list->item(i)->checkState() == Qt::Checked) keep << list->item(i)->data(Qt::UserRole).toString();
    return keep;
  };
  ask_missing_views_ = [this](const QString& text, ViewRole missing) {
    QMessageBox box(this);
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle(tr("Visual Hull needs more drawings"));
    box.setText(text);
    box.setInformativeText(tr("Switching keeps your drawings and can be undone (Ctrl+Z)."));
    QPushButton* sw = box.addButton(tr("Switch to Low-Poly Studio (no Top needed)"), QMessageBox::AcceptRole);
    QPushButton* add = box.addButton(tr("Add the %1 drawing…").arg(view_title(missing)), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(sw);
    box.exec();
    return box.clickedButton() == sw    ? MissingViewsChoice::SwitchToLowpoly
           : box.clickedButton() == add ? MissingViewsChoice::AddView
                                        : MissingViewsChoice::Cancel;
  };
  ask_text_ = [this](const QString& title, const QString& label, const QString& value) -> std::optional<QString> {
    bool ok = false;
    const QString t = QInputDialog::getText(this, title, label, QLineEdit::Normal, value, &ok);
    if (!ok || t.trimmed().isEmpty()) return std::nullopt;
    return t.trimmed();
  };
  buildDocks();
  buildActions();
  buildToolbar();
  buildStatusBar();
  buildViewportOverlays();
  connectEditing();

  connect(controller_.get(), &AppController::viewChanged, this, &MainWindow::onViewChanged);
  connect(controller_.get(), &AppController::jobStarted, this, &MainWindow::onJobStarted);
  connect(controller_.get(), &AppController::jobFinished, this, &MainWindow::onJobFinished);
  connect(controller_.get(), &AppController::projectPathChanged, this, [this](const QString& p) {
    if (!p.isEmpty()) addRecent(p);
    updateTitle();
  });
  connect(controller_.get(), &AppController::progress, this, [this](const QString& name, float f, const QString& stage) {
    progress_->setValue(static_cast<int>(f * 100));
    statusBar()->showMessage(tr("%1 — %2…").arg(friendly_job(name), stage));
  });
  connect(controller_.get(), &AppController::compareReady, views_, &ViewsPanel::setCompare);
  connect(controller_.get(), &AppController::autosaved, this,
          [this](const QString&) { statusBar()->showMessage(tr("Autosaved"), 2500); });
  connect(viewport_, &Viewport::rendererChanged, this, [this](const QString& s) { inspector_->setRendererInfo(s); });
  connect(viewport_, &Viewport::viewStateChanged, this, &MainWindow::updateCameraButtons);
  connect(viewport_, &Viewport::triangleClicked, this, &MainWindow::onTriangleClicked);
  connect(viewport_, &Viewport::triangleHovered, this, &MainWindow::onTriangleHovered);

  // Start screen.
  connect(start_, &StartScreen::filesDropped, this, [this](const QStringList& f, std::optional<ViewRole> r) { importFiles(f, r); });
  connect(start_, &StartScreen::browseRequested, this, &MainWindow::browseRole);
  connect(start_, &StartScreen::importRequested, this, [this] { runAction("import"); });
  connect(start_, &StartScreen::image3dRequested, this, [this] { runAction("image3d"); });
  connect(start_, &StartScreen::openRequested, this, [this] { actions_["open"]->trigger(); });
  connect(start_, &StartScreen::reconstructRequested, this, [this] {
    if (controller_->view().doc.settings.lowpoly.enabled) {
      setStartScreen(false);
      runAction(controller_->view().doc.masks.parts.empty() ? "lowpoly_suggest" : "lowpoly_build");
    } else {
      runAction("run_all");
    }
  });
  connect(start_, &StartScreen::projectTypeRequested, this, &MainWindow::switchProjectType);
  connect(start_, &StartScreen::workspaceRequested, this, &MainWindow::showWorkspace);
  connect(start_, &StartScreen::singleViewRequested, this, [this] { newSingleView(); });
  connect(start_, &StartScreen::recentRequested, this, [this](const QString& p) {
    if (maybeSave()) openProjectFile(p);
  });
  start_->setRecent(recentFiles());

  // Aseprite workflow: drawings re-exported on disk are noticed.
  reload_timer_.setSingleShot(true);
  reload_timer_.setInterval(400);
  connect(&watcher_, &QFileSystemWatcher::fileChanged, this, &MainWindow::viewsChangedOnDisk);
  connect(&reload_timer_, &QTimer::timeout, this, [this] {
    if (!actions_["auto_reload"]->isChecked()) {
      reload_button_->show();
      return;
    }
    if (controller_->busy()) {
      reload_timer_.start();  // again when the current job is done
      return;
    }
    controller_->reloadViews();
  });

  setMinimumSize(1024, 640);
  resize(1440, 900);
  QSettings s;
  const bool advanced = s.value("ui/advanced", false).toBool();
  actions_["advanced"]->setChecked(advanced);
  inspector_->setAdvanced(advanced);
  restoreGeometry(s.value("ui/geometry").toByteArray());
  if (!restoreState(s.value("ui/state").toByteArray(), kLayoutVersion)) resetLayout();
  setStartScreen(true);
  onViewChanged();
  updateCameraButtons();
  QTimer::singleShot(500, this, [this] { inspector_->setRendererInfo(viewport_->rendererInfo()); });
}

MainWindow::~MainWindow() = default;

QDockWidget* MainWindow::dock(const QString& name) const {
  const auto it = docks_.find(name);
  return it == docks_.end() ? nullptr : it->second;
}

// ---- construction -----------------------------------------------------------------

void MainWindow::buildDocks() {
  views_ = new ViewsPanel;
  inspector_ = new Inspector;
  history_ = new HistoryPanel;
  log_ = new LogPanel;
  views_->setEditor(editor_);
  views_->setMinimumWidth(theme::metric::dock_min_left);
  inspector_->setMinimumWidth(theme::metric::dock_min_right);
  auto make = [this](const char* name, const QString& title, QWidget* w, Qt::DockWidgetArea area) {
    auto* d = new QDockWidget(title, this);
    d->setObjectName(name);
    d->setWidget(w);
    d->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    auto* t = new DockTitle(title);
    d->setTitleBarWidget(t);
    connect(t, &DockTitle::closeRequested, d, &QDockWidget::hide);
    addDockWidget(area, d);
    docks_[name] = d;
    return t;
  };
  DockTitle* vt = make("views", tr("Views"), views_, Qt::LeftDockWidgetArea);
  make("inspector", tr("Inspector"), inspector_, Qt::RightDockWidgetArea);
  make("history", tr("Checks & History"), history_, Qt::BottomDockWidgetArea);
  make("log", tr("Log"), log_, Qt::BottomDockWidgetArea);
  auto* imp = new QToolButton;
  imp->setIcon(icon(Icon::Import));
  imp->setIconSize(QSize(16, 16));
  imp->setAutoRaise(true);
  imp->setToolTip(tr("Import drawings (Ctrl+I)"));
  imp->setCursor(Qt::PointingHandCursor);
  connect(imp, &QToolButton::clicked, this, [this] { runAction("import"); });
  // The project type, always in sight in the Views header; its menu switches it (Model ▸ Project Type).
  mode_chip_ = new QToolButton;
  mode_chip_->setObjectName("projectTypeChip");
  mode_chip_->setAutoRaise(true);
  mode_chip_->setPopupMode(QToolButton::InstantPopup);
  mode_chip_->setCursor(Qt::PointingHandCursor);
  mode_chip_->setMenu(new QMenu(mode_chip_));
  vt->addTrailing(mode_chip_);
  vt->addTrailing(imp);

  connect(views_, &ViewsPanel::projectTypeRequested, this, &MainWindow::switchProjectType);
  connect(views_, &ViewsPanel::filesDropped, this, [this](const QStringList& f, std::optional<ViewRole> r) { importFiles(f, r); });
  connect(views_, &ViewsPanel::browseRequested, this, &MainWindow::browseRole);
  connect(views_, &ViewsPanel::compareRequested, controller_.get(), &AppController::requestCompare);

  connect(inspector_, &Inspector::issueRequested, this, &MainWindow::openIssue);
  connect(inspector_, &Inspector::partRequested, this, [this](int p) { selectPart(p, true); });
  connect(inspector_, &Inspector::overviewRequested, this, &MainWindow::clearSelection);
  connect(inspector_, &Inspector::actionRequested, this, &MainWindow::runAction);
  connect(inspector_, &Inspector::applyParts, this, [this](const std::vector<UserDecision>& d) { controller_->editParts(d); });
  connect(inspector_, &Inspector::applyDepth, this, [this](const DepthConstraint& c) { controller_->setDepth(c); });
  connect(inspector_, &Inspector::frameRequested, this, &MainWindow::frameSelection);
  connect(inspector_, &Inspector::candidatesHovered, this, [this](const std::vector<int>& c) {
    hover_candidates_ = c;
    updateHighlight();
  });
  connect(inspector_, &Inspector::pushPullRequested, this, [this](double distance) {
    const Scene& s = viewport_->scene();
    if (sel_tri_ < 0 || sel_tri_ >= static_cast<int>(s.tri_region.size())) return;
    PushPullRequest r;
    r.face = s.tri_region[static_cast<size_t>(sel_tri_)];
    r.distance = distance;
    r.snap = editor_->snap;
    r.respect_bounds = editor_->respect_bounds;
    controller_->pushPull(r);
  });
  connect(inspector_, &Inspector::singleViewDepthRequested, controller_.get(), &AppController::setSingleViewDepth);
  connect(inspector_, &Inspector::lowpolySuggestRequested, this, [this] { runAction("lowpoly_suggest"); });
  connect(inspector_, &Inspector::lowpolyLayerPartsRequested, this, [this] { suggestLayerParts(); });
  connect(inspector_, &Inspector::lowpolyBuildRequested, this, [this](const LowpolyUpdate& u) { controller_->buildLowpoly(u); });
  connect(inspector_, &Inspector::lowpolyPartTypeRequested, this, [this](int part, lowpoly::PartType t) {
    controller_->setPartType(PartTypeUpdate{part, t, std::nullopt});
  });
  connect(inspector_, &Inspector::lowpolyPoseToggled, this, [this](bool posed) { setLowpolyPose(posed); });
  connect(history_, &HistoryPanel::jumpRequested, controller_.get(), &AppController::jumpToHistory);
}

void MainWindow::buildActions() {
  auto add = [this](QMenu* menu, const QString& key, const QString& text, const QKeySequence& shortcut,
                    std::function<void()> fn) {
    auto* a = new QAction(text, this);
    if (!shortcut.isEmpty()) a->setShortcut(shortcut);
    connect(a, &QAction::triggered, this, std::move(fn));
    if (menu) menu->addAction(a);
    actions_[key] = a;
    return a;
  };
  auto toggle = [&](QMenu* menu, const QString& key, const QString& text, bool initial, std::function<void(bool)> fn) {
    QAction* a = add(menu, key, text, {}, [] {});
    a->setCheckable(true);
    a->setChecked(initial);
    connect(a, &QAction::toggled, this, std::move(fn));
    return a;
  };
  AppController* c = controller_.get();

  QMenu* file = menuBar()->addMenu(tr("&File"));
  add(file, "new", tr("&New Project…"), QKeySequence::New, [this] {
    if (!maybeSave()) return;
    NewProjectDialog d(this);
    if (d.exec() != QDialog::Accepted) return;
    controller_->newProject(d.settings());
    clearSelection();
    setStartScreen(true);
  })->setIcon(icon(Icon::New));
  add(file, "new_single", tr("New &Single-Drawing Project…"), {}, [this] { newSingleView(); });
  add(file, "open", tr("&Open…"), QKeySequence::Open, [this] {
    if (!maybeSave()) return;
    // A project, or drawings (images, .aseprite, .psd) that start a new Low-Poly Studio project.
    QStringList pats{"*.p3d"};
    for (const QString& e : source_image_extensions()) pats << "*." + e;
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Open a project or drawings"), {},
        tr("Projects and drawings (%1);;3re pixel projects (*.p3d);;%2").arg(pats.join(' '), source_image_filter()));
    if (files.isEmpty()) return;
    if (files.size() == 1 && files.front().endsWith(".p3d", Qt::CaseInsensitive)) openProjectFile(files.front());
    else openDrawingsAsLowpoly(files);
  })->setIcon(icon(Icon::Open));
  recent_menu_ = file->addMenu(tr("Open &Recent"));
  rebuildRecentMenu();
  add(file, "save", tr("&Save"), QKeySequence::Save, [this] {
    if (controller_->projectPath().isEmpty()) actions_["save_as"]->trigger();
    else saveProjectFile(controller_->projectPath());
  })->setIcon(icon(Icon::Save));
  add(file, "save_as", tr("Save &As…"), QKeySequence::SaveAs, [this] {
    const QString f = QFileDialog::getSaveFileName(this, tr("Save project"), controller_->projectPath(),
                                                   tr("3re pixel projects (*.p3d)"));
    if (!f.isEmpty()) saveProjectFile(f.endsWith(".p3d", Qt::CaseInsensitive) ? f : f + ".p3d");
  });
  file->addSeparator();
  add(file, "import", tr("&Import Views…"), QKeySequence(Qt::CTRL | Qt::Key_I), [this] { runAction("import"); });
  add(file, "reload_views", tr("Re&load Drawings from Disk"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R), [this] {
    reload_button_->hide();
    controller_->reloadViews();
  });
  toggle(file, "auto_reload", tr("Reload Changed Drawings &Automatically"), QSettings().value("views/auto_reload", false).toBool(),
         [](bool on) { QSettings().setValue("views/auto_reload", on); });
  add(file, "export", tr("&Export Model…"), QKeySequence(Qt::CTRL | Qt::Key_E), [this] { runAction("export"); });
  add(file, "renders", tr("Write Validation &Renders…"), {}, [this] {
    if (visualHullBlocked()) return;
    const QString d = QFileDialog::getExistingDirectory(this, tr("Folder for validation renders"));
    if (!d.isEmpty()) controller_->writeArtifacts(d, true);
  });
  file->addSeparator();
  add(file, "quit", tr("&Quit"), QKeySequence::Quit, [this] { close(); });

  QMenu* edit = menuBar()->addMenu(tr("&Edit"));
  add(edit, "undo", tr("&Undo"), QKeySequence::Undo, [c] { c->undo(); })->setIcon(icon(Icon::Undo));
  add(edit, "redo", tr("&Redo"), QKeySequence::Redo, [c] { c->redo(); })->setIcon(icon(Icon::Redo));
  edit->addSeparator();
  add(edit, "checkpoint", tr("Create &Checkpoint…"), {}, [this] {
    bool ok = false;
    const QString n = QInputDialog::getText(this, tr("Checkpoint"), tr("Name:"), QLineEdit::Normal, {}, &ok);
    if (ok && !n.trimmed().isEmpty()) controller_->createCheckpoint(n.trimmed());
  });
  add(edit, "restore", tr("Restore C&heckpoint…"), {}, [this] {
    QStringList names;
    for (const auto& n : controller_->view().checkpoints) names << qs(n);
    if (names.isEmpty()) {
      QMessageBox::information(this, tr("Checkpoints"), tr("There are no checkpoints yet."));
      return;
    }
    bool ok = false;
    const QString n = QInputDialog::getItem(this, tr("Restore checkpoint"), tr("Checkpoint:"), names, names.size() - 1,
                                            false, &ok);
    if (ok) controller_->restoreCheckpoint(n);
  });
  edit->addSeparator();
  add(edit, "clear_selection", tr("Clear &Selection"), QKeySequence(Qt::Key_Escape), [this] {
    cancelPreview();
    clearSelection();
  });

  QMenu* model = menuBar()->addMenu(tr("&Model"));
  add(model, "run_all", tr("&Reconstruct"), QKeySequence(Qt::Key_F5), [this, c] {
    if (c->view().doc.settings.lowpoly.enabled) runAction("lowpoly_build");  // the project's model is the character
    else if (!visualHullBlocked()) c->runAll(ReconstructMode::TrueSolid);
  });
  add(model, "validate", tr("&Validate"), QKeySequence(Qt::Key_F6), [this, c] {
    if (!visualHullBlocked()) c->validate();
  });
  model->addSeparator();
  QMenu* steps = model->addMenu(tr("Single &Steps"));
  add(steps, "align", tr("&Auto-align Views"), {}, [this, c] {
    if (!visualHullBlocked()) c->autoAlign();
  });
  add(steps, "reconstruct", tr("Reconstruct &Only (TRUE SOLID)"), QKeySequence(Qt::CTRL | Qt::Key_R), [this, c] {
    if (!visualHullBlocked()) c->reconstruct(ReconstructMode::TrueSolid);
  });
  add(steps, "reconstruct_voxel", tr("Reconstruct as &Voxels"), {}, [this, c] {
    if (!visualHullBlocked()) c->reconstruct(ReconstructMode::Voxel);
  });
  add(steps, "textures", tr("Project &Textures"), QKeySequence(Qt::CTRL | Qt::Key_T), [this, c] {
    if (!visualHullBlocked()) c->projectTextures();
  });
  add(steps, "cleanup", tr("&Clean Up Mesh"), {}, [this, c] {
    if (!visualHullBlocked()) c->cleanup();
  });
  model->addSeparator();
  QMenu* type = model->addMenu(tr("Project &Type"));
  auto* types = new QActionGroup(this);
  for (const bool lp : {true, false}) {
    QAction* a = add(type, lp ? "mode_lowpoly" : "mode_visual_hull",
                     QStringLiteral("%1 — %2").arg(project_type_name(lp), project_type_needs(lp)), {},
                     [this, lp] { switchProjectType(lp); });
    a->setCheckable(true);
    types->addAction(a);
    mode_chip_->menu()->addAction(a);
  }
  add(model, "settings", tr("Project &Settings…"), {}, [this] { openSettings(); })->setIcon(icon(Icon::Settings));

  QMenu* tools = menuBar()->addMenu(tr("&Tools"));
  tools_ = new QActionGroup(this);
  tools_->setExclusive(true);
  const std::tuple<const char*, QKeySequence, Tool> tool_defs[] = {
      {"tool_select", QKeySequence(Qt::Key_V), Tool::Select},
      {"tool_depth", QKeySequence(Qt::Key_D), Tool::Depth},
      {"tool_pushpull", QKeySequence(Qt::Key_U), Tool::PushPull},
      {"tool_volume", QKeySequence(Qt::Key_B), Tool::Volume},
      {"tool_parts", QKeySequence(Qt::Key_P), Tool::Parts},
  };
  for (const auto& [key, sc, t] : tool_defs) {
    const Tool tt = t;
    QAction* a = add(tools, key, QStringLiteral("&") + tool_name(tt), sc, [this, tt] { setTool(tt); });
    a->setCheckable(true);
    a->setStatusTip(tool_hint(tt));
    a->setProperty("tip", tool_hint(tt));
    tools_->addAction(a);
  }
  actions_["tool_select"]->setChecked(true);
  // Mirror: an action with a menu (copy or flip, across X, Y or Z).
  mirror_menu_ = new QMenu(this);
  QAction* mirror = add(tools, "tool_mirror", tr("&Mirror"), QKeySequence(Qt::Key_M), [this] {
    mirror_menu_->exec(QCursor::pos());
  });
  mirror->setMenu(mirror_menu_);
  tools->addSeparator();
  add(tools, "frame", tr("&Frame Selection"), QKeySequence(Qt::Key_F), [this] { frameSelection(); })->setIcon(icon(Icon::Frame));
  tools->addSeparator();
  add(tools, "image3d", tr("&Image to 3D · TRELLIS.2…"), {}, [this] { runAction("image3d"); })
      ->setIcon(icon(Icon::Reconstruct));
#ifdef PIXEL3D_ENABLE_SMARTQUAD
  add(tools, "smart_quad", tr("Smart &Quad · Optimize Mesh…"), {}, [this] { runAction("smart_quad"); })
      ->setIcon(icon(Icon::Reconstruct));
#endif

  part_menu_ = menuBar()->addMenu(tr("&Part"));
  buildPartMenu(part_menu_);
  for (int axis = 0; axis < 3; ++axis) mirror_menu_->addAction(actions_[QStringLiteral("part_mirror_copy_%1").arg(axis)]);
  mirror_menu_->addSeparator();
  for (int axis = 0; axis < 3; ++axis) mirror_menu_->addAction(actions_[QStringLiteral("part_mirror_%1").arg(axis)]);

  QMenu* view = menuBar()->addMenu(tr("&View"));
  QMenu* cam = view->addMenu(tr("&Camera"));
  cameras_ = new QActionGroup(this);
  cameras_->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
  const std::pair<ViewRole, int> presets[] = {{ViewRole::Front, Qt::Key_1}, {ViewRole::Back, Qt::Key_2},
                                              {ViewRole::Side, Qt::Key_3},  {ViewRole::Right, Qt::Key_4},
                                              {ViewRole::Top, Qt::Key_5},   {ViewRole::Bottom, Qt::Key_6}};
  for (const auto& [role, key] : presets) {
    const ViewRole r = role;
    QAction* a = add(cam, QStringLiteral("view_%1").arg(static_cast<int>(r)), view_title(r), QKeySequence(key), [this, r] {
      viewport_->setPreset(r);
      views_->setRole(r);
      updateCameraButtons();
    });
    a->setCheckable(true);
    a->setIconText(view_title(r));
    cameras_->addAction(a);
  }
  actions_["view_0"]->setIcon(icon(Icon::ViewFront));
  actions_["view_2"]->setIcon(icon(Icon::ViewSide));
  actions_["view_4"]->setIcon(icon(Icon::ViewTop));
  QAction* iso = add(cam, "view_iso", tr("&3D (perspective)"), QKeySequence(Qt::Key_0), [this] {
    viewport_->setIso();
    updateCameraButtons();
  });
  iso->setCheckable(true);
  iso->setIcon(icon(Icon::View3D));
  iso->setIconText(tr("3D"));
  cameras_->addAction(iso);
  cam->addSeparator();
  toggle(cam, "ortho", tr("&Orthographic"), false, [this](bool on) {
    viewport_->state().ortho = on;
    if (!on) viewport_->state().preset.reset();
    viewport_->refresh();
  })->setIcon(icon(Icon::Ortho));
  actions_["ortho"]->setShortcut(QKeySequence(Qt::Key_O));
  QMenu* controls = cam->addMenu(tr("Mouse &Controls"));
  auto* schemes = new QActionGroup(this);
  for (int i = 0; i < 3; ++i) {
    const auto s = static_cast<CameraScheme>(i);
    QAction* a = add(controls, QStringLiteral("camera_%1").arg(i), camera_scheme_name(s), {}, [this, s, i] {
      editor_->setCameraScheme(s);
      QSettings().setValue("camera/scheme", i);
    });
    a->setCheckable(true);
    a->setChecked(editor_->cameraScheme() == s);
    schemes->addAction(a);
  }
  view->addSeparator();
  auto refresh = [this] { viewport_->refresh(); };
  toggle(view, "textured", tr("&Textured"), true, [this, refresh](bool on) { viewport_->state().textured = on, refresh(); })
      ->setIcon(icon(Icon::Texture));
  toggle(view, "wireframe", tr("&Wireframe"), false, [this, refresh](bool on) { viewport_->state().wireframe = on, refresh(); })
      ->setIcon(icon(Icon::Wireframe));
  toggle(view, "lighting", tr("&Lighting"), true, [this, refresh](bool on) { viewport_->state().lit = on, refresh(); })
      ->setIcon(icon(Icon::Lighting));
  toggle(view, "pixel_preview", tr("&Pixel Preview"), false,
         [this, refresh](bool on) { viewport_->state().pixel_preview = on, refresh(); })
      ->setIcon(icon(Icon::PixelPreview));
  toggle(view, "normals", tr("&Normals"), false, [this, refresh](bool on) { viewport_->state().normals = on, refresh(); })
      ->setIcon(icon(Icon::Normals));
  toggle(view, "grid", tr("&Grid"), true, [this, refresh](bool on) { viewport_->state().grid = on, refresh(); })
      ->setIcon(icon(Icon::Grid));
  toggle(view, "snap", tr("&Snap to Whole Pixels"), true, [this](bool on) {
    editor_->snap = on;
    editor_->touch();
  })->setIcon(icon(Icon::Snap));
  toggle(view, "overlay", tr("&Drawing Overlay in Front/Side/Top"), true,
         [this, refresh](bool on) { viewport_->state().overlay = on, refresh(); })
      ->setIcon(icon(Icon::DrawingOverlay));
  toggle(view, "show_issues", tr("Show &Issues and Conflicts"), true,
         [this, refresh](bool on) { viewport_->state().show_issues = on, refresh(); })
      ->setIcon(icon(Icon::IssuesOverlay));
  toggle(view, "show_estimated", tr("Show &Estimated Geometry"), false,
         [this, refresh](bool on) { viewport_->state().show_estimated = on, refresh(); })
      ->setIcon(icon(Icon::EstimatedOverlay));
  toggle(view, "turntable", tr("T&urntable"), false, [this](bool on) { viewport_->setTurntable(on); })->setIcon(icon(Icon::Turntable));
  actions_["textured"]->setShortcut(QKeySequence(Qt::Key_T));
  actions_["wireframe"]->setShortcut(QKeySequence(Qt::Key_W));
  actions_["pixel_preview"]->setShortcut(QKeySequence(Qt::Key_K));
  actions_["normals"]->setShortcut(QKeySequence(Qt::Key_N));
  actions_["grid"]->setShortcut(QKeySequence(Qt::Key_G));
  actions_["snap"]->setShortcut(QKeySequence(Qt::Key_S));
  actions_["show_issues"]->setShortcut(QKeySequence(Qt::Key_C));
  actions_["show_estimated"]->setShortcut(QKeySequence(Qt::Key_E));
  view->addSeparator();
  QMenu* panels = view->addMenu(tr("&Panels"));
  for (const char* n : {"views", "inspector", "history", "log"}) {
    QAction* a = docks_[n]->toggleViewAction();
    panels->addAction(a);
    actions_[QStringLiteral("panel_%1").arg(n)] = a;
  }
  actions_["panel_log"]->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
  actions_["panel_history"]->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_H));
  toggle(view, "advanced", tr("Show &Advanced Details"), false, [this](bool on) {
    inspector_->setAdvanced(on);
    QSettings().setValue("ui/advanced", on);
  });
  add(view, "reset_layout", tr("&Reset Layout"), {}, [this] { resetLayout(); });

  QMenu* help = menuBar()->addMenu(tr("&Help"));
  add(help, "workflow", tr("&How It Works"), QKeySequence::HelpContents, [this] {
    QMessageBox::information(
        this, tr("How the Pixel Art Builder works"),
        tr("1. Drop your Front, Side (seen from the left) and Top (front edge at the bottom) drawings on their slots — the "
           "PNGs you export from Aseprite. Back, Right and Bottom are optional and remove guesswork. Only one drawing? "
           "File ▸ New Single-Drawing Project.\n\n"
           "2. Press Reconstruct (F5). 3re builds a solid model and checks it against every drawing.\n\n"
           "3. The status card says PASS, NEEDS REVIEW or FAIL. Amber areas need your decision: click one (or Review "
           "issues) and answer with the tool that opens — only decide what you know, or add another view.\n\n"
           "4. Edit with the tools: Select (V) parts or faces; Depth (D) paints depth on a drawing; Push/Pull (U) drags a "
           "face; Volume (B) adds or erases a rectangle of a drawing; Parts (P) lists parts and paints part masks; "
           "Mirror (M). Every edit is one undo step, and Checks & History shows at once when the model stops matching "
           "a drawing.\n\n"
           "5. Look at the model from every side (1–6, 0; drag to orbit, right-drag to pan, scroll to zoom).\n\n"
           "6. Export (Ctrl+E): GLB, glTF or OBJ, with presets for Godot, Unity and Roblox. A model that is not PASS is "
           "exported only after you confirm.\n\n"
           "Re-exported a drawing? File ▸ Reload Drawings (Ctrl+Shift+R) keeps your edits and masks."));
  });
  add(help, "about", tr("&About 3re"), {}, [this] {
    QMessageBox::about(this, tr("About 3re"),
                       tr("<b>3re %1</b><br>Images and pixel art to validated 3D models (GLB, glTF, OBJ), with "
                          "an editor for your drawings.<br><br>%2<br><br>Third-party components and their licences: "
                          "THIRD-PARTY-NOTICES.txt and the LICENSES folder next to 3re.exe. Qt is used under the "
                          "LGPL-3.0.")
                           .arg(qs(version()), viewport_->rendererInfo().toHtmlEscaped()));
  });
}

void MainWindow::buildPartMenu(QMenu* menu) {
  auto add = [this, menu](const QString& key, const QString& text, const QKeySequence& sc, std::function<void()> fn) {
    auto* a = new QAction(text, this);
    if (!sc.isEmpty()) a->setShortcut(sc);
    connect(a, &QAction::triggered, this, std::move(fn));
    menu->addAction(a);
    actions_[key] = a;
    return a;
  };
  add("part_new_define", tr("&New Part (painted on the drawings)…"), {}, [this] { newPart(true); });
  add("part_new_label", tr("New &Label for Existing Pixels…"), {}, [this] { newPart(false); });
  add("part_rename", tr("&Rename…"), QKeySequence(Qt::Key_F2), [this] { runAction("part_rename"); });
  menu->addSeparator();
  add("part_duplicate", tr("&Duplicate"), QKeySequence(Qt::CTRL | Qt::Key_D), [this] { runAction("part_duplicate"); });
  add("part_transform", tr("&Transform…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T), [this] { runAction("part_transform"); });
  QMenu* mirror = menu->addMenu(tr("&Mirror"));
  for (int axis = 0; axis < 3; ++axis) {
    const QString name = QStringLiteral("XYZ").mid(axis, 1);
    auto* copy = new QAction(tr("Mirror Copy across %1").arg(name), this);
    connect(copy, &QAction::triggered, this, [this, axis] { runAction(QStringLiteral("part_mirror_copy_%1").arg(axis)); });
    mirror->addAction(copy);
    actions_[QStringLiteral("part_mirror_copy_%1").arg(axis)] = copy;
  }
  mirror->addSeparator();
  for (int axis = 0; axis < 3; ++axis) {
    const QString name = QStringLiteral("XYZ").mid(axis, 1);
    auto* flip = new QAction(tr("Flip in Place across %1").arg(name), this);
    connect(flip, &QAction::triggered, this, [this, axis] { runAction(QStringLiteral("part_mirror_%1").arg(axis)); });
    mirror->addAction(flip);
    actions_[QStringLiteral("part_mirror_%1").arg(axis)] = flip;
  }
  menu->addSeparator();
  add("part_split", tr("&Split by Plane…"), {}, [this] { runAction("part_split"); });
  add("part_split_shells", tr("Split into &Pieces"), {}, [this] { runAction("part_split_shells"); });
  add("part_merge", tr("&Merge Selected Parts"), QKeySequence(Qt::CTRL | Qt::Key_J), [this] { runAction("part_merge"); });
  add("part_cut", tr("C&ut with Plane…"), {}, [this] { runAction("part_cut"); });
  add("part_delete", tr("De&lete Part"), QKeySequence(Qt::Key_Delete), [this] { runAction("part_delete"); });
  menu->addSeparator();
  add("part_paint_masks", tr("&Paint Masks of This Part"), {}, [this] { runAction("part_paint_masks"); });
  add("part_sync_masks", tr("&Fill Masks in the Other Views"), {}, [this] { runAction("part_sync_masks"); });
}

void MainWindow::buildToolbar() {
  toolbar_ = new QToolBar(tr("Main"));
  toolbar_->setObjectName("MainToolbar");
  toolbar_->setMovable(false);
  toolbar_->setFloatable(false);
  toolbar_->setIconSize(QSize(theme::icon_size::md, theme::icon_size::md));
  toolbar_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
  toolbar_->toggleViewAction()->setVisible(false);
  addToolBar(Qt::TopToolBarArea, toolbar_);
  const std::pair<const char*, QString> texts[] = {
      {"import", tr("Import")},       {"run_all", tr("Reconstruct")},   {"tool_select", tr("Select")},
      {"tool_depth", tr("Depth")},    {"tool_pushpull", tr("Push/Pull")}, {"tool_volume", tr("Volume")},
      {"tool_parts", tr("Parts")},    {"tool_mirror", tr("Mirror")},   {"validate", tr("Validate")},
      {"export", tr("Export")}};
  const std::map<QString, QString> tips = {
      {"import", tr("Import drawings — Front, Side, Top PNGs (Ctrl+I)")},
      {"run_all", tr("Reconstruct — build the model from the drawings and check it (F5)")},
      {"tool_select", tr("Select — click a part or face, in the model or on a drawing (V)")},
      {"tool_depth", tr("Depth — paint depth on a drawing; decide undecided depths (D)")},
      {"tool_pushpull", tr("Push/Pull — drag a face along its normal (U)")},
      {"tool_volume", tr("Volume — drag a rectangle on a drawing to add, erase or extrude (B)")},
      {"tool_parts", tr("Parts — inspect parts, paint part masks (P)")},
      {"tool_mirror", tr("Mirror the selected part or the whole model (M)")},
      {"validate", tr("Validate — check the model against every drawing (F6)")},
      {"export", tr("Export — GLB, glTF or OBJ for your engine (Ctrl+E)")}};
  for (const auto& [key, text] : texts) {
    QAction* a = actions_[key];
    a->setIconText(text);
    a->setToolTip(tips.at(key));
    for (const auto& [k, ic] : kToolbarIcons)
      if (QString(k) == key) a->setIcon(icon(ic));
    toolbar_->addAction(a);
    if (QString(key) == "import" || QString(key) == "run_all" || QString(key) == "tool_mirror")
      toolbar_->addWidget(make_vdivider());
  }
  for (QAction* a : toolbar_->actions())
    if (QWidget* w = toolbar_->widgetForAction(a)) {
      w->setCursor(Qt::PointingHandCursor);
      w->setMinimumWidth(theme::metric::toolbar_min);
      if (auto* b = qobject_cast<QToolButton*>(w); b && a == actions_["tool_mirror"]) b->setPopupMode(QToolButton::InstantPopup);
    }
}

void MainWindow::buildStatusBar() {
  QStatusBar* sb = statusBar();
  sb->setSizeGripEnabled(false);
  reload_button_ = new QToolButton;
  reload_button_->setProperty("kind", "text");
  reload_button_->setIcon(icon(Icon::StatusReview, IconStyle::Toned, Tone::Review));
  reload_button_->setIconSize(QSize(16, 16));
  reload_button_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  reload_button_->setText(tr("Drawings changed — Reload"));
  reload_button_->setToolTip(tr("A drawing was saved again (e.g. re-exported from Aseprite). Reload keeps your edits (Ctrl+Shift+R)."));
  reload_button_->setAutoRaise(true);
  reload_button_->setCursor(Qt::PointingHandCursor);
  reload_button_->hide();
  connect(reload_button_, &QToolButton::clicked, this, [this] { actions_["reload_views"]->trigger(); });
  sb->addPermanentWidget(reload_button_);
  progress_ = new QProgressBar;
  progress_->setRange(0, 100);
  progress_->setFixedWidth(160);
  progress_->hide();
  cancel_ = make_button(tr("Cancel"), "ghost");
  cancel_->hide();
  connect(cancel_, &QPushButton::clicked, controller_.get(), &AppController::cancel);
  sb->addPermanentWidget(progress_);
  sb->addPermanentWidget(cancel_);
  auto* undo = make_tool_button(actions_["undo"], nullptr, theme::icon_size::sm);
  auto* redo = make_tool_button(actions_["redo"], nullptr, theme::icon_size::sm);
  sb->addPermanentWidget(undo);
  sb->addPermanentWidget(redo);
  log_button_ = new QToolButton;
  log_button_->setProperty("kind", "text");
  log_button_->setIcon(icon(Icon::Log));
  log_button_->setIconSize(QSize(16, 16));
  log_button_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  log_button_->setText(tr("Log"));
  log_button_->setCheckable(true);
  log_button_->setAutoRaise(true);
  log_button_->setToolTip(tr("Operation log (Ctrl+L)"));
  log_button_->setCursor(Qt::PointingHandCursor);
  connect(log_button_, &QToolButton::clicked, this, [this](bool on) { docks_["log"]->setVisible(on); });
  connect(docks_["log"], &QDockWidget::visibilityChanged, this, [this](bool on) {
    log_button_->setChecked(docks_["log"]->isVisibleTo(this));
    if (on) {
      unseen_warnings_ = 0;
      log_button_->setText(tr("Log"));
    }
  });
  sb->addPermanentWidget(log_button_);
  status_pill_ = new QToolButton;
  status_pill_->setProperty("kind", "text");
  status_pill_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  status_pill_->setIconSize(QSize(16, 16));
  status_pill_->setAutoRaise(true);
  status_pill_->setCursor(Qt::PointingHandCursor);
  connect(status_pill_, &QToolButton::clicked, this, [this] {
    const ValidationStatus st = controller_->view().status.status;
    if (st == ValidationStatus::NeedsReview || st == ValidationStatus::Fail) openFirstIssue();
    else {
      docks_["inspector"]->show();
      clearSelection();
    }
  });
  sb->addPermanentWidget(status_pill_);
}

void MainWindow::buildViewportOverlays() {
  // Camera bar, top left.
  auto* cams = new QFrame;
  cams->setProperty("overlay", true);
  auto* ch = new QHBoxLayout(cams);
  ch->setContentsMargins(4, 4, 4, 4);
  ch->setSpacing(2);
  for (const char* k : {"view_0", "view_2", "view_4", "view_iso"}) {
    auto* b = make_tool_button(actions_[k], cams, theme::icon_size::sm);
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    b->setObjectName(QStringLiteral("cam_%1").arg(k));
    ch->addWidget(b);
  }
  auto* more = new QToolButton(cams);
  more->setIcon(icon(Icon::More));
  more->setIconSize(QSize(16, 16));
  more->setAutoRaise(true);
  more->setToolTip(tr("Back, Right and Bottom views; orthographic; mouse controls"));
  more->setPopupMode(QToolButton::InstantPopup);
  auto* more_menu = new QMenu(more);
  for (const char* k : {"view_1", "view_3", "view_5"}) more_menu->addAction(actions_[k]);
  more_menu->addSeparator();
  more_menu->addAction(actions_["ortho"]);
  more->setMenu(more_menu);
  ch->addWidget(more);
  viewport_->addOverlay(cams, Qt::AlignLeft | Qt::AlignTop);

  // Display toggles, top right.
  auto* disp = new QFrame;
  disp->setProperty("overlay", true);
  auto* dh = new QHBoxLayout(disp);
  dh->setContentsMargins(4, 4, 4, 4);
  dh->setSpacing(2);
  for (const char* k : {"textured", "wireframe", "lighting", "pixel_preview", "normals", "grid", "overlay", "show_issues",
                        "show_estimated", "turntable"})
    dh->addWidget(make_tool_button(actions_[k], disp, theme::icon_size::sm + 2));
  viewport_->addOverlay(disp, Qt::AlignRight | Qt::AlignTop);

  // Tool options, under the camera bar: only what the active tool needs.
  tool_options_ = new QFrame;
  tool_options_->setProperty("overlay", true);
  auto* th = new QHBoxLayout(tool_options_);
  th->setContentsMargins(10, 4, 4, 4);
  th->setSpacing(theme::space::sm);
  tool_hint_ = make_label(QString(), "small");
  th->addWidget(tool_hint_);
  mode_object_ = make_button(tr("Object"), "segment");
  mode_parts_ = make_button(tr("Parts"), "segment");
  mode_faces_ = make_button(tr("Faces"), "segment");
  auto* modes = new QButtonGroup(tool_options_);
  for (QPushButton* b : {mode_object_, mode_parts_, mode_faces_}) {
    b->setCheckable(true);
    modes->addButton(b);
    th->addWidget(b);
  }
  mode_parts_->setChecked(true);
  mode_object_->setToolTip(tr("Click selects the whole model"));
  mode_parts_->setToolTip(tr("Click selects whole parts"));
  mode_faces_->setToolTip(tr("Click selects single faces"));
  connect(mode_faces_, &QPushButton::toggled, this, [this](bool on) {
    face_mode_ = on;
    editor_->setSelectMode(on ? SelectMode::Face : mode_object_->isChecked() ? SelectMode::Object : SelectMode::Part);
    if (sel_tri_ >= 0 && !on) clearSelection();
    updateHighlight();
  });
  connect(mode_object_, &QPushButton::toggled, this, [this](bool on) {
    if (on) editor_->setSelectMode(SelectMode::Object);
  });
  connect(mode_parts_, &QPushButton::toggled, this, [this](bool on) {
    if (on) editor_->setSelectMode(SelectMode::Part);
  });
  // Depth brush.
  depth_opts_ = new QWidget;
  {
    auto* h = new QHBoxLayout(depth_opts_);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(theme::space::xs);
    depth_raise_ = make_button(tr("Raise"), "segment");
    depth_lower_ = make_button(tr("Lower"), "segment");
    auto* g = new QButtonGroup(depth_opts_);
    for (QPushButton* b : {depth_raise_, depth_lower_}) {
      b->setCheckable(true);
      g->addButton(b);
      h->addWidget(b);
    }
    depth_raise_->setToolTip(tr("Paint the surface toward you"));
    depth_lower_->setToolTip(tr("Paint the surface into the model"));
    depth_size_ = small_spin(0, 32, 0.5, editor_->depth_brush.radius, tr(" px"), tr("Brush radius"));
    depth_strength_ = small_spin(0.5, 64, 0.5, editor_->depth_brush.strength, tr(" deep"), tr("Depth change at the brush centre"));
    depth_selected_ = new QCheckBox(tr("Selected part only"));
    h->addWidget(depth_size_);
    h->addWidget(depth_strength_);
    h->addWidget(depth_selected_);
    auto apply = [this] {
      if (updating_options_) return;
      editor_->depth_brush.raise = depth_raise_->isChecked();
      editor_->depth_brush.radius = depth_size_->value();
      editor_->depth_brush.strength = depth_strength_->value();
      editor_->depth_selected_only = depth_selected_->isChecked();
      editor_->touch();
    };
    connect(depth_raise_, &QPushButton::toggled, this, apply);
    connect(depth_size_, &QDoubleSpinBox::valueChanged, this, apply);
    connect(depth_strength_, &QDoubleSpinBox::valueChanged, this, apply);
    connect(depth_selected_, &QCheckBox::toggled, this, apply);
  }
  th->addWidget(depth_opts_);
  // Push/pull.
  push_opts_ = new QWidget;
  {
    auto* h = new QHBoxLayout(push_opts_);
    h->setContentsMargins(0, 0, 0, 0);
    push_snap_ = new QCheckBox(tr("Whole pixels"));
    push_bounds_ = new QCheckBox(tr("Stay inside the drawings"));
    push_bounds_->setToolTip(tr("Pulling past the drawings' outline would change a silhouette"));
    h->addWidget(push_snap_);
    h->addWidget(push_bounds_);
    auto apply = [this] {
      if (updating_options_) return;
      editor_->snap = push_snap_->isChecked();
      editor_->respect_bounds = push_bounds_->isChecked();
      actions_["snap"]->setChecked(editor_->snap);
      editor_->touch();
    };
    connect(push_snap_, &QCheckBox::toggled, this, apply);
    connect(push_bounds_, &QCheckBox::toggled, this, apply);
  }
  th->addWidget(push_opts_);
  // Part masks.
  mask_opts_ = new QWidget;
  {
    auto* h = new QHBoxLayout(mask_opts_);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(theme::space::xs);
    mask_part_ = new QComboBox;
    mask_part_->setMinimumWidth(120);
    mask_part_->setToolTip(tr("The part whose pixels you paint"));
    auto* add = make_button(tr("New…"), "ghost");
    add->setToolTip(tr("A new part painted on the drawings"));
    mask_size_ = small_spin(0, 32, 0.5, editor_->mask_radius, tr(" px"), tr("Brush radius"));
    mask_erase_ = new QCheckBox(tr("Erase"));
    h->addWidget(mask_part_);
    h->addWidget(add);
    h->addWidget(mask_size_);
    h->addWidget(mask_erase_);
    connect(add, &QPushButton::clicked, this, [this] { newPart(true); });
    auto apply = [this] {
      if (updating_options_) return;
      editor_->mask_part = mask_part_->currentData().isValid() ? mask_part_->currentData().toInt() : -1;
      editor_->mask_radius = mask_size_->value();
      editor_->mask_erase = mask_erase_->isChecked();
      editor_->touch();
    };
    connect(mask_part_, &QComboBox::currentIndexChanged, this, apply);
    connect(mask_size_, &QDoubleSpinBox::valueChanged, this, apply);
    connect(mask_erase_, &QCheckBox::toggled, this, apply);
  }
  th->addWidget(mask_opts_);
  viewport_->addOverlay(tool_options_, Qt::AlignLeft | Qt::AlignTop, QPoint(0, 48));

  // Banner, top centre: single-drawing projects and the edit being previewed.
  banner_ = new QFrame;
  banner_->setProperty("hint", true);
  auto* bh = new QHBoxLayout(banner_);
  bh->setContentsMargins(12, 4, 12, 4);
  banner_text_ = make_label(QString(), "small");
  banner_text_->setProperty("tone", "review");
  bh->addWidget(banner_text_);
  viewport_->addOverlay(banner_, Qt::AlignHCenter | Qt::AlignTop, QPoint(0, 48));
  banner_->hide();

  // Next-step hint, bottom centre.
  hint_ = new QFrame;
  hint_->setProperty("hint", true);
  auto* hh = new QHBoxLayout(hint_);
  hh->setContentsMargins(14, 6, 6, 6);
  hh->setSpacing(theme::space::sm + 2);
  hint_icon_ = new QLabel;
  hint_icon_->setFixedSize(16, 16);
  hint_text_ = new QLabel;
  hint_button_ = make_button(QString(), "primary");
  hint_button_->setMinimumHeight(28);
  hh->addWidget(hint_icon_);
  hh->addWidget(hint_text_);
  hh->addWidget(hint_button_);
  connect(hint_button_, &QPushButton::clicked, this, [this] { runAction(hint_action_); });
  viewport_->addOverlay(hint_, Qt::AlignHCenter | Qt::AlignBottom);
  setTool(Tool::Select);
}

void MainWindow::connectEditing() {
  AppController* c = controller_.get();
  // Drawing preview.
  connect(views_, &ViewsPanel::strokeChanged, this, &MainWindow::onStroke);
  connect(views_, &ViewsPanel::pixelClicked, this, &MainWindow::onPixelClicked);
  connect(views_, &ViewsPanel::regionChosen, this, &MainWindow::onRegion);
  // Viewport.
  connect(viewport_, &Viewport::strokeChanged, this, &MainWindow::onStroke);
  connect(viewport_, &Viewport::pushPullChanged, this, &MainWindow::onPushPull);
  connect(viewport_, &Viewport::strokeCancelled, this, &MainWindow::cancelPreview);
  connect(viewport_, &Viewport::pushPullCancelled, this, &MainWindow::cancelPreview);
  connect(viewport_, &Viewport::hint, this, [this](const QString& t) { statusBar()->showMessage(t, 5000); });
  connect(c, &AppController::previewReady, this, [this](std::shared_ptr<const Mesh> mesh, const QString& summary) {
    preview_ = mesh ? flat_mesh(*mesh) : nullptr;
    preview_text_ = mesh ? summary : QString();
    if (!mesh && !summary.isEmpty()) statusBar()->showMessage(tr("Preview: %1").arg(summary), 5000);
    updateHighlight(false);
    updateBanner();
  });
  connect(c, &AppController::selectionResult, this, &MainWindow::onSelectionResult);
  connect(editor_, &EditorState::changed, this, [this] {
    const QString keys[] = {"tool_select", "tool_depth", "tool_pushpull", "tool_volume", "tool_parts"};
    const QString key = keys[static_cast<int>(editor_->tool())];
    if (!actions_[key]->isChecked()) actions_[key]->setChecked(true);
    if (actions_["snap"]->isChecked() != editor_->snap) actions_["snap"]->setChecked(editor_->snap);
    updateToolOptions();
  });
}

// ---- layout -------------------------------------------------------------------------

void MainWindow::resetLayout() {
  for (auto& [n, d] : docks_) {
    d->setFloating(false);
    removeDockWidget(d);
  }
  addDockWidget(Qt::LeftDockWidgetArea, docks_["views"]);
  addDockWidget(Qt::RightDockWidgetArea, docks_["inspector"]);
  addDockWidget(Qt::BottomDockWidgetArea, docks_["history"]);
  addDockWidget(Qt::BottomDockWidgetArea, docks_["log"]);
  tabifyDockWidget(docks_["history"], docks_["log"]);
  docks_["views"]->setVisible(!start_visible_);
  docks_["inspector"]->setVisible(!start_visible_);
  docks_["history"]->setVisible(!start_visible_);
  docks_["log"]->hide();
  if (start_visible_) docks_before_start_ = {"views", "inspector", "history"};
  resizeDocks({docks_["views"], docks_["inspector"]}, {theme::metric::left_dock, theme::metric::right_dock}, Qt::Horizontal);
  resizeDocks({docks_["history"]}, {150}, Qt::Vertical);
  toolbar_->setVisible(!start_visible_);
}

void MainWindow::saveLayout() {
  QSettings s;
  s.setValue("ui/geometry", saveGeometry());
  const QByteArray state = start_visible_ ? workspace_state_ : saveState(kLayoutVersion);
  if (!state.isEmpty()) s.setValue("ui/state", state);
}

bool MainWindow::startScreenVisible() const { return start_visible_; }

void MainWindow::setStartScreen(bool on) {
  if (on == start_visible_ && center_->currentWidget() == (on ? static_cast<QWidget*>(start_) : viewport_)) return;
  if (on && !start_visible_) workspace_state_ = saveState(kLayoutVersion);
  start_visible_ = on;
  center_->setCurrentWidget(on ? static_cast<QWidget*>(start_) : viewport_);
  if (on) {
    docks_before_start_.clear();
    for (auto& [n, d] : docks_)
      if (d->isVisibleTo(this)) docks_before_start_.insert(n);
    for (auto& [n, d] : docks_) d->hide();
    toolbar_->hide();
  } else {
    for (auto& [n, d] : docks_)
      if (docks_before_start_.contains(n)) d->show();
    toolbar_->show();
    workspace_seen_ = true;
  }
  updateGuidance();
}

void MainWindow::showWorkspace() { setStartScreen(false); }

void MainWindow::resizeEvent(QResizeEvent* e) {
  QMainWindow::resizeEvent(e);
  const bool compact = width() < theme::metric::compact_width;
  if (compact != toolbar_compact_) {
    toolbar_compact_ = compact;
    toolbar_->setToolButtonStyle(compact ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextUnderIcon);
    for (QToolButton* b : viewport_->findChildren<QToolButton*>())
      if (b->objectName().startsWith("cam_"))
        b->setToolButtonStyle(compact ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
  }
}

// ---- actions --------------------------------------------------------------------------

std::optional<QString> MainWindow::askText(const QString& title, const QString& label, const QString& value) {
  return ask_text_ ? ask_text_(title, label, value) : std::nullopt;
}

int MainWindow::selectedPart() const { return sel_parts_.empty() ? -1 : *sel_parts_.begin(); }

void MainWindow::runAction(const QString& key) {
  if (key == "image3d") {
    emit imageTo3DRequested();
    return;
  }
#ifdef PIXEL3D_ENABLE_SMARTQUAD
  if (key == "smart_quad") {
    auto* dialog = findChild<SmartQuadDialog*>("SmartQuadDialog", Qt::FindDirectChildrenOnly);
    if (!dialog) {
      dialog = new SmartQuadDialog(this);
      dialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    return;
  }
#endif
  AppController* c = controller_.get();
  if (key == "import") {
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Import drawings (Front, Side, Top, …)"), {},
                                                            source_image_filter());
    if (!files.isEmpty()) importFiles(files);
  } else if (key == "run_all") {
    if (actions_["run_all"]->isEnabled()) actions_["run_all"]->trigger();
  } else if (key == "validate") {
    if (actions_["validate"]->isEnabled()) actions_["validate"]->trigger();
  } else if (key == "review") {
    showWorkspace();
    openFirstIssue();
  } else if (key == "lowpoly_suggest") {
    suggestLowpolyParts();
  } else if (key == "lowpoly_build") {
    c->buildLowpoly({});
  } else if (key == "switch_lowpoly") {
    switchProjectType(true);
  } else if (key == "export" && c->view().doc.settings.lowpoly.enabled) {
    QString suggestion = controller_->projectPath();
    suggestion = suggestion.isEmpty() ? QDir::home().filePath(qs(c->view().doc.settings.name) + ".glb")
                                      : QFileInfo(suggestion).path() + "/" + QFileInfo(suggestion).completeBaseName() + ".glb";
    const QString path = ask_save_path_ ? ask_save_path_(suggestion)
                                        : QFileDialog::getSaveFileName(this, tr("Export low-poly character"), suggestion,
                                                                       tr("glTF binary (*.glb)"));
    if (!path.isEmpty()) exportTo(path);
  } else if (key == "export") {
    if (visualHullBlocked()) return;
    const std::unique_ptr<ExportDialog> dlg = newExportDialog();
    if (dlg->exec() != QDialog::Accepted) return;
    const ModelExportRequest r = dlg->request();
    QSettings().setValue("export/preset", static_cast<int>(r.preset));
    exportModel(r);
  } else if (key == "settings") {
    openSettings();
  } else if (key == "part_new_define") {
    newPart(true);
  } else if (key == "part_new_label") {
    newPart(false);
  } else if (key.startsWith("part_mirror_copy_")) {
    if (selectedPart() >= 0) c->mirror(selectedPart(), key.right(1).toInt(), std::nullopt, true);
    else statusBar()->showMessage(tr("Select a part to copy first."), 5000);
  } else if (key.startsWith("part_mirror_")) {
    c->mirror(selectedPart(), key.right(1).toInt(), std::nullopt, false);  // no part selected: the whole model
  } else if (key == "part_cut") {
    if (!controller_->view().doc.align) return;
    PlaneDialog d(PlaneDialog::Kind::Cut, controller_->view().doc.align->dims, this);
    if (d.exec() == QDialog::Accepted) c->cut(selectedPart(), d.axis(), d.at(), d.keep());
  } else if (key == "part_merge") {
    if (sel_parts_.size() >= 2) c->mergeParts(std::vector<int>(sel_parts_.begin(), sel_parts_.end()));
    else statusBar()->showMessage(tr("Select two or more parts (Ctrl+click) to merge them."), 5000);
  } else if (key.startsWith("part_")) {
    const int p = selectedPart();
    if (p < 0) {
      statusBar()->showMessage(tr("Select a part first."), 5000);
      return;
    }
    if (key == "part_rename") {
      const auto& names = controller_->view().doc.part_names;
      if (auto n = askText(tr("Rename part"), tr("Name:"), names.contains(p) ? qs(names.at(p)) : QString())) c->renamePart(p, *n);
    } else if (key == "part_duplicate") {
      TransformRequest r;
      r.part = p;
      r.copy = true;
      r.translate = {2, 0, 0};
      c->transformPart(r);
    } else if (key == "part_transform") {
      TransformDialog d(p, false, this);
      if (d.exec() == QDialog::Accepted) c->transformPart(d.request());
    } else if (key == "part_split") {
      if (!controller_->view().doc.align) return;
      PlaneDialog d(PlaneDialog::Kind::Split, controller_->view().doc.align->dims, this);
      if (d.exec() == QDialog::Accepted) c->splitPart(p, d.axis(), d.at());
    } else if (key == "part_split_shells") {
      c->splitPartShells(p);
    } else if (key == "part_delete") {
      c->deletePart(p);
    } else if (key == "part_paint_masks") {
      editor_->mask_part = p;
      setTool(Tool::Parts);
      editor_->touch();
    } else if (key == "part_sync_masks") {
      c->syncPartMasks(editor_->mask_part >= 0 ? editor_->mask_part : p);
    }
  } else if (hasAction(key) && actions_[key]->isEnabled()) {
    actions_[key]->trigger();
  }
}

void MainWindow::openSettings() {
  NewProjectDialog d(this);
  d.setWindowTitle(tr("Project settings"));
  d.setSettings(controller_->view().doc.settings);
  if (d.exec() != QDialog::Accepted) return;
  applySettings(d.settings());
}

void MainWindow::applySettings(const ProjectSettings& s) {
  SettingsUpdate u;
  u.name = s.name;
  u.pixels_per_meter = s.pixels_per_meter;
  u.profile = s.profile;
  u.normalize.enabled = s.normalize.enabled;
  u.normalize.target_size = s.normalize.target_size;
  u.normalize.bg_tolerance = s.normalize.bg_tolerance;
  u.style = s.style;
  controller_->updateSettings(u);
  // The project type (visual hull or Low-Poly Studio) re-aligns the views: its own undoable step.
  if (s.lowpoly.enabled != controller_->view().doc.settings.lowpoly.enabled) {
    LowpolyUpdate lu;
    lu.enabled = s.lowpoly.enabled;
    controller_->setLowpolySettings(lu);
  }
}

void MainWindow::switchProjectType(bool lowpoly) {
  if (controller_->view().doc.settings.lowpoly.enabled == lowpoly) {
    updateActions();  // the menu's check marks follow the project
    return;
  }
  LowpolyUpdate u;
  u.enabled = lowpoly;
  controller_->setLowpolySettings(u);
  statusBar()->showMessage(
      tr("Project type: %1 — your drawings are kept (Ctrl+Z switches back)").arg(project_type_name(lowpoly)), 10000);
}

bool MainWindow::visualHullBlocked() {
  const Document& d = controller_->view().doc;
  const std::vector<ViewRole> missing = missing_views(d);
  if (d.views.empty() || missing.empty()) return false;
  last_error_ = "view.missing_required";
  if (!lowpoly_can_rescue(d)) {
    statusBar()->showMessage(tr("Add the Front drawing first: every model starts from it."), 10000);
    return true;
  }
  // Not a failure: say what is missing and offer the drawings the user has a use.
  const ViewRole add = std::find(missing.begin(), missing.end(), ViewRole::Top) != missing.end() ? ViewRole::Top : missing.front();
  const bool has_parts = !d.masks.parts.empty();
  switch (ask_missing_views_(missing_views_text(d), add)) {
    case MissingViewsChoice::SwitchToLowpoly:
      switchProjectType(true);
      setStartScreen(false);
      runAction(has_parts ? "lowpoly_build" : "lowpoly_suggest");  // what Reconstruct means in Low-Poly Studio
      break;
    case MissingViewsChoice::AddView: browseRole(add); break;
    case MissingViewsChoice::Cancel: break;
  }
  return true;
}

void MainWindow::newSingleView(const QString& path) {
  if (!maybeSave()) return;
  SingleViewDialog d(this);
  if (!path.isEmpty()) d.setPath(path);
  if (d.exec() != QDialog::Accepted || d.path().isEmpty()) return;
  clearSelection();
  controller_->newSingleViewProject(d.project(), d.path(), d.role(), d.settings());
  QSettings().setValue(settings_key_for(d.path()), qs(to_string(d.role())));
}

void MainWindow::browseRole(ViewRole r) {
  const QString f = QFileDialog::getOpenFileName(this, tr("%1 drawing (%2)").arg(view_title(r), view_orientation(r)), {},
                                                 source_image_filter());
  if (!f.isEmpty()) importFiles({f}, r);
}

void MainWindow::importFiles(const QStringList& files, std::optional<ViewRole> role) {
  std::vector<ViewImport> views;
  QSettings settings;
  for (const QString& f : files) {
    // The slot it was dropped on, the file name (front.png, side.png, …), the role chosen for this file before, or ask.
    std::optional<ViewRole> r = files.size() == 1 && role ? role : role_from_filename(f);
    if (!r) r = view_role_from_string(utf8(settings.value(settings_key_for(f)).toString()));
    if (!r) {
      QStringList roles;
      for (ViewRole x : kAllViewRoles) roles << QStringLiteral("%1 — %2").arg(view_title(x), view_orientation(x));
      bool ok = false;
      const QString pick = QInputDialog::getItem(this, tr("Which view is this?"),
                                                 tr("%1 shows the model…").arg(QFileInfo(f).fileName()), roles, 0, false, &ok);
      if (!ok) continue;
      r = kAllViewRoles[roles.indexOf(pick)];
    }
    settings.setValue(settings_key_for(f), qs(to_string(*r)));
    views.push_back(ViewImport{*r, to_path(f), std::nullopt});
  }
  if (!views.empty()) controller_->importViews(views);
}

QStringList MainWindow::reviewLines(const json& why) const {
  QStringList lines;
  for (const auto& c : why.value("blocking_conflicts", json::array())) {
    const QString kind = qs(c.value("kind", std::string()));
    QString title = qs(c.value("text", std::string()));
    for (const Issue& i : issues_)
      if (i.kind == kind) title = i.title;
    if (!lines.contains("• " + title)) lines << "• " + title;
  }
  for (const auto& c : why.value("failed_critical_checks", json::array()))
    if (c.is_string()) lines << "• " + check_title(qs(c.get<std::string>()));
  return lines;
}

std::unique_ptr<ExportDialog> MainWindow::newExportDialog() {
  QString suggestion = controller_->projectPath();
  if (!suggestion.isEmpty()) {
    suggestion = QFileInfo(suggestion).path() + "/" + QFileInfo(suggestion).completeBaseName() + ".glb";
  } else {
    const QString name = qs(controller_->view().doc.settings.name);
    suggestion = QDir::homePath() + "/" + (name.isEmpty() ? QStringLiteral("model") : name) + ".glb";
  }
  const Document& d = controller_->view().doc;
  auto dlg = std::make_unique<ExportDialog>(suggestion, d.align ? d.align->dims : Dims{1, 1, 1},
                                            d.settings.pixels_per_meter, this);
  const int saved = QSettings().value("export/preset", 0).toInt();
  if (saved > 0) dlg->setPreset(static_cast<EnginePreset>(saved));
  return dlg;
}

void MainWindow::exportTo(const QString& path) {
  ModelExportRequest r;
  r.path = to_path(path);
  exportModel(r);
}

void MainWindow::suggestLowpolyParts() {
  // Proposals first (nothing changes), then the user's confirmation, then one undoable step.
  controller_->suggestLowpolyParts({}, [this](const OpResult& r) {
    if (!r.success) return;
    QStringList lines;
    for (const auto& p : r.data.value("parts", json::array()))
      lines << QStringLiteral("• %1 — %2").arg(qs(p.value("name", std::string())), qs(p.value("type", std::string())));
    const bool replace = !controller_->view().doc.masks.parts.empty();
    const QString text = tr("3re suggests %1 parts for a %2 body plan:\n%3\n\n%4")
                             .arg(lines.size())
                             .arg(qs(r.data.value("template", std::string())))
                             .arg(lines.join("\n"))
                             .arg(replace ? tr("Your current parts will be replaced (you can undo). Use the suggestion?")
                                          : tr("You can change every type afterwards. Use the suggestion?"));
    if (!confirm_(tr("Suggested parts"), text)) return;
    controller_->suggestLowpolyParts({.apply = true, .replace = replace});
  });
}

void MainWindow::openDrawingsAsLowpoly(const QStringList& files) {
  QStringList drawings;
  for (const QString& f : files)
    if (is_source_image(f)) drawings << f;
  if (drawings.isEmpty()) {
    statusBar()->showMessage(tr("Open a project (.p3d) or drawings (%1).").arg(source_image_extensions().join(", ")), 8000);
    return;
  }
  ProjectSettings ps;
  ps.name = utf8(QFileInfo(drawings.front()).completeBaseName());
  ps.lowpoly.enabled = true;
  controller_->newProject(ps);
  clearSelection();
  setStartScreen(false);  // straight to the workspace: the Low-Poly page takes it from here
  // One drawing is the Front (front only); several are placed by name or asked for.
  importFiles(drawings, drawings.size() == 1 ? std::optional<ViewRole>(ViewRole::Front) : std::nullopt);
  if (std::any_of(drawings.begin(), drawings.end(), [](const QString& f) { return is_layered_source(f); }))
    suggestLayerParts();
}

void MainWindow::suggestLayerParts() {
  // Proposal first (nothing changes), then the user's choice, then one undoable step.
  controller_->suggestLayerParts({}, [this](const OpResult& r) {
    if (!r.success) return;
    const json layers = r.data.value("layers", json::array());
    const auto keep = choose_layers_(layers);
    if (!keep || keep->isEmpty()) return;
    LayerPartsRequest req;
    req.apply = true;
    req.replace = !controller_->view().doc.masks.parts.empty();
    if (req.replace && !confirm_(tr("Replace the parts?"),
                                 tr("Your current parts will be replaced by the chosen layers (you can undo). Continue?")))
      return;
    for (const QString& k : *keep) req.accept.push_back(utf8(k));
    controller_->suggestLayerParts(req);
  });
}

void MainWindow::setLowpolyPose(bool posed) {
  lowpoly_pose_ = posed;
  inspector_->setLowpolyPose(posed);
  viewport_->setScene(scene_from(controller_->view().doc, lowpoly_pose_));
}

void MainWindow::exportModel(const ModelExportRequest& req) {
  if (controller_->view().doc.settings.lowpoly.enabled) {
    // LOW-POLY STUDIO: the character's GLB (PASS, or NEEDS_REVIEW the user accepts; FAIL is never written).
    LowpolyExportRequest r;
    r.path = req.path;
    r.engine_files = true;
    const ValidationStatus st = controller_->view().status.status;
    if (st == ValidationStatus::Fail) {
      statusBar()->showMessage(tr("The low-poly character fails a check and cannot be exported."), 8000);
      last_error_ = "lowpoly.export_refused";
      return;
    }
    const json& why = controller_->view().status.validation;
    const bool accepted_depth = why.is_object() && why.value("export_allowed", false);
    if (st != ValidationStatus::Pass && !accepted_depth) {
      if (!confirm_(tr("Export a character that needs review?"),
                    tr("The low-poly character is NEEDS REVIEW:\n%1\n\nExport it anyway?")
                        .arg(reviewLines(controller_->view().status.validation).join("\n")))) {
        last_error_ = "export.needs_review";
        return;
      }
      r.allow_needs_review = true;
    }
    pending_export_ = from_path(r.path);
    controller_->exportLowpoly(r);
    return;
  }
  ModelExportRequest r = req;
  const ValidationStatus st = controller_->view().status.status;
  if (st == ValidationStatus::NeedsReview || st == ValidationStatus::Fail) {
    const QString word = st == ValidationStatus::Fail ? tr("FAIL") : tr("NEEDS REVIEW");
    if (!confirm_(tr("Export a model that is %1?").arg(word),
                  tr("The model is %1:\n%2\n\nExport it anyway?").arg(word, reviewLines(controller_->view().status.validation).join("\n"))))
      return;
    r.allow_needs_review = true;
    r.allow_fail = st == ValidationStatus::Fail;
  }
  pending_export_ = from_path(r.path);
  pending_request_ = r;
  controller_->exportModel(r);
}

void MainWindow::openProjectFile(const QString& path) { controller_->openProject(path); }

void MainWindow::saveProjectFile(const QString& path) { controller_->saveProject(path); }

// ---- selection --------------------------------------------------------------------------

void MainWindow::openIssue(const QString& key) {
  const Issue* is = nullptr;
  for (const Issue& i : issues_)
    if (i.key() == key) is = &i;
  if (!is) return;
  showWorkspace();
  docks_["inspector"]->show();
  open_issue_ = key;
  sel_tri_ = -1;
  sel_parts_ = std::set<int>(is->parts.begin(), is->parts.end());
  hover_candidates_.clear();
  inspector_->showIssue(key);
  if (is->view) views_->setRole(*is->view);
  else if (!is->candidates.empty()) views_->setRole(ViewRole::Top);
  if (!is->regions.empty()) viewport_->focusOn(bounds(is->regions));
  updateHighlight();
  updateGuidance();
  syncSessionSelection();
}

bool MainWindow::openFirstIssue() {
  for (const Issue& i : issues_)
    if (i.level == IssueLevel::Blocking) {
      openIssue(i.key());
      return true;
    }
  return false;
}

void MainWindow::selectPart(int part, bool frame) {
  showWorkspace();
  open_issue_.clear();
  sel_tri_ = -1;
  sel_parts_ = {part};
  inspector_->showPart(part);
  if (frame) {
    const Scene& s = viewport_->scene();
    std::vector<bool> member(s.triangles(), false);
    for (size_t t = 0; t < member.size() && t < s.tri_part.size(); ++t) member[t] = s.tri_part[t] == part;
    viewport_->focusOn(triangles_bounds(s, member));
  }
  updateHighlight();
  updateGuidance();
  syncSessionSelection();
}

void MainWindow::selectFace(int triangle) {
  open_issue_.clear();
  sel_parts_.clear();
  sel_tri_ = triangle;
  inspector_->showFace(triangle);
  updateHighlight();
  updateGuidance();
  syncSessionSelection();
}

void MainWindow::clearSelection() {
  open_issue_.clear();
  sel_parts_.clear();
  sel_tri_ = -1;
  hover_candidates_.clear();
  inspector_->showOverview();
  updateHighlight();
  updateGuidance();
  syncSessionSelection();
}

// The engine keeps the same selection as the window (MCP's get_selection, selection-based edits).
void MainWindow::syncSessionSelection() {
  if (controller_->view().doc.views.empty()) return;
  const Scene& s = viewport_->scene();
  if (sel_tri_ >= 0 && s.mesh && sel_tri_ < static_cast<int>(s.triangles())) {
    // A ray onto the face's own triangle, from just in front of it.
    const TexturedMesh& m = *s.mesh;
    const vec3d a = m.positions[m.indices[3 * sel_tri_]], b = m.positions[m.indices[3 * sel_tri_ + 1]],
                c = m.positions[m.indices[3 * sel_tri_ + 2]];
    const vec3d n = normalize(cross(b - a, c - a)), centre = (a + b + c) / 3.0;
    controller_->selectAtRay(centre + n * 0.01, n * -1.0, SelectMode::Face);
  } else if (!sel_parts_.empty()) {
    controller_->selectParts(std::vector<int>(sel_parts_.begin(), sel_parts_.end()), SelectMode::Part);
  } else if (!controller_->view().selection.empty()) {
    controller_->clearSelection();
  }
}

void MainWindow::onSelectionResult(const OpResult& r) {
  if (!r.success && !r.errors.empty()) statusBar()->showMessage(qs(r.errors.front().text), 5000);
  if (!select_from_drawing_) return;
  select_from_drawing_ = false;
  // A click on a drawing: the engine resolved the pixel; show its answer.
  const Selection& sel = controller_->view().selection;
  if (sel.mode == SelectMode::Face && sel.hit && sel.hit->tri >= 0) {
    open_issue_.clear();
    sel_parts_.clear();
    sel_tri_ = sel.hit->tri;
    inspector_->showFace(sel_tri_);
  } else if (!sel.parts.empty()) {
    open_issue_.clear();
    sel_tri_ = -1;
    sel_parts_ = std::set<int>(sel.parts.begin(), sel.parts.end());
    if (sel.parts.size() == 1) inspector_->showPart(sel.parts.front());
  } else {
    sel_parts_.clear();
    sel_tri_ = -1;
    inspector_->showOverview();
  }
  updateHighlight();
  updateGuidance();
}

void MainWindow::frameSelection() {
  const Scene& s = viewport_->scene();
  if (s.empty()) return;
  for (const Issue& i : issues_)
    if (i.key() == open_issue_ && !i.regions.empty()) {
      viewport_->focusOn(bounds(i.regions));
      return;
    }
  std::vector<bool> member(s.triangles(), false);
  bool any = false;
  const int region = sel_tri_ >= 0 && sel_tri_ < static_cast<int>(s.tri_region.size()) ? s.tri_region[sel_tri_] : -1;
  for (size_t t = 0; t < member.size(); ++t) {
    member[t] = sel_tri_ >= 0 ? (static_cast<int>(t) == sel_tri_ || (region >= 0 && s.tri_region[t] == region))
                              : t < s.tri_part.size() && sel_parts_.contains(s.tri_part[t]);
    any |= member[t];
  }
  if (!any) member.assign(member.size(), true);
  viewport_->focusOn(triangles_bounds(s, member));
}

void MainWindow::setTool(Tool t) {
  const Tool before = editor_->tool();
  editor_->setTool(t);
  const char* keys[] = {"tool_select", "tool_depth", "tool_pushpull", "tool_volume", "tool_parts"};
  const char* key = keys[static_cast<int>(t)];
  if (!actions_[key]->isChecked()) actions_[key]->setChecked(true);
  if (t == Tool::Parts && before != Tool::Parts && !start_visible_) clearSelection();
  if (t != Tool::Depth && t != Tool::Parts) views_->setPending({}, {});
  updateToolOptions();
  updateHighlight();
}

void MainWindow::updateToolOptions() {
  if (!tool_hint_) return;
  updating_options_ = true;
  const Tool tool = editor_->tool();
  int depth_issues = 0;
  for (const Issue& i : issues_) depth_issues += i.fix == IssueFix::SetDepth;
  QString hint;
  switch (tool) {
    case Tool::Select: hint = tr("Click to select"); break;
    case Tool::Depth:
      hint = depth_issues > 0 ? tr("Paint on a drawing · amber areas need a depth") : tr("Paint on a drawing (or in Front/Side/Top)");
      break;
    case Tool::PushPull: hint = tr("Drag a face"); break;
    case Tool::Volume: hint = tr("Drag a rectangle on a drawing (left panel)"); break;
    case Tool::Parts: hint = editor_->mask_part >= 0 ? tr("Paint the part's pixels on a drawing") : tr("Click a part to inspect it"); break;
  }
  tool_hint_->setText(hint);
  for (QPushButton* b : {mode_object_, mode_parts_, mode_faces_}) b->setVisible(tool == Tool::Select);
  depth_opts_->setVisible(tool == Tool::Depth);
  depth_raise_->setChecked(editor_->depth_brush.raise);
  depth_lower_->setChecked(!editor_->depth_brush.raise);
  depth_size_->setValue(editor_->depth_brush.radius);
  depth_strength_->setValue(editor_->depth_brush.strength);
  depth_selected_->setChecked(editor_->depth_selected_only);
  push_opts_->setVisible(tool == Tool::PushPull);
  push_snap_->setChecked(editor_->snap);
  push_bounds_->setChecked(editor_->respect_bounds);
  mask_opts_->setVisible(tool == Tool::Parts);
  mask_part_->clear();
  mask_part_->addItem(tr("(inspect only)"), QVariant());
  const Document& d = controller_->view().doc;
  for (const auto& [id, up] : d.masks.parts) {
    const auto n = d.part_names.find(id);
    mask_part_->addItem(n != d.part_names.end() ? qs(n->second) : tr("Part %1").arg(id), id);
  }
  if (editor_->mask_part >= 0 && !d.masks.parts.contains(editor_->mask_part)) editor_->mask_part = -1;
  mask_part_->setCurrentIndex(editor_->mask_part >= 0 ? mask_part_->findData(editor_->mask_part) : 0);
  mask_size_->setValue(editor_->mask_radius);
  mask_erase_->setChecked(editor_->mask_erase);
  tool_options_->setVisible(!viewport_->scene().empty());
  tool_options_->adjustSize();
  updating_options_ = false;
}

void MainWindow::updateBanner() {
  QStringList lines;
  const Document& doc = controller_->view().doc;
  if (doc.single_view.enabled) lines << tr("Single drawing — everything it does not show is estimated");
  // Low-Poly Studio from the Front drawing alone: every depth is estimated, never measured.
  if (doc.settings.lowpoly.enabled && !doc.views.empty() &&
      std::all_of(doc.views.begin(), doc.views.end(), [](const SourceView& sv) { return sv.role == ViewRole::Front; }))
    lines << (doc.settings.lowpoly.build.accept_inferred_depth
                  ? tr("Front only — every depth is INFERRED · NEEDS REVIEW · estimated depth accepted for export")
                  : tr("Front only — every depth is INFERRED · NEEDS REVIEW"));
  if (preview_) lines << (preview_text_.isEmpty() ? tr("Preview — release to apply") : preview_text_);
  banner_text_->setText(lines.join(QStringLiteral("  ·  ")));
  banner_->setVisible(!lines.isEmpty() && !start_visible_);
  banner_->adjustSize();
}

QString MainWindow::bannerText() const { return banner_->isVisible() ? banner_text_->text() : QString(); }

void MainWindow::onTriangleClicked(int tri, Qt::KeyboardModifiers mods) {
  const Scene& s = viewport_->scene();
  if (tri < 0 || tri >= static_cast<int>(s.triangles())) {
    clearSelection();
    return;
  }
  const int part = tri < static_cast<int>(s.tri_part.size()) ? s.tri_part[tri] : -1;
  const bool undecided = tri < static_cast<int>(s.tri_flags.size()) && (s.tri_flags[tri] & kTriUndecided);
  // Centre of the clicked triangle, for the region tests.
  QVector3D c;
  for (int k = 0; k < 3; ++k) {
    const vec3d& p = s.mesh->positions[s.mesh->indices[3 * tri + k]];
    c += QVector3D(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)) / 3.0f;
  }
  auto inside = [&](const Region3& r) {
    const QVector3D e(0.6f, 0.6f, 0.6f);
    const QVector3D lo = r.lo - e, hi = r.hi + e;
    return c.x() >= lo.x() && c.y() >= lo.y() && c.z() >= lo.z() && c.x() <= hi.x() && c.y() <= hi.y() && c.z() <= hi.z();
  };
  const Tool tool = editor_->tool();
  if (tool == Tool::Depth) {
    for (const Issue& i : issues_)
      if (i.fix == IssueFix::SetDepth)
        for (const Region3& r : i.regions)
          if (inside(r)) {
            openIssue(i.key());
            return;
          }
    tool_hint_->setText(tr("Paint depth on a drawing; amber areas need a depth"));
    return;
  }
  if (tool == Tool::Volume) {
    tool_hint_->setText(tr("Drag a rectangle on a drawing (left panel)"));
    return;
  }
  if ((tool == Tool::Select && face_mode_) || tool == Tool::PushPull) {
    selectFace(tri);
    return;
  }
  if (tool == Tool::Select && editor_->selectMode() == SelectMode::Object) {
    std::set<int> all(s.tri_part.begin(), s.tri_part.end());
    all.erase(-1);
    open_issue_.clear();
    sel_tri_ = -1;
    sel_parts_ = all;
    inspector_->showOverview();
    updateHighlight();
    syncSessionSelection();
    return;
  }
  if (tool == Tool::Select && undecided) {
    for (const Issue& i : issues_)
      if (i.fix == IssueFix::ChooseParts && std::find(i.parts.begin(), i.parts.end(), part) != i.parts.end()) {
        openIssue(i.key());
        return;
      }
  }
  if (part >= 0 && (mods & Qt::ControlModifier) && !sel_parts_.empty()) {
    // Ctrl+click adds or removes a part (merge needs several).
    if (sel_parts_.contains(part)) sel_parts_.erase(part);
    else sel_parts_.insert(part);
    sel_tri_ = -1;
    if (sel_parts_.size() == 1) inspector_->showPart(*sel_parts_.begin());
    updateHighlight();
    syncSessionSelection();
    return;
  }
  if (part >= 0) selectPart(part, false);
  else selectFace(tri);
}

void MainWindow::onTriangleHovered(int tri) {
  const Scene& s = viewport_->scene();
  auto target = [&](int t) {
    if (t < 0 || t >= static_cast<int>(s.triangles())) return -2;
    return (editor_->tool() == Tool::Select && face_mode_) || editor_->tool() == Tool::PushPull ? s.tri_region[t] : s.tri_part[t];
  };
  const bool same = target(tri) == target(hover_tri_);
  hover_tri_ = tri;
  viewport_->setCursor(tri >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
  if (!same) updateHighlight(false);
}

void MainWindow::updateHighlight(bool marks) {
  const Scene& s = viewport_->scene();
  const SessionView& v = controller_->view();
  Highlight h;
  h.parts = sel_parts_;
  if (sel_tri_ >= 0 && sel_tri_ < static_cast<int>(s.tri_region.size())) {
    h.region = s.tri_region[sel_tri_];
    h.parts.clear();
  }
  if (hover_tri_ >= 0 && hover_tri_ < static_cast<int>(s.triangles())) {
    if ((editor_->tool() == Tool::Select && face_mode_) || editor_->tool() == Tool::PushPull) h.hover_region = s.tri_region[hover_tri_];
    else h.hover_part = s.tri_part[hover_tri_];
  }
  const Issue* open = nullptr;
  for (const Issue& i : issues_)
    if (i.key() == open_issue_) open = &i;
  for (const Issue& i : issues_)
    if (i.level == IssueLevel::Blocking && i.fix == IssueFix::SetDepth && &i != open)
      h.issue_regions.insert(h.issue_regions.end(), i.regions.begin(), i.regions.end());
  if (open) h.issue_regions.insert(h.issue_regions.end(), open->regions.begin(), open->regions.end());
  for (int c : hover_candidates_) h.focus_regions.push_back(candidate_region(v.doc, c));
  h.preview = preview_;
  viewport_->setHighlight(h);
  if (!marks) return;
  views_->setSelection(h.parts, h.region);

  // The same areas on the drawings.
  ViewMarks all, focus, accent;
  std::map<ViewRole, int> counts;
  if (v.doc.align) {
    const Dims dims = v.doc.align->dims;
    std::vector<ViewRole> present;
    for (const SourceView& sv : v.doc.views) present.push_back(sv.role);
    auto mark = [&](const Issue& i, ViewMarks& into) {
      if (i.view && !i.box.isNull()) {
        into[*i.view].push_back(QRectF(i.box.left(), i.box.top(), i.box.width(), i.box.height()));
        return std::vector<ViewRole>{*i.view};
      }
      for (const Region3& r : i.regions)
        for (ViewRole role : present) into[role].push_back(view_rect(r, role, dims));
      return i.regions.empty() ? std::vector<ViewRole>{} : present;
    };
    for (const Issue& i : issues_)
      if (i.level == IssueLevel::Blocking)
        for (ViewRole r : mark(i, all)) ++counts[r];
    if (open) mark(*open, focus);
    for (int c : hover_candidates_) {
      const Region3 r = candidate_region(v.doc, c);
      for (ViewRole role : present) accent[role].push_back(view_rect(r, role, dims));
    }
  }
  views_->setMarks(all, focus, accent, counts);
}

// ---- editing input ------------------------------------------------------------------------

void MainWindow::cancelPreview() {
  controller_->clearPreview();
  views_->setPending({}, {});
}

void MainWindow::onStroke(ViewRole role, const QVector<QPointF>& points, bool finished) {
  const SessionView& v = controller_->view();
  if (!v.doc.align) {
    statusBar()->showMessage(tr("Reconstruct the model first (F5)."), 5000);
    return;
  }
  const ImageSize sz = view_image_size(role, v.doc.align->dims);
  const std::vector<vec2d> stroke = to_points(points);
  const theme::Colors& tc = theme::colors();
  if (editor_->tool() == Tool::Depth) {
    DepthPaintRequest r;
    r.view = role;
    r.stroke = stroke;
    r.brush = editor_->depth_brush;
    if (editor_->depth_selected_only && sel_parts_.size() == 1) r.part = *sel_parts_.begin();
    const std::vector<DepthSample> samples = depth_brush_samples(stroke, r.brush, sz.w, sz.h);
    if (!finished) {
      QVector<QPoint> px;
      for (const DepthSample& s : samples) px.push_back(QPoint(s.u, s.v));
      if (role == views_->role()) views_->setPending(px, theme::with_alpha(tc.accent, 0.45));
      controller_->previewDepth(r);
      return;
    }
    cancelPreview();
    if (samples.empty()) {
      statusBar()->showMessage(tr("The stroke changes no pixel (check strength and position)."), 5000);
      return;
    }
    controller_->paintDepth(r);
    return;
  }
  if (editor_->tool() == Tool::Parts) {
    if (editor_->mask_part < 0) return;
    BrushParams b;
    b.radius = editor_->mask_radius;
    if (!finished) {
      const Mask2D m = mask_brush(stroke, b, sz.w, sz.h);
      QVector<QPoint> px;
      for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x)
          if (m.get(x, y)) px.push_back(QPoint(x, y));
      if (role == views_->role())
        views_->setPending(px, editor_->mask_erase ? theme::with_alpha(tc.app, 0.7)
                                                   : theme::with_alpha(part_color(editor_->mask_part), 0.75));
      return;
    }
    MaskPaintRequest r;
    r.part = editor_->mask_part;
    r.view = role;
    r.stroke = stroke;
    r.brush = b;
    r.erase = editor_->mask_erase;
    controller_->paintPartMask(r);
  }
}

void MainWindow::onPixelClicked(ViewRole role, int u, int v) {
  const Tool tool = editor_->tool();
  if (controller_->view().doc.solid == nullptr) return;
  if (tool == Tool::PushPull) {
    PushPullRequest r;
    r.view = role;
    r.u = u;
    r.v = v;
    r.distance = editor_->push_distance;
    r.snap = editor_->snap;
    r.respect_bounds = editor_->respect_bounds;
    controller_->pushPull(r);
    return;
  }
  // Select (and Parts without a part to paint): the engine resolves the pixel; the answer selects in 3D.
  select_from_drawing_ = true;
  controller_->selectAtPixel(role, u, v, face_mode_ && tool == Tool::Select ? SelectMode::Face : SelectMode::Part);
}

void MainWindow::onPushPull(int face, double distance, bool finished) {
  PushPullRequest r;
  r.face = face;
  r.distance = distance;
  r.snap = editor_->snap;
  r.respect_bounds = editor_->respect_bounds;
  if (!finished) {
    if (distance == 0) controller_->clearPreview();
    else controller_->previewPushPull(r);
    statusBar()->showMessage(tr("Push/pull: %1 px").arg(distance, 0, 'g', 4));
    return;
  }
  cancelPreview();
  if (distance != 0) controller_->pushPull(r);
}

void MainWindow::onRegion(ViewRole role, QRect pixels) {
  QMenu menu(this);
  const std::pair<const char*, QString> items[] = {{"add", tr("Add Volume…")},
                                                   {"erase", tr("Erase Volume…")},
                                                   {"erase_through", tr("Erase All the Way Through")},
                                                   {"extrude", tr("Extrude Out by %1 px").arg(editor_->push_distance)},
                                                   {"push", tr("Push In by %1 px").arg(editor_->push_distance)},
                                                   {"mask", tr("Assign to the Painted Part")},
                                                   {"new_part", tr("New Part from This Rectangle…")}};
  for (const auto& [key, text] : items) {
    QAction* a = menu.addAction(text);
    a->setData(QString::fromLatin1(key));
    if (QString::fromLatin1(key) == "mask") a->setEnabled(editor_->mask_part >= 0);
  }
  const QAction* chosen = menu.exec(QCursor::pos());
  if (chosen) regionAction(role, pixels, chosen->data().toString());
}

void MainWindow::regionAction(ViewRole role, QRect px, const QString& what) {
  const SessionView& v = controller_->view();
  if (!v.doc.align || px.isEmpty()) return;
  const Polygons2D region = rect_polys(px.left(), px.top(), px.right() + 1, px.bottom() + 1);
  const double extent = view_depth_extent(role, v.doc.align->dims);
  if (what == "add" || what == "erase") {
    VolumeDialog d(what == "add", extent, this);
    if (d.exec() != QDialog::Accepted) return;
    if (what == "add") controller_->addVolume(role, region, d.from(), d.to(), -1);
    else controller_->eraseVolume(role, region, d.from(), d.to(), -1);
  } else if (what == "erase_through") {
    controller_->eraseVolume(role, region, -1, extent + 1, -1);
  } else if (what == "extrude" || what == "push") {
    const double dist = std::max(1.0, std::abs(editor_->push_distance)) * (what == "push" ? -1 : 1);
    controller_->extrude(role, region, dist, -1);
  } else if (what == "mask" || what == "new_part") {
    auto paint = [this, role, px](int part) {
      MaskPaintRequest r;
      r.part = part;
      r.view = role;
      for (int y = px.top(); y <= px.bottom(); ++y)
        for (int x = px.left(); x <= px.right(); ++x) r.pixels.emplace_back(x, y);
      controller_->paintPartMask(r);
    };
    if (what == "mask") {
      if (editor_->mask_part >= 0) paint(editor_->mask_part);
      return;
    }
    const auto name = askText(tr("New part"), tr("Name:"), tr("part"));
    if (!name) return;
    controller_->createPart(*name, true, [this, paint](int id) {
      editor_->mask_part = id;
      editor_->touch();
      paint(id);
    });
  }
}

void MainWindow::newPart(bool define) {
  const auto name = askText(define ? tr("New part") : tr("New label"),
                            define ? tr("Paint the part's pixels on two drawings (for example Front and Side); the part is "
                                        "exactly what the masks allow. Name:")
                                   : tr("Paint pixels of the existing model that belong to this part. Name:"),
                            tr("part"));
  if (!name) return;
  controller_->createPart(*name, define, [this](int id) {
    editor_->mask_part = id;
    setTool(Tool::Parts);
    editor_->touch();
  });
}

// ---- state updates ---------------------------------------------------------------------

void MainWindow::onViewChanged() {
  const SessionView& v = controller_->view();
  issues_ = build_issues(v);
  if (v.doc.revision != shown_revision_) {
    shown_revision_ = v.doc.revision;
    viewport_->setScene(scene_from(v.doc, lowpoly_pose_));
    hover_tri_ = -1;
    sel_tri_ = -1;  // triangle ids change with the mesh
    hover_candidates_.clear();
    std::set<int> keep;
    const Scene& s = viewport_->scene();
    const std::set<int> present(s.tri_part.begin(), s.tri_part.end());
    for (int p : sel_parts_)
      if (present.contains(p)) keep.insert(p);
    sel_parts_ = keep;
    preview_.reset();  // a committed edit replaces its preview
    preview_text_.clear();
    watchViews();
  }
  if (v.doc.align) place_on_surface(issues_, viewport_->scene(), v.doc.align->dims);
  views_->update_from(v);
  inspector_->update_from(v, issues_);
  history_->update_from(v);
  log_->update_from(v);
  start_->update_from(v, controller_->busy());
  if (inspector_->page() != InspectorPage::Issue) open_issue_.clear();
  if (inspector_->page() == InspectorPage::Overview && !(editor_->tool() == Tool::Select && editor_->selectMode() == SelectMode::Object))
    sel_parts_.clear();
  if (start_visible_ && v.doc.solid) setStartScreen(false);
  const ValidationStatus st = v.status.status;
  const StatusSummary sum = summarize(v, issues_);
  status_pill_->setIcon(status_icon(st));
  status_pill_->setText(sum.word);
  status_pill_->setToolTip(sum.sentence);
  status_pill_->setVisible(!v.doc.views.empty());
  updateToolOptions();
  updateHighlight();
  updateBanner();
  updateGuidance();
  updateTitle();
  updateActions();
}

void MainWindow::updateGuidance() {
  const SessionView& v = controller_->view();
  const Guidance g = next_step(v, issues_);
  hint_action_ = g.action;
  hint_text_->setText(g.text);
  hint_icon_->setPixmap(icon(g.tone == Tone::Review   ? Icon::StatusReview
                             : g.tone == Tone::Fail   ? Icon::StatusFail
                             : g.tone == Tone::Pass   ? Icon::StatusPass
                                                      : Icon::Info,
                             IconStyle::Toned, g.tone == Tone::Neutral ? Tone::Accent : g.tone)
                            .pixmap(16, 16));
  hint_button_->setText(g.action_label);
  hint_button_->setVisible(!g.action_label.isEmpty());
  hint_button_->setEnabled(!controller_->busy());
  const bool reviewing = g.action == "review" && !open_issue_.isEmpty();
  hint_->setVisible(!start_visible_ && !g.text.isEmpty() && !reviewing);
  hint_->adjustSize();
  // The next step is the primary toolbar button.
  const QString primary = g.action == "review" ? QString() : g.action;
  for (const auto& [k, ic] : kToolbarIcons) {
    QAction* a = actions_[k];
    QWidget* w = toolbar_->widgetForAction(a);
    if (!w) continue;
    const bool on = primary == k && a->isEnabled();
    if (w->property("primary").toBool() != on) {
      theme::set_prop(w, "primary", on);
      a->setIcon(icon(ic, on ? IconStyle::OnAccent : IconStyle::Default));
    }
  }
}

void MainWindow::updateCameraButtons() {
  const ViewState& s = viewport_->state();
  for (QAction* a : cameras_->actions()) a->setChecked(false);
  if (s.preset) actions_[QStringLiteral("view_%1").arg(static_cast<int>(*s.preset))]->setChecked(true);
  else actions_["view_iso"]->setChecked(true);
  if (actions_["ortho"]->isChecked() != s.ortho) {
    const QSignalBlocker block(actions_["ortho"]);
    actions_["ortho"]->setChecked(s.ortho);
  }
}

void MainWindow::onJobStarted(const QString& name) {
  progress_->setValue(0);
  progress_->show();
  cancel_->show();
  statusBar()->showMessage(friendly_job(name) + QStringLiteral("…"));
  updateActions();
  updateGuidance();
  start_->update_from(controller_->view(), true);
}

void MainWindow::onJobFinished(const QString& name, const OpResult& r) {
  if (!controller_->busy()) {
    progress_->hide();
    cancel_->hide();
  }
  log_->showResult(name, r);
  last_error_ = r.success || r.errors.empty() ? QString() : qs(r.errors.front().code);
  if (!r.warnings.empty() && !docks_["log"]->isVisible()) {
    unseen_warnings_ += static_cast<int>(r.warnings.size());
    log_button_->setText(tr("Log · %1").arg(unseen_warnings_));
  }
  if (r.success) {
    QString msg = friendly_job(name);
    if (name.startsWith("Reconstruct") || name == "Validate")
      msg += QStringLiteral(" — ") + summarize(controller_->view(), issues_).word;
    // An edit that moved a silhouette away from its drawing is said right away.
    QStringList changed;
    for (const Message& m : r.warnings)
      if (m.code == "edit.projection_changed") changed << qs(m.text);
    if (!changed.isEmpty()) {
      msg += QStringLiteral(" — ") + changed.join("; ");
      inspector_->showMessage(tr("This edit changed how the model matches the drawings: %1. Validate will ask you to "
                                 "review it; undo (Ctrl+Z) if it was not intended.")
                                  .arg(changed.join("; ")),
                              Tone::Review);
    }
    statusBar()->showMessage(msg, changed.isEmpty() ? 6000 : 15000);
  } else {
    const QString text = r.errors.empty() ? tr("failed") : qs(r.errors.front().text);
    statusBar()->showMessage(tr("%1 failed: %2").arg(friendly_job(name), text), 12000);
    if (last_error_ == "view.missing_required" && lowpoly_can_rescue(controller_->view().doc))
      // Never a dead end: the Views panel and the hint offer the switch to Low-Poly Studio.
      inspector_->showMessage(missing_views_text(controller_->view().doc), Tone::Review);
    else if (name != "Export model" && name != "Open project" && name != "Save project")
      inspector_->showMessage(tr("Not applied: %1").arg(text), Tone::Fail);
  }
  // An opened or recovered project goes to the workspace even before its mesh is rebuilt.
  if (r.success && (name == "Open project" || name == "Recover autosave" || name == "New single-view project") &&
      !controller_->view().doc.views.empty())
    setStartScreen(false);
  if (r.success && name == "Reload views") reload_button_->hide();
  if (r.success && name == "Import views" && lowpoly_can_rescue(controller_->view().doc))
    statusBar()->showMessage(missing_views_text(controller_->view().doc), 15000);
  if (name == "Edit parts" || name == "Set depth") {
    if (!r.success) {
      inspector_->showMessage(tr("Not applied: %1").arg(r.errors.empty() ? tr("the engine refused it") : qs(r.errors.front().text)),
                              Tone::Fail);
    } else {
      bool refused = false;
      for (const Message& m : r.warnings) refused |= m.code.rfind("edit.", 0) == 0;
      inspector_->showMessage(refused ? tr("Some choices were refused — the drawings need those parts.")
                                      : tr("Applied. Validate (F6) to check the model again."),
                              refused ? Tone::Review : Tone::Pass);
    }
  }
  if (name == "Export low-poly" && r.success)
    statusBar()->showMessage(tr("Exported %1 (%2 file(s), %3)")
                                 .arg(QFileInfo(pending_export_).fileName())
                                 .arg(r.data.value("files", json::array()).size())
                                 .arg(qs(to_string(r.status))),
                             10000);
  if (name == "Build low-poly" || name == "Part type" || name == "Apply suggested parts") {
    if (!r.success && !r.errors.empty())
      inspector_->showMessage(tr("Not done: %1").arg(qs(r.errors.front().text)), Tone::Fail);
    if (r.success && name == "Build low-poly") setStartScreen(false);
  }
  if (name == "Export model") {
    if (r.success)
      statusBar()->showMessage(tr("Exported %1 (%2, %3 file(s), %4)")
                                   .arg(QFileInfo(pending_export_).fileName(), qs(r.data.value("format", std::string())))
                                   .arg(r.data.value("files", json::array()).size())
                                   .arg(qs(r.data.value("status", std::string()))),
                               10000);
    else if (!r.errors.empty() && (r.errors[0].code == "export.needs_review" || r.errors[0].code == "export.validation_failed")) {
      // The status was unknown before export validated: ask now, then export with consent.
      const json why = r.errors[0].where;
      const bool fail = r.errors[0].code == "export.validation_failed";
      const QString word = fail ? tr("FAIL") : tr("NEEDS REVIEW");
      if (confirm_(tr("Export a model that is %1?").arg(word),
                   tr("The model is %1:\n%2\n\nExport it anyway?").arg(word, reviewLines(why).join("\n"))))
      {
        ModelExportRequest again = pending_request_;
        again.allow_needs_review = true;
        again.allow_fail = fail;
        pending_request_ = again;
        controller_->exportModel(again);
      }
    }
  }
  updateActions();
  updateGuidance();
}

void MainWindow::updateActions() {
  const SessionView& v = controller_->view();
  const bool idle = !controller_->busy();
  const bool has_views = !v.doc.views.empty();
  const bool has_model = v.doc.solid != nullptr;
  for (const char* k : {"new", "new_single", "open", "import", "save", "save_as", "checkpoint", "restore", "settings"})
    actions_[k]->setEnabled(idle);
  for (const char* k : {"run_all", "align", "reconstruct", "reconstruct_voxel", "textures", "validate", "cleanup", "export",
                        "renders", "reload_views"})
    actions_[k]->setEnabled(idle && has_views);
  if (v.doc.settings.lowpoly.enabled)  // the visual-hull steps do not run in a Low-Poly Studio project
    for (const char* k : {"reconstruct", "reconstruct_voxel", "textures", "validate", "cleanup", "renders"})
      actions_[k]->setEnabled(false);
  actions_["undo"]->setEnabled(idle && v.can_undo);
  actions_["redo"]->setEnabled(idle && v.can_redo);
  // The project type: checked in Model ▸ Project Type, named on the Views panel's chip.
  const bool lowpoly = v.doc.settings.lowpoly.enabled;
  actions_["mode_lowpoly"]->setChecked(lowpoly);
  actions_["mode_visual_hull"]->setChecked(!lowpoly);
  for (const char* k : {"mode_lowpoly", "mode_visual_hull"}) actions_[k]->setEnabled(idle);
  mode_chip_->setText(project_type_name(lowpoly));
  mode_chip_->setToolTip(tr("Project type: %1 (%2). Click to switch; your drawings are kept.")
                             .arg(project_type_name(lowpoly), project_type_needs(lowpoly)));
  const bool part = idle && has_model && !sel_parts_.empty();
  for (const char* k : {"part_rename", "part_duplicate", "part_transform", "part_split", "part_split_shells", "part_delete",
                        "part_mirror_copy_0", "part_mirror_copy_1", "part_mirror_copy_2", "part_paint_masks", "part_sync_masks"})
    actions_[k]->setEnabled(part);
  actions_["part_merge"]->setEnabled(part && sel_parts_.size() >= 2);
  for (const char* k : {"part_mirror_0", "part_mirror_1", "part_mirror_2", "part_cut", "tool_mirror"})
    actions_[k]->setEnabled(idle && has_model);
  for (const char* k : {"part_new_define", "part_new_label"}) actions_[k]->setEnabled(idle && has_views);
  QString last;
  for (auto it = v.log.rbegin(); it != v.log.rend(); ++it)
    if (it->success) {
      last = qs(it->name).replace('_', ' ');
      break;
    }
  actions_["undo"]->setToolTip(last.isEmpty() ? tr("Undo (Ctrl+Z)") : tr("Undo %1 (Ctrl+Z)").arg(last));
  actions_["redo"]->setToolTip(tr("Redo (Ctrl+Y)"));
}

void MainWindow::updateTitle() {
  const QString path = controller_->projectPath();
  const QString name = path.isEmpty() ? qs(controller_->view().doc.settings.name) : QFileInfo(path).fileName();
  setWindowTitle(QStringLiteral("%1%2 — %3 — 3re %4")
                     .arg(name, controller_->dirty() ? "*" : "",
                          project_type_name(controller_->view().doc.settings.lowpoly.enabled), qs(version())));
}

QString MainWindow::hintText() const { return hint_text_->text(); }

bool MainWindow::maybeSave() {
  if (!controller_->dirty()) return true;
  const auto answer = QMessageBox::question(this, tr("Unsaved changes"), tr("Save the current project first?"),
                                            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
  if (answer == QMessageBox::Cancel) return false;
  if (answer == QMessageBox::Save) {
    actions_["save"]->trigger();
    controller_->waitIdle();
    QApplication::processEvents();
    return !controller_->dirty();
  }
  return true;
}

void MainWindow::closeEvent(QCloseEvent* e) {
  if (!maybeSave()) {
    e->ignore();
    return;
  }
  saveLayout();
  controller_->cleanShutdown();
  e->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
  if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e) {
  QStringList pngs;
  for (const QUrl& u : e->mimeData()->urls()) {
    const QString f = u.toLocalFile();
    if (f.endsWith(".p3d", Qt::CaseInsensitive)) {
      if (maybeSave()) openProjectFile(f);
      return;
    }
    if (is_source_image(f)) pngs << f;
  }
  if (!pngs.isEmpty()) importFiles(pngs);
}

// ---- Aseprite workflow ------------------------------------------------------------------

void MainWindow::watchViews() {
  QStringList wanted;
  for (const SourceView& v : controller_->view().doc.views) {
    const QString p = QFileInfo(from_path(v.original_path)).absoluteFilePath();
    if (QFileInfo::exists(p)) wanted << p;
  }
  const QStringList current = watcher_.files();
  for (const QString& f : current)
    if (!wanted.contains(f)) watcher_.removePath(f);
  for (const QString& f : wanted)
    if (!current.contains(f)) watcher_.addPath(f);
}

void MainWindow::viewsChangedOnDisk(const QString& path) {
  // Editors often replace the file (write + rename): watch the new file again.
  QTimer::singleShot(200, this, [this, path] {
    if (QFileInfo::exists(path) && !watcher_.files().contains(path)) watcher_.addPath(path);
  });
  statusBar()->showMessage(tr("%1 changed on disk.").arg(QFileInfo(path).fileName()), 6000);
  reload_timer_.start();
}

QStringList MainWindow::recentFiles() const { return QSettings().value("recent").toStringList(); }

void MainWindow::addRecent(const QString& path) {
  QSettings s;
  QStringList r = s.value("recent").toStringList();
  r.removeAll(path);
  r.prepend(path);
  while (r.size() > 8) r.removeLast();
  s.setValue("recent", r);
  rebuildRecentMenu();
  start_->setRecent(r);
}

void MainWindow::rebuildRecentMenu() {
  if (!recent_menu_) return;
  recent_menu_->clear();
  const QStringList r = recentFiles();
  for (const QString& p : r) {
    QAction* a = recent_menu_->addAction(p);
    connect(a, &QAction::triggered, this, [this, p] {
      if (maybeSave()) openProjectFile(p);
    });
  }
  recent_menu_->setEnabled(!r.isEmpty());
}

}  // namespace p3dgui
