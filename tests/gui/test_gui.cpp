// Desktop application tests: the main window is driven through its actions,
// buttons and inline correction tools (offscreen platform, software 3D view).
// The test never calls the engine directly; it only reads what the window shows.
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QDropEvent>
#include <QFile>
#include <QLabel>
#include <QMimeData>
#include <QUrl>
#include <QListWidget>
#include <QMenu>
#include <QSpinBox>
#include <QTableWidget>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolBar>
#include <QWidgetAction>
#include <QtTest>
#include <algorithm>
#include <set>

#include "dialogs.hpp"
#include "lowpoly_panel.hpp"
#include "history_panel.hpp"
#include "inspector.hpp"
#include "issues.hpp"
#include "main_window.hpp"
#include "pixel3d/common/version.hpp"
#include "pixel3d/engine/autosave.hpp"
#include "pixel3d/io/json_io.hpp"
#include "qt_util.hpp"
#include "start_screen.hpp"
#include "viewport.hpp"
#include "view_state.hpp"
#include "views_panel.hpp"
#include "../test_helpers.hpp"

using namespace p3dgui;
using namespace pixel3d;

namespace {

QStringList fixture(const QString& name) {
  const QString d = QStringLiteral(PIXEL3D_FIXTURES_DIR) + "/" + name + "/";
  return {d + "front.png", d + "side.png", d + "top.png"};
}

bool wait_idle(MainWindow& w, int timeout_ms = 120000) {
  QElapsedTimer t;
  t.start();
  do {
    QTest::qWait(20);
  } while (w.controller().busy() && t.elapsed() < timeout_ms);
  QTest::qWait(20);
  return !w.controller().busy();
}

std::unique_ptr<MainWindow> make_window(const QString& autosave_dir = {}) {
  WindowOptions opt;
  opt.software_view = true;
  opt.autosave_dir = autosave_dir;
  auto w = std::make_unique<MainWindow>(opt);
  w->show();
  return w;
}

// Visual Hull is an explicit choice (new projects are Low-Poly Studio): the tests of its workflow start one.
std::unique_ptr<MainWindow> make_vh_window(const QString& autosave_dir = {}) {
  auto w = make_window(autosave_dir);
  ProjectSettings vh = default_project_settings();
  vh.lowpoly.enabled = false;
  w->controller().newProject(vh);
  wait_idle(*w);
  return w;
}

const Issue* find_issue(const MainWindow& w, IssueFix fix) {
  for (const Issue& i : const_cast<MainWindow&>(w).inspector().issues())
    if (i.fix == fix && i.level == IssueLevel::Blocking) return &i;
  return nullptr;
}

QPushButton* status_button(MainWindow& w) { return w.inspector().findChild<QPushButton*>("statusAction"); }

}  // namespace

class GuiTest : public QObject {
  Q_OBJECT
 private slots:
  void start_screen_until_a_model_exists();
  void toolbar_has_the_primary_actions_in_order();
  void fixtureA_reaches_pass_through_actions();
  void fixtureC_review_workflow_with_inline_tools();
  void viewport_click_selects_a_part();
  void export_asks_before_writing_a_needs_review_model();
  void undo_redo_and_checkpoints();
  void crash_recovery_restores_unsaved_work();
  void role_guessing_from_file_names();
  void guidance_follows_the_workflow();
  void regions_project_into_each_drawing();
  void picking_finds_the_front_face();
  // M4 editor
  void editing_tools_match_core();
  void viewport_selection_reaches_the_engine();
  void part_masks_resolve_c_like_core();
  void single_view_project_through_the_window();
  void aseprite_reload_keeps_pixels_and_roles();
  // M5 export
  void export_dialog_drives_export_model();
  // Input normalisation
  void normalisation_settings_and_view_notes();
  // 1.1 polished styles and the version
  void style_setting_shows_the_polished_model();
  // 1.2 Low-Poly Studio
  void lowpoly_studio_through_the_window();
  void layered_drawings_open_as_a_lowpoly_project();
  // 1.2.4 project type: Low-Poly Studio by default, Visual Hull explicit, no dead end without a Top drawing
  void new_projects_default_to_lowpoly_studio();
  void visual_hull_without_top_offers_lowpoly_studio();
  void drawings_without_top_never_end_in_a_dead_end();
};

void GuiTest::start_screen_until_a_model_exists() {
  auto w = make_vh_window();
  QVERIFY(w->startScreenVisible());
  QVERIFY(!w->dock("views")->isVisible());
  QVERIFY(!w->dock("inspector")->isVisible());
  QVERIFY(!w->mainToolBar()->isVisible());
  QPushButton* reconstruct = w->startScreen().reconstructButton();
  QVERIFY(!reconstruct->isEnabled());
  w->importFiles(fixture("A_synthetic_chair"));
  QVERIFY(wait_idle(*w));
  QVERIFY(w->startScreenVisible());  // views alone do not leave the start screen
  QVERIFY(reconstruct->isEnabled());
  reconstruct->click();
  QVERIFY(wait_idle(*w));
  QVERIFY(!w->startScreenVisible());
  QVERIFY(w->dock("views")->isVisible());
  QVERIFY(w->dock("inspector")->isVisible());
  QVERIFY(!w->dock("log")->isVisible());  // the log never asks for attention
  QVERIFY(w->mainToolBar()->isVisible());
  // Layout reset keeps the same default.
  w->dock("views")->hide();
  w->dock("log")->show();
  w->action("reset_layout")->trigger();
  QVERIFY(w->dock("views")->isVisible());
  QVERIFY(!w->dock("log")->isVisible());
  // New project returns to the start screen (nothing to save: decline is not asked).
  w->controller().cleanShutdown();
}

void GuiTest::toolbar_has_the_primary_actions_in_order() {
  auto w = make_window();
  QStringList texts;
  for (QAction* a : w->mainToolBar()->actions()) {
    if (qobject_cast<QWidgetAction*>(a)) continue;  // dividers
    texts << a->iconText();
    QVERIFY2(!a->icon().isNull(), qPrintable(a->iconText()));
  }
  QCOMPARE(texts, QStringList({"Import", "Reconstruct", "Select", "Depth", "Push/Pull", "Volume", "Parts", "Mirror",
                               "Validate", "Export"}));
  QVERIFY(w->action("tool_pushpull")->isEnabled());  // M4 tools
  QVERIFY(w->action("tool_volume")->isEnabled());
  QVERIFY(!w->action("tool_mirror")->isEnabled());  // nothing to mirror yet
  QVERIFY(w->action("tool_select")->isChecked());
  w->action("tool_depth")->trigger();
  QCOMPARE(w->tool(), Tool::Depth);
  w->controller().cleanShutdown();
}

void GuiTest::fixtureA_reaches_pass_through_actions() {
  auto w = make_vh_window();
  QCOMPARE(w->inspector().statusText(), QString("NOT_VALIDATED"));
  QVERIFY(!w->action("run_all")->isEnabled());  // nothing imported
  w->importFiles(fixture("A_synthetic_chair"));
  QVERIFY(wait_idle(*w));
  QCOMPARE(w->controller().view().doc.views.size(), size_t(3));
  QVERIFY(w->action("run_all")->isEnabled());
  QVERIFY(w->hintText().contains("build the 3D model"));
  w->action("run_all")->trigger();
  QVERIFY(!w->action("run_all")->isEnabled());  // disabled while the job runs
  QVERIFY(wait_idle(*w));
  QCOMPARE(w->inspector().statusText(), QString("PASS"));
  QCOMPARE(w->inspector().statusWord(), QString("PASS"));
  QVERIFY(w->controller().view().doc.texture != nullptr);
  QVERIFY(w->windowTitle().contains("*"));  // unsaved
  // PASS: Export is the primary action, in the toolbar and on the status card.
  QVERIFY(w->mainToolBar()->widgetForAction(w->action("export"))->property("primary").toBool());
  QCOMPARE(status_button(*w)->property("action").toString(), QString("export"));
  // View presets drive the camera.
  w->action("view_4")->trigger();  // Top
  QVERIFY(w->viewport().state().preset == ViewRole::Top);
  QVERIFY(w->viewport().state().ortho);
  QCOMPARE(w->viewsPanel().role(), ViewRole::Top);
  QVERIFY(w->action("view_4")->isChecked());
  QImage shot = w->grab().toImage();
  QVERIFY(!shot.isNull());
  // Save, then the title is clean.
  QTemporaryDir dir;
  w->saveProjectFile(dir.filePath("a.p3d"));
  QVERIFY(wait_idle(*w));
  QVERIFY(QFile::exists(dir.filePath("a.p3d")));
  QVERIFY(!w->windowTitle().contains("*"));
  w->controller().cleanShutdown();
}

