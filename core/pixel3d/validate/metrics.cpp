#include "pixel3d/validate/metrics.hpp"

#include <algorithm>
#include <set>

#include "pixel3d/render/camera.hpp"

namespace pixel3d {
namespace {

const SourceView* find_view(const std::vector<SourceView>& views, ViewRole r) {
  for (const SourceView& v : views)
    if (v.role == r && v.aligned) return &v;
  return nullptr;
}

double width_of(const Mask2D& m, double scale) {
  const auto b = m.bbox();
  return b.empty() ? 0.0 : b.width() / scale;
}

}  // namespace

TurntableMetric turntable_metrics(const Mesh& mesh, Dims d, int frames) {
  TurntableMetric t;
  t.frames = frames;
  t.all_non_empty = true;
  for (int k = 0; k < frames; ++k) {
    const Camera cam = camera_orbit(d, 360.0 * k / frames, 0.0, 160, 160);
    const RenderTarget rt = render_mesh_flat(mesh, cam);
    const double area = rt.coverage.count() / (cam.scale * cam.scale);
    t.areas.push_back(area);
    t.widths.push_back(width_of(rt.coverage, cam.scale));
    if (area <= 0) t.all_non_empty = false;
  }
  const auto [amin, amax] = std::minmax_element(t.areas.begin(), t.areas.end());
  const auto [wmin, wmax] = std::minmax_element(t.widths.begin(), t.widths.end());
  t.min_area_ratio = *amax > 0 ? *amin / *amax : 0;
  t.min_width_ratio = *wmax > 0 ? *wmin / *wmax : 0;
  return t;
}

QualityMetrics compute_metrics(const TexturedMesh& tm, const Mesh& mesh, const std::vector<SourceView>& views, Dims d,
                               const CorrespondenceResult& corr, const TextureResult& tex,
                               uint8_t alpha_threshold, const std::vector<RecessFinding>* recesses, bool full) {
  QualityMetrics q;
  // Projection of every supplied view at source resolution.
  for (const SourceView& v : views) {
    if (!v.aligned) continue;
    const RenderTarget rt = render(tm, camera_for_view(v.role, d, 1), RenderOptions{});
    const Mask2D src = opaque_mask(*v.aligned, alpha_threshold);
    ViewMetric m;
    m.iou = mask_iou(rt.coverage, src);
    m.mismatch_px = mask_xor_count(rt.coverage, src);
    m.mask_area = src.count();
    int both = 0, exact = 0;
    for (int y = 0; y < src.height(); ++y)
      for (int x = 0; x < src.width(); ++x) {
        if (!src.get(x, y) || !rt.coverage.get(x, y)) continue;
        ++both;
        const Rgba8 a = rt.color.at(x, y), b = v.aligned->at(x, y);
        exact += (a.r == b.r && a.g == b.g && a.b == b.b) ? 1 : 0;
      }
    m.color_exact_fraction = both > 0 ? static_cast<double>(exact) / both : 0;
    q.views[v.role] = m;
  }

  // Rotations: exact view cameras of the opposite/side directions vs mirrored sources.
  q.full = full;
  const SourceView* front = find_view(views, ViewRole::Front);
  const SourceView* side = find_view(views, ViewRole::Side);
  // Width seen from the front, whether or not a Front drawing exists.
  const double w0 = width_of(render_mesh_flat(mesh, camera_for_view(ViewRole::Front, d, 1)).coverage, 1.0);
  std::vector<double> widths = {w0};
  const std::pair<int, ViewRole> rot[] = {{90, ViewRole::Right}, {180, ViewRole::Back}, {270, ViewRole::Side}};
  for (const auto& [yaw, role] : rot) {
    if (!full) break;
    const RenderTarget rt = render_mesh_flat(mesh, camera_for_view(role, d, 1));
    RotationMetric r;
    r.yaw = yaw;
    r.projected_width = width_of(rt.coverage, 1.0);
    const SourceView* ref = yaw == 180 ? front : side;
    widths.push_back(r.projected_width);
    if (!ref) continue;  // no drawing to compare with (single view)
    Mask2D expected = opaque_mask(*ref->aligned, alpha_threshold);
    if (yaw != 270) expected = flip_x(expected);
    r.iou = mask_iou(rt.coverage, expected);
    q.rotations.push_back(r);
  }
  const auto [wmin, wmax] = std::minmax_element(widths.begin(), widths.end());
  q.thickness_ratio = *wmax > 0 ? *wmin / *wmax : 0;

  if (full) q.turntable = turntable_metrics(mesh, d);
  q.topology = analyze_topology(mesh);
  q.volume = q.topology.signed_volume;
  if (q.volume > 0) {
    q.inferred_volume_fraction = corr.inferred_volume / q.volume;
    // Ambiguous: candidates no evidence decides, plus the solid under every
    // enclosed region whose depth is undetermined (it may be a hollow).
    if (recesses)
      for (const RecessFinding& f : *recesses)
        if (f.kind == "possible_recess") q.recess_volume += f.column_volume;
    q.ambiguous_volume_fraction = std::min(1.0, (corr.ambiguous_volume + q.recess_volume) / q.volume);
  }

  double total = 0;
  for (const auto& [mode, a] : tex.area_by_mode) total += a;
  for (const auto& [mode, a] : tex.area_by_mode) q.texture[std::string(to_string(mode)) + "_fraction"] = total > 0 ? a / total : 0;
  q.texture["texel_density_min"] = tex.texel_density_min;
  q.texture["texel_density_max"] = tex.texel_density_max;
  if (tex.atlas.image) {
    q.texture["atlas_width"] = tex.atlas.image->width();
    q.texture["atlas_height"] = tex.atlas.image->height();
    // Palette preserved: every opaque atlas colour is a source colour.
    std::set<uint32_t> src;
    for (const SourceView& v : views)
      if (v.aligned)
        for (const Rgba8& p : v.aligned->pixels())
          if (p.a >= alpha_threshold) src.insert((uint32_t(p.r) << 16) | (uint32_t(p.g) << 8) | p.b);
    bool preserved = true;
    for (const Rgba8& p : tex.atlas.image->pixels())
      if (p.a == 255 && !src.contains((uint32_t(p.r) << 16) | (uint32_t(p.g) << 8) | p.b)) preserved = false;
    q.texture["palette_preserved"] = preserved ? 1 : 0;
  }
  return q;
}

}  // namespace pixel3d
