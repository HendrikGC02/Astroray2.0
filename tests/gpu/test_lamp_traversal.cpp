// SPDX-License-Identifier: Apache-2.0
// Lamp primitives on both devices: Embree and OptiX return the same lamp hits (instance exact, t
// within 1e-5 relative) over a scene holding each lamp type, the points the light sampler picks
// lie on the primitive that a ray toward them hits, and a shadow ray whose only blocker is a lamp
// is never occluded (0004 D8: lamp instances lack the shadow bit).
// Ref: docs/decisions/0002-kernel-architecture.md D11 (lamp programs, mask table), 0004 D8;
// OptiX 9.1 Programming Guide (custom primitives, built-in spheres); Embree 4 API.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "device/cpu/embree_trace.h"
#include "device/cuda/optix_accel.h"
#include "device/cuda/optix_context.h"
#include "device/cuda/optix_pipeline.h"
#include "device/cuda/scene_upload.h"
#include "host/device.h"
#include "host/ledger.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/thread_pool.h"
#include "light/light.h"

#include "stages/route_hit.h"

using namespace astroray;
using namespace astroray::host;
using ar::Stage;

namespace {

constexpr ar_key kQuadKey = 5;

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

// A scene: lamps first (instance i is light i), then optionally the quad mesh.
struct LampRig {
  InstanceTable table;
  std::vector<ar::LightRecord> lights;
  bool quad = false;
  uint32_t quad_instance = 0;
};

// The object transform of a lamp instance is deliberately wild: the primitive must follow the
// light record (068), never this.
ar_xform wild_xform() {
  ar_xform t{};
  t.m[0] = 3.0f, t.m[5] = 0.2f, t.m[10] = 7.0f;
  t.m[3] = 50.0f, t.m[7] = -40.0f, t.m[11] = 30.0f;
  return t;
}

void add_lamp(LampRig& s, const ar::LightRecord& rec) {
  Instance in;
  in.geom_key = 0;
  in.light_index = uint32_t(s.lights.size());
  in.mask = visibility_mask(kLampRayVisibility);
  s.table.instances.push_back(in);
  s.table.xf.push_back(wild_xform());
  s.lights.push_back(rec);
}

void add_quad(LampRig& s, float x, float y) {
  Instance in;
  in.geom_key = kQuadKey;
  in.mask = 0xFF;  // includes the shadow bit
  s.quad = true, s.quad_instance = uint32_t(s.table.instances.size());
  s.table.instances.push_back(in);
  ar_xform t{};
  t.m[0] = t.m[5] = t.m[10] = 1.0f;
  t.m[3] = x, t.m[7] = y;
  s.table.xf.push_back(t);
}

ar::SpectrumRef white() { return ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, 1.0f); }

// Lamps along x at z = 0: a tilted rectangle, a tilted disk, a sphere, a delta point lamp (no primitive).
LampRig lamp_scene() {
  LampRig s;
  const ar::Real3 ax{0.6f, 0.8f, 0.0f}, ay{-0.48f, 0.36f, 0.8f};
  add_lamp(s, ar::light_make_area(ar::LightType::AREA_RECT, ar::Real3{0, 0, 0}, ax, ay, 2.0f, 1.5f, 5.0f, white()));
  add_lamp(s, ar::light_make_area(ar::LightType::AREA_DISK, ar::Real3{4, 0, 0}, ax, ay, 1.2f, 0.0f, 5.0f, white()));
  add_lamp(s, ar::light_make(ar::LightType::SPHERE, ar::Real3{8, 0, 0}, 0.9f, 5.0f, white()));
  add_lamp(s, ar::light_make(ar::LightType::POINT, ar::Real3{12, 0, 0}, 0.0f, 5.0f, white()));
  return s;
}

uint32_t hash(uint32_t x) {
  x ^= x >> 16, x *= 0x7feb352dU, x ^= x >> 15, x *= 0x846ca68bU, x ^= x >> 16;
  return x;
}
float unit(uint32_t h) { return float(h >> 8) * (1.0f / 16777216.0f); }

struct Ray {
  ar::Real3 o, d;
  float tmax;
};

// What one device returned for a batch of closest rays and shadow rays.
struct Answer {
  std::vector<ar::HitRecord> hit;
  std::vector<uint8_t> occluded;
};

