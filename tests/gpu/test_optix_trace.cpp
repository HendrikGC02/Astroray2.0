// SPDX-License-Identifier: Apache-2.0
// The OptiX traversal pipeline against Embree: both raygens run on a hand-filled step table over
// the same instance table (unit quads in the z = 0 plane) and must write the same hit records
// (exact instance and primitive, t within 1e-5 relative) and next stages as the CPU intersect
// stages. Prints module and pipeline creation times, cold (empty cache directory) and warm.
// Ref: docs/decisions/0002-kernel-architecture.md D11 (programs, flags, hit record, mask table);
// OptiX 9.1 Programming Guide (optixLaunch); 0009 B-18 (module creation, warm and cold).
#include <doctest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "device/cpu/embree_scene.h"
#include "device/cuda/optix_accel.h"
#include "device/cuda/optix_context.h"
#include "device/cuda/optix_pipeline.h"
#include "device/cuda/scene_upload.h"
#include "host/device.h"
#include "host/fault.h"
#include "host/scene/instances.h"
#include "host/thread_pool.h"
#include "light/light.h"
#include "stages/route_hit.h"

using namespace astroray;
using namespace astroray::host;
using ar::Stage;

namespace {

constexpr ar_key kQuadKey = 5;
constexpr uint8_t kCamera = 0x01, kDiffuse = 0x02;

std::shared_ptr<MeshRecord> quad_record() {
  auto t = std::make_shared<MeshTopology>();
  t->id = 1, t->key = kQuadKey;
  t->vertex_count = 4, t->corner_count = 6, t->face_count = 2, t->uv_layer_count = 1, t->motion_steps = 1;
  t->corner_vert = {0, 1, 3, 0, 3, 2};
  t->tri_corners = {0, 1, 2, 3, 4, 5};
  t->tri_face = {0, 1};
  t->tri_material = {0, 0};
  t->face_start = {0, 3, 6};
  t->uv.assign(12, 0.f);
  auto rec = std::make_shared<MeshRecord>();
  rec->key = kQuadKey;
  rec->positions = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0};
  rec->corner_normals.assign(18, 0.f);
  rec->topo = t;
  return rec;
}

ar_xform translate(float x) {
  ar_xform t{};
  t.m[0] = t.m[5] = t.m[10] = 1.0f;
  t.m[3] = x;
  return t;
}

// The lamp of instance 1: a 1 x 1 rectangle where the quad would be, facing down. Its two triangles
// split along the same diagonal as the quad's, so the hand-typed rays below hit the same ones.
std::vector<ar::LightRecord> make_lights() {
  return {ar::light_make_area(ar::LightType::AREA_RECT, ar::Real3{3.5f, 0.5f, 0.0f}, ar::Real3{1, 0, 0}, ar::Real3{0, 1, 0},
                              1.0f, 1.0f, 1.0f, ar::SpectrumRef{})};
}

// Instances at x = 0, 3, 6: 0 plain, 1 a lamp (the geometry key is unused for a lamp), 2 camera
// rays only (no shadow bit).
InstanceTable make_table() {
  InstanceTable table;
  table.motion_steps = 1;
  const uint8_t masks[3] = {0xFF, visibility_mask(kLampRayVisibility), kCamera};  // a lamp has no shadow bit (0004 D8)
  for (int i = 0; i < 3; ++i) {
    Instance in;
    in.geom_key = kQuadKey;
    in.mask = masks[i];
    if (i == 1) in.light_index = 0;
    table.instances.push_back(in);
    table.xf.push_back(translate(3.0f * float(i)));
  }
  return table;
}

// The host arrays of one run: the main pool (closest segment) and the shadow pool.
struct Pools {
  std::vector<uint8_t> next, ray_type;
  std::vector<uint32_t> flags, hit_prim, hit_inst, main_queue;
  std::vector<ar::Real> hit_t, ray_tmax;
  std::vector<ar::Real2> hit_uv;
  std::vector<ar::Real3> ray_o, ray_d;
  std::vector<uint8_t> s_next;
  std::vector<ar::Real3> s_o, s_d;
  std::vector<ar::Real> s_tmax;
  std::vector<uint32_t> shadow_queue;
  ar::StepTable step{};

