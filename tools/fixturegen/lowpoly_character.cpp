#include "fixturegen/lowpoly_character.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace p3dfix {
namespace {

using pixel3d::Dims;
using pixel3d::ImageRGBA;
using pixel3d::Rgba8;
using pixel3d::vec2d;
using pixel3d::vec3d;
using pixel3d::ViewRole;

constexpr Rgba8 kInk{0x10, 0x0e, 0x16, 0xff};

struct Prim {
  enum Kind { Ellipsoid, Capsule, Frustum } kind = Ellipsoid;
  int part = 0;  // index into CharFixture::parts
  Rgba8 color;
  vec3d a, b;          // ellipsoid: a = centre; capsule: axis a → b; frustum: a = (cx, y0, cz), b.y = y1
  vec3d r;             // ellipsoid radii
  double ra = 0, rb = 0;               // capsule radius at a / b
  double rx0 = 0, rz0 = 0, rx1 = 0, rz1 = 0;  // frustum radii at y0 / y1
  bool has_hole = false;  // an ellipsoid cut out of the primitive (a hood's face opening)
  vec3d hole_c, hole_r;

  bool inside(vec3d p) const {
    if (has_hole) {
      const vec3d q{(p.x - hole_c.x) / hole_r.x, (p.y - hole_c.y) / hole_r.y, (p.z - hole_c.z) / hole_r.z};
      if (dot(q, q) <= 1.0) return false;
    }
    switch (kind) {
      case Ellipsoid: {
        const vec3d q{(p.x - a.x) / r.x, (p.y - a.y) / r.y, (p.z - a.z) / r.z};
        return dot(q, q) <= 1.0;
      }
      case Capsule: {
        const vec3d ab = b - a;
        const double t = std::clamp(dot(p - a, ab) / dot(ab, ab), 0.0, 1.0);
        const double rad = ra + (rb - ra) * t;
        const vec3d d = p - (a + ab * t);
        return dot(d, d) <= rad * rad;
      }
      case Frustum: {
        if (p.y < a.y || p.y > b.y) return false;
        const double t = (p.y - a.y) / (b.y - a.y);
        const double rx = rx0 + (rx1 - rx0) * t, rz = rz0 + (rz1 - rz0) * t;
        const double qx = (p.x - a.x) / rx, qz = (p.z - a.z) / rz;
        return qx * qx + qz * qz <= 1.0;
      }
    }
    return false;
  }
  void shift(vec3d d) {
    a = a + d;
    b = b + d;
    hole_c = hole_c + d;
  }
};

struct Decal {
  enum Kind { EllipseFront, AboveY } kind = EllipseFront;
  int part = 0;
  Rgba8 color;
  vec2d c, r;       // ellipse centre / radii in x, y
  double y = 0;     // AboveY threshold
  double zmin = 0;  // EllipseFront: only on points with z ≥ zmin (the front of the part)
  bool applies(vec3d p) const {
    if (kind == AboveY) return p.y >= y;
    const double qx = (p.x - c.x) / r.x, qy = (p.y - c.y) / r.y;
    return p.z >= zmin && qx * qx + qy * qy <= 1.0;
  }
  void shift(vec3d d) {
    c = c + vec2d{d.x, d.y};
    y += d.y;
    zmin += d.z;
  }
};

struct Model {
  std::string name;
  std::vector<CharPart> parts;
  std::vector<Prim> prims;
  std::vector<Decal> decals;
};

struct Hit {
  bool hit = false;
  int part = -1;
  double depth = 0;
  Rgba8 color;
};

Hit trace(const Model& m, ViewRole role, Dims d, double u, double v) {
  const vec3d o = pixel3d::world_from_view(role, d, u, v, 0), dir = pixel3d::view_dir(role);
  const double extent = pixel3d::view_depth_extent(role, d);
  auto any_inside = [&](double t) {
    const vec3d p = o + dir * t;
    for (const Prim& pr : m.prims)
      if (pr.inside(p)) return true;
    return false;
  };
  constexpr double kStep = 0.25;
  double t = 0;
  while (t <= extent && !any_inside(t)) t += kStep;
  if (t > extent) return {};
  double lo = std::max(0.0, t - kStep), hi = t;
  if (t > 0)
    for (int i = 0; i < 12; ++i) {
      const double mid = (lo + hi) / 2;
      (any_inside(mid) ? hi : lo) = mid;
    }
  Hit h;
  h.hit = true;
  h.depth = hi;
  vec3d p;
  // The bisection ends on the surface; step in until a primitive contains the point (grazing rays).
  for (double eps = 1e-4; h.part < 0 && eps < 2 * kStep; eps *= 2) {
    p = o + dir * (hi + eps);
    for (const Prim& pr : m.prims)
      if (pr.inside(p)) {
        h.part = pr.part;
        h.color = pr.color;
        break;
      }
  }
  if (h.part < 0) return {};
  for (const Decal& dc : m.decals)
    if (dc.part == h.part && dc.applies(p)) h.color = dc.color;
  return h;
}

CharView render(const Model& m, ViewRole role, Dims d) {
  const auto sz = pixel3d::view_image_size(role, d);
  std::vector<Hit> hits(static_cast<size_t>(sz.w) * sz.h);
  for (int v = 0; v < sz.h; ++v)
    for (int u = 0; u < sz.w; ++u) hits[static_cast<size_t>(v) * sz.w + u] = trace(m, role, d, u + 0.5, v + 0.5);
  CharView out{ImageRGBA(sz.w, sz.h), ImageRGBA(sz.w, sz.h)};
  auto at = [&](int u, int v) -> const Hit* {
    if (u < 0 || v < 0 || u >= sz.w || v >= sz.h) return nullptr;
    return &hits[static_cast<size_t>(v) * sz.w + u];
  };
  for (int v = 0; v < sz.h; ++v)
    for (int u = 0; u < sz.w; ++u) {
      const Hit& h = *at(u, v);
      if (!h.hit) continue;
      bool ink = false;
      for (auto [du, dv] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
        const Hit* n = at(u + du, v + dv);
        if (!n || !n->hit) {
          ink = true;  // silhouette line
        } else if (n->part != h.part) {
          // Part border: the line goes on the farther side (or on the higher id when they touch).
          if (h.depth > n->depth + 0.01 || (std::abs(h.depth - n->depth) <= 0.01 && h.part > n->part)) ink = true;
        }
      }
      out.art.at(u, v) = ink ? kInk : h.color;
      out.parts.at(u, v) = m.parts[static_cast<size_t>(h.part)].key;
    }
  return out;
}

// Renders at a generous box, then shifts the model so the drawings touch every side of the box.
CharFixture finish(Model m, Dims generous) {
  const CharView f0 = render(m, ViewRole::Front, generous), s0 = render(m, ViewRole::Side, generous);
  int u0 = generous.W, u1 = 0, v0 = generous.H, v1 = 0, z0 = generous.D, z1 = 0;
  for (int v = 0; v < generous.H; ++v)
    for (int u = 0; u < generous.W; ++u)
      if (f0.art.at(u, v).a) u0 = std::min(u0, u), u1 = std::max(u1, u + 1), v0 = std::min(v0, v), v1 = std::max(v1, v + 1);
  for (int v = 0; v < generous.H; ++v)
    for (int u = 0; u < generous.D; ++u)
      if (s0.art.at(u, v).a) z0 = std::min(z0, u), z1 = std::max(z1, u + 1);
  const Dims d{u1 - u0, v1 - v0, z1 - z0};
  const vec3d shift{-double(u0), -double(generous.H - v1), -double(z0)};
  for (Prim& p : m.prims) p.shift(shift);
  for (Decal& dc : m.decals) dc.shift(shift);
  CharFixture fx;
  fx.name = m.name;
  fx.dims = d;
  fx.parts = m.parts;
  fx.front = render(m, ViewRole::Front, d);
  fx.side = render(m, ViewRole::Side, d);
  return fx;
}

// A 3-view reference: the same tight box, plus Top and the per-part ground truth.
CharFixture finish_reference(Model m, Dims generous) {
  const Model original = m;
  CharFixture fx = finish(std::move(m), generous);
  // finish() shifted a copy: redo the shift here to sample and render Top.
  const CharView f0 = render(original, ViewRole::Front, generous), s0 = render(original, ViewRole::Side, generous);
  int u0 = generous.W, v1 = 0, z0 = generous.D;
  for (int v = 0; v < generous.H; ++v)
    for (int u = 0; u < generous.W; ++u)
      if (f0.art.at(u, v).a) u0 = std::min(u0, u), v1 = std::max(v1, v + 1);
  for (int v = 0; v < generous.H; ++v)
    for (int u = 0; u < generous.D; ++u)
      if (s0.art.at(u, v).a) z0 = std::min(z0, u);
  Model shifted = original;
  const vec3d shift{-double(u0), -double(generous.H - v1), -double(z0)};
  for (Prim& p : shifted.prims) p.shift(shift);
  for (Decal& dc : shifted.decals) dc.shift(shift);
  fx.top = render(shifted, ViewRole::Top, fx.dims);
  // Ground truth per part: sample the box at 0.25 px.
  constexpr double kStep = 0.25;
  const double cell = kStep * kStep * kStep;
  for (size_t i = 0; i < shifted.parts.size(); ++i) {
    PartExtent e;
    e.id = shifted.parts[i].id;
    e.lo = {1e30, 1e30, 1e30};
    e.hi = {-1e30, -1e30, -1e30};
    fx.extents.push_back(e);
  }
  for (double z = kStep / 2; z < fx.dims.D; z += kStep)
    for (double y = kStep / 2; y < fx.dims.H; y += kStep)
      for (double x = kStep / 2; x < fx.dims.W; x += kStep) {
        const vec3d p{x, y, z};
        int owner = -1;
        for (const Prim& pr : shifted.prims) {
          if (!pr.inside(p)) continue;
          if (owner < 0) owner = pr.part;
          PartExtent& e = fx.extents[static_cast<size_t>(pr.part)];
          e.lo = {std::min(e.lo.x, x - kStep / 2), std::min(e.lo.y, y - kStep / 2), std::min(e.lo.z, z - kStep / 2)};
          e.hi = {std::max(e.hi.x, x + kStep / 2), std::max(e.hi.y, y + kStep / 2), std::max(e.hi.z, z + kStep / 2)};
        }
        if (owner >= 0) fx.extents[static_cast<size_t>(owner)].volume += cell;
      }
  return fx;
}

Prim ellipsoid(int part, Rgba8 c, vec3d centre, vec3d r) {
  Prim p;
  p.kind = Prim::Ellipsoid;
  p.part = part;
  p.color = c;
  p.a = centre;
  p.r = r;
  return p;
}

Prim capsule(int part, Rgba8 c, vec3d a, vec3d b, double ra, double rb) {
  Prim p;
  p.kind = Prim::Capsule;
  p.part = part;
  p.color = c;
  p.a = a;
  p.b = b;
  p.ra = ra;
  p.rb = rb;
  return p;
}

Prim frustum(int part, Rgba8 c, double cx, double cz, double y0, double y1, double rx0, double rz0, double rx1,
             double rz1) {
  Prim p;
  p.kind = Prim::Frustum;
  p.part = part;
  p.color = c;
  p.a = {cx, y0, cz};
  p.b = {cx, y1, cz};
  p.rx0 = rx0;
  p.rz0 = rz0;
  p.rx1 = rx1;
  p.rz1 = rz1;
  return p;
}

Decal eye(int part, Rgba8 c, double x, double y, double rx, double ry, double zmin) {
  Decal d;
  d.kind = Decal::EllipseFront;
  d.part = part;
  d.color = c;
  d.c = {x, y};
  d.r = {rx, ry};
  d.zmin = zmin;
  return d;
}

Decal above(int part, Rgba8 c, double y) {
  Decal d;
  d.kind = Decal::AboveY;
  d.part = part;
  d.color = c;
  d.y = y;
  return d;
}

}  // namespace

