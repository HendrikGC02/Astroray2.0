// SPDX-License-Identifier: Apache-2.0
// An analytic world for the bake_environment tests: a smooth positive function of direction and
// wavelength, written once so the CPU test, the GPU agreement case and the host's double
// reference evaluate the same formula (the reference instantiates the template with double).
//   L(x, z, lambda) = (1 + q^2) (1.5 + z) (1 + 0.5 x x),   q = (lambda - 595) / 235
// Its relative error in float, e_s = 8 u with u = 2^-24, counted per operation (every factor is
// positive, so nothing cancels): lambda - 595 is exact (small integers); the division costs u,
// q q 3 u, and 1 + q^2 at most 3 u (the error of q^2 enters scaled by q^2 / (1 + q^2) <= 1/2 for
// |q| <= 1, over the observer's 360 to 830 nm, plus one rounding); 1.5 + z is one rounding of
// exact inputs; 0.5 x is exact, (0.5 x) x one rounding, 1 + that u + u/3 + ... < 2 u; the
// direction factor is then u + 2 u + u = 4 u; the product of the two factors adds u: 3 u + 4 u + u.
// The z term makes it vary over the sphere (a flipped hemisphere is caught); x x separates the
// x axis from y (a transposed cell index is caught).
#pragma once
#include "color/observer.h"
#include "core/spectrum.h"
#include "core/vec.h"
#include "light/equal_area.h"
#include "portability.h"

AR_NS_BEGIN

template <class T>
AR_FN T analytic_radiance(T x, T z, T lambda) {
  const T q = (lambda - T(595)) / T(235);
  const T spectral = T(1) + q * q;
  const T directional = (T(1.5) + z) * (T(1) + T(0.5) * x * x);
  return spectral * directional;
}

struct AnalyticWorld {
  AR_FN Spec operator()(const Real3& d, const Lambda& lam) const {
    Spec s;
    for (int k = 0; k < kLanes; ++k) s.v[k] = analytic_radiance<Real>(d.x, d.z, lam.nm[k]);
    return s;
  }
};

// e_s above, in units of u.
inline constexpr double kAnalyticWorldUnits = 8.0;

#if !defined(__CUDACC__)
// The host's exact evaluation of the cell the kernel computes, for an observer `obs`: the same
// sum in double over the 471 samples, at the direction equal_area_square_to_sphere returns for the
// cell centre (the mapping's own accuracy is test_env_table's), divided by the weight total.
inline double analytic_cell_reference(const ObserverView& obs, uint32_t n, uint32_t cell) {
  const uint32_t iv = cell / n, iu = cell % n;
  const Real3 d = equal_area_square_to_sphere(Real2{(Real(iu) + Real(0.5)) / Real(n), (Real(iv) + Real(0.5)) / Real(n)});
  double num = 0.0, weights = 0.0;
  for (int j = 0; j < kObsSamples; ++j) {
    const double c = double(obs.cmf[j]) + double(obs.cmf[kObsSamples + j]) + double(obs.cmf[2 * kObsSamples + j]);
    num += analytic_radiance<double>(double(d.x), double(d.z), double(kObsLambdaMin + j)) * c;
    weights += c;
  }
  return num / weights;
}
#endif

AR_NS_END
