// SPDX-License-Identifier: Apache-2.0
// Ray-origin offset: how a ray spawned on a triangle (or past a lamp) avoids hitting the
// surface it leaves. One function serves continuation rays, the origin end of shadow rays
// and lamp pass-through re-queues; every ray traces with tmin = 0 (0002 D16).
//
// Method: the origin is moved along the geometric normal by a fixed number of float ulps
// per component, to the side of the outgoing direction. An ulp offset scales with the
// magnitude of the coordinate, so it clears the rounding error of a hit point at 1e4 m and
// stays tiny at 1 m; components near zero get a small fixed offset instead, because an
// integer step there would be below the error of the intersection.
// Ref: Waechter and Binder 2019, "A Fast and Robust Method for Avoiding Self-Intersection",
// Ray Tracing Gems ch. 6, section 6.2 (constants: origin 1/32, float_scale 1/65536,
// int_scale 256, as given there). Implemented from the chapter's description.
// Considered and not chosen: pbrt-v4 OffsetRayOrigin (src/pbrt/interaction.h, ray.h,
// Apache-2.0), which offsets by a per-hit floating-point error bound. It needs an error
// bound carried from every hit (the barycentric interpolation of each triangle) through
// the hit record, and the OptiX and Embree hits would each have to produce one.
// Ref: docs/decisions/0002-kernel-architecture.md D16, task 038.
#pragma once
#include <cstdint>

#include "../core/vec.h"
#include "../portability.h"

AR_NS_BEGIN

// Embree and OptiX traverse in single precision whatever Real is (0002 C3), and the
// constants below are the paper's float constants; a double build must not use them.
static_assert(sizeof(Real) == 4, "ray_offset.h is float-only: the constants are for IEEE binary32");

inline constexpr Real kRayTmin = Real(0);  // every ray: the offset, not tmin, avoids self-hits

namespace ray_offset_detail {
// Ref: Waechter and Binder 2019, section 6.2.
inline constexpr Real kOrigin = Real(1) / Real(32);
inline constexpr Real kFloatScale = Real(1) / Real(65536);
inline constexpr Real kIntScale = Real(256);

AR_FN Real offset_component(Real p, Real n) {
  if (ar::abs(p) < kOrigin) return p + kFloatScale * n;
  const int32_t step = static_cast<int32_t>(kIntScale * n);
  const int32_t bits = bit_cast<int32_t>(p);
  // Float bits are sign-magnitude: stepping the integer grows |p|, so a negative p
  // needs the opposite sign to move toward +n.
  return bit_cast<Real>(static_cast<int32_t>(bits + (p < Real(0) ? -step : step)));
}
}  // namespace ray_offset_detail

// `ng` is the unit geometric normal (either orientation); `side` is the outgoing direction
// that decides which side of the surface the origin is pushed to.
AR_FN Real3 offset_ray_origin(Real3 p, Real3 ng, Real3 side) {
  const Real3 n = dot(ng, side) < Real(0) ? -ng : ng;
  return Real3{ray_offset_detail::offset_component(p.x, n.x), ray_offset_detail::offset_component(p.y, n.y),
               ray_offset_detail::offset_component(p.z, n.z)};
}

// Lamp pass-through re-queue (0004 D8): the origin just past the lamp surface at `p`, along
// the ray direction `dir`, which serves as both normal and side.
AR_FN Real3 offset_past_surface(Real3 p, Real3 dir) { return offset_ray_origin(p, dir, dir); }

// A shadow ray from an offset origin to an offset light point; `dir` is a unit vector and
// the ray is traced over [kRayTmin, tmax].
struct ShadowSegment {
  Real3 origin;
  Real3 dir;
  Real tmax;
};

AR_FN ShadowSegment shadow_segment_between(Real3 o, Real3 e, Real3 fallback_dir) {
  const Real3 d = e - o;
  const Real len = length(d);
  // Coincident offset points: an empty segment, never a NaN direction.
  if (!(len > Real(0))) return ShadowSegment{o, fallback_dir, Real(0)};
  return ShadowSegment{o, d / len, len};
}

// Shading point `p` (normal `ng`) to a point `q` on an emitting triangle (normal `nq`): both
// ends are offset toward each other and tmax is the distance between the offset points.
// Ref: task 038 Do 2.
AR_FN ShadowSegment shadow_segment(Real3 p, Real3 ng, Real3 q, Real3 nq) {
  return shadow_segment_between(offset_ray_origin(p, ng, q - p), offset_ray_origin(q, nq, p - q), ng);
}

// Shading point to a lamp point: lamp instances lack the SHADOW mask bit (0002 D11), so the
// far end needs no offset.
AR_FN ShadowSegment shadow_segment_to_lamp(Real3 p, Real3 ng, Real3 q) {
  return shadow_segment_between(offset_ray_origin(p, ng, q - p), q, ng);
}

AR_NS_END
