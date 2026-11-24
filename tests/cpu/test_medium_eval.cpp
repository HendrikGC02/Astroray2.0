// SPDX-License-Identifier: Apache-2.0
// ar-f64: compiled twice into this executable, once with AR_REAL_DOUBLE (cmake/sources.cmake).
// 0006 A.1, A.2: medium_eval runs a medium's volume program and returns the MediumSample at the
// path's four wavelengths. A folded colour equals the Jakob-Hanika evaluation of its own albedo
// coefficients, the complement of an absorber is exact, a slab's transmittance is exp(-sigma_t d),
// and a volume op outside a volume entry, an over-full block or an unresolvable ref stops the run.
// Flat media follow Real (0006 item 39), so the tolerance is 1e-6 in float and 1e-12 in double,
// relative to the density that scales the coefficient. Against a double evaluation the float build
// cannot be that close: the sigmoid's argument x = c0 l^2 + c1 l + c2 cancels in float (every colour
// slot has the same noise). That comparison therefore uses a bound derived per lane: x comes from
// two roundings (Horner with fma), so |dx| <= 2 u T with T = |c0| l^2 + |c1| l + |c2|; the sigmoid
// S(x) = 0.5 + x / (2 sqrt(1 + x^2)) has |S'| <= 1/2, and its own evaluation (x*x, 1 +, sqrt, 2 *,
// /, +) and the product with the density add at most 5 u (all values are <= 1). So the error in
// units of the density is at most u_sum (T + 5), u_sum the unit roundoff of Real plus that of the
// double reference (2^-24 + 2^-53 in float, 2^-52 in double); infinite coefficients (a grey 0 or
// 1) are exact. The test prints the worst error as a fraction of that bound.
// Ref: docs/decisions/0006-volumes-and-astrophysics.md A.1, A.2, D21; Jakob and Hanika 2019; the
// reference sigmoid is the 2019 form 0.5 + x / (2 sqrt(1 + x^2)) evaluated in double on the
// coefficients the colour's own lift (albedo_coeffs) returns, never on the block's refs.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../../src/host/data_resource.h"
#include "color/observer.h"
#include "color/rgb2spec.h"
#include "medium/medium_eval.h"
#include "sample/sobol.h"

