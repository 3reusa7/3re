#pragma once
// Objective quality metrics (spec §7), all from the deterministic CPU renderer.
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pixel3d/analysis/recess.hpp"
#include "pixel3d/correspond/solve.hpp"
#include "pixel3d/mesh/topology.hpp"
#include "pixel3d/render/rasterizer.hpp"
#include "pixel3d/texture/projector.hpp"

namespace pixel3d {

struct ViewMetric {
  double iou = 0;                    // 1× re-render mask vs source mask
  int mismatch_px = 0;               // symmetric difference
  double color_exact_fraction = 0;   // over pixels opaque in both
  int mask_area = 0;                 // source opaque pixels
};

struct RotationMetric {
  int yaw = 0;              // 90: camera +X vs mirrored Side; 180: camera −Z vs mirrored Front; 270: camera −X vs Side
  double iou = 0;
  double projected_width = 0;  // world units
};

struct TurntableMetric {
  int frames = 0;
  double min_area_ratio = 0;   // min / max projected area
  double min_width_ratio = 0;  // min / max projected width
  bool all_non_empty = false;
  std::vector<double> areas, widths;  // world units² / units per frame
};

struct QualityMetrics {
  std::map<ViewRole, ViewMetric> views;
  std::vector<RotationMetric> rotations;
  double thickness_ratio = 0;  // min / max projected width over 0/90/180/270
  TurntableMetric turntable;
  TopologyReport topology;
  double volume = 0;
  double inferred_volume_fraction = 0, ambiguous_volume_fraction = 0;
  std::map<std::string, double> texture;
  nlohmann::json glb;  // null until a GLB round-trip ran
  std::optional<double> volume_iou_gt;
  bool full = true;          // false: live checks only (no rotations / turntable)
  double recess_volume = 0;  // column volume under undetermined possible_recess regions
};

// full = false: live checks after an edit (no rotations, no turntable).
// `recesses`: undetermined possible_recess columns count as ambiguous volume.
QualityMetrics compute_metrics(const TexturedMesh& tm, const Mesh& mesh, const std::vector<SourceView>& views, Dims d,
                               const CorrespondenceResult& corr, const TextureResult& tex,
                               uint8_t alpha_threshold = 128, const std::vector<RecessFinding>* recesses = nullptr,
                               bool full = true);

TurntableMetric turntable_metrics(const Mesh& mesh, Dims d, int frames = 24);

}  // namespace pixel3d