CharFixture make_cloaked_character() {
  Model m;
  m.name = "E_lowpoly_character/cloaked";
  // Part order = priority where primitives overlap (first wins).
  m.parts = {{1000, "head", "head", {255, 0, 0, 255}},     {1001, "body", "cloth", {0, 255, 0, 255}},
             {1002, "horn_l", "accessory", {0, 0, 255, 255}}, {1003, "horn_r", "accessory", {255, 255, 0, 255}},
             {1004, "leg_l", "limb", {255, 0, 255, 255}},    {1005, "leg_r", "limb", {0, 255, 255, 255}}};
  const Rgba8 shell{0xef, 0xec, 0xe4, 0xff}, cloak{0x3b, 0x41, 0x63, 0xff}, collar{0x5a, 0x61, 0x8c, 0xff},
      legs{0x2a, 0x2e, 0x4c, 0xff}, eyes{0x1c, 0x1a, 0x2a, 0xff};
  const double cx = 50, cz = 30;
  m.prims.push_back(ellipsoid(0, shell, {cx, 90, cz + 1}, {19, 17, 16}));
  m.prims.push_back(frustum(1, cloak, cx, cz, 16, 73, 25, 17, 12, 11));
  for (int s : {-1, 1}) {
    const int horn = s > 0 ? 2 : 3;
    // The character's left is +x (it faces the Front camera).
    m.prims.push_back(capsule(horn, shell, {cx + s * 8.0, 101, cz + 1}, {cx + s * 13.0, 113, cz + 1}, 3.6, 2.8));
    m.prims.push_back(capsule(horn, shell, {cx + s * 13.0, 113, cz + 1}, {cx + s * 14.5, 126, cz + 1}, 2.8, 1.3));
    const int leg = s > 0 ? 4 : 5;
    m.prims.push_back(capsule(leg, legs, {cx + s * 7.0, 24, cz}, {cx + s * 7.5, 6, cz + 1}, 3.8, 3.4));
    m.prims.push_back(ellipsoid(leg, legs, {cx + s * 8.0, 4.2, cz + 3}, {4.4, 3.2, 6.0}));
  }
  for (int s : {-1, 1}) m.decals.push_back(eye(0, eyes, cx + s * 7.5, 88, 4.4, 6.2, cz + 1 + 0.3 * 16));
  m.decals.push_back(above(1, collar, 66));
  return finish(std::move(m), Dims{100, 140, 60});
}

