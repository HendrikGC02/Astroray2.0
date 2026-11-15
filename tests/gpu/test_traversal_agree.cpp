// SPDX-License-Identifier: Apache-2.0
// Embree against OptiX at scale: 10^6 fixed host-generated rays per scene through both
// closest-hit stages (10% with a finite tmax, also through the shadow stages), counting
// hit/miss mismatches and measuring how far each lies from a triangle edge. A watertight
// traversal and a robust one still own a hit exactly on an edge differently, so edge cases are
// expected and bounded, never ignored. Also the instance-mask fixture on both backends.
// Ref: docs/decisions/0002-kernel-architecture.md D11 (verification row: traversal agreement,
// mask fixture), D12; Woop, Benthin, Wald 2013, "Watertight Ray/Triangle Intersection", JCGT.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <random>
#include <vector>

#include "common/procedural_meshes.h"
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
#include "stages/route_hit.h"

using namespace astroray;
using namespace astroray::host;
using ar::Stage;

namespace {

using Records = std::vector<std::shared_ptr<MeshRecord>>;

constexpr uint32_t kRays = 1000000;
constexpr float kEdgeTol = 1e-4f;  // 0002 D11 verification row: barycentric distance to an edge
constexpr uint8_t kCamera = 0x01;

struct Rays {
  std::vector<uint8_t> type;
  std::vector<ar::Real3> o, d;
  std::vector<ar::Real> tmax;
  std::vector<uint32_t> shadow_slots;  // which rays are also traced through the shadow stage
};

struct Result {
  std::vector<uint32_t> inst, prim;
  std::vector<ar::Real> t;
  std::vector<ar::Real2> uv;
  std::vector<uint8_t> shadow_next;  // one per entry of Rays::shadow_slots
  bool hit(size_t i) const { return inst[i] != ar::kNoInstance; }
};

// The pool arrays of one run, plus the SoA wiring shared by both backends.
struct Pools {
  std::vector<uint8_t> next, s_next;
  std::vector<uint32_t> flags, main_queue, shadow_queue;
  std::vector<ar::Real3> s_o, s_d;
  std::vector<ar::Real> s_tmax;
  ar::StepTable step{};
  Result res;
};

Pools make_pools(const Rays& r) {
  Pools p;
  const uint32_t n = uint32_t(r.o.size()), m = uint32_t(r.shadow_slots.size());
  p.next.assign(n, 0xEE), p.flags.assign(n, 0);
  p.res.inst.assign(n, 7), p.res.prim.assign(n, 7), p.res.t.assign(n, -1.f), p.res.uv.assign(n, {-1, -1});
  p.main_queue.resize(n);
  for (uint32_t i = 0; i < n; ++i) p.main_queue[i] = i;
  for (uint32_t j = 0; j < m; ++j) {
    const uint32_t s = r.shadow_slots[j];
    p.s_o.push_back(r.o[s]), p.s_d.push_back(r.d[s]), p.s_tmax.push_back(r.tmax[s]);
    p.shadow_queue.push_back(j);
  }
  p.s_next.assign(m, 0xEE);
  p.step.stage[static_cast<uint32_t>(Stage::INTERSECT_CLOSEST)] = {0, n, 0};
  p.step.stage[static_cast<uint32_t>(Stage::INTERSECT_SHADOW)] = {0, m, 0};
  return p;
}

InstanceTable one_table(const std::vector<std::pair<ar_key, ar_xform>>& placed, const std::vector<uint8_t>& masks) {
  InstanceTable table;
  table.motion_steps = 1;
  for (size_t i = 0; i < placed.size(); ++i) {
    Instance in;
    in.geom_key = placed[i].first;
    in.mask = masks.empty() ? uint8_t(0xFF) : masks[i];
    table.instances.push_back(in);
    table.xf.push_back(placed[i].second);
  }
  return table;
}

void run_cpu(const Records& recs, const InstanceTable& table, const Rays& rays, Pools& p) {
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool(ctx, 8);
  CpuDevice dev(ledger, pool);
  MeshStore meshes;
  for (const auto& r : recs) meshes.install(r);
  EmbreeDevice embree;
  EmbreeScene accel(embree, meshes, table, std::vector<ar::LightRecord>{}, ledger, pool);
  std::vector<ar::DeviceInstanceRecord> records(table.instances.size());
  for (auto& r : records) r = {};
  ar::SceneData scene{};
  scene.instances = records.data(), scene.instance_count = uint32_t(records.size());
  scene.accel = &accel;
  dev.register_stage(Stage::INTERSECT_CLOSEST, &cpu_stage_intersect_closest);
  dev.register_stage(Stage::INTERSECT_SHADOW, &cpu_stage_intersect_shadow);
  ar::PathStateSoA path{};
  ar::ShadowItemSoA shadow{};
  std::vector<uint8_t> type = rays.type;
  std::vector<ar::Real3> o = rays.o, d = rays.d;
  std::vector<ar::Real> tmax = rays.tmax;
  path.next_stage = p.next.data(), path.ray_type = type.data(), path.flags = p.flags.data();
  path.hit_prim = p.res.prim.data(), path.hit_inst = p.res.inst.data(), path.hit_t = p.res.t.data();
  path.ray_tmax = tmax.data(), path.hit_uv = p.res.uv.data(), path.ray_o = o.data(), path.ray_d = d.data();
  shadow.next_stage = p.s_next.data(), shadow.ray_o = p.s_o.data(), shadow.ray_d = p.s_d.data(), shadow.tmax = p.s_tmax.data();
  ar::KernelGlobals kg{};
  kg.path = &path, kg.shadow = &shadow, kg.step_table = &p.step, kg.scene = &scene;
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
  DeviceBuffer b = dev.allocate(cat, sizeof(T) * std::max<size_t>(v.size(), 1), Owner{1, 0});
  if (!v.empty()) dev.upload(b, 0, v.data(), sizeof(T) * v.size());
  return b;
}
template <class T>
void get(Device& dev, std::vector<T>& v, const DeviceBuffer& b) {
  if (!v.empty()) dev.download(v.data(), b, 0, sizeof(T) * v.size());
}

struct Gpu {
  Ledger ledger;
  std::unique_ptr<ModuleDevice> dev;
  std::unique_ptr<OptixContext> optix;
  std::unique_ptr<SceneUpload> upload;
  std::unique_ptr<OptixAccel> accel;
  std::unique_ptr<TraversalPipeline> pipeline;
  std::filesystem::path cache;

