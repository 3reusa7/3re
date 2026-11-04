#include "smart_quad_dialog.hpp"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

#include "icons.hpp"
#include "widgets.hpp"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace p3dgui {
namespace {
constexpr quint64 kGiB = 1024ull * 1024 * 1024;
constexpr qint64 kTimeLimitMs = 1200 * 1000;
constexpr int kMaximumLine = 64 * 1024;

QString existing(const QStringList& paths) {
  for (const QString& path : paths)
    if (!path.isEmpty() && QFileInfo(path).isFile()) return QFileInfo(path).absoluteFilePath();
  return {};
}

QString findBlender() {
  QStringList paths{qEnvironmentVariable("PIXEL3D_BLENDER"), QStandardPaths::findExecutable("blender")};
#ifdef Q_OS_WIN
  const QString base = qEnvironmentVariable("ProgramFiles", "C:/Program Files") + "/Blender Foundation";
  paths << base + "/Blender 4.2/blender.exe";
  const QDir dir(base);
  for (const QString& name : dir.entryList({"Blender*"}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed))
    paths << dir.filePath(name + "/blender.exe");
#endif
  return existing(paths);
}

QString findRemesher() {
  const QString app = QCoreApplication::applicationDirPath();
  return existing({qEnvironmentVariable("PIXEL3D_AUTOREMESHER"), app + "/smartquad/AutoRemesher.exe",
                   app + "/smartquad/autoremesher.exe", app + "/tools/smartquad/AutoRemesher.exe",
                   app + "/smartquad/AutoRemesher/AutoRemesher.exe",
                   app + "/../../.toolchain/autoremesher/1.2.0/AutoRemesher.exe",
                   app + "/../../../.toolchain/autoremesher/1.2.0/AutoRemesher.exe",
                   app + "/../../../.toolchain/autoremesher/1.2.0/autoremesher.exe"});
}

quint64 availableMemory() {
#ifdef Q_OS_WIN
  MEMORYSTATUSEX state{};
  state.dwLength = sizeof(state);
  if (GlobalMemoryStatusEx(&state)) return state.ullAvailPhys;
  return 0;  // Fail closed if the resource check is unavailable.
#else
  return 16 * kGiB;
#endif
}

QWidget* pathRow(QLineEdit* edit, QPushButton* button) {
  auto* row = new QWidget;
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(edit, 1);
  layout->addWidget(button);
  return row;
}
}  // namespace

