#include "pixel3d/lowpoly/skinned_gltf.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include <nlohmann/json.hpp>
#define TINYGLTF_NO_INCLUDE_JSON
#define TINYGLTF_NO_INCLUDE_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_EXTERNAL_IMAGE
#include <tiny_gltf.h>

#include "pixel3d/common/version.hpp"
#include "pixel3d/lowpoly/style_render.hpp"

namespace pixel3d::lowpoly {
namespace {

constexpr uint32_t kGlbMagic = 0x46546C67, kChunkJson = 0x4E4F534A, kChunkBin = 0x004E4942;
constexpr int kNearest = 9728, kClampToEdge = 33071;
constexpr int kFloat = 5126, kUByte = 5121, kUShort = 5123, kUInt = 5125;
constexpr int kArrayBuffer = 34962, kElementArrayBuffer = 34963;

template <class T>
void put(std::vector<uint8_t>& b, T v) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  b.insert(b.end(), p, p + sizeof(T));
}
void pad4(std::vector<uint8_t>& b) {
  while (b.size() % 4) b.push_back(0);
}

std::vector<double> read_floats(const tinygltf::Model& m, int index, int* comps, std::string* err) {
  std::vector<double> out;
  if (index < 0 || index >= static_cast<int>(m.accessors.size())) {
    *err = "missing accessor";
    return out;
  }
  const auto& a = m.accessors[static_cast<size_t>(index)];
  if (a.bufferView < 0) {
    *err = "accessor without bufferView";
    return out;
  }
  const auto& bv = m.bufferViews[static_cast<size_t>(a.bufferView)];
  const auto& buf = m.buffers[static_cast<size_t>(bv.buffer)];
  const int nc = tinygltf::GetNumComponentsInType(a.type), cs = tinygltf::GetComponentSizeInBytes(a.componentType);
  *comps = nc;
  const size_t stride = bv.byteStride ? bv.byteStride : static_cast<size_t>(nc * cs), base = bv.byteOffset + a.byteOffset;
  if (base + (a.count ? (a.count - 1) * stride : 0) + static_cast<size_t>(nc * cs) > buf.data.size()) {
    *err = "accessor reads past its buffer";
    return out;
  }
  for (size_t i = 0; i < a.count; ++i)
    for (int c = 0; c < nc; ++c) {
      const uint8_t* p = buf.data.data() + base + i * stride + static_cast<size_t>(c * cs);
      switch (a.componentType) {
        case kFloat: {
          float f;
          std::memcpy(&f, p, 4);
          out.push_back(f);
          break;
        }
        case kUByte: out.push_back(*p); break;
        case kUShort: {
          uint16_t v;
          std::memcpy(&v, p, 2);
          out.push_back(v);
          break;
        }
        case kUInt: {
          uint32_t v;
          std::memcpy(&v, p, 4);
          out.push_back(v);
          break;
        }
        default: *err = "unexpected component type"; return {};
      }
    }
  return out;
}

}  // namespace

ExportFrame skinned_frame(const SkinnedMesh& m, const ExportOptions& opt) {
  TexturedMesh t;
  t.positions = m.positions;
  return export_frame(t, opt);
}

