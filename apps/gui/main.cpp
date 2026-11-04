// 3re-studio.exe — the 3re desktop application (spec docs/superpowers/specs/2026-10-20-3re-design.md;
// earlier GUI specs: 2026-10-07-pixel3d-m3-gui.md, 2026-10-07-pixel3d-ux-redesign.md).
#include <cstdio>
#include <QDateTime>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QEventLoop>
#include <QAction>
#include <QApplication>
#include <QCommandLineParser>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QImageReader>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <memory>

#include "backends/retexture_batch.hpp"
#include "app_identity.hpp"
#include "backends/settings_env.hpp"
#include "dialogs.hpp"
#include "inspector.hpp"
#include "main_window.hpp"
#include "pixel3d/common/crash_report.hpp"
#include "pixel3d/common/version.hpp"
#include "pixel3d/engine/lowpoly_stage.hpp"
#include "qt_util.hpp"
#include "self_test.hpp"
#include "segmentation/mask_set.hpp"
#include "studio/agent_chat_panel.hpp"
#include "studio/agent_page.hpp"
#include "studio/e2e_driver.hpp"
#include "studio/image3d_workspace.hpp"
#include "studio/settings_page.hpp"
#include "studio/splash.hpp"
#include "studio/studio_window.hpp"
#include "studio/update_ui.hpp"
#include "theme.hpp"
#include "viewport.hpp"
#include "views_panel.hpp"

using namespace p3dgui;

namespace {

// Once per crash: the previous run left a report (spec M5 §2). Autosave recovery follows.
void mention_crash_report(QWidget* parent) {
  const auto reports = pixel3d::list_crash_reports();
  if (reports.empty()) return;
  const QString newest = from_path(reports.back().filename());
  QSettings settings;
  if (newest <= settings.value("crash/last_seen").toString()) return;
  settings.setValue("crash/last_seen", newest);
  QMessageBox box(QMessageBox::Information, QObject::tr("3re closed unexpectedly"),
                  QObject::tr("3re closed unexpectedly last time. A crash report was saved:\n%1\n\n"
                              "Autosaved work is offered for recovery next.")
                      .arg(QDir::toNativeSeparators(from_path(reports.back()))),
                  QMessageBox::Ok, parent);
  QPushButton* open = box.addButton(QObject::tr("Open Folder"), QMessageBox::ActionRole);
  box.exec();
  if (box.clickedButton() == open) QDesktopServices::openUrl(QUrl::fromLocalFile(from_path(pixel3d::crash_dir())));
}

// The stored interface size (Settings > Display), read before the application exists.
double stored_ui_scale(const QString& portableData) {
  if (!portableData.isEmpty())
    return QSettings(QDir(portableData).absoluteFilePath("Settings/3re/3re.ini"), QSettings::IniFormat).value("ui/scale", 0.0).toDouble();
  return QSettings("3re", "3re").value("ui/scale", 0.0).toDouble();
}

bool wants(int argc, char** argv, const char* option) {
  for (int i = 1; i < argc; ++i)
    if (qstrcmp(argv[i], option) == 0) return true;
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  pixel3d::install_crash_handler("3re");
  QSurfaceFormat fmt;
  fmt.setVersion(3, 3);
  fmt.setProfile(QSurfaceFormat::CoreProfile);
  fmt.setDepthBufferSize(24);
  fmt.setSamples(4);
  QSurfaceFormat::setDefaultFormat(fmt);

  const auto portableData = p3dasset::setting_env("PORTABLE_USERDATA");
  const bool scripted = wants(argc, argv, "--screenshot") || wants(argc, argv, "--self-test");
  if (const double scale = stored_ui_scale(portableData); scale > 0 && !scripted && qEnvironmentVariableIsEmpty("QT_SCALE_FACTOR"))
    qputenv("QT_SCALE_FACTOR", QByteArray::number(scale, 'f', 2));
  QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

  QApplication app(argc, argv);
  // One number format everywhere (labels, spin boxes, exported reports): the UI is English.
  QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
  if (!portableData.isEmpty()) {
    const auto settingsPath = QDir(portableData).absoluteFilePath("Settings");
    if (!QDir().mkpath(settingsPath)) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsPath);
  }
  QString buildVersion = qs(pixel3d::version());
#ifdef PIXEL3D_BUILD_LABEL
  buildVersion += QStringLiteral("+") + QStringLiteral(PIXEL3D_BUILD_LABEL);
#endif
  identity::apply(app);
  QApplication::setApplicationVersion(buildVersion);
  theme::apply(app);

