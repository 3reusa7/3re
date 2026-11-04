#include "segmentation/segment_editor.hpp"

#include <QAction>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cmath>

#include "icons.hpp"
#include "segmentation/color_regions.hpp"
#include "segmentation/sam_engine.hpp"
#include "shell/spinner.hpp"
#include "theme.hpp"
#include "widgets.hpp"

namespace p3dgui {

using p3dseg::MaskSet;
using p3dseg::Region;

namespace {

long count_inside(const QImage& m) {
  long n = 0;
  for (int y = 0; y < m.height(); ++y) {
    const uchar* r = m.constScanLine(y);
    for (int x = 0; x < m.width(); ++x) n += r[x] > 127;
  }
  return n;
}

double mask_iou(const QImage& a, const QImage& b) {
  long inter = 0, uni = 0;
  for (int y = 0; y < a.height(); ++y) {
    const uchar *p = a.constScanLine(y), *q = b.constScanLine(y);
    for (int x = 0; x < a.width(); ++x) {
      inter += p[x] > 127 && q[x] > 127;
      uni += p[x] > 127 || q[x] > 127;
    }
  }
  return uni ? double(inter) / uni : 0;
}

QImage combine(const QImage& a, const QImage& b, bool subtract) {
  QImage out = a.copy();
  for (int y = 0; y < out.height(); ++y) {
    uchar* d = out.scanLine(y);
    const uchar* s = b.constScanLine(y);
    for (int x = 0; x < out.width(); ++x) d[x] = subtract ? (s[x] > 127 ? 0 : d[x]) : std::max(d[x], s[x]);
  }
  return out;
}

}  // namespace

// ---- canvas -------------------------------------------------------------------

class SegmentCanvas : public QWidget {
 public:
  explicit SegmentCanvas(SegmentEditor* ed) : QWidget(ed), ed_(ed) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(360, 300);
    setCursor(Qt::CrossCursor);
    overlay_timer_.setSingleShot(true);
    overlay_timer_.setInterval(30);
    QObject::connect(&overlay_timer_, &QTimer::timeout, this, [this] { rebuildOverlay(); });
  }
  void imageChanged() {
    fit_ = true;
    rebuildOverlay();
  }
  void masksChanged() { overlay_timer_.start(); }
  void fit() {
    fit_ = true;
    update();
  }

