// SPDX-License-Identifier: Apache-2.0
// ar-f64: compiled twice into this executable, once with AR_REAL_DOUBLE (cmake/sources.cmake).
// Planck's law and the Blackbody Y normalisation. Ref: 0003 Verification checks 10 and 11
// (Blackbody part); 0003 d25, d11.
// Pinned values come from numpy (float64) with the exact SI constants of the 2019
// redefinition, which CODATA 2018 adopts: c1L = 2 h c^2 = 1.1910429723971884e8
// W um^4 m^-2 sr^-1 and c2 = h c / k_B = 14387.768775039336 um K (h = 6.62607015e-34,
// c = 299792458, k_B = 1.380649e-23), B = c1L l^-5 / expm1(c2 / (l T)) * 1e-3 per nm. The
// kernel uses the rounded constants the record states, so pins agree to ~1e-9 relative.
#include <doctest.h>

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../src/host/data_resource.h"
#include "host/spectral/blackbody.h"
#include "spectral/planck.h"

namespace {

using ar::Real;

constexpr double kEps = std::numeric_limits<Real>::epsilon();

std::vector<uint8_t> read_data(const std::string& rel) {
  std::string here = __FILE__;
  std::string dir = here.substr(0, here.find_last_of("/\\") + 1);
  std::ifstream f(dir + "../../data/" + rel, std::ios::binary);
  if (!f) f.open("data/" + rel, std::ios::binary);
  REQUIRE_MESSAGE(f.good(), "data/" << rel << " not found");
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

astroray::ObserverTables load_observer() {
  auto b = read_data("spectral/observer.bin");
  astroray::ObserverTables t;
  const std::string err = astroray::parse_observer(b.data(), b.size(), &t);
  REQUIRE_MESSAGE(err.empty(), err);
  return t;
}

// The test's own Planck in double, in a different evaluation order from the kernel's: the
// e^x form with pow, valid while expm1(x) does not overflow (x < 709). The record's constants.
// Ref: 0003 d25.
double reference_planck(double lambda_nm, double T) {
  const double c1L = 1.191042972e8, c2 = 14387.7688;
  const double l = lambda_nm * 1e-3;
  const double x = c2 / (l * T);
  return c1L * std::pow(l, -5.0) / std::expm1(x) * 1e-3;
}

// Wien's displacement law: the maximum over lambda of B is at lambda = b / T.
// Ref: CODATA 2018 b = 2.897771955e-3 m K.
double peak_planck(double T) { return reference_planck(2897771.955 / T, T); }

}  // namespace

TEST_SUITE("fast") {
  AR_RT_CASE("planck/pinned radiances") {
    // lambda nm, T K, B W m^-2 sr^-1 nm^-1 (numpy model above), x = c2 / (lambda T).
    const struct { double l, T, b; } pins[] = {
        {550.0, 6500.0, 43063.119249405696},     // x = 4.02
        {400.0, 20000.0, 2307607.9304326274},    // x = 1.80
        {10000.0, 5778.0, 4.212290760805613},    // x = 0.249
        {200.0, 1e5, 353407702.7847071},         // x = 0.719
        {1e6, 3000.0, 2.4774984893328876e-08},   // x = 0.0048, expm1 regime
        {1000.0, 300.0, 1.7680673783294294e-16}  // x = 48
    };
    for (const auto& p : pins) {
      const double x = 14387.768775 / (p.l * 1e-3 * p.T);
      // Float: the record's (2x + 8) 2^-23. Double: the record's rounded constants differ from the
      // exact ones by 3.3e-10 relative in c1L and 1.74e-9 in c2; d ln B / d ln c2 = -x / (1 - e^-x),
      // so 3.4e-10 + 1.75e-9 x / (1 - e^-x), with a factor 1.5 as a safety margin.
      const double tol = sizeof(Real) == 4 ? (2 * x + 8) * kEps : 1.5 * (3.4e-10 + 1.75e-9 * x / -std::expm1(-x));
      const double got = static_cast<double>(ar::planck_radiance(static_cast<Real>(p.l), static_cast<Real>(p.T)));
      CHECK_MESSAGE(std::fabs(got / p.b - 1.0) <= tol, "lambda " << p.l << " T " << p.T << " got " << got << " want " << p.b);
    }
  }

  // Check 10: the float kernel against double over lambda in [0.1 nm, 1 m] and T in [3 K, 1e9 K].
  // x is the condition number of e^-x, hence the (2x + 8) 2^-23 bound where B >= 1e-30 of
  // the peak at that T. In the double copy the same formula uses 2^-52. A reference value below
  // the smallest normal Real is skipped: a subnormal carries fewer than 24 significand bits, so
  // no bound in units of 2^-23 applies to it (at T = 3 K the 1e-30 of the peak is below 1e-38).
  AR_RT_CASE("planck/float vs double") {
    const int n = 640;  // 80 points per decade in both
    double worst = 0.0, worst_l = 0.0, worst_T = 0.0, worst_ratio = 0.0;
    long compared = 0, bad = 0, nonfinite = 0, subnormal = 0;
    for (int a = 0; a < n; ++a) {
      const Real lambda = static_cast<Real>(std::pow(10.0, -1.0 + 10.0 * a / (n - 1)));  // 0.1 nm .. 1e9 nm
      for (int b = 0; b < n; ++b) {
        const Real T = static_cast<Real>(3.0 * std::pow(10.0, std::log10(1e9 / 3.0) * b / (n - 1)));  // 3 K .. 1e9 K
        const double l = lambda, t = T;
        const Real got = ar::planck_radiance(lambda, T);
        if (!(std::isfinite(got) && got >= Real(0))) {
          if (nonfinite++ < 5) std::printf("planck: non-finite %g at lambda %g nm, T %g K\n", double(got), l, t);
          continue;
        }
        const double want = reference_planck(l, t);
        if (!(want >= 1e-30 * peak_planck(t))) continue;
        if (want < static_cast<double>(std::numeric_limits<Real>::min())) {
          ++subnormal;
          continue;
        }
        const double x = 14387.7688 / (l * 1e-3 * t);
        const double tol = (2.0 * x + 8.0) * kEps;
        const double rel = std::fabs(static_cast<double>(got) / want - 1.0);
        ++compared;
        if (rel > tol) ++bad;
        if (rel / tol > worst_ratio) {
          worst_ratio = rel / tol;
          worst = rel;
          worst_l = l;
          worst_T = t;
        }
      }
    }
    std::printf("planck float vs double%s: %ld points compared (%ld subnormal skipped), worst relative error %.3e at lambda %.6g nm, T %.6g K (%.3f of its bound)\n",
                AR_RT_SUFFIX, compared, subnormal, worst, worst_l, worst_T, worst_ratio);
    CHECK(nonfinite == 0);
    CHECK(bad == 0);
    CHECK(compared > 100000);
  }

  // The 6500 K Planckian chromaticity of Wyszecki and Stiles 1982, Table 1(3.11).
  AR_RT_CASE("planck/6500 K chromaticity") {
    const astroray::ObserverTables o = load_observer();
    double xyz[3] = {0, 0, 0};
    for (uint32_t i = 0; i < astroray::kObserverSamples; ++i) {
      const double b = static_cast<double>(ar::planck_radiance(Real(360 + i), Real(6500)));
      for (int k = 0; k < 3; ++k) xyz[k] += b * static_cast<double>(o.cmf[k][i]);
    }
    const double s = xyz[0] + xyz[1] + xyz[2];
    CHECK(std::fabs(xyz[0] / s - 0.3135) < 1e-3);
    CHECK(std::fabs(xyz[1] / s - 0.3237) < 1e-3);
  }

  // Check 11, Blackbody part: Y of the normalised spectrum is 1, and the scale is the one
  // numpy finds from the same table (sum over the 471 samples of B ybar / Y_n).
  AR_RT_CASE("planck/blackbody Y normalisation") {
    const astroray::ObserverTables o = load_observer();
    const struct { double T, scale; } pins[] = {
        {1000.0, 27161.98751499474}, {6500.0, 2.3753562901515304e-05}, {20000.0, 1.1668737541359705e-06}};
    for (const auto& p : pins) {
      const double scale = astroray::host::blackbody_y_normalised(p.T);
      // The rounded constants move B by about 1.75e-9 x relative, x up to 40 at 360 nm and 1000 K.
      CHECK(std::fabs(scale / p.scale - 1.0) < 1e-7);
      double y = 0.0;
      for (uint32_t i = 0; i < astroray::kObserverSamples; ++i)
        y += scale * static_cast<double>(ar::planck_radiance(Real(360 + i), static_cast<Real>(p.T))) * static_cast<double>(o.cmf[1][i]);
      y /= o.y_n;
      CHECK_MESSAGE(std::fabs(y - 1.0) < 1e-4, "T " << p.T << " Y " << y);
    }
    CHECK_THROWS_AS(astroray::host::blackbody_y_normalised(0.0), std::invalid_argument);
    CHECK_THROWS_AS(astroray::host::blackbody_y_normalised(3.0), std::invalid_argument);  // no visible radiance
  }
}
