#pragma once
// A 3re Image-to-3D project: one folder with project.3re (JSON), a copy of the source
// image, its segmentation and every generation run (spec 2026-10-20-3re-design.md §6).
// Errors are std::runtime_error with a message a user can act on.
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

namespace p3dgui {

struct ThreeReProject {
  QString dir, name;
  QString source_file;  // relative to dir, e.g. "source/knight.png"
  QString source_sha256;
  QSize source_size;
  QStringList runs;     // relative run folders
  // Pictures from other angles (3re 2.3): copies in source/angles/, the worker's angle label each. Older 3re
  // versions ignore the "views" key, so projects stay readable both ways.
  struct View { QString file, label, sha256; };
  QVector<View> views;
  QString result_glb;   // relative, empty until a result exists
  QString created, modified;

  // New project folder <parentDir>/<name>[ (n)] with a copy of the source image.
  static ThreeReProject create(const QString& parentDir, const QString& name, const QString& sourceImage);
  static ThreeReProject open(const QString& projectFile);
  void save();  // atomic (temporary file + rename); the previous file stays intact on failure

  QString projectFile() const { return dir + "/project.3re"; }
  QString sourcePath() const { return dir + "/" + source_file; }
  QString segmentationDir() const { return dir + "/segmentation"; }
  QString runsDir() const { return dir + "/runs"; }
  bool sourceIntact() const;  // the source copy still has the recorded SHA-256
  QString viewPath(const View& v) const { return dir + "/" + v.file; }
  // Copies a picture into source/angles/ (never overwriting) and returns its relative path.
  QString importView(const QString& file);
};

QString default_projects_dir();  // Documents/3re/Projects (or the portable user data folder)
QString file_sha256(const QString& file);
void remember_recent(const QString& projectFile);
QStringList recent_projects();  // newest first, existing files only, at most 10

}  // namespace p3dgui
