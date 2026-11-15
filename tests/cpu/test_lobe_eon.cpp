// SPDX-License-Identifier: Apache-2.0
// ar-f64: compiled twice into this executable, once with AR_REAL_DOUBLE (cmake/sources.cmake).
// EON diffuse lobe (0005 item 4, 0009 d14-d15). Ref: Portsmouth, Kutz, Hill 2025,
// JCGT 14(1), "EON". The pinned numbers below come from an independent
// double-precision NumPy transcription of the paper's Listings 1-4 and
// Equation 28 (not from the code under test). That script also checked, by 2-D
// quadrature, that the analytic albedo equals the integral of f cos and that the
// pdf integrates to 1 (agreement to 1e-5 at 80x160 points).
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "common/stat.h"
#include "material/lobes/eon.h"

namespace st = astroray::stat;

namespace {

using ar::EonParams;
using ar::LobeEval;
using ar::LobeSample;
using ar::Real;
using ar::Real2;
using ar::Real3;
using ar::Spec;
using ar::TransportMode;

constexpr double kPi = 3.14159265358979323846;
constexpr bool kF32 = sizeof(Real) == 4;
// Float rounding of the E(mu) cancellation limits f32; f64 matches the 12-digit literals.
constexpr double kPinTol = kF32 ? 2e-5 : 1e-9;

// Albedo of the four test lanes (the 4th is the energy-conserving limit rho = 1).
const double kRho[4] = {0.2, 0.5, 0.8, 1.0};
const double kRough[3] = {0.0, 0.5, 1.0};
const double kMu[5] = {0.05, 0.25, 0.5, 0.75, 1.0};

EonParams make_params(double r) {
  EonParams p;
  for (int i = 0; i < ar::kLanes; ++i) p.base_color.v[i] = Real(kRho[i]);
  p.roughness = Real(r);
  return p;
}

Real3 dir_from(double z, double phi) {
  const double s = std::sqrt(1.0 - z * z);
  return Real3{Real(s * std::cos(phi)), Real(s * std::sin(phi)), Real(z)};
}

bool rel_close(double got, double want, double tol) {
  return std::fabs(got - want) <= tol * std::fmax(std::fabs(want), 1e-30);
}

// splitmix64; the kernel sampler is not under test here.
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

// Integral of eon_pdf over the upper hemisphere, midpoint rule in (z, phi), which is
// uniform in solid angle.
double pdf_integral(const EonParams& p, Real3 wo, int nz, int nphi) {
  double sum = 0.0;
  for (int i = 0; i < nz; ++i) {
    const double z = (i + 0.5) / nz;
    for (int j = 0; j < nphi; ++j) {
      const double phi = 2.0 * kPi * (j + 0.5) / nphi;
      sum += double(ar::eon_pdf(p, wo, dir_from(z, phi), TransportMode::Radiance));
    }
  }
  return sum * (1.0 / nz) * (2.0 * kPi / nphi);
}

// Analytic EON albedo E_EON(rho, r, mu), Equation 28, rows r in {0, 0.5, 1}, columns
// mu in {0.05, 0.25, 0.5, 0.75, 1}, one value per rho in {0.2, 0.5, 0.8, 1}.
const double kAlbedo[3][5][4] = {
    {{0.2, 0.5, 0.8, 1}, {0.2, 0.5, 0.8, 1}, {0.2, 0.5, 0.8, 1}, {0.2, 0.5, 0.8, 1}, {0.2, 0.5, 0.8, 1}},
    {{0.198841572942, 0.498136331162, 0.79877083518, 1},
     {0.193885393951, 0.490162867249, 0.793512013906, 1},
     {0.187637503665, 0.480111307807, 0.786882604755, 1},
     {0.182362795993, 0.47162539732, 0.781285804283, 1},
     {0.179486665719, 0.466998300318, 0.778234047052, 1}},
    {{0.197910909383, 0.496557139238, 0.797668987981, 1},
     {0.188973007806, 0.481827308762, 0.787696057253, 1},
     {0.177705652743, 0.463258494981, 0.775123917074, 1},
     {0.168193321145, 0.447581992991, 0.764510036035, 1},
     {0.163006549369, 0.439034095218, 0.758722624394, 1}}};

}  // namespace

