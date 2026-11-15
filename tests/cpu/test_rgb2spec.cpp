// SPDX-License-Identifier: Apache-2.0
// ar-f64: compiled twice into this executable, once with AR_REAL_DOUBLE (cmake/sources.cmake).
// 0003 Verification checks 3 (round trip), 5 (white), 6 (illuminant convention) and the
// conventions of d16, on the committed data/spectral/rec709.jh and observer.bin.
// Ref: Jakob and Hanika 2019, CGF 38(2); pbrt-v4 util/color.h, util/spectrum.h.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../../src/host/data_resource.h"
#include "color/observer.h"
#include "color/rgb2spec.h"
#include "core/packet.h"

namespace {

using ar::Lambda;
using ar::Real;
using ar::Rgb;
using ar::Spec;

static_assert(ar::kObsSamples == int(astroray::kObserverSamples), "kernel and host agree on the observer grid");
static_assert(ar::kObsLambdaMin == int(astroray::kObserverLambdaMin), "kernel and host agree on the observer grid");

std::vector<uint8_t> read_data(const std::string& rel) {
  std::string here = __FILE__;
  std::string dir = here.substr(0, here.find_last_of("/\\") + 1);
  std::ifstream f(dir + "../../data/" + rel, std::ios::binary);
  if (!f) f.open("data/" + rel, std::ios::binary);
  REQUIRE_MESSAGE(f.good(), "data/" << rel << " not found");
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// observer.bin plus rec709.jh, with the views the kernel code takes.
struct Fixture {
  std::vector<uint8_t> jh_bytes;
  astroray::ObserverTables obs;
  ar::Rgb2SpecTable table;
  ar::ObserverView view;
  Fixture() {
    auto ob = read_data("spectral/observer.bin");
    std::string err = astroray::parse_observer(ob.data(), ob.size(), &obs);
    REQUIRE_MESSAGE(err.empty(), err);
    jh_bytes = read_data("spectral/rec709.jh");
    const char* e = ar::jh_view(jh_bytes.data(), jh_bytes.size(), &table);
    REQUIRE_MESSAGE(e == nullptr, (e ? e : ""));
    view.cmf = &obs.cmf[0][0];
    view.illum = obs.d65;
  }
};

// working-space RGB from XYZ (already divided by Y_n).
void to_rgb(const astroray::ObserverTables& o, const double xyz[3], double rgb[3]) {
  for (int r = 0; r < 3; ++r) rgb[r] = o.m_ws[3 * r] * xyz[0] + o.m_ws[3 * r + 1] * xyz[1] + o.m_ws[3 * r + 2] * xyz[2];
}

Lambda lanes(Real a, Real b, Real c, Real d) { return Lambda{{a, b, c, d}}; }

}  // namespace

TEST_SUITE("fast") {
  // Check 3. The reflectance goes through D65 and the CMFs at the 471 samples (the engine's own
  // definition, 0003 d11) back to working-space RGB, in double, independent of the optimiser.
  AR_RT_CASE("rgb2spec/round trip rec709") {
    Fixture fx;
    double worst = 0;
    Rgb worst_rgb{{0, 0, 0}};
    for (int ir = 0; ir < 7; ++ir)
      for (int ig = 0; ig < 7; ++ig)
        for (int ib = 0; ib < 7; ++ib) {
          const Rgb rgb{{Real(0.05 + 0.15 * ir), Real(0.05 + 0.15 * ig), Real(0.05 + 0.15 * ib)}};
          double xyz[3] = {0, 0, 0};
          for (uint32_t i = 0; i < astroray::kObserverSamples; i += 4) {
            const uint32_t n = astroray::kObserverSamples;
            Lambda lam;
            for (uint32_t l = 0; l < 4; ++l) lam.nm[l] = Real(360 + (i + l < n ? i + l : n - 1));
            uint32_t flags = 0;
            const Spec s = ar::albedo(fx.table, rgb, lam, &flags);
            CHECK(flags == 0u);
            for (uint32_t l = 0; l < 4 && i + l < n; ++l)
              for (int k = 0; k < 3; ++k) xyz[k] += double(s.v[l]) * double(fx.obs.d65[i + l]) * double(fx.obs.cmf[k][i + l]);
          }
          for (double& v : xyz) v /= fx.obs.y_n;
          double out[3];
          to_rgb(fx.obs, xyz, out);
          for (int c = 0; c < 3; ++c)
            if (!(std::fabs(out[c] - double(rgb.v[c])) <= worst)) {  // a NaN is kept, never skipped
              worst = std::fabs(out[c] - double(rgb.v[c]));
              worst_rgb = rgb;
            }
        }
    CHECK_MESSAGE(worst <= 1e-3, "max abs error " << worst << " at rgb " << worst_rgb.v[0] << " " << worst_rgb.v[1] << " " << worst_rgb.v[2]);
  }

  // Check 5.
  AR_RT_CASE("rgb2spec/white is one") {
    Fixture fx;
    double worst = 0;
    for (int i = 0; i < ar::kObsSamples; i += 4) {
      Lambda lam;
      for (int l = 0; l < 4; ++l) lam.nm[l] = Real(360 + (i + l < ar::kObsSamples ? i + l : ar::kObsSamples - 1));
      uint32_t flags = 0;
      const Spec s = ar::albedo(fx.table, Rgb{{Real(1), Real(1), Real(1)}}, lam, &flags);
      for (int l = 0; l < 4; ++l) {
        const double e = std::fabs(double(s.v[l]) - 1.0);
        if (!(e <= worst)) worst = e;  // written so that a NaN is kept, never skipped
      }
      CHECK(flags == 0u);
    }
    CHECK_MESSAGE(worst <= 1e-4, "max |s - 1| = " << worst);
  }

  // Check 6. An RGB emitter through the illuminant convention, film_weights and the XYZ
  // quadrature, with a deterministic midpoint rule over lambda in place of sampling: packet m
  // carries lanes 4m..4m+3 of 16384 points, each with the uniform pdf 1/470.
  AR_RT_CASE("rgb2spec/illuminant convention returns its rgb") {
    Fixture fx;
    const Rgb emitters[] = {{{1, 1, 1}}, {{0.2f, 0.5f, 0.8f}}, {{1, 0.1f, 0.1f}}, {{0.9f, 0.9f, 0.1f}}, {{5, 2, 0.5f}}, {{40, 10, 25}}};
    const int packets = 4096;
    const double span = 470.0;
    const Spec pf = ar::spec_fill(Real(1.0 / span));
    const Spec w = ar::film_weights(0u, pf);
    for (const Rgb& rgb : emitters) {
      double xyz[3] = {0, 0, 0};
      uint32_t flags = 0;
      for (int m = 0; m < packets; ++m) {
        Lambda lam;
        for (int l = 0; l < 4; ++l) lam.nm[l] = Real(360.0 + span * (4.0 * m + l + 0.5) / (4.0 * packets));
        const Spec L = ar::illuminant(fx.table, fx.view, rgb, lam, &flags);
        for (int l = 0; l < 4; ++l)
          for (int k = 0; k < 3; ++k) xyz[k] += double(w.v[l]) * double(L.v[l]) * double(ar::cmf_at(fx.view, k, lam.nm[l]));
      }
      for (double& v : xyz) v /= double(packets) * fx.obs.y_n;
      double out[3];
      to_rgb(fx.obs, xyz, out);
      const double scale = std::fmax(rgb.v[0], std::fmax(rgb.v[1], rgb.v[2]));
      for (int c = 0; c < 3; ++c)
        CHECK_MESSAGE(std::fabs(out[c] - double(rgb.v[c])) <= 1e-3 * scale,
                      "rgb " << rgb.v[0] << " " << rgb.v[1] << " " << rgb.v[2] << " channel " << c << " came back as " << out[c]);
      CHECK(flags == 0u);
    }
  }

  // Expected values: computed in double with NumPy from the committed rec709.jh and observer.bin
  // (an independent implementation of pbrt-v4's RGBToSpectrumTable lookup and sigmoid, run
  // from the same table files, with the float-rounded inputs below), quoted to 10 digits.
  // The double build must match to 1e-6. The float build is limited by float arithmetic: the
  // lookup position rgb * 63 / z and the polynomial in nm (c2 and the lambda terms cancel to
  // O(1)) cost about 1e-6 relative to the value, measured 1.0e-5 at worst here, so it is held
  // to 2e-5 * max(1, value); the task's 1e-6 for float is not reachable with pbrt's convention.
  AR_RT_CASE("rgb2spec/float and double agree with the reference") {
    Fixture fx;
    const Lambda lam = lanes(Real(405.5), Real(487.25), Real(560.0), Real(689.75));
    struct Case {
      int kind;  // 0 albedo, 1 unbounded, 2 illuminant
      Rgb rgb;
      double want[4];
    };
    const Case cases[] = {
        {0, {{0.2f, 0.5f, 0.8f}}, {0.8524076093, 0.6799922158, 0.4405129201, 0.1576194574}},
        {0, {{0.9f, 0.1f, 0.4f}}, {0.9093813396, 0.1363850958, 0.1712236684, 0.9905395721}},
        {0, {{0.05f, 0.95f, 0.5f}}, {0.0357312034, 0.8851352570, 0.8731764565, 0.0050958320}},
        {1, {{3.0f, 1.5f, 0.2f}}, {0.1567160025, 0.6228597231, 1.8409987979, 2.2158788682}},
        {1, {{0.02f, 0.4f, 7.0f}}, {9.8401572233, 2.9432840402, 0.4016627418, 0.0416579007}},
        {2, {{1.0f, 0.5f, 0.25f}}, {0.1971168000, 0.3859982361, 0.5784029768, 0.9613232393}},
        {2, {{0.3f, 0.05f, 2.0f}}, {3.2924313102, 0.5564852619, 0.1730495281, 0.4094716302}},
    };
    const bool is_float = sizeof(Real) == 4;
    for (const Case& c : cases) {
      uint32_t flags = 0;
      const Spec s = c.kind == 0   ? ar::albedo(fx.table, c.rgb, lam, &flags)
                     : c.kind == 1 ? ar::unbounded(fx.table, c.rgb, lam, &flags)
                                   : ar::illuminant(fx.table, fx.view, c.rgb, lam, &flags);
      CHECK(flags == 0u);
      for (int l = 0; l < 4; ++l)
        CHECK_MESSAGE(std::fabs(double(s.v[l]) - c.want[l]) <= (is_float ? 2e-5 * std::fmax(1.0, c.want[l]) : 1e-6),
                      "kind " << c.kind << " rgb " << c.rgb.v[0] << " " << c.rgb.v[1] << " " << c.rgb.v[2] << " lane " << l
                              << ": " << double(s.v[l]) << " vs " << c.want[l]);
    }
  }

  AR_RT_CASE("rgb2spec/conventions: zero, grey, clamps and flags") {
    Fixture fx;
    const Lambda lam = lanes(Real(380), Real(500), Real(600), Real(800));
    uint32_t flags = 0;
    const Rgb zero{{0, 0, 0}};
    for (const Spec& s : {ar::albedo(fx.table, zero, lam, &flags), ar::unbounded(fx.table, zero, lam, &flags),
                          ar::illuminant(fx.table, fx.view, zero, lam, &flags)})
      for (int l = 0; l < 4; ++l) CHECK(s.v[l] == Real(0));  // exactly zero, not merely small
    CHECK(flags == 0u);
    // A grey is flat: albedo g gives g at every wavelength, unbounded g likewise.
    const Rgb grey{{Real(0.37), Real(0.37), Real(0.37)}};
    const Spec a = ar::albedo(fx.table, grey, lam, &flags), u = ar::unbounded(fx.table, grey, lam, &flags);
    for (int l = 0; l < 4; ++l) {
      CHECK(std::fabs(double(a.v[l]) - 0.37) < 1e-6);
      CHECK(std::fabs(double(u.v[l]) - 0.37) < 1e-6);
    }
    CHECK(flags == 0u);
    // Negative components clamp to 0 and are reported; albedo above 1 clamps and is reported.
    flags = 0;
    const Spec neg = ar::albedo(fx.table, Rgb{{Real(-0.5), Real(0.5), Real(0.5)}}, lam, &flags);
    const Spec ref = ar::albedo(fx.table, Rgb{{Real(0), Real(0.5), Real(0.5)}}, lam, &flags);
    CHECK((flags & ar::kRgbNegativeClamped) != 0u);
    for (int l = 0; l < 4; ++l) CHECK(neg.v[l] == ref.v[l]);
    flags = 0;
    ar::albedo(fx.table, Rgb{{Real(2), Real(0.5), Real(0.5)}}, lam, &flags);
    CHECK(flags == ar::kRgbAboveOneClamped);
    flags = 0;
    ar::unbounded(fx.table, Rgb{{Real(2), Real(0.5), Real(0.5)}}, lam, &flags);  // above 1 is legal here
    CHECK(flags == 0u);
  }

  AR_RT_CASE("rgb2spec/jh file header and observer lookup") {
    Fixture fx;
    CHECK(fx.table.res == 64);
    CHECK(fx.table.illuminant_id == ar::kJhIlluminantD65);
    CHECK(std::memcmp(fx.table.observer_hash, fx.obs.observer_hash, 8) == 0);
    // Corrupt blobs are refused with a reason.
    ar::Rgb2SpecTable t;
    std::vector<uint8_t> bad = fx.jh_bytes;
    CHECK(ar::jh_view(bad.data(), bad.size() - 4, &t) != nullptr);
    bad[0] = 'X';
    CHECK(ar::jh_view(bad.data(), bad.size(), &t) != nullptr);
    // The embedded copy is the committed file.
    astroray::DataBlob blob;
    REQUIRE(astroray::find_data("spectral/rec709.jh", &blob));
    CHECK((blob.size == fx.jh_bytes.size() && std::memcmp(blob.data, fx.jh_bytes.data(), blob.size) == 0));
    // Lookup: linear between samples, 0 outside 360-830.
    const double y0 = fx.obs.cmf[1][100], y1 = fx.obs.cmf[1][101];
    CHECK(std::fabs(double(ar::cmf_at(fx.view, 1, Real(460.25))) - (0.75 * y0 + 0.25 * y1)) < 1e-6);
    CHECK(ar::cmf_at(fx.view, 1, Real(830)) == Real(fx.obs.cmf[1][470]));
    CHECK(ar::cmf_at(fx.view, 1, Real(359.5)) == Real(0));
    CHECK(ar::cmf_at(fx.view, 1, Real(830.5)) == Real(0));
    CHECK(ar::illuminant_at(fx.view, Real(900)) == Real(0));
    CHECK(std::fabs(double(ar::illuminant_at(fx.view, Real(560))) - double(fx.obs.d65[200])) < 1e-6);
  }
}
