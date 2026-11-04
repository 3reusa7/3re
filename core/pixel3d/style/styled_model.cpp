#include "pixel3d/style/styled_model.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <sstream>

#include "pixel3d/style/bake.hpp"
#include "pixel3d/style/materials.hpp"
#include "pixel3d/texture/filtering.hpp"
#include "pixel3d/texture/transfer.hpp"

namespace pixel3d {
namespace {

// Baked maps stay small enough to bake on a laptop CPU and to ship in a GLB:
// at most kMaxBakedTexels texels, never more than 8 per source pixel.
constexpr double kMaxBakedTexels = 512.0 * 1024.0;
constexpr int kMaxAtlasSide = 4096;

double surface_area(const TexturedMesh& m) {
  double a = 0;
  for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    const vec3d p = m.positions[m.indices[t]], q = m.positions[m.indices[t + 1]], r = m.positions[m.indices[t + 2]];
    a += 0.5 * length(cross(q - p, r - p));
  }
  return a;
}

std::string fmt(double v) {
  std::ostringstream o;
  o.precision(3);
  o << v;
  return o.str();
}

}  // namespace

Expected<StyledModel> build_styled_model(const StyleInputs& in) {
  if (in.settings.preset == StylePreset::Crisp)
    return make_error("style.not_polished", "the crisp style has no polished model");
  if (!in.parts || !in.crisp || !in.crisp_texture || !in.families)
    return make_error("style.empty_model", "the polished model needs the crisp parts and texture");
  StyledModel m;
  m.preset = in.settings.preset;
  m.params = style_params(in.settings);
  const StyleParams& p = m.params;

  // 1. Rounded volumes inside the hull (pixel centres kept per corner), bevels on thin corners.
  CornerOptions co;
  co.radius = p.radius;
  co.exponent = p.exponent;
  co.bevel = p.bevel;
  if (p.exponent <= 1) co.segments = 1;
  PartGeometry g = p.radius > 0 || p.bevel > 0 ? round_parts(*in.parts, co, &m.rounding) : *in.parts;

  // 2. Mirror symmetry, only when every drawing that shows X is its own mirror image.
  if (p.symmetry) {
    m.symmetry = detect_mirror_symmetry(in.masks, in.dims);
    if (m.symmetry.symmetric) g = mirror_parts(g, m.symmetry.axis_x, &m.mirror);
  }
  m.solid = assemble_parts(g, in.cleanup);
  if (m.solid.topology.triangle_count == 0)
    return make_error("style.empty_model", "the polished model has no geometry");

  // 3. Texture from the crisp result (one view per texel), then shading.
  m.texture = transfer_textures(m.solid.mesh, *in.crisp, *in.crisp_texture, in.dims);
  TexturedMesh mesh = m.texture.mesh;
  if (p.smooth_angle > 0) {
    NormalOptions no;
    no.smooth_angle_deg = p.smooth_angle;
    mesh = smooth_normals(mesh, no, &m.normals);
  }

  // 4. Unique UVs and baked maps (shading bevel, ambient occlusion).
  if (p.shade_radius > 0 || p.ao) {
    const double area = std::max(1.0, surface_area(mesh));
    double density = std::clamp(std::floor(std::sqrt(kMaxBakedTexels / area)), 1.0, 8.0);
    TexturedMesh with_uv = add_unique_uv(mesh, density, 2, &m.uv);
    while ((m.uv.width > 2048 || m.uv.height > 2048) && density > 1) {
      density = std::max(1.0, std::floor(density / 2));
      with_uv = add_unique_uv(mesh, density, 2, &m.uv);
    }
    mesh = std::move(with_uv);
    if (p.shade_radius > 0) {
      BevelOptions bv;
      bv.radius = p.shade_radius;
      mesh.normal_map = std::make_shared<const ImageRGBA>(bake_bevel_normals(mesh, *in.crisp, m.uv.width, m.uv.height, bv));
    }
    if (p.ao) mesh.occlusion = std::make_shared<const ImageRGBA>(bake_ambient_occlusion(mesh, m.uv.width, m.uv.height));
  }

  // 5. Materials per family (the user's overrides on top of the style's defaults).
  m.materials.materials = default_materials(*in.families, std::string(to_string(m.preset)));
  m.materials.tri_material = triangle_families(mesh, *in.families);
  for (const FamilyMaterial& fm : in.settings.materials) {
    if (fm.family < 0 || fm.family >= static_cast<int>(m.materials.materials.size())) {
      m.notes.push_back(Message{"style.material_unknown",
                                "material settings for family " + std::to_string(fm.family) +
                                    " are ignored: the drawings have no such family",
                                Severity::Warning, {{"family", fm.family}}});
      continue;
    }
    StyleMaterial& sm = m.materials.materials[static_cast<size_t>(fm.family)];
    if (fm.roughness) sm.roughness = *fm.roughness;
    if (fm.metallic) sm.metallic = *fm.metallic;
    if (fm.emissive) sm.emissive = *fm.emissive;
  }

  // 6. Mipmap-safe colour atlas: gutters filled, enlarged with nearest sampling.
  m.texture.mesh = std::move(mesh);
  if (m.texture.mesh.texture) {
    const int side = std::max(m.texture.mesh.texture->width(), m.texture.mesh.texture->height());
    m.atlas_scale = std::clamp(kMaxAtlasSide / std::max(1, side), 1, 8);
    m.texture = pad_for_filtering(m.texture, m.atlas_scale);
  }

  // What was done, honestly.
  CornerReport corners;
  for (const PartRoundReport& r : m.rounding) {
    corners += r.corners;
    if (r.fallback)
      m.notes.push_back(Message{"style.fallback",
                                "part " + std::to_string(r.part) + " keeps its crisp shape: " + r.reason,
                                Severity::Info, {{"part", r.part}, {"reason", r.reason}}});
  }
  std::string what = "style " + std::string(to_string(m.preset)) + ": " + std::to_string(corners.rounded) + " of " +
                     std::to_string(corners.corners) + " corners rounded (largest " + fmt(corners.max_size) + " px)";
  if (p.smooth_angle > 0) what += ", smooth shading below " + fmt(p.smooth_angle) + " degrees";
  if (p.shade_radius > 0) what += ", shading bevel " + fmt(p.shade_radius) + " px";
  if (p.ao) what += ", ambient occlusion";
  m.notes.insert(m.notes.begin(), Message{"style.applied", what + "; silhouettes keep every drawn pixel", Severity::Info,
                                          to_json(m)});
  if (corners.restored_pixels > 0)
    m.notes.push_back(Message{"style.restored_pixels",
                              std::to_string(corners.restored_pixels) +
                                  " pixel square(s) were put back so the silhouettes keep every drawn pixel",
                              Severity::Warning, {{"pixels", corners.restored_pixels}}});
  if (m.symmetry.symmetric)
    m.notes.push_back(Message{"style.symmetry",
                              "the drawings are pixel-exact mirror images: the right side was built as the mirror of the "
                              "left (" + std::to_string(m.mirror.mirrored_pairs) + " part pair(s), " +
                                  std::to_string(m.mirror.halved) + " part(s) across the middle)",
                              Severity::Info,
                              {{"axis_x", m.symmetry.axis_x}, {"pairs", m.mirror.mirrored_pairs}, {"halved", m.mirror.halved}}});
  return m;
}

