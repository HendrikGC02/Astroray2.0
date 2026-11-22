// SPDX-License-Identifier: Apache-2.0
// Phase functions: eval, sample and pdf of a PhaseLobe. HG is the only PhaseKind.
//
// Convention. wo and wi are unit vectors pointing away from the scattering point: wo toward where
// the light goes (the camera side of the path), wi toward where it comes from (the side a sample
// continues to). Light propagates along -wi before the event and along wo after it, so the
// scattering angle theta of Henyey and Greenstein has cos theta = mu = dot(-wi, wo) = -dot(wo, wi).
// g > 0 is forward scattering (peak at wi = -wo), g < 0 backward (peak at wi = wo). The phase value
// is per unit solid angle of wi and integrates to 1 over the sphere; it carries no sigma_s, which is
// the lobe's weight.
//
// Henyey and Greenstein 1941, ApJ 93, 70:
//   p(mu) = (1 / 4 pi) (1 - g^2) / d^(3/2),   d = 1 + g^2 - 2 g mu,   -1 < g < 1.
// For unit wo and wi, |wo + wi|^2 = 2 (1 - mu) and |wo - wi|^2 = 2 (1 + mu), so
//   d = (1 - g)^2 + g |wo + wi|^2 = (1 + g)^2 - g |wo - wi|^2.
// eval uses the first form for g >= 0 and the second for g < 0: both are sums of non-negative
// terms, so d > 0 with no cancellation at the peak, where float mu would lose the digits that set
// p. g is clamped to [-kHgMaxAbsG, kHgMaxAbsG]: the 1941 formula holds on the open interval
// (-1, 1) and d reaches 0 at |g| = 1. The margin 1e-3 is our choice, not taken from a reference.
//
// Sampling (exact, so p / pdf = 1; 0006 D12). The CDF of mu with u in [0, 1) is
//   P(mu) = (1 - g^2) / (2 g) [d^(-1/2) - 1 / (1 + g)],
// and P(mu) = u gives d^(1/2) = (1 - g^2) / t with t = 1 + g (2u - 1). Then
//   pdf = (1 - g^2) / (4 pi d^(3/2)) = t^3 / (4 pi (1 - g^2)^2),
//   1 - mu = (d - (1 - g)^2) / (2 g) = (1 - g)^2 (1 - u) (1 + g + t) / t^2,
//   1 + mu = ((1 + g)^2 - d) / (2 g) = (1 + g)^2 u (1 - g + t) / t^2,
// where the factor g cancels by algebra: g = 0 gives mu = 2u - 1 (isotropic) without a branch, and
// both forms are products of positive terms for |g| < 1, so sin^2 theta = (1 - mu)(1 + mu) >= 0.
// The azimuth about -wo is 2 pi u.y. The derivation was checked against the 1941 formula with an
// independent float64 NumPy integration of 2 pi p(mu) d mu; it was not cross-checked against
// pbrt-v4 or Cycles.
// Ref: Henyey and Greenstein 1941; docs/decisions/0006-volumes-and-astrophysics.md D12, V7.
#pragma once
#include <cstdint>

#include "core/frame.h"
#include "core/real.h"
#include "core/vec.h"
#include "medium/medium.h"
#include "portability.h"

AR_NS_BEGIN

inline constexpr Real kHgMaxAbsG = Real(0.999);
inline constexpr Real kPhaseInv4Pi = Real(0.25 / kPiDouble);

struct PhaseSample {
  Real3 wi;
  Real p;    // phase value at wi, per solid angle
  Real pdf;  // equal to p: HG is sampled exactly
};

AR_FN Real hg_g(const PhaseLobe& lobe) { return ar::min(kHgMaxAbsG, ar::max(-kHgMaxAbsG, Real(lobe.p0))); }

AR_FN Real phase_eval(const PhaseLobe& lobe, Real3 wo, Real3 wi) {
  const Real g = hg_g(lobe);
  Real d;
  if (g >= Real(0)) {
    const Real3 h = wo + wi;
    d = (Real(1) - g) * (Real(1) - g) + g * dot(h, h);
  } else {
    const Real3 h = wo - wi;
    d = (Real(1) + g) * (Real(1) + g) - g * dot(h, h);
  }
  return kPhaseInv4Pi * (Real(1) - g * g) / (d * ar::sqrt(d));
}

AR_FN Real phase_pdf(const PhaseLobe& lobe, Real3 wo, Real3 wi) { return phase_eval(lobe, wo, wi); }

AR_FN PhaseSample phase_sample(const PhaseLobe& lobe, Real3 wo, Real2 u) {
  const Real g = hg_g(lobe);
  const Real t = Real(1) + g * (Real(2) * u.x - Real(1));
  const Real inv_t2 = Real(1) / (t * t);
  const Real one_minus_mu = (Real(1) - g) * (Real(1) - g) * (Real(1) - u.x) * (Real(1) + g + t) * inv_t2;
  const Real one_plus_mu = (Real(1) + g) * (Real(1) + g) * u.x * (Real(1) - g + t) * inv_t2;
  // Take mu from whichever side is nearer, so the rounding of 1 - mu or 1 + mu stays relative.
  const Real mu = one_minus_mu < one_plus_mu ? Real(1) - one_minus_mu : one_plus_mu - Real(1);
  const Real sin_theta = ar::sqrt(one_minus_mu * one_plus_mu);
  const Real phi_turns = Real(2) * u.y;
  const Frame f = frame_from_normal(-wo);
  PhaseSample s;
  s.wi = to_world(f, Real3{sin_theta * ar::cospi(phi_turns), sin_theta * ar::sinpi(phi_turns), mu});
  const Real one_minus_g2 = (Real(1) - g) * (Real(1) + g);
  s.pdf = kPhaseInv4Pi * t * t * t / (one_minus_g2 * one_minus_g2);
  s.p = s.pdf;
  return s;
}

AR_NS_END
