// M3 core: background jobs, autosave / crash recovery, checkpoint retention.
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "pixel3d/engine/autosave.hpp"
#include "pixel3d/engine/job_runner.hpp"

using namespace pixel3d;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

std::string fixture(const std::string& name, const std::string& role) {
  return std::string(PIXEL3D_FIXTURES_DIR) + "/" + name + "/" + role + ".png";
}

std::vector<ViewImport> fst(const std::string& name) {
  return {{ViewRole::Front, fixture(name, "front"), {}},
          {ViewRole::Side, fixture(name, "side"), {}},
          {ViewRole::Top, fixture(name, "top"), {}}};
}

fs::path fresh_dir(const std::string& name) {
  const fs::path d = fs::temp_directory_path() / "pixel3d_tests" / name;
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d);
  return d;
}

}  // namespace

TEST_CASE("jobs: operations run in order off the caller's thread") {
  std::atomic<int> progress_calls{0};
  JobRunner runner(Source::Gui, [&](const std::string&, float, std::string_view) { ++progress_calls; });
  std::vector<std::string> done;
  std::mutex mu;
  const auto caller = std::this_thread::get_id();
  std::thread::id worker;
  auto record = [&](const std::string& name, const OpResult& r) {
    std::lock_guard lock(mu);
    done.push_back(name + (r.success ? ":ok" : ":fail"));
    worker = std::this_thread::get_id();
  };
  runner.post("import", [](Session& s, const JobContext&) { return s.import_views(fst("A_synthetic_chair")); }, record);
  runner.post("reconstruct", [](Session& s, const JobContext& j) { return s.reconstruct(ReconstructOptions{}, j); }, record);
  runner.post("validate", [](Session& s, const JobContext& j) { return s.validate(ValidateOptions{}, j); }, record);
  runner.wait_idle();
  CHECK(done == std::vector<std::string>{"import:ok", "reconstruct:ok", "validate:ok"});
  CHECK(worker != caller);
  CHECK(progress_calls > 0);
  const SessionView v = runner.view();
  CHECK(v.status.status == ValidationStatus::Pass);
  CHECK(v.doc.solid != nullptr);
  CHECK(v.can_undo);
  CHECK(v.log.size() == 3);
  CHECK_FALSE(runner.busy());
}

TEST_CASE("jobs: cancel stops the running job and drops queued ones") {
  JobRunner runner;
  std::atomic<bool> started{false};
  OpResult first, second;
  bool second_ran = false;
  runner.post(
      "wait",
      [&](Session&, const JobContext& j) {
        started = true;
        while (!j.stop.stop_requested()) std::this_thread::sleep_for(1ms);
        return OpResult::fail(Message{"job.cancelled", "cancelled", Severity::Error, {}});
      },
      [&](const std::string&, const OpResult& r) { first = r; });
  runner.post("never", [&](Session& s, const JobContext&) { second_ran = true; return s.current_status(); },
              [&](const std::string&, const OpResult& r) { second = r; });
  while (!started) std::this_thread::sleep_for(1ms);
  runner.cancel();
  runner.wait_idle();
  CHECK(first.errors.at(0).code == "job.cancelled");
  CHECK_FALSE(second_ran);
  // The runner keeps working after a cancel.
  OpResult third;
  runner.post("again", [](Session& s, const JobContext&) { return s.current_status(); },
              [&](const std::string&, const OpResult& r) { third = r; });
  runner.wait_idle();
  CHECK(third.success);
}

TEST_CASE("jobs: a throwing client operation becomes an error result") {
  JobRunner runner;
  OpResult r;
  runner.post("boom", [](Session&, const JobContext&) -> OpResult { throw std::runtime_error("boom"); },
              [&](const std::string&, const OpResult& res) { r = res; });
  runner.wait_idle();
  CHECK_FALSE(r.success);
  CHECK(r.errors.at(0).code == "internal.exception");
}

TEST_CASE("jobs: views can be read while jobs run") {
  JobRunner runner;
  runner.post("import", [](Session& s, const JobContext&) { return s.import_views(fst("B_hand_chair")); });
  runner.post("validate", [](Session& s, const JobContext& j) { return s.validate(ValidateOptions{}, j); });
  int reads = 0;
  while (runner.busy()) {
    const SessionView v = runner.view();
    reads += v.doc.views.size() <= 6 ? 1 : 0;
  }
  runner.wait_idle();
  CHECK(reads > 0);
  CHECK(runner.view().status.status == ValidationStatus::Pass);
}

TEST_CASE("autosave: a crashed session is recoverable, a clean exit is not") {
  const fs::path dir = fresh_dir("autosave");
  Session s;
  REQUIRE(s.import_views(fst("A_synthetic_chair")).success);
  REQUIRE(s.reconstruct(ReconstructOptions{}).success);
  {
    auto slot = AutosaveSlot::acquire(dir);
    REQUIRE(slot);
    REQUIRE(s.save_copy((*slot)->project_file()).success);
    REQUIRE((*slot)->write_info("C:/work/chair.p3d", s.document().revision).code == "ok");
    CHECK(find_recoverable(dir).empty());  // the lock is held: a running instance
    // Destroyed without clean_exit: what a crash leaves behind.
  }
  const auto rec = find_recoverable(dir);
  REQUIRE(rec.size() == 1);
  CHECK(rec[0].original_project == "C:/work/chair.p3d");
  CHECK(rec[0].revision == s.document().revision);
  Session t;
  REQUIRE(t.load(rec[0].project_file).success);
  CHECK(t.document().views.size() == 3);
  CHECK(t.reconstruct(ReconstructOptions{}).data["cache_hit"] == true);
  discard(dir, rec[0]);
  CHECK(find_recoverable(dir).empty());

  auto slot = AutosaveSlot::acquire(dir);
  REQUIRE(slot);
  REQUIRE(s.save_copy((*slot)->project_file()).success);
  (*slot)->clean_exit();
  slot->reset();
  CHECK(find_recoverable(dir).empty());
  CHECK(fs::is_empty(dir));
}

TEST_CASE("autosave: old and orphaned slots are pruned") {
  const fs::path dir = fresh_dir("autosave_prune");
  Session s;
  REQUIRE(s.import_views(fst("A_synthetic_chair")).success);
  std::string id;
  {
    auto slot = AutosaveSlot::acquire(dir);
    REQUIRE(slot);
    id = (*slot)->id();
    REQUIRE(s.save_copy((*slot)->project_file()).success);
  }
  fs::last_write_time(dir / (id + ".p3d"), fs::file_time_type::clock::now() - std::chrono::hours(24 * 40));
  { std::ofstream(dir / "orphan.lock") << "x"; }
  CHECK(find_recoverable(dir).empty());
  CHECK(fs::is_empty(dir));
}

TEST_CASE("checkpoints: automatic ones are capped, named ones kept") {
  Session s;
  REQUIRE(s.import_views(fst("A_synthetic_chair")).success);
  REQUIRE(s.create_checkpoint("mine").success);
  for (int i = 0; i < 24; ++i) REQUIRE(s.reconstruct(ReconstructOptions{}).success);
  int autos = 0;
  bool mine = false, newest = false, oldest = false;
  for (const auto& n : s.checkpoint_names()) {
    autos += n.rfind("auto:", 0) == 0 ? 1 : 0;
    mine |= n == "mine";
    newest |= n == "auto:before-reconstruct-24";
    oldest |= n == "auto:before-reconstruct-1";
  }
  CHECK(autos == 20);
  CHECK(mine);
  CHECK(newest);
  CHECK_FALSE(oldest);
}