  QCommandLineParser cli;
  cli.setApplicationDescription("3re — images and pixel art to validated 3D models (GLB, glTF, OBJ)");
  cli.addHelpOption();
  cli.addVersionOption();
  const QCommandLineOption software(QStringLiteral("software-view"), "Use the software 3D view instead of OpenGL.");
  const QCommandLineOption autosave_dir(QStringLiteral("autosave-dir"), "Folder for autosaves.", "dir");
  const QCommandLineOption no_recovery(QStringLiteral("no-recovery"), "Do not offer to recover autosaved work.");
  const QCommandLineOption image_to_3d(QStringLiteral("image-to-3d"), "Open the Image to 3D workspace (optionally with an image: --image-to-3d --open-image F).");
  const QCommandLineOption open_image(QStringLiteral("open-image"), "Import this picture into Image to 3D.", "file");
  const QCommandLineOption open_result(QStringLiteral("open-result"), "Open a saved generated GLB in Image to 3D (checks it again; no inference).", "glb");
  const QCommandLineOption screenshot(QStringLiteral("screenshot"), "Run the given views, save a window screenshot, exit.", "png");
  const QCommandLineOption preset(QStringLiteral("preset"), "Camera for --screenshot: front, side, top, ... or iso.", "view");
  const QCommandLineOption size(QStringLiteral("size"), "Window size for --screenshot, e.g. 1366x768.", "WxH");
  const QCommandLineOption open_issue(QStringLiteral("open-issue"), "For --screenshot: open issue N (0 = first).", "n");
  const QCommandLineOption select_part(QStringLiteral("select-part"), "For --screenshot: select part N.", "id");
  const QCommandLineOption tool(QStringLiteral("tool"), "For --screenshot: select, depth, pushpull, volume or parts.", "name");
  const QCommandLineOption workspace(QStringLiteral("workspace"), "For --screenshot: show the workspace even without a model.");
  const QCommandLineOption no_run(QStringLiteral("no-run"), "For --screenshot: import the views but do not reconstruct.");
  const QCommandLineOption toggle(QStringLiteral("toggle"), "For --screenshot: toggle a display action (wireframe, show_estimated, ...).", "action");
  const QCommandLineOption dialog(QStringLiteral("dialog"), "For --screenshot: capture this dialog instead of the window (export, settings, image3d).", "name");
  const QCommandLineOption page(QStringLiteral("page"), "Show this workspace: home, image, retopo, export, rigging, animation, settings.", "id");
  const QCommandLineOption image_mode(QStringLiteral("image-mode"), "For the image workspace: ai or pixel.", "mode");
  const QCommandLineOption step(QStringLiteral("step"), "For --screenshot of Image to 3D: import, segment, generate or preview.", "name");
  const QCommandLineOption advanced(QStringLiteral("advanced"), "For --screenshot: show advanced options.");
  const QCommandLineOption self_test(QStringLiteral("self-test"), "Check that this 3re folder is complete and self-contained, print the result and exit (0 = pass).");
  const QCommandLineOption deep(QStringLiteral("deep"), "For --self-test: also verify every model checksum (slower).");
  const QCommandLineOption json(QStringLiteral("json"), "For --self-test: also write the result as JSON.", "file");
  const QCommandLineOption e2e(QStringLiteral("e2e-generate"), "End-to-end check: import this picture, generate, export, exit.", "file");
  const QCommandLineOption e2e_out(QStringLiteral("e2e-out"), "For --e2e-generate: the summary JSON.", "file");
  const QCommandLineOption e2e_shot(QStringLiteral("e2e-shot"), "For --e2e-generate: a screenshot of the result.", "png");
  const QCommandLineOption project_dir(QStringLiteral("project-dir"), "For --e2e-generate: where the project is created.", "dir");
  const QCommandLineOption region_click(QStringLiteral("region-click"), "For --e2e-generate: one SAM region from a click at X,Y (picture pixels).", "x,y");
  const QCommandLineOption e2e_view(QStringLiteral("e2e-view"), "For --e2e-generate: another angle of the subject, LABEL=PICTURE (back, left, right, front_left, ..., auto; repeatable).", "label=file");
  const QCommandLineOption e2e_seed(QStringLiteral("e2e-seed"), "For --e2e-generate: the generation seed (default: the workspace's, 42).", "n");
  const QCommandLineOption retexture_all(QStringLiteral("retexture-all"), "Re-texture every saved model that has TRELLIS's texture (native.post) with the current engine, print progress, write the summary JSON next to the projects and exit.");
  const QCommandLineOption retexture_root(QStringLiteral("retexture-root"), "For --retexture-all: search this folder instead of the default projects/results folders (repeatable).", "dir");
  const QCommandLineOption view_angle(QStringLiteral("view-angle"), "For --screenshot of an Image to 3D result: camera yaw,pitch in degrees (yaw 0 = front, 90 = from +X).", "yaw,pitch");
  const QCommandLineOption view_focus(QStringLiteral("view-focus"), "For --screenshot with --view-angle: frame the sphere X,Y,Z,R given in the model's GLB coordinates.", "x,y,z,r");
  const QCommandLineOption e2e_retopo(QStringLiteral("e2e-retopo"), "End-to-end Smart Retopology: open this GLB/OBJ/project, retopologize, save GLB, exit.", "file");
  const QCommandLineOption retopo_faces(QStringLiteral("retopo-faces"), "For --e2e-retopo: the target polygon count.", "n");
  const QCommandLineOption agent_run(QStringLiteral("agent-run"), "Show this 3re-agent run folder in the Agent workspace (live while it runs).", "dir");
  const QCommandLineOption agent_scroll(QStringLiteral("agent-scroll"), "For --screenshot with --agent-run: scroll to question, final, top or item N.", "where");
  const QCommandLineOption agent_conversation(QStringLiteral("agent-conversation"), "Reopen this chat conversation folder on the Agent page (its run is followed again).", "dir");
  const QCommandLineOption agent_full(QStringLiteral("agent-full"), "For --screenshot with --agent-run: capture the whole timeline (tall picture) instead of the window.");
  const QCommandLineOption agent_answer(QStringLiteral("agent-answer"), "With --agent-run: answer the open question with this option (as its button does), e.g. needed or picture.", "choice");
  const QCommandLineOption agent_picture(QStringLiteral("agent-picture"), "With --agent-answer picture: the picture file and its side, FILE or FILE=back|left|right.", "file");
  const QCommandLineOption region_box(QStringLiteral("region-box"), "For --e2e-generate: one SAM region from a box X0,Y0,X1,Y1 (picture pixels).", "box");
  cli.addOptions({software, autosave_dir, no_recovery, image_to_3d, open_image, open_result, screenshot, preset, size, open_issue, select_part, tool,
                  workspace, no_run, toggle, dialog, page, image_mode, step, advanced, self_test, deep, json, e2e, e2e_out, e2e_shot,
                  project_dir, region_click, region_box, retexture_all, retexture_root, view_angle, view_focus, e2e_view,
                  e2e_seed, e2e_retopo, retopo_faces, agent_run, agent_scroll, agent_full, agent_answer, agent_picture, agent_conversation});
  cli.addPositionalArgument("files", "Project (.p3d or project.3re) or view PNGs (front/side/top in the file names).");
  cli.process(app);

