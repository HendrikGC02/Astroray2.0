// SPDX-License-Identifier: Apache-2.0
// The display ring: three host slots that hold finished frames for the addon's draw. The render
// thread publishes (copy from the device present buffer into the next free slot); the draw thread
// acquires the newest frame newer than the one it last drew and releases it when done. A slot
// being read is never overwritten, and a frame that cannot get a slot is dropped and counted,
// never queued: the producer never waits for Blender.
// Slots are plain host memory behind SlotAllocator so the pinned allocator of the CUDA device can
// be supplied without changing the ring; the copy is Device::download, which on the CPU device is
// a memcpy and on CUDA is the synchronous host copy until the device seam gains a copy stream.
// Ref: docs/decisions/0007-blender-integration.md d29 (3-slot ring, frames tagged with their
// generation), d25 (a frame is shown only if its generation >= the last presented), d30 (formats).
#pragma once
#include <cstdint>
#include <mutex>
#include <optional>

#include "host/device.h"

namespace astroray::host {

enum class DisplayFormat : uint32_t { RGBA16F = 0, RGBA8_SRGB = 1 };

inline uint32_t display_pixel_bytes(DisplayFormat f) { return f == DisplayFormat::RGBA16F ? 8u : 4u; }

// A frame as the consumer sees it. `data` stays valid until release(frame_id).
struct DisplayFrame {
  uint64_t frame_id = 0;    // 1, 2, ...: increases with every published frame
  uint64_t generation = 0;  // the scene generation the frame shows (0007 d25)
  uint32_t width = 0, height = 0;
  DisplayFormat format = DisplayFormat::RGBA16F;
  uint32_t row_bytes = 0;
  const void* data = nullptr;
};

// Where slot memory comes from. Both null selects the C++ heap.
struct SlotAllocator {
  void* (*allocate)(uint64_t bytes) = nullptr;
  void (*release)(void* ptr) = nullptr;
};

class DisplayRing {
 public:
  static constexpr uint32_t kSlots = 3;

  // Slots of `slot_bytes` each (the largest frame). Throws std::invalid_argument for 0 bytes.
  explicit DisplayRing(uint64_t slot_bytes, SlotAllocator allocator = {});
  ~DisplayRing();
  DisplayRing(const DisplayRing&) = delete;
  DisplayRing& operator=(const DisplayRing&) = delete;

  // Copies the height x row_bytes bytes at the start of `src` into a free slot and publishes it as
  // the newest frame. Returns the frame id, or 0 when every slot is being read (the frame is
  // dropped and counted). Throws std::invalid_argument for an empty frame or one larger than a
  // slot, and std::logic_error when `generation` is below the last published one.
  uint64_t publish(Device& device, const DeviceBuffer& src, uint64_t generation, uint32_t width, uint32_t height,
                   DisplayFormat format);
  // The same for bytes already on the host.
  uint64_t publish_host(const void* src, uint64_t generation, uint32_t width, uint32_t height,
                        DisplayFormat format);

  // The newest published frame with frame_id > after_frame, marked as being read; none when
  // there is no newer frame. Every acquire needs one release.
  std::optional<DisplayFrame> acquire(uint64_t after_frame);
  // Throws std::invalid_argument for a frame that is not currently acquired.
  void release(uint64_t frame_id);

  uint64_t dropped() const;

 private:
  enum class State : uint8_t { EMPTY, FILLING, READY };
  struct Slot {
    void* data = nullptr;
    State state = State::EMPTY;
    uint32_t readers = 0;
    DisplayFrame frame;
  };
  template <class Copy>
  uint64_t publish_with(uint64_t generation, uint32_t width, uint32_t height, DisplayFormat format, Copy&& copy);

  SlotAllocator allocator_;
  uint64_t slot_bytes_;
  mutable std::mutex mutex_;
  Slot slots_[kSlots];
  uint64_t next_frame_id_ = 1;
  uint64_t last_generation_ = 0;
  uint64_t dropped_ = 0;
};

}  // namespace astroray::host