Answer run_embree(const LampRig& s, const std::vector<Ray>& closest, const std::vector<Ray>& shadow) {
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool(ctx, 4);
  MeshStore meshes;
  if (s.quad) meshes.install(quad_record());
  EmbreeDevice device;
  EmbreeScene scene(device, meshes, s.table, s.lights, ledger, pool);
  auto batch = [](const std::vector<Ray>& rays, std::vector<std::vector<float>>& c) {
    c.assign(8, {});
    for (const Ray& r : rays) {
      c[0].push_back(r.o.x), c[1].push_back(r.o.y), c[2].push_back(r.o.z);
      c[3].push_back(r.d.x), c[4].push_back(r.d.y), c[5].push_back(r.d.z);
      c[6].push_back(0.0f), c[7].push_back(r.tmax);
    }
    return RayBatch{c[0].data(), c[1].data(), c[2].data(), c[3].data(), c[4].data(), c[5].data(), c[6].data(), c[7].data()};
  };
  Answer a;
  std::vector<std::vector<float>> c1, c2;
  a.hit.resize(closest.size());
  const std::vector<uint8_t> mask(closest.size(), 0x01);  // camera rays
  trace_closest(scene, batch(closest, c1), mask.data(), uint32_t(closest.size()), a.hit.data(), pool);
  a.occluded.resize(shadow.size());
  trace_occluded(scene, batch(shadow, c2), uint32_t(shadow.size()), a.occluded.data(), pool);
  return a;
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

Answer run_optix(const LampRig& s, const std::vector<Ray>& closest, const std::vector<Ray>& shadow) {
  Ledger ledger;
  auto dev_owner = make_cuda_device(ledger, 0);
  Device& dev = *dev_owner;
  const std::filesystem::path cache = std::filesystem::temp_directory_path() / "astroray_lamp_traversal_cache";
  OptixContext optix(*dev_owner, 0);
  SceneUpload upload(*dev_owner, Owner{1, 0});
  OptixAccel accel(optix, Owner{1, 0});
  optix.set_cache_dir(cache.string());
  auto pipeline = std::make_unique<TraversalPipeline>(optix, *dev_owner, optix.stream(), Owner{1, 0});
  if (s.quad) upload.sync_mesh(quad_record());
  accel.update_gas(upload);
  accel.build_tlas(upload, s.table, s.lights);

  // The instance records route_hit reads: only the light index matters for a lamp hit.
  std::vector<ar::DeviceInstanceRecord> records(s.table.instances.size());
  for (size_t i = 0; i < records.size(); ++i) records[i] = {}, records[i].light_index = s.table.instances[i].light_index;

  const uint32_t n = uint32_t(closest.size()), m = uint32_t(shadow.size());
  std::vector<uint8_t> next(n, 0xEE), type(n, 0x01), s_next(m, 0xEE);
  std::vector<uint32_t> flags(n, 0), prim(n, 7), inst(n, 7), mq(n), sq(m);
  std::vector<ar::Real> hit_t(n, -1.0f), tmax(n), s_tmax(m);
  std::vector<ar::Real2> uv(n, ar::Real2{0, 0});
  std::vector<ar::Real3> o(n), d(n), s_o(m), s_d(m);
  for (uint32_t i = 0; i < n; ++i) mq[i] = i, o[i] = closest[i].o, d[i] = closest[i].d, tmax[i] = closest[i].tmax;
  for (uint32_t i = 0; i < m; ++i) sq[i] = i, s_o[i] = shadow[i].o, s_d[i] = shadow[i].d, s_tmax[i] = shadow[i].tmax;
  ar::StepTable step{};
  step.stage[static_cast<uint32_t>(Stage::INTERSECT_CLOSEST)] = {0, n, 0};
  step.stage[static_cast<uint32_t>(Stage::INTERSECT_SHADOW)] = {0, m, 0};

  std::vector<DeviceBuffer> owned;
  auto keep = [&](DeviceBuffer b) {
    owned.push_back(b);
    return b;
  };
  const DeviceBuffer b_next = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, next)),
                     b_type = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, type)),
                     b_flags = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, flags)),
                     b_prim = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, prim)),
                     b_inst = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, inst)),
                     b_t = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, hit_t)),
                     b_uv = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, uv)),
                     b_tmax = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, tmax)),
                     b_o = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, o)),
                     b_d = keep(put(dev, LedgerCategory::MAIN_PATH_POOL, d)),
                     b_sn = keep(put(dev, LedgerCategory::SHADOW_POOL, s_next)),
                     b_so = keep(put(dev, LedgerCategory::SHADOW_POOL, s_o)),
                     b_sd = keep(put(dev, LedgerCategory::SHADOW_POOL, s_d)),
                     b_st = keep(put(dev, LedgerCategory::SHADOW_POOL, s_tmax)),
                     b_mq = keep(put(dev, LedgerCategory::QUEUE_SORT_INDICES, mq)),
                     b_sq = keep(put(dev, LedgerCategory::QUEUE_SORT_INDICES, sq)),
                     b_rec = keep(put(dev, LedgerCategory::TABLES, records));
  ar::PathStateSoA path{};
  ar::ShadowItemSoA shadow_soa{};
  path.next_stage = static_cast<uint8_t*>(b_next.ptr), path.ray_type = static_cast<uint8_t*>(b_type.ptr);
  path.flags = static_cast<uint32_t*>(b_flags.ptr), path.hit_prim = static_cast<uint32_t*>(b_prim.ptr);
  path.hit_inst = static_cast<uint32_t*>(b_inst.ptr), path.hit_t = static_cast<ar::Real*>(b_t.ptr);
  path.hit_uv = static_cast<ar::Real2*>(b_uv.ptr), path.ray_tmax = static_cast<ar::Real*>(b_tmax.ptr);
  path.ray_o = static_cast<ar::Real3*>(b_o.ptr), path.ray_d = static_cast<ar::Real3*>(b_d.ptr);
  shadow_soa.next_stage = static_cast<uint8_t*>(b_sn.ptr), shadow_soa.ray_o = static_cast<ar::Real3*>(b_so.ptr);
  shadow_soa.ray_d = static_cast<ar::Real3*>(b_sd.ptr), shadow_soa.tmax = static_cast<ar::Real*>(b_st.ptr);
  const DeviceBuffer path_buf = keep(put(dev, LedgerCategory::TABLES, std::vector<ar::PathStateSoA>{path}));
  const DeviceBuffer shadow_buf = keep(put(dev, LedgerCategory::TABLES, std::vector<ar::ShadowItemSoA>{shadow_soa}));
  const DeviceBuffer step_buf = keep(put(dev, LedgerCategory::TABLES, std::vector<ar::StepTable>{step}));
  ar::SceneData scene{};  // no meshes or materials: only lamp hits are routed
  scene.instances = static_cast<const ar::DeviceInstanceRecord*>(b_rec.ptr);
  scene.instance_count = uint32_t(records.size());
  const DeviceBuffer scene_buf = keep(put(dev, LedgerCategory::TABLES, std::vector<ar::SceneData>{scene}));
  ar::KernelGlobals kg{};
  kg.path = static_cast<const ar::PathStateSoA*>(path_buf.ptr), kg.shadow = static_cast<const ar::ShadowItemSoA*>(shadow_buf.ptr);
  kg.step_table = static_cast<const ar::StepTable*>(step_buf.ptr);
  kg.main_queue = static_cast<const uint32_t*>(b_mq.ptr), kg.shadow_queue = static_cast<const uint32_t*>(b_sq.ptr);
  kg.scene = static_cast<const ar::SceneData*>(scene_buf.ptr);
  const DeviceBuffer kg_buf = keep(put(dev, LedgerCategory::TABLES, std::vector<ar::KernelGlobals>{kg}));

  if (n) pipeline->launch_closest(accel.tlas_handle(), kg_buf, 1024);
  if (m) pipeline->launch_shadow(accel.tlas_handle(), kg_buf, 1024);
  optix.sync();
  get(dev, hit_t, b_t), get(dev, prim, b_prim), get(dev, inst, b_inst), get(dev, uv, b_uv), get(dev, s_next, b_sn);
  for (DeviceBuffer& b : owned) dev.free(b);

  Answer a;
  for (uint32_t i = 0; i < n; ++i) a.hit.push_back(ar::HitRecord{hit_t[i], prim[i], inst[i], uv[i].x, uv[i].y});
  for (uint32_t i = 0; i < m; ++i) a.occluded.push_back(s_next[i] == uint8_t(Stage::SHADOW_TERMINATED) ? 1 : 0);
  pipeline.reset();
  std::error_code ec;  // the OptiX cache database may still be held by the driver: best effort
  std::filesystem::remove_all(cache, ec);
  return a;
}