std::vector<uint8_t> write_skinned_glb(const SkinnedMesh& m, const Skeleton& s, const ImageRGBA& texture,
                                       const SkinnedExportOptions& opt) {
  const ExportFrame F = skinned_frame(m, opt.frame);
  const size_t nv = m.positions.size(), nb = s.bones.size();
  std::vector<uint8_t> bin;
  nlohmann::json views = nlohmann::json::array(), accessors = nlohmann::json::array();
  auto view = [&](size_t start, int target) {
    nlohmann::json v{{"buffer", 0}, {"byteOffset", start}, {"byteLength", bin.size() - start}};
    if (target) v["target"] = target;
    views.push_back(v);
    return static_cast<int>(views.size() - 1);
  };
  // POSITION
  size_t start = bin.size();
  std::array<float, 3> mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
  for (const vec3d& p0 : m.positions) {
    const vec3d p = F.apply(p0);
    const float c[3] = {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)};
    for (int k = 0; k < 3; ++k) {
      put(bin, c[k]);
      mn[static_cast<size_t>(k)] = std::min(mn[static_cast<size_t>(k)], c[k]);
      mx[static_cast<size_t>(k)] = std::max(mx[static_cast<size_t>(k)], c[k]);
    }
  }
  accessors.push_back({{"bufferView", view(start, kArrayBuffer)}, {"componentType", kFloat}, {"count", nv}, {"type", "VEC3"}, {"min", mn}, {"max", mx}});
  // NORMAL
  start = bin.size();
  for (const vec3d& n0 : m.normals) {
    const vec3d n = normalize(F.apply_dir(n0));
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
  // JOINTS_0 (bytes while the skeleton fits) — unused slots: joint 0, weight 0.
  start = bin.size();
  const bool small_joints = nb <= 256;
  for (size_t v = 0; v < nv; ++v)
    for (int k = 0; k < 4; ++k) {
      const uint16_t j = m.weights[v][static_cast<size_t>(k)] > 0 ? m.joints[v][static_cast<size_t>(k)] : 0;
      small_joints ? put(bin, static_cast<uint8_t>(j)) : put(bin, j);
    }
  pad4(bin);
  accessors.push_back({{"bufferView", view(start, kArrayBuffer)}, {"componentType", small_joints ? kUByte : kUShort}, {"count", nv}, {"type", "VEC4"}});
  // WEIGHTS_0 (normalised in float)
  start = bin.size();
  for (size_t v = 0; v < nv; ++v) {
    float sum = 0;
    for (float w : m.weights[v]) sum += std::max(0.f, w);
    for (int k = 0; k < 4; ++k) put(bin, sum > 0 ? std::max(0.f, m.weights[v][static_cast<size_t>(k)]) / sum : (k == 0 ? 1.f : 0.f));
  }
  accessors.push_back({{"bufferView", view(start, kArrayBuffer)}, {"componentType", kFloat}, {"count", nv}, {"type", "VEC4"}});
  const int a_pos = 0, a_nrm = 1, a_uv = 2, a_jnt = 3, a_wgt = 4;
  // indices: character, then hull
  const size_t split = m.hull_appended ? m.hull_first_index : m.indices.size();
  const bool small_idx = nv < 65536;
  auto put_indices = [&](size_t i0, size_t i1) {
    const size_t st = bin.size();
    for (size_t i = i0; i < i1; ++i) small_idx ? put(bin, static_cast<uint16_t>(m.indices[i])) : put(bin, m.indices[i]);
    pad4(bin);
    accessors.push_back({{"bufferView", view(st, kElementArrayBuffer)}, {"componentType", small_idx ? kUShort : kUInt}, {"count", i1 - i0}, {"type", "SCALAR"}});
    return static_cast<int>(accessors.size() - 1);
  };
  const int a_idx = put_indices(0, split);
  const int a_hull = m.hull_appended ? put_indices(split, m.indices.size()) : -1;
  // inverse bind matrices (column-major): inverse(T(head')) = T(−head')
  start = bin.size();
  for (const Bone& b : s.bones) {
    const vec3d h = F.apply(b.head);
    const float mat[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, static_cast<float>(-h.x), static_cast<float>(-h.y), static_cast<float>(-h.z), 1};
    for (float f : mat) put(bin, f);
  }
  accessors.push_back({{"bufferView", view(start, 0)}, {"componentType", kFloat}, {"count", nb}, {"type", "MAT4"}});
  const int a_ibm = static_cast<int>(accessors.size() - 1);
  // image
  const std::vector<uint8_t> png = encode_png(texture);
  start = bin.size();
  bin.insert(bin.end(), png.begin(), png.end());
  const int v_img = view(start, 0);
  pad4(bin);

  nlohmann::json j;
  j["asset"] = {{"version", "2.0"}, {"generator", "Pixel3D " + std::string(version()) + " (low-poly studio)"}};
  if (!opt.report.is_null()) j["asset"]["extras"] = {{"pixel3d_lowpoly", opt.report}};
  // Nodes: 0 = the skinned mesh (a scene root: parent transforms never apply to
  // skinned meshes), 1… = joints (bone i → node 1 + i); root joints are scene roots too.
  nlohmann::json nodes = nlohmann::json::array();
  nlohmann::json roots = nlohmann::json::array({0});
  nodes.push_back({{"name", "pixel3d_character"}, {"mesh", 0}, {"skin", 0}});
  int skeleton_root = -1;
  for (size_t b = 0; b < nb; ++b) {
    const Bone& bone = s.bones[b];
    const vec3d h = F.apply(bone.head);
    const vec3d ph = bone.parent >= 0 ? F.apply(s.bones[static_cast<size_t>(bone.parent)].head) : vec3d{};
    const vec3d t = h - ph;
    nlohmann::json n{{"name", bone.name}, {"translation", {t.x, t.y, t.z}}};
    nlohmann::json kids = nlohmann::json::array();
    for (size_t c = 0; c < nb; ++c)
      if (s.bones[c].parent == static_cast<int>(b)) kids.push_back(1 + c);
    if (!kids.empty()) n["children"] = kids;
    nodes.push_back(n);
    if (bone.parent < 0) {
      roots.push_back(1 + b);
      if (skeleton_root < 0) skeleton_root = static_cast<int>(1 + b);
    }
  }
  j["nodes"] = nodes;
  j["scene"] = 0;
  j["scenes"] = {{{"nodes", roots}}};
  nlohmann::json joints = nlohmann::json::array();
  for (size_t b = 0; b < nb; ++b) joints.push_back(1 + b);
  j["skins"] = {{{"name", "pixel3d_skeleton"}, {"inverseBindMatrices", a_ibm}, {"joints", joints}, {"skeleton", skeleton_root}}};
  nlohmann::json attrs{{"POSITION", a_pos}, {"NORMAL", a_nrm}, {"TEXCOORD_0", a_uv}, {"JOINTS_0", a_jnt}, {"WEIGHTS_0", a_wgt}};
  nlohmann::json prims = nlohmann::json::array({{{"attributes", attrs}, {"indices", a_idx}, {"material", 0}, {"mode", 4}}});
  if (a_hull >= 0) {
    nlohmann::json hull_attrs = attrs;  // the black hull samples no texture
    hull_attrs.erase("TEXCOORD_0");
    prims.push_back({{"attributes", hull_attrs}, {"indices", a_hull}, {"material", 1}, {"mode", 4}});
  }
  j["meshes"] = {{{"name", "pixel3d_character"}, {"primitives", prims}}};
  const bool unlit = opt.style == Style::Flat || opt.baked_cel;
  nlohmann::json style{{"style", to_string(opt.style)},
                       {"tones", opt.tones},
                       {"ramp", opt.tones <= 2 ? nlohmann::json::array({cel_factor(-1, 2), 1.0}) : nlohmann::json::array({cel_factor(-1, 3), cel_factor(0.3, 3), 1.0})},
                       {"ramp_thresholds", opt.tones <= 2 ? nlohmann::json::array({0.15}) : nlohmann::json::array({0.05, 0.55})},
                       {"outline", {{"enabled", opt.style == Style::Outline || opt.style == Style::CelOutline}, {"width_px", opt.outline_width_px}, {"color", {20, 18, 28}}, {"method", m.hull_appended ? "inverted_hull" : "engine"}}},
                       {"baked_cel", opt.baked_cel},
                       {"light_dir", {-0.4, 0.8, 0.45}}};
  nlohmann::json mat0{{"name", "pixel3d_lowpoly"},
                      {"pbrMetallicRoughness", {{"baseColorTexture", {{"index", 0}}}, {"metallicFactor", 0.0}, {"roughnessFactor", 1.0}}},
                      {"alphaMode", "OPAQUE"},
                      {"doubleSided", false},
                      {"extras", {{"pixel3d_style", style}}}};
  nlohmann::json used = nlohmann::json::array();
  if (unlit) {
    mat0["extensions"] = {{"KHR_materials_unlit", nlohmann::json::object()}};
    used.push_back("KHR_materials_unlit");
  }
  nlohmann::json mats = nlohmann::json::array({mat0});
  if (a_hull >= 0) {
    mats.push_back({{"name", "pixel3d_outline_hull"},
                    {"pbrMetallicRoughness", {{"baseColorFactor", {20 / 255.0, 18 / 255.0, 28 / 255.0, 1.0}}, {"metallicFactor", 0.0}, {"roughnessFactor", 1.0}}},
                    {"extensions", {{"KHR_materials_unlit", nlohmann::json::object()}}}});
    if (used.empty()) used.push_back("KHR_materials_unlit");
  }
  if (!used.empty()) j["extensionsUsed"] = used;
  j["materials"] = mats;
  j["samplers"] = {{{"magFilter", kNearest}, {"minFilter", kNearest}, {"wrapS", kClampToEdge}, {"wrapT", kClampToEdge}}};
  j["textures"] = {{{"sampler", 0}, {"source", 0}}};
  j["images"] = {{{"mimeType", "image/png"}, {"bufferView", v_img}}};
  j["accessors"] = accessors;
  j["bufferViews"] = views;
  j["buffers"] = {{{"byteLength", bin.size()}}};
  std::string js = j.dump();
  while (js.size() % 4) js.push_back(' ');
  std::vector<uint8_t> out;
  put(out, kGlbMagic);
  put(out, uint32_t{2});
  put(out, static_cast<uint32_t>(12 + 8 + js.size() + 8 + bin.size()));
  put(out, static_cast<uint32_t>(js.size()));
  put(out, kChunkJson);
  out.insert(out.end(), js.begin(), js.end());
  put(out, static_cast<uint32_t>(bin.size()));
  put(out, kChunkBin);
  out.insert(out.end(), bin.begin(), bin.end());
  return out;
}

