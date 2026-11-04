#include "pixel3d/io/glb_check.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>

#define TINYGLTF_NO_INCLUDE_JSON
#define TINYGLTF_NO_INCLUDE_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_EXTERNAL_IMAGE
#include <tiny_gltf.h>

#include "pixel3d/image/image.hpp"
#include "pixel3d/render/camera.hpp"

namespace pixel3d {
namespace {

constexpr int kNearest = 9728, kNearestMipmapLinear = 9986;

// Reads an accessor as floats (FLOAT) or unsigned integers (indices).
std::vector<double> read_accessor(const tinygltf::Model& m, int index, std::string* error) {
  std::vector<double> out;
  if (index < 0 || index >= static_cast<int>(m.accessors.size())) {
    *error = "missing accessor";
    return out;
  }
  const tinygltf::Accessor& a = m.accessors[index];
  if (a.bufferView < 0 || a.bufferView >= static_cast<int>(m.bufferViews.size())) {
    *error = "accessor has no valid bufferView";
    return out;
  }
  const tinygltf::BufferView& bv = m.bufferViews[a.bufferView];
  if (bv.buffer < 0 || bv.buffer >= static_cast<int>(m.buffers.size())) {
    *error = "bufferView has no valid buffer";
    return out;
  }
  const tinygltf::Buffer& buf = m.buffers[bv.buffer];
  const int comps = tinygltf::GetNumComponentsInType(a.type);
  const int csize = tinygltf::GetComponentSizeInBytes(a.componentType);
  const size_t stride = bv.byteStride ? bv.byteStride : static_cast<size_t>(comps * csize);
  const size_t base = bv.byteOffset + a.byteOffset;
  if (base + (a.count ? (a.count - 1) * stride : 0) + comps * csize > buf.data.size()) {
    *error = "accessor reads past the buffer";
    return out;
  }
  for (size_t i = 0; i < a.count; ++i)
    for (int c = 0; c < comps; ++c) {
      const uint8_t* p = buf.data.data() + base + i * stride + c * csize;
      switch (a.componentType) {
        case TINYGLTF_COMPONENT_TYPE_FLOAT: {
          float f;
          std::memcpy(&f, p, 4);
          out.push_back(f);
          break;
        }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
          uint16_t v;
          std::memcpy(&v, p, 2);
          out.push_back(v);
          break;
        }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
          uint32_t v;
          std::memcpy(&v, p, 4);
          out.push_back(v);
          break;
        }
        default: *error = "unexpected component type"; return {};
      }
    }
  return out;
}

}  // namespace

