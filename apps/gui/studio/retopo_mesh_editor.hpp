#pragma once
#include <QWidget>
#include <memory>
#include "backends/retopo_engine.hpp"
#include "studio/retopo_edit_document.hpp"

class QComboBox;
class QLabel;
class QPushButton;
namespace p3dgui {
class Viewport;
// Geometry editing component. Material-preserving import/export is owned by the
// Retopology workspace; this component never replaces an asset on disk.
class RetopoMeshEditor : public QWidget {
  Q_OBJECT
 public:
  explicit RetopoMeshEditor(bool software, QWidget* parent = nullptr);
  void setMesh(const pixel3d::Mesh& mesh);
  void setMesh(std::vector<pixel3d::vec3d> positions, std::vector<RetopoEditDocument::Face> faces);
  // QuadWild result: quads and triangles as they are, larger polygons fanned into triangles.
  void setMesh(const p3dasset::retopo::PolyMesh& mesh);
  // The model the result was made from, shown by "Original model" in the same frame (textured when
  // `appearance` is set). Display only: never edited.
  void setReference(std::shared_ptr<const pixel3d::Mesh> geometry, std::shared_ptr<const pixel3d::TexturedMesh> appearance);
  void showOriginal(bool original);
  // The GLB the result was saved to, read back (new UVs, the original's colours): shown by "Saved with
  // texture" until the mesh is edited again (then the file no longer matches and the view goes away).
  void setSaved(std::shared_ptr<const pixel3d::TexturedMesh> saved);
  bool showingSaved() const;
  bool hasMesh() const { return !document_.positions().empty(); }
  void clear();
  const RetopoEditDocument& document() const { return document_; }
  Viewport& viewport() { return *view_; }
 signals:
  void edited();
 protected:
  bool eventFilter(QObject* object, QEvent* event) override;
 private:
  void selectAt(QPoint point);
  void draw(const RetopoEditDocument& document);
  void drawReference();
  void drawSaved();
  void clearSaved();
  std::shared_ptr<std::vector<float>> polygonEdges(const std::vector<pixel3d::vec3d>& placed) const;
  void frame(const std::vector<pixel3d::vec3d>& positions);
  void refresh();
  bool transformDrag(RetopoEditDocument& document, int dx);
  RetopoEditDocument document_, original_, preview_;
  Viewport* view_;
  QComboBox *selection_, *tool_, *axis_, *comparison_;
  QPushButton *undo_, *redo_, *apply_, *revert_;
  QLabel* status_;
  std::shared_ptr<const pixel3d::Mesh> reference_;
  std::shared_ptr<const pixel3d::TexturedMesh> reference_look_, saved_;
  std::set<uint32_t> selected_;
  std::vector<size_t> triangle_faces_;
  pixel3d::vec3d center_{}, mid_{};
  double display_scale_ = 1, diagonal_ = 1;
  int width_ = 100, height_ = 100, depth_ = 100;
  bool dragging_ = false;
  QPoint press_;
  uint64_t revision_ = 0;
};
}