  explicit Gpu(const std::string& cache_name) {
    cache = std::filesystem::temp_directory_path() / cache_name;
    dev = make_cuda_device(ledger, 0);
    optix = std::make_unique<OptixContext>(*dev, 0);
    upload = std::make_unique<SceneUpload>(*dev, Owner{1, 0});
    accel = std::make_unique<OptixAccel>(*optix, Owner{1, 0});
    optix->set_cache_dir(cache.string());
    pipeline = std::make_unique<TraversalPipeline>(*optix, *dev, optix->stream(), Owner{1, 0});
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

void run_gpu(Gpu& g, const Records& recs, const InstanceTable& table, const Rays& rays, Pools& p) {
  for (const auto& r : recs) g.upload->sync_mesh(r);
  g.upload->upload_instances(table);
  g.accel->update_gas(*g.upload);
  g.accel->build_tlas(*g.upload, table, std::vector<ar::LightRecord>{});
  Device& dev = *g.dev;
  std::vector<DeviceBuffer> owned;
  auto keep = [&](DeviceBuffer b) {
    owned.push_back(b);
    return b;
  };
  const DeviceBuffer b_next = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, p.next)),
                     b_type = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, rays.type)),
                     b_flags = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, p.flags)),
                     b_prim = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, p.res.prim)),
                     b_inst = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, p.res.inst)),
                     b_t = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, p.res.t)),
                     b_uv = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, p.res.uv)),
                     b_tmax = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, rays.tmax)),
                     b_o = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, rays.o)),
                     b_d = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, rays.d)),
                     b_sn = keep(put(dev, LedgerCategory::SHADOW_POOL, p.s_next)),
                     b_so = keep(put(dev, LedgerCategory::SHADOW_POOL, p.s_o)),
                     b_sd = keep(put(dev, LedgerCategory::SHADOW_POOL, p.s_d)),
                     b_st = keep(put(dev, LedgerCategory::SHADOW_POOL, p.s_tmax)),
                     b_mq = keep(put(dev, LedgerCategory::QUEUE_SORT_INDICES, p.main_queue)),
                     b_sq = keep(put(dev, LedgerCategory::QUEUE_SORT_INDICES, p.shadow_queue));
  ar::PathStateSoA path{};
  ar::ShadowItemSoA shadow{};
  path.next_stage = static_cast<uint8_t*>(b_next.ptr), path.ray_type = static_cast<uint8_t*>(b_type.ptr);
  path.flags = static_cast<uint32_t*>(b_flags.ptr), path.hit_prim = static_cast<uint32_t*>(b_prim.ptr);
  path.hit_inst = static_cast<uint32_t*>(b_inst.ptr), path.hit_t = static_cast<ar::Real*>(b_t.ptr);
  path.hit_uv = static_cast<ar::Real2*>(b_uv.ptr), path.ray_tmax = static_cast<ar::Real*>(b_tmax.ptr);
  path.ray_o = static_cast<ar::Real3*>(b_o.ptr), path.ray_d = static_cast<ar::Real3*>(b_d.ptr);
  shadow.next_stage = static_cast<uint8_t*>(b_sn.ptr), shadow.ray_o = static_cast<ar::Real3*>(b_so.ptr);
  shadow.ray_d = static_cast<ar::Real3*>(b_sd.ptr), shadow.tmax = static_cast<ar::Real*>(b_st.ptr);
  const DeviceBuffer path_buf = keep(put(dev, LedgerCategory::TABLES, std::vector<ar::PathStateSoA>{path}));
  const DeviceBuffer shadow_buf = keep(put(dev, LedgerCategory::TABLES, std::vector<ar::ShadowItemSoA>{shadow}));
  const DeviceBuffer step_buf = keep(put(dev, LedgerCategory::TABLES, std::vector<ar::StepTable>{p.step}));
  ar::SceneData scene{};
  scene.instances = static_cast<const ar::DeviceInstanceRecord*>(g.upload->instance_records().ptr);
  scene.instance_count = g.upload->instance_count();
  scene.meshes = static_cast<const ar::DeviceMeshView*>(g.upload->mesh_table().ptr);
  scene.mesh_count = uint32_t(recs.size());
  const DeviceBuffer scene_buf = keep(put(dev, LedgerCategory::TABLES, std::vector<ar::SceneData>{scene}));
  ar::KernelGlobals kg{};
  kg.path = static_cast<const ar::PathStateSoA*>(path_buf.ptr), kg.shadow = static_cast<const ar::ShadowItemSoA*>(shadow_buf.ptr);
  kg.step_table = static_cast<const ar::StepTable*>(step_buf.ptr);
  kg.main_queue = static_cast<const uint32_t*>(b_mq.ptr), kg.shadow_queue = static_cast<const uint32_t*>(b_sq.ptr);
  kg.scene = static_cast<const ar::SceneData*>(scene_buf.ptr);
  const DeviceBuffer kg_buf = keep(put(dev, LedgerCategory::TABLES, std::vector<ar::KernelGlobals>{kg}));

  g.pipeline->launch_closest(g.accel->tlas_handle(), kg_buf, 1u << 18);
  g.pipeline->launch_shadow(g.accel->tlas_handle(), kg_buf, 1u << 18);
  g.optix->sync();
  get(dev, p.next, b_next), get(dev, p.res.prim, b_prim), get(dev, p.res.inst, b_inst), get(dev, p.res.t, b_t);
  get(dev, p.res.uv, b_uv), get(dev, p.s_next, b_sn);
  for (DeviceBuffer& b : owned) dev.free(b);
}

