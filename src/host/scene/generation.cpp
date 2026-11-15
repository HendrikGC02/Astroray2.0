// SPDX-License-Identifier: Apache-2.0
#include "host/scene/generation.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "host/thread.h"

namespace astroray::host {

WorkerPool::WorkerPool(ContextState& ctx, unsigned workers) {
  if (workers == 0) throw std::invalid_argument("worker pool: zero workers");
  try {
    for (unsigned i = 0; i < workers; ++i) threads_.push_back(start_engine_thread(ctx, [this] { main(); }));
  } catch (...) {
    shutdown();  // a constructor that throws never runs the destructor
    throw;
  }
}

WorkerPool::~WorkerPool() { shutdown(); }

void WorkerPool::shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  for (std::thread& t : threads_)
    if (t.joinable()) t.join();
}

void WorkerPool::submit(std::function<void()> job) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(std::move(job));
  }
  wake_.notify_one();
}

void WorkerPool::main() {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (queue_.empty()) return;  // stop_ and drained
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    try {
      job();
    } catch (...) {
      // Jobs report their own failures; an escape must not end the worker.
    }
  }
}

void GenerationTracker::job_started(Pending& p) {
  std::lock_guard<std::mutex> lock(mutex_);
  ++p.outstanding;
}

void GenerationTracker::job_done(const std::shared_ptr<Pending>& p, uint64_t key, const std::string& fail) {
  bool activated;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    --p->outstanding;
    if (!fail.empty() && !p->failed) {
      p->failed = true;
      p->failed_key = key;
      p->failed_detail = fail;
    }
    activated = pump_locked();
  }
  if (activated) notify_listeners();
}

uint64_t GenerationTracker::commit(const std::shared_ptr<Pending>& p, const std::function<void()>& on_commit) {
  bool activated;
  uint64_t number;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (on_commit) on_commit();
    p->number = next_++;
    p->committed = true;
    queue_.push_back(p);
    number = p->number;
    activated = pump_locked();
  }
  if (activated) notify_listeners();
  return number;
}

void GenerationTracker::abort(const std::shared_ptr<Pending>& p) {
  bool activated;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    p->aborted = true;
    aborted_.push_back(p);
    activated = pump_locked();
  }
  if (activated) notify_listeners();
}

bool GenerationTracker::pump_locked() {
  bool activated = false;
  // Waiters must wake even if a callback throws.
  struct Wake {
    std::condition_variable& cv;
    ~Wake() { cv.notify_all(); }
  } wake{idle_};
  for (size_t i = 0; i < aborted_.size();) {
    if (aborted_[i]->outstanding == 0) {
      for (auto& f : aborted_[i]->on_dropped) f();
      aborted_.erase(aborted_.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      ++i;
    }
  }
  while (!queue_.empty() && queue_.front()->outstanding == 0) {
    std::shared_ptr<Pending> p = std::move(queue_.front());
    queue_.pop_front();
    if (p->failed) {
      for (auto& f : p->on_dropped) f();
      failed_ = p->number;
      const double value[2] = {static_cast<double>(p->number), 0.0};
      report_.raise(AR_REPORT_GENERATION_REJECTED, p->failed_key, p->failed_detail, 1, value, p->number);
      if (drop_hook_) drop_hook_(queue_);
      ctx_.log.line("ERROR", "generation " + std::to_string(p->number) + " rejected: key " +
                                 std::to_string(p->failed_key) + ": " + p->failed_detail);
    } else {
      for (auto& f : p->on_active) f();
      active_ = p->number;
      report_.on_generation_active(p->number, p->subjects, p->reset);
      activated = true;
    }
  }
  return activated;
}

uint64_t GenerationTracker::add_listener(Listener listener) {
  std::lock_guard<std::mutex> lock(listeners_mutex_);
  const uint64_t id = next_listener_++;
  listeners_.emplace_back(id, std::move(listener));
  return id;
}

void GenerationTracker::remove_listener(uint64_t id) {
  // Taking the mutex waits for a notification in flight, so the listener is not running on return.
  std::lock_guard<std::mutex> lock(listeners_mutex_);
  listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(), [id](const auto& l) { return l.first == id; }),
                   listeners_.end());
}

void GenerationTracker::notify_listeners() {
  std::lock_guard<std::mutex> lock(listeners_mutex_);
  for (auto& l : listeners_) l.second();
}

uint64_t GenerationTracker::active() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_;
}
uint64_t GenerationTracker::pending() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return next_ - 1;
}
uint64_t GenerationTracker::failed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return failed_;
}
void GenerationTracker::wait_idle() {
  std::unique_lock<std::mutex> lock(mutex_);
  idle_.wait(lock, [this] { return queue_.empty() && aborted_.empty(); });
}

}  // namespace astroray::host
