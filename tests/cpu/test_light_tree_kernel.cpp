// SPDX-License-Identifier: Apache-2.0
// ar-f64: compiled twice into this executable, once with AR_REAL_DOUBLE (cmake/sources.cmake).
// The kernel's light sampler with the tree and D10's floor (kernel/light/light_sampler.h): the
// mixture (1 - eps) p_tree + eps p_rad sums to 1 and matches its draws (0004 Verification D7), a
// light the tree never draws keeps eps p_rad, an estimate-0 triangle stays at 0 (D9), every light of
// positive power has a positive pmf, the world keeps a positive pmf beside a lamp it cannot outweigh
// (lead ruling on the world node), the draws follow the pmf (chi-squared, Sidak), and the pmf the
// emission hit in shade_surface reports is the frequency of the sampler at the same context. The
// gate compares the tree with the alias-table hook on 64 lamps (light-only NEE: the items are the
// whole direct-light estimate) by Welch's test and prints the variance ratio and the B-23 build time.
// Expected values are written out here: eps = 0.05 and p_rad as each light's power over the total.
// The view is built here from the host tree builder and a local alias table; the host light table,
// the stage and the scene are compiled for the float Real only, so those cases are float-only.
// Ref: docs/decisions/0004-integrator.md D7, D9, D10 and the Verification rows D7/D11, D10 tree;
// pbrt-v4 src/pbrt/util/sampling.cpp AliasTable::AliasTable (the local table) (Apache-2.0);
// Welch 1947 (unequal-variance comparison of two means).
#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <functional>
#include <random>
#include <thread>
#include <vector>

#include "common/stat.h"
#include "host/light/light_tree.h"
#include "light/light.h"
#include "light/light_sampler.h"
#include "light/light_tree.h"
#if !defined(AR_REAL_DOUBLE)
#include "../../include/astroray/astroray.h"
#include "device/cpu/cpu_device.h"
#include "film/present.h"
#include "host/data_resource.h"
#include "host/film/film_layout.h"
#include "host/light/env_bake.h"
#include "host/light/env_table.h"
#include "host/light/light_builder.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/scene/scene.h"
#include "host/scene/scene_tables.h"
#include "host/session/kg_builder.h"
#include "host/thread_pool.h"
#include "kernel/stages/shade_surface.h"
#include "vm/ops.h"
#endif

namespace st = astroray::stat;

namespace {

using ar::LightRecord;
using ar::Real;
using ar::Real3;

constexpr double kEps = 0.05;  // D10's floor weight
constexpr bool kF32 = sizeof(Real) == 4;
const ar::SceneSphere kScene{Real3{Real(5), Real(5), Real(5)}, Real(12)};

Real3 r3(double x, double y, double z) { return Real3{Real(x), Real(y), Real(z)}; }

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
  uint32_t bits() { return uint32_t(next() * 4294967296.0); }
};

ar::SpectrumRef white() { return ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, Real(1)); }

// Lights with their importance (the tree's) and radiometric power (the floor's), as the test
// states them: power = strength times the type's geometric factor, the same for both with a
// white spectrum, except where a case sets one of them to 0.
struct Lights {
  std::vector<LightRecord> rec;
  std::vector<double> imp, rad;
  uint32_t world = ar::kNoLight;
  std::vector<uint32_t> away;  // one-sided emitters facing away from every shading point
};

void push(Lights& L, const LightRecord& r, double strength, double imp_scale = 1.0, double rad_scale = 1.0) {
  const double power = double(ar::light_power(r, kScene, Real(1))) * strength;
  L.rec.push_back(r);
  L.imp.push_back(power * imp_scale);
  L.rad.push_back(power * rad_scale);
}

// 64 local lights at z in [1, 10] cycling point, sphere, rect, disk (down-facing), two-sided
// triangle, triangle facing down, and every eighth a triangle facing up (away from the shading
// points below: the tree gives it 0, the floor does not); optionally the world, with no radiometric
// power as an environment world has none (0004 D10).
Lights mixed(bool world) {
  Rng rng{175};
  auto u = [&](double a, double b) { return a + (b - a) * rng.next(); };
  Lights L;
  for (int i = 0; i < 64; ++i) {
    const Real3 p = r3(u(0, 10), u(0, 10), u(1, 10));
    const double s = u(1, 100), a = u(0.2, 1), b = u(0.2, 1);
    if (i % 8 == 7) {
      L.away.push_back(uint32_t(L.rec.size()));
      push(L, ar::light_make_triangle(p, p + r3(a, 0, 0), p + r3(0, b, 0), 1, uint32_t(i), 0, Real(s), white()), s);
      continue;
    }
    switch (i % 6) {
      case 0: push(L, ar::light_make(ar::LightType::POINT, p, Real(0), Real(s), white()), s); break;
      case 1: push(L, ar::light_make(ar::LightType::SPHERE, p, Real(u(0.05, 0.4)), Real(s), white()), s); break;
      case 2: push(L, ar::light_make_area(ar::LightType::AREA_RECT, p, r3(1, 0, 0), r3(0, 1, 0), Real(a), Real(b), Real(s), white()), s); break;
      case 3: push(L, ar::light_make_area(ar::LightType::AREA_DISK, p, r3(1, 0, 0), r3(0, 1, 0), Real(a), Real(0), Real(s), white()), s); break;
      case 4: push(L, ar::light_make_triangle(p, p + r3(a, 0, 0), p + r3(0, b, 0), 1, uint32_t(i), ar::kLightTwoSided, Real(s), white()), s); break;
      default: push(L, ar::light_make_triangle(p, p + r3(0, b, 0), p + r3(a, 0, 0), 1, uint32_t(i), 0, Real(s), white()), s);
    }
  }
  if (world) {
    L.world = uint32_t(L.rec.size());
    push(L, ar::light_make_world(Real(0.02), white()), 0.02, 1.0, 0.0);
  }
  return L;
}

