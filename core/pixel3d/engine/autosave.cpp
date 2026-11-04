#include "pixel3d/engine/autosave.hpp"

#include <cstdio>
#include <map>
#include <random>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include "pixel3d/common/file_io.hpp"
#include "pixel3d/common/path_utf8.hpp"
#include "pixel3d/io/json_safe.hpp"

namespace pixel3d {
namespace fs = std::filesystem;
namespace {

std::string utc_now(const char* fmt) {
  const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[40];
  std::strftime(buf, sizeof buf, fmt, &tm);
  return buf;
}

unsigned long process_id() {
#ifdef _WIN32
  return GetCurrentProcessId();
#else
  return static_cast<unsigned long>(getpid());
#endif
}

// Opens `path` as this process's exclusive lock. nullptr on failure.
void* take_lock(const fs::path& path) {
#ifdef _WIN32
  // No FILE_SHARE_DELETE: nobody can delete the lock while we hold it.
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  return h == INVALID_HANDLE_VALUE ? nullptr : h;
#else
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0) return nullptr;
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    return nullptr;
  }
  return reinterpret_cast<void*>(static_cast<intptr_t>(fd) + 1);
#endif
}

void release_lock(void* lock) {
  if (!lock) return;
#ifdef _WIN32
  CloseHandle(static_cast<HANDLE>(lock));
#else
  ::close(static_cast<int>(reinterpret_cast<intptr_t>(lock) - 1));
#endif
}

// True if a live process still holds the lock file. A lock nobody holds is removed.
bool lock_is_held(const fs::path& lock) {
  std::error_code ec;
  if (!fs::exists(lock, ec)) return false;
#ifdef _WIN32
  if (DeleteFileW(lock.c_str())) return false;
  const DWORD err = GetLastError();
  return err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED;
#else
  const int fd = ::open(lock.c_str(), O_RDWR);
  if (fd < 0) return false;
  const bool free = flock(fd, LOCK_EX | LOCK_NB) == 0;
  if (free) ::unlink(lock.c_str());
  ::close(fd);
  return !free;
#endif
}

void remove_slot(const fs::path& dir, const std::string& id) {
  std::error_code ec;
  for (const char* ext : {".p3d", ".json", ".lock"}) fs::remove(dir / (id + ext), ec);
}

}  // namespace

AutosaveSlot::AutosaveSlot(fs::path dir, std::string id, void* lock)
    : dir_(std::move(dir)), id_(std::move(id)), lock_(lock) {}

Expected<std::unique_ptr<AutosaveSlot>> AutosaveSlot::acquire(const fs::path& dir) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) return make_error("autosave.unavailable", "cannot create the autosave folder " + to_utf8(dir), {{"path", to_utf8(dir)}});
  std::random_device rd;
  for (int attempt = 0; attempt < 8; ++attempt) {
    char suffix[16];
    std::snprintf(suffix, sizeof suffix, "%04x", static_cast<unsigned>(rd() & 0xffff));
    const std::string id = utc_now("%Y%m%dT%H%M%SZ") + "-" + std::to_string(process_id()) + "-" + suffix;
    if (void* lock = take_lock(dir / (id + ".lock")))
      return std::unique_ptr<AutosaveSlot>(new AutosaveSlot(dir, id, lock));
  }
  return make_error("autosave.unavailable", "cannot create an autosave lock in " + to_utf8(dir), {{"path", to_utf8(dir)}});
}

AutosaveSlot::~AutosaveSlot() { release_lock(lock_); }

Message AutosaveSlot::write_info(const std::string& project_path, uint64_t revision) {
  const nlohmann::json info{{"original_project", project_path},
                            {"revision", revision},
                            {"saved_at", utc_now("%Y-%m-%dT%H:%M:%SZ")},
                            {"pid", process_id()}};
  return write_file_atomic(dir_ / (id_ + ".json"), info.dump(2, ' ', false, nlohmann::json::error_handler_t::replace));
}

void AutosaveSlot::clean_exit() {
  if (cleaned_) return;
  release_lock(lock_);
  lock_ = nullptr;
  remove_slot(dir_, id_);
  cleaned_ = true;
}

std::vector<RecoverableSession> find_recoverable(const fs::path& dir, std::chrono::hours retention) {
  std::vector<RecoverableSession> out;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return out;
  std::map<std::string, int> ids;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    const fs::path p = e.path();
    const std::string ext = p.extension().string();
    if (ext == ".p3d" || ext == ".json" || ext == ".lock") ++ids[to_utf8(p.stem())];
  }
  const auto now = fs::file_time_type::clock::now();
  for (const auto& [id, n] : ids) {
    if (lock_is_held(dir / (id + ".lock"))) continue;  // a running instance
    const fs::path project = dir / (id + ".p3d");
    if (!fs::exists(project, ec)) {
      remove_slot(dir, id);  // leftovers without an autosave
      continue;
    }
    const auto age = now - fs::last_write_time(project, ec);
    if (ec || age > retention) {
      remove_slot(dir, id);
      continue;
    }
    RecoverableSession s;
    s.id = id;
    s.project_file = project;
    if (auto bytes = read_file(dir / (id + ".json"), 1 << 20)) {
      const auto j = parse_json_untrusted(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()),
                                          1 << 20);
      if (j && j->is_object()) {
        if ((*j).contains("original_project") && (*j)["original_project"].is_string())
          s.original_project = (*j)["original_project"].get<std::string>();
        if ((*j).contains("revision") && (*j)["revision"].is_number_unsigned()) s.revision = (*j)["revision"].get<uint64_t>();
        if ((*j).contains("saved_at") && (*j)["saved_at"].is_string()) s.saved_at = (*j)["saved_at"].get<std::string>();
      }
    }
    out.push_back(std::move(s));
  }
  return out;
}

void discard(const fs::path& dir, const RecoverableSession& s) { remove_slot(dir, s.id); }

}  // namespace pixel3d
