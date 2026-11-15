// SPDX-License-Identifier: Apache-2.0
// The CPU intersect stages: trace each item of the stage's segment on the scene's Embree
// structure and write the hit record and next stage. The chunk runs on a pool worker already,
// so rays are traced one by one here (embree_trace.h's batch calls would nest the pool).
// Ref: docs/decisions/0002-kernel-architecture.md D4 (CPU stages loop over the segment), D11
// (hit record, ray-type mask, opaque shadow rule, camera rays without any-hit).
#include <cstdint>
#include <limits>

#include "device/cpu/embree_ray.h"
#include "device/cpu/embree_scene.h"
#include "device/cpu/embree_trace.h"
#include "kernel/geom/ray_offset.h"
#include "kernel/stages/route_hit.h"

namespace {

using astroray::host::EmbreeScene;

RTCScene accel_of(const ar::KernelGlobals& kg) {
  const ar::SceneData& s = ar::scene_of(kg);
  AR_ASSERT(s.accel != nullptr);
  return static_cast<RTCScene>(static_cast<const EmbreeScene*>(s.accel)->handle());
}

// The origin offset, not tmin, avoids self-hits (0002 D16).
RTCRay stage_ray(ar::Real3 o, ar::Real3 d, ar::Real tmax, unsigned mask) {
  return astroray::host::make_ray(float(o.x), float(o.y), float(o.z), float(d.x), float(d.y), float(d.z),
                                  float(ar::kRayTmin), float(tmax), mask);
}

}  // namespace

// Camera and light segments: the ray type is the mask bit of the next ray (0003 d19 row 4).
void cpu_stage_intersect_closest(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first,
                                 uint32_t last) {
  const RTCScene scene = accel_of(kg);
  RTCIntersectArguments args;
  rtcInitIntersectArguments(&args);
  for (uint32_t item = first; item < last; ++item) {
    const uint32_t slot = kg.main_queue[step.start + item];
    // ray_tmax exists only with a curved region or subsurface; a flat scene's segment is unbounded.
    const ar::Real tmax = kg.path->ray_tmax ? kg.path->ray_tmax[slot] : std::numeric_limits<ar::Real>::infinity();
    RTCRayHit rh;
    rh.ray = stage_ray(ar::ps_ray_o(kg, slot), ar::ps_ray_d(kg, slot), tmax, ar::ps_ray_type(kg, slot));
    rh.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    rh.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
    rtcIntersect1(scene, &rh, &args);
    ar::HitRecord h;
    if (rh.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
      h = {tmax, ~0u, ar::kNoInstance, 0, 0};
    } else {
      h = {ar::Real(rh.ray.tfar), rh.hit.primID, rh.hit.instID[0], ar::Real(rh.hit.u), ar::Real(rh.hit.v)};
    }
    ar::ps_hit_t(kg, slot) = h.t;
    ar::ps_hit_prim(kg, slot) = h.prim;
    ar::ps_hit_inst(kg, slot) = h.inst;
    ar::ps_hit_uv(kg, slot) = ar::Real2{h.u, h.v};
    ar::ps_next_stage(kg, slot) = static_cast<uint8_t>(ar::route_hit(kg, h, ar::ps_flags(kg, slot)));
  }
}

// Occlusion up to tmax against the SHADOW bit only; opaque rule (transparent shadows come later).
void cpu_stage_intersect_shadow(const ar::KernelGlobals& kg, const ar::StepEntry& step, uint32_t first,
                                uint32_t last) {
  const RTCScene scene = accel_of(kg);
  RTCOccludedArguments args;
  rtcInitOccludedArguments(&args);
  for (uint32_t item = first; item < last; ++item) {
    const uint32_t slot = kg.shadow_queue[step.start + item];
    RTCRay ray = stage_ray(ar::sh_ray_o(kg, slot), ar::sh_ray_d(kg, slot), ar::sh_tmax(kg, slot),
                          ar::kShadowMaskBit);
    rtcOccluded1(scene, &ray, &args);
    // Embree signals an occluded ray by setting tfar to -inf.
    ar::sh_next_stage(kg, slot) =
        static_cast<uint8_t>(ray.tfar < 0.0f ? ar::Stage::SHADOW_TERMINATED : ar::Stage::SHADE_SHADOW);
  }
}
