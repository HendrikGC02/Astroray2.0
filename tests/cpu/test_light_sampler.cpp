// SPDX-License-Identifier: Apache-2.0
// The alias-table light sampler, emissive-triangle and constant-world lights, the world compile
// and the scene's light table (0004 D9, D10; 0005 items 40, 43).
// Ref: pbrt-v4 AliasTable / PowerLightSampler (sampling and pmf agree), UniformInfiniteLight
// (pdf 1 / (4 pi)); 0004 D9 (estimate 0 gives pmf 0); 0005 item 40 (magenta and one entry).
#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "common/stat.h"
#include "host/data_resource.h"
#include "host/light/light_builder.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/generation.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "light/light_sampler.h"
#include "vm/ops.h"

namespace st = astroray::stat;
using namespace astroray::host;
using ar::Real;
using ar::Real3;

namespace {

Real3 r3(double x, double y, double z) { return Real3{Real(x), Real(y), Real(z)}; }

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

ar::SpectrumRef white() { return ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, Real(1)); }

// 1,000 lights of every type; the first `zeros` are triangles whose estimate is 0.
std::vector<LightInput> mixed(int zeros) {
  std::mt19937 rng(67);
  std::uniform_real_distribution<double> u(0.1, 10.0);
  std::vector<LightInput> v;
  for (int i = 0; i < 1000; ++i) {
    const Real3 p = r3(u(rng), u(rng), u(rng));
    LightInput li;
    if (i < zeros) {
      li.record = ar::light_make_triangle(p, p + r3(1, 0, 0), p + r3(0, 1, 0), 0, uint32_t(i), ar::kLightEmissionProgram, Real(0), white());
      li.terms = {{white(), 0.0f}};
    } else {
      switch (i % 6) {
        case 0: li.record = ar::light_make(ar::LightType::POINT, p, Real(0), Real(u(rng)), white()); break;
        case 1: li.record = ar::light_make(ar::LightType::SPHERE, p, Real(0.2), Real(u(rng)), white()); break;
        case 2: li.record = ar::light_make_area(ar::LightType::AREA_RECT, p, r3(1, 0, 0), r3(0, 1, 0), Real(1), Real(2), Real(u(rng)), white()); break;
        case 3: li.record = ar::light_make_area(ar::LightType::AREA_DISK, p, r3(1, 0, 0), r3(0, 1, 0), Real(0.5), Real(0), Real(u(rng)), white()); break;
        default:
          li.record = ar::light_make_triangle(p, p + r3(u(rng), 0, 0), p + r3(0, u(rng), 0), 1, uint32_t(i), 0, Real(u(rng)), white());
      }
    }
    v.push_back(li);
  }
  LightInput w;
  w.record = ar::light_make_world(Real(0.5), white());
  v.push_back(w);
  return v;
}

std::shared_ptr<const LightTable> table_of(const std::vector<LightInput>& in) {
  return build_light_table(in, ar::SceneSphere{r3(5, 5, 5), Real(10)}, observer(), nullptr);
}

// The smallest 32-bit value whose bin is b: offset = floor(u n / 2^32).
uint32_t bin_u(uint32_t b, uint32_t n) { return uint32_t(((uint64_t(b) << 32) + n - 1) / n); }

ar::LightSampleContext ctx() { return ar::light_context_point(r3(0, 0, 0), r3(0, 0, 1), r3(0, 0, 1)); }