CharFixture make_humanoid_character() {
  Model m;
  m.name = "E_lowpoly_character/humanoid";
  m.parts = {{1000, "head", "head", {255, 0, 0, 255}},           {1001, "torso", "torso", {0, 255, 0, 255}},
             {1002, "upper_arm_l", "limb", {0, 0, 255, 255}},    {1003, "forearm_l", "limb", {255, 255, 0, 255}},
             {1004, "upper_arm_r", "limb", {255, 0, 255, 255}},  {1005, "forearm_r", "limb", {0, 255, 255, 255}},
             {1006, "thigh_l", "limb", {128, 0, 0, 255}},        {1007, "shin_l", "limb", {0, 128, 0, 255}},
             {1008, "thigh_r", "limb", {0, 0, 128, 255}},        {1009, "shin_r", "limb", {128, 128, 0, 255}}};
  const Rgba8 skin{0xe8, 0xc3, 0x9e, 0xff}, hair{0x5b, 0x3a, 0x29, 0xff}, shirt{0xb0, 0x4a, 0x3c, 0xff},
      pants{0x3c, 0x5a, 0x8a, 0xff}, shoes{0x2b, 0x2b, 0x33, 0xff}, eyes{0x1c, 0x1a, 0x2a, 0xff};
  const double cx = 50, cz = 25;
  m.prims.push_back(ellipsoid(0, skin, {cx, 112, cz}, {11, 13, 11}));
  m.prims.push_back(capsule(0, skin, {cx, 95, cz}, {cx, 102, cz}, 4.2, 4.2));
  m.prims.push_back(frustum(1, shirt, cx, cz, 58, 97, 14, 9, 17, 10));
  for (int s : {-1, 1}) {
    const int base = s > 0 ? 2 : 4;  // left (+x) arm first
    m.prims.push_back(capsule(base, shirt, {cx + s * 16.0, 93, cz}, {cx + s * 27.0, 74, cz}, 4.4, 3.8));
    m.prims.push_back(capsule(base + 1, skin, {cx + s * 27.0, 74, cz}, {cx + s * 34.0, 57, cz}, 3.8, 3.1));
    m.prims.push_back(ellipsoid(base + 1, skin, {cx + s * 35.5, 53.5, cz}, {3.4, 4.0, 3.0}));
    const int leg = s > 0 ? 6 : 8;
    m.prims.push_back(capsule(leg, pants, {cx + s * 7.0, 60, cz}, {cx + s * 7.5, 34, cz}, 5.6, 4.7));
    m.prims.push_back(capsule(leg + 1, pants, {cx + s * 7.5, 34, cz}, {cx + s * 7.5, 9, cz}, 4.7, 3.9));
    m.prims.push_back(ellipsoid(leg + 1, shoes, {cx + s * 7.5, 5, cz + 2}, {4.6, 3.6, 7.0}));
  }
  for (int s : {-1, 1}) m.decals.push_back(eye(0, eyes, cx + s * 4.2, 111, 1.6, 2.4, cz + 0.4 * 11));
  m.decals.push_back(above(0, hair, 117));
  return finish(std::move(m), Dims{110, 140, 50});
}

