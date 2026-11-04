#include "pixel3d/io/glb_writer.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include <nlohmann/json.hpp>

#include "pixel3d/common/version.hpp"

namespace pixel3d {
namespace {

constexpr uint32_t kGlbMagic = 0x46546C67;  // "glTF"
constexpr uint32_t kChunkJson = 0x4E4F534A;
constexpr uint32_t kChunkBin = 0x004E4942;
constexpr int kNearest = 9728, kNearestMipmapLinear = 9986, kClampToEdge = 33071;
constexpr int kFloat = 5126, kUShort = 5123, kUInt = 5125;
constexpr int kArrayBuffer = 34962, kElementArrayBuffer = 34963;

void put_u32(std::vector<uint8_t>& b, uint32_t v) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  b.insert(b.end(), p, p + 4);
}

template <class T>
void put(std::vector<uint8_t>& b, T v) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  b.insert(b.end(), p, p + sizeof(T));
}

void pad4(std::vector<uint8_t>& b, uint8_t fill) {
  while (b.size() % 4) b.push_back(fill);
}

struct Built {
  nlohmann::json json;
  std::vector<uint8_t> bin, png;
  std::vector<std::pair<std::string, std::vector<uint8_t>>> extra_png;  // polished maps (.gltf companions)
};

// Polished styles: shared vertex attributes (POSITION, NORMAL, TEXCOORD_0 and,
// when present, TEXCOORD_1 + TANGENT), one primitive per material, the base
// atlas (NEAREST) plus normal / occlusion maps (LINEAR, texCoord 1).
Built build_polished(const TexturedMesh& m, const ExportOptions& opt, bool embed_image, const std::string& base) {
  constexpr int kLinear = 9729, kLinearMipmapLinear = 9987;
  const StyleExport& st = *opt.style;
  const ExportFrame frame = export_frame(m, opt);
  const size_t nv = m.positions.size(), nt = m.indices.size() / 3;
  Built b;
  std::vector<uint8_t>& bin = b.bin;
  nlohmann::json views = nlohmann::json::array(), accessors = nlohmann::json::array();
  auto view = [&](size_t start, int target) {
    nlohmann::json v{{"buffer", 0}, {"byteOffset", start}, {"byteLength", bin.size() - start}};
    if (target) v["target"] = target;
    views.push_back(v);
    return static_cast<int>(views.size() - 1);
  };
  nlohmann::json attributes;
  size_t start = bin.size();
  std::array<float, 3> mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
  for (const vec3d& p : m.positions) {
    const vec3d q = frame.apply(p);
    const float c[3] = {static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z)};
    for (int k = 0; k < 3; ++k) {
      put(bin, c[k]);
      mn[k] = std::min(mn[k], c[k]);
      mx[k] = std::max(mx[k], c[k]);
    }
  }
  accessors.push_back({{"bufferView", view(start, kArrayBuffer)}, {"componentType", kFloat}, {"count", nv},
                       {"type", "VEC3"}, {"min", mn}, {"max", mx}});
  attributes["POSITION"] = accessors.size() - 1;
  start = bin.size();
  for (const vec3d& n0 : m.normals) {
    const vec3d n = frame.apply_dir(n0);
    put(bin, static_cast<float>(n.x));
    put(bin, static_cast<float>(n.y));
    put(bin, static_cast<float>(n.z));
  }
  accessors.push_back({{"bufferView", view(start, kArrayBuffer)}, {"componentType", kFloat}, {"count", nv}, {"type", "VEC3"}});
  attributes["NORMAL"] = accessors.size() - 1;
  auto uv_accessor = [&](const std::vector<vec2d>& uvs) {
    const size_t s0 = bin.size();
    for (const vec2d& uv : uvs) {
      put(bin, static_cast<float>(uv.x));
      put(bin, static_cast<float>(uv.y));
    }
    accessors.push_back({{"bufferView", view(s0, kArrayBuffer)}, {"componentType", kFloat}, {"count", nv}, {"type", "VEC2"}});
    return accessors.size() - 1;
  };
  attributes["TEXCOORD_0"] = uv_accessor(m.uvs);
  const bool has_uv1 = m.uvs1.size() == nv;
  const bool maps = has_uv1 && (m.normal_map || m.occlusion);
  if (maps) attributes["TEXCOORD_1"] = uv_accessor(m.uvs1);
  if (maps && m.normal_map && m.tangents.size() == nv) {
    start = bin.size();
    for (size_t i = 0; i < nv; ++i) {
      const vec3d n = frame.apply_dir(m.normals[i]);
      vec3d t = frame.apply_dir({m.tangents[i][0], m.tangents[i][1], m.tangents[i][2]});
      t = normalize(t - n * dot(t, n));  // glTF: unit length, orthogonal to the normal
      if (length(t) == 0) t = normalize(cross(std::abs(n.y) < 0.9 ? vec3d{0, 1, 0} : vec3d{1, 0, 0}, n));
      put(bin, static_cast<float>(t.x));
      put(bin, static_cast<float>(t.y));
      put(bin, static_cast<float>(t.z));
      put(bin, static_cast<float>(m.tangents[i][3] < 0 ? -1.0 : 1.0));
    }
    accessors.push_back({{"bufferView", view(start, kArrayBuffer)}, {"componentType", kFloat}, {"count", nv}, {"type", "VEC4"}});
    attributes["TANGENT"] = accessors.size() - 1;
  }
  // One index accessor per material that has triangles, in material order.
  const size_t nm = std::max<size_t>(1, st.materials.size());
  auto material_of = [&](size_t t) {
    const int k = t < st.tri_material.size() ? st.tri_material[t] : 0;
    return k >= 0 && static_cast<size_t>(k) < nm ? static_cast<size_t>(k) : size_t{0};
  };
  const bool small = nv < 65536;
  nlohmann::json primitives = nlohmann::json::array();
  for (size_t k = 0; k < nm; ++k) {
    start = bin.size();
    size_t count = 0;
    for (size_t t = 0; t < nt; ++t) {
      if (material_of(t) != k) continue;
      for (int c = 0; c < 3; ++c) {
        if (small) put(bin, static_cast<uint16_t>(m.indices[3 * t + c]));
        else put(bin, m.indices[3 * t + c]);
      }
      count += 3;
    }
    pad4(bin, 0);
    if (count == 0) continue;
    accessors.push_back({{"bufferView", view(start, kElementArrayBuffer)}, {"componentType", small ? kUShort : kUInt},
                         {"count", count}, {"type", "SCALAR"}});
    primitives.push_back({{"attributes", attributes}, {"indices", accessors.size() - 1}, {"material", k}, {"mode", 4}});
  }
  // Images: base atlas, then the maps.
  nlohmann::json images = nlohmann::json::array();
  auto add_image = [&](const ImageRGBA& img, const std::string& suffix, std::vector<uint8_t>* keep) {
    std::vector<uint8_t> png = encode_png(img);
    nlohmann::json image{{"mimeType", "image/png"}};
    if (embed_image) {
      const size_t s0 = bin.size();
      bin.insert(bin.end(), png.begin(), png.end());
      image["bufferView"] = view(s0, 0);
      pad4(bin, 0);
    } else {
      image = {{"uri", base + suffix + ".png"}};
    }
    if (keep) *keep = std::move(png);
    else b.extra_png.push_back({suffix, std::move(png)});
    images.push_back(image);
    return static_cast<int>(images.size() - 1);
  };
  nlohmann::json textures = nlohmann::json::array();
  const int base_img = m.texture ? add_image(*m.texture, "", &b.png) : -1;
  if (base_img >= 0) textures.push_back({{"sampler", 0}, {"source", base_img}});
  int normal_tex = -1, occl_tex = -1;
  if (maps && m.normal_map) {
    textures.push_back({{"sampler", 1}, {"source", add_image(*m.normal_map, "_normal", nullptr)}});
    normal_tex = static_cast<int>(textures.size() - 1);
  }
  if (maps && m.occlusion) {
    textures.push_back({{"sampler", 1}, {"source", add_image(*m.occlusion, "_occlusion", nullptr)}});
    occl_tex = static_cast<int>(textures.size() - 1);
  }
  nlohmann::json materials = nlohmann::json::array();
  std::vector<std::string> used;
  auto use = [&](const char* ext) {
    if (std::find(used.begin(), used.end(), ext) == used.end()) used.push_back(ext);
  };
  for (size_t k = 0; k < nm; ++k) {
    const StyleMaterial sm = k < st.materials.size() ? st.materials[k] : StyleMaterial{"default"};
    nlohmann::json pbr{{"metallicFactor", sm.metallic}, {"roughnessFactor", sm.roughness}};
    if (base_img >= 0) pbr["baseColorTexture"] = {{"index", 0}};
    nlohmann::json mat{{"name", sm.name}, {"pbrMetallicRoughness", pbr}, {"alphaMode", "OPAQUE"}, {"doubleSided", false}};
    if (normal_tex >= 0) mat["normalTexture"] = {{"index", normal_tex}, {"texCoord", 1}};
    if (occl_tex >= 0) mat["occlusionTexture"] = {{"index", occl_tex}, {"texCoord", 1}};
    if (sm.emissive > 0) {
      const double f = std::min(1.0, sm.emissive);
      mat["emissiveFactor"] = {f, f, f};
      if (base_img >= 0) mat["emissiveTexture"] = {{"index", 0}};
      if (sm.emissive > 1) {
        mat["extensions"]["KHR_materials_emissive_strength"] = {{"emissiveStrength", sm.emissive}};
        use("KHR_materials_emissive_strength");
      }
    }
    if (opt.unlit) {
      mat["extensions"]["KHR_materials_unlit"] = nlohmann::json::object();
      use("KHR_materials_unlit");
    }
    materials.push_back(mat);
  }
  nlohmann::json& j = b.json;
  j["asset"] = {{"version", "2.0"}, {"generator", opt.generator.empty() ? "Pixel3D " + std::string(version()) : opt.generator}};
  j["scene"] = 0;
  j["scenes"] = {{{"nodes", {0}}}};
  j["nodes"] = {{{"mesh", 0}, {"name", "pixel3d_model"}}};
  j["meshes"] = {{{"name", "pixel3d_model"}, {"primitives", primitives}}};
  if (!used.empty()) j["extensionsUsed"] = used;
  j["materials"] = materials;
  j["samplers"] = {{{"magFilter", kNearest},
                    {"minFilter", opt.mipmaps ? kNearestMipmapLinear : kNearest},
                    {"wrapS", kClampToEdge},
                    {"wrapT", kClampToEdge}},
                   {{"magFilter", kLinear}, {"minFilter", kLinearMipmapLinear}, {"wrapS", kClampToEdge}, {"wrapT", kClampToEdge}}};
  j["textures"] = textures;
  j["images"] = images;
  j["accessors"] = accessors;
  j["bufferViews"] = views;
  nlohmann::json buffer{{"byteLength", bin.size()}};
  if (!embed_image) buffer["uri"] = base + ".bin";
  j["buffers"] = {buffer};
  return b;
}


