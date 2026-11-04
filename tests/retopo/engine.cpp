// Smart Retopology engine side, without QuadWild: OBJ round trip, welding rules, closest points against brute
// force, two-sided distance and the colour transfer (texture read back at known points).
#include <algorithm>
#include <cmath>
#include <fstream>
#include <numbers>
#include <filesystem>
#include <iostream>
#include <random>
#include "backends/retopo_engine.hpp"
#include "backends/mesh_asset.hpp"
#include "backends/retopo_material.hpp"
#include "backends/retopo_acceptance.hpp"

namespace {
using pixel3d::vec3d;
using namespace p3dasset::retopo;
int failures = 0;
void check(bool ok, const char* what) {
  if (!ok) {
    std::cerr << "FAIL " << what << "\n";
    ++failures;
  }
}

// Unit sphere, latitude/longitude triangles (shared vertices).
pixel3d::Mesh sphere(int n, double r = 1) {
  pixel3d::Mesh m;
  for (int i = 0; i <= n; ++i)
    for (int j = 0; j < 2 * n; ++j) {
      const double th = std::numbers::pi * i / n, ph = std::numbers::pi * j / n;
      m.positions.push_back({r * std::sin(th) * std::cos(ph), r * std::cos(th), r * std::sin(th) * std::sin(ph)});
    }
  const auto id = [n](int i, int j) { return uint32_t(i * 2 * n + (j % (2 * n))); };
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < 2 * n; ++j) {
      if (i > 0) m.tris.push_back({id(i, j), id(i + 1, j), id(i, j + 1)});
      if (i + 1 < n) m.tris.push_back({id(i, j + 1), id(i + 1, j), id(i + 1, j + 1)});
    }
  return m;
}
}  // namespace

