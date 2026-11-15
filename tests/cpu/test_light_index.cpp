// SPDX-License-Identifier: Apache-2.0
// The emissive-hit light index (kernel/light/light_sampler.h light_of_hit) on the CPU path: for a
// scene with two emissive meshes (one emissive on one material slot only), a non-emissive mesh, a
// lamp and the world, every emitting triangle maps to the light record holding its vertices,
// every other triangle and the lamp instance to kNoLight, a hit outside the scene to
// kLightIndexInvalid, and a material edit that turns emission off rebuilds the index.
// Ref: docs/decisions/0004-integrator.md D1 (emitter hits weighted with the light's pmf), D9.
#include <doctest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include "host/light/lamps.h"
#include "host/light/light_builder.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/scene/scene_tables.h"
#include "host/session/kg_builder.h"
#include "kernel/light/light_sampler.h"
#include "vm/ops.h"

using namespace astroray;
using namespace astroray::host;

namespace {

// A constant Emission graph of colour (1, 0.5, 0.25) and the given strength.
struct Graph {
  std::vector<ar_graph_node> nodes;
  std::vector<ar_graph_link> links;
  std::vector<float> f;
  int add(ar::Op op, std::vector<float> fp) {
    ar_graph_node n = {};
    n.op = uint32_t(op);
    n.f32_first = uint32_t(f.size()), n.f32_count = uint32_t(fp.size());
    f.insert(f.end(), fp.begin(), fp.end());
    nodes.push_back(n);
    return int(nodes.size()) - 1;
  }
  explicit Graph(float strength) {
    const int c = add(ar::Op::CONST_F32X3, {1.0f, 0.5f, 0.25f}), s = add(ar::Op::CONST_F32, {strength});
    const int e = add(ar::Op::EMIT_LE, {});
    links.push_back({uint32_t(c), 0u, uint32_t(e), 0u});
    links.push_back({uint32_t(s), 0u, uint32_t(e), 1u});
  }
  ar_graph_desc desc() const {
    ar_graph_desc d = {};
    d.node_count = uint32_t(nodes.size()), d.link_count = uint32_t(links.size()), d.f32_count = uint32_t(f.size());
    d.nodes = nodes.data(), d.links = links.data(), d.params_f32 = f.data();
    return d;
  }
};

// Instance i: mesh, material slots, translation.
struct Inst {
  ar_key geom;
  std::vector<ar_key> pool;
  float t[3];
};
const std::vector<Inst> kInstances = {
    {100, {42, 50}, {10, 0, 0}},  // two quads: face 0 on slot 1 (not emissive), face 1 on slot 0 (emissive)
    {101, {50}, {0, 10, 0}},      // not emissive
    {101, {43}, {0, 0, 10}},      // emissive
};

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
  // Material `key` with its own constant Emission graph (graph key = key + 1000); strength 0 does
  // not emit (AUTO sampling).
  void material(Edit& e, ar_key key, float strength) {
    const Graph g(strength);
    const ar_graph_desc d = g.desc();
    REQUIRE(scene.materials->set_graph(e, key + 1000, &d) == AR_OK);
    ar_material_desc m = {};
    m.surface_graph = key + 1000;
    m.emission_sampling = AR_EMISSION_SAMPLING_AUTO;
    REQUIRE(scene.materials->set_material(e, key, &m) == AR_OK);
  }
  void mesh(Edit& e, ar_key key, const std::vector<float>& pos, const std::vector<int32_t>& cv,
            const std::vector<int32_t>& fs, const std::vector<int32_t>& mat) {
    ar_mesh_desc d = {};
    d.key = key, d.vertex_count = uint32_t(pos.size() / 3), d.corner_count = uint32_t(cv.size());
    d.face_count = uint32_t(fs.size()), d.motion_steps = 1;
    if (!mat.empty()) d.flags = AR_MESH_HAS_MATERIAL_INDEX;
    ar_mesh_buffers b = {};
    REQUIRE(mesh_begin(scene, e, &d, &b) == AR_OK);
    std::memcpy(*b.positions, pos.data(), pos.size() * 4);
    std::memcpy(*b.corner_vert, cv.data(), cv.size() * 4);
    std::memcpy(*b.face_start, fs.data(), fs.size() * 4);
    if (!mat.empty()) std::memcpy(*b.material_index, mat.data(), mat.size() * 4);
    for (uint64_t i = 0; i < b.corner_normals_count; ++i) (*b.corner_normals)[i] = i % 3 == 2 ? 1.0f : 0.0f;
    REQUIRE(e.item_commit(key) == AR_OK);
  }
  void build() {
    const Graph gw(0.5f);
    commit([&](Edit& e) {
      const ar_graph_desc dw = gw.desc();
      ar_world_desc w = {};
      w.surface_graph = 40;
      REQUIRE(scene.materials->set_graph(e, 40, &dw) == AR_OK);
      REQUIRE(scene.materials->set_world(e, &w) == AR_OK);
      material(e, 42, 2.0f);
      material(e, 43, 3.0f);
      material(e, 50, 0.0f);
      ar_light_desc l = {};
      l.type = AR_LIGHT_POINT;
      l.color.kind = AR_SPECTRUM_RGB;
      l.color.rgb[0] = l.color.rgb[1] = l.color.rgb[2] = 1.0f;
      l.xf.m[0] = l.xf.m[5] = l.xf.m[10] = 1.0f, l.xf.m[3] = 2.0f;
      l.visibility = AR_VISIBILITY_CAMERA | AR_VISIBILITY_DIFFUSE;
      l.power = 10.0f, l.normalize = 1, l.cast_shadow = 1;
      REQUIRE(set_light(e, 200, &l, scene.materials->table()) == AR_OK);
    });
    commit([&](Edit& e) {
      mesh(e, 100, {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 2, 0, 0, 2, 1, 0}, {0, 1, 2, 3, 1, 4, 5, 2}, {0, 4}, {1, 0});
      mesh(e, 101, {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0}, {0, 1, 2, 3}, {0}, {});
    });
    commit([&](Edit& e) {
      const uint32_t n = uint32_t(kInstances.size());
      std::vector<ar_key> geom, object, pool;
      std::vector<ar_xform> xf(n);
      std::vector<uint32_t> zeros(n, 0), vis(n, AR_VISIBILITY_CAMERA), moff, mcount;
      for (uint32_t i = 0; i < n; ++i) {
        const Inst& in = kInstances[i];
        geom.push_back(in.geom), object.push_back(i + 1);
        xf[i].m[0] = xf[i].m[5] = xf[i].m[10] = 1.0f;
        xf[i].m[3] = in.t[0], xf[i].m[7] = in.t[1], xf[i].m[11] = in.t[2];
        moff.push_back(uint32_t(pool.size())), mcount.push_back(uint32_t(in.pool.size()));
        pool.insert(pool.end(), in.pool.begin(), in.pool.end());
      }
      std::vector<ar_rgba> colors(n, ar_rgba{{1, 1, 1, 1}});
      std::vector<uint8_t> lg(n, 0);
      ar_instances_desc d = {};
      d.instance_count = n, d.motion_steps = 1, d.transform_count = n, d.material_key_count = uint32_t(pool.size());
      d.geom_keys = geom.data(), d.xf = xf.data(), d.object_keys = object.data();
      d.random_ids = d.pass_indices = d.crypto_objects = d.crypto_assets = zeros.data();
      d.colors = colors.data(), d.visibility = vis.data(), d.flags = zeros.data(), d.light_groups = lg.data();
      d.material_offsets = moff.data(), d.material_counts = mcount.data(), d.material_keys = pool.data();
      REQUIRE(set_instances(e, &d) == AR_OK);
    });
  }
};

