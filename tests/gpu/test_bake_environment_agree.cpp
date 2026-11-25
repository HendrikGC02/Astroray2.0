// SPDX-License-Identifier: Apache-2.0
// The production k_bake_environment on the CPU device and on CUDA over a world program, an
// Environment Texture (0009 d17 step iii, the per-kernel fixture of 0002 D4). A uniform world (a
// 1 x 1 image, Closest) agrees to 1e-6 relative on each device against the host's display_energy
// of its colour, and through EnvironmentBaker the table is the uniform density 1, max and mean are
// that energy, and an unchanged key bakes nothing again. A world of a 64 x 32 grey image whose every
// texel is its own grey (a grey's lift is analytic), and the same world in saturated colours, agree
// cell for cell between the CPU and the no-FMA module to 1e-6 wherever a cell centre is clear of a
// texel edge: Closest reads one texel there on both devices, so the lift sees identical inputs and
// is bit-identical (RULINGS 96 (b)); the ulp of texel position RULINGS 96 found amplified by the lift
// comes from Linear lookups, which this case does not make. The cell function on an analytic world
// is agree_pure's case.
// Ref: docs/decisions/0004-integrator.md D11; 0002 D12; RULINGS 96; kernel/stages/bake_environment.h
// (the error bound: a direction-independent world has e_s = u, so 9 u = 5.4e-7 per device, under the
// 1e-6 of this test for each device against the host and 2 x 5.4e-7 for the pair).
#include <doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "color/srgb.h"
#include "device/cpu/cpu_device.h"
#include "host/data_resource.h"
#include "host/device.h"
#include "host/light/env_bake.h"
#include "host/light/light_builder.h"
#include "host/material/material_store.h"
#include "host/scene/world.h"
#include "host/thread_pool.h"
#include "scene/scene_view.h"
#include "stages/bake_environment.h"
#include "vm/context.h"
#include "vm/ops.h"

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

ar::ObserverView host_observer() { return ar::ObserverView{&observer_tables().cmf[0][0], observer_tables().d65}; }

// A world program over one float RGBA image (rows bottom-up): LOAD_CTX_P -> TEX_ENVIRONMENT (image 7,
// Closest) -> EMIT_LE at `strength`, as the addon lowers an Environment Texture on a Background.
struct World {
  uint32_t w = 1, h = 1;
  std::vector<float> rgba;
  WorldOutput out;
  World(const MaterialStore& store, uint32_t width, uint32_t height, std::vector<float> px, float strength)
      : w(width), h(height), rgba(std::move(px)) {
    StoredGraph g;
    g.f32 = {strength};
    g.i32 = {7, 0};
    g.nodes.assign(4, ar_graph_node{});
    g.nodes[0].op = uint32_t(ar::Op::LOAD_CTX_P);
    g.nodes[1].op = uint32_t(ar::Op::TEX_ENVIRONMENT), g.nodes[1].i32_first = 0, g.nodes[1].i32_count = 2;
    g.nodes[2].op = uint32_t(ar::Op::CONST_F32), g.nodes[2].f32_first = 0, g.nodes[2].f32_count = 1;
    g.nodes[3].op = uint32_t(ar::Op::EMIT_LE);
    g.links = {{0u, 0u, 1u, 0u}, {1u, 0u, 3u, 0u}, {2u, 0u, 3u, 1u}};
    const ImageSlotFn slots = [](uint32_t key, uint32_t* slot) { return key == 7u ? (*slot = 0u, true) : false; };
    const ImageMeanFn means = [](uint32_t, std::array<float, 3>* rgb) { return (*rgb = {1.0f, 1.0f, 1.0f}, true); };
    ar_world_desc desc = {};
    desc.surface_graph = 1;
    out = compile_world(desc, &g, store.table(), &means, &slots);
    REQUIRE_MESSAGE(!out.magenta, "the environment world compiled to magenta");
    REQUIRE(out.program.size() > 2);
  }
};

struct Result {
  std::vector<float> grid;      // the kernel's raw cells
  BakedEnvironment baked;       // through the baker
  uint32_t rebuilds_after_two = 0;
  double launch_ms = 0.0;       // the raw launch, synchronised
};