SmartQuadDialog::SmartQuadDialog(QWidget* parent) : QDialog(parent) {
  setObjectName("SmartQuadDialog");
  setWindowTitle(tr("Smart Quad · Optimize Mesh"));
  setModal(false);
  resize(1060, 760);
  setMinimumSize(820, 620);
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(20, 20, 20, 16);
  root->addWidget(make_label(tr("Smart Quad"), "title"));
  auto* intro = make_label(tr("Create a quad master and a lighter game mesh from a GLB. Your source stays intact."), "muted");
  intro->setWordWrap(true);
  root->addWidget(intro);
  auto* body = new QHBoxLayout;
  root->addLayout(body, 1);
  auto* scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setMinimumWidth(330);
  scroll->setMaximumWidth(410);
  controls_ = new QWidget;
  auto* controls = new QVBoxLayout(controls_);
  controls->setContentsMargins(0, 0, 10, 0);
  scroll->setWidget(controls_);
  body->addWidget(scroll);
  auto* asset = new Section(tr("Asset"));
  controls->addWidget(asset);
  input_ = new QLineEdit;
  input_->setObjectName("smartQuadInput");
  input_->setPlaceholderText(tr("Choose a GLB model"));
  auto* browseInput = make_button(tr("Browse…"), "ghost");
  asset->body()->addWidget(make_label(tr("Input GLB"), "small"));
  asset->body()->addWidget(pathRow(input_, browseInput));
  output_ = new QLineEdit;
  output_->setObjectName("smartQuadOutput");
  output_->setPlaceholderText(tr("New results folder"));
  auto* browseOutput = make_button(tr("Browse…"), "ghost");
  asset->body()->addWidget(make_label(tr("New output folder"), "small"));
  asset->body()->addWidget(pathRow(output_, browseOutput));
  connect(browseInput, &QPushButton::clicked, this, [this] {
    const QString file = QFileDialog::getOpenFileName(this, tr("Choose input model"), input_->text(), tr("GLB model (*.glb)"));
    if (file.isEmpty()) return;
    input_->setText(file);
    output_->setText(newOutputPath(QFileInfo(file).absolutePath()));
  });
  connect(browseOutput, &QPushButton::clicked, this, [this] {
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Choose a location for the new results folder"),
                                                           QFileInfo(output_->text()).absolutePath());
    if (!folder.isEmpty()) output_->setText(newOutputPath(folder));
  });

  auto* optimize = new Section(tr("Optimization"));
  controls->addWidget(optimize);
  fidelity_ = new QCheckBox(tr("High Fidelity"));
  fidelity_->setObjectName("smartQuadHighFidelity");
  fidelity_->setChecked(true);
  fidelity_->setToolTip(tr("Favor detail and conservative surface preservation."));
  optimize->body()->addWidget(fidelity_);
  auto* form = new QFormLayout;
  budget_ = new QSpinBox;
  budget_->setObjectName("smartQuadTriangleBudget");
  budget_->setRange(100, 200000);
  budget_->setSingleStep(1000);
  budget_->setValue(20000);
  budget_->setGroupSeparatorShown(true);
  form->addRow(tr("Triangle budget"), budget_);
  lods_ = new QSpinBox;
  lods_->setObjectName("smartQuadLods");
  lods_->setRange(0, 3);
  lods_->setSpecialValueText(tr("Off"));
  form->addRow(tr("Additional LODs"), lods_);
  optimize->body()->addLayout(form);
  shape_ = new QCheckBox(tr("Preserve Shape"));
  materials_ = new QCheckBox(tr("Preserve Materials"));
  sharp_ = new QCheckBox(tr("Preserve Sharp Edges"));
  shape_->setObjectName("smartQuadPreserveShape");
  materials_->setObjectName("smartQuadPreserveMaterials");
  sharp_->setObjectName("smartQuadPreserveSharpEdges");
  for (QCheckBox* box : {shape_, materials_, sharp_}) {
    box->setChecked(true);
    optimize->body()->addWidget(box);
  }
  auto* note = make_label(tr("Results include QuadMaster.blend, Game.glb and a Before / After comparison. Texture size: 1024."), "muted");
  note->setWordWrap(true);
  optimize->body()->addWidget(note);

  auto* advanced = new Section(tr("Advanced"), nullptr, false);
  controls->addWidget(advanced);
  const QSettings settings;
  blender_ = new QLineEdit(settings.value("smartquad/blender", findBlender()).toString());
  remesher_ = new QLineEdit(settings.value("smartquad/remesher", findRemesher()).toString());
  blender_->setObjectName("smartQuadBlender");
  remesher_->setObjectName("smartQuadRemesher");
  for (const auto& pair : {std::pair{blender_, tr("Blender executable")}, std::pair{remesher_, tr("AutoRemesher executable")}}) {
    auto* browse = make_button(tr("Browse…"), "ghost");
    advanced->body()->addWidget(make_label(pair.second, "small"));
    advanced->body()->addWidget(pathRow(pair.first, browse));
    connect(browse, &QPushButton::clicked, this, [this, edit = pair.first] {
      const QString file = QFileDialog::getOpenFileName(this, tr("Choose executable"), edit->text(), tr("Executable (*.exe);;All files (*)"));
      if (!file.isEmpty()) edit->setText(file);
    });
  }
  auto* resourceNote = make_label(tr("CPU only · 2 worker threads · 20 minute limit. Requires 2.5 GB free memory and 4 GB free disk space."), "muted");
  resourceNote->setWordWrap(true);
  advanced->body()->addWidget(resourceNote);
  controls->addStretch();

  auto* result = new QVBoxLayout;
  body->addLayout(result, 1);
  result->addWidget(make_label(tr("Before / After"), "section"));
  preview_ = new QLabel(tr("The rendered comparison will appear here after optimization."));
  preview_->setObjectName("smartQuadPreview");
  preview_->setAlignment(Qt::AlignCenter);
  preview_->setWordWrap(true);
  preview_->setMinimumSize(350, 220);
  preview_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
  preview_->setStyleSheet("QLabel { background: #181b21; border: 1px solid #343943; border-radius: 8px; padding: 8px; }");
  result->addWidget(preview_, 1);
  stats_ = make_label(tr("No result yet."), "muted");
  stats_->setObjectName("smartQuadStats");
  stats_->setWordWrap(true);
  result->addWidget(stats_);
  auto* details = new Section(tr("Run details"), nullptr, false);
  log_ = new QPlainTextEdit;
  log_->setReadOnly(true);
  log_->setMaximumBlockCount(300);
  log_->setMaximumHeight(140);
  details->body()->addWidget(log_);
  result->addWidget(details);
  status_ = make_label(tr("Choose a GLB to begin."));
  status_->setObjectName("smartQuadStatus");
  status_->setWordWrap(true);
  root->addWidget(status_);
  progress_ = new QProgressBar;
  progress_->setRange(0, 100);
  root->addWidget(progress_);
  auto* buttons = new QHBoxLayout;
  root->addLayout(buttons);
  folder_ = make_button(tr("Open results"), "ghost", icon(Icon::Open));
  game_ = make_button(tr("Open Game.glb"), "ghost");
  folder_->setEnabled(false);
  game_->setEnabled(false);
  buttons->addWidget(folder_);
  buttons->addWidget(game_);
  buttons->addStretch();
  cancel_ = make_button(tr("Cancel run"), "ghost");
  cancel_->setObjectName("smartQuadCancel");
  cancel_->setEnabled(false);
  run_ = make_button(tr("Optimize & export"), "primary");
  run_->setObjectName("smartQuadRun");
  buttons->addWidget(cancel_);
  buttons->addWidget(run_);
  connect(run_, &QPushButton::clicked, this, &SmartQuadDialog::start);
  connect(cancel_, &QPushButton::clicked, this, &SmartQuadDialog::cancel);
  connect(folder_, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(run_output_)); });
  connect(game_, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(game_path_)); });
  process_.setProcessChannelMode(QProcess::MergedChannels);
  connect(&process_, &QProcess::readyReadStandardOutput, this, &SmartQuadDialog::readOutput);
  connect(&process_, &QProcess::started, this, [this] {
    if (!containProcess()) {
      stopProcess(tr("Could not apply the worker's process and memory limits."));
      return;
    }
    elapsed_.start();
    watchdog_->start();
    setStatus(tr("Starting Smart Quad…"));
  });
  connect(&process_, &QProcess::finished, this, &SmartQuadDialog::processFinished);
  connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error != QProcess::FailedToStart) return;
    releaseJob();
    setRunning(false);
    setStatus(tr("Could not start Blender: %1").arg(process_.errorString()), true);
  });
  watchdog_ = new QTimer(this);
  watchdog_->setInterval(1000);
  connect(watchdog_, &QTimer::timeout, this, &SmartQuadDialog::checkResources);
