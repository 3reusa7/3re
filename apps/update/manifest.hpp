#pragma once
// The signed update manifest of a 3re release (update-manifest.json + update-manifest.json.minisig, both
// release assets) and the choice of a release from the GitHub Releases list. Qt-free.
//
// {
//   "format": "3re-update-manifest", "format_version": 1, "product": "3re",
//   "version": "2.10.0-alpha", "channel": "alpha", "published": "2026-11-03T12:00:00Z", "commit": "abc1234",
//   "notes": {"en": "What changed, in plain words.", "ar": "..."},
//   "packages": [{"platform": "windows-x64", "kind": "portable-zip", "name": "3re-2.10.0-alpha-windows-x64.zip",
//                 "url": "https://github.com/.../3re-2.10.0-alpha-windows-x64.zip", "size": 1, "sha256": "...",
//                 "files": [{"path": "3re.exe", "size": 1, "sha256": "..."}, ...]}]
// }
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "update/version.hpp"

namespace threere::update {

inline constexpr std::string_view kPlatform = "windows-x64";
inline constexpr std::string_view kManifestAsset = "update-manifest.json";
inline constexpr std::string_view kSignatureAsset = "update-manifest.json.minisig";

struct FileEntry {
  std::string path;  // relative, '/' separated
  std::uint64_t size = 0;
  std::string sha256;  // lower-case hex
};

struct Package {
  std::string platform, kind, name, url, sha256;
  std::uint64_t size = 0;
  std::vector<FileEntry> files;
};

struct Manifest {
  Version version;
  std::string version_text, channel, published, commit, notes_en, notes_ar;
  std::vector<Package> packages;

  const Package* package_for(std::string_view platform, std::string_view kind = "portable-zip") const;
};

// Throws std::runtime_error with a plain reason when the text is not a valid 3re manifest.
Manifest parse_manifest(std::string_view json);

// A path from a manifest is safe to unpack: relative, no "..", no drive or root, no backslash tricks.
bool safe_relative_path(std::string_view path);

struct ReleaseChoice {
  Version version;
  std::string tag, html_url, manifest_url, signature_url;
  bool prerelease = false;
};

// From the JSON array of GET /repos/{owner}/{repo}/releases: the newest release above `current` that the
// channel allows ("stable": no pre-releases; "alpha": everything) and that carries both manifest assets.
std::optional<ReleaseChoice> choose_release(std::string_view releases_json, const Version& current, std::string_view channel);

}  // namespace threere::update