void GuiTest::fixtureC_review_workflow_with_inline_tools() {
  auto w = make_vh_window();
  w->importFiles(fixture("C_asym_planter"));
  QVERIFY(wait_idle(*w));
  w->action("run_all")->trigger();
  QVERIFY(wait_idle(*w));
  QCOMPARE(w->inspector().statusText(), QString("NEEDS_REVIEW"));
  QCOMPARE(w->inspector().statusWord(), QString("NEEDS REVIEW"));
  // Plain language: no engine vocabulary in what the artist reads.
  for (const Issue& i : w->inspector().issues()) {
    QVERIFY2(!i.title.contains('_'), qPrintable(i.title));
    QVERIFY2(!i.explanation.contains('_'), qPrintable(i.explanation));
  }
  const Issue* parts = find_issue(*w, IssueFix::ChooseParts);
  const Issue* hole = find_issue(*w, IssueFix::SetDepth);
  QVERIFY(parts && hole);
  QCOMPARE(parts->title, QString("Parts don't match between views"));
  QCOMPARE(hole->title, QString("Hole or painted detail?"));
  QVERIFY(!parts->regions.empty() && !parts->parts.empty());
  const QString hole_key = hole->key();

  // The status card's button opens the first issue: parts selected, camera framed,
  // regions outlined, the drawing showing it selected, the correction tool open.
  QCOMPARE(status_button(*w)->property("action").toString(), QString("review"));
  const QVector3D pan_before = w->viewport().state().pan;
  status_button(*w)->click();
  QCOMPARE(w->inspector().page(), InspectorPage::Issue);
  QVERIFY(!w->selectedParts().empty());
  QVERIFY(!w->viewport().highlight().issue_regions.empty());
  QTRY_VERIFY_WITH_TIMEOUT((w->viewport().state().pan - pan_before).length() > 1.0f, 10000);  // camera transition
  QCOMPARE(w->viewsPanel().role(), ViewRole::Top);
  PartsChooser* chooser = w->inspector().partsChooser();
  QVERIFY(chooser);
  QVERIFY(chooser->arrangementCount() > 1);

  // The user knows the legs: the engine's arrangement {1, 2, 5}.
  const Issue* open = find_issue(*w, IssueFix::ChooseParts);
  int pick = -1;
  for (size_t i = 0; i < open->alternatives.size(); ++i) {
    std::vector<int> a = open->alternatives[i];
    std::sort(a.begin(), a.end());
    if (a == std::vector<int>{1, 2, 5}) pick = static_cast<int>(i);
  }
  QVERIFY(pick >= 0);
  chooser->chooseArrangement(pick);
  chooser->applyButton()->click();
  QVERIFY(wait_idle(*w));
  w->action("validate")->trigger();
  QVERIFY(wait_idle(*w));
  QCOMPARE(w->inspector().statusText(), QString("NEEDS_REVIEW"));  // the cavity is still open
  QVERIFY(find_issue(*w, IssueFix::ChooseParts) == nullptr);

  // The user knows the cavity is 11 px deep.
  w->openIssue(hole_key);
  DepthEditor* depth = w->inspector().depthEditor();
  QVERIFY(depth);
  depth->setDepth(11);
  depth->applyButton()->click();
  QVERIFY(wait_idle(*w));
  w->action("validate")->trigger();
  QVERIFY(wait_idle(*w));
  QCOMPARE(w->inspector().statusText(), QString("PASS"));
  QCOMPARE(w->inspector().page(), InspectorPage::Overview);  // the resolved issue is gone
  w->controller().cleanShutdown();
}

void GuiTest::viewport_click_selects_a_part() {
  auto w = make_vh_window();
  w->importFiles(fixture("A_synthetic_chair"));
  QVERIFY(wait_idle(*w));
  w->action("run_all")->trigger();
  QVERIFY(wait_idle(*w));
  w->action("view_0")->trigger();  // Front
  auto* cpu = w->viewport().findChild<CpuViewport*>();
  QVERIFY(cpu);
  cpu->grab();  // paint once: the software view picks from its last frame
  // Find a drawn point (the seat back is near the top centre).
  QPoint hit(-1, -1);
  for (int y = cpu->height() / 4; y < cpu->height() && hit.x() < 0; y += 6)
    for (int x = cpu->width() / 3; x < 2 * cpu->width() / 3; x += 6)
      if (cpu->triangleAt(QPoint(x, y)) >= 0) {
        hit = QPoint(x, y);
        break;
      }
  QVERIFY(hit.x() >= 0);
  QTest::mouseClick(cpu, Qt::LeftButton, {}, hit);
  QVERIFY(w->inspector().page() == InspectorPage::Part || w->inspector().page() == InspectorPage::Face);
  // Clicking empty space clears the selection.
  QTest::mouseClick(cpu, Qt::LeftButton, {}, QPoint(3, 3));
  QCOMPARE(w->inspector().page(), InspectorPage::Overview);
  QVERIFY(w->selectedParts().empty());
  w->controller().cleanShutdown();
}

void GuiTest::export_asks_before_writing_a_needs_review_model() {
  auto w = make_vh_window();
  w->importFiles(fixture("C_asym_planter"));
  QVERIFY(wait_idle(*w));
  QTemporaryDir dir;
  const QString glb = dir.filePath("c.glb");
  int asked = 0;
  QString question;
  // Not validated yet: export validates, finds NEEDS_REVIEW and asks; the user declines.
  w->setConfirmHandler([&](const QString&, const QString& text) {
    ++asked;
    question = text;
    return false;
  });
  w->exportTo(glb);
  QVERIFY(wait_idle(*w));
  QCOMPARE(asked, 1);
  QVERIFY(!QFile::exists(glb));
  QCOMPARE(w->lastError(), QString("export.needs_review"));
  // Now known NEEDS_REVIEW: asked before exporting, in plain words; the user accepts.
  w->setConfirmHandler([&](const QString&, const QString& text) {
    ++asked;
    question = text;
    return true;
  });
  w->action("validate")->trigger();
  QVERIFY(wait_idle(*w));
  w->exportTo(glb);
  QVERIFY(wait_idle(*w));
  QCOMPARE(asked, 2);
  QVERIFY(question.contains("Parts don't match between views"));
  QVERIFY(QFile::exists(glb));
  w->controller().cleanShutdown();
}

void GuiTest::undo_redo_and_checkpoints() {
  auto w = make_vh_window();
  QVERIFY(!w->action("undo")->isEnabled());
  w->importFiles(fixture("A_synthetic_chair"));
  QVERIFY(wait_idle(*w));
  QVERIFY(w->action("undo")->isEnabled());
  w->action("reconstruct")->trigger();
  QVERIFY(wait_idle(*w));
  QVERIFY(w->controller().view().doc.solid != nullptr);
  w->action("undo")->trigger();
  QVERIFY(wait_idle(*w));
  QVERIFY(w->controller().view().doc.solid == nullptr);
  QVERIFY(w->action("redo")->isEnabled());
  w->action("redo")->trigger();
  QVERIFY(wait_idle(*w));
  QVERIFY(w->controller().view().doc.solid != nullptr);
  w->controller().createCheckpoint("mine");
  QVERIFY(wait_idle(*w));
  bool found = false;
  for (const auto& n : w->controller().view().checkpoints) found |= n == "mine";
  QVERIFY(found);
  w->controller().cleanShutdown();
}

void GuiTest::crash_recovery_restores_unsaved_work() {
  QTemporaryDir autosave;
  {
    auto w = make_vh_window(autosave.path());
    w->importFiles(fixture("B_hand_chair"));
    QVERIFY(wait_idle(*w));
    w->action("reconstruct")->trigger();
    QVERIFY(wait_idle(*w));
    QSignalSpy saved(&w->controller(), &AppController::autosaved);
    w->controller().autosaveNow();
    QVERIFY(saved.wait(20000));
    // No clean shutdown: the window goes away the way a crash would leave things.
  }
  const auto found = find_recoverable(to_path(autosave.path()));
  QCOMPARE(found.size(), size_t(1));
  auto w = make_window(autosave.path());
  w->controller().recover(found[0]);
  QVERIFY(wait_idle(*w));
  QCOMPARE(w->controller().view().doc.views.size(), size_t(3));
  QVERIFY(w->controller().dirty());  // recovered work is unsaved
  QVERIFY(!w->startScreenVisible());  // a recovered model goes straight to the workspace
  QVERIFY(find_recoverable(to_path(autosave.path())).empty());
  w->controller().cleanShutdown();
  QVERIFY(QDir(autosave.path()).isEmpty());
}

void GuiTest::role_guessing_from_file_names() {
  QCOMPARE(role_from_filename("C:/art/chair_front.png"), std::optional<ViewRole>(ViewRole::Front));
  QCOMPARE(role_from_filename("left.png"), std::optional<ViewRole>(ViewRole::Side));
  QCOMPARE(role_from_filename("x/TOP.PNG"), std::optional<ViewRole>(ViewRole::Top));
  QCOMPARE(role_from_filename("bottom_view.png"), std::optional<ViewRole>(ViewRole::Bottom));
  QVERIFY(!role_from_filename("chair.png").has_value());
}

