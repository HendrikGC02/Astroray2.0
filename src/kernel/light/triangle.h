// SPDX-License-Identifier: Apache-2.0
// Emissive triangle: one light per triangle of an emissive material (0004 D9), constant
// radiance Le = strength * s(lambda) or, with kLightEmissionProgram, the material's emission entry
// at the sampled point, which the consumer evaluates (deferred_le). Sampled by the spherical triangle it subtends when that
// solid angle is inside pbrt's stable range, else uniformly by area. The front is the winding
// normal cross(p1 - p0, p2 - p0); a one-sided triangle emits only toward it. Include light.h.
//
// Ref: docs/decisions/0004-integrator.md D9 (sampling switch, sidedness, power = A pi estimate);
// Arvo 1995, "Stratified Sampling of Spherical Triangles" (parameterisation; steps after Peters 2021,
// see tri_sample_spherical); pbrt-v4 src/pbrt/shapes.h
// Triangle::Sample(ctx, u), Triangle::PDF(ctx, wi), Triangle::SolidAngle, MinSphericalSampleArea
// and MaxSphericalSampleArea; src/pbrt/util/sampling.cpp SampleSphericalTriangle; util/sampling.h
// SampleUniformTriangle; util/vecmath.h SphericalTriangleArea; src/pbrt/lights.cpp
// DiffuseAreaLight::SampleLe (two-sided side choice) and lights.h DiffuseAreaLight::L (one-sided
// test) (Apache-2.0). pbrt's optional cosine warp (SampleBilinear) is not applied, so the
// spherical pdf is exactly 1 / solid angle in both sample_li and pdf_li.
#pragma once
#ifndef AR_LIGHT_TYPES_DEFINED
#error "include light/light.h, not triangle.h"
#endif

AR_NS_BEGIN

// pbrt-v4 Triangle::MinSphericalSampleArea / MaxSphericalSampleArea (solid angle, sr).
inline constexpr Real kTriMinSolidAngle = Real(3e-4);
inline constexpr Real kTriMaxSolidAngle = Real(6.22);

AR_FN Real3 tri_cross(const LightRecord& r) {
  const Real3 p0 = tri_vertex(r, 0);
  return cross(tri_vertex(r, 1) - p0, tri_vertex(r, 2) - p0);
}
AR_FN Real tri_area(const LightRecord& r) { return Real(0.5) * length(tri_cross(r)); }
AR_FN bool tri_two_sided(const LightRecord& r) { return (r.flags & kLightTwoSided) != 0; }
// The emitting normal: the winding normal, or its opposite for a back-only emitter (BACK).
AR_FN Real3 tri_normal(const LightRecord& r) {
  const Real3 n = normalize(tri_cross(r));
  return (r.flags & kLightBackFace) ? -n : n;
}

AR_FN Real tri_clamp(Real x, Real lo, Real hi) { return x < lo ? lo : (x > hi ? hi : x); }

// pbrt-v4 SphericalTriangleArea of the directions from p (Triangle::SolidAngle).
AR_FN Real tri_solid_angle(const LightRecord& r, Real3 p) {
  const Real3 a = normalize(tri_vertex(r, 0) - p), b = normalize(tri_vertex(r, 1) - p), c = normalize(tri_vertex(r, 2) - p);
  return ar::abs(Real(2) * ar::atan2(dot(a, cross(b, c)), Real(1) + dot(a, b) + dot(a, c) + dot(b, c)));
}
AR_FN bool tri_spherical(Real S) { return S >= kTriMinSolidAngle && S <= kTriMaxSolidAngle; }

// The context may receive light: strictly in front, or on either side of a two-sided triangle.
AR_FN bool tri_sees(const LightRecord& r, Real3 p, Real3 n) {
  const Real side = dot(p - tri_vertex(r, 0), n);
  return tri_two_sided(r) ? side != Real(0) : side > Real(0);
}

