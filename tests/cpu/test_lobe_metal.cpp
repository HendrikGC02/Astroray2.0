// SPDX-License-Identifier: Apache-2.0
// ar-f64: compiled twice into this executable, once with AR_REAL_DOUBLE (cmake/sources.cmake).
// F82-tint metal lobe (0005 items 4, 7, 8, 12; 0009 d14-d15; 0004 D19).
// Ref: OpenPBR Surface 1.1.2 index.html lines 545-563 (F82-tint, Kutz, Hasan, Edmondson 2021;
// Hoffman 2019); Walter et al. 2007 Eq. 33; Heitz 2014 Eq. 72, 99; Heitz 2018 Eq. 3. Every pinned
// literal below comes from an independent float64 NumPy transcription of those equations
// (F_Schlick and F_82 written from the spec text with mu_bar = 1/7, D in tan form, Lambda, G2,
// D_wo, pdf = D_wo(m) / (4 wo.m)), not from the code under test.
// Lane setup of the script: base_color {.9, .7, .5, .95}, specular_color (tint) {1, .8, .6, .3};
// directions are (z, phi) -> (sqrt(1 - z^2) cos phi, sqrt(1 - z^2) sin phi, z).
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include "common/stat.h"
#include "material/lobes/metal.h"

namespace st = astroray::stat;

namespace {

using ar::GgxParams;
using ar::LobeEval;
using ar::LobeSample;
using ar::MetalParams;
using ar::Real;
using ar::Real2;
using ar::Real3;
using ar::Spec;
using ar::TransportMode;

constexpr double kPi = 3.14159265358979323846;
constexpr bool kF32 = sizeof(Real) == 4;
constexpr double kPinTol = kF32 ? 5e-5 : 1e-9;
constexpr double kEps = std::numeric_limits<Real>::epsilon();

const double kBase[4] = {0.9, 0.7, 0.5, 0.95};
const double kTint[4] = {1.0, 0.8, 0.6, 0.3};

MetalParams make_params(double roughness, double base_weight = 1.0, double specular_weight = 1.0) {
  MetalParams p;
  for (int i = 0; i < ar::kLanes; ++i) {
    p.base_color.v[i] = Real(kBase[i]);
    p.specular_color.v[i] = Real(kTint[i]);
  }
  p.base_weight = Real(base_weight);
  p.specular_weight = Real(specular_weight);
  p.roughness = Real(roughness);
  return p;
}

Spec spec_of(const double (&v)[4]) {
  Spec s;
  for (int i = 0; i < 4; ++i) s.v[i] = Real(v[i]);
  return s;
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

// Integral over wi by the change of variables wi = reflect(wo, m), dwi = 4 (wo.m) dm, midpoint rule
// in (s, phi) with tan(theta_m) = alpha tan(pi s / 2), which resolves lobes down to roughness 0.1
// (alpha 0.01). Accumulates both the pdf and f cos(wi) per lane.
struct HalfQuad {
  double pdf;
  double fcos[4];
};
HalfQuad half_vector_quadrature(const MetalParams& p, Real3 wo, int ns, int nphi) {
  const double alpha = double(p.roughness) * double(p.roughness);
  const D3 o = to_d3(wo);
  HalfQuad q{0.0, {0, 0, 0, 0}};
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
      if (wi.z <= 0.0) continue;
      const double jac = 4.0 * om * std::sin(th) * dth * (2.0 * kPi / nphi);
      const LobeEval e = ar::metal_eval(p, wo, to_real3(wi), TransportMode::Radiance);
      q.pdf += double(ar::metal_pdf(p, wo, to_real3(wi), TransportMode::Radiance)) * jac;
      for (int l = 0; l < 4; ++l) q.fcos[l] += double(e.f.v[l]) * wi.z * jac;
    }
  }
  return q;
}

}  // namespace

