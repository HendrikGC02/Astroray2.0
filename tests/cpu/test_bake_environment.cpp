// SPDX-License-Identifier: Apache-2.0
// bake_environment (0004 D11): the cell function against an independent host evaluation, the
// kernel through the CPU device, and the host baker (normalisation, cache, refusals).
// Ref: docs/decisions/0004-integrator.md D10 "Importance weighting", D11; kernel/stages/
// bake_environment.h (the error bound); pbrt-v4 ImageInfiniteLight (the grid per cell).
//
// Tolerance against the host. The kernel sums the world over the 471 observer samples with
// Neumaier's compensated sum in float, then divides by the weight total (bake_environment.h). With
// u = 2^-24 and e_s the callable's relative error, the cell differs from an exact evaluation of
// the same sum by at most (e_s + 8) u, because every term is positive: the product s c costs
// 2 u (c = (x + y) + z in float) + u, the two compensated sums u each, the weight total 3 u and
// the division u; the compensation's own second-order error 2 n u^2 with n = 471 is below 1e-11
// and is not counted. The analytic callable below has e_s = 8 u: 1 + q^2 with q = (lambda - 595) /
// 235 (the difference is exact, the division u, q q 3 u, the sum 2.5 u at most since q^2 <= 1:
// 3 u), (1.5 + z) (1 + 0.5 x x) (u, then u + u/3 + u, product 4 u in all) and the product of the
// two u. The host side is double throughout and takes the direction the kernel computes (the
// mapping's own accuracy is test_env_table's), so no term of the direction enters. A plain float
// sum would add up to (118 + 3) u here; the tolerance does not allow for it, which is the point of
// the compensation. The constant world has e_s = u (the float strength product; the spectrum is
// the same resolve() call on both sides): 9 u = 5.4e-7, inside the 1e-6 the task states.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "host/data_resource.h"
#include "host/light/env_bake.h"
#include "host/light/env_table.h"
#include "host/light/light_builder.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/generation.h"
#include "host/scene/mesh.h"
#include "host/thread_pool.h"
#include "stages/bake_environment.h"
#include "vm/ops.h"

using namespace astroray::host;
using ar::Real;
using ar::Real3;

