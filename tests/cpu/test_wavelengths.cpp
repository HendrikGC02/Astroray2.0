// SPDX-License-Identifier: Apache-2.0
// ar-f64: compiled twice into this executable, once with AR_REAL_DOUBLE (cmake/sources.cmake).
// Film wavelengths (0003 d2, d3): the host table, the stratified kernel sampler and the
// display-only wavelength estimator. Ref: 0003 Verification checks 7 (display part) and 8.
// Expected values are independent of the code under test: the literals below come from a
// numpy model that integrates pbrt-v4's VisibleWavelengthsPDF, 0.0039398042 /
// cosh^2(0.0072 (l - 538)), by composite Simpson on 64 sub-panels per bin and mixes in the
// 5% uniform floor; the model never uses the tanh closed form of the C++.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "../../src/host/data_resource.h"
#include "common/stat.h"
#include "color/observer.h"
#include "host/spectral/wavelength_table.h"
#include "spectral/wavelength.h"

namespace st = astroray::stat;
namespace host = astroray::host;

namespace {

using ar::Real;

ar::WavelengthTableView view_of(const host::WavelengthTable& t) {
  return ar::WavelengthTableView{t.edges.data(), t.cdf.data(), t.bins()};
}

// The CDF at lambda, from the float tables in double (independent of the kernel search).
double cdf_at(const host::WavelengthTable& t, double lambda) {
  const uint32_t k = t.bins();
  for (uint32_t b = 0; b < k; ++b) {
    if (lambda <= t.edges[b + 1] || b + 1 == k) {
      const double e0 = t.edges[b], e1 = t.edges[b + 1];
      return t.cdf[b] + (lambda - e0) / (e1 - e0) * (static_cast<double>(t.cdf[b + 1]) - t.cdf[b]);
    }
  }
  return 1.0;
}

double frac(double x) { return x - std::floor(x); }

}  // namespace

