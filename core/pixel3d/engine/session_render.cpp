// Session operations that produce images or files (renders, artifacts, GLB).
#include <cmath>
#include <cstdio>

#include "pixel3d/common/file_io.hpp"
#include "pixel3d/common/limits.hpp"
#include "pixel3d/common/path_utf8.hpp"
#include "pixel3d/engine/session.hpp"
#include "pixel3d/io/glb_check.hpp"
#include "pixel3d/io/obj_writer.hpp"
#include "pixel3d/render/camera.hpp"

namespace pixel3d {
namespace {

Message not_ready(const char* what) {
  return Message{"state.not_ready", std::string("no textured model yet: ") + what, Severity::Error, {}};
}

Message bad_request(const std::string& text) { return Message{"input.invalid", text, Severity::Error, {}}; }

template <class F>
OpResult guarded(F&& fn) {
  try {
    return fn();
  } catch (const std::bad_alloc&) {
    return OpResult::fail(Message{"internal.out_of_memory", "not enough memory for this operation", Severity::Error, {}});
  } catch (const std::exception& e) {
    return OpResult::fail(Message{"internal.exception", e.what(), Severity::Error, {}});
  }
}

ImageRGBA upscale(const ImageRGBA& src, int k) {
  ImageRGBA out(src.width() * k, src.height() * k);
  for (int y = 0; y < out.height(); ++y)
    for (int x = 0; x < out.width(); ++x) out.at(x, y) = src.at(x / k, y / k);
  return out;
}

// Source vs 1× re-render, one colour per agreement class (see RenderRequest::compare).
ImageRGBA compare_image(const ImageRGBA& source, const RenderTarget& rt, uint8_t alpha_threshold, nlohmann::json* counts) {
  ImageRGBA out(source.width(), source.height());
  int match = 0, extra = 0, missing = 0, recolored = 0;
  for (int y = 0; y < source.height(); ++y)
    for (int x = 0; x < source.width(); ++x) {
      const bool s = source.at(x, y).a >= alpha_threshold;
      const bool m = rt.coverage.in_bounds(x, y) && rt.coverage.get(x, y);
      if (s && m) {
        const Rgba8 a = source.at(x, y), b = rt.color.at(x, y);
        const bool same = a.r == b.r && a.g == b.g && a.b == b.b;
        out.at(x, y) = same ? Rgba8{128, 128, 128, 255} : Rgba8{255, 220, 0, 255};
        (same ? match : recolored)++;
      } else if (m) {
        out.at(x, y) = Rgba8{230, 40, 40, 255};
        ++extra;
      } else if (s) {
        out.at(x, y) = Rgba8{40, 90, 255, 255};
        ++missing;
      }
    }
  *counts = {{"match", match}, {"rendered_not_drawn", extra}, {"drawn_not_rendered", missing}, {"color_differs", recolored}};
  return out;
}

// Beauty renders of a view role look along that view's camera direction.
std::pair<double, double> beauty_angles(ViewRole role) {
  switch (role) {
    case ViewRole::Front: return {0, 0};
    case ViewRole::Right: return {90, 0};
    case ViewRole::Back: return {180, 0};
    case ViewRole::Side: return {270, 0};
    case ViewRole::Top: return {0, 90};
    case ViewRole::Bottom: return {0, -90};
  }
  return {0, 0};
}

// Renders show the model that is validated and exported. A polished style is
// built at validation; until then the renders show the crisp working model.
void note_model(const Document& d, OpResult& r) {
  if (d.settings.style.preset == StylePreset::Crisp || d.styled) return;
  r.add(Message{"style.not_built",
                "style " + std::string(to_string(d.settings.style.preset)) +
                    " is built at validation: this render shows the crisp working model (run validate)",
                Severity::Info, {{"style", to_string(d.settings.style.preset)}}});
}

BeautyOptions beauty_options(int size) {
  BeautyOptions bo;
  bo.width = bo.height = size;
  bo.supersample = size > 1024 ? 2 : 3;
  return bo;
}

}  // namespace

void write_named_images(const std::filesystem::path& dir, const std::vector<NamedImage>& images, OpResult& r) {
  if (!r.data.contains("files")) r.data["files"] = nlohmann::json::array();
  for (const NamedImage& img : images) {
    const std::filesystem::path p = dir / (img.name + ".png");
    const Message m = save_png(img.image, p);
    if (m.code == "ok") {
      r.data["files"].push_back({{"name", img.name}, {"path", to_utf8(p)}, {"bytes", m.where.value("bytes", 0)}});
    } else {
      r.add(m);
      r.success = false;
    }
  }
}

OpResult Session::render_views(const RenderRequest& req, std::vector<NamedImage>* out) const {
  return guarded([&] {
    const Document& d = history_.current();
    if (!d.texture) return OpResult::fail(not_ready("run reconstruct and project_textures first"));
    if (req.pixels_per_unit < 1 || req.pixels_per_unit > 64) return OpResult::fail(bad_request("pixels_per_unit must be in [1, 64]"));
    std::vector<ViewRole> roles = req.views;
    if (roles.empty())
      for (const SourceView& v : d.views) roles.push_back(v.role);
    OpResult r;
    r.data["images"] = nlohmann::json::array();
    if (req.style == RenderStyle::Beauty) {
      if (req.compare) return OpResult::fail(bad_request("compare needs the validation style"));
      if (req.beauty_size < 16 || req.beauty_size > limits::kMaxRenderSide)
        return OpResult::fail(bad_request("size must be in [16, " + std::to_string(limits::kMaxRenderSide) + "]"));
      for (ViewRole role : roles) {
        const auto [yaw, el] = beauty_angles(role);
        NamedImage img{std::string(to_string(role)) + "_beauty",
                       render_beauty(pipeline::model_mesh(d), yaw, el, beauty_options(req.beauty_size))};
        r.data["images"].push_back({{"name", img.name}, {"view", to_string(role)}, {"style", "beauty"},
                                    {"width", img.image.width()}, {"height", img.image.height()}});
        if (out) out->push_back(std::move(img));
      }
      note_model(d, r);
      r.success = true;
      finish(r);
      return r;
    }
    RenderOptions ro;
    ro.lit = req.lit;
    for (ViewRole role : roles) {
      const ImageSize sz = view_image_size(role, d.align->dims);
      if (static_cast<int64_t>(std::max(sz.w, sz.h)) * req.pixels_per_unit > limits::kMaxRenderSide)
        return OpResult::fail(bad_request("the render would exceed " + std::to_string(limits::kMaxRenderSide) +
                                          " px; lower pixels_per_unit"));
      const Camera cam = camera_for_view(role, d.align->dims, req.pixels_per_unit);
      NamedImage img{std::string(to_string(role)) + "_" + std::to_string(req.pixels_per_unit) + "x",
                     render(pipeline::model_mesh(d), cam, ro).color};
      r.data["images"].push_back({{"name", img.name}, {"view", to_string(role)}, {"width", img.image.width()},
                                  {"height", img.image.height()}});
      if (out) out->push_back(std::move(img));
      if (!req.compare) continue;
      const SourceView* src = nullptr;
      for (const SourceView& v : d.views)
        if (v.role == role) src = &v;
      if (!src || !src->aligned) {
        r.add(Message{"render.no_source", "no source drawing to compare for " + std::string(to_string(role)),
                      Severity::Warning, {{"view", to_string(role)}}});
        continue;
      }
      RenderOptions flat;
      const RenderTarget one = render(pipeline::model_mesh(d), camera_for_view(role, d.align->dims, 1), flat);
      nlohmann::json counts;
      const ImageRGBA cmp = compare_image(*src->aligned, one, d.settings.alpha_threshold, &counts);
      NamedImage ci{std::string(to_string(role)) + "_compare", upscale(cmp, req.pixels_per_unit)};
      r.data["images"].push_back({{"name", ci.name},
                                  {"view", to_string(role)},
                                  {"width", ci.image.width()},
                                  {"height", ci.image.height()},
                                  {"legend", "grey=match, red=rendered not drawn, blue=drawn not rendered, yellow=colour differs"},
                                  {"pixels", counts}});
      if (out) out->push_back(std::move(ci));
    }
    note_model(d, r);
    r.success = true;
    finish(r);
    return r;
  });
}

OpResult Session::render_orbit(const OrbitRequest& req, std::vector<NamedImage>* out) const {
  return guarded([&] {
    const Document& d = history_.current();
    if (!d.texture) return OpResult::fail(not_ready("run reconstruct and project_textures first"));
    if (!std::isfinite(req.yaw_deg) || !std::isfinite(req.elevation_deg) || std::abs(req.elevation_deg) > 90)
      return OpResult::fail(bad_request("yaw must be finite and elevation in [-90, 90]"));
    if (req.size < 16 || req.size > limits::kMaxRenderSide)
      return OpResult::fail(bad_request("size must be in [16, " + std::to_string(limits::kMaxRenderSide) + "]"));
    RenderOptions ro;
    ro.lit = req.lit;
    char name[64];
    std::snprintf(name, sizeof name, "orbit_yaw%03d_el%+03d", static_cast<int>(std::lround(req.yaw_deg)),
                  static_cast<int>(std::lround(req.elevation_deg)));
    const bool beauty = req.style == RenderStyle::Beauty;
    NamedImage img{beauty ? std::string(name) + "_beauty" : std::string(name),
                   beauty ? render_beauty(pipeline::model_mesh(d), req.yaw_deg, req.elevation_deg, beauty_options(req.size))
                          : render(pipeline::model_mesh(d), camera_orbit(d.align->dims, req.yaw_deg, req.elevation_deg, req.size, req.size), ro).color};
    OpResult r;
    r.data["images"] = {{{"name", img.name}, {"yaw", req.yaw_deg}, {"elevation", req.elevation_deg},
                         {"width", req.size}, {"height", req.size}}};
    if (beauty) r.data["images"][0]["style"] = "beauty";
    if (out) out->push_back(std::move(img));
    note_model(d, r);
    r.success = true;
    finish(r);
    return r;
  });
}

OpResult Session::render_turntable(const TurntableRequest& req, std::vector<NamedImage>* out,
                                   const JobContext& job) const {
  return guarded([&] {
    const Document& d = history_.current();
    if (!d.texture) return OpResult::fail(not_ready("run reconstruct and project_textures first"));
    if (req.frames < 1 || req.frames > limits::kMaxTurntableFrames)
      return OpResult::fail(bad_request("frames must be in [1, " + std::to_string(limits::kMaxTurntableFrames) + "]"));
    if (req.size < 16 || req.size > 1024) return OpResult::fail(bad_request("size must be in [16, 1024]"));
    if (!std::isfinite(req.elevation_deg) || std::abs(req.elevation_deg) > 90)
      return OpResult::fail(bad_request("elevation must be in [-90, 90]"));
    if (static_cast<int64_t>(req.frames) * req.size * req.size > limits::kMaxImagePixels * 4)
      return OpResult::fail(bad_request("frames × size² is too large"));
    const int frames = req.frames, size = req.size;
    const int cols = frames == 24 ? 6 : static_cast<int>(std::ceil(std::sqrt(static_cast<double>(frames))));
    const int rows = (frames + cols - 1) / cols;
    ImageRGBA sheet(cols * size, rows * size, Rgba8{40, 44, 52, 255});
    RenderOptions ro;
    ro.lit = true;
    OpResult r;
    r.data["frames"] = nlohmann::json::array();
    for (int k = 0; k < frames; ++k) {
      if (job.stop.stop_requested()) return OpResult::fail(Message{"job.cancelled", "cancelled", Severity::Error, {}});
      if (job.progress) job.progress(static_cast<float>(k) / frames, "turntable");
      const double yaw = 360.0 * k / frames;
      const ImageRGBA frame =
          req.style == RenderStyle::Beauty
              ? render_beauty(pipeline::model_mesh(d), yaw, req.elevation_deg, beauty_options(size))
              : render(pipeline::model_mesh(d), camera_orbit(d.align->dims, yaw, req.elevation_deg, size, size), ro).color;
      for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
          if (frame.at(x, y).a > 0) sheet.at((k % cols) * size + x, (k / cols) * size + y) = frame.at(x, y);
      char name[32];
      std::snprintf(name, sizeof name, "frame_%02d", k);
      r.data["frames"].push_back({{"name", name}, {"yaw", yaw}});
      if (out) out->push_back(NamedImage{name, frame});
    }
    if (req.contact_sheet) {
      r.data["contact_sheet"] = {{"name", "contact_sheet"}, {"columns", cols}, {"rows", rows}, {"cell", size}};
      if (req.style == RenderStyle::Beauty) r.data["style"] = "beauty";
      if (out) out->push_back(NamedImage{"contact_sheet", std::move(sheet)});
    }
    note_model(d, r);
    r.success = true;
    finish(r);
    return r;
  });
}

OpResult Session::write_artifacts(const ArtifactRequest& req, const JobContext& job) const {
  return guarded([&] {
    const Document& d = history_.current();
    if (!d.texture) return OpResult::fail(not_ready("run reconstruct and project_textures first"));
    if (req.dir.empty()) return OpResult::fail(bad_request("an output directory is required"));
    OpResult r;
    r.success = true;
    r.data["files"] = nlohmann::json::array();
    auto stage = [&](const OpResult& rendered) {
      if (rendered.success) return true;
      for (const Message& m : rendered.errors) r.add(m);
      r.success = false;
      return false;
    };
    if (req.views) {
      std::vector<NamedImage> imgs;
      if (stage(render_views(RenderRequest{{}, 1, false, false}, &imgs)) &&
          stage(render_views(RenderRequest{{}, 8, false, false}, &imgs)))
        write_named_images(req.dir / "views", imgs, r);
    }
    if (req.rotations) {
      // Yaw 90 / 180 / 270 are exactly the Right, Back and Side cameras.
      std::vector<NamedImage> imgs;
      if (stage(render_views(RenderRequest{{ViewRole::Right, ViewRole::Back, ViewRole::Side}, 8, true, false}, &imgs))) {
        const char* names[] = {"yaw_090", "yaw_180", "yaw_270"};
        for (size_t i = 0; i < imgs.size() && i < 3; ++i) imgs[i].name = names[i];
        write_named_images(req.dir / "rotations", imgs, r);
      }
    }
    if (req.turntable) {
      std::vector<NamedImage> imgs;
      if (stage(render_turntable(TurntableRequest{}, &imgs, job))) write_named_images(req.dir / "turntable", imgs, r);
    }
    r.data["written"] = r.data["files"].size();
    if (!r.success)
      r.errors.insert(r.errors.begin(), Message{"artifacts.incomplete", "some requested render files were not written",
                                                Severity::Error, {{"dir", to_utf8(req.dir)}}});
    finish(r);
    return r;
  });
}

namespace {

std::vector<uint8_t> bytes_of(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

std::string ascii_lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

}  // namespace

OpResult Session::export_glb(const GlbExportRequest& req) {
  ModelExportRequest m;
  m.path = req.path;
  m.format = ExportFormat::Glb;
  m.allow_needs_review = req.allow_needs_review;
  m.allow_fail = req.allow_fail;
  return export_model(m);
}

OpResult Session::export_model(const ModelExportRequest& req) {
  const PresetDefaults preset = preset_defaults(req.preset);
  // Format: explicit, else the file extension, else the preset's.
  std::optional<ExportFormat> from_ext;
  if (!req.path.empty()) {
    std::string ext = ascii_lower(to_utf8(req.path.extension()));
    if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
    from_ext = export_format_from_string(ext);
  }
  const ExportFormat format = req.format ? *req.format : from_ext ? *from_ext : preset.format;
  ExportOptions eo;
  eo.units = req.units.value_or(preset.units);
  eo.front_negative_z = req.front_negative_z.value_or(preset.front_negative_z);
  eo.unlit = req.unlit;
  const bool glb_only = format == ExportFormat::Glb && req.preset == EnginePreset::Generic && !req.units && !req.front_negative_z && !req.unlit;
  nlohmann::json params{{"path", to_utf8(req.path)},
                        {"format", to_string(format)},
                        {"allow_needs_review", req.allow_needs_review},
                        {"allow_fail", req.allow_fail}};
  if (!glb_only) {
    params["preset"] = to_string(req.preset);
    params["units"] = to_string(eo.units);
    params["front"] = eo.front_negative_z ? "-z" : "+z";
    params["unlit"] = eo.unlit;
  }
  return apply(
      glb_only ? "export_glb" : "export_model", params, Commit::Replace, [&](Document& d) {
        OpResult r;
        if (!req.path.empty() && req.format && from_ext && *from_ext != *req.format)
          return OpResult::fail(Message{"input.invalid",
                                        "the file extension does not match the format (" + std::string(extension(format)) + ")",
                                        Severity::Error, {{"path", to_utf8(req.path)}}});
        pipeline::Notes notes;
        // Always validated first: the status of what is exported is never unknown.
        if (auto e = pipeline::ensure(d, pipeline::Stage::Validated, {}, notes)) return OpResult::fail(*e);
        for (const Message& m : notes) r.add(m);
        const ValidationStatus status = d.gate->status;
        nlohmann::json why{{"status", to_string(status)}};
        for (const Check& c : d.gate->checks)
          if (c.critical && !c.passed) why["failed_critical_checks"].push_back(c.name);
        for (const Conflict& c : *d.conflicts)
          if (c.blocks_pass) why["blocking_conflicts"].push_back({{"kind", c.kind}, {"text", c.text}});
        if (status == ValidationStatus::NeedsReview && !req.allow_needs_review)
          return OpResult::fail(Message{"export.needs_review",
                                        "the model is NEEDS_REVIEW: resolve the blocking conflicts or accept "
                                        "the status explicitly (allow_needs_review)",
                                        Severity::Error, why});
        if (status == ValidationStatus::Fail && !req.allow_fail)
          return OpResult::fail(Message{"export.validation_failed",
                                        "the model FAILS validation: fix the failed checks or accept the status "
                                        "explicitly (allow_fail)",
                                        Severity::Error, why});
        eo.pixels_per_meter = d.settings.pixels_per_meter;  // the project's scale, for every client
        // The validated model: polished (GLB: with its maps and per-family materials) or crisp.
        TexturedMesh plain;
        const TexturedMesh* model = &pipeline::model_mesh(d);
        if (d.styled) {
          if (format == ExportFormat::Glb) {
            apply_style_export(*d.styled, eo);
          } else {
            // glTF companions and OBJ: the polished geometry and colours only (see the warning below).
            plain = *model;
            plain.uvs1.clear();
            plain.tangents.clear();
            plain.normal_map.reset();
            plain.occlusion.reset();
            model = &plain;
          }
        }
        const TexturedMesh& mesh = *model;
        const std::string base = req.path.empty() ? std::string("model") : to_utf8(req.path.stem());
        // Build every file in memory and check it independently before anything is written.
        std::vector<std::pair<std::filesystem::path, std::vector<uint8_t>>> files;  // companions first, main file last
        GlbCheck check;
        const std::filesystem::path dir = req.path.parent_path();
        auto beside = [&](const std::string& ext) { return dir / path_from_utf8(base + ext); };
        switch (format) {
          case ExportFormat::Glb: {
            std::vector<uint8_t> glb = write_glb(mesh, eo);
            check = check_glb_roundtrip(glb, mesh, eo, d.align->dims);
            files.push_back({req.path, std::move(glb)});
            break;
          }
          case ExportFormat::Gltf: {
            GltfFiles g = write_gltf(mesh, eo, base);
            check = check_gltf_roundtrip(g.json, g.bin, g.png, base, mesh, eo, d.align->dims);
            files.push_back({beside(".bin"), std::move(g.bin)});
            files.push_back({beside(".png"), std::move(g.png)});
            files.push_back({req.path, bytes_of(g.json)});
            break;
          }
          case ExportFormat::Obj: {
            ObjFiles o = write_obj(mesh, eo, base);
            check = check_obj_roundtrip(o.obj, o.mtl, o.png, base, mesh, eo, d.align->dims);
            files.push_back({beside(".mtl"), bytes_of(o.mtl)});
            files.push_back({beside(".png"), std::move(o.png)});
            files.push_back({req.path, bytes_of(o.obj)});
            break;
          }
        }
        if (!check.ok)
          return OpResult::fail(Message{"export.roundtrip_failed",
                                        "the exported " + std::string(to_string(format)) + " did not reload identically",
                                        Severity::Error, to_json(check)});
        nlohmann::json written = nlohmann::json::array();
        if (!req.path.empty()) {
          for (const auto& [path, bytes] : files) {
            const Message w = write_file_atomic(path, bytes);
            if (w.code != "ok") {
              // All or nothing: companions already written are removed again.
              for (const auto& done : written) {
                std::error_code ec;
                std::filesystem::remove(path_from_utf8(done.get<std::string>()), ec);
              }
              return OpResult::fail(w);
            }
            written.push_back(to_utf8(std::filesystem::path(path).make_preferred()));
          }
        }
        if (status != ValidationStatus::Pass)
          r.add(Message{"export.status_accepted",
                        "exported a " + std::string(to_string(status)) + " model because the caller accepted it",
                        Severity::Warning, why});
        if (d.styled && format != ExportFormat::Glb)
          r.add(Message{"export.style_maps_dropped",
                        std::string(to_string(format)) + " carries the polished geometry and colours only: the normal "
                        "and occlusion maps and the per-family materials are exported in GLB",
                        Severity::Warning, {{"style", to_string(d.styled->preset)}}});
        if (format == ExportFormat::Obj)
          r.add(Message{"export.obj_sampling",
                        "OBJ cannot store texture filtering: set point (nearest) filtering in the importer to keep pixels crisp",
                        Severity::Info, {}});
        // Size of the model in the output unit (bbox of the exported coordinates).
        const ExportFrame frame = export_frame(mesh, eo);
        vec3d lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
        for (const vec3d& p : mesh.positions) {
          const vec3d q = frame.apply(p);
          lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
          hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
        }
        r.data = {{"path", to_utf8(req.path)},
                  {"written", !req.path.empty()},
                  {"files", written},
                  {"bytes", files.back().second.size()},
                  {"format", to_string(format)},
                  {"preset", to_string(req.preset)},
                  {"preset_note", preset.note},
                  {"units", to_string(eo.units)},
                  {"front", eo.front_negative_z ? "-z" : "+z"},
                  {"unlit", eo.unlit},
                  {"size", {hi.x - lo.x, hi.y - lo.y, hi.z - lo.z}},
                  {"pixels_per_meter", eo.pixels_per_meter},
                  {"status", to_string(status)},
                  {"check", to_json(check)}};
        if (format == ExportFormat::Glb) r.data["glb"] = to_json(check);
        if (d.styled) r.data["style"] = to_json(*d.styled);
        r.success = true;
        r.changed = {"export"};
        return r;
      });
}

}  // namespace pixel3d
