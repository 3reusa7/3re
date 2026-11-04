#pragma once
// Runs Session operations off the caller's thread, one at a time and in order,
// so an interactive client never freezes. The JobRunner owns the Session: only
// the worker touches it. Clients read a SessionView — an immutable snapshot
// published after every operation — from any thread. Callbacks run on the
// worker thread; a GUI marshals them onto its own thread.
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "pixel3d/engine/session.hpp"

namespace pixel3d {

struct SessionView {
  Document doc;  // heavy members are shared, so this copy is cheap
  std::vector<std::string> checkpoints;
  bool can_undo = false, can_redo = false;
  std::vector<LogEntry> log;
  OpResult status;           // Session::current_status() at publication
  nlohmann::json conflicts;  // Session::get_conflicts().data
  Selection selection;       // the session's current selection
  std::vector<std::string> history;  // undo list: operation names, [0] = start
  size_t history_cursor = 0;         // index of the current state in `history`
};

class JobRunner {
 public:
  using Op = std::function<OpResult(Session&, const JobContext&)>;
  using Done = std::function<void(const std::string& name, const OpResult& result)>;
  using Progress = std::function<void(const std::string& name, float fraction, std::string_view stage)>;

  explicit JobRunner(Source source = Source::Gui, Progress progress = {});
  ~JobRunner();  // cancels the running job, drops queued ones, joins
  JobRunner(const JobRunner&) = delete;
  JobRunner& operator=(const JobRunner&) = delete;

  // Queues an operation. `done` runs on the worker after the new view is published.
  void post(std::string name, Op op, Done done = {});
  // Requests stop of the running job and drops queued jobs that have not started.
  void cancel();
  bool busy() const;
  void wait_idle();

  SessionView view() const;

 private:
  struct Job {
    std::string name;
    Op op;
    Done done;
  };
  void run(std::stop_token stop);
  void publish();

  Session session_;
  Progress progress_;
  mutable std::mutex mu_;
  std::condition_variable cv_, idle_cv_;
  std::deque<Job> queue_;
  bool running_ = false;
  std::stop_source job_stop_;
  SessionView view_;
  std::jthread worker_;  // last member: starts after everything above is constructed
};

}  // namespace pixel3d
