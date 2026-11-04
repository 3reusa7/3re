// UI/request and worker-containment tests: never launch Blender or AutoRemesher.
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QProcess>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <cstdio>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "main_window.hpp"
#include "smart_quad_dialog.hpp"

using namespace p3dgui;

namespace {
bool writeJson(const QString& path, const QJsonObject& object) {
  QFile file(path);
  const auto bytes = QJsonDocument(object).toJson();
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

bool touch(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write("test fixture") == 12;
}

// Reuse this test executable as a tiny worker so the production launch and Job
// Object path is exercised without a neural model, a remesher or Blender.
int fakeWorker(QCoreApplication& app, bool child) {
  QTimer::singleShot(30000, &app, &QCoreApplication::quit);
  if (child) return app.exec();
  const QStringList args = app.arguments();
  const int requestIndex = args.indexOf("--request");
  if (requestIndex < 0 || requestIndex + 1 >= args.size()) return 4;
  QFile requestFile(args[requestIndex + 1]);
  if (!requestFile.open(QIODevice::ReadOnly)) return 5;
  const auto request = QJsonDocument::fromJson(requestFile.readAll()).object();
  const QDir output(request.value("output_dir").toString());
  const QString mode = qEnvironmentVariable("PIXEL3D_SMARTQUAD_TEST_WORKER_MODE");
  QProcess descendant;
  if (mode == "cancel") {
    descendant.start(app.applicationFilePath(), {"--smartquad-test-child"});
    if (!descendant.waitForStarted(5000)) return 6;
    if (!writeJson(output.filePath("worker.json"),
                   {{"child_pid", static_cast<double>(descendant.processId())}})) return 7;
    return app.exec();
  }
  if (!touch(output.filePath("QuadMaster.blend")) || !touch(output.filePath("Game.glb"))) return 8;
  QImage preview(8, 8, QImage::Format_RGBA8888);
  preview.fill(Qt::green);
  if (!preview.save(output.filePath("BeforeAfter.png"))) return 9;
  const QString game = mode == "escaped-output" ? request.value("input").toString()
                                                 : output.filePath("Game.glb");
  const auto report = QJsonObject{{"status", "accepted"}, {"quality_pass", true},
      {"preview_sheet", output.filePath("BeforeAfter.png")},
      {"stats", QJsonObject{{"input_triangles", 1000}, {"output_triangles", 400},
                            {"quad_faces", 200}, {"budget_met", true}}},
      {"outputs", QJsonObject{{"quad_master", output.filePath("QuadMaster.blend")}, {"game", game}}}};
  return writeJson(output.filePath("report.json"), report) ? 0 : 10;
}

bool workerResourcesAvailable() {
#ifdef Q_OS_WIN
  MEMORYSTATUSEX state{};
  state.dwLength = sizeof(state);
  return GlobalMemoryStatusEx(&state) && state.ullAvailPhys >= 5ull * 1024 * 1024 * 1024 / 2;
#else
  return true;
#endif
}

bool prepareWorker(SmartQuadDialog& dialog, const QTemporaryDir& folder, const char* mode) {
  const QString input = folder.filePath("source.glb");
  if (!touch(input)) return false;
  qputenv("PIXEL3D_SMARTQUAD_TEST_WORKER_MODE", mode);
  dialog.findChild<QLineEdit*>("smartQuadInput")->setText(input);
  dialog.findChild<QLineEdit*>("smartQuadOutput")->setText(folder.filePath("results"));
  dialog.findChild<QLineEdit*>("smartQuadBlender")->setText(QCoreApplication::applicationFilePath());
  dialog.findChild<QLineEdit*>("smartQuadRemesher")->setText(QCoreApplication::applicationFilePath());
  return true;
}
}  // namespace

class SmartQuadTests : public QObject {
  Q_OBJECT
 private slots:
  void defaultsAreConservative() {
    SmartQuadDialog dialog;
    const QJsonObject request = dialog.request();
    QCOMPARE(request.value("triangle_budget").toInt(), 20000);
    QCOMPARE(dialog.findChild<QSpinBox*>("smartQuadTriangleBudget")->maximum(), 200000);
    QCOMPARE(request.value("texture_size").toInt(), 1024);
    QVERIFY(request.value("high_fidelity").toBool());
    QVERIFY(request.value("preserve_shape").toBool());
    QVERIFY(request.value("preserve_materials").toBool());
    QVERIFY(request.value("preserve_sharp_edges").toBool());
    QCOMPARE(request.value("lods").toInt(), 0);
    QVERIFY(!dialog.isRunning());
    QVERIFY(!dialog.findChild<QPushButton*>("smartQuadCancel")->isEnabled());
  }

