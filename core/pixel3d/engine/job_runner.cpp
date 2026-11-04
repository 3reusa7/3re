#include "pixel3d/engine/job_runner.hpp"

namespace pixel3d {

JobRunner::JobRunner(Source source, Progress progress)
    : session_(source), progress_(std::move(progress)), worker_([this](std::stop_token st) { run(st); }) {
  std::lock_guard lock(mu_);
  view_.status = session_.current_status();
}

JobRunner::~JobRunner() {
  {
    std::lock_guard lock(mu_);
    queue_.clear();
    job_stop_.request_stop();
  }
  worker_.request_stop();
  cv_.notify_all();
  // jthread joins in its destructor.
}

void JobRunner::post(std::string name, Op op, Done done) {
  {
    std::lock_guard lock(mu_);
    queue_.push_back(Job{std::move(name), std::move(op), std::move(done)});
  }
  cv_.notify_all();
}

void JobRunner::cancel() {
  std::lock_guard lock(mu_);
  queue_.clear();
  job_stop_.request_stop();
}

bool JobRunner::busy() const {
  std::lock_guard lock(mu_);
  return running_ || !queue_.empty();
}

void JobRunner::wait_idle() {
  std::unique_lock lock(mu_);
  idle_cv_.wait(lock, [&] { return !running_ && queue_.empty(); });
}

SessionView JobRunner::view() const {
  std::lock_guard lock(mu_);
  return view_;
}

void JobRunner::publish() {
  SessionView v;
  v.doc = session_.snapshot();
  v.checkpoints = session_.checkpoint_names();
  v.can_undo = session_.can_undo();
  v.can_redo = session_.can_redo();
  v.log = session_.log();
  v.status = session_.current_status();
  v.conflicts = session_.get_conflicts().data;
  v.selection = session_.selection();
  v.history = session_.history_steps();
  v.history_cursor = session_.history_cursor();
  std::lock_guard lock(mu_);
  view_ = std::move(v);
}

void JobRunner::run(std::stop_token stop) {
  while (true) {
    Job job;
    std::stop_source job_stop;
    {
      std::unique_lock lock(mu_);
      cv_.wait(lock, [&] { return stop.stop_requested() || !queue_.empty(); });
      if (stop.stop_requested()) return;
      job = std::move(queue_.front());
      queue_.pop_front();
      running_ = true;
      job_stop_ = std::stop_source();
      job_stop = job_stop_;
    }
    JobContext ctx;
    ctx.stop = job_stop.get_token();
    if (progress_) {
      const std::string name = job.name;
      ctx.progress = [this, name](float f, std::string_view stage) { progress_(name, f, stage); };
    }
    OpResult r;
    try {
      r = job.op(session_, ctx);
    } catch (const std::exception& e) {  // Session never throws; a client lambda might
      r = OpResult::fail(Message{"internal.exception", e.what(), Severity::Error, {}});
    }
    publish();
    if (job.done) job.done(job.name, r);
    {
      std::lock_guard lock(mu_);
      running_ = false;
    }
    idle_cv_.notify_all();
  }
}

}  // namespace pixel3d
