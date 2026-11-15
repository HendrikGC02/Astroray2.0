// SPDX-License-Identifier: Apache-2.0
// Emissive triangle: one light per triangle of an emissive material (0004 D9), constant
// radiance Le = strength * s(lambda) or, with kLightEmissionProgram, the material's emission entry
// at the sampled point, which the consumer evaluates (deferred_le). Sampled by the spherical triangle it subtends when that
// solid angle is inside pbrt's stable range, else uniformly by area. The front is the winding
// normal cross(p1 - p0, p2 - p0); a one-sided triangle emits only toward it. Include light.h.
//
// Ref: docs/decisions/0004-integrator.md D9 (sampling switch, sidedness, power = A pi estimate);
// Arvo 1995, "Stratified Sampling of Spherical Triangles"; pbrt-v4 src/pbrt/shapes.h
// Triangle::Sample(ctx, u), Triangle::PDF(ctx, wi), Triangle::SolidAngle, MinSphericalSampleArea
// and MaxSphericalSampleArea; src/pbrt/util/sampling.cpp SampleSphericalTriangle; util/sampling.h
// SampleUniformTriangle; util/vecmath.h SphericalTriangleArea, AngleBetween; src/pbrt/lights.cpp
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
AR_FN Real tri_safe_asin(Real x) { return ar::asin(tri_clamp(x, Real(-1), Real(1))); }

// pbrt-v4 AngleBetween: the numerically stable angle between unit vectors.
AR_FN Real tri_angle_between(Real3 a, Real3 b) {
  return dot(a, b) < Real(0) ? kLightPi - Real(2) * tri_safe_asin(length(a + b) / Real(2))
                             : Real(2) * tri_safe_asin(length(b - a) / Real(2));
}

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

// pbrt-v4 SampleSphericalTriangle (Arvo 1995): the point of the triangle in the sampled
// direction from p, from clamped barycentrics. False when the spherical triangle is degenerate.
AR_FN bool tri_sample_spherical(const LightRecord& r, Real3 p, Real2 u, Real3* out) {
  const Real3 v0 = tri_vertex(r, 0), v1 = tri_vertex(r, 1), v2 = tri_vertex(r, 2);
  const Real3 a = normalize(v0 - p), b = normalize(v1 - p), c = normalize(v2 - p);
  Real3 n_ab = cross(a, b), n_bc = cross(b, c), n_ca = cross(c, a);
  if (dot(n_ab, n_ab) == Real(0) || dot(n_bc, n_bc) == Real(0) || dot(n_ca, n_ca) == Real(0)) return false;
  n_ab = normalize(n_ab);
  n_bc = normalize(n_bc);
  n_ca = normalize(n_ca);
  const Real alpha = tri_angle_between(n_ab, -n_ca);
  const Real beta = tri_angle_between(n_bc, -n_ab);
  const Real gamma = tri_angle_between(n_ca, -n_bc);
  const Real A_pi = alpha + beta + gamma;
  if (!(A_pi - kLightPi > Real(0))) return false;  // pbrt's triPDF == 0
  const Real Ap_pi = (Real(1) - u.x) * kLightPi + u.x * A_pi;  // Lerp(u0, pi, A_pi)
  // area_rect.h's light_sincos: no sinf / cosf in a GPU shade kernel, no precision lost to x / pi.
  const LightSinCos sa = light_sincos(alpha), sp = light_sincos(Ap_pi);
  const Real cos_alpha = sa.c, sin_alpha = sa.s, sin_ap = sp.s, cos_ap = sp.c;
  const Real sin_phi = sin_ap * cos_alpha - cos_ap * sin_alpha;
  const Real cos_phi = cos_ap * cos_alpha + sin_ap * sin_alpha;
  const Real k1 = cos_phi + cos_alpha;
  const Real k2 = sin_phi - sin_alpha * dot(a, b);
  // pbrt clamps; a NaN here means a triangle covering a hemisphere, outside kTriMaxSolidAngle.
  const Real cos_bp = tri_clamp((k2 + (k2 * cos_phi - k1 * sin_phi) * cos_alpha) / ((k2 * sin_phi + k1 * cos_phi) * sin_alpha),
                                Real(-1), Real(1));
  const Real sin_bp = light_safe_sqrt(Real(1) - cos_bp * cos_bp);
  const Real3 cp = a * cos_bp + normalize(c - a * dot(c, a)) * sin_bp;  // GramSchmidt(c, a)
  const Real cos_t = Real(1) - u.y * (Real(1) - dot(cp, b));
  const Real sin_t = light_safe_sqrt(Real(1) - cos_t * cos_t);
  const Real3 w = b * cos_t + normalize(cp - b * dot(cp, b)) * sin_t;
  const Real3 e1 = v1 - v0, e2 = v2 - v0;
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
