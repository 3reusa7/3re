// pixel3d-lowpoly-dev summary <cloaked|humanoid> <budget> [front]
// pixel3d-lowpoly-dev look    <cloaked|humanoid> <budget> <out_dir> [front]   (geometry turntable, shell colours + wire)
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "lowpoly_helpers.hpp"
#include "pixel3d/lowpoly/allocate.hpp"
#include "pixel3d/lowpoly/build.hpp"
#include "pixel3d/lowpoly/gate.hpp"
#include "pixel3d/lowpoly/skinned_gltf.hpp"
#include "pixel3d/lowpoly/assemble.hpp"
#include "pixel3d/lowpoly/layout.hpp"
#include "pixel3d/lowpoly/rig.hpp"
#include "pixel3d/lowpoly/style_render.hpp"
#include "pixel3d/lowpoly/suggest.hpp"

using namespace pixel3d;
using namespace pixel3d::lowpoly;
namespace fs = std::filesystem;

int main(int argc, char** argv) {
  if (argc < 4) {
    std::cerr << "usage: pixel3d-lowpoly-dev summary|look <cloaked|humanoid> <budget> [out_dir] [front]\n";
    return 1;
  }
  const std::string cmd = argv[1], variant = argv[2];
  bool front_only = false, auto_depth = false;
  for (int i = 4; i < argc; ++i) {
    front_only |= std::string(argv[i]) == "front";
    auto_depth |= std::string(argv[i]) == "auto";
  }
  Input in = p3dtest::load_lowpoly_fixture(variant, !front_only, front_only && !auto_depth ? 20 : 0);
  in.settings.tri_budget = std::atoi(argv[3]);
  const auto l = build_layout(in);
  if (!l) {
    std::cerr << l.error().text << "\n";
    return 1;
  }
  const auto a = allocate(*l, in.settings);
  if (!a) {
    std::cerr << a.error().text << "\n";
    return 1;
  }
  if (cmd == "summary") {
    std::cout << "template " << to_string(l->templ) << ", dims " << in.dims.W << "x" << in.dims.H << "x" << in.dims.D << "\n";
    for (const auto& b : l->skeleton.bones)
      std::cout << "  bone " << b.name << " parent " << b.parent << " head (" << b.head.x << ", " << b.head.y << ", " << b.head.z << ")\n";
    for (size_t i = 0; i < l->tubes.size(); ++i) {
      const auto& t = l->tubes[i];
      const auto& p = a->tubes[i];
      std::cout << "tube " << t.name << (t.trunk ? " [trunk]" : "") << (t.chain ? " [chain]" : "") << (t.branch ? " [branch]" : "")
                << " depth=" << t.depth_source << " joints=" << t.joints.size() << " | N=" << p.sides << " rings=" << p.rings
                << " loops=" << p.loops << " extra=" << p.extra << " tris=" << p.triangles << " err=" << p.profile_error
                << " imp=" << p.importance << "\n";
    }
    std::cout << "total " << a->total << " / " << a->budget << "\n";
    for (const auto& m : a->messages) std::cout << "  " << m.code << ": " << m.text << "\n";
    for (const auto& m : l->messages) std::cout << "  " << m.code << ": " << m.text << "\n";
    return 0;
  }
  if (cmd == "look" && argc >= 5) {
    const fs::path out = argv[4];
    fs::create_directories(out);
    const Assembly as = assemble(*l, *a, in.dims.H);
    vec3d lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
    for (const auto& p : as.mesh.positions) {
      lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
      hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    StyleRenderOptions o;
    o.show_wire = true;
    int k = 0;
    for (double yaw : {0, 45, 90, 135, 180, 225, 270, 315}) {
      const ImageRGBA img = render_style(as.mesh, nullptr, nullptr, nullptr, yaw, 12, lo, hi, o);
      save_png(img, out / ("look_" + std::to_string(k++) + ".png"));
    }
    const Pose pose = test_pose(*l);
    const PoseResult pr = apply_pose(as.mesh, l->skeleton, pose);
    for (const auto& p : pr.positions) {
      lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
      hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    k = 0;
    for (double yaw : {0, 45, 90, 135, 180, 225, 270, 315}) {
      const ImageRGBA img = render_style(as.mesh, nullptr, &pr.positions, &pr.normals, yaw, 12, lo, hi, o);
      save_png(img, out / ("pose_" + std::to_string(k++) + ".png"));
    }
    const RigCheck rc = check_rig(as.mesh, l->skeleton, pose, pr, &as);
    for (const auto& [n, v] : rc.joint_volume) std::cout << "joint volume " << n << " " << v << "\n";
    std::cout << "triangles " << as.mesh.triangle_count() << "\n";
    return 0;
  }
  if (cmd == "suggest") {
    std::vector<const ImageRGBA*> imgs{in.views[0].aligned.get()};
    const Suggestion sg = suggest_parts(in.views[0], analyze_palette(imgs), 128);
    std::cout << "template " << to_string(sg.templ) << '\n';
    for (const auto& p : sg.parts) {
      const auto b = p.masks.at(ViewRole::Front).bbox();
      std::cout << "  " << p.name << " " << to_string(p.type) << " px " << p.masks.at(ViewRole::Front).count() << " box " << b.x0 << "," << b.y0 << "-" << b.x1 << "," << b.y1 << '\n';
    }
    if (argc > 4) {  // write a part map
      ImageRGBA map(in.views[0].aligned->width(), in.views[0].aligned->height());
      const Rgba8 keys[] = {{230, 60, 60, 255}, {60, 200, 80, 255}, {60, 90, 230, 255}, {230, 200, 40, 255}, {200, 60, 220, 255}, {40, 210, 210, 255}, {140, 80, 30, 255}, {120, 120, 120, 255}, {250, 140, 0, 255}, {0, 120, 60, 255}, {90, 0, 160, 255}, {255, 180, 200, 255}};
      for (size_t i = 0; i < sg.parts.size(); ++i) {
        const Mask2D& m = sg.parts[i].masks.at(ViewRole::Front);
        for (int y = 0; y < m.height(); ++y)
          for (int x = 0; x < m.width(); ++x)
            if (m.get(x, y)) map.at(x, y) = keys[i % 12];
      }
      save_png(map, argv[argc - 1]);
    }
    return 0;
  }
  if (cmd == "normals") {
    const Assembly as = assemble(*l, *a, in.dims.H);
    for (size_t v = 0; v < as.mesh.normals.size(); ++v)
      if (length(as.mesh.normals[v]) < 0.5) {
        const int sh = as.mesh.vertex_shell[v];
        const TubeShape& s = as.shapes[static_cast<size_t>(sh)];
        std::cout << "vertex " << v << " shell " << sh << " pos (" << as.mesh.positions[v].x << "," << as.mesh.positions[v].y << ","
                  << as.mesh.positions[v].z << ") rings " << s.rings.size() << " caps " << int(s.cap_start) << int(s.cap_end)
                  << " apex_end (" << s.apex_end.x << "," << s.apex_end.y << "," << s.apex_end.z << ") last ring c (" << s.rings.back().c.x
                  << "," << s.rings.back().c.y << "," << s.rings.back().c.z << ") a " << s.rings.back().a << " b " << s.rings.back().b << "\n";
      }
    return 0;
  }
  if (cmd == "render" && argc >= 5) {
    // Textured style renders: turntable (8 yaws) + test pose (4 yaws) + the atlas + stats.json.
    const fs::path out = argv[4];
    fs::create_directories(out);
    for (int i = 5; i < argc; ++i) {
      const std::string a2 = argv[i];
      if (a2.rfind("style=", 0) == 0) in.settings.style = *style_from_string(a2.substr(6));
      if (a2 == "baked") in.settings.baked_cel = true;
      if (a2 == "hull") in.settings.hull = true;
      if (a2.rfind("loops=", 0) == 0) in.settings.joint_loops = std::stoi(a2.substr(6));
    }
    const auto r = build_lowpoly(in);
    if (!r) {
      std::cerr << r.error().text << "\n";
      return 1;
    }
    const SkinnedMesh& m = r->mesh();
    const Pose pose = test_pose(r->layout);
    const PoseResult pr = apply_pose(m, r->layout.skeleton, pose);
    vec3d lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
    for (const auto* ps : {&m.positions, &pr.positions})
      for (const auto& p : *ps) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
      }
    StyleRenderOptions o;
    o.style = in.settings.baked_cel ? (in.settings.style == Style::CelOutline ? Style::Outline : Style::Flat) : in.settings.style;
    o.tones = in.settings.tones;
    int k = 0;
    for (double yaw : {0, 45, 90, 135, 180, 225, 270, 315})
      save_png(render_style(m, r->bake.texture.get(), nullptr, nullptr, yaw, 10, lo, hi, o), out / ("turn_" + std::to_string(k++) + ".png"));
    k = 0;
    for (double yaw : {0, 60, 90, 150})
      save_png(render_style(m, r->bake.texture.get(), &pr.positions, &pr.normals, yaw, 10, lo, hi, o), out / ("pose_" + std::to_string(k++) + ".png"));
    save_png(*r->bake.texture, out / "atlas.png");
    const RigCheck rc = check_rig(m, r->layout.skeleton, pose, pr, &r->assembly);
    nlohmann::json j;
    j["variant"] = variant;
    j["front_only"] = front_only;
    j["budget"] = in.settings.tri_budget;
    j["triangles"] = r->triangles;
    j["template"] = to_string(r->layout.templ);
    j["atlas"] = r->pack.width;
    j["density"] = r->pack.density;
    j["share"] = r->bake.share;
    for (const auto& [n, v] : rc.joint_volume) j["joint_volume"][n] = v;
    j["rigid_max_error"] = rc.rigid_max_error;
    for (const auto& t : r->tubes)
      j["tubes"].push_back({{"name", t.name}, {"tris", t.triangles}, {"sides", t.sides}, {"rings", t.rings},
                            {"loops", t.joint_loops}, {"err", t.profile_error_px}, {"depth", t.depth_source}});
    for (const auto& p : r->parts)
      j["parts"].push_back({{"name", p.name}, {"tris", p.triangles}, {"depth_ratio", p.depth_ratio}, {"depth", p.depth_source}});
    j["auto_depth"] = auto_depth;
    for (const auto& msg : r->messages) j["messages"].push_back(msg.code);
    for (const auto& n : pose.notes) j["pose_notes"].push_back(n);
    SkinnedExportOptions eo;
    eo.style = in.settings.style;
    eo.baked_cel = in.settings.baked_cel;
    const auto glb = write_skinned_glb(m, r->layout.skeleton, *r->bake.texture, eo);
    std::ofstream(out / "model.glb", std::ios::binary).write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    const SkinnedCheck sc = check_skinned_glb(glb, m, r->layout.skeleton, eo);
    const LowpolyValidation val = validate_lowpoly(in, *r, &sc);
    j["status"] = to_string(val.status);
    for (const auto& [role, iou] : val.silhouette_iou) j["silhouette_iou"][std::string(to_string(role))] = iou;
    for (const auto& c : val.conflicts)
      if (c.blocking) j["blocking"].push_back(c.kind);
    std::ofstream(out / "validation.json") << to_json(val).dump(2);
    std::ofstream(out / "stats.json") << j.dump(2);
    std::cout << variant << (front_only ? " (front only)" : "") << ": " << r->triangles << " / " << in.settings.tri_budget << " triangles\n";
    return 0;
  }
  std::cerr << "unknown command\n";
  return 1;
}
