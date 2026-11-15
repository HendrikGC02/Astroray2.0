// SPDX-License-Identifier: Apache-2.0
// Lamp primitives: which unit shape a light record becomes, the transform that places it exactly
// where the light sampler samples, and the one ray-disk test both devices run (Embree user
// geometry, OptiX `__intersection__lamp`). The test is purely geometric: whether the lamp emits
// toward the ray is the shading stage's decision (0004 D8), so a ray reaching the back face hits.
//
// Shapes live in object space and the instance transform places them:
//   rectangle  unit square [-1/2, 1/2]^2 in z = 0, two triangles (both devices)
//   disk       unit disk in z = 0, a custom primitive
//   sphere     unit sphere at the origin, a built-in sphere (OptiX) / sphere point (Embree)
// A POINT lamp of radius 0 is a delta light: it has no surface, nothing can hit it, so it gets no
// primitive at all (an ellipse lamp is already the equal-area disk, 068).
// The placement is built from the light record, not from the object's transform: rectangle axes
// scaled by its width and height, disk axes by its radius, sphere by its radius whatever the
// object scale was (068), so the primitive is the surface light.h samples.
//
// Ref: pbrt-v4 src/pbrt/shapes.h Disk::BasicIntersect (plane test, then the radius test on the
// hit point; Apache-2.0); OptiX 9.1 Programming Guide, "Custom primitives" and "Built-in
// primitives" (spheres); Embree 4 API, RTC_GEOMETRY_TYPE_USER and RTC_GEOMETRY_TYPE_SPHERE_POINT
// (RenderKit/embree, Apache-2.0); docs/decisions/0002 D11, 0004 D8.
#pragma once
#include <cstdint>

#include "../core/vec.h"
#include "../light/light.h"
#include "../portability.h"

AR_NS_BEGIN

static_assert(sizeof(Real) == 4, "lamp_intersect.h feeds Embree and OptiX, which traverse in float");

enum class LampShape : uint8_t { NONE = 0, RECT, DISK, SPHERE, INVALID };

// Half-thickness of a disk's bounding box in object space: 2 gamma(3) with gamma(n) = n u / (1 - n u),
// u = 2^-24, i.e. about 6 float ulps at unit scale. That is the factor pbrt-v4's BVH widens its ray-box
// slab test by (2 gamma(3), Bounds3::IntersectP), so a flat box this thin survives the slab test's
// rounding; the intersection program computes the exact plane hit either way.
// Ref: pbrt-v4 src/pbrt/util/math.h gamma(n), src/pbrt/cpu/aggregates.cpp BVHAggregate::Intersect.
inline constexpr float kLampDiskBoundPad = 2.0f * (3.0f / 16777216.0f) / (1.0f - 3.0f / 16777216.0f);

AR_FN LampShape lamp_shape(const LightRecord& r) {
  switch (LightType(r.type)) {
    // A point lamp is a delta light (radius 0); a radius belongs to a SPHERE record. light_problem
    // refuses the mixed record and sampling would treat it as a delta, so it is not a lamp here.
    case LightType::POINT: return r.size[0] == 0.0f ? LampShape::NONE : LampShape::INVALID;
    case LightType::SPHERE: return LampShape::SPHERE;
    case LightType::AREA_RECT: return LampShape::RECT;
    case LightType::AREA_DISK: return LampShape::DISK;
    default: return LampShape::INVALID;  // emissive triangles and the world are not lamps
  }
}

// Row-major 3x4 object to world of the unit shape of `r`; m is written whole. Columns 0 and 1 are
// the lamp's local axes scaled by the shape's extent, column 2 the third axis (the unit shapes
// have no thickness); a sphere is isotropic, so only its radius and position matter.
AR_FN void lamp_geometry_xform(const LightRecord& r, LampShape shape, float m[12]) {
  const float s0 = r.size[0];
  const float s1 = shape == LampShape::RECT ? r.size[1] : r.size[0];
  for (int i = 0; i < 3; ++i) {
    if (shape == LampShape::SPHERE) {
      m[4 * i + 0] = i == 0 ? s0 : 0.0f;
      m[4 * i + 1] = i == 1 ? s0 : 0.0f;
      m[4 * i + 2] = i == 2 ? s0 : 0.0f;
    } else {
      m[4 * i + 0] = r.xf[4 * i + 0] * s0;
      m[4 * i + 1] = r.xf[4 * i + 1] * s1;
      m[4 * i + 2] = r.xf[4 * i + 2];
    }
    m[4 * i + 3] = r.xf[4 * i + 3];
  }
}

// The unit disk of z = 0 against the ray o + t d, t in [tmin, tmax] (object space; d need not be
// unit, t is the same in every space an affine map relates). A ray parallel to the plane misses.
AR_FN bool lamp_disk_intersect(Real3 o, Real3 d, Real tmin, Real tmax, Real* t_out) {
  if (d.z == Real(0)) return false;
  const Real t = -o.z / d.z;
  if (!(t >= tmin && t <= tmax)) return false;  // also rejects NaN
  const Real x = o.x + t * d.x, y = o.y + t * d.y;
  if (x * x + y * y > Real(1)) return false;
  *t_out = t;
  return true;
}

AR_NS_END