namespace {

// Compares a reloaded mesh (output coordinates) with the exported one, then
// re-renders it back in world units: shared by every format.
void compare_mesh(GlbCheck& c, const std::vector<double>& pos, const std::vector<double>& nrm, const std::vector<double>& uv,
                  const std::vector<double>& idx, const TexturedMesh& expected, const ExportOptions& opt,
                  std::optional<Dims> rerender_dims, double tolerance) {
  auto problem = [&](std::string p) { c.problems.push_back(std::move(p)); };
  c.vertices = static_cast<int>(pos.size() / 3);
  c.triangles = static_cast<int>(idx.size() / 3);
  if (c.vertices != static_cast<int>(expected.positions.size())) problem("vertex count differs");
  if (idx.size() != expected.indices.size()) problem("index count differs");
  const ExportFrame frame = export_frame(expected, opt);
  double miny = 1e30, maxy = -1e30, minx = 1e30, maxx = -1e30;
  if (c.vertices == static_cast<int>(expected.positions.size()) && nrm.size() == pos.size() &&
      uv.size() == expected.uvs.size() * 2 && expected.normals.size() == expected.positions.size()) {
    for (size_t i = 0; i < expected.positions.size(); ++i) {
      const vec3d q = frame.apply(expected.positions[i]);
      const double want[3] = {static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z)};
      for (int k = 0; k < 3; ++k)
        if (!(std::abs(pos[3 * i + k] - want[k]) <= tolerance * std::max(1.0, std::abs(want[k])))) {
          problem("position " + std::to_string(i) + " differs");
          break;
        }
      if (!(std::abs(uv[2 * i] - static_cast<float>(expected.uvs[i].x)) <= std::max(1e-7, tolerance)) ||
          !(std::abs(uv[2 * i + 1] - static_cast<float>(expected.uvs[i].y)) <= std::max(1e-7, tolerance)))
        problem("uv " + std::to_string(i) + " differs");
      const vec3d en = frame.apply_dir(expected.normals[i]);
      const double nx = nrm[3 * i], ny = nrm[3 * i + 1], nz = nrm[3 * i + 2];
      if (!(std::abs(nx - static_cast<float>(en.x)) <= std::max(1e-6, tolerance) &&
            std::abs(ny - static_cast<float>(en.y)) <= std::max(1e-6, tolerance) &&
            std::abs(nz - static_cast<float>(en.z)) <= std::max(1e-6, tolerance)) ||
          !(std::abs(std::sqrt(nx * nx + ny * ny + nz * nz) - 1.0) <= 1e-4))
        problem("normal " + std::to_string(i) + " differs or is not unit length");
      miny = std::min(miny, pos[3 * i + 1]), maxy = std::max(maxy, pos[3 * i + 1]);
      minx = std::min(minx, pos[3 * i]), maxx = std::max(maxx, pos[3 * i]);
      if (c.problems.size() > 8) break;
    }
  } else {
    problem("attribute sizes differ");
  }
  if (idx.size() == expected.indices.size())
    for (size_t i = 0; i < idx.size(); ++i)
      if (static_cast<uint32_t>(idx[i]) != expected.indices[i]) {
        problem("index " + std::to_string(i) + " differs");
        break;
      }
  c.bbox_min_y = miny;
  c.bbox_height = maxy - miny;
  c.bbox_center_x = (minx + maxx) / 2;
  // Re-render the reloaded mesh (back in world units) and compare with the original.
  if (rerender_dims && c.problems.empty()) {
    TexturedMesh back = expected;
    for (size_t i = 0; i < back.positions.size(); ++i) back.positions[i] = frame.invert({pos[3 * i], pos[3 * i + 1], pos[3 * i + 2]});
    for (size_t i = 0; i < back.uvs.size(); ++i) back.uvs[i] = {uv[2 * i], uv[2 * i + 1]};
    // Stored floats move a vertex like k/3 by ~1e-7, which may flip a pixel whose
    // centre lies exactly on an edge or a depth tie. Real export defects (axes,
    // UVs, winding) change far more than a handful of pixels.
    const Camera cam = camera_for_view(ViewRole::Front, *rerender_dims, 1);
    const RenderTarget a = render(back, cam, RenderOptions{}), b = render(expected, cam, RenderOptions{});
    for (int y = 0; y < cam.height; ++y)
      for (int x = 0; x < cam.width; ++x) c.rerender_diff_px += a.color.at(x, y) == b.color.at(x, y) ? 0 : 1;
    c.rerender_identical = c.rerender_diff_px == 0;
    const int allowed = std::max(2, static_cast<int>(0.002 * b.coverage.count()));
    if (c.rerender_diff_px > allowed)
      problem("re-render of the reloaded mesh differs in " + std::to_string(c.rerender_diff_px) + " pixels");
  }
}

void compare_image(GlbCheck& c, const ImageRGBA* got, const TexturedMesh& expected) {
  if (!got) {
    c.problems.push_back("expected exactly one image");
    return;
  }
  c.image_w = got->width();
  c.image_h = got->height();
  if (!expected.texture || got->width() != expected.texture->width() || got->height() != expected.texture->height()) {
    c.problems.push_back("image size differs");
    return;
  }
  for (int y = 0; y < got->height(); ++y)
    for (int x = 0; x < got->width(); ++x)
      if (!(got->at(x, y) == expected.texture->at(x, y))) {
        c.problems.push_back("image pixels differ");
        return;
      }
}

// Embedded PNG of image `index` (GLB), decoded independently of tinygltf.
std::optional<ImageRGBA> embedded_image(const tinygltf::Model& model, int index) {
  if (index < 0 || index >= static_cast<int>(model.images.size())) return std::nullopt;
  const tinygltf::Image& img = model.images[index];
  if (img.bufferView < 0 || img.bufferView >= static_cast<int>(model.bufferViews.size())) return std::nullopt;
  const tinygltf::BufferView& bv = model.bufferViews[img.bufferView];
  if (bv.buffer < 0 || bv.buffer >= static_cast<int>(model.buffers.size())) return std::nullopt;
  const auto& data = model.buffers[bv.buffer].data;
  if (bv.byteOffset + bv.byteLength > data.size()) return std::nullopt;
  auto d = decode_png(std::span<const uint8_t>(data.data() + bv.byteOffset, bv.byteLength));
  if (!d) return std::nullopt;
  return std::move(*d);
}

