#include "studio/retopo_mesh_editor.hpp"
#include <QApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <limits>
#include "studio/preview_bar.hpp"
#include "viewport.hpp"

namespace p3dgui {
RetopoMeshEditor::RetopoMeshEditor(bool software, QWidget* parent) : QWidget(parent) {
  setObjectName("retopoMeshEditor");
  auto* root = new QVBoxLayout(this);
  auto* tools = new QHBoxLayout;
  selection_ = new QComboBox; selection_->addItems({tr("Vertices"), tr("Edges"), tr("Faces")});
  selection_->setObjectName("retopoSelection");
  tool_ = new QComboBox; tool_->addItems({tr("Select"), tr("Move"), tr("Rotate"), tr("Scale")});
  tool_->setObjectName("retopoTransform");
  axis_ = new QComboBox; axis_->addItems({"X", "Y", "Z"});
  comparison_ = new QComboBox; comparison_->addItems({tr("Retopology"), tr("Original model")});
  comparison_->setToolTip(tr("Before/after: the new quad mesh (editable) or the model it was made from"));
  comparison_->setObjectName("retopoComparison");
  comparison_->setSizeAdjustPolicy(QComboBox::AdjustToContents);  // "Saved with texture" appears later
  for (auto* c : {selection_, tool_, axis_, comparison_}) tools->addWidget(c);
  tools->addStretch(); root->addLayout(tools);
  view_ = new Viewport(software, this);
  view_->setObjectName("retopoEditViewport");
  root->addWidget(view_, 1);
  root->addWidget(new PreviewBar(view_, this, true));
  auto* history = new QHBoxLayout;
  undo_ = new QPushButton(tr("Undo")); redo_ = new QPushButton(tr("Redo"));
  apply_ = new QPushButton(tr("Apply")); revert_ = new QPushButton(tr("Revert"));
  undo_->setObjectName("retopoUndo"); redo_->setObjectName("retopoRedo");
  apply_->setObjectName("retopoApply"); revert_->setObjectName("retopoRevert");
  for (auto* b : {undo_, redo_, apply_, revert_}) history->addWidget(b);
  history->addStretch(); root->addLayout(history);
  status_ = new QLabel(tr("Load a mesh. Edits are local and never modify the original."));
  status_->setWordWrap(true); root->addWidget(status_);
  connect(view_, &Viewport::clickedAt, this, &RetopoMeshEditor::selectAt);
  connect(undo_, &QPushButton::clicked, this, [this] { if (document_.undo()) { clearSaved(); refresh(); emit edited(); } });
  connect(redo_, &QPushButton::clicked, this, [this] { if (document_.redo()) { clearSaved(); refresh(); emit edited(); } });
  connect(apply_, &QPushButton::clicked, this, [this] { document_.apply(); refresh(); });
  connect(revert_, &QPushButton::clicked, this, [this] { document_.revert(); clearSaved(); refresh(); emit edited(); });
  connect(comparison_, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
  connect(selection_, &QComboBox::currentIndexChanged, this, [this] { selected_.clear(); refresh(); });
  view_->surface()->installEventFilter(this);
  view_->surface()->setFocusPolicy(Qt::StrongFocus);
  refresh();
}

void RetopoMeshEditor::setMesh(const pixel3d::Mesh& mesh) {
  std::vector<RetopoEditDocument::Face> faces;
  for (auto t : mesh.tris) faces.push_back({t[0], t[1], t[2]});
  setMesh(mesh.positions, std::move(faces));
}

void RetopoMeshEditor::setMesh(const p3dasset::retopo::PolyMesh& mesh) {
  std::vector<RetopoEditDocument::Face> faces;
  for (const auto& f : mesh.faces) {
    if (f.size() <= 4) faces.push_back(f);
    else for (size_t j = 1; j + 1 < f.size(); ++j) faces.push_back({f[0], f[j], f[j+1]});
  }
  setMesh(mesh.positions, std::move(faces));
}

void RetopoMeshEditor::setReference(std::shared_ptr<const pixel3d::Mesh> geometry, std::shared_ptr<const pixel3d::TexturedMesh> appearance) {
  reference_ = std::move(geometry); reference_look_ = std::move(appearance);
  if (!hasMesh() && reference_ && !reference_->positions.empty()) {
    frame(reference_->positions); view_->setIso();
    comparison_->setCurrentIndex(1);
  }
  refresh();
}

void RetopoMeshEditor::showOriginal(bool original) { comparison_->setCurrentIndex(original ? 1 : 0); }

void RetopoMeshEditor::setSaved(std::shared_ptr<const pixel3d::TexturedMesh> saved) {
  saved_ = std::move(saved);
  if (!saved_ || !saved_->texture) { clearSaved(); refresh(); return; }
  if (comparison_->count() < 3) comparison_->addItem(tr("Saved with texture"));
  if (comparison_->currentIndex() == 2) refresh();
  else comparison_->setCurrentIndex(2);
}

bool RetopoMeshEditor::showingSaved() const { return saved_ && comparison_->currentIndex() == 2; }

void RetopoMeshEditor::clearSaved() {
  saved_.reset();
  if (comparison_->count() < 3) return;
  const QSignalBlocker block(comparison_);
  if (comparison_->currentIndex() == 2) comparison_->setCurrentIndex(0);
  comparison_->removeItem(2);
}

void RetopoMeshEditor::clear() {
  document_ = {}; original_ = {}; preview_ = {}; selected_.clear(); triangle_faces_.clear();
  reference_.reset(); reference_look_.reset(); clearSaved();
  view_->setScene({}); refresh();
}

void RetopoMeshEditor::setMesh(std::vector<pixel3d::vec3d> positions, std::vector<RetopoEditDocument::Face> faces) {
  document_.load(std::move(positions), std::move(faces));
  original_ = document_; selected_.clear(); triangle_faces_.clear(); clearSaved();
  for (size_t i = 0; i < document_.faces().size(); ++i)
    for (size_t j = 2; j < document_.faces()[i].size(); ++j) triangle_faces_.push_back(i);
  frame(document_.positions());
  comparison_->setCurrentIndex(0);
  view_->setIso(); refresh();
}

void RetopoMeshEditor::frame(const std::vector<pixel3d::vec3d>& positions) {
  auto lo = positions.front(), hi = lo;
  for (auto p : positions) {
    lo = {std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z)};
    hi = {std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z)};
  }
  center_ = (lo+hi)*.5; diagonal_ = pixel3d::length(hi-lo);
  display_scale_ = 100 / std::max(diagonal_, 1e-12);
  const auto extent = (hi-lo)*display_scale_;
  width_ = int(std::ceil(extent.x*1.08))+2;
  height_ = int(std::ceil(extent.y*1.08))+2;
  depth_ = int(std::ceil(extent.z*1.08))+2;
  mid_ = {width_*.5,height_*.5,depth_*.5};
}