 protected:
  QRectF imageRect() const {
    const QSizeF img = ed_->working_.size();
    if (img.isEmpty()) return {};
    if (fit_) {
      const double s = std::min((width() - 24) / img.width(), (height() - 24) / img.height());
      zoom_ = std::max(0.05, s);
      offset_ = QPointF((width() - img.width() * zoom_) / 2, (height() - img.height() * zoom_) / 2);
    }
    return QRectF(offset_, img * zoom_);
  }
  QPointF toImage(QPointF w) const {
    imageRect();
    return (w - offset_) / zoom_;
  }

  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    const auto& c = theme::colors();
    p.fillRect(rect(), c.preview_bg);
    if (ed_->working_.isNull()) {
      p.setPen(c.text_secondary);
      p.drawText(rect(), Qt::AlignCenter, QObject::tr("Import a picture first."));
      return;
    }
    const QRectF r = imageRect();
    p.setRenderHint(QPainter::SmoothPixmapTransform, zoom_ < 2 && !ed_->crisp_);
    p.drawImage(r, ed_->working_);
    if (!overlay_.isNull()) p.drawImage(r, overlay_);
    if (dragging_ && ed_->tool_ == SegmentEditor::Tool::Box) {
      p.setPen(QPen(c.accent_hover, 1.5, Qt::DashLine));
      p.setBrush(theme::with_alpha(c.accent_hover, 0.12));
      p.drawRect(QRectF(press_, last_).normalized());
    }
    if ((ed_->tool_ == SegmentEditor::Tool::Brush || ed_->tool_ == SegmentEditor::Tool::Eraser) && underMouse()) {
      const double radius = ed_->brush_size_ ? ed_->brush_size_->value() * zoom_ : 8;
      p.setPen(QPen(ed_->tool_ == SegmentEditor::Tool::Eraser ? c.fail : c.text, 1.2));
      p.setBrush(Qt::NoBrush);
      p.drawEllipse(last_, radius, radius);
    }
  }

  void wheelEvent(QWheelEvent* e) override {
    if (ed_->working_.isNull()) return;
    imageRect();
    const QPointF anchor = toImage(e->position());
    const double factor = std::pow(1.0015, e->angleDelta().y());
    fit_ = false;
    zoom_ = std::clamp(zoom_ * factor, 0.05, 32.0);
    offset_ = e->position() - anchor * zoom_;
    update();
  }

  void mousePressEvent(QMouseEvent* e) override {
    if (ed_->working_.isNull() || !ed_->masks_) return;
    setFocus();
    press_ = last_ = e->position();
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && space_)) {
      panning_ = true;
      fit_ = false;
      return;
    }
    dragging_ = true;
    const auto t = ed_->tool_;
    if (t == SegmentEditor::Tool::Brush || t == SegmentEditor::Tool::Eraser) {
      int id = ed_->current_;
      if (!ed_->masks_->region(id) && t == SegmentEditor::Tool::Brush) {
        QImage empty(ed_->masks_->size(), QImage::Format_Grayscale8);
        empty.fill(0);
        id = ed_->masks_->add(empty, QObject::tr("Object"));
        ed_->setCurrentRegion(id);
      }
      if (!ed_->masks_->region(id)) {
        ed_->setNotice(QObject::tr("Select a region to erase from."));
        dragging_ = false;
        return;
      }
      ed_->masks_->beginStroke(id);
      dab(e->position());
    }
  }
  void mouseMoveEvent(QMouseEvent* e) override {
    const QPointF prev = last_;
    last_ = e->position();
    if (panning_) {
      offset_ += last_ - prev;
      update();
      return;
    }
    if (dragging_ && ed_->masks_ && ed_->masks_->stroking()) {
      const double step = std::max(1.0, ed_->brush_size_->value() * zoom_ * 0.5);
      const double dist = std::hypot(last_.x() - prev.x(), last_.y() - prev.y());
      for (double t = step; t < dist; t += step) dab(prev + (last_ - prev) * (t / dist));
      dab(last_);
    }
    update();
  }
  void mouseReleaseEvent(QMouseEvent* e) override {
    if (panning_) {
      panning_ = false;
      return;
    }
    if (!dragging_ || !ed_->masks_) return;
    dragging_ = false;
    const auto t = ed_->tool_;
    if (ed_->masks_->stroking()) {
      ed_->masks_->endStroke();
    } else if (t == SegmentEditor::Tool::Box) {
      const QRectF w = QRectF(press_, e->position()).normalized();
      if (w.width() > 6 && w.height() > 6) ed_->boxAt(QRectF(toImage(w.topLeft()), toImage(w.bottomRight())));
    } else if (t == SegmentEditor::Tool::Click) {
      if ((e->position() - press_).manhattanLength() > 6) return;
      auto mode = SegmentEditor::ClickMode::NewRegion;
      if (e->button() == Qt::RightButton || (e->modifiers() & Qt::AltModifier)) mode = SegmentEditor::ClickMode::SubtractFromCurrent;
      else if (e->modifiers() & Qt::ShiftModifier) mode = SegmentEditor::ClickMode::AddToCurrent;
      ed_->clickAt(toImage(e->position()), mode);
    }
    update();
  }
  void keyPressEvent(QKeyEvent* e) override {
    if (e->key() == Qt::Key_Space) space_ = true;
    QWidget::keyPressEvent(e);
  }
  void keyReleaseEvent(QKeyEvent* e) override {
    if (e->key() == Qt::Key_Space) space_ = false;
    QWidget::keyReleaseEvent(e);
  }
  void resizeEvent(QResizeEvent*) override {
    if (fit_) update();
  }
  void leaveEvent(QEvent*) override { update(); }

 private:
  void dab(QPointF w) {
    const QPointF i = toImage(w);
    ed_->masks_->paint(ed_->masks_->stroking() ? ed_->current_ : ed_->current_, i.toPoint(), ed_->brush_size_->value(),
                       ed_->tool_ == SegmentEditor::Tool::Eraser);
  }
  void rebuildOverlay() {
    const MaskSet* set = ed_->masks_.get();
    if (!set || ed_->working_.isNull()) {
      overlay_ = QImage();
      update();
      return;
    }
    QImage o(set->size(), QImage::Format_ARGB32_Premultiplied);
    o.fill(Qt::transparent);
    for (const Region& r : set->regions()) {
      if (!r.visible) continue;
      const bool current = r.id == ed_->current_;
      const int a = r.include ? (current ? 150 : 110) : 45;
      const QRgb fill = qPremultiply(qRgba(r.color.red(), r.color.green(), r.color.blue(), a));
      const QRgb edge = qPremultiply(qRgba(current ? 255 : r.color.red(), current ? 255 : r.color.green(), current ? 255 : r.color.blue(), 235));
      const int w = set->size().width(), h = set->size().height();
      for (int y = 0; y < h; ++y) {
        const uchar* m = r.mask.constScanLine(y);
        const uchar* up = r.mask.constScanLine(std::max(0, y - 1));
        const uchar* dn = r.mask.constScanLine(std::min(h - 1, y + 1));
        QRgb* d = reinterpret_cast<QRgb*>(o.scanLine(y));
        for (int x = 0; x < w; ++x) {
          if (m[x] <= 127) continue;
          const bool border = x == 0 || x == w - 1 || m[x - 1] <= 127 || m[x + 1] <= 127 || up[x] <= 127 || dn[x] <= 127;
          d[x] = border ? edge : fill;
        }
      }
    }
    overlay_ = o;
    update();
  }

  SegmentEditor* ed_;
  QImage overlay_;
  QTimer overlay_timer_;
  mutable double zoom_ = 1;
  mutable QPointF offset_;
  bool fit_ = true, dragging_ = false, panning_ = false, space_ = false;
  QPointF press_, last_;
};

