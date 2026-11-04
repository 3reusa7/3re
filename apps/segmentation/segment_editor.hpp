#pragma once
// The Segment step of Image to 3D (spec 3re §7): mark the regions of a picture to keep,
// label them, correct them with a brush. SAM 2.1-tiny (CPU) proposes a region for a
// click or a box; without SAM, or for pixel art, colour regions do. Every edit can be
// undone (Ctrl+Z, Ctrl+Shift+Z / Ctrl+Y). Regions are labelled by the user — never
// presented as automatic body-part detection.
#include <QFutureWatcher>
#include <QImage>
#include <QPointer>
#include <QSet>
#include <QWidget>
#include <memory>
#include <optional>

#include "segmentation/mask_set.hpp"

class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QToolButton;
class QVBoxLayout;

namespace p3dseg {
class SamEngine;
}

namespace p3dgui {

class Spinner;
class SegmentCanvas;

class SegmentEditor : public QWidget {
  Q_OBJECT
 public:
  enum class Tool { Click, Box, Brush, Eraser };
  enum class ClickMode { NewRegion, AddToCurrent, SubtractFromCurrent };
  explicit SegmentEditor(QWidget* parent = nullptr);
  ~SegmentEditor() override;

  // The picture (full resolution) and the SHA-256 of its file. Starts a new, empty set.
  void setImage(const QImage& original, const QString& sha256);
  // Replaces the regions (a set loaded from the project). Its size must be the working size.
  void setMasks(std::unique_ptr<p3dseg::MaskSet> masks);
  p3dseg::MaskSet* masks() const { return masks_.get(); }
  const QImage& workingImage() const { return working_; }
  QSize originalSize() const { return original_size_; }
  QString sha256() const { return sha_; }
  bool hasIncludedRegions() const;
  void setTool(Tool t);
  Tool tool() const { return tool_; }
  int currentRegion() const { return current_; }
  void setCurrentRegion(int id);
  // SAM on/off (off: colour regions only; tests and pixel art).
  void setEngineEnabled(bool on);
  void setSamPaths(const QString& dir, const QString& ortDll);
  // Frees SAM (about 1 GB of RAM) — before a generation; it reloads on the next click.
  void releaseEngine();
  QString statusText() const;
  void setNotice(const QString& text);  // e.g. "saved regions not applied"

  // Interaction entry points (also used by the canvas).
  void clickAt(QPointF p, ClickMode mode);
  void boxAt(QRectF r);
  void suggestRegions();

 signals:
  void regionsChanged();
  void engineMetrics(const QString& text);

 protected:
  bool eventFilter(QObject* o, QEvent* e) override;
  void keyPressEvent(QKeyEvent* e) override;

 private:
  void rebuildList();
  void updateStatus();
  void ensureEngine();
  void runJob(std::function<QList<QImage>(p3dseg::SamEngine*)> sam, std::function<QList<QImage>()> colour,
              std::function<void(const QList<QImage>&)> apply, const QString& busy);
  QImage colourAt(QPointF p) const;

  std::unique_ptr<p3dseg::MaskSet> masks_;
  std::unique_ptr<p3dseg::SamEngine> engine_;
  QFutureWatcher<QList<QImage>> job_;
  std::function<void(const QList<QImage>&)> apply_;
  QImage working_;
  QSize original_size_;
  QString sha_, sam_dir_, ort_dll_, notice_, status_;
  bool engine_enabled_ = true, engine_failed_ = false, encoded_ = false, crisp_ = false;
  Tool tool_ = Tool::Click;
  int current_ = 0;
  QSet<int> selected_;  // rows picked for Merge (Ctrl+click)
  QString list_signature_;  // what the region list shows (ids, names, labels, use, selection)

  SegmentCanvas* canvas_ = nullptr;
  QToolButton *click_ = nullptr, *box_ = nullptr, *brush_ = nullptr, *eraser_ = nullptr;
  QPushButton *suggest_ = nullptr, *undo_ = nullptr, *redo_ = nullptr, *merge_ = nullptr, *split_ = nullptr;
  QSlider* brush_size_ = nullptr;
  QComboBox* mode_ = nullptr;
  QLabel *status_label_ = nullptr, *summary_ = nullptr, *notice_label_ = nullptr;
  Spinner* spinner_ = nullptr;
  QWidget* list_ = nullptr;
  QVBoxLayout* list_layout_ = nullptr;
  friend class SegmentCanvas;
};

}  // namespace p3dgui
