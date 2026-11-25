// SPDX-License-Identifier: Apache-2.0
// The KernelGlobals builder on the CPU path (task 102): for a scene with one object of each kind
// (mesh, instance, material, image, lamp, world) every pointer KernelGlobals carries is set or
// optional by rule, every count matches the scene's host arrays, an unchanged scene reuses its
// table set, and a failed table build names the table and keeps the previous set (RULINGS 70).
// Ref: docs/decisions/0002-kernel-architecture.md D4, D13; RULINGS 70.
#include <doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <string>
#include <vector>

#include "host/image/image_store.h"
#include "host/light/lamps.h"
#include "host/light/light_builder.h"
#include "host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/instances.h"
#include "host/scene/media.h"
#include "host/scene/mesh.h"
#include "host/scene/scene_tables.h"
#include "host/session/kg_builder.h"
#include "host/session/session.h"
#include "film/present.h"
#include "medium/medium_eval.h"
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
  uint64_t commit(F&& body) {
    auto e = scene.begin_edit();
    REQUIRE(e);
    body(*e);
    uint64_t g = 0;
    REQUIRE(e->commit(&g) == AR_OK);
    scene.materials->end_edit();
    e.reset();
    scene.generations().wait_idle();
    REQUIRE(scene.generations().failed() == 0);
    return g;
  }
  // Material `key` with its own constant Emission graph (graph key = key + 1000).
  void material(Edit& e, ar_key key, float strength) {
    const Graph g(strength);
    const ar_graph_desc d = g.desc();
    REQUIRE(scene.materials->set_graph(e, key + 1000, &d) == AR_OK);
    ar_material_desc m = {};
    m.surface_graph = key + 1000;
    m.emission_sampling = AR_EMISSION_SAMPLING_AUTO;
    REQUIRE(scene.materials->set_material(e, key, &m) == AR_OK);
  }
  // The quad instanced once with the material slots `pool` (slot 0 is the one its faces use) and, when
  // `medium` is not 0, that medium inside it.
  void instances(Edit& e, std::vector<ar_key> pool, ar_key medium = 0) {
    std::vector<ar_key> geom{100}, object{1};
    std::vector<ar_xform> xf(1);
    xf[0].m[0] = xf[0].m[5] = xf[0].m[10] = 1.0f;
    std::vector<uint32_t> zeros(1, 0), vis(1, AR_VISIBILITY_CAMERA), moff{0}, mcount{uint32_t(pool.size())};
    std::vector<ar_rgba> colors(1, ar_rgba{{1, 1, 1, 1}});
    std::vector<uint8_t> lg(1, 0);
    ar_instances_desc d = {};
    d.instance_count = 1, d.motion_steps = 1, d.transform_count = 1, d.material_key_count = uint32_t(pool.size());
    d.geom_keys = geom.data(), d.xf = xf.data(), d.object_keys = object.data();
    d.random_ids = d.pass_indices = d.crypto_objects = d.crypto_assets = zeros.data();
    d.colors = colors.data(), d.visibility = vis.data(), d.flags = zeros.data(), d.light_groups = lg.data();
    d.material_offsets = moff.data(), d.material_counts = mcount.data(), d.material_keys = pool.data();
    std::vector<ar_key> media{medium};
    d.medium_keys = medium ? media.data() : nullptr;
    REQUIRE(set_instances(e, &d) == AR_OK);
  }
  // A lit world, emissive material 42 on one instanced quad, an unreferenced image (missing file:
  // the magenta stand-in) and one point lamp.
  void build() {
    const Graph gw(0.5f);
    commit([&](Edit& e) {
      const ar_graph_desc dw = gw.desc();
      ar_world_desc w = {};
      w.surface_graph = 40;
      REQUIRE(scene.materials->set_graph(e, 40, &dw) == AR_OK);
      REQUIRE(scene.materials->set_world(e, &w) == AR_OK);
      material(e, 42, 2.0f);
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
    commit([&](Edit& e) { instances(e, {42}); });
  }
};

uint32_t count(const SceneTableSet& s, TableId id) { return s.counts[static_cast<uint32_t>(id)]; }

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("kg_builder/device table carries the luminance row") {
  Rig r;
  r.build();
  const auto set = acquire_scene_tables(r.scene);
  REQUIRE(set);
  ar::FilmData film{};
  ar::DisplayTarget display{};
  uint32_t active = 0;
  KgSession in;
  in.film = &film, in.display = &display, in.active_pixels = &active;
  const ar::KernelGlobals kg = build_kernel_globals(set.get(), in);
  const ar::Rgb2SpecTable& host = r.scene.materials->table();
  REQUIRE(kg.tables->rgb2spec != nullptr);
  for (int k = 0; k < 3; ++k) {
    CHECK(host.y_row[k] > 0.0f);
    CHECK(kg.tables->rgb2spec->y_row[k] == host.y_row[k]);  // the header copy the kernels read
  }
}

TEST_CASE("kg_builder/no null tables") {
  Rig r;
  r.build();
  const auto set = acquire_scene_tables(r.scene);
  REQUIRE(set);
  const auto lights = set->lights;
  REQUIRE(lights);
  CHECK(set->generation == r.scene.generations().active());
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
                        (const void*)kg.e_spec, (const void*)kg.e_spec->e,
                        (const void*)kg.tables->rgb2spec, (const void*)kg.tables->images,
                        (const void*)kg.tables->srgb_decode, (const void*)kg.tables->observer.cmf,
                        (const void*)kg.scene->instances, (const void*)kg.scene->meshes,
                        (const void*)kg.scene->material_slots, (const void*)kg.scene->material_headers,
                        (const void*)kg.scene->material_constants, (const void*)kg.light_sampler->bins})
    CHECK(p != nullptr);
  CHECK(kg.scene->accel != nullptr);  // the snapshot's traversal structure (114)

  const auto mats = r.scene.materials->arrays();
  const auto imgs = r.scene.materials->images().arrays();
  CHECK(kg.scene->instance_count == 2u);  // the quad and the lamp's record
  CHECK(kg.scene->mesh_count == 1u);
  // The header array ends with the default material, one past the counted headers.
  CHECK(kg.scene->material_header_count == mats->headers.size() - 1u);
  CHECK(mats->headers.size() == mats->keys.size() + 1u);
  CHECK(kg.scene->material_pass_index == mats->pass_index.data());
  CHECK(mats->spectra.empty());                   // no material of this scene holds a TABLE
  CHECK(kg.scene->material_spectra == nullptr);   // null exactly when the arena is empty
  CHECK(kg.scene->material_spectra_count == 0u);
  CHECK(kg.scene->medium_descs == nullptr);  // no medium: null exactly when the table is empty
  CHECK(kg.scene->medium_count == 0u);
  CHECK(kg.scene->instances[0].medium == 0u);
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
  CHECK((acquire_scene_tables(r.scene) == set));
}