bool same_image(const std::optional<ImageRGBA>& got, const std::shared_ptr<const ImageRGBA>& want) {
  return got && want && *got == *want;
}

// Polished styles: shared attributes, one primitive per material in material
// order, per-material PBR values, the base atlas and the baked maps.
GlbCheck check_polished(const tinygltf::Model& model, const TexturedMesh& expected, const ExportOptions& opt,
                        std::optional<Dims> rerender_dims) {
  GlbCheck c;
  auto problem = [&](std::string p) { c.problems.push_back(std::move(p)); };
  const StyleExport& st = *opt.style;
  const size_t nm = std::max<size_t>(1, st.materials.size()), nt = expected.indices.size() / 3;
  auto material_of = [&](size_t t) {
    const int k = t < st.tri_material.size() ? st.tri_material[t] : 0;
    return k >= 0 && static_cast<size_t>(k) < nm ? static_cast<size_t>(k) : size_t{0};
  };
  // What the file must hold: triangles grouped by material, in material order.
  TexturedMesh grouped = expected;
  grouped.indices.clear();
  std::vector<size_t> used_materials;
  for (size_t k = 0; k < nm; ++k) {
    bool any = false;
    for (size_t t = 0; t < nt; ++t)
      if (material_of(t) == k) {
        grouped.indices.insert(grouped.indices.end(), expected.indices.begin() + 3 * t, expected.indices.begin() + 3 * t + 3);
        any = true;
      }
    if (any) used_materials.push_back(k);
  }
  if (model.meshes.size() != 1 || model.meshes[0].primitives.size() != used_materials.size()) {
    problem("expected one mesh with one primitive per material");
    return c;
  }
  const auto& prims = model.meshes[0].primitives;
  std::string e;
  std::vector<double> idx;
  for (size_t i = 0; i < prims.size(); ++i) {
    if (prims[i].attributes != prims[0].attributes) problem("primitives do not share their vertex attributes");
    if (prims[i].material != static_cast<int>(used_materials[i])) problem("primitive " + std::to_string(i) + " has the wrong material");
    const auto part = read_accessor(model, prims[i].indices, &e);
    idx.insert(idx.end(), part.begin(), part.end());
  }
  auto attr = [&](const char* name) {
    const auto it = prims[0].attributes.find(name);
    return it == prims[0].attributes.end() ? -1 : it->second;
  };
  const auto pos = read_accessor(model, attr("POSITION"), &e);
  const auto nrm = read_accessor(model, attr("NORMAL"), &e);
  const auto uv = read_accessor(model, attr("TEXCOORD_0"), &e);
  if (!e.empty()) problem("accessor error: " + e);
  const bool maps = expected.uvs1.size() == expected.positions.size() && (expected.normal_map || expected.occlusion);
  if (maps != (attr("TEXCOORD_1") >= 0)) problem("TEXCOORD_1 is missing or unexpected");
  if (maps && attr("TEXCOORD_1") >= 0) {
    const auto uv1 = read_accessor(model, attr("TEXCOORD_1"), &e);
    if (uv1.size() != expected.uvs1.size() * 2) problem("TEXCOORD_1 size differs");
    else
      for (size_t i = 0; i < expected.uvs1.size(); ++i)
        if (std::abs(uv1[2 * i] - static_cast<float>(expected.uvs1[i].x)) > 1e-7 ||
            std::abs(uv1[2 * i + 1] - static_cast<float>(expected.uvs1[i].y)) > 1e-7) {
          problem("TEXCOORD_1 " + std::to_string(i) + " differs");
          break;
        }
  }
  const bool tangents = maps && expected.normal_map && expected.tangents.size() == expected.positions.size();
  if (tangents != (attr("TANGENT") >= 0)) problem("TANGENT is missing or unexpected");
  if (tangents && attr("TANGENT") >= 0) {
    const auto tg = read_accessor(model, attr("TANGENT"), &e);
    if (tg.size() != expected.positions.size() * 4 || nrm.size() != expected.positions.size() * 3) {
      problem("TANGENT size differs");
    } else {
      for (size_t i = 0; i < expected.positions.size(); ++i) {
        const vec3d t{tg[4 * i], tg[4 * i + 1], tg[4 * i + 2]}, n{nrm[3 * i], nrm[3 * i + 1], nrm[3 * i + 2]};
        if (std::abs(length(t) - 1) > 1e-4 || std::abs(dot(t, n)) > 1e-3 || std::abs(std::abs(tg[4 * i + 3]) - 1) > 0) {
          problem("tangent " + std::to_string(i) + " is not a unit vector orthogonal to the normal with w = ±1");
          break;
        }
      }
    }
  }
  // Materials.
  c.material_opaque = model.materials.size() == nm;
  c.unlit = !model.materials.empty();
  if (model.materials.size() != nm) problem("material count differs");
  for (size_t k = 0; k < model.materials.size() && k < nm; ++k) {
    const tinygltf::Material& mt = model.materials[k];
    const StyleMaterial want = k < st.materials.size() ? st.materials[k] : StyleMaterial{"default"};
    c.material_opaque = c.material_opaque && mt.alphaMode == "OPAQUE";
    c.unlit = c.unlit && mt.extensions.contains("KHR_materials_unlit");
    const auto& pbr = mt.pbrMetallicRoughness;
    if (mt.name != want.name || std::abs(pbr.roughnessFactor - want.roughness) > 1e-9 ||
        std::abs(pbr.metallicFactor - want.metallic) > 1e-9)
      problem("material " + std::to_string(k) + " differs from the style's");
    const double glow = std::min(1.0, want.emissive);
    if (mt.emissiveFactor.size() != 3 || std::abs(mt.emissiveFactor[0] - glow) > 1e-9)
      problem("material " + std::to_string(k) + " emissive factor differs");
    const bool strength = mt.extensions.contains("KHR_materials_emissive_strength");
    if (strength != (want.emissive > 1)) problem("material " + std::to_string(k) + " emissive strength differs");
    if ((mt.normalTexture.index >= 0) != (maps && expected.normal_map != nullptr) ||
        (mt.normalTexture.index >= 0 && mt.normalTexture.texCoord != 1))
      problem("material " + std::to_string(k) + " normal map differs");
    if ((mt.occlusionTexture.index >= 0) != (maps && expected.occlusion != nullptr) ||
        (mt.occlusionTexture.index >= 0 && mt.occlusionTexture.texCoord != 1))
      problem("material " + std::to_string(k) + " occlusion map differs");
  }
  if (c.unlit != opt.unlit) problem(opt.unlit ? "the unlit material extension is missing" : "unexpected unlit extension");
  if (!c.material_opaque) problem("a material is not OPAQUE");
  // Base texture: sampler and pixels; maps: pixels.
  if (model.textures.empty() || model.textures[0].sampler < 0 ||
      model.textures[0].sampler >= static_cast<int>(model.samplers.size())) {
    problem("missing base texture");
  } else {
    const tinygltf::Sampler& s = model.samplers[model.textures[0].sampler];
    c.sampler_nearest = s.magFilter == kNearest && s.minFilter == (opt.mipmaps ? kNearestMipmapLinear : kNearest);
    if (!c.sampler_nearest) problem("base texture sampler differs");
    const auto base = embedded_image(model, model.textures[0].source);
    compare_image(c, base ? &*base : nullptr, expected);
  }
  if (!model.materials.empty()) {
    const tinygltf::Material& mt = model.materials[0];
    auto source = [&](int tex) { return tex >= 0 && tex < static_cast<int>(model.textures.size()) ? model.textures[tex].source : -1; };
    if (mt.normalTexture.index >= 0 && !same_image(embedded_image(model, source(mt.normalTexture.index)), expected.normal_map))
      problem("normal map pixels differ");
    if (mt.occlusionTexture.index >= 0 && !same_image(embedded_image(model, source(mt.occlusionTexture.index)), expected.occlusion))
      problem("occlusion map pixels differ");
  }
  if (c.problems.empty()) compare_mesh(c, pos, nrm, uv, idx, grouped, opt, rerender_dims, 1e-6);
  c.ok = c.problems.empty();
  return c;
}

