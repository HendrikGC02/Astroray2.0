// SPDX-License-Identifier: Apache-2.0
// 0005 items 14-17 and 43: compile_extension turns a GRATING material into the EXTENSION flag and its
// 64 B ExtParams, refuses what is not built, reports dropped BSDF stores, and the store refuses the
// result until the routing task registers SHADE_SURFACE_EXT.
// Ref: docs/decisions/0005-material-model.md items 14, 15, 16, 17, 43; 0002 D8, D11 (route_hit).
#include <doctest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../../src/host/ledger.h"
#include "../../src/host/material/compiler.h"
#include "../../src/host/material/material_store.h"
#include "host/scene/edit.h"
#include "material/material_header.h"
#include "vm/interp.h"
#include "stages/route_hit.h"

namespace {

using ar::Op;
using astroray::host::compile_extension;
using astroray::host::MaterialOutput;

const ar::Rgb2SpecTable& table() {
  static const auto store = std::make_shared<astroray::host::MaterialStore>(std::make_shared<astroray::host::Ledger>());
  return store->table();
}

struct GB {
  std::vector<ar_graph_node> nodes;
  std::vector<ar_graph_link> links;
  std::vector<float> f;
  std::vector<int32_t> i;
  int add(Op op, std::vector<float> fp = {}, std::vector<int32_t> ip = {}) {
    ar_graph_node n = {};
    n.op = uint32_t(op);
    n.f32_first = uint32_t(f.size());
    n.f32_count = uint32_t(fp.size());
    n.i32_first = uint32_t(i.size());
    n.i32_count = uint32_t(ip.size());
    f.insert(f.end(), fp.begin(), fp.end());
    i.insert(i.end(), ip.begin(), ip.end());
    nodes.push_back(n);
    return int(nodes.size()) - 1;
  }
  void store(float v, ar::RecordSlot slot) {
    const int c = add(Op::CONST_F32, {v});
    const int n = add(Op::STORE_SLOT, {}, {int32_t(slot)});
    links.push_back({uint32_t(c), 0u, uint32_t(n), 0u});
  }
  ar_graph_desc desc() const {
    ar_graph_desc d = {};
    d.node_count = uint32_t(nodes.size());
    d.link_count = uint32_t(links.size());
    d.f32_count = uint32_t(f.size());
    d.i32_count = uint32_t(i.size());
    d.nodes = nodes.data();
    d.links = links.data();
    d.params_f32 = f.data();
    d.params_i32 = i.data();
    return d;
  }
};

ar_material_desc material(uint32_t kind) {
  ar_material_desc d = {};
  d.ext_kind = kind;
  for (uint32_t b = 0; b < sizeof(d.ext_constants); ++b) d.ext_constants[b] = uint8_t(0x11u * (b % 15u) + 7u);
  return d;
}

uint32_t count_code(const MaterialOutput& m, uint32_t code) {
  uint32_t n = 0;
  for (const auto& e : m.entries) n += e.code == code ? 1u : 0u;
  return n;
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("ext_material/compiled grating carries flag and params") {
  GB g;
  g.store(0.5f, ar::RecordSlot::geometry_opacity);
  const ar_material_desc d = material(1);
  const ar_graph_desc gd = g.desc();
  const MaterialOutput m = compile_extension(d, gd, table());
  if (m.magenta) MESSAGE(m.entries[0].detail);
  REQUIRE(!m.magenta);
  CHECK((m.flags & ar::EXTENSION) != 0u);
  CHECK((m.flags & ar::MAY_BE_DELTA) != 0u);
  CHECK((m.flags & ar::NON_OPAQUE) == 0u);
  CHECK(m.compiled.ext_kind == uint32_t(ar::ExtKind::GRATING));
  CHECK(std::memcmp(&m.compiled.ext, d.ext_constants, 64) == 0);
  CHECK(m.entries.empty());  // opacity is kept (item 15)
  // The same without any graph, as the store compiles a material with no surface key.
  const MaterialOutput bare = compile_extension(d, ar_graph_desc{}, table());
  REQUIRE(!bare.magenta);
  CHECK((bare.flags & ar::EXTENSION) != 0u);
}

TEST_CASE("ext_material/unbuilt and unknown kinds refused") {
  const ar_graph_desc none = {};
  for (uint32_t kind : {2u, 3u, 4u, 5u, 6u, 255u}) {
    INFO("kind " << kind);
    const MaterialOutput m = compile_extension(material(kind), none, table());
    CHECK(m.magenta);
    CHECK((m.flags & ar::EXTENSION) == 0u);
    REQUIRE(m.entries.size() == 1);
    CHECK(m.entries[0].code == AR_REPORT_PROGRAM_REJECTED);
    CHECK(m.entries[0].detail.find(kind <= 5 ? "is not built" : "is not in the list") != std::string::npos);
  }
}

TEST_CASE("ext_material/dropped bsdf stores reported per slot") {
  GB g;
  g.store(0.2f, ar::RecordSlot::specular_roughness);
  g.store(0.4f, ar::RecordSlot::coat_weight);
  const ar_graph_desc gd = g.desc();
  const MaterialOutput m = compile_extension(material(1), gd, table());
  REQUIRE(!m.magenta);
  CHECK(count_code(m, AR_REPORT_INPUT_IGNORED) == 2u);
  bool rough = false, coat = false;
  for (const auto& e : m.entries) {
    rough = rough || e.detail.find("specular_roughness") == 0;
    coat = coat || e.detail.find("coat_weight") == 0;
  }
  CHECK(rough);
  CHECK(coat);
}

TEST_CASE("ext_material/routes to ext stage") {
  GB g;
  g.store(1.0f, ar::RecordSlot::geometry_opacity);
  const ar_graph_desc gd = g.desc();
  const MaterialOutput m = compile_extension(material(1), gd, table());
  REQUIRE(!m.magenta);
  // One triangle, one instance, one slot whose header carries the compiled flags.
  std::vector<int32_t> tri_material{0};
  std::vector<ar::DeviceMeshView> meshes(1);
  meshes[0] = {};
  meshes[0].tri_material = uint64_t(reinterpret_cast<uintptr_t>(tri_material.data()));
  meshes[0].triangle_count = 1;
  std::vector<ar::DeviceInstanceRecord> instances(1);
  instances[0] = {};
  instances[0].light_index = ar::kNoLightIndex;
  instances[0].material_offset = 0;
  instances[0].material_count = 1;
  std::vector<uint32_t> slots{0};
  std::vector<ar::MaterialHeader> headers{{0, 0, 0, m.flags}};
  ar::SceneData scene{};
  scene.instances = instances.data(), scene.instance_count = 1;
  scene.meshes = meshes.data(), scene.mesh_count = 1;
  scene.material_slots = slots.data();
  scene.material_headers = headers.data();
  scene.material_header_count = 1;
  ar::KernelGlobals kg{};
  kg.scene = &scene;
  const ar::HitRecord h{1.0f, 0u, 0u, 0.25f, 0.25f};
  CHECK(ar::route_hit(kg, h, 0u) == ar::Stage::SHADE_SURFACE_EXT);
  CHECK(ar::route_hit(kg, h, ar::PATH_KIND) == ar::Stage::SHADE_LIGHT_PATH_EXT);
}

TEST_CASE("ext_material/store refuses until the stage exists") {
  using namespace astroray::host;
  const ar_key M = 30;
  astroray::ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  auto store = std::make_shared<MaterialStore>(std::make_shared<Ledger>());
  auto e = scene.begin_edit();
  REQUIRE(e);
  const ar_material_desc d = material(1);
  REQUIRE(store->set_material(*e, M, &d) == AR_OK);
  uint64_t gen = 0;
  REQUIRE(e->commit(&gen) == AR_OK);
  store->end_edit();
  scene.generations().wait_idle();
  scene.report().snapshot();
  ar_report_info info = {};
  REQUIRE(scene.report().info(&info) == AR_OK);
  std::vector<std::string> rejected;
  for (uint32_t k = 0; k < info.entry_count; ++k) {
    ar_report_entry en = {};
    REQUIRE(scene.report().entry(k, &en) == AR_OK);
    if (en.subject == M && en.code == AR_REPORT_PROGRAM_REJECTED) rejected.emplace_back(reinterpret_cast<const char*>(en.detail));
  }
  REQUIRE(rejected.size() == 1);
  CHECK(rejected[0].find("SHADE_SURFACE_EXT") != std::string::npos);
}

}  // TEST_SUITE("fast")