TEST_CASE("kg_builder/media table") {
  Rig r;
  r.build();
  const auto before = acquire_scene_tables(r.scene);
  REQUIRE(count(*before, TableId::MEDIA) == 0u);
  ar_medium_desc md{};
  md.sigma_a[0] = 1.0f;
  r.commit([&](Edit& e) { REQUIRE(set_medium(e, 60, &md, r.scene.materials->table()) == AR_OK); });
  // A medium nothing instances is in the table but on no instance.
  auto set = acquire_scene_tables(r.scene);
  REQUIRE(set != before);
  CHECK(count(*set, TableId::MEDIA) == 1u);
  CHECK(set->scene_host.instances[0].medium == 0u);
  r.commit([&](Edit& e) { r.instances(e, {42}, 60); });
  set = acquire_scene_tables(r.scene);
  ar::FilmData film{};
  ar::DisplayTarget display{};
  uint32_t active = 0;
  KgSession in;
  in.film = &film, in.display = &display, in.active_pixels = &active;
  const ar::KernelGlobals kg = build_kernel_globals(set.get(), in);
  CHECK(missing_pointers(kg, *set, false).empty());
  REQUIRE(kg.scene->medium_count == 1u);
  REQUIRE(kg.scene->medium_descs != nullptr);
  CHECK(kg.scene->instances[0].medium == 1u);  // index + 1
  CHECK(kg.scene->instances[1].medium == 0u);  // the lamp record
  const std::shared_ptr<const Medium> stored = r.scene.medium(60);
  REQUIRE(stored);
  CHECK(stored->desc.program == Medium::kNoMediumProgram);  // the store never places a medium
  CHECK(kg.scene->medium_descs[0].program == 0u);           // the table does
  CHECK(kg.scene->medium_descs[0].estimator == stored->desc.estimator);
  // Unchanged: shared. Material-only edits keep the media copy; a medium edit uploads it again.
  CHECK((acquire_scene_tables(r.scene) == set));
  r.commit([&](Edit& e) { r.material(e, 42, 3.0f); });
  const auto after_material = acquire_scene_tables(r.scene);
  CHECK(after_material != set);
  CHECK_FALSE(after_material->uploaded[static_cast<uint32_t>(TableId::MEDIA)]);
  r.commit([&](Edit& e) { REQUIRE(set_medium(e, 60, &md, r.scene.materials->table()) == AR_OK); });
  const auto after_medium = acquire_scene_tables(r.scene);
  CHECK(after_medium->uploaded[static_cast<uint32_t>(TableId::MEDIA)]);
  CHECK(after_medium->uploaded[static_cast<uint32_t>(TableId::INSTANCES)]);  // the indices ride the media list
}