GlbCheck check_model(const tinygltf::Model& model, std::span<const uint8_t> external_png, const TexturedMesh& expected,
                     const ExportOptions& opt, std::optional<Dims> rerender_dims) {
  if (opt.style) return check_polished(model, expected, opt, rerender_dims);
  GlbCheck c;
  auto problem = [&](std::string p) { c.problems.push_back(std::move(p)); };
  if (model.meshes.size() != 1 || model.meshes[0].primitives.size() != 1) {
    problem("expected exactly one mesh with one primitive");
    return c;
  }
  const tinygltf::Primitive& prim = model.meshes[0].primitives[0];
  auto attr = [&](const char* name) {
    const auto it = prim.attributes.find(name);
    return it == prim.attributes.end() ? -1 : it->second;
  };
  std::string e;
  const auto pos = read_accessor(model, attr("POSITION"), &e);
  const auto nrm = read_accessor(model, attr("NORMAL"), &e);
  const auto uv = read_accessor(model, attr("TEXCOORD_0"), &e);
  const auto idx = read_accessor(model, prim.indices, &e);
  if (!e.empty()) problem("accessor error: " + e);
  compare_mesh(c, pos, nrm, uv, idx, expected, opt, std::nullopt, 1e-6);

  // Material, sampler, image.
  c.material_opaque = model.materials.size() == 1 && model.materials[0].alphaMode == "OPAQUE";
  if (!c.material_opaque) problem("material is not a single OPAQUE material");
  c.unlit = model.materials.size() == 1 && model.materials[0].extensions.contains("KHR_materials_unlit");
  if (c.unlit != opt.unlit) problem(opt.unlit ? "the unlit material extension is missing" : "unexpected unlit extension");
  c.sampler_nearest = model.samplers.size() == 1 && model.samplers[0].magFilter == kNearest &&
                      model.samplers[0].minFilter == (opt.mipmaps ? kNearestMipmapLinear : kNearest);
  if (!c.sampler_nearest)
    problem(opt.mipmaps ? "sampler is not NEAREST/NEAREST_MIPMAP_LINEAR" : "sampler is not NEAREST/NEAREST");
  if (model.images.size() != 1) {
    problem("expected exactly one image");
  } else {
    const tinygltf::Image& img = model.images[0];
    std::optional<ImageRGBA> decoded;
    if (!img.image.empty() && img.component == 4) {
      ImageRGBA im(img.width, img.height);
      for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
          const uint8_t* q = &img.image[(static_cast<size_t>(y) * img.width + x) * 4];
          im.at(x, y) = Rgba8{q[0], q[1], q[2], q[3]};
        }
      decoded = std::move(im);
    } else if (!external_png.empty()) {
      if (auto d = decode_png(external_png)) decoded = std::move(*d);
      else problem("the texture file is not a readable PNG");
    }
    compare_image(c, decoded ? &*decoded : nullptr, expected);
  }
  if (c.problems.empty() && rerender_dims) compare_mesh(c, pos, nrm, uv, idx, expected, opt, rerender_dims, 1e-6);
  c.ok = c.problems.empty();
  return c;
}

}  // namespace