void GuiTest::guidance_follows_the_workflow() {
  auto w = make_vh_window();
  auto step = [&] { return next_step(w->controller().view(), w->inspector().issues()); };
  QCOMPARE(step().action, QString("import"));
  const QStringList a = fixture("A_synthetic_chair");
  w->importFiles({a[0], a[1]});
  QVERIFY(wait_idle(*w));
  // Front + Side in a visual-hull project: the hint offers the switch to Low-Poly Studio (never a dead end).
  QCOMPARE(step().action, QString("switch_lowpoly"));
  QVERIFY(step().text.contains("Top"));
  w->importFiles({a[2]});
  QVERIFY(wait_idle(*w));
  QCOMPARE(step().action, QString("run_all"));
  QVERIFY(w->mainToolBar()->widgetForAction(w->action("run_all"))->property("primary").toBool());
  w->action("run_all")->trigger();
  QVERIFY(wait_idle(*w));
  QCOMPARE(step().action, QString("export"));
  w->controller().cleanShutdown();
}

void GuiTest::regions_project_into_each_drawing() {
  const Dims d{10, 20, 30};
  const Region3 r{QVector3D(1, 2, 3), QVector3D(4, 5, 6)};
  QCOMPARE(view_rect(r, ViewRole::Top, d), QRectF(QPointF(1, 3), QPointF(4, 6)));      // u = x, v = z
  QCOMPARE(view_rect(r, ViewRole::Front, d), QRectF(QPointF(1, 15), QPointF(4, 18)));  // v = H - y
  QCOMPARE(view_rect(r, ViewRole::Side, d), QRectF(QPointF(3, 15), QPointF(6, 18)));   // u = z
  QCOMPARE(position_name(QRectF(0, 25, 2, 4), 0, 4, d), QString("Front left, low"));
  QCOMPARE(position_name(QRectF(4, 12, 2, 4), 8, 12, d), QString("Centre"));
}

void GuiTest::picking_finds_the_front_face() {
  auto w = make_vh_window();
  w->importFiles(fixture("A_synthetic_chair"));
  QVERIFY(wait_idle(*w));
  w->action("run_all")->trigger();
  QVERIFY(wait_idle(*w));
  const Scene s = scene_from(w->controller().view().doc);
  ViewState v;
  v.set_preset(ViewRole::Front);
  const CameraMatrices cam = camera_matrices(v, s.dims, 800, 800);
  // A ray through the middle of the seat back hits a face that looks at the camera.
  const int t = pick_triangle(s, cam, QPointF(0.0, 0.35));
  QVERIFY(t >= 0);
  const auto& m = *s.mesh;
  const vec3d n = m.normals[m.indices[3 * static_cast<size_t>(t)]];
  QVERIFY(n.z > 0.9);
  QCOMPARE(pick_triangle(s, cam, QPointF(0.98, 0.98)), -1);  // empty corner
  w->controller().cleanShutdown();
}

// ---- M4 editor: the window's tools make the same core calls ------------------------------

namespace {

std::vector<ViewImport> core_views(const QString& name) {
  const QStringList f = fixture(name);
  return {{ViewRole::Front, to_path(f[0]), {}}, {ViewRole::Side, to_path(f[1]), {}}, {ViewRole::Top, to_path(f[2]), {}}};
}

double mesh_volume(const Mesh& m) {
  double v = 0;
  for (const auto& t : m.tris) v += dot(m.positions[t[0]], cross(m.positions[t[1]], m.positions[t[2]])) / 6.0;
  return v;
}

double gui_volume(MainWindow& w) { return w.controller().view().status.stats.volume; }

std::vector<int> ints(const nlohmann::json& j) {
  std::vector<int> out;
  for (const auto& x : j) out.push_back(x.get<int>());
  return out;
}

std::vector<int> session_parts(MainWindow& w) { return w.controller().view().selection.parts; }

// The drawing preview of the Views panel, painted once (it maps clicks through its last frame).
SourcePreview* drawing(MainWindow& w, ViewRole r) {
  w.viewsPanel().setRole(r);
  SourcePreview* p = w.viewsPanel().preview();
  p->grab();
  return p;
}

}  // namespace

void GuiTest::editing_tools_match_core() {
  auto w = make_vh_window();
  w->importFiles(fixture("A_synthetic_chair"));
  QVERIFY(wait_idle(*w));
  w->action("run_all")->trigger();
  QVERIFY(wait_idle(*w));
  QCOMPARE(w->inspector().statusText(), QString("PASS"));
  QVERIFY(w->historyPanel().liveText().contains("matches its drawing"));
  Session core;
  QVERIFY(core.import_views(core_views("A_synthetic_chair")).success);
  QVERIFY(core.project_textures(TextureOptions{}).success);
  QCOMPARE(gui_volume(*w), core.current_status().stats.volume);

  // 2D -> 3D: clicking a Front drawing pixel selects that part everywhere (and in the engine).
  w->action("tool_select")->trigger();
  SourcePreview* front = drawing(*w, ViewRole::Front);
  QSignalSpy selected(&w->controller(), &AppController::selectionResult);
  QTest::mouseClick(front, Qt::LeftButton, {}, front->widgetPos({1.5, 30.5}));
  QVERIFY(selected.wait(20000));
  const OpResult sel = core.select_at_pixel(ViewRole::Front, 1, 30, SelectMode::Part);
  const std::vector<int> leg_parts = ints(sel.data["parts"]);
  QCOMPARE(std::vector<int>(w->selectedParts().begin(), w->selectedParts().end()), leg_parts);
  QCOMPARE(session_parts(*w), leg_parts);
  QCOMPARE(w->inspector().page(), InspectorPage::Part);
  QVERIFY(w->viewport().highlight().parts.contains(leg_parts.at(0)));
  const int leg = leg_parts.at(0);

  // Depth brush on the Top drawing: live preview while dragging, one edit on release.
  w->action("tool_depth")->trigger();
  QCOMPARE(w->tool(), Tool::Depth);
  w->editor().depth_brush = BrushParams{1.5, 1.0, 1.0, false, 1.0};
  w->editor().touch();
  SourcePreview* top = drawing(*w, ViewRole::Top);
  QSignalSpy previews(&w->controller(), &AppController::previewReady);
  QTest::mousePress(top, Qt::LeftButton, {}, top->widgetPos({6.5, 12.5}));
  QTest::mouseMove(top, top->widgetPos({11.5, 12.5}));
  QTRY_COMPARE_WITH_TIMEOUT(previews.count(), 2, 30000);  // press, then drag (newest wins)
  const auto preview = qvariant_cast<std::shared_ptr<const Mesh>>(previews.last().at(0));
  QVERIFY(preview != nullptr);
  QTRY_VERIFY(w->viewport().highlight().preview != nullptr);
  QVERIFY(w->bannerText().contains("Preview", Qt::CaseInsensitive));
  QCOMPARE(w->controller().view().doc.edits.size(), size_t(0));  // nothing committed yet
  QTest::mouseRelease(top, Qt::LeftButton, {}, top->widgetPos({11.5, 12.5}));
  QVERIFY(wait_idle(*w));
  QCOMPARE(w->controller().view().doc.edits.size(), size_t(1));
  QVERIFY(w->viewport().highlight().preview == nullptr);
  const nlohmann::json gui_edit = edit_to_json(w->controller().view().doc.edits.back());
  QVERIFY(gui_edit["op"] == "depth_stroke");
  DepthPaintRequest dp;
  dp.view = ViewRole::Top;
  for (const auto& s : gui_edit["samples"]) dp.samples.push_back({s[0].get<int>(), s[1].get<int>(), s[2].get<double>()});
  const OpResult painted = core.paint_depth(dp);
  QVERIFY(painted.success);
  QVERIFY(edit_to_json(core.document().edits.back()) == gui_edit);
  QCOMPARE(gui_volume(*w), painted.stats.volume);
  QVERIFY(std::abs(mesh_volume(*preview) - painted.stats.volume) < 1e-6);  // the preview was the commit

  // Mirror copy of the selected leg (toolbar Mirror menu / Part menu).
  QCOMPARE(w->selectedParts(), std::set<int>{leg});
  QVERIFY(w->action("part_mirror_copy_2")->isEnabled());
  w->action("part_mirror_copy_2")->trigger();
  QVERIFY(wait_idle(*w));
  const OpResult mirrored = core.mirror(leg, 2, std::nullopt, true);
  QVERIFY(mirrored.success);
  QVERIFY(edit_to_json(w->controller().view().doc.edits.back()) == edit_to_json(core.document().edits.back()));
  QCOMPARE(gui_volume(*w), mirrored.stats.volume);

  // Push/pull by dragging a face in the Front view (it faces the camera: drag down pushes in).
  w->action("view_0")->trigger();
  w->action("tool_pushpull")->trigger();
  QWidget* surface = w->viewport().surface();
  surface->grab();
  const Dims dims = w->controller().view().doc.align->dims;
  const QPoint at = w->viewport().project(world_from_view(ViewRole::Front, dims, 9.5, 17.5, 0)).toPoint();
  const auto uv = w->viewport().viewPixelAt(at);
  QVERIFY(uv && static_cast<int>(uv->x) == 9 && static_cast<int>(uv->y) == 17);
  const double one_px = (w->viewport().project(world_from_view(ViewRole::Front, dims, 9.5, 18.5, 0)) -
                         w->viewport().project(world_from_view(ViewRole::Front, dims, 9.5, 17.5, 0)))
                            .y();
  const QPoint to = at + QPoint(0, static_cast<int>(std::lround(2 * one_px)));
  QSignalSpy pp_previews(&w->controller(), &AppController::previewReady);
  QTest::mousePress(surface, Qt::LeftButton, {}, at);
  QTest::mouseMove(surface, to);
  QVERIFY(pp_previews.wait(30000));
  QTest::mouseRelease(surface, Qt::LeftButton, {}, to);
  QVERIFY(wait_idle(*w));
  const nlohmann::json sweep = edit_to_json(w->controller().view().doc.edits.back());
  QVERIFY(sweep["op"] == "sweep");
  QCOMPARE(sweep["distance"].get<double>(), -2.0);
  PushPullRequest pr;
  pr.view = ViewRole::Front;
  pr.u = 9;
  pr.v = 17;
  pr.distance = -2;
  const OpResult pushed = core.push_pull(pr);
  QVERIFY(pushed.success);
  QVERIFY(edit_to_json(core.document().edits.back()) == sweep);
  QCOMPARE(gui_volume(*w), pushed.stats.volume);

  // History (bottom panel): double-click two steps back = two undos.
  QVERIFY(w->dock("history")->isVisible());
  QListWidget* list = w->historyPanel().list();
  const int cursor = static_cast<int>(w->controller().view().history_cursor);
  QCOMPARE(list->count(), cursor + 1);
  QSignalSpy jumps(&w->historyPanel(), &HistoryPanel::jumpRequested);
  const QPoint item_pos = list->visualItemRect(list->item(cursor - 2)).center();
  QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, item_pos);  // item views need the press first
  QTest::mouseDClick(list->viewport(), Qt::LeftButton, {}, item_pos);
  QCOMPARE(jumps.count(), 1);
  QVERIFY(wait_idle(*w));
  QCOMPARE(static_cast<int>(w->controller().view().history_cursor), cursor - 2);
  QVERIFY(core.undo().success);
  QVERIFY(core.undo().success);
  QCOMPARE(w->controller().view().doc.edits.size(), core.document().edits.size());
  QCOMPARE(gui_volume(*w), core.current_status().stats.volume);
  w->action("redo")->trigger();
  QVERIFY(wait_idle(*w));

  // Save and reopen: the edits replay to the same model.
  QTemporaryDir dir;
  const double before = gui_volume(*w);
  const size_t edits = w->controller().view().doc.edits.size();
  w->saveProjectFile(dir.filePath("edited.p3d"));
  QVERIFY(wait_idle(*w));
  w->openProjectFile(dir.filePath("edited.p3d"));
  QVERIFY(wait_idle(*w));
  w->action("textures")->trigger();
  QVERIFY(wait_idle(*w));
  QCOMPARE(w->controller().view().doc.edits.size(), edits);
  QCOMPARE(gui_volume(*w), before);
  w->controller().cleanShutdown();
}

