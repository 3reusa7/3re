#include "update/manifest.hpp"

#include <nlohmann/json.hpp>
#include <stdexcept>

namespace threere::update {
namespace {

using json = nlohmann::json;

std::string text(const json& o, const char* key, bool required = true) {
  const auto it = o.find(key);
  if (it == o.end() || !it->is_string()) {
    if (required) throw std::runtime_error(std::string("the manifest has no \"") + key + "\"");
    return {};
  }
  return it->get<std::string>();
}

std::uint64_t size_of(const json& o) {
  const auto it = o.find("size");
  if (it == o.end() || !it->is_number_unsigned()) throw std::runtime_error("a manifest entry has no valid \"size\"");
  return it->get<std::uint64_t>();
}

bool hex64(const std::string& s) {
  if (s.size() != 64) return false;
  for (const char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

}  // namespace

const Package* Manifest::package_for(std::string_view platform, std::string_view kind) const {
  for (const auto& p : packages)
    if (p.platform == platform && p.kind == kind) return &p;
  return nullptr;
}

bool safe_relative_path(std::string_view path) {
  if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos || path.find(':') != std::string_view::npos)
    return false;
  std::size_t start = 0;
  while (start <= path.size()) {
    const auto end = path.find('/', start);
    const auto part = path.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return true;
}

Manifest parse_manifest(std::string_view text_in) {
  const json j = json::parse(text_in, nullptr, false);
  if (j.is_discarded() || !j.is_object()) throw std::runtime_error("the manifest is not valid JSON");
  if (text(j, "format") != "3re-update-manifest") throw std::runtime_error("not a 3re update manifest");
  if (!j.contains("format_version") || !j["format_version"].is_number_integer() || j["format_version"].get<int>() != 1)
    throw std::runtime_error("unsupported manifest format version (update 3re manually)");
  if (text(j, "product") != "3re") throw std::runtime_error("the manifest is for another product");
  Manifest m;
  m.version_text = text(j, "version");
  const auto v = parse_version(m.version_text);
  if (!v) throw std::runtime_error("the manifest version is not valid: " + m.version_text);
  m.version = *v;
  m.channel = text(j, "channel");
  if (m.channel != "stable" && m.channel != "alpha") throw std::runtime_error("unknown channel: " + m.channel);
  m.published = text(j, "published", false);
  m.commit = text(j, "commit", false);
  if (const auto n = j.find("notes"); n != j.end() && n->is_object()) {
    m.notes_en = text(*n, "en", false);
    m.notes_ar = text(*n, "ar", false);
  }
  const auto pk = j.find("packages");
  if (pk == j.end() || !pk->is_array() || pk->empty()) throw std::runtime_error("the manifest lists no packages");
  for (const auto& p : *pk) {
    if (!p.is_object()) throw std::runtime_error("a manifest package is not an object");
    Package out;
    out.platform = text(p, "platform");
    out.kind = text(p, "kind");
    out.name = text(p, "name");
    out.url = text(p, "url");
    out.sha256 = text(p, "sha256");
    out.size = size_of(p);
    if (!hex64(out.sha256)) throw std::runtime_error("package " + out.name + " has no valid SHA-256");
    if (!(out.url.starts_with("https://") || out.url.starts_with("http://") || out.url.starts_with("file:")))
      throw std::runtime_error("package " + out.name + " has an unsupported URL");
    if (const auto fl = p.find("files"); fl != p.end()) {
      if (!fl->is_array()) throw std::runtime_error("package " + out.name + ": \"files\" is not a list");
      for (const auto& f : *fl) {
        FileEntry e{text(f, "path"), size_of(f), text(f, "sha256")};
        if (!safe_relative_path(e.path)) throw std::runtime_error("unsafe path in the manifest: " + e.path);
        if (!hex64(e.sha256)) throw std::runtime_error("no valid SHA-256 for " + e.path);
        out.files.push_back(std::move(e));
      }
    }
    m.packages.push_back(std::move(out));
  }
  return m;
}

std::optional<ReleaseChoice> choose_release(std::string_view releases_json, const Version& current, std::string_view channel) {
  const json j = json::parse(releases_json, nullptr, false);
  if (j.is_discarded() || !j.is_array()) return std::nullopt;
  std::optional<ReleaseChoice> best;
  for (const auto& r : j) {
    if (!r.is_object() || r.value("draft", false)) continue;
    const bool pre = r.value("prerelease", false);
    if (pre && channel != "alpha") continue;
    const auto tag = r.value("tag_name", std::string());
    const auto v = parse_version(tag);
    if (!v || !(current < *v)) continue;
    // A stable channel never offers a version that is itself a pre-release, whatever the flag says.
    if (channel != "alpha" && v->prerelease()) continue;
    ReleaseChoice c;
    c.version = *v;
    c.tag = tag;
    c.prerelease = pre || v->prerelease();
    c.html_url = r.value("html_url", std::string());
    if (const auto a = r.find("assets"); a != r.end() && a->is_array()) {
      for (const auto& asset : *a) {
        const auto name = asset.value("name", std::string());
        if (name == kManifestAsset) c.manifest_url = asset.value("browser_download_url", std::string());
        if (name == kSignatureAsset) c.signature_url = asset.value("browser_download_url", std::string());
      }
    }
    if (c.manifest_url.empty() || c.signature_url.empty()) continue;
    if (!best || best->version < c.version) best = std::move(c);
  }
  return best;
}

}  // namespace threere::update
