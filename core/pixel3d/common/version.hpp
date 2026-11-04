#pragma once
#include <string_view>

namespace pixel3d {

// Release version, e.g. "1.1.0-alpha" (from the VERSION file at the repository root).
std::string_view version();

}  // namespace pixel3d