// The glTF document and its binary buffer. `embed_image`: the PNG goes into the
// buffer (GLB); otherwise the image refers to `<base>.png` and the buffer to `<base>.bin`.
Built build(const TexturedMesh& m, const ExportOptions& opt, bool embed_image, const std::string& base) {
  if (opt.style) return build_polished(m, opt, embed_image, base);
  const ExportFrame frame = export_frame(m, opt);
  const size_t nv = m.positions.size();
  Built b;
  std::vector<uint8_t>& bin = b.bin;
  nlohmann::json views = nlohmann::json::array(), accessors = nlohmann::json::array();
  auto view = [&](size_t start, int target) {
    nlohmann::json v{{"buffer", 0}, {"byteOffset", start}, {"byteLength", bin.size() - start}};
    if (target) v["target"] = target;
    views.push_back(v);
    return static_cast<int>(views.size() - 1);
  };

  // POSITION (with required min/max)
  size_t start = bin.size();
  std::array<float, 3> mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
  for (const vec3d& p : m.positions) {
    const vec3d q = frame.apply(p);
    const float c[3] = {static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z)};
    for (int k = 0; k < 3; ++k) {
      put(bin, c[k]);
      mn[k] = std::min(mn[k], c[k]);
      mx[k] = std::max(mx[k], c[k]);
    }
  }
  accessors.push_back({{"bufferView", view(start, kArrayBuffer)}, {"componentType", kFloat}, {"count", nv},
                       {"type", "VEC3"}, {"min", mn}, {"max", mx}});
  // NORMAL
  start = bin.size();
  for (const vec3d& n0 : m.normals) {
    const vec3d n = frame.apply_dir(n0);
    put(bin, static_cast<float>(n.x));
    put(bin, static_cast<float>(n.y));
    put(bin, static_cast<float>(n.z));
  }
  accessors.push_back({{"bufferView", view(start, kArrayBuffer)}, {"componentType", kFloat}, {"count", nv}, {"type", "VEC3"}});
  // TEXCOORD_0
  start = bin.size();
  for (const vec2d& uv : m.uvs) {
    put(bin, static_cast<float>(uv.x));
    put(bin, static_cast<float>(uv.y));
  }
  accessors.push_back({{"bufferView", view(start, kArrayBuffer)}, {"componentType", kFloat}, {"count", nv}, {"type", "VEC2"}});
  // indices
  start = bin.size();
  const bool small = nv < 65536;
  for (uint32_t i : m.indices) small ? put(bin, static_cast<uint16_t>(i)) : put(bin, i);
  pad4(bin, 0);
  accessors.push_back({{"bufferView", view(start, kElementArrayBuffer)}, {"componentType", small ? kUShort : kUInt},
                       {"count", m.indices.size()}, {"type", "SCALAR"}});
  // image
  if (m.texture) b.png = encode_png(*m.texture);
  nlohmann::json image{{"mimeType", "image/png"}};
  if (embed_image) {
    start = bin.size();
    bin.insert(bin.end(), b.png.begin(), b.png.end());
    image["bufferView"] = view(start, 0);
    pad4(bin, 0);
  } else {
    image = {{"uri", base + ".png"}};
  }

  nlohmann::json& j = b.json;
  j["asset"] = {{"version", "2.0"}, {"generator", opt.generator.empty() ? "Pixel3D " + std::string(version()) : opt.generator}};
  j["scene"] = 0;
  j["scenes"] = {{{"nodes", {0}}}};
  j["nodes"] = {{{"mesh", 0}, {"name", "pixel3d_model"}}};
  j["meshes"] = {{{"name", "pixel3d_model"},
                  {"primitives",
                   {{{"attributes", {{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}}},
                     {"indices", 3},
                     {"material", 0},
                     {"mode", 4}}}}}};
  nlohmann::json material{{"name", "pixel_art"},
                          {"pbrMetallicRoughness",
                           {{"baseColorTexture", {{"index", 0}}}, {"metallicFactor", 0.0}, {"roughnessFactor", 1.0}}},
                          {"alphaMode", "OPAQUE"},
                          {"doubleSided", false}};
  if (opt.unlit) {
    material["extensions"] = {{"KHR_materials_unlit", nlohmann::json::object()}};
    j["extensionsUsed"] = {"KHR_materials_unlit"};
  }
  j["materials"] = {material};
  j["samplers"] = {{{"magFilter", kNearest},
                    {"minFilter", opt.mipmaps ? kNearestMipmapLinear : kNearest},
                    {"wrapS", kClampToEdge},
                    {"wrapT", kClampToEdge}}};
  j["textures"] = {{{"sampler", 0}, {"source", 0}}};
  j["images"] = {image};
  j["accessors"] = accessors;
  j["bufferViews"] = views;
  nlohmann::json buffer{{"byteLength", bin.size()}};
  if (!embed_image) buffer["uri"] = base + ".bin";
  j["buffers"] = {buffer};
  return b;
}

}  // namespace