TEST_CASE("kg_builder/medium blocks and programs") {
  // Two media: each block's entry is relocated past the reserved word and the earlier program, and
  // medium_eval through the uploaded tables returns each medium's own coefficients (grey, so exact).
  Rig r;
  r.build();
  ar_medium_desc a{}, b{};
  for (int k = 0; k < 3; ++k) a.sigma_a[k] = 1.0f, b.sigma_a[k] = 2.0f, b.sigma_s[k] = 0.5f;
  r.commit([&](Edit& e) {
    REQUIRE(set_medium(e, 60, &a, r.scene.materials->table()) == AR_OK);
    REQUIRE(set_medium(e, 61, &b, r.scene.materials->table()) == AR_OK);
  });
  const auto set = acquire_scene_tables(r.scene);
  ar::FilmData film{};
  ar::DisplayTarget display{};
  uint32_t active = 0;
  KgSession in;
  in.film = &film, in.display = &display, in.active_pixels = &active;
  const ar::KernelGlobals kg = build_kernel_globals(set.get(), in);
  CHECK(missing_pointers(kg, *set, false).empty());
  const auto media = r.scene.media();
  REQUIRE(media.size() == 2u);
  REQUIRE(count(*set, TableId::MEDIUM_BLOCKS) == 2u);
  const ar::SceneData& s = *kg.scene;
  REQUIRE(s.medium_blocks != nullptr);
  REQUIRE(s.medium_program_words == 1u + media[0]->program.size() + media[1]->program.size());
  CHECK(s.medium_programs[0] == 0u);
  CHECK(s.medium_blocks[0].entry == 1u);
  CHECK(s.medium_blocks[1].entry == 1u + media[0]->program.size());
  const ar::MediumTables mt{s.medium_programs, s.medium_blocks, *kg.tables};
  const float want_a[2] = {1.0f, 2.0f}, want_s[2] = {0.0f, 0.5f};
  for (uint32_t m = 0; m < 2; ++m) {
    ar::VmRegs regs;
    ar::MediumSample ms;
    const ar::Lambda lam{{450.0f, 520.0f, 590.0f, 660.0f}};
    REQUIRE(ar::medium_eval(mt, s.medium_descs[m], ar::Real3{0, 0, 0}, lam, &regs, &ms) == ar::VmStatus::OK);
    for (int i = 0; i < ar::kLanes; ++i) {
      CHECK(ms.sigma_a.v[i] == doctest::Approx(want_a[m]).epsilon(1e-6));
      CHECK(ms.sigma_s.v[i] == doctest::Approx(want_s[m]).epsilon(1e-6));
    }
  }
}

TEST_CASE("kg_builder/medium dropped with its instance still builds") {
  Rig r;
  r.build();
  ar_medium_desc md{};
  r.commit([&](Edit& e) { REQUIRE(set_medium(e, 60, &md, r.scene.materials->table()) == AR_OK); });
  r.commit([&](Edit& e) { r.instances(e, {42}, 60); });
  auto set = acquire_scene_tables(r.scene);
  REQUIRE(count(*set, TableId::MEDIA) == 1u);
  REQUIRE(set->scene_host.instances[0].medium == 1u);
  // The instance loses its medium and the medium goes in one commit. This covers the sequential case only:
  // the pairing under a concurrent activation comes from Scene::snapshot reading media inside with_active.
  r.commit([&](Edit& e) {
    r.instances(e, {42});
    REQUIRE(e.remove(AR_KIND_MEDIUM, 60) == AR_OK);
  });
  REQUIRE_NOTHROW(set = acquire_scene_tables(r.scene));
  CHECK(count(*set, TableId::MEDIA) == 0u);
  CHECK(set->scene_host.instances[0].medium == 0u);
}