void apply_style_export(const StyledModel& m, ExportOptions& eo) {
  eo.style = &m.materials;
  eo.mipmaps = m.atlas_scale > 1;
}

nlohmann::json to_json(const StyledModel& m) {
  CornerReport c;
  int fallbacks = 0;
  for (const PartRoundReport& r : m.rounding) c += r.corners, fallbacks += r.fallback;
  nlohmann::json j{{"style", to_string(m.preset)},
                   {"radius", m.params.radius},
                   {"exponent", m.params.exponent},
                   {"bevel", m.params.bevel},
                   {"smooth_angle", m.params.smooth_angle},
                   {"shade_radius", m.params.shade_radius},
                   {"ao", m.params.ao},
                   {"corners", {{"total", c.corners}, {"rounded", c.rounded}, {"clamped", c.clamped}, {"thin", c.thin},
                                {"beveled", c.beveled}, {"joined", c.joined}, {"restored_pixels", c.restored_pixels},
                                {"largest_px", c.max_size}}},
                   {"fallback_parts", fallbacks},
                   {"symmetric", m.symmetry.symmetric},
                   {"triangles", m.solid.topology.triangle_count},
                   {"materials", m.materials.materials.size()},
                   {"atlas_scale", m.atlas_scale}};
  if (m.uv.charts > 0) j["baked_maps"] = {{"charts", m.uv.charts}, {"width", m.uv.width}, {"height", m.uv.height}};
  return j;
}

}  // namespace pixel3d
