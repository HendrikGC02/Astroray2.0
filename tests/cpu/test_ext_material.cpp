// SPDX-License-Identifier: Apache-2.0
// 0005 items 14-17, 23-24 and 43: compile_extension turns a GRATING material's authored block (grating_authored.h)
// into the EXTENSION flag, its ExtParams and a host efficiency table, refuses what is bad or not
// built and reports dropped BSDF stores; the store packs the header's ext word, places each distinct
// table once in the scene TableArena (16 B aligned u32 offsets in ExtParams w[2]) and the CPU session
// device runs SHADE_SURFACE_EXT.
// Ref: docs/decisions/0005-material-model.md items 14-17, 19, 23, 24, 43; 0002 D8, D11 (route_hit).
#include <doctest.h>

#include <cstdint>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../../src/host/ledger.h"
#include "../../src/host/material/compiler.h"
#include "../../src/host/material/grating_authored.h"
#include "../../src/host/material/material_store.h"
#include "device/cpu/cpu_device.h"
#include "host/session/session.h"
#include "host/scene/edit.h"
#include "common/grating_cells.h"
#include "material/ext/grating_table.h"
#include "material/lobes/metal.h"
#include "material/material_header.h"
#include "vm/interp.h"
#include "stages/route_hit.h"
#include "stages/shade_surface_ext.h"