  void add_path(float x, float y, uint8_t type, uint32_t f) {
    next.push_back(0xEE), ray_type.push_back(type), flags.push_back(f);
    hit_prim.push_back(7), hit_inst.push_back(7), hit_t.push_back(-1.0f), hit_uv.push_back({-1, -1});
    ray_tmax.push_back(100.0f);
    ray_o.push_back({x, y, 2.0f}), ray_d.push_back({0, 0, -1.0f});
  }
  void add_shadow(float x, float y, float tmax) {
    s_next.push_back(0xEE), s_o.push_back({x, y, 2.0f}), s_d.push_back({0, 0, -1.0f}), s_tmax.push_back(tmax);
  }
};

uint32_t hash(uint32_t x) {
  x ^= x >> 16, x *= 0x7feb352dU, x ^= x >> 15, x *= 0x846ca68bU, x ^= x >> 16;
  return x;
}
float unit(uint32_t h) { return float(h >> 8) * (1.0f / 16777216.0f); }

// A hand-typed block followed by pseudo-random rays over the three quads. Points near the shared
// diagonal are skipped: Embree and OptiX may differ on which triangle owns an edge hit.
Pools make_pools() {
  Pools p;
  p.add_path(0.75f, 0.25f, kCamera, 0);              // inst 0 triangle 0
  p.add_path(3.25f, 0.75f, kCamera, 0);              // inst 1 (lamp) triangle 1
  p.add_path(9.5f, 0.25f, kCamera, 0);               // miss
  p.add_path(0.25f, 0.5f, kDiffuse, ar::PATH_KIND);  // light path hit
  p.add_path(9.5f, 0.25f, kDiffuse, ar::PATH_KIND);  // light path miss
  p.add_path(6.5f, 0.25f, kDiffuse, 0);              // camera-only instance: a diffuse ray misses
  p.add_path(6.5f, 0.25f, kCamera, 0);               // the camera ray hits it
  p.add_path(0.5f, 0.25f, kCamera, 0);               // outside the segment: must stay untouched
  for (uint32_t i = 0; i < 300; ++i) {
    const uint32_t h = hash(i + 1);
    const float lx = unit(hash(h)), ly = unit(hash(h + 1));
    if (std::fabs(lx - ly) < 2e-3f) continue;
    const float x = 3.0f * float(h % 4) + lx;  // quad 3 is empty space: a miss
    const uint8_t type = (h >> 8) % 3 == 0 ? kDiffuse : kCamera;
    p.add_path(x, ly, type, (h >> 12) % 2 ? ar::PATH_KIND : 0u);
    if (h % 5 == 0) p.ray_tmax.back() = 1.5f;  // ends before the surface at t = 2: a bounded miss
  }
  p.ray_tmax[2] = 50.0f, p.ray_tmax[4] = 25.0f;  // a miss reports its own tmax
  const uint32_t n = uint32_t(p.next.size());
  // Segment = queue entries [1, n): slots in reverse after one foreign entry (slot 7).
  p.main_queue.push_back(7);
  for (uint32_t s = n; s-- > 0;)
    if (s != 7) p.main_queue.push_back(s);
  p.step.stage[static_cast<uint32_t>(Stage::INTERSECT_CLOSEST)] = {1, n - 1, 0};

  p.add_shadow(0.5f, 0.25f, 100.0f);  // quad of instance 0 at t = 2
  p.add_shadow(3.5f, 0.25f, 2.5f);    // the lamp rectangle at t = 2, inside tmax; a lamp has no shadow bit
  p.add_shadow(0.5f, 0.25f, 1.5f);    // tmax ends before the surface
  p.add_shadow(9.5f, 0.25f, 100.0f);  // nothing there
  p.add_shadow(6.5f, 0.25f, 100.0f);  // instance 2 has no shadow bit
  for (uint32_t i = 0; i < 200; ++i) {
    const uint32_t h = hash(i + 7777);
    const float lx = unit(hash(h)), ly = unit(hash(h + 1));
    if (std::fabs(lx - ly) < 2e-3f) continue;
    p.add_shadow(3.0f * float(h % 4) + lx, ly, (h >> 8) % 2 ? 1.5f : 2.5f);
  }
  const uint32_t m = uint32_t(p.s_next.size());
  for (uint32_t s = 0; s < m; ++s) p.shadow_queue.push_back(m - 1 - s);  // reversed: the queue, not the slot, orders items
  p.step.stage[static_cast<uint32_t>(Stage::INTERSECT_SHADOW)] = {0, m, 0};
  return p;
}

