#include "update/version.hpp"

#include <charconv>

namespace threere::update {
namespace {

bool all_digits(std::string_view s) {
  if (s.empty()) return false;
  for (const char c : s)
    if (c < '0' || c > '9') return false;
  return true;
}

bool identifier_ok(std::string_view s) {
  if (s.empty()) return false;
  for (const char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-')) return false;
  return true;
}

std::optional<long> number(std::string_view s) {
  if (!all_digits(s) || (s.size() > 1 && s[0] == '0') || s.size() > 9) return std::nullopt;
  long v = 0;
  std::from_chars(s.data(), s.data() + s.size(), v);
  return v;
}

}  // namespace

std::string Version::str() const {
  std::string s = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
  for (std::size_t i = 0; i < pre.size(); ++i) s += (i ? "." : "-") + pre[i];
  return s;
}

std::optional<Version> parse_version(std::string_view text) {
  if (!text.empty() && (text[0] == 'v' || text[0] == 'V')) text.remove_prefix(1);
  if (const auto plus = text.find('+'); plus != std::string_view::npos) text = text.substr(0, plus);
  std::string_view core = text, pre;
  if (const auto dash = text.find('-'); dash != std::string_view::npos) {
    core = text.substr(0, dash);
    pre = text.substr(dash + 1);
    if (pre.empty()) return std::nullopt;
  }
  Version v;
  long* parts[3] = {&v.major, &v.minor, &v.patch};
  for (int i = 0; i < 3; ++i) {
    const auto dot = core.find('.');
    if ((i < 2) == (dot == std::string_view::npos)) return std::nullopt;
    const auto n = number(core.substr(0, dot));
    if (!n) return std::nullopt;
    *parts[i] = *n;
    core = i < 2 ? core.substr(dot + 1) : std::string_view{};
  }
  while (!pre.empty()) {
    const auto dot = pre.find('.');
    const auto id = pre.substr(0, dot);
    if (!identifier_ok(id) || (all_digits(id) && id.size() > 1 && id[0] == '0')) return std::nullopt;
    v.pre.emplace_back(id);
    if (dot == std::string_view::npos) break;
    pre.remove_prefix(dot + 1);
    if (pre.empty()) return std::nullopt;
  }
  return v;
}

std::strong_ordering compare(const Version& a, const Version& b) {
  if (auto c = a.major <=> b.major; c != 0) return c;
  if (auto c = a.minor <=> b.minor; c != 0) return c;
  if (auto c = a.patch <=> b.patch; c != 0) return c;
  // A release ranks above any of its pre-releases.
  if (a.pre.empty() != b.pre.empty()) return a.pre.empty() ? std::strong_ordering::greater : std::strong_ordering::less;
  if (a.pre.empty()) return std::strong_ordering::equal;
  for (std::size_t i = 0; i < a.pre.size() && i < b.pre.size(); ++i) {
    const auto& x = a.pre[i];
    const auto& y = b.pre[i];
    const bool xn = all_digits(x), yn = all_digits(y);
    if (xn && yn) {
      if (auto c = (x.size() <=> y.size()); c != 0) return c;  // no leading zeros: longer is larger
      if (auto c = x.compare(y) <=> 0; c != 0) return c;
    } else if (xn != yn) {
      return xn ? std::strong_ordering::less : std::strong_ordering::greater;  // numeric < alphanumeric
    } else if (auto c = x.compare(y) <=> 0; c != 0) {
      return c;
    }
  }
  return a.pre.size() <=> b.pre.size();
}

}  // namespace threere::update