GlbCheck check_glb_roundtrip(std::span<const uint8_t> glb, const TexturedMesh& expected, const ExportOptions& opt,
                             std::optional<Dims> rerender_dims) {
  tinygltf::TinyGLTF loader;
  tinygltf::Model model;
  std::string err, warn;
  if (!loader.LoadBinaryFromMemory(&model, &err, &warn, glb.data(), static_cast<unsigned int>(glb.size()))) {
    GlbCheck c;
    c.problems.push_back("tinygltf could not load the GLB: " + err);
    return c;
  }
  return check_model(model, {}, expected, opt, rerender_dims);
}

GlbCheck check_gltf_roundtrip(const std::string& gltf_json, std::span<const uint8_t> bin, std::span<const uint8_t> png,
                              const std::string& base_name, const TexturedMesh& expected, const ExportOptions& opt,
                              std::optional<Dims> rerender_dims) {
  // The companion files come from memory: exactly the bytes that are (or will be) written.
  const std::string bin_name = base_name + ".bin";
  tinygltf::FsCallbacks fs;
  fs.FileExists = [&](const std::string& f, void*) { return f == bin_name || f == base_name + ".png"; };
  fs.ExpandFilePath = [](const std::string& f, void*) { return f; };
  fs.ReadWholeFile = [&](std::vector<unsigned char>* out, std::string* err, const std::string& f, void*) {
    if (f != bin_name) {
      if (err) *err = "unexpected file " + f;
      return false;
    }
    out->assign(bin.begin(), bin.end());
    return true;
  };
  fs.WriteWholeFile = [](std::string*, const std::string&, const std::vector<unsigned char>&, void*) { return false; };
  fs.GetFileSizeInBytes = [&](size_t* size, std::string*, const std::string& f, void*) {
    if (f != bin_name) return false;
    *size = bin.size();
    return true;
  };
  fs.user_data = nullptr;
  tinygltf::TinyGLTF loader;
  GlbCheck c;
  c.format = "gltf";
  std::string err, warn;
  if (!loader.SetFsCallbacks(fs, &err)) {
    c.problems.push_back("tinygltf: " + err);
    return c;
  }
  tinygltf::Model model;
  if (!loader.LoadASCIIFromString(&model, &err, &warn, gltf_json.data(), static_cast<unsigned int>(gltf_json.size()), "")) {
    c.problems.push_back("tinygltf could not load the glTF: " + err);
    return c;
  }
  if (model.buffers.size() != 1 || model.buffers[0].uri != bin_name) c.problems.push_back("the buffer must be " + bin_name);
  if (model.images.size() == 1 && model.images[0].uri != base_name + ".png")
    c.problems.push_back("the image must be " + base_name + ".png");
  if (!c.problems.empty()) return c;
  GlbCheck r = check_model(model, png, expected, opt, rerender_dims);
  r.format = "gltf";
  return r;
}

