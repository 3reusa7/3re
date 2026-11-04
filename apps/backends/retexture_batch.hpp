#pragma once
// "Re-texture all saved models" (3re 2.3): runs the current texture engine once more on every saved
// generation that still has TRELLIS's texture volume (native.post) next to its geometry, one at a time in
// the background, cancellable. Nothing is overwritten: each result is a new textured-v2.glb (or the next
// free name) with its report next to the run; a project's result points to it afterwards (the previous
// project file is kept as project.3re.before-v2).
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QStringList>

namespace p3dasset {

class RetextureBatch : public QObject {
  Q_OBJECT
 public:
  struct Job {
    QString run_dir, geometry, picture, native_post, project_file, output;
    bool crisp = false;
    QStringList view_args;  // --view FILE LABEL ... (project's other angles)
  };
  explicit RetextureBatch(QObject* parent = nullptr);
  ~RetextureBatch() override;
  // Saved runs under these folders (depth <= 4) with geometry.glb + native.post + a picture.
  static QVector<Job> scan(const QStringList& roots);
  // Default roots: Documents/3re/Projects and Documents/Pixel3D/Generated (plus the portable data folder).
  static QStringList defaultRoots();
  bool start(const QVector<Job>& jobs, const QString& worker);
  bool running() const { return running_; }
  QJsonObject summary() const { return summary_; }
 public slots:
  void cancel();
 signals:
  void progress(int done, int total, const QString& text);
  void finished(bool cancelled, const QJsonObject& summary);
 private:
  void next();
  void jobDone(int code, QProcess::ExitStatus status);
  QVector<Job> jobs_;
  int index_ = -1;
  bool running_ = false, cancelled_ = false;
  QString worker_;
  QProcess process_;
  QElapsedTimer timer_;
  QJsonArray results_;
  QJsonObject summary_;
};

}  // namespace p3dasset