TEST_CASE("kg_builder/medium table refuses an unplaced or out of range block") {
  ar::MediumDesc descs[2] = {};
  descs[0].program = 0, descs[1].program = 1;
  CHECK_NOTHROW(check_medium_table(descs, 2));
  descs[1].program = Medium::kNoMediumProgram;
  CHECK_THROWS_WITH_AS(check_medium_table(descs, 2), "medium table entry 1 was never placed (its program is the unplaced sentinel)",
                       std::invalid_argument);
  descs[1].program = 2;
  CHECK_THROWS_WITH_AS(check_medium_table(descs, 2), "medium table entry 1 names block 2, but the table holds 2",
                       std::invalid_argument);
  // The table maker places the store's unplaced media in order, so what it returns passes the check.
  MaterialStore store(std::make_shared<Ledger>());
  ar_medium_desc md{};
  std::vector<std::shared_ptr<const Medium>> media;
  for (ar_key k : {5, 9}) media.push_back(build_medium(k, md, store.table()));
  REQUIRE(media[1]->desc.program == Medium::kNoMediumProgram);
  const std::vector<ar::MediumDesc> table = medium_table(media);
  REQUIRE(table.size() == 2u);
  CHECK(table[0].program == 0u);
  CHECK(table[1].program == 1u);
  const MediumIndex index = medium_index(media);
  CHECK(index.at(5) == 0u);
  CHECK(index.at(9) == 1u);
}

TEST_CASE("kg_builder/a program world carries its program and block") {
  Rig r;
  r.scene.table_cache().bake_size = 16;  // the program world bakes at the latch: kept short on the CPU
  r.build();
  const auto constant = acquire_scene_tables(r.scene);
  REQUIRE(constant);
  CHECK(count(*constant, TableId::WORLD_PROGRAM) == 0u);  // a constant world holds no program
  CHECK(count(*constant, TableId::WORLD_CONSTANTS) == 0u);
  CHECK(constant->scene_host.world_program == nullptr);
  CHECK(constant->scene_host.world_constants == nullptr);
  // LOAD_CTX_P -> TEX_ENVIRONMENT (image 7, the missing-file stand-in) -> EMIT_LE: a program world.
  const int32_t i32[2] = {7, 1};
  const float f32[1] = {1.0f};
  ar_graph_node n[4] = {};
  n[0].op = uint32_t(ar::Op::LOAD_CTX_P);
  n[1].op = uint32_t(ar::Op::TEX_ENVIRONMENT), n[1].i32_first = 0, n[1].i32_count = 2;
  n[2].op = uint32_t(ar::Op::CONST_F32), n[2].f32_first = 0, n[2].f32_count = 1;
  n[3].op = uint32_t(ar::Op::EMIT_LE);
  const ar_graph_link links[3] = {{0, 0, 1, 0}, {1, 0, 3, 0}, {2, 0, 3, 1}};
  ar_graph_desc d = {};
  d.node_count = 4, d.link_count = 3, d.f32_count = 1, d.i32_count = 2;
  d.nodes = n, d.links = links, d.params_f32 = f32, d.params_i32 = i32;
  r.commit([&](Edit& e) { REQUIRE(r.scene.materials->set_graph(e, 40, &d) == AR_OK); });
  const auto set = acquire_scene_tables(r.scene);
  REQUIRE(set);
  CHECK(set != constant);
  const auto mats = r.scene.materials->arrays();
  REQUIRE(!mats->world_program.empty());
  CHECK(count(*set, TableId::WORLD_PROGRAM) == mats->world_program.size());
  CHECK(count(*set, TableId::WORLD_CONSTANTS) == 1u);
  ar::FilmData film{};
  ar::DisplayTarget display{};
  uint32_t active = 0;
  KgSession in;
  in.film = &film, in.display = &display, in.active_pixels = &active;
  const ar::KernelGlobals kg = build_kernel_globals(set.get(), in);
  const std::vector<std::string> missing = missing_pointers(kg, *set, false);
  CHECK_MESSAGE(missing.empty(), (missing.empty() ? std::string() : missing.front()));
  CHECK(kg.scene->world_program == mats->world_program.data());  // read in place on the CPU path
  CHECK(kg.scene->world_program_words == mats->world_program.size());
  CHECK(kg.scene->world_constants == mats->world_constants.data());
  // The light record names the program, and nothing samples it.
  const auto lights = set->lights;
  REQUIRE(lights);
  REQUIRE(lights->world_light != ar::kNoLight);
  CHECK((lights->lights[lights->world_light].flags & ar::kLightWorldProgram) != 0u);
  CHECK(lights->importance[lights->world_light] == 0.0);
  CHECK(lights->pmf[lights->world_light] == 0.0f);
  CHECK(kg.world_light == lights->world_light);
  // An unrelated edit keeps the program in the new set: the material arrays carry it forward.
  r.commit([&](Edit& e) { r.material(e, 43, 1.0f); });
  const auto after = acquire_scene_tables(r.scene);
  CHECK(after->scene_host.world_program_words == mats->world_program.size());
}

