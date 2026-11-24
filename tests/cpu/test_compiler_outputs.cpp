// SPDX-License-Identifier: Apache-2.0
// 0005 items 32, 38, 40 and 43: what the compiler hands other stages (MAY_BE_DELTA, NON_OPAQUE, the
// constant flags, the emission estimate, the opacity entry), the unsupported-node policy (magenta
// plus one entry per node type, 0001 V15 part) and Verification 6's register-file reuse clause for
// the three entries of one compiled material. The gate case times the B-24 compile row on a fixed
// 100-node graph.
// Ref: docs/decisions/0005-material-model.md items 4, 32, 38, 40, 43, Verification 6; 0001 D2.11;
// 0009 d13 B-24.
#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <future>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../src/host/data_resource.h"
#include "../../src/host/ledger.h"
#include "../../src/host/material/compiler.h"
#include "../../src/host/light/light_builder.h"
#include "../../src/host/material/material_store.h"
#include "../../src/host/spectral/blackbody.h"
#include "../../src/host/material/op_meta.h"
#include "host/scene/edit.h"
#include "material/material_header.h"
#include "sample/sobol.h"
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
  MaterialOutput compile() const { return compile_material(desc(), table()); }
  ar_graph_desc desc() const {
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
    return d;
  }
};

// A per-point colour: image 7 sampled at UV set 0.
int tex(GB& g) {
  const int uv = g.add(Op::LOAD_CTX_UV, {}, {0});
  const int t = g.add(Op::TEX_IMAGE, {}, {7, 0});
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

uint32_t bits(float f) { return ar::bit_cast<uint32_t>(f); }

// Channel `ch` of a colour node: the per-point scalar an opacity is authored from.
int channel(GB& g, int colour, int32_t ch) {
  const int n = g.add(Op::CHANNEL_F32X3, {}, {ch});
  g.link(colour, n, 0);
  return n;
}

// The opcodes of the entry at word `at` up to its END; *end is the word after that END. Immediate
// counts come from each row's operand tokens.
std::vector<Op> entry_ops(const std::vector<uint32_t>& prog, uint32_t at, uint32_t* end = nullptr) {
  std::vector<Op> out;
  uint32_t pc = at;
  while (pc < prog.size()) {
    const Op op = Op(prog[pc] & 0xffu);
    out.push_back(op);
    uint32_t imm = 0;
    const std::string tok = astroray::host::op_meta(op).operand_types;
    for (size_t k = 0; (k = tok.find("imm", k)) != std::string::npos; k += 3) ++imm;  // imm and slotimm
    if (tok.find("jmpoff") != std::string::npos) ++imm;
    pc += 1 + imm;
    if (op == Op::END) break;
  }
  if (end != nullptr) *end = pc;
  return out;
}

std::vector<uint8_t> read_data(const std::string& rel) {
  std::string here = __FILE__;
  std::string dir = here.substr(0, here.find_last_of("/\\") + 1);
  std::ifstream f(dir + "../../data/" + rel, std::ios::binary);
  if (!f) f.open("data/" + rel, std::ios::binary);
  REQUIRE_MESSAGE(f.good(), "data/" << rel << " not found");
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("compiler_outputs/may be delta constant roughness") {
  // alpha = roughness^2 against lobe_is_delta's 1e-3 (0005 items 4, 43).
  CHECK((roughness(0.0f).flags & ar::MAY_BE_DELTA) != 0);
  CHECK((roughness(1e-4f).flags & ar::MAY_BE_DELTA) != 0);
  CHECK((roughness(0.5f).flags & ar::MAY_BE_DELTA) == 0);
  CHECK((roughness(0.02f).flags & ar::MAY_BE_DELTA) != 0);  // alpha 4e-4: roughness itself is above 1e-3
  CHECK((roughness(0.04f).flags & ar::MAY_BE_DELTA) == 0);  // alpha 1.6e-3
  // A coat that can be sampled counts with its own roughness; one of weight 0 cannot be sampled.
  GB coat;
  coat.store(coat.add(Op::CONST_F32, {0.5f}), ar::RecordSlot::specular_roughness);
  coat.store(coat.add(Op::CONST_F32, {1.0f}), ar::RecordSlot::coat_weight);
  CHECK((coat.compile().flags & ar::MAY_BE_DELTA) != 0);  // coat_roughness defaults to 0
}

TEST_CASE("compiler_outputs/may be delta textured roughness") {
  // The rule is driven through a compiled result whose roughness slots are marked as stored per
  // point, over constants that alone would clear it.
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

TEST_CASE("compiler_outputs/runtime slots mark per-point stores") {
  const uint64_t bit = uint64_t(1) << uint32_t(ar::RecordSlot::base_color);
  GB textured;
  textured.store(tex(textured), ar::RecordSlot::base_color);
  CHECK((textured.compile().compiled.runtime_slots & bit) != 0);
  GB constant;
  constant.store(constant.add(Op::CONST_F32X3, {0.2f, 0.4f, 0.6f}), ar::RecordSlot::base_color);
  CHECK(constant.compile().compiled.runtime_slots == 0);
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
  GB black;
  black.op2(Op::EMIT_LE, black.add(Op::CONST_F32X3, {0.0f, 0.0f, 0.0f}), black.add(Op::CONST_F32, {5.0f}));
  CHECK(black.compile().emission_estimate.empty());
  CHECK(roughness(0.5f).emission_estimate.empty());
}

TEST_CASE("compiler_outputs/emission estimate textured") {
  GB g;
  g.op2(Op::EMIT_LE, tex(g), g.add(Op::CONST_F32, {2.0f}));
  const MaterialOutput m = g.compile();
  REQUIRE(m.emission_estimate.size() == 1);
  // Compiled without image means (a worker that may run before the decode) the input is bounded
  // by 1 and the result names the image, so the store compiles it again with the means.
  CHECK(m.emission_estimate[0].color.kind == uint8_t(ar::SpectrumKind::CONSTANT));
  CHECK(m.emission_estimate[0].color.p[0] == 1.0f);
  CHECK(m.emission_estimate[0].strength == 2.0f);
  CHECK((m.flags & ar::CONSTANT_EMISSION) == 0);
  CHECK(!m.magenta);
  CHECK((m.images == std::vector<uint32_t>{7}));
}

TEST_CASE("compiler_outputs/emission estimate image mean") {
  GB g;
  g.op2(Op::EMIT_LE, tex(g), g.add(Op::CONST_F32, {2.0f}));
  auto expect = [&](float r, float gr, float b) {
    ar::SpectrumRef ref = {};
    uint32_t flags = 0;
    ar::make_rgb_ref(table(), ar::Rgb{{ar::Real(r), ar::Real(gr), ar::Real(b)}}, ar::kSlotSemantic[int(ar::Slot::EMISSION_COLOR)], &ref, &flags);
    return ref;
  };
  const astroray::host::ImageMeanFn means = [](uint32_t image, std::array<float, 3>* rgb) {
    if (image != 7) return false;
    *rgb = {0.25f, 0.5f, 0.75f};
    return true;
  };
  const MaterialOutput m = compile_material(g.desc(), table(), &means);
  REQUIRE(m.emission_estimate.size() == 1);
  CHECK(same_ref(m.emission_estimate[0].color, expect(0.25f, 0.5f, 0.75f)));  // the mean, not the bound
  CHECK(m.emission_estimate[0].strength == 2.0f);
  CHECK((m.images == std::vector<uint32_t>{7}));
  // An image the scene does not have samples magenta, and so does the estimate.
  const astroray::host::ImageMeanFn none = [](uint32_t, std::array<float, 3>*) { return false; };
  const MaterialOutput lost = compile_material(g.desc(), table(), &none);
  REQUIRE(lost.emission_estimate.size() == 1);
  CHECK(same_ref(lost.emission_estimate[0].color, expect(1.0f, 0.0f, 1.0f)));
  // A black image folds the estimate away exactly as a constant black does.
  const astroray::host::ImageMeanFn black = [](uint32_t, std::array<float, 3>* rgb) { return *rgb = {0.0f, 0.0f, 0.0f}, true; };
  CHECK(compile_material(g.desc(), table(), &black).emission_estimate.empty());
}

TEST_CASE("compiler_outputs/emission estimate derived from an image") {
  // ADD(tex, -0.5) emits wherever the texel exceeds 0.5, so a mean of 0.4 must not zero the
  // estimate (0005 item 43: zero only when emission folds to 0); it falls back to the bound 1.
  const astroray::host::ImageMeanFn forty = [](uint32_t, std::array<float, 3>* rgb) { return *rgb = {0.4f, 0.4f, 0.4f}, true; };
  GB neg;
  const int sub = neg.op2(Op::ADD_F32X3, tex(neg), neg.add(Op::CONST_F32X3, {-0.5f, -0.5f, -0.5f}));
  neg.op2(Op::EMIT_LE, sub, neg.add(Op::CONST_F32, {1.0f}));
  const MaterialOutput m = compile_material(neg.desc(), table(), &forty);
  REQUIRE(m.emission_estimate.size() == 1);
  CHECK(m.emission_estimate[0].color.kind == uint8_t(ar::SpectrumKind::CONSTANT));
  CHECK(m.emission_estimate[0].color.p[0] == 1.0f);
  // A non-negative offset keeps the exact mean: 0.4 + 0.25.
  GB pos;
  const int add = pos.op2(Op::ADD_F32X3, tex(pos), pos.add(Op::CONST_F32X3, {0.25f, 0.25f, 0.25f}));
  pos.op2(Op::EMIT_LE, add, pos.add(Op::CONST_F32, {1.0f}));
  const MaterialOutput p = compile_material(pos.desc(), table(), &forty);
  REQUIRE(p.emission_estimate.size() == 1);
  ar::SpectrumRef want = {};
  uint32_t flags = 0;
  ar::make_rgb_ref(table(), ar::Rgb{{ar::Real(0.65f), ar::Real(0.65f), ar::Real(0.65f)}}, ar::kSlotSemantic[int(ar::Slot::EMISSION_COLOR)], &want, &flags);
  CHECK(same_ref(p.emission_estimate[0].color, want));
}

TEST_CASE("compiler_outputs/emission estimate of an image channel") {
  // A known colour mean gives a known channel mean: the strength estimate is that channel of the
  // image mean, not the bound 1 (0005 item 43). The three means differ so a wrong channel shows.
  const astroray::host::ImageMeanFn means = [](uint32_t image, std::array<float, 3>* rgb) {
    if (image != 7) return false;
    *rgb = {0.25f, 0.5f, 0.75f};
    return true;
  };
  GB g;
  g.op2(Op::EMIT_LE, g.add(Op::CONST_F32X3, {1.0f, 1.0f, 1.0f}), channel(g, tex(g), 1));
  const MaterialOutput m = compile_material(g.desc(), table(), &means);
  REQUIRE(m.emission_estimate.size() == 1);
  CHECK(bits(m.emission_estimate[0].strength) == bits(0.5f));
  CHECK((m.flags & ar::CONSTANT_EMISSION) == 0);
  CHECK((m.images == std::vector<uint32_t>{7}));
  // Without the means the channel is unknown and the estimate falls back to the bound 1.
  const MaterialOutput bound = g.compile();
  REQUIRE(bound.emission_estimate.size() == 1);
  CHECK(bound.emission_estimate[0].strength == 1.0f);
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

TEST_CASE("compiler/spectrum kinds accepted per semantic") {
  using SK = ar::SpectrumKind;
  // A Blackbody feeding the only emission, with a constant strength: one ref in the block.
  {
    GB g;
    g.op2(Op::EMIT_LE, g.add(Op::BLACKBODY, {2700.0f, 2.0f}), g.add(Op::CONST_F32, {3.0f}));
    const MaterialOutput m = g.compile();
    REQUIRE_FALSE(m.magenta);
    const ar::SpectrumRef& r = m.compiled.block.emission_color;
    CHECK(r.kind == uint8_t(SK::BLACKBODY));
    CHECK(r.semantic == uint8_t(ar::SpectrumSemantic::ILLUMINANT));
    CHECK(r.p[0] == 2700.0f);
    CHECK(r.p[1] == float(2.0 * astroray::host::blackbody_y_normalised(2700.0)));
    CHECK(m.compiled.block.emission_luminance == 3.0f);
    REQUIRE(m.emission_estimate.size() == 1);
    CHECK(same_ref(m.emission_estimate[0].color, r));
    CHECK(m.emission_estimate[0].strength == 3.0f);
    CHECK((m.flags & ar::CONSTANT_EMISSION) != 0u);
  }
  // A Cauchy-Abbe node with constant parameters feeding specular_ior.
  {
    GB g;
    g.store(g.add(Op::CAUCHY_ABBE, {1.5168f, 64.17f, 0.5f}), ar::RecordSlot::specular_ior);
    const MaterialOutput m = g.compile();
    REQUIRE_FALSE(m.magenta);
    const ar::SpectrumRef& r = m.compiled.block.specular_ior;
    CHECK(r.kind == uint8_t(SK::CAUCHY_ABBE));
    CHECK(r.semantic == uint8_t(ar::SpectrumSemantic::IOR));
    CHECK(r.p[0] == 1.5168f);
    CHECK(r.p[1] == 64.17f);
    CHECK(r.p[2] == 0.5f);
    CHECK(m.compiled.constant_surface);
  }
  // Everything else is PROGRAM_REJECTED with one entry that names the node.
  auto rejected = [](GB& g, const char* what, const char* node) {
    const MaterialOutput m = g.compile();
    INFO(what);
    CHECK(m.magenta);
    REQUIRE(m.entries.size() == 1);
    CHECK(m.entries[0].code == AR_REPORT_PROGRAM_REJECTED);
    CHECK(m.entries[0].detail.find(node) != std::string::npos);
    CHECK(same_ref(m.compiled.block.base_color, magenta_ref()));
  };
  {
    GB g;  // a Blackbody in a REFLECTANCE slot
    g.store(g.add(Op::BLACKBODY, {6500.0f, 1.0f}), ar::RecordSlot::base_color);
    rejected(g, "BLACKBODY into base_color", "BLACKBODY");
  }
  {
    GB g;  // a Blackbody in a scalar slot
    g.store(g.add(Op::BLACKBODY, {6500.0f, 1.0f}), ar::RecordSlot::base_weight);
    rejected(g, "BLACKBODY into base_weight", "BLACKBODY");
  }
  {
    GB g;  // a Blackbody beside a second emission
    const int bb = g.add(Op::BLACKBODY, {6500.0f, 1.0f});
    g.op2(Op::EMIT_LE, bb, g.add(Op::CONST_F32, {1.0f}));
    g.op2(Op::EMIT_LE, g.add(Op::CONST_F32X3, {1.0f, 1.0f, 1.0f}), g.add(Op::CONST_F32, {1.0f}));
    rejected(g, "BLACKBODY with two emissions", "BLACKBODY");
  }
  {
    GB g;  // the strength socket takes a scalar, not a spectrum
    const int bb = g.add(Op::BLACKBODY, {6500.0f, 1.0f});
    g.op2(Op::EMIT_LE, g.add(Op::CONST_F32X3, {1.0f, 1.0f, 1.0f}), bb);
    rejected(g, "BLACKBODY into the strength", "BLACKBODY");
  }
  for (float t : {0.0f, -300.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
    GB g;
    g.op2(Op::EMIT_LE, g.add(Op::BLACKBODY, {t, 1.0f}), g.add(Op::CONST_F32, {1.0f}));
    rejected(g, "bad temperature", "BLACKBODY");
  }
  {
    GB g;  // a CAUCHY_ABBE anywhere but an IOR slot
    g.store(g.add(Op::CAUCHY_ABBE, {1.5f, 50.0f, 1.0f}), ar::RecordSlot::coat_color);
    rejected(g, "CAUCHY_ABBE into coat_color", "CAUCHY_ABBE");
  }
  {
    GB g;
    g.store(g.add(Op::CAUCHY_ABBE, {1.5f, 50.0f, 1.0f}), ar::RecordSlot::coat_ior);
    rejected(g, "CAUCHY_ABBE into coat_ior", "CAUCHY_ABBE");
  }
  {
    GB g;
    g.op2(Op::EMIT_LE, g.add(Op::CAUCHY_ABBE, {1.5f, 50.0f, 1.0f}), g.add(Op::CONST_F32, {1.0f}));
    rejected(g, "CAUCHY_ABBE into an emission", "CAUCHY_ABBE");
  }
  for (float vd : {0.0f, -20.0f, std::numeric_limits<float>::quiet_NaN()}) {
    GB g;
    g.store(g.add(Op::CAUCHY_ABBE, {1.5f, vd, 1.0f}), ar::RecordSlot::specular_ior);
    rejected(g, "bad Abbe number", "CAUCHY_ABBE");
  }
  {
    GB g;  // a linked n_d: the node has no input socket (per-point dispersion is M2)
    const int c = g.add(Op::CAUCHY_ABBE, {1.5f, 50.0f, 1.0f});
    g.link(g.add(Op::CONST_F32, {1.5f}), c, 0);
    g.store(c, ar::RecordSlot::specular_ior);
    rejected(g, "linked input", "graph node 0: has no such input socket");
  }
  {
    GB g;  // a colour into specular_ior is still refused
    g.store(g.add(Op::CONST_F32X3, {1.5f, 1.5f, 1.5f}), ar::RecordSlot::specular_ior);
    rejected(g, "colour into specular_ior", "specular_ior");
  }
}

TEST_CASE("compiler/blackbody outside the float domain is rejected") {
  auto rejected = [](float t, float scale, const char* what) {
    GB g;
    g.op2(Op::EMIT_LE, g.add(Op::BLACKBODY, {t, scale}), g.add(Op::CONST_F32, {1.0f}));
    const MaterialOutput m = g.compile();
    INFO(what);
    CHECK(m.magenta);
    REQUIRE(m.entries.size() == 1);
    CHECK(m.entries[0].code == AR_REPORT_PROGRAM_REJECTED);
    CHECK(m.entries[0].detail.find("BLACKBODY") != std::string::npos);
  };
  rejected(100.0f, 1.0f, "cold: 1 / Y overflows a float");
  rejected(2.0f, 1.0f, "below planck.h's 3 K");
  rejected(2.0e9f, 1.0f, "above planck.h's 1e9 K");
  rejected(6500.0f, 3.4e38f, "a huge scale");
  GB ok;  // a cool but representable temperature still compiles
  ok.op2(Op::EMIT_LE, ok.add(Op::BLACKBODY, {1000.0f, 1.0f}), ok.add(Op::CONST_F32, {1.0f}));
  CHECK_FALSE(ok.compile().magenta);
}

TEST_CASE("compiler/blackbody strength must be constant") {
  // A per-point strength cannot reach the Blackbody ref. The strength socket takes a scalar and a
  // texture is a colour, so the graph's type rule refuses it first.
  GB g;
  const int bb = g.add(Op::BLACKBODY, {6500.0f, 1.0f});
  g.op2(Op::EMIT_LE, bb, tex(g));
  const MaterialOutput m = g.compile();
  CHECK(m.magenta);
  REQUIRE(m.entries.size() == 1);
  CHECK(m.entries[0].code == AR_REPORT_PROGRAM_REJECTED);
  CHECK(m.entries[0].detail.find("input socket wants another type") != std::string::npos);
}

TEST_CASE("compiler_outputs/blackbody emission sampling is tested by luminance") {
  // AUTO keeps the 0.5 threshold on the term's luminance: Y = scale x strength (scale 2 here, so a
  // test that ignored the scale would read 0.3 and 0.2, both below the threshold).
  auto sided = [](float strength) {
    GB g;
    g.op2(Op::EMIT_LE, g.add(Op::BLACKBODY, {4000.0f, 2.0f}), g.add(Op::CONST_F32, {strength}));
    const MaterialOutput m = g.compile();
    REQUIRE_FALSE(m.magenta);
    return astroray::host::emission_sidedness(AR_EMISSION_SAMPLING_AUTO, m.emission_estimate);
  };
  CHECK(sided(0.3f) == (astroray::host::kLightLit | ar::kLightTwoSided));  // Y 0.6
  CHECK(sided(0.2f) == 0u);                                                // Y 0.4
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

TEST_CASE("compiler_outputs/dropped generation does not leak into queued ones") {
  using namespace astroray::host;
  const ar_key G = 10, H = 11, A = 20, B = 21, BLOCKER = 99;
  astroray::ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  auto store = std::make_shared<MaterialStore>(std::make_shared<Ledger>());
  GB good;
  good.store(good.add(Op::CONST_F32, {0.5f}), ar::RecordSlot::specular_roughness);
  GB bevel, ao;
  bevel.unsupported("ShaderNodeBevel");
  ao.unsupported("ShaderNodeAmbientOcclusion");
  auto material = [](ar_key graph) {
    ar_material_desc m = {};
    m.surface_graph = graph;
    return m;
  };
  auto commit = [&](const std::function<void(Edit&)>& body) {
    auto e = scene.begin_edit();
    REQUIRE(e);
    body(*e);
    uint64_t g = 0;
    REQUIRE(e->commit(&g) == AR_OK);
    store->end_edit();
    return g;
  };
  auto entries = [&](ar_key subject) {
    scene.report().snapshot();
    ar_report_info info = {};
    REQUIRE(scene.report().info(&info) == AR_OK);
    std::vector<std::string> out;
    for (uint32_t k = 0; k < info.entry_count; ++k) {
      ar_report_entry e = {};
      REQUIRE(scene.report().entry(k, &e) == AR_OK);
      if (e.subject == subject && e.code == AR_REPORT_NODE_UNSUPPORTED) out.emplace_back(reinterpret_cast<const char*>(e.detail));
    }
    return out;
  };
  // N: G and H good, A uses G.
  commit([&](Edit& e) {
    const ar_graph_desc dg = good.desc();
    const ar_material_desc ma = material(G);
    REQUIRE(store->set_graph(e, G, &dg) == AR_OK);
    REQUIRE(store->set_graph(e, H, &dg) == AR_OK);
    REQUIRE(store->set_material(e, A, &ma) == AR_OK);
  });
  scene.generations().wait_idle();
  // N+1, dropped: H becomes Bevel and A is removed, with a mesh-like job that fails once released.
  std::promise<void> release;
  std::shared_future<void> gate = release.get_future().share();
  commit([&](Edit& e) {
    const ar_graph_desc db = bevel.desc();
    REQUIRE(store->set_graph(e, H, &db) == AR_OK);
    REQUIRE(e.remove(AR_KIND_MATERIAL, A) == AR_OK);
    REQUIRE(store->note_remove(e, AR_KIND_MATERIAL, A) == AR_OK);
    auto blocker = std::make_shared<EditItem>();
    blocker->key = BLOCKER;
    blocker->needs_device = false;
    blocker->job = [gate] {
      gate.wait();
      throw std::runtime_error("corner_vert out of range");
    };
    REQUIRE(e.stage(blocker) == AR_OK);
    REQUIRE(e.item_commit(BLOCKER) == AR_OK);
  });
  // N+2 and N+3, queued behind it: B uses H; then G becomes AO.
  commit([&](Edit& e) {
    const ar_material_desc mb = material(H);
    REQUIRE(store->set_material(e, B, &mb) == AR_OK);
  });
  commit([&](Edit& e) {
    const ar_graph_desc da = ao.desc();
    REQUIRE(store->set_graph(e, G, &da) == AR_OK);
  });
  release.set_value();
  scene.generations().wait_idle();
  REQUIRE(scene.generations().failed() == 2);
  // B compiled against the active (good) H, not N+1's Bevel; A still exists and recompiled for AO.
  CHECK(entries(B).empty());
  CHECK(entries(A) == std::vector<std::string>{"ShaderNodeAmbientOcclusion"});
  CHECK(store->arrays()->keys == std::vector<ar_key>{A, B});
  // After the drop the latest state is rebuilt: a later edit of H recompiles B.
  commit([&](Edit& e) {
    const ar_graph_desc db = bevel.desc();
    REQUIRE(store->set_graph(e, H, &db) == AR_OK);
  });
  scene.generations().wait_idle();
  CHECK(entries(B) == std::vector<std::string>{"ShaderNodeBevel"});
}

TEST_CASE("compiler_outputs/constant channel folds") {
  // Item 35: CHANNEL_F32X3 reads no context, so a constant colour's channel folds into the block and
  // a constant opacity needs no entry (item 32). A textured base colour keeps a program, so word 1
  // of the entry table can be read.
  const float colour[3] = {0.25f, 0.5f, 1.0f};
  for (int32_t ch = 0; ch < 3; ++ch) {
    INFO("channel " << ch);
    GB g;
    g.store(tex(g), ar::RecordSlot::base_color);
    g.store(channel(g, g.add(Op::CONST_F32X3, {colour[0], colour[1], colour[2]}), ch), ar::RecordSlot::geometry_opacity);
    const MaterialOutput m = g.compile();
    REQUIRE(!m.magenta);
    const std::vector<uint32_t>& prog = m.compiled.program;
    REQUIRE(prog.size() > 4);
    CHECK(prog[1] == 0u);
    CHECK(bits(m.compiled.block.geometry_opacity) == bits(colour[ch]));
    CHECK(m.compiled.runtime_slots == (uint64_t(1) << uint32_t(ar::RecordSlot::base_color)));
    for (Op o : entry_ops(prog, prog[0])) CHECK(o != Op::CHANNEL_F32X3);
    CHECK(((m.flags & ar::NON_OPAQUE) != 0) == (ch != 2));  // only the channel holding 1 is opaque
    CHECK(m.entries.empty());
  }
}

TEST_CASE("compiler_outputs/non opaque flag") {
  // Item 43: NON_OPAQUE is set unless T folds to 0, that is unless opacity is the constant 1.
  auto constant = [](float a) {
    GB g;
    g.store(g.add(Op::CONST_F32, {a}), ar::RecordSlot::geometry_opacity);
    return g.compile();
  };
  GB per;
  per.store(channel(per, tex(per), 0), ar::RecordSlot::geometry_opacity);
  const MaterialOutput p = per.compile();
  REQUIRE(!p.magenta);
  REQUIRE(p.compiled.program.size() > 4);
  CHECK(p.compiled.program[1] != 0u);
  CHECK((p.flags & ar::NON_OPAQUE) != 0);  // the block still holds the default 1

  const MaterialOutput half = constant(0.5f);
  CHECK(half.compiled.program.empty());  // a constant opacity: no entry, word 1 is absent
  CHECK(half.compiled.block.geometry_opacity == 0.5f);
  CHECK((half.flags & ar::NON_OPAQUE) != 0);
  CHECK(half.entries.empty());
  CHECK((constant(1.0f).flags & ar::NON_OPAQUE) == 0);
  CHECK((roughness(0.5f).flags & ar::NON_OPAQUE) == 0);  // nothing stored: the default 1
  GB bevel;
  bevel.unsupported("ShaderNodeBevel");
  CHECK((bevel.compile().flags & ar::NON_OPAQUE) == 0);  // the magenta material is opaque

  // Out of range: clamped and reported once; NaN rejects the program.
  const MaterialOutput over = constant(1.5f);
  CHECK(!over.magenta);
  CHECK(over.compiled.block.geometry_opacity == 1.0f);
  CHECK((over.flags & ar::NON_OPAQUE) == 0);
  REQUIRE(over.entries.size() == 1);
  CHECK(over.entries[0].code == AR_REPORT_NODE_APPROXIMATED);
  CHECK(over.entries[0].detail == "opacity clamped");
  const MaterialOutput under = constant(-0.25f);
  CHECK(under.compiled.block.geometry_opacity == 0.0f);
  CHECK((under.flags & ar::NON_OPAQUE) != 0);
  CHECK(under.entries.size() == 1);
  const MaterialOutput nan = constant(std::nanf(""));
  CHECK(nan.magenta);
  REQUIRE(nan.entries.size() == 1);
  CHECK(nan.entries[0].code == AR_REPORT_PROGRAM_REJECTED);
  CHECK(nan.entries[0].detail.find("NaN") != std::string::npos);
}

TEST_CASE("compiler_outputs/opacity entry layout") {
  // Item 38: the opacity entry is the dead-code-eliminated graph of the per-point opacity store,
  // laid out after the surface and emission entries; the surface entry keeps the store.
  using RS = ar::RecordSlot;
  GB g;
  const int t = tex(g);
  g.store(t, RS::base_color);
  g.store(channel(g, t, 1), RS::geometry_opacity);
  const MaterialOutput m = g.compile();
  REQUIRE(!m.magenta);
  const std::vector<uint32_t>& prog = m.compiled.program;
  REQUIRE(prog.size() > 4);
  REQUIRE(prog[1] != 0u);
  CHECK(prog[2] == 0u);
  uint32_t end = 0;
  const std::vector<Op> want = {Op::LOAD_CTX_UV, Op::TEX_IMAGE, Op::CHANNEL_F32X3, Op::STORE_SLOT, Op::END};
  CHECK(entry_ops(prog, prog[1], &end) == want);
  CHECK(end == prog.size());
  CHECK(prog[end - 2] == uint32_t(RS::geometry_opacity));  // the STORE_SLOT immediate, before END
  const std::vector<Op> surface = entry_ops(prog, prog[0], &end);
  CHECK(end == prog[1]);
  CHECK(std::count(surface.begin(), surface.end(), Op::STORE_SLOT) == 2);
  CHECK(std::count(surface.begin(), surface.end(), Op::CHANNEL_F32X3) == 1);

  // With emission: [table, surface, emission, opacity], each entry ending where the next begins.
  GB e;
  const int te = tex(e);
  e.op2(Op::EMIT_LE, te, e.add(Op::CONST_F32, {2.0f}));
  e.store(channel(e, te, 0), RS::geometry_opacity);
  const MaterialOutput me = e.compile();
  REQUIRE(!me.magenta);
  const std::vector<uint32_t>& pe = me.compiled.program;
  REQUIRE(pe.size() > 4);
  CHECK(pe[0] == 4u);
  REQUIRE((pe[2] > pe[0] && pe[1] > pe[2]));
  CHECK(pe[3] == 0u);
  entry_ops(pe, pe[0], &end);
  CHECK(end == pe[2]);
  const std::vector<Op> emission = entry_ops(pe, pe[2], &end);
  CHECK(end == pe[1]);
  CHECK(std::count(emission.begin(), emission.end(), Op::EMIT_LE) == 1);
  CHECK(std::count(emission.begin(), emission.end(), Op::STORE_SLOT) == 0);
  const std::vector<Op> opacity = entry_ops(pe, pe[1], &end);
  CHECK(end == pe.size());
  CHECK(opacity == want);
}

TEST_CASE("compiler_outputs/entries share one register file") {
  // Verification 6, register-file reuse: the surface, emission and opacity entries of one compiled
  // material, run one after another on one VmRegs in every order, give the Le, record and opacity
  // each gives alone on a zeroed and on a NaN-filled file, bit for bit. Each entry gets only the
  // outputs its kernel passes: the emission entry no record, the opacity entry no Le (item 32: it
  // writes geometry_opacity and nothing else).
  using RS = ar::RecordSlot;
  const auto ob = read_data("spectral/observer.bin");
  astroray::ObserverTables obs;
  const std::string err = astroray::parse_observer(ob.data(), ob.size(), &obs);
  REQUIRE_MESSAGE(err.empty(), err);
  std::vector<uint8_t> texels = {0, 40, 255, 128, 7, 64, 200, 90, 13, 1, 254, 77, 33, 160, 99, 250, 20, 180};
  ar::DeviceImageView image = {};
  image.texels = uint64_t(uintptr_t(texels.data()));
  image.width = 3, image.height = 2, image.channels = 3, image.encoding = uint32_t(ar::TexEncoding::SRGB);
  std::vector<float> srgb(ar::kSrgbTableSize);
  ar::srgb_decode_table(srgb.data());
  ar::SceneTables tables = {};
  tables.srgb_decode = srgb.data();
  tables.images = &image;
  tables.image_count = 1;
  tables.rgb2spec = &table();
  tables.observer.cmf = &obs.cmf[0][0];
  tables.observer.illum = obs.d65;

  GB g;
  const int t = tex(g);
  g.store(t, RS::base_color);
  g.op2(Op::EMIT_LE, g.op2(Op::MUL_F32X3, t, g.add(Op::CONST_F32X3, {0.25f, 0.5f, 2.0f})), g.add(Op::CONST_F32, {3.0f}));
  g.store(channel(g, t, 1), RS::geometry_opacity);
  const astroray::host::ImageSlotFn slots = [](uint32_t key, uint32_t* slot) { return key == 7 ? (*slot = 0, true) : false; };
  const MaterialOutput m = compile_material(g.desc(), table(), nullptr, &slots);
  REQUIRE(!m.magenta);
  const std::vector<uint32_t>& prog = m.compiled.program;
  REQUIRE(prog.size() > 4);
  REQUIRE((prog[0] != 0u && prog[1] != 0u && prog[2] != 0u));

  struct Out {
    ar::OpenPBRParams rec = {};
    ar::Spec le = {};
  };
  // Entry k is the table word: 0 surface, 1 opacity, 2 emission.
  auto run = [&](uint32_t k, ar::VmRegs* regs, const ar::ShadeContext& ctx, Out* o) {
    return ar::run_entry(prog.data(), prog[k], regs, ctx, tables, k == 2 ? nullptr : &o->rec, k == 1 ? nullptr : &o->le);
  };
  auto same = [](const Out& a, const Out& b) {
    for (int l = 0; l < ar::kLanes; ++l)
      if (bits(float(a.le.v[l])) != bits(float(b.le.v[l]))) return false;
    return bits(float(a.rec.geometry_opacity)) == bits(float(b.rec.geometry_opacity)) &&
           std::memcmp(&a.rec, &b.rec, sizeof a.rec) == 0;
  };
  const uint32_t orders[6][3] = {{0, 2, 1}, {0, 1, 2}, {2, 0, 1}, {2, 1, 0}, {1, 0, 2}, {1, 2, 0}};
  uint32_t bad = 0, lit = 0;
  for (uint32_t i = 0; i < 1000; ++i) {
    ar::ShadeContext ctx = {};
    auto u = [&](uint32_t d) { return ar::sample_independent(i, 0u, 0u, d, 0x147u); };
    ctx.uv[0] = {u(0) * 4.0f - 2.0f, u(1) * 4.0f - 2.0f};
    ctx.P = {u(2), u(3), u(4)};
    for (int l = 0; l < 4; ++l) ctx.lambda_t.nm[l] = float(360.0 + 470.0 * double(u(5 + uint32_t(l))));
    Out alone[3];
    for (uint32_t k = 0; k < 3; ++k) {
      ar::VmRegs zero = {};
      REQUIRE(run(k, &zero, ctx, &alone[k]) == ar::VmStatus::OK);
      ar::VmRegs nan;
      std::memset(&nan, 0xff, sizeof nan);
      Out o;
      REQUIRE(run(k, &nan, ctx, &o) == ar::VmStatus::OK);
      bad += !same(o, alone[k]);
    }
    for (const auto& order : orders) {
      ar::VmRegs regs = {};
      for (uint32_t k : order) {
        Out o;
        REQUIRE(run(k, &regs, ctx, &o) == ar::VmStatus::OK);
        bad += !same(o, alone[k]);
      }
    }
    // The values compared are the material's: the opacity is the texel's green channel, the
    // opacity entry leaves the rest of the record alone, and the surface carries the same opacity.
    float texel[3];
    const float uv[2] = {float(ctx.uv[0].x), float(ctx.uv[0].y)};
    ar::op_tex_image(tables, 0u, 0u, uv, texel);
    bad += bits(float(alone[1].rec.geometry_opacity)) != bits(texel[1]);
    bad += bits(float(alone[0].rec.geometry_opacity)) != bits(texel[1]);
    Out only_opacity;
    only_opacity.rec.geometry_opacity = alone[1].rec.geometry_opacity;
    bad += std::memcmp(&only_opacity.rec, &alone[1].rec, sizeof only_opacity.rec) != 0;
    lit += alone[2].le.v[0] > 0.0f;
  }
  CHECK(bad == 0u);
  CHECK(lit > 500u);  // the Le compared is not zero
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
