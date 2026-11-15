// SPDX-License-Identifier: Apache-2.0
// The CPU intersect stages through CpuDevice on a hand-filled step table over a scene of
// unit quads in the z = 0 plane. Expected hits and stages are typed from that geometry.
// Ref: docs/decisions/0002-kernel-architecture.md D4, D8, D11 (hit record, mask, opaque shadow).
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "device/cpu/embree_scene.h"
#include "host/fault.h"
#include "host/ledger.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/thread_pool.h"
#include "stages/route_hit.h"

using namespace astroray;
using namespace astroray::host;
using ar::Stage;

namespace {

constexpr ar_key kQuadKey = 5;
constexpr uint8_t kCamera = 0x01, kDiffuse = 0x02;

std::shared_ptr<MeshRecord> quad_record() {
  auto topo = std::make_shared<MeshTopology>();
  topo->key = kQuadKey;
  topo->vertex_count = 4, topo->corner_count = 4, topo->face_count = 1, topo->motion_steps = 1;
  topo->corner_vert = {0, 1, 2, 3};
  topo->face_start = {0, 4};
  topo->tri_corners = {0, 1, 2, 0, 2, 3};
  topo->tri_face = {0, 0};
  topo->tri_material = {0, 0};
  auto rec = std::make_shared<MeshRecord>();
  rec->key = kQuadKey;
  rec->topo = topo;
  rec->positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
  return rec;
}

ar_xform translate(float x) {
  ar_xform t{};
  t.m[0] = t.m[5] = t.m[10] = 1.0f;
  t.m[3] = x;
  return t;
}

// Instances: 0 at x = 0 (plain, every ray type), 1 at x = 3 (extension material, every ray
// type), 2 at x = 6 (camera rays only: no shadow bit).
struct Rig {
  ContextState ctx;
  Ledger ledger;
  ThreadPool pool{ctx, 4};
  CpuDevice dev{ledger, pool};
  MeshStore meshes;
  EmbreeDevice embree;
  InstanceTable table;
  std::unique_ptr<EmbreeScene> accel;
  std::vector<int32_t> tri_material{0, 0};
  std::vector<ar::DeviceMeshView> mesh_views{1};
  std::vector<ar::DeviceInstanceRecord> records{3};
  std::vector<uint32_t> slots{0, 1};
  std::vector<ar::MaterialHeader> headers{{0, 0, 0, 0}, {0, 0, 0, ar::EXTENSION}};
  ar::SceneData scene{};

  // The main and shadow pools.
  std::vector<uint8_t> next, ray_type;
  std::vector<uint32_t> flags, hit_prim, hit_inst, main_queue;
  std::vector<ar::Real> hit_t;
  std::vector<ar::Real2> hit_uv;
  std::vector<ar::Real3> ray_o, ray_d;
  std::vector<uint8_t> s_next;
  std::vector<ar::Real3> s_o, s_d;
  std::vector<ar::Real> s_tmax;
  std::vector<uint32_t> shadow_queue;
  ar::StepTable step{};
  ar::PathStateSoA path{};
  ar::ShadowItemSoA shadow{};
  ar::KernelGlobals kg{};
  DeviceBuffer kg_buf;

  Rig() {
    meshes.install(quad_record());
    const uint8_t masks[3] = {0xFF, 0xFF, kCamera};
    for (int i = 0; i < 3; ++i) {
      Instance in;
      in.geom_key = kQuadKey;
      in.mask = masks[i];
      table.instances.push_back(in);
      table.xf.push_back(translate(3.0f * float(i)));
    }
    accel = std::make_unique<EmbreeScene>(embree, meshes, table, ledger, pool);
    mesh_views[0] = {};
    mesh_views[0].tri_material = uint64_t(reinterpret_cast<uintptr_t>(tri_material.data()));
    mesh_views[0].triangle_count = 2;
    for (auto& r : records) r = {}, r.light_index = ar::kNoLightIndex;
    records[0].material_offset = 0, records[0].material_count = 1;
    records[1].material_offset = 1, records[1].material_count = 1;
    scene.instances = records.data(), scene.instance_count = 3;
    scene.meshes = mesh_views.data(), scene.mesh_count = 1;
    scene.material_slots = slots.data(), scene.material_headers = headers.data(), scene.material_header_count = 2;
    scene.accel = accel.get();
    dev.register_stage(Stage::INTERSECT_CLOSEST, &cpu_stage_intersect_closest);
    dev.register_stage(Stage::INTERSECT_SHADOW, &cpu_stage_intersect_shadow);
  }
  ~Rig() { dev.free(kg_buf); }

