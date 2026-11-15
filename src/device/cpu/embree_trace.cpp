// SPDX-License-Identifier: Apache-2.0
#include "device/cpu/embree_trace.h"

#include <embree4/rtcore.h>

namespace astroray::host {

namespace {

// Ray i of the batch as an Embree ray (single precision).
RTCRay make_ray(const RayBatch& r, uint32_t i, unsigned mask) {
  RTCRay ray;
  ray.org_x = float(r.ox[i]), ray.org_y = float(r.oy[i]), ray.org_z = float(r.oz[i]);
  ray.dir_x = float(r.dx[i]), ray.dir_y = float(r.dy[i]), ray.dir_z = float(r.dz[i]);
  ray.tnear = float(r.tmin[i]);
  ray.tfar = float(r.tmax[i]);
  ray.mask = mask;
  ray.flags = 0;
  ray.time = 0.0f;
  return ray;
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
      rh.ray = make_ray(rays, i, mask[i]);
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
      RTCRay ray = make_ray(rays, i, kShadowRayMask);
      rtcOccluded1(s, &ray, &args);
      // Embree signals an occluded ray by setting tfar to -inf.
      occluded[i] = ray.tfar < 0.0f ? 1 : 0;
    }
  });
}

}  // namespace astroray::host