TEST_SUITE("fast") {
  AR_RT_CASE("lobe_eon/pinned values") {
    struct Row {
      double r;
      Real3 wo, wi;
      double f[4], pdf;
    };
    const Row rows[] = {
        {0.5, {Real(0.8), Real(0), Real(0.6)}, {Real(0.324181383521), Real(0.504882590885), Real(0.8)},
         {0.0659458072203, 0.169867177932, 0.28028050942, 0.357795789623}, 0.2766207416},
        {1.0, {Real(0.729612945348), Real(0.614544506102), Real(0.3)},
         {Real(-0.56607198069), Real(-0.655410186584), Real(0.5)},
         {0.00981766512731, 0.0274551185823, 0.0491250713371, 0.066171777772}, 0.0645926119033},
        {1.0, {Real(-0.181394200618), Real(0.396353559315), Real(0.9)},
         {Real(0.859851793518), Real(0.469739175696), Real(0.2)},
         {0.0518417681804, 0.132662983554, 0.217721176253, 0.277158552476}, 0.0645417056479},
        {0.0, {Real(0.8), Real(0), Real(0.6)}, {Real(0.324181383521), Real(0.504882590885), Real(0.8)},
         {0.0636619772368, 0.159154943092, 0.254647908947, 0.318309886184}, 0.254647908947}};
    for (const Row& row : rows) {
      const EonParams p = make_params(row.r);
      const LobeEval e = ar::eon_eval(p, row.wo, row.wi, TransportMode::Radiance);
      for (int i = 0; i < 4; ++i) CHECK(rel_close(double(e.f.v[i]), row.f[i], kPinTol));
      CHECK(rel_close(double(e.pdf), row.pdf, kPinTol));
      // The BRDF is reciprocal.
      const LobeEval sw = ar::eon_eval(p, row.wi, row.wo, TransportMode::Radiance);
      for (int i = 0; i < 4; ++i) CHECK(rel_close(double(sw.f.v[i]), row.f[i], kPinTol));
    }
    // Below the shading plane: no value, no density.
    const EonParams p = make_params(0.5);
    const LobeEval below = ar::eon_eval(p, rows[0].wo, Real3{Real(0.6), Real(0), Real(-0.8)}, TransportMode::Radiance);
    CHECK(below.pdf == Real(0));
    CHECK(below.f.v[0] == Real(0));
  }

  AR_RT_CASE("lobe_eon/pinned samples") {
    // Python: sample_EON of the transcription at wo = (sin t, 0, 0.5); u1 > P_u takes the CLTC
    // branch (cases 1 and 3), u1 <= P_u the uniform lobe (case 2).
    struct Row {
      double r, u1, u2;
      Real3 wi;
      double pdf, f[4];
    };
    const Row rows[] = {
        {1.0, 0.9, 0.3, {Real(0.391526937951), Real(0.879505420366), Real(0.270512240768)}, 0.116445159945,
         {0.0835979657987, 0.211609780311, 0.343243925569, 0.433335625562}},
        {1.0, 0.03, 0.6, {Real(-0.741667047328), Real(-0.538852651504), Real(0.399459398281)}, 0.0680824882728,
         {0.0186362776049, 0.0504925979883, 0.0877541619846, 0.116080392755}},
        {0.5, 0.4, 0.8, {Real(-0.019642461315), Real(-0.570304657553), Real(0.821198375112)}, 0.240876379527,
         {0.0562715039831, 0.144973036106, 0.239247258154, 0.30544998134}}};
    const Real3 wo{Real(0.866025403784), Real(0), Real(0.5)};
    for (const Row& row : rows) {
      const LobeSample s = ar::eon_sample(make_params(row.r), wo, Real2{Real(row.u1), Real(row.u2)}, TransportMode::Radiance);
      CHECK(rel_close(double(s.wi.x), double(row.wi.x), kPinTol * 5));
      CHECK(rel_close(double(s.wi.y), double(row.wi.y), kPinTol * 5));
      CHECK(rel_close(double(s.wi.z), double(row.wi.z), kPinTol * 5));
      CHECK(rel_close(double(s.pdf), row.pdf, kPinTol * 5));
      for (int i = 0; i < 4; ++i) CHECK(rel_close(double(s.f.v[i]), row.f[i], kPinTol * 5));
      CHECK((s.flags & ar::LOBE_REFLECTION) != 0);
      CHECK((s.flags & ar::LOBE_DELTA) == 0);
    }
  }

  AR_RT_CASE("lobe_eon/albedo table") {
    for (int ri = 0; ri < 3; ++ri)
      for (int mi = 0; mi < 5; ++mi) {
        const Spec e = ar::eon_albedo(make_params(kRough[ri]), Real(kMu[mi]));
        for (int i = 0; i < 4; ++i) CHECK(rel_close(double(e.v[i]), kAlbedo[ri][mi][i], kPinTol));
      }
  }

  AR_RT_CASE("lobe_eon/pdf integral") {
    // The pdf is a density over the upper hemisphere: its integral must not exceed 1; the
    // paper's construction (clipped lobe + uniform) makes it 1.
    const double r_list[] = {0.0, 0.25, 1.0};
    const double mu_list[] = {0.05, 0.3, 0.7, 1.0};
    for (double r : r_list)
      for (double mu : mu_list) {
        const Real3 wo = dir_from(mu, 0.9);
        const double integral = pdf_integral(make_params(r), wo, 256, 512);
        INFO("r=" << r << " mu=" << mu << " integral=" << integral);
        CHECK(integral <= 1.0 + 1e-3);
        CHECK(integral >= 1.0 - 1e-3);
      }
  }

  AR_RT_CASE("lobe_eon/eval equals sample") {
    // Deterministic 16x16 stratified uniforms; no random seeds in the fast tier.
    const double r_list[] = {0.0, 0.3, 1.0};
    const double mu_list[] = {0.05, 0.5, 1.0};
    int checked = 0;
    for (double r : r_list)
      for (double mu : mu_list) {
        const EonParams p = make_params(r);
        const Real3 wo = dir_from(mu, 2.1);
        for (int i = 0; i < 16; ++i)
          for (int j = 0; j < 16; ++j) {
            const Real2 u{Real((i + 0.5) / 16), Real((j + 0.5) / 16)};
            const LobeSample s = ar::eon_sample(p, wo, u, TransportMode::Radiance);
            REQUIRE(s.pdf > Real(0));
            const LobeEval e = ar::eon_eval(p, wo, s.wi, TransportMode::Radiance);
            ++checked;
            CHECK(rel_close(double(e.pdf), double(s.pdf), 1e-5));
            for (int l = 0; l < 4; ++l) CHECK(rel_close(double(e.f.v[l]), double(s.f.v[l]), 1e-5));
          }
      }
    CHECK(checked == 9 * 256);
  }

  AR_RT_CASE("lobe_eon/boundary uniforms") {
    // u in [0, 1): 0 and the largest Real below 1 are legal. At r = 0, P_u = 0 and u1 = 0 must
    // not select the uniform lobe (0 / 0).
    const Real lo = Real(0), hi = std::nextafter(Real(1), Real(0));
    const Real vals[] = {lo, hi};
    const double r_list[] = {0.0, 0.5, 1.0};
    const double mu_list[] = {0.05, 0.5, 1.0};
    for (double r : r_list)
      for (double mu : mu_list)
        for (Real u1 : vals)
          for (Real u2 : vals) {
            const EonParams p = make_params(r);
            const Real3 wo = dir_from(mu, 0.8);
            const LobeSample s = ar::eon_sample(p, wo, Real2{u1, u2}, TransportMode::Radiance);
            INFO("r=" << r << " mu=" << mu << " u=(" << double(u1) << "," << double(u2) << ")");
            CHECK(std::isfinite(double(s.wi.x)));
            CHECK(std::isfinite(double(s.wi.y)));
            CHECK(std::isfinite(double(s.wi.z)));
            CHECK(std::isfinite(double(s.pdf)));
            for (int l = 0; l < 4; ++l) CHECK(std::isfinite(double(s.f.v[l])));
            // "No sample" (pdf 0, f 0) is the contract for a direction exactly on the horizon: the
            // uniform lobe's z = 0 rim (u1 = 0 with P_u > 0) or the CLTC rim (u1 -> 1). At r = 0
            // (P_u = 0, cosine sampling) u1 = 0 is the pole, so a sample is mandatory there.
            if (r == 0.0 && u1 == lo) {
              CHECK(s.pdf > Real(0));
              CHECK(s.wi.z > Real(0));
            } else if (s.pdf == Real(0)) {
              for (int l = 0; l < 4; ++l) CHECK(s.f.v[l] == Real(0));
            } else {
              CHECK(s.wi.z > Real(0));
            }
          }
  }

  AR_RT_CASE("lobe_eon/roughness 0 is Lambertian") {
    const EonParams p = make_params(0.0);
    const double wo_mu[] = {0.05, 0.4, 1.0};
    for (double mu : wo_mu) {
      const Real3 wo = dir_from(mu, 0.3);
      for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j) {
          const Real2 u{Real((i + 0.5) / 8), Real((j + 0.5) / 8)};
          const LobeSample s = ar::eon_sample(p, wo, u, TransportMode::Radiance);
          const double cos_i = double(s.wi.z);
          CHECK(rel_close(double(s.pdf), cos_i / kPi, 1e-6));
          for (int l = 0; l < 4; ++l) {
            CHECK(rel_close(double(s.f.v[l]), kRho[l] / kPi, 1e-6));
            const LobeEval e = ar::eon_eval(p, wo, s.wi, TransportMode::Radiance);
            CHECK(rel_close(double(e.f.v[l]), kRho[l] / kPi, 1e-6));
          }
        }
    }
  }
}

