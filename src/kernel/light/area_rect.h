// SPDX-License-Identifier: Apache-2.0
// Rectangle (and square) area lamp: constant radiance L = P / (pi A) s(lambda) from the front
// face only, which radiates exactly P (flux = L * pi * A), sampled by the solid angle it
// subtends (0004 D6, D8). Include light.h, not this file.
//
// Frame: the lamp's local X and Y axes are the first two columns of xf and span the rectangle,
// size[0] x size[1] is its full width and height in world units (the host bakes scale into size,
// light_problem refuses a scaled xf). The front is local -Z, the direction Blender's area lamp
// (like every Blender light) emits along; the back emits nothing.
//
// Ref: docs/decisions/0004-integrator.md D6 (area row), D8, D9 (theta_e = pi/2 one-sided);
// Urena, Fajardo and King 2013, "An Area-Preserving Parametrization for Spherical Rectangles",
// CGF 32(4), Algorithms 1 (precomputation) and 2 (sampling), reference code `sphquad`;
// pbrt-v4 src/pbrt/shapes.h BilinearPatch::Sample for the spherical/area switch; src/pbrt/lights.cpp
// DiffuseAreaLight::SampleLe (area point by u1, cosine hemisphere about the normal by u2) and
// Bounds (normal cone of one direction, cos(pi/2) emission spread).
#pragma once
#ifndef AR_LIGHT_TYPES_DEFINED
#error "include light/light.h, not area_rect.h"
#endif

AR_NS_BEGIN

// pbrt-v4 src/pbrt/shapes.h MinSphericalSampleArea and MaxSphericalSampleArea, the range in which
// BilinearPatch::Sample (a quad) samples spherically: inside [3e-4, 6.22] sr the lamp is sampled
// by solid angle, outside by area, with pdf d^2 / (cos A). The area branch is exact
// for its own pdf. sample_li and pdf_li take the same branch because both decide from the
// context alone.
inline constexpr Real kRectMinSolidAngle = Real(3e-4);
inline constexpr Real kRectMaxSolidAngle = Real(6.22);
AR_FN bool rect_spherical(Real S) { return S >= kRectMinSolidAngle && S <= kRectMaxSolidAngle; }

// sin and cos of an angle x of a few pi, for the lamp samplers (this header and triangle.h). CUDA's
// sinf / cosf carry a double-precision slow path a GPU shade kernel may not contain (portability.h),
// and sinpi(x / pi) alone loses the rounding of x / pi (2^-24 relative in x), which the spherical
// samplers amplify when the subtended solid angle is small. So x is reduced Cody-Waite style:
// t = RN(x / pi_hi), with pi_hi the Real nearest pi; r = x - t pi is the exact FMA remainder
// fma(-t, pi_hi, x) (representable for t = RN(x / pi_hi)) corrected by -t pi_lo, pi_lo = pi - pi_hi;
// then sin x = sinpi(t) cos r + cospi(t) sin r, with |r| of a few ulps of x, so cos r = 1 and
// sin r = r to the Real's precision.
// Ref: Cody and Waite 1980, "Software Manual for the Elementary Functions" (additive argument
// reduction with a split constant); Boldo, Daumas, Moreau-Finot and Thery 2002, "Computer validated
// proofs of a toolset for adaptable arithmetic" (the FMA remainder of a rounded quotient is exact);
// CUDA C++ Programming Guide, appendix "Mathematical Functions" (sinpif, cospif: 1 ulp).
struct LightSinCos {
  Real s, c;
};
AR_FN LightSinCos light_sincos(Real x) {
  const Real pi_hi = sizeof(Real) == 4 ? Real(float(3.14159265358979323846)) : Real(3.14159265358979323846);
  const Real pi_lo = sizeof(Real) == 4 ? Real(3.14159265358979323846 - double(float(3.14159265358979323846)))
                                       : Real(1.2246467991473532e-16);
  const Real t = x / pi_hi;
  const Real r = ar::fma(-t, pi_hi, x) - t * pi_lo;
  const Real st = ar::sinpi(t), ct = ar::cospi(t);
  return LightSinCos{st + ct * r, ct - st * r};
}
// Column `col` of the rotation part of xf: a lamp-local axis in world space.
AR_FN Real3 area_axis(const LightRecord& r, int col) {
  return Real3{Real(r.xf[col]), Real(r.xf[4 + col]), Real(r.xf[8 + col])};
}
// The face that emits: local -Z.
AR_FN Real3 area_front(const LightRecord& r) { return -area_axis(r, 2); }

struct RectGeom {
  Real3 corner, ex, ey, n;  // corner at local (-w/2, -h/2); n = front normal
  Real w, h;
};