TEST_SUITE("fast") {
  AR_RT_CASE("lobe_metal/pinned values") {
    struct Row {
      double roughness, base_weight, specular_weight;
      Real3 wo, wi;
      double f[4], pdf;
    };
    const Row rows[] = {
        {0.5, 1.0, 1.0, {Real(0.7642691913004849), Real(0.23641616532907164), Real(0.6)},
         {Real(-0.705201610143812), Real(-0.1126529584634818), Real(0.7)},
         {1.9280752662211458, 1.4948281528545555, 1.0634154200008619, 2.0055287364579435}, 1.52214217170034},
        {0.1, 0.8, 1.0, {Real(-0.1813942006183132), Real(0.3963535593154716), Real(0.9)},
         {Real(0.17952948073041322), Real(-0.4397376098864733), Real(0.88)},
         {14.988636800082634, 11.65781500053242, 8.32701284481898, 15.820972155278318}, 18.319465035537828},
        {1.0, 1.0, 0.7, {Real(0.8371604082671394), Real(-0.45734281543498245), Real(0.3)},
         {Real(-0.6938107231433583), Real(0.5182920802513676), Real(0.5)},
         {0.12635129576651885, 0.0932630960233445, 0.06203767181533929, 0.10328737097389772}, 0.12242687930145794},
        {0.3, 1.0, 1.0, {Real(0.7610083677172147), Real(0.6409885055633843), Real(0.1)},
         {Real(-0.605980414401263), Real(-0.6875956205227557), Real(0.4)},
         {1.2032402791644161, 0.8359530750929683, 0.5104510779466203, 0.5921431489930621}, 0.5268298218237664},
        {0.7, 0.6, 0.5, {Real(0.14163534351920037), Real(0.2782794089870793), Real(0.95)},
         {Real(-0.6404373378964295), Real(-0.7415119798277937), Real(0.2)},
         {0.07886664730899584, 0.061155399556769685, 0.0435032464896807, 0.08198234097310016}, 0.1045116238271171},
        {0.2, 1.0, 1.0, {Real(0.8752274397884532), Real(0.17741738539768184), Real(0.45)},
         {Real(-0.8372717951066933), Real(-0.22130508606630694), Real(0.5)},
         {18.0872880529511, 13.659282856350593, 9.381039785030854, 16.67496156167819}, 10.01650268367524}};
    for (const Row& row : rows) {
      const MetalParams p = make_params(row.roughness, row.base_weight, row.specular_weight);
      const LobeEval e = ar::metal_eval(p, row.wo, row.wi, TransportMode::Radiance);
      INFO("roughness=" << row.roughness << " base_weight=" << row.base_weight << " specular_weight=" << row.specular_weight);
      for (int i = 0; i < 4; ++i) CHECK(rel_close(double(e.f.v[i]), row.f[i], kPinTol));
      CHECK(rel_close(double(e.pdf), row.pdf, kPinTol));
      CHECK(rel_close(double(ar::metal_pdf(p, row.wo, row.wi, TransportMode::Radiance)), row.pdf, kPinTol));
      // One GGX: the metal density is the dielectric lobe's at the same roughness.
      GgxParams g;
      g.specular_color = ar::spec_fill(Real(1));
      g.specular_ior = ar::spec_fill(Real(1.5));
      g.specular_weight = Real(1);
      g.roughness = p.roughness;
      CHECK(ar::metal_pdf(p, row.wo, row.wi, TransportMode::Radiance) ==
            ar::ggx_pdf(g, row.wo, row.wi, TransportMode::Radiance));
      // f is reciprocal: F depends on wo.m = wi.m and G2, D are symmetric.
      const LobeEval sw = ar::metal_eval(p, row.wi, row.wo, TransportMode::Radiance);
      for (int i = 0; i < 4; ++i) CHECK(rel_close(double(sw.f.v[i]), double(e.f.v[i]), 1e-5));
      // TransportMode does not change a reflection.
      const LobeEval imp = ar::metal_eval(p, row.wo, row.wi, TransportMode::Importance);
      for (int i = 0; i < 4; ++i) CHECK(imp.f.v[i] == e.f.v[i]);
    }
    // Below the shading plane, or wi = -wo: no value, no density.
    const MetalParams p = make_params(0.5);
    const Real3 below{Real(0.6), Real(0), Real(-0.8)};
    for (const LobeEval& z : {ar::metal_eval(p, rows[0].wo, below, TransportMode::Radiance),
                              ar::metal_eval(p, below, rows[0].wi, TransportMode::Radiance),
                              ar::metal_eval(p, dir_from(0.5, 0.3), Real3{Real(1), Real(0), Real(0)}, TransportMode::Radiance)}) {
      CHECK(z.pdf == Real(0));
      for (int i = 0; i < 4; ++i) CHECK(z.f.v[i] == Real(0));
    }
    CHECK(ar::metal_pdf(p, rows[0].wo, below, TransportMode::Radiance) == Real(0));
  }

  AR_RT_CASE("lobe_metal/f82 fresnel") {
    const Spec f0 = spec_of(kBase);
    const Spec tint = spec_of(kTint);
    // Python pins of F_82 at base_weight 1.
    struct Pin {
      double mu, f[4];
    };
    const Pin pins[] = {{0.8, {0.900032, 0.6999443868751714, 0.4998956231550069, 0.9494003709903978}},
                        {0.5, {0.9031250000000001, 0.6862406730913494, 0.475284294892404, 0.8576249680172432}},
                        {0.05, {0.97737809375, 0.823296802174053, 0.6971040675551945, 0.5467516517655534}},
                        {0.0, {1.0, 1.0, 1.0, 1.0}}};
    for (const Pin& pin : pins) {
      const Spec f = ar::fresnel_f82(f0, tint, Real(pin.mu));
      INFO("mu=" << pin.mu);
      for (int l = 0; l < 4; ++l) CHECK(rel_close(double(f.v[l]), pin.f[l], kPinTol));
    }
    // Normal incidence returns F0 to the float floor.
    const Spec at1 = ar::fresnel_f82(f0, tint, Real(1));
    for (int l = 0; l < 4; ++l) CHECK(std::fabs(double(at1.v[l]) - double(f0.v[l])) <= 2 * kEps);
    // At mu_bar the edge reflectivity is the tint times Schlick (Hoffman's definition of the tint).
    const double schlick_bar[4] = {0.9462664366037961, 0.8387993098113882, 0.7313321830189803, 0.973133218301898};
    const Spec at_bar = ar::fresnel_f82(f0, tint, ar::kF82MuBar);
    for (int l = 0; l < 4; ++l) {
      CHECK(rel_close(double(ar::fresnel_schlick(f0.v[l], ar::kF82MuBar)), schlick_bar[l], kPinTol));
      CHECK(rel_close(double(at_bar.v[l]), schlick_bar[l] * kTint[l], 8 * kEps));
    }
    // Tint 1 is plain Schlick at every angle.
    const Spec one = ar::spec_fill(Real(1));
    for (double mu : {1.0, 0.9, 0.5, 0.3, 1.0 / 7.0, 0.1, 0.01, 0.0}) {
      const Spec f = ar::fresnel_f82(f0, one, Real(mu));
      for (int l = 0; l < 4; ++l) {
        const double s = kBase[l] + (1.0 - kBase[l]) * std::pow(1.0 - mu, 5.0);
        CHECK(rel_close(double(f.v[l]), s, 8 * kEps));
      }
    }
    // cos outside [0, 1] is clamped.
    const Spec hi = ar::fresnel_f82(f0, tint, Real(1.5));
    const Spec lo = ar::fresnel_f82(f0, tint, Real(-0.2));
    const Spec lo0 = ar::fresnel_f82(f0, tint, Real(0));
    for (int l = 0; l < 4; ++l) {
      CHECK(hi.v[l] == at1.v[l]);
      CHECK(lo.v[l] == lo0.v[l]);
    }
    // Dark, untinted input: the raw F_82 at F0 = 0, tint = 0 is (1 - mu)^5 (1 - (49/6) mu (1 - mu)),
    // -0.0325520833 at mu = 0.5; each lane is saturated to [0, 1] (openpbr-bsdf), so F = 0 there.
    const Spec zero = ar::spec_fill(Real(0));
    CHECK(ar::fresnel_f82(zero, zero, Real(0.5)).v[0] == Real(0));
    for (int k = 0; k <= 100; ++k) {
      const Spec f = ar::fresnel_f82(ar::spec_fill(Real(0.02)), zero, Real(k / 100.0));
      CHECK(f.v[0] >= Real(0));
      CHECK(f.v[0] <= Real(1));
    }
    // The lobe's value on such a lane is 0, never negative: wo.m = 0.5 with m the normal.
    MetalParams dark = make_params(0.5);
    dark.base_color.v[0] = Real(0);
    dark.specular_color.v[0] = Real(0);
    const LobeEval de = ar::metal_eval(dark, dir_from(0.5, 0.0), dir_from(0.5, kPi), TransportMode::Radiance);
    CHECK(de.pdf > Real(0));
    CHECK(de.f.v[0] >= Real(0));
    CHECK(de.f.v[0] == Real(0));
    CHECK(de.f.v[1] > Real(0));
    // The lobe's F: specular_weight * F_82 with F0 = base_weight * base_color.
    const Spec fm = ar::metal_fresnel(make_params(0.5, 1.0, 0.7), Real(0.5));
    for (int l = 0; l < 4; ++l) CHECK(rel_close(double(fm.v[l]), 0.7 * pins[1].f[l], kPinTol));
    const Spec fw = ar::metal_fresnel(make_params(0.5, 0.6, 1.0), Real(1));
    for (int l = 0; l < 4; ++l) CHECK(rel_close(double(fw.v[l]), 0.6 * kBase[l], kPinTol));
  }

  // The VNDF pdf is a density over the upper hemisphere; reflections that leave it are lost. One case
  // per roughness so that no case nears the per-test budget (0009 B-12).
  static void check_pdf_integral(double r) {
    for (double mu : {0.9, 0.5, 0.1}) {
      const double integral = half_vector_quadrature(make_params(r), dir_from(mu, 0.9), 2000, 180).pdf;
      INFO("roughness=" << r << " mu=" << mu << " integral=" << integral);
      CHECK(integral <= 1.0 + 1e-3);
      CHECK(integral > 0.5);
    }
  }
  AR_RT_CASE("lobe_metal/pdf integral roughness 0.1") { check_pdf_integral(0.1); }
  AR_RT_CASE("lobe_metal/pdf integral roughness 0.5") { check_pdf_integral(0.5); }
  AR_RT_CASE("lobe_metal/pdf integral roughness 1.0") { check_pdf_integral(1.0); }

  AR_RT_CASE("lobe_metal/eval equals sample") {
    // Deterministic 16x16 stratified uniforms; no random seeds in the fast tier.
    constexpr double kSelfTol = 1e-5;
    int checked = 0;
    for (double r : {0.2, 0.5, 1.0})
      for (double mu : {0.3, 0.6, 1.0}) {
        const MetalParams p = make_params(r, 0.9, 0.8);
        const Real3 wo = dir_from(mu, 2.1);
        for (int i = 0; i < 16; ++i)
          for (int j = 0; j < 16; ++j) {
            const Real2 u{Real((i + 0.5) / 16), Real((j + 0.5) / 16)};
            const LobeSample s = ar::metal_sample(p, wo, u, TransportMode::Radiance);
            if (s.pdf == Real(0)) continue;  // reflected below the horizon: no sample
            CHECK((s.flags & ar::LOBE_REFLECTION) != 0);
            CHECK((s.flags & ar::LOBE_DELTA) == 0);
            const LobeEval e = ar::metal_eval(p, wo, s.wi, TransportMode::Radiance);
            ++checked;
            INFO("r=" << r << " mu=" << mu << " i=" << i << " j=" << j);
            CHECK(rel_close(double(e.pdf), double(s.pdf), kSelfTol));
            for (int l = 0; l < 4; ++l) CHECK(rel_close(double(e.f.v[l]), double(s.f.v[l]), kSelfTol));
          }
      }
    CHECK(checked > 9 * 128);
  }

  AR_RT_CASE("lobe_metal/delta below threshold") {
    // Pins: specular_weight 0.7 times F_82 at cos 0.5 and 0.05 (python, as in the fresnel case).
    const double f82[2][4] = {{0.9031250000000001, 0.6862406730913494, 0.475284294892404, 0.8576249680172432},
                              {0.97737809375, 0.823296802174053, 0.6971040675551945, 0.5467516517655534}};
    const double mus[2] = {0.5, 0.05};
    for (double r : {0.0, 1e-4})
      for (int k = 0; k < 2; ++k) {
        const MetalParams p = make_params(r, 1.0, 0.7);
        const Real3 wo = dir_from(mus[k], 0.7);
        const LobeSample s = ar::metal_sample(p, wo, Real2{Real(0.3), Real(0.6)}, TransportMode::Radiance);
        INFO("roughness=" << r << " mu=" << mus[k]);
        CHECK((s.flags & ar::LOBE_DELTA) != 0);
        CHECK((s.flags & ar::LOBE_REFLECTION) != 0);
        CHECK(s.pdf == Real(1));
        CHECK(s.wi.x == -wo.x);
        CHECK(s.wi.y == -wo.y);
        CHECK(s.wi.z == wo.z);
        // The weight f cos / pdf is F.
        for (int l = 0; l < 4; ++l)
          CHECK(rel_close(double(s.f.v[l]) * double(s.wi.z) / double(s.pdf), 0.7 * f82[k][l], kPinTol));
        // A delta lobe has no density or value, even at the mirror direction.
        const LobeEval e = ar::metal_eval(p, wo, s.wi, TransportMode::Radiance);
        CHECK(e.pdf == Real(0));
        for (int l = 0; l < 4; ++l) CHECK(e.f.v[l] == Real(0));
        CHECK(ar::metal_pdf(p, wo, s.wi, TransportMode::Radiance) == Real(0));
      }
    // Just above the threshold (alpha 1.02e-3) the lobe is sampled as a microfacet lobe.
    const LobeSample s = ar::metal_sample(make_params(0.032), dir_from(0.5, 0.7), Real2{Real(0.3), Real(0.6)},
                                          TransportMode::Radiance);
    CHECK((s.flags & ar::LOBE_DELTA) == 0);
    CHECK(s.pdf > Real(0));
    // Below the shading plane even a delta lobe returns no sample.
    const LobeSample none = ar::metal_sample(make_params(0.0), Real3{Real(0.6), Real(0), Real(-0.8)}, Real2{Real(0.3), Real(0.6)},
                                             TransportMode::Radiance);
    CHECK(none.pdf == Real(0));
    for (int l = 0; l < 4; ++l) CHECK(none.f.v[l] == Real(0));
  }

  AR_RT_CASE("lobe_metal/grazing is finite") {
    // wo.z = 1e-4: every output finite; pdf 0 means "no sample" with f 0.
    const Real lo = Real(0), hi = std::nextafter(Real(1), Real(0));
    const Real3 wo = dir_from(1e-4, 0.8);
    for (double r : {0.0, 1e-4, 0.05, 0.1, 0.5, 1.0}) {
      const MetalParams p = make_params(r);
      for (int i = 0; i < 9; ++i)
        for (int j = 0; j < 9; ++j) {
          const Real u1 = i == 8 ? hi : (i == 0 ? lo : Real(i / 8.0));
          const Real u2 = j == 8 ? hi : (j == 0 ? lo : Real(j / 8.0));
          const LobeSample s = ar::metal_sample(p, wo, Real2{u1, u2}, TransportMode::Radiance);
          INFO("r=" << r << " u=(" << double(u1) << "," << double(u2) << ")");
          CHECK(std::isfinite(double(s.wi.x)));
          CHECK(std::isfinite(double(s.wi.y)));
          CHECK(std::isfinite(double(s.wi.z)));
          CHECK(std::isfinite(double(s.pdf)));
          for (int l = 0; l < 4; ++l) CHECK(std::isfinite(double(s.f.v[l])));
          if (s.pdf == Real(0)) {
            for (int l = 0; l < 4; ++l) CHECK(s.f.v[l] == Real(0));
          } else {
            CHECK(s.wi.z > Real(0));
          }
        }
      for (double z : {1e-4, 0.01, 0.5, 1.0}) {
        const LobeEval e = ar::metal_eval(p, wo, dir_from(z, 4.0), TransportMode::Radiance);
        CHECK(std::isfinite(double(e.pdf)));
        for (int l = 0; l < 4; ++l) CHECK(std::isfinite(double(e.f.v[l])));
      }
    }
  }
}