TEST_SUITE("gate") {
  AR_RT_CASE("lobe_eon/energy vs analytic albedo") {
    const st::Run run = st::begin("lobe_eon/energy vs analytic albedo" AR_RT_SUFFIX, st::gate_family());
    constexpr int kN = 100000;
    std::printf("lobe_eon energy: mean of f cos / pdf against the analytic albedo, %d samples\n", kN);
    for (int ri = 0; ri < 3; ++ri)
      for (int mi = 0; mi < 5; ++mi) {
        const EonParams p = make_params(kRough[ri]);
        const Real3 wo = dir_from(kMu[mi], 0.4);
        Rng rng{st::mix_bits(run.seed ^ uint64_t(ri * 5 + mi + 1))};
        std::vector<double> w[4];
        for (int n = 0; n < kN; ++n) {
          const Real2 u{Real(rng.next()), Real(rng.next())};
          const LobeSample s = ar::eon_sample(p, wo, u, TransportMode::Radiance);
          for (int l = 0; l < 4; ++l)
            w[l].push_back(s.pdf > Real(0) ? double(s.f.v[l]) * double(s.wi.z) / double(s.pdf) : 0.0);
        }
        for (int l = 0; l < 4; ++l) {
          const double truth = kAlbedo[ri][mi][l];
          if (kRough[ri] == 0.0) {
            // Zero-variance estimator (cosine sampling of a Lambertian): the z test is
            // meaningless, so require agreement at float precision.
            double mean = 0.0;
            for (double x : w[l]) mean += x;
            mean /= kN;
            std::printf("  r=%.2f mu=%.2f rho=%.1f  mean %.7f truth %.7f  error %+.2e (deterministic)\n",
                        kRough[ri], kMu[mi], kRho[l], mean, truth, mean - truth);
            CHECK(rel_close(mean, truth, 1e-6));
            continue;
          }
          const st::EnergyResult res = st::energy_test(run, w[l], truth);
          std::printf("  r=%.2f mu=%.2f rho=%.1f  mean %.7f truth %.7f  error %+.2e  se %.1e  z %+.2f\n",
                      kRough[ri], kMu[mi], kRho[l], res.mean, truth, res.mean - truth, res.se, res.z);
          CHECK_MESSAGE(res.pass, res.message);
        }
      }
  }

  AR_RT_CASE("lobe_eon/chi2") {
    const st::Run run = st::begin("lobe_eon/chi2" AR_RT_SUFFIX, st::gate_family());
    struct Cfg {
      double r, mu;
    };
    const Cfg cfgs[] = {{0.2, 0.3}, {0.5, 0.5}, {1.0, 0.05}, {1.0, 0.5}, {1.0, 0.95}};
    int ci = 0;
    for (const Cfg& cfg : cfgs) {
      const EonParams p = make_params(cfg.r);
      const Real3 wo = dir_from(cfg.mu, 0.7);
      const uint64_t seed = st::mix_bits(run.seed ^ uint64_t(++ci));
      // Cells: ires equal-area bands in z times 2 ires equal azimuth sectors. Expected counts are
      // the pdf integrated per cell by a 12x12 midpoint rule.
      auto gen = [&](int ires) {
        const int nz = ires, nphi = 2 * ires;
        const int cells = nz * nphi;
        const int n = 2000 * cells;
        st::Histogram h;
        h.observed.assign(cells, 0.0);
        h.expected.assign(cells, 0.0);
        constexpr int kSub = 12;
        for (int iz = 0; iz < nz; ++iz)
          for (int ip = 0; ip < nphi; ++ip) {
            double e = 0.0;
            for (int a = 0; a < kSub; ++a)
              for (int b = 0; b < kSub; ++b) {
                const double z = (iz + (a + 0.5) / kSub) / nz;
                const double phi = 2.0 * kPi * (ip + (b + 0.5) / kSub) / nphi;
                e += double(ar::eon_pdf(p, wo, dir_from(z, phi), TransportMode::Radiance));
              }
            const double cell_area = (1.0 / nz) * (2.0 * kPi / nphi);
            h.expected[iz * nphi + ip] = n * e / (kSub * kSub) * cell_area;
          }
        Rng rng{seed};
        for (int k = 0; k < n; ++k) {
          const LobeSample s = ar::eon_sample(p, wo, Real2{Real(rng.next()), Real(rng.next())}, TransportMode::Radiance);
          double phi = std::atan2(double(s.wi.y), double(s.wi.x));
          if (phi < 0) phi += 2.0 * kPi;
          const int iz = std::min(nz - 1, int(double(s.wi.z) * nz));
          const int ip = std::min(nphi - 1, int(phi / (2.0 * kPi) * nphi));
          h.observed[iz * nphi + ip] += 1.0;
        }
        return h;
      };
      const st::Chi2Result res = st::chi2_with_retry(run, gen, 8);
      std::printf("lobe_eon chi2 r=%.2f mu=%.2f: chi2 %.1f dof %d p %.4f%s\n", cfg.r, cfg.mu, res.chi2, res.dof,
                  res.p, res.retried ? " (retried at 16)" : "");
      CHECK_MESSAGE(res.pass, res.message);
    }
  }
}