  void controlsReachTheWorkerRequest() {
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    SmartQuadDialog dialog;
    const QString input = folder.filePath("source.glb");
    const QString output = folder.filePath("results");
    dialog.findChild<QLineEdit*>("smartQuadInput")->setText(input);
    dialog.findChild<QLineEdit*>("smartQuadOutput")->setText(output);
    dialog.findChild<QSpinBox*>("smartQuadTriangleBudget")->setValue(12000);
    dialog.findChild<QSpinBox*>("smartQuadLods")->setValue(3);
    dialog.findChild<QCheckBox*>("smartQuadHighFidelity")->setChecked(false);
    dialog.findChild<QCheckBox*>("smartQuadPreserveMaterials")->setChecked(false);
    const QJsonObject request = dialog.request();
    QCOMPARE(request.value("input").toString(), input);
    QCOMPARE(request.value("output_dir").toString(), output);
    QCOMPARE(request.value("triangle_budget").toInt(), 12000);
    QCOMPARE(request.value("lods").toInt(), 3);
    QVERIFY(!request.value("high_fidelity").toBool());
    QVERIFY(!request.value("preserve_materials").toBool());
    QVERIFY(request.value("preserve_shape").toBool());
  }

  void aMissingInputDoesNotLaunchOrCreateOutput() {
    QTemporaryDir folder;
    SmartQuadDialog dialog;
    const QString output = folder.filePath("never-created");
    dialog.findChild<QLineEdit*>("smartQuadInput")->setText(folder.filePath("missing.glb"));
    dialog.findChild<QLineEdit*>("smartQuadOutput")->setText(output);
    dialog.start();
    QVERIFY(!dialog.isRunning());
    QVERIFY(dialog.statusText().contains("existing GLB"));
    QVERIFY(!QDir(output).exists());
  }

  void acceptedRequiresTheQualityGate() {
    QVERIFY(!SmartQuadDialog::acceptedReport({}));
    QVERIFY(!SmartQuadDialog::acceptedReport({{"status", "accepted"}, {"quality_pass", false}}));
    QVERIFY(!SmartQuadDialog::acceptedReport({{"status", "rejected"}, {"quality_pass", true}}));
    QVERIFY(!SmartQuadDialog::acceptedReport({{"status", "accepted"}, {"quality_pass", "true"}}));
    QVERIFY(SmartQuadDialog::acceptedReport({{"status", "accepted"}, {"quality_pass", true}}));
  }

  void workerCompletesAndLoadsOnlyAcceptedOutputs() {
    if (!workerResourcesAvailable()) QSKIP("Preserving the production 2.5 GB RAM preflight.");
    QTemporaryDir folder;
    SmartQuadDialog dialog;
    QVERIFY(prepareWorker(dialog, folder, "accepted"));
    QVERIFY(!dialog.request().value("check_exe").toString().isEmpty());
    dialog.start();
    QVERIFY2(dialog.isRunning(), qPrintable(dialog.statusText()));
    QTRY_VERIFY_WITH_TIMEOUT(!dialog.isRunning(), 10000);
    QVERIFY2(dialog.statusText().contains("Quality checks passed"), qPrintable(dialog.statusText()));
    QVERIFY(dialog.findChild<QLabel*>("smartQuadStats")->text().contains("60.0%"));
    bool gameEnabled = false;
    for (auto* button : dialog.findChildren<QPushButton*>())
      if (button->text() == "Open Game.glb") gameEnabled = button->isEnabled();
    QVERIFY(gameEnabled);
  }