std::string_view to_string(ExportUnits u) {
  switch (u) {
    case ExportUnits::Metres: return "metres";
    case ExportUnits::Centimetres: return "centimetres";
    case ExportUnits::Studs: return "studs";
    case ExportUnits::Pixels: return "pixels";
  }
  return "metres";
}

std::optional<ExportUnits> export_units_from_string(std::string_view s) {
  if (s == "metres" || s == "meters" || s == "m") return ExportUnits::Metres;
  if (s == "centimetres" || s == "centimeters" || s == "cm") return ExportUnits::Centimetres;
  if (s == "studs") return ExportUnits::Studs;
  if (s == "pixels" || s == "px") return ExportUnits::Pixels;
  return std::nullopt;
}

ExportFrame export_frame(const TexturedMesh& m, const ExportOptions& opt) {
  double scale = 1.0 / opt.pixels_per_meter;  // metres per source pixel
  switch (opt.units) {
    case ExportUnits::Metres: break;
    case ExportUnits::Centimetres: scale *= 100.0; break;
    case ExportUnits::Studs: scale /= kMetresPerStud; break;
    case ExportUnits::Pixels: scale = 1.0; break;
  }
  ExportFrame f{{0, 0, 0}, scale, opt.front_negative_z};
  if (opt.center_bottom && !m.positions.empty()) {
    vec3d lo = m.positions[0], hi = m.positions[0];
    for (const vec3d& p : m.positions) {
      lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
      hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    f.origin = {(lo.x + hi.x) / 2, lo.y, (lo.z + hi.z) / 2};
  }
  return f;
}

std::vector<uint8_t> write_glb(const TexturedMesh& m, const ExportOptions& opt) {
  Built b = build(m, opt, true, {});
  std::string js = b.json.dump();
  while (js.size() % 4) js.push_back(' ');
  std::vector<uint8_t> out;
  put_u32(out, kGlbMagic);
  put_u32(out, 2);
  put_u32(out, static_cast<uint32_t>(12 + 8 + js.size() + 8 + b.bin.size()));
  put_u32(out, static_cast<uint32_t>(js.size()));
  put_u32(out, kChunkJson);
  out.insert(out.end(), js.begin(), js.end());
  put_u32(out, static_cast<uint32_t>(b.bin.size()));
  put_u32(out, kChunkBin);
  out.insert(out.end(), b.bin.begin(), b.bin.end());
  return out;
}

GltfFiles write_gltf(const TexturedMesh& m, const ExportOptions& opt, const std::string& base_name) {
  Built b = build(m, opt, false, base_name);
  return GltfFiles{b.json.dump(2), std::move(b.bin), std::move(b.png), std::move(b.extra_png)};
}

}  // namespace pixel3d