TEST_SUITE("fast") {
  AR_RT_CASE("wavelengths/table structure and pins") {
    const host::WavelengthTable t = host::WavelengthTableBuilder().build();
    REQUIRE(t.bins() == 1024u);
    REQUIRE(t.cdf.size() == 1025u);
    CHECK(t.edges[0] == 360.0f);
    CHECK(t.edges[1024] == 830.0f);
    CHECK(t.cdf[0] == 0.0f);
    CHECK(t.cdf[1024] == 1.0f);
    // The 5% floor: every bin's pdf is at least 0.05 / 470 per nm (float rounding of the
    // table is at most 1e-6 relative at this scale).
    double min_pdf = 1e9;
    for (uint32_t i = 0; i < 1024; ++i) {
      REQUIRE(t.edges[i + 1] > t.edges[i]);
      REQUIRE(t.cdf[i + 1] > t.cdf[i]);
      min_pdf = std::fmin(min_pdf, (static_cast<double>(t.cdf[i + 1]) - t.cdf[i]) / (static_cast<double>(t.edges[i + 1]) - t.edges[i]));
    }
    CHECK(min_pdf >= 0.05 / 470.0 * (1.0 - 1e-5));
    // Pinned CDF values at bin edges (numpy model above). Float storage: half an ulp of a
    // value below 1 is 6e-8, so 1e-7 absolute.
    const struct { int i; double edge, cdf; } pins[] = {
        {1, 360.458984375, 0.000506574}, {100, 405.8984375, 0.065503852}, {256, 477.5, 0.244824764},
        {387, 537.626953125, 0.462952618}, {512, 595.0, 0.672570707}, {768, 712.5, 0.924848014},
        {1023, 829.541015625, 0.999851296}};
    for (const auto& p : pins) {
      CHECK(static_cast<double>(t.edges[p.i]) == p.edge);
      CHECK(std::fabs(static_cast<double>(t.cdf[p.i]) - p.cdf) < 1e-7);
    }
    // Pinned bin pdfs (per nm).
    const struct { int i; double pdf; } pdfs[] = {{0, 1.103684859e-03}, {300, 3.554096102e-03}, {512, 3.279288751e-03}, {1023, 3.239853409e-04}};
    for (const auto& p : pdfs) {
      const double got = (static_cast<double>(t.cdf[p.i + 1]) - t.cdf[p.i]) / (static_cast<double>(t.edges[p.i + 1]) - t.edges[p.i]);
      CHECK(std::fabs(got / p.pdf - 1.0) < 1e-4);  // a bin's cdf step is ~1e-3: 6e-8 / 1e-3 = 6e-5
    }
  }

  AR_RT_CASE("wavelengths/lines hook accepts an empty list and refuses a non-empty one") {
    host::WavelengthTableBuilder b;
    CHECK_NOTHROW(b.add_lines({}));
    CHECK_THROWS_AS(b.add_lines({host::LineComponent{589.0, 1.0}}), std::invalid_argument);
    CHECK(b.build().bins() == 1024u);
  }

  // Check 8. On the identity table (one bin, edges and CDF both [0, 1]) the wavelength is the
  // CDF position itself, so frac(u + i/4) is compared with no table error in between.
  AR_RT_CASE("wavelengths/stratified lane positions") {
    const float edges[2] = {0.0f, 1.0f};
    const float cdf[2] = {0.0f, 1.0f};
    const ar::WavelengthTableView id{edges, cdf, 1u};
    std::mt19937_64 rng(0x5eedu);
    // u on the sampler's 2^-24 grid (sobol.h bits_to_unit): random values plus every grid point
    // within 3 steps of 1 - i/4, where u + i/4 crosses 1 and a plain float sum rounds.
    std::vector<double> us;
    for (int n = 0; n < 4096; ++n) us.push_back(static_cast<double>(rng() & 0xFFFFFFu) / 16777216.0);
    for (int i = 1; i < ar::kLanes; ++i)
      for (int d = -3; d <= 3; ++d) us.push_back(1.0 - i / 4.0 + d / 16777216.0);
    us.push_back(0.0);
    us.push_back(1.0 - 1.0 / 16777216.0);
    for (const double u : us) {
      const ar::FilmWavelengths w = ar::sample_film_wavelengths(id, static_cast<Real>(u));
      for (int i = 0; i < ar::kLanes; ++i) {
        CHECK_MESSAGE(static_cast<double>(w.lf.nm[i]) == frac(u + i / 4.0), "u " << u << " lane " << i);
        CHECK(static_cast<double>(w.pf.v[i]) == 1.0);
      }
    }
    // On the real table: the CDF position of every lane is frac(u + i/4) up to the float
    // rounding of lambda (ulp 6.1e-5 nm at 830 nm times the peak pdf 4.5e-3 per nm = 2.8e-7).
    const host::WavelengthTable t = host::WavelengthTableBuilder().build();
    const double tol = sizeof(Real) == 4 ? 1e-6 : 1e-12;
    for (int n = 0; n < 4096; ++n) {
      const double u = static_cast<double>(rng() & 0xFFFFFFu) / 16777216.0;
      const ar::FilmWavelengths w = ar::sample_film_wavelengths(view_of(t), static_cast<Real>(u));
      for (int i = 0; i < ar::kLanes; ++i) {
        CHECK(w.lf.nm[i] >= Real(360));
        CHECK(w.lf.nm[i] <= Real(830));
        CHECK(std::fabs(cdf_at(t, w.lf.nm[i]) - frac(u + i / 4.0)) < tol);
      }
    }
    // Pinned lanes for u = 1/16 (positions 1/16, 5/16, 9/16, 13/16) from the numpy model.
    // lambda: float arithmetic of 5e-5 nm plus the pins' cdf error 1e-7 / pdf >= 1.1e-3 = 1e-4 nm.
    const ar::FilmWavelengths p = ar::sample_film_wavelengths(view_of(t), Real(0.0625));
    const double lam[4] = {404.213354, 497.457272, 563.772549, 645.136488};
    const double pdf[4] = {1.769177499e-03, 3.547668988e-03, 3.725245410e-03, 2.276393172e-03};
    for (int i = 0; i < ar::kLanes; ++i) {
      CHECK(std::fabs(static_cast<double>(p.lf.nm[i]) - lam[i]) < 5e-4);
      CHECK(std::fabs(static_cast<double>(p.pf.v[i]) / pdf[i] - 1.0) < 1e-4);
    }
  }
}

