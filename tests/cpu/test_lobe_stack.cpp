// SPDX-License-Identifier: Apache-2.0
// M1a lobe stack: the E_diel table, its lookup and the glossy-diffuse layering (0005 items 4-6
// and Verification 1-2; 0009 d15).
// Ref: OpenPBR Surface 1.1.2 layering by albedo scaling; Kulla and Conty 2017. Pins that are
// not self-references come from an independent NumPy transcription (table read from the file,
// trilinear lookup, specular_weight eta modulation of the OpenPBR reference mtlx, and the EON
// paper's Listing 1 albedo), printed with 10 digits; the r = 0 table cells are checked against
// the unpolarised Fresnel equations written out in this file.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../../src/host/data_resource.h"
#include "common/stat.h"
#include "material/lobes/stack.h"

namespace st = astroray::stat;

namespace {

using ar::EDielTable;
using ar::Frame;
using ar::LobeEval;
using ar::LobeSample;
using ar::OpenPBRParams;
using ar::Real;
using ar::Real2;
using ar::Real3;
using ar::Spec;
using ar::StackSelect;
using ar::TransportMode;

constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kN = ar::kEDielN;

// The committed table as embedded in this executable (0001 D8.4), copied so that it is aligned.
struct Fixture {
  std::vector<uint8_t> bytes;
  EDielTable table;
  Fixture() {
    astroray::DataBlob blob;
    REQUIRE_MESSAGE(astroray::find_data("tables/e_diel_refl.bin", &blob), "tables/e_diel_refl.bin is not embedded");
    bytes.assign(blob.data, blob.data + blob.size);
    const char* err = ar::e_diel_view(bytes.data(), bytes.size(), &table);
    REQUIRE_MESSAGE(err == nullptr, (err ? err : ""));
  }
  // Raw node value read straight from the bytes, independent of the lookup code.
  double raw(uint32_t imu, uint32_t ir, uint32_t ieta) const {
    float v;
    std::memcpy(&v, bytes.data() + ar::kETableHeaderBytes + 4 * ((size_t(ieta) * kN + ir) * kN + imu), 4);
    return double(v);
  }
};

OpenPBRParams make_params(const double color[4], const double ior[4], double weight, double rough, double base_weight,
                          const double base[4], double diff_rough, double closure) {
  OpenPBRParams p = {};
  for (int i = 0; i < ar::kLanes; ++i) {
    p.specular_color.v[i] = Real(color[i]);
    p.specular_ior.v[i] = Real(ior[i]);
    p.base_color.v[i] = Real(base[i]);
  }
  p.specular_weight = Real(weight);
  p.specular_roughness = Real(rough);
  p.base_weight = Real(base_weight);
  p.base_diffuse_roughness = Real(diff_rough);
  p.closure_weight = Real(closure);
  return p;
}

const double kOnes[4] = {1, 1, 1, 1};
const double kIor15[4] = {1.5, 1.5, 1.5, 1.5};

OpenPBRParams white_params(double rough) { return make_params(kOnes, kIor15, 1.0, rough, 1.0, kOnes, 0.0, 1.0); }

// A frame that is not the identity, so that the world/local moves are exercised.
Frame tilted_frame() {
  const double x = 0.3, y = -0.5, z = 0.8;
  const double l = std::sqrt(x * x + y * y + z * z);
  return ar::frame_from_normal(Real3{Real(x / l), Real(y / l), Real(z / l)});
}

Real3 local_dir(double z, double phi) {
  const double s = std::sqrt(std::fmax(0.0, 1.0 - z * z));
  return Real3{Real(s * std::cos(phi)), Real(s * std::sin(phi)), Real(z)};
}

bool rel_close(double got, double want, double tol) { return std::fabs(got - want) <= tol * std::fmax(std::fabs(want), 1e-30); }

// Unpolarised dielectric Fresnel by the textbook amplitudes, double precision.
double fresnel_ref(double cos_i, double eta) {
  const double sin2_t = (1.0 - cos_i * cos_i) / (eta * eta);
  const double cos_t = std::sqrt(1.0 - sin2_t);
  const double rp = (eta * cos_i - cos_t) / (eta * cos_i + cos_t);
  const double rs = (cos_i - eta * cos_t) / (cos_i + eta * cos_t);
  return 0.5 * (rp * rp + rs * rs);
}

// Midpoint rule of ggx f cos over the hemisphere (independent of the sampler), lane 0.
double ggx_albedo_quadrature(const ar::GgxParams& g, Real3 wo, int nz, int nphi) {
  double acc = 0;
  for (int i = 0; i < nz; ++i) {
    const double z = (i + 0.5) / nz;
    for (int j = 0; j < nphi; ++j) {
      const double phi = 2.0 * kPi * (j + 0.5) / nphi;
      const LobeEval e = ar::ggx_eval(g, wo, local_dir(z, phi), TransportMode::Radiance);
      acc += double(e.f.v[0]) * z;
    }
  }
  return acc / nz * (2.0 * kPi / nphi);
}

// splitmix64 hash to a double in [0, 1).
double hash_unit(uint64_t z) {
  z += 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  return double(z >> 11) * (1.0 / 9007199254740992.0);
}

}  // namespace

