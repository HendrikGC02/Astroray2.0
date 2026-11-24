// SPDX-License-Identifier: Apache-2.0
// ar-f64: compiled twice into this executable, once with AR_REAL_DOUBLE (cmake/sources.cmake).
// GRATING extension kind functions (0005 items 21-23, Verification 8, function-level half): order
// directions, evanescence, table lookup, order selection by the lane-mean efficiency, lane handling.
// Ref: docs/decisions/0005-material-model.md items 21-23; C. Palmer, Diffraction Grating Handbook,
// 7th ed. (Newport 2014), eq. 2-1. The pinned angles come from an independent float64 NumPy
// evaluation of eq. 2-1 (asin(m lambda / d - sin alpha), 600 lines/mm, 632.8 nm), not from the code.
// Efficiency tables here are hand-built test data, J_m^2 of a made-up argument (Bessel squares sum
// to 1 over all orders, so every cell is <= 1); they are not a physical grating model.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/stat.h"
#include "../../src/host/material/grating_table.h"
#include "astroray/astroray.h"
#include "material/ext/grating.h"

namespace st = astroray::stat;

namespace {

using ar::Frame;
using ar::GratingParams;
using ar::GratingTable;
using ar::Lambda;
using ar::LobeSample;
using ar::Real;
using ar::Real3;
using ar::Spec;
using ar::TransportMode;

constexpr double kPi = 3.14159265358979323846;
constexpr bool kF32 = sizeof(Real) == 4;
constexpr double kPitch600 = 1.0e6 / 600.0;  // nm
constexpr double kHeNe = 632.8;              // nm

struct Rng {
  uint64_t s;
  double next() {
    s += 0x9E3779B97F4A7C15ull;
    uint64_t z = s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
  }
};

GratingParams params(double pitch_nm, int m_min, int m_max, bool transmit = false, double n_sub = 1.5,
                     bool dispersive = false) {
  GratingParams g = {};
  g.pitch_nm = float(pitch_nm);
  g.mode = uint32_t(transmit ? ar::GratingMode::Transmit : ar::GratingMode::Reflect);
  g.m_min = m_min;
  g.m_max = m_max;
  g.substrate_ior.semantic = uint8_t(ar::SpectrumSemantic::IOR);
  g.substrate_ior.kind = uint8_t(dispersive ? ar::SpectrumKind::CAUCHY_ABBE : ar::SpectrumKind::CONSTANT);
  g.substrate_ior.p[0] = float(n_sub);
  g.substrate_ior.p[1] = 30.0f;  // Abbe number, read by CAUCHY_ABBE only
  g.substrate_ior.p[2] = 1.0f;
  return g;
}

// A table filled from fn(m, lambda_nm, cos_i) at every node.
struct HostTable {
  std::vector<float> eta;
  int m_min = 0, count = 0;
  GratingTable view() const { return GratingTable{eta.data(), m_min, count}; }
};

double node_lambda(int w) { return 360.0 + 5.0 * w; }
double node_cos(int c) { return double(c) / double(ar::kGratingCosBins - 1); }

HostTable make_table(int m_min, int m_max, const std::function<double(int, double, double)>& fn) {
  HostTable t;
  t.m_min = m_min;
  t.count = m_max - m_min + 1;
  t.eta.assign(size_t(t.count) * ar::kGratingCosBins * ar::kGratingWavelengths, 0.0f);
  for (int o = 0; o < t.count; ++o)
    for (int c = 0; c < ar::kGratingCosBins; ++c)
      for (int w = 0; w < ar::kGratingWavelengths; ++w)
        t.eta[ar::grating_table_index(o, c, w)] = float(fn(m_min + o, node_lambda(w), node_cos(c)));
  return t;
}

// Test data: J_m^2 of an argument that varies across the visible and with incidence.
double bessel_eta(int m, double lambda_nm, double cos_i) {
  const double x = 900.0 / lambda_nm * (0.6 + 0.4 * cos_i);
  const double j = std::cyl_bessel_j(double(std::abs(m)), x);
  return j * j;
}

// Independent propagation test (eq. 2-1 with the substrate index): order m from an in-plane wo with
// tangential component s_y along g leaves with |(-n_i s_y + m lambda / d) / n_o| <= 1.
bool propagates(double s_y, int m, double lambda_nm, double pitch, double n_i = 1.0, double n_o = 1.0) {
  return std::fabs((-n_i * s_y + m * lambda_nm / pitch) / n_o) <= 1.0;
}

Frame frame_z() { return ar::frame_from_normal_tangent(Real3{0, 0, 1}, Real3{1, 0, 0}); }

Real3 in_plane(double cos_i, bool front = true) {
  const double s = std::sqrt(1.0 - cos_i * cos_i);
  return Real3{Real(0), Real(s), Real(front ? cos_i : -cos_i)};
}

Lambda lambda_of(double a, double b, double c, double d) { return Lambda{{Real(a), Real(b), Real(c), Real(d)}}; }

double deg(double rad) { return rad * 180.0 / kPi; }

// The angle from the normal, signed by the component along g (local y), of a local direction.
double signed_angle(Real3 w) { return deg(std::atan2(double(w.y), std::fabs(double(w.z)))); }

// Which order a sampled world direction is: the m whose direction it matches.
int order_of(const GratingParams& p, Real3 wo, Real3 wi, double lambda0, double n_i, double n_o) {
  for (int m = p.m_min; m <= p.m_max; ++m) {
    Real3 d{};
    if (!ar::grating_order_direction(wo, m, Real(lambda0), Real(p.pitch_nm), p.mode == 1u, Real(n_i), Real(n_o), &d))
      continue;
    if (std::fabs(double(d.x - wi.x)) + std::fabs(double(d.y - wi.y)) + std::fabs(double(d.z - wi.z)) < 1e-4) return m;
  }
  return 1000;
}

}  // namespace