// Ground truth for the triangle pdfs, in double and independent of the kernel: the solid angle
// of a triangle seen from the origin as the integral of |cos| / d^2 over its area (midpoint rule
// on k^2 sub-triangles), and the cell of a point in that subdivision.
struct D3 {
  double x, y, z;
};
D3 sub(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 crs(D3 a, D3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double dt(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 at(D3 v0, D3 e1, D3 e2, double b1, double b2) { return {v0.x + b1 * e1.x + b2 * e2.x, v0.y + b1 * e1.y + b2 * e2.y, v0.z + b1 * e1.z + b2 * e2.z}; }

double omega_quad(D3 v0, D3 v1, D3 v2, int k) {
  const D3 e1 = sub(v1, v0), e2 = sub(v2, v0), n = crs(e1, e2);
  const double nl = std::sqrt(dt(n, n)), a = 0.5 * nl / (double(k) * k);
  double sum = 0.0;
  for (int i = 0; i < k; ++i)
    for (int j = 0; i + j < k; ++j)
      for (int up = 0; up < 2 && (up == 0 || i + j < k - 1); ++up) {
        const double b1 = (i + (up ? 2.0 : 1.0) / 3.0) / k, b2 = (j + (up ? 2.0 : 1.0) / 3.0) / k;
        const D3 q = at(v0, e1, e2, b1, b2);
        const double d2 = dt(q, q);
        sum += std::fabs(dt(n, q)) / nl / (d2 * std::sqrt(d2)) * a;
      }
  return sum;
}

// Cell index of barycentrics (b1, b2) in the k^2 subdivision: 2 (i k + j) + up.
int cell_of(double b1, double b2, int k) {
  const double u = b1 * k, v = b2 * k;
  int i = std::min(int(u), k - 1), j = std::min(int(v), k - 1);
  const int up = (u - i) + (v - j) > 1.0 ? 1 : 0;
  return 2 * (i * k + j) + up;
}

D3 d3(Real3 v) { return {double(v.x), double(v.y), double(v.z)}; }

ar::Lambda lanes() {
  ar::Lambda lam;
  for (int k = 0; k < ar::kLanes; ++k) lam.nm[k] = Real(450 + 50 * k);
  return lam;
}

// Lit from below (back face toward the origin), shifted along x by `x` to tilt it away.
ar::LightRecord back_tri(double d, uint16_t flags = ar::kLightBackFace, double x = 0.0) {
  return ar::light_make_triangle(r3(x - 1, -1, d), r3(x + 1, -1, d), r3(x, 1, d), 0, 0, flags, Real(2), white());
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("light_sampler/pmf matches sample") {
  const auto t = table_of(mixed(10));
  const ar::LightSamplerView v = t->view();
  double sum = 0.0;
  for (uint32_t i = 0; i < v.light_count; ++i) sum += double(ar::light_sampler_pmf(v, ctx(), i, false));
  CHECK(std::fabs(sum - 1.0) < 1e-5);
  // Importance = weighted energy e times strength times the geometric factor of 0004 D9 / pbrt
  // Phi, written out here: 1 for lamps, pi A for a triangle, 4 pi^2 R^2 for the world.
  const double e = display_energy({{white(), 1.0f}}, observer()), pi = 3.14159265358979323846;
  for (uint32_t i = 10; i < v.light_count; ++i) {
    const ar::LightRecord& r = t->lights[i];
    double geo = 1.0;
    if (r.type == uint8_t(ar::LightType::TRIANGLE)) {
      const double ax = double(r.xf[3]) - r.xf[0], ay = double(r.xf[4]) - r.xf[1], bx = double(r.xf[6]) - r.xf[0], by = double(r.xf[7]) - r.xf[1];
      geo = pi * 0.5 * std::fabs(ax * by - ay * bx);  // the test triangles lie in z = const
    } else if (r.type == uint8_t(ar::LightType::WORLD_CONST)) {
      geo = 4.0 * pi * pi * 10.0 * 10.0;
    }
    CHECK(t->importance[i] == doctest::Approx(e * double(r.strength) * geo).epsilon(1e-5));
    CHECK(double(t->pmf[i]) == doctest::Approx(t->importance[i] / std::accumulate(t->importance.begin(), t->importance.end(), 0.0)).epsilon(1e-6));
  }
  for (int k = 0; k < 100000; ++k) {
    const uint32_t ub = uint32_t((uint64_t(k) << 32) / 100000u);
    const ar::SampledLight s = ar::light_sampler_sample(v, ctx(), ub, Real(((k * 7919) % 1000 + 0.5) / 1000.0), false);
    REQUIRE(s.light < v.light_count);
    REQUIRE(std::fabs(double(ar::light_sampler_pmf(v, ctx(), s.light, false)) - double(s.pmf)) <= 1e-6);
  }
  CHECK(ar::light_sampler_pmf(v, ctx(), v.light_count, false) == Real(0));
}

TEST_CASE("light_sampler/zero estimate never chosen") {
  const auto t = table_of(mixed(10));
  const ar::LightSamplerView v = t->view();
  for (uint32_t i = 0; i < 10; ++i) CHECK(ar::light_sampler_pmf(v, ctx(), i, false) == Real(0));
  CHECK(v.bin_count == v.light_count - 10);
  // Every bin at both ends and its middle.
  for (uint32_t b = 0; b < v.bin_count; ++b)
    for (double f : {0.0, 0.5, 0.999999}) {
      const ar::SampledLight s = ar::light_sampler_sample(v, ctx(), bin_u(b, v.bin_count), Real(f), true);
      REQUIRE(s.light >= 10u);
      REQUIRE(s.pmf > Real(0));
    }
  // A scene of zero-estimate lights only has no light to give.
  const std::vector<LightInput> all = mixed(1000);
  const std::vector<LightInput> dark(all.begin(), all.begin() + 1000);
  const auto d = table_of(dark);
  CHECK(ar::light_sampler_sample(d->view(), ctx(), 0x40000000u, Real(0.3), false).pmf == Real(0));
}

TEST_CASE("light_sampler/world pdf integrates to 1") {
  const ar::LightRecord w = ar::light_make_world(Real(0.5), white());
  // Midpoint rule in (cos theta, phi): the measure is exact for a constant pdf.
  double integral = 0.0;
  const int n = 200;
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < n; ++j) {
      const double z = -1.0 + 2.0 * (i + 0.5) / n, phi = 2.0 * 3.14159265358979323846 * (j + 0.5) / n;
      const double s = std::sqrt(1.0 - z * z);
      integral += double(ar::light_pdf_li(w, ctx(), r3(s * std::cos(phi), s * std::sin(phi), z), ar::LightHit{})) * (2.0 / n) *
                  (2.0 * 3.14159265358979323846 / n);
    }
  CHECK(std::fabs(integral - 1.0) < 1e-5);
  ar::Lambda lam;
  for (int k = 0; k < ar::kLanes; ++k) lam.nm[k] = Real(450 + 50 * k);
  const ar::LightLiSample s = ar::light_sample_li(w, ctx(), ar::Real2{Real(0.3), Real(0.7)}, lam, observer());
  CHECK(s.infinite);
  CHECK(!s.delta);
  CHECK(s.pdf == ar::light_pdf_li(w, ctx(), s.wi, ar::LightHit{}));
  CHECK(std::fabs(double(length(s.wi)) - 1.0) < 1e-6);
}

TEST_CASE("light_sampler/triangle sample and pdf agree") {
  ar::Lambda lam;
  for (int k = 0; k < ar::kLanes; ++k) lam.nm[k] = Real(450 + 50 * k);
  // Near (spherical branch) and far (area branch, solid angle below 3e-4 sr).
  for (double d : {1.0, 200.0}) {
    const ar::LightRecord t = ar::light_make_triangle(r3(-1, -1, d), r3(1, -1, d), r3(0, 1, d), 0, 0, 0, Real(2), white());
    const ar::LightSampleContext c = ctx();
    CHECK(ar::tri_spherical(ar::tri_solid_angle(t, c.p)) == (d < 10.0));
    // The winding normal is +z, so a one-sided triangle above the context does not light it.
    CHECK(ar::light_sample_li(t, c, ar::Real2{Real(0.4), Real(0.4)}, lam, observer()).pdf == Real(0));
    const ar::LightRecord back = ar::light_make_triangle(r3(-1, -1, d), r3(1, -1, d), r3(0, 1, d), 0, 0, ar::kLightBackFace, Real(2), white());
    for (int k = 0; k < 64; ++k) {
      const ar::Real2 u{Real((k % 8 + 0.5) / 8), Real((k / 8 + 0.5) / 8)};
      const ar::LightLiSample s = ar::light_sample_li(back, c, u, lam, observer());
      REQUIRE(s.pdf > Real(0));
      CHECK(std::fabs(double(s.p_light.z) - d) < 1e-4 * d);
      const ar::LightHit hit{s.p_light, r3(0, 0, -1)};
      CHECK(double(ar::light_pdf_li(back, c, s.wi, hit)) == doctest::Approx(double(s.pdf)).epsilon(1e-4));
    }
  }
}

TEST_CASE("light_sampler/triangle pdf matches the solid angle") {
  // E[1 / pdf] over the sampling is the solid angle for both branches (area: (1/A) int cos/d^2 dA).
  // The far triangle is seen at 45 degrees, so the area pdf's cosine matters.
  for (double d : {1.0, 200.0}) {
    const ar::LightRecord t = back_tri(d, ar::kLightBackFace, d < 10.0 ? 0.0 : d);
    const double truth = omega_quad(d3(ar::tri_vertex(t, 0)), d3(ar::tri_vertex(t, 1)), d3(ar::tri_vertex(t, 2)), 600);
    REQUIRE(ar::tri_spherical(ar::tri_solid_angle(t, ctx().p)) == (d < 10.0));
    const int m = 128;
    double s1 = 0.0, s2 = 0.0;
    for (int a = 0; a < m; ++a)
      for (int b = 0; b < m; ++b) {
        const ar::LightLiSample s = ar::light_sample_li(t, ctx(), ar::Real2{Real((a + 0.5) / m), Real((b + 0.5) / m)}, lanes(), observer());
        REQUIRE(s.pdf > Real(0));
        const double x = 1.0 / double(s.pdf);
        s1 += x, s2 += x * x;
      }
    const double n = double(m) * m, mean = s1 / n, se = std::sqrt(std::max(0.0, s2 / n - mean * mean) / n);
    CHECK_MESSAGE(std::fabs(mean - truth) <= 4.0 * se + 2e-4 * truth, "d " << d << " mean " << mean << " truth " << truth);
  }
}

TEST_CASE("light_sampler/spherical triangle directions are uniform") {
  // Directions of the spherical branch are uniform in solid angle: counts per sub-triangle follow
  // its own solid angle.
  const st::Run run = st::begin("light_sampler/spherical triangle directions are uniform");
  const ar::LightRecord t = back_tri(1.0);
  const D3 v0 = d3(ar::tri_vertex(t, 0)), v1 = d3(ar::tri_vertex(t, 1)), v2 = d3(ar::tri_vertex(t, 2));
  const D3 e1 = sub(v1, v0), e2 = sub(v2, v0), n = crs(e1, e2);
  const int k = 6, cells = 2 * k * k, samples = 100000;
  std::vector<double> observed(size_t(cells), 0.0), expected(size_t(cells), 0.0);
  const double total = omega_quad(v0, v1, v2, 600);
  for (int i = 0; i < k; ++i)
    for (int j = 0; i + j < k; ++j)
      for (int up = 0; up < 2 && (up == 0 || i + j < k - 1); ++up) {
        const auto c = [&](double a, double b) { return at(v0, e1, e2, (i + a) / k, (j + b) / k); };
        const double o = up ? omega_quad(c(1, 0), c(1, 1), c(0, 1), 100) : omega_quad(c(0, 0), c(1, 0), c(0, 1), 100);
        expected[size_t(2 * (i * k + j) + up)] = samples * o / total;
      }
  std::mt19937_64 rng(run.seed);
  std::uniform_real_distribution<double> uni(0.0, 1.0);
  for (int q = 0; q < samples; ++q) {
    const ar::LightLiSample s = ar::light_sample_li(t, ctx(), ar::Real2{Real(uni(rng)), Real(uni(rng))}, lanes(), observer());
    REQUIRE(s.pdf > Real(0));
    const D3 r = sub(d3(s.p_light), v0);
    const double b1 = dt(crs(r, e2), n) / dt(n, n), b2 = dt(crs(e1, r), n) / dt(n, n);
    observed[size_t(cell_of(b1, b2, k))] += 1.0;
  }
  const st::Chi2Result res = st::chi2_test(run, observed, expected);
  CHECK_MESSAGE(res.pass, res.message);
}

TEST_CASE("light_sampler/program emission defers Le") {
  const ar::LightRecord c = back_tri(1.0), p = back_tri(1.0, ar::kLightBackFace | ar::kLightEmissionProgram);
  const ar::Real2 u{Real(0.3), Real(0.6)};
  const ar::LightLiSample sc = ar::light_sample_li(c, ctx(), u, lanes(), observer());
  const ar::LightLiSample sp = ar::light_sample_li(p, ctx(), u, lanes(), observer());
  CHECK(!sc.deferred_le);
  CHECK(sc.Le.v[0] > Real(0));
  REQUIRE(sp.pdf > Real(0));
  CHECK(sp.deferred_le);
  CHECK(sp.Le.v[0] == Real(0));
  const ar::SceneSphere world{r3(0, 0, 0), Real(10)};
  CHECK(ar::light_sample_le(p, u, u, lanes(), observer(), world).deferred_le);
  CHECK(!ar::light_sample_le(c, u, u, lanes(), observer(), world).deferred_le);
}

TEST_CASE("light_sampler/emission sampling resolves sidedness") {
  const std::vector<EmissionTerm> bright = {{white(), 1.0f}}, dim = {{white(), 0.4f}};
  CHECK(emission_sidedness(AR_EMISSION_SAMPLING_NONE, bright) == 0u);
  CHECK(emission_sidedness(AR_EMISSION_SAMPLING_FRONT, bright) == kLightLit);
  CHECK(emission_sidedness(AR_EMISSION_SAMPLING_BACK, bright) == (kLightLit | ar::kLightBackFace));
  CHECK(emission_sidedness(AR_EMISSION_SAMPLING_FRONT_BACK, dim) == (kLightLit | ar::kLightTwoSided));
  CHECK(emission_sidedness(AR_EMISSION_SAMPLING_AUTO, bright) == (kLightLit | ar::kLightTwoSided));  // max 1 > 0.5
  CHECK(emission_sidedness(AR_EMISSION_SAMPLING_AUTO, dim) == 0u);                                   // max 0.4
  CHECK_THROWS_AS(emission_sidedness(5, bright), std::invalid_argument);
  // AUTO takes the maximum of the summed estimate: two terms of 0.3 make 0.6.
  CHECK(emission_sidedness(AR_EMISSION_SAMPLING_AUTO, {{white(), 0.3f}, {white(), 0.3f}}) == (kLightLit | ar::kLightTwoSided));
  ar::SpectrumRef table = white();
  table.kind = uint8_t(ar::SpectrumKind::TABLE);
  CHECK_THROWS_AS(emission_sidedness(AR_EMISSION_SAMPLING_AUTO, {{table, 1.0f}}), std::invalid_argument);
}

}  // TEST_SUITE("fast")

namespace {

struct GB {
  std::vector<ar_graph_node> nodes;
  std::vector<ar_graph_link> links;
  std::vector<float> f;
  std::vector<int32_t> i;
  int add(ar::Op op, std::vector<float> fp = {}, std::vector<int32_t> ip = {}) {
    ar_graph_node n = {};
    n.op = uint32_t(op);
    n.f32_first = uint32_t(f.size()), n.f32_count = uint32_t(fp.size());
    n.i32_first = uint32_t(i.size()), n.i32_count = uint32_t(ip.size());
    f.insert(f.end(), fp.begin(), fp.end());
    i.insert(i.end(), ip.begin(), ip.end());
    nodes.push_back(n);
    return int(nodes.size()) - 1;
  }
  void emit(int color, int strength) {
    const int e = add(ar::Op::EMIT_LE);
    links.push_back({uint32_t(color), 0u, uint32_t(e), 0u});
    links.push_back({uint32_t(strength), 0u, uint32_t(e), 1u});
  }
  ar_graph_desc desc() const {
    ar_graph_desc d = {};
    d.node_count = uint32_t(nodes.size()), d.link_count = uint32_t(links.size());
    d.f32_count = uint32_t(f.size()), d.i32_count = uint32_t(i.size());
    d.nodes = nodes.data(), d.links = links.data(), d.params_f32 = f.data(), d.params_i32 = i.data();
    return d;
  }
};

GB constant_emission(float strength) {
  GB g;
  g.emit(g.add(ar::Op::CONST_F32X3, {1.0f, 0.5f, 0.25f}), g.add(ar::Op::CONST_F32, {strength}));
  return g;
}

struct Fixture {
  astroray::ContextState ctx;
  Scene scene{ctx, SceneConfig{2, {}}};
  Fixture() {
    scene.meshes = std::make_shared<MeshStore>();
    scene.materials = std::make_shared<MaterialStore>(scene.meshes->shared_ledger());
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
  std::vector<std::string> unsupported(ar_key subject) {
    scene.report().snapshot();
    ar_report_info info = {};
    REQUIRE(scene.report().info(&info) == AR_OK);
    std::vector<std::string> out;
    for (uint32_t k = 0; k < info.entry_count; ++k) {
      ar_report_entry e = {};
      REQUIRE(scene.report().entry(k, &e) == AR_OK);
      if (e.subject == subject && e.code == AR_REPORT_NODE_UNSUPPORTED) out.emplace_back(reinterpret_cast<const char*>(e.detail));
    }
    return out;
  }
};

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("light_sampler/world with a non-constant node raises one entry") {
  const ar_key W = 40, G = 41, M = 50;
  Fixture fx;
  GB varying;
  const int uv = varying.add(ar::Op::LOAD_CTX_UV, {}, {0});
  const int tex = varying.add(ar::Op::TEX_IMAGE, {}, {7, 0});
  varying.links.push_back({uint32_t(uv), 0u, uint32_t(tex), 0u});
  varying.emit(tex, varying.add(ar::Op::CONST_F32, {2.0f}));
  fx.commit([&](Edit& e) {
    const ar_graph_desc d = varying.desc();
    ar_world_desc w = {};
    w.surface_graph = W;
    REQUIRE(fx.scene.materials->set_graph(e, W, &d) == AR_OK);
    REQUIRE(fx.scene.materials->set_world(e, &w) == AR_OK);
  });
  CHECK(fx.unsupported(W) == std::vector<std::string>{"node 0 (LOAD_CTX_UV)"});
  auto world = fx.scene.materials->world();
  REQUIRE(world);
  CHECK(world->magenta);
  CHECK(world->strength == 1.0f);
  // An unrelated edit keeps the entry, once.
  fx.commit([&](Edit& e) {
    const GB g = constant_emission(1.0f);
    const ar_graph_desc d = g.desc();
    ar_material_desc m = {};
    m.surface_graph = G;
    REQUIRE(fx.scene.materials->set_graph(e, G, &d) == AR_OK);
    REQUIRE(fx.scene.materials->set_material(e, M, &m) == AR_OK);
  });
  CHECK(fx.unsupported(W) == std::vector<std::string>{"node 0 (LOAD_CTX_UV)"});
  // A constant Background replaces it: the entry goes and the world carries its radiance.
  fx.commit([&](Edit& e) {
    const GB g = constant_emission(3.0f);
    const ar_graph_desc d = g.desc();
    REQUIRE(fx.scene.materials->set_graph(e, W, &d) == AR_OK);
  });
  CHECK(fx.unsupported(W).empty());
  world = fx.scene.materials->world();
  REQUIRE(world);
  CHECK(!world->magenta);
  CHECK(world->strength == 3.0f);
}

TEST_CASE("light_sampler/scene table rebuilds only for emissive changes") {
  const ar_key E = 60, N = 61, GE = 62, GN = 63, MESH_E = 100, MESH_N = 101;
  Fixture fx;
  fx.commit([&](Edit& e) {
    const GB ge = constant_emission(2.0f);
    GB gn;
    gn.add(ar::Op::CONST_F32, {0.5f});
    const ar_graph_desc de = ge.desc(), dn = gn.desc();
    ar_material_desc me = {}, mn = {};
    me.surface_graph = GE, me.emission_sampling = AR_EMISSION_SAMPLING_FRONT;
    mn.surface_graph = GN;
    REQUIRE(fx.scene.materials->set_graph(e, GE, &de) == AR_OK);
    REQUIRE(fx.scene.materials->set_graph(e, GN, &dn) == AR_OK);
    REQUIRE(fx.scene.materials->set_material(e, E, &me) == AR_OK);
    REQUIRE(fx.scene.materials->set_material(e, N, &mn) == AR_OK);
  });
  const std::vector<float> pos{0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
  const std::vector<int32_t> cv{0, 1, 2, 3}, fs{0};
  for (ar_key k : {MESH_E, MESH_N})
    fx.commit([&](Edit& e) {
      ar_mesh_desc d = {};
      d.key = k, d.vertex_count = 4, d.corner_count = 4, d.face_count = 1, d.motion_steps = 1;
      ar_mesh_buffers b = {};
      REQUIRE(mesh_begin(fx.scene, e, &d, &b) == AR_OK);
      std::memcpy(*b.positions, pos.data(), pos.size() * 4);
      std::memcpy(*b.corner_vert, cv.data(), cv.size() * 4);
      std::memcpy(*b.face_start, fs.data(), fs.size() * 4);
      for (uint64_t i = 0; i < b.corner_normals_count; ++i) (*b.corner_normals)[i] = i % 3 == 2 ? 1.0f : 0.0f;
      REQUIRE(e.item_commit(k) == AR_OK);
    });
  std::vector<ar_key> geom{MESH_E, MESH_N}, object{1, 2}, pool{E, N};
  std::vector<ar_xform> xf(2);
  for (ar_xform& x : xf) x.m[0] = x.m[5] = x.m[10] = 1.0f;
  std::vector<uint32_t> zeros(2, 0), vis(2, AR_VISIBILITY_CAMERA), moff{0, 1}, mcount{1, 1};
  std::vector<ar_rgba> colors(2, ar_rgba{{1, 1, 1, 1}});
  std::vector<uint8_t> lg(2, 0);
  ar_instances_desc d = {};
  d.instance_count = 2, d.motion_steps = 1, d.transform_count = 2, d.material_key_count = 2;
  d.geom_keys = geom.data(), d.xf = xf.data(), d.object_keys = object.data();
  d.random_ids = d.pass_indices = d.crypto_objects = d.crypto_assets = zeros.data();
  d.colors = colors.data(), d.visibility = vis.data(), d.flags = zeros.data(), d.light_groups = lg.data();
  d.material_offsets = moff.data(), d.material_counts = mcount.data(), d.material_keys = pool.data();
  fx.commit([&](Edit& e) { REQUIRE(set_instances(e, &d) == AR_OK); });

  auto t = fx.scene.lights();
  CHECK(fx.scene.light_rebuilds() == 1);
  REQUIRE(t->lights.size() == 2);  // the emissive quad's two triangles; no world
  CHECK(t->lights[0].type == uint8_t(ar::LightType::TRIANGLE));
  CHECK(fx.scene.lights() == t);
  // Moving the non-emissive instance leaves the table alone.
  ar_xform moved = xf[1];
  moved.m[3] = 5.0f;
  uint32_t idx = 1;
  fx.commit([&](Edit& e) { REQUIRE(update_transforms(e, 1, &idx, &moved) == AR_OK); });
  CHECK(fx.scene.lights() == t);
  CHECK(fx.scene.light_rebuilds() == 1);
  // Moving the emissive one rebuilds it, with the triangles where the instance now is.
  idx = 0;
  fx.commit([&](Edit& e) { REQUIRE(update_transforms(e, 1, &idx, &moved) == AR_OK); });
  t = fx.scene.lights();
  CHECK(fx.scene.light_rebuilds() == 2);
  REQUIRE(t->lights.size() == 2);
  CHECK(ar::tri_vertex(t->lights[0], 0).x >= Real(5));
  // A new emission strength and a new emission sampling each rebuild it.
  fx.commit([&](Edit& e) {
    const GB g = constant_emission(4.0f);
    const ar_graph_desc dg = g.desc();
    REQUIRE(fx.scene.materials->set_graph(e, GE, &dg) == AR_OK);
  });
  t = fx.scene.lights();
  CHECK(fx.scene.light_rebuilds() == 3);
  REQUIRE(t->lights.size() == 2);
  CHECK(t->lights[0].strength == 4.0f * float(t->radiance_scale));
  fx.commit([&](Edit& e) {
    ar_material_desc me = {};
    me.surface_graph = GE, me.emission_sampling = AR_EMISSION_SAMPLING_BACK;
    REQUIRE(fx.scene.materials->set_material(e, E, &me) == AR_OK);
  });
  t = fx.scene.lights();
  CHECK(fx.scene.light_rebuilds() == 4);
  REQUIRE(t->lights.size() == 2);
  CHECK((t->lights[0].flags & ar::kLightBackFace) != 0);
}

TEST_CASE("light_sampler/parallel build is bit-identical to the serial build") {
  // Lamps, the world and 12,000 triangles in runs of 1-300 sharing one of three term sets (the
  // shape of a multi-material mesh), so chunk edges of 97 fall inside and between runs.
  std::vector<LightInput> in = mixed(10);
  const std::vector<std::vector<EmissionTerm>> sets = {
      {{white(), 2.0f}},
      {{ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, Real(0.5)), 3.0f}},
      {{white(), 1.0f}, {ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, Real(0.25)), 5.0f}}};
  std::mt19937 rng(125);
  std::uniform_int_distribution<int> run(1, 300);
  std::uniform_real_distribution<double> u(0.1, 2.0);
  for (uint32_t t = 0, s = 0; t < 12000; ++s) {
    for (int k = run(rng); k > 0 && t < 12000; --k, ++t) {
      const Real3 p = r3(u(rng), u(rng), u(rng));
      LightInput li;
      li.record = ar::light_make_triangle(p, p + r3(u(rng), 0, 0), p + r3(0, u(rng), 0), 2, t, ar::kLightEmissionProgram, Real(0), white());
      li.terms = sets[s % sets.size()];
      in.push_back(li);
    }
  }
  const ar::SceneSphere sphere{r3(5, 5, 5), Real(10)};
  const auto serial = build_light_table(in, sphere, observer(), nullptr);
  astroray::ContextState ctx;
  WorkerPool pool(ctx, 4);
  uint32_t chunks = 0;
  const ChunkRunner par = [&](uint32_t count, const std::function<void(uint32_t, uint32_t)>& body) {
    chunks = (count + 96) / 97;
    pool.parallel_for(count, 97, body);
  };
  const auto parallel = build_light_table(in, sphere, observer(), nullptr, par);
  REQUIRE(chunks > 100);
  const auto same = [](const auto& a, const auto& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])) == 0);
  };
  CHECK(same(serial->lights, parallel->lights));
  CHECK(same(serial->importance, parallel->importance));
  CHECK(same(serial->bins, parallel->bins));
  CHECK(same(serial->bin_light, parallel->bin_light));
  CHECK(same(serial->pmf, parallel->pmf));
  CHECK(same(serial->light_index, parallel->light_index));
  CHECK(serial->world_light == parallel->world_light);
  // Both equal the per-light formula with no reuse across lights: energy times light_power.
  int differ = 0;
  for (size_t i = 0; i < in.size(); ++i) {
    const LightInput& li = in[i];
    const double e = li.terms.empty() ? display_energy({{li.record.emission, li.record.strength}}, observer())
                                      : display_energy(li.terms, observer());
    const double imp = e * double(ar::light_power(li.record, sphere, Real(1)));
    differ += std::memcmp(&imp, &serial->importance[i], sizeof imp) != 0;
  }
  CHECK(differ == 0);
}