void GuiTest::viewport_selection_reaches_the_engine() {
  auto w = make_vh_window();
  w->importFiles(fixture("A_synthetic_chair"));
  QVERIFY(wait_idle(*w));
  w->action("run_all")->trigger();
  QVERIFY(wait_idle(*w));
  Session core;
  QVERIFY(core.import_views(core_views("A_synthetic_chair")).success);
  QVERIFY(core.project_textures(TextureOptions{}).success);
  // A click in the 3D view selects locally at once and tells the engine the same selection.
  w->action("view_iso")->trigger();
  QWidget* surface = w->viewport().surface();
  surface->grab();
  const QPoint centre = surface->rect().center();
  QSignalSpy synced(&w->controller(), &AppController::selectionResult);
  QTest::mouseClick(surface, Qt::LeftButton, {}, centre);
  QVERIFY(synced.wait(20000));
  const Ray ray = w->viewport().rayAt(centre);
  const OpResult by_ray = core.select_at_ray(ray.origin, ray.dir, SelectMode::Part);
  QVERIFY(!by_ray.data["parts"].empty());
  QCOMPARE(std::vector<int>(w->selectedParts().begin(), w->selectedParts().end()), ints(by_ray.data["parts"]));
  QCOMPARE(session_parts(*w), ints(by_ray.data["parts"]));
  // Face mode in the Top view: the selected face is the engine's face for that drawing pixel.
  QPushButton* faces = nullptr;
  for (QPushButton* b : w->viewport().findChildren<QPushButton*>())
    if (b->text() == "Faces") faces = b;
  QVERIFY(faces);
  faces->click();
  w->action("view_4")->trigger();
  surface->grab();
  const Dims dims = w->controller().view().doc.align->dims;
  const QPoint p = w->viewport().project(world_from_view(ViewRole::Top, dims, 9.5, 12.5, 0)).toPoint();
  QTest::mouseClick(surface, Qt::LeftButton, {}, p);
  QVERIFY(synced.wait(20000));
  const OpResult by_pixel = core.select_at_pixel(ViewRole::Top, 9, 12, SelectMode::Face);
  QCOMPARE(w->controller().view().selection.face, by_pixel.data["face"]["id"].get<int>());
  QCOMPARE(w->viewport().highlight().region, by_pixel.data["face"]["id"].get<int>());
  QCOMPARE(w->inspector().page(), InspectorPage::Face);
  // The face page pushes the face by a number.
  QTest::qWait(50);
  QPushButton* apply = nullptr;
  for (QPushButton* b : w->inspector().findChildren<QPushButton*>("facePushPull"))
    if (b->isVisibleTo(&w->inspector())) apply = b;
  QVERIFY(apply);
  apply->click();  // +1 px: the seat top is inside the box, so it moves
  QVERIFY(wait_idle(*w));
  QVERIFY(edit_to_json(w->controller().view().doc.edits.back())["op"] == "sweep");
  // Escape clears the selection, in the window and in the engine.
  w->action("clear_selection")->trigger();
  QVERIFY(synced.wait(20000));
  QVERIFY(w->selectedParts().empty());
  QVERIFY(w->controller().view().selection.empty());
  w->controller().cleanShutdown();
}

void GuiTest::part_masks_resolve_c_like_core() {
  auto w = make_vh_window();
  w->importFiles(fixture("C_asym_planter"));
  QVERIFY(wait_idle(*w));
  w->action("run_all")->trigger();
  QVERIFY(wait_idle(*w));
  QCOMPARE(w->inspector().statusText(), QString("NEEDS_REVIEW"));
  Session core;
  QVERIFY(core.import_views(core_views("C_asym_planter")).success);
  const int legs[3][2] = {{1, 11}, {8, 1}, {15, 11}};  // Front x0, Side z0
  int n = 0;
  for (const auto& l : legs) {
    // New part from the Part menu (the name prompt is answered by the test).
    w->setTextPrompt([&](const QString&, const QString&, const QString&) { return std::optional<QString>("leg"); });
    w->action("part_new_define")->trigger();
    QVERIFY(wait_idle(*w));
    const int id = w->editor().mask_part;
    QVERIFY(id >= 0);
    QCOMPARE(w->tool(), Tool::Parts);
    const OpResult cp = core.create_part("leg", true);
    QCOMPARE(cp.data["part"].get<int>(), id);
    if (n++ == 0) {
      // First leg, Front: painted with the brush on the drawing preview, column by column.
      w->editor().mask_radius = 0.4;
      w->editor().touch();
      SourcePreview* front = drawing(*w, ViewRole::Front);
      for (int u = l[0]; u < l[0] + 2; ++u) {
        QTest::mousePress(front, Qt::LeftButton, {}, front->widgetPos({u + 0.5, 14.5}));
        QTest::mouseMove(front, front->widgetPos({u + 0.5, 21.5}));
        QTest::mouseRelease(front, Qt::LeftButton, {}, front->widgetPos({u + 0.5, 21.5}));
        QVERIFY(wait_idle(*w));
      }
    } else {
      w->regionAction(ViewRole::Front, QRect(l[0], 14, 2, 8), "mask");  // the Volume tool's rectangle, assigned
      QVERIFY(wait_idle(*w));
    }
    w->regionAction(ViewRole::Side, QRect(l[1], 14, 2, 8), "mask");
    QVERIFY(wait_idle(*w));
    for (const auto& [view, u0] : {std::pair{ViewRole::Front, l[0]}, std::pair{ViewRole::Side, l[1]}}) {
      MaskPaintRequest r;
      r.part = id;
      r.view = view;
      for (int v = 14; v < 22; ++v)
        for (int u = u0; u < u0 + 2; ++u) r.pixels.emplace_back(u, v);
      QVERIFY(core.paint_part_mask(r).success);
    }
  }
  QVERIFY(w->controller().view().doc.masks == core.document().masks);
  w->action("validate")->trigger();
  QVERIFY(wait_idle(*w));
  std::set<std::string> blocking;
  for (const auto& b : w->controller().view().status.validation["blocking_conflicts"])
    blocking.insert(b["kind"].get<std::string>());
  QVERIFY(blocking == std::set<std::string>{"possible_recess"});  // the masks decided the legs
  w->controller().cleanShutdown();
}

