// SPDX-License-Identifier: Apache-2.0
// ar-f64: compiled twice into this executable, once with AR_REAL_DOUBLE (cmake/sources.cmake).
// GGX dielectric specular reflection lobe (0005 items 4, 8, 13; 0009 d14-d15; 0004 D19).
// Ref: Walter et al. 2007 Eq. 33; Heitz 2014 (G2, Lambda); Heitz 2018 Listing 1 (VNDF);
// OpenPBR Surface 1.1.2 specular_weight. Every pinned literal below comes from an
// independent float64 NumPy transcription of those equations (Snell + Fresnel amplitudes,
// D in tan form, Lambda, G2, D_wo, Listing 1 verbatim), not from the code under test.
// Lane setup of the script: specular_color {1, .9, .6, .3}, specular_ior {1.3, 1.5, 1.7, .9}.
// The script also integrated the pdf over wi by a change of variables to the half vector
// (D_wo(m) over m with wi.z > 0), which gives the pdf-integral pins.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "common/stat.h"
#include "material/lobes/ggx.h"

namespace st = astroray::stat;

namespace {

using ar::GgxParams;
using ar::LobeEval;
using ar::LobeSample;
using ar::Real;
using ar::Real2;
using ar::Real3;
using ar::Spec;
using ar::TransportMode;

constexpr double kPi = 3.14159265358979323846;
constexpr bool kF32 = sizeof(Real) == 4;
constexpr double kPinTol = kF32 ? 5e-5 : 1e-9;

const double kColor[4] = {1.0, 0.9, 0.6, 0.3};
const double kIor[4] = {1.3, 1.5, 1.7, 0.9};  // the last lane is below 1: total internal reflection at grazing

GgxParams make_params(double roughness, double weight) {
  GgxParams p;
  for (int i = 0; i < ar::kLanes; ++i) {
    p.specular_color.v[i] = Real(kColor[i]);
    p.specular_ior.v[i] = Real(kIor[i]);
  }
  p.specular_weight = Real(weight);
  p.roughness = Real(roughness);
  return p;
}

Real3 dir_from(double z, double phi) {
  const double s = std::sqrt(std::fmax(0.0, 1.0 - z * z));
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

struct D3 {
  double x, y, z;
};
D3 to_d3(Real3 v) { return D3{double(v.x), double(v.y), double(v.z)}; }
double dot3(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 cross3(D3 a, D3 b) { return D3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
D3 norm3(D3 a) {
  const double l = std::sqrt(dot3(a, a));
  return D3{a.x / l, a.y / l, a.z / l};
}
Real3 to_real3(D3 v) { return Real3{Real(v.x), Real(v.y), Real(v.z)}; }

// Integral of ggx_pdf over wi. Change of variables wi = reflect(wo, m), dwi = 4 (wo.m) dm, then a
// midpoint rule in (s, phi) with tan(theta_m) = alpha tan(pi s / 2), which resolves lobes down to
// roughness 0.1 (alpha 0.01).
double pdf_integral(const GgxParams& p, Real3 wo, int ns, int nphi) {
  const double alpha = double(p.roughness) * double(p.roughness);
  const D3 o = to_d3(wo);
  double sum = 0.0;
  for (int i = 0; i < ns; ++i) {
    const double s = (i + 0.5) / ns;
    const double t = std::tan(0.5 * kPi * s);
    const double th = std::atan(alpha * t);
    const double c = std::cos(0.5 * kPi * s);
    const double dth = alpha / (c * c) * (0.5 * kPi) / (1.0 + alpha * alpha * t * t) / ns;
    for (int j = 0; j < nphi; ++j) {
      const double phi = 2.0 * kPi * (j + 0.5) / nphi;
      const D3 m{std::sin(th) * std::cos(phi), std::sin(th) * std::sin(phi), std::cos(th)};
      const double om = dot3(o, m);
      if (om <= 0.0) continue;
      const D3 wi{2 * om * m.x - o.x, 2 * om * m.y - o.y, 2 * om * m.z - o.z};
      const double pdf = double(ar::ggx_pdf(p, wo, to_real3(wi), TransportMode::Radiance));
      sum += pdf * 4.0 * om * std::sin(th) * dth * (2.0 * kPi / nphi);
    }
  }
  return sum;
}

// Midpoint rule of f cos over the upper hemisphere, uniform in solid angle (z, phi).
Spec albedo_quadrature(const GgxParams& p, Real3 wo, int nz, int nphi) {
  double acc[4] = {0, 0, 0, 0};
  for (int i = 0; i < nz; ++i) {
    const double z = (i + 0.5) / nz;
    for (int j = 0; j < nphi; ++j) {
      const double phi = 2.0 * kPi * (j + 0.5) / nphi;
      const LobeEval e = ar::ggx_eval(p, wo, dir_from(z, phi), TransportMode::Radiance);
      for (int l = 0; l < 4; ++l) acc[l] += double(e.f.v[l]) * z;
    }
  }
  Spec out;
  for (int l = 0; l < 4; ++l) out.v[l] = Real(acc[l] / nz * (2.0 * kPi / nphi));
  return out;
}

}  // namespace

TEST_SUITE("fast") {
  AR_RT_CASE("lobe_ggx/pinned values") {
    struct Row {
      double roughness, weight;
      Real3 wo, wi;
      double f[4], pdf;
    };
    const Row rows[] = {
        {0.5, 1, {Real(0.7642691913), Real(0.236416165329), Real(0.6)}, {Real(-0.705201610144), Real(-0.112652958463), Real(0.7)},
         {0.0600488946231, 0.108285527139, 0.109793294558, 0.00740780093146}, 1.5221421717},
        {0.1, 0.6, {Real(-0.181394200618), Real(0.396353559315), Real(0.9)}, {Real(0.22253308914), Real(-0.419617712016), Real(0.88)},
         {0.165014611838, 0.345802189284, 0.384819424631, 0.00839402038964}, 13.6646062443},
        {1, 1, {Real(0.837160408267), Real(-0.457342815435), Real(0.3)}, {Real(-0.693810723143), Real(0.518292080251), Real(0.5)},
         {0.0171475075133, 0.0229824062257, 0.0191169934935, 0.0596831036595}, 0.122426879301},
        {0.5, 0, {Real(0.7642691913), Real(0.236416165329), Real(0.6)}, {Real(-0.705201610144), Real(-0.112652958463), Real(0.7)},
         {0, 0, 0, 0}, 1.5221421717},
        {0.3, 1, {Real(0.761008367717), Real(0.640988505563), Real(0.1)}, {Real(-0.599074274027), Real(-0.693620944176), Real(0.4)},
         {0.215290373069, 0.24490979962, 0.182874745638, 0.330974440337}, 0.445263410512}};
    for (const Row& row : rows) {
      const GgxParams p = make_params(row.roughness, row.weight);
      const LobeEval e = ar::ggx_eval(p, row.wo, row.wi, TransportMode::Radiance);
      INFO("roughness=" << row.roughness << " weight=" << row.weight);
      for (int i = 0; i < 4; ++i) {
        if (row.f[i] == 0.0)
          CHECK(e.f.v[i] == Real(0));
        else
          CHECK(rel_close(double(e.f.v[i]), row.f[i], kPinTol));
      }
      CHECK(rel_close(double(e.pdf), row.pdf, kPinTol));
      CHECK(rel_close(double(ar::ggx_pdf(p, row.wo, row.wi, TransportMode::Radiance)), row.pdf, kPinTol));
      // f is reciprocal: F depends on wo.m = wi.m and G2, D are symmetric.
      const LobeEval sw = ar::ggx_eval(p, row.wi, row.wo, TransportMode::Radiance);
      for (int i = 0; i < 4; ++i) CHECK(rel_close(double(sw.f.v[i]), double(e.f.v[i]), 1e-5));
    }
    // Below the shading plane: no value, no density.
    const GgxParams p = make_params(0.5, 1.0);
    const LobeEval below = ar::ggx_eval(p, rows[0].wo, Real3{Real(0.6), Real(0), Real(-0.8)}, TransportMode::Radiance);
    CHECK(below.pdf == Real(0));
    CHECK(below.f.v[0] == Real(0));
    const LobeEval below_o = ar::ggx_eval(p, Real3{Real(0.6), Real(0), Real(-0.8)}, rows[0].wi, TransportMode::Radiance);
    CHECK(below_o.pdf == Real(0));
    CHECK(below_o.f.v[0] == Real(0));
  }

  AR_RT_CASE("lobe_ggx/pinned samples") {
    // Python: wi = reflect(wo, Listing 1 sampleGGXVNDF(wo, u)), pdf and f from the closed forms.
    struct Row {
      double roughness, weight;
      Real3 wo;
      double u1, u2;
      Real3 wi;
      double pdf, f[4];
    };
    const Row rows[] = {
        {0.5, 1, {Real(0.7642691913), Real(0.236416165329), Real(0.6)}, 0.3, 0.7,
         {Real(-0.336491330724), Real(-0.205578825667), Real(0.918972758456)}, 0.465168209731,
         {0.0100219222766, 0.0201837978094, 0.0219216367169, 0.000639015103047}},
        {0.1, 0.6, {Real(-0.181394200618), Real(0.396353559315), Real(0.9)}, 0.8, 0.2,
         {Real(0.185414377623), Real(-0.431375149458), Real(0.882913919361)}, 36.7155206488,
         {0.44144252374, 0.925327111332, 1.02991782066, 0.0224290039816}},
        {1, 1, {Real(0.837160408267), Real(-0.457342815435), Real(0.3)}, 0.55, 0.9,
         {Real(0.696065690236), Real(0.713628709827), Real(0.0789089309667)}, 0.122426879301,
         {0.00819100492088, 0.0165934831924, 0.0180874791031, 0.000504861418374}},
        {0.3, 1, {Real(0.761008367717), Real(0.640988505563), Real(0.1)}, 0.4, 0.45,
         {Real(-0.740812198631), Real(-0.6497834508), Real(0.170231470141)}, 25.6524447579,
         {60.6893185492, 60.8124311064, 42.2716014479, 42.8725164583}}};
    // The sampled direction is sensitive to rounding of the stretch/unstretch; allow a few ulp more.
    const double tol = kPinTol * 20;
    for (const Row& row : rows) {
      const LobeSample s = ar::ggx_sample(make_params(row.roughness, row.weight), row.wo,
                                          Real2{Real(row.u1), Real(row.u2)}, TransportMode::Radiance);
      INFO("roughness=" << row.roughness);
      CHECK(rel_close(double(s.wi.x), double(row.wi.x), tol));
      CHECK(rel_close(double(s.wi.y), double(row.wi.y), tol));
      CHECK(rel_close(double(s.wi.z), double(row.wi.z), tol));
      CHECK(rel_close(double(s.pdf), row.pdf, tol));
      for (int i = 0; i < 4; ++i) CHECK(rel_close(double(s.f.v[i]), row.f[i], tol));
      CHECK((s.flags & ar::LOBE_REFLECTION) != 0);
      CHECK((s.flags & ar::LOBE_DELTA) == 0);
    }
  }

  AR_RT_CASE("lobe_ggx/fresnel and specular weight") {
    // eta' = (1 + eps) / (1 - eps), eps = sgn(eta - 1) sqrt(xi F0) (OpenPBR); python values.
    CHECK(rel_close(double(ar::specular_weighted_eta(Real(1.5), Real(0.6))), 1.366637979197329, kPinTol));
    CHECK(rel_close(double(ar::specular_weighted_eta(Real(1.3), Real(0.6))), 1.2247791068665486, kPinTol));
    CHECK(rel_close(double(ar::specular_weighted_eta(Real(0.9), Real(0.6))), 0.921657398914605, kPinTol));
    CHECK(rel_close(double(ar::specular_weighted_eta(Real(1.5), Real(1.0))), 1.5, kPinTol));
    // Normal incidence: F = xi F0 exactly; F0 of 1.5 is 0.04.
    CHECK(rel_close(double(ar::fresnel_dielectric(Real(1), ar::specular_weighted_eta(Real(1.5), Real(1)))), 0.04, 1e-5));
    CHECK(rel_close(double(ar::fresnel_dielectric(Real(1), ar::specular_weighted_eta(Real(1.5), Real(0.6)))), 0.024, 1e-5));
    // Light arriving from inside (cos_i < 0): eta inverted and the sign of cos flipped. Python: the
    // general n1 -> n2 Fresnel form with n1 = eta, n2 = 1 (no inversion trick). Cases: no TIR,
    // TIR (1.5 at cos -0.5 exceeds the critical angle, F = 1), and eta < 1.
    struct Inside {
      double eta, cos_i, want;
    };
    const Inside inside[] = {{1.5, -0.9, 0.04633264795403766}, {1.5, -0.8, 0.1141411002213538}, {1.5, -0.5, 1.0},
                             {0.9, -0.5, 0.015241750401811504}, {0.9, -0.9, 0.0028875880223376512},
                             {1.3, -0.7, 0.10503818894518768}};
    for (const Inside& c : inside) {
      INFO("eta=" << c.eta << " cos_i=" << c.cos_i);
      CHECK(rel_close(double(ar::fresnel_dielectric(Real(c.cos_i), Real(c.eta))), c.want, kPinTol));
    }
    // Weight 0 removes the reflection; a large weight saturates F0 at 1 without a NaN.
    CHECK(ar::fresnel_dielectric(Real(0.5), ar::specular_weighted_eta(Real(1.5), Real(0))) == Real(0));
    const double f_big = double(ar::fresnel_dielectric(Real(1), ar::specular_weighted_eta(Real(1.5), Real(50))));
    CHECK(std::isfinite(f_big));
    CHECK(f_big > 0.99);
    CHECK(f_big <= 1.0);
    // Per lane at weight 0.6; cos = 1, 0.5, 0.1; the last lane (eta' < 1) reaches TIR at 0.1.
    const double want[3][4] = {{0.010207939508506637, 0.023999999999999983, 0.04032921810699588, 0.0016620498614958448},
                               {0.038482725134893266, 0.06593954698141256, 0.08961269995726066, 0.03655860582333388},
                               {0.49763174884497885, 0.5485298412543208, 0.5719068889431945, 1.0}};
    const double cosv[3] = {1.0, 0.5, 0.1};
    const GgxParams p = make_params(0.5, 0.6);
    for (int k = 0; k < 3; ++k) {
      const Spec f = ar::fresnel_dielectric_weighted(Real(cosv[k]), p.specular_ior, p.specular_weight);
      for (int l = 0; l < 4; ++l) CHECK(rel_close(double(f.v[l]), want[k][l], kPinTol));
    }
  }

  AR_RT_CASE("lobe_ggx/pdf integral") {
    // The VNDF pdf is a density over the upper hemisphere; reflections that leave it are lost, so
    // the integral is <= 1. The pins are the python quadrature of the same integral.
    struct Row {
      double roughness, mu, want;
    };
    const Row rows[] = {{0.1, 0.9, 0.99989427553838}, {0.1, 0.5, 0.9998253278913073}, {0.1, 0.1, 0.9975364322298702},
                        {0.5, 0.9, 0.9393897976583593}, {0.5, 0.5, 0.930008110410481},   {0.5, 0.1, 0.969170000808662},
                        {1.0, 0.9, 0.526314535715448},  {1.0, 0.5, 0.6666621227915798},  {1.0, 0.1, 0.9090716924498142}};
    for (const Row& row : rows) {
      const double integral = pdf_integral(make_params(row.roughness, 1.0), dir_from(row.mu, 0.9), 2000, 180);
      INFO("roughness=" << row.roughness << " mu=" << row.mu << " integral=" << integral);
      CHECK(integral <= 1.0 + 1e-3);
      CHECK(std::fabs(integral - row.want) <= 1e-3);
    }
  }

  AR_RT_CASE("lobe_ggx/eval equals sample") {
    // Deterministic 16x16 stratified uniforms; no random seeds in the fast tier. Eval recovers the
    // half vector from wo + wi, which in float loses up to ~1.2e-4 (worst case, analysed in review) on
    // the narrow roughness-0.1 lobe at grazing wo (the sampler holds the exact m); f64 meets 1e-5.
    constexpr double kSelfTol = kF32 ? 2e-4 : 1e-5;
    const double r_list[] = {0.1, 0.5, 1.0};
    const double mu_list[] = {0.1, 0.5, 1.0};
    int checked = 0;
    for (double r : r_list)
      for (double mu : mu_list) {
        const GgxParams p = make_params(r, 0.8);
        const Real3 wo = dir_from(mu, 2.1);
        for (int i = 0; i < 16; ++i)
          for (int j = 0; j < 16; ++j) {
            const Real2 u{Real((i + 0.5) / 16), Real((j + 0.5) / 16)};
            const LobeSample s = ar::ggx_sample(p, wo, u, TransportMode::Radiance);
            if (s.pdf == Real(0)) continue;  // reflected below the horizon: no sample
            const LobeEval e = ar::ggx_eval(p, wo, s.wi, TransportMode::Radiance);
            ++checked;
            INFO("r=" << r << " mu=" << mu << " i=" << i << " j=" << j);
            CHECK(rel_close(double(e.pdf), double(s.pdf), kSelfTol));
            for (int l = 0; l < 4; ++l) CHECK(rel_close(double(e.f.v[l]), double(s.f.v[l]), kSelfTol));
          }
      }
    CHECK(checked > 9 * 128);
  }

  AR_RT_CASE("lobe_ggx/peak at mirror direction") {
    // v1 lesson: a VNDF sampler can be self-consistent yet centred off the mirror direction.
    // Roughness 0.05 (alpha 0.0025): the density in a small cap around the mirror direction must
    // exceed the density in equal caps displaced by 0.08 rad in 8 directions.
    const GgxParams p = make_params(0.05, 1.0);
    const D3 wo = to_d3(dir_from(0.7, 0.8));
    const D3 mir = norm3(D3{-wo.x, -wo.y, wo.z});
    const D3 e1 = norm3(cross3(D3{0, 0, 1}, mir));
    const D3 e2 = cross3(mir, e1);
    const double rho = 0.02, cos_rho = std::cos(rho), off = 0.08;
    D3 centres[9];
    centres[0] = mir;
    for (int k = 1; k < 9; ++k) {
      const double a = 2.0 * kPi * k / 8.0;
      centres[k] = norm3(D3{mir.x + off * (std::cos(a) * e1.x + std::sin(a) * e2.x),
                            mir.y + off * (std::cos(a) * e1.y + std::sin(a) * e2.y),
                            mir.z + off * (std::cos(a) * e1.z + std::sin(a) * e2.z)});
    }
    int count[9] = {0};
    const int n = 100000;
    Rng rng{12345};
    int valid = 0;
    for (int k = 0; k < n; ++k) {
      const LobeSample s = ar::ggx_sample(p, to_real3(wo), Real2{Real(rng.next()), Real(rng.next())}, TransportMode::Radiance);
      if (s.pdf == Real(0)) continue;
      ++valid;
      const D3 w = to_d3(s.wi);
      for (int c = 0; c < 9; ++c)
        if (dot3(w, centres[c]) >= cos_rho) ++count[c];
    }
    CHECK(valid > n * 99 / 100);
    CHECK(count[0] > valid * 8 / 10);
    for (int c = 1; c < 9; ++c) {
      INFO("offset cap " << c << " count " << count[c] << " central " << count[0]);
      CHECK(count[0] > 20 * count[c]);
    }
  }

  AR_RT_CASE("lobe_ggx/delta below threshold") {
    // The single predicate: both alphas below 1e-3.
    CHECK(ar::lobe_is_delta(Real(0), Real(0)));
    CHECK(ar::lobe_is_delta(Real(9e-4), Real(9e-4)));
    CHECK(!ar::lobe_is_delta(Real(1.1e-3), Real(1.1e-3)));
    CHECK(!ar::lobe_is_delta(Real(1e-4), Real(1e-2)));
    CHECK(!ar::lobe_is_delta(Real(1e-2), Real(1e-4)));

    // Pins: F * color at weight 0.6 for cos = 0.5 and 0.1 (python, as in the fresnel case).
    const double want_f[2][4] = {{0.038482725134893266 * 1.0, 0.06593954698141256 * 0.9, 0.08961269995726066 * 0.6,
                                  0.03655860582333388 * 0.3},
                                 {0.49763174884497885 * 1.0, 0.5485298412543208 * 0.9, 0.5719068889431945 * 0.6, 1.0 * 0.3}};
    const double mus[2] = {0.5, 0.1};
    // Roughness 0 and 1e-4 (alpha 1e-8) are delta; so is 0.03 (alpha 9e-4); 0.032 (alpha 1.02e-3) is not.
    const double deltas[3] = {0.0, 1e-4, 0.03};
    for (double r : deltas)
      for (int k = 0; k < 2; ++k) {
        const GgxParams p = make_params(r, 0.6);
        const Real3 wo = dir_from(mus[k], 0.7);
        const LobeSample s = ar::ggx_sample(p, wo, Real2{Real(0.3), Real(0.6)}, TransportMode::Radiance);
        INFO("roughness=" << r << " mu=" << mus[k]);
        CHECK((s.flags & ar::LOBE_DELTA) != 0);
        CHECK((s.flags & ar::LOBE_REFLECTION) != 0);
        CHECK(s.pdf == Real(1));
        CHECK(rel_close(double(s.wi.x), -double(wo.x), 1e-6));
        CHECK(rel_close(double(s.wi.y), -double(wo.y), 1e-6));
        CHECK(rel_close(double(s.wi.z), double(wo.z), 1e-6));
        // The weight f cos / pdf is F tinted by specular_color.
        for (int l = 0; l < 4; ++l) CHECK(rel_close(double(s.f.v[l]) * double(s.wi.z) / double(s.pdf), want_f[k][l], kPinTol));
        // A delta lobe has no density or value away from the mirror direction.
        const LobeEval e = ar::ggx_eval(p, wo, s.wi, TransportMode::Radiance);
        CHECK(e.pdf == Real(0));
        CHECK(e.f.v[0] == Real(0));
        CHECK(ar::ggx_pdf(p, wo, s.wi, TransportMode::Radiance) == Real(0));
      }
    const GgxParams rough = make_params(0.032, 0.6);
    const LobeSample s = ar::ggx_sample(rough, dir_from(0.5, 0.7), Real2{Real(0.3), Real(0.6)}, TransportMode::Radiance);
    CHECK((s.flags & ar::LOBE_DELTA) == 0);
    CHECK(s.pdf > Real(0));
    // Below the shading plane even a delta lobe returns no sample.
    const LobeSample none = ar::ggx_sample(make_params(0.0, 1.0), Real3{Real(0.6), Real(0), Real(-0.8)}, Real2{Real(0.3), Real(0.6)},
                                           TransportMode::Radiance);
    CHECK(none.pdf == Real(0));
    CHECK(none.f.v[0] == Real(0));
  }

  AR_RT_CASE("lobe_ggx/boundary uniforms") {
    // u in [0, 1): 0 and the largest Real below 1 are legal; outputs are finite and pdf 0 means
    // "no sample" with f 0.
    const Real lo = Real(0), hi = std::nextafter(Real(1), Real(0));
    const Real vals[] = {lo, hi};
    const double r_list[] = {0.0, 0.05, 0.1, 0.5, 1.0};
    const double mu_list[] = {0.001, 0.05, 0.5, 1.0};
    for (double r : r_list)
      for (double mu : mu_list)
        for (Real u1 : vals)
          for (Real u2 : vals) {
            const GgxParams p = make_params(r, 1.0);
            const Real3 wo = dir_from(mu, 0.8);
            const LobeSample s = ar::ggx_sample(p, wo, Real2{u1, u2}, TransportMode::Radiance);
            INFO("r=" << r << " mu=" << mu << " u=(" << double(u1) << "," << double(u2) << ")");
            CHECK(std::isfinite(double(s.wi.x)));
            CHECK(std::isfinite(double(s.wi.y)));
            CHECK(std::isfinite(double(s.wi.z)));
            CHECK(std::isfinite(double(s.pdf)));
            for (int l = 0; l < 4; ++l) CHECK(std::isfinite(double(s.f.v[l])));
            if (s.pdf == Real(0)) {
              for (int l = 0; l < 4; ++l) CHECK(s.f.v[l] == Real(0));
            } else {
              CHECK(s.wi.z > Real(0));
              CHECK(s.pdf > Real(0));
            }
          }
    // wo on or below the horizon: no sample, no NaN.
    for (double z : {0.0, -0.5}) {
      const LobeSample s = ar::ggx_sample(make_params(0.5, 1.0), Real3{Real(1), Real(0), Real(z)}, Real2{Real(0.5), Real(0.5)},
                                          TransportMode::Radiance);
      CHECK(s.pdf == Real(0));
      CHECK(s.f.v[0] == Real(0));
    }
    // wi at the horizon and wi == -wo (half vector of zero length): zero, finite.
    const GgxParams p = make_params(0.5, 1.0);
    const LobeEval grazing = ar::ggx_eval(p, dir_from(0.5, 0.3), Real3{Real(1), Real(0), Real(0)}, TransportMode::Radiance);
    CHECK(grazing.pdf == Real(0));
    CHECK(grazing.f.v[0] == Real(0));
  }

  AR_RT_CASE("lobe_ggx/albedo quadrature pin") {
    // Same midpoint grid (300 z x 600 phi) as the python transcription, so the sums agree to
    // rounding; this pins the integrand f cos independently of the sampler.
    struct Row {
      double roughness, weight, mu;
      double want[4];
    };
    const Row rows[] = {{0.5, 1.0, 0.5, {0.03922834356317084, 0.060208889762190514, 0.055545360723600244, 0.0492027221642209}},
                        {1.0, 0.6, 0.3, {0.01304346491887492, 0.021374390867751787, 0.02066809566552088, 0.011271744271409789}}};
    for (const Row& row : rows) {
      const Spec a = albedo_quadrature(make_params(row.roughness, row.weight), dir_from(row.mu, 0.4), 300, 600);
      for (int l = 0; l < 4; ++l) CHECK(rel_close(double(a.v[l]), row.want[l], kF32 ? 1e-4 : 1e-8));
    }
  }
}

TEST_SUITE("gate") {
  AR_RT_CASE("lobe_ggx/energy vs quadrature") {
    // Consistency only (0009 d15 a): the mean of f cos / pdf over sampled directions must equal the
    // deterministic quadrature of f cos computed from the same eval.
    const st::Run run = st::begin("lobe_ggx/energy vs quadrature" AR_RT_SUFFIX, st::gate_family());
    constexpr int kN = 100000;
    std::printf("lobe_ggx energy: mean of f cos / pdf against quadrature of f cos, %d samples\n", kN);
    const double r_list[3] = {0.4, 0.7, 1.0};
    const double w_list[3] = {1.0, 0.6, 1.0};
    const double mu_list[3] = {0.3, 0.7, 0.95};
    int ci = 0;
    for (int ri = 0; ri < 3; ++ri)
      for (int mi = 0; mi < 3; ++mi) {
        const GgxParams p = make_params(r_list[ri], w_list[ri]);
        const Real3 wo = dir_from(mu_list[mi], 0.4);
        const Spec truth = albedo_quadrature(p, wo, 512, 1024);
        Rng rng{st::mix_bits(run.seed ^ uint64_t(++ci))};
        std::vector<double> w[4];
        for (int n = 0; n < kN; ++n) {
          const LobeSample s = ar::ggx_sample(p, wo, Real2{Real(rng.next()), Real(rng.next())}, TransportMode::Radiance);
          for (int l = 0; l < 4; ++l)
            w[l].push_back(s.pdf > Real(0) ? double(s.f.v[l]) * double(s.wi.z) / double(s.pdf) : 0.0);
        }
        for (int l = 0; l < 4; ++l) {
          const st::EnergyResult res = st::energy_test(run, w[l], double(truth.v[l]));
          std::printf("  r=%.2f xi=%.1f mu=%.2f lane %d  mean %.7f truth %.7f  error %+.2e  se %.1e  z %+.2f\n", r_list[ri],
                      w_list[ri], mu_list[mi], l, res.mean, double(truth.v[l]), res.mean - double(truth.v[l]), res.se, res.z);
          CHECK_MESSAGE(res.pass, res.message);
        }
      }
  }

  // Chi-squared of sampled directions against the pdf, in cells centred on the mirror direction:
  // bands of angle from it (log spaced from alpha / 2 to pi) times azimuth sectors. Expected
  // counts integrate ggx_pdf over each cell with a 48x48 midpoint rule in (angle, azimuth).
  static void chi2_roughness(double roughness, const char* name) {
    const st::Run run = st::begin(name, st::gate_family());
    const double alpha = roughness * roughness;
    const double mus[3] = {0.3, 0.6, 0.95};
    int ci = 0;
    for (double mu : mus) {
      const GgxParams p = make_params(roughness, 1.0);
      const D3 wo = to_d3(dir_from(mu, 0.7));
      const D3 mir = norm3(D3{-wo.x, -wo.y, wo.z});
      const D3 e1 = norm3(cross3(D3{0, 0, 1}, mir));
      const D3 e2 = cross3(mir, e1);
      const uint64_t seed = st::mix_bits(run.seed ^ uint64_t(++ci));
      auto gen = [&](int ires) {
        const int nb = ires, ns = 2 * ires;
        const int cells = nb * ns;
        const int n = 2000 * cells;
        const double lo = std::fmin(alpha * 0.5, kPi / 4);
        std::vector<double> edge(nb + 1);
        edge[0] = 0.0;
        for (int k = 1; k <= nb; ++k) edge[k] = kPi * std::exp(std::log(lo / kPi) * double(nb - k) / double(nb - 1));
        st::Histogram h;
        h.observed.assign(cells, 0.0);
        h.expected.assign(cells, 0.0);
        constexpr int kSub = 48;
        for (int b = 0; b < nb; ++b)
          for (int s = 0; s < ns; ++s) {
            double e = 0.0;
            for (int a = 0; a < kSub; ++a) {
              const double th = edge[b] + (edge[b + 1] - edge[b]) * (a + 0.5) / kSub;
              for (int c = 0; c < kSub; ++c) {
                const double ph = 2.0 * kPi * (s + (c + 0.5) / kSub) / ns;
                const double st_ = std::sin(th), ct = std::cos(th), cp = std::cos(ph), sp = std::sin(ph);
                const D3 wi{ct * mir.x + st_ * (cp * e1.x + sp * e2.x), ct * mir.y + st_ * (cp * e1.y + sp * e2.y),
                            ct * mir.z + st_ * (cp * e1.z + sp * e2.z)};
                e += double(ar::ggx_pdf(p, to_real3(wo), to_real3(wi), TransportMode::Radiance)) * st_ *
                     (edge[b + 1] - edge[b]) * (2.0 * kPi / ns);
              }
            }
            h.expected[b * ns + s] = n * e / (kSub * kSub);
          }
        Rng rng{seed};
        for (int k = 0; k < n; ++k) {
          const LobeSample smp = ar::ggx_sample(p, to_real3(wo), Real2{Real(rng.next()), Real(rng.next())}, TransportMode::Radiance);
          if (smp.pdf == Real(0)) continue;  // lost below the horizon; the expected counts already exclude it
          const D3 w = to_d3(smp.wi);
          const double th = std::acos(std::fmin(1.0, std::fmax(-1.0, dot3(w, mir))));
          double ph = std::atan2(dot3(w, e2), dot3(w, e1));
          if (ph < 0) ph += 2.0 * kPi;
          int b = int(std::upper_bound(edge.begin(), edge.end(), th) - edge.begin()) - 1;
          b = std::min(nb - 1, std::max(0, b));
          const int s = std::min(ns - 1, int(ph / (2.0 * kPi) * ns));
          h.observed[b * ns + s] += 1.0;
        }
        return h;
      };
      const st::Chi2Result res = st::chi2_with_retry(run, gen, 8);
      std::printf("lobe_ggx chi2 roughness=%.2f mu=%.2f: chi2 %.1f dof %d p %.4f%s\n", roughness, mu, res.chi2, res.dof, res.p,
                  res.retried ? " (retried at 16)" : "");
      CHECK_MESSAGE(res.pass, res.message);
    }
  }

  AR_RT_CASE("lobe_ggx/chi2 roughness 0.3") { chi2_roughness(0.3, "lobe_ggx/chi2 roughness 0.3" AR_RT_SUFFIX); }
  AR_RT_CASE("lobe_ggx/chi2 roughness 0.5") { chi2_roughness(0.5, "lobe_ggx/chi2 roughness 0.5" AR_RT_SUFFIX); }
  AR_RT_CASE("lobe_ggx/chi2 roughness 1") { chi2_roughness(1.0, "lobe_ggx/chi2 roughness 1" AR_RT_SUFFIX); }
}
