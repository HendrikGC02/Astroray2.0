// SPDX-License-Identifier: Apache-2.0
// The traversal structures of a scene snapshot on the CPU (task 114): a scene with one quad, one
// rectangle lamp and one sphere lamp gives camera rays that hit each at the analytic t through
// the KernelGlobals the table set builds; moving an instance rebuilds the instance structure and
// reuses every mesh and lamp structure (the builder's counters); an older snapshot keeps tracing
// its own geometry after a newer one exists (RULINGS 70); an empty scene has a valid structure
// that traces to a miss; a null accel throws in every build.
// Ref: docs/decisions/0002-kernel-architecture.md D11, D13; RULINGS 70.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include "device/cpu/cpu_device.h"
#include "host/fault.h"
#include "host/ledger.h"
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
#include "vm/ops.h"

using namespace astroray;
using namespace astroray::host;
using ar::Stage;

namespace {

constexpr ar_key kQuad = 100, kRectLamp = 200, kSphereLamp = 201;

struct Rig {
  ContextState ctx;
  Scene scene{ctx, SceneConfig{2, {}, {}}};
  Rig() {
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
  // The quad [0, 1]^2 in z = 0 (key 100), a 2 x 2 rectangle lamp in z = 3 centred at (5, 0), and a
  // sphere lamp of radius 1 at (-5, 0, 0).
  void build() {
    commit([&](Edit& e) {
      ar_light_desc rect = lamp(AR_LIGHT_AREA_RECT, 5, 0, 3, 2, 2), sphere = lamp(AR_LIGHT_POINT, -5, 0, 0, 1, 0);
      REQUIRE(set_light(e, kRectLamp, &rect, scene.materials->table()) == AR_OK);
      REQUIRE(set_light(e, kSphereLamp, &sphere, scene.materials->table()) == AR_OK);
    });
    const std::vector<float> pos{0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    const std::vector<int32_t> cv{0, 1, 2, 3}, fs{0};
    commit([&](Edit& e) {
      ar_mesh_desc d = {};
      d.key = kQuad, d.vertex_count = 4, d.corner_count = 4, d.face_count = 1, d.motion_steps = 1;
      ar_mesh_buffers b = {};
      REQUIRE(mesh_begin(scene, e, &d, &b) == AR_OK);
      std::memcpy(*b.positions, pos.data(), pos.size() * 4);
      std::memcpy(*b.corner_vert, cv.data(), cv.size() * 4);
      std::memcpy(*b.face_start, fs.data(), fs.size() * 4);
      for (uint64_t i = 0; i < b.corner_normals_count; ++i) (*b.corner_normals)[i] = i % 3 == 2 ? 1.0f : 0.0f;
      REQUIRE(e.item_commit(kQuad) == AR_OK);
    });
    place_quad(0.0f);
  }
  // Material `key` with a constant Emission graph (graph key = key + 1000).
  void material(ar_key key, float strength) {
    std::vector<ar_graph_node> nodes;
    std::vector<ar_graph_link> links;
    std::vector<float> f;
    const auto add = [&](ar::Op op, std::vector<float> fp) {
      ar_graph_node n = {};
      n.op = uint32_t(op), n.f32_first = uint32_t(f.size()), n.f32_count = uint32_t(fp.size());
      f.insert(f.end(), fp.begin(), fp.end());
      nodes.push_back(n);
      return uint32_t(nodes.size()) - 1;
    };
    const uint32_t c = add(ar::Op::CONST_F32X3, {1.0f, 0.5f, 0.25f}), s = add(ar::Op::CONST_F32, {strength});
    const uint32_t e = add(ar::Op::EMIT_LE, {});
    links.push_back({c, 0u, e, 0u});
    links.push_back({s, 0u, e, 1u});
    ar_graph_desc d = {};
    d.node_count = uint32_t(nodes.size()), d.link_count = uint32_t(links.size()), d.f32_count = uint32_t(f.size());
    d.nodes = nodes.data(), d.links = links.data(), d.params_f32 = f.data();
    commit([&](Edit& ed) {
      REQUIRE(scene.materials->set_graph(ed, key + 1000, &d) == AR_OK);
      ar_material_desc m = {};
      m.surface_graph = key + 1000;
      REQUIRE(scene.materials->set_material(ed, key, &m) == AR_OK);
    });
  }
  // The quad's one instance, its object translated by `x` along x.
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
  uint8_t next = 0;
};

// Camera rays from z = 10 straight down at each (x, y), through the stage a session launches.
std::vector<Hit> trace(const SceneTableSet& set, const std::vector<std::pair<float, float>>& xy) {
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
  KgSession none;
  ar::KernelGlobals kg = build_kernel_globals(&set, none);
  kg.path = &path, kg.step_table = &step, kg.main_queue = queue.data();
  DeviceBuffer kg_buf = dev.allocate(LedgerCategory::TABLES, sizeof kg, {1, 1});
  dev.upload(kg_buf, 0, &kg, sizeof kg);
  dev.set_kernel_globals(kg_buf);
  dev.launch(Stage::INTERSECT_CLOSEST, uint32_t(n));
  dev.free(kg_buf);
  std::vector<Hit> out(n);
  for (size_t i = 0; i < n; ++i) out[i] = {hit_t[i], hit_inst[i], next[i]};
  return out;
}

// The light type of the lamp a hit instance carries.
ar::LightType lamp_type(const SceneTableSet& set, uint32_t inst) {
  const ar::DeviceInstanceRecord& r = set.scene_host.instances[inst];
  REQUIRE(r.light_index != ar::kNoLightIndex);
  return ar::LightType(static_cast<const ar::LightRecord*>(set.tables[static_cast<uint32_t>(TableId::LIGHTS)]->ptr())[r.light_index].type);
}

}  // namespace

TEST_SUITE("gate") {

TEST_CASE("scene_accel/hits mesh and lamps") {
  Rig r;
  r.build();
  const auto set = acquire_scene_tables(r.scene);
  REQUIRE(set->accel);
  CHECK(set->scene_host.accel != nullptr);
  const auto h = trace(*set, {{0.75f, 0.25f}, {5.2f, 0.3f}, {-5.0f, 0.0f}, {20.0f, 20.0f}});
  CHECK(h[0].inst == 0u);                       // the quad: z = 0, t = 10
  CHECK(std::fabs(h[0].t - 10.0f) < 1e-5f);
  REQUIRE(h[1].inst != ar::kNoInstance);        // the rectangle lamp: z = 3, t = 7
  CHECK(h[1].inst >= 1u);
  CHECK(lamp_type(*set, h[1].inst) == ar::LightType::AREA_RECT);
  CHECK(std::fabs(h[1].t - 7.0f) < 1e-5f);
  REQUIRE(h[2].inst != ar::kNoInstance);        // the sphere lamp: top of a unit sphere at z = 1, t = 9
  CHECK(h[2].inst >= 1u);
  CHECK(lamp_type(*set, h[2].inst) == ar::LightType::SPHERE);
  CHECK(std::fabs(h[2].t - 9.0f) < 1e-5f);
  CHECK(h[3].inst == ar::kNoInstance);          // nothing there
}

TEST_CASE("scene_accel/transform edit rebuilds only the instance structure") {
  Rig r;
  r.build();
  AccelBuilder& builder = r.scene.accel_builder();
  const auto first = acquire_scene_tables(r.scene);
  const AccelCounters a = builder.counters();
  CHECK(a.gas_builds == 3u);  // the quad and the two lamp shapes
  CHECK(a.tlas_builds == 1u);

  r.place_quad(2.0f);  // moves the one instance; the mesh record is the same object
  const auto second = acquire_scene_tables(r.scene);
  REQUIRE((second != first));
  const AccelCounters b = builder.counters();
  CHECK(b.gas_builds == a.gas_builds);       // no mesh or lamp structure rebuilt
  CHECK(b.gas_reused == a.gas_reused + 3u);  // all three carried over by identity
  CHECK(b.tlas_builds == a.tlas_builds + 1u);

  // The newer snapshot sees the quad at x in [2, 3], the older one still traces its own geometry.
  const std::vector<std::pair<float, float>> rays{{0.75f, 0.25f}, {2.75f, 0.25f}};
  const auto on_second = trace(*second, rays), on_first = trace(*first, rays);
  CHECK(on_second[0].inst == ar::kNoInstance);
  CHECK(on_second[1].inst == 0u);
  CHECK(on_first[0].inst == 0u);
  CHECK(on_first[1].inst == ar::kNoInstance);

  // A snapshot with the same instances, lamps and meshes shares the structure outright.
  const auto third = acquire_scene_tables(r.scene);
  CHECK((third == second));
  CHECK(builder.counters().tlas_builds == b.tlas_builds);
}

// A material-only edit changes the set but not the geometry, so the structure is the same object;
// editing a lamp changes the TLAS (the lamp's placement) and nothing else.
TEST_CASE("scene_accel/whole structure reuse and lamp edit") {
  Rig r;
  r.build();
  AccelBuilder& builder = r.scene.accel_builder();
  const auto first = acquire_scene_tables(r.scene);
  const AccelCounters a = builder.counters();
  const std::vector<std::pair<float, float>> probe{{5.2f, 0.3f}};
  CHECK(std::fabs(trace(*first, probe)[0].t - 7.0f) < 1e-5f);

  r.material(43, 0.5f);  // referenced by no instance
  const auto second = acquire_scene_tables(r.scene);
  REQUIRE((second != first));
  CHECK((second->accel == first->accel));
  CHECK(builder.counters().tlas_builds == a.tlas_builds);
  CHECK(builder.counters().gas_builds == a.gas_builds);

  // The rectangle lamp moves to z = 4 and grows to 4 x 4: the probe now hits at t = 6.
  r.commit([&](Edit& e) {
    ar_light_desc rect = Rig::lamp(AR_LIGHT_AREA_RECT, 5, 0, 4, 4, 4);
    REQUIRE(set_light(e, kRectLamp, &rect, r.scene.materials->table()) == AR_OK);
  });
  const auto third = acquire_scene_tables(r.scene);
  CHECK((third->accel != second->accel));
  const AccelCounters b = builder.counters();
  CHECK(b.tlas_builds == a.tlas_builds + 1u);
  CHECK(b.gas_builds == a.gas_builds);
  CHECK(std::fabs(trace(*third, probe)[0].t - 6.0f) < 1e-5f);
  CHECK(std::fabs(trace(*second, probe)[0].t - 7.0f) < 1e-5f);  // the pinned older set is unchanged
}

TEST_CASE("scene_accel/empty scene has a valid structure") {
  Rig r;
  const auto set = acquire_scene_tables(r.scene);
  REQUIRE(set->accel);
  CHECK(set->scene_host.accel != nullptr);
  const auto missing = missing_pointers(build_kernel_globals(set.get(), KgSession{}), *set, false);
  CHECK(std::find(missing.begin(), missing.end(), "scene.accel") == missing.end());
  const auto h = trace(*set, {{0.5f, 0.5f}});
  CHECK(h[0].inst == ar::kNoInstance);
  CHECK(h[0].next == uint8_t(Stage::SHADE_BACKGROUND));
}

TEST_CASE("scene_accel/missing accel throws") {
  ar::SceneData sd{};
  ar::KernelGlobals kg{};
  kg.scene = &sd;
  ar::StepEntry step{};
  CHECK_THROWS_AS(cpu_stage_intersect_closest(kg, step, 0, 1), std::logic_error);
  CHECK_THROWS_AS(cpu_stage_intersect_shadow(kg, step, 0, 1), std::logic_error);
}

}  // TEST_SUITE
