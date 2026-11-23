// SPDX-License-Identifier: Apache-2.0
// ar-f64: compiled twice into this executable, once with AR_REAL_DOUBLE (cmake/sources.cmake).
// MediumSample, MediumDesc and the Henyey-Greenstein phase lobe (0006 D2, D3, D12, V1, V7).
// Ref: Henyey and Greenstein 1941. The pinned p values come from an independent float64 NumPy
// transcription of the 1941 formula, not from the code under test, with g rounded to f32 first
// because PhaseLobe::p0 is f32 in both builds:
//   g = float(np.float32(g)); p = (1 - g*g) / (4*np.pi*(1 + g*g - 2*g*mu)**1.5)
// for (g, mu) in the rows of kPins below, printed with "%.12g".
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "common/stat.h"
#include "medium/medium.h"
#include "medium/phase.h"

namespace st = astroray::stat;

namespace {

using ar::MediumDesc;
using ar::MediumSample;
using ar::PhaseLobe;
using ar::PhaseSample;
using ar::Real;
using ar::Real2;
using ar::Real3;
using ar::Spec;

constexpr double kPi = 3.14159265358979323846;
constexpr bool kF32 = sizeof(Real) == 4;
constexpr double kPinTol = kF32 ? 2e-5 : 1e-10;

static_assert(sizeof(MediumDesc) == 40, "0006 D3");
static_assert(offsetof(MediumDesc, model) == 0 && offsetof(MediumDesc, estimator) == 1 && offsetof(MediumDesc, flags) == 2,
              "0006 D3 field order");
static_assert(offsetof(MediumDesc, program) == 4 && offsetof(MediumDesc, majorant_grid) == 8 &&
                  offsetof(MediumDesc, nebular_record) == 12 && offsetof(MediumDesc, bounds) == 16,
              "0006 D3 field order");
static_assert(sizeof(ar::PhaseKind) == 1 && sizeof(ar::MediumModel) == 1 && sizeof(ar::MediumEstimator) == 1, "u8 enums");
static_assert(uint8_t(ar::PhaseKind::HG) == 0 && uint8_t(ar::MediumModel::BLENDER_VOLUME) == 0, "0006 D3, D12");
static_assert(uint8_t(ar::MediumEstimator::MARCH_EMISSION_ABSORPTION) == 0 && uint8_t(ar::MediumEstimator::DELTA_TRACK) == 1,
              "0006 D3");
static_assert((ar::MEDIUM_HAS_EMISSION | ar::MEDIUM_HAS_SCATTERING | ar::MEDIUM_HAS_VELOCITY | ar::MEDIUM_HAS_FLUORESCENCE) == 0xF,
              "four distinct flag bits");

PhaseLobe hg(float g) {
  PhaseLobe l;
  l.kind = uint8_t(ar::PhaseKind::HG);
  l.p0 = g;
  l.p1 = 0.0f;
  l.weight = ar::spec_fill(Real(1));
  return l;
}

struct D3 {
  double x, y, z;
};
D3 to_d3(Real3 v) { return D3{double(v.x), double(v.y), double(v.z)}; }
Real3 to_real3(D3 v) { return Real3{Real(v.x), Real(v.y), Real(v.z)}; }
double dot3(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 cross3(D3 a, D3 b) { return D3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
D3 norm3(D3 a) {
  const double l = std::sqrt(dot3(a, a));
  return D3{a.x / l, a.y / l, a.z / l};
}

// An orthonormal basis around axis, built in double independently of core/frame.h.
struct Basis {
  D3 e1, e2, axis;
  D3 dir(double cos_t, double phi) const {
    const double s = std::sqrt(std::fmax(0.0, 1.0 - cos_t * cos_t));
    const double c = s * std::cos(phi), d = s * std::sin(phi);
    return D3{c * e1.x + d * e2.x + cos_t * axis.x, c * e1.y + d * e2.y + cos_t * axis.y, c * e1.z + d * e2.z + cos_t * axis.z};
  }
};
Basis basis(D3 axis) {
  const D3 helper = std::fabs(axis.x) < 0.9 ? D3{1, 0, 0} : D3{0, 1, 0};
  const D3 e1 = norm3(cross3(helper, axis));
  return Basis{e1, cross3(axis, e1), axis};
}

const D3 kWo[2] = {norm3(D3{0.3, -0.5, 0.81}), norm3(D3{-0.2, 0.1, -0.97})};

bool rel_close(double got, double want, double tol) {
  return std::fabs(got - want) <= tol * std::fmax(std::fabs(want), 1e-300);
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

// Zeroth and first moments of phase_eval over the sphere of wi: integral of p and of p cos theta,
// cos theta = -dot(wo, wi). Midpoint rule in (cos theta, phi) about -wo, uniform in solid angle; the
// directions are passed to eval as full vectors, so its vector algebra is exercised.
void moments(const PhaseLobe& l, D3 wo, int nmu, int nphi, double& m0, double& m1) {
  const Basis b = basis(D3{-wo.x, -wo.y, -wo.z});
  m0 = m1 = 0.0;
  for (int i = 0; i < nmu; ++i) {
    const double mu = -1.0 + 2.0 * (i + 0.5) / nmu;
    for (int j = 0; j < nphi; ++j) {
      const D3 wi = b.dir(mu, 2.0 * kPi * (j + 0.5) / nphi);
      const double p = double(ar::phase_eval(l, to_real3(wo), to_real3(wi)));
      m0 += p;
      m1 += p * -dot3(wo, wi);
    }
  }
  const double dw = (2.0 / nmu) * (2.0 * kPi / nphi);
  m0 *= dw;
  m1 *= dw;
}

}  // namespace

TEST_SUITE("fast") {
  AR_RT_CASE("medium/hg normalisation") {
    // 0006 V7 (deterministic part) and the defining property of HG: mean scattering cosine = g.
    const float gs[3] = {-0.7f, 0.0f, 0.9f};
    for (float g : gs)
      for (const D3& wo : kWo) {
        double m0 = 0, m1 = 0;
        moments(hg(g), wo, 40000, 8, m0, m1);
        std::printf("hg g=%+.2f wo.z=%+.2f: integral %.8f  mean cos %.6f\n", double(g), wo.z, m0, m1);
        CHECK(std::fabs(m0 - 1.0) <= 1e-4);
        CHECK(std::fabs(m1 - double(g)) <= 1e-3);
      }

    struct Pin {
      float g;
      double mu, p;
    };
    const Pin kPins[] = {{0.8999999761581421f, 1.0, 15.1197121944},         {0.8999999761581421f, -1.0, 0.00220436267357},
                         {0.8999999761581421f, 0.5, 0.0174173372131},       {-0.699999988079071f, 0.3, 0.0153748286222},
                         {-0.699999988079071f, -1.0, 1.50312988809},        {0.0f, 0.2, 0.0795774715459},
                         {0.9900000095367432f, 1.0, 1583.59471182},         {-0.9900000095367432f, -1.0, 1583.59471182},
                         {0.9900000095367432f, -1.0, 0.000200947942962},    {0.5f, 0.0, 0.042705752605}};
    for (const Pin& pin : kPins)
      for (const D3& wo : kWo) {
        const D3 wi = basis(D3{-wo.x, -wo.y, -wo.z}).dir(pin.mu, 1.1);
        const double got = double(ar::phase_eval(hg(pin.g), to_real3(wo), to_real3(wi)));
        const double pdf = double(ar::phase_pdf(hg(pin.g), to_real3(wo), to_real3(wi)));
        INFO("g " << pin.g << " mu " << pin.mu << " got " << got << " want " << pin.p);
        CHECK(rel_close(got, pin.p, kPinTol));
        CHECK(pdf == got);
      }

    // Sampled directions: unit length, p equal to pdf exactly, and the pdf the sampler reports is
    // the eval of the direction it returned.
    Rng rng{0x152u};
    for (float g : gs)
      for (const D3& wo : kWo)
        for (int n = 0; n < 20000; ++n) {
          const PhaseSample s = ar::phase_sample(hg(g), to_real3(wo), Real2{Real(rng.next()), Real(rng.next())});
          const double e = double(ar::phase_eval(hg(g), to_real3(wo), s.wi));
          const double len_err = std::fabs(std::sqrt(dot3(to_d3(s.wi), to_d3(s.wi))) - 1.0);
          if (!(len_err <= (kF32 ? 1e-6 : 1e-14)) || !(s.p == s.pdf) || !rel_close(e, double(s.pdf), 1e-5)) {
            INFO("g " << g << " sample " << n << " |wi| - 1 " << len_err << " pdf " << double(s.pdf) << " eval " << e
                      << " rel " << (e / double(s.pdf) - 1.0));
            CHECK(false);
            break;
          }
        }

    // No NaN at g = +-0.99 for cos theta = +-1 and at the ends of the sample square; |g| >= 1 is
    // clamped to the largest admitted |g|.
    const float edge_g[4] = {0.99f, -0.99f, 1.0f, -1.0f};
    const Real u_edge[3] = {Real(0), Real(0.5), Real(1) - Real(kF32 ? 5.9604645e-8 : 1.1102230246251565e-16)};
    for (float g : edge_g)
      for (const D3& wo : kWo) {
        for (double mu : {1.0, -1.0}) {
          const D3 wi = D3{-mu * wo.x, -mu * wo.y, -mu * wo.z};
          const Real p = ar::phase_eval(hg(g), to_real3(wo), to_real3(wi));
          CHECK(std::isfinite(double(p)));
          CHECK(p > Real(0));
        }
        for (Real ux : u_edge)
          for (Real uy : u_edge) {
            const PhaseSample s = ar::phase_sample(hg(g), to_real3(wo), Real2{ux, uy});
            CHECK((std::isfinite(double(s.wi.x)) && std::isfinite(double(s.wi.y)) && std::isfinite(double(s.wi.z))));
            CHECK((std::isfinite(double(s.pdf)) && s.pdf > Real(0)));
          }
      }
    const Real3 wo = to_real3(kWo[0]);
    CHECK(ar::phase_eval(hg(1.0f), wo, -wo) == ar::phase_eval(hg(1.5f), wo, -wo));
    CHECK(ar::phase_eval(hg(-1.0f), wo, wo) == ar::phase_eval(hg(1.0f), wo, -wo));
    CHECK(ar::phase_eval(hg(1.0f), wo, -wo) > ar::phase_eval(hg(0.99f), wo, -wo));
  }

  AR_RT_CASE("medium/desc layout and transmittance") {
    // 0006 V1, deterministic part: Beer-Lambert per lane for a chromatic sigma_t.
    MediumSample s = ar::medium_sample_zero();
    const double sa[4] = {0.5, 1.2, 3.0, 7.0}, ss[4] = {0.1, 0.4, 0.0, 2.0};
    for (int i = 0; i < 4; ++i) {
      s.sigma_a.v[i] = Real(sa[i]);
      s.sigma_s.v[i] = Real(ss[i]);
    }
    const double tol = kF32 ? 1e-5 : 1e-12;
    for (double d : {0.0, 0.01, 0.37, 2.5}) {
      const Spec t = ar::medium_transmittance(s, Real(d));
      const Spec st_ = ar::medium_sigma_t(s);
      for (int i = 0; i < 4; ++i) {
        const double sigma_t = double(Real(sa[i]) + Real(ss[i]));
        const double want = std::exp(-sigma_t * double(Real(d)));
        INFO("d " << d << " lane " << i << " got " << double(t.v[i]) << " want " << want);
        CHECK(double(st_.v[i]) == sigma_t);
        CHECK(rel_close(double(t.v[i]), want, tol));
      }
    }

    const MediumSample z = ar::medium_sample_zero();
    for (int i = 0; i < 4; ++i) {
      CHECK(ar::medium_sigma_t(z).v[i] == Real(0));
      CHECK(z.emission.v[i] == Real(0));
      for (double d : {0.0, 1.0, 1e6}) CHECK(ar::medium_transmittance(z, Real(d)).v[i] == Real(1));
    }
    for (const PhaseLobe& l : z.phase) {
      CHECK(l.kind == uint8_t(ar::PhaseKind::HG));
      CHECK(l.p0 == 0.0f);
      CHECK(avg(l.weight) == Real(0));
    }
    CHECK((z.velocity.x == Real(0) && z.velocity.y == Real(0) && z.velocity.z == Real(0)));
  }
}

TEST_SUITE("gate") {
  // Chi-squared of sampled directions against phase_eval (0006 V7, 0009 d15). Cells: bands of angle
  // from the peak direction (-wo for g >= 0, wo for g < 0), log spaced from (1 - |g|) / 2 to pi,
  // times azimuth sectors. Expected counts integrate phase_eval over each cell with a 48x48
  // midpoint rule in (angle, azimuth).
  static void chi2_g(float g, const char* name) {
    const st::Run run = st::begin(name, st::gate_family());
    int ci = 0;
    for (const D3& wo : kWo) {
      const PhaseLobe l = hg(g);
      const Basis b = basis(g >= 0.0f ? D3{-wo.x, -wo.y, -wo.z} : wo);
      const uint64_t seed = st::mix_bits(run.seed ^ uint64_t(++ci));
      auto gen = [&](int ires) {
        const int nb = ires, ns = 2 * ires;
        const int cells = nb * ns;
        const int n = 2000 * cells;
        const double lo = std::fmin(0.5 * (1.0 - std::fabs(double(g))), kPi / 4);
        std::vector<double> edge(nb + 1);
        edge[0] = 0.0;
        for (int k = 1; k <= nb; ++k) edge[k] = kPi * std::exp(std::log(lo / kPi) * double(nb - k) / double(nb - 1));
        st::Histogram h;
        h.observed.assign(cells, 0.0);
        h.expected.assign(cells, 0.0);
        constexpr int kSub = 48;
        for (int bi = 0; bi < nb; ++bi)
          for (int si = 0; si < ns; ++si) {
            double e = 0.0;
            for (int a = 0; a < kSub; ++a) {
              const double th = edge[bi] + (edge[bi + 1] - edge[bi]) * (a + 0.5) / kSub;
              for (int c = 0; c < kSub; ++c) {
                const double ph = 2.0 * kPi * (si + (c + 0.5) / kSub) / ns;
                e += double(ar::phase_eval(l, to_real3(wo), to_real3(b.dir(std::cos(th), ph)))) * std::sin(th) *
                     (edge[bi + 1] - edge[bi]) * (2.0 * kPi / ns);
              }
            }
            h.expected[bi * ns + si] = n * e / (kSub * kSub);
          }
        Rng rng{seed};
        for (int k = 0; k < n; ++k) {
          const PhaseSample smp = ar::phase_sample(l, to_real3(wo), Real2{Real(rng.next()), Real(rng.next())});
          const D3 w = to_d3(smp.wi);
          const double th = std::acos(std::fmin(1.0, std::fmax(-1.0, dot3(w, b.axis))));
          double ph = std::atan2(dot3(w, b.e2), dot3(w, b.e1));
          if (ph < 0) ph += 2.0 * kPi;
          int bi = int(std::upper_bound(edge.begin(), edge.end(), th) - edge.begin()) - 1;
          bi = std::min(nb - 1, std::max(0, bi));
          const int si = std::min(ns - 1, int(ph / (2.0 * kPi) * ns));
          h.observed[bi * ns + si] += 1.0;
        }
        return h;
      };
      const st::Chi2Result res = st::chi2_with_retry(run, gen, 8);
      std::printf("medium hg chi2 g=%+.2f wo.z=%+.2f: chi2 %.1f dof %d p %.4f%s\n", double(g), wo.z, res.chi2, res.dof, res.p,
                  res.retried ? " (retried at 16)" : "");
      CHECK_MESSAGE(res.pass, res.message);
    }
  }

  AR_RT_CASE("medium/hg chi2 g -0.7") { chi2_g(-0.7f, "medium/hg chi2 g -0.7" AR_RT_SUFFIX); }
  AR_RT_CASE("medium/hg chi2 g 0") { chi2_g(0.0f, "medium/hg chi2 g 0" AR_RT_SUFFIX); }
  AR_RT_CASE("medium/hg chi2 g 0.9") { chi2_g(0.9f, "medium/hg chi2 g 0.9" AR_RT_SUFFIX); }
}
