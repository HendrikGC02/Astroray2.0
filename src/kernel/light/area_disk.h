// SPDX-License-Identifier: Apache-2.0
// Disk area lamp, radius r > 0: constant radiance L = P / (pi A) s(lambda), A = pi r^2, from the
// front face only, sampled uniformly by area and converted to solid angle (0004 D6, D8).
// Include light.h, not this file; it uses the frame helpers of area_rect.h.
//
// Frame as the rectangle: the disk lies in the lamp's local XY plane, size[0] is the radius
// (size[1] is 0), the front is local -Z. A Blender disk lamp has size = diameter; the host halves
// it (068).
//
// Ref: docs/decisions/0004-integrator.md D6 (area row), D8, D9; pbrt-v4 src/pbrt/shapes.h
// Disk::Sample(ctx, u) (concentric disk point, area pdf 1 / A converted by |cos| / d^2, no sample
// when the pdf is infinite) and Disk::PDF(ctx, wi); src/pbrt/lights.cpp DiffuseAreaLight::SampleLe
// and Bounds. The disk's world bounds are the exact per-axis extent r sqrt(1 - n_i^2).
#pragma once
#ifndef AR_LIGHT_TYPES_DEFINED
#error "include light/light.h, not area_disk.h"
#endif

AR_NS_BEGIN

AR_FN Real disk_radius(const LightRecord& r) { return Real(r.size[0]); }
AR_FN Real disk_area(const LightRecord& r) { return kLightPi * disk_radius(r) * disk_radius(r); }

AR_FN Spec disk_radiance(const LightRecord& r, const Lambda& lam, const ObserverView& obs) {
  return light_emission(r, lam, obs) / (kLightPi * disk_area(r));
}

AR_FN Real3 disk_point(const LightRecord& r, Real2 u) {
  const Real2 pd = sample_disk_concentric(u);
  const Real rad = disk_radius(r);
  return light_position(r) + area_axis(r, 0) * (pd.x * rad) + area_axis(r, 1) * (pd.y * rad);
}

// Solid-angle pdf at the lamp point reached along wi from the context: (1 / A) d^2 / cos, 0 where
// the lamp is seen edge-on or from behind or the pdf does not fit Real (pbrt: no sample).
AR_FN Real disk_solid_angle_pdf(const LightRecord& r, Real d2, Real cosl) {
  if (!(d2 > Real(0) && cosl > Real(0))) return Real(0);
  const Real pdf = d2 / (cosl * disk_area(r));
  return pdf - pdf == Real(0) ? pdf : Real(0);
}

AR_FN LightLiSample disk_sample_li(const LightRecord& r, const LightSampleContext& ctx, Real2 u, const Lambda& lam,
                                   const ObserverView& obs) {
  LightLiSample s = light_li_none();
  const Real3 p = disk_point(r, u);
  const Real3 d = p - ctx.p;
  const Real d2 = dot(d, d);
  if (!(d2 > Real(0))) return s;
  const Real dist = ar::sqrt(d2);
  const Real3 wi = d / dist;
  const Real pdf = disk_solid_angle_pdf(r, d2, dot(area_front(r), -wi));
  if (!(pdf > Real(0))) return s;
  s.p_light = p;
  s.dist = dist;
  s.wi = wi;
  s.pdf = pdf;
  s.Le = disk_radiance(r, lam, obs);
  if (!is_finite(s.Le)) return light_li_none();  // backstop: light_problem refuses overflowing radii
  return s;
}

AR_FN Real disk_pdf_li(const LightRecord& r, const LightSampleContext& ctx, Real3 wi, const LightHit& hit) {
  const Real3 d = hit.p - ctx.p;
  return disk_solid_angle_pdf(r, dot(d, d), dot(area_front(r), -wi));
}

AR_FN LightLeSample disk_sample_le(const LightRecord& r, Real2 u_pos, Real2 u_dir, const Lambda& lam,
                                   const ObserverView& obs) {
  const Real area = disk_area(r);
  if (!(area > Real(0))) return light_le_none();
  const Real3 local = sample_cosine_hemisphere(u_dir);
  LightLeSample s;
  s.ray.o = disk_point(r, u_pos);
  s.ray.d = to_world(frame_from_normal(area_front(r)), local);
  s.Le = disk_radiance(r, lam, obs);
  s.pdf_pos = Real(1) / area;
  s.pdf_dir = local.z / kLightPi;
  if (s.pdf_dir == Real(0)) return light_le_none();  // horizon: pbrt's `if (pdf == 0) return {}`
  if (!is_finite(s.Le) || !(s.pdf_pos - s.pdf_pos == Real(0))) return light_le_none();
  return s;
}

AR_FN Real disk_power(const LightRecord& r) { return Real(r.strength); }

AR_FN LightBounds disk_bounds(const LightRecord& r) {
  const Real3 c = light_position(r), n = area_front(r);
  const Real rad = disk_radius(r);
  const Real3 e{rad * light_safe_sqrt(Real(1) - n.x * n.x), rad * light_safe_sqrt(Real(1) - n.y * n.y),
                rad * light_safe_sqrt(Real(1) - n.z * n.z)};
  LightBounds b;
  b.bbox.lo = c - e;
  b.bbox.hi = c + e;
  b.theta_o = Real(0);
  b.theta_e = kLightPi / Real(2);
  b.two_sided = false;
  return b;
}

AR_NS_END
