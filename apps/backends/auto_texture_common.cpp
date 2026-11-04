#include "auto_texture.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <stdexcept>
#include "pixel3d/render/camera.hpp"

namespace p3dasset::autotex {
using namespace pixel3d;

Deadline Deadline::in_seconds(double s) {
  return {std::chrono::steady_clock::now() + std::chrono::milliseconds(int64_t(s * 1000))};
}
double Deadline::remaining() const {
  return std::chrono::duration<double>(end - std::chrono::steady_clock::now()).count();
}
void Deadline::check(const char* stage) const {
  if (remaining() < 0) throw BudgetExceeded(std::string("Time budget exceeded during ") + stage);
}

float linear_from_srgb8(uint8_t v) {
  static const auto table = [] {
    std::array<float, 256> t{};
    for (int i = 0; i < 256; ++i) {
      const double x = i / 255.0;
      t[size_t(i)] = float(x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4));
    }
    return t;
  }();
  return table[v];
}
uint8_t srgb8_from_linear(float v) {
  const double x = std::clamp(double(v), 0.0, 1.0);
  return uint8_t(std::lround(255 * (x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1 / 2.4) - 0.055)));
}
std::array<float, 3> oklab(const std::array<float, 4>& c) {
  const double r = c[0], g = c[1], b = c[2];
  const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
  const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
  const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
  return {float(0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s),
          float(1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s),
          float(0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s)};
}

std::array<float, 4> linear_from_oklab(const std::array<float, 3>& lab) {
  const double l_ = lab[0] + 0.3963377774 * lab[1] + 0.2158037573 * lab[2];
  const double m_ = lab[0] - 0.1055613458 * lab[1] - 0.0638541728 * lab[2];
  const double s_ = lab[0] - 0.0894841775 * lab[1] - 1.2914855480 * lab[2];
  const double l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
  return {float(std::max(0.0, 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s)),
          float(std::max(0.0, -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s)),
          float(std::max(0.0, -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s)), 1.f};
}

Source Source::from(const ImageRGBA& image, bool crisp) {
  if (image.width() <= 0 || image.height() <= 0 || image.width() > 4096 || image.height() > 4096)
    throw std::runtime_error("Source picture missing or larger than 4096 px");
  Source s;
  s.width = image.width();
  s.height = image.height();
  s.crisp = crisp;
  s.px.resize(size_t(s.width) * s.height);
  for (int y = 0; y < s.height; ++y)
    for (int x = 0; x < s.width; ++x) {
      const auto p = image.at(x, y);
      s.px[size_t(y) * s.width + x] = {linear_from_srgb8(p.r), linear_from_srgb8(p.g), linear_from_srgb8(p.b), p.a / 255.f};
    }
  return s;
}
std::array<float, 4> Source::sample(double x, double y) const {
  if (crisp) {
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    if (ix < 0 || iy < 0 || ix >= width || iy >= height) return {0, 0, 0, 0};
    return px[size_t(iy) * width + ix];
  }
  x -= 0.5;
  y -= 0.5;
  const int ix = int(std::floor(x)), iy = int(std::floor(y));
  const double fx = x - ix, fy = y - iy;
  double r = 0, g = 0, b = 0, a = 0;
  for (int dy = 0; dy < 2; ++dy)
    for (int dx = 0; dx < 2; ++dx) {
      const int sx = ix + dx, sy = iy + dy;
      if (sx < 0 || sy < 0 || sx >= width || sy >= height) continue;
      const auto& p = px[size_t(sy) * width + sx];
      const double w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy) * p[3];
      r += p[0] * w;
      g += p[1] * w;
      b += p[2] * w;
      a += w;
    }
  if (a <= 1e-9) return {0, 0, 0, 0};
  return {float(r / a), float(g / a), float(b / a), float(a)};
}

std::array<float, 4> Source::sample_sharp(double x, double y) const {
  const auto bl = sample(x, y);
  if (crisp || bl[3] <= 1e-6f) return bl;
  // Catmull-Rom over premultiplied colour (keeps edges as sharp as the picture is when the atlas has more
  // texels than the picture has pixels), clamped to the 2x2 neighbourhood so it never rings or halos.
  x -= 0.5;
  y -= 0.5;
  const int ix = int(std::floor(x)), iy = int(std::floor(y));
  const double fx = x - ix, fy = y - iy;
  auto cr = [](double t, double w[4]) {
    const double t2 = t * t, t3 = t2 * t;
    w[0] = -0.5 * t3 + t2 - 0.5 * t;
    w[1] = 1.5 * t3 - 2.5 * t2 + 1;
    w[2] = -1.5 * t3 + 2 * t2 + 0.5 * t;
    w[3] = 0.5 * t3 - 0.5 * t2;
  };
  double wx[4], wy[4];
  cr(fx, wx);
  cr(fy, wy);
  double c[4]{}, lo[3]{1e9, 1e9, 1e9}, hi[3]{-1e9, -1e9, -1e9};
  for (int dy = -1; dy <= 2; ++dy)
    for (int dx = -1; dx <= 2; ++dx) {
      const int sx = std::clamp(ix + dx, 0, width - 1), sy = std::clamp(iy + dy, 0, height - 1);
      const auto& p = px[size_t(sy) * width + sx];
      const double w = wx[dx + 1] * wy[dy + 1];
      for (int d = 0; d < 3; ++d) c[d] += p[size_t(d)] * p[3] * w;
      c[3] += p[3] * w;
      if (dx >= 0 && dx <= 1 && dy >= 0 && dy <= 1 && p[3] > 0.05f)
        for (int d = 0; d < 3; ++d) { lo[d] = std::min(lo[d], double(p[size_t(d)])); hi[d] = std::max(hi[d], double(p[size_t(d)])); }
    }
  if (c[3] <= 1e-6 || lo[0] > hi[0]) return bl;
  std::array<float, 4> out;
  for (int d = 0; d < 3; ++d) out[size_t(d)] = float(std::clamp(c[d] / c[3], lo[d], hi[d]));
  out[3] = bl[3];
  return out;
}