// pbrt-v4 AliasTable::AliasTable over the positive weights; pmf[i] = w_i / sum.
void alias_table(const std::vector<double>& w, std::vector<ar::AliasBin>* bins, std::vector<uint32_t>* bin_light, std::vector<float>* pmf) {
  double sum = 0.0;
  for (double x : w) sum += x;
  pmf->assign(w.size(), 0.0f);
  std::vector<double> p;
  for (uint32_t i = 0; i < w.size(); ++i)
    if (w[i] > 0.0) (*pmf)[i] = float(w[i] / sum), bin_light->push_back(i), p.push_back(w[i] / sum);
  const uint32_t n = uint32_t(p.size());
  bins->assign(n, ar::AliasBin{1.0f, 0u});
  std::vector<std::pair<double, uint32_t>> under, over;
  for (uint32_t b = 0; b < n; ++b) (p[b] * n < 1.0 ? under : over).push_back({p[b] * n, b});
  while (!under.empty() && !over.empty()) {
    const auto un = under.back(), ov = over.back();
    under.pop_back(), over.pop_back();
    (*bins)[un.second] = {float(un.first), ov.second};
    const double excess = un.first + ov.first - 1.0;
    (excess < 1.0 ? under : over).push_back({excess, ov.second});
  }
  for (const auto& o : over) (*bins)[o.second] = {1.0f, o.second};
  for (const auto& o : under) (*bins)[o.second] = {1.0f, o.second};
}

// The sampler's view over `L`: the host tree of the importances, the importance alias table (the
// oracle) and the radiometric one.
struct View {
  astroray::host::LightTree tree;
  std::vector<ar::AliasBin> bins, rbins;
  std::vector<uint32_t> bl, rbl;
  std::vector<float> pmf, rpmf;
  ar::LightSamplerView v{};
  explicit View(const Lights& L) {
    tree = astroray::host::build_light_tree(L.rec, L.imp, kScene);
    alias_table(L.imp, &bins, &bl, &pmf);
    alias_table(L.rad, &rbins, &rbl, &rpmf);
    v.bins = bins.data(), v.bin_light = bl.data(), v.pmf = pmf.data();
    v.bin_count = uint32_t(bins.size()), v.light_count = uint32_t(L.rec.size());
    v.tree = tree.view();
    v.rad_bins = rbins.data(), v.rad_bin_light = rbl.data(), v.rad_pmf = rpmf.data(), v.rad_bin_count = uint32_t(rbins.size());
  }
};

// Below every light, normal within 5 degrees of +z.
ar::LightSampleContext shade_point(Rng& rng) {
  const double kPi = 3.14159265358979323846;
  const double ct = 1.0 - rng.next() * (1.0 - std::cos(5.0 * kPi / 180.0)), s = std::sqrt(1.0 - ct * ct), ph = 2.0 * kPi * rng.next();
  const Real3 n = r3(s * std::cos(ph), s * std::sin(ph), ct);
  return ar::light_context_point(r3(10 * rng.next(), 10 * rng.next(), -2 - 8 * rng.next()), n, n);
}

#if !defined(AR_REAL_DOUBLE)
const ar::ObserverView& observer() {
  static astroray::ObserverTables t;
  static ar::ObserverView v = [] {
    astroray::DataBlob b;
    REQUIRE(astroray::find_data("spectral/observer.bin", &b));
    REQUIRE(astroray::parse_observer(b.data, b.size, &t).empty());
    return ar::ObserverView{&t.cmf[0][0], t.d65};
  }();
  return v;
}
#endif

double total(const std::vector<double>& w) {
  double s = 0.0;
  for (double x : w) s += x;
  return s;
}

}  // namespace