// ---- editor -------------------------------------------------------------------

SegmentEditor::SegmentEditor(QWidget* parent) : QWidget(parent) {
  setObjectName("SegmentEditor");
  auto* row = new QHBoxLayout(this);
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(theme::space::lg);

  auto* left = new Card(this);
  auto* l = new QVBoxLayout(left);
  l->setContentsMargins(12, 10, 12, 12);
  l->setSpacing(8);
  auto* tools = new QHBoxLayout;
  tools->setSpacing(2);
  auto* group = new QButtonGroup(this);
  auto tool_button = [&](Icon ic, const QString& text, const QString& tip, Tool t, const char* name, const QKeySequence& key) {
    auto* b = new QToolButton(left);
    b->setObjectName(name);
    b->setIcon(icon(ic));
    b->setIconSize(QSize(theme::icon_size::md, theme::icon_size::md));
    b->setText(text);
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    b->setCheckable(true);
    b->setToolTip(QStringLiteral("%1 (%2)").arg(tip, key.toString()));
    b->setShortcut(key);
    b->setCursor(Qt::PointingHandCursor);
    group->addButton(b);
    tools->addWidget(b);
    connect(b, &QToolButton::clicked, this, [this, t] { setTool(t); });
    return b;
  };
  click_ = tool_button(Icon::ClickSelect, tr("Select"), tr("Click a region to add it. Shift+click adds to the selected region, "
                                                          "right-click or Alt+click removes from it"), Tool::Click, "segToolClick", QKeySequence("C"));
  box_ = tool_button(Icon::BoxSelect, tr("Box"), tr("Drag a box around a part"), Tool::Box, "segToolBox", QKeySequence("R"));
  brush_ = tool_button(Icon::Brush, tr("Brush"), tr("Paint onto the selected region (a new one if none is selected)"), Tool::Brush, "segToolBrush", QKeySequence("B"));
  eraser_ = tool_button(Icon::Eraser, tr("Eraser"), tr("Erase from the selected region"), Tool::Eraser, "segToolEraser", QKeySequence("E"));
  click_->setChecked(true);
  tools->addSpacing(8);
  brush_size_ = new QSlider(Qt::Horizontal, left);
  brush_size_->setObjectName("segBrushSize");
  brush_size_->setRange(1, 80);
  brush_size_->setValue(12);
  brush_size_->setFixedWidth(110);
  brush_size_->setToolTip(tr("Brush size ([ and ])"));
  tools->addWidget(make_label(tr("Size"), "secondary", left));
  tools->addWidget(brush_size_);
  tools->addStretch(1);
  undo_ = make_button(QString(), "ghost", icon(Icon::Undo), left);
  undo_->setObjectName("segUndo");
  undo_->setToolTip(tr("Undo (Ctrl+Z)"));
  redo_ = make_button(QString(), "ghost", icon(Icon::Redo), left);
  redo_->setObjectName("segRedo");
  redo_->setToolTip(tr("Redo (Ctrl+Shift+Z / Ctrl+Y)"));
  tools->addWidget(undo_);
  tools->addWidget(redo_);
  l->addLayout(tools);

  canvas_ = new SegmentCanvas(this);
  l->addWidget(canvas_, 1);

  auto* status = new QHBoxLayout;
  spinner_ = new Spinner(left, 16);
  status->addWidget(spinner_);
  status_label_ = make_label(QString(), "secondary", left);
  status_label_->setObjectName("segStatus");
  status_label_->setWordWrap(true);
  status->addWidget(status_label_, 1);
  mode_ = new QComboBox(left);
  mode_->setObjectName("segMode");
  mode_->addItems({tr("Smart selection (SAM)"), tr("Colour regions")});
  mode_->setToolTip(tr("Smart selection finds the shape you click (runs on the CPU). Colour regions select areas of similar "
                       "colour — best for pixel art."));
  status->addWidget(mode_);
  l->addLayout(status);
  row->addWidget(left, 1);

  auto* right = new Card(this);
  right->setMinimumWidth(300);
  right->setMaximumWidth(360);
  auto* r = new QVBoxLayout(right);
  r->setContentsMargins(14, 12, 14, 12);
  r->setSpacing(8);
  r->addWidget(make_label(tr("Regions"), "section", right));
  auto* help = make_label(tr("You decide what each region is. Only regions marked “Use” are turned into 3D."), "secondary", right);
  help->setWordWrap(true);
  r->addWidget(help);
  suggest_ = make_button(tr("Suggest Regions"), nullptr, icon(Icon::Regions), right);
  suggest_->setObjectName("segSuggest");
  suggest_->setToolTip(tr("Proposes regions for you to keep, label or delete (shapes only — nothing is detected by name)."));
  r->addWidget(suggest_);
  list_ = new QWidget(right);
  list_layout_ = new QVBoxLayout(list_);
  list_layout_->setContentsMargins(0, 0, 0, 0);
  list_layout_->setSpacing(4);
  auto* scroll = new QScrollArea(right);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidget(list_);
  r->addWidget(scroll, 1);
  auto* ops = new QHBoxLayout;
  merge_ = make_button(tr("Merge"), "ghost", {}, right);
  merge_->setObjectName("segMerge");
  merge_->setToolTip(tr("Merge the checked regions (Ctrl+click rows to select several)."));
  split_ = make_button(tr("Split"), "ghost", {}, right);
  split_->setObjectName("segSplit");
  split_->setToolTip(tr("Split the selected region into its separate pieces."));
  ops->addWidget(merge_);
  ops->addWidget(split_);
  ops->addStretch(1);
  r->addLayout(ops);
  summary_ = make_label(QString(), "secondary", right);
  summary_->setObjectName("segSummary");
  r->addWidget(summary_);
  notice_label_ = make_label(QString(), "secondary", right);
  notice_label_->setWordWrap(true);
  theme::set_prop(notice_label_, "tone", "review");
  notice_label_->hide();
  r->addWidget(notice_label_);
  row->addWidget(right);

  // Undo / redo: buttons, and Ctrl+Z, Ctrl+Shift+Z, Ctrl+Y inside the editor.
  auto act = [this](const QList<QKeySequence>& keys, bool redo) {
    auto* a = new QAction(this);
    a->setShortcuts(keys);
    a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(a, &QAction::triggered, this, [this, redo] {
      if (!masks_) return;
      redo ? masks_->undo().redo() : masks_->undo().undo();
    });
    addAction(a);
  };
  act({QKeySequence::Undo}, false);
  act({QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), QKeySequence(Qt::CTRL | Qt::Key_Y)}, true);
  connect(undo_, &QPushButton::clicked, this, [this] { if (masks_) masks_->undo().undo(); });
  connect(redo_, &QPushButton::clicked, this, [this] { if (masks_) masks_->undo().redo(); });
  connect(suggest_, &QPushButton::clicked, this, &SegmentEditor::suggestRegions);
  connect(mode_, &QComboBox::currentIndexChanged, this, [this](int) { updateStatus(); });
  connect(merge_, &QPushButton::clicked, this, [this] {
    QList<int> ids;
    if (current_) ids << current_;
    for (int id : selected_)
      if (id != current_) ids << id;
    if (ids.size() < 2) {
      setNotice(tr("Ctrl+click a second region in the list to merge it into the selected one."));
      return;
    }
    selected_.clear();
    if (masks_) masks_->merge(ids);
  });
  connect(split_, &QPushButton::clicked, this, [this] {
    if (masks_ && masks_->region(current_) && masks_->split(current_).isEmpty()) setNotice(tr("This region is already one piece."));
  });
  connect(&job_, &QFutureWatcher<QList<QImage>>::finished, this, [this] {
    spinner_->stop();
    QList<QImage> result;
    try {
      result = job_.result();
    } catch (const std::exception& e) {
      status_ = QString::fromUtf8(e.what());
    }
    if (apply_ && masks_) apply_(result);
    apply_ = nullptr;
    if (engine_ && engine_->ready()) {
      const auto m = engine_->metrics();
      emit engineMetrics(tr("SAM 2.1-tiny (CPU, %1 threads): load %2 s · picture analysis %3 s · last click %4 ms · +%5 MiB RAM")
                             .arg(m.threads).arg(m.load_ms / 1000.0, 0, 'f', 1).arg(m.encode_ms / 1000.0, 0, 'f', 1)
                             .arg(m.decode_ms, 0, 'f', 0).arg(m.rss_delta_bytes / 1048576.0, 0, 'f', 0));
    }
    updateStatus();
  });
  updateStatus();
}

