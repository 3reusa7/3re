#pragma once
// Smart Retopology workspace (3re 2.5): a generated or opened model becomes a clean quad mesh with
// QuadWild-BiMDF, run by the pixel3d-retopo worker (no Blender, no command line). The result is compared
// with the model (before/after, quad wireframe, counts, surface distance), can be edited by hand
// (RetopoMeshEditor) and is saved as a GLB with the model's colours, read back before success is shown.
#include <QProcess>
#include <QWidget>
#include <memory>
#include <nlohmann/json.hpp>
#include "pixel3d/mesh/mesh.hpp"
#include "pixel3d/render/rasterizer.hpp"

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;

namespace p3dgui {
class Image3DWorkspace;
class RetopoMeshEditor;
class Section;

class RetopoWorkspace : public QWidget {
  Q_OBJECT
 public:
  explicit RetopoWorkspace(bool software, QWidget* parent = nullptr);
  ~RetopoWorkspace() override;
  void setGenerator(Image3DWorkspace* ai) { ai_ = ai; refresh(); }
  void refresh();  // the generated model may have changed
  void setAdvanced(bool on);

  // Also used by tests and the end-to-end driver.
  bool useGenerated();
  bool openModel(const QString& file);
  bool retopologize();
  bool saveGlb(const QString& file);
  void cancel();
  bool running() const { return process_.state() != QProcess::NotRunning; }
  QString statusText() const;
  QString engineDir() const;
  RetopoMeshEditor* editor() const { return editor_; }
  QSpinBox* facesBox() const { return faces_; }
  const nlohmann::json& lastReport() const { return report_; }

 signals:
  void busyChanged(bool busy, const QString& text);
  void finished(bool ok, const QString& message);  // a retopology or a save ended

 private:
  void setSource(const QString& name, const QString& file, std::shared_ptr<const pixel3d::Mesh> geometry,
                 std::shared_ptr<const pixel3d::TexturedMesh> appearance);
  void setStatus(const QString& text, const char* tone = "");
  void setRunning(bool on, const QString& text);
  void readOutput();
  void processFinished(int code, QProcess::ExitStatus status);
  void showResult();
  QString worker() const;
  QString outputParent() const;

  enum class Job { None, Remesh, Export };
  Image3DWorkspace* ai_ = nullptr;
  RetopoMeshEditor* editor_ = nullptr;
  QLabel *source_name_ = nullptr, *source_info_ = nullptr, *engine_info_ = nullptr, *status_ = nullptr, *stats_ = nullptr,
         *verdict_ = nullptr;
  QPushButton *use_generated_ = nullptr, *open_ = nullptr, *run_ = nullptr, *cancel_ = nullptr, *save_ = nullptr,
              *folder_ = nullptr;
  QSpinBox* faces_ = nullptr;
  QSlider* density_ = nullptr;
  QComboBox *mode_ = nullptr, *texture_size_ = nullptr;
  QProgressBar* progress_ = nullptr;
  Section* advanced_ = nullptr;
  QProcess process_;
  Job job_ = Job::None;
  QString source_file_, source_label_, run_dir_, export_file_, engine_override_;
  std::shared_ptr<const pixel3d::Mesh> source_;
  std::shared_ptr<const pixel3d::TexturedMesh> look_;
  nlohmann::json report_;
  bool result_edited_ = false, cancelled_ = false;
};

}  // namespace p3dgui
