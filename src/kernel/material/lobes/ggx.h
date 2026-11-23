// SPDX-License-Identifier: Apache-2.0
// GGX dielectric specular reflection lobe of OpenPBR: height-correlated Smith
// masking, visible-normal sampling, specular_weight / specular_color / specular_ior.
// Directions are in the local shading frame (z = normal); f excludes the cosine.
// Isotropic only (anisotropy, transmission, coat and energy compensation are later
// tasks). The alpha-level helpers below are shared with the metal lobe (metal.h).
//
// Ref: GGX / Trowbridge-Reitz D and the microfacet BRDF f = F D G2 / (4 cos_o cos_i):
// Walter et al. 2007 (EGSR), Eq. 33 and the reflection term of Eq. 21;
// height-correlated Smith: Heitz 2014, JCGT 3(2), Eq. 99 (G2) and Eq. 72 (Lambda
// for GGX); VNDF: Heitz 2018, JCGT 7(4), Listing 1 and Eq. 3 (D_wo);
// alpha = specular_roughness^2: OpenPBR Surface 1.1.2; delta threshold: delta.h.
// Cross-check: Adobe openpbr-bsdf (Apache-2.0), pbrt-v4 TrowbridgeReitzDistribution.
//
// Contract (0005 item 8): wo and wi must lie above the shading plane (z > 0); at
// z <= 0 the lobe returns f = 0, pdf = 0 ("no sample"; the caller terminates).
// Reflection has no eta^2 term and the adjoint shading-normal correction belongs
// to the caller, so TransportMode does not change values.
//
// Delta convention: below the threshold ggx_sample returns the mirror direction
// with flag LOBE_DELTA, pdf = 1 and f = F / cos(wi), so f cos / pdf = F as for
// every other lobe; eval and pdf return 0 for a delta lobe (0004 D19: a delta
// lobe has no BSDF-strategy density).
#pragma once
#include <cstdint>

#include "core/real.h"
#include "core/spectrum.h"
#include "core/vec.h"
#include "material/lobes/delta.h"
#include "material/lobes/fresnel.h"
#include "material/lobes/lobe.h"
#include "portability.h"

AR_NS_BEGIN

struct GgxParams {
  Spec specular_color;  // tints the reflection (OpenPBR specular_color)
  Spec specular_ior;    // per transport wavelength
  Real specular_weight;
  Real roughness;  // specular_roughness in [0, 1]
};

inline constexpr Real kGgxPi = Real(3.14159265358979323846);

// alpha = roughness^2 (OpenPBR Surface 1.1.2); shared by every GGX-based lobe.
AR_FN Real ggx_alpha_of(Real roughness) { return roughness * roughness; }
AR_FN Real ggx_alpha(const GgxParams& p) { return ggx_alpha_of(p.roughness); }

// Heitz 2014 Eq. 72: Lambda(w) = (sqrt(1 + alpha^2 tan^2) - 1) / 2, w.z > 0. tan^2 is
// formed from w.x, w.y rather than 1 - w.z^2, which cancels badly near the normal in float.
AR_FN Real ggx_lambda(Real alpha, Real3 w) {
  const Real tan2 = (w.x * w.x + w.y * w.y) / (w.z * w.z);
  return Real(0.5) * (ar::sqrt(Real(1) + alpha * alpha * tan2) - Real(1));
}

// Normal distribution D(m), per solid angle of m, unit m.
// Walter 2007 Eq. 33 in the algebraically equal form
// alpha^2 / (pi ((1 - c^2) + alpha^2 c^2)^2) with c = m.z, which stays finite at grazing m;
// 1 - c^2 is taken as m.x^2 + m.y^2 for float accuracy near the normal.
AR_FN Real ggx_d(Real alpha, Real3 m) {
  const Real a2 = alpha * alpha;
  const Real t = m.x * m.x + m.y * m.y + a2 * m.z * m.z;
  return a2 / (kGgxPi * t * t);
}

// Height-correlated masking-shadowing G2(wo, wi) (Heitz 2014 Eq. 99) and the
// one-direction G1 = 1 / (1 + Lambda) (Heitz 2014).
AR_FN Real ggx_g2(Real alpha, Real3 wo, Real3 wi) {
  return Real(1) / (Real(1) + ggx_lambda(alpha, wo) + ggx_lambda(alpha, wi));
}
AR_FN Real ggx_g1(Real alpha, Real3 wo) { return Real(1) / (Real(1) + ggx_lambda(alpha, wo)); }

// Reflection half vector m = (wo + wi) / |wo + wi|; false when the sum has zero length
// (wi = -wo), which every caller treats as "no value, no density".
AR_FN bool ggx_half_vector(Real3 wo, Real3 wi, Real3& m) {
  const Real3 h = wo + wi;
  const Real len = length(h);
  if (!(len > Real(0))) return false;
  m = h / len;
  return true;
}

// Density per solid angle of wi = reflect(wo, m) when m is drawn by ggx_sample_vndf:
// D_wo(m) / (4 wo.m) = G1(wo) D(m) / (4 cos_o) (Heitz 2018 Eq. 3; reflection Jacobian 1 / (4 wo.m)).
AR_FN Real ggx_reflect_pdf(Real alpha, Real3 wo, Real3 m) {
  return ggx_g1(alpha, wo) * ggx_d(alpha, m) / (Real(4) * wo.z);
}

// The Fresnel-free part of the microfacet reflection BRDF, D G2 / (4 cos_o cos_i) (Walter 2007 Eq. 21).
AR_FN Real ggx_reflect_dg(Real alpha, Real3 wo, Real3 wi, Real3 m) {
  return ggx_d(alpha, m) * ggx_g2(alpha, wo, wi) / (Real(4) * wo.z * wi.z);
}