// Rays from points on a box around the scene toward random points inside it; every tenth has a
// finite tmax (0.5 to 1.5 of the distance to its target) like a shadow ray.
Rays make_rays(const float lo[3], const float hi[3], uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> u(0.f, 1.f);
  float c[3], h[3];
  for (int a = 0; a < 3; ++a) c[a] = 0.5f * (lo[a] + hi[a]), h[a] = 0.5f * (hi[a] - lo[a]);
  Rays r;
  r.type.assign(kRays, kCamera);
  for (uint32_t i = 0; i < kRays; ++i) {
    float p[3];
    for (int a = 0; a < 3; ++a) p[a] = c[a] + h[a] * (2.f * u(rng) - 1.f) * 1.5f;
    const int face = int(u(rng) * 6.f) % 6;  // project onto one face of the 1.5x box
    p[face / 2] = c[face / 2] + (face % 2 ? 1.5f : -1.5f) * h[face / 2];
    float t[3], dd[3], len = 0;
    for (int a = 0; a < 3; ++a) t[a] = lo[a] + (hi[a] - lo[a]) * u(rng), dd[a] = t[a] - p[a], len += dd[a] * dd[a];
    len = std::sqrt(len);
    r.o.push_back({p[0], p[1], p[2]}), r.d.push_back({dd[0] / len, dd[1] / len, dd[2] / len});
    if (i % 10 == 9) {
      r.tmax.push_back(len * (0.5f + u(rng)));
      r.shadow_slots.push_back(i);
    } else {
      r.tmax.push_back(1e5f);
    }
  }
  return r;
}