namespace {

using ar::Lambda;
using ar::MediumBlock;
using ar::MediumSample;
using ar::Real;
using ar::VmStatus;
using ar::VolumeTerm;

constexpr bool kF32 = sizeof(Real) == 4;
constexpr double kTol = kF32 ? 1e-6 : 1e-12;
// Unit roundoff of Real plus that of the double reference (see the header comment).
constexpr double kUnitSum = (kF32 ? 5.9604644775390625e-8 : 1.1102230246251565e-16) + 1.1102230246251565e-16;

std::vector<uint8_t> read_data(const std::string& rel) {
  std::string here = __FILE__;
  std::string dir = here.substr(0, here.find_last_of("/\\") + 1);
  std::ifstream f(dir + "../../data/" + rel, std::ios::binary);
  if (!f) f.open("data/" + rel, std::ios::binary);
  REQUIRE_MESSAGE(f.good(), "data/" << rel << " not found");
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

struct Fixture {
  std::vector<uint8_t> jh_bytes;
  astroray::ObserverTables obs;
  ar::Rgb2SpecTable table;
  ar::SceneTables scene = {};
  Fixture() {
    auto ob = read_data("spectral/observer.bin");
    std::string err = astroray::parse_observer(ob.data(), ob.size(), &obs);
    REQUIRE_MESSAGE(err.empty(), err);
    jh_bytes = read_data("spectral/rec709.jh");
    const char* e = ar::jh_view(jh_bytes.data(), jh_bytes.size(), &table);
    REQUIRE_MESSAGE(e == nullptr, (e ? e : ""));
    scene.rgb2spec = &table;
    scene.observer.cmf = &obs.cmf[0][0];
    scene.observer.illum = obs.d65;
  }
};
const Fixture& fixture() {
  static const Fixture f;
  return f;
}

uint32_t word(ar::Op op) { return uint32_t(op); }

// One medium: the program {reserved 0, VOLUME_COEFFS, END} and its block at entry 1.
struct Rig {
  std::vector<uint32_t> program = {0u, word(ar::Op::VOLUME_COEFFS), word(ar::Op::END)};
  MediumBlock block = ar::medium_block_vacuum();
  ar::MediumDesc desc = {};
  ar::VmRegs regs = {};
  Rig() { block.entry = 1u; }
  ar::MediumTables tables() const { return ar::MediumTables{program.data(), &block, fixture().scene}; }
  VmStatus eval(const Lambda& lam, MediumSample* out) {
    return ar::medium_eval(tables(), desc, ar::Real3{Real(0.25), Real(-1), Real(3)}, lam, &regs, out);
  }
};

// A, S0, S1: the colours and densities of an absorber and two scatter lobes.
struct Terms {
  ar::Rgb a, s0, s1;
  float da, ds0, ds1, g0, g1;
};

void fill(Rig& r, const Terms& t) {
  uint32_t flags = 0;
  const ar::Rgb2SpecTable& tb = fixture().table;
  r.block.sigma_a = ar::volume_term_ref(tb, t.a, t.da, VolumeTerm::ABSORPTION, &flags);
  r.block.sigma_s[0] = ar::volume_term_ref(tb, t.s0, t.ds0, VolumeTerm::SCATTER, &flags);
  r.block.sigma_s[1] = ar::volume_term_ref(tb, t.s1, t.ds1, VolumeTerm::SCATTER, &flags);
  r.block.g[0] = t.g0;
  r.block.g[1] = t.g1;
  r.block.lobe_count = 2;
}

double sigmoid_d(const double* c, double lam) {
  const double x = (c[0] * lam + c[1]) * lam + c[2];
  if (std::isinf(x)) return x > 0 ? 1.0 : 0.0;
  return 0.5 + x / (2.0 * std::sqrt(1.0 + x * x));
}

// The reflectance of `rgb` at lam: the lift's own coefficients (stored as the float they are in a
// SpectrumRef), then the sigmoid in double.
double albedo_d(const ar::Rgb& rgb, double lam) {
  uint32_t flags = 0;
  const ar::Upsampled u = ar::albedo_coeffs(fixture().table, rgb, &flags);
  const double c[3] = {double(float(u.c.c[0])), double(float(u.c.c[1])), double(float(u.c.c[2]))};
  return sigmoid_d(c, lam);
}

// The derived error bound of albedo_d's float counterpart, in units of the density: u_sum (T + 5).
// An infinite coefficient gives an exact 0 or 1.
double albedo_bound(const ar::Rgb& rgb, double lam) {
  uint32_t flags = 0;
  const ar::Upsampled u = ar::albedo_coeffs(fixture().table, rgb, &flags);
  const double t = std::fabs(double(float(u.c.c[0]))) * lam * lam + std::fabs(double(float(u.c.c[1]))) * lam +
                   std::fabs(double(float(u.c.c[2])));
  return std::isinf(t) ? 0.0 : kUnitSum * (t + 5.0);
}

// err / bound, with an error at a zero bound counted as infinite.
double ratio(double err, double bound) { return err == 0.0 ? 0.0 : (bound > 0.0 ? err / bound : 1e300); }

// The colour slot's own lift of rgb: make_rgb_ref (float coefficients) then resolve() at the
// wavelengths, in Real: the Jakob-Hanika evaluation a REFLECTANCE slot gives.
ar::Spec lift(const ar::Rgb& rgb, const Lambda& lam) {
  ar::SpectrumRef ref;
  uint32_t flags = 0;
  REQUIRE(ar::make_rgb_ref(fixture().table, rgb, ar::SpectrumSemantic::REFLECTANCE, &ref, &flags));
  return ar::resolve(ref, lam, fixture().scene.observer);
}

Lambda lambdas(uint32_t i) {
  Lambda l;
  for (uint32_t k = 0; k < 4; ++k) l.nm[k] = Real(360.0 + 470.0 * double(ar::sample_independent(i, 0u, 0u, k, 0x6d3du)));
  return l;
}

ar::Rgb rgb(double r, double g, double b) { return ar::Rgb{{Real(r), Real(g), Real(b)}}; }

const Terms kCases[] = {
    {rgb(0.8, 0.3, 0.1), rgb(0.05, 0.9, 0.4), rgb(0.2, 0.2, 0.7), 1.5f, 2.0f, 0.75f, 0.6f, -0.3f},
    {rgb(0.9, 0.9, 0.2), rgb(0.5, 0.5, 0.5), rgb(0.0, 0.0, 0.0), 0.2f, 4.0f, 1.0f, 0.0f, 0.85f},
    {rgb(1.0, 1.0, 1.0), rgb(1.0, 1.0, 1.0), rgb(0.1, 0.6, 0.95), 3.0f, 0.5f, 2.5f, -0.9f, 0.99f},
    {rgb(0.0, 0.0, 0.0), rgb(0.7, 0.1, 0.05), rgb(0.33, 0.66, 0.99), 0.9f, 1.25f, 5.0f, 0.3f, 0.3f},
};

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("medium_eval/folded colour equals jakob-hanika") {
  constexpr uint32_t kN = 10000;
  double worst = 0.0, worst_independent = 0.0;
  for (const Terms& t : kCases) {
    Rig r;
    fill(r, t);
    for (uint32_t i = 0; i < kN; ++i) {
      const Lambda lam = lambdas(i);
      MediumSample s;
      REQUIRE(r.eval(lam, &s) == VmStatus::OK);
      const ar::Spec la = lift(t.a, lam), l0 = lift(t.s0, lam), l1 = lift(t.s1, lam);
      for (int l = 0; l < 4; ++l) {
        // sigma_a = density (1 - R), sigma_s lobe = density R: R the slot lift in Real, and again
        // as an independent double sigmoid of the same coefficients.
        const double want_a = double(t.da) * (1.0 - double(la.v[l]));
        const double want_0 = double(t.ds0) * double(l0.v[l]), want_1 = double(t.ds1) * double(l1.v[l]);
        worst = std::fmax(worst, std::fabs(double(s.sigma_a.v[l]) - want_a) / double(t.da));
        worst = std::fmax(worst, std::fabs(double(s.phase[0].weight.v[l]) - want_0) / double(t.ds0));
        worst = std::fmax(worst, std::fabs(double(s.phase[1].weight.v[l]) - want_1) / double(t.ds1));
        const double nm = double(lam.nm[l]);
        const double ia = double(t.da) * (1.0 - albedo_d(t.a, nm));
        const double i0 = double(t.ds0) * albedo_d(t.s0, nm), i1 = double(t.ds1) * albedo_d(t.s1, nm);
        worst_independent = std::fmax(worst_independent, ratio(std::fabs(double(s.sigma_a.v[l]) - ia) / double(t.da), albedo_bound(t.a, nm)));
        worst_independent = std::fmax(worst_independent, ratio(std::fabs(double(s.phase[0].weight.v[l]) - i0) / double(t.ds0), albedo_bound(t.s0, nm)));
        worst_independent = std::fmax(worst_independent, ratio(std::fabs(double(s.phase[1].weight.v[l]) - i1) / double(t.ds1), albedo_bound(t.s1, nm)));
        // sigma_s is the lobes' sum, and the phase weights are those lobes.
        CHECK(double(s.sigma_s.v[l]) == doctest::Approx(double(s.phase[0].weight.v[l]) + double(s.phase[1].weight.v[l])).epsilon(kTol));
        CHECK(s.emission.v[l] == Real(0));
      }
      CHECK(s.phase[0].kind == uint8_t(ar::PhaseKind::HG));
      CHECK(s.phase[0].p0 == t.g0);
      CHECK(s.phase[1].p0 == t.g1);
      CHECK(s.velocity.x == Real(0));
      CHECK(s.velocity.y == Real(0));
      CHECK(s.velocity.z == Real(0));
    }
  }
  std::printf("medium_eval folded colour: max error %.3e of the density (tolerance %.0e); against double %.3f of the derived bound\n",
              worst, kTol, worst_independent);
  CHECK(worst <= kTol);
  CHECK(worst_independent <= 1.0);
}

TEST_CASE("medium_eval/absorber complement is exact at grey and a lobe count of one") {
  // A white absorber absorbs nothing and a black one everything, to the last bit; a medium with
  // one lobe leaves the second lobe's weight and g at zero.
  Rig r;
  uint32_t flags = 0;
  const ar::Rgb2SpecTable& tb = fixture().table;
  r.block.sigma_a = ar::volume_term_ref(tb, rgb(1, 1, 1), 2.5f, VolumeTerm::ABSORPTION, &flags);
  r.block.sigma_s[0] = ar::volume_term_ref(tb, rgb(0, 0, 0), 4.0f, VolumeTerm::SCATTER, &flags);
  r.block.g[0] = 0.4f;
  r.block.lobe_count = 1;
  for (uint32_t i = 0; i < 100; ++i) {
    MediumSample s;
    REQUIRE(r.eval(lambdas(i), &s) == VmStatus::OK);
    for (int l = 0; l < 4; ++l) {
      CHECK(s.sigma_a.v[l] == Real(0));
      CHECK(s.sigma_s.v[l] == Real(0));
      CHECK(s.phase[1].weight.v[l] == Real(0));
    }
    CHECK(s.phase[1].p0 == 0.0f);
  }
  r.block.sigma_a = ar::volume_term_ref(tb, rgb(0, 0, 0), 2.5f, VolumeTerm::ABSORPTION, &flags);
  MediumSample s;
  REQUIRE(r.eval(lambdas(7), &s) == VmStatus::OK);
  for (int l = 0; l < 4; ++l) CHECK(s.sigma_a.v[l] == Real(2.5));
  CHECK(flags == 0u);
}

TEST_CASE("medium_eval/transmittance over a slab") {
  // Beer-Lambert through a slab of constant medium: exp(-sigma_t d) per lane, sigma_t the sample's
  // own extinction (absorber and both lobes), the exponential in double.
  Rig r;
  fill(r, kCases[0]);
  double worst = 0.0;
  for (const double d : {0.1, 0.5, 1.5}) {
    for (uint32_t i = 0; i < 2000; ++i) {
      const Lambda lam = lambdas(i);
      MediumSample s;
      REQUIRE(r.eval(lam, &s) == VmStatus::OK);
      const ar::Spec tr = ar::medium_transmittance(s, Real(d));
      for (int l = 0; l < 4; ++l) {
        const double sigma_t = double(s.sigma_a.v[l]) + double(s.sigma_s.v[l]);
        const double want = std::exp(-sigma_t * d);
        worst = std::fmax(worst, std::fabs(double(tr.v[l]) - want) / want);
      }
    }
  }
  std::printf("medium_eval slab transmittance: max relative error %.3e (tolerance %.0e)\n", worst, kTol);
  CHECK(worst <= kTol);
}

TEST_CASE("medium_eval/refused runs") {
  Rig r;
  fill(r, kCases[0]);
  MediumSample s;
  // No program: vacuum.
  r.block.entry = 0u;
  REQUIRE(r.eval(lambdas(1), &s) == VmStatus::OK);
  for (int l = 0; l < 4; ++l) CHECK(s.sigma_a.v[l] + s.sigma_s.v[l] + s.emission.v[l] == Real(0));
  r.block.entry = 1u;
  // More lobes than the block holds.
  r.block.lobe_count = 3;
  CHECK(r.eval(lambdas(1), &s) == VmStatus::BAD_OPERAND);
  r.block.lobe_count = 2;
  // A ref kind this build cannot resolve is refused, not read as zero.
  const ar::SpectrumRef good = r.block.sigma_s[1];
  r.block.sigma_s[1].kind = uint8_t(ar::SpectrumKind::TABLE);
  CHECK(r.eval(lambdas(1), &s) == VmStatus::BAD_OPERAND);
  r.block.sigma_s[1] = good;
  CHECK(r.eval(lambdas(1), &s) == VmStatus::OK);
  // A pair the exact interval bound does not cover is refused, not resolved: resolve() clamps a
  // CONSTANT REFLECTANCE to 1 and scales an ILLUMINANT by the observer.
  const ar::SpectrumRef absorber = r.block.sigma_a;
  r.block.sigma_a = ar::make_const_ref(ar::SpectrumSemantic::REFLECTANCE, Real(3));
  CHECK(r.eval(lambdas(1), &s) == VmStatus::BAD_OPERAND);
  r.block.sigma_a = absorber;
  r.block.sigma_s[0].semantic = uint8_t(ar::SpectrumSemantic::ILLUMINANT);
  CHECK(r.eval(lambdas(1), &s) == VmStatus::BAD_OPERAND);
  r.block.sigma_s[0].semantic = uint8_t(ar::SpectrumSemantic::REFLECTANCE);
  r.block.emission = ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, Real(1));
  CHECK(r.eval(lambdas(1), &s) == VmStatus::BAD_OPERAND);
  r.block.emission = ar::make_const_ref(ar::SpectrumSemantic::UNBOUNDED, Real(0));
  CHECK(r.eval(lambdas(1), &s) == VmStatus::OK);
  // VOLUME_COEFFS in a surface run has no medium to fill.
  ar::ShadeContext ctx = {};
  ar::OpenPBRParams rec = {};
  ar::Spec le = {};
  CHECK(ar::run_entry(r.program.data(), 1u, &r.regs, ctx, fixture().scene, &rec, &le) == VmStatus::NULL_POINTER);
}

}  // TEST_SUITE
