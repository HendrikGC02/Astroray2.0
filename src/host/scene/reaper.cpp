// SPDX-License-Identifier: Apache-2.0
#include "host/scene/reaper.h"

#include <chrono>
#include <cstdio>
#include <stdexcept>

#include "host/thread.h"

namespace astroray::host {

void Reaper::submit(std::shared_ptr<void> item, std::string what) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stop_) throw std::logic_error("reaper: submit after drain");
  if (!thread_.joinable()) thread_ = start_engine_thread(ctx_, [this] { main(); });
  queue_.emplace_back(std::move(item), std::move(what));
  wake_.notify_one();
}

void Reaper::main() {
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    wake_.wait(lock, [this] { return stop_ || (!queue_.empty() && !hold_); });
    if (queue_.empty()) break;  // stop_ with nothing left
    // Stays queued while it is freed: a fault in its destructor leaves it for drain() to leak.
    std::shared_ptr<void>& item = queue_.front().first;
    std::string what = queue_.front().second;
    lock.unlock();
    const auto t0 = std::chrono::steady_clock::now();
    item.reset();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    char text[64];
    std::snprintf(text, sizeof text, " freed in %.1f ms", ms);
    ctx_.log.line("INFO", what + text);
    lock.lock();
    queue_.pop_front();
  }
}

void Reaper::drain() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  wake_.notify_one();
  if (thread_.joinable()) thread_.join();
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_.empty()) return;
  // Left only when a fault ended the thread mid-free; deliberately leaked.
  const size_t n = queue_.size();
  for (auto& q : queue_) new std::shared_ptr<void>(std::move(q.first));
  queue_.clear();
  try {
    ctx_.log.line("ERROR", "reaper: an engine fault stopped deferred frees; " + std::to_string(n) +
                               " item(s) were not freed (the context is poisoned)");
  } catch (...) {
  }
}

}  // namespace astroray::host
