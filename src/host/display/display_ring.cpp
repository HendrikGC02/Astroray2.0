// SPDX-License-Identifier: Apache-2.0
#include "host/display/display_ring.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>

namespace astroray::host {

namespace {
void* heap_allocate(uint64_t bytes) { return std::malloc(bytes); }
void heap_release(void* p) { std::free(p); }
}  // namespace

DisplayRing::DisplayRing(uint64_t slot_bytes, SlotAllocator allocator) : allocator_(allocator), slot_bytes_(slot_bytes) {
  if (slot_bytes == 0) throw std::invalid_argument("display ring: slot of zero bytes");
  if (allocator_.allocate == nullptr || allocator_.release == nullptr) allocator_ = {heap_allocate, heap_release};
  for (uint32_t i = 0; i < kSlots; ++i) {
    slots_[i].data = allocator_.allocate(slot_bytes);
    if (slots_[i].data == nullptr) {
      for (uint32_t k = 0; k < i; ++k) allocator_.release(slots_[k].data);
      throw std::bad_alloc();
    }
  }
}

DisplayRing::~DisplayRing() {
  for (Slot& s : slots_) allocator_.release(s.data);
}

template <class Copy>
uint64_t DisplayRing::publish_with(uint64_t generation, uint32_t width, uint32_t height, DisplayFormat format,
                                   Copy&& copy) {
  if (width == 0 || height == 0) throw std::invalid_argument("display ring: empty frame");
  const uint64_t row_bytes = uint64_t(width) * display_pixel_bytes(format);
  const uint64_t bytes = row_bytes * height;
  if (bytes > slot_bytes_)
    throw std::invalid_argument("display ring: frame of " + std::to_string(bytes) + " bytes exceeds the " +
                                std::to_string(slot_bytes_) + "-byte slot");
  Slot* slot = nullptr;
  uint64_t id = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation < last_generation_)
      throw std::logic_error("display ring: generation " + std::to_string(generation) + " is below the published " +
                             std::to_string(last_generation_));
    // An empty slot first, else the oldest ready frame nobody is reading (the frame being
    // published supersedes it).
    for (Slot& s : slots_) {
      if (s.state == State::EMPTY) {
        slot = &s;
        break;
      }
    }
    if (slot == nullptr) {
      for (Slot& s : slots_)
        if (s.state == State::READY && s.readers == 0 &&
            (slot == nullptr || s.frame.frame_id < slot->frame.frame_id))
          slot = &s;
    }
    if (slot == nullptr) {
      ++dropped_;
      return 0;
    }
    slot->state = State::FILLING;
    slot->frame = DisplayFrame{};
    last_generation_ = generation;
    id = next_frame_id_++;
  }
  // The copy runs outside the lock: the slot is FILLING, so neither acquire nor another publish
  // touches it.
  try {
    copy(slot->data, bytes);
  } catch (...) {
    std::lock_guard<std::mutex> lock(mutex_);
    slot->state = State::EMPTY;
    throw;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  slot->frame = DisplayFrame{id, generation, width, height, format, uint32_t(row_bytes), slot->data};
  slot->state = State::READY;
  return id;
}

uint64_t DisplayRing::publish(Device& device, const DeviceBuffer& src, uint64_t generation, uint32_t width,
                              uint32_t height, DisplayFormat format) {
  return publish_with(generation, width, height, format,
                      [&](void* dst, uint64_t bytes) { device.download(dst, src, 0, bytes); });
}

uint64_t DisplayRing::publish_host(const void* src, uint64_t generation, uint32_t width, uint32_t height,
                                   DisplayFormat format) {
  return publish_with(generation, width, height, format,
                      [&](void* dst, uint64_t bytes) { std::memcpy(dst, src, bytes); });
}

std::optional<DisplayFrame> DisplayRing::acquire(uint64_t after_frame) {
  std::lock_guard<std::mutex> lock(mutex_);
  Slot* best = nullptr;
  for (Slot& s : slots_)
    if (s.state == State::READY && s.frame.frame_id > after_frame &&
        (best == nullptr || s.frame.frame_id > best->frame.frame_id))
      best = &s;
  if (best == nullptr) return std::nullopt;
  ++best->readers;
  return best->frame;
}

void DisplayRing::release(uint64_t frame_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (Slot& s : slots_) {
    if (s.state == State::READY && s.frame.frame_id == frame_id && s.readers > 0) {
      --s.readers;
      return;
    }
  }
  throw std::invalid_argument("display ring: frame " + std::to_string(frame_id) + " is not acquired");
}

uint64_t DisplayRing::dropped() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dropped_;
}

}  // namespace astroray::host
