#pragma once
// Paths cross the API as std::filesystem::path (wide on Windows) and appear in
// messages/JSON as UTF-8, so file names in any script (e.g. Arabic) survive.
#include <filesystem>
#include <string>
#include <string_view>

namespace pixel3d {

inline std::string to_utf8(const std::filesystem::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

inline std::filesystem::path path_from_utf8(std::string_view s) {
  return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

}  // namespace pixel3d
