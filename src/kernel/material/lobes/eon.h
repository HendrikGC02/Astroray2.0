// SPDX-License-Identifier: Apache-2.0
// EON diffuse lobe: the energy-preserving Oren-Nayar BRDF of OpenPBR, with the
// paper's CLTC + uniform importance sampling. Directions are in the local
// shading frame (z = normal) and f excludes the cosine.
//
// Ref: Portsmouth, Kutz, Hill 2025, "EON: A practical energy-preserving rough
// diffuse BRDF", JCGT 14(1) 116-139 (arXiv 2410.18026). Equation numbers and
// "Listing n" below are that paper's. Cross-check implementation: Adobe
// openpbr-bsdf (Apache-2.0).
//
// Contract (docs/decisions/0005 item 8): wo and wi must lie above the shading
// plane (z > 0). A sample or eval at z <= 0 returns f = 0 and pdf = 0; reflected
// samples below the geometric surface are rejected by the caller. EON is
// reciprocal and has no eta or adjoint term, so TransportMode does not change
// its values.
#pragma once
#include <cstdint>

#include "core/real.h"
#include "core/spectrum.h"
#include "core/vec.h"
#include "material/lobes/lobe.h"
#include "portability.h"

AR_NS_BEGIN

struct EonParams {
  Spec base_color;  // single-scattering albedo rho, per lane
  Real roughness;   // base_diffuse_roughness r in [0, 1]
};

inline constexpr Real kEonPi = Real(3.14159265358979323846);
// Ref: EON paper Listing 1 (constant1_FON, constant2_FON): the FON A coefficient
// and the average-albedo coefficient.
inline constexpr Real kEonC1 = Real(0.5) - Real(2) / (Real(3) * kEonPi);
inline constexpr Real kEonC2 = Real(2) / Real(3) - Real(28) / (Real(15) * kEonPi);

// Exact directional albedo of the FON lobe, for mu = cos(theta) > 0.
// Ref: EON paper Listing 1 (E_FON_exact).
AR_FN Real eon_fon_albedo(Real mu, Real r) {
  const Real af = Real(1) / (Real(1) + kEonC1 * r);
  const Real bf = r * af;
  const Real si = ar::sqrt(max(Real(0), Real(1) - mu * mu));
  const Real g = si * (ar::acos(min(mu, Real(1))) - si * mu) +
                 (Real(2) / Real(3)) * ((si / mu) * (Real(1) - si * si * si) - si);
  return af + (bf / kEonPi) * g;
}

// Directional albedo of the full EON lobe for wo.z = mu, per lane.
// Ref: EON paper Listing 1 (E_EON), Equation 28; analytic, so no table (0005 item 6).
AR_FN Spec eon_albedo(const EonParams& p, Real mu) {
  const Real r = p.roughness;
  const Real af = Real(1) / (Real(1) + kEonC1 * r);
  const Real ef = eon_fon_albedo(mu, r);
  const Real avg = af * (Real(1) + kEonC2 * r);
  Spec e;
  for (int i = 0; i < kLanes; ++i) {
    const Real rho = p.base_color.v[i];
    const Real rho_ms = rho * rho * avg / (Real(1) - rho * (Real(1) - avg));
    e.v[i] = rho * ef + rho_ms * (Real(1) - ef);
  }
  return e;
}

// BRDF value, Equation 16 / Listing 1 (f_EON, exact albedo variant).
// The paper floors 1 - E at 1e-7 so that r = 0 stays finite, which leaves a
// 1e-7 rho^2 / pi residue and so is not exactly Lambertian at r = 0. Here the
// numerators are floored at 0 (E can exceed 1 by rounding) and only the
// denominator keeps the paper's 1e-7, so r = 0 gives rho / pi exactly.
AR_FN Spec eon_f(const EonParams& p, Real3 wo, Real3 wi) {
  const Real r = p.roughness;
  const Real mu_o = wo.z, mu_i = wi.z;
  const Real s = dot(wi, wo) - mu_i * mu_o;
  const Real s_over_t = s > Real(0) ? s / max(mu_i, mu_o) : s;
  const Real af = Real(1) / (Real(1) + kEonC1 * r);
  const Real ef_o = eon_fon_albedo(mu_o, r);
  const Real ef_i = eon_fon_albedo(mu_i, r);
  const Real avg = af * (Real(1) + kEonC2 * r);
  const Real ms_shape = max(Real(0), Real(1) - ef_o) * max(Real(0), Real(1) - ef_i) /
                        max(Real(1e-7), Real(1) - avg);
  Spec f;
  for (int i = 0; i < kLanes; ++i) {
    const Real rho = p.base_color.v[i];
    const Real f_ss = (rho / kEonPi) * af * (Real(1) + r * s_over_t);
    const Real rho_ms = rho * rho * avg / (Real(1) - rho * (Real(1) - avg));
    f.v[i] = f_ss + (rho_ms / kEonPi) * ms_shape;
  }
  return f;
}