// The pointers of a Pools instance wired into the SoAs and KernelGlobals.
struct Wired {
  ar::PathStateSoA path{};
  ar::ShadowItemSoA shadow{};
};

void wire(Pools& p, Wired& w) {
  w.path.next_stage = p.next.data(), w.path.ray_type = p.ray_type.data(), w.path.flags = p.flags.data();
  w.path.hit_prim = p.hit_prim.data(), w.path.hit_inst = p.hit_inst.data(), w.path.hit_t = p.hit_t.data();
  w.path.ray_tmax = p.ray_tmax.data(), w.path.hit_uv = p.hit_uv.data(), w.path.ray_o = p.ray_o.data(), w.path.ray_d = p.ray_d.data();
  w.shadow.next_stage = p.s_next.data(), w.shadow.ray_o = p.s_o.data(), w.shadow.ray_d = p.s_d.data();
  w.shadow.tmax = p.s_tmax.data();
}

// Embree: the CPU intersect stages on the same table.
void run_cpu(const InstanceTable& table, Pools& p) {
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool(ctx, 4);
  CpuDevice dev(ledger, pool);
  MeshStore meshes;
  meshes.install(quad_record());
  EmbreeDevice embree;
  EmbreeScene accel(embree, meshes, table, make_lights(), ledger, pool);
  std::vector<ar::DeviceInstanceRecord> records(table.instances.size());
  for (size_t i = 0; i < records.size(); ++i) records[i] = {}, records[i].light_index = table.instances[i].light_index;
  ar::SceneData scene{};
  scene.instances = records.data(), scene.instance_count = uint32_t(records.size());
  scene.accel = &accel;  // no material tables: every material is Common
  dev.register_stage(Stage::INTERSECT_CLOSEST, &cpu_stage_intersect_closest);
  dev.register_stage(Stage::INTERSECT_SHADOW, &cpu_stage_intersect_shadow);
  Wired w;
  wire(p, w);
  ar::KernelGlobals kg{};
  kg.path = &w.path, kg.shadow = &w.shadow, kg.step_table = &p.step, kg.scene = &scene;
  kg.main_queue = p.main_queue.data(), kg.shadow_queue = p.shadow_queue.data();
  DeviceBuffer kg_buf = dev.allocate(LedgerCategory::TABLES, sizeof kg, {1, 1});
  dev.upload(kg_buf, 0, &kg, sizeof kg);
  dev.set_kernel_globals(kg_buf);
  dev.launch(Stage::INTERSECT_CLOSEST, 64);
  dev.launch(Stage::INTERSECT_SHADOW, 64);
  dev.free(kg_buf);
}

template <class T>
DeviceBuffer put(Device& dev, LedgerCategory cat, const std::vector<T>& v) {
  DeviceBuffer b = dev.allocate(cat, sizeof(T) * v.size(), Owner{1, 0});
  dev.upload(b, 0, v.data(), sizeof(T) * v.size());
  return b;
}
template <class T>
void get(Device& dev, std::vector<T>& v, const DeviceBuffer& b) {
  dev.download(v.data(), b, 0, sizeof(T) * v.size());
}

struct Gpu {
  Ledger ledger;
  std::unique_ptr<ModuleDevice> dev;
  std::unique_ptr<OptixContext> optix;
  std::unique_ptr<SceneUpload> upload;
  std::unique_ptr<OptixAccel> accel;
  std::unique_ptr<TraversalPipeline> pipeline;
  std::filesystem::path cache;
  double module_ms = 0, pipeline_ms = 0;

