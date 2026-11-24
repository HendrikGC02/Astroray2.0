// SPDX-License-Identifier: Apache-2.0
// 0005 items 29-32, 35, 38: the material compiler core. Constant graphs fold to length 0 with the
// constant block as expected, folding stops at texture and context reads, the register allocator
// rejects a program that does not fit, and compiled programs run through the interpreter give the
// record that hand evaluation gives.
// Ref: docs/decisions/0005-material-model.md items 29-32, 35, 38.
#include <doctest.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <cstring>
#include <functional>
#include <limits>
#include <memory>

#include "../../src/host/data_resource.h"
#include "../../src/host/ledger.h"
#include "../../src/host/material/compiler.h"
#include "../../src/host/ledger.h"
#include "../../src/host/material/material_store.h"
#include "../../src/host/material/regalloc.h"
#include "host/scene/edit.h"
#include "material/constant_block.h"
#include "medium/medium_eval.h"
#include "medium/phase.h"
#include "vm/interp.h"

namespace {

using ar::Op;
using astroray::host::compile_surface;
using astroray::host::CompileResult;

std::vector<uint8_t> read_data(const std::string& rel) {
  std::string here = __FILE__;
  std::string dir = here.substr(0, here.find_last_of("/\\") + 1);
  std::ifstream f(dir + "../../data/" + rel, std::ios::binary);
  if (!f) f.open("data/" + rel, std::ios::binary);
  REQUIRE_MESSAGE(f.good(), "data/" << rel << " not found");
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

struct Fixture {
  std::vector<uint8_t> jh_bytes;
  astroray::ObserverTables obs;
  ar::Rgb2SpecTable table;
  // One 1x1 linear image in slot 0, for compiled programs that sample.
  std::vector<uint8_t> texel = {51, 102, 204};
  ar::DeviceImageView image = {};
  float srgb[ar::kSrgbTableSize];
  ar::SceneTables tables;
  Fixture() {
    ar::srgb_decode_table(srgb);
    tables.srgb_decode = srgb;
    image.texels = uint64_t(uintptr_t(texel.data()));
    image.width = image.height = 1, image.channels = 3, image.encoding = uint32_t(ar::TexEncoding::LINEAR);
    tables.images = &image;
    tables.image_count = 1;
    auto ob = read_data("spectral/observer.bin");
    std::string err = astroray::parse_observer(ob.data(), ob.size(), &obs);
    REQUIRE_MESSAGE(err.empty(), err);
    jh_bytes = read_data("spectral/rec709.jh");
    const char* e = ar::jh_view(jh_bytes.data(), jh_bytes.size(), &table);
    REQUIRE_MESSAGE(e == nullptr, (e ? e : ""));
    tables.rgb2spec = &table;
    tables.observer.cmf = &obs.cmf[0][0];
    tables.observer.illum = obs.d65;
  }
};
const Fixture& fixture() {
  static const Fixture f;
  return f;
}

// Builds an ar_graph_desc from calls, as the addon would.
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
  void link(int from, int to, uint32_t socket) { links.push_back({uint32_t(from), 0u, uint32_t(to), socket}); }
  int store(int src, ar::RecordSlot s) {
    const int n = add(Op::STORE_SLOT, {}, {int32_t(s)});
    link(src, n, 0);
    return n;
  }
  int op2(Op op, int a, int b) {
    const int n = add(op);
    link(a, n, 0);
    link(b, n, 1);
    return n;
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

CompileResult compile(const GB& g) {
  const ar_graph_desc d = g.desc();
  return compile_surface(d, fixture().table);
}

// The opcodes of the entry at `offset`, up to and including its END.
std::vector<Op> ops_of(const CompileResult& r, uint32_t offset) {
  std::vector<Op> out;
  for (uint32_t pc = offset; pc < r.program.size();) {
    const Op op = Op(r.program[pc] & 0xffu);
    out.push_back(op);
    if (op == Op::END) break;
    uint32_t imm = 0;
    if (op == Op::CONST_F32 || op == Op::STORE_SLOT || op == Op::JMP) imm = 1;
    if (op == Op::TEX_IMAGE) imm = 2;
    if (op == Op::CONST_F32X3) imm = 3;
    pc += 1 + imm;
  }
  return out;
}

bool has(const std::vector<Op>& v, Op o) {
  for (Op x : v)
    if (x == o) return true;
  return false;
}

float f(float v) { return v; }

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("compiler/constant surface folds to length 0") {
  GB g;
  const int rgb = g.add(Op::CONST_F32X3, {0.2f, 0.4f, 0.6f});
  const int half = g.add(Op::CONST_F32, {0.5f});
  g.store(g.op2(Op::MUL_F32X3_F32, rgb, half), ar::RecordSlot::base_color);
  g.store(g.add(Op::CONST_F32, {0.7f}), ar::RecordSlot::base_metalness);
  g.store(g.add(Op::CONST_F32, {1.45f}), ar::RecordSlot::specular_ior);
  g.store(g.add(Op::CONST_F32, {1.5f}), ar::RecordSlot::transmission_color);  // above 1 in a REFLECTANCE slot
  g.store(g.add(Op::CONST_F32X3, {0.0f, 1.0f, 0.0f}), ar::RecordSlot::geometry_tangent);
  const int ec = g.add(Op::CONST_F32X3, {1.0f, 0.5f, 0.25f});
  const int es = g.add(Op::CONST_F32, {3.0f});
  const int emit = g.add(Op::EMIT_LE);
  g.link(ec, emit, 0);
  g.link(es, emit, 1);
  const CompileResult r = compile(g);
  REQUIRE_MESSAGE(r.ok, r.error);
  CHECK(r.constant_surface);
  CHECK(r.program.size() == 0);
  std::printf("compiler: constant surface program length %zu words\n", r.program.size());

  // Expected block: the folded value (0.2, 0.4, 0.6) * 0.5 lifted once on the host (023).
  ar::SpectrumRef want;
  uint32_t fl = 0;
  REQUIRE(ar::make_rgb_ref(fixture().table, ar::Rgb{{0.1f, 0.2f, 0.3f}}, ar::SpectrumSemantic::REFLECTANCE, &want, &fl));
  CHECK(r.block.base_color.kind == uint8_t(ar::SpectrumKind::RGB));
  for (int k = 0; k < 4; ++k) CHECK(r.block.base_color.p[k] == want.p[k]);
  CHECK(r.block.base_metalness == 0.7f);
  CHECK(r.block.specular_ior.kind == uint8_t(ar::SpectrumKind::CONSTANT));
  CHECK(r.block.specular_ior.p[0] == 1.45f);
  CHECK(r.block.transmission_color.p[0] == 1.0f);
  CHECK((r.clamp_flags & ar::kRgbAboveOneClamped) != 0u);
  CHECK(r.block.geometry_tangent[1] == 1.0f);
  CHECK(r.block.emission_luminance == 3.0f);
  ar::SpectrumRef want_e;
  REQUIRE(ar::make_rgb_ref(fixture().table, ar::Rgb{{1.0f, 0.5f, 0.25f}}, ar::SpectrumSemantic::ILLUMINANT, &want_e, &fl));
  CHECK(r.block.emission_color.kind == uint8_t(ar::SpectrumKind::RGB));
  CHECK(r.block.emission_color.semantic == uint8_t(ar::SpectrumSemantic::ILLUMINANT));
  for (int k = 0; k < 4; ++k) CHECK(r.block.emission_color.p[k] == want_e.p[k]);
  CHECK(r.block.base_weight == 1.0f);  // untouched slots keep the OpenPBR default
}

TEST_CASE("compiler/texture base colour keeps its ops and folds the rest") {
  GB g;
  const int uv = g.add(Op::LOAD_CTX_UV, {}, {0});
  const int tex = g.add(Op::TEX_IMAGE, {}, {3, 0});
  g.link(uv, tex, 0);
  const int lift = g.add(Op::RGB_LIFT, {}, {int32_t(ar::SpectrumSemantic::REFLECTANCE)});
  g.link(tex, lift, 0);
  g.store(lift, ar::RecordSlot::base_color);
  const int a = g.add(Op::CONST_F32, {0.5f});
  g.store(g.op2(Op::MUL_F32, a, a), ar::RecordSlot::specular_roughness);
  const CompileResult r = compile(g);
  REQUIRE_MESSAGE(r.ok, r.error);
  CHECK_FALSE(r.constant_surface);
  const std::vector<Op> want = {Op::LOAD_CTX_UV, Op::TEX_IMAGE, Op::RGB_LIFT, Op::STORE_SLOT, Op::END};
  CHECK(ops_of(r, 4) == want);
  CHECK(r.block.specular_roughness == 0.25f);
  // The pre-lifted flag (bit 8) marks a spectrum source.
  CHECK((r.program[r.program.size() - 2] & 0x1ffu) == (uint32_t(ar::RecordSlot::base_color) | ar::kStoreSlotSpecSource));
}

TEST_CASE("compiler/folding never crosses LOAD_CTX_UV") {
  GB g;
  const int uv = g.add(Op::LOAD_CTX_UV, {}, {0});
  const int half = g.add(Op::CONST_F32, {0.5f});
  g.store(g.op2(Op::MUL_F32X3_F32, uv, half), ar::RecordSlot::base_color);
  const CompileResult r = compile(g);
  REQUIRE_MESSAGE(r.ok, r.error);
  const std::vector<Op> ops = ops_of(r, 4);
  CHECK(has(ops, Op::LOAD_CTX_UV));
  CHECK(has(ops, Op::MUL_F32X3_F32));
  CHECK(has(ops, Op::CONST_F32));  // the folded constant operand is materialised for its reader
  CHECK_FALSE(r.constant_surface);
}

TEST_CASE("compiler/no folding across texture read") {
  GB g;
  const int c = g.add(Op::CONST_F32X3, {0.25f, 0.5f, 0.0f});  // constant coordinates, still a texture read
  const int tex = g.add(Op::TEX_IMAGE, {}, {1, 0});
  g.link(c, tex, 0);
  const int half = g.add(Op::CONST_F32, {0.5f});
  g.store(g.op2(Op::MUL_F32X3_F32, tex, half), ar::RecordSlot::base_color);
  const CompileResult r = compile(g);
  REQUIRE_MESSAGE(r.ok, r.error);
  CHECK_FALSE(r.constant_surface);
  const std::vector<Op> ops = ops_of(r, 4);
  CHECK(has(ops, Op::TEX_IMAGE));
  CHECK(has(ops, Op::MUL_F32X3_F32));
}

TEST_CASE("compiler/register overflow rejected") {
  GB g;
  std::vector<int> loads;
  for (int k = 0; k < 14; ++k) loads.push_back(g.add(Op::LOAD_CTX_P));  // 14 x 3 = 42 registers live
  int sum = loads[0];
  for (int k = 1; k < 14; ++k) sum = g.op2(Op::ADD_F32X3, sum, loads[size_t(k)]);
  g.store(sum, ar::RecordSlot::geometry_normal);
  const CompileResult r = compile(g);
  CHECK_FALSE(r.ok);
  CHECK(r.error.find("needs 45 registers") == 0);
  CHECK(r.error.find(" registers, 32 available") != std::string::npos);
}

TEST_CASE("compiler/run-time colour into specular_ior and negative strength rejected") {
  GB g;
  const int uv = g.add(Op::LOAD_CTX_UV, {}, {0});
  const int tex = g.add(Op::TEX_IMAGE, {}, {0, 0});
  g.link(uv, tex, 0);
  g.store(tex, ar::RecordSlot::specular_ior);
  CompileResult r = compile(g);
  CHECK_FALSE(r.ok);
  CHECK(r.error.find("specular_ior takes a scalar") != std::string::npos);
  GB h;
  const int lift = h.add(Op::RGB_LIFT, {}, {0});
  h.link(h.add(Op::LOAD_CTX_P), lift, 0);
  h.store(lift, ar::RecordSlot::specular_ior);
  CHECK_FALSE(compile(h).ok);
  GB e;
  const int emit = e.add(Op::EMIT_LE);
  e.link(e.add(Op::CONST_F32X3, {1.0f, 1.0f, 1.0f}), emit, 0);
  e.link(e.add(Op::CONST_F32, {-1.0f}), emit, 1);
  r = compile(e);
  CHECK_FALSE(r.ok);
  CHECK(r.error.find("negative") != std::string::npos);
}

TEST_CASE("compiler/registers are reused only after the last read") {
  // B starts where A ends (A is still read by B's instruction): `<=` would give both register 0.
  const std::vector<astroray::host::LiveRange> rr = {{0, 2, 1}, {2, 3, 1}, {4, 5, 1}};
  std::vector<int> reg;
  std::string err;
  REQUIRE(astroray::host::allocate_registers(rr, &reg, &err));
  CHECK(reg[0] != reg[1]);
  CHECK(reg[2] == reg[0]);  // A is dead by position 4 and its register is free again
}

TEST_CASE("compiler/type errors and cycles are returned") {
  GB g;
  const int a = g.add(Op::CONST_F32, {1.0f});
  g.store(a, ar::RecordSlot::geometry_normal);  // f32 into a frame slot
  CHECK_FALSE(compile(g).ok);
  GB c;
  const int x = c.add(Op::ADD_F32X3);
  const int y = c.add(Op::ADD_F32X3);
  c.link(y, x, 0);
  c.link(y, x, 1);
  c.link(x, y, 0);
  c.link(x, y, 1);
  const CompileResult r = compile(c);
  CHECK_FALSE(r.ok);
  CHECK(r.error.find("cycle") != std::string::npos);
}

TEST_CASE("compiler/compiled programs match hand evaluation") {
  const Fixture& fx = fixture();
  ar::VmRegs regs = {};
  ar::ShadeContext ctx = {};
  ctx.lambda_t = ar::Lambda{{450.0f, 520.0f, 590.0f, 660.0f}};
  ctx.P = {0.8f, -0.4f, 2.0f};
  ctx.uv[0] = {0.6f, 0.2f};

  {  // A: uv * 0.5 + 0.1 into base_color, lifted at the slot's semantic.
    GB g;
    const int uv = g.add(Op::LOAD_CTX_UV, {}, {0});
    const int half = g.add(Op::CONST_F32, {0.5f});
    const int off = g.add(Op::CONST_F32X3, {0.1f, 0.1f, 0.1f});
    g.store(g.op2(Op::ADD_F32X3, g.op2(Op::MUL_F32X3_F32, uv, half), off), ar::RecordSlot::base_color);
    const CompileResult r = compile(g);
    REQUIRE_MESSAGE(r.ok, r.error);
    std::printf("compiler: graph A (uv tint) program length %zu words\n", r.program.size());
    ar::OpenPBRParams rec = {};
    ar::Spec le = {};
    REQUIRE(ar::run_entry(r.program.data(), r.program.at(0), &regs, ctx, fx.tables, &rec, &le) == ar::VmStatus::OK);
    uint32_t fl = 0;
    const ar::Rgb rgb{{f(f(0.6f * 0.5f) + 0.1f), f(f(0.2f * 0.5f) + 0.1f), f(f(0.0f * 0.5f) + 0.1f)}};
    const ar::Spec want = ar::albedo(fx.table, rgb, ctx.lambda_t, &fl);
    for (int k = 0; k < ar::kLanes; ++k) CHECK(rec.base_color.v[k] == want.v[k]);
  }
  {  // B: emission P * 0.5 with strength 2, run as the surface entry and as the emission entry.
    GB g;
    const int p = g.add(Op::LOAD_CTX_P);
    const int half = g.add(Op::CONST_F32, {0.5f});
    const int col = g.op2(Op::MUL_F32X3_F32, p, half);
    const int emit = g.add(Op::EMIT_LE);
    g.link(col, emit, 0);
    g.link(g.add(Op::CONST_F32, {2.0f}), emit, 1);
    g.store(g.add(Op::CONST_F32, {0.3f}), ar::RecordSlot::base_metalness);  // constant: stays out of the code
    const CompileResult r = compile(g);
    REQUIRE_MESSAGE(r.ok, r.error);
    REQUIRE(r.program.at(2) != 0u);
    std::printf("compiler: graph B (emission) program length %zu words\n", r.program.size());
    uint32_t fl = 0;
    const ar::Rgb rgb{{f(0.8f * 0.5f), f(-0.4f * 0.5f), f(2.0f * 0.5f)}};
    const ar::Spec s = ar::illuminant(fx.table, fx.tables.observer, rgb, ctx.lambda_t, &fl);
    for (uint32_t entry : {r.program[0], r.program[2]}) {
      ar::OpenPBRParams rec = {};
      ar::Spec le = {};
      REQUIRE(ar::run_entry(r.program.data(), entry, &regs, ctx, fx.tables, &rec, &le) == ar::VmStatus::OK);
      for (int k = 0; k < ar::kLanes; ++k) CHECK(le.v[k] == s.v[k] * ar::Real(2.0f));
    }
  }
  {  // C: texture, explicit lift, pre-lifted store; image key 7 resolves to the fixture's slot 0.
    GB g;
    const int uv = g.add(Op::LOAD_CTX_UV, {}, {0});
    const int tex = g.add(Op::TEX_IMAGE, {}, {7, 0});
    g.link(uv, tex, 0);
    const int lift = g.add(Op::RGB_LIFT, {}, {int32_t(ar::SpectrumSemantic::REFLECTANCE)});
    g.link(tex, lift, 0);
    g.store(lift, ar::RecordSlot::base_color);
    const ar_graph_desc d = g.desc();
    const astroray::host::ImageSlotFn slots = [](uint32_t key, uint32_t* slot) { return key == 7 ? (*slot = 0, true) : false; };
    const CompileResult r = compile_surface(d, fx.table, nullptr, &slots);
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(r.missing_images.empty());
    CHECK((r.images == std::vector<uint32_t>{7}));
    std::printf("compiler: graph C (texture lift) program length %zu words\n", r.program.size());
    ar::OpenPBRParams rec = {};
    ar::Spec le = {};
    REQUIRE(ar::run_entry(r.program.data(), r.program.at(0), &regs, ctx, fx.tables, &rec, &le) == ar::VmStatus::OK);
    uint32_t fl = 0;
    const float k = 1.0f / 255.0f;  // a linear image's texel is byte * (1 / 255), as load_texel reads it
    const ar::Spec want = ar::albedo(fx.table, ar::Rgb{{51.0f * k, 102.0f * k, 204.0f * k}}, ctx.lambda_t, &fl);
    for (int i = 0; i < ar::kLanes; ++i) CHECK(rec.base_color.v[i] == want.v[i]);
  }
}

TEST_CASE("compiler/missing image key samples magenta with one entry") {
  GB g;
  const int uv = g.add(Op::LOAD_CTX_UV, {}, {0});
  const int t1 = g.add(Op::TEX_IMAGE, {}, {9, 1});
  const int t2 = g.add(Op::TEX_IMAGE, {}, {9, 3});  // the same key twice: still one entry
  g.link(uv, t1, 0);
  g.link(uv, t2, 0);
  g.store(t1, ar::RecordSlot::base_color);
  g.store(t2, ar::RecordSlot::coat_color);
  const ar_graph_desc d = g.desc();
  const astroray::host::ImageSlotFn none = [](uint32_t, uint32_t*) { return false; };
  const astroray::host::MaterialOutput m = astroray::host::compile_material(d, fixture().table, nullptr, &none);
  CHECK_FALSE(m.magenta);  // only the lookups are magenta; the material compiles
  REQUIRE(m.entries.size() == 1);
  CHECK(m.entries[0].code == AR_REPORT_IMAGE_UNREADABLE);
  CHECK(m.entries[0].detail == "image key 9 is not set");
  CHECK((m.images == std::vector<uint32_t>{9}));  // setting key 9 later recompiles the material
  const std::vector<uint32_t>& p = m.compiled.program;
  uint32_t lookups = 0;
  for (size_t pc = 4; pc + 2 < p.size(); ++pc)
    if ((p[pc] & 0xffu) == uint32_t(Op::TEX_IMAGE) && p[pc + 1] == ar::kTexNoImage) ++lookups, pc += 2;
  CHECK(lookups == 2);
  // A mode outside CLOSEST/LINEAR x REPEAT/EXTEND is a rejected program.
  GB b;
  const int bt = b.add(Op::TEX_IMAGE, {}, {9, 4});
  b.link(b.add(Op::LOAD_CTX_UV, {}, {0}), bt, 0);
  b.store(bt, ar::RecordSlot::base_color);
  const ar_graph_desc bd = b.desc();
  const astroray::host::MaterialOutput bm = astroray::host::compile_material(bd, fixture().table, nullptr, &none);
  CHECK(bm.magenta);
  REQUIRE(bm.entries.size() == 1);
  CHECK(bm.entries[0].code == AR_REPORT_PROGRAM_REJECTED);
  CHECK(bm.entries[0].detail.find("TEX_IMAGE mode") != std::string::npos);
}

TEST_CASE("compiler/interior fields stored") {
  // A constant specular_ior reaches interior_ior; the priority comes from the descriptor.
  GB g;
  g.store(g.add(Op::CONST_F32, {1.33f}), ar::RecordSlot::specular_ior);
  const ar_graph_desc d = g.desc();
  const astroray::host::MaterialOutput m = astroray::host::compile_material(d, fixture().table, nullptr, nullptr, 7);
  CHECK_FALSE(m.magenta);
  CHECK(m.compiled.block.medium_priority == 7);
  CHECK(m.compiled.block.interior_ior.kind == uint8_t(ar::SpectrumKind::CONSTANT));
  CHECK(m.compiled.block.interior_ior.p[0] == doctest::Approx(1.33f));
  CHECK(m.entries.empty());
  // No specular_ior stored: the OpenPBR default 1.5, priority 0.
  const astroray::host::MaterialOutput def = astroray::host::compile_material(ar_graph_desc{}, fixture().table);
  CHECK(def.compiled.block.medium_priority == 0);
  CHECK(def.compiled.block.interior_ior.p[0] == doctest::Approx(1.5f));
  // A per-point specular_ior stores 1.0 and reports once, however often it is asked; a priority
  // past a byte clamps and reports.
  astroray::host::MaterialOutput t = def;
  t.compiled.runtime_slots = uint64_t(1) << uint32_t(ar::RecordSlot::specular_ior);
  astroray::host::set_medium_fields(&t, 300);
  astroray::host::set_medium_fields(&t, 300);
  CHECK(t.compiled.block.interior_ior.p[0] == doctest::Approx(1.0f));
  CHECK(t.compiled.block.medium_priority == 255);
  REQUIRE(t.entries.size() == 2);
  CHECK(t.entries[0].code == AR_REPORT_NODE_APPROXIMATED);
  CHECK(t.entries[0].detail.find("specular_ior") != std::string::npos);
  CHECK(t.entries[1].detail.find("medium_priority") != std::string::npos);
}

TEST_CASE("compiler/store passes medium_priority to the block") {
  using namespace astroray::host;
  const ar_key G = 10, A = 20;
  astroray::ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  auto store = std::make_shared<MaterialStore>(std::make_shared<Ledger>());
  GB g;
  g.store(g.add(Op::CONST_F32, {0.5f}), ar::RecordSlot::specular_roughness);
  auto e = scene.begin_edit();
  REQUIRE(e);
  const ar_graph_desc dg = g.desc();
  ar_material_desc ma = {};
  ma.surface_graph = G;
  ma.medium_priority = 7;
  REQUIRE(store->set_graph(*e, G, &dg) == AR_OK);
  REQUIRE(store->set_material(*e, A, &ma) == AR_OK);
  uint64_t gen = 0;
  REQUIRE(e->commit(&gen) == AR_OK);
  store->end_edit();
  scene.generations().wait_idle();
  const auto arrays = store->arrays();
  REQUIRE(arrays->keys.size() == 1);
  CHECK(arrays->constants[0].medium_priority == 7);
}

// ---- the volume graph (0006 A.1, A.2) -------------------------------------------------------------

// One Volume Absorption or Volume Scatter node with constant colour and density.
int term(GB& g, ar::VolumeTerm k, std::vector<float> rgb, float density, float aniso = 0.0f) {
  const int c = g.add(Op::CONST_F32X3, std::move(rgb));
  const int d = g.add(Op::CONST_F32, {density});
  const int n = g.add(Op::VOLUME_COEFFS, {aniso}, {int32_t(k)});
  g.link(c, n, 0);
  g.link(d, n, 1);
  return n;
}

bool same_ref(const ar::SpectrumRef& a, const ar::SpectrumRef& b) { return std::memcmp(&a, &b, sizeof a) == 0; }

astroray::host::VolumeOutput volume(const GB& g) { return astroray::host::compile_volume(g.desc(), fixture().table); }

TEST_CASE("medium_eval/compiled graph folds to one block") {
  using ar::VolumeTerm;
  GB g;
  // The absorber's density is a MUL of two constants: it folds before it reaches the block.
  const int two = g.add(Op::CONST_F32, {2.0f}), three_q = g.add(Op::CONST_F32, {0.75f});
  const int dens = g.op2(Op::MUL_F32, two, three_q);
  const int col = g.add(Op::CONST_F32X3, {0.9f, 0.6f, 0.3f});
  const int absorb = g.add(Op::VOLUME_COEFFS, {0.0f}, {int32_t(VolumeTerm::ABSORPTION)});
  g.link(col, absorb, 0);
  g.link(dens, absorb, 1);
  term(g, VolumeTerm::SCATTER, {0.8f, 0.5f, 0.2f}, 2.0f, 0.5f);
  term(g, VolumeTerm::SCATTER, {0.2f, 0.4f, 0.9f}, 0.5f, -0.3f);
  const astroray::host::VolumeOutput v = volume(g);
  REQUIRE(v.entries.empty());
  REQUIRE(v.has_medium);
  const ar::MediumBlock& b = v.medium.block;
  uint32_t fl = 0;
  const ar::Rgb2SpecTable& tb = fixture().table;
  auto rgb = [](float r, float gg, float bb) { return ar::Rgb{{ar::Real(r), ar::Real(gg), ar::Real(bb)}}; };
  CHECK(same_ref(b.sigma_a, ar::volume_term_ref(tb, rgb(0.9f, 0.6f, 0.3f), 1.5f, VolumeTerm::ABSORPTION, &fl)));
  CHECK(same_ref(b.sigma_s[0], ar::volume_term_ref(tb, rgb(0.8f, 0.5f, 0.2f), 2.0f, VolumeTerm::SCATTER, &fl)));
  CHECK(same_ref(b.sigma_s[1], ar::volume_term_ref(tb, rgb(0.2f, 0.4f, 0.9f), 0.5f, VolumeTerm::SCATTER, &fl)));
  CHECK(b.lobe_count == 2u);
  CHECK(b.g[0] == 0.5f);
  CHECK(b.g[1] == -0.3f);
  CHECK((v.medium.program == std::vector<uint32_t>{uint32_t(Op::VOLUME_COEFFS), uint32_t(Op::END)}));
  CHECK(fl == 0u);
  // Run as the kernel does, entry at word 1: the sample's absorption and lobes are the block's.
  std::vector<uint32_t> program = {0u, v.medium.program[0], v.medium.program[1]};
  ar::MediumBlock block = b;
  block.entry = 1u;
  const ar::MediumTables tables{program.data(), &block, fixture().tables};
  ar::VmRegs regs = {};
  ar::MediumSample s;
  const ar::Lambda lam{{450.0f, 520.0f, 590.0f, 660.0f}};
  REQUIRE(ar::medium_eval(tables, ar::MediumDesc{}, ar::Real3{0, 0, 0}, lam, &regs, &s) == ar::VmStatus::OK);
  const ar::Spec a = ar::resolve(b.sigma_a, lam, fixture().tables.observer);
  const ar::Spec w1 = ar::resolve(b.sigma_s[1], lam, fixture().tables.observer);
  for (int l = 0; l < 4; ++l) {
    CHECK(s.sigma_a.v[l] == a.v[l]);
    CHECK(s.sigma_a.v[l] > 0.0f);
    CHECK(s.phase[1].weight.v[l] == w1.v[l]);
  }
}

TEST_CASE("medium_eval/g clamp reported once") {
  using ar::VolumeTerm;
  GB g;
  term(g, VolumeTerm::SCATTER, {0.5f, 0.5f, 0.5f}, 1.0f, 1.5f);
  term(g, VolumeTerm::SCATTER, {0.5f, 0.5f, 0.5f}, 1.0f, -3.0f);
  const astroray::host::VolumeOutput v = volume(g);
  REQUIRE(v.has_medium);
  // Both anisotropies are beyond Cycles' bound; they are stored at it and reported once (BACKLOG 46).
  CHECK(v.medium.block.g[0] == float(ar::kHgMaxAbsG));
  CHECK(v.medium.block.g[1] == -float(ar::kHgMaxAbsG));
  REQUIRE(v.entries.size() == 1u);
  CHECK(v.entries[0].code == AR_REPORT_NODE_APPROXIMATED);
  CHECK(v.entries[0].detail.find("anisotropy clamped") != std::string::npos);
  // In range: stored as is, nothing reported.
  GB ok;
  term(ok, VolumeTerm::SCATTER, {0.5f, 0.5f, 0.5f}, 1.0f, 0.999f);
  const astroray::host::VolumeOutput w = volume(ok);
  CHECK(w.entries.empty());
  CHECK(w.medium.block.g[0] == 0.999f);
}

TEST_CASE("medium_eval/refused volume programs") {
  using ar::VolumeTerm;
  using astroray::host::VolumeOutput;
  auto only = [](const VolumeOutput& v, uint32_t code, const char* text) {
    CHECK_FALSE(v.has_medium);
    REQUIRE(v.entries.size() == 1u);
    CHECK(v.entries[0].code == code);
    CHECK_MESSAGE(v.entries[0].detail.find(text) != std::string::npos, v.entries[0].detail);
  };
  {  // An op with no volume meaning is refused with the node named.
    GB g;
    term(g, VolumeTerm::SCATTER, {0.5f, 0.5f, 0.5f}, 1.0f);
    g.store(g.add(Op::CONST_F32, {0.5f}), ar::RecordSlot::specular_roughness);
    only(volume(g), AR_REPORT_PROGRAM_REJECTED, "graph node 4 (STORE_SLOT)");
  }
  {  // A colour that depends on the position: spatially varying media are not built.
    GB g;
    const int p = g.add(Op::LOAD_CTX_P);
    const int d = g.add(Op::CONST_F32, {1.0f});
    const int n = g.add(Op::VOLUME_COEFFS, {0.0f}, {int32_t(VolumeTerm::SCATTER)});
    g.link(p, n, 0);
    g.link(d, n, 1);
    only(volume(g), AR_REPORT_PROGRAM_REJECTED, "graph node 2 (VOLUME_COEFFS): the colour is not constant");
  }
  {  // A third lobe and a second absorber are unsupported.
    GB g;
    for (int k = 0; k < 3; ++k) term(g, VolumeTerm::SCATTER, {0.5f, 0.5f, 0.5f}, 1.0f);
    only(volume(g), AR_REPORT_NODE_UNSUPPORTED, "third phase lobe");
    GB h;
    for (int k = 0; k < 2; ++k) term(h, VolumeTerm::ABSORPTION, {0.5f, 0.5f, 0.5f}, 1.0f);
    only(volume(h), AR_REPORT_NODE_UNSUPPORTED, "second absorber");
  }
  {  // A node the addon could not lower is reported by type, as for a surface.
    GB g;
    term(g, VolumeTerm::SCATTER, {0.5f, 0.5f, 0.5f}, 1.0f);
    g.add(Op::UNSUPPORTED, {}, {0});
    const VolumeOutput v = volume(g);
    CHECK_FALSE(v.has_medium);
    REQUIRE(v.entries.size() == 1u);
    CHECK(v.entries[0].code == AR_REPORT_NODE_UNSUPPORTED);
  }
  {  // A non-finite density is rejected; a negative one is clamped to 0 and reported.
    GB g;
    term(g, VolumeTerm::SCATTER, {0.5f, 0.5f, 0.5f}, std::numeric_limits<float>::quiet_NaN());
    only(volume(g), AR_REPORT_PROGRAM_REJECTED, "must be finite");
    GB h;
    term(h, VolumeTerm::ABSORPTION, {0.2f, 0.4f, 0.6f}, -2.0f);
    const VolumeOutput v = volume(h);
    REQUIRE(v.has_medium);
    CHECK(v.medium.block.sigma_a.p[3] == 0.0f);
    REQUIRE(v.entries.size() == 1u);
    CHECK(v.entries[0].detail == "volume density clamped to 0");
  }
  {  // A colour outside [0, 1] is clamped as a reflectance and reported.
    GB g;
    term(g, VolumeTerm::SCATTER, {1.5f, 0.5f, -0.5f}, 1.0f);
    const VolumeOutput v = volume(g);
    REQUIRE(v.has_medium);
    REQUIRE(v.entries.size() == 1u);
    CHECK(v.entries[0].detail == "volume RGB clamped");
  }
  {  // No volume node: no medium and nothing to report.
    GB g;
    g.add(Op::CONST_F32, {1.0f});
    const VolumeOutput v = volume(g);
    CHECK_FALSE(v.has_medium);
    CHECK(v.entries.empty());
  }
  {  // A volume term in a surface graph has no surface meaning: magenta and one rejection.
    GB g;
    term(g, VolumeTerm::SCATTER, {0.5f, 0.5f, 0.5f}, 1.0f);
    const ar_graph_desc d = g.desc();
    const astroray::host::MaterialOutput m = astroray::host::compile_material(d, fixture().table);
    CHECK(m.magenta);
    REQUIRE(m.entries.size() == 1u);
    CHECK(m.entries[0].code == AR_REPORT_PROGRAM_REJECTED);
    CHECK(m.entries[0].detail.find("VOLUME_COEFFS") != std::string::npos);
  }
}

TEST_CASE("medium_eval/material store compiles the volume graph") {
  using namespace astroray::host;
  const ar_key S = 10, V = 11, V3 = 13, M = 20;
  astroray::ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  auto store = std::make_shared<MaterialStore>(std::make_shared<Ledger>());
  GB surface, vol, vol2, bad;
  surface.store(surface.add(Op::CONST_F32, {0.5f}), ar::RecordSlot::specular_roughness);
  term(vol, ar::VolumeTerm::SCATTER, {0.8f, 0.5f, 0.2f}, 2.0f, 2.0f);
  term(vol2, ar::VolumeTerm::SCATTER, {0.8f, 0.5f, 0.2f}, 6.0f, 0.5f);
  for (int k = 0; k < 3; ++k) term(bad, ar::VolumeTerm::SCATTER, {0.5f, 0.5f, 0.5f}, 1.0f);
  auto commit = [&](const std::function<void(Edit&)>& body) {
    auto e = scene.begin_edit();
    REQUIRE(e);
    body(*e);
    uint64_t gen = 0;
    REQUIRE(e->commit(&gen) == AR_OK);
    store->end_edit();
    scene.generations().wait_idle();
  };
  auto set_graph = [&](Edit& e, ar_key key, const GB& g) {
    const ar_graph_desc d = g.desc();
    REQUIRE(store->set_graph(e, key, &d) == AR_OK);
  };
  auto set_material = [&](Edit& e, ar_key volume_graph) {
    ar_material_desc m = {};
    m.surface_graph = S;
    m.volume_graph = volume_graph;
    REQUIRE(store->set_material(e, M, &m) == AR_OK);
  };
  auto entries = [&](uint32_t code) {
    scene.report().snapshot();
    ar_report_info info = {};
    REQUIRE(scene.report().info(&info) == AR_OK);
    std::vector<std::string> out;
    for (uint32_t k = 0; k < info.entry_count; ++k) {
      ar_report_entry e = {};
      REQUIRE(scene.report().entry(k, &e) == AR_OK);
      if (e.subject == M && e.code == code) out.emplace_back(reinterpret_cast<const char*>(e.detail));
    }
    return out;
  };
  const ar::Lambda lam{{450.0f, 520.0f, 590.0f, 660.0f}};
  auto sigma_s0 = [&]() {
    const std::shared_ptr<const MaterialArrays> a = store->arrays();
    REQUIRE(a->media.size() == 1u);
    ar::VmRegs regs = {};
    ar::MediumSample s;
    const ar::MediumTables t{a->medium_programs.data(), a->media.data(), fixture().tables};
    ar::MediumDesc desc = {};
    desc.program = a->medium_index[0];
    REQUIRE(ar::medium_eval(t, desc, ar::Real3{0, 0, 0}, lam, &regs, &s) == ar::VmStatus::OK);
    return float(s.sigma_s.v[0]);
  };
  // A volume graph compiles to a medium beside the surface; its clamped g is reported once.
  commit([&](Edit& e) {
    set_graph(e, S, surface);
    set_graph(e, V, vol);
    set_material(e, V);
  });
  {
    const std::shared_ptr<const MaterialArrays> a = store->arrays();
    REQUIRE(a->keys == std::vector<ar_key>{M});
    CHECK(a->medium_index == std::vector<uint32_t>{0u});
    CHECK((a->medium_programs == std::vector<uint32_t>{0u, uint32_t(Op::VOLUME_COEFFS), uint32_t(Op::END)}));
    CHECK(a->media[0].entry == 1u);
    CHECK(a->media[0].g[0] == float(ar::kHgMaxAbsG));
    CHECK(entries(AR_REPORT_NODE_UNSUPPORTED).empty());
    CHECK(entries(AR_REPORT_NODE_APPROXIMATED).size() == 1u);
    CHECK(entries(AR_REPORT_PROGRAM_REJECTED).empty());
  }
  const float first = sigma_s0();
  // Setting another graph under the volume key recompiles the material: the coefficients change.
  commit([&](Edit& e) { set_graph(e, V, vol2); });
  CHECK(sigma_s0() > 2.5f * first);
  CHECK(store->arrays()->media[0].g[0] == 0.5f);
  // A volume the compiler cannot take is reported and not rendered; the surface is untouched.
  commit([&](Edit& e) {
    set_graph(e, V3, bad);
    set_material(e, V3);
  });
  CHECK(store->arrays()->media.empty());
  CHECK(store->arrays()->medium_index == std::vector<uint32_t>{MaterialArrays::kNoMedium});
  CHECK(entries(AR_REPORT_NODE_UNSUPPORTED).size() == 1u);
  CHECK((store->arrays()->headers[0].flags & ar::CONSTANT_SURFACE) != 0u);
}

}  // TEST_SUITE