  if (cli.isSet(self_test)) {
    const auto items = run_self_test(QCoreApplication::applicationDirPath(), cli.isSet(deep));
    QTextStream out(stdout);
    for (const auto& i : items) out << (i.ok ? "PASS " : i.fatal ? "FAIL " : "WARN ") << i.name << " - " << i.detail << '\n';
    const bool passed = self_test_passed(items);
    out << (passed ? "SELF-TEST PASSED" : "SELF-TEST FAILED") << '\n';
    out.flush();
    if (cli.isSet(json)) {
      QFile f(cli.value(json));
      if (f.open(QIODevice::WriteOnly)) f.write(self_test_json(items));
    }
    return passed ? 0 : 1;
  }

  if (cli.isSet(retexture_all)) {
    QTextStream out(stdout);
    const QStringList roots = cli.isSet(retexture_root) ? cli.values(retexture_root) : p3dasset::RetextureBatch::defaultRoots();
    const auto jobs = p3dasset::RetextureBatch::scan(roots);
    out << "RETEXTURE " << jobs.size() << " saved model(s)\n";
    for (const auto& j : jobs) out << "  " << QDir::toNativeSeparators(j.run_dir) << (j.view_args.isEmpty() ? "" : QString(" (+%1 angle(s))").arg(j.view_args.size() / 2)) << '\n';
    out.flush();
    p3dasset::RetextureBatch batch;
    QEventLoop loop;
    QJsonObject summary;
    QObject::connect(&batch, &p3dasset::RetextureBatch::progress, [&out](int done, int total, const QString& text) {
      out << "[" << done << "/" << total << "] " << text << '\n';
      out.flush();
    });
    QObject::connect(&batch, &p3dasset::RetextureBatch::finished, [&](bool, const QJsonObject& s) { summary = s; loop.quit(); });
    if (batch.start(jobs, QDir(QCoreApplication::applicationDirPath()).filePath("pixel3d-auto-texture.exe"))) loop.exec();
    else summary = batch.summary();
    summary["roots"] = QJsonArray::fromStringList(roots);
    const QString file = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/3re")
                             .filePath("retexture-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".json");
    QDir().mkpath(QFileInfo(file).absolutePath());
    QFile f(file);
    if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(summary).toJson());
    int ok = 0;
    for (const auto& r : summary.value("runs").toArray()) ok += r.toObject().value("status").toString() == "OK";
    out << "RETEXTURE DONE " << ok << "/" << jobs.size() << " ok · summary " << QDir::toNativeSeparators(file) << '\n';
    out.flush();
    return summary.value("status").toString() == "ERROR" ? 1 : 0;
  }

