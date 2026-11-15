// SPDX-License-Identifier: Apache-2.0
// 0005 items 40 and 43: what the compiler hands other stages (MAY_BE_DELTA, the constant flags,
// the emission estimate) and the unsupported-node policy (magenta plus one entry per node type,
// 0001 V15 part). The gate case times the B-24 compile row on a fixed 100-node graph.
// Ref: docs/decisions/0005-material-model.md items 4, 40, 43; 0001 D2.11; 0009 d13 B-24.
#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "../../src/host/ledger.h"
#include "../../src/host/material/compiler.h"
#include "../../src/host/material/material_store.h"
#include "material/material_header.h"
#include "vm/interp.h"

namespace {

using ar::Op;
using astroray::host::compile_material;
using astroray::host::MaterialOutput;

const ar::Rgb2SpecTable& table() {
  static const auto store = std::make_shared<astroray::host::MaterialStore>(std::make_shared<astroray::host::Ledger>());
  return store->table();
}

// Builds an ar_graph_desc from calls, as the addon would.
struct GB {
  std::vector<ar_graph_node> nodes;
  std::vector<ar_graph_link> links;
  std::vector<float> f;
  std::vector<int32_t> i;
  std::vector<uint8_t> s;
  uint32_t strings = 0;
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
  int unsupported(const std::string& type) {
    const int n = add(Op::UNSUPPORTED);
    nodes[size_t(n)].string_first = strings++;
    nodes[size_t(n)].string_count = 1;
    s.insert(s.end(), type.begin(), type.end());
    s.push_back(0);
    return n;
  }
  void link(int from, int to, uint32_t socket) { links.push_back({uint32_t(from), 0u, uint32_t(to), socket}); }
  int store(int src, ar::RecordSlot slot) {
    const int n = add(Op::STORE_SLOT, {}, {int32_t(slot)});
    link(src, n, 0);
    return n;
  }
  int op2(Op op, int a, int b) {
    const int n = add(op);
    link(a, n, 0);
    link(b, n, 1);
    return n;
  }
  MaterialOutput compile() const {
    ar_graph_desc d = {};
    d.node_count = uint32_t(nodes.size());
    d.link_count = uint32_t(links.size());
    d.f32_count = uint32_t(f.size());
    d.i32_count = uint32_t(i.size());
    d.string_bytes_count = uint32_t(s.size());
    d.nodes = nodes.data();
    d.links = links.data();
    d.params_f32 = f.data();
    d.params_i32 = i.data();
    d.string_bytes = s.data();
    return compile_material(d, table());
  }
};

// A per-point colour: image 7 sampled at UV set 0.
int tex(GB& g) {
  const int uv = g.add(Op::LOAD_CTX_UV, {}, {0});
  const int t = g.add(Op::TEX_IMAGE, {}, {7});
  g.link(uv, t, 0);
  return t;
}

MaterialOutput roughness(float r) {
  GB g;
  g.store(g.add(Op::CONST_F32, {r}), ar::RecordSlot::specular_roughness);
  return g.compile();
}

bool same_ref(const ar::SpectrumRef& a, const ar::SpectrumRef& b) {
  return a.kind == b.kind && a.semantic == b.semantic && a.p[0] == b.p[0] && a.p[1] == b.p[1] && a.p[2] == b.p[2] &&
         a.p[3] == b.p[3];
}

ar::SpectrumRef magenta_ref() {
  ar::SpectrumRef r = {};
  uint32_t flags = 0;
  ar::make_rgb_ref(table(), ar::Rgb{{ar::Real(1), ar::Real(0), ar::Real(1)}}, ar::SpectrumSemantic::REFLECTANCE, &r, &flags);
  return r;
}

size_t count(const MaterialOutput& m, uint32_t code) {
  return size_t(std::count_if(m.entries.begin(), m.entries.end(), [&](const auto& e) { return e.code == code; }));
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("compiler_outputs/may be delta constant roughness") {
  // alpha = roughness^2 against lobe_is_delta's 1e-3 (0005 items 4, 43).
  CHECK((roughness(0.0f).flags & ar::MAY_BE_DELTA) != 0);
  CHECK((roughness(1e-4f).flags & ar::MAY_BE_DELTA) != 0);
  CHECK((roughness(0.5f).flags & ar::MAY_BE_DELTA) == 0);
  // A coat that can be sampled counts with its own roughness; one of weight 0 cannot be sampled.
  GB coat;
  coat.store(coat.add(Op::CONST_F32, {0.5f}), ar::RecordSlot::specular_roughness);
  coat.store(coat.add(Op::CONST_F32, {1.0f}), ar::RecordSlot::coat_weight);
  CHECK((coat.compile().flags & ar::MAY_BE_DELTA) != 0);  // coat_roughness defaults to 0
}

TEST_CASE("compiler_outputs/may be delta textured roughness") {
  // M1a ops give no per-point scalar, so the rule is driven through a compiled result whose
  // roughness slots are marked as stored per point, over constants that alone would clear it.
  astroray::host::CompileResult r = roughness(0.5f).compiled;
  REQUIRE((astroray::host::material_flags(r) & ar::MAY_BE_DELTA) == 0);
  r.runtime_slots = uint64_t(1) << uint32_t(ar::RecordSlot::specular_roughness);
  CHECK((astroray::host::material_flags(r) & ar::MAY_BE_DELTA) != 0);
  r.runtime_slots = uint64_t(1) << uint32_t(ar::RecordSlot::coat_weight);
  r.block.coat_roughness = 0.5f;
  CHECK((astroray::host::material_flags(r) & ar::MAY_BE_DELTA) == 0);
  r.runtime_slots |= uint64_t(1) << uint32_t(ar::RecordSlot::coat_roughness);
  CHECK((astroray::host::material_flags(r) & ar::MAY_BE_DELTA) != 0);
}

TEST_CASE("compiler_outputs/emission estimate constant") {
  GB g;
  g.op2(Op::EMIT_LE, g.add(Op::CONST_F32X3, {1.0f, 0.5f, 0.25f}), g.add(Op::CONST_F32, {3.0f}));
  const MaterialOutput m = g.compile();
  REQUIRE(m.emission_estimate.size() == 1);
  CHECK(m.emission_estimate[0].strength == 3.0f);
  CHECK(same_ref(m.emission_estimate[0].color, m.compiled.block.emission_color));  // exact: the stored spectrum
  CHECK((m.flags & (ar::CONSTANT_SURFACE | ar::CONSTANT_EMISSION)) == (ar::CONSTANT_SURFACE | ar::CONSTANT_EMISSION));
  GB dark;
  dark.op2(Op::EMIT_LE, dark.add(Op::CONST_F32X3, {1.0f, 1.0f, 1.0f}), dark.add(Op::CONST_F32, {0.0f}));
  CHECK(dark.compile().emission_estimate.empty());  // zero only when emission folds to 0
  CHECK(roughness(0.5f).emission_estimate.empty());
}

TEST_CASE("compiler_outputs/emission estimate textured") {
  GB g;
  g.op2(Op::EMIT_LE, tex(g), g.add(Op::CONST_F32, {2.0f}));
  const MaterialOutput m = g.compile();
  REQUIRE(m.emission_estimate.size() == 1);
  // The texture's input is bounded by 1 until images carry their mean (task 060).
  CHECK(m.emission_estimate[0].color.kind == uint8_t(ar::SpectrumKind::CONSTANT));
  CHECK(m.emission_estimate[0].color.p[0] == 1.0f);
  CHECK(m.emission_estimate[0].strength == 2.0f);
  CHECK((m.flags & ar::CONSTANT_EMISSION) == 0);
  CHECK(!m.magenta);
}

TEST_CASE("compiler_outputs/two node types two entries") {
  GB g;
  g.unsupported("ShaderNodeBevel");
  g.unsupported("ShaderNodeBevel");
  g.unsupported("ShaderNodeAmbientOcclusion");
  g.store(g.add(Op::CONST_F32, {0.5f}), ar::RecordSlot::specular_roughness);
  const MaterialOutput m = g.compile();
  CHECK(m.magenta);
  REQUIRE(m.entries.size() == 2);
  CHECK(count(m, AR_REPORT_NODE_UNSUPPORTED) == 2);
  CHECK(m.entries[0].detail == "ShaderNodeBevel");
  CHECK(m.entries[1].detail == "ShaderNodeAmbientOcclusion");
  // Diffuse magenta and nothing else (0005 item 40).
  CHECK(same_ref(m.compiled.block.base_color, magenta_ref()));
  CHECK(m.compiled.block.specular_weight == 0.0f);
  CHECK(m.compiled.program.empty());
}

TEST_CASE("compiler_outputs/compile error is magenta with one PROGRAM_REJECTED") {
  GB g;
  g.store(g.add(Op::CONST_F32, {0.1f}), ar::RecordSlot::base_metalness);
  g.store(g.add(Op::CONST_F32, {0.2f}), ar::RecordSlot::base_metalness);
  const MaterialOutput m = g.compile();
  CHECK(m.magenta);
  REQUIRE(m.entries.size() == 1);
  CHECK(m.entries[0].code == AR_REPORT_PROGRAM_REJECTED);
  CHECK(m.entries[0].detail.find("stored twice") != std::string::npos);
  CHECK(same_ref(m.compiled.block.base_color, magenta_ref()));
}

TEST_CASE("compiler_outputs/clamped reflectance is approximated") {
  GB g;
  g.store(g.add(Op::CONST_F32X3, {1.5f, 0.2f, 0.2f}), ar::RecordSlot::base_color);
  const MaterialOutput m = g.compile();
  CHECK(!m.magenta);
  REQUIRE(m.entries.size() == 1);
  CHECK(m.entries[0].code == AR_REPORT_NODE_APPROXIMATED);
  CHECK(m.entries[0].detail == "RGB clamped");
}

}  // TEST_SUITE("fast")

TEST_SUITE("gate") {

// B-24 compile row (0009 d13): tools/check.ps1 gate reads the printed line and compares it with
// the limit in tools/budgets.toml; this case only measures.
TEST_CASE("compiler_outputs/B-24 compile time of the 100-node fixture") {
  GB g;
  const int t = tex(g);  // 2 nodes
  int v = t;
  for (int k = 0; k < 48; ++k) {  // 96 nodes
    const float c = 0.5f + 0.01f * float(k);
    v = g.op2(Op::MUL_F32X3, v, g.add(Op::CONST_F32X3, {c, 1.0f - c, 0.75f}));
  }
  v = g.op2(Op::ADD_F32X3, v, t);  // 1 node
  g.store(v, ar::RecordSlot::base_color);  // 1 node
  REQUIRE(g.nodes.size() == 100);
  REQUIRE(!g.compile().magenta);
  std::vector<double> ms;
  for (int run = 0; run < 101; ++run) {
    const auto t0 = std::chrono::steady_clock::now();
    const MaterialOutput m = g.compile();
    const auto t1 = std::chrono::steady_clock::now();
    REQUIRE(!m.magenta);
    ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
  }
  std::nth_element(ms.begin(), ms.begin() + 50, ms.end());
  std::printf("B-24 compile: %.4f ms per 100 nodes (median of 101)\n", ms[50]);
  std::fflush(stdout);
}

}  // TEST_SUITE("gate")
