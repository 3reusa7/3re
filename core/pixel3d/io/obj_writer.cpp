#include "pixel3d/io/obj_writer.hpp"

#include <charconv>

#include "pixel3d/common/version.hpp"
#include "pixel3d/image/image.hpp"

namespace pixel3d {
namespace {

// Locale-independent shortest round-trip text of a float (the value glTF would store).
void put_number(std::string& out, double v) {
  char buf[32];
  const auto r = std::to_chars(buf, buf + sizeof buf, static_cast<float>(v));
  out.append(buf, r.ptr);
}

}  // namespace

ObjFiles write_obj(const TexturedMesh& m, const ExportOptions& opt, const std::string& base_name) {
  const ExportFrame frame = export_frame(m, opt);
  ObjFiles f;
  std::string& o = f.obj;
  o.reserve(m.positions.size() * 96 + m.indices.size() * 12);
  o += "# Pixel3D " + std::string(version()) + " — units: " + std::string(to_string(opt.units)) +
       (opt.front_negative_z ? ", front faces -Z" : ", front faces +Z") + "\n";
  o += "mtllib " + base_name + ".mtl\n";
  o += "o pixel3d_model\n";
  for (const vec3d& p0 : m.positions) {
    const vec3d p = frame.apply(p0);
    o += "v ";
    put_number(o, p.x);
    o += ' ';
    put_number(o, p.y);
    o += ' ';
    put_number(o, p.z);
    o += '\n';
  }
  for (const vec2d& uv : m.uvs) {
    o += "vt ";
    put_number(o, uv.x);
    o += ' ';
    put_number(o, 1.0 - uv.y);
    o += '\n';
  }
  for (const vec3d& n0 : m.normals) {
    const vec3d n = frame.apply_dir(n0);
    o += "vn ";
    put_number(o, n.x);
    o += ' ';
    put_number(o, n.y);
    o += ' ';
    put_number(o, n.z);
    o += '\n';
  }
  o += "usemtl pixel_art\ns off\n";
  for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    o += 'f';
    for (int k = 0; k < 3; ++k) {
      const std::string i = std::to_string(m.indices[t + k] + 1);
      o += ' ' + i + '/' + i + '/' + i;
    }
    o += '\n';
  }
  f.mtl = "# Pixel3D " + std::string(version()) + " — set the texture to point (nearest) filtering\n"
          "newmtl pixel_art\nKa 0 0 0\nKd 1 1 1\nKs 0 0 0\nd 1\nillum 1\nmap_Kd " + base_name + ".png\n";
  if (m.texture) f.png = encode_png(*m.texture);
  return f;
}

}  // namespace pixel3d
