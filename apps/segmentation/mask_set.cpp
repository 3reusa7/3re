#include "segmentation/mask_set.hpp"

#include <QPainter>
#include <QUndoCommand>
#include <algorithm>
#include <cmath>
#include <vector>

namespace p3dseg {

QSize working_size(QSize original) {
  const int longest = std::max(original.width(), original.height());
  if (longest <= 2048 || longest <= 0) return original;
  const double s = 2048.0 / longest;
  return QSize(std::max(1, int(std::lround(original.width() * s))), std::max(1, int(std::lround(original.height() * s))));
}

const QStringList& standard_labels() {
  static const QStringList l{QObject::tr("Body"), QObject::tr("Head"), QObject::tr("Hair"), QObject::tr("Clothing"),
                             QObject::tr("Armor"), QObject::tr("Weapon"), QObject::tr("Accessory"), QObject::tr("Object"),
                             QObject::tr("Background")};
  return l;
}

// One undoable edit: the whole region list before and after. Masks are implicitly
// shared, so only the masks an edit changed cost memory.
class SnapshotCommand : public QUndoCommand {
 public:
  SnapshotCommand(MaskSet* set, const QString& text, QList<Region> before, QList<Region> after)
      : QUndoCommand(text), set_(set), before_(std::move(before)), after_(std::move(after)) {}
  void undo() override { set_->apply(before_); }
  void redo() override { set_->apply(after_); }

 private:
  MaskSet* set_;
  QList<Region> before_, after_;
};

MaskSet::MaskSet(QSize workingSize, QObject* parent) : QObject(parent), size_(workingSize) { undo_.setUndoLimit(64); }

const Region* MaskSet::region(int id) const {
  for (const Region& r : regions_)
    if (r.id == id) return &r;
  return nullptr;
}

Region* MaskSet::find(QList<Region>& list, int id) {
  for (Region& r : list)
    if (r.id == id) return &r;
  return nullptr;
}

QColor MaskSet::nextColor() const {
  // Muted, distinguishable overlay colours (theme-calm, readable on dark and light pictures).
  static const QColor palette[] = {QColor(0x7c, 0x93, 0xc4), QColor(0x5f, 0xae, 0x7a), QColor(0xd3, 0xa3, 0x5a), QColor(0xc0, 0x6f, 0x9a),
                                   QColor(0x6f, 0xa8, 0xb8), QColor(0xb5, 0x8c, 0xd8), QColor(0xd0, 0x6a, 0x64), QColor(0x9b, 0xb0, 0x5a),
                                   QColor(0xe0, 0x96, 0x6a), QColor(0x8a, 0x9a, 0xb0)};
  return palette[(next_id_ - 1) % 10];
}

void MaskSet::apply(const QList<Region>& state) {
  regions_ = state;
  emit changed();
}

void MaskSet::commit(const QString& text, QList<Region> after, const QList<Region>* before) {
  undo_.push(new SnapshotCommand(this, text, before ? *before : regions_, std::move(after)));
}

int MaskSet::add(const QImage& mask, const QString& label) {
  Region r;
  r.id = next_id_;
  r.label = label;
  r.name = label;
  r.color = nextColor();
  r.mask = (mask.size() == size_ ? mask : mask.scaled(size_, Qt::IgnoreAspectRatio, Qt::FastTransformation)).convertToFormat(QImage::Format_Grayscale8);
  ++next_id_;
  auto after = regions_;
  after.append(r);
  commit(tr("Add %1").arg(label), after);
  return r.id;
}

QList<int> MaskSet::addMany(const QList<QImage>& masks, const QString& label, const QString& namePrefix) {
  auto after = regions_;
  QList<int> ids;
  for (const QImage& mask : masks) {
    Region r;
    r.id = next_id_;
    r.label = label;
    r.name = QStringLiteral("%1 %2").arg(namePrefix).arg(ids.size() + 1);
    r.color = nextColor();
    r.mask = (mask.size() == size_ ? mask : mask.scaled(size_, Qt::IgnoreAspectRatio, Qt::FastTransformation)).convertToFormat(QImage::Format_Grayscale8);
    ++next_id_;
    after.append(r);
    ids << r.id;
  }
  if (!ids.isEmpty()) commit(tr("Add %1 suggested regions").arg(ids.size()), after);
  return ids;
}

void MaskSet::setMask(int id, const QImage& mask, const QString& undoText) {
  auto after = regions_;
  Region* r = find(after, id);
  if (!r) return;
  r->mask = (mask.size() == size_ ? mask : mask.scaled(size_, Qt::IgnoreAspectRatio, Qt::FastTransformation)).convertToFormat(QImage::Format_Grayscale8);
  commit(undoText, after);
}

void MaskSet::beginStroke(int id) {
  if (!region(id)) return;
  stroke_id_ = id;
  stroke_before_ = regions_;
}

void MaskSet::paint(int id, QPoint c, int radius, bool erase) {
  Region* r = find(regions_, id);
  if (!r) return;
  const bool single = stroke_id_ != id;
  const QList<Region> before = single ? regions_ : QList<Region>();
  QImage& m = r->mask;  // detaches from the snapshot copy on the first write
  QPainter p(&m);
  p.setRenderHint(QPainter::Antialiasing, false);
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(erase ? 0 : 255, erase ? 0 : 255, erase ? 0 : 255));
  p.drawEllipse(c, radius, radius);
  p.end();
  if (single) {
    const auto after = regions_;
    regions_ = before;
    commit(erase ? tr("Erase") : tr("Brush"), after);
  } else {
    emit changed();
  }
}