#ifdef Q_OS_WIN
  process_.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
    // Attach the suspended process to the Job Object before it can create children.
    args->flags |= CREATE_SUSPENDED | CREATE_NO_WINDOW;
    args->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
    args->startupInfo->wShowWindow = SW_HIDE;
  });
#endif
}

SmartQuadDialog::~SmartQuadDialog() {
  watchdog_->stop();
  releaseJob();
  if (isRunning()) {
    process_.kill();
    process_.waitForFinished(3000);
  }
}

QString SmartQuadDialog::newOutputPath(const QString& parent) const {
  QString name = QFileInfo(input_->text()).completeBaseName();
  if (name.isEmpty()) name = "model";
  return QDir(parent).filePath(name + "_SmartQuad_" + QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss_zzz"));
}

QString SmartQuadDialog::pipelinePath() const {
  const QString app = QCoreApplication::applicationDirPath();
  return existing({app + "/smartquad/pipeline.py", app + "/tools/smartquad/pipeline.py"});
}

QString SmartQuadDialog::auditPath() const {
  return existing({QCoreApplication::applicationDirPath() + "/pixel3d-smartquad-audit.exe",
                   QCoreApplication::applicationDirPath() + "/pixel3d-smartquad-audit"});
}

QString SmartQuadDialog::checkPath() const {
  return existing({QCoreApplication::applicationDirPath() + "/pixel3d-smartquad-check.exe",
                   QCoreApplication::applicationDirPath() + "/pixel3d-smartquad-check"});
}

QJsonObject SmartQuadDialog::request() const {
  return {{"input", QFileInfo(input_->text().trimmed()).absoluteFilePath()},
          {"output_dir", QFileInfo(output_->text().trimmed()).absoluteFilePath()},
          {"triangle_budget", budget_->value()}, {"high_fidelity", fidelity_->isChecked()},
          {"preserve_shape", shape_->isChecked()}, {"preserve_materials", materials_->isChecked()},
          {"preserve_sharp_edges", sharp_->isChecked()}, {"lods", lods_->value()},
          {"remesher_exe", remesher_->text().trimmed()}, {"check_exe", checkPath()}, {"texture_size", 1024}};
}

QString SmartQuadDialog::statusText() const { return status_->text(); }

bool SmartQuadDialog::acceptedReport(const QJsonObject& report) {
  return report.value("quality_pass").toBool(false) && report.value("status").toString() == "accepted";
}

void SmartQuadDialog::setStatus(const QString& text, bool error) {
  status_->setText(text);
  status_->setStyleSheet(error ? "color: #ef8b83;" : QString());
}

void SmartQuadDialog::setRunning(bool running) {
  controls_->setEnabled(!running);
  run_->setEnabled(!running);
  cancel_->setEnabled(running);
  if (running) {
    folder_->setEnabled(false);
    game_->setEnabled(false);
  }
}

void SmartQuadDialog::start() {
  if (isRunning()) return;
  const QFileInfo source(input_->text().trimmed());
  const QString destination = output_->text().trimmed();
  if (!source.isFile() || source.suffix().compare("glb", Qt::CaseInsensitive) != 0) {
    setStatus(tr("Choose an existing GLB input file."), true);
    return;
  }
  if (destination.isEmpty() || QFileInfo::exists(destination)) {
    setStatus(tr("Choose a new output folder that does not already exist."), true);
    return;
  }
  if (!QFileInfo(blender_->text().trimmed()).isFile() || !QFileInfo(remesher_->text().trimmed()).isFile()) {
    setStatus(tr("Set the Blender and AutoRemesher executable paths under Advanced."), true);
    return;
  }
  if (pipelinePath().isEmpty() || auditPath().isEmpty() || checkPath().isEmpty()) {
    setStatus(tr("Smart Quad components are missing beside Pixel3D. Install the complete application bundle."), true);
    return;
  }
  const QString parent = QFileInfo(destination).absolutePath();
  if (!QDir(parent).exists()) {
    setStatus(tr("Choose an existing parent folder for the results."), true);
    return;
  }
  const QStorageInfo storage(parent);
  if (!storage.isValid() || storage.bytesAvailable() < static_cast<qint64>(4 * kGiB)) {
    setStatus(tr("At least 4 GB of free disk space is required at the output location."), true);
    return;
  }
  if (availableMemory() < 5 * kGiB / 2) {
    setStatus(tr("At least 2.5 GB of free memory is required. Close other heavy applications and try again."), true);
    return;
  }
  if (!QDir().mkdir(destination)) {
    setStatus(tr("Could not create the output folder."), true);
    return;
  }
  run_output_ = QFileInfo(destination).absoluteFilePath();
  const QString requestPath = QDir(run_output_).filePath("request.json");
  QSaveFile file(requestPath);
  const QByteArray json = QJsonDocument(request()).toJson();
  if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size() || !file.commit()) {
    setStatus(tr("Could not write the optimization request."), true);
    return;
  }
#ifdef Q_OS_WIN
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_JOB_MEMORY;
  limits.JobMemoryLimit = static_cast<SIZE_T>(6 * kGiB);
  if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
    if (job) CloseHandle(job);
    setStatus(tr("Could not establish safe worker memory limits."), true);
    return;
  }
  job_ = job;
