// SPDX-License-Identifier: Apache-2.0
// The scene's staging arena: host memory that Python wraps as numpy views and fills in
// place, so staged arrays are never copied (0007 decision 6, 0001 D2.3). It grows in
// 64 MiB chunks; a worker pre-faults the next chunk before it is needed. A pointer stays
// valid until its block is released, and a chunk is recycled once every block in it has
// been released (after the jobs that read it finished).
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace astroray::host {

class Arena {
 public:
  static constexpr size_t kChunkBytes = size_t(64) << 20;

  struct Block {
    void* ptr = nullptr;
    size_t bytes = 0;
    uint32_t chunk = 0;  // chunk id, for release
  };

  // chunk_bytes is a parameter so tests can use small chunks.
  explicit Arena(size_t chunk_bytes = kChunkBytes);
  ~Arena();
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // Called (outside the arena lock) when the arena wants a chunk pre-faulted; the callee
  // must arrange a call of prefault_one() on a worker. Set before the first alloc.
  void set_prefault_request(std::function<void()> request);

  // Zero bytes throws std::invalid_argument; a failed allocation throws std::bad_alloc.
  // A request larger than a chunk gets a chunk of its own, freed on release.
  Block alloc(size_t bytes, size_t align = 64);
  void release(const Block& b);

  // Worker side: allocates one chunk and touches every page. Never throws.
  void prefault_one() noexcept;

  size_t chunk_bytes() const { return chunk_bytes_; }
  size_t chunks_allocated() const;  // memory-holding chunks (including free and ready ones)
  size_t chunks_free() const;       // recycled or pre-faulted chunks waiting for use
  size_t chunks_prefaulted() const;  // chunks a worker has faulted so far

 private:
  struct Chunk {
    void* mem = nullptr;
    size_t size = 0;
    size_t used = 0;
    size_t live = 0;
    bool dedicated = false;
  };
  static void* map(size_t bytes);
  static void unmap(void* p);
  uint32_t take_chunk_locked();

  const size_t chunk_bytes_;
  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<Chunk>> chunks_;
  std::vector<uint32_t> ready_;  // pre-faulted, never used
  std::vector<uint32_t> free_;   // recycled
  int64_t current_ = -1;
  bool ahead_pending_ = false;
  size_t prefaulted_ = 0;
  std::function<void()> request_;
};

}  // namespace astroray::host
