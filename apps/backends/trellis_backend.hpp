#pragma once
#include <QElapsedTimer>
#include <QFile>
#include <QFutureWatcher>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <atomic>
#include <memory>
#include "mesh_asset.hpp"
#include "image_input.hpp"

namespace p3dasset {
struct BackendConfig {
  QString runtime_dir, models_dir, post_executable, weights_manifest;
  QString engine_sha256, post_sha256;
  // The 3re config ({"trellis":{...}}, paths relative to the file) or the legacy flat setup file.
  static BackendConfig read(const QString& path);
  QString engine() const;
};
// Segmentation (SAM 2.1-tiny + ONNX Runtime) from the same config's "sam" block; empty when absent.
struct SamConfig {
  QString dir, onnxruntime;
  static SamConfig read(const QString& path);
};
// The setup the application uses: THREERE_/PIXEL3D_TRELLIS_CONFIG, <appDir>/3re.config.json,
// <appDir>/trellis-backend.local.json, then the development tree's .toolchain/trellis/backend.json.
// Empty when none exists. appDir defaults to the executable's folder.
QString find_backend_config(const QString& appDir = {});
quint64 available_ram();
constexpr quint64 generation_admission_ram() { return 13ull*1024*1024*1024/4; }
struct PreparedAsset {
  std::shared_ptr<const pixel3d::Mesh> geometry;
  std::shared_ptr<const pixel3d::TexturedMesh> appearance;
  QString textured_glb, material_error;
  nlohmann::json materials = {{"mode", "geometry_only"}, {"kind", "geometry_only"}};
};

// A separate backend/controller, reusable by future mesh tools. Only Qt signals
// cross the GUI boundary; no drawing Session, Python, Blender or network needed.
class TrellisBackend : public QObject {
  Q_OBJECT
 public:
  explicit TrellisBackend(QObject* parent = nullptr);
  ~TrellisBackend() override;
  bool busy() const { return active_; }
  const MeshAsset& asset() const { return asset_; }
  QJsonObject report() const { return report_; }
  QString runDirectory() const { return run_dir_; }
  bool generate(const BackendConfig& config, const QImage& cutout, const QString& source,
                const QString& output_parent, quint32 seed = 42, bool capture = true);
  bool generateTexture(const BackendConfig& config,const QString& bundle,const QString& parent);
  bool openSaved(const QString& glb, const QString& output_parent, bool reproject_source = false);
  bool resumeCleanup(const BackendConfig& config,const QString& raw_glb,const QString& output_parent);
  void setAppearance(const QImage& image,bool enabled=true,int size=1024,bool crisp=false) { appearance_input_=image;appearance_enabled_=enabled;texture_size_=size;crisp_=crisp; }
  // Pictures of the same object from other angles (multi-angle import, 2.3): cutouts + the worker's angle label
  // ("back", "left", "auto", "left:95", ...). Used by every texture pass (generation and re-texturing).
  struct ExtraPicture { QImage image; QString label, name; };
  void setExtraPictures(const QVector<ExtraPicture>& pictures) { extra_pictures_=pictures; }
  bool exportGlb(const QString& destination, bool acknowledge_issues, QString& error) const;
  static bool exportAsset(const MeshAsset& asset, const QString& destination, bool acknowledge_issues, QString& error);
  // native_texture: TRELLIS also runs its texture stage and dumps mesh + PBR volume (no --no-texture;
  // stage captures are incompatible with texture and are skipped).
  static QStringList inferenceArguments(const BackendConfig& config, const QString& image,
                                        const QString& output, quint32 seed, const QString& captures,
                                        bool native_texture = false);
  // Texture weights (tex_flow_512, tex_dec) present and matching the manifest: {"available", "reason"}.
  static QJsonObject nativeTextureAvailability(const BackendConfig& config);
 signals:
  void progress(int percent, const QString& stage);
  void resources(double ramMiB,double gpuMiB,bool gpuMeasured);
  void logLine(const QString& text);
  void finished(bool output_available, const QString& message);
 public slots:
  void cancel();
 private:
  enum class Stage { None, TextureValidate, TextureGenerate, TextureBake, Generate, Cleanup, Inspect, Intersections, AutoTexture };
  bool begin(const QString& parent, bool inference, quint64 saved_required = 0);
  bool launch(Stage stage, const QString& executable, const QStringList& arguments);
  void readLogs();
  void processDone(int code, QProcess::ExitStatus status);
  void validateOutput();
  void finishInspection();
  // Auto Texture worker after the geometry audit (new generations and explicit retexturing).
  void startAppearance();
  void finishAutoTexture(int code, bool crashed);
  // The textured run failed: report it and run the verified geometry-only generation once.
  void retryGeometryOnly(const QString& reason);
  void preparePreview();
  void fail(const QString& message);
  bool saveReport();
  void sampleResources();
  bool containAndResume();
  void releaseJob();
  bool startGpuTelemetry();
  void stopGpuTelemetry();
  QString helper(const QString& name) const;

  QProcess process_;
  QTimer monitor_;
  QElapsedTimer timer_, phase_timer_;
  QFutureWatcher<QString> prepare_;
  QFutureWatcher<PreparedAsset> preview_;
  std::shared_ptr<std::atomic_bool> cancelled_;
  BackendConfig config_;
  MeshAsset asset_;
  QJsonObject report_, phases_, texture_job_;
  QStringList texture_args_;
  bool texture_request_=false;
  QVector<ExtraPicture> extra_pictures_;
  QFile stdout_, stderr_, telemetry_;
  QByteArray buffer_;
  QString run_dir_, glb_, original_, output_parent_, textured_;
  QImage appearance_input_;
  bool appearance_enabled_=true,crisp_=false,reproject_source_=false;
  int texture_size_=1024;
  quint32 seed_ = 42;
  bool active_ = false, inference_ = false, capture_ = true;
  bool native_texture_ = false;  // this generation runs TRELLIS's texture stage too (hybrid texture)
  Stage stage_ = Stage::None;
  void* job_ = nullptr;
  void* gpu_query_ = nullptr;
  void* gpu_dedicated_ = nullptr;
  void* gpu_shared_ = nullptr;
  int telemetry_failures_ = 0, flow_base_ = 8;
  int gpu_samples_ = 0;
  quint64 peak_rss_ = 0, peak_commit_ = 0, peak_gpu_ = 0, peak_shared_ = 0, minimum_free_ = ~quint64(0);
  quint64 preview_required_ = 1024ull*1024*1024;
};
}  // namespace p3dasset
