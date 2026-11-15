// SPDX-License-Identifier: Apache-2.0
#include "host/scene/arena.h"

#include <new>
#include <stdexcept>
#include <utility>

namespace astroray::host {

namespace {
constexpr size_t kPage = 4096;
}

void* Arena::map(size_t bytes) { return ::operator new(bytes, std::align_val_t{kPage}); }
void Arena::unmap(void* p) { ::operator delete(p, std::align_val_t{kPage}); }

Arena::Arena(size_t chunk_bytes) : chunk_bytes_(chunk_bytes) {
  if (chunk_bytes == 0) throw std::invalid_argument("arena: zero chunk size");
}

Arena::~Arena() {
  for (auto& c : chunks_)
    if (c && c->mem) unmap(c->mem);
}

void Arena::set_prefault_request(std::function<void()> request) {
  std::lock_guard<std::mutex> lock(mutex_);
  request_ = std::move(request);
}

uint32_t Arena::take_chunk_locked() {
  if (!ready_.empty()) {
    uint32_t id = ready_.back();
    ready_.pop_back();
    return id;
  }
  if (!free_.empty()) {
    uint32_t id = free_.back();
    free_.pop_back();
    return id;
  }
  // Not yet faulted: the pages commit on first touch, by the caller.
  auto c = std::make_unique<Chunk>();
  c->mem = map(chunk_bytes_);
  c->size = chunk_bytes_;
  chunks_.push_back(std::move(c));
  return static_cast<uint32_t>(chunks_.size() - 1);
}

Arena::Block Arena::alloc(size_t bytes, size_t align) {
  if (bytes == 0) throw std::invalid_argument("arena: zero-byte block");
  if (align == 0 || (align & (align - 1)) != 0 || align > kPage) throw std::invalid_argument("arena: bad alignment");
  std::function<void()> request;
  Block out;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bytes + align > chunk_bytes_) {
      auto c = std::make_unique<Chunk>();
      c->mem = map(bytes);
      c->size = bytes;
      c->used = bytes;
      c->live = 1;
      c->dedicated = true;
      chunks_.push_back(std::move(c));
      out = {chunks_.back()->mem, bytes, static_cast<uint32_t>(chunks_.size() - 1)};
      return out;
    }
    auto fit = [&](Chunk& c) {
      size_t off = (c.used + align - 1) & ~(align - 1);
      return off + bytes <= c.size ? off + 1 : 0;  // offset + 1, 0 = does not fit
    };
    size_t off1 = current_ >= 0 ? fit(*chunks_[size_t(current_)]) : 0;
    if (!off1) {
      if (current_ >= 0 && chunks_[size_t(current_)]->live == 0) free_.push_back(uint32_t(current_));
      current_ = take_chunk_locked();
      chunks_[size_t(current_)]->used = 0;
      off1 = fit(*chunks_[size_t(current_)]);
      if (ready_.empty() && free_.empty() && !ahead_pending_ && request_) {
        ahead_pending_ = true;
        request = request_;
      }
    }
    Chunk& c = *chunks_[size_t(current_)];
    size_t off = off1 - 1;
    c.used = off + bytes;
    ++c.live;
    out = {static_cast<char*>(c.mem) + off, bytes, static_cast<uint32_t>(current_)};
  }
  if (request) request();
  return out;
}

void Arena::release(const Block& b) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (b.chunk >= chunks_.size() || !chunks_[b.chunk] || chunks_[b.chunk]->live == 0)
    throw std::invalid_argument("arena: release of a block that is not live");
  Chunk& c = *chunks_[b.chunk];
  if (--c.live != 0) return;
  if (c.dedicated) {
    unmap(c.mem);
    chunks_[b.chunk].reset();
  } else if (int64_t(b.chunk) == current_) {
    c.used = 0;
  } else {
    c.used = 0;
    free_.push_back(b.chunk);
  }
}

void Arena::prefault_one() noexcept {
  try {
    void* mem = map(chunk_bytes_);
    for (size_t i = 0; i < chunk_bytes_; i += kPage) static_cast<volatile char*>(mem)[i] = 0;
    auto c = std::make_unique<Chunk>();
    c->mem = mem;
    c->size = chunk_bytes_;
    std::lock_guard<std::mutex> lock(mutex_);
    chunks_.push_back(std::move(c));
    ready_.push_back(static_cast<uint32_t>(chunks_.size() - 1));
    ++prefaulted_;
    ahead_pending_ = false;
  } catch (...) {
    std::lock_guard<std::mutex> lock(mutex_);
    ahead_pending_ = false;
  }
}

size_t Arena::chunks_allocated() const {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t n = 0;
  for (const auto& c : chunks_) n += (c && c->mem) ? 1 : 0;
  return n;
}
size_t Arena::chunks_free() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ready_.size() + free_.size();
}
size_t Arena::chunks_prefaulted() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return prefaulted_;
}

}  // namespace astroray::host