  void workerCannotOpenAnAcceptedPathOutsideItsOutputFolder() {
    if (!workerResourcesAvailable()) QSKIP("Preserving the production 2.5 GB RAM preflight.");
    QTemporaryDir folder;
    SmartQuadDialog dialog;
    QVERIFY(prepareWorker(dialog, folder, "escaped-output"));
    dialog.start();
    QVERIFY2(dialog.isRunning(), qPrintable(dialog.statusText()));
    QTRY_VERIFY_WITH_TIMEOUT(!dialog.isRunning(), 10000);
    QVERIFY2(dialog.statusText().contains("did not pass"), qPrintable(dialog.statusText()));
    for (auto* button : dialog.findChildren<QPushButton*>())
      if (button->text() == "Open Game.glb") QVERIFY(!button->isEnabled());
  }

  void cancellationTerminatesTheWorkerAndItsDescendant() {
#ifndef Q_OS_WIN
    QSKIP("Windows Job Object containment test.");
#else
    if (!workerResourcesAvailable()) QSKIP("Preserving the production 2.5 GB RAM preflight.");
    QTemporaryDir folder;
    SmartQuadDialog dialog;
    QVERIFY(prepareWorker(dialog, folder, "cancel"));
    dialog.start();
    QVERIFY2(dialog.isRunning(), qPrintable(dialog.statusText()));
    const QString ready = folder.filePath("results/worker.json");
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(ready), 10000);
    QFile childInfo(ready);
    QVERIFY(childInfo.open(QIODevice::ReadOnly));
    const auto info = QJsonDocument::fromJson(childInfo.readAll()).object();
    const DWORD pid = static_cast<DWORD>(info.value("child_pid").toDouble());
    HANDLE child = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    QVERIFY(child);
    dialog.cancel();
    QTRY_VERIFY_WITH_TIMEOUT(!dialog.isRunning(), 5000);
    const DWORD state = WaitForSingleObject(child, 5000);
    CloseHandle(child);
    QCOMPARE(state, static_cast<DWORD>(WAIT_OBJECT_0));
    QVERIFY(dialog.statusText().contains("Cancelled"));
    QVERIFY(QFile::exists(folder.filePath("source.glb")));
#endif
  }

  void actionWorksWithoutDrawingsAndReusesItsWindow() {
    WindowOptions options;
    options.software_view = true;
    MainWindow window(options);
    QVERIFY(window.controller().view().doc.views.empty());
    QVERIFY(window.hasAction("smart_quad"));
    QVERIFY(window.action("smart_quad")->isEnabled());
    window.action("smart_quad")->trigger();
    auto* first = window.findChild<SmartQuadDialog*>();
    QVERIFY(first);
    QVERIFY(!first->isModal());
    window.action("smart_quad")->trigger();
    QCOMPARE(window.findChildren<SmartQuadDialog*>().size(), 1);
    QCOMPARE(window.findChild<SmartQuadDialog*>(), first);
    QVERIFY(window.controller().view().doc.views.empty());
  }
};

int main(int argc, char** argv) {
  bool worker = false;
  bool child = false;
  for (int i = 1; i < argc; ++i) {
    if (QByteArray(argv[i]) == "--background") worker = true;
    if (QByteArray(argv[i]) == "--smartquad-test-child") child = true;
  }
  if (worker || child) {
    QCoreApplication app(argc, argv);
    return fakeWorker(app, child);
  }
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  QTemporaryDir settings;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
  QCoreApplication::setOrganizationName("Pixel3DTests");
  QCoreApplication::setApplicationName("SmartQuadTests");
  SmartQuadTests tests;
  return QTest::qExec(&tests, argc, argv);
}

#include "test_smart_quad.moc"
