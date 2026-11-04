#pragma once
// Minimal value-type vector math used across the core. Kept independent of
// Manifold's linalg so public headers do not leak a geometry library.
#include <cmath>

namespace pixel3d {

struct vec2d {
  double x = 0, y = 0;
  friend constexpr vec2d operator+(vec2d a, vec2d b) { return {a.x + b.x, a.y + b.y}; }
  friend constexpr vec2d operator-(vec2d a, vec2d b) { return {a.x - b.x, a.y - b.y}; }
  friend constexpr vec2d operator*(vec2d a, double s) { return {a.x * s, a.y * s}; }
  friend constexpr vec2d operator*(double s, vec2d a) { return a * s; }
  friend constexpr bool operator==(vec2d a, vec2d b) = default;
};

struct vec3d {
  double x = 0, y = 0, z = 0;
  friend constexpr vec3d operator+(vec3d a, vec3d b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
  friend constexpr vec3d operator-(vec3d a, vec3d b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
  friend constexpr vec3d operator-(vec3d a) { return {-a.x, -a.y, -a.z}; }
  friend constexpr vec3d operator*(vec3d a, double s) { return {a.x * s, a.y * s, a.z * s}; }
  friend constexpr vec3d operator*(double s, vec3d a) { return a * s; }
  friend constexpr vec3d operator/(vec3d a, double s) { return {a.x / s, a.y / s, a.z / s}; }
  friend constexpr bool operator==(vec3d a, vec3d b) = default;
};

constexpr double dot(vec2d a, vec2d b) { return a.x * b.x + a.y * b.y; }
constexpr double cross(vec2d a, vec2d b) { return a.x * b.y - a.y * b.x; }
inline double length(vec2d a) { return std::sqrt(dot(a, a)); }

constexpr double dot(vec3d a, vec3d b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr vec3d cross(vec3d a, vec3d b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double length(vec3d a) { return std::sqrt(dot(a, a)); }
inline vec3d normalize(vec3d a) {
  const double l = length(a);
  return l > 0 ? a / l : vec3d{};
}

}  // namespace pixel3d