  std::unique_ptr<Splash> splash;
  if (!cli.isSet(screenshot) && !cli.isSet(e2e) && !cli.isSet(e2e_retopo)) {
    splash = std::make_unique<Splash>();
    splash->show();
    splash->setStatus(QObject::tr("Preparing the workspace…"));
  }

  WindowOptions opt;
  opt.software_view = cli.isSet(software) || QSettings().value("ui/software_view", false).toBool();
  opt.autosave_dir = cli.isSet(autosave_dir) ? cli.value(autosave_dir)
                                             : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/autosave";
  StudioWindow studio(opt);
  MainWindow& w = studio.pixelBuilder();
  if (splash) splash->setStatus(QObject::tr("Opening 3re…"));
  if (cli.isSet(screenshot)) {
    // Reproducible pictures: default layout and size, no advanced details.
    w.resetLayout();
    QSize s(1440, 900);
    if (cli.isSet(size)) {
      const QStringList p = cli.value(size).toLower().split('x');
      if (p.size() == 2) s = QSize(p[0].toInt(), p[1].toInt());
    }
    studio.resize(s);
    const QSignalBlocker block(w.action("advanced"));
    w.action("advanced")->setChecked(false);
    w.inspector().setAdvanced(false);
    studio.imageWorkspace().setAdvanced(cli.isSet(advanced));
  }
  studio.show();
  if (splash) splash->finish(&studio);