TEST_CASE("light_sampler/worker pool parallel_for covers every index once and rethrows") {
  astroray::ContextState ctx;
  WorkerPool pool(ctx, 3);
  std::vector<int> hits(10007, 0);
  pool.parallel_for(uint32_t(hits.size()), 64, [&](uint32_t first, uint32_t last) {
    for (uint32_t i = first; i < last; ++i) ++hits[i];
  });
  CHECK(std::count(hits.begin(), hits.end(), 1) == int(hits.size()));
  CHECK_THROWS_AS(pool.parallel_for(1000, 10, [](uint32_t first, uint32_t) {
                    if (first == 500) throw std::runtime_error("chunk 50");
                  }),
                  std::runtime_error);
}

TEST_CASE("light_sampler/worker pool parallel_for runs while every worker is blocked") {
  // Scene::lights calls parallel_for under locks that queued jobs may wait for: with every worker
  // stuck behind such a lock, the caller alone must finish the loop. Bounded: the loop runs on its
  // own thread and the lock is released after the check either way.
  astroray::ContextState ctx;
  WorkerPool pool(ctx, 3);
  std::mutex held;
  std::unique_lock<std::mutex> hold(held);
  for (unsigned i = 0; i < pool.workers(); ++i) pool.submit([&] { std::lock_guard<std::mutex> l(held); });
  std::vector<int> hits(5000, 0);
  const WorkerPool::Body body = [&](uint32_t first, uint32_t last) {
    for (uint32_t i = first; i < last; ++i) ++hits[i];
  };
  auto done = std::async(std::launch::async, [&] { pool.parallel_for(uint32_t(hits.size()), 50, body); });
  const bool finished = done.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
  hold.unlock();
  done.get();
  CHECK(finished);
  CHECK(std::count(hits.begin(), hits.end(), 1) == int(hits.size()));
  pool.shutdown();  // the blocked jobs use locals of this scope
}