void RetopoMeshEditor::drawReference() {
  Scene scene; scene.dims = {width_,height_,depth_}; scene.revision = ++revision_;
  const auto place = [this](pixel3d::vec3d p) { return (p-center_)*display_scale_+mid_; };
  if (reference_look_ && reference_look_->texture) {
    auto look = std::make_shared<pixel3d::TexturedMesh>(*reference_look_);
    for (auto& p : look->positions) p = place(p);
    scene.mesh = look; scene.textured = true; scene.smooth_texture = true;
  } else if (reference_) {
    auto mesh = std::make_shared<pixel3d::Mesh>(*reference_);
    for (auto& p : mesh->positions) p = place(p);
    scene.mesh = flat_mesh(*mesh); scene.solid = mesh;
  }
  view_->setScene(scene); view_->setHighlight({});
}

void RetopoMeshEditor::drawSaved() {
  Scene scene; scene.dims = {width_,height_,depth_}; scene.revision = ++revision_;
  auto look = std::make_shared<pixel3d::TexturedMesh>(*saved_);
  for (auto& p : look->positions) p = (p-center_)*display_scale_+mid_;
  scene.mesh = look; scene.textured = true; scene.smooth_texture = true;
  // The saved vertices are the document's (split at UV seams): its quad edges fit them exactly.
  std::vector<pixel3d::vec3d> placed = document_.positions();
  for (auto& p : placed) p = (p-center_)*display_scale_+mid_;
  scene.wire_edges = polygonEdges(placed);
  view_->setScene(scene); view_->setHighlight({});
}