void GuiTest::single_view_project_through_the_window() {
  auto w = make_window();
  QVERIFY(w->startScreen().findChild<QPushButton*>("singleViewLink"));
  SingleViewDialog d(w.get());
  d.setPath(fixture("B_hand_chair")[0]);
  QCOMPARE(d.role(), ViewRole::Front);  // from the file name
  QVERIFY(!d.settings().user_depth);     // "choose later"
  w->controller().newSingleViewProject(d.project(), d.path(), d.role(), d.settings());
  QVERIFY(wait_idle(*w));
  QVERIFY(!w->startScreenVisible());
  w->action("validate")->trigger();
  QVERIFY(wait_idle(*w));
  QCOMPARE(w->inspector().statusText(), QString("NEEDS_REVIEW"));
  bool depth_blocks = false;
  for (const auto& b : w->controller().view().status.validation["blocking_conflicts"])
    depth_blocks |= b["kind"] == "single_view_depth";
  QVERIFY(depth_blocks);
  QVERIFY(w->viewport().scene().single_view);
  QVERIFY(w->bannerText().contains("Single drawing"));
  // The inspector's single-drawing section sets the depth.
  w->clearSelection();
  QTest::qWait(50);  // new page widgets are shown by a queued call; replaced pages are deleted later
  QComboBox* preset = nullptr;
  for (QComboBox* c : w->inspector().findChildren<QComboBox*>())
    if (c->isVisibleTo(&w->inspector()) && c->findData(QString("thin")) >= 0) preset = c;
  QVERIFY(preset);
  preset->setCurrentIndex(preset->findData(QString("thin")));
  QPushButton* apply = nullptr;
  for (QPushButton* b : w->inspector().findChildren<QPushButton*>("singleViewApply"))
    if (b->isVisibleTo(&w->inspector())) apply = b;
  QVERIFY(apply);
  apply->click();
  QVERIFY(wait_idle(*w));
  w->action("validate")->trigger();
  QVERIFY(wait_idle(*w));
  for (const auto& b : w->controller().view().status.validation["blocking_conflicts"]) QVERIFY(b["kind"] != "single_view_depth");
  QCOMPARE(w->controller().view().status.metrics.value("inferred_volume_fraction", 0.0), 1.0);
  // The same three core commands directly.
  Session core;
  QVERIFY(core.new_project(d.project()).success);
  QVERIFY(core.import_views({ViewImport{ViewRole::Front, to_path(d.path()), std::nullopt}}).success);
  QVERIFY(core.set_single_view(d.settings()).success);
  QVERIFY(core.set_single_view_depth("thin", std::nullopt).success);
  QVERIFY(core.validate(ValidateOptions{}).success);
  QCOMPARE(gui_volume(*w), core.current_status().stats.volume);
  w->controller().cleanShutdown();
}

void GuiTest::aseprite_reload_keeps_pixels_and_roles() {
  QTemporaryDir dir;
  const QStringList src = fixture("A_synthetic_chair");
  const QString front = dir.filePath("chair_a.png"), side = dir.filePath("side.png"), top = dir.filePath("top.png");
  QVERIFY(QFile::copy(src[0], front) && QFile::copy(src[1], side) && QFile::copy(src[2], top));
  // A name without a role: the role chosen for this file before is reused (no prompt).
  QSettings().setValue("view_roles/" + QFileInfo(front).absoluteFilePath(), "front");
  auto w = make_window();
  w->importFiles({front, side, top});
  QVERIFY(wait_idle(*w));
  QCOMPARE(w->controller().view().doc.views.size(), size_t(3));
  auto source = [&](ViewRole r) -> const SourceView& {
    for (const SourceView& v : w->controller().view().doc.views)
      if (v.role == r) return v;
    throw std::runtime_error("missing view");
  };
  QCOMPARE(from_path(source(ViewRole::Front).original_path), front);
  // Exact pixels and palette: the imported image is the file, bit for bit.
  const QImage file = QImage(top).convertToFormat(QImage::Format_RGBA8888);
  QCOMPARE(to_qimage(*source(ViewRole::Top).original), file);
  // The drawing changes on disk (re-exported from Aseprite): Reload Drawings picks it up.
  QPoint opaque(-1, -1);
  for (int y = 0; y < file.height() && opaque.x() < 0; ++y)
    for (int x = 0; x < file.width(); ++x)
      if (file.pixelColor(x, y).alpha() == 255) {
        opaque = {x, y};
        break;
      }
  QImage edited = file;
  edited.setPixelColor(opaque, QColor(1, 2, 3));
  QVERIFY(edited.save(top, "PNG"));
  w->action("reload_views")->trigger();
  QVERIFY(wait_idle(*w));
  QCOMPARE(to_qimage(*source(ViewRole::Top).original).pixelColor(opaque), QColor(1, 2, 3));
  QCOMPARE(w->controller().view().doc.views.size(), size_t(3));
  QCOMPARE(QString::fromStdString(w->controller().view().history.back()), QString("reload_views"));
  w->controller().cleanShutdown();
}

void GuiTest::export_dialog_drives_export_model() {
  auto w = make_vh_window();
  w->importFiles(fixture("A_synthetic_chair"));
  QVERIFY(wait_idle(*w));
  w->action("validate")->trigger();
  QVERIFY(wait_idle(*w));
  QTemporaryDir dir;
  const Document& d = w->controller().view().doc;
  ExportDialog dlg(dir.filePath("chair.glb"), d.align->dims, d.settings.pixels_per_meter);
  // A preset sets format, units and facing; the file name follows the format.
  dlg.setPreset(EnginePreset::Roblox);
  ModelExportRequest r = dlg.request();
  QCOMPARE(*r.format, ExportFormat::Obj);
  QCOMPARE(*r.units, ExportUnits::Studs);
  QCOMPARE(*r.front_negative_z, true);
  QVERIFY(from_path(r.path).endsWith("chair.obj"));
  QVERIFY(!r.unlit);  // OBJ has no unlit material
  // Typing an extension picks the format.
  dlg.setPath(dir.filePath("chair.gltf"));
  QCOMPARE(*dlg.request().format, ExportFormat::Gltf);
  // The window exports exactly what the dialog asked for, through the engine command.
  dlg.setPreset(EnginePreset::Generic);
  QCOMPARE(*dlg.request().format, ExportFormat::Glb);
  dlg.setPreset(EnginePreset::Roblox);
  QCOMPARE(*dlg.request().format, ExportFormat::Obj);
  w->exportModel(dlg.request());
  QVERIFY(wait_idle(*w));
  for (const char* f : {"chair.obj", "chair.mtl", "chair.png"}) QVERIFY2(QFile::exists(dir.filePath(f)), f);
  QFile obj(dir.filePath("chair.obj"));
  QVERIFY(obj.open(QIODevice::ReadOnly));
  QVERIFY(obj.readLine().contains("units: studs, front faces -Z"));
  w->controller().cleanShutdown();
}