struct Obs {
  float cmf[3 * ar::kObsSamples];
  float illum[ar::kObsSamples];
  ar::ObserverView view;
  Obs() {
    for (float& c : cmf) c = 0.0f;
    for (int i = 0; i < ar::kObsSamples; ++i) illum[i] = 1.0f;
    view.cmf = cmf;
    view.illum = illum;
  }
};

void check_same(const Answer& e, const Answer& g, const char* what, uint32_t expect_hits_min) {
  REQUIRE(e.hit.size() == g.hit.size());
  uint32_t wrong = 0, hits = 0;
  for (size_t i = 0; i < e.hit.size(); ++i) {
    bool ok = e.hit[i].inst == g.hit[i].inst;
    if (e.hit[i].inst != ar::kNoInstance) {
      ++hits;
      ok = ok && std::fabs(e.hit[i].t - g.hit[i].t) <= 1e-5f * std::fabs(e.hit[i].t);
      // Primitive and (u, v) too, except within 1e-4 of the rectangle's diagonal where the two devices
      // may own the edge hit differently (instance 0 is the rectangle; its diagonal is u = 0 on
      // triangle 0 and v = 0 on triangle 1). Disk and sphere hits have u = v = 0 on both.
      const bool diagonal = e.hit[i].inst == 0u && (e.hit[i].prim == 0u ? e.hit[i].u : e.hit[i].v) < 1e-4f;
      if (!diagonal)
        ok = ok && e.hit[i].prim == g.hit[i].prim && std::fabs(e.hit[i].u - g.hit[i].u) <= 1e-5f &&
             std::fabs(e.hit[i].v - g.hit[i].v) <= 1e-5f;
    }
    if (!ok && ++wrong <= 5)
      MESSAGE(what << " ray " << i << ": embree inst " << e.hit[i].inst << " t " << e.hit[i].t << "; optix inst " << g.hit[i].inst
                   << " t " << g.hit[i].t);
  }
  CHECK(wrong == 0);
  CHECK(hits >= expect_hits_min);
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("lamp_traversal/embree and optix agree") {
  const LampRig s = lamp_scene();
  std::vector<Ray> rays;
  for (uint32_t i = 0; i < 100000; ++i) {
    const uint32_t h = hash(i + 1);
    const ar::Real3 from{-3.0f + 18.0f * unit(hash(h)), -3.0f + 6.0f * unit(hash(h + 1)), -6.0f + 3.0f * unit(hash(h + 2))};
    const ar::Real3 to{-3.0f + 18.0f * unit(hash(h + 3)), -2.0f + 4.0f * unit(hash(h + 4)), -1.0f + 2.0f * unit(hash(h + 5))};
    const ar::Real3 d = ar::normalize(to - from);
    rays.push_back({from, d, 100.0f});
  }
  const Answer e = run_embree(s, rays, {}), g = run_optix(s, rays, {});
  check_same(e, g, "closest", 5000);
  // Each lamp type is hit, the delta point lamp (instance 3) never.
  uint32_t per[4] = {0, 0, 0, 0};
  for (const auto& h : g.hit)
    if (h.inst < 4) ++per[h.inst];
  CHECK(per[0] > 500);
  CHECK(per[1] > 500);
  CHECK(per[2] > 500);
  CHECK(per[3] == 0);
}

TEST_CASE("lamp_traversal/sampled points lie on the primitive") {
  const LampRig s = lamp_scene();
  Obs obs;
  ar::Lambda lam;
  for (int i = 0; i < 4; ++i) lam.nm[i] = ar::Real(450.0f + 70.0f * float(i));
  std::vector<Ray> rays;
  std::vector<uint32_t> owner;
  for (uint32_t li = 0; li < 3; ++li) {  // rectangle, disk, sphere; the delta lamp has no surface
    const ar::LightRecord& r = s.lights[li];
    // In front of the lamp (-Z of its frame for the area lamps), off axis.
    const ar::Real3 centre = ar::light_position(r);
    const ar::Real3 front = li < 2 ? ar::area_front(r) : ar::Real3{0.0f, 0.0f, -1.0f};
    const ar::Real3 p = centre + front * 4.0f + ar::Real3{0.3f, -0.2f, 0.1f};
    const ar::LightSampleContext ctx = ar::light_context_point(p, front * -1.0f, front * -1.0f);
    for (int a = 0; a < 16; ++a)
      for (int b = 0; b < 16; ++b) {
        const ar::LightLiSample ls =
            ar::light_sample_li(r, ctx, ar::Real2{(a + 0.5f) / 16.0f, (b + 0.5f) / 16.0f}, lam, obs.view);
        REQUIRE(ls.pdf > 0.0f);
        rays.push_back({p, ls.p_light - p, 2.0f});  // unnormalised: t = 1 at the sampled point
        owner.push_back(li);
      }
  }
  const Answer e = run_embree(s, rays, {}), g = run_optix(s, rays, {});
  for (size_t i = 0; i < rays.size(); ++i) {
    INFO("sample " << i << " of lamp " << owner[i]);
    CHECK(e.hit[i].inst == owner[i]);
    CHECK(g.hit[i].inst == owner[i]);
    CHECK(std::fabs(e.hit[i].t - 1.0f) <= 1e-5f);
    CHECK(std::fabs(g.hit[i].t - 1.0f) <= 1e-5f);
  }
}

TEST_CASE("lamp_traversal/lamps never block shadows") {
  LampRig s = lamp_scene();
  add_quad(s, 20.0f, 0.0f);  // the control: a mesh with the shadow bit, x in [20, 21], y in [0, 1]
  std::vector<Ray> shadow;
  // Straight down -z from z = 2 through the lamps' centres, then through the quad.
  const ar::Real3 down{0, 0, -1};
  for (float x : {0.0f, 4.0f, 8.0f}) shadow.push_back({ar::Real3{x, 0.0f, 2.0f}, down, 100.0f});
  shadow.push_back({ar::Real3{20.5f, 0.5f, 2.0f}, down, 100.0f});
  for (uint32_t i = 0; i < 4000; ++i) {
    const uint32_t h = hash(i + 99);
    shadow.push_back({ar::Real3{-2.0f + 24.0f * unit(h), -2.0f + 4.0f * unit(hash(h)), 2.0f}, down, 100.0f});
  }
  const Answer e = run_embree(s, {}, shadow), g = run_optix(s, {}, shadow);
  uint32_t blocked = 0;
  for (size_t i = 0; i < shadow.size(); ++i) {
    const ar::Real3 o = shadow[i].o;
    // Occluded exactly when the ray goes through the quad (away from its diagonal); lamps never count.
    const bool in_quad = o.x >= 20.0f && o.x <= 21.0f && o.y >= 0.0f && o.y <= 1.0f;
    const bool near_edge = std::fabs(o.x - 20.0f) < 1e-3f || std::fabs(o.x - 21.0f) < 1e-3f || std::fabs(o.y) < 1e-3f ||
                           std::fabs(o.y - 1.0f) < 1e-3f || std::fabs((o.x - 20.0f) - o.y) < 1e-3f;
    if (near_edge) continue;
    INFO("shadow ray " << i << " at " << o.x << ", " << o.y);
    CHECK(int(e.occluded[i]) == int(in_quad));
    CHECK(int(g.occluded[i]) == int(in_quad));
    blocked += in_quad;
  }
  CHECK(blocked > 5);
  CHECK(e.occluded[0] == 0);  // through the rectangle lamp
  CHECK(g.occluded[0] == 0);
  CHECK(e.occluded[1] == 0);  // the disk
  CHECK(g.occluded[1] == 0);
  CHECK(e.occluded[2] == 0);  // the sphere
  CHECK(g.occluded[2] == 0);
  CHECK(e.occluded[3] == 1);  // the control
  CHECK(g.occluded[3] == 1);
}

TEST_CASE("lamp_traversal/invalid lamp instances throw") {
  using doctest::Contains;
  Ledger ledger;
  auto dev = make_cuda_device(ledger, 0);
  OptixContext optix(*dev, 0);
  SceneUpload upload(*dev, Owner{1, 0});
  OptixAccel accel(optix, Owner{1, 0});
  auto build = [&](uint32_t light, uint32_t extra_mask, std::vector<ar::LightRecord> lights) {
    InstanceTable table;
    Instance in;
    in.light_index = light;
    in.mask = uint8_t(visibility_mask(kLampRayVisibility) | extra_mask);
    table.instances.push_back(in);
    table.xf.push_back(ar_xform{});
    accel.build_tlas(upload, table, lights);
  };
  CHECK_THROWS_WITH_AS(build(3, 0, {}), Contains("instance 0 names light 3"), std::runtime_error);
  const ar::LightRecord tri =
      ar::light_make_triangle(ar::Real3{0, 0, 0}, ar::Real3{1, 0, 0}, ar::Real3{0, 1, 0}, 0, 0, 0, 1.0f, white());
  CHECK_THROWS_WITH_AS(build(0, 0, {tri}), Contains("instance 0 names light 0 which is not a lamp"), std::runtime_error);
  const ar::LightRecord point_r = ar::light_make(ar::LightType::POINT, ar::Real3{0, 0, 0}, 0.5f, 1.0f, white());
  CHECK_THROWS_WITH_AS(build(0, 0, {point_r}), Contains("instance 0 names light 0 which is not a lamp"), std::runtime_error);
  const ar::LightRecord disk = ar::light_make_area(ar::LightType::AREA_DISK, ar::Real3{0, 0, 0}, ar::Real3{1, 0, 0},
                                                   ar::Real3{0, 1, 0}, 1.0f, 0.0f, 1.0f, white());
  CHECK_THROWS_WITH_AS(build(0, ar::kShadowMaskBit, {disk}), Contains("instance 0 is a lamp whose mask carries the shadow bit"),
                       std::runtime_error);
}

}  // TEST_SUITE