#endif
  QSettings settings;
  settings.setValue("smartquad/blender", blender_->text().trimmed());
  settings.setValue("smartquad/remesher", remesher_->text().trimmed());
  stop_reason_.clear();
  buffered_.clear();
  log_->clear();
  progress_->setValue(0);
  stats_->setText(tr("Working on %1…").arg(source.fileName()));
  preview_image_ = QPixmap();
  preview_->clear();
  preview_->setText(tr("Preparing the quad master and game mesh…"));
  setRunning(true);
  setStatus(tr("Starting the optimization worker…"));
  auto environment = QProcessEnvironment::systemEnvironment();
  for (const QString& variable : {QStringLiteral("OMP_NUM_THREADS"), QStringLiteral("OPENBLAS_NUM_THREADS"),
                                  QStringLiteral("MKL_NUM_THREADS")}) environment.insert(variable, "2");
  environment.insert("CUDA_VISIBLE_DEVICES", "-1");
  environment.insert("HIP_VISIBLE_DEVICES", "-1");
  environment.insert("PYTHONUNBUFFERED", "1");
  process_.setProcessEnvironment(environment);
  process_.setWorkingDirectory(run_output_);
  process_.start(blender_->text().trimmed(), {"--background", "--factory-startup", "--threads", "2", "--python-exit-code", "1",
                                           "--python", pipelinePath(), "--", "--request", requestPath,
                                           "--audit-exe", auditPath()});
}

