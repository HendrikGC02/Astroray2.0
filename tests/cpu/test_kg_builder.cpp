// SPDX-License-Identifier: Apache-2.0
// The KernelGlobals builder on the CPU path (task 102): for a scene with one object of each kind
// (mesh, instance, material, image, lamp, world) every pointer KernelGlobals carries is set or
// optional by rule, every count matches the scene's host arrays, an unchanged scene reuses its
// table set, and a failed table build names the table and keeps the previous set (RULINGS 70).
// Ref: docs/decisions/0002-kernel-architecture.md D4, D13; RULINGS 70.
#include <doctest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "host/image/image_store.h"
#include "host/light/lamps.h"
#include "host/light/light_builder.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/mesh.h"
#include "host/scene/scene_tables.h"
#include "host/session/kg_builder.h"
#include "film/present.h"
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
  // A lit world, an emissive material on one instanced quad, an unreferenced image (missing file:
  // the magenta stand-in) and one point lamp. `pool_key` is the instance's material key.
  void build(ar_key pool_key = 42) {
    const Graph gw(0.5f), ge(2.0f);
    commit([&](Edit& e) {
      const ar_graph_desc dw = gw.desc(), de = ge.desc();
      ar_world_desc w = {};
      w.surface_graph = 40;
      REQUIRE(scene.materials->set_graph(e, 40, &dw) == AR_OK);
      REQUIRE(scene.materials->set_world(e, &w) == AR_OK);
      REQUIRE(scene.materials->set_graph(e, 41, &de) == AR_OK);
      ar_material_desc m = {};
      m.surface_graph = 41;
      m.emission_sampling = AR_EMISSION_SAMPLING_AUTO;
      REQUIRE(scene.materials->set_material(e, 42, &m) == AR_OK);
      ar_image_desc im = {};
      im.key = 7, im.source = AR_IMAGE_SOURCE_FILE, im.color_interop = AR_COLOR_INTEROP_LIN_REC709;
      im.path = "no_such_image_102.png";
      REQUIRE(scene.materials->set_image(e, &im) == AR_OK);
      ar_light_desc l = {};
      l.type = AR_LIGHT_POINT;
      l.color.kind = AR_SPECTRUM_RGB;
      l.color.rgb[0] = l.color.rgb[1] = l.color.rgb[2] = 1.0f;
      l.xf.m[0] = l.xf.m[5] = l.xf.m[10] = 1.0f, l.xf.m[3] = 2.0f;
      l.visibility = AR_VISIBILITY_CAMERA | AR_VISIBILITY_DIFFUSE;
      l.power = 10.0f, l.normalize = 1, l.cast_shadow = 1;
      REQUIRE(set_light(e, 200, &l, scene.materials->table()) == AR_OK);
    });
    const std::vector<float> pos{0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    const std::vector<int32_t> cv{0, 1, 2, 3}, fs{0};
    commit([&](Edit& e) {
      ar_mesh_desc d = {};
      d.key = 100, d.vertex_count = 4, d.corner_count = 4, d.face_count = 1, d.motion_steps = 1;
      ar_mesh_buffers b = {};
      REQUIRE(mesh_begin(scene, e, &d, &b) == AR_OK);
      std::memcpy(*b.positions, pos.data(), pos.size() * 4);
      std::memcpy(*b.corner_vert, cv.data(), cv.size() * 4);
      std::memcpy(*b.face_start, fs.data(), fs.size() * 4);
      for (uint64_t i = 0; i < b.corner_normals_count; ++i) (*b.corner_normals)[i] = i % 3 == 2 ? 1.0f : 0.0f;
      REQUIRE(e.item_commit(100) == AR_OK);
    });
    std::vector<ar_key> geom{100}, object{1}, pool{pool_key};
    std::vector<ar_xform> xf(1);
    xf[0].m[0] = xf[0].m[5] = xf[0].m[10] = 1.0f;
    std::vector<uint32_t> zeros(1, 0), vis(1, AR_VISIBILITY_CAMERA), moff{0}, mcount{1};
    std::vector<ar_rgba> colors(1, ar_rgba{{1, 1, 1, 1}});
    std::vector<uint8_t> lg(1, 0);
    ar_instances_desc d = {};
    d.instance_count = 1, d.motion_steps = 1, d.transform_count = 1, d.material_key_count = 1;
    d.geom_keys = geom.data(), d.xf = xf.data(), d.object_keys = object.data();
    d.random_ids = d.pass_indices = d.crypto_objects = d.crypto_assets = zeros.data();
    d.colors = colors.data(), d.visibility = vis.data(), d.flags = zeros.data(), d.light_groups = lg.data();
    d.material_offsets = moff.data(), d.material_counts = mcount.data(), d.material_keys = pool.data();
    commit([&](Edit& e) { REQUIRE(set_instances(e, &d) == AR_OK); });
  }
};

