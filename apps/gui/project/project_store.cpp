#include "project/project_store.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <stdexcept>

#include "backends/settings_env.hpp"

namespace p3dgui {
namespace {

[[noreturn]] void fail(const QString& why) { throw std::runtime_error(why.toStdString()); }

QString safe_name(QString name) {
  name = name.trimmed();
  static const QString bad = QStringLiteral("<>:\"/\\|?*");
  for (QChar& c : name)
    if (bad.contains(c) || c.unicode() < 32) c = '_';
  while (name.endsWith('.') || name.endsWith(' ')) name.chop(1);
  return name.isEmpty() ? QStringLiteral("Untitled") : name.left(80);
}

QString now() { return QDateTime::currentDateTime().toString(Qt::ISODate); }

}  // namespace

QString file_sha256(const QString& file) {
  QFile f(file);
  if (!f.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash h(QCryptographicHash::Sha256);
  h.addData(&f);
  return QString::fromLatin1(h.result().toHex());
}

QString default_projects_dir() {
  const QString portable = p3dasset::setting_env("PORTABLE_USERDATA");
  if (!portable.isEmpty()) return QDir(portable).absoluteFilePath("Projects");
  const QString chosen = QSettings().value("projects/dir").toString();
  if (!chosen.isEmpty()) return chosen;
  return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/3re/Projects";
}

ThreeReProject ThreeReProject::create(const QString& parentDir, const QString& name, const QString& sourceImage) {
  QImageReader reader(sourceImage);
  const QSize size = reader.size();
  if (!QFileInfo(sourceImage).isFile() || !size.isValid()) fail(QObject::tr("%1 is not an image 3re can read.").arg(QDir::toNativeSeparators(sourceImage)));
  if (!QDir().mkpath(parentDir)) fail(QObject::tr("Could not create the projects folder %1.").arg(QDir::toNativeSeparators(parentDir)));
  const QString base = safe_name(name);
  QString dir = QDir(parentDir).filePath(base);
  for (int n = 2; QFileInfo::exists(dir); ++n) dir = QDir(parentDir).filePath(QStringLiteral("%1 (%2)").arg(base).arg(n));
  ThreeReProject p;
  p.dir = QDir::cleanPath(dir);
  p.name = name.trimmed().isEmpty() ? base : name.trimmed();
  if (!QDir().mkpath(p.dir + "/source") || !QDir().mkpath(p.segmentationDir()) || !QDir().mkpath(p.runsDir()))
    fail(QObject::tr("Could not create the project folder %1.").arg(QDir::toNativeSeparators(p.dir)));
  p.source_file = "source/" + QFileInfo(sourceImage).fileName();
  if (!QFile::copy(sourceImage, p.sourcePath())) fail(QObject::tr("Could not copy the image into the project."));
  QFile::setPermissions(p.sourcePath(), QFile::permissions(p.sourcePath()) | QFileDevice::WriteOwner);
  p.source_sha256 = file_sha256(p.sourcePath());
  p.source_size = size;
  p.created = p.modified = now();
  p.save();
  return p;
}

ThreeReProject ThreeReProject::open(const QString& projectFile) {
  QFile f(projectFile);
  if (!f.open(QIODevice::ReadOnly)) fail(QObject::tr("Could not open %1.").arg(QDir::toNativeSeparators(projectFile)));
  QJsonParseError err;
  const QJsonObject o = QJsonDocument::fromJson(f.readAll(), &err).object();
  if (err.error != QJsonParseError::NoError || o.value("format").toString() != "3re-project")
    fail(QObject::tr("%1 is not a 3re project.").arg(QDir::toNativeSeparators(projectFile)));
  if (o.value("version").toInt() > 1) fail(QObject::tr("This project was saved by a newer 3re. Update 3re to open it."));
  ThreeReProject p;
  p.dir = QDir::cleanPath(QFileInfo(projectFile).absolutePath());
  p.name = o.value("name").toString();
  const QJsonObject src = o.value("source").toObject();
  p.source_file = src.value("file").toString();
  p.source_sha256 = src.value("sha256").toString();
  p.source_size = QSize(src.value("width").toInt(), src.value("height").toInt());
  for (const auto& r : o.value("runs").toArray()) p.runs << r.toString();
  for (const auto& v : o.value("views").toArray()) {
    const auto vo = v.toObject();
    const QString file = vo.value("file").toString();
    if (file.isEmpty() || file.contains("..")) continue;
    p.views.push_back({file, vo.value("label").toString("auto"), vo.value("sha256").toString()});
  }
  p.result_glb = o.value("result").toString();
  p.created = o.value("created").toString();
  p.modified = o.value("modified").toString();
  if (p.source_file.isEmpty() || p.source_file.contains("..")) fail(QObject::tr("The project file is damaged (no source image)."));
  return p;
}

void ThreeReProject::save() {
  modified = now();
  QJsonArray r;
  for (const QString& run : runs) r.append(run);
  QJsonArray v;
  for (const auto& view : views) v.append(QJsonObject{{"file", view.file}, {"label", view.label}, {"sha256", view.sha256}});
  const QJsonObject o{{"format", "3re-project"},
                      {"version", 1},
                      {"name", name},
                      {"created", created},
                      {"modified", modified},
                      {"source", QJsonObject{{"file", source_file}, {"sha256", source_sha256}, {"width", source_size.width()}, {"height", source_size.height()}}},
                      {"segmentation", "segmentation/segmentation.json"},
                      {"runs", r},
                      {"views", v},
                      {"result", result_glb}};
  QSaveFile f(projectFile());
  if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(o).toJson()) < 0 || !f.commit())
    fail(QObject::tr("Could not save the project %1: %2").arg(QDir::toNativeSeparators(projectFile()), f.errorString()));
}

QString ThreeReProject::importView(const QString& file) {
  const QString inside = QDir(dir).relativeFilePath(file);
  if (!inside.startsWith("..") && QFileInfo(file).isFile()) return inside;  // already in the project
  if (!QDir().mkpath(dir + "/source/angles")) fail(QObject::tr("Could not create the project's angles folder."));
  const QFileInfo fi(file);
  QString rel = "source/angles/" + fi.fileName();
  for (int n = 2; QFileInfo::exists(dir + "/" + rel); ++n) rel = QStringLiteral("source/angles/%1 (%2).%3").arg(fi.completeBaseName()).arg(n).arg(fi.suffix());
  if (!QFile::copy(file, dir + "/" + rel)) fail(QObject::tr("Could not copy %1 into the project.").arg(QDir::toNativeSeparators(file)));
  QFile::setPermissions(dir + "/" + rel, QFile::permissions(dir + "/" + rel) | QFileDevice::WriteOwner);
  return rel;
}

bool ThreeReProject::sourceIntact() const { return !source_sha256.isEmpty() && file_sha256(sourcePath()) == source_sha256; }

void remember_recent(const QString& projectFile) {
  QSettings s;
  QStringList list = s.value("projects/recent").toStringList();
  const QString f = QDir::cleanPath(QFileInfo(projectFile).absoluteFilePath());
  list.removeAll(f);
  list.prepend(f);
  while (list.size() > 10) list.removeLast();
  s.setValue("projects/recent", list);
}

QStringList recent_projects() {
  QStringList out;
  for (const QString& f : QSettings().value("projects/recent").toStringList())
    if (QFileInfo(f).isFile()) out << f;
  return out;
}

}  // namespace p3dgui
