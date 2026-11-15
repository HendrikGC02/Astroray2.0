// SPDX-License-Identifier: Apache-2.0
// The hit record every traversal backend (Embree, OptiX) writes and the intersect stages
// read. Plain data, so it compiles for the CPU and the GPU from this one source.
// Ref: docs/decisions/0002-kernel-architecture.md D11 (hit record).
#pragma once
#include <cstdint>

#include "../core/real.h"

AR_NS_BEGIN

// HitRecord::inst of a miss.
inline constexpr uint32_t kNoInstance = ~0u;

struct HitRecord {
  Real t;         // ray parameter of the hit
  uint32_t prim;  // triangle index within the instance's geometry
  uint32_t inst;  // instance index, kNoInstance for a miss
  Real u, v;      // barycentric weights of corners 1 and 2 of the triangle
};

AR_NS_END
