// SPDX-License-Identifier: Apache-2.0
// ar-f64: compiled twice into this executable, once with AR_REAL_DOUBLE (cmake/sources.cmake).
// SpectrumRef kinds BLACKBODY and CAUCHY_ABBE: the pure functions resolve() calls, the Abbe number
// the Cauchy law reproduces, Planck x scale, and the display Y of a stored Blackbody emission.
// Ref: docs/decisions/0005-material-model.md items 9, 10 and Verification 3; 0003 d11, d25;
// OpenPBR 1.1.2 dispersion section (Abbe number, Cauchy law).
#include <doctest.h>

#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "../../src/host/data_resource.h"
#include "host/spectral/blackbody.h"
#include "material/slot_resolve.h"
#include "spectral/planck.h"

namespace {

using ar::Real;

constexpr double kEps = std::numeric_limits<Real>::epsilon();

astroray::ObserverTables load_observer() {
  std::string here = __FILE__;
  std::string dir = here.substr(0, here.find_last_of("/\\") + 1);
  std::ifstream f(dir + "../../data/spectral/observer.bin", std::ios::binary);
  if (!f) f.open("data/spectral/observer.bin", std::ios::binary);
  REQUIRE_MESSAGE(f.good(), "data/spectral/observer.bin not found");
  const std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  astroray::ObserverTables t;
  const std::string err = astroray::parse_observer(b.data(), b.size(), &t);
  REQUIRE_MESSAGE(err.empty(), err);
  return t;
}

ar::SpectrumRef blackbody_ref(double T, double scale) {
  ar::SpectrumRef r = {};
  r.kind = uint8_t(ar::SpectrumKind::BLACKBODY);
  r.semantic = uint8_t(ar::SpectrumSemantic::ILLUMINANT);
  r.p[0] = float(T);
  r.p[1] = float(scale * astroray::host::blackbody_y_normalised(T));
  return r;
}

ar::SpectrumRef cauchy_ref(float nd, float vd, float scale) {
  ar::SpectrumRef r = {};
  r.kind = uint8_t(ar::SpectrumKind::CAUCHY_ABBE);
  r.semantic = uint8_t(ar::SpectrumSemantic::IOR);
  r.p[0] = nd, r.p[1] = vd, r.p[2] = scale;
  return r;
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("spectrum_ref/cauchy reproduces the abbe number") {
  const double nd = 1.5168, vd = 64.17;  // BK7
  for (double s : {1.0, 0.5, 0.25}) {
    // The double instantiation is the same template the kernel compiles for Real. n_F - n_C
    // cancels five digits of n, so a float evaluation of the Abbe number cannot reach 1e-6
    // (its n values carry 6e-8 relative error): the Abbe number is checked in double, and the
    // build's own Real instantiation is checked against it value by value.
    const double n_d = ar::cauchy_abbe_ior_t<double>(nd, vd, s, ar::kLambdaD);
    const double n_f = ar::cauchy_abbe_ior_t<double>(nd, vd, s, ar::kLambdaF);
    const double n_c = ar::cauchy_abbe_ior_t<double>(nd, vd, s, ar::kLambdaC);
    CHECK(n_d == nd);  // the d line returns n_d exactly
    const double V = (n_d - 1.0) / (n_f - n_c);
    INFO("dispersion scale " << s);
    CHECK(std::fabs(V - vd / s) <= 1e-12 * vd / s);  // OpenPBR: V_d_medium = V_d / scale
    for (double lam : {360.0, 450.0, 486.1, 587.6, 656.3, 700.0, 830.0}) {
      const double want = ar::cauchy_abbe_ior_t<double>(nd, vd, s, lam);
      const double got = double(ar::cauchy_abbe_ior_t<Real>(Real(nd), Real(vd), Real(s), Real(lam)));
      CHECK(std::fabs(got - want) <= 1e-6 * want);
    }
    // The Real instantiation returns n_d at the d line to the last bit.
    CHECK(ar::cauchy_abbe_ior_t<Real>(Real(nd), Real(vd), Real(s), Real(ar::kLambdaD)) == Real(nd));
  }
  // Scale 0 is flat n_d at every wavelength; shorter wavelengths bend more (normal dispersion).
  for (double lam : {360.0, 500.0, 587.6, 830.0}) CHECK(ar::cauchy_abbe_ior_t<double>(nd, vd, 0.0, lam) == nd);
  CHECK(ar::cauchy_abbe_ior_t<double>(nd, vd, 1.0, 450.0) > ar::cauchy_abbe_ior_t<double>(nd, vd, 1.0, 650.0));
  // lambda is clamped to [360, 830] (0003 d8).
  CHECK(ar::cauchy_abbe_ior_t<double>(nd, vd, 1.0, 100.0) == ar::cauchy_abbe_ior_t<double>(nd, vd, 1.0, 360.0));
  CHECK(ar::cauchy_abbe_ior_t<double>(nd, vd, 1.0, 2000.0) == ar::cauchy_abbe_ior_t<double>(nd, vd, 1.0, 830.0));
  // resolve() of the ref is the same function, per lane, with no illuminant factor.
  const astroray::ObserverTables ot = load_observer();
  ar::ObserverView obs;
  obs.cmf = &ot.cmf[0][0];
  obs.illum = ot.d65;
  const ar::Lambda lam{{Real(400), Real(500), Real(600), Real(700)}};
  const ar::Spec got = ar::resolve(cauchy_ref(1.5168f, 64.17f, 1.0f), lam, obs);
  const ar::Spec want = ar::cauchy_abbe_ior(Real(1.5168f), Real(64.17f), Real(1.0f), lam);
  for (int i = 0; i < ar::kLanes; ++i) CHECK(got.v[i] == want.v[i]);
}

TEST_CASE("spectrum_ref/blackbody planck times scale") {
  const astroray::ObserverTables ot = load_observer();
  ar::ObserverView obs;
  obs.cmf = &ot.cmf[0][0];
  obs.illum = ot.d65;
  const ar::Lambda lam{{Real(420), Real(550), Real(650), Real(780)}};
  for (double T : {2700.0, 6500.0, 12000.0}) {
    for (double scale : {1.0, 0.375}) {  // exact in float
      const ar::Spec direct = ar::blackbody_spec(Real(T), Real(scale), lam);
      // resolve() of an ILLUMINANT BLACKBODY ref is analytic: the illuminant multiply is skipped.
      ar::SpectrumRef r = blackbody_ref(T, 1.0);
      r.p[1] = float(scale);
      const ar::Spec resolved = ar::resolve(r, lam, obs);
      for (int i = 0; i < ar::kLanes; ++i) {
        const double want = scale * ar::planck_radiance_t<double>(double(lam.nm[i]), T);
        INFO("T " << T << " scale " << scale << " lane " << i);
        CHECK(std::fabs(double(direct.v[i]) - want) <= 64.0 * kEps * want);
        CHECK(double(resolved.v[i]) == doctest::Approx(want).epsilon(64.0 * kEps));
      }
    }
  }
}

TEST_CASE("spectrum_ref/blackbody display y") {
  // 0003 d11: Y(scale x Planck(T) / Y(Planck(T))) = scale, summed over the observer table.
  const astroray::ObserverTables ot = load_observer();
  ar::ObserverView obs;
  obs.cmf = &ot.cmf[0][0];
  obs.illum = ot.d65;
  for (double T : {2700.0, 6500.0, 12000.0}) {
    const ar::SpectrumRef r = blackbody_ref(T, 1.0);
    double y = 0.0;
    for (uint32_t i = 0; i < astroray::kObserverSamples; i += ar::kLanes) {
      ar::Lambda lam;
      for (int k = 0; k < ar::kLanes; ++k)
        lam.nm[k] = Real(astroray::kObserverLambdaMin + std::min(i + uint32_t(k), astroray::kObserverSamples - 1));
      const ar::Spec s = ar::resolve(r, lam, obs);
      for (int k = 0; k < ar::kLanes && i + uint32_t(k) < astroray::kObserverSamples; ++k)
        y += double(s.v[k]) * double(ot.cmf[1][i + uint32_t(k)]);
    }
    y /= ot.y_n;
    INFO("T " << T);
    CHECK(std::fabs(y - 1.0) <= 1e-3);
  }
}

}  // TEST_SUITE