// Smallest barycentric coordinate of a hit: the distance, in barycentric units, to an edge.
float edge_distance(const Result& r, size_t i) {
  const float u = r.uv[i].x, v = r.uv[i].y;
  return std::min({u, v, 1.0f - u - v});
}

// Cosine of the angle between the ray and the normal of the triangle the hit names, in double.
double grazing_cos(const Records& recs, const InstanceTable& table, const Rays& rays, const Result& r, size_t i) {
  const MeshRecord& rec = *recs[table.instances[r.inst[i]].geom_key - 1];
  const ar_xform& x = table.xf[r.inst[i]];
  const uint32_t* cv = rec.topo->corner_vert.data() + 3 * size_t(r.prim[i]);
  double P[3][3];
  for (int k = 0; k < 3; ++k)
    for (int a = 0; a < 3; ++a) {
      const float* v = &rec.positions[3 * size_t(cv[k])];
      P[k][a] = double(x.m[4 * a]) * v[0] + double(x.m[4 * a + 1]) * v[1] + double(x.m[4 * a + 2]) * v[2] + x.m[4 * a + 3];
    }
  double e1[3], e2[3];
  for (int a = 0; a < 3; ++a) e1[a] = P[1][a] - P[0][a], e2[a] = P[2][a] - P[0][a];
  const double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
  const double nn = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  return (n[0] * rays.d[i].x + n[1] * rays.d[i].y + n[2] * rays.d[i].z) / nn;
}