// The spherical triangle (a, b, c) = directions from p to (v0, v1, v2) sampled uniformly by solid
// angle with Arvo's parameterisation, which pbrt-v4 SampleSphericalTriangle uses: u.x picks the
// sub-triangle (a, b, c') of solid angle u.x * Omega with c' on the arc from a to c, and u.y the
// point on the arc from b to c' with cos linear in u.y. The steps follow Peters' variant of Arvo's
// method, which is the same map in exact arithmetic but avoids the cos(beta') -> sin(beta')
// cancellation of pbrt's form (in float it moved directions by about 1e-4 rad when u.x is small):
//  - V = |det[a, b, c]| by a Householder reflection that sends b to +-e_x (so V is a 2x2
//    determinant), Q = 1 + a.b and S = a.c + b.c; Omega / 2 = atan2(V, Q + S) (Van Oosterom and
//    Strackee 1983);
//  - with theta = u.x Omega / 2, o = (V cos theta - S sin theta) a + (Q sin theta) c, and c' is a
//    reflected about o: c' = 2 (a.o) / (o.o) o - a, unit by construction;
//  - s2 = b.c', s = 1 - u.y (1 - s2) by fused multiply-adds, t = sqrt((1 - s^2) / (1 - s2^2)), or
//    t = u.y (its limit) when 1 - s2^2 <= 0; the direction is (s - t s2) b + t c', unit by
//    construction (no normalisation).
// Then the point of the triangle in that direction from clamped barycentrics, as pbrt does. False
// when the spherical triangle is degenerate.
// Ref: Peters 2021, "BRDF Importance Sampling for Polygonal Lights", ACM TOG 40(4) (solid angle
// sampling, "our variant of Arvo's method"), and its public code src/shaders/polygon_sampling.glsl
// (MomentsInGraphics/vulkan_renderer, BSD-3-Clause option) prepare_solid_angle_polygon_sampling and
// sample_solid_angle_polygon for one triangle (vertices[0], [1], [2] = a, b, c); Arvo 1995; Van
// Oosterom and Strackee 1983, "The Solid Angle of a Plane Triangle", IEEE TBME 30(2); pbrt-v4
// src/pbrt/util/sampling.cpp SampleSphericalTriangle (barycentrics and clamps; Apache-2.0).
AR_FN bool tri_sample_spherical(const LightRecord& r, Real3 p, Real2 u, Real3* out) {
  const Real3 v0 = tri_vertex(r, 0), v1 = tri_vertex(r, 1), v2 = tri_vertex(r, 2);
  const Real3 a = normalize(v0 - p), b = normalize(v1 - p), c = normalize(v2 - p);
  // det[a, b, c] after the Householder reflection H with H b = -sgn(b.x) e_x: the yz rows of H a
  // and H c, m_X = X_yz - (b.X - sh X_x) h, h = b_yz / (|b_x| + 1).
  const Real sh = b.x > Real(0) ? Real(-1) : Real(1);
  const Real inv = Real(1) / (ar::abs(b.x) + Real(1));
  const Real hy = b.y * inv, hz = b.z * inv;
  const Real fa = dot(b, a) - sh * a.x, fc = dot(b, c) - sh * c.x;
  const Real V = ar::abs((a.y - fa * hy) * (c.z - fc * hz) - (a.z - fa * hz) * (c.y - fc * hy));
  const Real Q = Real(1) + dot(a, b), S = dot(a, c) + dot(b, c);
  const Real half = ar::atan2(V, Q + S);  // Omega / 2, in [0, pi)
  if (!(half > Real(0))) return false;
  const LightSinCos th = light_sincos(u.x * half);  // area_rect.h: no sinf / cosf on the GPU
  const Real3 o = a * (V * th.c - S * th.s) + c * (Q * th.s);
  const Real oo = dot(o, o);
  if (!(oo > Real(0))) return false;
  const Real3 cp = o * (Real(2) * dot(a, o) / oo) - a;
  const Real s2 = dot(b, cp);
  const Real cs = ar::fma(u.y, s2, ar::fma(-u.y, Real(1), Real(1)));  // (1 - u.y) + u.y s2: cos from b
  const Real d = Real(1) - s2 * s2;
  const Real t = d > Real(0) ? ar::sqrt(ar::max(Real(0), Real(1) - cs * cs) / d) : u.y;
  const Real3 w = b * (cs - t * s2) + cp * t;  const Real3 e1 = v1 - v0, e2 = v2 - v0;
  const Real3 s1 = cross(w, e2);
  const Real divisor = dot(s1, e1);
  if (divisor == Real(0)) {  // pbrt: a triangle covering (nearly) a hemisphere, its centroid
    *out = (v0 + v1 + v2) / Real(3);
    return true;
  }
  const Real3 s = p - v0;
  Real b1 = tri_clamp(dot(s, s1) / divisor, Real(0), Real(1));
  Real b2 = tri_clamp(dot(w, cross(s, e1)) / divisor, Real(0), Real(1));
  if (b1 + b2 > Real(1)) {
    const Real sum = b1 + b2;
    b1 /= sum;
    b2 /= sum;
  }
  *out = v0 * (Real(1) - b1 - b2) + v1 * b1 + v2 * b2;
  return true;
}

// pbrt-v4 SampleUniformTriangle.
AR_FN Real3 tri_sample_area(const LightRecord& r, Real2 u) {
  Real b0, b1;
  if (u.x < u.y) {
    b0 = u.x / Real(2);
    b1 = u.y - b0;
  } else {
    b1 = u.y / Real(2);
    b0 = u.x - b1;
  }
  return tri_vertex(r, 0) * b0 + tri_vertex(r, 1) * b1 + tri_vertex(r, 2) * (Real(1) - b0 - b1);
}