int main() {
  const auto dir = std::filesystem::temp_directory_path() / "threere-retopo-engine-tests";
  std::filesystem::create_directories(dir);

  // Regression for Claude's saved hoodie: originally closed, output had 370 boundary edges.
  nlohmann::json valid{{"components",1},{"boundary_edges",0},{"overfull_edges",0},{"invalid_vertex_links",0},
    {"inconsistent_winding_edges",0},{"duplicate_faces",0},{"zero_area_faces",0}};
  require_preserved_topology(valid,valid,1,1);
  for (const auto* defect : {"boundary_edges","components","overfull_edges","invalid_vertex_links","duplicate_faces"}) {
    auto broken=valid; broken[defect]=std::string(defect)=="boundary_edges"?370:2;
    bool stopped=false;try{require_preserved_topology(valid,broken,1202,69);}catch(const std::exception&){stopped=true;}
    check(stopped,"invalid or disconnected result rejected");
  }
  bool topology_stopped=false;
  try{require_preserved_topology(valid,valid,1202,69);}catch(const std::exception&){topology_stopped=true;}
  check(topology_stopped,"tunnel closure rejected even without boundary edges");
  auto open=valid;open["boundary_edges"]=8;topology_stopped=false;
  try{require_preserved_topology(open,valid,0,0);}catch(const std::exception&){topology_stopped=true;}
  check(topology_stopped,"intentional open boundary cannot silently close");

  // OBJ: quads and triangles survive a round trip; negative indices and texture/normal refs are read.
  PolyMesh quad{{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {2, 0, 0}}, {{0, 1, 2, 3}, {1, 4, 2}}};
  write_obj(quad, dir / "q.obj");
  const auto back = read_obj(dir / "q.obj");
  check(back.faces.size() == 2 && back.faces[0].size() == 4 && back.faces[1].size() == 3, "obj round trip");
  check(triangulate(back).tris.size() == 3, "triangulate quad + triangle");
  {
    std::ofstream(dir / "n.obj") << "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nf -3/1 -2/1 -1/1\n";
    const auto n = read_obj(dir / "n.obj");
    check(n.faces.size() == 1 && n.faces[0][0] == 0 && n.faces[0][2] == 2, "negative indices");
  }

  // Welding: a soup is welded; a mesh with shared vertices keeps its (coincident) shells apart.
  pixel3d::Mesh soup;
  soup.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
  soup.tris = {{0, 1, 2}, {3, 4, 5}};
  check(engine_input(soup).positions.size() == 4, "soup welded");
  pixel3d::Mesh shells;
  shells.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
  shells.tris = {{0, 1, 2}, {1, 3, 2}, {4, 6, 5}};
  check(engine_input(shells).positions.size() == 7, "shared-vertex mesh not welded");

  // Split attribute vertex on one face of a tetrahedron: exact UV seam,
  // original shell remains connected via its other edges.
  pixel3d::Mesh seam;
  seam.positions={{0,0,0},{1,0,0},{0,1,0},{0,0,1},{0,0,0}};
  seam.tris={{4,2,1},{0,1,3},{1,2,3},{2,0,3}};
  const auto joined=engine_input(seam);
  check(joined.positions.size()==4 && joined.tris.size()==4,"exact opposite seam connected without face deletion");
  check(p3dasset::geometrically_identical(seam,joined),"seam keeps all oriented surface triangles exactly");
  check(p3dasset::inspect_geometry(joined)["boundary_edges"]==0,"seam closes attribute crack");
  seam.positions[4].z=-0.000001;
  check(engine_input(seam).positions.size()==5,"real narrow gap is not welded");
  auto two=joined;
  for(auto p:joined.positions)two.positions.push_back(p);
  for(auto t:joined.tris){for(auto& i:t)i+=4;two.tris.push_back(t);}
  check(engine_input(two).positions.size()==8,"coincident closed components remain separate");

  // Closest point: grid answer equals brute force.
  const auto s = sphere(24);
  const SurfaceIndex index(s);
  std::mt19937 rng(7);
  std::uniform_real_distribution<double> u(-2, 2);
  double worst = 0;
  for (int k = 0; k < 400; ++k) {
    const vec3d p{u(rng), u(rng), u(rng)};
    double best = 1e300;
    for (const auto& t : s.tris) {
      // dense brute force: sample the triangle finely is too slow; use the index itself on single triangles
      pixel3d::Mesh one;
      one.positions = {s.positions[t[0]], s.positions[t[1]], s.positions[t[2]]};
      one.tris = {{0, 1, 2}};
      best = std::min(best, SurfaceIndex(one).closest(p).distance);
    }
    worst = std::max(worst, std::abs(index.closest(p).distance - best));
  }
  check(worst < 1e-9, "closest point = brute force");

  // Distance: identical meshes measure 0; a 2% larger sphere measures ~1% of the diagonal (r*0.02 / 2r*sqrt3).
  const auto same = measure(s, to_poly(s), 4000);
  check(same["deviation"]["source_to_result"]["max"].get<double>() < 1e-9, "identical mesh distance 0");
  check(same["quads"] == 0 && same["non_manifold_edges"] == 0, "triangle sphere counts");
  // A closed quad cube: 6 quads, no open or non-manifold edges.
  const PolyMesh cube{{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}},
                      {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}}};
  const auto box = measure(triangulate(cube), cube, 2000);
  check(box["quads"] == 6 && box["open_edges"] == 0 && box["non_manifold_edges"] == 0 && box["quad_share"] == 1.0, "closed quad cube");
  const auto big = measure(s, to_poly(sphere(24, 1.02)), 4000);
  const double mean = big["deviation"]["source_to_result"]["mean"].get<double>();
  check(mean > 0.004 && mean < 0.008, "scaled sphere distance");

  // Simplification: a fine sphere down to ~1/8 keeps its shape (distance well under 1% of the size), stays
  // closed where it was closed, and leaves an open border in place.
  {
    const auto fine = sphere(80);
    const auto coarse = simplify(fine, fine.tris.size() / 8);
    check(coarse.tris.size() <= fine.tris.size() / 8 + 2 && coarse.tris.size() > fine.tris.size() / 10, "simplify reaches the target");
    const auto d = measure(fine, to_poly(coarse), 6000);
    check(d["deviation"]["source_to_result"]["max"].get<double>() < 0.01, "simplified sphere keeps its shape");
    check(d["non_manifold_edges"] == 0, "simplify keeps manifold edges");
    // Open grid: its border vertices must stay on the border line.
    pixel3d::Mesh grid;
    const int n = 40;
    for (int y = 0; y <= n; ++y)
      for (int x = 0; x <= n; ++x) grid.positions.push_back({double(x) / n, double(y) / n, 0.02 * std::sin(x * 0.4) * std::cos(y * 0.3)});
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) {
        const uint32_t i = uint32_t(y * (n + 1) + x);
        grid.tris.push_back({i, i + 1, i + uint32_t(n) + 2});
        grid.tris.push_back({i, i + uint32_t(n) + 2, i + uint32_t(n) + 1});
      }
    const auto g = simplify(grid, grid.tris.size() / 6);
    double lo_x = 1, hi_x = 0, lo_y = 1, hi_y = 0;
    for (auto p : g.positions) { lo_x = std::min(lo_x, p.x); hi_x = std::max(hi_x, p.x); lo_y = std::min(lo_y, p.y); hi_y = std::max(hi_y, p.y); }
    check(lo_x < 1e-9 && lo_y < 1e-9 && hi_x > 1 - 1e-9 && hi_y > 1 - 1e-9, "simplify keeps the open border");
  }

  // Closed shell: a sphere with a narrow tunnel bored through it (genus 1) comes back genus 0 at a coarser
  // edge than the tunnel, and keeps its radius.
  {
    const auto ball = sphere(40);
    const auto shell = closed_shell(ball, 0.05);
    check(handles(shell) == 0 && !shell.tris.empty(), "shell of a sphere is a sphere");
    const auto d = measure(ball, to_poly(shell), 4000);
    check(d["deviation"]["source_to_result"]["max"].get<double>() < 0.02, "shell keeps the shape");
    // Torus (genus 1) with a fat ring survives; one with a hair-thin hole closes.
    const auto torus = [](double R, double r) {
      pixel3d::Mesh t;
      const int n = 48, k = 24;
      for (int i = 0; i < n; ++i)
        for (int j = 0; j < k; ++j) {
          const double a = 2 * std::numbers::pi * i / n, b = 2 * std::numbers::pi * j / k;
          t.positions.push_back({(R + r * std::cos(b)) * std::cos(a), r * std::sin(b), (R + r * std::cos(b)) * std::sin(a)});
        }
      for (int i = 0; i < n; ++i)
        for (int j = 0; j < k; ++j) {
          const uint32_t p = uint32_t(i * k + j), q = uint32_t(((i + 1) % n) * k + j), p2 = uint32_t(i * k + (j + 1) % k), q2 = uint32_t(((i + 1) % n) * k + (j + 1) % k);
          t.tris.push_back({p, p2, q});
          t.tris.push_back({q, p2, q2});
        }
      return t;
    };
    check(handles(torus(1, 0.4)) == 1, "torus genus 1");
    check(handles(closed_shell(torus(1, 0.4), 0.05)) == 1, "fat torus keeps its hole");
    check(handles(closed_shell(torus(0.42, 0.4), 0.06)) == 0, "hair-thin hole closes");
  }

  // Colour transfer: left half red, right half blue (by x) on a textured source; the new mesh's texture
  // shows the same colours at the same places.
  pixel3d::TexturedMesh src;
  src.positions = s.positions;
  for (auto p : s.positions) {
    src.normals.push_back(pixel3d::normalize(p));
    src.uvs.push_back({p.x < 0 ? 0.25 : 0.75, 0.5});
  }
  for (const auto& t : s.tris) src.indices.insert(src.indices.end(), t.begin(), t.end());
  auto tex = std::make_shared<pixel3d::ImageRGBA>(4, 1);
  tex->at(0, 0) = tex->at(1, 0) = {255, 0, 0, 255};
  tex->at(2, 0) = tex->at(3, 0) = {0, 0, 255, 255};
  src.texture = tex;
  const auto target = sphere(12, 1.001);
  nlohmann::json evidence;
  const auto out = transfer_appearance(target, &src, 256, 60, evidence);
  check(out.texture && out.texture->width() == 256, "texture size");
  int right = 0, total = 0;
  for (size_t f = 0; f + 2 < out.indices.size(); f += 3) {
    vec3d c{};
    pixel3d::vec2d uv{};
    for (int k = 0; k < 3; ++k) {
      c = c + out.positions[out.indices[f + k]] / 3.0;
      uv = uv + out.uvs[out.indices[f + k]] * (1.0 / 3.0);
    }
    if (std::abs(c.x) < 0.2) continue;  // the colour boundary
    const auto px = out.texture->at(std::clamp(int(uv.x * 256), 0, 255), std::clamp(int(uv.y * 256), 0, 255));
    ++total;
    if ((c.x < 0 && px.r > 200 && px.b < 60) || (c.x > 0 && px.b > 200 && px.r < 60)) ++right;
  }
  check(total > 50 && right == total, "colours land where they were");
  // No source colours: plain grey, geometry still exported.
  nlohmann::json grey;
  const auto plain = transfer_appearance(target, nullptr, 128, 60, grey);
  check(plain.texture && plain.texture->at(10, 10).r == 200, "plain grey without colours");

  // Retopology export must preserve PBR factors and the raw linear MR channels.
  nlohmann::json mat{{"name","fixture"},{"doubleSided",true},
    {"pbrMetallicRoughness",{{"baseColorTexture",{{"index",0}}},{"metallicRoughnessTexture",{{"index",1}}},
      {"metallicFactor",0.7},{"roughnessFactor",0.3}}}};
  const auto base = p3dasset::autotex::write_glb(target, out, {{"kind","auto_texture"}});
  pixel3d::ImageRGBA mr(4,4,{255,73,129,255});
  const auto preserved=split_glb(preserve_material(base,mat,&mr));
  const auto reread=source_material(preserved);
  check(reread["pbrMetallicRoughness"]["roughnessFactor"]==0.3 && reread["doubleSided"]==true, "PBR factors and sidedness preserved");
  const auto map=material_map(preserved,reread["pbrMetallicRoughness"]["metallicRoughnessTexture"]);
  check(map->at(2,2)==mr.at(2,2), "MR channel values survive GLB round trip");
  bool rejected=false;
  try { (void)preserve_material(base,mat,nullptr); } catch(const std::exception&) { rejected=true; }
  check(rejected,"missing MR map is rejected, never silently dropped");
  auto unsupported=preserved;
  unsupported.doc["materials"][0]["normalTexture"]={{"index",0}};
  rejected=false;
  try { (void)source_material(unsupported); } catch(const std::exception&) { rejected=true; }
  check(rejected,"unsupported normal map is rejected before export");

  std::filesystem::remove_all(dir);
  if (failures) return 1;
  std::cout << "PASS retopo engine\n";
  return 0;
}
