#include "trellis_native_post.hpp"
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace p3dasset {
namespace {
void put_u32(std::vector<uint8_t>& o, uint32_t v) {
  for (int i = 0; i < 4; ++i) o.push_back(uint8_t(v >> (8 * i)));
}
void write_all(const std::filesystem::path& path, const std::vector<char>& head, const std::vector<uint8_t>& body) {
  if (std::filesystem::exists(path)) throw std::runtime_error("Refusing to overwrite " + path.string());
  auto tmp = path;
  tmp += ".partial";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(head.data(), std::streamsize(head.size()));
    out.write(reinterpret_cast<const char*>(body.data()), std::streamsize(body.size()));
    if (!out) throw std::runtime_error("Could not write " + path.string());
  }
  std::filesystem::rename(tmp, path);
}
}  // namespace

NativePostSplit split_native_post(const std::filesystem::path& post, const std::filesystem::path& raw_ply,
                                  const std::filesystem::path& raw_glb, int64_t seed) {
  std::error_code ec;
  const uint64_t size = std::filesystem::file_size(post, ec);
  if (ec || size < 16) throw std::runtime_error("The native texture dump is missing");
  std::ifstream in(post, std::ios::binary);
  int32_t h[4]{};
  in.read(reinterpret_cast<char*>(h), sizeof h);
  const int64_t V = h[0], F = h[1], M = h[2];
  if (!in || V <= 0 || F <= 0 || M < 0 || h[3] <= 0 || h[3] > 2048) throw std::runtime_error("The native texture dump has an invalid header");
  if (uint64_t(16 + V * 12 + F * 12 + M * 36) != size) throw std::runtime_error("The native texture dump is truncated");
  std::vector<float> verts(size_t(V) * 3);
  std::vector<int32_t> faces(size_t(F) * 3);
  in.read(reinterpret_cast<char*>(verts.data()), std::streamsize(verts.size() * 4));
  in.read(reinterpret_cast<char*>(faces.data()), std::streamsize(faces.size() * 4));
  if (!in) throw std::runtime_error("Could not read the native texture dump");
  for (auto f : faces)
    if (f < 0 || f >= V) throw std::runtime_error("The native texture dump has an invalid face index");

  // raw.ply — trellis::write_ply(path, verts, V, faces, F, nullptr).
  {
    char header[256];
    const int n = std::snprintf(header, sizeof header,
                                "ply\nformat binary_little_endian 1.0\nelement vertex %lld\nproperty float x\nproperty float y\nproperty float z\n"
                                "element face %lld\nproperty list uchar int vertex_indices\nend_header\n",
                                (long long)V, (long long)F);
    std::vector<uint8_t> body(size_t(V) * 12 + size_t(F) * 13);
    std::memcpy(body.data(), verts.data(), size_t(V) * 12);
    uint8_t* p = body.data() + size_t(V) * 12;
    for (int64_t i = 0; i < F; ++i) {
      *p++ = 3;
      std::memcpy(p, &faces[size_t(i) * 3], 12);
      p += 12;
    }
    write_all(raw_ply, std::vector<char>(header, header + n), body);
  }
  // raw.glb — trellis::write_glb(path, verts, V, faces, F, nullptr, seed): positions (x, z, -y), uint32 indices.
  {
    std::vector<float> pos(size_t(V) * 3);
    float mn[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, mx[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (int64_t i = 0; i < V; ++i) {
      const float q[3] = {verts[size_t(3 * i)], verts[size_t(3 * i + 2)], -verts[size_t(3 * i + 1)]};
      for (int c = 0; c < 3; ++c) {
        pos[size_t(3 * i + c)] = q[c];
        mn[c] = std::min(mn[c], q[c]);
        mx[c] = std::max(mx[c], q[c]);
      }
    }
    const uint32_t pos_bytes = uint32_t(V * 12), idx_bytes = uint32_t(F * 12);
    std::vector<uint8_t> bin(size_t(pos_bytes) + idx_bytes);
    std::memcpy(bin.data(), pos.data(), pos_bytes);
    std::memcpy(bin.data() + pos_bytes, faces.data(), idx_bytes);
    char buf[2048];
    std::snprintf(buf, sizeof buf,
                  "{\"asset\":{\"version\":\"2.0\",\"minVersion\":\"2.0\",\"generator\":\"trellis.cpp v0.8.1\",\"extras\":{\"seed\":%lld,"
                  "\"split_from\":\"native texture POST dump\"}},"
                  "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
                  "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1,\"mode\":4}]}],"
                  "\"accessors\":["
                  "{\"bufferView\":0,\"componentType\":5126,\"count\":%lld,\"type\":\"VEC3\","
                  "\"min\":[%.9g,%.9g,%.9g],\"max\":[%.9g,%.9g,%.9g]},"
                  "{\"bufferView\":1,\"componentType\":5125,\"count\":%lld,\"type\":\"SCALAR\"}],"
                  "\"bufferViews\":["
                  "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":%u,\"target\":34962},"
                  "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34963}],"
                  "\"buffers\":[{\"byteLength\":%u}]}",
                  (long long)seed, (long long)V, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], (long long)(F * 3), pos_bytes, pos_bytes, idx_bytes,
                  pos_bytes + idx_bytes);
    std::string json(buf);
    while (json.size() % 4) json.push_back(' ');
    while (bin.size() % 4) bin.push_back(0);
    std::vector<uint8_t> out;
    put_u32(out, 0x46546C67);
    put_u32(out, 2);
    put_u32(out, uint32_t(12 + 8 + json.size() + 8 + bin.size()));
    put_u32(out, uint32_t(json.size()));
    put_u32(out, 0x4E4F534A);
    out.insert(out.end(), json.begin(), json.end());
    put_u32(out, uint32_t(bin.size()));
    put_u32(out, 0x004E4942);
    out.insert(out.end(), bin.begin(), bin.end());
    write_all(raw_glb, {}, out);
  }
  return {V, F, M, h[3]};
}
}  // namespace p3dasset
