#include "backends/retexture_batch.hpp"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include "backends/image_input.hpp"
#include "backends/settings_env.hpp"
#include "backends/trellis_backend.hpp"

namespace p3dasset {
namespace {
QJsonObject read_json(const QString& file) {
  QFile f(file);
  if (!f.open(QIODevice::ReadOnly) || f.size() > 64 * 1024 * 1024) return {};
  return QJsonDocument::fromJson(f.readAll()).object();
}
bool write_json(const QString& file, const QJsonObject& o) {
  QSaveFile f(file);
  return f.open(QIODevice::WriteOnly) && f.write(QJsonDocument(o).toJson()) >= 0 && f.commit();
}
// First free name: textured-v2.glb, then "textured-v2 (2).glb", ... (also free of its report).
QString free_output(const QString& dir) {
  for (int n = 1; n < 100; ++n) {
    const QString base = n == 1 ? QStringLiteral("textured-v2") : QStringLiteral("textured-v2 (%1)").arg(n);
    const QString glb = QDir(dir).filePath(base + ".glb");
    if (!QFileInfo::exists(glb) && !QFileInfo::exists(glb + ".texture.json") && !QFileInfo::exists(glb + ".partial")) return glb;
  }
  return {};
}
}  // namespace

RetextureBatch::RetextureBatch(QObject* parent) : QObject(parent) {
  connect(&process_, &QProcess::finished, this, &RetextureBatch::jobDone);
  process_.setProcessChannelMode(QProcess::MergedChannels);
}
RetextureBatch::~RetextureBatch() {
  if (process_.state() != QProcess::NotRunning) {
    process_.kill();
    process_.waitForFinished(5000);
  }
}

QStringList RetextureBatch::defaultRoots() {
  const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  QStringList roots{docs + "/3re/Projects", docs + "/Pixel3D/Generated", docs + "/3re/Generated"};
  const QString portable = setting_env("PORTABLE_USERDATA");
  if (!portable.isEmpty()) roots << QDir(portable).absoluteFilePath("Projects") << QDir(portable).absoluteFilePath("Generated");
  return roots;
}

QVector<RetextureBatch::Job> RetextureBatch::scan(const QStringList& roots) {
  QVector<Job> jobs;
  QStringList seen;
  for (const QString& root : roots) {
    if (!QFileInfo(root).isDir()) continue;
    QDirIterator it(root, {"native.post"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
      const QString post = it.next();
      const QString dir = QFileInfo(post).absolutePath();
      if (QDir(root).relativeFilePath(dir).count('/') > 4 || seen.contains(dir)) continue;
      seen << dir;
      Job j;
      j.run_dir = dir;
      j.native_post = post;
      j.geometry = QDir(dir).filePath("geometry.glb");
      j.picture = QFileInfo(QDir(dir).filePath("texture-source.png")).isFile() ? QDir(dir).filePath("texture-source.png") : QDir(dir).filePath("input.png");
      if (!QFileInfo(j.geometry).isFile() || !QFileInfo(j.picture).isFile()) continue;
      const auto report = read_json(QDir(dir).filePath("report.json"));
      j.crisp = report.value("preprocessing").toObject().value("crisp").toBool() || report.value("crisp").toBool();
      // A project's run (<project>/runs/<run>): its other angles go along.
      const QDir projectDir(QFileInfo(dir).absolutePath() + "/..");
      const QString pf = QDir::cleanPath(projectDir.absoluteFilePath("project.3re"));
      if (QFileInfo(QFileInfo(dir).absolutePath()).fileName() == "runs" && QFileInfo(pf).isFile()) {
        j.project_file = pf;
        const auto project = read_json(pf);
        for (const auto& v : project.value("views").toArray()) {
          const auto o = v.toObject();
          const QString file = QDir(QFileInfo(pf).absolutePath()).absoluteFilePath(o.value("file").toString());
          if (!QFileInfo(file).isFile() || o.value("file").toString().contains("..")) continue;
          QString label = o.value("label").toString();
          if (o.contains("yaw") && !o.value("yaw").isNull()) label += ":" + QString::number(o.value("yaw").toDouble());
          j.view_args << file << label;
        }
      }
      jobs << j;
    }
  }
  return jobs;
}

bool RetextureBatch::start(const QVector<Job>& jobs, const QString& worker) {
  if (running_) return false;
  if (!QFileInfo(worker).isFile()) {
    summary_ = {{"status", "ERROR"}, {"message", "The texture worker is missing: " + QDir::toNativeSeparators(worker)}};
    emit finished(false, summary_);
    return false;
  }
  jobs_ = jobs;
  worker_ = worker;
  index_ = -1;
  cancelled_ = false;
  running_ = true;
  results_ = {};
  timer_.start();
  next();
  return true;
}

void RetextureBatch::cancel() {
  if (!running_) return;
  cancelled_ = true;
  if (process_.state() != QProcess::NotRunning) process_.kill();
  else next();
}

void RetextureBatch::next() {
  ++index_;
  if (cancelled_ || index_ >= jobs_.size()) {
    running_ = false;
    summary_ = {{"status", cancelled_ ? "CANCELLED" : "OK"}, {"finished", QDateTime::currentDateTime().toString(Qt::ISODate)},
                {"seconds", timer_.elapsed() / 1000.0}, {"runs", results_}};
    emit progress(int(results_.size()), int(jobs_.size()), cancelled_ ? tr("Cancelled") : tr("Done"));
    emit finished(cancelled_, summary_);
    return;
  }
  auto& j = jobs_[index_];
  const QString name = QFileInfo(j.run_dir).fileName();
  j.output = free_output(j.run_dir);
  // The app's 1 GiB reserve plus a 2048 bake (~0.4 GiB); the worker itself picks 4096 only with 2 GiB free.
  if (j.output.isEmpty() || available_ram() < 1434ull * 1024 * 1024) {
    results_.append(QJsonObject{{"run", j.run_dir}, {"status", "SKIPPED"},
                                {"reason", j.output.isEmpty() ? "no free output name" : "less than 1.4 GiB of free memory"}});
    emit progress(index_ + 1, int(jobs_.size()), tr("Skipped %1").arg(name));
    QMetaObject::invokeMethod(this, &RetextureBatch::next, Qt::QueuedConnection);
    return;
  }
  QStringList args{j.geometry, j.picture, j.output, "--max-size", "4096", "--seconds", "600", "--native", j.native_post,
                   "--views", QFileInfo(j.output).completeBaseName() == "textured-v2" ? QDir(j.run_dir).filePath("textured-v2-views")
                                                                                      : j.output + "-views"};
  if (j.crisp) args << "--crisp";
  // Other angles: cutouts like the front picture's, saved into the run next to the result.
  for (int k = 0; k + 1 < j.view_args.size(); k += 2) {
    const auto input = prepare_image(j.view_args[k], true);
    if (input.cutout.isNull()) continue;
    const QString f = QDir(j.run_dir).filePath(QString("retexture-view-%1.png").arg(k / 2 + 1));
    if (!QFileInfo::exists(f) && !input.cutout.save(f, "PNG")) continue;
    args << "--view" << f << j.view_args[k + 1];
  }
  emit progress(index_, int(jobs_.size()), tr("Re-texturing %1 (%2 of %3)").arg(name).arg(index_ + 1).arg(jobs_.size()));
  process_.setWorkingDirectory(j.run_dir);
  process_.start(worker_, args);
}

void RetextureBatch::jobDone(int code, QProcess::ExitStatus status) {
  const auto& j = jobs_[index_];
  const auto report = read_json(j.output + ".texture.json");
  QJsonObject r{{"run", j.run_dir}, {"output", j.output}, {"exit_code", code}, {"crashed", status != QProcess::NormalExit},
                {"verdict", report.value("verdict")}, {"selected", report.value("selected")}, {"seconds", report.value("seconds")},
                {"peak_working_set_mib", report.value("peak_working_set_mib")}, {"report", j.output + ".texture.json"}};
  QJsonArray views;
  for (const auto& v : report.value("views").toArray())
    views.append(QJsonObject{{"name", v.toObject().value("name")}, {"status", v.toObject().value("status")}, {"reason", v.toObject().value("reason")}});
  if (!views.isEmpty()) r["views"] = views;
  const bool ok = !cancelled_ && status == QProcess::NormalExit && code == 0 && QFileInfo(j.output).isFile();
  r["status"] = cancelled_ ? "CANCELLED" : ok ? "OK" : code == 3 ? "KEPT_EXISTING" : "ERROR";
  if (!ok && !cancelled_) r["error"] = report.value("error");
  // The project opens with the new texture; the previous project file is kept.
  if (ok && !j.project_file.isEmpty()) {
    auto project = read_json(j.project_file);
    const QString backup = j.project_file + ".before-v2";
    if (!project.isEmpty() && (QFileInfo::exists(backup) || QFile::copy(j.project_file, backup))) {
      project["result"] = QDir(QFileInfo(j.project_file).absolutePath()).relativeFilePath(j.output);
      project["modified"] = QDateTime::currentDateTime().toString(Qt::ISODate);
      r["project_updated"] = write_json(j.project_file, project);
      r["project_backup"] = backup;
    }
  }
  results_.append(r);
  emit progress(index_ + 1, int(jobs_.size()), tr("%1: %2").arg(QFileInfo(j.run_dir).fileName(), r.value("status").toString()));
  next();
}

}  // namespace p3dasset
