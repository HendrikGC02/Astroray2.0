// SPDX-License-Identifier: Apache-2.0
// Generations and the scene's worker pool (0007 decisions 9, 23-25). An edit's jobs run on
// workers as soon as their item is committed; the edit's generation becomes active only
// when every job has succeeded, in commit order. A failed generation is dropped whole: its
// new objects are freed, the previous generation stays active, and a GENERATION_REJECTED
// entry names the key.
// Ref: 0007 decision 9 (outcomes), decision 25 (terms); 0001 D2.11 (GENERATION_REJECTED).
#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "host/fault.h"
#include "host/report.h"

namespace astroray::host {

// A queue of jobs run by threads started through start_engine_thread. The destructor
// runs every queued job, then joins.
class WorkerPool {
 public:
  WorkerPool(ContextState& ctx, unsigned workers);
  ~WorkerPool();
  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;
  void submit(std::function<void()> job);
  unsigned workers() const { return static_cast<unsigned>(threads_.size()); }

 private:
  void main();
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::function<void()>> queue_;
  bool stop_ = false;
  std::vector<std::thread> threads_;
};

// The jobs and callbacks of one edit. Created by the edit, shared with its workers.
struct Pending {
  uint64_t number = 0;  // set by commit
  bool reset = false;
  bool committed = false;
  bool aborted = false;
  unsigned outstanding = 0;  // item jobs started and not finished
  bool failed = false;
  uint64_t failed_key = 0;
  std::string failed_detail;
  std::vector<uint64_t> subjects;
  // Run under the tracker lock, so they must not call back into it.
  std::vector<std::function<void()>> on_active;   // generation became active
  std::vector<std::function<void()>> on_dropped;  // failed or aborted: free the new objects
};

class GenerationTracker {
 public:
  GenerationTracker(ContextState& ctx, Report& report) : ctx_(ctx), report_(report) {}

  void job_started(Pending& p);
  // Records the first failure of the generation (fail is empty on success).
  void job_done(const std::shared_ptr<Pending>& p, uint64_t key, const std::string& fail);
  // Assigns the generation number and queues the generation for activation.
  uint64_t commit(const std::shared_ptr<Pending>& p);
  // Drops the edit: its callbacks run once its running jobs have finished.
  void abort(const std::shared_ptr<Pending>& p);

  uint64_t active() const;
  uint64_t pending() const;  // last number handed out
  uint64_t failed() const;   // last rejected generation, 0 if none
  // Blocks until every committed generation has been activated or dropped.
  void wait_idle();

 private:
  void pump_locked();
  ContextState& ctx_;
  Report& report_;
  mutable std::mutex mutex_;
  std::condition_variable idle_;
  std::deque<std::shared_ptr<Pending>> queue_;
  std::vector<std::shared_ptr<Pending>> aborted_;
  uint64_t next_ = 1;
  uint64_t active_ = 0;
  uint64_t failed_ = 0;
};

}  // namespace astroray::host