void compare_scene(const char* name, const Records& recs, const InstanceTable& table, const float lo[3], const float hi[3]) {
  const Rays rays = make_rays(lo, hi, 0xA57u);
  Pools cpu = make_pools(rays), gpu = make_pools(rays);
  run_cpu(recs, table, rays, cpu);
  Gpu g(std::string("astroray_traverse_agree_") + name);
  run_gpu(g, recs, table, rays, gpu);

  const Result &c = cpu.res, &o = gpu.res;
  uint32_t hits = 0, hit_miss = 0, prim_diff = 0, t_bad = 0, grazing = 0, shadow_diff = 0, shadow_occ = 0;
  float worst_edge = 0;  // the largest edge distance over every hit/miss and primitive mismatch
  uint32_t off_edge = 0;
  for (uint32_t i = 0; i < kRays; ++i) {
    if (c.hit(i) != o.hit(i)) {
      ++hit_miss;
      const float d = c.hit(i) ? edge_distance(c, i) : edge_distance(o, i);
      worst_edge = std::max(worst_edge, d);
      if (d > kEdgeTol) {
        if (off_edge++ < 10) MESSAGE(name << " ray " << i << ": hit/miss mismatch " << d << " from an edge");
      }
    } else if (c.hit(i)) {
      ++hits;
      if (c.inst[i] != o.inst[i] || c.prim[i] != o.prim[i]) {
        ++prim_diff;
        const float d = std::min(edge_distance(c, i), edge_distance(o, i));
        worst_edge = std::max(worst_edge, d);
        if (d > kEdgeTol) {
          if (off_edge++ < 10) MESSAGE(name << " ray " << i << ": different primitive " << d << " from an edge");
        }
      } else if (std::fabs(c.t[i] - o.t[i]) > 1e-5f * std::fabs(c.t[i])) {
        // A grazing ray amplifies the float rounding of the plane intersection by 1/|cos| (both
        // backends are float); the tolerance scales with it below 0.01, and the exact double
        // value is printed so a real error is distinguishable.
        const double cosi = std::fabs(grazing_cos(recs, table, rays, c, i));
        if (cosi < 0.01) ++grazing;
        const double tol = 1e-5 * std::fabs(c.t[i]) * std::max(1.0, 1e-2 / cosi);
        if (std::fabs(c.t[i] - o.t[i]) > tol) {
          if (t_bad++ < 10) MESSAGE(name << " ray " << i << ": t " << o.t[i] << " vs " << c.t[i] << ", cos " << cosi);
        }
      }
    }
  }
  for (size_t j = 0; j < cpu.s_next.size(); ++j) {
    shadow_occ += cpu.s_next[j] == uint8_t(Stage::SHADOW_TERMINATED);
    if (cpu.s_next[j] == gpu.s_next[j]) continue;
    ++shadow_diff;
    // The same ray's closest-hit result, from whichever backend hit, gives the edge distance.
    const uint32_t s = rays.shadow_slots[j];
    float d = 1e30f;
    if (c.hit(s)) d = std::min(d, edge_distance(c, s));
    if (o.hit(s)) d = std::min(d, edge_distance(o, s));
    worst_edge = std::max(worst_edge, d);
    if (d > kEdgeTol) {
      if (off_edge++ < 10) MESSAGE(name << " shadow " << j << ": mismatch " << d << " from an edge");
    }
  }
  std::printf("traversal_agree %s: %u rays, %u both hit, %u hit/miss mismatches, %u primitive differences, %u shadow "
              "mismatches of %zu (%u occluded), largest edge distance %.3g, %u grazing hits with t off by more than 1e-5\n",
              name, kRays, hits, hit_miss, prim_diff, shadow_diff, cpu.s_next.size(), shadow_occ, worst_edge, grazing);
  CHECK(hits > kRays / 10);
  CHECK(hit_miss <= kRays / 10000);
  CHECK(shadow_diff <= cpu.s_next.size() / 10000);
  CHECK(prim_diff <= kRays / 10000);
  CHECK(off_edge == 0);  // every mismatch lies within kEdgeTol of an edge
  CHECK(t_bad == 0);
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("traversal_agree/grid mismatches at edges only") {
  Records recs{test::displaced_grid(1, 1000, 500, 0x51u)};
  const InstanceTable table = one_table({{1, test::make_xform(1, 1, 1, 0, 0, 1, 0, 0, 0, 0)}}, {});
  const float lo[3] = {0, 0, -7}, hi[3] = {1000, 500, 7};
  compare_scene("grid", recs, table, lo, hi);
}

TEST_CASE("traversal_agree/instances mismatches at edges only") {
  Records recs{test::uv_sphere(1, 48, 24), test::cube(2)};
  std::vector<std::pair<ar_key, ar_xform>> placed;
  for (int i = 0; i < 10; ++i) {
    const float f = float(i);
    placed.push_back({ar_key(1 + i % 2), test::make_xform(0.6f + 0.1f * f, 0.8f + 0.05f * f, 1.0f - 0.04f * f, 1, 2, 3 - 0.2f * f,
                                                         0.3f * f, 2.5f * float(i % 5) - 5.f, 2.5f * float(i / 5) - 1.f, 0.4f * f - 2.f)});
  }
  const InstanceTable table = one_table(placed, {});
  const float lo[3] = {-8, -4, -5}, hi[3] = {8, 5, 5};
  compare_scene("instances", recs, table, lo, hi);
}

TEST_CASE("traversal_agree/mask fixture") {
  // One quad instance per mask bit 0-5 plus one carrying bits 1, 3 and 5; a ray of each type
  // straight down every column must hit exactly the instances that share its bit. Bit 4 is the
  // shadow bit, tested through the shadow stage as well.
  auto quad = std::make_shared<MeshRecord>();
  {
    auto t = std::make_shared<MeshTopology>();
    t->id = 1, t->key = 1, t->vertex_count = 4, t->corner_count = 6, t->face_count = 2, t->uv_layer_count = 1;
    t->motion_steps = 1;
    t->corner_vert = {0, 1, 3, 0, 3, 2}, t->tri_corners = {0, 1, 2, 3, 4, 5}, t->tri_face = {0, 1}, t->tri_material = {0, 0};
    t->face_start = {0, 3, 6};
    t->uv.assign(12, 0.f);
    quad->key = 1, quad->topo = t;
    quad->positions = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0};
    quad->corner_normals.assign(18, 0.f);
    quad->bounds_max[0] = quad->bounds_max[1] = 1.f;
  }
  Records recs{quad};
  const std::vector<uint8_t> masks = {1 << 0, 1 << 1, 1 << 2, 1 << 3, uint8_t(ar::kShadowMaskBit), 1 << 5, 0x2A};
  std::vector<std::pair<ar_key, ar_xform>> placed;
  for (size_t i = 0; i < masks.size(); ++i) placed.push_back({1, test::make_xform(1, 1, 1, 0, 0, 1, 0, 3.f * float(i), 0, 0)});
  const InstanceTable table = one_table(placed, masks);

  const uint32_t bits[6] = {0, 1, 2, 3, 4, 5};
  Rays rays;
  for (uint32_t b : bits)
    for (size_t i = 0; i < masks.size(); ++i) {
      rays.type.push_back(uint8_t(1u << b)), rays.o.push_back({3.f * float(i) + 0.25f, 0.5f, 2.f}), rays.d.push_back({0, 0, -1.f});
      rays.tmax.push_back(100.f);
    }
  for (size_t i = 0; i < masks.size(); ++i) {  // shadow rays: the first column-count entries of the pool
    rays.shadow_slots.push_back(uint32_t(4 * masks.size() + i));  // slots of type bit 4, same geometry
  }
  Pools cpu = make_pools(rays), gpu = make_pools(rays);
  run_cpu(recs, table, rays, cpu);
  Gpu g("astroray_traverse_mask_cache");
  run_gpu(g, recs, table, rays, gpu);

  for (const Pools* p : {&cpu, &gpu}) {
    INFO((p == &cpu ? "embree" : "optix"));
    uint32_t wrong = 0;
    for (uint32_t b : bits)
      for (size_t i = 0; i < masks.size(); ++i) {
        const size_t s = b * masks.size() + i;
        const bool want = (masks[i] >> b) & 1;
        const bool got = p->res.inst[s] != ar::kNoInstance;
        if (got != want || (got && p->res.inst[s] != i)) {
          ++wrong;
          MESSAGE("bit " << b << " instance " << i << " got " << got);
        }
      }
    CHECK(wrong == 0);
    for (size_t i = 0; i < masks.size(); ++i) {  // shadow rays are occluded exactly by instances with the shadow bit
      const bool want = (masks[i] & ar::kShadowMaskBit) != 0;
      CHECK((p->s_next[i] == uint8_t(Stage::SHADOW_TERMINATED)) == want);
    }
  }
}

}  // TEST_SUITE