// LTC matrix M = [[a 0 b] [0 c 0] [d 0 1]] of the CLTC lobe.
// Ref: EON paper Listing 2 (ltc_coeffs), a polynomial/rational fit in mu = cos(theta_o) and r.
struct EonLtc {
  Real a, b, c, d;
};

AR_FN EonLtc eon_ltc_coeffs(Real mu, Real r) {
  EonLtc m;
  m.a = Real(1) + r * (Real(0.303392) + (Real(-0.518982) + Real(0.111709) * mu) * mu +
                       (Real(-0.276266) + Real(0.335918) * mu) * r);
  m.b = r * (Real(-1.16407) + Real(1.15859) * mu + (Real(0.150815) - Real(0.150105) * mu) * r) /
        (mu * mu * mu - Real(1.43545));
  m.c = Real(1) + r * (Real(0.20013) + (Real(-0.506373) + Real(0.261777) * mu) * mu);
  m.d = r * (Real(0.540852) + (Real(-1.01625) + Real(0.475392) * mu) * mu) /
        (Real(-1.0743) + (Real(0.0725628) + mu) * mu);
  return m;
}

// Probability of taking the uniform lobe of the one-sample MIS.
// Ref: EON paper Listing 4 (P_u).
AR_FN Real eon_uniform_prob(Real mu, Real r) {
  return ar::pow(r, Real(0.1)) *
         (Real(0.162925) + (Real(-0.372058) + (Real(0.538233) - Real(0.290822) * mu) * mu) * mu);
}

// The uniform lobe's density over the hemisphere.
AR_FN Real eon_uniform_pdf() { return Real(1) / (Real(2) * kEonPi); }

// LTC space is the local frame rotated about z so that wo lies in the x-z plane.
// Ref: EON paper Listing 3 (orthonormal_basis_ltc). Returns the unit x axis of that
// basis; y is (-x.y, x.x) and z is the normal.
AR_FN Real2 eon_ltc_x_axis(Real3 wo) {
  const Real len_sqr = wo.x * wo.x + wo.y * wo.y;
  if (len_sqr > Real(0)) {
    const Real inv = Real(1) / ar::sqrt(len_sqr);
    return Real2{wo.x * inv, wo.y * inv};
  }
  return Real2{Real(1), Real(0)};
}

// Density of the CLTC lobe at wi, per solid angle.
// Ref: EON paper Listing 3 (cltc_pdf), Equations 22-23 and 27.
AR_FN Real eon_cltc_pdf(Real3 wo, Real3 wi_local, Real r) {
  const Real2 x = eon_ltc_x_axis(wo);
  const Real3 wi{x.x * wi_local.x + x.y * wi_local.y, -x.y * wi_local.x + x.x * wi_local.y,
                 wi_local.z};
  const EonLtc m = eon_ltc_coeffs(wo.z, r);
  const Real det_m = m.c * (m.a - m.b * m.d);
  const Real3 wh{m.c * (wi.x - m.b * wi.z), (m.a - m.b * m.d) * wi.y,
                 -m.c * (m.d * wi.x - m.a * wi.z)};
  const Real len_sqr = dot(wh, wh);
  const Real vz = Real(1) / ar::sqrt(m.d * m.d + Real(1));
  const Real s = Real(0.5) * (Real(1) + vz);
  return det_m * det_m / (len_sqr * len_sqr) * max(wh.z, Real(0)) / (kEonPi * s);
}

struct EonDir {
  Real3 wi;
  Real pdf;
};

