#pragma once
// The install swap of 3re-update.exe, the same steps as tools/package.ps1 (the current release moves aside,
// the staged one takes its place, a failure puts the previous one back), plus the carry-over of the
// person's own files: everything in the install folder that its RELEASE.json does not list (downloaded
// models and engine libraries, the installer's uninstaller, files the person added) moves into the new
// release first. Projects and settings live outside the install folder and are never touched. Qt-free.
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace threere::update {

namespace fs = std::filesystem;

// Written into the new install: the carried files and the previous version, for a later roll-back.
inline constexpr const char* kJournal = ".3re-update.json";

struct SwapResult {
  bool ok = false;
  std::string message;
  std::vector<std::string> carried;  // relative '/' paths moved from the old release into the new one
};

std::string u8(const fs::path& p);         // generic UTF-8 string
fs::path from_u8(const std::string& s);    // UTF-8 to path

// Relative '/' paths of the release in `dir` (RELEASE.json "files", plus RELEASE.json itself);
// nullopt when RELEASE.json is missing or not readable.
std::optional<std::set<std::string>> release_files(const fs::path& dir);
std::string release_version(const fs::path& dir);  // RELEASE.json "version", or "unknown"

// The person's files in `install` that `staged` will not replace: not listed by install's RELEASE.json,
// not the journal, and not present in `staged`.
std::vector<std::string> files_to_carry(const fs::path& install, const fs::path& staged, const std::set<std::string>& release);

// Moves the person's files into `staged`, then install -> backup and staged -> install. `backup` must
// not exist and must be on the same volume. On failure everything is left as it was (ok == false).
SwapResult swap_in(const fs::path& install, const fs::path& staged, const fs::path& backup);

// Undoes swap_in: the carried files go back into `backup`, install -> set_aside, backup -> install.
SwapResult roll_back(const fs::path& install, const fs::path& backup, const fs::path& set_aside);

}  // namespace threere::update