TEST_SUITE("fast") {
  TEST_CASE("lobe_stack/table lookup") {
    Fixture fx;
    // Header: generator and sample count recorded, the lobe hash is a real SHA-256 (not zero).
    CHECK(ar::etable_u32(fx.bytes.data() + 16) == 1u);
    CHECK(ar::etable_u32(fx.bytes.data() + ar::kETableSamplesOffset) == 65536u);
    bool any = false;
    for (int i = 0; i < 32; ++i) any = any || fx.bytes[ar::kETableLobeHashOffset + i] != 0;
    CHECK(any);

    // At every grid point the lookup returns the stored value.
    int bad = 0;
    for (uint32_t ie = 0; ie < kN; ++ie)
      for (uint32_t ir = 0; ir < kN; ++ir)
        for (uint32_t im = 0; im < kN; ++im) {
          const Real mu = Real(im) / Real(kN - 1), r = Real(ir) / Real(kN - 1);
          const Real eta = Real(1) + Real(2) * Real(ie) / Real(kN - 1);
          const double got = double(ar::e_diel_lookup(fx.table, mu, r, eta));
          if (std::fabs(got - fx.raw(im, ir, ie)) > 1e-6) ++bad;
        }
    CHECK(bad == 0);

    // At a cell centre trilinear interpolation is the mean of the 8 corner nodes.
    {
      const uint32_t im = 6, ir = 3, ie = 9;
      double mean = 0;
      for (int c = 0; c < 8; ++c) mean += fx.raw(im + (c & 1), ir + ((c >> 1) & 1), ie + ((c >> 2) & 1)) / 8.0;
      const Real mu = (Real(im) + Real(0.5)) / Real(kN - 1), r = (Real(ir) + Real(0.5)) / Real(kN - 1);
      const Real eta = Real(1) + Real(2) * (Real(ie) + Real(0.5)) / Real(kN - 1);
      CHECK(std::fabs(double(ar::e_diel_lookup(fx.table, mu, r, eta)) - mean) < 1e-6);
    }
    // The last cell interpolates up to node 15, and arguments clamp to the axes.
    CHECK(std::fabs(double(ar::e_diel_lookup(fx.table, Real(1), Real(1), Real(3))) - fx.raw(15, 15, 15)) < 1e-6);
    CHECK(std::fabs(double(ar::e_diel_lookup(fx.table, Real(2), Real(7), Real(9))) - fx.raw(15, 15, 15)) < 1e-6);
    CHECK(std::fabs(double(ar::e_diel_lookup(fx.table, Real(-1), Real(-1), Real(0.2))) - fx.raw(0, 0, 0)) < 1e-6);

    // Smooth cells (r = 0) are the Fresnel reflectance itself, which pins the table to the physics
    // independently of the generator's sampler. Node mu = 0 is evaluated at kEDielMuFloor.
    for (uint32_t ie = 0; ie < kN; ++ie)
      for (uint32_t im = 0; im < kN; ++im) {
        const double eta = 1.0 + 2.0 * ie / (kN - 1);
        const double mu = std::fmax(double(im) / (kN - 1), double(ar::kEDielMuFloor));
        INFO("eta=" << eta << " mu=" << mu);
        // 3e-5: at eta = 1 the float Fresnel leaves ~1e-5 of cancellation noise where the exact value is 0.
        CHECK(std::fabs(fx.raw(im, 0, ie) - fresnel_ref(mu, eta)) < 3e-5);
      }

    // Rough cells against a midpoint quadrature of f cos from the lobe's eval (a different integrator
    // from the generator's importance sampling).
    struct Node {
      uint32_t im, ir, ie;
    };
    for (const Node& n : {Node{8, 6, 4}, Node{3, 12, 9}, Node{14, 15, 13}}) {
      ar::GgxParams g;
      g.specular_color = ar::spec_fill(Real(1));
      g.specular_ior = ar::spec_fill(Real(1) + Real(2) * Real(n.ie) / Real(kN - 1));
      g.specular_weight = Real(1);
      g.roughness = Real(n.ir) / Real(kN - 1);
      const double mu = double(n.im) / (kN - 1);
      const double q = ggx_albedo_quadrature(g, local_dir(mu, 0.0), 400, 800);
      INFO("node mu=" << n.im << " r=" << n.ir << " eta=" << n.ie << " quadrature " << q << " table " << fx.raw(n.im, n.ir, n.ie));
      CHECK(std::fabs(q - fx.raw(n.im, n.ir, n.ie)) < 5e-4);
    }
  }

  TEST_CASE("lobe_stack/selection shared") {
    Fixture fx;
    const Frame frame = tilted_frame();
    const double color[4] = {1, 0.9, 0.6, 0.3}, ior[4] = {1.3, 1.5, 1.7, 2.2}, base[4] = {0.8, 0.5, 0.2, 0.9};

    // Pins from the NumPy transcription (see the file comment): p_spec and the per-lane E_spec.
    {
      const StackSelect s = ar::stack_select(fx.table, white_params(0.5), Real(0.5));
      CHECK(std::fabs(double(s.p_spec) - 0.0670779347) < 2e-6);
      const OpenPBRParams p2 = make_params(color, ior, 0.6, 0.3, 0.8, base, 0.4, 1.0);
      const StackSelect s2 = ar::stack_select(fx.table, p2, Real(0.25));
      CHECK(std::fabs(double(s2.p_spec) - 0.2309670755) < 2e-6);
      const double want_e[4] = {0.1369547170, 0.1639000524, 0.1259514628, 0.0748187186};
      for (int l = 0; l < 4; ++l) CHECK(std::fabs(double(s2.e_spec.v[l]) - want_e[l]) < 2e-6);
      const OpenPBRParams p3 = make_params(kOnes, kIor15, 1.0, 0.0, 1.0, std::vector<double>(4, 0.5).data(), 0.0, 1.0);
      CHECK(std::fabs(double(ar::stack_select(fx.table, p3, Real(0.8)).p_spec) - 0.0843784987) < 2e-6);
    }

    struct Cfg {
      double w, rough, base_w, diff_rough, closure;
    };
    const Cfg cfgs[] = {{1.0, 0.0, 1.0, 0.0, 1.0}, {1.0, 0.3, 1.0, 0.0, 1.0}, {0.6, 0.5, 0.8, 0.4, 0.7},
                        {0.0, 0.7, 1.0, 1.0, 1.0}, {1.0, 1.0, 0.5, 0.2, 2.0}};
    const double mus[] = {0.05, 0.3, 0.7, 1.0};
    int compared = 0, deltas = 0;
    for (const Cfg& c : cfgs)
      for (double mu : mus) {
        const OpenPBRParams p = make_params(color, ior, c.w, c.rough, c.base_w, base, c.diff_rough, c.closure);
        const Real3 wo = ar::to_world(frame, local_dir(mu, 0.9));
        // The lobes below see the direction the stack sees: world and back, with its float rounding.
        const Real3 wo_l = ar::to_local(frame, wo);
        const StackSelect s = ar::stack_select(fx.table, p, wo_l.z);
        INFO("w=" << c.w << " rough=" << c.rough << " mu=" << mu);
        // The probabilities are a distribution and drive every entry point.
        CHECK(std::fabs(double(s.p_spec) + double(s.p_diff) - 1.0) < 1e-6);
        CHECK(s.p_spec >= Real(0));
        CHECK(s.p_diff >= Real(0));

        const ar::GgxParams g = ar::stack_ggx_params(p);
        const ar::EonParams ep = ar::stack_eon_params(p);
        for (double zi : {0.2, 0.6, 0.95})
          for (double phi : {0.4, 2.5, 4.6}) {
            const Real3 wi = ar::to_world(frame, local_dir(zi, phi));
            const Real3 wi_l = ar::to_local(frame, wi);
            const LobeEval e = ar::stack_eval(fx.table, p, frame, wo, wi, TransportMode::Radiance);
            // Eval of the mixture is the sum of the weighted lobes: specular plus the diffuse lobe scaled
            // by 1 - E_spec per lane, times closure_weight; pdf is the selection-weighted sum.
            const LobeEval es = ar::ggx_eval(g, wo_l, wi_l, TransportMode::Radiance);
            const LobeEval ed = ar::eon_eval(ep, wo_l, wi_l, TransportMode::Radiance);
            for (int l = 0; l < 4; ++l) {
              const double want = c.closure * (double(es.f.v[l]) + (1.0 - double(s.e_spec.v[l])) * double(ed.f.v[l]));
              CHECK(rel_close(double(e.f.v[l]), want, 2e-4));
            }
            const double want_pdf = double(s.p_spec) * double(es.pdf) + double(s.p_diff) * double(ed.pdf);
            CHECK(rel_close(double(e.pdf), want_pdf, 2e-4));
            CHECK(rel_close(double(ar::stack_pdf(fx.table, p, frame, wo, wi, TransportMode::Radiance)), want_pdf, 2e-4));
            ++compared;
          }

        // Sample agrees with eval and pdf (the same selection function), or is a delta sample whose
        // weight is closure * F / p_spec.
        for (int i = 0; i < 12; ++i)
          for (int j = 0; j < 12; ++j) {
            const Real2 u{Real((i + 0.5) / 12), Real((j + 0.5) / 12)};
            const LobeSample smp = ar::stack_sample(fx.table, p, frame, wo, u, TransportMode::Radiance);
            if (smp.pdf == Real(0)) continue;
            if ((smp.flags & ar::LOBE_DELTA) != 0) {
              ++deltas;
              CHECK(c.rough == 0.0);
              CHECK(rel_close(double(smp.pdf), double(s.p_spec), 1e-6));
              const Spec fr = ar::fresnel_dielectric_weighted(Real(mu), p.specular_ior, p.specular_weight);
              const Real3 wi_l = ar::to_local(frame, smp.wi);
              CHECK(rel_close(double(wi_l.z), mu, 1e-5));
              for (int l = 0; l < 4; ++l)
                CHECK(rel_close(double(smp.f.v[l]) * double(wi_l.z) / double(smp.pdf),
                                c.closure * double(color[l]) * double(fr.v[l]) / double(s.p_spec), 1e-4));
              continue;
            }
            const LobeEval e = ar::stack_eval(fx.table, p, frame, wo, smp.wi, TransportMode::Radiance);
            CHECK(rel_close(double(smp.pdf), double(e.pdf), 5e-4));
            for (int l = 0; l < 4; ++l) CHECK(rel_close(double(smp.f.v[l]), double(e.f.v[l]), 5e-4));
          }
      }
    CHECK(compared == 180);
    CHECK(deltas > 0);

    // At roughness 0 the share of delta samples is p_spec: with stratified u.x it is exact to 1/N.
    {
      const OpenPBRParams p = make_params(kOnes, kIor15, 1.0, 0.0, 1.0, base, 0.0, 1.0);
      const double mu = 0.8;
      const StackSelect s = ar::stack_select(fx.table, p, Real(mu));
      const Real3 wo = ar::to_world(frame, local_dir(mu, 0.3));
      const int n = 4096;
      int delta = 0;
      for (int i = 0; i < n; ++i) {
        const LobeSample smp = ar::stack_sample(fx.table, p, frame, wo, Real2{Real((i + 0.5) / n), Real(0.37)}, TransportMode::Radiance);
        if (smp.pdf > Real(0) && (smp.flags & ar::LOBE_DELTA) != 0) ++delta;
      }
      CHECK(std::fabs(double(delta) / n - double(s.p_spec)) < 2.0 / n);
    }
  }

  TEST_CASE("lobe_stack/boundary uniforms") {
    // u in [0, 1): the extremes give finite output, and pdf 0 means "no sample" with f 0.
    Fixture fx;
    const Frame frame = tilted_frame();
    const Real lo = Real(0), hi = std::nextafter(Real(1), Real(0));
    const Real vals[] = {lo, hi, Real(0.5)};
    const double base[4] = {0.8, 0.5, 0.2, 0.9};
    for (double rough : {0.0, 0.1, 0.5, 1.0})
      for (double mu : {0.001, 0.05, 0.5, 1.0})
        for (Real u1 : vals)
          for (Real u2 : vals) {
            const OpenPBRParams p = make_params(kOnes, kIor15, 1.0, rough, 1.0, base, 0.5, 1.0);
            const LobeSample s = ar::stack_sample(fx.table, p, frame, ar::to_world(frame, local_dir(mu, 0.8)), Real2{u1, u2}, TransportMode::Radiance);
            INFO("rough=" << rough << " mu=" << mu << " u=(" << double(u1) << "," << double(u2) << ")");
            CHECK(std::isfinite(double(s.pdf)));
            CHECK(std::isfinite(double(s.wi.x)));
            for (int l = 0; l < 4; ++l) CHECK(std::isfinite(double(s.f.v[l])));
            if (s.pdf == Real(0)) {
              for (int l = 0; l < 4; ++l) CHECK(s.f.v[l] == Real(0));
            } else {
              CHECK(ar::to_local(frame, s.wi).z > Real(0));
            }
          }
    // wo below the shading plane: no sample, no NaN, eval and pdf zero.
    const OpenPBRParams p = white_params(0.5);
    const Real3 below = ar::to_world(frame, Real3{Real(0.6), Real(0), Real(-0.8)});
    const LobeSample s = ar::stack_sample(fx.table, p, frame, below, Real2{Real(0.5), Real(0.5)}, TransportMode::Radiance);
    CHECK(s.pdf == Real(0));
    CHECK(s.f.v[0] == Real(0));
    const Real3 up = ar::to_world(frame, local_dir(0.5, 0.2));
    CHECK(ar::stack_eval(fx.table, p, frame, below, up, TransportMode::Radiance).pdf == Real(0));
    CHECK(ar::stack_pdf(fx.table, p, frame, up, below, TransportMode::Radiance) == Real(0));
  }
}

