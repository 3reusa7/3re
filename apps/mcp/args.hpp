#pragma once
// Typed access to tool arguments. Every accessor validates type and range and
// throws ArgError naming the field, which the server turns into a tool result
// with isError = true and code input.invalid, so the agent can correct itself.
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pixel3d/views/view_role.hpp"

namespace pixel3d::mcp {

struct ArgError : std::runtime_error {
  ArgError(std::string field, const std::string& why) : std::runtime_error(field + ": " + why), field(std::move(field)) {}
  std::string field;
};

// Where client paths may point. Empty = anywhere (the client decides).
struct PathPolicy {
  std::vector<std::filesystem::path> roots;
  std::filesystem::path resolve(const std::string& field, const std::string& utf8) const;
};

class Args {
 public:
  explicit Args(nlohmann::json j);  // null or object; anything else is an ArgError

  bool has(const char* key) const;
  // Rejects keys the tool does not declare (typos must not be silently ignored).
  void allow_only(std::initializer_list<const char*> keys) const;

  std::string str(const char* key) const;  // required
  std::optional<std::string> opt_str(const char* key) const;
  std::optional<double> opt_num(const char* key, double lo, double hi) const;
  double num(const char* key, double lo, double hi) const;
  std::optional<int> opt_int(const char* key, int lo, int hi) const;
  std::optional<bool> opt_bool(const char* key) const;
  std::vector<int> int_list(const char* key, int lo, int hi, size_t max_items) const;  // missing = empty
  ViewRole role(const char* key) const;
  std::optional<ViewRole> opt_role(const char* key) const;
  std::vector<ViewRole> role_list(const char* key) const;  // missing = empty
  const nlohmann::json& raw(const char* key) const;        // null if missing

 private:
  nlohmann::json j_;  // owned: arguments often come from temporaries
  static const nlohmann::json kNull;
};

ViewRole parse_role(const std::string& field, const std::string& s);

}  // namespace pixel3d::mcp
