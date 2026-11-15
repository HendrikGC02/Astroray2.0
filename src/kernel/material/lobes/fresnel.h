// SPDX-License-Identifier: Apache-2.0
// Unpolarised dielectric Fresnel reflectance and OpenPBR's specular_weight
// modulation of it. No polarisation in 2.0: Rs and Rp are computed and averaged.
//
// Ref: docs/decisions/0005-material-model.md item 13 (mean of Rs and Rp);
// pbrt-v4 src/pbrt/util/scattering.h FrDielectric (Apache-2.0); OpenPBR Surface
// 1.1.2, dielectric specular reflection, specular_weight (also summarised in
// docs/research/05-spectral-and-materials.md B3).
#pragma once
#include "core/real.h"
#include "core/spectrum.h"
#include "portability.h"

AR_NS_BEGIN

// Reflectance of a smooth dielectric interface for light arriving from the side
// of index 1 at cos_i = cos(theta_i) with the other side's relative index eta.
// A negative cos_i means arriving from inside (eta inverted), as in pbrt-v4.
// Returns 1 under total internal reflection.
// Ref: pbrt-v4 FrDielectric; r_parl, r_perp are the Fresnel amplitudes for the
// two polarisations and the result is the mean of their squares (0005 item 13).
AR_FN Real fresnel_dielectric(Real cos_i, Real eta) {
  cos_i = min(max(cos_i, Real(-1)), Real(1));
  if (cos_i < Real(0)) {
    eta = Real(1) / eta;
    cos_i = -cos_i;
  }
  const Real sin2_i = Real(1) - cos_i * cos_i;
  const Real sin2_t = sin2_i / (eta * eta);
  if (sin2_t >= Real(1)) return Real(1);
  const Real cos_t = ar::sqrt(max(Real(0), Real(1) - sin2_t));
  const Real r_parl = (eta * cos_i - cos_t) / (eta * cos_i + cos_t);
  const Real r_perp = (cos_i - eta * cos_t) / (cos_i + eta * cos_t);
  return Real(0.5) * (r_parl * r_parl + r_perp * r_perp);
}

// Numerical guard only: xi F0 is clamped below 1 so that eta' stays finite
// (xi F0 = 1 is the mirror limit, eta' = infinity).
inline constexpr Real kSpecularMaxXiF0 = Real(0.9999);

// The index the Fresnel term uses once specular_weight xi scales the normal-
// incidence reflectance: F0 = ((eta-1)/(eta+1))^2, eps = sgn(eta-1) sqrt(xi F0),
// eta' = (1 + eps) / (1 - eps). Then F0(eta') = xi F0 while grazing reflectance
// still reaches 1. xi = 1 returns eta unchanged.
// Ref: OpenPBR Surface 1.1.2, specular_weight (dielectric specular reflection).
AR_FN Real specular_weighted_eta(Real eta, Real weight) {
  const Real r = (eta - Real(1)) / (eta + Real(1));
  const Real xi_f0 = min(weight * r * r, kSpecularMaxXiF0);
  const Real eps = ar::sqrt(max(Real(0), xi_f0));
  const Real signed_eps = eta >= Real(1) ? eps : -eps;
  return (Real(1) + signed_eps) / (Real(1) - signed_eps);
}

// Per-lane form: each transport wavelength has its own specular_ior (0005 item 7).
// Returns F(cos_i) per lane with the weight applied.
AR_FN Spec fresnel_dielectric_weighted(Real cos_i, const Spec& ior, Real weight) {
  Spec f;
  for (int i = 0; i < kLanes; ++i)
    f.v[i] = fresnel_dielectric(cos_i, specular_weighted_eta(ior.v[i], weight));
  return f;
}

AR_NS_END