AR_FN RectGeom rect_geom(const LightRecord& r) {
  RectGeom g;
  g.ex = area_axis(r, 0);
  g.ey = area_axis(r, 1);
  g.n = area_front(r);
  g.w = Real(r.size[0]);
  g.h = Real(r.size[1]);
  g.corner = light_position(r) - g.ex * (g.w * Real(0.5)) - g.ey * (g.h * Real(0.5));
  return g;
}

AR_FN Real rect_area(const LightRecord& r) { return Real(r.size[0]) * Real(r.size[1]); }

AR_FN Spec rect_radiance(const LightRecord& r, const Lambda& lam, const ObserverView& obs) {
  return light_emission(r, lam, obs) / (kLightPi * rect_area(r));
}

// Algorithm 1: the rectangle seen from o, in the frame (ex, ey, n) with o at the origin and the
// rectangle on the plane z = z0 < 0. The frame may be left-handed; every use below is a dot
// product or its inverse, so only z0 < 0 matters.
struct SphQuad {
  Real x0, y0, x1, y1, z0, z0sq, y0sq, y1sq;
  Real b0, b1, b0sq, k, S;
};

AR_FN Real rect_clamp(Real x, Real lo, Real hi) { return x < lo ? lo : (x > hi ? hi : x); }

// False when o is not strictly in front of the rectangle's plane or the quad is degenerate.
AR_FN bool sphquad_init(const RectGeom& g, Real3 o, SphQuad* q) {
  const Real3 d = g.corner - o;
  q->z0 = dot(d, g.n);
  if (!(q->z0 < Real(0))) return false;
  q->z0sq = q->z0 * q->z0;
  q->x0 = dot(d, g.ex);
  q->y0 = dot(d, g.ey);
  q->x1 = q->x0 + g.w;
  q->y1 = q->y0 + g.h;
  q->y0sq = q->y0 * q->y0;
  q->y1sq = q->y1 * q->y1;
  const Real3 v00{q->x0, q->y0, q->z0}, v01{q->x0, q->y1, q->z0};
  const Real3 v10{q->x1, q->y0, q->z0}, v11{q->x1, q->y1, q->z0};
  const Real3 n0 = normalize(cross(v00, v10)), n1 = normalize(cross(v10, v11));
  const Real3 n2 = normalize(cross(v11, v01)), n3 = normalize(cross(v01, v00));
  const Real g0 = ar::acos(rect_clamp(-dot(n0, n1), Real(-1), Real(1)));
  const Real g1 = ar::acos(rect_clamp(-dot(n1, n2), Real(-1), Real(1)));
  const Real g2 = ar::acos(rect_clamp(-dot(n2, n3), Real(-1), Real(1)));
  const Real g3 = ar::acos(rect_clamp(-dot(n3, n0), Real(-1), Real(1)));
  q->b0 = n0.z;
  q->b1 = n2.z;
  q->b0sq = q->b0 * q->b0;
  q->k = Real(2) * kLightPi - g2 - g3;
  q->S = g0 + g1 - q->k;
  return q->S > Real(0) && q->S - q->S == Real(0);  // also rejects NaN from a degenerate corner
}

// Algorithm 2: the point of the rectangle at (u.x, u.y), area-preserving in solid angle.
AR_FN Real3 sphquad_sample(const RectGeom& g, const SphQuad& q, Real3 o, Real2 u) {
  // 1. cu, the cosine that fixes the x coordinate
  const Real au = u.x * q.S + q.k;
  const LightSinCos sc = light_sincos(au);
  const Real fu = (sc.c * q.b0 - q.b1) / sc.s;
  const Real cu = rect_clamp((fu > Real(0) ? Real(1) : Real(-1)) / ar::sqrt(fu * fu + q.b0sq), Real(-1), Real(1));
  // 2. xu; cu = +-1 is the x0 or x1 edge, where the quotient below is infinite
  const Real den = light_safe_sqrt(Real(1) - cu * cu);
  const Real xu = den > Real(0) ? rect_clamp(-(cu * q.z0) / den, q.x0, q.x1) : (cu > Real(0) ? q.x1 : q.x0);
  // 3. yv from the second uniform
  const Real dd = ar::sqrt(xu * xu + q.z0sq);
  const Real h0 = q.y0 / ar::sqrt(dd * dd + q.y0sq);
  const Real h1 = q.y1 / ar::sqrt(dd * dd + q.y1sq);
  const Real hv = h0 + u.y * (h1 - h0);
  const Real hv2 = hv * hv;
  const Real yv = hv2 < Real(1) - Real(1e-6) ? rect_clamp(hv * dd / ar::sqrt(Real(1) - hv2), q.y0, q.y1) : q.y1;
  // 4. back to world coordinates
  return o + g.ex * xu + g.ey * yv + g.n * q.z0;
}