// Every table the world program reads, copied to `dev`, then the raw grid and the baker's result. With
// `nofma` (a ModuleDevice) the raw grid comes from the no-FMA module of 0002 D12.
Result run(Device& dev, const MaterialStore& store, const World& world, ModuleDevice* nofma = nullptr) {
  std::vector<DeviceBuffer> held;
  const auto put = [&](const void* p, uint64_t bytes) {
    DeviceBuffer b = dev.allocate(LedgerCategory::TABLES, bytes, kOwner);
    dev.upload(b, 0, p, bytes);
    held.push_back(b);
    return b.ptr;
  };
  const ObserverTables& obs = observer_tables();
  std::array<float, ar::kSrgbTableSize> srgb{};
  ar::srgb_decode_table(srgb.data());
  const ar::Rgb2SpecTable& jh = store.table();
  ar::Rgb2SpecTable jt = jh;
  jt.z_nodes = static_cast<const float*>(put(jh.z_nodes, uint64_t(jh.res) * 4));
  jt.coeffs = static_cast<const float*>(put(jh.coeffs, 36ull * uint64_t(jh.res) * uint64_t(jh.res) * uint64_t(jh.res)));
  ar::DeviceImageView view{};
  view.texels = uint64_t(uintptr_t(put(world.rgba.data(), world.rgba.size() * 4)));
  view.width = world.w, view.height = world.h, view.channels = 4, view.format = uint32_t(ar::TexFormat::FLOAT);
  view.encoding = uint32_t(ar::TexEncoding::LINEAR);
  ar::SceneTables tables{};
  tables.rgb2spec = static_cast<const ar::Rgb2SpecTable*>(put(&jt, sizeof jt));
  tables.observer = ar::ObserverView{static_cast<const float*>(put(&obs.cmf[0][0], sizeof obs.cmf)),
                                     static_cast<const float*>(put(obs.d65, sizeof obs.d65))};
  tables.images = static_cast<const ar::DeviceImageView*>(put(&view, sizeof view));
  tables.image_count = 1;
  tables.srgb_decode = static_cast<const float*>(put(srgb.data(), sizeof srgb));
  ar::SceneData sd{};
  sd.world_program = static_cast<const uint32_t*>(put(world.out.program.data(), world.out.program.size() * 4));
  sd.world_program_words = uint32_t(world.out.program.size());
  sd.world_constants = static_cast<const ar::ConstantBlock*>(put(&world.out.block, sizeof world.out.block));
  ar::KernelGlobals kg{};
  kg.tables = static_cast<const ar::SceneTables*>(put(&tables, sizeof tables));
  kg.scene = static_cast<const ar::SceneData*>(put(&sd, sizeof sd));
  DeviceBuffer kg_buf = dev.allocate(LedgerCategory::TABLES, sizeof kg, kOwner);
  dev.upload(kg_buf, 0, &kg, sizeof kg);
  held.push_back(kg_buf);
  const uint64_t baseline = dev.ledger().total();

  Result r;
  {
    DeviceBuffer grid = dev.allocate(LedgerCategory::TABLES, uint64_t(kN) * kN * sizeof(float), kOwner);
    const ar::EnvBake bake{static_cast<float*>(grid.ptr), kN};
    dev.upload(kg_buf, offsetof(ar::KernelGlobals, env_bake), &bake, sizeof bake);
    dev.set_kernel_globals(kg_buf);
    dev.memset(grid, 0, 0xFF, grid.bytes);  // a cell the kernel forgets reads back as NaN
    const auto t0 = std::chrono::steady_clock::now();
    if (nofma) {
      const uint32_t module = nofma->load_module_file(std::string(AR_TEST_KERNEL_NOFMA_DIR) + "/bake_environment.cubin");
      const ar::KernelGlobals* kg_dev = static_cast<const ar::KernelGlobals*>(kg_buf.ptr);
      void* args[] = {&kg_dev};
      nofma->launch_module_kernel(module, "k_bake_environment", (kN * kN + 255) / 256, 256, args);
    } else {
      dev.launch(KernelId::BAKE_ENVIRONMENT, kN * kN);
    }
    dev.synchronize();
    r.launch_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    r.grid.resize(size_t(kN) * kN);
    dev.download(r.grid.data(), grid, 0, grid.bytes);
    dev.free(grid);
    const ar::EnvBake none{nullptr, 0};
    dev.upload(kg_buf, offsetof(ar::KernelGlobals, env_bake), &none, sizeof none);
  }
  EnvironmentBaker baker(kOwner);
  try {
    r.baked = baker.get(dev, kg_buf, {1, 2, 3}, nullptr, kN);
    baker.get(dev, kg_buf, {1, 2, 3}, nullptr, kN);
  } catch (const BakeRefused&) {
    // a world with no table to build (the varied world's grid is checked raw)
  }
  r.rebuilds_after_two = baker.rebuild_count();
  CHECK(dev.ledger().total() == baseline);
  for (DeviceBuffer& b : held) dev.free(b);
  return r;
}

