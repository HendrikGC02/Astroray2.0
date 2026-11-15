// SPDX-License-Identifier: Apache-2.0
// Closest-hit and occlusion queries over structure-of-arrays ray batches on an EmbreeScene.
// The CPU intersect stages call these (never per ray through a virtual call). Embree
// traverses in single precision whatever Real is (0002 C3).
// Ref: docs/decisions/0002-kernel-architecture.md D11 (hit record, mask, opaque shadow rule).
#pragma once
#include <cstdint>

#include "device/cpu/embree_scene.h"
#include "kernel/geom/hit.h"

namespace astroray::host {

// Ray i is origin (ox, oy, oz)[i] + t * direction (dx, dy, dz)[i] for t in [tmin[i], tmax[i]].
// Directions need not be normalised; t is in units of the direction's length.
struct RayBatch {
  const ar::Real *ox, *oy, *oz;
  const ar::Real *dx, *dy, *dz;
  const ar::Real *tmin, *tmax;
};

// The mask a shadow ray traces: only instances with the SHADOW bit (0002 D11).
inline constexpr uint8_t kShadowRayMask = 1u << 4;

// out[i] is the nearest hit of ray i against the instances sharing a bit with mask[i]; a
// miss has inst == ar::kNoInstance. Chunks run on `pool`.
void trace_closest(const EmbreeScene& scene, const RayBatch& rays, const uint8_t* mask, uint32_t count,
                   ar::HitRecord* out, ThreadPool& pool);

// occluded[i] is 1 when any instance with the SHADOW bit lies within [tmin, tmax] of ray i,
// else 0. This is the opaque rule; transparent shadows are a later task.
void trace_occluded(const EmbreeScene& scene, const RayBatch& rays, uint32_t count, uint8_t* occluded,
                    ThreadPool& pool);

}  // namespace astroray::host
