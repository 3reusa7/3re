#pragma once
// Autosave and crash recovery (spec §10, M3). Each running instance owns a slot
// in the autosave directory:
//   <id>.p3d    the latest autosave (a normal project file, written atomically)
//   <id>.json   what it is: original project path, revision, times
//   <id>.lock   held open exclusively by the running process
// A clean exit removes the slot. A slot whose lock is no longer held belongs to
// an instance that ended without cleaning up (crash, kill, power loss): its
// autosave is recoverable. One autosave per instance; stale slots older than
// the retention period are pruned.
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pixel3d/common/diagnostics.hpp"

namespace pixel3d {

class AutosaveSlot {
 public:
  static Expected<std::unique_ptr<AutosaveSlot>> acquire(const std::filesystem::path& dir);
  ~AutosaveSlot();  // releases the lock but keeps the files (as a crash would)
  AutosaveSlot(const AutosaveSlot&) = delete;
  AutosaveSlot& operator=(const AutosaveSlot&) = delete;

  const std::string& id() const { return id_; }
  std::filesystem::path project_file() const { return dir_ / (id_ + ".p3d"); }
  // Records what the autosave holds (call after writing project_file()).
  Message write_info(const std::string& project_path, uint64_t revision);
  // Normal shutdown: removes the autosave, its info and the lock.
  void clean_exit();

 private:
  AutosaveSlot(std::filesystem::path dir, std::string id, void* lock);
  std::filesystem::path dir_;
  std::string id_;
  void* lock_ = nullptr;  // platform lock handle
  bool cleaned_ = false;
};

struct RecoverableSession {
  std::string id;
  std::filesystem::path project_file;
  std::string original_project;  // empty if it was never saved
  uint64_t revision = 0;
  std::string saved_at;           // ISO 8601 UTC
};

// Sessions in `dir` whose owner is gone and that have an autosave. Slots older
// than `retention` (and leftovers without an autosave) are deleted.
std::vector<RecoverableSession> find_recoverable(const std::filesystem::path& dir,
                                                 std::chrono::hours retention = std::chrono::hours(24 * 30));
void discard(const std::filesystem::path& dir, const RecoverableSession& s);

}  // namespace pixel3d