struct Devices {
  Ledger cpu_ledger, gpu_ledger;
  ContextState ctx;
  ThreadPool pool{ctx, 2};
  CpuDevice cpu{cpu_ledger, pool};
  std::unique_ptr<ModuleDevice> gpu = make_cuda_device(gpu_ledger, 0);
  Devices() { cpu.register_production_stages(); }
};

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("agree/bake environment cpu vs cuda") {
  MaterialStore store{std::make_shared<Ledger>()};
  const float rgb[3] = {1.0f, 0.5f, 0.25f}, strength = 1.75f;
  const World world(store, 1, 1, {rgb[0], rgb[1], rgb[2], 1.0f}, strength);
  ar::SpectrumRef ref;
  uint32_t flags = 0;
  ar::make_rgb_ref(store.table(), ar::Rgb{{ar::Real(rgb[0]), ar::Real(rgb[1]), ar::Real(rgb[2])}}, ar::SpectrumSemantic::ILLUMINANT,
                   &ref, &flags);
  const double want = display_energy({{ref, strength}}, host_observer());
  REQUIRE(want > 0.0);

  Devices d;
  const Result c = run(d.cpu, store, world);
  const Result g = run(*d.gpu, store, world);
  std::printf("bake_environment 64^2 launch, uniform program world: cpu %.2f ms, cuda %.2f ms\n", c.launch_ms, g.launch_ms);
  for (const Result* r : {&c, &g}) {
    uint32_t bad = 0;
    for (float v : r->grid) bad += !(std::fabs(double(v) - want) <= 1e-6 * want);
    CHECK(bad == 0u);
    CHECK(r->rebuilds_after_two == 1u);
    REQUIRE(bool(r->baked.table));
    CHECK(std::fabs(r->baked.max - want) <= 1e-6 * want);
    CHECK(std::fabs(r->baked.mean - want) <= 1e-6 * want);
    for (const EnvDistribution* dist : {&r->baked.table->plain, &r->baked.table->compensated}) {
      CHECK(dist->func_integral == 1.0f);
      CHECK(std::all_of(dist->func.begin(), dist->func.end(), [](float f) { return f == 1.0f; }));
    }
  }
  // The two devices agree with each other cell for cell.
  double worst = 0.0;
  for (size_t i = 0; i < c.grid.size(); ++i) worst = std::max(worst, std::fabs(double(c.grid[i]) - double(g.grid[i])) / want);
  CHECK(worst <= 1e-6);
  CHECK(std::fabs(c.baked.max - g.baked.max) <= 1e-6 * want);
  CHECK(std::fabs(c.baked.mean - g.baked.mean) <= 1e-6 * want);
  CHECK(d.cpu_ledger.total() == 0);
  CHECK(d.gpu_ledger.total() == 0);
}

// The varied world of `grey` texels (r = g = b) or saturated colours, CPU against the no-FMA module:
// the worst relative difference over cells clear of a texel edge, and how many were compared.
struct Compared {
  double worst = 0.0;
  uint32_t checked = 0, skipped = 0;
};
Compared compare_varied(bool grey) {
  constexpr uint32_t W = 64, H = 32;
  std::vector<float> px(size_t(W) * H * 4, 1.0f);
  for (uint32_t y = 0; y < H; ++y)
    for (uint32_t x = 0; x < W; ++x) {
      const float g = 0.1f + 0.9f * float(x) / float(W) + 0.5f * float(y) / float(H);
      const float c[3] = {grey ? g : 0.1f + 0.9f * float(x) / float(W), grey ? g : 0.1f + 0.9f * float(y) / float(H),
                          grey ? g : 0.25f + 0.5f * float((x + y) & 1u)};
      for (int k = 0; k < 3; ++k) px[(size_t(y) * W + x) * 4 + size_t(k)] = c[k];
    }
  MaterialStore store{std::make_shared<Ledger>()};
  const World world(store, W, H, px, 1.5f);
  Devices d;
  const Result c = run(d.cpu, store, world);
  const Result g = run(*d.gpu, store, world, d.gpu.get());
  Compared out;
  const double pi = 3.14159265358979323846;
  for (uint32_t cell = 0; cell < kN * kN; ++cell) {
    const ar::Real3 dir = ar::bake_environment_direction(kN, cell);
    // Cycles' direction_to_equirectangular in double; a centre within 1e-3 texel of an edge may read
    // the neighbouring texel on one device and is not compared.
    const double fu = (0.5 - std::atan2(double(dir.y), double(dir.x)) / (2.0 * pi)) * W;
    const double fv = (1.0 - std::acos(std::max(-1.0, std::min(1.0, double(dir.z)))) / pi) * H;
    if (std::fabs(fu - std::round(fu)) < 1e-3 || std::fabs(fv - std::round(fv)) < 1e-3) {
      ++out.skipped;
      continue;
    }
    REQUIRE(c.grid[cell] > 0.0f);
    out.worst = std::max(out.worst, std::fabs(double(c.grid[cell]) - double(g.grid[cell])) / double(c.grid[cell]));
    ++out.checked;
  }
  std::printf("agree bake_environment %s program world, cpu vs cuda nofma: %u cells, %u at texel edges, worst relative %.3e\n",
              grey ? "grey" : "colour", out.checked, out.skipped, out.worst);
  CHECK(out.checked > kN * kN * 3 / 4);
  return out;
}

TEST_CASE("agree/bake environment program world") {
  CHECK(compare_varied(true).worst <= 1e-6);
  CHECK(compare_varied(false).worst <= 1e-6);
}

}  // TEST_SUITE
