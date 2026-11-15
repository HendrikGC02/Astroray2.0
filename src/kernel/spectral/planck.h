// SPDX-License-Identifier: Apache-2.0
// Planck's law, spectral radiance per nanometre, safe in float over lambda in
// [0.1 nm, 1 m] and T in [3 K, 1e9 K] (no overflow, underflow goes to 0).
// B(lambda, T) = c1L lambda^-5 e^-x / (-expm1(-x)), x = c2 / (lambda T), lambda in
// micrometres, in W m^-2 sr^-1 nm^-1 (the 1e-3 of the nm). The e^-x form never forms
// e^x, and expm1 keeps the denominator exact as x -> 0.
// Ref: docs/decisions/0003-spectral-model-and-path-state.md d25; constants CODATA 2018,
// Tiesinga et al., Rev. Mod. Phys. 93, 025010 (2021): c1L = 2 h c^2, c2 = h c / k_B.
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../portability.h"

AR_NS_BEGIN

// Ref: CODATA 2018 (above); c1L in W um^4 m^-2 sr^-1, c2 in um K.
inline constexpr double kPlanckC1L = double(1.191042972e8);
inline constexpr double kPlanckC2 = double(14387.7688);
inline constexpr double kUmPerNm = double(1.0e-3);
inline constexpr double kNmPerUm = double(1.0e3);

// The same code serves the kernel (R = Real) and host double constant folding (R = double).
template <class R>
AR_FN R planck_radiance_t(R lambda_nm, R T) {
  const R l = lambda_nm / R(kNmPerUm);  // um; dividing by 1000 costs one rounding
  const R l2 = l * l;
  const R l4 = l2 * l2;
  // l^5 spans (1e-4)^5 = 1e-20 to (1e6)^5 = 1e30 over the domain: inside float range.
  const R inv_l5 = R(1) / (l4 * l);
  // x = c2 / (l T) with the um -> nm factor folded into the constant: one rounding fewer.
  const R x = R(kPlanckC2 * kNmPerUm) / (lambda_nm * T);
  const R ratio = ar::exp(-x) / (-ar::expm1(-x));
  return R(kPlanckC1L * kUmPerNm) * inv_l5 * ratio;
}

AR_FN Real planck_radiance(Real lambda_nm, Real T) { return planck_radiance_t<Real>(lambda_nm, T); }

AR_NS_END
