#pragma once
// Image to 3D · AI Generation (spec 2026-10-20-3re-design.md §5.2): a guided
// Import → Segment → Generate → Preview workspace over the TRELLIS.2 backend.
// Same backend, safeguards and honest labels as the earlier Image3DDialog; rarely
// used tools (seed, captures, setup file, saved results, cleanup resume, saved
// texture sessions) live under Advanced. Results are kept in a 3re project folder.
#include <QFutureWatcher>
#include <QImage>
#include <QJsonObject>
#include <QWidget>
#include <optional>

#include "backends/trellis_backend.hpp"
#include "project/project_store.hpp"
#include "view_state.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTimer;
class QDragEnterEvent;
class QDropEvent;
class QMimeData;

namespace p3dgui {

class Viewport;
class StepBar;
class Section;
class PreviewBar;
class SegmentEditor;
class AnglesPanel;

class Image3DWorkspace : public QWidget {
  Q_OBJECT
 public:
  enum Step { Import = 0, Segment = 1, Generate = 2, Preview = 3 };
  explicit Image3DWorkspace(bool force_cpu = false, QWidget* parent = nullptr);
  ~Image3DWorkspace() override;

  p3dasset::TrellisBackend& backend() { return backend_; }
  Viewport& preview() { return *viewport_; }
  void importImage(const QString& file);
  void setTextureSession(const QString& file);
  void openSaved(const QString& file);
  void openProject(const QString& projectFile);
  QString statusText() const;
  QString projectName() const;
  const std::optional<ThreeReProject>& project() const { return project_; }
  void setProjectsDir(const QString& dir) { projects_dir_ = dir; }
  void showStep(int step);
  int currentStep() const;
  bool hasResult() const;
  // Asked before the window closes: false = keep the window open (a generation is running and the user said no).
  bool confirmClose();
  // The cutout that generation will use (the automatic cutout, or the included segmentation regions).
  QImage generationCutout() const;
  SegmentEditor* segmentEditor() const { return segment_; }
  AnglesPanel* anglesPanel() const { return angles_; }
  // Screenshots and reviews: orbit camera at (yaw, pitch), framed on a sphere given in the model's GLB
  // coordinates (radius <= 0: the whole model).
  void lookAt(double yaw, double pitch, double x, double y, double z, double radius);

 public slots:
  void generate();
  void cancel();
  void setAdvanced(bool on);

 signals:
  void busyChanged(bool busy, const QString& text);
  void resultReady();
  void continueToExport();
  void pixelBuilderRequested();
  void segmentationMetrics(const QString& text);

 protected:
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dropEvent(QDropEvent* e) override;

 private:
  static QString droppedImage(const QMimeData* mime);
  static Scene makeScene(const p3dasset::MeshAsset& asset, bool crisp);
  QWidget* buildImportStep();
  QWidget* buildSegmentStep();
  QWidget* buildGenerateStep();
  QWidget* buildPreviewStep();
  void setBusy(bool busy);
  void checkGpu();
  void showResult(bool available, const QString& message);
  void refreshMemoryState();
  bool hasGenerationInput() const;
  void ensureProject();
  void recordResult();
  void applyAppearance();
  void loadSegmentation();
  void saveSegmentation();
  void updateSamPaths();
  bool prepareSegmentedInput(QString* error);
  void saveAngles();
  void loadAngles();

  p3dasset::TrellisBackend backend_;
  QFutureWatcher<p3dasset::ImageInput> image_job_;
  QFutureWatcher<Scene> scene_job_;
  QFutureWatcher<QJsonObject> gpu_job_;
  QString image_path_, texture_session_, projects_dir_, auto_output_;
  std::optional<ThreeReProject> project_;
  QImage cutout_, appearance_source_, original_;
  QImage seg_cutout_, seg_appearance_;  // prepared from the included regions for the running generation
  bool use_segmented_ = false;
  QTimer* seg_save_ = nullptr;
  bool crisp_ = false, gpu_checked_ = false;

  StepBar* steps_ = nullptr;
  QStackedWidget* pages_ = nullptr;
  Viewport* viewport_ = nullptr;
  PreviewBar* preview_bar_ = nullptr;
  SegmentEditor* segment_ = nullptr;
  AnglesPanel* angles_ = nullptr;
  Section* angles_section_ = nullptr;
  QString pending_result_;  // a project's result, opened once its picture is ready
  QWidget* advanced_box_ = nullptr;
  QLabel *image_ = nullptr, *original_view_ = nullptr, *import_hint_ = nullptr, *pixel_hint_ = nullptr;
  QLabel *status_ = nullptr, *checks_ = nullptr, *memory_ = nullptr, *gpu_status_ = nullptr, *resources_ = nullptr;
  QLabel *elapsed_ = nullptr, *result_chip_ = nullptr, *result_summary_ = nullptr, *material_ = nullptr, *project_label_ = nullptr;
  QLineEdit *setup_ = nullptr, *output_ = nullptr;
  QSpinBox* seed_ = nullptr;
  QCheckBox *white_ = nullptr, *capture_ = nullptr, *texture_ = nullptr;
  QComboBox *appearance_mode_ = nullptr, *input_type_ = nullptr, *profile_ = nullptr, *resolution_ = nullptr;
  QPushButton *import_ = nullptr, *saved_ = nullptr, *resume_ = nullptr, *generate_ = nullptr, *cancel_ = nullptr;
  QPushButton *export_ = nullptr, *folder_ = nullptr, *gpu_check_ = nullptr, *apply_source_ = nullptr, *texture_session_button_ = nullptr;
  QPushButton *to_segment_ = nullptr, *to_generate_ = nullptr, *skip_segment_ = nullptr;
  QPlainTextEdit* log_ = nullptr;
  QProgressBar* progress_ = nullptr;
  QTimer *memory_timer_ = nullptr, *clock_ = nullptr;
  qint64 started_ms_ = 0;
};

}  // namespace p3dgui
