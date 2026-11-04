#pragma once
// Right panel. A status card (PASS / NEEDS REVIEW / FAIL / NOT CHECKED with
// the few facts that matter and one button), then exactly one context page:
// overview (nothing selected), issue (with its inline correction tool), part
// or face. Technical data lives in collapsed Advanced sections. The panel only
// displays and emits requests; MainWindow owns selection and engine calls.
#include <QPointer>
#include <QWidget>
#include <optional>
#include <vector>

#include "issues.hpp"

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QRadioButton;
class QVBoxLayout;

namespace p3dgui {

class Card;
class Section;

enum class InspectorPage { Overview, Issue, Part, Face };

// "Which parts are real?" — keep/remove per candidate, suggested arrangements.
class PartsChooser : public QWidget {
  Q_OBJECT
 public:
  PartsChooser(const Issue& issue, const pixel3d::SessionView& v, bool advanced, QWidget* parent = nullptr);
  std::vector<pixel3d::UserDecision> decisions() const;
  int arrangementCount() const { return static_cast<int>(alternatives_.size()); }
  void chooseArrangement(int index);  // index into the engine's alternatives
  QPushButton* applyButton() const { return apply_; }

 signals:
  void apply(const std::vector<pixel3d::UserDecision>& decisions);
  void candidatesHovered(const std::vector<int>& candidates);  // empty = none

 protected:
  bool eventFilter(QObject* o, QEvent* e) override;

 private:
  std::vector<std::pair<int, QCheckBox*>> rows_;
  std::vector<std::vector<int>> alternatives_;
  std::vector<QPushButton*> arrangement_buttons_;
  QPushButton* apply_ = nullptr;
};

// "Hole or painted detail?" — recess depth or flush.
class DepthEditor : public QWidget {
  Q_OBJECT
 public:
  DepthEditor(const Issue& issue, const pixel3d::SessionView& v, QWidget* parent = nullptr);
  std::optional<pixel3d::DepthConstraint> constraint() const;
  void setDepth(double px);
  void setFlush(bool flush);
  QPushButton* applyButton() const { return apply_; }

 signals:
  void apply(const pixel3d::DepthConstraint& c);

 private:
  Issue issue_;
  QRadioButton* hole_ = nullptr;
  QRadioButton* flush_ = nullptr;
  QDoubleSpinBox* depth_ = nullptr;
  QPushButton* apply_ = nullptr;
};

class Inspector : public QWidget {
  Q_OBJECT
 public:
  explicit Inspector(QWidget* parent = nullptr);
  void update_from(const pixel3d::SessionView& v, const std::vector<Issue>& issues);
  void setAdvanced(bool on);
  void setRendererInfo(const QString& s) { renderer_ = s; }
  void setLowpolyPose(bool posed) { lowpoly_pose_ = posed; }

  void showOverview();
  bool showIssue(const QString& key);  // false if there is no such issue
  void showPart(int part);
  void showFace(int triangle);
  void showMessage(const QString& text, theme::Tone tone);

  InspectorPage page() const { return page_; }
  QString currentIssue() const { return issue_key_; }
  int currentPart() const { return part_; }
  QString statusText() const;  // engine status code: PASS, NEEDS_REVIEW, FAIL, NOT_VALIDATED
  QString statusWord() const;  // what the card shows: PASS, NEEDS REVIEW, FAIL, NOT CHECKED
  PartsChooser* partsChooser() const { return chooser_; }
  DepthEditor* depthEditor() const { return depth_editor_; }
  const std::vector<Issue>& issues() const { return issues_; }

 signals:
  void issueRequested(const QString& key);
  void partRequested(int part);
  void overviewRequested();
  // review, export, validate, run_all, import, settings, and the part_* / face_* editing actions.
  void actionRequested(const QString& key);
  void pushPullRequested(double distance);  // the selected face
  void singleViewDepthRequested(const std::string& preset, std::optional<double> depth);
  // LOW-POLY STUDIO page
  void lowpolySuggestRequested();
  void lowpolyLayerPartsRequested();
  void lowpolyBuildRequested(const pixel3d::LowpolyUpdate& u);
  void lowpolyPartTypeRequested(int part, pixel3d::lowpoly::PartType type);
  void lowpolyPoseToggled(bool posed);
  void applyParts(const std::vector<pixel3d::UserDecision>& decisions);
  void applyDepth(const pixel3d::DepthConstraint& c);
  void candidatesHovered(const std::vector<int>& candidates);
  void frameRequested();

 private:
  void rebuildStatus();
  void rebuildPage();
  QWidget* buildOverview();
  QWidget* buildIssue(const Issue& is);
  QWidget* buildPart(int part);
  QWidget* buildFace(int triangle);
  QWidget* navRow(const QString& back_text, QWidget* trailing = nullptr);
  Section* advancedSection(const QString& title);
  void setPage(QWidget* w);

  pixel3d::SessionView view_;
  std::vector<Issue> issues_;
  bool advanced_ = false;
  QString renderer_;
  bool lowpoly_pose_ = false;
  InspectorPage page_ = InspectorPage::Overview;
  QString issue_key_;
  int part_ = -1, face_ = -1;
  uint64_t face_revision_ = 0;
  QVBoxLayout* root_ = nullptr;
  QWidget* status_host_ = nullptr;
  QVBoxLayout* status_layout_ = nullptr;
  QPointer<QWidget> page_widget_;
  QLabel* message_ = nullptr;
  QLabel* status_word_ = nullptr;
  QPointer<PartsChooser> chooser_;
  QPointer<DepthEditor> depth_editor_;
};

}  // namespace p3dgui
