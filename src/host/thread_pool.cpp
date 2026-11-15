// SPDX-License-Identifier: Apache-2.0
#include "host/thread_pool.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

#include "host/thread.h"

namespace astroray::host {

ThreadPool::ThreadPool(ContextState& ctx, unsigned workers) : ctx_(ctx) {
  if (workers == 0) throw std::invalid_argument("thread pool: zero workers");
  threads_.reserve(workers);
  for (unsigned i = 0; i < workers; ++i) threads_.push_back(start_engine_thread(ctx, [this] { worker_main(); }));
}

ThreadPool::~ThreadPool() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  for (std::thread& t : threads_) t.join();
}

void ThreadPool::worker_main() {
  uint64_t seen = 0;
  for (;;) {
    const Body* body = nullptr;
    uint32_t count = 0;
    uint32_t chunk = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [&] { return stop_ || generation_ != seen; });
      if (stop_) return;
      seen = generation_;
      body = body_;
      count = count_;
      chunk = chunk_;
    }
    for (;;) {
      const uint64_t begin = next_.fetch_add(chunk);
      if (begin >= count) break;
      const uint32_t first = static_cast<uint32_t>(begin);
      const uint32_t last = static_cast<uint32_t>(std::min<uint64_t>(begin + chunk, count));
      try {
        (*body)(first, last);
      } catch (...) {
        // A C++ exception must not leave the thread (that would be an engine fault);
        // keep the first one and stop handing out chunks.
        std::lock_guard<std::mutex> lock(mutex_);
        if (!error_) error_ = std::current_exception();
        next_.store(count);
      }
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++finished_;
    }
    done_.notify_one();
  }
}

void ThreadPool::parallel_for(uint32_t count, uint32_t chunk, const Body& body) {
  if (chunk == 0) throw std::invalid_argument("parallel_for: chunk size zero");
  if (count == 0) return;
  std::lock_guard<std::mutex> run(run_mutex_);
  const auto poisoned = [this] {
    if (!ctx_.poisoned.load(std::memory_order_acquire)) return;
    throw std::runtime_error(ctx_.poison_message);
  };
  poisoned();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    body_ = &body;
    count_ = count;
    chunk_ = chunk;
    next_.store(0);
    finished_ = 0;
    error_ = nullptr;
    ++generation_;
  }
  wake_.notify_all();

  std::unique_lock<std::mutex> lock(mutex_);
  // The wake-up slice only bounds how long a faulted worker can go unnoticed.
  while (!done_.wait_for(lock, std::chrono::milliseconds(20),
                         [&] { return finished_ == threads_.size() || ctx_.poisoned.load(std::memory_order_acquire); })) {
  }
  if (finished_ != threads_.size()) {
    next_.store(count);
    lock.unlock();
    poisoned();
  }
  if (error_) std::rethrow_exception(error_);
}

}  // namespace astroray::host
