// SPDX-License-Identifier: Apache-2.0
// The one predicate that decides whether a microfacet lobe is a delta lobe.
// Every consumer (lobe sampling, the MAY_BE_DELTA material flag, the caster
// rule of the light pass) calls this function so they cannot disagree.
//
// Ref: docs/decisions/0005-material-model.md item 4; 0004 D19; pbrt-v4
// TrowbridgeReitzDistribution::EffectivelySmooth (max(alpha_x, alpha_y) < 1e-3).
#pragma once
#include "core/real.h"
#include "portability.h"

AR_NS_BEGIN

inline constexpr Real kDeltaAlphaThreshold = Real(1e-3);

// True when both axes are below the threshold.
AR_FN bool lobe_is_delta(Real alpha_x, Real alpha_y) {
  return alpha_x < kDeltaAlphaThreshold && alpha_y < kDeltaAlphaThreshold;
}

AR_NS_END