TEST_CASE("light_sampler/worker pool parallel_for throws on a poisoned context") {
  // A worker's chunk never finishes (as after a fault): on a poisoned context the caller throws
  // instead of waiting for it. Bounded as above; the gate opens after the check either way.
  astroray::ContextState ctx;
  WorkerPool pool(ctx, 2);
  ctx.poison_message = "test poison";
  ctx.poisoned.store(true, std::memory_order_release);
  std::mutex m;
  std::condition_variable cv;
  bool worker_in = false, open = false;
  std::thread::id loop_id;
  const WorkerPool::Body body = [&](uint32_t, uint32_t) {
    std::unique_lock<std::mutex> l(m);
    if (std::this_thread::get_id() == loop_id) {
      cv.wait_for(l, std::chrono::seconds(10), [&] { return worker_in; });  // let a worker claim one
    } else {
      worker_in = true;
      cv.notify_all();
      cv.wait(l, [&] { return open; });
    }
  };
  auto done = std::async(std::launch::async, [&] {
    {
      std::lock_guard<std::mutex> l(m);
      loop_id = std::this_thread::get_id();
    }
    pool.parallel_for(64, 1, body);
  });
  const bool finished = done.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
  {
    std::lock_guard<std::mutex> l(m);
    open = true;
  }
  cv.notify_all();
  CHECK(finished);
  CHECK_THROWS_AS(done.get(), std::runtime_error);
  CHECK(worker_in);
  pool.shutdown();  // a worker may still be inside body, which uses locals of this scope
}

}  // TEST_SUITE("fast")

