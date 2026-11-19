// SPDX-License-Identifier: Apache-2.0
// Deferred destruction for one context (0007 d23): ar_scene_destroy hands the scene here and
// returns, and this context's reaper thread frees it. Freeing a large scene is slow (device frees
// synchronise the GPU, host mesh arrays go back to the OS: ~110 ms on B-5M) and must not block
// Blender's main thread beyond 0009 B-21. Frees run one at a time in submission order.
// Ref: 0007 decision 23 (no engine call blocks the main thread beyond B-21), 0001 D2.5 (engine
// threads run under fault handling; a poisoned context's memory may be corrupt), D2.6.
#pragma once
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "host/fault.h"

namespace astroray::host {

class Reaper {
 public:
  explicit Reaper(ContextState& ctx) : ctx_(ctx) {}
  ~Reaper() { drain(); }
  Reaper(const Reaper&) = delete;
  Reaper& operator=(const Reaper&) = delete;

  // Queues `item` (its last reference) to be released on the reaper thread, which starts at the
  // first submit. `what` names it in the log line written after the free. Throws when the thread
  // cannot start; the item is then still owned by the caller's copy, if any.
  void submit(std::shared_ptr<void> item, std::string what);
  // Frees everything queued, then joins the thread; idempotent. After a fault ended the thread
  // (context poisoned) the remaining items are leaked with a log line, never freed here: their
  // memory may be corrupt and this is the host's thread.
  void drain();
  // Gate-build test hook ASTRORAY_TEST_REAPER_HOLD: nothing is freed until drain(), so a test can
  // see that a destroy returned before its free and that the context destroy waited for it.
  void hold_until_drain() { hold_ = true; }

 private:
  void main();

  ContextState& ctx_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::pair<std::shared_ptr<void>, std::string>> queue_;
  bool stop_ = false;
  bool hold_ = false;  // set before the first submit, read under mutex_
  std::thread thread_;
};

}  // namespace astroray::host
