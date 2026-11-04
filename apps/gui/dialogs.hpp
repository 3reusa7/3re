#pragma once
// Modal dialogs (project settings, crash recovery, single view, part transform/plane/volume).
// They never call the engine.
// Part and depth decisions are made inline in the Inspector.
#include <QDialog>
#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

#include "pixel3d/engine/autosave.hpp"
#include "pixel3d/engine/job_runner.hpp"

class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QSpinBox;
class QLabel;
class QCheckBox;

namespace p3dgui {

// What File ▸ New and the window's first project start with: a Low-Poly Studio project (no Top drawing needed).
pixel3d::ProjectSettings default_project_settings();

class NewProjectDialog : public QDialog {
  Q_OBJECT
 public:
  explicit NewProjectDialog(QWidget* parent = nullptr);
  pixel3d::ProjectSettings settings() const;
  void setSettings(const pixel3d::ProjectSettings& s);  // show a project's current values

 private:
  QLineEdit* name_;
  QDoubleSpinBox* ppm_;
  QComboBox* profile_;
  QSpinBox* alpha_;
  QCheckBox* normalize_;
  QSpinBox* target_;
  QSpinBox* bg_tol_;
  QComboBox* style_;
  QComboBox* mode_;
  pixel3d::LowpolyProjectSettings lowpoly_;  // build settings are edited on the Low-Poly page; kept as they are
  double axis_tolerance_ = pixel3d::NormalizeSettings{}.axis_tolerance;  // not edited here; kept as it is
  pixel3d::StyleSettings style_settings_;  // tuning and materials are not edited here; kept as they are
};

// SINGLE VIEW: one drawing, an editable default depth. All depth is inferred.
class SingleViewDialog : public QDialog {
  Q_OBJECT
 public:
  explicit SingleViewDialog(QWidget* parent = nullptr);
  void setPath(const QString& path);
  QString path() const;
  pixel3d::ViewRole role() const;
  pixel3d::SingleViewSettings settings() const;
  pixel3d::ProjectSettings project() const;

 private:
  QLineEdit* path_;
  QComboBox* role_;
  QComboBox* preset_;
  QDoubleSpinBox* depth_;
  QLineEdit* name_;
};

// Move / rotate / scale (or duplicate) a part.
class TransformDialog : public QDialog {
  Q_OBJECT
 public:
  TransformDialog(int part, bool copy, QWidget* parent = nullptr);
  pixel3d::TransformRequest request() const;

 private:
  int part_;
  bool copy_;
  QDoubleSpinBox *tx_, *ty_, *tz_, *deg_, *scale_;
  QComboBox* axis_;
  QCheckBox* snap_;
};

// A plane x|y|z = at: split a part in two, or cut away one side.
class PlaneDialog : public QDialog {
  Q_OBJECT
 public:
  enum class Kind { Split, Cut };
  PlaneDialog(Kind kind, pixel3d::Dims dims, QWidget* parent = nullptr);
  int axis() const;
  double at() const;
  int keep() const;  // Cut: -1 keep the negative side, +1 the positive side

 private:
  pixel3d::Dims dims_;
  QComboBox* axis_;
  QDoubleSpinBox* at_;
  QComboBox* keep_ = nullptr;
};

// Depth range (from the view's camera side) for Add/Erase Volume.
class VolumeDialog : public QDialog {
  Q_OBJECT
 public:
  VolumeDialog(bool add, double extent, QWidget* parent = nullptr);
  double from() const;
  double to() const;

 private:
  QDoubleSpinBox* from_;
  QDoubleSpinBox* to_;
};

// Export: file, engine preset, format, units, front direction, unlit.
class ExportDialog : public QDialog {
  Q_OBJECT
 public:
  ExportDialog(const QString& suggestion, pixel3d::Dims dims, double pixels_per_meter, QWidget* parent = nullptr);
  pixel3d::ModelExportRequest request() const;
  void setPath(const QString& path);
  void setPreset(pixel3d::EnginePreset p);

 private:
  void presetChanged();
  void update();
  pixel3d::Dims dims_;
  double ppm_;
  QLineEdit* path_;
  QComboBox* preset_;
  QComboBox* format_;
  QComboBox* units_;
  QComboBox* front_;
  QCheckBox* unlit_;
  QLabel* note_;
  QLabel* size_;
  bool updating_ = false;
};

// Startup recovery prompt. Returns the session to recover, or nullopt.
// `discard_all` is set when the user chose to discard the listed autosaves.
std::optional<pixel3d::RecoverableSession> ask_recovery(QWidget* parent,
                                                        const std::vector<pixel3d::RecoverableSession>& sessions,
                                                        bool* discard_all);

}  // namespace p3dgui