uint32_t count(const SceneTableSet& s, TableId id) { return s.counts[static_cast<uint32_t>(id)]; }

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("kg_builder/no null tables") {
  Rig r;
  r.build();
  const auto lights = r.scene.lights();
  const auto set = acquire_scene_tables(r.scene, lights);
  REQUIRE(set);
  // The session's part: any distinct non-null addresses (the builder only copies them).
  ar::FilmData film{};
  ar::DisplayTarget display{};
  uint32_t active = 0;
  KgSession in;
  in.film = &film, in.display = &display, in.active_pixels = &active;
  const ar::KernelGlobals kg = build_kernel_globals(set.get(), in);
  const std::vector<std::string> missing = missing_pointers(kg, *set, false);
  CHECK_MESSAGE(missing.empty(), (missing.empty() ? std::string() : missing.front()));
  // Every pointer KernelGlobals and SceneData carry for this scene, by name, besides the rule.
  for (const void* p : {(const void*)kg.scene, (const void*)kg.tables, (const void*)kg.lights,
                        (const void*)kg.light_sampler, (const void*)kg.e_diel, (const void*)kg.e_diel->e,
                        (const void*)kg.tables->rgb2spec, (const void*)kg.tables->images,
                        (const void*)kg.tables->srgb_decode, (const void*)kg.tables->observer.cmf,
                        (const void*)kg.scene->instances, (const void*)kg.scene->meshes,
                        (const void*)kg.scene->material_slots, (const void*)kg.scene->material_headers,
                        (const void*)kg.scene->material_constants, (const void*)kg.light_sampler->bins})
    CHECK(p != nullptr);
  CHECK(kg.scene->accel == nullptr);  // marked unused until task 114

  const auto mats = r.scene.materials->arrays();
  const auto imgs = r.scene.materials->images().arrays();
  CHECK(kg.scene->instance_count == 2u);  // the quad and the lamp's record
  CHECK(kg.scene->mesh_count == 1u);
  CHECK(kg.scene->material_header_count == mats->headers.size());
  CHECK(kg.material_slot_count == 1u);
  CHECK(kg.material_program_words == mats->programs.size());
  CHECK(kg.material_constant_count == mats->constants.size());
  CHECK(kg.tables->image_count == imgs->views.size());
  CHECK(kg.tables->image_count == 1u);
  CHECK(kg.light_sampler->light_count == lights->lights.size());
  CHECK(kg.light_sampler->bin_count == lights->bins.size());
  CHECK(lights->lights.size() == 4u);  // world, two triangles, the lamp
  CHECK(count(*set, TableId::LIGHTS) == 4u);
  CHECK(kg.scene->instances[1].light_index == lights->lamp_first);
  CHECK(kg.scene->instances[1].mesh == kNoMeshIndex);
  CHECK(kg.scene->material_slots[0] < kg.scene->material_header_count);
  CHECK(kg.scene->meshes[0].triangle_count == 2u);
  // Store arrays are read in place on the CPU path (no copy, no second ledger record).
  CHECK(kg.scene->material_headers == mats->headers.data());
  CHECK(kg.lights == lights->lights.data());

  // Unchanged snapshot: the same set, shared.
  CHECK((acquire_scene_tables(r.scene, lights) == set));
}

TEST_CASE("kg_builder/failed table keeps the previous set") {
  Rig r;
  r.build(99);  // the instance names material key 99, which nothing compiled
  const auto lights = r.scene.lights();
  CHECK_THROWS_WITH_AS(acquire_scene_tables(r.scene, lights), doctest::Contains("material slots"), std::runtime_error);
  CHECK(!r.scene.table_cache().current);
}

}  // TEST_SUITE
