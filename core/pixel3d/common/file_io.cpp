#include "pixel3d/common/file_io.hpp"

#include <atomic>
#include <fstream>
#include <random>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "pixel3d/common/path_utf8.hpp"

namespace pixel3d {
namespace fs = std::filesystem;
namespace {

Message write_error(const fs::path& path, const std::string& why) {
  return Message{"io.write_failed", "cannot write " + to_utf8(path) + ": " + why, Severity::Error,
                 {{"path", to_utf8(path)}, {"reason", why}}};
}

// Replaces `to` with `from` in one step where the platform allows it.
bool replace_file(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
  return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  std::error_code ec;
  fs::rename(from, to, ec);
  return !ec;
#endif
}

std::string unique_suffix() {
  static std::atomic<unsigned> counter{0};
  static const unsigned seed = std::random_device{}();
  return ".tmp" + std::to_string(seed) + "-" + std::to_string(counter.fetch_add(1));
}

}  // namespace

Expected<std::vector<uint8_t>> read_file(const fs::path& path, uint64_t max_bytes) {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec))
    return make_error("io.not_found", "file not found: " + to_utf8(path), {{"path", to_utf8(path)}});
  const uintmax_t size = fs::file_size(path, ec);
  if (ec) return make_error("io.read_failed", "cannot read: " + to_utf8(path), {{"path", to_utf8(path)}});
  if (size > max_bytes)
    return make_error("io.too_large",
                      "file is too large (" + std::to_string(size) + " bytes, limit " + std::to_string(max_bytes) +
                          "): " + to_utf8(path),
                      {{"path", to_utf8(path)}, {"bytes", size}, {"limit", max_bytes}});
  std::ifstream in(path, std::ios::binary);
  if (!in) return make_error("io.read_failed", "cannot open: " + to_utf8(path), {{"path", to_utf8(path)}});
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  if (size > 0) in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
  if (in.gcount() != static_cast<std::streamsize>(size) || in.bad())
    return make_error("io.read_failed", "read failed: " + to_utf8(path), {{"path", to_utf8(path)}});
  return bytes;
}

Message write_file_atomic(const fs::path& path, std::span<const uint8_t> bytes) {
  if (path.empty()) return write_error(path, "empty path");
  std::error_code ec;
  if (path.has_parent_path() && !fs::exists(path.parent_path(), ec)) {
    fs::create_directories(path.parent_path(), ec);
    if (ec) return write_error(path, "cannot create directory " + to_utf8(path.parent_path()));
  }
  if (fs::is_directory(path, ec)) return write_error(path, "a directory exists at this path");
  fs::path tmp = path;
  tmp += unique_suffix();
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return write_error(path, "cannot create file");
    if (!bytes.empty()) f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    f.flush();
    const bool ok = static_cast<bool>(f);
    f.close();
    if (!ok || f.fail()) {
      fs::remove(tmp, ec);
      return write_error(path, "write failed (disk full or not writable?)");
    }
  }
  if (fs::file_size(tmp, ec) != bytes.size() || ec) {
    fs::remove(tmp, ec);
    return write_error(path, "short write");
  }
  if (!replace_file(tmp, path)) {
    fs::remove(tmp, ec);
    return write_error(path, "cannot replace the existing file (locked or read-only?)");
  }
  return Message{"ok", "", Severity::Info, {{"path", to_utf8(path)}, {"bytes", bytes.size()}}};
}

Message write_file_atomic(const fs::path& path, std::string_view text) {
  return write_file_atomic(path, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

}  // namespace pixel3d