SegmentEditor::~SegmentEditor() { job_.waitForFinished(); }

void SegmentEditor::setImage(const QImage& original, const QString& sha256) {
  job_.waitForFinished();
  original_size_ = original.size();
  sha_ = sha256;
  const QSize ws = p3dseg::working_size(original.size());
  working_ = original.convertToFormat(QImage::Format_ARGB32).scaled(ws, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  // Pixel art (few colours, hard edges): colour regions work, SAM does not (1.3.0 benchmark).
  QSet<QRgb> colours;
  for (int y = 0; y < working_.height() && colours.size() <= 64; y += std::max(1, working_.height() / 64))
    for (int x = 0; x < working_.width() && colours.size() <= 64; x += std::max(1, working_.width() / 64)) colours.insert(working_.pixel(x, y));
  crisp_ = colours.size() <= 32 && std::max(original.width(), original.height()) <= 512;
  mode_->setCurrentIndex(crisp_ ? 1 : 0);
  encoded_ = false;
  current_ = 0;
  notice_.clear();
  notice_label_->hide();
  setMasks(std::make_unique<MaskSet>(ws));
  canvas_->imageChanged();
}

void SegmentEditor::setMasks(std::unique_ptr<MaskSet> masks) {
  if (masks && masks->size() != p3dseg::working_size(original_size_)) return;
  masks_ = std::move(masks);
  if (!masks_) return;
  connect(masks_.get(), &MaskSet::changed, this, [this] {
    if (masks_ && !masks_->region(current_)) current_ = masks_->regions().isEmpty() ? 0 : masks_->regions().back().id;
    // Brush strokes change pixels only: the list is rebuilt only when what it shows changed.
    QString sig = QString::number(current_);
    for (const Region& r : masks_->regions()) sig += QStringLiteral("|%1,%2,%3,%4").arg(r.id).arg(r.name, r.label).arg(r.include);
    if (sig != list_signature_) rebuildList();
    else updateStatus();
    canvas_->masksChanged();
    emit regionsChanged();
  });
  connect(&masks_->undo(), &QUndoStack::canUndoChanged, undo_, &QPushButton::setEnabled);
  connect(&masks_->undo(), &QUndoStack::canRedoChanged, redo_, &QPushButton::setEnabled);
  undo_->setEnabled(masks_->undo().canUndo());
  redo_->setEnabled(masks_->undo().canRedo());
  current_ = masks_->regions().isEmpty() ? 0 : masks_->regions().front().id;
  rebuildList();
  canvas_->masksChanged();
}

bool SegmentEditor::hasIncludedRegions() const {
  if (!masks_) return false;
  return std::any_of(masks_->regions().begin(), masks_->regions().end(), [](const Region& r) { return r.include && count_inside(r.mask) > 0; });
}

void SegmentEditor::setTool(Tool t) {
  tool_ = t;
  QToolButton* b = t == Tool::Click ? click_ : t == Tool::Box ? box_ : t == Tool::Brush ? brush_ : eraser_;
  b->setChecked(true);
  canvas_->setCursor(t == Tool::Brush || t == Tool::Eraser ? Qt::BlankCursor : Qt::CrossCursor);
  canvas_->update();
}

void SegmentEditor::setCurrentRegion(int id) {
  current_ = id;
  rebuildList();
  canvas_->masksChanged();
}

void SegmentEditor::setEngineEnabled(bool on) {
  engine_enabled_ = on;
  mode_->setEnabled(on);
  if (!on) mode_->setCurrentIndex(1);
  updateStatus();
}

void SegmentEditor::setSamPaths(const QString& dir, const QString& ortDll) {
  if (dir == sam_dir_ && ortDll == ort_dll_) return;
  releaseEngine();
  sam_dir_ = dir;
  ort_dll_ = ortDll;
  engine_failed_ = false;
  updateStatus();
}

void SegmentEditor::releaseEngine() {
  job_.waitForFinished();
  engine_.reset();
  encoded_ = false;
  updateStatus();
}

QString SegmentEditor::statusText() const { return status_label_->text(); }

void SegmentEditor::setNotice(const QString& text) {
  notice_ = text;
  notice_label_->setText(text);
  notice_label_->setVisible(!text.isEmpty());
}

void SegmentEditor::updateStatus() {
  const bool sam = engine_enabled_ && !engine_failed_ && mode_->currentIndex() == 0 && !sam_dir_.isEmpty();
  QString s;
  if (job_.isRunning()) s = status_;
  else if (!status_.isEmpty() && engine_failed_) s = status_;
  else if (working_.isNull()) s = tr("Import a picture first.");
  else if (sam) s = encoded_ ? tr("Smart selection (SAM 2.1-tiny, CPU): click a part of the picture.")
                             : tr("Smart selection (SAM 2.1-tiny, CPU): the first click analyses the picture (a few seconds).");
  else s = tr("Colour regions: click an area of one colour.") + (engine_failed_ ? QStringLiteral(" ") + status_ : QString());
  status_label_->setText(s);
  if (masks_) {
    int used = 0;
    for (const Region& r : masks_->regions()) used += r.include;
    summary_->setText(masks_->regions().isEmpty() ? tr("No regions yet — the automatic cutout will be used.")
                                                  : tr("%1 of %2 regions used for 3D").arg(used).arg(masks_->regions().size()));
  }
  const bool idle = !job_.isRunning() && !working_.isNull();
  suggest_->setEnabled(idle);
  split_->setEnabled(masks_ && masks_->region(current_));
}

void SegmentEditor::ensureEngine() {
  if (!engine_) engine_ = std::make_unique<p3dseg::SamEngine>();
}

QImage SegmentEditor::colourAt(QPointF p) const { return p3dseg::color_region_at(working_, p.toPoint(), 28); }

void SegmentEditor::runJob(std::function<QList<QImage>(p3dseg::SamEngine*)> sam, std::function<QList<QImage>()> colour,
                           std::function<void(const QList<QImage>&)> apply, const QString& busy) {
  if (job_.isRunning() || working_.isNull() || !masks_) {
    setNotice(tr("Still working on the previous step — one moment."));
    return;
  }
  setNotice(QString());
  const bool use_sam = engine_enabled_ && !engine_failed_ && mode_->currentIndex() == 0 && !sam_dir_.isEmpty();
  apply_ = std::move(apply);
  if (!use_sam) {
    status_.clear();
    job_.setFuture(QtConcurrent::run([colour] { return colour(); }));
    spinner_->start();
    return;
  }
  ensureEngine();
  p3dseg::SamEngine* e = engine_.get();
  const QString dir = sam_dir_, dll = ort_dll_;
  const QImage img = working_;
  const bool first = !e->ready(), encode = !encoded_;
  status_ = first ? tr("Loading SAM and analysing the picture (about 10 s, once)…") : encode ? tr("Analysing the picture (a few seconds, once)…") : busy;
  spinner_->start();
  encoded_ = true;
  job_.setFuture(QtConcurrent::run([this, e, dir, dll, img, sam, colour]() -> QList<QImage> {
    QString error;
    if (!e->ready() && !e->load(dir, dll, &error)) {
      QMetaObject::invokeMethod(this, [this, error] { engine_failed_ = true; status_ = error + QStringLiteral(" ") + tr("Using colour regions."); }, Qt::QueuedConnection);
      return colour();
    }
    if (!e->hasImage() && !e->setImage(img, &error)) throw std::runtime_error(error.toStdString());
    return sam(e);
  }));
  updateStatus();
}

void SegmentEditor::clickAt(QPointF p, ClickMode mode) {
  if (working_.isNull() || !QRectF(QPointF(0, 0), QSizeF(working_.size())).contains(p)) return;
  if (mode != ClickMode::NewRegion && !masks_->region(current_)) mode = ClickMode::NewRegion;
  runJob([p](p3dseg::SamEngine* e) { return QList<QImage>{p3dseg::clean_mask(e->predict({p}, {}, std::nullopt))}; },
         [this, p] { return QList<QImage>{colourAt(p)}; },
         [this, mode](const QList<QImage>& r) {
           if (r.isEmpty() || r.front().isNull() || count_inside(r.front()) == 0) {
             setNotice(tr("Nothing to select there. Try another spot, or the Box tool."));
             return;
           }
           if (mode == ClickMode::NewRegion) {
             setCurrentRegion(masks_->add(r.front(), tr("Object")));
           } else if (const Region* cur = masks_->region(current_)) {
             masks_->setMask(current_, combine(cur->mask, r.front(), mode == ClickMode::SubtractFromCurrent),
                             mode == ClickMode::SubtractFromCurrent ? tr("Remove from region") : tr("Add to region"));
           }
         },
         tr("Selecting…"));
}

void SegmentEditor::boxAt(QRectF r) {
  const QRectF box = r.intersected(QRectF(QPointF(0, 0), QSizeF(working_.size())));
  if (box.isEmpty()) return;
  runJob([box](p3dseg::SamEngine* e) { return QList<QImage>{p3dseg::clean_mask(e->predict({}, {}, box))}; },
         [this, box] {
           QImage m(masks_->size(), QImage::Format_Grayscale8);
           m.fill(0);
           QPainter p(&m);
           p.fillRect(box, QColor(255, 255, 255));
           return QList<QImage>{m};
         },
         [this](const QList<QImage>& res) {
           if (!res.isEmpty() && !res.front().isNull() && count_inside(res.front()) > 0) setCurrentRegion(masks_->add(res.front(), tr("Object")));
         },
         tr("Selecting…"));
}

void SegmentEditor::suggestRegions() {
  const QSize s = working_.size();
  const long total = long(s.width()) * s.height();
  QList<QImage> existing;
  for (const Region& r : masks_->regions()) existing << r.mask;
  auto filter = [total, existing](QList<QImage> found) {
    QList<QImage> kept;
    std::sort(found.begin(), found.end(), [](const QImage& a, const QImage& b) { return count_inside(a) > count_inside(b); });
    for (const QImage& m : found) {
      const long n = count_inside(m);
      if (n < total / 300 || n > total * 85 / 100) continue;  // specks and the background
      bool dup = false;
      for (const QImage& k : kept) dup |= mask_iou(k, m) > 0.6;
      for (const QImage& k : existing) dup |= mask_iou(k, m) > 0.6;
      if (!dup) kept << m;
      if (kept.size() >= 12) break;
    }
    return kept;
  };
  runJob(
      [s, filter](p3dseg::SamEngine* e) {
        QList<QImage> found;
        constexpr int grid = 8;  // 64 clicks, about 6 s on a laptop CPU
        for (int gy = 0; gy < grid; ++gy)
          for (int gx = 0; gx < grid; ++gx)
            found << p3dseg::clean_mask(e->predict({QPointF((gx + 0.5) * s.width() / grid, (gy + 0.5) * s.height() / grid)}, {}, std::nullopt));
        return filter(found);
      },
      [this, total, filter] { return filter(p3dseg::suggest_color_regions(working_, int(total / 300))); },
      [this](const QList<QImage>& res) {
        if (res.isEmpty()) {
          setNotice(tr("No new regions found. Click the parts you want instead."));
          return;
        }
        const auto ids = masks_->addMany(res, tr("Object"), tr("Suggestion"));
        if (!ids.isEmpty()) setCurrentRegion(ids.front());
      },
      tr("Looking for regions (about 6 s)…"));
}

bool SegmentEditor::eventFilter(QObject* o, QEvent* e) {
  if (e->type() == QEvent::MouseButtonPress && o->objectName() == QLatin1String("segRow")) {
    const int id = o->property("regionId").toInt();
    if (static_cast<QMouseEvent*>(e)->modifiers() & Qt::ControlModifier) {
      if (!selected_.remove(id)) selected_.insert(id);
      if (current_ && current_ != id) selected_.insert(current_);
      rebuildList();
      canvas_->masksChanged();
    } else {
      selected_.clear();
      setCurrentRegion(id);
    }
    return true;
  }
  return QWidget::eventFilter(o, e);
}

void SegmentEditor::keyPressEvent(QKeyEvent* e) {
  // The same keys as the editor's shortcuts, for when no shortcut consumed them.
  if (masks_ && e->matches(QKeySequence::Undo)) {
    masks_->undo().undo();
    return;
  }
  if (masks_ && ((e->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier) && e->key() == Qt::Key_Z) ||
                 (e->modifiers() == Qt::ControlModifier && e->key() == Qt::Key_Y))) {
    masks_->undo().redo();
    return;
  }
  if (e->key() == Qt::Key_BracketLeft) brush_size_->setValue(brush_size_->value() - 2);
  else if (e->key() == Qt::Key_BracketRight) brush_size_->setValue(brush_size_->value() + 2);
  else QWidget::keyPressEvent(e);
}