namespace {

using ar::Op;
using astroray::host::compile_extension;
using astroray::host::GratingAuthored;
using astroray::host::GratingAuthoredMode;
using astroray::host::GratingAuthoredProfile;
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

// A valid reflective grating block (0005 items 23-24): a CD, 1.6 um pitch and 120 nm deep
// sinusoidal grooves, orders -2..2, an aluminium-like constant coating.
GratingAuthored good_grating() {
  GratingAuthored g = {};
  g.pitch_nm = 1600.0f;
  g.depth_nm = 120.0f;
  g.coating_n = 1.0f;
  g.coating_k = 6.0f;
  g.mode = uint8_t(GratingAuthoredMode::Reflect);
  g.profile = uint8_t(GratingAuthoredProfile::Sinusoidal);
  g.m_min = -2;
  g.m_max = 2;
  return g;
}

ar_material_desc material(uint32_t kind, const GratingAuthored& g = good_grating()) {
  ar_material_desc d = {};
  d.ext_kind = kind;
  if (kind == uint32_t(ar::ExtKind::GRATING)) {
    std::memcpy(d.ext_constants, &g, sizeof(g));
  } else {
    // Other kinds are refused before their block is read: a pattern in every byte shows it.
    for (uint32_t b = 0; b < sizeof(d.ext_constants); ++b) d.ext_constants[b] = uint8_t(0x11u * (b % 15u) + 7u);
  }
  return d;
}

uint32_t count_code(const MaterialOutput& m, uint32_t code) {
  uint32_t n = 0;
  for (const auto& e : m.entries) n += e.code == code ? 1u : 0u;
  return n;
}

// The largest sum of eta over a cell's orders in a compiled grating table.
double max_cell_sum(const astroray::host::CompileResult& r) {
  const int orders = int(r.ext_table.size() / (size_t(ar::kGratingCosBins) * ar::kGratingWavelengths));
  return astroray::testing::grating_max_cell_sum(r.ext_table, orders);
}

// The store's arrays for the materials set in one edit on a fresh scene.
std::shared_ptr<const astroray::host::MaterialArrays> arrays_of(const std::vector<std::pair<ar_key, ar_material_desc>>& mats) {
  using namespace astroray::host;
  astroray::ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  auto store = std::make_shared<MaterialStore>(std::make_shared<Ledger>());
  auto e = scene.begin_edit();
  REQUIRE(e);
  for (const auto& m : mats) REQUIRE(store->set_material(*e, m.first, &m.second) == AR_OK);
  uint64_t gen = 0;
  REQUIRE(e->commit(&gen) == AR_OK);
  store->end_edit();
  scene.generations().wait_idle();
  return store->arrays();
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("ext_material/grating authored block compiles") {
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
  // The device words come from the authored fields; the generator inputs (depth, coating) are not in them.
  const ar::GratingParams gp = ar::grating_params(m.compiled.ext);
  CHECK(gp.pitch_nm == 1600.0f);
  CHECK(gp.mode == uint32_t(ar::GratingMode::Reflect));
  CHECK(gp.m_min == -2);
  CHECK(gp.m_max == 2);
  CHECK(gp.table == 0u);  // the placeholder until the routing task assigns an arena offset
  CHECK(m.entries.empty());  // opacity is kept (item 15)
  // The table is the builder's for these inputs: 5 orders, and every cell sums to at most 1.
  REQUIRE(m.compiled.ext_table.size() ==
          size_t(5) * ar::kGratingCosBins * ar::kGratingWavelengths);
  CHECK(max_cell_sum(m.compiled) <= 1.0 + 1e-6);
  CHECK(max_cell_sum(m.compiled) > 0.0);
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

TEST_CASE("ext_material/grating bad field is named") {
  const ar_graph_desc none = {};
  struct Bad {
    const char* what;
    GratingAuthored g;
    const char* detail;
  };
  std::vector<Bad> bad;
  auto with = [](auto edit) {
    GratingAuthored g = good_grating();
    edit(g);
    return g;
  };
  bad.push_back({"zero pitch", with([](GratingAuthored& g) { g.pitch_nm = 0.0f; }), "pitch_nm"});
  bad.push_back({"negative pitch", with([](GratingAuthored& g) { g.pitch_nm = -1.0f; }), "pitch_nm"});
  bad.push_back({"nan pitch", with([](GratingAuthored& g) { g.pitch_nm = std::nanf(""); }), "pitch_nm"});
  bad.push_back({"zero depth", with([](GratingAuthored& g) { g.depth_nm = 0.0f; }), "depth_nm"});
  bad.push_back({"inf depth", with([](GratingAuthored& g) { g.depth_nm = HUGE_VALF; }), "depth_nm"});
  bad.push_back({"negative coating_n", with([](GratingAuthored& g) { g.coating_n = -0.1f; }), "coating_n"});
  bad.push_back({"index-matched coating", with([](GratingAuthored& g) { g.coating_n = 1.0f; g.coating_k = 0.0f; }),
                 "index-matched coating reflects nothing"});
  bad.push_back({"nan coating_k", with([](GratingAuthored& g) { g.coating_k = std::nanf(""); }), "coating_k"});
  bad.push_back({"mode 2", with([](GratingAuthored& g) { g.mode = 2; }), "mode 2"});
  bad.push_back({"profile 3", with([](GratingAuthored& g) { g.profile = 3; }), "profile 3"});
  bad.push_back({"m_max 9", with([](GratingAuthored& g) { g.m_max = 9; }), "m_max"});
  bad.push_back({"m_min -9", with([](GratingAuthored& g) { g.m_min = -9; }), "m_min"});
  bad.push_back({"m_min > m_max", with([](GratingAuthored& g) { g.m_min = 2; g.m_max = 1; }), "m_min 2 exceeds m_max 1"});
  for (const Bad& b : bad) {
    INFO(b.what);
    const MaterialOutput m = compile_extension(material(1, b.g), none, table());
    CHECK(m.magenta);
    CHECK((m.flags & ar::EXTENSION) == 0u);
    REQUIRE(m.entries.size() == 1);
    CHECK(m.entries[0].code == AR_REPORT_PROGRAM_REJECTED);
    CHECK(m.entries[0].detail.find(b.detail) != std::string::npos);
  }
  // Every reserved byte is required zero, not only the first.
  for (size_t i = 0; i < sizeof(GratingAuthored::reserved); ++i) {
    GratingAuthored g = good_grating();
    g.reserved[i] = 1;
    const MaterialOutput m = compile_extension(material(1, g), none, table());
    INFO("reserved byte " << i);
    REQUIRE(m.magenta);
    CHECK(m.entries[0].detail.find("reserved byte " + std::to_string(20 + i)) != std::string::npos);
  }
  // Valid choices that are not built are refused as such, through the same path.
  GratingAuthored tr = good_grating();
  tr.mode = uint8_t(GratingAuthoredMode::Transmit);
  MaterialOutput m = compile_extension(material(1, tr), none, table());
  REQUIRE(m.magenta);
  REQUIRE(m.entries.size() == 1);
  CHECK(m.entries[0].code == AR_REPORT_PROGRAM_REJECTED);
  CHECK(m.entries[0].detail.find("transmission grating not built") != std::string::npos);
  for (uint8_t profile : {uint8_t(GratingAuthoredProfile::Blazed), uint8_t(GratingAuthoredProfile::Csv)}) {
    GratingAuthored p = good_grating();
    p.profile = profile;
    m = compile_extension(material(1, p), none, table());
    REQUIRE(m.magenta);
    REQUIRE(m.entries.size() == 1);
    CHECK(m.entries[0].detail.find("not built") != std::string::npos);
  }
  // The edges of the ranges compile.
  GratingAuthored edge = good_grating();
  edge.m_min = -8;
  edge.m_max = 8;
  CHECK(!compile_extension(material(1, edge), none, table()).magenta);
}

TEST_CASE("ext_material/grating out of range pitch reported once") {
  const ar_graph_desc none = {};
  GratingAuthored g = good_grating();
  g.pitch_nm = 740.0f;  // DVD class: h/d = 0.16
  const MaterialOutput m = compile_extension(material(1, g), none, table());
  CHECK(m.magenta);
  REQUIRE(m.entries.size() == 1);
  CHECK(m.entries[0].code == AR_REPORT_GRATING_TABLE_LIMIT);
  CHECK(m.entries[0].detail.find("scalar theory out of range") != std::string::npos);
  CHECK(m.compiled.ext_table.empty());
}

TEST_CASE("ext_material/grating equal blocks give equal tables") {
  const ar_graph_desc none = {};
  const MaterialOutput a = compile_extension(material(1), none, table());
  const MaterialOutput b = compile_extension(material(1), none, table());
  REQUIRE(!a.magenta);
  REQUIRE(!b.magenta);
  REQUIRE(!a.compiled.ext_table.empty());
  CHECK(a.compiled.ext_table.size() == b.compiled.ext_table.size());
  CHECK(std::memcmp(a.compiled.ext_table.data(), b.compiled.ext_table.data(), a.compiled.ext_table.size() * sizeof(float)) == 0);
  // The table follows the generator inputs: a deeper groove changes it.
  GratingAuthored deeper = good_grating();
  deeper.depth_nm = 140.0f;
  const MaterialOutput c = compile_extension(material(1, deeper), none, table());
  REQUIRE(!c.magenta);
  CHECK(std::memcmp(a.compiled.ext_table.data(), c.compiled.ext_table.data(), a.compiled.ext_table.size() * sizeof(float)) != 0);
}

TEST_CASE("ext_material/grating no coating is reflectance one and reported") {
  const ar_graph_desc none = {};
  GratingAuthored g = good_grating();
  g.coating_n = 0.0f;
  g.coating_k = 0.0f;
  const MaterialOutput m = compile_extension(material(1, g), none, table());
  REQUIRE(!m.magenta);
  CHECK(count_code(m, AR_REPORT_INPUT_IGNORED) == 1u);
  CHECK(std::isfinite(max_cell_sum(m.compiled)));
  // A lossless coating reaches the Bessel sum bound, a metal sits below it.
  const MaterialOutput metal = compile_extension(material(1), none, table());
  CHECK(max_cell_sum(m.compiled) > max_cell_sum(metal.compiled));
  CHECK(count_code(metal, AR_REPORT_INPUT_IGNORED) == 0u);
}

TEST_CASE("ext_material/grating coating scales the table by the conductor Fresnel") {
  const ar_graph_desc none = {};
  GratingAuthored lossless = good_grating();
  lossless.coating_n = 0.0f;
  lossless.coating_k = 0.0f;
  const MaterialOutput coated = compile_extension(material(1), none, table());  // (n, k) = (1, 6)
  const MaterialOutput bare = compile_extension(material(1, lossless), none, table());
  REQUIRE(!coated.magenta);
  REQUIRE(!bare.magenta);
  REQUIRE(coated.compiled.ext_table.size() == bare.compiled.ext_table.size());
  // Same cells, same wavelength-independent factor: eta_coated = F(cos) * eta_lossless, F the
  // unpolarised conductor reflectance. c = 0 is grazing (phase 0, all light in order 0); the
  // orders are -2..2, so o = 2 is m = 0.
  const struct { int o, c, w; } cells[] = {{2, 0, 0}, {2, 0, 94}, {2, 15, 10}, {2, 8, 50}, {3, 15, 30}, {3, 8, 40}, {1, 12, 20}, {4, 15, 5}};
  for (const auto& cell : cells) {
    const size_t i = ar::grating_table_index(cell.o, cell.c, cell.w);
    const double plain = bare.compiled.ext_table[i];
    INFO("order index " << cell.o << " cos bin " << cell.c << " wavelength bin " << cell.w);
    REQUIRE(plain > 1e-3);  // a cell where the ratio is meaningful
    const double want = ar::fresnel_conductor_scalar<double>(double(cell.c) / double(ar::kGratingCosBins - 1), 1.0, 6.0);
    CHECK(double(coated.compiled.ext_table[i]) / plain == doctest::Approx(want).epsilon(1e-4));
  }
  // Grazing incidence is a real cell of the table: order 0 carries all light there, so its value is F(0).
  CHECK(double(coated.compiled.ext_table[ar::grating_table_index(2, 0, 0)]) ==
        doctest::Approx(ar::fresnel_conductor_scalar<double>(0.0, 1.0, 6.0)).epsilon(1e-5));
}

TEST_CASE("ext_material/ext params check of the device words") {
  using astroray::host::ext_params_valid;
  const uint32_t grating = uint32_t(ar::ExtKind::GRATING);
  auto words = [](auto edit) {
    ar::GratingParams g = {};
    g.pitch_nm = 1600.0f;
    g.mode = uint32_t(ar::GratingMode::Reflect);
    g.m_min = -2;
    g.m_max = 2;
    edit(g);
    return ar::grating_ext_params(g);
  };
  auto cauchy = [&](float nd, float vd, float scale) {
    return words([=](ar::GratingParams& g) {
      g.mode = uint32_t(ar::GratingMode::Transmit);
      g.substrate_ior.kind = uint8_t(ar::SpectrumKind::CAUCHY_ABBE);
      g.substrate_ior.semantic = uint8_t(ar::SpectrumSemantic::IOR);
      g.substrate_ior.p[0] = nd;
      g.substrate_ior.p[1] = vd;
      g.substrate_ior.p[2] = scale;
    });
  };
  std::string why;
  CHECK(ext_params_valid(grating, words([](ar::GratingParams&) {}), &why));
  CHECK(ext_params_valid(7u, ar::ExtParams{}, &why));  // other kinds have no check
  struct Bad {
    const char* what;
    ar::ExtParams e;
    const char* text;
  };
  const Bad bad[] = {
      {"zero pitch", words([](ar::GratingParams& g) { g.pitch_nm = 0.0f; }), "pitch_nm"},
      {"nan pitch", words([](ar::GratingParams& g) { g.pitch_nm = std::nanf(""); }), "pitch_nm"},
      {"m_max 9", words([](ar::GratingParams& g) { g.m_max = 9; }), "orders"},
      {"m_min -9", words([](ar::GratingParams& g) { g.m_min = -9; }), "orders"},
      {"m_min > m_max", words([](ar::GratingParams& g) { g.m_min = 2; g.m_max = 1; }), "orders"},
      {"mode 2", words([](ar::GratingParams& g) { g.mode = 2u; }), "mode"},
      {"transmit without substrate", words([](ar::GratingParams& g) { g.mode = 1u; }), "substrate"},
      {"Abbe number 0", cauchy(1.5f, 0.0f, 1.0f), "Abbe number"},
      {"dispersion scale negative", cauchy(1.5f, 30.0f, -1.0f), "dispersion scale"},
  };
  for (const Bad& b : bad) {
    INFO(b.what);
    why.clear();
    CHECK(!ext_params_valid(grating, b.e, &why));
    CHECK(why.find(b.text) != std::string::npos);
  }
  CHECK(ext_params_valid(grating, cauchy(1.5f, 30.0f, 1.0f), &why));
  CHECK(ext_params_valid(grating, words([](ar::GratingParams& g) {
          g.m_min = -8;
          g.m_max = 8;
        }),
                         &why));
}

TEST_CASE("ext_material/grating inputs reach no device table") {
  // The device sees the grating through its ExtParams words and its efficiency table only: depth,
  // coating and profile are in neither (item 24), and the words are compile_extension's with the
  // arena offset in w[2].
  const auto grating = arrays_of({{30, material(1)}});
  REQUIRE(grating);
  const ar_graph_desc none = {};
  const MaterialOutput m = compile_extension(material(1), none, table());
  REQUIRE(!m.magenta);
  REQUIRE(grating->ext_params.size() == 1u);
  CHECK(std::memcmp(&grating->ext_params[0], &m.compiled.ext, sizeof(ar::ExtParams)) == 0);  // w[2] = offset 0
  CHECK(grating->table_arena.size() == m.compiled.ext_table.size() * sizeof(float));
  CHECK(std::memcmp(grating->table_arena.data(), m.compiled.ext_table.data(), grating->table_arena.size()) == 0);
  for (int w = 9; w < 16; ++w) CHECK(grating->ext_params[0].w[w] == 0u);
  CHECK(grating->spectra.empty());
}

TEST_CASE("ext_material/dropped bsdf stores reported per slot") {
  GB g;
  // Two stores to one ignored slot: an OpenPBR graph would be invalid, an extension reports it once.
  g.store(0.2f, ar::RecordSlot::specular_roughness);
  g.store(0.3f, ar::RecordSlot::specular_roughness);
  g.store(0.4f, ar::RecordSlot::coat_weight);
  g.store(0.5f, ar::RecordSlot::geometry_opacity);
  const ar_graph_desc gd = g.desc();
  const MaterialOutput m = compile_extension(material(1), gd, table());
  REQUIRE(!m.magenta);
  CHECK(count_code(m, AR_REPORT_INPUT_IGNORED) == 2u);
  uint32_t rough = 0, coat = 0;
  for (const auto& e : m.entries) {
    rough += e.detail.find("specular_roughness") == 0 ? 1u : 0u;
    coat += e.detail.find("coat_weight") == 0 ? 1u : 0u;
  }
  CHECK(rough == 1u);
  CHECK(coat == 1u);
}

TEST_CASE("ext_material/ignored inputs do not reach the program") {
  // A refused SpectrumRef (BLACKBODY into a reflectance slot) and an image, both on ignored slots.
  GB g;
  const int bb = g.add(Op::BLACKBODY, {6500.0f, 1.0f});
  const int s1 = g.add(Op::STORE_SLOT, {}, {int32_t(ar::RecordSlot::coat_color)});
  g.links.push_back({uint32_t(bb), 0u, uint32_t(s1), 0u});
  const int uv = g.add(Op::LOAD_CTX_UV, {}, {0});
  const int t = g.add(Op::TEX_IMAGE, {}, {7, 0});
  g.links.push_back({uint32_t(uv), 0u, uint32_t(t), 0u});
  const int s2 = g.add(Op::STORE_SLOT, {}, {int32_t(ar::RecordSlot::base_color)});
  g.links.push_back({uint32_t(t), 0u, uint32_t(s2), 0u});
  g.store(0.5f, ar::RecordSlot::geometry_opacity);
  const ar_graph_desc gd = g.desc();
  CHECK(astroray::host::compile_material(gd, table()).magenta);  // the same graph is refused as OpenPBR
  const MaterialOutput m = compile_extension(material(1), gd, table());
  REQUIRE(!m.magenta);
  CHECK(m.entries.size() == 2);
  CHECK(count_code(m, AR_REPORT_INPUT_IGNORED) == 2u);
  CHECK(m.images.empty());
  CHECK(m.compiled.missing_images.empty());
}

TEST_CASE("ext_material/mix under extension refused") {
  GB g;
  g.store(0.5f, ar::RecordSlot::geometry_opacity);
  const int mix = g.add(Op::BRANCH_MIX, {}, {0});
  const ar_graph_desc gd = g.desc();
  const MaterialOutput m = compile_extension(material(1), gd, table());
  CHECK(m.magenta);
  CHECK((m.flags & ar::EXTENSION) == 0u);
  REQUIRE(m.entries.size() == 1);
  CHECK(m.entries[0].code == AR_REPORT_PROGRAM_REJECTED);
  CHECK(m.entries[0].detail.find("node " + std::to_string(mix) + " (Mix Shader)") == 0);
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

TEST_CASE("ext_material/grating compiles and routes") {
  using namespace astroray::host;
  // Two distinct gratings (another groove depth gives another table) and a third equal to the first: the
  // store keeps every one (none is magenta), packs kind and index into each header, and places the
  // two distinct tables once each at distinct 16 B aligned offsets.
  GratingAuthored other = good_grating();
  other.depth_nm = 90.0f;
  const auto a = arrays_of({{30, material(1)}, {31, material(1, other)}, {32, material(1)}});
  REQUIRE(a);
  REQUIRE(a->keys.size() == 3u);
  REQUIRE(a->ext_params.size() == 3u);
  for (uint32_t i = 0; i < 3u; ++i) {
    INFO("material " << a->keys[i]);
    CHECK(ar::ext_kind(a->headers[i]) == uint32_t(ar::ExtKind::GRATING));
    CHECK(ar::ext_index(a->headers[i]) == i);
    CHECK((a->headers[i].flags & ar::EXTENSION) != 0u);
  }
  CHECK(a->headers[3].ext == 0u);  // the default material
  const uint32_t o0 = ar::grating_params(a->ext_params[0]).table, o1 = ar::grating_params(a->ext_params[1]).table;
  CHECK(o0 == 0u);
  CHECK(o1 != o0);
  CHECK(o1 % 16u == 0u);
  CHECK(ar::grating_params(a->ext_params[2]).table == o0);  // an equal table is stored once
  const size_t table_bytes = size_t(5) * ar::kGratingCosBins * ar::kGratingWavelengths * sizeof(float);
  CHECK(a->table_arena.size() == o1 + table_bytes);
  // The second table at its offset is the second material's own.
  const ar_graph_desc none = {};
  const MaterialOutput m1 = compile_extension(material(1, other), none, table());
  REQUIRE(m1.compiled.ext_table.size() * sizeof(float) == table_bytes);
  REQUIRE(a->table_arena.size() >= size_t(o1) + table_bytes);
  CHECK(std::memcmp(a->table_arena.data() + o1, m1.compiled.ext_table.data(), table_bytes) == 0);
  // It routes to a stage the session device runs.
  astroray::ContextState ctx;
  std::shared_ptr<Device> dev = make_cpu_session_device(ctx, 1);
  CHECK(static_cast<const CpuDevice&>(*dev).stage_fn(ar::Stage::SHADE_SURFACE_EXT) == &cpu_stage_shade_surface_ext);
}

TEST_CASE("ext_material/a table past the arena limit renders magenta") {
  // The arena limit lowered to one table's bytes: the first of two distinct gratings fits at offset 0,
  // the second would end past it, so the store renders it magenta (no EXTENSION, ext word 0, no
  // ExtParams) and raises GRATING_TABLE_LIMIT naming the arena (0001's row, 0005 item 16).
  using namespace astroray::host;
  const size_t table_bytes = size_t(5) * ar::kGratingCosBins * ar::kGratingWavelengths * sizeof(float);
  astroray::ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  auto store = std::make_shared<MaterialStore>(std::make_shared<Ledger>());
  store->set_table_arena_limit(table_bytes);
  GratingAuthored other = good_grating();
  other.depth_nm = 90.0f;
  auto e = scene.begin_edit();
  REQUIRE(e);
  const ar_material_desc a = material(1), b = material(1, other);
  REQUIRE(store->set_material(*e, 30, &a) == AR_OK);
  REQUIRE(store->set_material(*e, 31, &b) == AR_OK);
  uint64_t gen = 0;
  REQUIRE(e->commit(&gen) == AR_OK);
  store->end_edit();
  scene.generations().wait_idle();
  const auto arr = store->arrays();
  REQUIRE(arr->keys.size() == 2u);
  CHECK(arr->ext_params.size() == 1u);
  CHECK(arr->table_arena.size() == table_bytes);
  CHECK(ar::ext_kind(arr->headers[0]) == uint32_t(ar::ExtKind::GRATING));
  CHECK((arr->headers[0].flags & ar::EXTENSION) != 0u);
  CHECK(arr->headers[1].ext == 0u);
  CHECK((arr->headers[1].flags & ar::EXTENSION) == 0u);
  scene.report().snapshot();
  ar_report_info info = {};
  REQUIRE(scene.report().info(&info) == AR_OK);
  std::vector<std::string> limit;
  for (uint32_t k = 0; k < info.entry_count; ++k) {
    ar_report_entry en = {};
    REQUIRE(scene.report().entry(k, &en) == AR_OK);
    if (en.code == AR_REPORT_GRATING_TABLE_LIMIT) {
      CHECK(en.subject == 31u);
      limit.emplace_back(reinterpret_cast<const char*>(en.detail));
    }
  }
  REQUIRE(limit.size() == 1u);
  CHECK(limit[0] == "scene TableArena past 4 GiB (u32 offsets)");
}

TEST_CASE("ext_material/table arena placement and its limit") {
  using astroray::host::place_table;
  std::vector<uint8_t> arena;
  std::map<std::vector<float>, uint32_t> placed;
  const std::vector<float> t1{1.0f, 2.0f, 3.0f}, t2{4.0f, 5.0f};
  uint32_t o = 99;
  REQUIRE(place_table(t1, 64, &arena, &placed, &o));
  CHECK(o == 0u);
  REQUIRE(place_table(t2, 64, &arena, &placed, &o));
  CHECK(o == 16u);  // 12 bytes, padded to the next 16 B boundary
  CHECK(arena.size() == 24u);
  for (int b = 12; b < 16; ++b) CHECK(arena[size_t(b)] == 0u);
  REQUIRE(place_table(t1, 64, &arena, &placed, &o));
  CHECK(o == 0u);  // stored once
  // A table that would end past the limit is refused and changes nothing.
  const std::vector<float> t3(9, 0.5f);  // 36 B at offset 32 ends at 68
  o = 77;
  CHECK(!place_table(t3, 64, &arena, &placed, &o));
  CHECK(o == 77u);
  CHECK(arena.size() == 24u);
  CHECK(placed.size() == 2u);
  REQUIRE(place_table(t3, 68, &arena, &placed, &o));  // ending exactly at the limit fits
  CHECK(o == 32u);
  CHECK(astroray::host::kTableArenaLimit == 0xffffffffull);  // u32 offsets and byte count
}

}  // TEST_SUITE("fast")
