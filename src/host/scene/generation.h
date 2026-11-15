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
#include <set>
#include <string>
#include <thread>
#include <utility>
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
  // Runs the queued jobs and joins; idempotent. submit() stays safe afterwards.
  void shutdown();
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
  // Key-set changes of this generation as (kind, key) pairs, applied in order by the
  // scene's known-key set (optimistically at commit, rebuilt when an earlier one drops).
  bool known_reset = false;
  std::set<std::pair<uint32_t, uint64_t>> known_removed, known_added;
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
  // Assigns the generation number and queues the generation for activation. `on_commit`
  // runs under the tracker lock before the generation is queued, so a concurrent drop
  // never sees the queue without it.
  uint64_t commit(const std::shared_ptr<Pending>& p, const std::function<void()>& on_commit);
  // Called under the tracker lock after a generation was dropped, with the generations
  // still queued, in order.
  using DropHook = std::function<void(const std::deque<std::shared_ptr<Pending>>&)>;
  void set_drop_hook(DropHook hook) { drop_hook_ = std::move(hook); }
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
  DropHook drop_hook_;
  uint64_t next_ = 1;
  uint64_t active_ = 0;
  uint64_t failed_ = 0;
};

}  // namespace astroray::host
