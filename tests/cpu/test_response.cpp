// SPDX-License-Identifier: Apache-2.0
// ar-f64: compiled twice into this executable, once with AR_REAL_DOUBLE (cmake/sources.cmake).
// Response channels and the display channel (0008 d7, V4; 0003 d11, d13). Expected values are
// independent of the code under test: every literal below comes from a numpy float64 model that
// reads data/spectral/observer.bin (xbar, ybar, zbar, D65 with Y = 1, Y_n, M_ws), interpolates
// with np.interp and applies M_ws @ XYZ / Y_n; photon pins use 1e-9 / (h c) with the exact SI
// h = 6.62607015e-34 J s and c = 299792458 m/s. The conversion accumulates in double, so the
// float and double copies of each case use the same pins and tolerances.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../src/host/data_resource.h"
#include "../gpu/agree.h"
#include "film/response.h"
#include "host/film/response_table.h"

namespace host = astroray::host;

namespace {

using ar::Lambda;
using ar::Real;
using ar::Spec;

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

Spec spec4(double a, double b, double c, double d) {
  Spec s;
  s.v[0] = Real(a);
  s.v[1] = Real(b);
  s.v[2] = Real(c);
  s.v[3] = Real(d);
  return s;
}

Lambda lambda4(double a, double b, double c, double d) {
  Lambda l;
  l.nm[0] = Real(a);
  l.nm[1] = Real(b);
  l.nm[2] = Real(c);
  l.nm[3] = Real(d);
  return l;
}

// Deterministic quadrature over 360..830 nm at 1 nm: weight 1 nm per point, four points per
// call; the last call is padded with a weight-0 lane. Calls `f(L, lf, w)` once per group,
// with L(lambda) from `source`.
template <class Source, class F>
void quadrature_360_830(const Source& source, F f) {
  const int n = 471;
  for (int g = 0; g * 4 < n; ++g) {
    Lambda lf;
    Spec L, w;
    for (int i = 0; i < 4; ++i) {
      const int p = g * 4 + i;
      const bool real = p < n;
      lf.nm[i] = Real(real ? 360 + p : 830);
      L.v[i] = real ? Real(source(360 + p)) : Real(0);
      w.v[i] = real ? Real(1) : Real(0);
    }
    f(L, lf, w);
  }
}

}  // namespace