void MaskSet::endStroke() {
  if (!stroke_id_) return;
  stroke_id_ = 0;
  const auto after = regions_;
  const auto before = stroke_before_;
  stroke_before_.clear();
  // The live strokes are already applied: record them as one step without replaying twice.
  commit(tr("Brush stroke"), after, &before);
}

void MaskSet::merge(QList<int> ids) {
  ids.erase(std::remove_if(ids.begin(), ids.end(), [this](int id) { return !region(id); }), ids.end());
  if (ids.size() < 2) return;
  auto after = regions_;
  Region* keep = find(after, ids.front());
  QImage m = keep->mask;
  for (int i = 1; i < ids.size(); ++i) {
    const Region* other = region(ids[i]);
    for (int y = 0; y < size_.height(); ++y) {
      uchar* d = m.scanLine(y);
      const uchar* s = other->mask.constScanLine(y);
      for (int x = 0; x < size_.width(); ++x) d[x] = std::max(d[x], s[x]);
    }
  }
  keep->mask = m;
  after.erase(std::remove_if(after.begin(), after.end(), [&](const Region& r) { return r.id != ids.front() && ids.contains(r.id); }), after.end());
  commit(tr("Merge regions"), after);
}

QList<int> MaskSet::split(int id) {
  const Region* src = region(id);
  if (!src) return {};
  const int w = size_.width(), h = size_.height();
  std::vector<int> label(size_t(w) * h, 0);
  int count = 0;
  std::vector<int> stack;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      if (src->mask.constScanLine(y)[x] <= 127 || label[size_t(y) * w + x]) continue;
      ++count;
      stack.assign(1, y * w + x);
      label[size_t(y) * w + x] = count;
      while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        const int px = i % w, py = i / w;
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx) {
            const int nx = px + dx, ny = py + dy;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            const size_t j = size_t(ny) * w + nx;
            if (!label[j] && src->mask.constScanLine(ny)[nx] > 127) {
              label[j] = count;
              stack.push_back(int(j));
            }
          }
      }
    }
  if (count < 2) return {};
  auto after = regions_;
  QList<int> created;
  for (int c = 1; c <= count; ++c) {
    QImage m(size_, QImage::Format_Grayscale8);
    m.fill(0);
    for (int y = 0; y < h; ++y) {
      uchar* d = m.scanLine(y);
      for (int x = 0; x < w; ++x) d[x] = label[size_t(y) * w + x] == c ? 255 : 0;
    }
    if (c == 1) {
      find(after, id)->mask = m;
      continue;
    }
    Region r = *src;
    r.id = next_id_;
    r.name = QStringLiteral("%1 %2").arg(src->name).arg(c);
    r.color = nextColor();
    r.mask = m;
    ++next_id_;
    after.append(r);
    created << r.id;
  }
  commit(tr("Split region"), after);
  return created;
}

void MaskSet::setLabel(int id, const QString& label) {
  auto after = regions_;
  if (Region* r = find(after, id); r && r->label != label) {
    r->label = label;
    commit(tr("Label %1").arg(label), after);
  }
}

void MaskSet::setName(int id, const QString& name) {
  auto after = regions_;
  if (Region* r = find(after, id); r && r->name != name) {
    r->name = name;
    commit(tr("Rename"), after);
  }
}

void MaskSet::setInclude(int id, bool include) {
  auto after = regions_;
  if (Region* r = find(after, id); r && r->include != include) {
    r->include = include;
    commit(include ? tr("Include region") : tr("Exclude region"), after);
  }
}

void MaskSet::setVisible(int id, bool visible) {
  // Display only: not an edit, no undo step.
  if (Region* r = find(regions_, id); r && r->visible != visible) {
    r->visible = visible;
    emit changed();
  }
}

void MaskSet::remove(int id) {
  auto after = regions_;
  const auto n = after.size();
  after.erase(std::remove_if(after.begin(), after.end(), [id](const Region& r) { return r.id == id; }), after.end());
  if (after.size() != n) commit(tr("Delete region"), after);
}

QImage MaskSet::unionMask(bool includedOnly) const {
  QImage u(size_, QImage::Format_Grayscale8);
  u.fill(0);
  for (const Region& r : regions_) {
    if (includedOnly && !r.include) continue;
    for (int y = 0; y < size_.height(); ++y) {
      uchar* d = u.scanLine(y);
      const uchar* s = r.mask.constScanLine(y);
      for (int x = 0; x < size_.width(); ++x) d[x] = std::max(d[x], s[x]);
    }
  }
  return u;
}

void MaskSet::restore(QList<Region> regions, int nextId) {
  undo_.clear();
  regions_ = std::move(regions);
  next_id_ = nextId;
  emit changed();
}

}  // namespace p3dseg