TEST_CASE("kg_builder/failed table keeps the previous set") {
  Rig r;
  r.build();
  const auto first = acquire_scene_tables(r.scene);
  // The instance now names material key 99, which nothing compiled; the generation still activates.
  const uint64_t g = r.commit([&](Edit& e) { r.instances(e, {42, 99}); });
  try {
    acquire_scene_tables(r.scene);
    FAIL("the material slots of key 99 should not resolve");
  } catch (const TableUploadFailed& e) {
    CHECK(std::string(e.what()).find("material slots") != std::string::npos);
    CHECK(e.generation() == g);
  }
  CHECK((r.scene.table_cache().current == first));
}

}  // TEST_SUITE

TEST_SUITE("gate") {

// An activation that publishes the instances, then (held) the materials, must never be seen half
// done: a capture racing it sees the generation before or after, and every pool key of the
// instances it sees resolves in the materials it sees.
TEST_CASE("kg_builder/snapshot is one generation") {
  Rig r;
  r.build();
  std::atomic<bool> entered{false};
  SceneSnapshot seen;
  std::thread capture([&] {
    while (!entered.load()) std::this_thread::yield();
    seen = r.scene.snapshot(true);
  });
  auto hold = std::make_shared<EditItem>();
  hold->job = [] {};
  hold->needs_device = false;
  hold->on_active = [&] {
    entered = true;
    // Long enough for the capture to run in the middle if it were not under the tracker's lock.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  };
  const uint64_t g = r.commit([&](Edit& e) {
    r.instances(e, {99});                       // internal item 1: publishes the instances
    REQUIRE(e.stage_internal(hold) == AR_OK);  // internal item 2: holds the activation
    r.material(e, 99, 1.0f);                    // the material pass (internal item 3)
  });
  capture.join();
  CHECK(seen.generation == g);
  REQUIRE(seen.instances);
  REQUIRE(seen.materials);
  for (const ar_key k : seen.instances->material_pool)
    CHECK(std::binary_search(seen.materials->keys.begin(), seen.materials->keys.end(), k));
  REQUIRE(seen.lights);
  CHECK((seen.lights == r.scene.lights()));  // built from the same state
}

// RULINGS 70 in a session: a failed upload keeps the previous set (and its radiance scale), sets
// failed_generation, raises exactly one entry, and the next latch retries.
TEST_CASE("kg_builder/session keeps its set on a failed upload") {
  Rig r;
  r.build();
  auto device = make_cpu_session_device(r.ctx, 1);
  SessionOptions o;
  o.max_width = o.max_height = 4;
  auto s = std::make_unique<Session>(r.scene, *device, o);
  s->start();
  const auto latched = [&](uint64_t g) {
    for (int i = 0; i < 500 && s->progress().active_generation < g; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(s->progress().active_generation >= g);
    double inv = 0.0;
    s->read_film([&](const FilmReadContext& c) { inv = c.film.data().inv_radiance_scale; });  // after the latch
    return inv;
  };
  const auto rejected = [&] {
    const ar_report_info info = s->report_snapshot();
    uint32_t n = 0;
    for (uint32_t i = 0; i < info.entry_count; ++i) {
      ar_report_entry e = {};
      s->report_entry(i, &e);
      n += e.code == AR_REPORT_GENERATION_REJECTED ? 1u : 0u;
    }
    return n;
  };
  const double inv_a = latched(r.scene.generations().active());
  CHECK(inv_a == 1.0 / r.scene.lights()->radiance_scale);  // s = 2^k: exact

  // One generation that changes s (material 42 much brighter) and names the missing key 99.
  const uint64_t g2 = r.commit([&](Edit& e) {
    r.material(e, 42, 200.0f);
    r.instances(e, {42, 99});
  });
  const double inv_b = latched(g2);
  CHECK(inv_b == inv_a);  // the pinned set's s, not the failed generation's
  CHECK(1.0 / r.scene.lights()->radiance_scale != inv_a);
  CHECK(s->progress().failed_generation == g2);
  CHECK(rejected() == 1u);

  // Material 99 arrives: the next latch uploads, and s follows the new set.
  const uint64_t g3 = r.commit([&](Edit& e) { r.material(e, 99, 1.0f); });
  const double inv_c = latched(g3);
  CHECK(inv_c == 1.0 / r.scene.lights()->radiance_scale);  // s = 2^k: exact
  CHECK(inv_c != inv_a);
  CHECK(s->progress().failed_generation == g2);
  CHECK(rejected() == 1u);
  s->close();
  s.reset();
}

}  // TEST_SUITE