TEST_SUITE("fast") {

AR_RT_CASE("light_tree_kernel/pmf sums to one with floor") {
  // Every light's mixture pmf over 200 points sums to 1: the tree's branches sum to 1 at every
  // node these points reach, and eps p_rad sums to eps. p_rad is stored as float, so the sum is
  // compared with 1 - eps + eps times the sum of the stored p_rad, which is 1 to float rounding.
  const Lights L = mixed(true);
  const View V(L);
  const double tol = kF32 ? 1e-5 : 1e-12;
  double rad_sum = 0.0;
  for (float x : V.rpmf) rad_sum += double(x);
  CHECK(std::fabs(rad_sum - 1.0) <= 1e-6);
  const double want = 1.0 - kEps + kEps * rad_sum;
  Rng rng{1751};
  double worst = 0.0;
  for (int k = 0; k < 200; ++k) {
    const ar::LightSampleContext ctx = shade_point(rng);
    double sum = 0.0;
    for (uint32_t i = 0; i < V.v.light_count; ++i) {
      const double p = double(ar::light_sampler_pmf(V.v, ctx, i, false));
      REQUIRE(std::isfinite(p));
      REQUIRE(p >= 0.0);
      sum += p;
    }
    worst = std::max(worst, std::fabs(sum - want));
  }
  INFO("worst |sum - 1| = " << worst);
  CHECK(worst <= tol);
  CHECK(ar::light_sampler_pmf(V.v, ar::light_context_point(r3(0, 0, 0), r3(0, 0, 1), r3(0, 0, 1)), V.v.light_count, false) == Real(0));
}

AR_RT_CASE("light_tree_kernel/pmf matches sample with floor") {
  // 10^4 draws over 10 points, both branches of the mixture: pmf(ctx, sampled) is the returned pmf
  // (0004 Verification D7, 1e-4 relative), and both branches drew.
  const Lights L = mixed(true);
  const View V(L);
  Rng rng{1752};
  int drawn = 0, floor_draws = 0, bad = 0;
  for (int k = 0; k < 10; ++k) {
    const ar::LightSampleContext ctx = shade_point(rng);
    for (int j = 0; j < 1000; ++j) {
      const Real uq = Real(rng.next());
      const ar::SampledLight s = ar::light_sampler_sample(V.v, ctx, rng.bits(), uq, false);
      REQUIRE(s.light < V.v.light_count);
      REQUIRE(s.pmf > Real(0));
      const double p = double(ar::light_sampler_pmf(V.v, ctx, s.light, false));
      bad += !(std::fabs(p - double(s.pmf)) <= 1e-4 * double(s.pmf));
      floor_draws += double(uq) < kEps;
      ++drawn;
    }
  }
  CHECK(bad == 0);
  CHECK(drawn == 10000);
  CHECK(floor_draws > 300);
  CHECK(floor_draws < 700);
}

AR_RT_CASE("light_tree_kernel/zero importance has radiometric pmf") {
  // A light the tree has no leaf for (importance 0) but with radiometric power, as a UV lamp's
  // display importance is 0 without fluorescence (no M1b lamp has such a spectrum, so the table is
  // built here): its pmf is eps p_rad at every point, and the floor's draw on its bin returns it
  // with that pmf.
  Lights L = mixed(false);
  const uint32_t uv = uint32_t(L.rec.size());
  push(L, ar::light_make(ar::LightType::POINT, r3(4, 4, 3), Real(0), Real(30), white()), 30.0, 0.0, 1.0);
  const View V(L);
  REQUIRE(V.tree.emitters[uv].depth == ar::kLightTreeAbsent);
  const double p_rad = L.rad[uv] / total(L.rad);
  Rng rng{1753};
  for (int k = 0; k < 20; ++k) {
    const ar::LightSampleContext ctx = shade_point(rng);
    CHECK(double(ar::light_sampler_pmf(V.v, ctx, uv, false)) == doctest::Approx(kEps * p_rad).epsilon(1e-6));  // p_rad is stored as float
    CHECK(ar::light_sampler_pmf(V.v, ctx, uv, true) == Real(0));  // the oracle has no bin for it
  }
  uint32_t bin = 0;
  while (V.rbl[bin] != uv) ++bin;
  const uint32_t u_bin = uint32_t(((uint64_t(bin) << 32) + V.v.rad_bin_count - 1) / V.v.rad_bin_count);
  const ar::LightSampleContext ctx = ar::light_context_point(r3(5, 5, -3), r3(0, 0, 1), r3(0, 0, 1));
  const ar::SampledLight s = ar::light_sampler_sample(V.v, ctx, u_bin, Real(0.1 * kEps * double(V.rbins[bin].q)), false);
  CHECK(s.light == uv);
  CHECK(double(s.pmf) == doctest::Approx(kEps * p_rad).epsilon(1e-6));
}

AR_RT_CASE("light_tree_kernel/every light of positive power has positive pmf") {
  // Including the triangles facing away, which the tree gives 0 at every point here: the floor
  // gives them eps p_rad; and the world, which has no radiometric bin, through the tree.
  const Lights L = mixed(true);
  const View V(L);
  Rng rng{1754};
  int zero = 0, away_tree_zero = 0;
  for (int k = 0; k < 200; ++k) {
    const ar::LightSampleContext ctx = shade_point(rng);
    for (uint32_t i = 0; i < V.v.light_count; ++i) {
      const double p = double(ar::light_sampler_pmf(V.v, ctx, i, false));
      zero += !(p > 0.0);
      if (L.rad[i] > 0.0) CHECK(p >= (1.0 - 1e-6) * kEps * L.rad[i] / total(L.rad));
    }
    for (uint32_t a : L.away) away_tree_zero += ar::light_tree_pmf(V.v.tree, ctx.p, ctx.n_shade, a) == Real(0);
  }
  CHECK(zero == 0);
  CHECK(away_tree_zero == 200 * int(L.away.size()));
}

AR_RT_CASE("light_tree_kernel/world pmf stays positive") {
  // The world (no radiometric bin) beside lamps that outweigh it by any factor: at random points,
  // exactly on a point lamp, a nearest representable step from one (d^2 near FLT_MIN), and 1e-20
  // from a lamp near the origin, where 1 / d^2 overflows float. Its tree pmf and its mixture pmf
  // stay above 0, so the floor's absence for it never makes it unreachable.
  Lights L = mixed(true);
  const uint32_t tiny = uint32_t(L.rec.size());
  push(L, ar::light_make(ar::LightType::POINT, r3(0, 0, 1e-20), Real(0), Real(1000), white()), 1000.0);
  const View V(L);
  REQUIRE(L.world != ar::kNoLight);
  REQUIRE(V.rpmf[L.world] == 0.0f);
  std::vector<ar::LightSampleContext> pts;
  Rng rng{1755};
  for (int k = 0; k < 200; ++k) pts.push_back(shade_point(rng));
  const Real3 up = r3(0, 0, 1);
  const Real3 lamp = ar::light_position(L.rec[0]);
  REQUIRE(L.rec[0].type == uint8_t(ar::LightType::POINT));
  pts.push_back(ar::light_context_point(lamp, up, up));  // on the lamp
  pts.push_back(ar::light_context_point(r3(double(lamp.x), double(lamp.y), double(std::nextafter(lamp.z, Real(-100)))), up, up));
  pts.push_back(ar::light_context_point(r3(0, 0, 0), up, up));  // 1e-20 below the tiny lamp
  pts.push_back(ar::light_context_point(r3(0, 0, -1e-4), up, up));
  int bad_tree = 0, bad_mix = 0;
  for (const ar::LightSampleContext& c : pts) {
    bad_tree += !(ar::light_tree_pmf(V.v.tree, c.p, c.n_shade, L.world) > Real(0));
    bad_mix += !(ar::light_sampler_pmf(V.v, c, L.world, false) > Real(0));
  }
  CHECK(bad_tree == 0);
  CHECK(bad_mix == 0);
  // At the origin the tiny lamp's importance is infinite in float: the world still gets the floor
  // branch probability of light_tree_split, and its pmf is what a draw reports.
  const ar::LightSampleContext c0 = ar::light_context_point(r3(0, 0, 0), up, up);
  const double pw = double(ar::light_tree_pmf(V.v.tree, c0.p, c0.n_shade, L.world));
  INFO("world tree pmf at the origin " << pw);
  CHECK(std::isfinite(pw));
  CHECK(double(ar::light_sampler_pmf(V.v, c0, tiny, false)) <= 1.0);
}

AR_RT_CASE("light_tree_kernel/selection frequency matches the mixture") {
  // 10^6 draws at one point against N pmf per light (chi-squared, pooled cells, Sidak).
  const st::Run run = st::begin("light_tree_kernel/selection frequency matches the mixture" AR_RT_SUFFIX);
  const Lights L = mixed(true);
  const View V(L);
  const ar::LightSampleContext ctx = ar::light_context_point(r3(4, 6, -3), r3(0.05, -0.03, 1), r3(0.05, -0.03, 1));
  const int n = 1000000;
  std::vector<double> observed(V.v.light_count + 1, 0.0), expected(V.v.light_count + 1, 0.0);
  std::mt19937_64 rng(run.seed);
  for (int k = 0; k < n; ++k) {
    const uint64_t b = rng();
    const ar::SampledLight s = ar::light_sampler_sample(V.v, ctx, uint32_t(b), Real(double(b >> 40) * (1.0 / 16777216.0)), false);
    observed[s.light == ar::kNoLight ? V.v.light_count : s.light] += 1.0;
  }
  double sum = 0.0;
  for (uint32_t i = 0; i < V.v.light_count; ++i) sum += expected[i] = double(ar::light_sampler_pmf(V.v, ctx, i, false));
  expected[V.v.light_count] = std::max(0.0, 1.0 - sum);
  for (double& e : expected) e *= n;
  const st::Chi2Result res = st::chi2_test(run, observed, expected);
  std::printf("light_tree_kernel chi2 %.1f dof %d p %.4f\n", res.chi2, res.dof, res.p);
  CHECK_MESSAGE(res.pass, res.message);
}

#if !defined(AR_REAL_DOUBLE)

TEST_CASE("light_tree_kernel/estimate zero stays zero") {
  // The host table: a triangle whose emission estimate is 0 gets no tree leaf and no radiometric
  // bin, so its pmf is 0 under the floor too (0004 D9) and nothing draws it; beside it a triangle
  // facing away, which the tree gives 0, keeps eps p_rad, so the zero is D9's and not a missing floor.
  using namespace astroray::host;
  std::vector<LightInput> in;
  LightInput zero, away, lamp;
  zero.record = ar::light_make_triangle(r3(1, 1, 3), r3(1, 2, 3), r3(2, 1, 3), 0, 0, ar::kLightEmissionProgram, Real(0), white());
  zero.terms = {{white(), 0.0f}};
  away.record = ar::light_make_triangle(r3(4, 4, 3), r3(5, 4, 3), r3(4, 5, 3), 0, 1, 0, Real(10), white());  // faces +z
  lamp.record = ar::light_make(ar::LightType::POINT, r3(3, 3, 4), Real(0), Real(50), white());
  const auto t = build_light_table({zero, away, lamp}, kScene, observer(), nullptr);
  CHECK(t->importance[0] == 0.0);
  CHECK(t->radiometric[0] == 0.0);
  CHECK(t->tree_emitters[0].depth == ar::kLightTreeAbsent);
  CHECK(t->rad_pmf[0] == 0.0f);
  // Radiometric power written out: an ILLUMINANT constant 1 is the observer's illuminant table, so
  // its integral is that table's sum over the 1 nm samples, times the strength, times pi A for a
  // triangle and 1 for a lamp.
  double illum = 0.0;
  for (int i = 0; i < ar::kObsSamples; ++i) illum += double(observer().illum[i]);
  const double pi = 3.14159265358979323846, rad_away = illum * 10.0 * pi * 0.5, rad_lamp = illum * 50.0;
  CHECK(t->radiometric[1] == doctest::Approx(rad_away).epsilon(1e-5));
  CHECK(t->radiometric[2] == doctest::Approx(rad_lamp).epsilon(1e-5));
  const ar::LightSamplerView v = t->view();
  Rng rng{1756};
  uint32_t drawn_zero = 0;
  for (int k = 0; k < 50; ++k) {
    const ar::LightSampleContext ctx = ar::light_context_point(r3(6 * rng.next(), 6 * rng.next(), -1 - 4 * rng.next()), r3(0, 0, 1), r3(0, 0, 1));
    CHECK(ar::light_sampler_pmf(v, ctx, 0, false) == Real(0));
    CHECK(ar::light_tree_pmf(v.tree, ctx.p, ctx.n_shade, 1) == Real(0));
    CHECK(double(ar::light_sampler_pmf(v, ctx, 1, false)) == doctest::Approx(kEps * rad_away / (rad_away + rad_lamp)).epsilon(1e-5));
    for (int j = 0; j < 200; ++j) drawn_zero += ar::light_sampler_sample(v, ctx, rng.bits(), Real(rng.next()), false).light == 0u;
  }
  CHECK(drawn_zero == 0u);
}

TEST_CASE("light_tree_kernel/environment world has no radiometric bin") {
  // A baked program world is WORLD_ENV: importance from its bake, radiometric power 0 (no bin), a
  // tree leaf, and a positive pmf from the tree at every point (0004 D10).
  using namespace astroray::host;
  LightInput lamp, world;
  lamp.record = ar::light_make(ar::LightType::POINT, r3(3, 3, 4), Real(0), Real(50), white());
  world.record = ar::light_make_world(Real(1.5), white(), ar::kLightWorldProgram);
  BakedEnvironment baked;
  baked.table = std::make_shared<const EnvTable>(build_env_table(std::vector<double>(64, 1.0), 8));
  baked.max = 1.0, baked.mean = 0.5;
  const auto t = build_light_table({lamp, world}, kScene, observer(), nullptr, {}, &baked);
  REQUIRE(t->lights[1].type == uint8_t(ar::LightType::WORLD_ENV));
  CHECK(t->importance[1] > 0.0);
  CHECK(t->radiometric[1] == 0.0);
  CHECK(t->rad_pmf[1] == 0.0f);
  CHECK(t->rad_bins.size() == 1u);
  CHECK(t->tree_emitters[1].depth != ar::kLightTreeAbsent);
  const ar::LightSamplerView v = t->view();
  Rng rng{1757};
  for (int k = 0; k < 50; ++k) {
    const ar::LightSampleContext ctx = ar::light_context_point(r3(6 * rng.next(), 6 * rng.next(), -1 - 4 * rng.next()), r3(0, 0, 1), r3(0, 0, 1));
    CHECK(ar::light_sampler_pmf(v, ctx, 1, false) > Real(0));
  }
}
#endif

}  // TEST_SUITE("fast")