TEST_SUITE("fast") {
  AR_RT_CASE("response/table layout and pins") {
    const astroray::ObserverTables obs = load_observer();
    const host::ResponseTable t = host::make_response_table(obs);
    static_assert(static_cast<uint32_t>(host::ResponseUnit::Energy) == 0u, "ENERGY is 0");
    static_assert(static_cast<uint32_t>(host::ResponseUnit::Photon) == 1u, "PHOTON is 1");
    static_assert(ar::RESPONSE_ENERGY == 0u && ar::RESPONSE_PHOTON == 1u, "kernel unit values");
    REQUIRE(t.channels.size() == 3u);
    CHECK(t.display_first == 0u);
    const uint32_t first[3] = {0, 471, 942};
    for (int k = 0; k < 3; ++k) {
      CHECK(t.channels[k].first == first[k]);
      CHECK(t.channels[k].count == 471u);
      CHECK(t.channels[k].unit == ar::RESPONSE_ENERGY);
    }
    REQUIRE(t.knot_nm.size() == 3u * 471u);
    REQUIRE(t.knot_value.size() == 3u * 471u);
    for (int k = 0; k < 3; ++k)
      for (int i = 0; i < 471; ++i) {
        REQUIRE(t.knot_nm[471 * k + i] == float(360 + i));
        REQUIRE(t.knot_value[471 * k + i] == obs.cmf[k][i]);
      }
    CHECK(t.y_w == obs.y_n);
    CHECK(std::fabs(t.y_w / 106.85691710117192 - 1.0) < 1e-12);  // Y_n, numpy sum of ybar
    // M_ws of lin_rec709_scene, numpy read of observer.bin.
    const double m_ws[9] = {3.2409699419045226,  -1.537383177570094,   -0.49861076029300344,
                            -0.9692436362808796, 1.8759675015077204,   0.0415550574071756,
                            0.05563007969699363, -0.2039769588889765,  1.0569715142428784};
    for (int i = 0; i < 9; ++i) CHECK(std::fabs(t.m_ws[i] - m_ws[i]) < 1e-12);
    // 1e-9 / (h c), numpy.
    CHECK(std::fabs(t.photon_scale / 5034116567542710.0 - 1.0) < 1e-12);

    const ar::ResponseTableView v = host::response_view(t);
    CHECK(v.channel_count == 3u);
    CHECK(v.display_first == 0u);
    CHECK(v.y_w == t.y_w);
    CHECK(v.photon_scale == t.photon_scale);
    for (int i = 0; i < 9; ++i) CHECK(v.m_ws[i] == t.m_ws[i]);
  }

  AR_RT_CASE("response/CMFs integrate to Y_n") {
    const astroray::ObserverTables obs = load_observer();
    const host::ResponseTable t = host::make_response_table(obs);
    const ar::ResponseTableView v = host::response_view(t);
    double sum[3] = {0, 0, 0};
    quadrature_360_830([](int) { return 1.0; }, [&](const Spec& L, const Lambda& lf, const Spec& w) {
      for (int k = 0; k < 3; ++k) sum[k] += ar::convert(v, L, lf, w, uint32_t(k));
    });
    // numpy sums of the float tables xbar, ybar, zbar, and Y_n = 106.85691710117192 for ybar.
    const double expect[3] = {106.8654692257204, 106.85691728466267, 106.89225084637235};
    for (int k = 0; k < 3; ++k) CHECK_MESSAGE(std::fabs(sum[k] / expect[k] - 1.0) < 1e-9, "channel " << k << " = " << sum[k]);
    CHECK(std::fabs(sum[1] / obs.y_n - 1.0) < 1e-6);
  }

  // 0008 V4 asks for 1e-4. The shipped M_ws is built from the D65 chromaticity (0.3127, 0.3290),
  // while the SPD's own white is XYZ = (0.9504707, 1.0000000, 1.0888285), so the data itself gives
  // (1.00016, 0.99998, 0.99976); the bound here is 0003 V1's 5e-4 (v1 found 3e-4 at 2 degrees),
  // the one test_observer.cpp already applies, and the exact values are pinned from numpy.
  AR_RT_CASE("response/white reflector is 1 1 1") {
    const astroray::ObserverTables obs = load_observer();
    const host::ResponseTable t = host::make_response_table(obs);
    const ar::ResponseTableView v = host::response_view(t);
    double rgb[3] = {0, 0, 0};
    quadrature_360_830([&](int nm) { return double(obs.d65[nm - 360]); },
                       [&](const Spec& L, const Lambda& lf, const Spec& w) {
                         const ar::DisplayRgb d = ar::display_rgb(v, L, lf, w, 1.0);
                         for (int k = 0; k < 3; ++k) rgb[k] += d.v[k];
                       });
    const double pin[3] = {1.0001622032294102, 0.9999761549913413, 0.9997584588521734};
    for (int k = 0; k < 3; ++k) {
      CHECK_MESSAGE(std::fabs(rgb[k] - 1.0) < 5e-4, "channel " << k << " = " << rgb[k]);
      CHECK_MESSAGE(std::fabs(rgb[k] - pin[k]) < 1e-9, "channel " << k << " = " << rgb[k]);
    }
  }

  AR_RT_CASE("response/display of four lanes and film_exposure") {
    const host::ResponseTable t = host::make_response_table(load_observer());
    const ar::ResponseTableView v = host::response_view(t);
    // lambda 450.5 and 600.25 sit between table knots (linear interpolation); numpy XYZ of the
    // weighted sum is (1.13232127064839, 1.3256762619130313, 0.4486912333959481).
    const Lambda lf = lambda4(450.5, 555.0, 600.25, 700.0);
    const Spec L = spec4(1.0, 2.0, 0.5, 4.0);
    const Spec w = spec4(0.25, 0.5, 1.0, 0.125);
    const double xyz[3] = {1.13232127064839, 1.3256762619130313, 0.4486912333959481};
    for (int k = 0; k < 3; ++k) {
      const double got = ar::convert(v, L, lf, w, uint32_t(k));
      CHECK_MESSAGE(std::fabs(got / xyz[k] - 1.0) < 1e-9, "channel " << k << " = " << got);
    }
    // film_exposure = 2: 2 * M_ws @ XYZ / Y_n.
    const double rgb2[3] = {0.026353456190917314, 0.02635441536641664, 0.004994296481337251};
    const ar::DisplayRgb d2 = ar::display_rgb(v, L, lf, w, 2.0);
    const ar::DisplayRgb d1 = ar::display_rgb(v, L, lf, w, 1.0);
    for (int k = 0; k < 3; ++k) {
      CHECK_MESSAGE(std::fabs(d2.v[k] / rgb2[k] - 1.0) < 1e-9, "rgb " << k << " = " << d2.v[k]);
      CHECK(std::fabs(d2.v[k] - 2.0 * d1.v[k]) < 1e-15);
    }
  }

  AR_RT_CASE("response/constant 1 energy channel returns the weight sum") {
    host::ResponseTable t = host::make_response_table(load_observer());
    host::ResponseCurve c;
    c.nm = {300.0f, 900.0f};
    c.value = {1.0f, 1.0f};
    const uint32_t ch = host::add_response_channel(t, c);
    CHECK(ch == 3u);
    CHECK(t.channels[3].first == 3u * 471u);
    CHECK(t.channels[3].count == 2u);
    CHECK(t.channels[3].unit == ar::RESPONSE_ENERGY);
    const ar::ResponseTableView v = host::response_view(t);
    // The lane at 250 nm is outside the curve and contributes nothing: 0.5 + 0.25 + 0.125.
    CHECK(ar::convert(v, spec4(1, 1, 1, 1), lambda4(320.5, 555.25, 899.75, 250.0), spec4(0.5, 0.25, 0.125, 0.0625), ch) == 0.875);
    // Quadrature at 1 nm over 400..799 with weight 0.5: 400 points, sum 200 exactly.
    double sum = 0;
    for (int g = 0; g < 100; ++g)
      sum += ar::convert(v, spec4(1, 1, 1, 1), lambda4(400 + 4 * g, 401 + 4 * g, 402 + 4 * g, 403 + 4 * g), spec4(0.5, 0.5, 0.5, 0.5), ch);
    CHECK(sum == 200.0);
    // A lane at NaN contributes nothing, not NaN.
    CHECK(ar::convert(v, spec4(1, 1, 1, 1), lambda4(500.0, std::numeric_limits<double>::quiet_NaN(), 600.0, 700.0), spec4(1, 1, 1, 1), ch) == 3.0);
  }

  AR_RT_CASE("response/piecewise linear curve and its ends") {
    host::ResponseTable t = host::make_response_table(load_observer());
    host::ResponseCurve c;
    c.nm = {400.0f, 500.0f, 700.0f};
    c.value = {0.0f, 1.0f, 0.5f};
    const uint32_t ch = host::add_response_channel(t, c);
    const ar::ResponseTableView v = host::response_view(t);
    const Spec one = spec4(1, 1, 1, 1);
    // By hand: 450 is halfway 0 -> 1, 600 is halfway 1 -> 0.5; 399 and 701 lie outside.
    CHECK(ar::convert(v, one, lambda4(450.0, 600.0, 399.0, 701.0), spec4(1, 1, 1, 1), ch) == 0.5 + 0.75);
    // Knots are hit exactly, both ends included: 0 + 0.5 + 1 + 0.2 (420 nm: 0.2).
    CHECK(ar::convert(v, one, lambda4(400.0, 700.0, 500.0, 420.0), spec4(1, 1, 1, 1), ch) == doctest::Approx(1.7).epsilon(1e-12));
  }

  AR_RT_CASE("response/photon unit scales by lambda over h c") {
    host::ResponseTable t = host::make_response_table(load_observer());
    host::ResponseCurve c;
    c.nm = {300.0f, 900.0f};
    c.value = {1.0f, 1.0f};
    c.unit = host::ResponseUnit::Photon;
    const uint32_t photon = host::add_response_channel(t, c);
    c.unit = host::ResponseUnit::Energy;
    const uint32_t energy = host::add_response_channel(t, c);
    CHECK(t.channels[photon].unit == ar::RESPONSE_PHOTON);
    const ar::ResponseTableView v = host::response_view(t);
    // One lane at 555 nm, L = 1, w = 1: 555e-9 / (h c) = 2.7939346949862036e18 photons per joule
    // (numpy, exact SI h and c).
    const double got = ar::convert(v, spec4(1, 0, 0, 0), lambda4(555.0, 0, 0, 0), spec4(1, 0, 0, 0), photon);
    CHECK_MESSAGE(std::fabs(got / 2.7939346949862036e18 - 1.0) < 1e-12, "photon = " << got);
    CHECK(ar::convert(v, spec4(1, 0, 0, 0), lambda4(555.0, 0, 0, 0), spec4(1, 0, 0, 0), energy) == 1.0);
    // The ratio is lambda per lane: 450 nm gives 450e-9 / (h c) = 450/555 of the value above.
    const double at450 = ar::convert(v, spec4(1, 0, 0, 0), lambda4(450.0, 0, 0, 0), spec4(1, 0, 0, 0), photon);
    CHECK(std::fabs(at450 / got - 450.0 / 555.0) < 1e-12);
  }

  AR_RT_CASE("response/channel requests that cannot be honoured are refused") {
    host::ResponseTable t = host::make_response_table(load_observer());
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    auto refused = [&](std::vector<float> nm, std::vector<float> val) {
      host::ResponseCurve c;
      c.nm = nm;
      c.value = val;
      try {
        host::add_response_channel(t, c);
      } catch (const std::invalid_argument&) {
        return true;
      }
      return false;
    };
    CHECK(refused({500.0f}, {1.0f}));                             // one knot
    CHECK(refused({}, {}));                                       // none
    CHECK(refused({400.0f, 500.0f}, {1.0f}));                     // sizes differ
    CHECK(refused({500.0f, 400.0f}, {1.0f, 1.0f}));               // descending
    CHECK(refused({400.0f, 400.0f}, {1.0f, 1.0f}));               // repeated nm
    CHECK(refused({400.0f, nan}, {1.0f, 1.0f}));                  // NaN nm
    CHECK(refused({400.0f, inf}, {1.0f, 1.0f}));                  // Inf nm
    CHECK(refused({400.0f, 500.0f}, {1.0f, -0.001f}));            // negative response
    CHECK(refused({400.0f, 500.0f}, {nan, 1.0f}));                // NaN response
    CHECK(refused({400.0f, 500.0f}, {1.0f, inf}));                // Inf response
    host::ResponseCurve bad;
    bad.nm = {400.0f, 500.0f};
    bad.value = {1.0f, 1.0f};
    bad.unit = static_cast<host::ResponseUnit>(2);
    CHECK_THROWS_AS(host::add_response_channel(t, bad), std::invalid_argument);
    CHECK(t.channels.size() == 3u);  // nothing was added by a refused request
    CHECK(t.knot_nm.size() == 3u * 471u);
  }

  AR_RT_CASE("response/float conversion against double") {
    // The float instantiation a shade kernel calls (the escaped path's albedo guide, 0008 d15)
    // against the resolve's double one: the real observer tables, wavelengths over the display
    // range, radiances over six decades, film weights over a 40:1 range and exposures over 2^+-4. The bound is
    // 0002 D12's float-against-double evaluation threshold (tests/gpu/agree.h PROD_VS_CPU_F64,
    // eval_p999), applied here to the maximum over every input as agree.h's REL_L2 metric.
    // The bitwise double-to-float read the float path uses for the double table values rounds as
    // a float conversion does, normals, subnormals, ties and specials included.
    const host::ResponseTable t = host::make_response_table(load_observer());
    const ar::ResponseTableView v = host::response_view(t);
    std::mt19937_64 rng(0x174u);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    double worst = 0.0;
    for (int n = 0; n < 20000; ++n) {
      Lambda lf;
      Spec L, w;
      for (int i = 0; i < ar::kLanes; ++i) {
        lf.nm[i] = Real(360.0 + 470.0 * uni(rng));
        L.v[i] = Real(std::pow(10.0, -3.0 + 6.0 * uni(rng)));
        w.v[i] = Real(0.05 + 2.0 * uni(rng));
      }
      const double exposure = std::pow(2.0, -4.0 + 8.0 * uni(rng));
      const ar::DisplayRgb d = ar::display_rgb(v, L, lf, w, exposure);
      const ar::DisplayRgbOf<float> f = ar::display_rgb<float>(v, L, lf, w, exposure);
      double num = 0.0, den = 0.0;
      for (int k = 0; k < 3; ++k) num += (double(f.v[k]) - d.v[k]) * (double(f.v[k]) - d.v[k]), den += d.v[k] * d.v[k];
      worst = std::max(worst, std::sqrt(num / den));
    }
    MESSAGE("float display conversion: max relative L2 error " << worst);
    CHECK(worst <= astroray::agree::PROD_VS_CPU_F64.eval_p999);
    const double values[] = {0.0, -0.0, 1.0, -2.5, 3.0e38, 3.5e38, -1.0e300, 1.0e-300, 1.17549435e-38, 1.0e-40,
                             1.401298464324817e-45, 7.006492321624085e-46, 1.0 + std::ldexp(1.0, -24),
                             1.0 + 3.0 * std::ldexp(1.0, -24), 5.0e15, 0.1, -0.7, 655.0, 1.0e-39,
                             std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
    for (double x : values) CHECK_MESSAGE(ar::f64_bits_to_f32(x) == float(x), "value " << x);
    CHECK(std::isnan(ar::f64_bits_to_f32(std::numeric_limits<double>::quiet_NaN())));
    for (int n = 0; n < 100000; ++n) {
      const double x = (uni(rng) - 0.5) * std::pow(10.0, -50.0 + 90.0 * uni(rng));
      REQUIRE_MESSAGE(ar::f64_bits_to_f32(x) == float(x), "value " << x);
    }
  }
}