void GuiTest::normalisation_settings_and_view_notes() {
  // The settings dialog shows the project's values and returns the normalisation settings.
  ProjectSettings ps;
  ps.name = "hero";
  ps.normalize = NormalizeSettings{false, 96, 30, 0.15};
  NewProjectDialog d;
  d.setSettings(ps);
  QCOMPARE(qs(d.settings().name), QString("hero"));
  QVERIFY(d.settings().normalize == ps.normalize);

  // Every supported source format can be chosen or dropped.
  for (const char* f : {"a.png", "b.JPG", "c.jpeg", "d.bmp", "e.gif", "f.tga"}) QVERIFY2(is_source_image(f), f);
  QVERIFY(!is_source_image("notes.txt"));
  QVERIFY(source_image_filter().contains("*.jpg"));

  // Views on an opaque white background: the Views panel says what was done to each drawing.
  QTemporaryDir dir;
  const Rgba8 wood{90, 50, 30, 255}, seat{170, 110, 60, 255}, back{60, 90, 160, 255};
  const std::vector<p3dtest::Box> chair{{0, 0, 0, 1, 6, 1, wood}, {6, 0, 0, 7, 6, 1, wood}, {0, 0, 6, 1, 6, 7, wood},
                                        {6, 0, 6, 7, 6, 7, wood}, {0, 6, 0, 7, 8, 7, seat},  {0, 8, 6, 7, 15, 7, back}};
  QStringList files;
  for (auto& [role, img] : p3dtest::render_views(chair, Dims{7, 15, 7})) {
    ImageRGBA big = p3dtest::upscale_padded(img, 5, 10, 10, 10, 10);
    for (int y = 0; y < big.height(); ++y)
      for (int x = 0; x < big.width(); ++x)
        if (big.at(x, y).a == 0) big.at(x, y) = Rgba8{255, 255, 255, 255};
    const QString f = dir.filePath(qs(to_string(role)) + ".png");
    QVERIFY(save_png(big, to_path(f)).code == "ok");
    files << f;
  }
  auto w = make_vh_window();
  w->importFiles(files);
  QVERIFY(wait_idle(*w));
  w->action("reconstruct")->trigger();
  QVERIFY(wait_idle(*w));
  QVERIFY(w->controller().view().doc.align != nullptr);
  QVERIFY(w->controller().view().doc.align->dims == (Dims{7, 15, 7}));
  const DrawingInfo info = drawing_info(w->controller().view(), ViewRole::Front);
  QVERIFY2(info.notes.contains("background removed"), qPrintable(info.notes));
  QVERIFY2(info.notes.contains(QStringLiteral("÷5")), qPrintable(info.notes));

  // Switching normalisation off through the window's settings re-aligns with the background kept.
  SettingsUpdate u;
  u.normalize.enabled = false;
  w->controller().updateSettings(u);
  QVERIFY(wait_idle(*w));
  QVERIFY(!w->controller().view().doc.settings.normalize.enabled);
  w->action("reconstruct")->trigger();
  QVERIFY(wait_idle(*w));
  QVERIFY(w->controller().view().doc.align != nullptr);
  QVERIFY(w->controller().view().doc.align->dims.W > 7);
  QVERIFY(!drawing_info(w->controller().view(), ViewRole::Front).notes.contains("background removed"));
  w->controller().cleanShutdown();
}

void GuiTest::style_setting_shows_the_polished_model() {
  // The settings dialog offers the four styles and keeps the tuning it does not show.
  ProjectSettings ps;
  ps.style.preset = StylePreset::Clay;
  ps.style.shade_radius = 2.5;
  NewProjectDialog d;
  d.setSettings(ps);
  auto* combo = d.findChild<QComboBox*>(QStringLiteral("styleCombo"));
  QVERIFY(combo != nullptr);
  QCOMPARE(combo->count(), 4);
  QVERIFY(d.settings().style == ps.style);
  combo->setCurrentIndex(combo->findData(static_cast<int>(StylePreset::Soft)));
  QCOMPARE(d.settings().style.preset, StylePreset::Soft);
  QVERIFY(d.settings().style.shade_radius == 2.5);

  // A soft project: after Reconstruct & Validate the viewport shows the polished model.
  const QString fx = QStringLiteral(PIXEL3D_FIXTURES_DIR) + "/D_character/";
  auto w = make_vh_window();
  QVERIFY(w->windowTitle().contains(qs(version())));
  SettingsUpdate u;
  StyleSettings soft;
  soft.preset = StylePreset::Soft;
  u.style = soft;
  w->controller().updateSettings(u);
  QVERIFY(wait_idle(*w));
  w->importFiles({fx + "front.png", fx + "side.png", fx + "top.png"});
  QVERIFY(wait_idle(*w));
  w->action("run_all")->trigger();  // reconstruct + validate
  QVERIFY(wait_idle(*w, 600000));
  const Document& doc = w->controller().view().doc;
  QVERIFY(doc.gate != nullptr);
  QVERIFY(doc.styled != nullptr);
  QCOMPARE(doc.gate->status, ValidationStatus::Pass);
  const Scene scene = scene_from(doc);
  QVERIFY(scene.styled);
  QVERIFY(scene.mesh.get() == &doc.styled->texture.mesh);
  // Back to crisp: the crisp model is shown again.
  u.style = StyleSettings{};
  w->controller().updateSettings(u);
  QVERIFY(wait_idle(*w));
  QVERIFY(!scene_from(w->controller().view().doc).styled);
  w->controller().cleanShutdown();
}

void GuiTest::lowpoly_studio_through_the_window() {
  // The project dialog offers the mode and keeps the build settings it does not show.
  {
    NewProjectDialog d;
    ProjectSettings ps;
    ps.lowpoly.build.tri_budget = 1200;
    d.setSettings(ps);
    auto* mode = d.findChild<QComboBox*>(QStringLiteral("projectMode"));
    QVERIFY(mode);
    QVERIFY(!d.settings().lowpoly.enabled);  // shows the project's own type
    mode->setCurrentIndex(mode->findData(true));
    QVERIFY(d.settings().lowpoly.enabled);
    QCOMPARE(d.settings().lowpoly.build.tri_budget, 1200);
  }
  auto w = make_window();
  ProjectSettings ps;
  ps.name = "cloaked";
  ps.lowpoly.enabled = true;
  w->controller().newProject(ps);
  QVERIFY(wait_idle(*w));
  const QString dir = QStringLiteral(PIXEL3D_FIXTURES_DIR) + "/E_lowpoly_character/cloaked/";
  w->importFiles({dir + "front.png", dir + "side.png"});  // Front + Side, no Top
  QVERIFY(wait_idle(*w));
  QPushButton* start = w->startScreen().reconstructButton();
  QCOMPARE(start->text(), QString("Build character"));
  QVERIFY(start->isEnabled());
  QVERIFY(!w->action("validate")->isEnabled());  // the visual-hull steps do not run here
  // Suggest: a proposal is shown first; declined = nothing changes.
  int asked = 0;
  w->setConfirmHandler([&](const QString&, const QString&) {
    ++asked;
    return false;
  });
  start->click();
  QVERIFY(wait_idle(*w));
  QCOMPARE(asked, 1);
  QVERIFY(w->controller().view().doc.masks.parts.empty());
  QVERIFY(!w->startScreenVisible());
  w->setConfirmHandler([&](const QString&, const QString& text) {
    ++asked;
    return text.contains("head") || text.contains("NEEDS REVIEW");
  });
  w->runAction("lowpoly_suggest");
  QVERIFY(wait_idle(*w));
  const Document& d = w->controller().view().doc;
  QVERIFY(d.masks.parts.size() >= 3);
  QCOMPARE(d.part_types.size(), d.masks.parts.size());
  // The Inspector shows the Low-Poly page with one type selector per part.
  auto* panel = w->inspector().findChild<LowpolyPanel*>();
  QVERIFY(panel);
  QCOMPARE(panel->partsTable()->rowCount(), static_cast<int>(d.masks.parts.size()));
  QVERIFY(status_button(*w));
  QCOMPARE(status_button(*w)->property("action").toString(), QString("lowpoly_build"));
  // Build through the page's button.
  panel->buildButton()->click();
  QVERIFY(wait_idle(*w));
  QVERIFY(w->controller().view().status.status != ValidationStatus::NotValidated);
  QVERIFY(w->viewport().scene().lowpoly);
  QVERIFY(w->viewport().scene().triangles() > 0);
  QVERIFY(w->viewport().scene().triangles() <= 1000);
  // A smaller budget from the spin box: never exceeded.
  panel = w->inspector().findChild<LowpolyPanel*>();
  auto* budget = panel->findChild<QSpinBox*>("lowpolyBudget");
  QVERIFY(budget);
  budget->setValue(600);
  panel->buildButton()->click();
  QVERIFY(wait_idle(*w));
  QCOMPARE(w->controller().view().doc.settings.lowpoly.build.tri_budget, 600);
  QVERIFY(w->viewport().scene().triangles() <= 600);
  // Undo returns to the earlier budget and its build.
  w->action("undo")->trigger();
  QVERIFY(wait_idle(*w));
  QCOMPARE(w->controller().view().doc.settings.lowpoly.build.tri_budget, 1000);
  QVERIFY(w->viewport().scene().lowpoly);
  // Test pose: the same triangles, moved by the rig.
  const auto rest = w->viewport().scene().mesh->positions;
  w->setLowpolyPose(true);
  QVERIFY(w->lowpolyPose());
  QCOMPARE(w->viewport().scene().mesh->positions.size(), rest.size());
  QVERIFY(w->viewport().scene().mesh->positions != rest);
  w->setLowpolyPose(false);
  // Changing a part type through its selector drops the build until it is rebuilt.
  panel = w->inspector().findChild<LowpolyPanel*>();
  const int first = w->controller().view().doc.masks.parts.begin()->first;
  auto* type = panel->findChild<QComboBox*>(QStringLiteral("lowpolyType_%1").arg(first));
  QVERIFY(type);
  const int other = type->currentIndex() == 3 ? 4 : 3;  // accessory or cloth
  type->setCurrentIndex(other);
  emit type->activated(other);
  QVERIFY(wait_idle(*w));
  QVERIFY(!w->viewport().scene().lowpoly);
  w->action("undo")->trigger();
  QVERIFY(wait_idle(*w));
  QVERIFY(w->viewport().scene().lowpoly);
  // Export: asked when the character needs review, written only with consent.
  QTemporaryDir tmp;
  const QString glb = tmp.filePath("cloaked.glb");
  w->setSavePathPrompt([&](const QString&) { return glb; });
  const int before = asked;
  w->runAction("export");
  QVERIFY(wait_idle(*w));
  if (w->controller().view().status.status == ValidationStatus::NeedsReview) QCOMPARE(asked, before + 1);
  QVERIFY(QFile::exists(glb));
  QVERIFY(QFile::exists(tmp.filePath("cloaked_toon.gdshader")));
  w->controller().cleanShutdown();
  // Project Settings switch an existing project's type (one more undoable step).
  auto s = make_vh_window();
  s->importFiles(fixture("A_synthetic_chair"));
  QVERIFY(wait_idle(*s));
  ProjectSettings changed = s->controller().view().doc.settings;
  changed.lowpoly.enabled = true;
  s->applySettings(changed);
  QVERIFY(wait_idle(*s));
  QVERIFY(s->controller().view().doc.settings.lowpoly.enabled);
  changed.lowpoly.enabled = false;
  s->applySettings(changed);
  QVERIFY(wait_idle(*s));
  QVERIFY(!s->controller().view().doc.settings.lowpoly.enabled);
  s->controller().cleanShutdown();
}