// Area pdf converted to solid angle with |cos| (pbrt Triangle::Sample / PDF); 0 if not finite.
AR_FN Real tri_area_pdf(const LightRecord& r, Real3 n, Real3 d, Real3 wi) {
  const Real cosl = ar::abs(dot(n, wi));
  const Real pdf = dot(d, d) / (cosl * tri_area(r));
  return pdf - pdf == Real(0) ? pdf : Real(0);
}

AR_FN Spec tri_radiance(const LightRecord& r, const Lambda& lam, const ObserverView& obs) {
  if (r.flags & kLightEmissionProgram) return spec_fill(Real(0));  // deferred_le: the consumer evaluates it
  return light_emission(r, lam, obs);
}

AR_FN LightLiSample tri_sample_li(const LightRecord& r, const LightSampleContext& ctx, Real2 u, const Lambda& lam,
                                  const ObserverView& obs) {
  LightLiSample s = light_li_none();
  const Real3 n = tri_normal(r);
  if (!tri_sees(r, ctx.p, n)) return s;
  const Real S = tri_solid_angle(r, ctx.p);
  const bool spherical = tri_spherical(S);
  Real3 p;
  if (spherical) {
    if (!tri_sample_spherical(r, ctx.p, u, &p)) return s;
  } else {
    p = tri_sample_area(r, u);
  }
  const Real3 d = p - ctx.p;
  const Real dist = length(d);
  if (!(dist > Real(0))) return s;
  const Real pdf = spherical ? Real(1) / S : tri_area_pdf(r, n, d, d / dist);
  if (!(pdf > Real(0)) || !(pdf - pdf == Real(0))) return s;
  s.p_light = p;
  s.dist = dist;
  s.wi = d / dist;
  s.pdf = pdf;
  s.Le = tri_radiance(r, lam, obs);
  s.deferred_le = (r.flags & kLightEmissionProgram) != 0;
  if (!is_finite(s.Le)) return light_li_none();
  return s;
}

// wi is the direction of a BSDF ray that hit this triangle at hit.p (D1).
AR_FN Real tri_pdf_li(const LightRecord& r, const LightSampleContext& ctx, Real3 wi, const LightHit& hit) {
  const Real3 n = tri_normal(r);
  if (!tri_sees(r, ctx.p, n)) return Real(0);
  const Real S = tri_solid_angle(r, ctx.p);
  const Real pdf = tri_spherical(S) ? Real(1) / S : tri_area_pdf(r, n, hit.p - ctx.p, wi);
  return pdf - pdf == Real(0) ? pdf : Real(0);
}

AR_FN LightLeSample tri_sample_le(const LightRecord& r, Real2 u_pos, Real2 u_dir, const Lambda& lam,
                                  const ObserverView& obs) {
  const Real area = tri_area(r);
  if (!(area > Real(0))) return light_le_none();
  Real2 ud = u_dir;
  bool back = false;
  if (tri_two_sided(r)) {  // DiffuseAreaLight::SampleLe: choose a side, remap u
    back = ud.x >= Real(0.5);
    ud.x = ar::min(back ? (ud.x - Real(0.5)) * Real(2) : ud.x * Real(2), kLightOneMinusEpsilon);
  }
  Real3 local = sample_cosine_hemisphere(ud);
  if (back) local.z = -local.z;
  LightLeSample s;
  s.ray.o = tri_sample_area(r, u_pos);
  s.ray.d = to_world(frame_from_normal(tri_normal(r)), local);
  s.Le = tri_radiance(r, lam, obs);
  s.deferred_le = (r.flags & kLightEmissionProgram) != 0;
  s.pdf_pos = Real(1) / area;
  s.pdf_dir = ar::abs(local.z) / kLightPi / (tri_two_sided(r) ? Real(2) : Real(1));
  if (s.pdf_dir == Real(0)) return light_le_none();
  if (!is_finite(s.Le) || !(s.pdf_pos - s.pdf_pos == Real(0))) return light_le_none();
  return s;
}

// D9's power: A pi times the emission (the strength unweighted, the weighted energy for D10).
AR_FN Real tri_power(const LightRecord& r, Real energy) { return tri_area(r) * kLightPi * energy; }

AR_FN LightBounds tri_bounds(const LightRecord& r) {
  const Real3 a = tri_vertex(r, 0), b = tri_vertex(r, 1), c = tri_vertex(r, 2);
  LightBounds lb;
  lb.bbox.lo = Real3{ar::min(a.x, ar::min(b.x, c.x)), ar::min(a.y, ar::min(b.y, c.y)), ar::min(a.z, ar::min(b.z, c.z))};
  lb.bbox.hi = Real3{ar::max(a.x, ar::max(b.x, c.x)), ar::max(a.y, ar::max(b.y, c.y)), ar::max(a.z, ar::max(b.z, c.z))};
  lb.theta_o = Real(0);
  lb.two_sided = tri_two_sided(r);
  lb.theta_e = lb.two_sided ? kLightPi : kLightPi / Real(2);  // D9
  return lb;
}

AR_NS_END