nlohmann::json to_json(const SkinnedCheck& c) {
  return {{"ok", c.ok},           {"problems", c.problems},           {"vertices", c.vertices},
          {"triangles", c.triangles}, {"joints", c.joints},          {"primitives", c.primitives},
          {"max_weight_sum_error", c.max_weight_sum_error}, {"max_ibm_error", c.max_ibm_error},
          {"max_attribute_error", c.max_attribute_error}};
}

SkinnedCheck check_skinned_glb(std::span<const uint8_t> glb, const SkinnedMesh& e, const Skeleton& s, const SkinnedExportOptions& opt) {
  SkinnedCheck c;
  tinygltf::TinyGLTF loader;
  tinygltf::Model model;
  std::string err, warn;
  if (!loader.LoadBinaryFromMemory(&model, &err, &warn, glb.data(), static_cast<unsigned int>(glb.size()))) {
    c.problems.push_back("tinygltf could not load the GLB: " + err);
    return c;
  }
  if (model.meshes.size() != 1 || model.skins.size() != 1) {
    c.problems.push_back("expected one mesh and one skin");
    return c;
  }
  const auto& mesh = model.meshes[0];
  c.primitives = static_cast<int>(mesh.primitives.size());
  if (c.primitives != (e.hull_appended ? 2 : 1)) c.problems.push_back("unexpected primitive count");
  const ExportFrame F = skinned_frame(e, opt.frame);
  const auto& prim = mesh.primitives[0];
  for (const char* a : {"POSITION", "NORMAL", "TEXCOORD_0", "JOINTS_0", "WEIGHTS_0"})
    if (!prim.attributes.count(a)) c.problems.push_back(std::string("missing attribute ") + a);
  if (!c.problems.empty()) return c;
  int nc = 0;
  const auto pos = read_floats(model, prim.attributes.at("POSITION"), &nc, &err);
  const auto nrm = read_floats(model, prim.attributes.at("NORMAL"), &nc, &err);
  const auto uv = read_floats(model, prim.attributes.at("TEXCOORD_0"), &nc, &err);
  const auto jn = read_floats(model, prim.attributes.at("JOINTS_0"), &nc, &err);
  const auto wt = read_floats(model, prim.attributes.at("WEIGHTS_0"), &nc, &err);
  if (!err.empty()) {
    c.problems.push_back(err);
    return c;
  }
  const size_t nv = e.positions.size();
  c.vertices = static_cast<int>(pos.size() / 3);
  if (pos.size() != 3 * nv || nrm.size() != 3 * nv || uv.size() != 2 * nv || jn.size() != 4 * nv || wt.size() != 4 * nv) {
    c.problems.push_back("attribute counts differ from the mesh");
    return c;
  }
  for (size_t v = 0; v < nv; ++v) {
    const vec3d p = F.apply(e.positions[v]);
    c.max_attribute_error = std::max({c.max_attribute_error, std::abs(pos[3 * v] - p.x), std::abs(pos[3 * v + 1] - p.y), std::abs(pos[3 * v + 2] - p.z),
                                      std::abs(uv[2 * v] - e.uvs[v].x), std::abs(uv[2 * v + 1] - e.uvs[v].y)});
    double sum = 0;
    for (int k = 0; k < 4; ++k) {
      const double w = wt[4 * v + static_cast<size_t>(k)];
      sum += w;
      if (w > 0) {
        const int jj = static_cast<int>(jn[4 * v + static_cast<size_t>(k)]);
        if (jj != e.joints[v][static_cast<size_t>(k)]) c.problems.push_back("joint index differs at vertex " + std::to_string(v));
        if (jj < 0 || static_cast<size_t>(jj) >= s.bones.size()) c.problems.push_back("joint index out of range");
        if (std::abs(w - e.weights[v][static_cast<size_t>(k)]) > 1e-5) c.problems.push_back("weight differs at vertex " + std::to_string(v));
      }
    }
    c.max_weight_sum_error = std::max(c.max_weight_sum_error, std::abs(sum - 1));
    if (c.problems.size() > 20) break;
  }
  if (c.max_attribute_error > 1e-4) c.problems.push_back("attributes differ after the float round trip");
  if (c.max_weight_sum_error > 1e-5) c.problems.push_back("weights do not sum to 1");
  // Indices.
  std::vector<double> idx = read_floats(model, prim.indices, &nc, &err);
  size_t total_idx = idx.size();
  if (e.hull_appended) total_idx += read_floats(model, mesh.primitives[1].indices, &nc, &err).size();
  c.triangles = static_cast<int>(total_idx / 3);
  if (total_idx != e.indices.size()) c.problems.push_back("index count differs");
  for (size_t i = 0; i < idx.size() && i < e.indices.size(); ++i)
    if (static_cast<uint32_t>(idx[i]) != e.indices[i]) {
      c.problems.push_back("indices differ");
      break;
    }
  // Skin: joints in bone order, IBM · global rest transform = identity.
  const auto& skin = model.skins[0];
  c.joints = static_cast<int>(skin.joints.size());
  if (skin.joints.size() != s.bones.size()) c.problems.push_back("joint count differs from the skeleton");
  std::vector<int> parent(model.nodes.size(), -1);
  for (size_t n = 0; n < model.nodes.size(); ++n)
    for (int ch : model.nodes[n].children) parent[static_cast<size_t>(ch)] = static_cast<int>(n);
  auto global_t = [&](int node) {
    vec3d t{};
    for (int n = node; n >= 0; n = parent[static_cast<size_t>(n)]) {
      const auto& nd = model.nodes[static_cast<size_t>(n)];
      if (!nd.rotation.empty() || !nd.scale.empty() || !nd.matrix.empty()) c.problems.push_back("joint rest pose has rotation/scale");
      if (nd.translation.size() == 3) t = t + vec3d{nd.translation[0], nd.translation[1], nd.translation[2]};
    }
    return t;
  };
  const auto ibm = read_floats(model, skin.inverseBindMatrices, &nc, &err);
  if (ibm.size() != 16 * skin.joints.size()) {
    c.problems.push_back("inverseBindMatrices count differs");
  } else {
    for (size_t b = 0; b < skin.joints.size() && b < s.bones.size(); ++b) {
      const vec3d g = global_t(skin.joints[b]);
      const double* M = &ibm[16 * b];
      // IBM (column-major) applied to the joint's global translation must give the origin.
      for (int r = 0; r < 3; ++r) {
        const double v = M[r] * g.x + M[4 + r] * g.y + M[8 + r] * g.z + M[12 + r];
        c.max_ibm_error = std::max(c.max_ibm_error, std::abs(v));
      }
      const vec3d expect = F.apply(s.bones[b].head);
      c.max_ibm_error = std::max(c.max_ibm_error, length(g - expect));
      if (model.nodes[static_cast<size_t>(skin.joints[b])].name != s.bones[b].name) c.problems.push_back("joint name differs");
    }
    if (c.max_ibm_error > 1e-4) c.problems.push_back("inverse bind matrices do not invert the joint rest transforms");
  }
  if (model.textures.empty() || model.images.empty() || model.images[0].width <= 0) c.problems.push_back("texture missing");
  c.ok = c.problems.empty();
  return c;
}

}  // namespace pixel3d::lowpoly
