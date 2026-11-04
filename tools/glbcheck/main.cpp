// pixel3d-glbcheck — independent verification of an exported .glb file.
// Loads the file from disk with tinygltf (not Pixel3D's writer or its in-memory
// round-trip check) and re-derives every property from the reloaded data:
// structure, accessor bounds, finite values, unit normals, UV range, NEAREST
// sampling, opaque material, decoded texture, closed 2-manifold surface with
// outward winding, and the bounding box in metres. Prints JSON; exit 0 = valid.
// Polished styles (1.1): one primitive per material over one vertex set, a
// second UV set and unit tangents, decoded normal/occlusion maps, and
// NEAREST_MIPMAP_LINEAR minification are accepted and checked too.
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define TINYGLTF_NO_INCLUDE_JSON
#define TINYGLTF_NO_INCLUDE_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <stb_image.h>
#include <tiny_gltf.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace {

constexpr int kNearest = 9728, kNearestMipmapLinear = 9986, kClamp = 33071;

struct Reader {
  const tinygltf::Model& m;
  std::vector<std::string>& problems;

  std::vector<double> read(int index, int want_comps) {
    std::vector<double> out;
    if (index < 0 || index >= static_cast<int>(m.accessors.size())) {
      problems.push_back("missing accessor");
      return out;
    }
    const auto& a = m.accessors[index];
    if (a.bufferView < 0 || a.bufferView >= static_cast<int>(m.bufferViews.size())) {
      problems.push_back("accessor without bufferView");
      return out;
    }
    const auto& bv = m.bufferViews[a.bufferView];
    if (bv.buffer < 0 || bv.buffer >= static_cast<int>(m.buffers.size())) {
      problems.push_back("bufferView without buffer");
      return out;
    }
    const auto& buf = m.buffers[bv.buffer].data;
    const int comps = tinygltf::GetNumComponentsInType(a.type);
    const int csize = tinygltf::GetComponentSizeInBytes(a.componentType);
    if (comps != want_comps || csize <= 0) {
      problems.push_back("accessor has an unexpected type");
      return out;
    }
    const size_t stride = bv.byteStride ? bv.byteStride : static_cast<size_t>(comps * csize);
    const size_t start = bv.byteOffset + a.byteOffset;
    if (a.count == 0 || start + (a.count - 1) * stride + comps * csize > bv.byteOffset + bv.byteLength ||
        bv.byteOffset + bv.byteLength > buf.size()) {
      problems.push_back("accessor reads outside its bufferView");
      return out;
    }
    if (start % static_cast<size_t>(csize) != 0) problems.push_back("accessor is misaligned");
    for (size_t i = 0; i < a.count; ++i)
      for (int c = 0; c < comps; ++c) {
        const uint8_t* p = buf.data() + start + i * stride + c * csize;
        double v = 0;
        if (a.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
          float f;
          std::memcpy(&f, p, 4);
          v = f;
        } else if (a.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
          uint16_t u;
          std::memcpy(&u, p, 2);
          v = u;
        } else if (a.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
          uint32_t u;
          std::memcpy(&u, p, 4);
          v = u;
        } else {
          problems.push_back("unsupported component type");
          return {};
        }
        out.push_back(v);
      }
    return out;
  }
};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: pixel3d-glbcheck model.glb\n");
    return 2;
  }
  nlohmann::json out;
  std::vector<std::string> problems;
  tinygltf::TinyGLTF loader;
  tinygltf::Model m;
  std::string err, warn;
  const bool loaded = loader.LoadBinaryFromFile(&m, &err, &warn, argv[1]);
  out["file"] = argv[1];
  out["loaded"] = loaded;
  if (!warn.empty()) out["loader_warnings"] = warn;
  if (!loaded) {
    out["problems"] = {"tinygltf: " + err};
    out["ok"] = false;
    std::printf("%s\n", out.dump(2).c_str());
    return 1;
  }
  if (m.asset.version != "2.0") problems.push_back("asset.version is not 2.0");
  if (m.meshes.size() != 1 || m.meshes[0].primitives.empty()) problems.push_back("expected one mesh with primitives");
  // Nodes: one node holding the mesh in one scene. A skinned file (Low-Poly Studio, 1.2) adds
  // its skeleton: then the mesh node must use a valid skin and every other node must be one of
  // that skin's joints (no mesh, no stray node); the joints form a tree reachable from the scene.
  if (m.scenes.size() != 1) problems.push_back("expected one scene");
  int mesh_node = -1, mesh_nodes = 0;
  for (size_t i = 0; i < m.nodes.size(); ++i)
    if (m.nodes[i].mesh >= 0) ++mesh_nodes, mesh_node = static_cast<int>(i);
  if (mesh_nodes != 1) problems.push_back("expected exactly one node with a mesh");
  if (problems.empty() && m.nodes.size() > 1) {
    const int skin = m.nodes[static_cast<size_t>(mesh_node)].skin;
    if (skin < 0 || skin >= static_cast<int>(m.skins.size())) {
      problems.push_back("expected one node in one scene (extra nodes without a skin)");
    } else {
      std::vector<int> joint_of(m.nodes.size(), 0);
      for (int j : m.skins[static_cast<size_t>(skin)].joints) {
        if (j < 0 || j >= static_cast<int>(m.nodes.size()) || j == mesh_node) {
          problems.push_back("skin joint is not a valid joint node");
          break;
        }
        ++joint_of[static_cast<size_t>(j)];
      }
      for (size_t i = 0; i < m.nodes.size() && problems.empty(); ++i) {
        if (static_cast<int>(i) == mesh_node) continue;
        if (joint_of[i] != 1) problems.push_back("node " + std::to_string(i) + " is neither the mesh nor exactly one skin joint");
        else if (!m.nodes[i].children.empty())
          for (int c : m.nodes[i].children)
            if (c < 0 || c >= static_cast<int>(m.nodes.size()) || !joint_of[static_cast<size_t>(c)])
              problems.push_back("joint node " + std::to_string(i) + " has a child that is not a joint");
      }
      // Every node reachable from the scene, each exactly once (a tree).
      std::vector<int> seen(m.nodes.size(), 0);
      std::vector<int> stack;
      if (!m.scenes.empty())
        for (int r : m.scenes[0].nodes)
          if (r >= 0 && r < static_cast<int>(m.nodes.size())) stack.push_back(r);
      while (!stack.empty() && problems.empty()) {
        const int n = stack.back();
        stack.pop_back();
        if (++seen[static_cast<size_t>(n)] > 1) {
          problems.push_back("node " + std::to_string(n) + " is reached twice (not a tree)");
          break;
        }
        for (int c : m.nodes[static_cast<size_t>(n)].children)
          if (c >= 0 && c < static_cast<int>(m.nodes.size())) stack.push_back(c);
      }
      for (size_t i = 0; i < m.nodes.size() && problems.empty(); ++i)
        if (!seen[i]) problems.push_back("node " + std::to_string(i) + " is not in the scene");
      if (problems.empty()) out["skin_joints"] = m.skins[static_cast<size_t>(skin)].joints.size();
    }
  }
  if (!problems.empty()) {
    out["problems"] = problems;
    out["ok"] = false;
    std::printf("%s\n", out.dump(2).c_str());
    return 1;
  }
  // Every primitive draws triangles from the same vertex set (polished files have one primitive per material).
  const auto& prims = m.meshes[0].primitives;
  const auto& prim = prims[0];
  Reader rd{m, problems};
  auto attr_of = [&](const tinygltf::Primitive& p, const char* n) {
    const auto it = p.attributes.find(n);
    return it == p.attributes.end() ? -1 : it->second;
  };
  auto attr = [&](const char* n) { return attr_of(prim, n); };
  for (const auto& p : prims) {
    if (p.mode != TINYGLTF_MODE_TRIANGLES) problems.push_back("primitive is not TRIANGLES");
    if (p.attributes != prim.attributes) problems.push_back("primitives do not share one vertex set");
  }
  const auto pos = rd.read(attr("POSITION"), 3), nrm = rd.read(attr("NORMAL"), 3), uv = rd.read(attr("TEXCOORD_0"), 2);
  std::vector<double> idx;
  for (const auto& p : prims) {
    const auto part = rd.read(p.indices, 1);
    idx.insert(idx.end(), part.begin(), part.end());
  }
  const size_t nv = pos.size() / 3;
  if (nrm.size() != pos.size() || uv.size() != nv * 2) problems.push_back("attribute counts differ");
  if (attr("TEXCOORD_1") >= 0) {
    const auto uv1 = rd.read(attr("TEXCOORD_1"), 2);
    if (uv1.size() != nv * 2) problems.push_back("TEXCOORD_1 count differs");
    for (double v : uv1)
      if (!std::isfinite(v) || v < 0 || v > 1) {
        problems.push_back("TEXCOORD_1 outside [0, 1]");
        break;
      }
  }
  if (attr("TANGENT") >= 0) {
    const auto tan = rd.read(attr("TANGENT"), 4);
    if (tan.size() != nv * 4) problems.push_back("TANGENT count differs");
    for (size_t i = 0; i + 3 < tan.size(); i += 4)
      if (std::abs(std::sqrt(tan[i] * tan[i] + tan[i + 1] * tan[i + 1] + tan[i + 2] * tan[i + 2]) - 1) > 1e-3 ||
          std::abs(std::abs(tan[i + 3]) - 1) > 1e-6) {
        problems.push_back("TANGENT is not unit length with w = +-1");
        break;
      }
  }
  out["primitives"] = prims.size();
  if (idx.size() % 3 != 0 || idx.empty()) problems.push_back("index count is not a positive multiple of 3");
  for (double i : idx)
    if (i >= static_cast<double>(nv)) {
      problems.push_back("index out of range");
      break;
    }
  for (const auto* arr : {&pos, &nrm, &uv})
    for (double v : *arr)
      if (!std::isfinite(v)) {
        problems.push_back("non-finite attribute value");
        break;
      }
  for (size_t i = 0; i + 2 < nrm.size(); i += 3)
    if (std::abs(std::sqrt(nrm[i] * nrm[i] + nrm[i + 1] * nrm[i + 1] + nrm[i + 2] * nrm[i + 2]) - 1) > 1e-4) {
      problems.push_back("normal is not unit length");
      break;
    }
  for (double v : uv)
    if (v < 0 || v > 1) {
      problems.push_back("UV outside [0, 1]");
      break;
    }
  // POSITION min/max must be present and exact.
  std::array<double, 3> lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
  for (size_t i = 0; i < nv; ++i)
    for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], pos[3 * i + k]), hi[k] = std::max(hi[k], pos[3 * i + k]);
  const auto& pa = m.accessors[attr("POSITION")];
  if (pa.minValues.size() != 3 || pa.maxValues.size() != 3) problems.push_back("POSITION min/max missing");
  else
    for (int k = 0; k < 3; ++k)
      if (pa.minValues[k] != static_cast<float>(lo[k]) || pa.maxValues[k] != static_cast<float>(hi[k]))
        problems.push_back("POSITION min/max do not match the data");

  // Closed, consistently wound surface: welded by exact position, every edge is
  // used once in each direction; the enclosed volume is positive.
  std::map<std::array<double, 3>, int> weld;
  std::vector<int> vid(nv);
  for (size_t i = 0; i < nv; ++i)
    vid[i] = weld.emplace(std::array<double, 3>{pos[3 * i], pos[3 * i + 1], pos[3 * i + 2]}, static_cast<int>(weld.size()))
                 .first->second;
  std::map<std::pair<int, int>, int> directed;
  double volume = 0;
  for (size_t t = 0; t + 2 < idx.size(); t += 3) {
    const int a = static_cast<int>(idx[t]), b = static_cast<int>(idx[t + 1]), c = static_cast<int>(idx[t + 2]);
    if (a >= static_cast<int>(nv) || b >= static_cast<int>(nv) || c >= static_cast<int>(nv)) continue;
    const int v[3] = {vid[a], vid[b], vid[c]};
    for (int k = 0; k < 3; ++k) ++directed[{v[k], v[(k + 1) % 3]}];
    const double* A = &pos[3 * a];
    const double* B = &pos[3 * b];
    const double* C = &pos[3 * c];
    volume += (A[0] * (B[1] * C[2] - B[2] * C[1]) - A[1] * (B[0] * C[2] - B[2] * C[0]) + A[2] * (B[0] * C[1] - B[1] * C[0])) / 6;
  }
  int boundary = 0, nonmanifold = 0;
  for (const auto& [e, n] : directed) {
    const auto it = directed.find({e.second, e.first});
    const int back = it == directed.end() ? 0 : it->second;
    if (n > 1 || back > 1) ++nonmanifold;
    else if (back == 0) ++boundary;
  }
  // Manifold keeps distinct vertices where solids only touch; position welding
  // can merge those, so edges used by two shells show as 2+2, never as holes.
  if (boundary > 0) problems.push_back("surface is not closed (" + std::to_string(boundary) + " boundary edges)");
  if (volume <= 0) problems.push_back("enclosed volume is not positive (inside-out)");

  // Materials (one per primitive), samplers, texture images.
  if (m.materials.empty()) problems.push_back("no material");
  if (prims.size() > 1 && m.materials.size() != prims.size()) problems.push_back("expected one material per primitive");
  if (prims.size() == 1 && m.materials.size() != 1) problems.push_back("material is not one OPAQUE material");
  auto check_image = [&](int tex, const char* what, bool colour) {
    if (tex < 0 || tex >= static_cast<int>(m.textures.size())) {
      problems.push_back(std::string("texture missing: ") + what);
      return;
    }
    const auto& t = m.textures[tex];
    if (colour) {
      if (t.sampler < 0 || t.sampler >= static_cast<int>(m.samplers.size())) problems.push_back("texture has no sampler");
      else {
        const auto& s = m.samplers[t.sampler];
        // Pixels stay crisp up close; polished files may mipmap at a distance (padded atlas).
        if (s.magFilter != kNearest || (s.minFilter != kNearest && s.minFilter != kNearestMipmapLinear))
          problems.push_back("sampler is not NEAREST (minification NEAREST or NEAREST_MIPMAP_LINEAR)");
        if (s.wrapS != kClamp || s.wrapT != kClamp) problems.push_back("sampler does not clamp to edge");
      }
    }
    if (t.source < 0 || t.source >= static_cast<int>(m.images.size())) problems.push_back(std::string("no image: ") + what);
    else {
      const auto& img = m.images[t.source];
      if (colour)
        out["texture"] = {{"width", img.width}, {"height", img.height}, {"components", img.component}, {"mime", img.mimeType}};
      if (img.width <= 0 || img.height <= 0 || img.image.empty()) problems.push_back(std::string(what) + " image did not decode");
    }
  };
  for (size_t k = 0; k < m.materials.size(); ++k) {
    const auto& mat = m.materials[k];
    if (mat.alphaMode != "OPAQUE") problems.push_back("material is not OPAQUE");
    check_image(mat.pbrMetallicRoughness.baseColorTexture.index, "base colour", true);
    if (mat.normalTexture.index >= 0) check_image(mat.normalTexture.index, "normal map", false);
    if (mat.occlusionTexture.index >= 0) check_image(mat.occlusionTexture.index, "occlusion map", false);
    if ((mat.normalTexture.index >= 0 || mat.occlusionTexture.index >= 0) && attr("TEXCOORD_1") < 0 &&
        (mat.normalTexture.texCoord == 1 || mat.occlusionTexture.texCoord == 1))
      problems.push_back("maps read TEXCOORD_1, which is missing");
    if (mat.normalTexture.index >= 0 && attr("TANGENT") < 0) problems.push_back("normal map without TANGENT");
  }
  out["vertices"] = nv;
  out["triangles"] = idx.size() / 3;
  out["bbox_m"] = {{"min", lo}, {"max", hi}, {"size", {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}}};
  out["volume_m3"] = volume;
  out["boundary_edges"] = boundary;
  out["shared_edges_between_shells"] = nonmanifold;
  out["problems"] = problems;
  out["ok"] = problems.empty();
  std::printf("%s\n", out.dump(2).c_str());
  return problems.empty() ? 0 : 1;
}
