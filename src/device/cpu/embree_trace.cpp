// SPDX-License-Identifier: Apache-2.0
#include "device/cpu/embree_trace.h"

#include "device/cpu/embree_ray.h"

namespace astroray::host {

namespace {

// Ray i of the batch as an Embree ray (single precision).
RTCRay batch_ray(const RayBatch& r, uint32_t i, unsigned mask) {
  return make_ray(float(r.ox[i]), float(r.oy[i]), float(r.oz[i]), float(r.dx[i]), float(r.dy[i]), float(r.dz[i]),
                  float(r.tmin[i]), float(r.tmax[i]), mask);
}

}  // namespace

void trace_closest(const EmbreeScene& scene, const RayBatch& rays, const uint8_t* mask, uint32_t count,
                   ar::HitRecord* out, ThreadPool& pool) {
  RTCScene s = static_cast<RTCScene>(scene.handle());
  pool.parallel_for(count, [&](uint32_t first, uint32_t last) {
    RTCIntersectArguments args;
    rtcInitIntersectArguments(&args);
    for (uint32_t i = first; i < last; ++i) {
      RTCRayHit rh;
      rh.ray = batch_ray(rays, i, mask[i]);
      rh.hit.geomID = RTC_INVALID_GEOMETRY_ID;
      rh.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
      rtcIntersect1(s, &rh, &args);
      ar::HitRecord& h = out[i];
      if (rh.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
        h = {rays.tmax[i], ~0u, ar::kNoInstance, 0, 0};
      } else {
        h.t = ar::Real(rh.ray.tfar), h.prim = rh.hit.primID, h.inst = rh.hit.instID[0];
        h.u = ar::Real(rh.hit.u), h.v = ar::Real(rh.hit.v);
      }
    }
  });
}

void trace_occluded(const EmbreeScene& scene, const RayBatch& rays, uint32_t count, uint8_t* occluded,
                    ThreadPool& pool) {
  RTCScene s = static_cast<RTCScene>(scene.handle());
  pool.parallel_for(count, [&](uint32_t first, uint32_t last) {
    RTCOccludedArguments args;
    rtcInitOccludedArguments(&args);
    for (uint32_t i = first; i < last; ++i) {
      RTCRay ray = batch_ray(rays, i, ar::kShadowMaskBit);
      rtcOccluded1(s, &ray, &args);
      // Embree signals an occluded ray by setting tfar to -inf.
      occluded[i] = ray.tfar < 0.0f ? 1 : 0;
    }
  });
}

}  // namespace astroray::host