bool SmartQuadDialog::containProcess() {
#ifdef Q_OS_WIN
  if (!job_) return false;
  const DWORD pid = static_cast<DWORD>(process_.processId());
  HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!child) return false;
  const bool assigned = AssignProcessToJobObject(static_cast<HANDLE>(job_), child) != FALSE;
  CloseHandle(child);
  if (!assigned) return false;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return false;
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  bool resumed = false;
  if (Thread32First(snapshot, &entry)) {
    do {
      if (entry.th32OwnerProcessID != pid) continue;
      HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
      if (thread) {
        resumed = ResumeThread(thread) != static_cast<DWORD>(-1);
        CloseHandle(thread);
      }
      break;  // A CREATE_SUSPENDED process has exactly its initial thread.
    } while (Thread32Next(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return resumed;
#else
  return true;
#endif
}

void SmartQuadDialog::releaseJob() {
#ifdef Q_OS_WIN
  if (job_) CloseHandle(static_cast<HANDLE>(job_));
#endif
  job_ = nullptr;
}

void SmartQuadDialog::readOutput() {
  buffered_ += process_.readAllStandardOutput();
  while (true) {
    const qsizetype newline = buffered_.indexOf('\n');
    if (newline < 0) break;
    const QByteArray line = buffered_.left(newline);
    buffered_.remove(0, newline + 1);
    consumeLine(line.left(kMaximumLine));
  }
  if (buffered_.size() > kMaximumLine) {
    consumeLine(buffered_.left(kMaximumLine));
    buffered_.clear();
  }
}

void SmartQuadDialog::consumeLine(const QByteArray& line) {
  const QString text = QString::fromUtf8(line).trimmed();
  if (text.isEmpty()) return;
  log_->appendPlainText(text);
  const QJsonObject message = QJsonDocument::fromJson(line).object();
  if (message.value("event").toString() != "progress") return;
  progress_->setValue(std::clamp(message.value("progress").toInt(), 0, 99));
  const QString detail = message.value("message").toString();
  setStatus(detail.isEmpty() ? message.value("stage").toString() : detail);
}

void SmartQuadDialog::checkResources() {
  if (elapsed_.elapsed() >= kTimeLimitMs) stopProcess(tr("Stopped after the 20 minute time limit."));
  else if (availableMemory() < kGiB) stopProcess(tr("Stopped because free system memory fell below 1 GB."));
}

void SmartQuadDialog::stopProcess(const QString& reason) {
  if (!isRunning()) return;
  stop_reason_ = reason;
  watchdog_->stop();
  releaseJob();
  process_.kill();
  setStatus(reason, true);
  cancel_->setEnabled(false);
}

void SmartQuadDialog::cancel() { stopProcess(tr("Cancelled. The source model is unchanged.")); }
void SmartQuadDialog::reject() { cancel(); QDialog::reject(); }

void SmartQuadDialog::processFinished(int code, QProcess::ExitStatus exitStatus) {
  watchdog_->stop();
  readOutput();
  if (!buffered_.isEmpty()) consumeLine(buffered_);
  buffered_.clear();
  releaseJob();
  setRunning(false);
  folder_->setEnabled(QDir(run_output_).exists());
  if (!stop_reason_.isEmpty()) {
    setStatus(stop_reason_, true);
    stats_->setText(tr("Run stopped. Any partial results remain in the output folder."));
    return;
  }
  loadReport(exitStatus == QProcess::NormalExit && code == 0);
}

void SmartQuadDialog::loadReport(bool processOk) {
  QFile file(QDir(run_output_).filePath("report.json"));
  if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) {
    setStatus(tr("Optimization did not produce a readable quality report. See Run details."), true);
    return;
  }
  const QJsonObject report = QJsonDocument::fromJson(file.readAll()).object();
  const QJsonObject stats = report.value("stats").toObject();
  const qint64 before = static_cast<qint64>(stats.value("input_triangles").toDouble());
  const qint64 after = static_cast<qint64>(stats.value("output_triangles").toDouble());
  const double reduction = before > 0 ? (1.0 - static_cast<double>(after) / before) * 100.0 : 0;
  stats_->setText(tr("Before: %1 triangles · After: %2 triangles · %3% reduction\nQuad master: %4 faces · Budget: %5")
                      .arg(QLocale().toString(before), QLocale().toString(after), QString::number(reduction, 'f', 1),
                           QLocale().toString(static_cast<qint64>(stats.value("quad_faces").toDouble())),
                           stats.value("budget_met").toBool() ? tr("met") : tr("not met")));
  // Only files inside this newly created output directory may be opened from the report.
  const auto resultPath = [this](const QString& value) {
    const QString resolved = QFileInfo(QDir(run_output_).absoluteFilePath(value)).canonicalFilePath();
    const QString prefix = QFileInfo(run_output_).canonicalFilePath() + '/';
    return !resolved.isEmpty() && resolved.startsWith(prefix, Qt::CaseInsensitive) ? resolved : QString();
  };
  const QString sheet = resultPath(report.value("preview_sheet").toString());
  if (!sheet.isEmpty()) {
    preview_image_.load(sheet);
    refreshPreview();
  }
  const QString master = resultPath(report.value("outputs").toObject().value("quad_master").toString());
  game_path_ = resultPath(report.value("outputs").toObject().value("game").toString());
  const bool accepted = processOk && acceptedReport(report) && !master.isEmpty() && !game_path_.isEmpty()
                        && !preview_image_.isNull();
  game_->setEnabled(accepted && !game_path_.isEmpty());
  if (accepted) {
    progress_->setValue(100);
    setStatus(tr("Optimization complete. Quality checks passed; the quad master and game mesh are ready."));
  } else {
    const QString reason = report.value("message").toString();
    setStatus(tr("Quality checks did not pass. %1").arg(reason.isEmpty() ? tr("Inspect the report and comparison in the results folder.") : reason), true);
  }
}

void SmartQuadDialog::refreshPreview() {
  if (preview_image_.isNull()) return;
  preview_->setPixmap(preview_image_.scaled(preview_->size() - QSize(20, 20), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void SmartQuadDialog::resizeEvent(QResizeEvent* event) {
  QDialog::resizeEvent(event);
  refreshPreview();
}
}  // namespace p3dgui
