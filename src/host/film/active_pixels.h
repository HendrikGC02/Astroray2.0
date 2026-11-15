// SPDX-License-Identifier: Apache-2.0
// The film's active-pixel list: u32 plane indices of the pixels a pass samples, capacity the
// allocated pixel count, one allocation under the ledger's film category. The film owns it, the
// per-pass kernel film_build_active (kernel/stages/film_build_active.h) writes it at pass start,
// and every other stage only reads it until the pass drains (KernelGlobals::active_pixels).
// Ref: docs/decisions/0008-film-outputs-denoise.md d3 (active-pixel list, slot assignment),
// d21 (film bytes go to the film category); 0002 D8 (film_build_active).
#pragma once
#include <cstdint>

#include "host/device.h"
#include "kernel/film/film_write.h"
#include "kernel/stages/film_build_active.h"

namespace astroray::host {

class ActivePixels {
 public:
  // Allocates 4 * max_width * max_height bytes under LedgerCategory::FILM. Throws
  // std::invalid_argument for a zero size and what Device::allocate throws.
  ActivePixels(Device& device, uint32_t max_width, uint32_t max_height, Owner owner);
  ~ActivePixels();
  ActivePixels(const ActivePixels&) = delete;
  ActivePixels& operator=(const ActivePixels&) = delete;

  const DeviceBuffer& buffer() const { return buffer_; }
  // The device address to store in KernelGlobals::active_pixels.
  uint32_t* list() const { return static_cast<uint32_t*>(buffer_.ptr); }
  uint32_t capacity() const { return capacity_; }

  // M1a stand-in for the device-written count: u of the pass that film_build_active will write,
  // every pixel of the render sub-rectangle (film_build_active_count). Under M4 compaction the
  // kernel writes u to a device word the host reads before the refill (0008 d3) and this goes.
  // Throws std::length_error when u exceeds the capacity (the film was resized past this list).
  uint32_t plan(const ar::FilmData& film);
  // The count of the last plan(), 0 before the first.
  uint32_t count() const { return count_; }

 private:
  Device& device_;
  DeviceBuffer buffer_;
  uint32_t capacity_;
  uint32_t count_ = 0;
};

}  // namespace astroray::host