  const QStringList files = cli.positionalArguments();
  if (files.size() == 1 && files[0].endsWith(".3re", Qt::CaseInsensitive)) {
    studio.showPage("image");
    studio.setImageMode("ai");
    studio.imageWorkspace().openProject(files[0]);
  } else if (files.size() == 1 && files[0].endsWith(".p3d", Qt::CaseInsensitive)) {
    studio.showPage("image");
    studio.setImageMode("pixel");
    w.openProjectFile(files[0]);
  } else if (!files.isEmpty()) {
    studio.showPage("image");
    studio.setImageMode("pixel");
    // New projects are Low-Poly Studio; a scripted screenshot of a full Front + Side + Top set shows the visual hull.
    if (cli.isSet(screenshot) &&
        std::any_of(files.begin(), files.end(), [](const QString& f) { return role_from_filename(f) == pixel3d::ViewRole::Top; })) {
      pixel3d::ProjectSettings vh = default_project_settings();
      vh.lowpoly.enabled = false;
      w.controller().startProject(vh);
    }
    w.importFiles(files);
  }
  if (cli.isSet(image_to_3d) || cli.isSet(open_image)) {
    studio.showPage("image");
    studio.setImageMode("ai");
    if (cli.isSet(open_image)) studio.imageWorkspace().importImage(cli.value(open_image));
  }
  if (cli.isSet(open_result)) {
    studio.showPage("image");
    studio.setImageMode("ai");
    studio.imageWorkspace().openSaved(cli.value(open_result));
  }
  if (cli.isSet(page)) studio.showPage(cli.value(page).toLower());
  if (cli.isSet(agent_run)) {
    studio.showPage("agent");
    studio.agentPage().openRun(cli.value(agent_run));
    if (cli.isSet(agent_answer)) {
      auto& ap = studio.agentPage();
      const int qi = ap.model().openQuestion();
      QString error = qi < 0 ? QStringLiteral("no open question in this run") : QString();
      QString file = cli.value(agent_picture), label = QStringLiteral("back");
      if (const int eq = file.lastIndexOf('='); eq > 1) {
        label = file.mid(eq + 1);
        file = file.left(eq);
      }
      if (qi >= 0) ap.answer(qi, cli.value(agent_answer), file, file.isEmpty() ? QString() : label, &error);
      if (!error.isEmpty()) std::fprintf(stderr, "agent answer: %s\n", error.toUtf8().constData());
    }
  }
  if (cli.isSet(agent_conversation)) {
    studio.showPage("agent");
    if (!studio.agentPage().chat()->openConversation(cli.value(agent_conversation)))
      std::fprintf(stderr, "agent conversation: cannot open %s\n", cli.value(agent_conversation).toUtf8().constData());
  }
  if (cli.isSet(e2e_retopo)) {
    RetopoE2EOptions o;
    o.model = cli.value(e2e_retopo);
    o.out = cli.isSet(e2e_out) ? cli.value(e2e_out) : QDir::current().filePath("3re-retopo-e2e.json");
    o.shot = cli.value(e2e_shot);
    o.faces = cli.value(retopo_faces).toInt();
    studio.resize(1440, 900);
    start_retopo_e2e(studio, o);
    return app.exec();
  }
  if (cli.isSet(e2e)) {
    E2EOptions o;
    o.image = cli.value(e2e);
    o.out = cli.isSet(e2e_out) ? cli.value(e2e_out) : QDir::current().filePath("3re-e2e.json");
    o.shot = cli.value(e2e_shot);
    o.project_dir = cli.value(project_dir);
    o.views = cli.values(e2e_view);
    if (cli.isSet(e2e_seed)) {
      bool ok = false;
      const auto seed = cli.value(e2e_seed).toLongLong(&ok);
      if (!ok || seed < 0 || seed > 2147483647) {
        std::fprintf(stderr, "--e2e-seed must be 0..2147483647\n");
        return 2;
      }
      o.seed = int(seed);
    }
    // Prompts are given in picture pixels; the editor works at the bounded working size.
    const QSize original = QImageReader(o.image).size();
    const double s = original.width() > 0 ? double(p3dseg::working_size(original).width()) / original.width() : 1.0;
    if (cli.isSet(region_click)) {
      const QStringList xy = cli.value(region_click).split(',');
      if (xy.size() == 2) o.region_click = QPointF(xy[0].toDouble() * s, xy[1].toDouble() * s);
    }
    if (cli.isSet(region_box)) {
      const QStringList b = cli.value(region_box).split(',');
      if (b.size() == 4) o.region_box = QRectF(QPointF(b[0].toDouble() * s, b[1].toDouble() * s), QPointF(b[2].toDouble() * s, b[3].toDouble() * s));
    }
    auto* driver = new E2EDriver(studio, o);
    QTimer::singleShot(0, driver, &E2EDriver::start);
    return app.exec();
  }
  if (cli.isSet(image_mode)) studio.setImageMode(cli.value(image_mode).toLower());

  if (!cli.isSet(screenshot)) QTimer::singleShot(0, &studio, [&studio] { mention_crash_report(&studio); });
  // The last update's result, the daily update check (Settings > Updates) and the first-run model offer.
  if (!cli.isSet(screenshot)) QTimer::singleShot(500, &studio, [&studio] { start_update_tasks(&studio); });
  if (!cli.isSet(no_recovery) && !cli.isSet(screenshot) && !opt.autosave_dir.isEmpty()) {
    QTimer::singleShot(0, &studio, [&studio, &w, dir = opt.autosave_dir] {
      const auto found = pixel3d::find_recoverable(to_path(dir));
      bool discard_all = false;
      if (auto pick = ask_recovery(&studio, found, &discard_all)) {
        studio.showPage("image");
        studio.setImageMode("pixel");
        w.controller().recover(*pick);
      } else if (discard_all) {
        for (const auto& s : found) pixel3d::discard(to_path(dir), s);
      }
    });
  }