TEST_SUITE("gate") {

TEST_CASE("light_sampler/selection frequencies match pmf") {
  const st::Run run = st::begin("light_sampler/selection frequencies match pmf");
  const auto t = table_of(mixed(10));
  const ar::LightSamplerView v = t->view();
  std::mt19937_64 rng(run.seed);
  const int n = 1 << 20;
  std::vector<double> observed(v.light_count, 0.0), expected(v.light_count, 0.0);
  for (int k = 0; k < n; ++k) {
    const uint64_t bits = rng();
    observed[ar::light_sampler_sample(v, ctx(), uint32_t(bits), Real(double(bits >> 40) * (1.0 / 16777216.0)), true).light] += 1.0;
  }
  for (uint32_t i = 0; i < v.light_count; ++i) expected[i] = double(t->pmf[i]) * n;
  for (uint32_t i = 0; i < 10; ++i) CHECK(observed[i] == 0.0);
  const st::Chi2Result res = st::chi2_test(run, observed, expected);
  std::printf("light_sampler chi2 %.1f dof %d p %.4f\n", res.chi2, res.dof, res.p);
  CHECK_MESSAGE(res.pass, res.message);
}

TEST_CASE("light_sampler/small q is exact at 2^20 bins") {
  // 2^20 lights of power 1 and one of 0.01, whose bin keeps it with q = n p ~ 0.01. Its selection
  // probability is P(bin) * P(q test) = q / n; sweep the q test of that bin and check nothing else
  // aliases to it, so its frequency is the returned pmf whatever the bin count.
  const uint32_t n = 1u << 20, small = 12345;
  std::vector<LightInput> in(n);
  for (uint32_t i = 0; i < n; ++i)
    in[i].record = ar::light_make(ar::LightType::POINT, r3(0, 0, 1), Real(0), Real(i == small ? 0.01 : 1.0), white());
  const auto t = table_of(in);
  const ar::LightSamplerView v = t->view();
  REQUIRE(v.bin_count == n);
  REQUIRE(t->bin_light[small] == small);
  for (uint32_t b = 0; b < n; ++b) REQUIRE((b == small || t->bins[b].alias != small || t->bins[b].q == 1.0f));
  const int m = 100000;
  int hits = 0;
  for (int k = 0; k < m; ++k) hits += ar::light_sampler_sample(v, ctx(), bin_u(small, n), Real((k + 0.5) / m), true).light == small;
  const double freq = double(hits) / m / n;
  std::printf("light_sampler small q: frequency %.6e pmf %.6e\n", freq, double(t->pmf[small]));
  CHECK(freq == doctest::Approx(double(t->pmf[small])).epsilon(2e-3));
}

}  // TEST_SUITE("gate")