namespace {

// Minimal OBJ/MTL reader, written independently of obj_writer: v, vt, vn and
// triangular f v/vt/vn faces; mtllib / usemtl / map_Kd.
struct ParsedObj {
  std::vector<double> v, vt, vn;
  std::vector<std::array<int, 3>> corners;  // (v, vt, vn), 0-based
  std::string mtllib, usemtl;
  std::string error;
};

bool parse_number(std::string_view s, double* out) {
  const char* b = s.data();
  const char* e = s.data() + s.size();
  const auto r = std::from_chars(b, e, *out);
  return r.ec == std::errc() && r.ptr == e;
}

std::vector<std::string_view> split_ws(std::string_view line) {
  std::vector<std::string_view> out;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
    const size_t s = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') ++i;
    if (i > s) out.push_back(line.substr(s, i - s));
  }
  return out;
}

ParsedObj parse_obj(const std::string& text) {
  ParsedObj o;
  size_t pos = 0;
  int line_no = 0;
  while (pos <= text.size() && o.error.empty()) {
    const size_t nl = text.find('\n', pos);
    const std::string_view line(text.data() + pos, (nl == std::string::npos ? text.size() : nl) - pos);
    pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
    ++line_no;
    const auto t = split_ws(line);
    if (t.empty() || t[0][0] == '#') continue;
    auto numbers = [&](std::vector<double>& into, size_t n) {
      if (t.size() != n + 1) {
        o.error = "line " + std::to_string(line_no) + ": expected " + std::to_string(n) + " numbers";
        return;
      }
      for (size_t k = 1; k <= n; ++k) {
        double d = 0;
        if (!parse_number(t[k], &d)) o.error = "line " + std::to_string(line_no) + ": bad number";
        into.push_back(d);
      }
    };
    if (t[0] == "v") numbers(o.v, 3);
    else if (t[0] == "vt") numbers(o.vt, 2);
    else if (t[0] == "vn") numbers(o.vn, 3);
    else if (t[0] == "f") {
      if (t.size() != 4) {
        o.error = "line " + std::to_string(line_no) + ": faces must be triangles";
        break;
      }
      for (size_t k = 1; k <= 3; ++k) {
        std::array<int, 3> c{};
        size_t start = 0;
        for (int part = 0; part < 3; ++part) {
          const size_t slash = t[k].find('/', start);
          const std::string_view num = t[k].substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
          int value = 0;
          const auto r = std::from_chars(num.data(), num.data() + num.size(), value);
          if (r.ec != std::errc() || value < 1) {
            o.error = "line " + std::to_string(line_no) + ": face corners must be v/vt/vn";
            break;
          }
          c[static_cast<size_t>(part)] = value - 1;
          start = slash == std::string_view::npos ? t[k].size() : slash + 1;
        }
        o.corners.push_back(c);
      }
    } else if (t[0] == "mtllib" && t.size() >= 2) o.mtllib = std::string(t[1]);
    else if (t[0] == "usemtl" && t.size() >= 2) o.usemtl = std::string(t[1]);
  }
  return o;
}

}  // namespace