  Gpu(const std::string& cache_name) {
    cache = std::filesystem::temp_directory_path() / cache_name;
    dev = make_cuda_device(ledger, 0);
    optix = std::make_unique<OptixContext>(*dev, 0);
    upload = std::make_unique<SceneUpload>(*dev, Owner{1, 0});
    accel = std::make_unique<OptixAccel>(*optix, Owner{1, 0});
    optix->set_cache_dir(cache.string());
    pipeline = std::make_unique<TraversalPipeline>(*optix, *dev, optix->stream(), Owner{1, 0});
    module_ms = pipeline->times().module_ms, pipeline_ms = pipeline->times().pipeline_ms;
  }
  ~Gpu() {
    pipeline.reset();
    accel.reset();
    upload.reset();
    optix.reset();
    dev.reset();
    std::error_code ec;  // the OptiX cache database may still be held by the driver: best effort
    std::filesystem::remove_all(cache, ec);
  }
};

// Runs both raygens on the GPU over `p` (results written into p).
void run_gpu(Gpu& g, const InstanceTable& table, Pools& p, uint32_t width) {
  g.upload->sync_mesh(quad_record());
  g.upload->upload_instances(table);
  g.accel->update_gas(*g.upload);
  g.accel->build_tlas(*g.upload, table, make_lights());
  Device& dev = *g.dev;

  std::vector<DeviceBuffer> owned;
  auto keep = [&](DeviceBuffer b) {
    owned.push_back(b);
    return b;
  };
  Wired w;
  const DeviceBuffer b_next = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, p.next)),
                     b_type = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, p.ray_type)),
                     b_flags = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, p.flags)),
                     b_prim = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, p.hit_prim)),
                     b_inst = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, p.hit_inst)),
                     b_t = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, p.hit_t)),
                     b_uv = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, p.hit_uv)),
                     b_tmax = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, p.ray_tmax)),
                     b_o = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, p.ray_o)),
                     b_d = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, p.ray_d)),
                     b_sn = keep(put(dev, LedgerCategory::SHADOW_POOL, p.s_next)),
                     b_so = keep(put(dev, LedgerCategory::SHADOW_POOL, p.s_o)),
                     b_sd = keep(put(dev, LedgerCategory::SHADOW_POOL, p.s_d)),
                     b_st = keep(put(dev, LedgerCategory::SHADOW_POOL, p.s_tmax)),
                     b_mq = keep(put(dev, LedgerCategory::QUEUE_SORT_INDICES, p.main_queue)),
                     b_sq = keep(put(dev, LedgerCategory::QUEUE_SORT_INDICES, p.shadow_queue));
  w.path.next_stage = static_cast<uint8_t*>(b_next.ptr), w.path.ray_type = static_cast<uint8_t*>(b_type.ptr);
  w.path.flags = static_cast<uint32_t*>(b_flags.ptr), w.path.hit_prim = static_cast<uint32_t*>(b_prim.ptr);
  w.path.hit_inst = static_cast<uint32_t*>(b_inst.ptr), w.path.hit_t = static_cast<ar::Real*>(b_t.ptr);
  w.path.hit_uv = static_cast<ar::Real2*>(b_uv.ptr), w.path.ray_tmax = static_cast<ar::Real*>(b_tmax.ptr);
  w.path.ray_o = static_cast<ar::Real3*>(b_o.ptr);
  w.path.ray_d = static_cast<ar::Real3*>(b_d.ptr);
  w.shadow.next_stage = static_cast<uint8_t*>(b_sn.ptr), w.shadow.ray_o = static_cast<ar::Real3*>(b_so.ptr);
  w.shadow.ray_d = static_cast<ar::Real3*>(b_sd.ptr), w.shadow.tmax = static_cast<ar::Real*>(b_st.ptr);
  const DeviceBuffer path_buf = keep(put(dev, LedgerCategory::TABLES, std::vector<ar::PathStateSoA>{w.path}));
  const DeviceBuffer shadow_buf = keep(put(dev, LedgerCategory::TABLES, std::vector<ar::ShadowItemSoA>{w.shadow}));
  const DeviceBuffer step_buf = keep(put(dev, LedgerCategory::TABLES, std::vector<ar::StepTable>{p.step}));

  ar::SceneData scene{};  // no material tables: every material is Common
  scene.instances = static_cast<const ar::DeviceInstanceRecord*>(g.upload->instance_records().ptr);
  scene.instance_count = g.upload->instance_count();
  scene.meshes = static_cast<const ar::DeviceMeshView*>(g.upload->mesh_table().ptr);
  scene.mesh_count = 1;
  const DeviceBuffer scene_buf = keep(put(dev, LedgerCategory::TABLES, std::vector<ar::SceneData>{scene}));
  ar::KernelGlobals kg{};
  kg.path = static_cast<const ar::PathStateSoA*>(path_buf.ptr), kg.shadow = static_cast<const ar::ShadowItemSoA*>(shadow_buf.ptr);
  kg.step_table = static_cast<const ar::StepTable*>(step_buf.ptr);
  kg.main_queue = static_cast<const uint32_t*>(b_mq.ptr), kg.shadow_queue = static_cast<const uint32_t*>(b_sq.ptr);
  kg.scene = static_cast<const ar::SceneData*>(scene_buf.ptr);
  const DeviceBuffer kg_buf = keep(put(dev, LedgerCategory::TABLES, std::vector<ar::KernelGlobals>{kg}));

  g.pipeline->launch_closest(g.accel->tlas_handle(), kg_buf, width);
  g.pipeline->launch_shadow(g.accel->tlas_handle(), kg_buf, width);
  g.optix->sync();
  get(dev, p.next, b_next), get(dev, p.hit_prim, b_prim), get(dev, p.hit_inst, b_inst), get(dev, p.hit_t, b_t);
  get(dev, p.hit_uv, b_uv), get(dev, p.s_next, b_sn);
  for (DeviceBuffer& b : owned) dev.free(b);
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("optix_trace/closest matches embree") {
  const InstanceTable table = make_table();
  Pools cpu = make_pools(), gpu = make_pools();
  run_cpu(table, cpu);
  Gpu g("astroray_optix_trace_cache");
  run_gpu(g, table, gpu, 64);  // fewer threads than items: the grid-stride loop covers the rest

  const uint32_t n = uint32_t(cpu.next.size());
  uint32_t hits = 0, misses = 0, wrong = 0;
  for (uint32_t s = 0; s < n; ++s) {
    INFO("slot " << s);
    if (s == 7) {  // outside the segment: untouched on both
      CHECK(gpu.next[s] == 0xEE);
      CHECK(gpu.hit_inst[s] == 7u);
      continue;
    }
    bool ok = gpu.hit_inst[s] == cpu.hit_inst[s] && gpu.next[s] == cpu.next[s];
    if (cpu.hit_inst[s] == ar::kNoInstance) {
      ++misses;
      ok = ok && gpu.hit_t[s] == cpu.hit_t[s] && gpu.hit_prim[s] == cpu.hit_prim[s];  // t = the ray's tmax, prim = ~0u
    } else {
      ++hits;
      ok = ok && gpu.hit_prim[s] == cpu.hit_prim[s];
      ok = ok && std::fabs(gpu.hit_t[s] - cpu.hit_t[s]) <= 1e-5f * std::fabs(cpu.hit_t[s]);
      ok = ok && std::fabs(gpu.hit_uv[s].x - cpu.hit_uv[s].x) <= 1e-5f && std::fabs(gpu.hit_uv[s].y - cpu.hit_uv[s].y) <= 1e-5f;
    }
    if (!ok) {
      ++wrong;
      MESSAGE("slot " << s << ": gpu inst " << gpu.hit_inst[s] << " prim " << gpu.hit_prim[s] << " t " << gpu.hit_t[s] << " next "
                      << int(gpu.next[s]) << "; embree inst " << cpu.hit_inst[s] << " prim " << cpu.hit_prim[s] << " t "
                      << cpu.hit_t[s] << " next " << int(cpu.next[s]));
    }
  }
  CHECK(wrong == 0);
  CHECK(hits > 50);
  CHECK(misses > 20);
  // Spot checks typed from the geometry, so a wrong CPU oracle cannot hide a wrong GPU.
  CHECK(gpu.hit_inst[0] == 0u);
  CHECK(gpu.hit_prim[0] == 0u);
  CHECK(std::fabs(gpu.hit_t[0] - 2.0f) < 1e-6f);
  CHECK(gpu.next[1] == uint8_t(Stage::SHADE_LIGHT));  // the lamp instance
  CHECK(gpu.next[2] == uint8_t(Stage::SHADE_BACKGROUND));
  CHECK(gpu.next[4] == uint8_t(Stage::TERMINATED));
  CHECK(gpu.hit_inst[5] == ar::kNoInstance);  // a diffuse ray misses the camera-only instance
  CHECK(gpu.hit_inst[6] == 2u);
  CHECK(gpu.hit_t[2] == 50.0f);
  CHECK(gpu.hit_t[4] == 25.0f);
}