#if !defined(AR_REAL_DOUBLE)
namespace {

std::vector<uint8_t> read_data(const std::string& rel) {
  std::string here = __FILE__;
  std::string dir = here.substr(0, here.find_last_of("/\\") + 1);
  std::ifstream f(dir + "../../data/" + rel, std::ios::binary);
  if (!f) f.open("data/" + rel, std::ios::binary);
  REQUIRE_MESSAGE(f.good(), "data/" << rel << " not found");
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// A smooth, positive spectrum with a full period over the support: nothing about it is
// special to the sech^2 shape, so a wrong pdf shows as bias.
double test_spectrum(double l) { return 1.0 + 0.5 * std::sin(6.283185307179586 * (l - 360.0) / 470.0); }

// ybar(l) * s(l) with ybar linearly interpolated as the kernel does.
double integrand(const float* ybar, double l) { return static_cast<double>(ar::obs_table_at(ybar, static_cast<Real>(l))) * test_spectrum(l); }

}  // namespace

// Check 7, display-only part: the estimator of the integral of ybar * s over the support,
// from 2^20 samples of four stratified lanes, against deterministic quadrature. Statistical
// and identical across builds, so only the f32 copy runs it.
TEST_SUITE("gate") {
  TEST_CASE("wavelengths/display estimator and pdf support") {
    auto bytes = read_data("spectral/observer.bin");
    astroray::ObserverTables obs;
    const std::string err = astroray::parse_observer(bytes.data(), bytes.size(), &obs);
    REQUIRE_MESSAGE(err.empty(), err);
    const float* ybar = obs.cmf[1];
    const host::WavelengthTable t = host::WavelengthTableBuilder().build();
    const ar::WavelengthTableView v = view_of(t);

    // pdf > 0 on the whole support: every bin, and every lane of a dense grid of u.
    for (uint32_t i = 0; i < t.bins(); ++i) REQUIRE(t.cdf[i + 1] > t.cdf[i]);
    for (int j = 0; j < (1 << 16); ++j) {
      const ar::FilmWavelengths w = ar::sample_film_wavelengths(v, static_cast<Real>(j) / static_cast<Real>(1 << 16));
      for (int i = 0; i < ar::kLanes; ++i) REQUIRE(w.pf.v[i] > Real(0));
    }

    // Truth: 5-point Gauss-Legendre on each 1 nm segment of the table (ybar is linear on it,
    // s is smooth).
    // Ref: Abramowitz and Stegun 25.4.30, n = 5 nodes and weights.
    const double xg[5] = {0.0, 0.5384693101056831, -0.5384693101056831, 0.9061798459386640, -0.9061798459386640};
    const double wg[5] = {0.5688888888888889, 0.4786286704993665, 0.4786286704993665, 0.2369268850561891, 0.2369268850561891};
    double truth = 0.0;
    for (int s = 0; s < 470; ++s)
      for (int g = 0; g < 5; ++g) truth += 0.5 * wg[g] * integrand(ybar, 360.0 + s + 0.5 + 0.5 * xg[g]);

    const st::Run run = st::begin("wavelengths/display estimator and pdf support");
    std::mt19937_64 rng(run.seed);
    std::vector<double> samples(1 << 20);
    for (double& s : samples) {
      const Real u = static_cast<Real>(static_cast<double>(rng() >> 11) / 9007199254740992.0);
      const ar::FilmWavelengths w = ar::sample_film_wavelengths(v, u);
      double sum = 0.0;
      for (int i = 0; i < ar::kLanes; ++i) sum += integrand(ybar, w.lf.nm[i]) / static_cast<double>(w.pf.v[i]);
      s = sum / ar::kLanes;
    }
    const st::EnergyResult e = st::energy_test(run, samples, truth);
    std::printf("wavelengths estimator: mean %.9g truth %.9g se %.3g z %.3f\n", e.mean, truth, e.se, e.z);
    INFO(e.message);
    CHECK(e.pass);
  }
}
#endif