std::shared_ptr<std::vector<float>> RetopoMeshEditor::polygonEdges(const std::vector<pixel3d::vec3d>& placed) const {
  auto edges = std::make_shared<std::vector<float>>();
  std::set<std::pair<uint32_t,uint32_t>> seen;
  for (const auto& f : document_.faces())
    for (size_t i=0;i<f.size();++i) {
      auto a=f[i], b=f[(i+1)%f.size()];
      if (!seen.insert({std::min(a,b),std::max(a,b)}).second) continue;
      for (auto v : {a,b}) { const auto p=placed[v]; edges->insert(edges->end(),{float(p.x),float(p.y),float(p.z)}); }
    }
  return edges;
}

void RetopoMeshEditor::draw(const RetopoEditDocument& d) {
  auto mesh = std::make_shared<pixel3d::Mesh>(d.triangulated());
  for (auto& p : mesh->positions) p = (p-center_)*display_scale_+mid_;
  Scene scene; scene.dims = {width_,height_,depth_}; scene.revision = ++revision_;
  scene.mesh = flat_mesh(*mesh); scene.solid = mesh;
  // Polygon edges for the wireframe: the quads, not the triangles they are drawn with.
  scene.wire_edges = polygonEdges(mesh->positions);
  Highlight highlight;
  for (size_t t=0;t<mesh->tris.size();++t) {
    scene.tri_part.push_back(int(t)); scene.tri_region.push_back(int(triangle_faces_[t]));
    if (comparison_->currentIndex()==0)
      for (auto i : mesh->tris[t]) if (selected_.contains(i)) { highlight.parts.insert(int(t)); break; }
  }
  view_->setScene(scene); view_->setHighlight(highlight);
}

void RetopoMeshEditor::refresh() {
  const bool loaded = !document_.positions().empty(), edit = loaded && comparison_->currentIndex()==0;
  undo_->setEnabled(edit && document_.canUndo()); redo_->setEnabled(edit && document_.canRedo());
  apply_->setEnabled(edit); revert_->setEnabled(edit);
  selection_->setEnabled(edit); tool_->setEnabled(edit); axis_->setEnabled(edit);
  comparison_->setEnabled(loaded && (reference_ || saved_));
  if (comparison_->currentIndex()==1 && reference_) {
    drawReference();
    status_->setText(tr("Original model: %1 triangles. Switch to Retopology to edit the new mesh.").arg(reference_->tris.size()));
    return;
  }
  if (comparison_->currentIndex()==2 && saved_ && loaded) {
    drawSaved();
    status_->setText(tr("Saved GLB, read back from the file: %1 triangles, %2 x %3 texture with the original's colours. "
                         "Switch to Retopology to edit (the saved view goes away until you save again).")
        .arg(saved_->indices.size()/3).arg(saved_->texture->width()).arg(saved_->texture->height()));
    return;
  }
  if (!loaded) return;
  draw(document_);
  status_->setText(tr("%1 vertices · %2 faces · %3 triangles · %4 selected vertices. Shift-click adds. Drag horizontally to transform along the chosen axis. Edits require a new geometry audit.")
      .arg(document_.positions().size()).arg(document_.faces().size()).arg(triangle_faces_.size()).arg(selected_.size()));
}

