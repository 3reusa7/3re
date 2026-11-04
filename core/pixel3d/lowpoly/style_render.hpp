#pragma once
// Preview renderer for LOW-POLY STUDIO styles (pictures for people; validation
// never uses it): orthographic orbit camera, supersampled, flat (unlit albedo),
// cel (2–3 tone ramp of N·L) and an ink outline found in screen space from
// depth and shell discontinuities — the post-process outline an engine would
// draw, not geometry.
#include <vector>

#include "pixel3d/image/image.hpp"
#include "pixel3d/lowpoly/types.hpp"

namespace pixel3d::lowpoly {

struct StyleRenderOptions {
  int width = 384, height = 384;
  int supersample = 3;
  Style style = Style::CelOutline;
  int tones = 3;
  // Key light relative to the camera (x right, y up, z towards the viewer), so
  // every turntable frame is lit alike.
  vec3d light_cam{-0.45, 0.65, 0.6};
  Rgba8 background{238, 238, 242, 255};
  double outline_px = 1.4;  // output pixels
  Rgba8 outline_color{20, 18, 28, 255};
  double margin = 0.06;
  bool show_wire = false;   // triangle edges (for reviewing topology)
};

// Albedo per vertex comes from `texture` at the mesh UVs (NEAREST) or, without
// a texture, from a colour per shell. `positions`/`normals` override the
// mesh's (posed renders); `lo`/`hi` are the bounds the camera fits (pass the
// same bounds for every frame of a turntable so the scale is shared).
ImageRGBA render_style(const SkinnedMesh& m, const ImageRGBA* texture, const std::vector<vec3d>* positions,
                       const std::vector<vec3d>* normals, double yaw_deg, double elevation_deg, vec3d lo, vec3d hi,
                       const StyleRenderOptions& opt);

// Cel ramp: the albedo factor for N·L (shared by the preview and baked_cel).
double cel_factor(double n_dot_l, int tones);

}  // namespace pixel3d::lowpoly