AR_FN LobeEval ggx_zero_eval() {
  LobeEval e;
  e.f = spec_fill(Real(0));
  e.pdf = Real(0);
  return e;
}

// Density of ggx_sample at wi, per solid angle: D_wo(m) / (4 wo.m) = G1(wo) D(m) / (4 cos_o)
// (Heitz 2018 Eq. 3; the reflection Jacobian is 1 / (4 wo.m)).
AR_FN Real ggx_pdf(const GgxParams& p, Real3 wo, Real3 wi, TransportMode mode) {
  (void)mode;
  const Real alpha = ggx_alpha(p);
  if (lobe_is_delta(alpha, alpha) || wo.z <= Real(0) || wi.z <= Real(0)) return Real(0);
  Real3 m{};
  if (!ggx_half_vector(wo, wi, m)) return Real(0);
  const Real pdf = ggx_reflect_pdf(alpha, wo, m);
  return pdf > Real(0) ? pdf : Real(0);  // also maps NaN to "no density"
}

AR_FN LobeEval ggx_eval(const GgxParams& p, Real3 wo, Real3 wi, TransportMode mode) {
  const Real alpha = ggx_alpha(p);
  if (lobe_is_delta(alpha, alpha) || wo.z <= Real(0) || wi.z <= Real(0)) return ggx_zero_eval();
  Real3 m{};
  if (!ggx_half_vector(wo, wi, m)) return ggx_zero_eval();
  const Real cos_o_m = dot(wo, m);
  const Real scale = ggx_reflect_dg(alpha, wo, wi, m);
  LobeEval e;
  const Spec fr = fresnel_dielectric_weighted(cos_o_m, p.specular_ior, p.specular_weight);
  for (int i = 0; i < kLanes; ++i) e.f.v[i] = p.specular_color.v[i] * fr.v[i] * scale;
  e.pdf = ggx_pdf(p, wo, wi, mode);
  if (!(e.pdf > Real(0)) || !is_finite(e.f)) return ggx_zero_eval();
  return e;
}

// Samples a visible normal. u in [0, 1).
// Ref: Heitz 2018, JCGT 7(4), Listing 1 (sampleGGXVNDF).
AR_FN Real3 ggx_sample_vndf(Real alpha, Real3 wo, Real2 u) {
  // Stretch the view direction to the hemisphere configuration.
  const Real3 vh = normalize(Real3{alpha * wo.x, alpha * wo.y, wo.z});
  // Orthonormal basis around vh, with the paper's special case at the pole.
  const Real lensq = vh.x * vh.x + vh.y * vh.y;
  const Real3 t1 = lensq > Real(0) ? Real3{-vh.y, vh.x, Real(0)} / ar::sqrt(lensq) : Real3{Real(1), Real(0), Real(0)};
  const Real3 t2 = cross(vh, t1);
  // Uniform disk sample warped to the projected half-disk.
  const Real r = ar::sqrt(u.x);
  // phi = 2 pi u.y, through cospi / sinpi (portability.h: no double slow path on the GPU).
  const Real a = r * ar::cospi(Real(2) * u.y);
  Real b = r * ar::sinpi(Real(2) * u.y);
  const Real s = Real(0.5) * (Real(1) + vh.z);
  b = (Real(1) - s) * ar::sqrt(max(Real(0), Real(1) - a * a)) + s * b;
  const Real3 nh = t1 * a + t2 * b + vh * ar::sqrt(max(Real(0), Real(1) - a * a - b * b));
  // Unstretch.
  return normalize(Real3{alpha * nh.x, alpha * nh.y, max(Real(0), nh.z)});
}

AR_FN LobeSample ggx_sample(const GgxParams& p, Real3 wo, Real2 u, TransportMode mode) {
  (void)mode;
  LobeSample s;
  s.wi = Real3{Real(0), Real(0), Real(1)};
  s.f = spec_fill(Real(0));
  s.pdf = Real(0);
  s.flags = LOBE_REFLECTION;
  if (wo.z <= Real(0)) return s;
  const Real alpha = ggx_alpha(p);

  if (lobe_is_delta(alpha, alpha)) {
    // Mirror direction; F at the unperturbed normal.
    s.wi = Real3{-wo.x, -wo.y, wo.z};
    const Spec fr = fresnel_dielectric_weighted(wo.z, p.specular_ior, p.specular_weight);
    for (int i = 0; i < kLanes; ++i) s.f.v[i] = p.specular_color.v[i] * fr.v[i] / wo.z;
    s.pdf = Real(1);
    s.flags = LOBE_REFLECTION | LOBE_DELTA;
    return s;
  }

  const Real3 m = ggx_sample_vndf(alpha, wo, u);
  const Real cos_o_m = dot(wo, m);
  const Real3 wi = m * (Real(2) * cos_o_m) - wo;
  // A reflected direction below the surface is "no sample" (0005 item 8).
  if (!(m.z > Real(0)) || !(cos_o_m > Real(0)) || !(wi.z > Real(0))) return s;
  const Real pdf = ggx_reflect_pdf(alpha, wo, m);
  const Real scale = ggx_reflect_dg(alpha, wo, wi, m);
  const Spec fr = fresnel_dielectric_weighted(cos_o_m, p.specular_ior, p.specular_weight);
  Spec f;
  for (int i = 0; i < kLanes; ++i) f.v[i] = p.specular_color.v[i] * fr.v[i] * scale;
  if (!(pdf > Real(0)) || !is_finite(f)) return s;
  s.wi = wi;
  s.pdf = pdf;
  s.f = f;
  return s;
}

AR_NS_END