// Checks every triangle of every instance against `emissive` (the material keys that emit) and
// returns how many map to a light.
uint32_t check_map(Rig& r, const SceneTableSet& set, const std::vector<ar_key>& emissive) {
  const ar::KernelGlobals kg = build_kernel_globals(&set, KgSession{});
  REQUIRE(kg.light_index != nullptr);
  REQUIRE(kg.light_index == set.lights->light_index.data());
  const uint32_t light_count = uint32_t(set.lights->lights.size());
  uint32_t mapped = 0;
  for (uint32_t i = 0; i < uint32_t(kInstances.size()); ++i) {
    const Inst& in = kInstances[i];
    const auto rec = r.scene.meshes->find(in.geom);
    REQUIRE(rec);
    const MeshTopology& topo = *rec->topo;
    for (uint32_t t = 0; t < topo.triangle_count(); ++t) {
      const ar_key mat = in.pool[size_t(topo.tri_material[t])];
      const bool emits = std::find(emissive.begin(), emissive.end(), mat) != emissive.end();
      const uint32_t l = ar::light_of_hit(kg, i, t);
      CAPTURE(i);
      CAPTURE(t);
      if (!emits) {
        CHECK(l == ar::kNoLight);
        continue;
      }
      REQUIRE(l < light_count);
      ++mapped;
      const ar::LightRecord& lr = kg.lights[l];
      CHECK(lr.type == uint8_t(ar::LightType::TRIANGLE));
      CHECK(ar::tri_instance(lr) == i);
      CHECK(ar::tri_index(lr) == t);
      for (int k = 0; k < 3; ++k) {
        const float* p = &rec->positions[size_t(topo.corner_vert[topo.tri_corners[3 * t + k]]) * 3];
        const ar::Real3 v = ar::tri_vertex(lr, k);
        CHECK(v.x == ar::Real(p[0] + in.t[0]));
        CHECK(v.y == ar::Real(p[1] + in.t[1]));
        CHECK(v.z == ar::Real(p[2] + in.t[2]));
      }
    }
  }
  return mapped;
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("light_index/emitting triangles map to their light") {
  Rig r;
  r.build();
  const auto set = acquire_scene_tables(r.scene);
  REQUIRE(set);
  // Two triangles of face 1 of instance 0, two of instance 2; then the world and the lamp.
  CHECK(set->lights->lights.size() == 6u);
  CHECK(check_map(r, *set, {42, 43}) == 4u);
  const ar::KernelGlobals kg = build_kernel_globals(set.get(), KgSession{});
  CHECK(kg.light_index_words == set->lights->light_index.size());
  // Instance 0's slice covers all four triangles, so triangles 0 and 1 read in-slice kNoLight.
  const std::vector<uint32_t>& map = set->lights->light_index;
  REQUIRE(map[1] != ar::kNoLight);
  CHECK(map[map[1]] == 4u);
  CHECK(ar::light_of_hit(kg, 0, 0) == ar::kNoLight);
  CHECK(ar::light_of_hit(kg, 0, 1) == ar::kNoLight);
  // The lamp's instance record (after the three geometry instances) is not a triangle light.
  REQUIRE(kg.scene->instance_count == 4u);
  CHECK(ar::light_of_hit(kg, 3, 0) == ar::kNoLight);
}

TEST_CASE("light_index/hits outside the scene are invalid") {
  Rig r;
  r.build();
  const auto set = acquire_scene_tables(r.scene);
  const ar::KernelGlobals kg = build_kernel_globals(set.get(), KgSession{});
  CHECK(ar::light_of_hit(kg, 4, 0) == ar::kLightIndexInvalid);       // past the instances
  CHECK(ar::light_of_hit(kg, ~0u, 0) == ar::kLightIndexInvalid);
  CHECK(ar::light_of_hit(kg, 0, 4) == ar::kLightIndexInvalid);       // mesh 100 has 4 triangles
  CHECK(ar::light_of_hit(kg, 1, 2) == ar::kLightIndexInvalid);       // unlit instance, mesh 101 has 2
  CHECK(ar::light_of_hit(kg, 2, ~0u) == ar::kLightIndexInvalid);
  CHECK(ar::kLightIndexInvalid != ar::kNoLight);
}

TEST_CASE("light_index/material edit rebuilds the map") {
  Rig r;
  r.build();
  const auto first = acquire_scene_tables(r.scene);
  REQUIRE(check_map(r, *first, {42, 43}) == 4u);
  r.commit([&](Edit& e) { r.material(e, 43, 0.0f); });  // instance 2 stops emitting
  const auto second = acquire_scene_tables(r.scene);
  REQUIRE((second->lights != first->lights));
  CHECK(second->lights->lights.size() == 4u);
  CHECK(check_map(r, *second, {42}) == 2u);
  // Instance 2 was the last emissive instance, so the index now ends at instance 0.
  CHECK(second->lights->light_index[0] == 1u);
}

TEST_CASE("light_index/duplicate triangle records are refused") {
  LightInput a;
  a.record = ar::light_make_triangle(ar::Real3{0, 0, 0}, ar::Real3{1, 0, 0}, ar::Real3{0, 1, 0}, 3, 5, 0, ar::Real(1),
                                     ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, ar::Real(1)));
  // A zero response: every importance is 0, which the index does not read.
  static const std::vector<float> zero(3 * size_t(ar::kObsSamples), 0.0f);
  const ar::ObserverView obs{zero.data(), zero.data()};
  CHECK_THROWS_AS(build_light_table({a, a}, ar::SceneSphere{}, obs, nullptr), std::runtime_error);
}

}  // TEST_SUITE