namespace {

constexpr double kU = 1.0 / 16777216.0;  // float unit roundoff 2^-24
constexpr double kReductionUnits = 8.0;  // bake_environment.h: the sums, the weights and the division
constexpr double kAnalyticUnits = 8.0;   // e_s of analytic_radiance, counted in the header comment
constexpr uint32_t kN = 64;              // tests use 64; production uses kEnvBakeSize

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

// A smooth positive function of direction and wavelength, written once for float and double.
template <class T>
T analytic_radiance(T x, T y, T z, T lambda) {
  (void)y;
  const T q = (lambda - T(595)) / T(235);
  const T spectral = T(1) + q * q;
  const T directional = (T(1.5) + z) * (T(1) + T(0.5) * x * x);
  return spectral * directional;
}

struct AnalyticWorld {
  ar::Spec operator()(const Real3& d, const ar::Lambda& lam) const {
    ar::Spec s;
    for (int k = 0; k < ar::kLanes; ++k) s.v[k] = analytic_radiance<Real>(d.x, d.y, d.z, lam.nm[k]);
    return s;
  }
};

// Exact double evaluation of the same sum at the cell's centre direction.
double host_cell(uint32_t n, uint32_t cell) {
  const uint32_t iv = cell / n, iu = cell % n;
  const Real3 d = ar::equal_area_square_to_sphere(
      ar::Real2{(Real(iu) + Real(0.5)) / Real(n), (Real(iv) + Real(0.5)) / Real(n)});
  const ar::ObserverView& obs = observer();
  double num = 0.0, weights = 0.0;
  for (int j = 0; j < ar::kObsSamples; ++j) {
    const double c = double(obs.cmf[j]) + double(obs.cmf[ar::kObsSamples + j]) + double(obs.cmf[2 * ar::kObsSamples + j]);
    num += analytic_radiance<double>(double(d.x), double(d.y), double(d.z), double(ar::kObsLambdaMin + j)) * c;
    weights += c;
  }
  return num / weights;
}

ar::SpectrumRef world_colour() {
  // A smooth illuminant spectrum centred near 550 nm: sigmoid(1 - ((lambda - 550) / 80)^2).
  ar::SpectrumRef c{};
  c.kind = uint8_t(ar::SpectrumKind::RGB);
  c.semantic = uint8_t(ar::SpectrumSemantic::ILLUMINANT);
  c.p[0] = -1.5625e-4f, c.p[1] = 0.171875f, c.p[2] = -46.265625f, c.p[3] = 1.0f;
  return c;
}

double energy_of(const ar::LightRecord& w) { return display_energy({{w.emission, w.strength}}, observer()); }

// A CPU device whose KernelGlobals hold one world light and the observer.
struct Rig {
  Ledger ledger;
  astroray::ContextState ctx;
  ThreadPool pool{ctx, 2};
  CpuDevice dev{ledger, pool};
  Owner owner{1, 0};
  DeviceBuffer lights, tables, kg;
  Rig() {
    dev.register_production_stages();
    lights = dev.allocate(LedgerCategory::TABLES, sizeof(ar::LightRecord), owner);
    tables = dev.allocate(LedgerCategory::TABLES, sizeof(ar::SceneTables), owner);
    kg = dev.allocate(LedgerCategory::TABLES, sizeof(ar::KernelGlobals), owner);
    ar::SceneTables t{};
    t.observer = observer();
    dev.upload(tables, 0, &t, sizeof t);
    set_world(Real(1), world_colour());
  }
  ~Rig() {
    for (DeviceBuffer* b : {&lights, &tables, &kg}) dev.free(*b);
  }
  void set_world(Real strength, const ar::SpectrumRef& colour) {
    const ar::LightRecord w = ar::light_make_world(strength, colour);
    dev.upload(lights, 0, &w, sizeof w);
    bind(0);
  }
  // `world` is the light index the KernelGlobals names.
  void bind(uint32_t world) {
    ar::KernelGlobals g{};
    g.lights = static_cast<const ar::LightRecord*>(lights.ptr);
    g.tables = static_cast<const ar::SceneTables*>(tables.ptr);
    g.world_light = world;
    dev.upload(kg, 0, &g, sizeof g);
    dev.set_kernel_globals(kg);
  }
  ar::LightRecord world_record() const { return *static_cast<const ar::LightRecord*>(lights.ptr); }
  // The raw n x n grid of the production kernel, as the baker would read it.
  std::vector<float> raw_grid(uint32_t n) {
    DeviceBuffer grid = dev.allocate(LedgerCategory::TABLES, uint64_t(n) * n * sizeof(float), owner);
    const ar::EnvBake b{static_cast<float*>(grid.ptr), n};
    dev.upload(kg, offsetof(ar::KernelGlobals, env_bake), &b, sizeof b);
    dev.launch(KernelId::BAKE_ENVIRONMENT, n * n);
    std::vector<float> out(size_t(n) * n);
    dev.download(out.data(), grid, 0, out.size() * sizeof(float));
    dev.free(grid);
    return out;
  }
};

// ---- a scene, for "which edits touch the world" ----------------------------------------------------

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
  const int color = g.add(ar::Op::CONST_F32X3, {1.0f, 0.5f, 0.25f});
  const int power = g.add(ar::Op::CONST_F32, {strength});
  const int e = g.add(ar::Op::EMIT_LE);
  g.links.push_back({uint32_t(color), 0u, uint32_t(e), 0u});
  g.links.push_back({uint32_t(power), 0u, uint32_t(e), 1u});
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
};

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("bake_environment/analytic against host") {
  const ar::ObserverView& obs = observer();
  const double tol = (kAnalyticUnits + kReductionUnits) * kU;
  double worst = 0.0;
  for (uint32_t cell = 0; cell < kN * kN; ++cell) {
    const double got = double(ar::bake_environment_cell(AnalyticWorld{}, obs, kN, cell));
    const double want = host_cell(kN, cell);
    REQUIRE(want > 0.0);
    worst = std::max(worst, std::fabs(got - want) / want);
  }
  std::printf("bake_environment analytic: worst relative error %.3e, bound %.3e (%g u, u = 2^-24)\n", worst, tol,
              kAnalyticUnits + kReductionUnits);
  CHECK(worst <= tol);
  // The function has a spread the bound does not hide behind: the grid is not constant.
  CHECK(host_cell(kN, 0) != doctest::Approx(host_cell(kN, kN * kN / 2 + 5)).epsilon(0.01));
}