TEST_SUITE("gate") {
  TEST_CASE("lobe_stack/white furnace") {
    // Lossless configuration of 0005 Verification 1: base_color 1, specular_weight 1, IOR 1.5, no
    // coat. E = mean of f cos / pdf over 2^20 stratified samples of stack_sample; the layering gives
    // E_spec + (1 - E_spec) E_eon = 1 when the table is the lobe's own albedo.
    Fixture fx;
    const Frame frame = tilted_frame();
    constexpr int kStrata = 1024;
    std::printf("lobe_stack furnace: mean of f cos / pdf, %d samples per row, IOR 1.5, band 0.99 <= E <= 1.002\n", kStrata * kStrata);
    double worst_lo = 10, worst_hi = 0;
    for (double rough : {0.25, 0.5, 1.0})
      for (double mu : {0.05, 0.25, 0.5, 0.75, 1.0}) {
        const OpenPBRParams p = white_params(rough);
        const Real3 wo = ar::to_world(frame, local_dir(mu, 0.6));
        double sum = 0;
        for (int i = 0; i < kStrata; ++i)
          for (int j = 0; j < kStrata; ++j) {
            const uint64_t h = (uint64_t(i) * kStrata + j) ^ (uint64_t(rough * 100) << 40) ^ (uint64_t(mu * 100) << 50);
            const double u1 = (i + hash_unit(h)) / kStrata, u2 = (j + hash_unit(h ^ 0xABCDEFull)) / kStrata;
            const LobeSample s = ar::stack_sample(fx.table, p, frame, wo, Real2{Real(u1), Real(u2)}, TransportMode::Radiance);
            if (!(s.pdf > Real(0))) continue;
            const double cos_i = double(ar::to_local(frame, s.wi).z);
            sum += double(s.f.v[0]) * cos_i / double(s.pdf);
          }
        const double e = sum / (double(kStrata) * kStrata);
        worst_lo = std::fmin(worst_lo, e);
        worst_hi = std::fmax(worst_hi, e);
        std::printf("  roughness %.2f ior 1.50 mu_o %.2f  E = %.5f\n", rough, mu, e);
        INFO("roughness=" << rough << " mu=" << mu << " E=" << e);
        CHECK(e >= 0.99);
        CHECK(e <= 1.002);
      }
    std::printf("lobe_stack furnace: E in [%.5f, %.5f]\n", worst_lo, worst_hi);
  }

  // Chi-squared of the mixture sampler against stack_pdf over the hemisphere, in cells of equal
  // cos(theta) bands times azimuth sectors (shading frame); expected counts integrate stack_pdf with a
  // 32x32 midpoint rule per cell.
  static void chi2_mixture(double rough, double mu, double diff_rough, const char* name) {
    Fixture fx;
    const st::Run run = st::begin(name, st::gate_family());
    const Frame frame = tilted_frame();
    const double base[4] = {0.8, 0.8, 0.8, 0.8};
    const OpenPBRParams p = make_params(kOnes, kIor15, 1.0, rough, 1.0, base, diff_rough, 1.0);
    const Real3 wo_l = local_dir(mu, 0.7);
    const Real3 wo = ar::to_world(frame, wo_l);
    auto gen = [&](int ires) {
      const int nb = ires, ns = 2 * ires, cells = nb * ns;
      const int n = 2000 * cells;
      st::Histogram h;
      h.observed.assign(cells, 0.0);
      h.expected.assign(cells, 0.0);
      constexpr int kSub = 32;
      for (int b = 0; b < nb; ++b)
        for (int s = 0; s < ns; ++s) {
          double e = 0.0;
          for (int a = 0; a < kSub; ++a) {
            const double z = (b + (a + 0.5) / kSub) / nb;
            for (int c = 0; c < kSub; ++c) {
              const double phi = 2.0 * kPi * (s + (c + 0.5) / kSub) / ns;
              e += double(ar::stack_pdf(fx.table, p, frame, wo, ar::to_world(frame, local_dir(z, phi)), TransportMode::Radiance));
            }
          }
          h.expected[b * ns + s] = n * e / (kSub * kSub) * (1.0 / nb) * (2.0 * kPi / ns);
        }
      for (int k = 0; k < n; ++k) {
        const uint64_t key = run.seed ^ (uint64_t(ires) << 56) ^ uint64_t(k);
        const LobeSample smp = ar::stack_sample(fx.table, p, frame, wo, Real2{Real(hash_unit(key)), Real(hash_unit(~key))}, TransportMode::Radiance);
        if (smp.pdf == Real(0)) continue;  // lost below the horizon; the expected counts exclude it
        const Real3 w = ar::to_local(frame, smp.wi);
        double ph = std::atan2(double(w.y), double(w.x));
        if (ph < 0) ph += 2.0 * kPi;
        const int b = std::min(nb - 1, std::max(0, int(double(w.z) * nb)));
        const int s = std::min(ns - 1, int(ph / (2.0 * kPi) * ns));
        h.observed[b * ns + s] += 1.0;
      }
      return h;
    };
    const st::Chi2Result res = st::chi2_with_retry(run, gen, 8);
    std::printf("lobe_stack chi2 roughness=%.2f mu=%.2f diffuse roughness=%.2f: chi2 %.1f dof %d p %.4f%s\n", rough, mu, diff_rough,
                res.chi2, res.dof, res.p, res.retried ? " (retried at 16)" : "");
    CHECK_MESSAGE(res.pass, res.message);
  }

  TEST_CASE("lobe_stack/chi2 mixture") {
    chi2_mixture(0.5, 0.3, 0.0, "lobe_stack/chi2 mixture r0.5 mu0.3");
    chi2_mixture(0.5, 0.8, 0.5, "lobe_stack/chi2 mixture r0.5 mu0.8 diffuse 0.5");
    chi2_mixture(1.0, 0.6, 1.0, "lobe_stack/chi2 mixture r1 mu0.6 diffuse 1");
  }
}
