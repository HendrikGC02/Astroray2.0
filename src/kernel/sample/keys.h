// SPDX-License-Identifier: Apache-2.0
// Sampler keys (id, index) of camera and light paths. No pixel or sample number
// is stored in the path: the camera key is rebuilt from the sample id and the
// pass's active list, which costs one 4-byte read.
// Ref: docs/decisions/0003-spectral-model-and-path-state.md d23 (Keys).
#pragma once
#include <cstdint>

#include "core/real.h"
#include "portability.h"

AR_NS_BEGIN

struct SamplerKey {
  uint32_t id;
  uint32_t index;
};

// Light-path ids start here so they never collide with a pixel id.
inline constexpr uint32_t LIGHT_ID_BASE = 0x80000000u;

// Pixel id at the allocated film size.
AR_FN uint32_t pixel_id(uint32_t x, uint32_t y, uint32_t width) { return y * width + x; }

// Camera path: pixel = active_list[sample_id / k], index = n + sample_id % k,
// with n (samples already taken per pixel) and k (samples per active pixel in
// this pass) pass-uniform.
AR_FN SamplerKey camera_key(const uint32_t* active_list, uint32_t sample_id, uint32_t n, uint32_t k) {
  AR_ASSERT(k != 0u);
  return SamplerKey{load_ro(active_list + sample_id / k), n + sample_id % k};
}

// Light path: id = 2^31 + sample_id, index = the pass number in the epoch.
AR_FN SamplerKey light_key(uint32_t sample_id, uint32_t pass) {
  AR_ASSERT(sample_id < LIGHT_ID_BASE);
  return SamplerKey{LIGHT_ID_BASE + sample_id, pass};
}

AR_NS_END
