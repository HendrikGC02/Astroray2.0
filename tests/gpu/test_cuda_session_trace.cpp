// SPDX-License-Identifier: Apache-2.0
// The traversal stages of a CUDA session (task 115): a session's own CudaDevice, with its own
// TraversalPipeline built on the scene's OptiX context, runs INTERSECT_CLOSEST and INTERSECT_SHADOW
// through Device::launch(Stage) on the pinned snapshot's TLAS, and agrees with the CPU device's
// stages on the same scene (114's quad, rectangle lamp and sphere lamp): instance and next stage
// exact, t within 1e-5 relative, shadow outcome exact. The pipeline's buffers are in the session's
// ledger and gone after detach. A stage without a route throws naming it. The scene's OptiX disk
// cache is ar_scene_desc.cache_dir: a directory receives the cache, none disables it.
// Ref: docs/decisions/0002-kernel-architecture.md D4, D8, D11; RULINGS 70; 0007 d45; OptiX 9.1
// Programming Guide (optixLaunch, compilation caching).
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "device/cuda/cuda_device.h"
#include "device/cuda/optix_accel.h"
#include "host/device.h"
#include "host/fault.h"
#include "host/light/lamps.h"
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

// The scene of tests/gpu/test_scene_accel_gpu.cpp on a CUDA scene device (or none, for the CPU),
// its OptiX context made with `cache_dir` as ar_scene_create makes it.
struct Rig {
  ContextState ctx;
  std::shared_ptr<Held> held = std::make_shared<Held>();
  std::unique_ptr<Scene> scene;
  Rig(bool cuda, const std::string& cache_dir, bool populate = true) {
    SceneConfig cfg;
    cfg.workers = 2;
    if (cuda) {
      auto h = held;
      cfg.init = [h, cache_dir]() -> std::shared_ptr<void> {
        h->device = make_cuda_device(*h->ledger, 0);
        h->accel = make_optix_accel_builder(*h->device, 0, cache_dir);
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
  SceneOptixBuilder& optix() { return dynamic_cast<SceneOptixBuilder&>(scene->accel_builder()); }
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
    commit([&](Edit& e) {
      std::vector<ar_key> geom{kQuad}, object{1};
      std::vector<ar_xform> xf(1);
      xf[0].m[0] = xf[0].m[5] = xf[0].m[10] = 1.0f;
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

struct Ray {
  ar::Real3 o, d;
  float tmax;
};

// What the two stages wrote.
struct Traced {
  std::vector<ar::Real> t;
  std::vector<uint32_t> inst;
  std::vector<uint8_t> next, shadow_next;
};

template <class T>
DeviceBuffer put(Device& dev, std::vector<DeviceBuffer>& owned, const std::vector<T>& v) {
  DeviceBuffer b = dev.allocate(LedgerCategory::TABLES, sizeof(T) * std::max<size_t>(v.size(), 1), Owner{0, 1});
  if (!v.empty()) dev.upload(b, 0, v.data(), sizeof(T) * v.size());
  owned.push_back(b);
  return b;
}

// Camera rays and shadow rays through Device::launch(Stage), the path the scheduler takes, on
// either device: the CPU device reads host memory, the CUDA device its own allocations.
Traced trace(Device& dev, const SceneTableSet& set, const std::vector<Ray>& camera, const std::vector<Ray>& shadow) {
  const uint32_t n = uint32_t(camera.size()), m = uint32_t(shadow.size());
  std::vector<uint8_t> next(n, 0xEE), type(n, 0x01), s_next(m, 0xEE);
  std::vector<uint32_t> flags(n, 0), prim(n, 7), inst(n, 7), mq(n), sq(m);
  std::vector<ar::Real> hit_t(n, -1.0f), tmax(n), s_tmax(m);
  std::vector<ar::Real2> uv(n, ar::Real2{0, 0});
  std::vector<ar::Real3> o(n), d(n), so(m), sd(m);
  for (uint32_t i = 0; i < n; ++i) mq[i] = i, o[i] = camera[i].o, d[i] = camera[i].d, tmax[i] = camera[i].tmax;
  for (uint32_t i = 0; i < m; ++i) sq[i] = i, so[i] = shadow[i].o, sd[i] = shadow[i].d, s_tmax[i] = shadow[i].tmax;
  ar::StepTable step{};
  step.stage[static_cast<uint32_t>(Stage::INTERSECT_CLOSEST)] = {0, n, 0};
  step.stage[static_cast<uint32_t>(Stage::INTERSECT_SHADOW)] = {0, m, 0};
  std::vector<DeviceBuffer> owned;
  const DeviceBuffer b_next = put(dev, owned, next), b_type = put(dev, owned, type), b_flags = put(dev, owned, flags),
                     b_prim = put(dev, owned, prim), b_inst = put(dev, owned, inst), b_t = put(dev, owned, hit_t),
                     b_uv = put(dev, owned, uv), b_tmax = put(dev, owned, tmax), b_o = put(dev, owned, o),
                     b_d = put(dev, owned, d), b_mq = put(dev, owned, mq), b_sn = put(dev, owned, s_next),
                     b_so = put(dev, owned, so), b_sd = put(dev, owned, sd), b_st = put(dev, owned, s_tmax),
                     b_sq = put(dev, owned, sq);
  ar::PathStateSoA path{};
  path.next_stage = static_cast<uint8_t*>(b_next.ptr), path.ray_type = static_cast<uint8_t*>(b_type.ptr);
  path.flags = static_cast<uint32_t*>(b_flags.ptr), path.hit_prim = static_cast<uint32_t*>(b_prim.ptr);
  path.hit_inst = static_cast<uint32_t*>(b_inst.ptr), path.hit_t = static_cast<ar::Real*>(b_t.ptr);
  path.hit_uv = static_cast<ar::Real2*>(b_uv.ptr), path.ray_tmax = static_cast<ar::Real*>(b_tmax.ptr);
  path.ray_o = static_cast<ar::Real3*>(b_o.ptr), path.ray_d = static_cast<ar::Real3*>(b_d.ptr);
  ar::ShadowItemSoA sh{};
  sh.next_stage = static_cast<uint8_t*>(b_sn.ptr), sh.ray_o = static_cast<ar::Real3*>(b_so.ptr);
  sh.ray_d = static_cast<ar::Real3*>(b_sd.ptr), sh.tmax = static_cast<ar::Real*>(b_st.ptr);
  const DeviceBuffer path_buf = put(dev, owned, std::vector<ar::PathStateSoA>{path});
  const DeviceBuffer shadow_buf = put(dev, owned, std::vector<ar::ShadowItemSoA>{sh});
  const DeviceBuffer step_buf = put(dev, owned, std::vector<ar::StepTable>{step});
  ar::KernelGlobals kg = build_kernel_globals(&set, KgSession{});
  kg.path = static_cast<const ar::PathStateSoA*>(path_buf.ptr);
  kg.shadow = static_cast<const ar::ShadowItemSoA*>(shadow_buf.ptr);
  kg.step_table = static_cast<const ar::StepTable*>(step_buf.ptr);
  kg.main_queue = static_cast<const uint32_t*>(b_mq.ptr), kg.shadow_queue = static_cast<const uint32_t*>(b_sq.ptr);
  const DeviceBuffer kg_buf = put(dev, owned, std::vector<ar::KernelGlobals>{kg});
  dev.set_kernel_globals(kg_buf);
  dev.launch(Stage::INTERSECT_CLOSEST, n);
  dev.launch(Stage::INTERSECT_SHADOW, m);
  dev.synchronize();
  Traced out;
  out.t.resize(n), out.inst.resize(n), out.next.resize(n), out.shadow_next.resize(m);
  dev.download(out.t.data(), b_t, 0, sizeof(ar::Real) * n);
  dev.download(out.inst.data(), b_inst, 0, sizeof(uint32_t) * n);
  dev.download(out.next.data(), b_next, 0, n);
  dev.download(out.shadow_next.data(), b_sn, 0, m);
  for (DeviceBuffer& b : owned) dev.free(b);
  return out;
}

Traced trace_cpu(const SceneTableSet& set, const std::vector<Ray>& camera, const std::vector<Ray>& shadow) {
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool{ctx, 2};
  CpuDevice dev{ledger, pool};
  dev.register_stage(Stage::INTERSECT_CLOSEST, &cpu_stage_intersect_closest);
  dev.register_stage(Stage::INTERSECT_SHADOW, &cpu_stage_intersect_shadow);
  return trace(dev, set, camera, shadow);
}

std::vector<Ray> camera_rays() {
  // Straight down from z = 10 onto the quad, both lamps and empty space, then oblique rays.
  std::vector<Ray> r;
  const ar::Real3 down{0, 0, -1};
  for (auto xy : {std::pair<float, float>{0.75f, 0.25f}, {5.2f, 0.3f}, {-5.0f, 0.0f}, {20.0f, 20.0f}, {-5.6f, 0.4f},
                  {4.2f, -0.9f}})
    r.push_back({{xy.first, xy.second, 10.0f}, down, 1e30f});
  for (int i = 0; i < 64; ++i) r.push_back({{0.01f + 0.015f * float(i), 0.9f - 0.013f * float(i), 10.0f}, down, 1e30f});
  for (int i = 0; i < 32; ++i) {
    const float a = 0.05f * float(i) - 0.8f;
    const float len = std::sqrt(a * a + 1.01f);
    r.push_back({{0.5f, 0.5f, 3.0f}, {a / len, 0.1f / len, -1.0f / len}, 1e30f});
  }
  return r;
}

std::vector<Ray> shadow_rays() {
  // Upward segments of length 2 through z = 0: occluded by the quad inside it; the lamps never block.
  std::vector<Ray> r;
  const ar::Real3 up{0, 0, 1};
  for (int i = 0; i < 16; ++i) r.push_back({{-0.4375f + 0.125f * float(i), 0.5f, -1.0f}, up, 2.0f});
  r.push_back({{5.0f, 0.0f, 2.0f}, up, 2.0f});   // through the rectangle lamp at z = 3
  r.push_back({{-5.0f, 0.0f, -2.0f}, up, 4.0f});  // through the sphere lamp
  return r;
}

// A directory's files with size and write time, to see whether anything wrote to it.
std::map<std::string, std::pair<uintmax_t, long long>> listing(const std::string& dir) {
  std::map<std::string, std::pair<uintmax_t, long long>> out;
  std::error_code ec;
  if (dir.empty() || !std::filesystem::exists(dir, ec)) return out;
  for (const auto& e : std::filesystem::recursive_directory_iterator(dir, ec)) {
    if (!e.is_regular_file(ec)) continue;
    out[e.path().string()] = {e.file_size(ec), static_cast<long long>(e.last_write_time(ec).time_since_epoch().count())};
  }
  return out;
}

std::filesystem::path fresh_dir(const char* name) {
  const std::filesystem::path p = std::filesystem::temp_directory_path() / name;
  std::error_code ec;
  std::filesystem::remove_all(p, ec);
  return p;
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("cuda_session_trace/agrees with cpu") {
  const std::filesystem::path cache = std::filesystem::temp_directory_path() / "astroray_cuda_session_trace_cache";
  Rig cpu(false, ""), gpu(true, cache.string());
  const auto cpu_set = acquire_scene_tables(*cpu.scene);
  const auto gpu_set = acquire_scene_tables(*gpu.scene);  // the snapshot a session pins
  REQUIRE(gpu_set->scene_host.accel != nullptr);

  // The session's own device: own stream and ledger on the primary context (RULINGS 70).
  Ledger session_ledger;
  auto session = make_cuda_device(session_ledger, 0);
  const TraversalAttach t = attach_cuda_traversal(*session, gpu.scene->accel_builder(), Owner{0, 1});
  std::printf("cuda_session_trace: pipeline creation module %.1f ms, pipeline %.1f ms\n", t.module_ms, t.pipeline_ms);
  CHECK(session_ledger.category_total(LedgerCategory::TABLES) > 0u);  // SBT and launch parameters
  set_cuda_traversable(*session, uint64_t(reinterpret_cast<uintptr_t>(gpu_set->scene_host.accel)));

  const auto camera = camera_rays();
  const auto shadow = shadow_rays();
  const Traced c = trace_cpu(*cpu_set, camera, shadow);
  const Traced g = trace(*session, *gpu_set, camera, shadow);
  uint32_t hits = 0;
  for (size_t i = 0; i < camera.size(); ++i) {
    CHECK_MESSAGE(c.inst[i] == g.inst[i], "ray " << i);
    CHECK_MESSAGE(c.next[i] == g.next[i], "ray " << i);
    if (c.inst[i] == ar::kNoInstance) continue;
    ++hits;
    CHECK_MESSAGE(std::fabs(c.t[i] - g.t[i]) <= 1e-5f * std::fabs(c.t[i]), "ray " << i << " cpu t " << c.t[i] << " cuda t " << g.t[i]);
  }
  CHECK(hits == 76u);  // 5 straight, 64 diagonal, 7 oblique (x = 0.5 + 3a inside the quad for i = 13..19)
  CHECK(std::fabs(g.t[0] - 10.0f) < 1e-4f);  // the quad
  CHECK(std::fabs(g.t[1] - 7.0f) < 1e-4f);   // the rectangle lamp
  CHECK(std::fabs(g.t[2] - 9.0f) < 1e-4f);   // the sphere lamp
  uint32_t occluded = 0;
  for (size_t i = 0; i < shadow.size(); ++i) {
    CHECK_MESSAGE(c.shadow_next[i] == g.shadow_next[i], "shadow ray " << i);
    occluded += g.shadow_next[i] == uint8_t(Stage::SHADOW_TERMINATED);
  }
  CHECK(occluded == 8u);  // x = -0.4375 + 0.125 i inside (0, 1) for i = 4..11, never on an edge
  CHECK(g.shadow_next[16] == uint8_t(Stage::SHADE_SHADOW));
  CHECK(g.shadow_next[17] == uint8_t(Stage::SHADE_SHADOW));

  detach_cuda_traversal(*session);
  CHECK(session_ledger.total() == 0u);  // nothing of the pipeline outlives the detach
}

TEST_CASE("cuda_session_trace/unrouted stages throw naming the stage") {
  Rig gpu(true, "", false);
  const auto set = acquire_scene_tables(*gpu.scene);
  Ledger ledger;
  auto session = make_cuda_device(ledger, 0);
  DeviceBuffer kg_buf = session->allocate(LedgerCategory::TABLES, sizeof(ar::KernelGlobals), Owner{0, 1});
  const ar::KernelGlobals kg = build_kernel_globals(set.get(), KgSession{});
  session->upload(kg_buf, 0, &kg, sizeof kg);
  session->set_kernel_globals(kg_buf);
  const auto message = [&](Stage s) -> std::string {
    try {
      session->launch(s, 1);
    } catch (const std::exception& e) {
      return e.what();
    }
    return "no throw";
  };
  // Not attached: the traversal stages never look for a k_ module.
  CHECK(message(Stage::INTERSECT_CLOSEST).find("intersect_closest") != std::string::npos);
  CHECK(message(Stage::INTERSECT_SHADOW).find("intersect_shadow") != std::string::npos);
  CHECK(message(Stage::INTERSECT_SUBSURFACE).find("intersect_subsurface") != std::string::npos);
  CHECK(message(Stage::SHADE_VOLUME).find("shade_volume") != std::string::npos);  // no module
  // Attached but no traversable set for the pass.
  attach_cuda_traversal(*session, gpu.scene->accel_builder(), Owner{0, 1});
  CHECK(message(Stage::INTERSECT_CLOSEST).find("traversable") != std::string::npos);
  CHECK_THROWS_AS(attach_cuda_traversal(*session, gpu.scene->accel_builder(), Owner{0, 1}), std::logic_error);
  // A CPU scene has no OptiX context to build on.
  Rig cpu(false, "", false);
  Ledger other_ledger;
  auto other = make_cuda_device(other_ledger, 0);
  CHECK_THROWS_AS(attach_cuda_traversal(*other, cpu.scene->accel_builder(), Owner{0, 1}), std::invalid_argument);
  detach_cuda_traversal(*session);
  session->free(kg_buf);
  CHECK(ledger.total() == 0u);
}

TEST_CASE("cuda_session_trace/cache dir receives the optix cache") {
  const std::filesystem::path dir = fresh_dir("astroray_cuda_session_cache_on");
  {
    Rig gpu(true, dir.string(), false);
    CHECK(gpu.optix().cache_enabled());
    Ledger ledger;
    auto session = make_cuda_device(ledger, 0);
    attach_cuda_traversal(*session, gpu.scene->accel_builder(), Owner{0, 1});
    detach_cuda_traversal(*session);
  }
  CHECK(!listing(dir.string()).empty());
  std::error_code ec;  // the driver may still hold the cache database: best effort
  std::filesystem::remove_all(dir, ec);
}

TEST_CASE("cuda_session_trace/no cache dir disables the optix cache") {
  Rig gpu(true, "", false);
  CHECK(!gpu.optix().cache_enabled());
  // The location OptiX would use by default: nothing may be written there.
  const std::string where = gpu.optix().cache_location();
  const auto before = listing(where);
  Ledger ledger;
  auto session = make_cuda_device(ledger, 0);
  const TraversalAttach t = attach_cuda_traversal(*session, gpu.scene->accel_builder(), Owner{0, 1});
  std::printf("cuda_session_trace: uncached pipeline creation module %.1f ms, pipeline %.1f ms (location %s)\n",
              t.module_ms, t.pipeline_ms, where.c_str());
  detach_cuda_traversal(*session);
  CHECK(listing(where) == before);
}

}  // TEST_SUITE