#if !defined(AR_REAL_DOUBLE)
namespace {

using namespace astroray;
using namespace astroray::host;

constexpr ar_key kMesh = 100, kEmit = 1;

// A constant emission of colour (1, 1, 1) and strength 4 (folded into the constant block).
struct Graph {
  std::vector<ar_graph_node> nodes;
  std::vector<ar_graph_link> links;
  std::vector<float> f;
  ar_graph_desc emit() {
    nodes.assign(3, ar_graph_node{});
    f = {1.0f, 1.0f, 1.0f, 4.0f};
    nodes[0].op = uint32_t(ar::Op::CONST_F32X3), nodes[0].f32_first = 0, nodes[0].f32_count = 3;
    nodes[1].op = uint32_t(ar::Op::CONST_F32), nodes[1].f32_first = 3, nodes[1].f32_count = 1;
    nodes[2].op = uint32_t(ar::Op::EMIT_LE);
    links = {{0u, 0u, 2u, 0u}, {1u, 0u, 2u, 1u}};
    ar_graph_desc d = {};
    d.node_count = 3, d.link_count = 2, d.f32_count = 4, d.nodes = nodes.data(), d.links = links.data(), d.params_f32 = f.data();
    return d;
  }
};

// A receiver quad of half-size 1 at z = 0 facing +z (instance 0, the default material), and the
// same quad instanced as emitters (scale, centre), flipped to face -z; n paths, the shadow pool and
// a film of n slots (the pool slot is the film slot and the shadow slot).
struct Emitter {
  double s, x, y, z;
};
struct Rig {
  ContextState sctx;
  Scene scene{sctx, SceneConfig{2, {}, {}}};
  std::shared_ptr<const SceneTableSet> set;
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool{ctx, 1};
  CpuDevice dev{ledger, pool};
  std::unique_ptr<Film> film;
  ar::DisplayTarget display{};
  ar::KernelGlobals kg{};
  ar::PathStateSoA path{};
  ar::ShadowItemSoA shadow{};
  std::vector<uint32_t> queue, active, sample_id, flags, hit_prim, hit_inst, prev_ng, prev_ns, sh_queue, sh_target;
  std::vector<uint8_t> next_stage, term, ray_type, sh_next;
  std::vector<uint16_t> event_index;
  std::vector<ar::U16x6> bounces;
  std::vector<ar::Real3> ray_o, ray_d, sh_o, sh_d;
  std::vector<ar::Real2> hit_uv;
  std::vector<ar::Real> hit_t, eta_scale, prev_dist, sh_tmax;
  std::vector<ar::Spec> lambda_t, beta, r_u, r_l, sh_contrib;
  ar::LightSamplerView sampler{};
  ar::Spec lambda0{};
  uint32_t n;

