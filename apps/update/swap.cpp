#include "update/swap.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace threere::update {
namespace {

using json = nlohmann::json;

std::optional<json> read_json(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return std::nullopt;
  std::stringstream ss;
  ss << in.rdbuf();
  auto j = json::parse(ss.str(), nullptr, false);
  if (j.is_discarded()) return std::nullopt;
  return j;
}

bool write_text(const fs::path& file, const std::string& text) {
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out << text;
  return bool(out);
}

// Moves rel from `from` to `to` (same volume), creating folders; false on failure.
bool move_one(const fs::path& from, const fs::path& to, const std::string& rel, std::string& why) {
  std::error_code ec;
  const auto dst = to / from_u8(rel);
  fs::create_directories(dst.parent_path(), ec);
  if (ec) {
    why = "cannot create " + u8(dst.parent_path()) + ": " + ec.message();
    return false;
  }
  fs::rename(from / from_u8(rel), dst, ec);
  if (ec) {
    why = "cannot move " + rel + ": " + ec.message();
    return false;
  }
  // Folders that held only the moved file go too (no empty "models" left in the other release).
  for (auto dir = (from / from_u8(rel)).parent_path(); dir != from && dir.has_relative_path(); dir = dir.parent_path())
    if (!fs::is_empty(dir, ec) || ec || !fs::remove(dir, ec)) break;
  return true;
}

void move_back(const fs::path& from, const fs::path& to, const std::vector<std::string>& rels) {
  std::string ignored;
  for (const auto& r : rels) move_one(from, to, r, ignored);
}

}  // namespace

std::string u8(const fs::path& p) {
  const auto s = p.generic_u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

fs::path from_u8(const std::string& s) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size())); }

std::optional<std::set<std::string>> release_files(const fs::path& dir) {
  const auto j = read_json(dir / "RELEASE.json");
  if (!j || !j->is_object() || !j->contains("files") || !(*j)["files"].is_array()) return std::nullopt;
  std::set<std::string> out{"RELEASE.json"};
  for (const auto& f : (*j)["files"])
    if (f.is_object() && f.contains("path") && f["path"].is_string()) out.insert(f["path"].get<std::string>());
  return out;
}

std::string release_version(const fs::path& dir) {
  const auto j = read_json(dir / "RELEASE.json");
  if (j && j->is_object() && j->contains("version") && (*j)["version"].is_string()) return (*j)["version"].get<std::string>();
  return "unknown";
}

std::vector<std::string> files_to_carry(const fs::path& install, const fs::path& staged, const std::set<std::string>& release) {
  std::vector<std::string> out;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(install, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it->is_symlink(ec) || !it->is_regular_file(ec)) continue;
    const auto rel = u8(fs::relative(it->path(), install, ec));
    if (ec || rel.empty() || rel == kJournal || release.count(rel)) continue;
    if (fs::exists(staged / from_u8(rel), ec)) continue;  // the new release brings its own
    out.push_back(rel);
  }
  return out;
}

SwapResult swap_in(const fs::path& install, const fs::path& staged, const fs::path& backup) {
  SwapResult r;
  std::error_code ec;
  if (!fs::is_directory(install, ec)) return {false, "the install folder is missing: " + u8(install), {}};
  if (!fs::is_directory(staged, ec)) return {false, "the downloaded release is missing: " + u8(staged), {}};
  if (fs::exists(backup, ec)) return {false, "the backup folder already exists: " + u8(backup), {}};
  const auto release = release_files(install);
  if (!release) return {false, "this folder has no RELEASE.json, so 3re cannot tell its files from yours: update by hand", {}};
  if (!release_files(staged)) return {false, "the downloaded release has no RELEASE.json", {}};

  const auto carry = files_to_carry(install, staged, *release);
  std::string why;
  for (const auto& rel : carry) {
    if (!move_one(install, staged, rel, why)) {
      move_back(staged, install, r.carried);
      return {false, why, {}};
    }
    r.carried.push_back(rel);
  }
  fs::rename(install, backup, ec);
  if (ec) {
    move_back(staged, install, r.carried);
    return {false, "cannot move the current release aside (is 3re still running?): " + ec.message(), {}};
  }
  fs::rename(staged, install, ec);
  if (ec) {
    std::error_code ec2;
    fs::rename(backup, install, ec2);
    move_back(staged, install, r.carried);
    return {false, "cannot put the new release in place; the previous one was restored: " + ec.message(), {}};
  }
  json journal{{"from_version", release_version(backup)}, {"to_version", release_version(install)}, {"backup", u8(backup)},
               {"carried", r.carried}};
  write_text(install / kJournal, journal.dump(1));
  r.ok = true;
  r.message = "updated " + release_version(backup) + " -> " + release_version(install) + " (" + std::to_string(r.carried.size()) +
              " of your files carried over)";
  return r;
}

SwapResult roll_back(const fs::path& install, const fs::path& backup, const fs::path& set_aside) {
  SwapResult r;
  std::error_code ec;
  if (!fs::is_directory(backup, ec)) return {false, "there is no previous version to go back to: " + u8(backup), {}};
  if (!release_files(backup)) return {false, "the previous version is incomplete (no RELEASE.json)", {}};
  if (fs::exists(set_aside, ec)) return {false, "the folder for the version being removed already exists: " + u8(set_aside), {}};
  std::vector<std::string> carried;
  if (const auto j = read_json(install / kJournal); j && j->contains("carried") && (*j)["carried"].is_array())
    for (const auto& c : (*j)["carried"])
      if (c.is_string()) carried.push_back(c.get<std::string>());
  std::string why;
  for (const auto& rel : carried) {
    if (!fs::exists(install / from_u8(rel), ec)) continue;
    if (fs::exists(backup / from_u8(rel), ec)) continue;
    if (!move_one(install, backup, rel, why)) {
      move_back(backup, install, r.carried);
      return {false, why, {}};
    }
    r.carried.push_back(rel);
  }
  fs::rename(install, set_aside, ec);
  if (ec) {
    move_back(backup, install, r.carried);
    return {false, "cannot move the current release aside (is 3re still running?): " + ec.message(), {}};
  }
  fs::rename(backup, install, ec);
  if (ec) {
    std::error_code ec2;
    fs::rename(set_aside, install, ec2);
    move_back(backup, install, r.carried);
    return {false, "cannot restore the previous version: " + ec.message(), {}};
  }
  r.ok = true;
  r.message = "back to " + release_version(install) + " (" + std::to_string(r.carried.size()) + " of your files moved back)";
  return r;
}

}  // namespace threere::update
