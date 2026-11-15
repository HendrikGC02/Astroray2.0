// SPDX-License-Identifier: Apache-2.0
// CIE 1931 2 degree observer and working-space illuminant lookup at one wavelength.
// The tables are those of data/spectral/observer.bin (0003 d14): 471 samples at 1 nm from
// 360 nm. Between samples the lookup is linear; outside 360-830 nm it is 0.
// Ref: docs/decisions/0003-spectral-model-and-path-state.md d14 (one observer), d11 (the
// quantities that use it).
#pragma once
#include <cstdint>

#include "../core/real.h"
#include "../portability.h"

AR_NS_BEGIN

inline constexpr int kObsSamples = 471;
inline constexpr int kObsLambdaMin = 360;
inline constexpr int kObsLambdaMax = 830;

// Views into float arrays the host owns: cmf is xbar[471], ybar[471], zbar[471] one after the
// other; illum is the working space's illuminant SPD with Y = 1.
struct ObserverView {
  const float* cmf;
  const float* illum;
};

// Linear interpolation in a 471-sample table at lambda nm; 0 outside the table and for NaN.
AR_FN Real obs_table_at(const float* t, Real lambda) {
  if (!(lambda >= Real(kObsLambdaMin) && lambda <= Real(kObsLambdaMax))) return Real(0);
  const Real f = lambda - Real(kObsLambdaMin);
  int i = static_cast<int>(ar::floor(f));
  if (i > kObsSamples - 2) i = kObsSamples - 2;
  const Real u = f - Real(i);
  return (Real(1) - u) * Real(ar::load_ro(t + i)) + u * Real(ar::load_ro(t + i + 1));
}

// channel 0, 1, 2 = xbar, ybar, zbar.
AR_FN Real cmf_at(const ObserverView& o, int channel, Real lambda) {
  return obs_table_at(o.cmf + channel * kObsSamples, lambda);
}

AR_FN Real illuminant_at(const ObserverView& o, Real lambda) { return obs_table_at(o.illum, lambda); }

AR_NS_END
