#pragma once
// Project format versions. Every released version keeps loading forever: each
// step migrates vN → vN+1; a file from a newer Pixel3D is refused, never misread.
#include <nlohmann/json.hpp>

#include "pixel3d/common/diagnostics.hpp"

namespace pixel3d {

// 2: editor inputs (edits, part masks, part names, single view); cleanup weld options dropped.
// 3: input normalisation settings; source images under their own extension.
// 4: polished style settings (settings.style).
// 5: LOW-POLY STUDIO settings (inputs.lowpoly) and part types (inputs.part_types).
constexpr int kProjectFormatVersion = 5;

Expected<nlohmann::json> migrate_project_json(nlohmann::json j, int from_version);

}  // namespace pixel3d
