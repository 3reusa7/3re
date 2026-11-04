#pragma once
// The regions a user marks on a picture (spec 2026-10-20-3re-design.md §7). Regions
// are user-labelled masks — not semantic detection. Every edit is one undo step
// (a brush stroke too); masks live at a bounded working size (longest side <= 2048).
#include <QColor>
#include <QImage>
#include <QList>
#include <QObject>
#include <QString>
#include <QUndoStack>

namespace p3dseg {

struct Region {
  int id = 0;
  QString name, label;
  QColor color;
  bool include = true, visible = true;
  QImage mask;  // Grayscale8, working size; > 127 = inside
};

QSize working_size(QSize original);
// The labels offered in the editor (users may type their own).
const QStringList& standard_labels();

class MaskSet : public QObject {
  Q_OBJECT
 public:
  explicit MaskSet(QSize workingSize, QObject* parent = nullptr);
  QSize size() const { return size_; }
  QUndoStack& undo() { return undo_; }
  const QList<Region>& regions() const { return regions_; }
  const Region* region(int id) const;

  int add(const QImage& mask, const QString& label);
  QList<int> addMany(const QList<QImage>& masks, const QString& label, const QString& namePrefix);  // one undo step
  void setMask(int id, const QImage& mask, const QString& undoText);  // replace a region's mask (one step)
  void beginStroke(int id);
  void paint(int id, QPoint centre, int radius, bool erase);  // inside a stroke: live; outside: its own step
  void endStroke();
  bool stroking() const { return stroke_id_ != 0; }
  void merge(QList<int> ids);  // into the first id
  QList<int> split(int id);    // connected parts; the first keeps the id; returns the new ids
  void setLabel(int id, const QString& label);
  void setName(int id, const QString& name);
  void setInclude(int id, bool include);
  void setVisible(int id, bool visible);
  void remove(int id);
  QImage unionMask(bool includedOnly) const;

  // Used by the store: replaces everything without history.
  void restore(QList<Region> regions, int nextId);
  int nextId() const { return next_id_; }

 signals:
  void changed();

 private:
  friend class SnapshotCommand;
  void apply(const QList<Region>& state);
  void commit(const QString& text, QList<Region> after, const QList<Region>* before = nullptr);
  Region* find(QList<Region>& list, int id);
  QColor nextColor() const;

  QSize size_;
  QList<Region> regions_;
  QUndoStack undo_;
  int next_id_ = 1;
  int stroke_id_ = 0;
  QList<Region> stroke_before_;
};

}  // namespace p3dseg