void GuiTest::layered_drawings_open_as_a_lowpoly_project() {
  const QString g = QStringLiteral(PIXEL3D_FIXTURES_DIR) + "/G_layered/";
  for (const char* f : {"x.webp", "x.aseprite", "x.ase", "x.PSD"}) QVERIFY2(is_source_image(f), f);
  QVERIFY(is_layered_source("x.aseprite") && is_layered_source("x.psd") && !is_layered_source("x.webp"));
  auto w = make_window();
  // File > Open with a drawing: a new Low-Poly Studio project, Front only, and the layer choice.
  int offered = 0;
  w->setLayerChooser([&](const nlohmann::json& layers) -> std::optional<QStringList> {
    ++offered;
    QStringList names;
    for (const auto& l : layers) names << QString::fromStdString(l["name"].get<std::string>());
    if (!names.contains("hat_hidden")) return std::nullopt;  // the hidden layer is listed (not suggested)
    return QStringList{"body", "head"};
  });
  w->openDrawingsAsLowpoly({g + "rgb_layers.aseprite"});
  QVERIFY(wait_idle(*w));
  QCOMPARE(offered, 1);
  const Document& d = w->controller().view().doc;
  QVERIFY(d.settings.lowpoly.enabled);
  QCOMPARE(d.views.size(), size_t(1));
  QCOMPARE(d.views.front().role, ViewRole::Front);
  QCOMPARE(d.masks.parts.size(), size_t(2));
  QStringList names;
  for (const auto& [id, n] : d.part_names) names << QString::fromStdString(n);
  names.sort();
  QCOMPARE(names, QStringList({"body", "head"}));
  // Front only: the banner says every depth is inferred.
  QVERIFY(w->bannerText().contains("INFERRED"));
  auto* panel = w->inspector().findChild<LowpolyPanel*>();
  QVERIFY(panel);
  QVERIFY(panel->layersButton()->isVisibleTo(panel));
  QVERIFY(panel->findChild<QLabel*>("lowpolyFrontOnlyNote"));
  panel->buildButton()->click();
  QVERIFY(wait_idle(*w));
  QCOMPARE(w->controller().view().status.status, ValidationStatus::NeedsReview);
  panel = w->inspector().findChild<LowpolyPanel*>();
  QVERIFY(panel);
  for (int r = 0; r < panel->partsTable()->rowCount(); ++r) {
    QVERIFY(panel->partsTable()->item(r, 3));
    QCOMPARE(panel->partsTable()->item(r, 3)->text(), QString("INFERRED"));
  }
  // The Side slot takes a PSD dropped on the window (role from the file name is unknown: given here).
  w->importFiles({g + "krita_layers.psd"}, ViewRole::Side);
  QVERIFY(wait_idle(*w));
  QCOMPARE(w->controller().view().doc.views.size(), size_t(2));
  QVERIFY(!w->bannerText().contains("Front only"));
  // Drop onto the window: a WebP with its role in the name goes through the same import.
  QMimeData mime;
  const QString webp = QDir::temp().filePath("front.webp");
  QFile::remove(webp);
  QVERIFY(QFile::copy(g + "lossless.webp", webp));
  mime.setUrls({QUrl::fromLocalFile(webp)});
  // As a real drag: the window accepts the DragEnter, then receives the Drop.
  QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(w.get(), &enter);
  QVERIFY(enter.isAccepted());
  QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(w.get(), &drop);
  QVERIFY(wait_idle(*w));
  bool webp_front = false;
  for (const SourceView& v : w->controller().view().doc.views)
    webp_front |= v.role == ViewRole::Front && QString::fromStdString(v.original_path).endsWith("front.webp");
  QVERIFY(webp_front);
  w->controller().cleanShutdown();
}

namespace {

QStringList front_side(const QString& fixture_dir) {
  const QString d = QStringLiteral(PIXEL3D_FIXTURES_DIR) + "/" + fixture_dir + "/";
  return {d + "front.png", d + "side.png"};
}

// Every state of the window offers a next step: a guidance action the user can click.
void verify_next_step(MainWindow& w) {
  const Guidance g = next_step(w.controller().view(), w.inspector().issues());
  QVERIFY2(!g.action.isEmpty() && !g.action_label.isEmpty(), qPrintable(g.text));
}

// A real drag of files onto `target`: DragEnter (returns whether it was accepted), then Drop.
bool drop_on(QWidget* target, const QStringList& files) {
  QMimeData mime;
  QList<QUrl> urls;
  for (const QString& f : files) urls << QUrl::fromLocalFile(f);
  mime.setUrls(urls);
  QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(target, &enter);
  QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(target, &drop);
  return enter.isAccepted();
}

}  // namespace

void GuiTest::new_projects_default_to_lowpoly_studio() {
  // File > New: the dialog starts on Low-Poly Studio; Visual Hull says what it needs.
  {
    NewProjectDialog d;
    QVERIFY(d.settings().lowpoly.enabled);
    auto* mode = d.findChild<QComboBox*>(QStringLiteral("projectMode"));
    QVERIFY(mode);
    QCOMPARE(mode->currentIndex(), 0);
    QVERIFY(mode->itemText(mode->findData(false)).contains("needs Front + Side + Top"));
    QVERIFY(default_project_settings().lowpoly.enabled);
  }
  // The window's first project is Low-Poly Studio, and the type is in sight everywhere.
  auto w = make_window();
  QVERIFY(w->controller().view().doc.settings.lowpoly.enabled);
  QVERIFY(!w->action("undo")->isEnabled());  // starting the project is not an edit
  QVERIFY(w->windowTitle().contains("Low-Poly Studio"));
  QCOMPARE(w->projectTypeChip()->text(), QString("Low-Poly Studio"));
  QVERIFY(w->action("mode_lowpoly")->isChecked());
  QVERIFY(w->startScreen().projectTypeLabel()->text().contains("Low-Poly Studio"));
  QVERIFY(w->startScreen().projectTypeSwitch()->text().contains("Visual Hull"));
  QVERIFY(drawing_info(w->controller().view(), ViewRole::Top).optional);
  QVERIFY(!drawing_info(w->controller().view(), ViewRole::Front).optional);
  // Front + Side (no Top), as an artist usually has: no error, and the character can be built.
  w->importFiles(front_side("E_lowpoly_character/cloaked"));
  QVERIFY(wait_idle(*w));
  QVERIFY(w->lastError().isEmpty());
  QCOMPARE(w->controller().view().doc.views.size(), size_t(2));
  QVERIFY(w->startScreen().reconstructButton()->isEnabled());
  QCOMPARE(w->startScreen().reconstructButton()->text(), QString("Build character"));
  QVERIFY(!w->startScreen().missingNotice()->isVisibleTo(&w->startScreen()));
  QVERIFY(!w->viewsPanel().missingNotice()->isVisibleTo(&w->viewsPanel()));
  QCOMPARE(next_step(w->controller().view(), w->inspector().issues()).action, QString("lowpoly_suggest"));
  // Switching the type from the Views panel chip's menu keeps the drawings and shows the new type.
  QVERIFY(w->projectTypeChip()->menu()->actions().contains(w->action("mode_visual_hull")));
  w->action("mode_visual_hull")->trigger();
  QVERIFY(wait_idle(*w));
  QVERIFY(!w->controller().view().doc.settings.lowpoly.enabled);
  QCOMPARE(w->controller().view().doc.views.size(), size_t(2));
  QVERIFY(w->windowTitle().contains("Visual Hull"));
  QCOMPARE(w->projectTypeChip()->text(), QString("Visual Hull"));
  QVERIFY(w->action("mode_visual_hull")->isChecked());
  QVERIFY(w->startScreen().projectTypeLabel()->text().contains("needs Front + Side + Top"));
  QVERIFY(!drawing_info(w->controller().view(), ViewRole::Top).optional);
  // Visual Hull without the Top: the way back is one click, and it is one undoable step.
  QVERIFY(w->startScreen().missingNotice()->isVisibleTo(&w->startScreen()));
  w->startScreen().missingNotice()->switchButton()->click();
  QVERIFY(wait_idle(*w));
  QVERIFY(w->controller().view().doc.settings.lowpoly.enabled);
  QCOMPARE(w->controller().view().doc.views.size(), size_t(2));
  QVERIFY(w->windowTitle().contains("Low-Poly Studio"));
  w->action("undo")->trigger();
  QVERIFY(wait_idle(*w));
  QVERIFY(!w->controller().view().doc.settings.lowpoly.enabled);
  w->controller().cleanShutdown();
}

