#pragma once
// Release versions as 3re writes them (VERSION, tags): MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD], ordered by
// SemVer 2.0.0 precedence (https://semver.org/#spec-item-11). A leading "v" (tags) is accepted.
#include <compare>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace threere::update {

struct Version {
  long major = 0, minor = 0, patch = 0;
  std::vector<std::string> pre;  // "alpha", "2" for 1.0.0-alpha.2; empty for a stable release

  bool prerelease() const { return !pre.empty(); }
  std::string str() const;
};

std::optional<Version> parse_version(std::string_view text);
std::strong_ordering compare(const Version& a, const Version& b);
inline bool operator<(const Version& a, const Version& b) { return compare(a, b) < 0; }
inline bool operator==(const Version& a, const Version& b) { return compare(a, b) == 0; }

}  // namespace threere::update