TEST_SUITE("fast") {
  AR_RT_CASE("grating/orders at 632.8 nm") {
    // NumPy: asin(m * 632.8 / (1e6 / 600)) for m = 1, 2; into n = 1.5: asin(m * 632.8 / (1e6 / 600) / 1.5).
    const double pinned[3] = {0.0, 22.313862533531342, 49.40780934362739};
    const double pinned_glass[3] = {0.0, 14.662214833279869, 30.413700970695643};
    std::printf("grating orders, 600 lines/mm, 632.8 nm, normal incidence (degrees):\n");
    std::printf("   m   reflection   transmission (glass->air)   transmission (air->glass)   eq. 2-1\n");
    for (int m : {-2, -1, 1, 2}) {
      const int a = std::abs(m);
      const double eq = deg(std::asin(m * kHeNe / kPitch600));
      CHECK(std::fabs(std::fabs(eq) - pinned[a]) < 1e-9);  // the test's own equation against NumPy
      Real3 r{}, tb{}, tf{};
      REQUIRE(ar::grating_order_direction(Real3{0, 0, 1}, m, Real(kHeNe), Real(kPitch600), false, 1, 1, &r));
      REQUIRE(ar::grating_order_direction(Real3{0, 0, -1}, m, Real(kHeNe), Real(kPitch600), true, Real(1.5), 1, &tb));
      REQUIRE(ar::grating_order_direction(Real3{0, 0, 1}, m, Real(kHeNe), Real(kPitch600), true, 1, Real(1.5), &tf));
      std::printf("  %+d   %+9.4f    %+9.4f                  %+9.4f                  %+9.4f\n", m, signed_angle(r),
                  signed_angle(tb), signed_angle(tf), eq);
      CHECK(r.z > Real(0));
      CHECK(tb.z > Real(0));  // from the substrate side out into the air above
      CHECK(tf.z < Real(0));
      CHECK(std::fabs(signed_angle(r) - (m > 0 ? 1 : -1) * pinned[a]) < 0.1);
      CHECK(std::fabs(signed_angle(tb) - (m > 0 ? 1 : -1) * pinned[a]) < 0.1);
      CHECK(std::fabs(signed_angle(tf) - (m > 0 ? 1 : -1) * pinned_glass[a]) < 0.1);
      CHECK(std::fabs(double(r.x)) < 1e-6);  // in the plane normal to the grooves
    }
    // Through grating_sample in a tilted frame: the world direction makes the same angle with N and
    // lies in the plane of N and g = N x T.
    const Real3 n = ar::normalize(Real3{Real(0.3), Real(-0.2), Real(0.9)});
    const Frame f = ar::frame_from_normal_tangent(n, Real3{Real(0.2), Real(1), Real(0.1)});
    for (int m : {-2, -1, 1, 2}) {
      const HostTable t = make_table(m, m, [](int, double, double) { return 0.5; });
      const LobeSample s = ar::grating_sample(params(kPitch600, m, m), t.view(), f, n, TransportMode::Radiance,
                                              lambda_of(kHeNe, 500, 600, 700), Real(0.5));
      REQUIRE(s.pdf > Real(0));
      const double c = double(ar::dot(s.wi, f.n));
      const double g = double(ar::dot(s.wi, f.t));
      CHECK(std::fabs(double(ar::dot(s.wi, f.s))) < 1e-5);
      CHECK(std::fabs(deg(std::atan2(g, c)) - (m > 0 ? 1 : -1) * pinned[std::abs(m)]) < 0.1);
    }
  }

  AR_RT_CASE("grating/no order three") {
    // 3 * 632.8 / 1666.7 = 1.139 > 1: evanescent in reflection and leaving the substrate into air.
    for (int m : {-3, 3}) {
      Real3 d{};
      CHECK(!ar::grating_order_direction(Real3{0, 0, 1}, m, Real(kHeNe), Real(kPitch600), false, 1, 1, &d));
      CHECK(!ar::grating_order_direction(Real3{0, 0, -1}, m, Real(kHeNe), Real(kPitch600), true, Real(1.5), 1, &d));
      // A table with efficiency only in order m gives no sample at all.
      const HostTable t = make_table(m, m, [](int, double, double) { return 0.5; });
      const LobeSample s = ar::grating_sample(params(kPitch600, m, m), t.view(), frame_z(), Real3{0, 0, 1},
                                              TransportMode::Radiance, lambda_of(kHeNe, kHeNe, kHeNe, kHeNe), Real(0.3));
      CHECK(s.pdf == Real(0));
    }
    // Orders -3..3 all with efficiency: sampling never returns |m| = 3.
    const GratingParams p = params(kPitch600, -3, 3);
    const HostTable t = make_table(-3, 3, [](int, double, double) { return 0.1; });
    int seen[7] = {};
    for (int k = 0; k < 7000; ++k) {
      const LobeSample s = ar::grating_sample(p, t.view(), frame_z(), Real3{0, 0, 1}, TransportMode::Radiance,
                                              lambda_of(kHeNe, kHeNe, kHeNe, kHeNe), Real((k + 0.5) / 7000.0));
      REQUIRE(s.pdf > Real(0));
      const int m = order_of(p, Real3{0, 0, 1}, s.wi, kHeNe, 1, 1);
      REQUIRE(std::abs(m) <= 3);
      ++seen[m + 3];
    }
    CHECK(seen[0] == 0);
    CHECK(seen[6] == 0);
    for (int m = -2; m <= 2; ++m) CHECK(std::abs(seen[m + 3] - 1400) <= 1);
  }

  AR_RT_CASE("grating/oblique incidence") {
    // In-plane, alpha = 30 degrees: sin(beta) = m lambda / d - sin(alpha) (eq. 2-1). NumPy values of
    // beta for m = -1, 0, 1, 2; m = -2 is evanescent (-1.259).
    const Real3 wo = in_plane(std::cos(30.0 * kPi / 180.0));
    const struct {
      int m;
      double beta;
    } rows[] = {{-1, -61.60378608176052}, {0, -29.999999999999996}, {1, -6.910571041397789}, {2, 15.032090207339495}};
    for (const auto& r : rows) {
      Real3 d{};
      REQUIRE(ar::grating_order_direction(wo, r.m, Real(kHeNe), Real(kPitch600), false, 1, 1, &d));
      const double eq = deg(std::asin(r.m * kHeNe / kPitch600 - 0.5));
      CHECK(std::fabs(eq - r.beta) < 1e-9);
      CHECK(std::fabs(signed_angle(d) - r.beta) < 0.1);
      CHECK(d.z > Real(0));
    }
    Real3 d{};
    CHECK(!ar::grating_order_direction(wo, -2, Real(kHeNe), Real(kPitch600), false, 1, 1, &d));
    // Conical: theta 40, phi 35 degrees (x along the grooves). NumPy: t_o = -t_i + (0, m lambda / d).
    const double th = 40.0 * kPi / 180.0, ph = 35.0 * kPi / 180.0;
    const Real3 wc{Real(std::sin(th) * std::cos(ph)), Real(std::sin(th) * std::sin(ph)), Real(std::cos(th))};
    const struct {
      int m;
      double x, y, z;
    } cone[] = {{-1, -0.5265407845183632, -0.7483678264946123, 0.4033613745842759},
                {1, -0.5265407845183632, 0.01099217350538767, 0.8500788047942461}};
    const double tol = kF32 ? 2e-6 : 1e-12;
    for (const auto& c : cone) {
      REQUIRE(ar::grating_order_direction(wc, c.m, Real(kHeNe), Real(kPitch600), false, 1, 1, &d));
      CHECK(std::fabs(double(d.x) - c.x) < tol);
      CHECK(std::fabs(double(d.y) - c.y) < tol);
      CHECK(std::fabs(double(d.z) - c.z) < 1e3 * tol);
    }
    CHECK(!ar::grating_order_direction(wc, -2, Real(kHeNe), Real(kPitch600), false, 1, 1, &d));
  }

  AR_RT_CASE("grating/table lookup") {
    // Linear in lambda and in cos_i, exact at nodes, clamped outside both ranges.
    const HostTable t = make_table(-1, 1, [](int m, double l, double c) { return 0.001 * l + 0.1 * c + 0.01 * m; });
    const GratingTable v = t.view();
    const double tol = kF32 ? 2e-6 : 1e-7;
    CHECK(std::fabs(double(ar::grating_table_eta(v, 1, Real(500), Real(node_cos(7)))) - (0.5 + 0.1 * node_cos(7) + 0.01)) < tol);
    CHECK(std::fabs(double(ar::grating_table_eta(v, 0, Real(502.5), Real(0.5))) - (0.5025 + 0.05)) < tol);
    CHECK(std::fabs(double(ar::grating_table_eta(v, -1, Real(300), Real(1.2))) - (0.36 + 0.1 - 0.01)) < tol);
    CHECK(std::fabs(double(ar::grating_table_eta(v, -1, Real(900), Real(-0.1))) - (0.83 - 0.01)) < tol);
    CHECK(ar::grating_table_eta(v, 2, Real(500), Real(0.5)) == Real(0));
    CHECK(ar::grating_table_eta(v, -2, Real(500), Real(0.5)) == Real(0));
    CHECK(std::isfinite(double(ar::grating_table_eta(v, 0, Real(NAN), Real(NAN)))));
  }

  AR_RT_CASE("grating/sample lanes and flags") {
    const HostTable t = make_table(-2, 2, bessel_eta);
    const Lambda lam = lambda_of(450, 550, 650, 750);
    const Real3 wo = in_plane(0.8);
    const double cos_i = 0.8;
    // Reflection: find order 0 and order 1 by walking u.
    const GratingParams pr = params(kPitch600, -2, 2);
    bool saw0 = false, saw1 = false;
    for (int k = 0; k < 400; ++k) {
      const LobeSample s = ar::grating_sample(pr, t.view(), frame_z(), wo, TransportMode::Radiance, lam, Real((k + 0.5) / 400));
      REQUIRE(s.pdf > Real(0));
      CHECK((s.flags & ar::LOBE_DELTA) != 0u);
      CHECK((s.flags & ar::LOBE_REFLECTION) != 0u);
      const int m = order_of(pr, wo, s.wi, 450, 1, 1);
      const double ac = std::fabs(double(s.wi.z));
      if (m == 0) {
        saw0 = true;
        CHECK((s.flags & ar::LOBE_WL_DEPENDENT_DIR) == 0u);
        for (int j = 0; j < 4; ++j) {
          const double e = double(ar::grating_table_eta(t.view(), 0, lam.nm[j], Real(cos_i)));
          CHECK(std::fabs(double(s.f.v[j]) * ac - e) < 1e-5 * (1 + e));
        }
      } else {
        saw1 = saw1 || m == 1;
        CHECK((s.flags & ar::LOBE_WL_DEPENDENT_DIR) != 0u);
        CHECK(s.f.v[0] > Real(0));
        for (int j = 1; j < 4; ++j) CHECK(s.f.v[j] == Real(0));
      }
    }
    CHECK(saw0);
    CHECK(saw1);
    // eval and pdf are zero (delta).
    CHECK(ar::grating_pdf(pr, t.view(), frame_z(), wo, Real3{0, 0, 1}, TransportMode::Radiance, lam) == Real(0));
    const ar::LobeEval ev = ar::grating_eval(pr, t.view(), frame_z(), wo, Real3{0, 0, 1}, TransportMode::Radiance, lam);
    CHECK(ev.pdf == Real(0));
    for (int j = 0; j < 4; ++j) CHECK(ev.f.v[j] == Real(0));
    // Transmission order 0: all lanes with a constant substrate, lane 0 only with a dispersive one.
    // Radiance mode carries (n_i / n_o)^2 = (1 / 1.5)^2 on top of Importance mode.
    const HostTable t0 = make_table(0, 0, [](int, double l, double) { return l / 1000.0; });
    for (bool dispersive : {false, true}) {
      const GratingParams pt = params(kPitch600, 0, 0, true, 1.5, dispersive);
      const LobeSample a = ar::grating_sample(pt, t0.view(), frame_z(), wo, TransportMode::Importance, lam, Real(0.5));
      const LobeSample b = ar::grating_sample(pt, t0.view(), frame_z(), wo, TransportMode::Radiance, lam, Real(0.5));
      REQUIRE(a.pdf > Real(0));
      REQUIRE(b.pdf > Real(0));
      CHECK(a.wi.z < Real(0));
      CHECK((a.flags & ar::LOBE_TRANSMISSION) != 0u);
      CHECK(((a.flags & ar::LOBE_WL_DEPENDENT_DIR) != 0u) == dispersive);
      const Spec n_sub = ar::grating_substrate_ior(pt, lam);
      for (int j = 0; j < 4; ++j) {
        if (dispersive && j > 0) {
          CHECK(a.f.v[j] == Real(0));
          continue;
        }
        const double e = double(lam.nm[j]) / 1000.0;
        CHECK(std::fabs(double(a.f.v[j]) * std::fabs(double(a.wi.z)) / double(a.pdf) - e) < 1e-5);
        const double k = 1.0 / (double(n_sub.v[j]) * double(n_sub.v[j]));
        CHECK(std::fabs(double(b.f.v[j]) - k * double(a.f.v[j])) < 1e-5 * double(a.f.v[j]));
      }
    }
  }

  AR_RT_CASE("grating/no sample without efficiency") {
    const Lambda lam = lambda_of(450, 550, 650, 750);
    const HostTable zero = make_table(-2, 2, [](int, double, double) { return 0.0; });
    const HostTable some = make_table(-2, 2, bessel_eta);
    const GratingParams p = params(kPitch600, -2, 2);
    for (double u : {0.0, 0.5, 0.999999}) {
      const LobeSample a = ar::grating_sample(p, zero.view(), frame_z(), in_plane(0.7), TransportMode::Radiance, lam, Real(u));
      CHECK(a.pdf == Real(0));
      for (int j = 0; j < 4; ++j) CHECK(a.f.v[j] == Real(0));
      // wo in the surface plane.
      const LobeSample b = ar::grating_sample(p, some.view(), frame_z(), Real3{0, 1, 0}, TransportMode::Radiance, lam, Real(u));
      CHECK(b.pdf == Real(0));
      // Every order evanescent: a pitch far below the wavelength leaves only order 0, absent here.
      const HostTable only1 = make_table(1, 1, [](int, double, double) { return 0.5; });
      const LobeSample c = ar::grating_sample(params(100.0, 1, 1), only1.view(), frame_z(), in_plane(0.9),
                                              TransportMode::Radiance, lam, Real(u));
      CHECK(c.pdf == Real(0));
      CHECK(ar::is_finite(c.f));
      Real3 dd{};
      CHECK(!ar::grating_order_direction(in_plane(0.9), 1, lam.nm[0], Real(100), false, 1, 1, &dd));
      // Total internal reflection of order 0 leaving the substrate at 60 degrees.
      CHECK(!ar::grating_order_direction(in_plane(0.5, false), 0, lam.nm[0], Real(kPitch600), true, Real(1.5), 1, &dd));
      const HostTable t0 = make_table(0, 0, [](int, double, double) { return 0.9; });
      const LobeSample d = ar::grating_sample(params(kPitch600, 0, 0, true, 1.5), t0.view(), frame_z(),
                                              in_plane(0.5, false), TransportMode::Radiance, lam, Real(u));
      CHECK(d.pdf == Real(0));
    }
  }

  AR_RT_CASE("grating/lossless furnace") {
    // Every cell normalised over the table orders (-6..6) that propagate for an in-plane wo at that node, so lane
    // 0's expected weight sum_m eta_m(lambda_0) is 1. u is stratified, so the mean over N samples
    // differs from the expectation by at most the sum over orders of weight / N, with every weight
    // at most 4 (p_m >= eta_m(lambda_0) / 4) and at most 11 orders propagating here: 44 / N.
    // A chosen order evanescent at lane 0 has weight 0 (pdf 0).
    constexpr int kN = 25000;
    struct Config {
      bool transmit;
      double n_i, n_o;  // wo side, far side
      bool front;
    };
    const Config configs[] = {{false, 1, 1, true}, {true, 1, 1.5, true}, {true, 1.5, 1, false}};
    std::printf("grating lossless furnace, lane-0 weight, %d stratified samples per row:\n", kN);
    for (const Config& cf : configs) {
      for (int ci : {15, 6}) {
        const double cos_i = node_cos(ci);
        const double pitch = 1600.0;
        const HostTable t = make_table(-6, 6, [&](int m, double l, double c) {
          if (std::fabs(c - cos_i) > 1e-12) return bessel_eta(m, l, c);
          const double sy = std::sqrt(1.0 - c * c);
          if (!propagates(sy, m, l, pitch, cf.n_i, cf.n_o)) return 0.0;
          double sum = 0.0;
          for (int k = -6; k <= 6; ++k)
            if (propagates(sy, k, l, pitch, cf.n_i, cf.n_o)) sum += bessel_eta(k, l, c);
          return bessel_eta(m, l, c) / sum;
        });
        const double n_sub = cf.transmit ? (cf.front ? cf.n_o : cf.n_i) : 1.5;
        const GratingParams p = params(pitch, -6, 6, cf.transmit, n_sub);
        for (int w0 : {ci == 15 ? 9 : 77}) {  // one wavelength set per incidence keeps the fast tier short
          const Lambda lam = lambda_of(node_lambda(w0), node_lambda((w0 + 24) % 95), node_lambda((w0 + 48) % 95),
                                       node_lambda((w0 + 72) % 95));
          double sum = 0.0;
          for (int k = 0; k < kN; ++k) {
            const LobeSample s = ar::grating_sample(p, t.view(), frame_z(), in_plane(cos_i, cf.front), TransportMode::Importance,
                                                    lam, Real((k + 0.5) / kN));
            if (s.pdf > Real(0)) sum += double(s.f.v[0]) * std::fabs(double(s.wi.z)) / double(s.pdf);
          }
          const double mean = sum / kN;
          std::printf("  %s cos %.3f lambda0 %.0f  mean %.6f\n", cf.transmit ? (cf.front ? "T air->sub" : "T sub->air") : "R",
                      cos_i, double(lam.nm[0]), mean);
          CHECK(std::fabs(mean - 1.0) <= 0.002);
        }
      }
    }
  }
  // Host builder (0005 item 24). Every cell of a generated table sums to at most 1 over its orders,
  // which bounds the propagating ones at any azimuth; storage in f32 may add one rounding per order.
  static double max_cell_sum(const astroray::host::GratingTableResult& r) {
    double worst = 0.0;
    for (int c = 0; c < ar::kGratingCosBins; ++c)
      for (int w = 0; w < ar::kGratingWavelengths; ++w) {
        double sum = 0.0;
        for (int o = 0; o < r.order_count; ++o) sum += double(r.eta[ar::grating_table_index(o, c, w)]);
        worst = std::fmax(worst, sum);
      }
    return worst;
  }

  AR_RT_CASE("grating/table cells sum le one") {
    using astroray::host::GratingTableSpec;
    const double tol = 17.0 * std::ldexp(1.0, -24);
    struct Row {
      const char* what;
      double pitch, depth;
      bool transmit;
      double coating;
      bool clips;
    };
    const Row rows[] = {{"CD-like, 120 nm, unit reflectance", 1600, 120, false, 1.0, false},
                        {"deeper, 150 nm", 1600, 150, false, 0.9, false},
                        {"transmission, 120 nm, n 1.5", 1600, 120, true, 0.96, false},
                        {"reflectance above one is clipped", 1600, 120, false, 1.08, true}};
    std::printf("grating tables, largest cell sum over orders -8..8:\n");
    for (const Row& r : rows) {
      GratingTableSpec s;
      s.pitch_nm = r.pitch;
      s.depth_nm = r.depth;
      s.m_min = -8;
      s.m_max = 8;
      s.transmit = r.transmit;
      const double c = r.coating;
      s.coating = [c](double, double) { return c; };
      s.substrate_ior = [](double) { return 1.5; };
      const astroray::host::GratingTableResult t = astroray::host::build_grating_table(s);
      const double worst = max_cell_sum(t);
      std::printf("  %-36s max sum %.7f  clipped cells %u\n", r.what, worst, t.clipped_cells);
      INFO(r.what);
      REQUIRE(t.eta.size() == size_t(17) * ar::kGratingCosBins * ar::kGratingWavelengths);
      CHECK(worst <= 1.0 + tol);
      CHECK((t.clipped_cells > 0) == r.clips);
      CHECK(t.report_code == (r.clips ? uint32_t(AR_REPORT_GRATING_TABLE_LIMIT) : 0u));
      CHECK(t.report_detail.empty() == !r.clips);
    }
  }

  AR_RT_CASE("grating/bessel orders sum to one at 20 um") {
    // J_m by std::cyl_bessel_j against its recurrence J_{m-1} + J_{m+1} = (2m / x) J_m.
    for (double x : {0.3, 1.0, 1.8412, 2.7, 4.2})
      for (int m = 1; m <= 8; ++m) {
        const double l = std::cyl_bessel_j(m - 1.0, x) + std::cyl_bessel_j(m + 1.0, x);
        CHECK(std::fabs(l - 2.0 * m / x * std::cyl_bessel_j(double(m), x)) < 1e-12);
      }
    // Lossless unit reflectance at 20 um: all 17 orders propagate at normal incidence and
    // sum_m J_m^2 = 1, so every cell sums to 1 up to the orders beyond |m| = 8 and f32 storage.
    astroray::host::GratingTableSpec s;
    s.pitch_nm = 20000.0;
    s.depth_nm = 120.0;
    s.m_min = -8;
    s.m_max = 8;
    s.coating = [](double, double) { return 1.0; };
    const astroray::host::GratingTableResult t = astroray::host::build_grating_table(s);
    double lo = 2.0, hi = 0.0;
    for (int c = 0; c < ar::kGratingCosBins; ++c)
      for (int w = 0; w < ar::kGratingWavelengths; ++w) {
        double sum = 0.0;
        for (int o = 0; o < t.order_count; ++o) sum += double(t.eta[ar::grating_table_index(o, c, w)]);
        lo = std::fmin(lo, sum);
        hi = std::fmax(hi, sum);
      }
    std::printf("grating 20 um lossless table: cell sums in [%.8f, %.8f]\n", lo, hi);
    CHECK(std::fabs(lo - 1.0) < 1e-6);
    CHECK(std::fabs(hi - 1.0) < 1e-6);
    CHECK(8 * 830.0 / 20000.0 < 1.0);
    CHECK(t.clipped_cells == 0u);
    // Cells against item 24 written out here: eta_m = J_m^2(phi / 2), phi = 4 pi h cos(theta_i) / lambda.
    for (int m : {-3, 0, 2})
      for (int c : {15, 9, 2})
        for (int w : {0, 40, 94}) {
          const double j = std::cyl_bessel_j(double(std::abs(m)), 2.0 * kPi * 120.0 * node_cos(c) / node_lambda(w));
          CHECK(std::fabs(double(t.eta[ar::grating_table_index(m + 8, c, w)]) - j * j) < 1e-7);
        }
  }

  AR_RT_CASE("grating/phase depth matches Palmer peaks") {
    // Reflection at normal incidence: the first-order efficiency of a scalar-domain sinusoidal
    // grating peaks at lambda = 3.41 h and the second order at 2.05 h (Palmer, Diffraction Grating
    // Handbook 7th ed., sections 9.3 and 9.5). Located here by scanning J_m^2(phi / 2).
    const double h = 100.0;
    for (int m : {1, 2}) {
      double best = 0.0, at = 0.0;
      for (double l = 150.0; l <= 600.0; l += 0.01) {
        const double j = std::cyl_bessel_j(double(m), 0.5 * astroray::host::grating_phase_pp(false, h, l, 1.0, 1.0));
        if (j * j > best) {
          best = j * j;
          at = l;
        }
      }
      const double expect = m == 1 ? 3.41 : 2.05;
      std::printf("grating order %d peak at lambda = %.4f h (Palmer %.2f h), efficiency %.4f\n", m, at / h, expect, best);
      CHECK(std::fabs(at / h - expect) < 0.01);
    }
    // The reflection phase scales with cos(theta_i); the transmission phase does not.
    const double p0 = astroray::host::grating_phase_pp(false, h, 500.0, 1.0, 1.0);
    CHECK(std::fabs(astroray::host::grating_phase_pp(false, h, 500.0, 0.5, 1.0) - 0.5 * p0) < 1e-12);
    const double t0 = astroray::host::grating_phase_pp(true, h, 500.0, 1.0, 1.5);
    CHECK(std::fabs(t0 - 2.0 * 3.14159265358979323846 * h * 0.5 / 500.0) < 1e-12);
    CHECK(astroray::host::grating_phase_pp(true, h, 500.0, 0.3, 1.5) == t0);
  }

  AR_RT_CASE("grating/builder refuses out of range") {
    using astroray::host::GratingTableSpec;
    auto spec = [](double pitch, double depth) {
      GratingTableSpec s;
      s.pitch_nm = pitch;
      s.depth_nm = depth;
      s.m_min = -3;
      s.m_max = 3;
      s.coating = [](double, double) { return 0.9; };
      return s;
    };
    // The modulation rule h/d < 0.10: the CD preset (1.6 um, 120 nm, 0.075) builds; a DVD-class
    // pitch at the same depth (0.74 um, 0.162) is refused.
    CHECK_NOTHROW(astroray::host::build_grating_table(spec(1600.0, 120.0)));
    std::string why;
    try {
      astroray::host::build_grating_table(spec(740.0, 120.0));
    } catch (const std::runtime_error& e) {
      why = e.what();
    }
    INFO(why);
    CHECK(why.find("scalar theory out of range") != std::string::npos);
    CHECK(why.find("h/d") != std::string::npos);
    CHECK_THROWS_AS(astroray::host::build_grating_table(spec(1000.0, 100.0)), std::runtime_error);  // exactly 0.10
    CHECK_THROWS_AS(astroray::host::build_grating_table(spec(0.0, 10.0)), std::runtime_error);
    CHECK_THROWS_AS(astroray::host::build_grating_table(spec(1600.0, -1.0)), std::runtime_error);
    GratingTableSpec s = spec(1600.0, 120.0);
    s.m_max = 9;
    CHECK_THROWS_AS(astroray::host::build_grating_table(s), std::runtime_error);
    s = spec(1600.0, 120.0);
    s.coating = [](double, double) { return -0.1; };
    CHECK_THROWS_AS(astroray::host::build_grating_table(s), std::runtime_error);
    s = spec(1600.0, 120.0);
    s.transmit = true;  // no substrate index
    CHECK_THROWS_AS(astroray::host::build_grating_table(s), std::runtime_error);
  }
}