void SegmentEditor::rebuildList() {
  list_signature_ = QString::number(current_);
  if (masks_)
    for (const Region& r : masks_->regions()) list_signature_ += QStringLiteral("|%1,%2,%3,%4").arg(r.id).arg(r.name, r.label).arg(r.include);
  while (auto* item = list_layout_->takeAt(0)) {
    delete item->widget();
    delete item;
  }
  if (!masks_) return;
  for (const Region& r : masks_->regions()) {
    auto* rowW = new QFrame(list_);
    rowW->setObjectName("segRow");
    rowW->setProperty("regionId", r.id);
    rowW->setProperty("selected", r.id == current_ || selected_.contains(r.id));
    rowW->setProperty("card", r.id == current_ || selected_.contains(r.id));
    rowW->setCursor(Qt::PointingHandCursor);
    auto* h = new QHBoxLayout(rowW);
    h->setContentsMargins(8, 6, 8, 6);
    h->setSpacing(6);
    auto* swatch = new QLabel(rowW);
    swatch->setFixedSize(12, 12);
    swatch->setStyleSheet(QStringLiteral("background:%1;border-radius:3px;").arg(r.color.name()));
    h->addWidget(swatch);
    auto* col = new QVBoxLayout;
    col->setSpacing(2);
    auto* name = new QLineEdit(r.name, rowW);
    name->setObjectName("segName");
    name->setToolTip(tr("Name of this region"));
    auto* label = new QComboBox(rowW);
    label->setObjectName("segLabel");
    label->setEditable(true);
    label->addItems(p3dseg::standard_labels());
    label->setCurrentText(r.label);
    label->setToolTip(tr("What this region is (your label)"));
    col->addWidget(name);
    col->addWidget(label);
    h->addLayout(col, 1);
    auto* use = new QCheckBox(tr("Use"), rowW);
    use->setObjectName("segUse");
    use->setChecked(r.include);
    use->setToolTip(tr("Turn this region into 3D"));
    h->addWidget(use);
    auto* del = new QToolButton(rowW);
    del->setIcon(icon(Icon::Close));
    del->setToolTip(tr("Delete region (undoable)"));
    h->addWidget(del);
    list_layout_->addWidget(rowW);
    const int id = r.id;
    connect(name, &QLineEdit::editingFinished, this, [this, id, name] { if (masks_) masks_->setName(id, name->text()); });
    // A label is committed when chosen from the list or when typing is finished — not per keystroke.
    connect(label, &QComboBox::activated, this, [this, id, label](int) {
      const QString t = label->currentText();
      QTimer::singleShot(0, this, [this, id, t] { if (masks_) masks_->setLabel(id, t); });
    });
    connect(label->lineEdit(), &QLineEdit::editingFinished, this, [this, id, label] {
      const QString t = label->currentText().trimmed();
      if (!t.isEmpty()) QTimer::singleShot(0, this, [this, id, t] { if (masks_) masks_->setLabel(id, t); });
    });
    connect(use, &QCheckBox::toggled, this, [this, id](bool on) {
      QTimer::singleShot(0, this, [this, id, on] { if (masks_) masks_->setInclude(id, on); });
    });
    connect(del, &QToolButton::clicked, this, [this, id] { QTimer::singleShot(0, this, [this, id] { if (masks_) masks_->remove(id); }); });
    rowW->installEventFilter(this);
  }
  list_layout_->addStretch(1);
  updateStatus();
}

}  // namespace p3dgui
