// SPDX-License-Identifier: Apache-2.0
// The production k_bake_environment on the CPU device and on CUDA, constant world (0009 d17 step
// iii, the per-kernel fixture of 0002 D4). The raw grids agree to 1e-6 relative to each other and
// to the host's display_energy of the world; through EnvironmentBaker on each device the table is
// the uniform density 1, max and mean are the world's display energy, and an unchanged key bakes
// nothing again. The cell function on a world that varies with direction is agree_pure's case.
// Ref: docs/decisions/0004-integrator.md D11; 0002 D12; kernel/stages/bake_environment.h (the
// error bound: a constant world has e_s = u, so 9 u = 5.4e-7 per device, under the 1e-6 of this
// test for each device against the host and 2 x 5.4e-7 for the pair).
#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "host/data_resource.h"
#include "host/device.h"
#include "host/light/env_bake.h"
#include "host/light/light_builder.h"
#include "host/thread_pool.h"
#include "vm/context.h"

using namespace astroray;
using namespace astroray::host;

namespace {

constexpr uint32_t kN = 64;
const Owner kOwner{1, 0};

const ObserverTables& observer_tables() {
  static ObserverTables t;
  static const bool loaded = [] {
    DataBlob b;
    REQUIRE(find_data("spectral/observer.bin", &b));
    const std::string err = parse_observer(b.data, b.size, &t);
    REQUIRE_MESSAGE(err.empty(), err);
    return true;
  }();
  (void)loaded;
  return t;
}

// A smooth illuminant spectrum centred near 550 nm: sigmoid(1 - ((lambda - 550) / 80)^2).
ar::SpectrumRef world_colour() {
  ar::SpectrumRef c{};
  c.kind = uint8_t(ar::SpectrumKind::RGB);
  c.semantic = uint8_t(ar::SpectrumSemantic::ILLUMINANT);
  c.p[0] = -1.5625e-4f, c.p[1] = 0.171875f, c.p[2] = -46.265625f, c.p[3] = 1.0f;
  return c;
}

struct Result {
  std::vector<float> grid;      // the kernel's raw cells
  BakedEnvironment baked;       // through the baker
  uint32_t rebuilds_after_two = 0;
  double launch_ms = 0.0;       // the raw launch, synchronised
};

Result run(Device& dev, const ar::LightRecord& world) {
  const auto put = [&](const void* p, uint64_t bytes) {
    DeviceBuffer b = dev.allocate(LedgerCategory::TABLES, bytes, kOwner);
    dev.upload(b, 0, p, bytes);
    return b;
  };
  const ObserverTables& obs = observer_tables();
  DeviceBuffer cmf = put(&obs.cmf[0][0], sizeof obs.cmf), illum = put(obs.d65, sizeof obs.d65);
  ar::SceneTables tables{};
  tables.observer = ar::ObserverView{static_cast<const float*>(cmf.ptr), static_cast<const float*>(illum.ptr)};
  DeviceBuffer tables_buf = put(&tables, sizeof tables);
  // [decoy lamp, world]: a wrong world index would read the lamp.
  const ar::LightRecord decoy =
      ar::light_make(ar::LightType::POINT, ar::Real3{1, 2, 3}, ar::Real(0), ar::Real(50), world_colour());
  const ar::LightRecord lights[2] = {decoy, world};
  DeviceBuffer lights_buf = put(lights, sizeof lights);
  ar::KernelGlobals kg{};
  kg.tables = static_cast<const ar::SceneTables*>(tables_buf.ptr);
  kg.lights = static_cast<const ar::LightRecord*>(lights_buf.ptr);
  kg.world_light = 1;
  DeviceBuffer kg_buf = put(&kg, sizeof kg);
  const uint64_t baseline = dev.ledger().total();

  Result r;
  {
    DeviceBuffer grid = dev.allocate(LedgerCategory::TABLES, uint64_t(kN) * kN * sizeof(float), kOwner);
    const ar::EnvBake bake{static_cast<float*>(grid.ptr), kN};
    dev.upload(kg_buf, offsetof(ar::KernelGlobals, env_bake), &bake, sizeof bake);
    dev.set_kernel_globals(kg_buf);
    dev.memset(grid, 0, 0xFF, grid.bytes);  // a cell the kernel forgets reads back as NaN
    const auto t0 = std::chrono::steady_clock::now();
    dev.launch(KernelId::BAKE_ENVIRONMENT, kN * kN);
    dev.synchronize();
    r.launch_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    r.grid.resize(size_t(kN) * kN);
    dev.download(r.grid.data(), grid, 0, grid.bytes);
    dev.free(grid);
  }
  EnvironmentBaker baker(kOwner);
  r.baked = baker.get(dev, kg_buf, {1, 2, 3}, 1.0, kN);
  baker.get(dev, kg_buf, {1, 2, 3}, 1.0, kN);
  r.rebuilds_after_two = baker.rebuild_count();
  CHECK(dev.ledger().total() == baseline);
  for (DeviceBuffer* b : {&cmf, &illum, &tables_buf, &lights_buf, &kg_buf}) dev.free(*b);
  return r;
}

void check_result(const Result& r, double want) {
  uint32_t bad = 0;
  for (float v : r.grid) bad += !(std::fabs(double(v) - want) <= 1e-6 * want);
  CHECK(bad == 0u);
  CHECK(r.rebuilds_after_two == 1u);
  REQUIRE(bool(r.baked.table));
  CHECK(std::fabs(r.baked.max - want) <= 1e-6 * want);
  CHECK(std::fabs(r.baked.mean - want) <= 1e-6 * want);
  for (const EnvDistribution* d : {&r.baked.table->plain, &r.baked.table->compensated}) {
    CHECK(d->func_integral == 1.0f);
    CHECK(std::all_of(d->func.begin(), d->func.end(), [](float f) { return f == 1.0f; }));
  }
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("agree/bake environment cpu vs cuda") {
  const ar::LightRecord world = ar::light_make_world(ar::Real(1.75), world_colour());
  const double want = display_energy({{world.emission, world.strength}},
                                     ar::ObserverView{&observer_tables().cmf[0][0], observer_tables().d65});
  REQUIRE(want > 0.0);

  Ledger cpu_ledger, gpu_ledger;
  ContextState ctx;
  ThreadPool pool(ctx, 2);
  CpuDevice cpu(cpu_ledger, pool);
  cpu.register_production_stages();
  std::unique_ptr<ModuleDevice> gpu = make_cuda_device(gpu_ledger, 0);
  const Result c = run(cpu, world);
  const Result g = run(*gpu, world);
  std::printf("bake_environment 64^2 launch: cpu %.2f ms, cuda %.2f ms\n", c.launch_ms, g.launch_ms);

  check_result(c, want);
  check_result(g, want);
  // The two devices agree with each other cell for cell.
  double worst = 0.0;
  for (size_t i = 0; i < c.grid.size(); ++i) worst = std::max(worst, std::fabs(double(c.grid[i]) - double(g.grid[i])) / want);
  CHECK(worst <= 1e-6);
  CHECK(std::fabs(c.baked.max - g.baked.max) <= 1e-6 * want);
  CHECK(std::fabs(c.baked.mean - g.baked.mean) <= 1e-6 * want);
  CHECK(cpu_ledger.total() == 0);
  CHECK(gpu_ledger.total() == 0);
}

}  // TEST_SUITE
