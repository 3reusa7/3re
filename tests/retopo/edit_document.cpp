// Standalone bounded test: no Qt, GPU, neural models or application mutation.
#include "gui/studio/retopo_edit_document.hpp"
#include <iostream>
#include <limits>

namespace {
void check(bool ok) { if (!ok) throw std::runtime_error("Assertion failed"); }
bool same(pixel3d::vec3d a, pixel3d::vec3d b) { return pixel3d::length(a-b) < 1e-12; }
template<class F> void rejects(F f) { bool rejected=false; try { f(); } catch (const std::runtime_error&) { rejected=true; } check(rejected); }
}
int main() {
  using D = p3dgui::RetopoEditDocument;
  D d;
  const std::vector<pixel3d::vec3d> p{{0,0,0},{1,0,0},{1,1,0},{0,1,0}, {0,0,0},{1,0,0},{0,1,0}};
  d.load(p, {{0,1,2,3},{4,5,6}});
  check(d.triangulated().tris.size()==3 && d.faces().size()==2);
  d.transform({0,1,2,3}, D::Transform::Move, {0,0,1});
  check(same(d.positions()[0], {0,0,1}) && same(d.positions()[4], p[4]));
  check(d.undo() && same(d.positions()[0], p[0]));
  check(d.redo() && same(d.positions()[0], {0,0,1}));
  d.revert(); check(!d.canUndo() && same(d.positions()[0], p[0]));
  rejects([&]{d.transform({0}, D::Transform::Move, {1,0,0});});
  check(same(d.positions()[0], p[0]) && !d.canUndo());
  rejects([&]{d.transform({99}, D::Transform::Move, {0,0,1});});
  rejects([&]{d.transform({0}, D::Transform::Move, {std::numeric_limits<double>::infinity(),0,0});});
  rejects([&]{d.transform({0,1,2,3}, D::Transform::Scale, {}, 0);});
  d.transform({0,1,2,3}, D::Transform::Rotate, {0,0,1}, std::acos(-1.)/2);
  check(same(d.positions()[0], {1,0,0}) && same(d.positions()[1], {1,1,0}));
  d.apply(); check(!d.canUndo());
  d.transform({0,1,2,3}, D::Transform::Scale, {}, 2);
  check(same(d.positions()[0], {1.5,-.5,0}));
  d.undo(); d.transform({0,1,2,3}, D::Transform::Move, {0,0,1}); check(!d.canRedo());
  for(int i=0;i<70;++i) d.transform({0,1,2,3}, D::Transform::Move, {0,0,.01});
  int undone=0; while(d.undo()) ++undone;
  check(undone==64 && d.historyBytes()<=16*1024*1024);
  check(d.faces()==std::vector<D::Face>({{0,1,2,3},{4,5,6}}));
  std::cout << "PASS: quad membership; coincident shell independence; transforms; atomic rejection; undo/redo; apply/revert; bounded history\n";
}