Pose Pose::make(double yaw_deg, double el_deg) {
  Pose p;
  p.yaw = yaw_deg;
  p.elevation = el_deg;
  const double yaw = yaw_deg * std::numbers::pi / 180, el = el_deg * std::numbers::pi / 180;
  const vec3d to_cam{std::sin(yaw) * std::cos(el), std::sin(el), std::cos(yaw) * std::cos(el)};
  p.forward = -to_cam;
  p.right = normalize(cross(p.forward, vec3d{0, 1, 0}));
  p.up = cross(p.right, p.forward);
  return p;
}

namespace {
struct Writer {
  nlohmann::json doc;
  std::vector<uint8_t> bin;
  int view(const void* data, size_t bytes, int target) {
    while (bin.size() % 4) bin.push_back(0);
    nlohmann::json v{{"buffer", 0}, {"byteOffset", bin.size()}, {"byteLength", bytes}};
    if (target) v["target"] = target;
    const auto* p = static_cast<const uint8_t*>(data);
    bin.insert(bin.end(), p, p + bytes);
    doc["bufferViews"].push_back(v);
    return int(doc["bufferViews"].size()) - 1;
  }
  int accessor(const void* data, size_t bytes, int component, const char* type, size_t count, int target,
               const nlohmann::json& min = nullptr, const nlohmann::json& max = nullptr) {
    nlohmann::json a{{"bufferView", view(data, bytes, target)}, {"componentType", component}, {"type", type}, {"count", count}};
    if (!min.is_null()) a["min"] = min;
    if (!max.is_null()) a["max"] = max;
    doc["accessors"].push_back(a);
    return int(doc["accessors"].size()) - 1;
  }
};
nlohmann::json bounds(const std::vector<float>& xyz, bool want_max) {
  std::array<float, 3> r{xyz[0], xyz[1], xyz[2]};
  for (size_t i = 0; i < xyz.size(); i += 3)
    for (int c = 0; c < 3; ++c) r[size_t(c)] = want_max ? std::max(r[size_t(c)], xyz[i + size_t(c)]) : std::min(r[size_t(c)], xyz[i + size_t(c)]);
  return {r[0], r[1], r[2]};
}
}  // namespace

