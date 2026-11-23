// SPDX-License-Identifier: Apache-2.0
// Point lamp, radius 0: a delta intensity I = P / (4 pi) s(lambda), seen by NEE only (pdf 1,
// weight 1) and never by BSDF rays (0004 D1, D6). Include light.h, not this file.
// Ref: docs/decisions/0004-integrator.md D6 (point row), D1; pbrt-v4 src/pbrt/lights.cpp
// PointLight::SampleLi (Li = I / d^2, pdf 1), SampleLe (uniform sphere, pdf_pos 1,
// pdf_dir 1 / (4 pi)), Bounds (cone of all directions, theta_e = pi/2).
#pragma once
#ifndef AR_LIGHT_TYPES_DEFINED
#error "include light/light.h, not point.h"
#endif

AR_NS_BEGIN

AR_FN Spec point_intensity(const LightRecord& r, const Lambda& lam, const ObserverView& obs) {
  return light_emission(r, lam, obs) / (Real(4) * kLightPi);
}

// The context point on the lamp has no direction: no sample rather than an infinite Le.
AR_FN LightLiSample point_sample_li(const LightRecord& r, const LightSampleContext& ctx, const Lambda& lam,
                                    const ObserverView& obs) {
  const Real3 c = light_position(r);
  const Real3 d = c - ctx.p;
  const Real d2 = dot(d, d);
  LightLiSample s = light_li_none();
  if (!(d2 > Real(0))) return s;
  s.dist = ar::sqrt(d2);
  s.wi = d / s.dist;
  s.pdf = Real(1);
  s.delta = true;
  s.p_light = c;
  s.Le = point_intensity(r, lam, obs) / d2;
  return s;
}

AR_FN LightLeSample point_sample_le(const LightRecord& r, Real2 u_dir, const Lambda& lam, const ObserverView& obs) {
  LightLeSample s;
  s.ray.o = light_position(r);
  s.ray.d = sample_uniform_sphere(u_dir);
  s.Le = point_intensity(r, lam, obs);
  s.pdf_pos = Real(1);  // a delta position: the pdf of a point mass is taken as 1, as pbrt does
  s.pdf_dir = Real(1) / (Real(4) * kLightPi);
  return s;
}

AR_FN Real point_power(const LightRecord& r) { return Real(r.strength); }

AR_FN LightBounds point_bounds(const LightRecord& r) {
  LightBounds b;
  b.bbox.lo = b.bbox.hi = light_position(r);
  b.w = Real3{Real(0), Real(0), Real(1)};  // isotropic: any unit axis
  b.theta_o = kLightPi;
  b.theta_e = kLightPi / Real(2);
  b.two_sided = false;
  return b;
}

AR_NS_END