  // A path slot: a ray at (x, y) at z = 2 looking down, of the given ray type and flags.
  void add_path(float x, float y, uint8_t type, uint32_t f) {
    next.push_back(0xEE), ray_type.push_back(type), flags.push_back(f);
    hit_prim.push_back(7), hit_inst.push_back(7), hit_t.push_back(-1.0f), hit_uv.push_back({-1, -1});
    ray_o.push_back({x, y, 2.0f}), ray_d.push_back({0, 0, -1.0f});
  }
  void add_shadow(float x, float y, float tmax) {
    s_next.push_back(0xEE), s_o.push_back({x, y, 2.0f}), s_d.push_back({0, 0, -1.0f}), s_tmax.push_back(tmax);
  }
  void bind() {
    path.next_stage = next.data(), path.ray_type = ray_type.data(), path.flags = flags.data();
    path.hit_prim = hit_prim.data(), path.hit_inst = hit_inst.data(), path.hit_t = hit_t.data();
    path.hit_uv = hit_uv.data(), path.ray_o = ray_o.data(), path.ray_d = ray_d.data();
    shadow.next_stage = s_next.data(), shadow.ray_o = s_o.data(), shadow.ray_d = s_d.data(), shadow.tmax = s_tmax.data();
    kg.path = &path, kg.shadow = &shadow, kg.step_table = &step, kg.scene = &scene;
    kg.main_queue = main_queue.data(), kg.shadow_queue = shadow_queue.data();
    kg_buf = dev.allocate(LedgerCategory::TABLES, sizeof kg, {1, 1});
    dev.upload(kg_buf, 0, &kg, sizeof kg);
    dev.set_kernel_globals(kg_buf);
  }
};

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("intersect_cpu/closest writes hit records and next stages") {
  Rig r;
  r.add_path(0.75f, 0.25f, kCamera, 0);              // 0: inst 0, triangle 0 -> SHADE_SURFACE
  r.add_path(3.25f, 0.75f, kCamera, 0);              // 1: inst 1, triangle 1 -> SHADE_SURFACE_EXT
  r.add_path(9.5f, 0.25f, kCamera, 0);               // 2: miss -> SHADE_BACKGROUND
  r.add_path(0.25f, 0.5f, kDiffuse, ar::PATH_KIND);  // 3: light path hit -> SHADE_LIGHT_PATH
  r.add_path(9.5f, 0.25f, kDiffuse, ar::PATH_KIND);  // 4: light path miss -> TERMINATED
  r.add_path(6.5f, 0.25f, kDiffuse, 0);              // 5: inst 2 is camera-only: a diffuse ray misses
  r.add_path(6.5f, 0.25f, kCamera, 0);               // 6: the same ray as a camera ray hits inst 2
  r.add_path(0.5f, 0.25f, kCamera, 0);               // 7: not in the segment, must stay untouched
  // The segment is queue entries [1, 8): slots in a permuted order after one foreign entry.
  r.main_queue = {7, 6, 0, 4, 2, 5, 3, 1};
  r.step.stage[static_cast<uint32_t>(Stage::INTERSECT_CLOSEST)] = {1, 7, 0};
  r.bind();
  r.dev.launch(Stage::INTERSECT_CLOSEST, 7);

  CHECK(r.hit_inst[0] == 0u);
  CHECK(r.hit_prim[0] == 0u);
  CHECK(std::fabs(r.hit_t[0] - 2.0f) < 1e-6f);
  CHECK(std::fabs(r.hit_uv[0].x - 0.5f) < 1e-6f);  // triangle 0: u = x - y, v = y
  CHECK(std::fabs(r.hit_uv[0].y - 0.25f) < 1e-6f);
  CHECK(r.next[0] == uint8_t(Stage::SHADE_SURFACE));

  CHECK(r.hit_inst[1] == 1u);
  CHECK(r.hit_prim[1] == 1u);
  CHECK(std::fabs(r.hit_uv[1].x - 0.25f) < 1e-6f);  // triangle 1 at local (0.25, 0.75): u = x, v = y - x
  CHECK(std::fabs(r.hit_uv[1].y - 0.5f) < 1e-6f);
  CHECK(r.next[1] == uint8_t(Stage::SHADE_SURFACE_EXT));

  CHECK(r.hit_inst[2] == ar::kNoInstance);
  CHECK(r.next[2] == uint8_t(Stage::SHADE_BACKGROUND));
  CHECK(r.hit_inst[3] == 0u);
  CHECK(r.next[3] == uint8_t(Stage::SHADE_LIGHT_PATH));
  CHECK(r.hit_inst[4] == ar::kNoInstance);
  CHECK(r.next[4] == uint8_t(Stage::TERMINATED));
  CHECK(r.hit_inst[5] == ar::kNoInstance);
  CHECK(r.next[5] == uint8_t(Stage::SHADE_BACKGROUND));
  CHECK(r.hit_inst[6] == 2u);
  CHECK(r.next[6] == uint8_t(Stage::SHADE_SURFACE));

  CHECK(r.next[7] == 0xEE);
  CHECK(r.hit_inst[7] == 7u);
}

TEST_CASE("intersect_cpu/occluded shadow terminates") {
  Rig r;
  r.add_shadow(0.5f, 0.25f, 100.0f);  // 0: the quad of inst 0 lies at t = 2
  r.add_shadow(3.5f, 0.25f, 2.5f);    // 1: inst 1 at t = 2, inside tmax
  r.add_shadow(0.5f, 0.25f, 1.5f);    // 2: tmax ends before the surface
  r.add_shadow(9.5f, 0.25f, 100.0f);  // 3: nothing there
  r.add_shadow(6.5f, 0.25f, 100.0f);  // 4: inst 2 has no shadow bit
  r.shadow_queue = {4, 3, 2, 1, 0};
  r.step.stage[static_cast<uint32_t>(Stage::INTERSECT_SHADOW)] = {0, 5, 0};
  r.bind();
  r.dev.launch(Stage::INTERSECT_SHADOW, 5);
  CHECK(r.s_next[0] == uint8_t(Stage::SHADOW_TERMINATED));
  CHECK(r.s_next[1] == uint8_t(Stage::SHADOW_TERMINATED));
  CHECK(r.s_next[2] == uint8_t(Stage::SHADE_SHADOW));
  CHECK(r.s_next[3] == uint8_t(Stage::SHADE_SHADOW));
  CHECK(r.s_next[4] == uint8_t(Stage::SHADE_SHADOW));
}

}  // TEST_SUITE
