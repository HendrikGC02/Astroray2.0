// SPDX-License-Identifier: Apache-2.0
// The five packet operations: the only code that changes a path's transport
// wavelengths or reads its film weights. Lane 0 survives termination, never the
// hero lane (keeping the hero is the biased case of 0003 d6).
// Ref: docs/decisions/0003-spectral-model-and-path-state.md d5-d7; pbrt-v4
// SampledWavelengths::TerminateSecondary (Apache-2.0); Rybicki and Lightman
// 1979 section 1.5 (I_lambda lambda^5 is invariant) for apply_frequency_shift.
#pragma once
#include <cstdint>

#include "core/real.h"
#include "core/spectrum.h"
#include "portability.h"
#include "state/flags.h"

AR_NS_BEGIN

// Every lane evaluates at lambda[0] afterwards, so lane reductions equal lane 0.
// r_u and r_l are not arguments: they keep their pre-termination ratios (d7).
AR_FN void terminate_secondaries(Lambda& lam, Spec& beta, uint32_t& flags) {
  for (int i = 1; i < kLanes; ++i) {
    lam.nm[i] = lam.nm[0];
    beta.v[i] = beta.v[0];
  }
  flags |= SECONDARIES_TERMINATED;
}

// Fluorescence. The caller has already terminated the secondaries and drew
// lambda_i; ratio = M(lambda_i -> lambda_t[0]) / p(lambda_i).
AR_FN void rewrite_wavelength(Lambda& lam, Spec& beta, Real lambda_i, Real ratio, uint32_t& flags) {
  AR_ASSERT((flags & SECONDARIES_TERMINATED) != 0u);
  for (int i = 0; i < kLanes; ++i) {
    lam.nm[i] = lambda_i;
    beta.v[i] *= ratio;
  }
  flags |= WAVELENGTHS_REWRITTEN;
}

// g = nu_from / nu_to, from on the camera side. Geodesics are achromatic, so no
// lane is terminated. g^5 is the wavelength form of the invariant I_nu / nu^3.
AR_FN void apply_frequency_shift(Lambda& lam, Spec& beta, Real g, uint32_t& flags) {
  const Real g2 = g * g;
  const Real g4 = g2 * g2;
  const Real g5 = g4 * g;
  for (int i = 0; i < kLanes; ++i) {
    lam.nm[i] *= g;
    beta.v[i] *= g5;
  }
  flags |= WAVELENGTHS_REWRITTEN;
}

// Per-lane film weight; pf holds the film wavelengths' pdfs. Terminated paths
// deliver through lane 0 alone (pbrt-v4's pdf[1..3] = 0, pdf[0] /= 4).
AR_FN Spec film_weights(uint32_t flags, const Spec& pf) {
  Spec w;
  if ((flags & SECONDARIES_TERMINATED) != 0u) {
    w.v[0] = Real(1) / pf.v[0];
    for (int i = 1; i < kLanes; ++i) w.v[i] = Real(0);
  } else {
    for (int i = 0; i < kLanes; ++i) w.v[i] = Real(1) / (Real(kLanes) * pf.v[i]);
  }
  return w;
}

AR_FN uint32_t hero(uint32_t flags) { return hero_lane(flags); }

AR_NS_END