TEST_CASE("optix_trace/shadow occluded") {
  const InstanceTable table = make_table();
  Pools cpu = make_pools(), gpu = make_pools();
  run_cpu(table, cpu);
  Gpu g("astroray_optix_shadow_cache");
  run_gpu(g, table, gpu, 64);

  uint32_t occluded = 0, delivered = 0, wrong = 0;
  for (size_t s = 0; s < cpu.s_next.size(); ++s) {
    (cpu.s_next[s] == uint8_t(Stage::SHADOW_TERMINATED) ? occluded : delivered)++;
    if (gpu.s_next[s] != cpu.s_next[s]) {
      ++wrong;
      MESSAGE("shadow item " << s << ": gpu " << int(gpu.s_next[s]) << " embree " << int(cpu.s_next[s]));
    }
  }
  CHECK(wrong == 0);
  CHECK(occluded > 20);
  CHECK(delivered > 20);
  CHECK(gpu.s_next[0] == uint8_t(Stage::SHADOW_TERMINATED));  // quad at t = 2 inside tmax 100
  CHECK(gpu.s_next[1] == uint8_t(Stage::SHADE_SHADOW));       // the lamp never blocks
  CHECK(gpu.s_next[2] == uint8_t(Stage::SHADE_SHADOW));       // tmax ends before the surface
  CHECK(gpu.s_next[3] == uint8_t(Stage::SHADE_SHADOW));       // nothing there
  CHECK(gpu.s_next[4] == uint8_t(Stage::SHADE_SHADOW));       // instance 2 lacks the shadow bit
}

