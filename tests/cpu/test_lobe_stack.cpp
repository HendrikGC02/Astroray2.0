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
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

#include "../../src/host/data_resource.h"
#include "common/stat.h"
#include "material/lobes/stack.h"

namespace st = astroray::stat;

namespace {

using ar::EDielTable;
using ar::ESpecTable;
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
constexpr uint32_t kNs = ar::kESpecN;

// The committed tables as embedded in this executable (0001 D8.4), copied so that they are aligned.
struct Fixture {
  std::vector<uint8_t> bytes, spec_bytes;
  EDielTable table;
  ESpecTable spec;
  Fixture() {
    astroray::DataBlob blob;
    REQUIRE_MESSAGE(astroray::find_data("tables/e_diel_refl.bin", &blob), "tables/e_diel_refl.bin is not embedded");
    bytes.assign(blob.data, blob.data + blob.size);
    const char* err = ar::e_diel_view(bytes.data(), bytes.size(), &table);
    REQUIRE_MESSAGE(err == nullptr, (err ? err : ""));
    REQUIRE_MESSAGE(astroray::find_data("tables/e_spec.bin", &blob), "tables/e_spec.bin is not embedded");
    spec_bytes.assign(blob.data, blob.data + blob.size);
    err = ar::e_spec_view(spec_bytes.data(), spec_bytes.size(), &spec);
    REQUIRE_MESSAGE(err == nullptr, (err ? err : ""));
  }
  // Raw E_spec node value read straight from the bytes.
  double raw_spec(uint32_t imu, uint32_t ir) const {
    float v;
    std::memcpy(&v, spec_bytes.data() + ar::kETableHeaderBytes + 4 * (size_t(ir) * kNs + imu), 4);
    return double(v);
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

// Independent transcriptions for the metal pins, in double, from the raw table bytes: the trilinear
// E_diel and bilinear E_spec lookups (node i of an axis at i / (n - 1), the last cell reaching the
// last node), and F_82 from the spec text (index.html lines 546-556, mu_bar = 1/7, each lane
// saturated to [0, 1]).
double diel_ref(const Fixture& fx, double mu, double r, double eta) {
  const double c[3] = {mu * (kN - 1), r * (kN - 1), (eta - 1.0) / 2.0 * (kN - 1)};
  int i0[3];
  double w[3];
  for (int k = 0; k < 3; ++k) {
    i0[k] = std::min(int(std::floor(c[k])), int(kN) - 2);
    w[k] = c[k] - i0[k];
  }
  double acc = 0;
  for (int dm = 0; dm < 2; ++dm)
    for (int dr = 0; dr < 2; ++dr)
      for (int de = 0; de < 2; ++de)
        acc += (dm ? w[0] : 1 - w[0]) * (dr ? w[1] : 1 - w[1]) * (de ? w[2] : 1 - w[2]) *
               fx.raw(uint32_t(i0[0] + dm), uint32_t(i0[1] + dr), uint32_t(i0[2] + de));
  return acc;
}
double spec_ref(const Fixture& fx, double mu, double r) {
  const double c[2] = {mu * (kNs - 1), r * (kNs - 1)};
  const int im = std::min(int(std::floor(c[0])), int(kNs) - 2), ir = std::min(int(std::floor(c[1])), int(kNs) - 2);
  const double wm = c[0] - im, wr = c[1] - ir;
  return (1 - wr) * ((1 - wm) * fx.raw_spec(im, ir) + wm * fx.raw_spec(im + 1, ir)) +
         wr * ((1 - wm) * fx.raw_spec(im, ir + 1) + wm * fx.raw_spec(im + 1, ir + 1));
}
double f82_ref(double f0, double tint, double mu) {
  const auto schlick = [f0](double m) { return f0 + (1 - f0) * std::pow(1 - m, 5); };
  const double mb = 1.0 / 7.0;
  const double f = schlick(mu) - mu * std::pow(1 - mu, 6) / (mb * std::pow(1 - mb, 6)) * (schlick(mb) - tint * schlick(mb));
  return std::fmin(std::fmax(f, 0.0), 1.0);
}

// FNV-1a 64 over the bit patterns of the stack's outputs: a golden of many values in one number.
struct Digest {
  uint64_t h = 1469598103934665603ull;
  void bytes(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  }
  void add(Real v) { bytes(&v, sizeof v); }
  void add(const Spec& s) {
    for (int l = 0; l < ar::kLanes; ++l) add(s.v[l]);
  }
  void add(Real3 v) { add(v.x), add(v.y), add(v.z); }
  void add_u32(uint32_t v) { bytes(&v, sizeof v); }
};

// Every output of the stack's entry points for one parameter set over four view directions: the
// selection, eval and pdf at 9 directions, and both sample forms on a 12 x 12 grid.
uint64_t stack_digest(const Fixture& fx, const OpenPBRParams& p) {
  const Frame frame = tilted_frame();
  Digest d;
  for (double mu : {0.05, 0.3, 0.7, 1.0}) {
    const Real3 wo = ar::to_world(frame, local_dir(mu, 0.9));
    const StackSelect s = ar::stack_select(fx.table, fx.spec, p, ar::to_local(frame, wo).z);
    d.add(s.p_spec), d.add(s.p_diff), d.add(s.e_spec), d.add(s.eta);
    for (double zi : {0.2, 0.6, 0.95})
      for (double phi : {0.4, 2.5, 4.6}) {
        const Real3 wi = ar::to_world(frame, local_dir(zi, phi));
        const LobeEval e = ar::stack_eval(fx.table, fx.spec, p, frame, wo, wi, TransportMode::Radiance);
        d.add(e.f), d.add(e.pdf);
        d.add(ar::stack_pdf(fx.table, fx.spec, p, frame, wo, wi, TransportMode::Radiance));
      }
    for (int i = 0; i < 12; ++i)
      for (int j = 0; j < 12; ++j) {
        const Real2 u{Real((i + 0.5) / 12), Real((j + 0.5) / 12)};
        const LobeSample a = ar::stack_sample(fx.table, fx.spec, p, frame, wo, u, TransportMode::Radiance);
        const LobeSample b = ar::stack_sample(fx.table, fx.spec, p, frame, wo, u.y, Real2{u.x, Real(1) - u.y}, TransportMode::Radiance);
        for (const LobeSample* x : {&a, &b}) d.add(x->wi), d.add(x->f), d.add(x->pdf), d.add_u32(x->flags);
      }
  }
  return d.h;
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
    // The last cell interpolates up to node 15. What the lookup does outside its axes is only a
    // memory-safety backstop; stack_select clamps and reports first (case "lobe_stack/eta clamp").
    CHECK(std::fabs(double(ar::e_diel_lookup(fx.table, Real(1), Real(1), Real(3))) - fx.raw(15, 15, 15)) < 1e-6);

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
      const StackSelect s = ar::stack_select(fx.table, fx.spec, white_params(0.5), Real(0.5));
      CHECK(std::fabs(double(s.p_spec) - 0.0670779347) < 2e-6);
      const OpenPBRParams p2 = make_params(color, ior, 0.6, 0.3, 0.8, base, 0.4, 1.0);
      const StackSelect s2 = ar::stack_select(fx.table, fx.spec, p2, Real(0.25));
      CHECK(std::fabs(double(s2.p_spec) - 0.2309670755) < 2e-6);
      const double want_e[4] = {0.1369547170, 0.1639000524, 0.1259514628, 0.0748187186};
      for (int l = 0; l < 4; ++l) CHECK(std::fabs(double(s2.e_spec.v[l]) - want_e[l]) < 2e-6);
      const OpenPBRParams p3 = make_params(kOnes, kIor15, 1.0, 0.0, 1.0, std::vector<double>(4, 0.5).data(), 0.0, 1.0);
      CHECK(std::fabs(double(ar::stack_select(fx.table, fx.spec, p3, Real(0.8)).p_spec) - 0.0843784987) < 2e-6);
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
        const StackSelect s = ar::stack_select(fx.table, fx.spec, p, wo_l.z);
        INFO("w=" << c.w << " rough=" << c.rough << " mu=" << mu);
        // The probabilities are a distribution and drive every entry point.
        CHECK(std::fabs(double(s.p_spec) + double(s.p_diff) - 1.0) < 1e-6);
        CHECK(s.p_spec >= Real(0));
        CHECK(s.p_diff >= Real(0));

        const ar::GgxParams g = ar::stack_ggx_params(p, s);
        const ar::EonParams ep = ar::stack_eon_params(p);
        for (double zi : {0.2, 0.6, 0.95})
          for (double phi : {0.4, 2.5, 4.6}) {
            const Real3 wi = ar::to_world(frame, local_dir(zi, phi));
            const Real3 wi_l = ar::to_local(frame, wi);
            const LobeEval e = ar::stack_eval(fx.table, fx.spec, p, frame, wo, wi, TransportMode::Radiance);
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
            CHECK(rel_close(double(ar::stack_pdf(fx.table, fx.spec, p, frame, wo, wi, TransportMode::Radiance)), want_pdf, 2e-4));
            ++compared;
          }

        // Sample agrees with eval and pdf (the same selection function), or is a delta sample whose
        // weight is closure * F / p_spec.
        for (int i = 0; i < 12; ++i)
          for (int j = 0; j < 12; ++j) {
            const Real2 u{Real((i + 0.5) / 12), Real((j + 0.5) / 12)};
            const LobeSample smp = ar::stack_sample(fx.table, fx.spec, p, frame, wo, u, TransportMode::Radiance);
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
            const LobeEval e = ar::stack_eval(fx.table, fx.spec, p, frame, wo, smp.wi, TransportMode::Radiance);
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
      const StackSelect s = ar::stack_select(fx.table, fx.spec, p, Real(mu));
      const Real3 wo = ar::to_world(frame, local_dir(mu, 0.3));
      const int n = 4096;
      int delta = 0;
      for (int i = 0; i < n; ++i) {
        const LobeSample smp = ar::stack_sample(fx.table, fx.spec, p, frame, wo, Real2{Real((i + 0.5) / n), Real(0.37)}, TransportMode::Radiance);
        if (smp.pdf > Real(0) && (smp.flags & ar::LOBE_DELTA) != 0) ++delta;
      }
      CHECK(std::fabs(double(delta) / n - double(s.p_spec)) < 2.0 / n);
    }
  }

  TEST_CASE("lobe_stack/eta clamp") {
    // An index outside the table's [1, 3] is clamped once in stack_select, the lobe and the table
    // both use the clamped value, and the clamp is flagged for the host's report.
    Fixture fx;
    const Frame frame = tilted_frame();
    const double mu = 0.6;
    const Real3 wo = ar::to_world(frame, local_dir(mu, 0.5));
    const Real3 wi = ar::to_world(frame, local_dir(0.7, 2.0));
    struct Row {
      double ior, weight, want_eta;
      bool clamped;
    };
    // Weight 0.5 at IOR 4: F0 = 0.36, xi F0 = 0.18, eps = sqrt(0.18), eta = (1 + eps) / (1 - eps) = 2.4739 (in range).
    const Row rows[] = {{1.5, 1.0, 1.5, false}, {4.0, 1.0, 3.0, true}, {0.9, 1.0, 1.0, true},
                        {4.0, 0.5, 2.4739, false}, {3.0, 1.0, 3.0, false}, {1.0, 1.0, 1.0, false}};
    for (const Row& row : rows) {
      const double ior[4] = {row.ior, row.ior, row.ior, row.ior};
      const OpenPBRParams p = make_params(kOnes, ior, row.weight, 0.5, 1.0, kOnes, 0.0, 1.0);
      const StackSelect s = ar::stack_select(fx.table, fx.spec, p, Real(mu));
      INFO("ior=" << row.ior << " weight=" << row.weight);
      CHECK(s.eta_clamped == row.clamped);
      for (int l = 0; l < 4; ++l) CHECK(std::fabs(double(s.eta.v[l]) - row.want_eta) < 1e-3);
      // The out-parameter of every entry point carries the flag.
      bool f_eval = false, f_pdf = false, f_sample = false;
      ar::stack_eval(fx.table, fx.spec, p, frame, wo, wi, TransportMode::Radiance, &f_eval);
      ar::stack_pdf(fx.table, fx.spec, p, frame, wo, wi, TransportMode::Radiance, &f_pdf);
      ar::stack_sample(fx.table, fx.spec, p, frame, wo, Real2{Real(0.5), Real(0.5)}, TransportMode::Radiance, &f_sample);
      CHECK(f_eval == row.clamped);
      CHECK(f_pdf == row.clamped);
      CHECK(f_sample == row.clamped);
      // The specular part of eval is the lobe at the clamped index (weight 1), and E_spec was built at
      // that same index: eval = closure (ggx(eta_used) + (1 - E_spec) eon).
      ar::GgxParams g;
      g.specular_color = p.specular_color;
      for (int l = 0; l < 4; ++l) g.specular_ior.v[l] = Real(row.want_eta);
      g.specular_weight = Real(1);
      g.roughness = p.specular_roughness;
      const Real3 wo_l = ar::to_local(frame, wo), wi_l = ar::to_local(frame, wi);
      const LobeEval es = ar::ggx_eval(g, wo_l, wi_l, TransportMode::Radiance);
      const LobeEval ed = ar::eon_eval(ar::stack_eon_params(p), wo_l, wi_l, TransportMode::Radiance);
      const LobeEval e = ar::stack_eval(fx.table, fx.spec, p, frame, wo, wi, TransportMode::Radiance);
      for (int l = 0; l < 4; ++l) {
        const double want = double(es.f.v[l]) + (1.0 - double(s.e_spec.v[l])) * double(ed.f.v[l]);
        CHECK(rel_close(double(e.f.v[l]), want, 1e-3));
      }
    }
  }

  TEST_CASE("lobe_stack/metalness zero is the dielectric stack") {
    // Goldens: the digest of every output of the dielectric stack (eval, pdf, both sample forms,
    // the selection) as it was before the metal joined it, for three parameter sets of the cases
    // above (the furnace's white base, a coloured set of "selection shared", the delta case).
    Fixture fx;
    const double color[4] = {1, 0.9, 0.6, 0.3}, ior[4] = {1.3, 1.5, 1.7, 2.2}, base[4] = {0.8, 0.5, 0.2, 0.9};
    const OpenPBRParams cfg[3] = {white_params(0.5), make_params(color, ior, 0.6, 0.5, 0.8, base, 0.4, 0.7),
                                  make_params(kOnes, kIor15, 1.0, 0.0, 1.0, base, 0.0, 1.0)};
    const uint64_t golden[3] = {0x590adf3e6b821288ull, 0x0ab6de169062dc8cull, 0x21a72d6f39d34092ull};
    for (int k = 0; k < 3; ++k) {
      const uint64_t got = stack_digest(fx, cfg[k]);
      INFO("set " << k << " digest 0x" << std::hex << got);
      CHECK(got == golden[k]);
      CHECK(ar::stack_select(fx.table, fx.spec, cfg[k], Real(0.6)).p_metal == Real(0));
    }

    // base_metalness 1 is the compensated metal lobe alone: f = closure c f_metal, pdf = metal_pdf,
    // and a sample is the lobe's own sample of the same uniforms (p_metal = 1, no rescale).
    const Frame frame = tilted_frame();
    for (double rough : {0.0, 0.3, 0.8})
      for (double mu : {0.1, 0.5, 0.9}) {
        OpenPBRParams p = make_params(color, ior, 0.8, rough, 0.9, base, 0.2, 0.7);
        p.base_metalness = Real(1);
        const Real3 wo = ar::to_world(frame, local_dir(mu, 1.3));
        const Real3 wo_l = ar::to_local(frame, wo);
        const StackSelect s = ar::stack_select(fx.table, fx.spec, p, wo_l.z);
        INFO("rough=" << rough << " mu=" << mu);
        CHECK(s.p_metal == Real(1));
        CHECK(s.p_spec == Real(0));
        CHECK(s.p_diff == Real(0));
        const ar::MetalParams mp = ar::stack_metal_params(p);
        const Spec comp = ar::metal_compensation(mp, ar::e_spec_lookup(fx.spec, wo_l.z, Real(rough)));
        for (int l = 0; l < 4; ++l) CHECK(rel_close(double(ar::stack_metal_comp(p, s).v[l]), double(comp.v[l]), 1e-6));
        for (double zi : {0.3, 0.8})
          for (double phi : {0.5, 3.5}) {
            const Real3 wi = ar::to_world(frame, local_dir(zi, phi));
            const LobeEval e = ar::stack_eval(fx.table, fx.spec, p, frame, wo, wi, TransportMode::Radiance);
            const LobeEval em = ar::metal_eval(mp, wo_l, ar::to_local(frame, wi), TransportMode::Radiance);
            for (int l = 0; l < 4; ++l) CHECK(rel_close(double(e.f.v[l]), 0.7 * double(ar::stack_metal_comp(p, s).v[l]) * double(em.f.v[l]), 1e-6));
            CHECK(rel_close(double(e.pdf), double(em.pdf), 1e-6));
          }
        for (int i = 0; i < 6; ++i) {
          const Real2 u{Real((i + 0.5) / 6), Real(0.37)};
          const LobeSample a = ar::stack_sample(fx.table, fx.spec, p, frame, wo, u, TransportMode::Radiance);
          const LobeSample b = ar::metal_sample(mp, wo_l, u, TransportMode::Radiance);
          CHECK((a.pdf > Real(0)) == (b.pdf > Real(0)));
          if (!(b.pdf > Real(0))) continue;
          CHECK(rel_close(double(a.pdf), double(b.pdf), 1e-5));
          CHECK((a.flags & ar::LOBE_DELTA) == (b.flags & ar::LOBE_DELTA));
          CHECK((a.flags & ar::LOBE_GLOSSY) != 0);
          const Real3 wi_l = ar::to_local(frame, a.wi);
          CHECK(std::fabs(double(wi_l.z) - double(b.wi.z)) < 1e-5);
          for (int l = 0; l < 4; ++l) CHECK(rel_close(double(a.f.v[l]), 0.7 * double(ar::stack_metal_comp(p, s).v[l]) * double(b.f.v[l]), 1e-5));
        }
      }
  }

  TEST_CASE("lobe_stack/metal mix") {
    // base_metalness 0.5: the three probabilities are a distribution, eval is the mix
    // closure ((1 - M) (f_spec + (1 - E) f_diff) + M c f_metal), pdf the selection-weighted sum,
    // and sample agrees with eval; a delta metal sample has pdf = p_metal.
    Fixture fx;
    const Frame frame = tilted_frame();
    const double color[4] = {1, 0.9, 0.6, 0.3}, ior[4] = {1.3, 1.5, 1.7, 2.2}, base[4] = {0.8, 0.5, 0.2, 0.9};
    int metal_samples = 0, metal_deltas = 0;
    for (double rough : {0.0, 0.3, 0.7})
      for (double mu : {0.1, 0.5, 0.95}) {
        OpenPBRParams p = make_params(color, ior, 0.9, rough, 1.0, base, 0.3, 0.8);
        p.base_metalness = Real(0.5);
        const Real3 wo = ar::to_world(frame, local_dir(mu, 0.4));
        const Real3 wo_l = ar::to_local(frame, wo);
        const StackSelect s = ar::stack_select(fx.table, fx.spec, p, wo_l.z);
        INFO("rough=" << rough << " mu=" << mu);
        CHECK(std::fabs(double(s.p_spec) + double(s.p_diff) + double(s.p_metal) - 1.0) < 1e-6);
        CHECK(s.p_metal > Real(0));
        const ar::GgxParams g = ar::stack_ggx_params(p, s);
        const ar::EonParams ep = ar::stack_eon_params(p);
        const ar::MetalParams mp = ar::stack_metal_params(p);
        for (double zi : {0.25, 0.7})
          for (double phi : {0.9, 4.0}) {
            const Real3 wi = ar::to_world(frame, local_dir(zi, phi));
            const Real3 wi_l = ar::to_local(frame, wi);
            const LobeEval e = ar::stack_eval(fx.table, fx.spec, p, frame, wo, wi, TransportMode::Radiance);
            const LobeEval es = ar::ggx_eval(g, wo_l, wi_l, TransportMode::Radiance);
            const LobeEval ed = ar::eon_eval(ep, wo_l, wi_l, TransportMode::Radiance);
            const LobeEval em = ar::metal_eval(mp, wo_l, wi_l, TransportMode::Radiance);
            for (int l = 0; l < 4; ++l) {
              const double want = 0.8 * (0.5 * (double(es.f.v[l]) + (1.0 - double(s.e_spec.v[l])) * double(ed.f.v[l])) +
                                         0.5 * double(ar::stack_metal_comp(p, s).v[l]) * double(em.f.v[l]));
              CHECK(rel_close(double(e.f.v[l]), want, 2e-4));
            }
            const double want_pdf = double(s.p_spec) * double(es.pdf) + double(s.p_diff) * double(ed.pdf) + double(s.p_metal) * double(em.pdf);
            CHECK(rel_close(double(e.pdf), want_pdf, 2e-4));
            CHECK(rel_close(double(ar::stack_pdf(fx.table, fx.spec, p, frame, wo, wi, TransportMode::Radiance)), want_pdf, 2e-4));
          }
        for (int i = 0; i < 16; ++i)
          for (int j = 0; j < 4; ++j) {
            const Real2 u{Real((i + 0.5) / 16), Real((j + 0.5) / 4)};
            const LobeSample smp = ar::stack_sample(fx.table, fx.spec, p, frame, wo, u, TransportMode::Radiance);
            if (smp.pdf == Real(0)) continue;
            const bool metal = double(u.x) >= double(s.p_spec) + double(s.p_diff);
            metal_samples += metal;
            if ((smp.flags & ar::LOBE_DELTA) != 0) {
              CHECK(rough == 0.0);
              CHECK((smp.flags & ar::LOBE_GLOSSY) != 0);
              if (!metal) continue;  // the dielectric delta is pinned by "selection shared"
              ++metal_deltas;
              CHECK(rel_close(double(smp.pdf), double(s.p_metal), 1e-6));
              const Spec fr = ar::metal_fresnel(mp, wo_l.z);
              const double cos_i = double(ar::to_local(frame, smp.wi).z);
              for (int l = 0; l < 4; ++l)
                CHECK(rel_close(double(smp.f.v[l]) * cos_i / double(smp.pdf),
                                0.8 * 0.5 * double(ar::stack_metal_comp(p, s).v[l]) * double(fr.v[l]) / double(s.p_metal), 1e-4));
              continue;
            }
            const LobeEval e = ar::stack_eval(fx.table, fx.spec, p, frame, wo, smp.wi, TransportMode::Radiance);
            CHECK(rel_close(double(smp.pdf), double(e.pdf), 5e-4));
            for (int l = 0; l < 4; ++l) CHECK(rel_close(double(smp.f.v[l]), double(e.f.v[l]), 5e-4));
          }
      }
    CHECK(metal_samples > 0);
    CHECK(metal_deltas > 0);

    // The compensation's F0 is the lobe's own normal-incidence Fresnel, specular_weight base_weight
    // base_color per lane: c = 1 + F0 (1 - E) / E with E a raw table node.
    {
      ar::MetalParams mp;
      for (int l = 0; l < 4; ++l) mp.base_color.v[l] = Real(base[l]), mp.specular_color.v[l] = Real(color[l]);
      mp.base_weight = Real(0.9);
      mp.specular_weight = Real(0.8);
      mp.roughness = Real(0.6);
      const double e = fx.raw_spec(20, 18);
      const Spec c = ar::metal_compensation(mp, Real(e));
      for (int l = 0; l < 4; ++l) CHECK(rel_close(double(c.v[l]), 1.0 + 0.8 * 0.9 * base[l] * (1.0 - e) / e, 1e-6));
    }
    // p_metal at M = 0.5 for a coloured set from the rule written out in double (0005 item 4): the
    // lane means of E_diel (times specular_color), of (1 - E_diel) E_EON (E_EON = base at diffuse
    // roughness 0) and of F_82(mu_o) E_spec c, each lane clamped to [0, 1], weighted (1 - M), (1 - M), M.
    {
      const double ior2[4] = {1.4, 1.6, 1.9, 2.3}, mu = 0.6, r = 0.5;
      OpenPBRParams p = make_params(color, ior2, 1.0, r, 0.9, base, 0.0, 1.0);
      p.base_metalness = Real(0.5);
      const double em = spec_ref(fx, mu, r);
      double a_spec = 0, a_diff = 0, a_metal = 0;
      for (int l = 0; l < 4; ++l) {
        const double es = std::fmin(std::fmax(color[l] * diel_ref(fx, mu, r, ior2[l]), 0.0), 1.0);
        const double f0 = 0.9 * base[l];
        a_spec += es / 4;
        a_diff += (1 - es) * f0 / 4;
        a_metal += std::fmin(std::fmax(f82_ref(f0, color[l], mu) * em * (1 + f0 * (1 - em) / em), 0.0), 1.0) / 4;
      }
      const double want = 0.5 * a_metal / (0.5 * a_spec + 0.5 * a_diff + 0.5 * a_metal);
      const StackSelect s = ar::stack_select(fx.table, fx.spec, p, Real(mu));
      INFO("p_metal " << double(s.p_metal) << " transcription " << want);
      CHECK(std::fabs(double(s.p_metal) - want) < 2e-6);
    }
    // M outside OpenPBR's [0, 1] is clamped: 1.5 is the metal alone, -0.5 the dielectric alone, so
    // no weight, probability or value goes negative.
    {
      OpenPBRParams hi = make_params(color, ior, 0.9, 0.4, 1.0, base, 0.3, 0.8), one = hi, lo = hi, zero = hi;
      hi.base_metalness = Real(1.5), one.base_metalness = Real(1), lo.base_metalness = Real(-0.5), zero.base_metalness = Real(0);
      const Real3 wo = ar::to_world(frame, local_dir(0.5, 0.4)), wi = ar::to_world(frame, local_dir(0.7, 2.6));
      for (const auto& pr : {std::make_pair(hi, one), std::make_pair(lo, zero)}) {
        const StackSelect a = ar::stack_select(fx.table, fx.spec, pr.first, Real(0.5));
        const StackSelect b = ar::stack_select(fx.table, fx.spec, pr.second, Real(0.5));
        CHECK(a.p_spec == b.p_spec);
        CHECK(a.p_diff == b.p_diff);
        CHECK(a.p_metal == b.p_metal);
        const LobeEval ea = ar::stack_eval(fx.table, fx.spec, pr.first, frame, wo, wi, TransportMode::Radiance);
        const LobeEval eb = ar::stack_eval(fx.table, fx.spec, pr.second, frame, wo, wi, TransportMode::Radiance);
        CHECK(ea.pdf == eb.pdf);
        for (int l = 0; l < 4; ++l) CHECK(ea.f.v[l] == eb.f.v[l]);
      }
    }

    // Black base_color with specular_color 0: F82 vanishes at mu_o = 0.5 on every lane yet the lobe
    // reflects at grazing half-vectors, so the metal keeps the floor probability.
    OpenPBRParams dark = make_params(std::vector<double>(4, 0.0).data(), kIor15, 1.0, 0.5, 1.0,
                                     std::vector<double>(4, 0.0).data(), 0.0, 1.0);
    dark.base_metalness = Real(1);
    const StackSelect sd = ar::stack_select(fx.table, fx.spec, dark, Real(0.5));
    CHECK(sd.p_metal == Real(1));
  }

  TEST_CASE("lobe_stack/conductor normal incidence") {
    // 0005 Verification 3: F(1) = ((n - 1)^2 + k^2) / ((n + 1)^2 + k^2), to 1e-6 in float and 1e-12
    // in the double instance of the same template. Oblique angles against an independent form, the
    // s and p amplitudes with the complex index squared (Born and Wolf 13.2), in std::complex double.
    struct NK {
      double n, k;
    };
    const NK pairs[5] = {{0.2, 3.0}, {1.5, 0.0}, {0.05, 4.2}, {2.9, 3.1}, {1.0, 1.0}};
    const auto oracle = [](double c, double n, double k) {
      const std::complex<double> e2 = std::complex<double>(n, k) * std::complex<double>(n, k);
      const std::complex<double> w = std::sqrt(e2 - (1.0 - c * c));
      const std::complex<double> rs = (c - w) / (c + w), rp = (e2 * c - w) / (e2 * c + w);
      return 0.5 * (std::norm(rs) + std::norm(rp));
    };
    for (const NK& q : pairs) {
      const double want = ((q.n - 1) * (q.n - 1) + q.k * q.k) / ((q.n + 1) * (q.n + 1) + q.k * q.k);
      INFO("n=" << q.n << " k=" << q.k);
      CHECK(std::fabs(double(ar::fresnel_conductor_scalar<float>(1.0f, float(q.n), float(q.k))) - want) < 1e-6);
      CHECK(std::fabs(ar::fresnel_conductor_scalar<double>(1.0, q.n, q.k) - want) < 1e-12);
      for (double c : {0.9, 0.5, 0.2, 0.05}) {
        INFO("cos=" << c);
        CHECK(std::fabs(double(ar::fresnel_conductor_scalar<float>(float(c), float(q.n), float(q.k))) - oracle(c, q.n, q.k)) < 1e-5);
        CHECK(std::fabs(ar::fresnel_conductor_scalar<double>(c, q.n, q.k) - oracle(c, q.n, q.k)) < 1e-12);
      }
      CHECK(std::fabs(ar::fresnel_conductor_scalar<double>(0.0, q.n, q.k) - 1.0) < 1e-12);  // grazing
    }
    // The lobe in the exact mode: per lane, times specular_weight; F82 inputs are not read.
    ar::MetalParams m;
    m.base_color = ar::spec_fill(Real(0.1));
    m.specular_color = ar::spec_fill(Real(0.2));
    m.base_weight = Real(1);
    m.specular_weight = Real(0.8);
    m.roughness = Real(0.5);
    m.conductor = true;
    for (int l = 0; l < 4; ++l) m.n.v[l] = Real(pairs[l].n), m.k.v[l] = Real(pairs[l].k);
    const Spec f = ar::metal_fresnel(m, Real(1));
    for (int l = 0; l < 4; ++l) {
      const double want = ((pairs[l].n - 1) * (pairs[l].n - 1) + pairs[l].k * pairs[l].k) /
                          ((pairs[l].n + 1) * (pairs[l].n + 1) + pairs[l].k * pairs[l].k);
      CHECK(std::fabs(double(f.v[l]) - 0.8 * want) < 1e-6);
    }
  }

  TEST_CASE("lobe_stack/transport mode swap") {
    // 0005 item 8: every lobe takes TransportMode. These are reflection lobes, where it changes no
    // value (no eta^2 term; the adjoint shading-normal correction is the caller's), so eval, pdf
    // and sample are bit-identical under Radiance and Importance for EON, GGX, metal and the stack.
    // The first lobe that reads the mode fails here until its expectation is written.
    // shade_surface passes TransportMode::Radiance (camera paths); the light pass will pass Importance.
    Fixture fx;
    const Frame frame = tilted_frame();
    const double color[4] = {1, 0.9, 0.6, 0.3}, ior[4] = {1.3, 1.5, 1.7, 2.2}, base[4] = {0.8, 0.5, 0.2, 0.9};
    const auto same = [](const LobeEval& a, const LobeEval& b) {
      bool ok = std::memcmp(&a.pdf, &b.pdf, sizeof a.pdf) == 0;
      for (int l = 0; l < 4; ++l) ok = ok && std::memcmp(&a.f.v[l], &b.f.v[l], sizeof(Real)) == 0;
      return ok;
    };
    const auto same_s = [](const LobeSample& a, const LobeSample& b) {
      bool ok = std::memcmp(&a.pdf, &b.pdf, sizeof a.pdf) == 0 && a.flags == b.flags && std::memcmp(&a.wi, &b.wi, sizeof a.wi) == 0;
      for (int l = 0; l < 4; ++l) ok = ok && std::memcmp(&a.f.v[l], &b.f.v[l], sizeof(Real)) == 0;
      return ok;
    };
    const auto bits = [](Real a, Real b) { return std::memcmp(&a, &b, sizeof a) == 0; };
    const TransportMode R = TransportMode::Radiance, I = TransportMode::Importance;
    int eon_bad = 0, ggx_bad = 0, metal_bad = 0, stack_bad = 0, n = 0;
    for (double rough : {0.0, 0.3, 0.8})
      for (double metal : {0.0, 0.5, 1.0})
        for (double mu : {0.1, 0.6, 1.0}) {
          OpenPBRParams p = make_params(color, ior, 0.9, rough, 1.0, base, 0.5, 1.0);
          p.base_metalness = Real(metal);
          const StackSelect s = ar::stack_select(fx.table, fx.spec, p, Real(mu));
          const ar::GgxParams g = ar::stack_ggx_params(p, s);
          const ar::EonParams ep = ar::stack_eon_params(p);
          const ar::MetalParams mp = ar::stack_metal_params(p);
          const Real3 wo_l = local_dir(mu, 0.8), wo = ar::to_world(frame, wo_l);
          for (double zi : {0.2, 0.9})
            for (double phi : {1.0, 3.9}) {
              const Real3 wi_l = local_dir(zi, phi), wi = ar::to_world(frame, wi_l);
              eon_bad += !same(ar::eon_eval(ep, wo_l, wi_l, R), ar::eon_eval(ep, wo_l, wi_l, I)) ||
                         !bits(ar::eon_pdf(ep, wo_l, wi_l, R), ar::eon_pdf(ep, wo_l, wi_l, I));
              ggx_bad += !same(ar::ggx_eval(g, wo_l, wi_l, R), ar::ggx_eval(g, wo_l, wi_l, I)) ||
                         !bits(ar::ggx_pdf(g, wo_l, wi_l, R), ar::ggx_pdf(g, wo_l, wi_l, I));
              metal_bad += !same(ar::metal_eval(mp, wo_l, wi_l, R), ar::metal_eval(mp, wo_l, wi_l, I)) ||
                           !bits(ar::metal_pdf(mp, wo_l, wi_l, R), ar::metal_pdf(mp, wo_l, wi_l, I));
              stack_bad += !same(ar::stack_eval(fx.table, fx.spec, p, frame, wo, wi, R), ar::stack_eval(fx.table, fx.spec, p, frame, wo, wi, I)) ||
                           !bits(ar::stack_pdf(fx.table, fx.spec, p, frame, wo, wi, R), ar::stack_pdf(fx.table, fx.spec, p, frame, wo, wi, I));
              ++n;
            }
          for (int i = 0; i < 8; ++i) {
            const Real2 u{Real((i + 0.5) / 8), Real((7 - i + 0.25) / 8)};
            eon_bad += !same_s(ar::eon_sample(ep, wo_l, u, R), ar::eon_sample(ep, wo_l, u, I));
            ggx_bad += !same_s(ar::ggx_sample(g, wo_l, u, R), ar::ggx_sample(g, wo_l, u, I));
            metal_bad += !same_s(ar::metal_sample(mp, wo_l, u, R), ar::metal_sample(mp, wo_l, u, I));
            stack_bad += !same_s(ar::stack_sample(fx.table, fx.spec, p, frame, wo, u, R), ar::stack_sample(fx.table, fx.spec, p, frame, wo, u, I)) ||
                         !same_s(ar::stack_sample(fx.table, fx.spec, p, frame, wo, u.y, u, R),
                                 ar::stack_sample(fx.table, fx.spec, p, frame, wo, u.y, u, I));
          }
        }
    CHECK(n == 108);
    CHECK(eon_bad == 0);
    CHECK(ggx_bad == 0);
    CHECK(metal_bad == 0);
    CHECK(stack_bad == 0);
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
            const LobeSample s = ar::stack_sample(fx.table, fx.spec, p, frame, ar::to_world(frame, local_dir(mu, 0.8)), Real2{u1, u2}, TransportMode::Radiance);
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
    const LobeSample s = ar::stack_sample(fx.table, fx.spec, p, frame, below, Real2{Real(0.5), Real(0.5)}, TransportMode::Radiance);
    CHECK(s.pdf == Real(0));
    CHECK(s.f.v[0] == Real(0));
    const Real3 up = ar::to_world(frame, local_dir(0.5, 0.2));
    CHECK(ar::stack_eval(fx.table, fx.spec, p, frame, below, up, TransportMode::Radiance).pdf == Real(0));
    CHECK(ar::stack_pdf(fx.table, fx.spec, p, frame, up, below, TransportMode::Radiance) == Real(0));
  }

  TEST_CASE("lobe_stack/non-finite input is not a zero result") {
    // A NaN the stack's own arithmetic meets (the closure weight on both the delta and the
    // non-delta sample, the diffuse roughness on the EON lobe) reaches the caller as a NaN f or a
    // pdf that is not a number, for the stage to end the sample INVALID, never as the zero of a lobe
    // with no value (rule 5, 0004 D17). Eval and both sample forms are checked.
    Fixture fx;
    const Frame frame = tilted_frame();
    const Real nan = std::numeric_limits<Real>::quiet_NaN();
    const auto loud = [](const Spec& f, Real pdf) {
      bool finite = std::isfinite(double(pdf));
      for (int l = 0; l < ar::kLanes; ++l) finite = finite && std::isfinite(double(f.v[l]));
      return !finite;
    };
    struct Case {
      const char* what;
      double rough, spec_weight, metal;
      bool nan_closure, nan_diffuse;
    };
    const Case cases[] = {{"closure weight, glossy", 0.5, 1.0, 0.0, true, false},
                          {"closure weight, delta", 0.0, 1.0, 0.0, true, false},
                          {"closure weight, with metal", 0.5, 1.0, 0.5, true, false},
                          {"diffuse roughness", 0.5, 0.0, 0.0, false, true}};
    for (const Case& c : cases)
      for (double mu : {0.3, 0.9}) {
        OpenPBRParams p = make_params(kOnes, kIor15, c.spec_weight, c.rough, 1.0, kOnes, 0.0, 1.0);
        p.base_metalness = Real(c.metal);
        if (c.nan_closure) p.closure_weight = nan;
        if (c.nan_diffuse) p.base_diffuse_roughness = nan;
        const Real3 wo = ar::to_world(frame, local_dir(mu, 0.9));
        const Real3 wi = ar::to_world(frame, local_dir(0.6, 0.4));
        INFO(c.what << ", mu=" << mu);
        const LobeEval e = ar::stack_eval(fx.table, fx.spec, p, frame, wo, wi, TransportMode::Radiance);
        CHECK(loud(e.f, e.pdf));
        for (Real uc : {Real(0.05), Real(0.4), Real(0.7), Real(0.95)}) {
          const LobeSample a = ar::stack_sample(fx.table, fx.spec, p, frame, wo, uc, Real2{Real(0.3), Real(0.6)}, TransportMode::Radiance);
          CHECK(loud(a.f, a.pdf));
        }
        const LobeSample b = ar::stack_sample(fx.table, fx.spec, p, frame, wo, Real2{Real(0.4), Real(0.6)}, TransportMode::Radiance);
        CHECK(loud(b.f, b.pdf));
      }
  }
}

TEST_SUITE("gate") {
  // A slow cross-check (a 400 x 800 quadrature at 10 cells), so it runs in the gate tier.
  TEST_CASE("lobe_stack/e_spec table") {
    Fixture fx;
    const uint8_t* b = fx.spec_bytes.data();
    CHECK(ar::etable_u32(b + 12) == ar::kETableKindSpec);
    CHECK(ar::etable_u32(b + 16) == 1u);
    CHECK(ar::etable_u32(b + ar::kETableSamplesOffset) == 65536u);
    bool any = false;
    for (int i = 0; i < 32; ++i) any = any || b[ar::kETableLobeHashOffset + i] != 0;
    CHECK(any);
    // Grid points return the stored value; every cell is in (0, 1]; the smooth row is 1 (F = 1, no
    // masking loss).
    int bad = 0;
    for (uint32_t ir = 0; ir < kNs; ++ir)
      for (uint32_t im = 0; im < kNs; ++im) {
        const double got = double(ar::e_spec_lookup(fx.spec, Real(im) / Real(kNs - 1), Real(ir) / Real(kNs - 1)));
        if (std::fabs(got - fx.raw_spec(im, ir)) > 1e-6) ++bad;
        if (!(fx.raw_spec(im, ir) > 0.0 && fx.raw_spec(im, ir) <= 1.0 + 1e-6)) ++bad;
        if (ir == 0 && std::fabs(fx.raw_spec(im, ir) - 1.0) > 1e-6) ++bad;
      }
    CHECK(bad == 0);
    // A cell centre is the mean of its 4 corners.
    {
      const uint32_t im = 11, ir = 20;
      const double mean = (fx.raw_spec(im, ir) + fx.raw_spec(im + 1, ir) + fx.raw_spec(im, ir + 1) + fx.raw_spec(im + 1, ir + 1)) / 4.0;
      const double got = double(ar::e_spec_lookup(fx.spec, (Real(im) + Real(0.5)) / Real(kNs - 1), (Real(ir) + Real(0.5)) / Real(kNs - 1)));
      CHECK(std::fabs(got - mean) < 1e-6);
    }
    // The lookup at 10 cells against a midpoint quadrature of f cos from metal_eval at F = 1 (an
    // integrator independent of the generator's importance sampling).
    struct Cell {
      uint32_t im, ir;
    };
    for (const Cell& c : {Cell{31, 12}, Cell{16, 14}, Cell{4, 16}, Cell{24, 18}, Cell{10, 20}, Cell{28, 22}, Cell{2, 24}, Cell{20, 26},
                          Cell{8, 29}, Cell{31, 31}}) {
      ar::MetalParams m;
      m.base_color = ar::spec_fill(Real(1));
      m.specular_color = ar::spec_fill(Real(1));
      m.base_weight = Real(1);
      m.specular_weight = Real(1);
      m.roughness = Real(c.ir) / Real(kNs - 1);
      const double mu = double(c.im) / (kNs - 1);
      const Real3 wo = local_dir(mu, 0.0);
      double q = 0;
      constexpr int nz = 400, nphi = 800;
      for (int i = 0; i < nz; ++i) {
        const double z = (i + 0.5) / nz;
        for (int j = 0; j < nphi; ++j) {
          const LobeEval e = ar::metal_eval(m, wo, local_dir(z, 2.0 * kPi * (j + 0.5) / nphi), TransportMode::Radiance);
          q += double(e.f.v[0]) * z;
        }
      }
      q *= (1.0 / nz) * (2.0 * kPi / nphi);
      const double got = double(ar::e_spec_lookup(fx.spec, Real(mu), m.roughness));
      INFO("cell mu=" << c.im << " r=" << c.ir << " quadrature " << q << " lookup " << got);
      CHECK(std::fabs(q - got) < 5e-4);
    }
  }

  TEST_CASE("lobe_stack/white furnace" * doctest::test_suite("full")) {
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
            const LobeSample s = ar::stack_sample(fx.table, fx.spec, p, frame, wo, Real2{Real(u1), Real(u2)}, TransportMode::Radiance);
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

  TEST_CASE("lobe_stack/metal white furnace") {
    // Lossless metal of 0005 Verification 1 (metal row): base_metalness 1, base_color 1,
    // specular_color 1, both weights 1, so F = 1 and the Turquin-compensated lobe must reflect
    // everything: E = E_ss c with c = 1 / E_spec. E = mean of f cos / pdf over 2^20 stratified
    // samples of stack_sample (the method of "lobe_stack/white furnace"), on each of the 4 lanes.
    // The 15 rows are independent and run on threads; each row sums in a fixed order, so the
    // result does not depend on the thread count.
    Fixture fx;
    const Frame frame = tilted_frame();
    constexpr int kStrata = 1024;
    std::printf("lobe_stack metal furnace: mean of f cos / pdf, %d samples per row, 4 lanes, band 0.99 <= E <= 1.002\n", kStrata * kStrata);
    const double roughs[3] = {0.25, 0.5, 1.0}, mus[5] = {0.05, 0.25, 0.5, 0.75, 1.0};
    double sums[15][4] = {};
    std::atomic<int> next{0};
    const auto worker = [&] {
      for (int row = next.fetch_add(1); row < 15; row = next.fetch_add(1)) {
        const double rough = roughs[row / 5], mu = mus[row % 5];
        OpenPBRParams p = white_params(rough);
        p.base_metalness = Real(1);
        const Real3 wo = ar::to_world(frame, local_dir(mu, 0.6));
        for (int i = 0; i < kStrata; ++i)
          for (int j = 0; j < kStrata; ++j) {
            const uint64_t h = (uint64_t(i) * kStrata + j) ^ (uint64_t(rough * 100) << 40) ^ (uint64_t(mu * 100) << 50);
            const double u1 = (i + hash_unit(h)) / kStrata, u2 = (j + hash_unit(h ^ 0x5A5A5Aull)) / kStrata;
            const LobeSample s = ar::stack_sample(fx.table, fx.spec, p, frame, wo, Real2{Real(u1), Real(u2)}, TransportMode::Radiance);
            if (!(s.pdf > Real(0))) continue;
            const double w = double(ar::to_local(frame, s.wi).z) / double(s.pdf);
            for (int l = 0; l < 4; ++l) sums[row][l] += double(s.f.v[l]) * w;
          }
      }
    };
    std::vector<std::thread> pool;
    const unsigned n_threads = std::min(15u, std::max(1u, std::thread::hardware_concurrency()));
    for (unsigned t = 0; t < n_threads; ++t) pool.emplace_back(worker);
    for (std::thread& t : pool) t.join();
    double worst_lo = 10, worst_hi = 0;
    for (int row = 0; row < 15; ++row) {
      const double rough = roughs[row / 5], mu = mus[row % 5];
      for (int l = 0; l < 4; ++l) {
        const double e = sums[row][l] / (double(kStrata) * kStrata);
        worst_lo = std::fmin(worst_lo, e);
        worst_hi = std::fmax(worst_hi, e);
        if (l == 0) std::printf("  roughness %.2f mu_o %.2f  E = %.5f\n", rough, mu, e);
        INFO("roughness=" << rough << " mu=" << mu << " lane=" << l << " E=" << e);
        CHECK(e >= 0.99);
        CHECK(e <= 1.002);
      }
    }
    std::printf("lobe_stack metal furnace: E in [%.5f, %.5f]\n", worst_lo, worst_hi);
  }

  TEST_CASE("lobe_stack/white furnace out of range ior") {
    // IOR outside the table's [1, 3] (all lanes): the stack clamps the index for the lobe and the
    // table alike, so the lossless furnace stays in the same band, and the clamp is flagged. IOR 1.5
    // must not set the flag.
    Fixture fx;
    const Frame frame = tilted_frame();
    constexpr int kStrata = 512;
    std::printf("lobe_stack furnace out of range: %d samples per row, band 0.99 <= E <= 1.002\n", kStrata * kStrata);
    for (double ior : {4.0, 0.9, 1.5})
      for (double rough : {0.25, 0.5, 1.0})
        for (double mu : {0.05, 0.5, 1.0}) {
          const double iors[4] = {ior, ior, ior, ior};
          const OpenPBRParams p = make_params(kOnes, iors, 1.0, rough, 1.0, kOnes, 0.0, 1.0);
          const Real3 wo = ar::to_world(frame, local_dir(mu, 0.6));
          bool clamped = false;
          double sum = 0;
          for (int i = 0; i < kStrata; ++i)
            for (int j = 0; j < kStrata; ++j) {
              const uint64_t h = (uint64_t(i) * kStrata + j) ^ (uint64_t(rough * 100) << 40) ^ (uint64_t(mu * 100) << 50);
              const double u1 = (i + hash_unit(h)) / kStrata, u2 = (j + hash_unit(h ^ 0x1234567ull)) / kStrata;
              const LobeSample s = ar::stack_sample(fx.table, fx.spec, p, frame, wo, Real2{Real(u1), Real(u2)}, TransportMode::Radiance, &clamped);
              if (!(s.pdf > Real(0))) continue;
              sum += double(s.f.v[0]) * double(ar::to_local(frame, s.wi).z) / double(s.pdf);
            }
          const double e = sum / (double(kStrata) * kStrata);
          std::printf("  ior %.2f roughness %.2f mu_o %.2f  E = %.5f  eta_clamped %d\n", ior, rough, mu, e, int(clamped));
          INFO("ior=" << ior << " roughness=" << rough << " mu=" << mu << " E=" << e);
          CHECK(e >= 0.99);
          CHECK(e <= 1.002);
          CHECK(clamped == (ior != 1.5));
        }
  }

  // Chi-squared of the mixture sampler against stack_pdf over the hemisphere, in cells of equal
  // cos(theta) bands times azimuth sectors (shading frame); expected counts integrate stack_pdf with a
  // 32x32 midpoint rule per cell.
  static void chi2_mixture(double rough, double mu, double diff_rough, const char* name, double metalness = 0.0) {
    Fixture fx;
    const st::Run run = st::begin(name, st::gate_family());
    const Frame frame = tilted_frame();
    const double base[4] = {0.8, 0.8, 0.8, 0.8};
    OpenPBRParams p = make_params(kOnes, kIor15, 1.0, rough, 1.0, base, diff_rough, 1.0);
    p.base_metalness = Real(metalness);
    const Real3 wo_l = local_dir(mu, 0.7);
    double pdf_integral = 0.0, alive = 0.0, n_last = 0.0;  // of the last resolution run
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
              e += double(ar::stack_pdf(fx.table, fx.spec, p, frame, wo, ar::to_world(frame, local_dir(z, phi)), TransportMode::Radiance));
            }
          }
          h.expected[b * ns + s] = n * e / (kSub * kSub) * (1.0 / nb) * (2.0 * kPi / ns);
        }
      pdf_integral = 0.0, alive = 0.0, n_last = double(n);
      for (double e : h.expected) pdf_integral += e / n;
      for (int k = 0; k < n; ++k) {
        const uint64_t key = run.seed ^ (uint64_t(ires) << 56) ^ uint64_t(k);
        const LobeSample smp = ar::stack_sample(fx.table, fx.spec, p, frame, wo, Real2{Real(hash_unit(key)), Real(hash_unit(~key))}, TransportMode::Radiance);
        if (smp.pdf == Real(0)) continue;  // lost below the horizon; the expected counts exclude it
        const Real3 w = ar::to_local(frame, smp.wi);
        double ph = std::atan2(double(w.y), double(w.x));
        if (ph < 0) ph += 2.0 * kPi;
        const int b = std::min(nb - 1, std::max(0, int(double(w.z) * nb)));
        const int s = std::min(ns - 1, int(ph / (2.0 * kPi) * ns));
        h.observed[b * ns + s] += 1.0;
        alive += 1.0 / n;
      }
      return h;
    };
    const st::Chi2Result res = st::chi2_with_retry(run, gen, 8);
    // 0005 Verification 2: the integral of the pdf over the hemisphere equals 1 minus the measured
    // dead fraction (samples reflected below the horizon) within 1e-3, plus 4 binomial standard
    // errors of that measured fraction.
    const double se = std::sqrt(std::fmax(alive * (1.0 - alive), 0.0) / n_last);
    std::printf("lobe_stack pdf integral roughness=%.2f mu=%.2f metalness=%.2f: %.5f, alive fraction %.5f\n", rough, mu, metalness,
                pdf_integral, alive);
    CHECK(std::fabs(pdf_integral - alive) < 1e-3 + 4.0 * se);
    std::printf("lobe_stack chi2 roughness=%.2f mu=%.2f diffuse roughness=%.2f: chi2 %.1f dof %d p %.4f%s\n", rough, mu, diff_rough,
                res.chi2, res.dof, res.p, res.retried ? " (retried at 16)" : "");
    CHECK_MESSAGE(res.pass, res.message);
  }

  TEST_CASE("lobe_stack/chi2 mixture") {
    chi2_mixture(0.5, 0.3, 0.0, "lobe_stack/chi2 mixture r0.5 mu0.3");
    chi2_mixture(0.5, 0.8, 0.5, "lobe_stack/chi2 mixture r0.5 mu0.8 diffuse 0.5");
    chi2_mixture(1.0, 0.6, 1.0, "lobe_stack/chi2 mixture r1 mu0.6 diffuse 1");
    chi2_mixture(0.5, 0.4, 0.5, "lobe_stack/chi2 mixture r0.5 mu0.4 metalness 0.5", 0.5);
  }
}