TEST_SUITE("gate") {
  AR_RT_CASE("lobe_metal/energy vs quadrature") {
    // Consistency only (0009 d15 a): the mean of f cos / pdf over sampled directions must equal the
    // deterministic half-vector quadrature of f cos computed from the same eval.
    const st::Run run = st::begin("lobe_metal/energy vs quadrature" AR_RT_SUFFIX, st::gate_family());
    constexpr int kN = 100000;
    std::printf("lobe_metal energy: mean of f cos / pdf against quadrature of f cos, %d samples\n", kN);
    int ci = 0;
    for (double r : {0.1, 0.5, 1.0})
      for (double mu : {0.3, 0.8}) {
        const MetalParams p = make_params(r, 0.9, 0.8);
        const Real3 wo = dir_from(mu, 0.4);
        const HalfQuad truth = half_vector_quadrature(p, wo, 1024, 256);
        Rng rng{st::mix_bits(run.seed ^ uint64_t(++ci))};
        std::vector<double> w[4];
        for (int n = 0; n < kN; ++n) {
          const LobeSample s = ar::metal_sample(p, wo, Real2{Real(rng.next()), Real(rng.next())}, TransportMode::Radiance);
          for (int l = 0; l < 4; ++l)
            w[l].push_back(s.pdf > Real(0) ? double(s.f.v[l]) * double(s.wi.z) / double(s.pdf) : 0.0);
        }
        for (int l = 0; l < 4; ++l) {
          const st::EnergyResult res = st::energy_test(run, w[l], truth.fcos[l]);
          std::printf("  r=%.2f mu=%.2f lane %d  mean %.7f truth %.7f  error %+.2e  se %.1e  z %+.2f\n", r, mu, l, res.mean,
                      truth.fcos[l], res.mean - truth.fcos[l], res.se, res.z);
          CHECK_MESSAGE(res.pass, res.message);
        }
      }
  }

  // Chi-squared of sampled directions against the pdf, in cells centred on the mirror direction:
  // bands of angle from it (log spaced from alpha / 2 to pi) times azimuth sectors. Expected
  // counts integrate metal_pdf over each cell with a 48x48 midpoint rule in (angle, azimuth).
  static void chi2_roughness(double roughness, const char* name) {
    const st::Run run = st::begin(name, st::gate_family());
    const double alpha = roughness * roughness;
    int ci = 0;
    for (double mu : {0.3, 0.8}) {
      const MetalParams p = make_params(roughness, 0.9, 0.8);
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
                e += double(ar::metal_pdf(p, to_real3(wo), to_real3(wi), TransportMode::Radiance)) * st_ *
                     (edge[b + 1] - edge[b]) * (2.0 * kPi / ns);
              }
            }
            h.expected[b * ns + s] = n * e / (kSub * kSub);
          }
        Rng rng{seed};
        for (int k = 0; k < n; ++k) {
          const LobeSample smp =
              ar::metal_sample(p, to_real3(wo), Real2{Real(rng.next()), Real(rng.next())}, TransportMode::Radiance);
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
      std::printf("lobe_metal chi2 roughness=%.2f mu=%.2f: chi2 %.1f dof %d p %.4f%s\n", roughness, mu, res.chi2, res.dof, res.p,
                  res.retried ? " (retried at 16)" : "");
      CHECK_MESSAGE(res.pass, res.message);
    }
  }

  AR_RT_CASE("lobe_metal/chi2 roughness 0.1") { chi2_roughness(0.1, "lobe_metal/chi2 roughness 0.1" AR_RT_SUFFIX); }
  AR_RT_CASE("lobe_metal/chi2 roughness 0.5") { chi2_roughness(0.5, "lobe_metal/chi2 roughness 0.5" AR_RT_SUFFIX); }
  AR_RT_CASE("lobe_metal/chi2 roughness 1") { chi2_roughness(1.0, "lobe_metal/chi2 roughness 1" AR_RT_SUFFIX); }
}
