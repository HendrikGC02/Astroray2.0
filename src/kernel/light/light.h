// SPDX-License-Identifier: Apache-2.0
// The one light interface: sample_li, pdf_li, sample_le, power and bounds, dispatched by a
// closed enum in a switch. "Absent" lives in the type: an infinite light says `infinite`, never a
// distance of 1e30; no sample is pdf == 0 with Le == 0, never a NaN.
// Include this file, not point.h or sphere.h: they need the types defined here.
//
// Ref: docs/decisions/0004-integrator.md D7 (interface), D1 (delta lights: NEE weight 1, BSDF
// pdf 0), D6 (energy conventions), D9 (theta_e: pi/2 one-sided); pbrt-v4 src/pbrt/lights.h
// (LightSampleContext, LightLiSample, LightLeSample, LightBounds), util/sampling.h
// (SampleUniformSphere, SampleUniformDiskConcentric, SampleCosineHemisphere); Shirley and
// Chiu 1997, "A Low Distortion Map Between Disk and Square" (Apache-2.0 sources).
#pragma once
#include <cstdint>

#include "../color/observer.h"
#include "../core/frame.h"
#include "../core/real.h"
#include "../core/spectrum.h"
#include "../core/vec.h"
#include "../portability.h"
#include "light_record.h"

#define AR_LIGHT_TYPES_DEFINED 1

AR_NS_BEGIN

// Closed list; a new type adds an enumerator and one row to each switch below.
enum class LightType : uint8_t {
  POINT = 0,  // radius 0: delta intensity
  SPHERE      // radius > 0: area-consistent radiance, visible-cone sampling
};

enum class LightContextKind : uint8_t { POINT = 0, SEGMENT };

// Where light is gathered. A POINT context is a surface vertex. A SEGMENT context is a stretch
// of a medium ray (Conty Estevez and Kulla 2018 section 5.3): the light sampler (067) reads o,
// d, t_max for its importance, while sample_li and pdf_li always work from `p`, the point on the
// segment the distance sampling picked. No light-linking set (R45).
struct LightSampleContext {
  Real3 p, n_geom, n_shade;
  LightContextKind kind;
  Real3 o, d;
  Real t_max;
};

AR_FN LightSampleContext light_context_point(Real3 p, Real3 n_geom, Real3 n_shade) {
  LightSampleContext c;
  c.p = p;
  c.n_geom = n_geom;
  c.n_shade = n_shade;
  c.kind = LightContextKind::POINT;
  c.o = p;
  c.d = Real3{Real(0), Real(0), Real(0)};
  c.t_max = Real(0);
  return c;
}

// What a BSDF ray found on a lamp (D1, D8); area types need it for the solid-angle conversion,
// point and sphere lamps ignore it.
struct LightHit {
  Real3 p, n;
};

struct LightLiSample {
  Real3 wi;       // unit, from the context point toward the light
  Real dist;      // to p_light; meaningless when `infinite`
  bool infinite;  // distant light: no distance
  Real pdf;       // solid angle, 1 for delta lights; 0 means no sample
  bool delta;     // NEE weight 1, BSDF-strategy pdf 0 (D1)
  Real3 p_light;
  Spec Le;        // radiance toward the context; for delta lights I / dist^2 (pbrt PointLight::SampleLi)
};

struct LightRay {
  Real3 o, d;
};

struct LightLeSample {
  LightRay ray;
  Spec Le;  // emitted radiance along the ray; intensity for delta lights (pbrt PointLight::SampleLe)
  Real pdf_pos;
  Real pdf_dir;
};

// The scene's bounding sphere, which light paths from distant lights aim at (067).
struct SceneSphere {
  Real3 center;
  Real radius;
};

struct Bounds3 {
  Real3 lo, hi;
};

// Light-tree bounds (Conty Estevez and Kulla 2018): theta_o is the half-angle of the normal
// cone, theta_e the emission spread around it.
struct LightBounds {
  Bounds3 bbox;
  Real theta_o, theta_e;
  bool two_sided;
};

inline constexpr Real kLightPi = Real(3.14159265358979323846);

AR_FN LightLiSample light_li_none() {
  LightLiSample s;
  s.wi = Real3{Real(0), Real(0), Real(1)};
  s.dist = Real(0);
  s.infinite = false;
  s.pdf = Real(0);
  s.delta = false;
  s.p_light = Real3{Real(0), Real(0), Real(0)};
  s.Le = spec_fill(Real(0));
  return s;
}

AR_FN LightLeSample light_le_none() {
  LightLeSample s;
  s.ray.o = Real3{Real(0), Real(0), Real(0)};
  s.ray.d = Real3{Real(0), Real(0), Real(1)};
  s.Le = spec_fill(Real(0));
  s.pdf_pos = Real(0);
  s.pdf_dir = Real(0);
  return s;
}

AR_FN Real light_safe_sqrt(Real x) { return ar::sqrt(ar::max(Real(0), x)); }

// pbrt-v4 SampleUniformSphere: z = 1 - 2 u0, phi = 2 pi u1; pdf 1 / (4 pi).
AR_FN Real3 sample_uniform_sphere(Real2 u) {
  const Real z = Real(1) - Real(2) * u.x;
  const Real r = light_safe_sqrt(Real(1) - z * z);
  const Real phi = Real(2) * kLightPi * u.y;
  return Real3{r * ar::cos(phi), r * ar::sin(phi), z};
}

