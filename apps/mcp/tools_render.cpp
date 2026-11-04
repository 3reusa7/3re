// Render tools: images come back inline (PNG image content) so an agent can look
// at the model when numbers are not enough; output_dir also writes them, with
// every failed write reported.
#include "mcp/tools_detail.hpp"

namespace pixel3d::mcp {
namespace {

RenderStyle style_arg(const Args& a) {
  const auto s = a.opt_str("style");
  if (!s) return RenderStyle::Validation;
  const auto st = render_style_from_string(*s);
  if (!st) throw ArgError("style", "style must be validation or beauty");
  return *st;
}

}  // namespace

ToolOutput render_view(ToolContext& c, const Args& a) {
  a.allow_only({"views", "pixels_per_unit", "lit", "compare", "yaw", "elevation", "size", "output_dir", "style"});
  const RenderStyle style = style_arg(a);
  const auto dir = a.opt_str("output_dir");
  const std::filesystem::path out_dir = dir ? c.paths.resolve("output_dir", *dir) : std::filesystem::path();
  ToolOutput out;
  const bool orbit = a.has("yaw") || a.has("elevation") || a.has("size");
  if (orbit) {
    if (a.has("views") || a.has("pixels_per_unit") || a.has("compare"))
      throw ArgError("yaw", "orbit renders (yaw/elevation/size) cannot be combined with views/pixels_per_unit/compare");
    OrbitRequest r;
    r.yaw_deg = a.opt_num("yaw", -3600, 3600).value_or(30);
    r.elevation_deg = a.opt_num("elevation", -90, 90).value_or(20);
    r.size = a.opt_int("size", 16, 4096).value_or(512);
    r.lit = a.opt_bool("lit").value_or(true);
    r.style = style;
    out.result = c.session.render_orbit(r, &out.images);
  } else {
    RenderRequest r;
    r.views = a.role_list("views");
    r.pixels_per_unit = a.opt_int("pixels_per_unit", 1, 64).value_or(8);
    r.lit = a.opt_bool("lit").value_or(false);
    r.compare = a.opt_bool("compare").value_or(false);
    r.style = style;
    out.result = c.session.render_views(r, &out.images);
  }
  if (out.result.success && dir) write_named_images(out_dir, out.images, out.result);
  return out;
}

ToolOutput render_turntable(ToolContext& c, const Args& a) {
  a.allow_only({"frames", "size", "elevation", "include_frames", "output_dir", "style"});
  TurntableRequest r;
  r.style = style_arg(a);
  r.frames = a.opt_int("frames", 1, 360).value_or(24);
  r.size = a.opt_int("size", 16, 1024).value_or(128);
  r.elevation_deg = a.opt_num("elevation", -90, 90).value_or(20);
  const bool include_frames = a.opt_bool("include_frames").value_or(false);
  const auto dir = a.opt_str("output_dir");
  const std::filesystem::path out_dir = dir ? c.paths.resolve("output_dir", *dir) : std::filesystem::path();
  ToolOutput out;
  std::vector<NamedImage> all;
  out.result = c.session.render_turntable(r, &all, c.job);
  if (!out.result.success) return out;
  if (dir) write_named_images(out_dir, all, out.result);
  for (NamedImage& img : all)
    if (include_frames || img.name == "contact_sheet") out.images.push_back(std::move(img));
  return out;
}

}  // namespace pixel3d::mcp