TEST_CASE("optix_pipeline/creation times cold and warm") {
  std::error_code ec;
  std::filesystem::remove_all(std::filesystem::temp_directory_path() / "astroray_optix_times_cache", ec);
  Gpu g("astroray_optix_times_cache");  // empty cache directory: a cold compile
  std::printf("optix_pipeline: cold cache  module %.1f ms, pipeline %.1f ms\n", g.module_ms, g.pipeline_ms);
  CHECK(g.module_ms > 0);
  CHECK(!g.pipeline->has_motion_variant());
  g.pipeline->create_motion_variant();
  CHECK(g.pipeline->has_motion_variant());
  std::printf("optix_pipeline: motion variant module %.1f ms, pipeline %.1f ms\n", g.pipeline->motion_times().module_ms,
              g.pipeline->motion_times().pipeline_ms);
  CHECK(g.pipeline->motion_times().module_ms > 0);
  // The same directory again: the disk cache is warm.
  g.pipeline.reset();
  g.pipeline = std::make_unique<TraversalPipeline>(*g.optix, *g.dev, g.optix->stream(), Owner{1, 0});
  std::printf("optix_pipeline: warm cache  module %.1f ms, pipeline %.1f ms\n", g.pipeline->times().module_ms,
              g.pipeline->times().pipeline_ms);
  CHECK(g.pipeline->times().module_ms > 0);
  // An empty cache directory is a caller bug, never a silent default location.
  CHECK_THROWS_AS(g.optix->set_cache_dir(""), std::invalid_argument);
  // The cache location belongs to the context: a different directory on the same context is refused.
  CHECK_THROWS_AS(g.optix->set_cache_dir(g.cache.string() + "_other"), std::invalid_argument);
}

}  // TEST_SUITE