GlbCheck check_obj_roundtrip(const std::string& obj, const std::string& mtl, std::span<const uint8_t> png,
                             const std::string& base_name, const TexturedMesh& expected, const ExportOptions& opt,
                             std::optional<Dims> rerender_dims) {
  GlbCheck c;
  c.format = "obj";
  const ParsedObj o = parse_obj(obj);
  if (!o.error.empty()) {
    c.problems.push_back("OBJ: " + o.error);
    return c;
  }
  if (o.mtllib != base_name + ".mtl") c.problems.push_back("mtllib must be " + base_name + ".mtl");
  // The writer emits one v/vt/vn triple per mesh vertex and corners with equal indices.
  std::vector<double> idx;
  for (const auto& corner : o.corners) {
    if (corner[0] != corner[1] || corner[0] != corner[2] || static_cast<size_t>(corner[0]) >= o.v.size() / 3) {
      c.problems.push_back("face corner indices are inconsistent");
      break;
    }
    idx.push_back(corner[0]);
  }
  std::vector<double> uv = o.vt;
  for (size_t i = 1; i < uv.size(); i += 2) uv[i] = 1.0 - uv[i];  // OBJ texture v points up
  // MTL: the material the OBJ uses, its texture.
  bool material = false, texture = false;
  {
    size_t pos = 0;
    while (pos < mtl.size()) {
      const size_t nl = mtl.find('\n', pos);
      const auto t = split_ws(std::string_view(mtl).substr(pos, (nl == std::string::npos ? mtl.size() : nl) - pos));
      pos = nl == std::string::npos ? mtl.size() : nl + 1;
      if (t.size() >= 2 && t[0] == "newmtl" && t[1] == o.usemtl) material = true;
      if (t.size() >= 2 && t[0] == "map_Kd" && t[1] == base_name + ".png") texture = true;
    }
  }
  if (!material) c.problems.push_back("the material named by usemtl is not in the .mtl");
  if (!texture) c.problems.push_back("map_Kd must be " + base_name + ".png");
  // OBJ stores decimal text: the reader compares at the precision the writer prints.
  if (c.problems.empty()) compare_mesh(c, o.v, o.vn, uv, idx, expected, opt, std::nullopt, 2e-6);
  std::optional<ImageRGBA> decoded;
  if (auto d = decode_png(png)) decoded = std::move(*d);
  else c.problems.push_back("the texture file is not a readable PNG");
  compare_image(c, decoded ? &*decoded : nullptr, expected);
  // OBJ/MTL cannot store sampling or an alpha mode: nothing is claimed (export.obj_sampling tells the user).
  c.sampler_nearest = false;
  c.material_opaque = false;
  if (c.problems.empty() && rerender_dims) compare_mesh(c, o.v, o.vn, uv, idx, expected, opt, rerender_dims, 2e-6);
  c.ok = c.problems.empty();
  return c;
}

nlohmann::json to_json(const GlbCheck& c) {
  return {{"ok", c.ok},
          {"format", c.format},
          {"problems", c.problems},
          {"unlit", c.unlit},
          {"vertices", c.vertices},
          {"triangles", c.triangles},
          {"image", {c.image_w, c.image_h}},
          {"sampler_nearest", c.sampler_nearest},
          {"material_opaque", c.material_opaque},
          {"rerender_identical", c.rerender_identical},
          {"rerender_diff_px", c.rerender_diff_px},
          {"bbox_min_y", c.bbox_min_y},
          {"bbox_height", c.bbox_height},
          {"bbox_center_x", c.bbox_center_x}};
}

}  // namespace pixel3d
