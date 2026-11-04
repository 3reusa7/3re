#include "mcp/args.hpp"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <set>

#include "pixel3d/common/path_utf8.hpp"

namespace pixel3d::mcp {
namespace fs = std::filesystem;

const nlohmann::json Args::kNull;

namespace {

std::wstring folded(const fs::path& p) {
  std::wstring s = p.lexically_normal().wstring();
#ifdef _WIN32
  for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
  std::replace(s.begin(), s.end(), L'/', L'\\');
#endif
  return s;
}

bool inside(const fs::path& p, const fs::path& root) {
  const std::wstring a = folded(p), r = folded(root);
  if (a.size() < r.size() || a.compare(0, r.size(), r) != 0) return false;
  if (a.size() == r.size()) return true;
  const wchar_t sep = a[r.size()];
  return sep == L'\\' || sep == L'/' || r.back() == L'\\' || r.back() == L'/';
}

}  // namespace

fs::path PathPolicy::resolve(const std::string& field, const std::string& utf8) const {
  if (utf8.empty()) throw ArgError(field, "path is empty");
  if (utf8.find('\0') != std::string::npos) throw ArgError(field, "path contains a NUL character");
  fs::path p = path_from_utf8(utf8);
  std::error_code ec;
  if (p.is_relative()) p = fs::absolute(p, ec);
  if (ec) throw ArgError(field, "cannot resolve path");
  if (roots.empty()) return p;
  const fs::path canon = fs::weakly_canonical(p, ec);
  if (ec) throw ArgError(field, "cannot resolve path");
  for (const fs::path& r : roots) {
    const fs::path rc = fs::weakly_canonical(r, ec);
    if (!ec && inside(canon, rc)) return canon;
  }
  throw ArgError(field, "path is outside the directories this server may access (--root)");
}

ViewRole parse_role(const std::string& field, const std::string& s) {
  if (const auto r = view_role_from_string(s)) return *r;
  if (s == "left") return ViewRole::Side;
  throw ArgError(field, "unknown view role '" + s + "' (front, back, side, right, top, bottom)");
}

Args::Args(nlohmann::json j) : j_(j.is_null() ? nlohmann::json::object() : std::move(j)) {
  if (!j_.is_object()) throw ArgError("arguments", "must be an object");
}

bool Args::has(const char* key) const { return j_.is_object() && j_.contains(key) && !j_[key].is_null(); }

void Args::allow_only(std::initializer_list<const char*> keys) const {
  if (!j_.is_object()) return;
  const std::set<std::string> ok(keys.begin(), keys.end());
  for (const auto& [k, v] : j_.items())
    if (!ok.contains(k)) throw ArgError(k, "unknown argument");
}

const nlohmann::json& Args::raw(const char* key) const { return has(key) ? j_[key] : kNull; }

std::string Args::str(const char* key) const {
  auto v = opt_str(key);
  if (!v) throw ArgError(key, "required");
  return *v;
}

std::optional<std::string> Args::opt_str(const char* key) const {
  if (!has(key)) return std::nullopt;
  if (!j_[key].is_string()) throw ArgError(key, "must be a string");
  return j_[key].get<std::string>();
}

std::optional<double> Args::opt_num(const char* key, double lo, double hi) const {
  if (!has(key)) return std::nullopt;
  if (!j_[key].is_number()) throw ArgError(key, "must be a number");
  const double v = j_[key].get<double>();
  if (!std::isfinite(v) || v < lo || v > hi)
    throw ArgError(key, "must be in [" + nlohmann::json(lo).dump() + ", " + nlohmann::json(hi).dump() + "]");
  return v;
}

double Args::num(const char* key, double lo, double hi) const {
  auto v = opt_num(key, lo, hi);
  if (!v) throw ArgError(key, "required");
  return *v;
}

std::optional<int> Args::opt_int(const char* key, int lo, int hi) const {
  if (!has(key)) return std::nullopt;
  const auto& v = j_[key];
  if (!v.is_number_integer() && !(v.is_number_float() && std::floor(v.get<double>()) == v.get<double>()))
    throw ArgError(key, "must be an integer");
  const double d = v.get<double>();
  if (d < lo || d > hi) throw ArgError(key, "must be in [" + std::to_string(lo) + ", " + std::to_string(hi) + "]");
  return static_cast<int>(d);
}

std::optional<bool> Args::opt_bool(const char* key) const {
  if (!has(key)) return std::nullopt;
  if (!j_[key].is_boolean()) throw ArgError(key, "must be true or false");
  return j_[key].get<bool>();
}

std::vector<int> Args::int_list(const char* key, int lo, int hi, size_t max_items) const {
  std::vector<int> out;
  if (!has(key)) return out;
  if (!j_[key].is_array()) throw ArgError(key, "must be an array of integers");
  if (j_[key].size() > max_items) throw ArgError(key, "too many items (max " + std::to_string(max_items) + ")");
  for (const auto& v : j_[key]) {
    if (!v.is_number_integer()) throw ArgError(key, "must be an array of integers");
    const long long x = v.get<long long>();
    if (x < lo || x > hi) throw ArgError(key, "items must be in [" + std::to_string(lo) + ", " + std::to_string(hi) + "]");
    out.push_back(static_cast<int>(x));
  }
  return out;
}

ViewRole Args::role(const char* key) const { return parse_role(key, str(key)); }

std::optional<ViewRole> Args::opt_role(const char* key) const {
  const auto s = opt_str(key);
  if (!s) return std::nullopt;
  return parse_role(key, *s);
}

std::vector<ViewRole> Args::role_list(const char* key) const {
  std::vector<ViewRole> out;
  if (!has(key)) return out;
  if (!j_[key].is_array()) throw ArgError(key, "must be an array of view roles");
  if (j_[key].size() > 6) throw ArgError(key, "at most 6 views");
  for (const auto& v : j_[key]) {
    if (!v.is_string()) throw ArgError(key, "must be an array of view roles");
    out.push_back(parse_role(key, v.get<std::string>()));
  }
  return out;
}

}  // namespace pixel3d::mcp