// pbrt-v4 SampleUniformDiskConcentric (Shirley and Chiu 1997).
AR_FN Real2 sample_disk_concentric(Real2 u) {
  const Real ox = Real(2) * u.x - Real(1), oy = Real(2) * u.y - Real(1);
  if (ox == Real(0) && oy == Real(0)) return Real2{Real(0), Real(0)};
  Real r, theta;
  if (ar::abs(ox) > ar::abs(oy)) {
    r = ox;
    theta = (kLightPi / Real(4)) * (oy / ox);
  } else {
    r = oy;
    theta = kLightPi / Real(2) - (kLightPi / Real(4)) * (ox / oy);
  }
  return Real2{r * ar::cos(theta), r * ar::sin(theta)};
}

// pbrt-v4 SampleCosineHemisphere about +z; pdf = z / pi (zero at the horizon).
AR_FN Real3 sample_cosine_hemisphere(Real2 u) {
  const Real2 d = sample_disk_concentric(u);
  return Real3{d.x, d.y, light_safe_sqrt(Real(1) - d.x * d.x - d.y * d.y)};
}

AR_NS_END

#include "point.h"
#include "sphere.h"

AR_NS_BEGIN

// Host side. A light record is built from these or checked by light_valid before upload; false
// means the host raises a visible error (a bad lamp is never rendered as something else).
AR_FN LightRecord light_make(LightType type, Real3 pos, Real radius, Real strength, const SpectrumRef& emission) {
  LightRecord r;
  r.type = uint8_t(type);
  r.light_group = 0;
  r.flags = 0;
  r.visibility = 0xFFFFFFFFu;  // every ray class until the host narrows it from the descriptor
  for (int i = 0; i < 12; ++i) r.xf[i] = (i % 5 == 0) ? 1.0f : 0.0f;
  r.xf[3] = float(pos.x);
  r.xf[7] = float(pos.y);
  r.xf[11] = float(pos.z);
  r.size[0] = float(radius);
  r.size[1] = 0.0f;
  r.strength = float(strength);
  r.emission = emission;
  return r;
}

AR_FN bool light_finite(float x) { return x - x == 0.0f; }

AR_FN bool light_valid(const LightRecord& r) {
  for (int i = 0; i < 12; ++i)
    if (!light_finite(r.xf[i])) return false;
  if (!light_finite(r.size[0]) || !light_finite(r.strength) || !(r.strength >= 0.0f)) return false;
  // Only the kinds resolve() evaluates; the illuminant semantic is what makes colour a light.
  if (r.emission.semantic != uint8_t(SpectrumSemantic::ILLUMINANT)) return false;
  if (r.emission.kind != uint8_t(SpectrumKind::CONSTANT) && r.emission.kind != uint8_t(SpectrumKind::RGB)) return false;
  switch (LightType(r.type)) {
    case LightType::POINT: return r.size[0] == 0.0f;
    case LightType::SPHERE: return r.size[0] > 0.0f;
  }
  return false;
}

// Deterministic in (ctx, u). The unknown-type default is a bug that a debug build traps.
AR_FN LightLiSample light_sample_li(const LightRecord& r, const LightSampleContext& ctx, Real2 u, const Lambda& lam,
                                    const ObserverView& obs) {
  switch (LightType(r.type)) {
    case LightType::POINT: return point_sample_li(r, ctx, lam, obs);
    case LightType::SPHERE: return sphere_sample_li(r, ctx, u, lam, obs);
  }
  AR_ASSERT(false);
  return light_li_none();
}

// Solid-angle pdf of reaching the light along wi (a BSDF ray's direction at the previous vertex,
// 0004 D1); 0 for delta lights.
AR_FN Real light_pdf_li(const LightRecord& r, const LightSampleContext& ctx, Real3 wi, const LightHit& hit) {
  switch (LightType(r.type)) {
    case LightType::POINT: return Real(0);
    case LightType::SPHERE: return sphere_pdf_li(r, ctx, wi, hit);
  }
  AR_ASSERT(false);
  return Real(0);
}

// For light paths. u_pos picks the origin and u_dir the direction; delta position lights ignore
// u_pos. `target` is for distant lights (067).
AR_FN LightLeSample light_sample_le(const LightRecord& r, Real2 u_pos, Real2 u_dir, const Lambda& lam,
                                    const ObserverView& obs, const SceneSphere& /*target*/) {
  switch (LightType(r.type)) {
    case LightType::POINT: return point_sample_le(r, u_dir, lam, obs);
    case LightType::SPHERE: return sphere_sample_le(r, u_pos, u_dir, lam, obs);
  }
  AR_ASSERT(false);
  return light_le_none();
}

// The unweighted scalar power, Blender's P times the radiance scale. 067 weights the spectral
// shape by the importance and radiometric weights of D10.
AR_FN Real light_power(const LightRecord& r) {
  switch (LightType(r.type)) {
    case LightType::POINT: return point_power(r);
    case LightType::SPHERE: return sphere_power(r);
  }
  AR_ASSERT(false);
  return Real(0);
}

AR_FN LightBounds light_bounds(const LightRecord& r) {
  switch (LightType(r.type)) {
    case LightType::POINT: return point_bounds(r);
    case LightType::SPHERE: return sphere_bounds(r);
  }
  AR_ASSERT(false);
  LightBounds b;
  b.bbox.lo = b.bbox.hi = Real3{Real(0), Real(0), Real(0)};
  b.theta_o = b.theta_e = Real(0);
  b.two_sided = false;
  return b;
}

AR_NS_END