TEST_CASE("bake_environment/constant world uniform") {
  Rig rig;
  const double want = energy_of(rig.world_record());
  REQUIRE(want > 0.0);
  const std::vector<float> grid = rig.raw_grid(kN);
  // Bit-equal: every cell is the same reduction over the same inputs.
  uint32_t different = 0;
  for (float v : grid) different += std::memcmp(&v, &grid[0], sizeof(float)) != 0;
  CHECK(different == 0u);
  CHECK(std::fabs(double(grid[0]) - want) <= 1e-6 * want);

  EnvironmentBaker baker;
  const BakedEnvironment b = baker.get(rig.dev, rig.kg, {}, kN);
  REQUIRE(b.table);
  CHECK(b.max == double(grid[0]));
  CHECK(b.mean == double(grid[0]));  // 4096 equal floats sum exactly in double
  CHECK(std::fabs(b.max - want) <= 1e-6 * want);
  CHECK(std::fabs(b.mean - want) <= 1e-6 * want);
  // The table is a density: uniform, 1 in every cell, the integral 1, for both distributions.
  CHECK(b.table->n == kN);
  for (const EnvDistribution* d : {&b.table->plain, &b.table->compensated}) {
    CHECK(d->func_integral == 1.0f);
    CHECK(std::all_of(d->func.begin(), d->func.end(), [](float f) { return f == 1.0f; }));
  }
  // The kernel left the KernelGlobals as it found them.
  ar::KernelGlobals back{};
  rig.dev.download(&back, rig.kg, 0, sizeof back);
  CHECK(back.env_bake.grid == nullptr);
  CHECK(back.env_bake.n == 0u);
  CHECK(rig.ledger.total() == rig.lights.bytes + rig.tables.bytes + rig.kg.bytes);
}

TEST_CASE("bake_environment/tiny uniform world builds") {
  // BACKLOG 55: build_env_table throws on a uniform grid whose integral is below FLT_MIN. A baked
  // grid holds floats, so for equal cells the double mean is exact and the older ulp-residual
  // mechanism (a mean that rounds below the value) cannot arise; what remains is a world whose
  // radiance is itself below FLT_MIN (strength 1e-39 gives cells near 6e-40). The baker divides
  // by the maximum, so both worlds build, as the density 1.
  Rig rig;
  for (float strength : {1e-30f, 1e-39f}) {
    CAPTURE(strength);
    rig.set_world(Real(strength), world_colour());
    const std::vector<float> grid = rig.raw_grid(kN);
    REQUIRE(grid[0] > 0.0f);
    if (strength < 1e-38f) {
      // The un-normalised grid is refused (the case would prove nothing if it were not).
      CHECK_THROWS_AS(build_env_table(std::vector<double>(grid.begin(), grid.end()), kN), std::invalid_argument);
    }
    EnvironmentBaker baker;
    BakedEnvironment b;
    REQUIRE_NOTHROW(b = baker.get(rig.dev, rig.kg, {}, kN));
    REQUIRE(b.table);
    CHECK(b.max == double(grid[0]));
    CHECK(std::all_of(b.table->plain.func.begin(), b.table->plain.func.end(), [](float f) { return f == 1.0f; }));
    CHECK(std::all_of(b.table->compensated.func.begin(), b.table->compensated.func.end(),
                      [](float f) { return f == 1.0f; }));
  }
}

TEST_CASE("bake_environment/black world gives no table") {
  Rig rig;
  EnvironmentBaker baker;
  rig.set_world(Real(0), world_colour());
  BakedEnvironment b = baker.get(rig.dev, rig.kg, {1}, kN);
  CHECK(b.table == nullptr);
  CHECK(b.max == 0.0);
  CHECK(b.mean == 0.0);
  // A scene without a world is the same black one.
  rig.bind(ar::kNoLight);
  b = baker.get(rig.dev, rig.kg, {2}, kN);
  CHECK(b.table == nullptr);
  CHECK(b.max == 0.0);
  CHECK(b.mean == 0.0);
  CHECK(baker.rebuild_count() == 2u);
}

TEST_CASE("bake_environment/non-finite cell is refused") {
  Rig rig;
  EnvironmentBaker baker;
  for (float bad : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(), -1.0f}) {
    rig.set_world(Real(bad), world_colour());
    std::string what;
    try {
      baker.get(rig.dev, rig.kg, {1}, kN);
    } catch (const std::runtime_error& e) {
      what = e.what();
    }
    CAPTURE(bad);
    CHECK(what.find("bake_environment: cell 0 (iu 0, iv 0) at direction (") != std::string::npos);
    CHECK(what.find("not a finite non-negative radiance") != std::string::npos);
  }
  // The refusals left no cache: a good world under the same key bakes.
  CHECK(baker.rebuild_count() == 0u);
  rig.set_world(Real(2), world_colour());
  CHECK(baker.get(rig.dev, rig.kg, {1}, kN).table != nullptr);
  CHECK(baker.rebuild_count() == 1u);
  CHECK_THROWS_AS(baker.get(rig.dev, rig.kg, {1}, 0), std::invalid_argument);
}

