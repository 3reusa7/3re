#pragma once
// LOW-POLY STUDIO page of the Inspector: typed parts (suggest, confirm, change
// types), build settings (budget, joint loops, style, template), Build, the
// per-part triangle table, rest / test pose, and the checks that keep the model
// from PASS. It only displays the SessionView and emits requests; MainWindow
// makes the engine calls (the same Session commands MCP and the CLI use).
#include <QWidget>

#include "pixel3d/engine/job_runner.hpp"

class QCheckBox;
class QComboBox;
class QPushButton;
class QSlider;
class QSpinBox;
class QTableWidget;

namespace p3dgui {

class LowpolyPanel : public QWidget {
  Q_OBJECT
 public:
  LowpolyPanel(const pixel3d::SessionView& v, bool posed, QWidget* parent = nullptr);
  // The build settings as the controls show them (only values that differ from the project's).
  pixel3d::LowpolyUpdate update() const;
  QPushButton* buildButton() const { return build_; }
  QPushButton* suggestButton() const { return suggest_; }
  QPushButton* layersButton() const { return layers_; }
  QTableWidget* partsTable() const { return parts_; }

 signals:
  void suggestRequested();
  void layerPartsRequested();
  void buildRequested(const pixel3d::LowpolyUpdate& u);
  void partTypeRequested(int part, pixel3d::lowpoly::PartType type);
  void poseToggled(bool posed);
  void exportRequested();

 private:
  pixel3d::LowpolyProjectSettings settings_;
  QSlider* budget_slider_ = nullptr;
  QSpinBox* budget_ = nullptr;
  QSpinBox* loops_ = nullptr;
  QComboBox* style_ = nullptr;
  QComboBox* template_ = nullptr;
  QCheckBox* baked_ = nullptr;
  QCheckBox* accept_depth_ = nullptr;
  QPushButton* suggest_ = nullptr;
  QPushButton* layers_ = nullptr;
  QPushButton* build_ = nullptr;
  QTableWidget* parts_ = nullptr;
  bool front_only_ = false;
};

}  // namespace p3dgui