void RetopoMeshEditor::selectAt(QPoint point) {
  if (comparison_->currentIndex()!=0 || tool_->currentIndex()!=0) return;
  const int tri = view_->pickAt(point);
  if (!(QApplication::keyboardModifiers() & Qt::ShiftModifier)) selected_.clear();
  if (tri>=0 && size_t(tri)<triangle_faces_.size()) {
    const auto& face = document_.faces()[triangle_faces_[size_t(tri)]];
    const auto screen = [this](uint32_t i) { return view_->project((document_.positions()[i]-center_)*display_scale_+mid_); };
    if (selection_->currentIndex()==2) selected_.insert(face.begin(),face.end());
    else {
      double best = std::numeric_limits<double>::infinity(); size_t index=0;
      for (size_t i=0;i<face.size();++i) {
        const auto a=screen(face[i]); auto nearest=a;
        if (selection_->currentIndex()==1) {
          const auto delta=screen(face[(i+1)%face.size()])-a;
          const double len=QPointF::dotProduct(delta,delta);
          if (len>0) nearest=a+delta*std::clamp(QPointF::dotProduct(QPointF(point)-a,delta)/len,0.,1.);
        }
        const auto distance=QPointF(point)-nearest;
        const double squared=QPointF::dotProduct(distance,distance);
        if(squared<best) { best=squared; index=i; }
      }
      selected_.insert(face[index]);
      if(selection_->currentIndex()==1) selected_.insert(face[(index+1)%face.size()]);
    }
  }
  refresh();
}

bool RetopoMeshEditor::transformDrag(RetopoEditDocument& d, int dx) {
  if (dx == 0) return false;
  pixel3d::vec3d axis = axis_->currentIndex()==0 ? pixel3d::vec3d{1,0,0} :
                          axis_->currentIndex()==1 ? pixel3d::vec3d{0,1,0} : pixel3d::vec3d{0,0,1};
  using T=RetopoEditDocument::Transform;
  if(tool_->currentIndex()==1) return d.transform(selected_,T::Move,axis*(double(dx)*diagonal_/400));
  if(tool_->currentIndex()==2) return d.transform(selected_,T::Rotate,axis,double(dx)*.01);
  if(tool_->currentIndex()==3) return d.transform(selected_,T::Scale,{},std::exp(std::clamp(double(dx)*.01,-4.,4.)));
  return false;
}

bool RetopoMeshEditor::eventFilter(QObject* object, QEvent* event) {
  if(object!=view_->surface()) return QWidget::eventFilter(object,event);
  if(event->type()==QEvent::MouseButtonPress) {
    auto* mouse=static_cast<QMouseEvent*>(event);
    if(mouse->button()==Qt::LeftButton && !(mouse->modifiers() & Qt::AltModifier) &&
       comparison_->currentIndex()==0 && tool_->currentIndex()!=0 && !selected_.empty()) {
      dragging_=true; press_=mouse->position().toPoint();
      preview_.load(document_.positions(),document_.faces());
      view_->surface()->setFocus();
      view_->surface()->grabMouse(); return true;
    }
  }
  if(dragging_ && (event->type()==QEvent::MouseMove || event->type()==QEvent::MouseButtonRelease)) {
    auto* mouse=static_cast<QMouseEvent*>(event);
    const bool finish=event->type()==QEvent::MouseButtonRelease;
    if(finish && mouse->button()!=Qt::LeftButton) return true;
    QString failure;
    try {
      if(finish) { if(transformDrag(document_,mouse->position().toPoint().x()-press_.x())) { clearSaved(); emit edited(); } }
      else { preview_.revert(); transformDrag(preview_,mouse->position().toPoint().x()-press_.x()); draw(preview_); }
    } catch(const std::exception& error) { failure=QString::fromUtf8(error.what()); }
    if(finish) { dragging_=false; view_->surface()->releaseMouse(); refresh(); }
    if(!failure.isEmpty()) status_->setText(failure);
    return true;
  }
  if(dragging_ && event->type()==QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key()==Qt::Key_Escape) {
    dragging_=false; view_->surface()->releaseMouse(); refresh(); return true;
  }
  return QWidget::eventFilter(object,event);
}
}
