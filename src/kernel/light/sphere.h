// SPDX-License-Identifier: Apache-2.0
// Sphere lamp, radius r > 0: constant outward radiance L = P / (4 pi^2 r^2) s(lambda), which
// radiates exactly P (flux = L * pi * 4 pi r^2), sampled by the cone of directions that sees it
// (0004 D6). Include light.h, not this file.
//
// Ref: docs/decisions/0004-integrator.md D6 (point/sphere row), D8 (one-sided, front-facing Le);
// pbrt-v4 src/pbrt/shapes.h Sphere::Sample(ctx, u) and Sphere::PDF(ctx, wi), the cone branch
// (Taylor expansion below sin^2 theta_max = 0.00068523, sin^2 of 1.5 degrees), Sphere::Sample(u)
// (uniform area, pdf 1 / (4 pi r^2)); src/pbrt/lights.cpp DiffuseAreaLight::SampleLe
// (area point by u1, cosine hemisphere about the normal by u2).
//
// A context point inside the sphere gets no sample: the lamp emits outward only (D8), so from
// inside it contributes nothing, which is what pbrt's area-sampled interior branch converges to
// for a one-sided emitter.
#pragma once
#ifndef AR_LIGHT_TYPES_DEFINED
#error "include light/light.h, not sphere.h"
#endif

AR_NS_BEGIN

inline constexpr Real kSphereSmallCone = Real(0.00068523);

AR_FN Real sphere_radius(const LightRecord& r) { return Real(r.size[0]); }

AR_FN Spec sphere_radiance(const LightRecord& r, const Lambda& lam, const ObserverView& obs) {
  const Real rad = sphere_radius(r);
  return light_emission(r, lam, obs) / (Real(4) * kLightPi * kLightPi * rad * rad);
}

// sin^2 of the cone's half angle for a context point at squared distance d2 from the centre;
// a value outside (0, 1) (inside the sphere, d2 = 0, radius 0 or an underflow) means no sample.
AR_FN Real sphere_sin2_max(Real radius, Real d2) { return radius * radius / d2; }

// 1 - cos(theta_max), by the Taylor form for narrow cones where the subtraction cancels.
AR_FN Real sphere_one_minus_cos_max(Real sin2_max) {
  return sin2_max < kSphereSmallCone ? sin2_max / Real(2) : Real(1) - light_safe_sqrt(Real(1) - sin2_max);
}

// Uniform pdf over the cone, 0 where there is no cone (including a pdf too large for Real).
AR_FN Real sphere_cone_pdf(Real sin2_max) {
  if (!(sin2_max > Real(0) && sin2_max < Real(1))) return Real(0);
  const Real pdf = Real(1) / (Real(2) * kLightPi * sphere_one_minus_cos_max(sin2_max));
  return pdf - pdf == Real(0) ? pdf : Real(0);
}

AR_FN LightLiSample sphere_sample_li(const LightRecord& r, const LightSampleContext& ctx, Real2 u, const Lambda& lam,
                                     const ObserverView& obs) {
  LightLiSample s = light_li_none();
  const Real radius = sphere_radius(r);
  const Real3 c = light_position(r);
  const Real3 to_c = c - ctx.p;
  const Real d2 = dot(to_c, to_c);
  const Real sin2_max = sphere_sin2_max(radius, d2);
  const Real pdf = sphere_cone_pdf(sin2_max);
  if (!(pdf > Real(0))) return s;

  const Real sin_max = ar::sqrt(sin2_max);
  const Real cos_max = light_safe_sqrt(Real(1) - sin2_max);
  Real cos_theta = (cos_max - Real(1)) * u.x + Real(1);
  Real sin2_theta = Real(1) - cos_theta * cos_theta;
  if (sin2_max < kSphereSmallCone) {
    sin2_theta = sin2_max * u.x;
    cos_theta = ar::sqrt(Real(1) - sin2_theta);
  }
  // Angle at the sphere's centre between the axis to the context point and the sampled point.
  const Real cos_alpha = sin2_theta / sin_max + cos_theta * light_safe_sqrt(Real(1) - sin2_theta / sin2_max);
  const Real sin_alpha = light_safe_sqrt(Real(1) - cos_alpha * cos_alpha);
  // phi = 2 pi u.y, through cospi / sinpi (portability.h: no double slow path on the GPU).
  const Real3 w = Real3{sin_alpha * ar::cospi(Real(2) * u.y), sin_alpha * ar::sinpi(Real(2) * u.y), cos_alpha};
  const Frame f = frame_from_normal(to_c / ar::sqrt(d2));
  const Real3 n = to_world(f, -w);

  s.p_light = c + n * radius;
  const Real3 d = s.p_light - ctx.p;
  s.dist = length(d);
  s.wi = d / s.dist;
  s.pdf = pdf;
  s.Le = sphere_radiance(r, lam, obs);
  if (!is_finite(s.Le)) return light_li_none();  // a radius so small that L overflows
  return s;
}

// wi is the direction of a BSDF ray that hit this lamp (D1, D8), so it lies in the cone by
// construction and the pdf depends on the distance only.
AR_FN Real sphere_pdf_li(const LightRecord& r, const LightSampleContext& ctx, Real3 /*wi*/, const LightHit& /*hit*/) {
  const Real3 to_c = light_position(r) - ctx.p;
  const Real sin2_max = sphere_sin2_max(sphere_radius(r), dot(to_c, to_c));
  return sphere_cone_pdf(sin2_max);
}

AR_FN LightLeSample sphere_sample_le(const LightRecord& r, Real2 u_pos, Real2 u_dir, const Lambda& lam,
                                     const ObserverView& obs) {
  const Real radius = sphere_radius(r);
  if (!(radius > Real(0))) return light_le_none();
  const Real3 n = sample_uniform_sphere(u_pos);
  const Real3 local = sample_cosine_hemisphere(u_dir);
  LightLeSample s;
  s.ray.o = light_position(r) + n * radius;
  s.ray.d = to_world(frame_from_normal(n), local);
  s.Le = sphere_radiance(r, lam, obs);
  s.pdf_pos = Real(1) / (Real(4) * kLightPi * radius * radius);
  s.pdf_dir = local.z / kLightPi;
  // pbrt-v4 DiffuseAreaLight::SampleLe: a direction on the horizon (cosine pdf 0) is no sample.
  if (s.pdf_dir == Real(0)) return light_le_none();
  // Backstop only: light_problem refuses radii whose L or 1 / (4 pi r^2) overflows.
  if (!is_finite(s.Le) || !(s.pdf_pos - s.pdf_pos == Real(0))) return light_le_none();
  return s;
}

AR_FN Real sphere_power(const LightRecord& r) { return Real(r.strength); }

AR_FN LightBounds sphere_bounds(const LightRecord& r) {
  const Real3 c = light_position(r);
  const Real3 e = Real3{sphere_radius(r), sphere_radius(r), sphere_radius(r)};
  LightBounds b;
  b.bbox.lo = c - e;
  b.bbox.hi = c + e;
  b.theta_o = kLightPi;  // the normals cover the whole sphere
  b.theta_e = kLightPi / Real(2);
  b.two_sided = false;
  return b;
}

AR_NS_END