CharFixture make_hooded_reference() {
  Model m;
  m.name = "F_hooded_reference";
  // Part order = priority where primitives overlap (first wins): the face and the hair
  // show through the hood's opening, the hands in front of the sleeves, the belt over the tunic.
  m.parts = {{1000, "head", "head", {255, 0, 0, 255}},          {1001, "hair", "accessory", {128, 0, 0, 255}},
             {1002, "hood", "cloth", {0, 0, 128, 255}},         {1003, "scarf", "cloth", {0, 128, 255, 255}},
             {1004, "hand_l", "limb", {255, 128, 0, 255}},      {1005, "hand_r", "limb", {128, 255, 0, 255}},
             {1006, "belt", "accessory", {128, 128, 128, 255}}, {1007, "pouch", "accessory", {255, 255, 128, 255}},
             {1008, "torso", "torso", {0, 255, 0, 255}},        {1009, "arm_l", "limb", {0, 0, 255, 255}},
             {1010, "arm_r", "limb", {255, 0, 255, 255}},       {1011, "leg_l", "limb", {0, 255, 255, 255}},
             {1012, "leg_r", "limb", {255, 255, 0, 255}},       {1013, "boot_l", "limb", {128, 0, 255, 255}},
             {1014, "boot_r", "limb", {0, 128, 0, 255}}};
  const Rgba8 skin{0xf2, 0xc8, 0xa0, 0xff}, hair{0x6b, 0x42, 0x26, 0xff}, hood{0x55, 0x4c, 0x6e, 0xff},
      scarf{0x2f, 0x5f, 0xc8, 0xff}, tunic{0x7a, 0x4e, 0x2c, 0xff}, sleeve{0x5e, 0x3c, 0x22, 0xff},
      belt{0x3a, 0x26, 0x18, 0xff}, buckle{0xd8, 0xb0, 0x3a, 0xff}, pouch{0x9a, 0x6e, 0x3c, 0xff},
      legs{0x5a, 0x5e, 0x6e, 0xff}, boots{0x4a, 0x2c, 0x1a, 0xff}, eyes{0x1c, 0x1a, 0x2a, 0xff};
  const double cx = 50, cz = 30;
  m.prims.push_back(ellipsoid(0, skin, {cx, 91, cz + 1}, {9, 10, 9}));
  m.prims.push_back(ellipsoid(1, hair, {cx, 96, cz + 1.5}, {9.6, 7.5, 9.6}));
  Prim h = ellipsoid(2, hood, {cx, 93, cz}, {13, 14, 12.5});
  h.has_hole = true;
  h.hole_c = {cx, 90, cz + 13};
  h.hole_r = {8.2, 9.5, 9};
  m.prims.push_back(h);
  m.prims.push_back(frustum(3, scarf, cx, cz, 75, 81, 11.5, 9.0, 10.5, 8.5));
  m.prims.push_back(capsule(3, scarf, {cx + 4.5, 77, cz + 8.2}, {cx + 5.5, 60, cz + 8.8}, 2.6, 2.2));
  for (int s : {-1, 1}) {
    // The character's left is +x (it faces the Front camera).
    m.prims.push_back(ellipsoid(s > 0 ? 4 : 5, skin, {cx + s * 13.2, 52.5, cz}, {2.8, 3.4, 2.8}));
  }
  m.prims.push_back(frustum(6, belt, cx, cz, 51, 55, 10.4, 7.4, 10.4, 7.4));
  m.prims.push_back(ellipsoid(7, pouch, {cx - 5.5, 49.5, cz + 7.2}, {3.2, 3.6, 2.4}));
  m.prims.push_back(frustum(8, tunic, cx, cz, 44, 79, 10.2, 7.0, 9.6, 6.6));
  for (int s : {-1, 1}) {
    m.prims.push_back(capsule(s > 0 ? 9 : 10, sleeve, {cx + s * 12.6, 76, cz}, {cx + s * 13.2, 56, cz}, 3.2, 2.8));
    m.prims.push_back(capsule(s > 0 ? 11 : 12, legs, {cx + s * 4.6, 46, cz}, {cx + s * 4.6, 12, cz}, 3.8, 3.4));
    m.prims.push_back(ellipsoid(s > 0 ? 13 : 14, boots, {cx + s * 4.8, 6.5, cz + 1.8}, {4.4, 6.5, 6.4}));
  }
  for (int s : {-1, 1}) m.decals.push_back(eye(0, eyes, cx + s * 3.6, 89, 1.4, 2.2, cz + 1 + 0.5 * 9));
  m.decals.push_back(eye(6, buckle, cx, 53, 1.6, 1.8, cz + 6));
  return finish_reference(std::move(m), Dims{100, 140, 60});
}

}  // namespace p3dfix