std::vector<uint8_t> write_glb(const Mesh& logical, const TexturedMesh& m, const nlohmann::json& provenance) {
  if (!m.texture || m.positions.empty() || m.indices.empty() || m.uvs.size() != m.positions.size() || m.normals.size() != m.positions.size())
    throw std::runtime_error("Incomplete textured mesh");
  Writer w;
  w.doc = {{"asset", {{"version", "2.0"}, {"generator", "3re Auto Texture"}}}, {"scene", 0}, {"scenes", {{{"nodes", {0}}}}},
           {"nodes", {{{"mesh", 0}}}}, {"bufferViews", nlohmann::json::array()}, {"accessors", nlohmann::json::array()}};
  std::vector<float> pos, nrm, uv;
  for (size_t i = 0; i < m.positions.size(); ++i) {
    const auto p = m.positions[i];
    auto n = normalize(m.normals[i]);
    if (!std::isfinite(n.x) || length(n) < 0.5) n = {0, 0, 1};
    pos.insert(pos.end(), {float(p.x), float(p.y), float(p.z)});
    // Renormalise in float so the stored normal is unit length within glTF tolerance.
    const float fx = float(n.x), fy = float(n.y), fz = float(n.z), fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    nrm.insert(nrm.end(), {fx / fl, fy / fl, fz / fl});
    uv.insert(uv.end(), {float(m.uvs[i].x), float(m.uvs[i].y)});
  }
  std::vector<uint32_t> idx(m.indices.begin(), m.indices.end());
  const int pa = w.accessor(pos.data(), pos.size() * 4, 5126, "VEC3", m.positions.size(), 34962, bounds(pos, false), bounds(pos, true));
  const int na = w.accessor(nrm.data(), nrm.size() * 4, 5126, "VEC3", m.positions.size(), 34962);
  const int ta = w.accessor(uv.data(), uv.size() * 4, 5126, "VEC2", m.positions.size(), 34962);
  const int ia = w.accessor(idx.data(), idx.size() * 4, 5125, "SCALAR", idx.size(), 34963);
  std::vector<float> lpos;
  for (auto p : logical.positions) lpos.insert(lpos.end(), {float(p.x), float(p.y), float(p.z)});
  std::vector<uint32_t> lidx;
  for (const auto& t : logical.tris) lidx.insert(lidx.end(), t.begin(), t.end());
  const int lpa = w.accessor(lpos.data(), lpos.size() * 4, 5126, "VEC3", logical.positions.size(), 0, bounds(lpos, false), bounds(lpos, true));
  const int lia = w.accessor(lidx.data(), lidx.size() * 4, 5125, "SCALAR", lidx.size(), 0);
  const auto png = encode_png(*m.texture);
  const int image_view = w.view(png.data(), png.size(), 0);
  w.doc["images"] = {{{"bufferView", image_view}, {"mimeType", "image/png"}, {"name", "base_color"}}};
  w.doc["samplers"] = {{{"magFilter", 9729}, {"minFilter", 9729}, {"wrapS", 33071}, {"wrapT", 33071}}};
  w.doc["textures"] = {{{"sampler", 0}, {"source", 0}}};
  bool blend = false;
  for (const auto& px : m.texture->pixels())
    if (px.a < 250) { blend = true; break; }
  w.doc["materials"] = {{{"name", "auto_texture"},
                         {"pbrMetallicRoughness", {{"baseColorTexture", {{"index", 0}}}, {"metallicFactor", 0.0}, {"roughnessFactor", blend ? 0.25 : 0.85}}}}};
  if (blend) w.doc["materials"][0]["alphaMode"] = "BLEND";  // glass: opacity from TRELLIS's volume
  nlohmann::json primitive{{"attributes", {{"POSITION", pa}, {"NORMAL", na}, {"TEXCOORD_0", ta}}}, {"indices", ia}, {"material", 0}, {"mode", 4}};
  w.doc["meshes"] = nlohmann::json::array({nlohmann::json{{"primitives", nlohmann::json::array({primitive})}}});
  w.doc["asset"]["extras"]["pixel3d_topology"] = {{"position_accessor", lpa}, {"index_accessor", lia}};
  w.doc["asset"]["extras"]["pixel3d_appearance"] = provenance;
  while (w.bin.size() % 4) w.bin.push_back(0);
  w.doc["buffers"] = {{{"byteLength", w.bin.size()}}};
  std::string js = w.doc.dump();
  while (js.size() % 4) js += ' ';
  std::vector<uint8_t> out;
  auto word = [&](uint32_t x) { uint8_t b[4]; std::memcpy(b, &x, 4); out.insert(out.end(), b, b + 4); };
  word(0x46546c67);
  word(2);
  word(uint32_t(28 + js.size() + w.bin.size()));
  word(uint32_t(js.size()));
  word(0x4e4f534a);
  out.insert(out.end(), js.begin(), js.end());
  word(uint32_t(w.bin.size()));
  word(0x004e4942);
  out.insert(out.end(), w.bin.begin(), w.bin.end());
  return out;
}

ImageRGBA render_view(const TexturedMesh& m, double yaw, double elevation, int size) {
  vec3d lo = m.positions.front(), hi = lo;
  for (auto p : m.positions) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  const vec3d centre = (lo + hi) * 0.5;
  const double radius = std::max(length(hi - lo) / 2, 1e-9);
  const Pose pose = Pose::make(yaw, elevation);
  const int ss = 2;
  Camera c;
  c.forward = pose.forward;
  c.right = pose.right;
  c.up = pose.up;
  c.eye = centre - c.forward * radius * 2;
  c.width = c.height = size * ss;
  c.scale = c.width * 0.46 / radius;
  c.offset = {c.width / 2.0, c.height / 2.0};
  RenderOptions o;
  o.lit = true;
  o.background = {46, 48, 54, 255};
  o.light_dir = normalize(-pose.forward + pose.up * 0.9 - pose.right * 0.35);
  const auto big = render(m, c, o);
  ImageRGBA out(size, size);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      int r = 0, g = 0, b = 0;
      for (int dy = 0; dy < ss; ++dy)
        for (int dx = 0; dx < ss; ++dx) {
          const auto p = big.color.at(x * ss + dx, y * ss + dy);
          r += p.r;
          g += p.g;
          b += p.b;
        }
      out.at(x, y) = {uint8_t(r / (ss * ss)), uint8_t(g / (ss * ss)), uint8_t(b / (ss * ss)), 255};
    }
  return out;
}
}  // namespace p3dasset::autotex