TEST_CASE("bake_environment/rebuild only on world edit") {
  const ar_key W = 40, G = 41, M = 50, MESH = 100;
  Fixture fx;
  Rig rig;
  EnvironmentBaker baker;
  const auto bake = [&] {
    const auto world = fx.scene.materials->world();
    REQUIRE(world);
    // The kernel's world light follows the scene's compiled world, as the light table will.
    rig.set_world(Real(world->strength), world->color);
    return baker.get(rig.dev, rig.kg, EnvironmentBaker::world_key(world.get()), kN);
  };
  const auto set_world_strength = [&](float strength) {
    fx.commit([&](Edit& e) {
      const GB g = constant_emission(strength);
      const ar_graph_desc d = g.desc();
      ar_world_desc w = {};
      w.surface_graph = W;
      REQUIRE(fx.scene.materials->set_graph(e, W, &d) == AR_OK);
      REQUIRE(fx.scene.materials->set_world(e, &w) == AR_OK);
    });
  };

  set_world_strength(3.0f);
  const BakedEnvironment first = bake();
  REQUIRE(first.table);
  CHECK(baker.rebuild_count() == 1u);
  // The key is the world's part of the light table's key.
  {
    const auto world = fx.scene.materials->world();
    const auto mats = fx.scene.materials->arrays();
    const std::vector<uint8_t> whole = gather_scene_lights(nullptr, *fx.scene.meshes, *mats, world.get(), false).key;
    const std::vector<uint8_t> part = EnvironmentBaker::world_key(world.get());
    REQUIRE(!part.empty());
    REQUIRE(whole.size() >= part.size());
    CHECK(std::equal(part.begin(), part.end(), whole.begin()));
  }

  // A camera edit.
  fx.commit([&](Edit& e) {
    ar_camera_desc cam = {};
    cam.type = AR_CAMERA_PERSP;
    REQUIRE(e.set_camera(&cam) == AR_OK);
  });
  CHECK(bake().max == first.max);
  CHECK(baker.rebuild_count() == 1u);
  // A material.
  fx.commit([&](Edit& e) {
    const GB g = constant_emission(7.0f);
    const ar_graph_desc d = g.desc();
    ar_material_desc m = {};
    m.surface_graph = G;
    REQUIRE(fx.scene.materials->set_graph(e, G, &d) == AR_OK);
    REQUIRE(fx.scene.materials->set_material(e, M, &m) == AR_OK);
  });
  CHECK(bake().max == first.max);
  CHECK(baker.rebuild_count() == 1u);
  // A mesh.
  {
    const std::vector<float> pos{0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    const std::vector<int32_t> cv{0, 1, 2, 3}, fs{0};
    fx.commit([&](Edit& e) {
      ar_mesh_desc d = {};
      d.key = MESH, d.vertex_count = 4, d.corner_count = 4, d.face_count = 1, d.motion_steps = 1;
      ar_mesh_buffers b = {};
      REQUIRE(mesh_begin(fx.scene, e, &d, &b) == AR_OK);
      std::memcpy(*b.positions, pos.data(), pos.size() * 4);
      std::memcpy(*b.corner_vert, cv.data(), cv.size() * 4);
      std::memcpy(*b.face_start, fs.data(), fs.size() * 4);
      for (uint64_t i = 0; i < b.corner_normals_count; ++i) (*b.corner_normals)[i] = i % 3 == 2 ? 1.0f : 0.0f;
      REQUIRE(e.item_commit(MESH) == AR_OK);
    });
  }
  CHECK(bake().max == first.max);
  CHECK(baker.rebuild_count() == 1u);
  // Asking again for the same world launches nothing and returns the same table.
  CHECK(bake().table == first.table);
  CHECK(baker.rebuild_count() == 1u);

  // A world edit: one more bake, and the grid follows the strength.
  set_world_strength(6.0f);
  const BakedEnvironment second = bake();
  CHECK(baker.rebuild_count() == 2u);
  CHECK(second.max == doctest::Approx(2.0 * first.max).epsilon(1e-6));
  CHECK(bake().table == second.table);
  CHECK(baker.rebuild_count() == 2u);
  // A different resolution is a different bake.
  CHECK(baker.get(rig.dev, rig.kg, EnvironmentBaker::world_key(fx.scene.materials->world().get()), kN / 2).table->n ==
        kN / 2);
  CHECK(baker.rebuild_count() == 3u);
}

}  // TEST_SUITE("fast")