  Rig(const std::vector<Emitter>& emitters, uint32_t slots) : n(slots) {
    scene.meshes = std::make_shared<MeshStore>();
    scene.materials = std::make_shared<MaterialStore>(scene.meshes->shared_ledger());
    commit([&](Edit& e) {
      if (!emitters.empty()) {
        Graph g;
        const ar_graph_desc d = g.emit();
        REQUIRE(scene.materials->set_graph(e, kEmit + 1000, &d) == AR_OK);
        ar_material_desc m = {};
        m.surface_graph = kEmit + 1000;
        m.emission_sampling = AR_EMISSION_SAMPLING_FRONT;
        REQUIRE(scene.materials->set_material(e, kEmit, &m) == AR_OK);
      }
      const std::vector<float> pos{-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0}, uv{0, 0, 1, 0, 1, 1, 0, 1};
      const std::vector<int32_t> cv{0, 1, 2, 3}, fs{0};
      ar_mesh_desc md = {};
      md.key = kMesh, md.vertex_count = 4, md.corner_count = 4, md.face_count = 1, md.motion_steps = 1, md.uv_layer_count = 1;
      ar_mesh_buffers b = {};
      REQUIRE(mesh_begin(scene, e, &md, &b) == AR_OK);
      std::memcpy(*b.positions, pos.data(), pos.size() * 4);
      std::memcpy(*b.corner_vert, cv.data(), cv.size() * 4);
      std::memcpy(*b.face_start, fs.data(), fs.size() * 4);
      for (uint64_t i = 0; i < b.corner_normals_count; ++i) (*b.corner_normals)[i] = i % 3 == 2 ? 1.0f : 0.0f;
      std::memcpy(*b.uv, uv.data(), uv.size() * 4);
      REQUIRE(e.item_commit(kMesh) == AR_OK);
    });
    commit([&](Edit& e) {
      const uint32_t ni = 1u + uint32_t(emitters.size());
      std::vector<ar_key> geom(ni, kMesh), object, keys{kEmit};
      std::vector<ar_xform> xf(ni);
      xf[0].m[0] = xf[0].m[5] = xf[0].m[10] = 1.0f;
      for (uint32_t k = 0; k < emitters.size(); ++k) {
        const Emitter& em = emitters[k];
        ar_xform& x = xf[k + 1];
        x.m[0] = float(em.s), x.m[5] = -float(em.s), x.m[10] = -1.0f;  // flipped: faces -z
        x.m[3] = float(em.x), x.m[7] = float(em.y), x.m[11] = float(em.z);
      }
      for (uint32_t i = 0; i < ni; ++i) object.push_back(500 + i);
      std::vector<uint32_t> zeros(ni, 0), vis(ni, AR_VISIBILITY_CAMERA | AR_VISIBILITY_SHADOW), moff(ni, 0), mcount(ni, 1);
      mcount[0] = 0;
      std::vector<ar_rgba> colors(ni, ar_rgba{{1, 1, 1, 1}});
      std::vector<uint8_t> lg(ni, 0);
      ar_instances_desc d = {};
      d.instance_count = ni, d.motion_steps = 1, d.transform_count = ni, d.material_key_count = emitters.empty() ? 0u : 1u;
      d.geom_keys = geom.data(), d.xf = xf.data(), d.object_keys = object.data();
      d.random_ids = d.pass_indices = d.crypto_objects = d.crypto_assets = zeros.data();
      d.colors = colors.data(), d.visibility = vis.data(), d.flags = zeros.data(), d.light_groups = lg.data();
      d.material_offsets = moff.data(), d.material_counts = mcount.data(), d.material_keys = keys.data();
      REQUIRE(set_instances(e, &d) == AR_OK);
    });
    set = acquire_scene_tables(scene);
    REQUIRE(set);
    FilmRequest req;
    req.max_width = n, req.max_height = 1, req.slot_capacity = n;
    req.statistics = true, req.termination_stats = true;
    film = std::make_unique<Film>(dev, FilmLayout::build(req), Owner{1, 1});
    film->data().inv_radiance_scale = 1.0 / set->lights->radiance_scale;
    for (uint32_t i = 0; i < n; ++i) {
      queue.push_back(i), active.push_back(i), sample_id.push_back(i), flags.push_back(ar::set_hero_lane(0u, i & 3u));
      next_stage.push_back(uint8_t(ar::Stage::SHADE_SURFACE)), term.push_back(uint8_t(ar::Term::ACTIVE)), ray_type.push_back(1u);
      event_index.push_back(0), bounces.push_back(ar::U16x6{{0, 0, 0, 0, 0, 0}});
      hit_inst.push_back(0u), hit_prim.push_back(0u), hit_uv.push_back(ar::Real2{ar::Real(0.3), ar::Real(0.3)}), hit_t.push_back(ar::Real(1));
      ray_o.push_back(ar::Real3{0, 0, 1}), ray_d.push_back(ar::Real3{0, 0, -1}), eta_scale.push_back(ar::Real(1));
      prev_ng.push_back(0u), prev_ns.push_back(0u), prev_dist.push_back(ar::Real(0));
      for (int l = 0; l < ar::kLanes; ++l) lambda0.v[l] = ar::Real(430 + 70 * l);
      lambda_t.push_back(lambda0), beta.push_back(ar::spec_fill(ar::Real(1))), r_u.push_back(ar::spec_fill(ar::Real(1)));
      r_l.push_back(ar::spec_fill(ar::Real(1)));
      sh_queue.push_back(i), sh_target.push_back(~0u), sh_next.push_back(uint8_t(ar::Stage::SHADOW_TERMINATED));
      sh_o.push_back(ar::Real3{0, 0, 0}), sh_d.push_back(ar::Real3{0, 0, 0}), sh_tmax.push_back(ar::Real(-1));
      sh_contrib.push_back(ar::spec_fill(ar::Real(0)));
    }
    path.sample_id = sample_id.data(), path.next_stage = next_stage.data(), path.term = term.data();
    path.ray_type = ray_type.data(), path.flags = flags.data(), path.bounces = bounces.data();
    path.event_index = event_index.data(), path.ray_o = ray_o.data(), path.ray_d = ray_d.data();
    path.lambda_t = lambda_t.data(), path.beta = beta.data(), path.r_u = r_u.data(), path.r_l = r_l.data();
    path.eta_scale = eta_scale.data(), path.prev_ng = prev_ng.data(), path.prev_ns = prev_ns.data(), path.prev_dist = prev_dist.data();
    path.hit_t = hit_t.data(), path.hit_prim = hit_prim.data(), path.hit_inst = hit_inst.data(), path.hit_uv = hit_uv.data();
    shadow.next_stage = sh_next.data(), shadow.ray_o = sh_o.data(), shadow.ray_d = sh_d.data();
    shadow.tmax = sh_tmax.data(), shadow.contribution = sh_contrib.data(), shadow.target = sh_target.data();
    KgSession in;
    in.film = &film->data(), in.display = &display, in.active_pixels = active.data();
    in.uniforms.width = n, in.uniforms.height = 1, in.uniforms.samples_per_pass = 1;
    in.uniforms.seed = 5;
    in.uniforms.sampler_mode = ar::SAMPLER_INDEPENDENT;
    in.uniforms.max_bounces[0] = 8;
    kg = build_kernel_globals(set.get(), in);
    kg.path = &path, kg.shadow = &shadow, kg.main_queue = queue.data(), kg.shadow_queue = sh_queue.data();
  }
  template <class F>
  void commit(F&& body) {
    auto e = scene.begin_edit();
    REQUIRE(e);
    body(*e);
    uint64_t g = 0;
    REQUIRE(e->commit(&g) == AR_OK);
    scene.materials->end_edit();
    e.reset();
    scene.generations().wait_idle();
    REQUIRE(scene.generations().failed() == 0);
  }
  void run() { cpu_stage_shade_surface(kg, ar::StepEntry{0, n, 0}, 0, n); }
  void clear_film() {
    for (uint32_t i = 0; i < 4u * n; ++i) film->data().slot_L[i] = 0.0f;
  }
};

// Nearest representable point of triangle `lr` at barycentrics (1/3, 1/3).
ar::Real3 centroid(const ar::LightRecord& lr) {
  return (ar::tri_vertex(lr, 0) + ar::tri_vertex(lr, 1) + ar::tri_vertex(lr, 2)) * ar::Real(1.0 / 3.0);
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("light_tree_kernel/emitter hit pmf matches the sampled frequency") {
  // Four emitter quads (eight one-sided triangle lights) of equal radiance at different sizes and
  // distances above a point P0 on the receiver, so the tree (distance) and the alias table (power)
  // disagree. One path per light arrives at the light's centroid from P0 after a diffuse bounce;
  // with r_u = r_l = 1 its emission is weighted 1 / (1 + p), p = pmf(P0, light) pdf_li, while the
  // same path at the first vertex is weighted 1, so their ratio gives the pmf shade_surface used.
  // That pmf must be the frequency with which the sampler picks the light at the same context (P0
  // and the oct-decoded normal prev_light_context rebuilds): chi-squared over 2^20 draws.
  const st::Run run = st::begin("light_tree_kernel/emitter hit pmf matches the sampled frequency");
  const std::vector<Emitter> em{{0.2, 0.5, 0.2, 1.0}, {1.0, -1.5, 1.0, 3.0}, {1.5, 2.0, -2.0, 4.0}, {0.5, -0.5, -1.5, 2.0}};
  const uint32_t nl = 8;  // two triangles per quad
  Rig rig(em, nl);
  const LightTable& t = *rig.set->lights;
  REQUIRE(t.lights.size() == nl);
  rig.kg.session.max_bounces[0] = 1;  // the emitter vertex adds its emission and ends
  const ar::Real3 p0{ar::Real(0.1), ar::Real(-0.05), ar::Real(0)}, n0{0, 0, 1};
  const uint32_t enc = ar::oct_encode(n0);
  const ar::LightSampleContext ctx = ar::light_context_point(p0, ar::oct_decode(enc), ar::oct_decode(enc));
  const auto setup = [&](uint16_t total) {
    for (uint32_t l = 0; l < nl; ++l) {
      const ar::LightRecord& lr = rig.kg.lights[l];
      REQUIRE(lr.type == uint8_t(ar::LightType::TRIANGLE));
      const ar::Real3 c = centroid(lr), d = c - p0;
      rig.next_stage[l] = uint8_t(ar::Stage::SHADE_SURFACE), rig.term[l] = uint8_t(ar::Term::ACTIVE), rig.flags[l] = 0u;
      rig.bounces[l] = ar::U16x6{{total, total, 0, 0, 0, 0}};
      rig.hit_inst[l] = ar::tri_instance(lr), rig.hit_prim[l] = ar::tri_index(lr);
      rig.hit_uv[l] = ar::Real2{ar::Real(1.0 / 3.0), ar::Real(1.0 / 3.0)}, rig.hit_t[l] = ar::length(d);
      rig.ray_o[l] = p0, rig.ray_d[l] = ar::normalize(d), rig.prev_dist[l] = ar::Real(0);
      rig.prev_ng[l] = enc, rig.prev_ns[l] = enc;
      rig.beta[l] = rig.r_u[l] = rig.r_l[l] = ar::spec_fill(ar::Real(1)), rig.eta_scale[l] = ar::Real(1), rig.lambda_t[l] = rig.lambda0;
      rig.sh_next[l] = uint8_t(ar::Stage::SHADOW_TERMINATED);
    }
    rig.clear_film();
  };
  std::vector<double> weighted(nl), first(nl);
  setup(1);
  rig.run();
  for (uint32_t l = 0; l < nl; ++l) weighted[l] = double(rig.film->data().slot_L[4u * l]);
  setup(0);
  rig.run();
  for (uint32_t l = 0; l < nl; ++l) first[l] = double(rig.film->data().slot_L[4u * l]);
  std::vector<double> pmf_hit(nl), observed(nl + 1, 0.0), expected(nl + 1, 0.0);
  double sum = 0.0;
  for (uint32_t l = 0; l < nl; ++l) {
    REQUIRE(first[l] > 0.0);
    REQUIRE(weighted[l] > 0.0);
    const ar::LightRecord& lr = rig.kg.lights[l];
    const ar::Real3 c = centroid(lr);
    const double p = first[l] / weighted[l] - 1.0;
    const double pdf = double(ar::tri_pdf_li(lr, ctx, ar::normalize(c - p0), ar::LightHit{c, ar::tri_normal(lr)}));
    pmf_hit[l] = p / pdf;
    sum += pmf_hit[l];
    std::printf("light_tree_kernel emitter %u: pmf at the hit %.6f, alias %.6f\n", l, pmf_hit[l], double(t.pmf[l]));
    // The same number the sampler's pmf gives at that context.
    CHECK(pmf_hit[l] == doctest::Approx(double(ar::light_sampler_pmf(*rig.kg.light_sampler, ctx, l, false))).epsilon(1e-4));
  }
  const int n = 1 << 20;
  std::mt19937_64 rng(run.seed);
  for (int k = 0; k < n; ++k) {
    const uint64_t b = rng();
    const ar::SampledLight s = ar::light_sampler_sample(*rig.kg.light_sampler, ctx, uint32_t(b), ar::Real(double(b >> 40) * (1.0 / 16777216.0)), false);
    observed[s.light == ar::kNoLight ? nl : s.light] += 1.0;
  }
  for (uint32_t l = 0; l < nl; ++l) expected[l] = pmf_hit[l] * n;
  expected[nl] = std::max(0.0, 1.0 - sum) * n;
  const st::Chi2Result res = st::chi2_test(run, observed, expected);
  std::printf("light_tree_kernel emitter hit chi2 %.1f dof %d p %.4f\n", res.chi2, res.dof, res.p);
  CHECK_MESSAGE(res.pass, res.message);
}

}  // TEST_SUITE("fast")

namespace {

// 64 rectangular lamps facing down over the receiver quad (an 8 x 8 grid over [-4, 4]^2, heights
// 1 to 3, mixed sizes and powers), through the host light table.
std::shared_ptr<const LightTable> lamps64() {
  Rng rng{1758};
  std::vector<LightInput> in;
  for (int i = 0; i < 64; ++i) {
    LightInput li;
    const double x = -3.5 + (i % 8), y = -3.5 + (i / 8), z = 1.0 + 2.0 * rng.next();
    const double a = 0.1 + 0.4 * rng.next(), b = 0.1 + 0.4 * rng.next(), s = 1.0 + 99.0 * rng.next();
    li.record = ar::light_make_area(ar::LightType::AREA_RECT, r3(x, y, z), r3(1, 0, 0), r3(0, 1, 0), Real(a), Real(b), Real(s), white());
    in.push_back(li);
  }
  return build_light_table(in, ar::SceneSphere{r3(0, 0, 1), Real(6)}, observer(), nullptr);
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("light_tree_kernel/tree mean equals alias mean on 64 lamps") {
  // Light-only NEE (TEST_HOOK_NEE_LIGHT_ONLY: weight 1 / (pmf pdf), so the items are the whole
  // direct-light estimate) at random receiver points under 64 lamps, once with the tree and once
  // with the alias-table hook on independent sample streams: the means agree (Welch, the gate
  // family's z); the variance ratio alias / tree is printed.
  const st::Run run = st::begin("light_tree_kernel/tree mean equals alias mean on 64 lamps");
  constexpr uint32_t kPaths = 1u << 14, kPasses = 16;
  Rig rig({}, kPaths);
  const auto table = lamps64();
  const ar::LightSamplerView view = table->view();
  rig.kg.lights = table->lights.data();
  rig.kg.light_sampler = &view;
  double mean[2], var[2];
  for (int mode = 0; mode < 2; ++mode) {
    rig.kg.session.test_hooks = ar::TEST_HOOK_NEE_LIGHT_ONLY | (mode == 1 ? ar::TEST_HOOK_LIGHT_ALIAS : 0u);
    rig.kg.session.seed = uint32_t(run.seed) + uint32_t(mode) * 7919u;
    double s1 = 0.0, s2 = 0.0;
    uint64_t m = 0;
    Rng pos{run.seed};
    for (uint32_t pass = 0; pass < kPasses; ++pass) {
      for (uint32_t s = 0; s < kPaths; ++s) {
        rig.event_index[s] = uint16_t(pass), rig.bounces[s] = ar::U16x6{{0, 0, 0, 0, 0, 0}}, rig.flags[s] = 0u;
        rig.next_stage[s] = uint8_t(ar::Stage::SHADE_SURFACE), rig.term[s] = uint8_t(ar::Term::ACTIVE);
        rig.ray_o[s] = ar::Real3{0, 0, 1}, rig.ray_d[s] = ar::Real3{0, 0, -1};
        rig.hit_inst[s] = 0u, rig.hit_prim[s] = s & 1u;
        double u = pos.next(), v = pos.next();
        if (u + v > 1.0) u = 1.0 - u, v = 1.0 - v;
        rig.hit_uv[s] = ar::Real2{ar::Real(u), ar::Real(v)};
        rig.sh_next[s] = uint8_t(ar::Stage::SHADOW_TERMINATED);
        // The BSDF continuation of the previous pass rewrote the throughput and the MIS ratios.
        rig.beta[s] = rig.r_u[s] = rig.r_l[s] = ar::spec_fill(ar::Real(1)), rig.eta_scale[s] = ar::Real(1);
        rig.lambda_t[s] = rig.lambda0;
      }
      rig.run();
      for (uint32_t s = 0; s < kPaths; ++s) {
        double x = 0.0;
        if (rig.sh_next[s] == uint8_t(ar::Stage::INTERSECT_SHADOW))
          for (int k = 0; k < ar::kLanes; ++k) x += double(rig.sh_contrib[s].v[k]) / ar::kLanes;
        REQUIRE(std::isfinite(x));
        s1 += x, s2 += x * x, ++m;
      }
    }
    mean[mode] = s1 / double(m);
    var[mode] = (s2 / double(m) - mean[mode] * mean[mode]) * double(m) / double(m - 1);
  }
  const double n = double(kPaths) * kPasses, se = std::sqrt(var[0] / n + var[1] / n), z = (mean[0] - mean[1]) / se;
  run.family->note_check();
  std::printf("light_tree_kernel 64 lamps cpu: tree %.6g, alias %.6g, se %.3g, z %+.2f (limit %.2f); variance ratio alias / tree %.3f\n",
              mean[0], mean[1], se, z, run.family->z(), var[1] / var[0]);
  INFO(st::reproduce(run));
  CHECK(std::fabs(z) < run.family->z());
}

}  // TEST_SUITE("gate")

TEST_SUITE("full") {

TEST_CASE("light_tree_kernel/B-23 tree build for 10k lights") {
  // The light table with its tree for 10^4 lights of every finite type, printed against B-23
  // (50 ms per 10k lights); the limit is not gated here.
  Rng rng{1759};
  std::vector<LightInput> in;
  for (int i = 0; i < 10000; ++i) {
    LightInput li;
    const Real3 p = r3(20 * rng.next(), 20 * rng.next(), 1 + 10 * rng.next());
    const double s = 1.0 + 99.0 * rng.next();
    switch (i % 4) {
      case 0: li.record = ar::light_make(ar::LightType::POINT, p, Real(0), Real(s), white()); break;
      case 1: li.record = ar::light_make(ar::LightType::SPHERE, p, Real(0.1), Real(s), white()); break;
      case 2: li.record = ar::light_make_area(ar::LightType::AREA_RECT, p, r3(1, 0, 0), r3(0, 1, 0), Real(0.3), Real(0.4), Real(s), white()); break;
      default: li.record = ar::light_make_triangle(p, p + r3(0.3, 0, 0), p + r3(0, 0.3, 0), 0, uint32_t(i), ar::kLightTwoSided, Real(s), white());
    }
    in.push_back(li);
  }
  // Serially, and with the per-light energies in chunks on every hardware thread as the scene's
  // workers run them (build_light_table's ChunkRunner).
  const uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
  const ChunkRunner threads = [hw](uint32_t count, const std::function<void(uint32_t, uint32_t)>& body) {
    const uint32_t step = (count + hw - 1) / hw;
    std::vector<std::thread> pool;
    for (uint32_t first = 0; first < count; first += step) pool.emplace_back(body, first, std::min(count, first + step));
    for (std::thread& th : pool) th.join();
  };
  for (int parallel = 0; parallel < 2; ++parallel) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto t = build_light_table(in, ar::SceneSphere{r3(10, 10, 6), Real(16)}, observer(), nullptr, parallel ? threads : ChunkRunner{});
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("light_tree_kernel B-23: 10000 lights on %u threads, table %.1f ms of which tree %.1f ms (%zu nodes); limit 50 ms per 10k lights\n",
                parallel ? hw : 1u, ms, t->tree_build_ms, t->tree_nodes.size());
    CHECK(t->tree_nodes.size() == 2 * 10000 - 1);
  }
}

}  // TEST_SUITE("full")
#endif