void GuiTest::visual_hull_without_top_offers_lowpoly_studio() {
  // An explicit Visual Hull project gets Front + Side only (the reported bug: "required view missing: top").
  auto w = make_vh_window();
  QVERIFY(w->windowTitle().contains("Visual Hull"));
  w->importFiles(front_side("E_lowpoly_character/cloaked"));
  QVERIFY(wait_idle(*w));
  MissingViewsNotice* notice = w->viewsPanel().missingNotice();
  QVERIFY(notice->isVisibleTo(&w->viewsPanel()));
  QVERIFY(notice->findChild<QLabel*>("missingViewsText")->text().contains("Top"));
  QCOMPARE(notice->switchButton()->text(), QString("Switch to Low-Poly Studio\n(no Top needed)"));
  QVERIFY(notice->minimumSizeHint().width() < theme::metric::dock_min_left);  // never widens the Views panel
  QVERIFY(notice->addButton()->text().contains("Top"));
  QVERIFY(w->startScreen().missingNotice()->isVisibleTo(&w->startScreen()));
  const Guidance g = next_step(w->controller().view(), w->inspector().issues());
  QCOMPARE(g.action, QString("switch_lowpoly"));
  QCOMPARE(g.action_label, QString("Switch to Low-Poly Studio (no Top needed)"));
  QCOMPARE(status_button(*w)->property("action").toString(), QString("switch_lowpoly"));  // the Inspector too
  // Reconstruct (F5) does not fail: it says what is missing and offers the switch. Cancel = nothing happens.
  int asked = 0;
  QString said;
  ViewRole missing = ViewRole::Front;
  w->setMissingViewsPrompt([&](const QString& text, ViewRole r) {
    ++asked, said = text, missing = r;
    return MainWindow::MissingViewsChoice::Cancel;
  });
  const size_t log_before = w->controller().view().log.size();
  w->showWorkspace();
  QVERIFY(w->action("run_all")->isEnabled());
  w->action("run_all")->trigger();
  QVERIFY(wait_idle(*w));
  QCOMPARE(asked, 1);
  QCOMPARE(missing, ViewRole::Top);
  QVERIFY(said.contains("Top"));
  QCOMPARE(w->controller().view().log.size(), log_before);  // no failed engine step
  QVERIFY(!w->controller().view().doc.settings.lowpoly.enabled);
  // Validate and Export ask the same way.
  w->action("validate")->trigger();
  w->runAction("export");
  QCOMPARE(asked, 3);
  // The engine refusing anyway (a step started without the window's check): the hint still offers the switch.
  w->controller().runAll(ReconstructMode::TrueSolid);
  QVERIFY(wait_idle(*w));
  QCOMPARE(w->lastError(), QString("view.missing_required"));
  QCOMPARE(next_step(w->controller().view(), w->inspector().issues()).action, QString("switch_lowpoly"));
  // "Switch to Low-Poly Studio": one click keeps the drawings and goes on to the character (parts suggested).
  w->setMissingViewsPrompt([&](const QString&, ViewRole) {
    ++asked;
    return MainWindow::MissingViewsChoice::SwitchToLowpoly;
  });
  int confirmed = 0;
  w->setConfirmHandler([&](const QString&, const QString&) {
    ++confirmed;
    return true;
  });
  w->action("run_all")->trigger();
  QVERIFY(wait_idle(*w));
  QCOMPARE(asked, 4);
  QCOMPARE(confirmed, 1);  // the suggested parts are shown before they are applied
  const Document& d = w->controller().view().doc;
  QVERIFY(d.settings.lowpoly.enabled);
  QCOMPARE(d.views.size(), size_t(2));
  QVERIFY(!d.masks.parts.empty());
  QVERIFY(w->windowTitle().contains("Low-Poly Studio"));
  QVERIFY(!notice->isVisibleTo(&w->viewsPanel()));
  QCOMPARE(next_step(w->controller().view(), w->inspector().issues()).action, QString("lowpoly_build"));
  w->controller().cleanShutdown();
  // The Views panel's own button does the same switch.
  auto v = make_vh_window();
  v->importFiles(front_side("A_synthetic_chair"));
  QVERIFY(wait_idle(*v));
  QVERIFY(v->viewsPanel().missingNotice()->isVisibleTo(&v->viewsPanel()));
  v->viewsPanel().missingNotice()->switchButton()->click();
  QVERIFY(wait_idle(*v));
  QVERIFY(v->controller().view().doc.settings.lowpoly.enabled);
  QCOMPARE(v->controller().view().doc.views.size(), size_t(2));
  v->controller().cleanShutdown();
}

void GuiTest::drawings_without_top_never_end_in_a_dead_end() {
  const QStringList fs = front_side("E_lowpoly_character/cloaked");
  // Dropped on the start screen of a new window (Low-Poly Studio): a character project, ready for its parts.
  {
    auto w = make_window();
    QVERIFY(drop_on(&w->startScreen(), fs));
    QVERIFY(wait_idle(*w));
    QVERIFY(w->lastError().isEmpty());
    QCOMPARE(w->controller().view().doc.views.size(), size_t(2));
    verify_next_step(*w);
    // The Front alone (the most common case) is enough too.
    auto f = make_window();
    QVERIFY(drop_on(f.get(), {fs[0]}));
    QVERIFY(wait_idle(*f));
    QCOMPARE(f->controller().view().doc.views.size(), size_t(1));
    QVERIFY(f->startScreen().reconstructButton()->isEnabled());
    verify_next_step(*f);
    f->controller().cleanShutdown();
    w->controller().cleanShutdown();
  }
  // Dropped on a Visual Hull project (start screen, then the window): told what is missing, with the switch.
  {
    auto w = make_vh_window();
    QVERIFY(drop_on(&w->startScreen(), fs));
    QVERIFY(wait_idle(*w));
    QCOMPARE(w->controller().view().doc.views.size(), size_t(2));
    QVERIFY(w->startScreen().missingNotice()->isVisibleTo(&w->startScreen()));
    QVERIFY(!w->startScreen().reconstructButton()->isEnabled());  // Reconstruct waits; the switch is offered instead
    verify_next_step(*w);
    w->controller().cleanShutdown();
    auto v = make_vh_window();
    v->showWorkspace();
    QVERIFY(drop_on(v.get(), {fs[0]}));  // the Front only
    QVERIFY(wait_idle(*v));
    QVERIFY(v->viewsPanel().missingNotice()->isVisibleTo(&v->viewsPanel()));
    QVERIFY(v->viewsPanel().missingNotice()->findChild<QLabel*>("missingViewsText")->text().contains("Side and Top"));
    const QString hint = next_step(v->controller().view(), v->inspector().issues()).action;
    QCOMPARE(hint, QString("switch_lowpoly"));
    v->runAction(hint);  // the hint's button
    QVERIFY(wait_idle(*v));
    QVERIFY(v->controller().view().doc.settings.lowpoly.enabled);
    verify_next_step(*v);
    QVERIFY(v->bannerText().contains("Front only"));
    v->controller().cleanShutdown();
  }
  // File > Open with drawings from a Visual Hull project: a new Low-Poly Studio project with them.
  {
    auto w = make_vh_window();
    w->openDrawingsAsLowpoly(fs);
    QVERIFY(wait_idle(*w));
    QVERIFY(w->controller().view().doc.settings.lowpoly.enabled);
    QCOMPARE(w->controller().view().doc.views.size(), size_t(2));
    QVERIFY(w->windowTitle().contains("Low-Poly Studio"));
    verify_next_step(*w);
    w->controller().cleanShutdown();
  }
}

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));  // as Pixel3D.exe
  QApplication::setOrganizationName("Pixel3D-tests");
  QApplication::setApplicationName("Pixel3D-tests");
  QTemporaryDir settings;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
  QSettings().clear();  // default layout, no recent files
  GuiTest t;
  return QTest::qExec(&t, argc, argv);
}

#include "test_gui.moc"