// Samples the clipped LTC lobe; pdf is its density at the returned wi.
// Ref: EON paper Listing 3 (cltc_sample); the half-disc/half-ellipse sampling is
// after Heitz 2018 as the paper states.
AR_FN EonDir eon_cltc_sample(Real3 wo, Real r, Real u1, Real u2) {
  const EonLtc m = eon_ltc_coeffs(wo.z, r);
  const Real rad = ar::sqrt(u1);
  const Real phi = Real(2) * kEonPi * u2;
  Real x = rad * ar::cos(phi);
  const Real y = rad * ar::sin(phi);
  const Real vz = Real(1) / ar::sqrt(m.d * m.d + Real(1));
  const Real s = Real(0.5) * (Real(1) + vz);
  // GLSL mix(sqrt(1 - y^2), x, s).
  x = -(ar::sqrt(max(Real(0), Real(1) - y * y)) * (Real(1) - s) + x * s);
  const Real3 wh{x, y, ar::sqrt(max(Real(1) - (x * x + y * y), Real(0)))};
  const Real pdf_wh = wh.z / (kEonPi * s);
  const Real3 mh{m.a * wh.x + m.b * wh.z, m.c * wh.y, m.d * wh.x + wh.z};
  const Real len = length(mh);
  const Real det_m = m.c * (m.a - m.b * m.d);
  const Real2 ax = eon_ltc_x_axis(wo);
  // fromLTC * (M H) / |M H|
  const Real3 wi{(ax.x * mh.x - ax.y * mh.y) / len, (ax.y * mh.x + ax.x * mh.y) / len, mh.z / len};
  return EonDir{wi, pdf_wh * len * len * len / det_m};
}

// Density of eon_sample at wi: the one-sample MIS of the uniform and CLTC lobes.
// Ref: EON paper Listing 4 (pdf_EON).
AR_FN Real eon_pdf(const EonParams& p, Real3 wo, Real3 wi, TransportMode mode) {
  (void)mode;
  if (wo.z <= Real(0) || wi.z <= Real(0)) return Real(0);
  const Real pu = eon_uniform_prob(wo.z, p.roughness);
  return pu * eon_uniform_pdf() + (Real(1) - pu) * eon_cltc_pdf(wo, wi, p.roughness);
}

AR_FN LobeEval eon_eval(const EonParams& p, Real3 wo, Real3 wi, TransportMode mode) {
  LobeEval e;
  if (wo.z <= Real(0) || wi.z <= Real(0)) {
    e.f = spec_fill(Real(0));
    e.pdf = Real(0);
    return e;
  }
  e.f = eon_f(p, wo, wi);
  e.pdf = eon_pdf(p, wo, wi, mode);
  return e;
}

// u = (u1, u2) uniform in [0, 1). u1 also picks the lobe, then is rescaled, as in
// the paper. The returned pdf is computed along the sampling path (the CLTC
// branch uses the sampler's own density, the uniform branch evaluates the CLTC
// density at wi), so it is an independent check of eon_pdf.
// Ref: EON paper Listing 4 (sample_EON).
AR_FN LobeSample eon_sample(const EonParams& p, Real3 wo, Real2 u, TransportMode mode) {
  (void)mode;
  LobeSample s;
  s.wi = Real3{Real(0), Real(0), Real(1)};
  s.f = spec_fill(Real(0));
  s.pdf = Real(0);
  s.flags = LOBE_REFLECTION;
  if (wo.z <= Real(0)) return s;

  const Real r = p.roughness;
  const Real pu = eon_uniform_prob(wo.z, r);
  const Real pc = Real(1) - pu;
  Real pdf_c;
  Real3 wi;
  // Strict, unlike Listing 4's `<=`: at pu = 0 (roughness 0) u.x = 0 would otherwise pick the
  // uniform lobe and divide 0 by 0. With `<`, pu = 0 never selects it, and u.x < pu implies pu > 0.
  if (u.x < pu) {
    const Real z = u.x / pu;
    const Real sin_t = ar::sqrt(max(Real(0), Real(1) - z * z));
    const Real phi = Real(2) * kEonPi * u.y;
    wi = Real3{sin_t * ar::cos(phi), sin_t * ar::sin(phi), z};
    pdf_c = eon_cltc_pdf(wo, wi, r);
  } else {
    const EonDir d = eon_cltc_sample(wo, r, (u.x - pu) / pc, u.y);
    wi = d.wi;
    pdf_c = d.pdf;
  }
  if (wi.z <= Real(0)) return s;  // a measure-zero rounding case: report "no sample"
  s.wi = wi;
  s.pdf = pu * eon_uniform_pdf() + pc * pdf_c;
  s.f = eon_f(p, wo, wi);
  return s;
}

AR_NS_END
