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
#include <functional>
#include <limits>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../src/host/ledger.h"
#include "../../src/host/material/compiler.h"
#include "../../src/host/light/light_builder.h"
#include "../../src/host/material/material_store.h"
#include "../../src/host/spectral/blackbody.h"
#include "host/scene/edit.h"
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

TEST_CASE("compiler_outputs/blackbody emission sampling is tested by luminance") {
  // AUTO keeps the 0.5 threshold on the term's luminance: Y = strength for a unit-scale Blackbody.
  auto sided = [](float strength) {
    GB g;
    g.op2(Op::EMIT_LE, g.add(Op::BLACKBODY, {4000.0f, 1.0f}), g.add(Op::CONST_F32, {strength}));
    const MaterialOutput m = g.compile();
    REQUIRE_FALSE(m.magenta);
    return astroray::host::emission_sidedness(AR_EMISSION_SAMPLING_AUTO, m.emission_estimate);
  };
  CHECK(sided(0.6f) == (astroray::host::kLightLit | ar::kLightTwoSided));
  CHECK(sided(0.4f) == 0u);
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
