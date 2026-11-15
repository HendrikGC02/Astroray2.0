// SPDX-License-Identifier: Apache-2.0
// A fixed pool of engine threads running chunked parallel loops. Owned by a session
// or scene (never process-global); its workers start through start_engine_thread, so
// a structured exception in a loop body writes a dump and poisons the context.
// Ref: pbrt-v4 src/pbrt/util/parallel.h ParallelFor (Apache-2.0): a shared atomic
// cursor hands out fixed-size chunks; 0002 D4 (chunks of 1,024 items).
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "host/fault.h"

namespace astroray::host {

inline constexpr uint32_t kDefaultChunk = 1024;

class ThreadPool {
 public:
  // fn(first, last) handles items [first, last) of one chunk.
  using Body = std::function<void(uint32_t first, uint32_t last)>;

  // ctx must outlive the pool. Throws std::invalid_argument for zero workers.
  ThreadPool(ContextState& ctx, unsigned workers);
  ~ThreadPool();
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  unsigned workers() const { return static_cast<unsigned>(threads_.size()); }

  // Runs body over [0, count) in chunks and returns when every chunk has finished.
  // Calls from several threads take turns. The first exception a body throws stops
  // the remaining chunks and is rethrown here. On a poisoned context (an engine
  // thread faulted, so a worker may be gone) it throws std::runtime_error with the
  // context's message instead of waiting forever; the caller must then stop using
  // the pool, because a surviving worker can still be inside its last chunk.
  void parallel_for(uint32_t count, const Body& body) { parallel_for(count, kDefaultChunk, body); }
  void parallel_for(uint32_t count, uint32_t chunk, const Body& body);

 private:
  void worker_main();

  ContextState& ctx_;
  std::vector<std::thread> threads_;
  std::mutex run_mutex_;  // one parallel_for at a time
  std::mutex mutex_;      // guards everything below except next_
  std::condition_variable wake_;
  std::condition_variable done_;
  const Body* body_ = nullptr;
  uint32_t count_ = 0;
  uint32_t chunk_ = 0;
  uint64_t generation_ = 0;  // bumped per job; a worker runs each generation once
  unsigned finished_ = 0;    // workers done with this generation
  bool stop_ = false;
  std::exception_ptr error_;
  std::atomic<uint64_t> next_{0};  // first index of the next unclaimed chunk
};

}  // namespace astroray::host
