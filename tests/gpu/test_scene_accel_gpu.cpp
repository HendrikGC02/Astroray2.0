// SPDX-License-Identifier: Apache-2.0
// The traversal structures of a scene snapshot on CUDA (task 114): the scene of the CPU test (one
// quad, a rectangle lamp, a sphere lamp) built through the scene's own OptiX builder gives the same
// closest hits as the CPU's Embree structures (instance exact, t within 1e-5 relative) when traced
// with the TLAS handle the table set carries; the scene ledger holds the structures while a
// snapshot is held and nothing once the last holder is gone; a moved instance rebuilds only the TLAS.
// Ref: docs/decisions/0002-kernel-architecture.md D11, D13; RULINGS 70; OptiX 9.1 Programming Guide.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "device/cuda/optix_accel.h"
#include "device/cuda/optix_pipeline.h"
#include "host/device.h"
#include "host/fault.h"
#include "host/light/lamps.h"
#include "host/light/light_builder.h"
#include "host/material/material_store.h"
#include "host/scene/accel.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/scene/scene_tables.h"
#include "host/session/kg_builder.h"
#include "host/thread_pool.h"
#include "stages/route_hit.h"

using namespace astroray;
using namespace astroray::host;
using ar::Stage;

namespace {

constexpr ar_key kQuad = 100, kRectLamp = 200, kSphereLamp = 201;

struct Held {
  std::shared_ptr<Ledger> ledger = std::make_shared<Ledger>();
  std::unique_ptr<ModuleDevice> device;
  std::unique_ptr<AccelBuilder> accel;
};

// The scene of tests/cpu/test_scene_accel.cpp, on a scene with a CUDA device or none.
struct Rig {
  ContextState ctx;
  std::shared_ptr<Held> held = std::make_shared<Held>();
  std::unique_ptr<Scene> scene;
  explicit Rig(bool cuda, bool populate = true) {
    SceneConfig cfg;
    cfg.workers = 2;
    if (cuda) {
      auto h = held;
      cfg.init = [h]() -> std::shared_ptr<void> {
        h->device = make_cuda_device(*h->ledger, 0);
        const std::filesystem::path cache = std::filesystem::temp_directory_path() / "astroray_scene_accel_cache";
        h->accel = make_optix_accel_builder(*h->device, 0, cache.string());
        return h;
      };
      cfg.device_of = [](void* p) -> Device* { return static_cast<Held*>(p)->device.get(); };
      cfg.accel_of = [](void* p) -> AccelBuilder* { return static_cast<Held*>(p)->accel.get(); };
    }
    scene = std::make_unique<Scene>(ctx, std::move(cfg));
    scene->meshes = std::make_shared<MeshStore>();
    scene->materials = std::make_shared<MaterialStore>(scene->meshes->shared_ledger());
    if (populate) build();
  }
  template <class F>
  void commit(F&& body) {
    auto e = scene->begin_edit();
    REQUIRE(e);
    body(*e);
    uint64_t g = 0;
    REQUIRE(e->commit(&g) == AR_OK);
    scene->materials->end_edit();
    e.reset();
    scene->generations().wait_idle();
    REQUIRE(scene->generations().failed() == 0);
  }
  static ar_light_desc lamp(uint32_t type, float x, float y, float z, float sx, float sy) {
    ar_light_desc l = {};
    l.type = type;
    l.color.kind = AR_SPECTRUM_RGB;
    l.color.rgb[0] = l.color.rgb[1] = l.color.rgb[2] = 1.0f;
    l.xf.m[0] = l.xf.m[5] = l.xf.m[10] = 1.0f;
    l.xf.m[3] = x, l.xf.m[7] = y, l.xf.m[11] = z;
    l.visibility = AR_VISIBILITY_CAMERA | AR_VISIBILITY_DIFFUSE;
    l.size_x = sx, l.size_y = sy, l.power = 10.0f, l.normalize = 1, l.cast_shadow = 1;
    return l;
  }
  void build() {
    commit([&](Edit& e) {
      ar_light_desc rect = lamp(AR_LIGHT_AREA_RECT, 5, 0, 3, 2, 2), sphere = lamp(AR_LIGHT_POINT, -5, 0, 0, 1, 0);
      REQUIRE(set_light(e, kRectLamp, &rect, scene->materials->table()) == AR_OK);
      REQUIRE(set_light(e, kSphereLamp, &sphere, scene->materials->table()) == AR_OK);
    });
    const std::vector<float> pos{0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    const std::vector<int32_t> cv{0, 1, 2, 3}, fs{0};
    commit([&](Edit& e) {
      ar_mesh_desc d = {};
      d.key = kQuad, d.vertex_count = 4, d.corner_count = 4, d.face_count = 1, d.motion_steps = 1;
      ar_mesh_buffers b = {};
      REQUIRE(mesh_begin(*scene, e, &d, &b) == AR_OK);
      std::memcpy(*b.positions, pos.data(), pos.size() * 4);
      std::memcpy(*b.corner_vert, cv.data(), cv.size() * 4);
      std::memcpy(*b.face_start, fs.data(), fs.size() * 4);
      for (uint64_t i = 0; i < b.corner_normals_count; ++i) (*b.corner_normals)[i] = i % 3 == 2 ? 1.0f : 0.0f;
      REQUIRE(e.item_commit(kQuad) == AR_OK);
    });
    place_quad(0.0f);
  }
  void place_quad(float x) {
    commit([&](Edit& e) {
      std::vector<ar_key> geom{kQuad}, object{1};
      std::vector<ar_xform> xf(1);
      xf[0].m[0] = xf[0].m[5] = xf[0].m[10] = 1.0f, xf[0].m[3] = x;
      std::vector<uint32_t> zeros(1, 0), vis(1, AR_VISIBILITY_CAMERA | AR_VISIBILITY_SHADOW), moff{0}, mcount{0};
      std::vector<ar_rgba> colors(1, ar_rgba{{1, 1, 1, 1}});
      std::vector<uint8_t> lg(1, 0);
      ar_instances_desc d = {};
      d.instance_count = 1, d.motion_steps = 1, d.transform_count = 1, d.material_key_count = 0;
      d.geom_keys = geom.data(), d.xf = xf.data(), d.object_keys = object.data();
      d.random_ids = d.pass_indices = d.crypto_objects = d.crypto_assets = zeros.data();
      d.colors = colors.data(), d.visibility = vis.data(), d.flags = zeros.data(), d.light_groups = lg.data();
      d.material_offsets = moff.data(), d.material_counts = mcount.data(), d.material_keys = nullptr;
      REQUIRE(set_instances(e, &d) == AR_OK);
    });
  }
};

struct Hit {
  ar::Real t = 0;
  uint32_t inst = 0;
};
using Rays = std::vector<std::pair<float, float>>;

// Camera rays from z = 10 straight down, on the CPU stage a session launches.
std::vector<Hit> trace_cpu(const SceneTableSet& set, const Rays& xy) {
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool{ctx, 2};
  CpuDevice dev{ledger, pool};
  dev.register_stage(Stage::INTERSECT_CLOSEST, &cpu_stage_intersect_closest);
  const size_t n = xy.size();
  std::vector<uint8_t> next(n, 0xEE), ray_type(n, 0x01);
  std::vector<uint32_t> flags(n, 0), hit_prim(n), hit_inst(n), queue(n);
  std::vector<ar::Real> hit_t(n);
  std::vector<ar::Real2> hit_uv(n);
  std::vector<ar::Real3> ray_o(n), ray_d(n, ar::Real3{0, 0, -1});
  for (size_t i = 0; i < n; ++i) ray_o[i] = {xy[i].first, xy[i].second, 10.0f}, queue[i] = uint32_t(i);
  ar::PathStateSoA path{};
  path.next_stage = next.data(), path.ray_type = ray_type.data(), path.flags = flags.data();
  path.hit_prim = hit_prim.data(), path.hit_inst = hit_inst.data(), path.hit_t = hit_t.data();
  path.hit_uv = hit_uv.data(), path.ray_o = ray_o.data(), path.ray_d = ray_d.data();
  ar::StepTable step{};
  step.stage[static_cast<uint32_t>(Stage::INTERSECT_CLOSEST)] = {0, uint32_t(n), 0};
  ar::KernelGlobals kg = build_kernel_globals(&set, KgSession{});
  kg.path = &path, kg.step_table = &step, kg.main_queue = queue.data();
  DeviceBuffer kg_buf = dev.allocate(LedgerCategory::TABLES, sizeof kg, {1, 1});
  dev.upload(kg_buf, 0, &kg, sizeof kg);
  dev.set_kernel_globals(kg_buf);
  dev.launch(Stage::INTERSECT_CLOSEST, uint32_t(n));
  dev.free(kg_buf);
  std::vector<Hit> out(n);
  for (size_t i = 0; i < n; ++i) out[i] = {hit_t[i], hit_inst[i]};
  return out;
}

template <class T>
DeviceBuffer put(Device& dev, const std::vector<T>& v) {
  DeviceBuffer b = dev.allocate(LedgerCategory::TABLES, sizeof(T) * std::max<size_t>(v.size(), 1), Owner{1, 0});
  if (!v.empty()) dev.upload(b, 0, v.data(), sizeof(T) * v.size());
  return b;
}

// The same rays on CUDA: the OptiX pipeline of the scene's context, traced against the TLAS handle
// the table set carries in SceneData::accel, through the device KernelGlobals the builder made.
std::vector<Hit> trace_cuda(Rig& r, const SceneTableSet& set, const Rays& xy) {
  Device& dev = *r.scene->device();
  OptixContext& optix = static_cast<SceneOptixBuilder&>(r.scene->accel_builder()).optix_context();
  auto pipeline = std::make_unique<TraversalPipeline>(optix, dev, optix.stream(), Owner{1, 0});
  const uint32_t n = uint32_t(xy.size());
  std::vector<uint8_t> next(n, 0xEE), type(n, 0x01);
  std::vector<uint32_t> flags(n, 0), prim(n, 7), inst(n, 7), queue(n);
  std::vector<ar::Real> hit_t(n, -1.0f), tmax(n, 1e30f);
  std::vector<ar::Real2> uv(n, ar::Real2{0, 0});
  std::vector<ar::Real3> o(n), d(n, ar::Real3{0, 0, -1});
  for (uint32_t i = 0; i < n; ++i) queue[i] = i, o[i] = {xy[i].first, xy[i].second, 10.0f};
  ar::StepTable step{};
  step.stage[static_cast<uint32_t>(Stage::INTERSECT_CLOSEST)] = {0, n, 0};
  std::vector<DeviceBuffer> owned;
  const auto keep = [&](DeviceBuffer b) {
    owned.push_back(b);
    return b;
  };
  const DeviceBuffer b_next = keep(put(dev, next)), b_type = keep(put(dev, type)), b_flags = keep(put(dev, flags)),
                     b_prim = keep(put(dev, prim)), b_inst = keep(put(dev, inst)), b_t = keep(put(dev, hit_t)),
                     b_uv = keep(put(dev, uv)), b_tmax = keep(put(dev, tmax)), b_o = keep(put(dev, o)),
                     b_d = keep(put(dev, d)), b_q = keep(put(dev, queue));
  ar::PathStateSoA path{};
  path.next_stage = static_cast<uint8_t*>(b_next.ptr), path.ray_type = static_cast<uint8_t*>(b_type.ptr);
  path.flags = static_cast<uint32_t*>(b_flags.ptr), path.hit_prim = static_cast<uint32_t*>(b_prim.ptr);
  path.hit_inst = static_cast<uint32_t*>(b_inst.ptr), path.hit_t = static_cast<ar::Real*>(b_t.ptr);
  path.hit_uv = static_cast<ar::Real2*>(b_uv.ptr), path.ray_tmax = static_cast<ar::Real*>(b_tmax.ptr);
  path.ray_o = static_cast<ar::Real3*>(b_o.ptr), path.ray_d = static_cast<ar::Real3*>(b_d.ptr);
  const DeviceBuffer path_buf = keep(put(dev, std::vector<ar::PathStateSoA>{path}));
  const DeviceBuffer step_buf = keep(put(dev, std::vector<ar::StepTable>{step}));
  ar::KernelGlobals kg = build_kernel_globals(&set, KgSession{});
  kg.path = static_cast<const ar::PathStateSoA*>(path_buf.ptr), kg.step_table = static_cast<const ar::StepTable*>(step_buf.ptr);
  kg.main_queue = static_cast<const uint32_t*>(b_q.ptr);
  const DeviceBuffer kg_buf = keep(put(dev, std::vector<ar::KernelGlobals>{kg}));
  // The convention of scene_view.h: the integer in SceneData::accel is the traversable.
  pipeline->launch_closest(uint64_t(reinterpret_cast<uintptr_t>(set.scene_host.accel)), kg_buf, 1024);
  optix.sync();
  dev.download(hit_t.data(), b_t, 0, sizeof(ar::Real) * n);
  dev.download(inst.data(), b_inst, 0, sizeof(uint32_t) * n);
  for (DeviceBuffer& b : owned) dev.free(b);
  pipeline.reset();
  std::vector<Hit> out(n);
  for (uint32_t i = 0; i < n; ++i) out[i] = {hit_t[i], inst[i]};
  return out;
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("scene_accel_gpu/matches cpu") {
  Rig cpu(false), gpu(true);
  const auto cpu_set = acquire_scene_tables(*cpu.scene);
  const auto gpu_set = acquire_scene_tables(*gpu.scene);
  REQUIRE(gpu_set->accel);
  CHECK(gpu_set->scene_host.accel != nullptr);
  // The quad, both lamps, misses, and a spread over the quad and the rectangle.
  Rays rays{{0.75f, 0.25f}, {5.2f, 0.3f}, {-5.0f, 0.0f}, {20.0f, 20.0f}, {-5.6f, 0.4f}, {4.2f, -0.9f}};
  for (int i = 0; i < 64; ++i) rays.push_back({0.01f + 0.015f * float(i), 0.9f - 0.013f * float(i)});
  const auto c = trace_cpu(*cpu_set, rays), g = trace_cuda(gpu, *gpu_set, rays);
  uint32_t hits = 0;
  for (size_t i = 0; i < rays.size(); ++i) {
    CHECK_MESSAGE(c[i].inst == g[i].inst, "ray " << i);
    if (c[i].inst == ar::kNoInstance) continue;
    ++hits;
    CHECK_MESSAGE(std::fabs(c[i].t - g[i].t) <= 1e-5f * std::fabs(c[i].t), "ray " << i << " cpu t " << c[i].t << " cuda t " << g[i].t);
  }
  CHECK(hits >= 60u);
  CHECK(std::fabs(g[0].t - 10.0f) < 1e-4f);
  CHECK(std::fabs(g[1].t - 7.0f) < 1e-4f);
  CHECK(std::fabs(g[2].t - 9.0f) < 1e-4f);
}

TEST_CASE("scene_accel_gpu/empty scene") {
  Rig r(true, false);
  const auto set = acquire_scene_tables(*r.scene);
  REQUIRE(set->accel);
  const auto missing = missing_pointers(build_kernel_globals(set.get(), KgSession{}), *set, false);
  REQUIRE(std::find(missing.begin(), missing.end(), "scene.accel") == missing.end());
  const auto h = trace_cuda(r, *set, {{0.5f, 0.5f}});
  CHECK(h[0].inst == ar::kNoInstance);
}

TEST_CASE("scene_accel_gpu/ledger and reuse") {
  Rig r(true);
  AccelBuilder& builder = r.scene->accel_builder();
  auto first = acquire_scene_tables(*r.scene);
  const AccelCounters a = builder.counters();
  CHECK(a.gas_builds == 3u);  // the quad and the two lamp shapes
  CHECK(a.tlas_builds == 1u);
  CHECK(first->accel->bytes > 0u);
  // The structures are in the scene's ledger while a snapshot holds them.
  CHECK(r.held->ledger->category_total(LedgerCategory::ACCELERATION) > 0u);

  r.place_quad(2.0f);
  auto second = acquire_scene_tables(*r.scene);
  REQUIRE((second != first));
  const AccelCounters b = builder.counters();
  CHECK(b.gas_builds == a.gas_builds);  // the GAS are shared by identity, only the TLAS is new
  CHECK(b.gas_reused == a.gas_reused + 3u);
  CHECK(b.tlas_builds == a.tlas_builds + 1u);
  // The older snapshot's TLAS stays valid while it is held (RULINGS 70).
  const Rays rays{{0.75f, 0.25f}, {2.75f, 0.25f}};
  const auto on_first = trace_cuda(r, *first, rays), on_second = trace_cuda(r, *second, rays);
  CHECK(on_first[0].inst == 0u);
  CHECK(on_first[1].inst == ar::kNoInstance);
  CHECK(on_second[0].inst == ar::kNoInstance);
  CHECK(on_second[1].inst == 0u);

  // Last holder gone: nothing of the structures is left in the ledger.
  first.reset();
  second.reset();
  r.scene->table_cache().current.reset();
  CHECK(r.held->ledger->total() == 0u);
}

}  // TEST_SUITE
