// SPDX-License-Identifier: Apache-2.0
// The Embree ray both traversal files build, so a batch trace and a stage trace cannot drift
// apart. Includes Embree, so only engine sources (src/device/cpu) include it; embree_trace.h
// stays free of Embree types for the tests that include it.
// Ref: Embree 4 API documentation (RenderKit/embree, Apache-2.0), RTCRay.
#pragma once
#include <embree4/rtcore.h>

namespace astroray::host {

inline RTCRay make_ray(float ox, float oy, float oz, float dx, float dy, float dz, float tmin, float tmax,
                       unsigned mask) {
  RTCRay ray;
  ray.org_x = ox, ray.org_y = oy, ray.org_z = oz;
  ray.dir_x = dx, ray.dir_y = dy, ray.dir_z = dz;
  ray.tnear = tmin;
  ray.tfar = tmax;
  ray.mask = mask;
  ray.flags = 0;
  ray.time = 0.0f;
  return ray;
}

}  // namespace astroray::host