// pbrt-v4 Disk::PDF / Shape::PDF area-to-solid-angle conversion: (1 / A) d^2 / cos, d the vector
// from the context to the lamp point and wi its unit direction; 0 where cos <= 0 or the pdf is
// not finite. Shared by sample_li and pdf_li so the two agree to the last bit.
AR_FN Real rect_area_pdf(const RectGeom& g, Real3 d, Real3 wi, const LightRecord& r) {
  const Real d2 = dot(d, d);
  const Real cosl = dot(g.n, -wi);
  if (!(d2 > Real(0) && cosl > Real(0))) return Real(0);
  const Real pdf = d2 / (cosl * rect_area(r));
  return pdf - pdf == Real(0) ? pdf : Real(0);
}

// pdf 0 and Le 0 for a context behind or on the lamp's plane: the lamp emits forward only.
AR_FN LightLiSample rect_sample_li(const LightRecord& r, const LightSampleContext& ctx, Real2 u, const Lambda& lam,
                                   const ObserverView& obs) {
  LightLiSample s = light_li_none();
  const RectGeom g = rect_geom(r);
  SphQuad q;
  if (!sphquad_init(g, ctx.p, &q)) return s;
  const bool spherical = rect_spherical(q.S);
  const Real3 p = spherical ? sphquad_sample(g, q, ctx.p, u) : g.corner + g.ex * (u.x * g.w) + g.ey * (u.y * g.h);
  const Real3 d = p - ctx.p;
  const Real dist = length(d);
  if (!(dist > Real(0))) return s;
  // The area branch takes its pdf from the normalised wi, as pdf_li does from a BSDF ray's wi.
  const Real pdf = spherical ? Real(1) / q.S : rect_area_pdf(g, d, d / dist, r);
  if (!(pdf > Real(0)) || !(pdf - pdf == Real(0))) return s;
  s.p_light = p;
  s.dist = dist;
  s.wi = d / dist;
  s.pdf = pdf;
  s.Le = rect_radiance(r, lam, obs);
  if (!is_finite(s.Le)) return light_li_none();  // backstop: light_problem refuses overflowing sizes
  return s;
}

// wi is the direction of a BSDF ray that hit the lamp at hit.p (D1, D8).
AR_FN Real rect_pdf_li(const LightRecord& r, const LightSampleContext& ctx, Real3 wi, const LightHit& hit) {
  const RectGeom g = rect_geom(r);
  SphQuad q;
  if (!sphquad_init(g, ctx.p, &q)) return Real(0);
  Real pdf;
  if (rect_spherical(q.S)) {
    pdf = Real(1) / q.S;
  } else {
    pdf = rect_area_pdf(g, hit.p - ctx.p, wi, r);
  }
  return pdf - pdf == Real(0) ? pdf : Real(0);
}

AR_FN LightLeSample rect_sample_le(const LightRecord& r, Real2 u_pos, Real2 u_dir, const Lambda& lam,
                                   const ObserverView& obs) {
  const RectGeom g = rect_geom(r);
  const Real area = rect_area(r);
  if (!(area > Real(0))) return light_le_none();
  const Real3 local = sample_cosine_hemisphere(u_dir);
  LightLeSample s;
  s.ray.o = g.corner + g.ex * (u_pos.x * g.w) + g.ey * (u_pos.y * g.h);
  s.ray.d = to_world(frame_from_normal(g.n), local);
  s.Le = rect_radiance(r, lam, obs);
  s.pdf_pos = Real(1) / area;
  s.pdf_dir = local.z / kLightPi;
  // pbrt-v4 DiffuseAreaLight::SampleLe: a direction on the horizon (cosine pdf 0) is no sample.
  if (s.pdf_dir == Real(0)) return light_le_none();
  // Backstop only: light_problem refuses sizes whose L or 1 / A overflows.
  if (!is_finite(s.Le) || !(s.pdf_pos - s.pdf_pos == Real(0))) return light_le_none();
  return s;
}

AR_FN Real rect_power(const LightRecord& r) { return Real(r.strength); }

AR_FN LightBounds rect_bounds(const LightRecord& r) {
  const RectGeom g = rect_geom(r);
  const Real3 a = g.corner, b = g.corner + g.ex * g.w, c = g.corner + g.ey * g.h, d = b + g.ey * g.h;
  LightBounds lb;
  lb.bbox.lo = Real3{ar::min(ar::min(a.x, b.x), ar::min(c.x, d.x)), ar::min(ar::min(a.y, b.y), ar::min(c.y, d.y)),
                     ar::min(ar::min(a.z, b.z), ar::min(c.z, d.z))};
  lb.bbox.hi = Real3{ar::max(ar::max(a.x, b.x), ar::max(c.x, d.x)), ar::max(ar::max(a.y, b.y), ar::max(c.y, d.y)),
                     ar::max(ar::max(a.z, b.z), ar::max(c.z, d.z))};
  lb.theta_o = Real(0);  // a flat lamp has one normal
  lb.theta_e = kLightPi / Real(2);
  lb.two_sided = false;
  return lb;
}

AR_NS_END