  if (cli.isSet(screenshot)) {
    // Scripted visual check: run everything, set up the requested state, save the window.
    const QString out = cli.value(screenshot), cam = cli.value(preset).toLower();
    if (!files.isEmpty() && !files[0].endsWith(".p3d", Qt::CaseInsensitive) && !files[0].endsWith(".3re", Qt::CaseInsensitive) &&
        !cli.isSet(no_run) && !w.controller().view().doc.settings.lowpoly.enabled)
      w.controller().runAll(pixel3d::ReconstructMode::TrueSolid);
    auto* poll = new QTimer(&studio);
    auto built = std::make_shared<bool>(false);
    QObject::connect(poll, &QTimer::timeout, &studio, [&, poll, out, cam, built] {
      if (w.controller().busy() || studio.imageWorkspace().property("image3dUiBusy").toBool()) return;
      // A Low-Poly Studio project shows its character: build it once (the project stores inputs only).
      if (!*built && w.controller().view().doc.settings.lowpoly.enabled && !w.controller().view().doc.masks.parts.empty() &&
          !pixel3d::current_lowpoly(w.controller().view().doc)) {
        *built = true;
        w.controller().buildLowpoly({});
        return;
      }
      poll->stop();
      if (w.controller().view().doc.settings.lowpoly.enabled && !files.isEmpty()) w.showWorkspace();
      if (cli.isSet(toggle) && cli.values(toggle).contains("lowpoly_pose")) w.setLowpolyPose(true);
      if (cli.isSet(workspace)) w.showWorkspace();
      const QString t = cli.value(tool).toLower();
      if (t == "depth") w.setTool(Tool::Depth);
      if (t == "parts") w.setTool(Tool::Parts);
      if (t == "pushpull") w.setTool(Tool::PushPull);
      if (t == "volume") w.setTool(Tool::Volume);
      if (auto r = pixel3d::view_role_from_string(cam.toStdString())) w.action(QStringLiteral("view_%1").arg(static_cast<int>(*r)))->trigger();
      for (const QString& k : cli.values(toggle))
        if (w.hasAction(k)) w.action(k)->toggle();
      if (cli.isSet(select_part)) w.selectPart(cli.value(select_part).toInt(), true);
      if (cli.isSet(open_issue)) {
        const int n = cli.value(open_issue).toInt();
        const auto& issues = w.inspector().issues();
        if (n >= 0 && n < static_cast<int>(issues.size())) w.openIssue(issues[static_cast<size_t>(n)].key());
      }
      if (cli.isSet(step)) {
        const QStringList steps{"import", "segment", "generate", "preview"};
        studio.imageWorkspace().showStep(std::max(0, int(steps.indexOf(cli.value(step).toLower()))));
      }
      if (cli.isSet(view_angle)) {
        const QStringList a = cli.value(view_angle).split(',');
        const QStringList f = cli.value(view_focus).split(',');
        if (a.size() == 2)
          studio.imageWorkspace().lookAt(a[0].toDouble(), a[1].toDouble(), f.size() == 4 ? f[0].toDouble() : 0, f.size() == 4 ? f[1].toDouble() : 0,
                                         f.size() == 4 ? f[2].toDouble() : 0, f.size() == 4 ? f[3].toDouble() : 0);
      }
      QWidget* shot = &studio;
      if (cli.isSet(agent_run)) {
        studio.agentPage().refresh();
        if (cli.isSet(agent_full)) {
          auto* t = studio.agentPage().timeline();
          t->resize(t->width(), t->sizeHint().height());
          shot = t;
        } else studio.agentPage().scrollTo(cli.isSet(agent_scroll) ? cli.value(agent_scroll).toLower() : QStringLiteral("top"));
      }
      if (cli.value(dialog).toLower() == "image3d") {
        studio.showPage("image");
        studio.setImageMode("ai");
      } else if (cli.value(dialog).toLower() == "export") {
        auto* dlg = w.newExportDialog().release();  // owned by the window
        dlg->show();
        shot = dlg;
      } else if (cli.value(dialog).toLower() == "settings") {
        auto* dlg = new NewProjectDialog(&w);  // owned by the window
        dlg->setWindowTitle(QObject::tr("Project settings"));
        dlg->setSettings(w.controller().view().doc.settings);
        dlg->show();
        shot = dlg;
      }
      QTimer::singleShot(900, &studio, [&w, out, shot] {
        shot->grab().save(out);
        w.controller().cleanShutdown();
        QApplication::exit(0);
      });
    });
    poll->start(200);
  }
  return app.exec();
}