TEST_SUITE("gate") {
  AR_RT_CASE("grating/selection frequencies") {
    // Order counts against p_m = lane mean of eta_m (0 where evanescent) over its sum, computed here
    // from the table and the independent propagation test. Per-order z at the family threshold.
    const st::Run run = st::begin("grating/selection frequencies" AR_RT_SUFFIX, st::gate_family());
    constexpr int kN = 200000;
    const HostTable t = make_table(-4, 4, bessel_eta);
    struct Config {
      bool transmit, front;
      double cos_i;
    };
    int ci = 0;
    for (const Config& cf : {Config{false, true, 1.0}, Config{true, true, node_cos(10)}}) {
      const GratingParams p = params(kPitch600, -4, 4, cf.transmit, 1.5);
      const Lambda lam = lambda_of(420, 515, 610, 705);
      const Real3 wo = in_plane(cf.cos_i, cf.front);
      const double s_y = double(wo.y);
      const double n_o = cf.transmit ? 1.5 : 1.0;
      double pbar[9] = {}, sum = 0.0;
      for (int m = -4; m <= 4; ++m) {
        for (int j = 0; j < 4; ++j)
          if (propagates(s_y, m, double(lam.nm[j]), kPitch600, 1.0, n_o))
            pbar[m + 4] += double(ar::grating_table_eta(t.view(), m, lam.nm[j], Real(cf.cos_i))) / 4.0;
        sum += pbar[m + 4];
      }
      Rng rng{st::mix_bits(run.seed ^ uint64_t(++ci))};
      int count[9] = {};
      int pdf_mismatch = 0, unidentified = 0;
      for (int k = 0; k < kN; ++k) {
        const LobeSample s = ar::grating_sample(p, t.view(), frame_z(), wo, TransportMode::Radiance, lam, Real(rng.next()));
        if (!(s.pdf > Real(0))) continue;  // the chosen order is evanescent at lane 0: not identifiable
        const int m = order_of(p, wo, s.wi, double(lam.nm[0]), 1.0, n_o);
        if (std::abs(m) > 4) {
          ++unidentified;
          continue;
        }
        ++count[m + 4];
        pdf_mismatch += std::fabs(double(s.pdf) - pbar[m + 4] / sum) < 1e-5 ? 0 : 1;
      }
      CHECK(pdf_mismatch == 0);
      CHECK(unidentified == 0);
      std::printf("grating selection %s cos %.3f, %d samples:\n", cf.transmit ? "T" : "R", cf.cos_i, kN);
      for (int m = -4; m <= 4; ++m) {
        const double pm = pbar[m + 4] / sum;
        // An order that is evanescent at lane 0 returns pdf 0 and cannot be counted: skip it.
        if (!propagates(s_y, m, double(lam.nm[0]), kPitch600, 1.0, n_o)) {
          std::printf("  m %+d  p %.5f  (evanescent at lane 0)\n", m, pm);
          continue;
        }
        if (pm == 0.0) {
          CHECK(count[m + 4] == 0);
          continue;
        }
        run.family->note_check();
        const double z = (count[m + 4] - kN * pm) / std::sqrt(kN * pm * (1.0 - pm));
        std::printf("  m %+d  p %.5f  observed %.5f  z %+.2f\n", m, pm, double(count[m + 4]) / kN, z);
        CHECK_MESSAGE(std::fabs(z) <= run.family->z(), st::reproduce(run));
      }
    }
  }

  AR_RT_CASE("grating/lane mean selection unbiased") {
    // Against a lane-0-only reference that selects m with eta_m(lambda_0) / sum_k eta_k(lambda_0),
    // driven by the same u and wavelengths (paired): lane 0's weight has the same mean (both
    // estimate sum_m eta_m(lambda_0)), and the order-0 weight of every lane, 1{m = 0} eta_0(lambda_j)
    // / p_0, has lower variance with the lane mean (0005 item 22, Verification 8). Wavelength
    // packets: lambda_0 uniform on 360-830 nm, the others at quarter-range offsets, wrapped.
    const st::Run run = st::begin("grating/lane mean selection unbiased" AR_RT_SUFFIX, st::gate_family());
    constexpr int kN = 200000;
    const HostTable t = make_table(-2, 2, bessel_eta);
    const GratingParams p = params(kPitch600, -2, 2);
    const double cos_i = 0.85;
    const Real3 wo = in_plane(cos_i);
    const double s_y = double(wo.y);
    Rng rng{st::mix_bits(run.seed ^ 1u)};
    double d1 = 0, d2 = 0, a1 = 0, b1 = 0;  // paired lane-0 weight differences; the two means
    double s1 = 0, s2 = 0, r1 = 0, r2 = 0;  // order-0 weights over all lanes
    for (int k = 0; k < kN; ++k) {
      Lambda lam;
      const double l0 = 360.0 + 470.0 * rng.next();
      for (int j = 0; j < 4; ++j) lam.nm[j] = Real(360.0 + std::fmod(l0 - 360.0 + j * 117.5, 470.0));
      const double u = rng.next();
      const LobeSample s = ar::grating_sample(p, t.view(), frame_z(), wo, TransportMode::Radiance, lam, Real(u));
      double eta0[5], sum0 = 0.0;
      for (int m = -2; m <= 2; ++m) {
        eta0[m + 2] = propagates(s_y, m, double(lam.nm[0]), kPitch600)
                          ? double(ar::grating_table_eta(t.view(), m, lam.nm[0], Real(cos_i)))
                          : 0.0;
        sum0 += eta0[m + 2];
      }
      // Lane-mean sampler (the code under test).
      const double ac = std::fabs(double(s.wi.z));
      const int m = s.pdf > Real(0) ? order_of(p, wo, s.wi, double(lam.nm[0]), 1, 1) : 1000;
      const double w = s.pdf > Real(0) ? double(s.f.v[0]) * ac / double(s.pdf) : 0.0;
      for (int j = 0; j < 4; ++j) {
        const double x = (m == 0) ? double(s.f.v[j]) * ac / double(s.pdf) : 0.0;
        s1 += x;
        s2 += x * x;
      }
      // Reference: select by lane 0 alone; its lane-0 weight is eta_m / (eta_m / sum0) = sum0.
      const double ur = u * sum0;
      double acc = 0.0;
      int mr = 2;
      for (int q = -2; q <= 2; ++q) {
        if (eta0[q + 2] <= 0.0) continue;
        acc += eta0[q + 2];
        mr = q;
        if (ur < acc) break;
      }
      for (int j = 0; j < 4; ++j) {
        const double x = (mr == 0) ? double(ar::grating_table_eta(t.view(), 0, lam.nm[j], Real(cos_i))) * sum0 / eta0[2] : 0.0;
        r1 += x;
        r2 += x * x;
      }
      a1 += w;
      b1 += sum0;
      d1 += w - sum0;
      d2 += (w - sum0) * (w - sum0);
    }
    const double n = kN;
    const double dm = d1 / n;
    const double dse = std::sqrt(std::max(d2 / n - dm * dm, 0.0) / (n - 1.0));
    run.family->note_check();
    const double z = dse > 0.0 ? dm / dse : 0.0;
    const double n4 = 4.0 * kN;
    const double var = s2 / n4 - (s1 / n4) * (s1 / n4);
    const double var_ref = r2 / n4 - (r1 / n4) * (r1 / n4);
    std::printf("grating lane-0 weight: lane mean %.6f, lane-0 reference %.6f, paired difference %+.6f +- %.6f, z %+.2f\n",
                a1 / n, b1 / n, dm, dse, z);
    std::printf("grating order-0 weight variance: lane mean %.6f, lane-0 reference %.6f, ratio %.3f\n", var, var_ref,
                var / var_ref);
    CHECK_MESSAGE(std::fabs(z) <= run.family->z(), st::reproduce(run));
    // Paired streams make the two variances equal to rounding if the code selected by lane 0, so
    // "lower" is asked with a margin far above float-versus-double selection noise.
    CHECK(var < 0.9 * var_ref);
  }
}
